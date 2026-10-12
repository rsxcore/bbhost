// The graphics journal (gfx_journal.h).
#include "host/gfx_journal.h"

#include "core/portable.h"
#include "host/gpu.h"
#include "log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace gfxj {

const int g_mode = [] {
    const char* e = std::getenv("BBHOST_GFX_LOG");
    if (!e || !e[0] || e[0] == '0') return 0;
    return e[0] == '2' ? 2 : 1;
}();

namespace {

using Clock = std::chrono::steady_clock;

const char* const kNames[kCount] = {"pipeline_gfx", "pipeline_cs", "fence_wait", "submit",   "mem_alloc", "target_make",
                                    "target_replaced", "copy_refused", "texture_upload", "acquire", "present", "swapchain",
                                    "dlss_eval", "dlss_fail", "vram_evict", "gpu_slow", "idle_wait", "draw_fail", "note"};

// Events that say the picture was wrong, not that something was slow.
constexpr bool marks_wrong(Kind k) { return k == kTargetReplaced || k == kCopyRefused || k == kDlssFail || k == kDrawFail; }

struct Ev {
    double t_ms;  // since the process's first journal call
    std::uint64_t ns, a, b;
    std::uint32_t tid;
    Kind kind;
    char tag[24];
};

struct Agg {
    std::uint64_t count = 0, total_ns = 0, max_ns = 0;
};

constexpr std::size_t kMaxListed = 48;

struct State {
    std::mutex mu;
    Clock::time_point origin = Clock::now();
    Clock::time_point last_flip{};
    Agg agg[kCount];
    std::vector<Ev> list;
    std::uint64_t dropped = 0;  // events over the list's limit
    bool wrong = false;         // an event marking a wrong picture is in the frame

    // The counters when the last frame closed.
    GpuStats stats{};
    GpuBusy busy{};
    bool have_prev = false;

    // The second under way.
    Clock::time_point second_start{};
    std::uint64_t frames = 0, hitches = 0, wrong_frames = 0;
    double worst_ms = 0, sum_ms = 0, busy_ms = 0;
    Agg second_agg[kCount];

    std::FILE* file = nullptr;
    bool tried = false;
    std::string path;
    double hitch_ms = 20.0, event_ms = 0.5;
    std::string cache_drive;  // where the shader caches are written, for the free-space reading
    std::uint64_t seq = 0;
};

State& st() {
    static State s;
    return s;
}

double ms_of(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

void json_string(std::string& out, const char* s) {
    out += '"';
    for (; s && *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            out += ' ';
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

void append(std::string& out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void append(std::string& out, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, std::min<std::size_t>(static_cast<std::size_t>(n), sizeof(buf) - 1));
}

double env_double(const char* name, double fallback) {
    const char* e = std::getenv(name);
    return e && e[0] ? std::atof(e) : fallback;
}

void open_file_locked(State& s) {
    if (s.tried) return;
    s.tried = true;
    s.hitch_ms = env_double("BBHOST_GFX_HITCH_MS", 20.0);
    s.event_ms = env_double("BBHOST_GFX_EVENT_MS", 0.5);
    std::error_code ec;
    std::string base;
    if (const char* d = std::getenv("BBHOST_GFX_LOG_DIR"); d && d[0]) {
        base = d;
    } else {
        base = std::filesystem::is_directory("logs", ec) ? "logs" : "build";
    }
    std::filesystem::create_directories(base, ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char name[48];
    std::strftime(name, sizeof(name), "gfx-%Y%m%d-%H%M%S.jsonl", &tm);
    s.path = base + "/" + name;
    s.file = std::fopen(s.path.c_str(), "wb");
    if (!s.file) {
        host_log("gfx journal: cannot write %s", s.path.c_str());
        return;
    }
#if defined(_WIN32)
    if (const char* l = std::getenv("LOCALAPPDATA"); l && l[0]) s.cache_drive = l;
#endif
    if (s.cache_drive.empty()) s.cache_drive = base;
    std::string head;
    append(head, "{\"schema\":\"bbhost-gfx-journal\",\"version\":1,\"mode\":%d,\"hitch_ms\":%.1f,\"event_ms\":%.2f,"
                 "\"kinds\":[", g_mode, s.hitch_ms, s.event_ms);
    for (int k = 0; k < kCount; ++k) append(head, "%s\"%s\"", k ? "," : "", kNames[k]);
    head += "]}\n";
    std::fputs(head.c_str(), s.file);
    std::fflush(s.file);
    host_log("gfx journal: %s (BBHOST_GFX_LOG=%d; hitch over %.1f ms, events over %.2f ms listed)", s.path.c_str(), g_mode, s.hitch_ms,
             s.event_ms);
}

// Free space on the drive the caches are written to, in MiB; -1 unknown.
std::int64_t free_disk_mb(const std::string& where) {
    std::error_code ec;
    const auto sp = std::filesystem::space(where, ec);
    return ec ? -1 : static_cast<std::int64_t>(sp.available >> 20);
}

struct SystemMem {
    std::int64_t avail_phys_mb = -1, avail_commit_mb = -1, working_set_mb = -1;
};

SystemMem system_mem() {
    SystemMem m;
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.avail_phys_mb = static_cast<std::int64_t>(ms.ullAvailPhys >> 20);
        m.avail_commit_mb = static_cast<std::int64_t>(ms.ullAvailPageFile >> 20);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) m.working_set_mb = static_cast<std::int64_t>(pmc.WorkingSetSize >> 20);
#endif
    return m;
}

std::unordered_map<std::uint32_t, std::string> thread_names() {
    std::unordered_map<std::uint32_t, std::string> out;
    for (const HostThreadTime& t : host_thread_times()) out.emplace(t.tid, t.name);
    return out;
}

void write_line_locked(State& s, const std::string& line) {
    if (!s.file) return;
    std::fwrite(line.data(), 1, line.size(), s.file);
}

void add_agg(Agg& a, std::uint64_t ns) {
    ++a.count;
    a.total_ns += ns;
    a.max_ns = std::max(a.max_ns, ns);
}

}  // namespace

void event(Kind kind, std::uint64_t ns, std::uint64_t a, std::uint64_t b, const char* tag) {
    if (!on() || kind >= kCount) return;
    State& s = st();
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lk(s.mu);
    add_agg(s.agg[kind], ns);
    if (marks_wrong(kind)) s.wrong = true;
    const bool listed = marks_wrong(kind) || ms_of(ns) >= (s.tried ? s.event_ms : 0.5) || kind == kSwapchain || kind == kNote;
    if (!listed) return;
    if (s.list.size() >= kMaxListed) {
        ++s.dropped;
        return;
    }
    Ev e{};
    e.t_ms = std::chrono::duration<double, std::milli>(now - s.origin).count();
    e.ns = ns;
    e.a = a;
    e.b = b;
    e.tid = host_thread_id();
    e.kind = kind;
    if (tag) std::snprintf(e.tag, sizeof(e.tag), "%s", tag);
    s.list.push_back(e);
}

namespace {
std::atomic<std::uint64_t> g_main_busy_us{0};  // the longest main-loop frame since the last flip
}

void main_frame(std::uint64_t busy_us) {
    if (!on()) return;
    std::uint64_t cur = g_main_busy_us.load(std::memory_order_relaxed);
    while (busy_us > cur && !g_main_busy_us.compare_exchange_weak(cur, busy_us, std::memory_order_relaxed)) {
    }
}

void on_flip(std::uint64_t flip) {
    if (!on()) return;
    State& s = st();
    const auto now = Clock::now();
    const GpuStats stats = host_gpu_stats();
    const GpuBusy busy = host_gpu_busy();
    std::lock_guard<std::mutex> lk(s.mu);
    open_file_locked(s);
    if (s.last_flip.time_since_epoch().count() == 0) {
        s.last_flip = s.second_start = now;
        s.stats = stats;
        s.busy = busy;
        s.have_prev = true;
        for (Agg& a : s.agg) a = Agg{};
        s.list.clear();
        s.dropped = 0;
        s.wrong = false;
        return;
    }
    const double dt = std::chrono::duration<double, std::milli>(now - s.last_flip).count();
    s.last_flip = now;

    const std::uint64_t d_draws = stats.draws - s.stats.draws, d_disp = stats.dispatches - s.stats.dispatches;
    const std::uint64_t d_fail = stats.draw_failures - s.stats.draw_failures;
    const std::uint64_t d_fallback = stats.draws_ps[kDrawPsFallback] - s.stats.draws_ps[kDrawPsFallback];
    const std::uint64_t d_pipes = stats.pipelines - s.stats.pipelines;
    const double gpu_ms = ms_of(busy.busy_ns - s.busy.busy_ns);
    const double main_ms = static_cast<double>(g_main_busy_us.exchange(0, std::memory_order_relaxed)) / 1000.0;
    const bool wrong = s.wrong || d_fail > 0;
    const bool hitch = dt >= s.hitch_ms;

    ++s.frames;
    s.sum_ms += dt;
    s.worst_ms = std::max(s.worst_ms, dt);
    s.busy_ms += gpu_ms;
    s.hitches += hitch;
    s.wrong_frames += wrong;
    for (int k = 0; k < kCount; ++k) {
        if (!s.agg[k].count) continue;
        s.second_agg[k].count += s.agg[k].count;
        s.second_agg[k].total_ns += s.agg[k].total_ns;
        s.second_agg[k].max_ns = std::max(s.second_agg[k].max_ns, s.agg[k].max_ns);
    }

    if (g_mode == 2 || hitch || wrong) {
        std::string line;
        line.reserve(1024);
        append(line, "{\"flip\":%llu,\"t\":%.3f,\"dt\":%.2f,\"main\":%.2f,\"gpu\":%.2f,\"draws\":%llu,\"disp\":%llu,\"fail\":%llu,\"fallback\":%llu,"
                     "\"pipes\":%llu",
               static_cast<unsigned long long>(flip), std::chrono::duration<double>(now - s.origin).count(), dt, main_ms, gpu_ms,
               static_cast<unsigned long long>(d_draws), static_cast<unsigned long long>(d_disp), static_cast<unsigned long long>(d_fail),
               static_cast<unsigned long long>(d_fallback), static_cast<unsigned long long>(d_pipes));
        // The command processor's time by phase in the frame, ms.
        line += ",\"cp\":{";
        bool first = true;
        for (int p = 0; p < 8; ++p) {
            const double v = ms_of(stats.phase_ns[p] - s.stats.phase_ns[p]);
            if (v < 0.05) continue;
            append(line, "%s\"%s\":%.2f", first ? "" : ",", kPhaseNames[p], v);
            first = false;
        }
        line += "}";
        if (d_fail) {
            line += ",\"fail_why\":{";
            first = true;
            for (int k = 0; k < 16; ++k) {
                const std::uint64_t n = stats.draw_fail_why[k] - s.stats.draw_fail_why[k];
                const char* nm = host_gpu_draw_fail_name(k);
                if (!n || !nm) continue;
                append(line, "%s\"%s\":%llu", first ? "" : ",", nm, static_cast<unsigned long long>(n));
                first = false;
            }
            line += "}";
        }
        line += ",\"ev\":{";
        first = true;
        for (int k = 0; k < kCount; ++k) {
            if (!s.agg[k].count) continue;
            append(line, "%s\"%s\":[%llu,%.2f,%.2f]", first ? "" : ",", kNames[k], static_cast<unsigned long long>(s.agg[k].count),
                   ms_of(s.agg[k].total_ns), ms_of(s.agg[k].max_ns));
            first = false;
        }
        line += "}";
        if (!s.list.empty()) {
            line += ",\"list\":[";
            for (std::size_t i = 0; i < s.list.size(); ++i) {
                const Ev& e = s.list[i];
                append(line, "%s{\"k\":\"%s\",\"t\":%.2f,\"ms\":%.2f,\"a\":%llu,\"b\":%llu,\"th\":%u", i ? "," : "", kNames[e.kind], e.t_ms,
                       ms_of(e.ns), static_cast<unsigned long long>(e.a), static_cast<unsigned long long>(e.b), e.tid);
                if (e.tag[0]) {
                    line += ",\"tag\":";
                    json_string(line, e.tag);
                }
                line += "}";
            }
            line += "]";
            if (s.dropped) append(line, ",\"dropped\":%llu", static_cast<unsigned long long>(s.dropped));
        }
        append(line, ",\"vram_free_mb\":%lld", static_cast<long long>(host_gpu_vram_headroom_mb()));
        if (hitch) line += ",\"hitch\":1";
        if (wrong) line += ",\"wrong\":1";
        line += "}\n";
        write_line_locked(s, line);
    }

    if (hitch) {
        // The same in the log: what took the time, longest first.
        std::vector<const Ev*> top;
        for (const Ev& e : s.list) top.push_back(&e);
        std::sort(top.begin(), top.end(), [](const Ev* x, const Ev* y) { return x->ns > y->ns; });
        std::unordered_map<std::uint32_t, std::string> names = thread_names();
        std::string msg;
        append(msg, "gfx hitch: flip %llu took %.1f ms (main loop %.1f, gpu busy %.1f; %llu draws, %llu failed, %llu on the fallback pipeline, %llu pipelines made)",
               static_cast<unsigned long long>(flip), dt, main_ms, gpu_ms, static_cast<unsigned long long>(d_draws),
               static_cast<unsigned long long>(d_fail), static_cast<unsigned long long>(d_fallback), static_cast<unsigned long long>(d_pipes));
        for (std::size_t i = 0; i < top.size() && i < 5; ++i) {
            const Ev& e = *top[i];
            if (e.ns == 0) continue;
            const auto it = names.find(e.tid);
            append(msg, "; %s %.1f ms%s%s on %s", kNames[e.kind], ms_of(e.ns), e.tag[0] ? " " : "", e.tag,
                   it != names.end() ? it->second.c_str() : "?");
        }
        for (int k = 0; k < kCount; ++k) {
            if (s.agg[k].count > 1 && ms_of(s.agg[k].total_ns) >= 2.0) {
                append(msg, "; %llu x %s %.1f ms in all", static_cast<unsigned long long>(s.agg[k].count), kNames[k],
                       ms_of(s.agg[k].total_ns));
            }
        }
        host_log("%s", msg.c_str());
    }

    s.stats = stats;
    s.busy = busy;
    for (Agg& a : s.agg) a = Agg{};
    s.list.clear();
    s.dropped = 0;
    s.wrong = false;

    const double secs = std::chrono::duration<double>(now - s.second_start).count();
    if (secs >= 1.0) {
        const SystemMem sm = system_mem();
        std::string line;
        append(line, "{\"sec\":%llu,\"t\":%.3f,\"frames\":%llu,\"avg\":%.2f,\"worst\":%.2f,\"hitches\":%llu,\"wrong\":%llu,\"gpu_pct\":%.1f,"
                     "\"vram_free_mb\":%lld,\"sys_free_mb\":%lld,\"commit_free_mb\":%lld,\"ws_mb\":%lld,\"disk_free_mb\":%lld,\"ev\":{",
               static_cast<unsigned long long>(++s.seq), std::chrono::duration<double>(now - s.origin).count(),
               static_cast<unsigned long long>(s.frames), s.frames ? s.sum_ms / static_cast<double>(s.frames) : 0.0, s.worst_ms,
               static_cast<unsigned long long>(s.hitches), static_cast<unsigned long long>(s.wrong_frames),
               100.0 * s.busy_ms / std::max(1.0, secs * 1000.0), static_cast<long long>(host_gpu_vram_headroom_mb()),
               static_cast<long long>(sm.avail_phys_mb), static_cast<long long>(sm.avail_commit_mb),
               static_cast<long long>(sm.working_set_mb), static_cast<long long>(free_disk_mb(s.cache_drive)));
        bool first = true;
        for (int k = 0; k < kCount; ++k) {
            if (!s.second_agg[k].count) continue;
            append(line, "%s\"%s\":[%llu,%.2f,%.2f]", first ? "" : ",", kNames[k], static_cast<unsigned long long>(s.second_agg[k].count),
                   ms_of(s.second_agg[k].total_ns), ms_of(s.second_agg[k].max_ns));
            first = false;
        }
        line += "}}\n";
        write_line_locked(s, line);
        if (s.file) std::fflush(s.file);
        // A drive that is nearly full cannot take the shader caches: say it.
        const std::int64_t disk = free_disk_mb(s.cache_drive);
        static bool said_disk = false;
        if (disk >= 0 && disk < 1024 && !said_disk) {
            said_disk = true;
            host_log("gfx journal: only %lld MiB free on the drive the caches are written to; shader and pipeline caches may not save", static_cast<long long>(disk));
        }
        s.second_start = now;
        s.frames = s.hitches = s.wrong_frames = 0;
        s.worst_ms = s.sum_ms = s.busy_ms = 0;
        for (Agg& a : s.second_agg) a = Agg{};
    }
}

void shutdown() {
    if (!on()) return;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.file) {
        std::fflush(s.file);
        std::fclose(s.file);
        s.file = nullptr;
    }
}

}  // namespace gfxj
