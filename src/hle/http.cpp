// sceHttp / sceSsl over libcurl. Objects are integer handles like the SDK's:
// context -> template (UA, headers) -> connection (base URL) -> request.
// Nonblocking requests (the game's mode) run on a worker thread and report
// completion through sceHttpWaitRequest events; blocking ones run inline.
// URLs to the official hosts are rewritten to the private server from the
// config (online.host / online.scheme).
#include "hle/common.h"
#include "core/futex.h"
#include "hle/hle.h"
#include "hle/modules.h"
#include "core/config.h"
#include "bbhost_version.h"
#include "host/plugins.h"
#include "net/account.h"
#include "engine/summon_invite.h"
#include "replay/json.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#if !defined(_WIN32)
#include <pthread.h>
#endif
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(BBHOST_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace {

constexpr int kHttpInvalidId = static_cast<int>(0x80431016u);   // SCE_HTTP_ERROR_INVALID_ID
constexpr int kHttpInvalidValue = static_cast<int>(0x80431019u);
constexpr int kHttpBeforeSend = static_cast<int>(0x80431002u);
constexpr int kHttpEagain = static_cast<int>(0x80431082u);      // SCE_HTTP_ERROR_EAGAIN
constexpr int kHttpNetwork = static_cast<int>(0x80431063u);     // SCE_HTTP_ERROR_NETWORK
constexpr int kHttpTimeout = static_cast<int>(0x80431068u);
constexpr int kHttpBusy = static_cast<int>(0x80431021u);
constexpr std::uint32_t kEvIn = 0x1, kEvOut = 0x2, kEvSockErr = 0x8, kEvHup = 0x10;

// ---- sceSsl: libcurl owns TLS; contexts are just ids.
int g_ssl_next = 1;
std::mutex g_ssl_mu;
std::unordered_set<int> g_ssl_ctx;

GUEST_ABI int hle_ssl_init(std::uint64_t pool) {
    std::lock_guard<std::mutex> lock(g_ssl_mu);
    int id = g_ssl_next++;
    g_ssl_ctx.insert(id);
    host_log("sceSslInit pool=%llu -> %d", static_cast<unsigned long long>(pool), id);
    return id;
}
GUEST_ABI int hle_ssl_term() {
    std::lock_guard<std::mutex> lock(g_ssl_mu);
    g_ssl_ctx.clear();
    return 0;
}

// ---- objects
struct HttpEpoll;
struct NbEvent {
    std::uint32_t events;
    std::uint32_t detail;
    int id;
    void* user;
};
// The lock and the sleep are core/futex.h's: sceHttpWaitRequest's timeout is
// in microseconds, and winpthreads' condition variable returned at once on
// one under a millisecond, the game's wait loop then spinning out the rest.
struct HttpEpoll {
    HostLock mu;
    HostCondVar cv;
    std::deque<NbEvent> events;
    bool aborting = false;
};
enum class HttpKind : int { Ctx = 1, Tmpl, Conn, Req };
struct Response {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int error = 0;  // SCE error or 0
    long status = 0;
    std::vector<std::uint8_t> body;
    std::size_t read_off = 0;
};
struct HttpObj {
    HttpKind kind = HttpKind::Ctx;
    int parent = 0;
    int nonblock = -1;  // -1 inherit
    HttpEpoll* ep = nullptr;
    void* user = nullptr;
    std::string ua;
    std::string url;
    int method = 0;
    std::vector<std::string> headers;
    std::uint64_t content_len = 0;
    unsigned connect_timeout_us = 0;
    bool sent = false;
    std::shared_ptr<Response> resp;
};
std::mutex g_http_mu;
int g_http_next = 1;
std::unordered_map<int, HttpObj> g_http;
std::unordered_set<HttpEpoll*> g_http_eps;

int http_alloc(HttpKind kind, int parent) {
    const int id = g_http_next++;
    HttpObj o{};
    o.kind = kind;
    o.parent = parent;
    g_http[id] = o;
    return id;
}
HttpObj* http_get(int id, HttpKind kind) {
    auto it = g_http.find(id);
    if (it == g_http.end() || it->second.kind != kind) {
        return nullptr;
    }
    return &it->second;
}
HttpObj* http_any(int id) {
    auto it = g_http.find(id);
    return it == g_http.end() ? nullptr : &it->second;
}

// "Name: value" -> whether its name is `name` (header names are case-insensitive).
bool header_named(const std::string& line, const std::string& name) {
    const auto colon = line.find(':');
    if (colon == std::string::npos || colon != name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < colon; i++) {
        if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}
std::string header_name(const std::string& line) {
    return line.substr(0, line.find(':'));
}

// Walk request -> connection -> template collecting inherited settings.
struct Effective {
    std::string ua;
    std::vector<std::string> headers;
    bool nonblock = false;
    HttpEpoll* ep = nullptr;
    void* user = nullptr;
    unsigned connect_timeout_us = 0;
};
Effective effective_locked(int req_id) {
    Effective e;
    int nb = -1;
    for (int id = req_id; id;) {
        HttpObj* o = http_any(id);
        if (!o) {
            break;
        }
        // A nearer object's header wins over a parent's of the same name
        // (one object's own repeats, added with SCE_HTTP_HEADER_ADD, stay).
        const std::vector<std::string> nearer = e.headers;
        for (auto it = o->headers.rbegin(); it != o->headers.rend(); ++it) {
            const std::string name = header_name(*it);
            if (std::any_of(nearer.begin(), nearer.end(), [&](const std::string& h) { return header_named(h, name); })) {
                continue;
            }
            e.headers.insert(e.headers.begin(), *it);
        }
        if (e.ua.empty()) {
            e.ua = o->ua;
        }
        if (nb < 0 && o->nonblock >= 0) {
            nb = o->nonblock;
        }
        if (!e.ep && o->ep) {
            e.ep = o->ep;
            e.user = o->user;
        }
        if (!e.connect_timeout_us && o->connect_timeout_us) {
            e.connect_timeout_us = o->connect_timeout_us;
        }
        id = o->parent;
    }
    e.nonblock = nb > 0;
    return e;
}

// online.host replaces the official hostnames; online.scheme forces http/https.
std::string rewrite_url(const std::string& url) {
    const HostConfig& cfg = config();
    std::size_t p = url.find("://");
    if (p == std::string::npos) {
        return url;
    }
    std::string scheme = url.substr(0, p);
    std::size_t host_start = p + 3;
    std::size_t host_end = url.find_first_of(":/", host_start);
    if (host_end == std::string::npos) {
        host_end = url.size();
    }
    std::string host = url.substr(host_start, host_end - host_start);
    std::string rest = url.substr(host_end);
    // The game uploads its play logs to FromSoftware's S3 buckets
    // (https://bb-playlog-{test,prod}.s3.amazonaws.com/<date>/<file>, PUT).
    // They go to the private server's game port instead, which stores them
    // (its PUT /{date}/{file}); left alone they went to Amazon under bucket
    // names we do not own.
    if (host == "bb-playlog-test.s3.amazonaws.com" || host == "bb-playlog-prod.s3.amazonaws.com") {
        const std::string to = cfg.online_host.empty() ? "thehuntersdream.com" : cfg.online_host;
        const std::string sch = cfg.online_scheme.empty() ? "http" : cfg.online_scheme;
        std::size_t path = rest.find('/');
        return sch + "://" + to + ":18671" + (path == std::string::npos ? "/" : rest.substr(path));
    }
    bool official = host.size() >= 15 && host.compare(host.size() - 15, 15, "scej-network.jp") == 0;
    if (official && !cfg.online_host.empty()) {
        host = cfg.online_host;
    }
    if (official && !cfg.online_scheme.empty()) {
        scheme = cfg.online_scheme;
    }
    return scheme + "://" + host + rest;
}

#if defined(BBHOST_HAVE_CURL)
std::once_flag g_curl_once;
std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* user) {
    auto* body = static_cast<std::vector<std::uint8_t>*>(user);
    body->insert(body->end(), reinterpret_cast<std::uint8_t*>(ptr), reinterpret_cast<std::uint8_t*>(ptr) + size * nmemb);
    return size * nmemb;
}
#endif

// The calling thread's name, for the log.
std::string thread_name_now() {
#if !defined(_WIN32)
    char n[32] = {};
    pthread_getname_np(pthread_self(), n, sizeof(n));
    return n;
#else
    return "?";
#endif
}

// BBHOST_HTTP_DELAY_MS (tests): every response held back that long before it
// counts as arrived - a server on this machine answers in a millisecond, the
// real one 23 ms and a TLS handshake away, and a caller that waits for the
// answer only shows against the second.
const int g_http_delay_ms = [] {
    const char* e = std::getenv("BBHOST_HTTP_DELAY_MS");
    return e ? std::atoi(e) : 0;
}();

// BBHOST_HTTP_TRACE (reverse engineering the game's server traffic): every
// request logged, not just the first 24, and each one's request and response
// bodies written to <dir>/NNNN-METHOD-<path>.req and .resp. "1" means the
// folder http-trace beside the working directory; anything else names it.
const std::string g_http_trace_dir = [] {
    const char* e = std::getenv("BBHOST_HTTP_TRACE");
    if (!e || !*e || std::strcmp(e, "0") == 0) return std::string();
    return std::strcmp(e, "1") == 0 ? std::string("http-trace") : std::string(e);
}();

void trace_bodies(int seq, const char* method, const std::string& url, const std::vector<std::uint8_t>& post,
                  const std::vector<std::uint8_t>& body) {
    static const bool ready = [] {
        std::error_code ec;
        std::filesystem::create_directories(g_http_trace_dir, ec);
        if (ec) {
            host_log("http: BBHOST_HTTP_TRACE: cannot create %s (%s); no bodies written", g_http_trace_dir.c_str(),
                     ec.message().c_str());
            return false;
        }
        host_log("http: writing every request's bodies to %s",
                 std::filesystem::absolute(g_http_trace_dir, ec).string().c_str());
        return true;
    }();
    if (!ready) return;
    // The URL's path, past the host, as a file name: /frpg2/ss/x.spd -> frpg2_ss_x.spd.
    std::size_t p = url.find("://");
    p = url.find('/', p == std::string::npos ? 0 : p + 3);
    std::string name = p == std::string::npos ? std::string("root") : url.substr(p + 1);
    if (std::size_t q = name.find('?'); q != std::string::npos) name.resize(q);
    for (char& ch : name) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '-') ch = '_';
    }
    if (name.size() > 80) name.resize(80);
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "%04d-%s-", seq, method);
    const std::filesystem::path base = std::filesystem::path(g_http_trace_dir) / (prefix + name);
    auto write = [](const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
        if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
            if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
            std::fclose(f);
        }
    };
    write(base.string() + ".req", post);
    write(base.string() + ".resp", body);
}

// BBHOST_INVADE_AREA (experiment: invading another area, as DS3's Wex Dust
// does): "AreaId,AreaRegionId" or "AreaId,AreaRegionId,PosX,PosY,PosZ". The
// summon sign requests (summon_messenger_create / summon_messenger_get) go
// out with those fields in place of the player's own. BBHOST_INVADE_ON picks
// which of the two: "create", "get" or "both" (the default). The bodies are
// the game's JSON; the fields are rewritten in the text, so everything else
// (CharaId is a 64-bit integer a double would round) goes out byte for byte.
struct InvadeArea {
    bool on = false;
    std::string area_id, region_id, pos[3];
    bool create = true, get = true;
};
const InvadeArea g_invade = [] {
    InvadeArea a;
    const char* e = std::getenv("BBHOST_INVADE_AREA");
    if (!e || !*e) return a;
    std::vector<std::string> parts(1);
    for (const char* p = e; *p; ++p) {
        if (*p == ',') parts.emplace_back();
        else if (*p != ' ') parts.back() += *p;
    }
    if (parts.size() != 2 && parts.size() != 5) return a;
    for (const auto& s : parts) {
        if (s.empty() || s.find_first_not_of("-0123456789") != std::string::npos) return a;
    }
    a.area_id = parts[0];
    a.region_id = parts[1];
    if (parts.size() == 5) {
        for (int i = 0; i < 3; ++i) a.pos[i] = parts[2 + i];
    }
    if (const char* on = std::getenv("BBHOST_INVADE_ON"); on && *on) {
        a.create = std::strcmp(on, "get") != 0;
        a.get = std::strcmp(on, "create") != 0;
    }
    a.on = true;
    return a;
}();

// The integer after "key": in a JSON text, replaced by `value`. False when the
// key is not there with an integer.
bool replace_int_field(std::string& text, const char* key, const std::string& value) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    const std::size_t from = at + needle.size();
    std::size_t to = from;
    if (to < text.size() && text[to] == '-') ++to;
    while (to < text.size() && std::isdigit(static_cast<unsigned char>(text[to]))) ++to;
    if (to == from || (to == from + 1 && text[from] == '-')) return false;
    text.replace(from, to - from, value);
    return true;
}

// The integer after "key": in a JSON text, as written; empty when absent.
std::string int_field(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) return {};
    const std::size_t from = at + needle.size();
    std::size_t to = from;
    if (to < text.size() && text[to] == '-') ++to;
    while (to < text.size() && std::isdigit(static_cast<unsigned char>(text[to]))) ++to;
    if (to == from || (to == from + 1 && text[from] == '-')) return {};
    return text.substr(from, to - from);
}

// ---- Wex: invading across the whole world (DS3's Wex Dust, for Bloodborne).
//
// What the traffic shows: the Sinister Bell makes the invader's game post a
// sign (summon_messenger_create, SummonType 2) for its own AreaId,
// AreaRegionId and position, about once a minute; a host whose world has a
// ringing Chime Maiden asks (summon_messenger_get, SummonType 2) for the signs
// of its own area, finds one, and summons it. So the sign's area decides who
// can find the invader. Wex moves it: each new sign goes to the next area of a
// list, and a host there finds it.
//
// The list is learned. Every summon request either instance makes carries its
// area and the player's position, and a new area is appended to the areas
// file (BBHOST_WEX_AREAS, default wex-areas.txt in the working directory,
// beside bbhost.exe for the run scripts), one line each:
//   AreaId AreaRegionId PosX PosY PosZ   # mNN_NN
// A line commented out with '#' is not visited (the blocklist).
//
// BBHOST_WEX=1 turns the rotation on. Only the invader's own requests are
// moved: a SummonType 2 sign, and the SummonType 2 search made while one is
// out. BBHOST_INVADE_AREA, one fixed area, wins over it.
struct WexArea {
    std::string area_id, region_id, pos[3];
    bool blocked = false;
};
// BBHOST_WEX=1: the sign goes round the areas, one per sign the game posts
// (about a minute each). BBHOST_WEX=all: the game's sign stays in its own area
// and a copy goes to every other open area at once - the host's game asks for
// signs about every 70 s, so a sign in every area is found at the next ask
// wherever the host is (if the server keeps one sign per area, not one per
// player: the log of the copies' answers and the hosts' finds tells).
enum class WexMode { Off, Rotate, All };
const WexMode g_wex_mode = [] {
    const char* e = std::getenv("BBHOST_WEX");
    if (!e) return WexMode::Off;
    if (std::strcmp(e, "all") == 0) return WexMode::All;
    return e[0] == '1' ? WexMode::Rotate : WexMode::Off;
}();
const bool g_wex_on = g_wex_mode != WexMode::Off;
// Set on the thread that posts the copies, so they are not rewritten again.
thread_local bool t_wex_copy = false;
const std::string g_wex_file = [] {
    const char* e = std::getenv("BBHOST_WEX_AREAS");
    return std::string(e && *e ? e : "wex-areas.txt");
}();
std::mutex g_wex_mu;
bool g_wex_loaded = false;
std::vector<WexArea> g_wex_areas;  // every area in the file, blocked ones too
std::size_t g_wex_next = 0;
int g_wex_current = -1;  // the area the sign out now was placed in

std::string area_name(const std::string& area_id) {
    const unsigned long long v = std::strtoull(area_id.c_str(), nullptr, 10);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "m%02llu_%02llu", (v >> 24) & 0xff, (v >> 16) & 0xff);
    return buf;
}

void wex_load_locked() {
    if (g_wex_loaded) return;
    g_wex_loaded = true;
    std::FILE* f = std::fopen(g_wex_file.c_str(), "rb");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        WexArea a;
        if (*p == '#') {
            a.blocked = true;
            ++p;
        }
        char id[24] = {}, region[24] = {}, x[24] = {}, y[24] = {}, z[24] = {};
        if (std::sscanf(p, "%23s %23s %23s %23s %23s", id, region, x, y, z) != 5) continue;
        const std::string fields[5] = {id, region, x, y, z};
        bool numeric = true;
        for (const auto& s : fields) numeric = numeric && s.find_first_not_of("-0123456789") == std::string::npos;
        if (!numeric) continue;  // a comment line
        a.area_id = id;
        a.region_id = region;
        a.pos[0] = x;
        a.pos[1] = y;
        a.pos[2] = z;
        g_wex_areas.push_back(a);
    }
    std::fclose(f);
    std::size_t open = 0;
    for (const auto& a : g_wex_areas) open += a.blocked ? 0 : 1;
    host_log("wex: %zu areas in %s (%zu blocked)%s", g_wex_areas.size(), g_wex_file.c_str(), g_wex_areas.size() - open,
             g_wex_on ? "; invading across them" : "");
}

// A request in an area the file does not have yet: append it.
// The player's position as the game keeps it for its sessions (FrpgNetMan,
// what a host's room carries as HostPos), rounded like the summon requests'.
bool player_pos(std::string out[3]) {
    json::Value extra = json::Value::make_object();
    summon_invite_host_extra(extra);
    const json::Value* pos = extra.find("HostPos");
    if (!pos || pos->type != json::Value::Type::Array || pos->array.size() < 3) return false;
    for (int i = 0; i < 3; ++i) out[i] = std::to_string(static_cast<long long>(std::lround(pos->array[static_cast<std::size_t>(i)].number)));
    return !(out[0] == "0" && out[1] == "0" && out[2] == "0");
}

void wex_learn(const std::string& text) {
    const std::string id = int_field(text, "AreaId"), region = int_field(text, "AreaRegionId");
    std::string x = int_field(text, "PosX"), y = int_field(text, "PosY"), z = int_field(text, "PosZ");
    if (id.empty() || region.empty() || id == "0") return;
    std::string mem[3];
    const bool have_mem = player_pos(mem);
    if (x.empty() || y.empty() || z.empty()) {
        // A request without a position (the wandering ghosts' post, made about
        // once a minute anywhere online): the position from the game's memory.
        if (!have_mem) return;
        x = mem[0];
        y = mem[1];
        z = mem[2];
    } else if (have_mem) {
        static std::atomic<int> checks{0};
        if (checks.fetch_add(1) < 3)
            host_log("wex: position check: the request says %s %s %s, the game's memory %s %s %s", x.c_str(), y.c_str(),
                     z.c_str(), mem[0].c_str(), mem[1].c_str(), mem[2].c_str());
    }
    std::lock_guard<std::mutex> lk(g_wex_mu);
    wex_load_locked();
    for (const auto& a : g_wex_areas) {
        if (a.area_id == id && a.region_id == region) return;
    }
    // Another instance may have added it since this one read the file.
    if (std::FILE* f = std::fopen(g_wex_file.c_str(), "rb")) {
        char line[256];
        bool there = false;
        const std::string key = id + " " + region + " ";
        while (!there && std::fgets(line, sizeof(line), f)) {
            const char* p = line;
            while (*p == ' ' || *p == '\t' || *p == '#') ++p;
            there = std::strncmp(p, key.c_str(), key.size()) == 0;
        }
        std::fclose(f);
        if (there) {
            g_wex_loaded = false;
            g_wex_areas.clear();
            wex_load_locked();
            return;
        }
    }
    WexArea a;
    a.area_id = id;
    a.region_id = region;
    a.pos[0] = x;
    a.pos[1] = y;
    a.pos[2] = z;
    g_wex_areas.push_back(a);
    if (std::FILE* f = std::fopen(g_wex_file.c_str(), "ab")) {
        std::fprintf(f, "%s %s %s %s %s   # %s\n", id.c_str(), region.c_str(), x.c_str(), y.c_str(), z.c_str(),
                     area_name(id).c_str());
        std::fclose(f);
    }
    host_log("wex: learned area %s (%s region %s) at %s %s %s", area_name(id).c_str(), id.c_str(), region.c_str(), x.c_str(),
             y.c_str(), z.c_str());
}

void place(std::string& text, const std::string& id, const std::string& region, const std::string* pos) {
    replace_int_field(text, "AreaId", id);
    replace_int_field(text, "AreaRegionId", region);
    if (pos && !pos[0].empty()) {
        replace_int_field(text, "PosX", pos[0]);
        replace_int_field(text, "PosY", pos[1]);
        replace_int_field(text, "PosZ", pos[2]);
    }
}

// Rewrites the game's summon request in place; returns the bodies of the
// copies BBHOST_WEX=all posts after it (the sign in every other open area).
std::vector<std::vector<std::uint8_t>> invade_rewrite(const std::string& url, std::vector<std::uint8_t>& post) {
    std::vector<std::vector<std::uint8_t>> copies;
    if (t_wex_copy) return copies;
    if (url.find("summon_messenger_delete") != std::string::npos) {
        // The sign is taken down (the bell rung again, or a summon began).
        std::lock_guard<std::mutex> lk(g_wex_mu);
        g_wex_current = -1;
        return copies;
    }
    if (post.empty()) return copies;
    if (url.find("wandering_ghost_create") != std::string::npos) {
        // Where the player is now, posted about once a minute: the areas file
        // fills as the world is walked, no bell needed.
        wex_learn(std::string(post.begin(), post.end()));
        return copies;
    }
    const bool create = url.find("summon_messenger_create") != std::string::npos;
    const bool get = url.find("summon_messenger_get") != std::string::npos;
    if (!create && !get) return copies;
    std::string text(post.begin(), post.end());
    wex_learn(text);
    const std::string own = int_field(text, "AreaId");
    const bool invader_sign = create && text.find("\"SummonType\":2") != std::string::npos;
    const bool invader_search = get && text.find("\"SummonType\":2") != std::string::npos;

    if (g_invade.on) {
        if (!(create && g_invade.create) && !(get && g_invade.get)) return copies;
        if (own.empty()) {
            host_log("invade: %s carries no AreaId; sent as the game made it", create ? "create" : "get");
            return copies;
        }
        place(text, g_invade.area_id, g_invade.region_id, g_invade.pos);
        post.assign(text.begin(), text.end());
        host_log("invade: %s sent for %s (area %s region %s) from %s", create ? "create" : "get",
                 area_name(g_invade.area_id).c_str(), g_invade.area_id.c_str(), g_invade.region_id.c_str(),
                 area_name(own).c_str());
        return copies;
    }
    if (!g_wex_on || own.empty()) return copies;

    std::lock_guard<std::mutex> lk(g_wex_mu);
    wex_load_locked();
    if (invader_sign && g_wex_mode == WexMode::All) {
        // The game's own sign as it is, and a copy for every other open area.
        std::string names;
        for (const WexArea& a : g_wex_areas) {
            if (a.blocked || (a.area_id == own && a.region_id == int_field(text, "AreaRegionId"))) continue;
            std::string copy = text;
            place(copy, a.area_id, a.region_id, a.pos);
            copies.emplace_back(copy.begin(), copy.end());
            names += " " + area_name(a.area_id) + "/" + a.region_id;
        }
        g_wex_current = -1;
        host_log("wex: sign in %s, and %zu copies:%s", area_name(own).c_str(), copies.size(), names.empty() ? " none" : names.c_str());
    } else if (invader_sign) {
        // The next open area after the last one, round the list.
        const std::size_t n = g_wex_areas.size();
        int pick = -1;
        for (std::size_t k = 0; k < n && pick < 0; ++k) {
            const std::size_t i = (g_wex_next + k) % n;
            if (!g_wex_areas[i].blocked) pick = static_cast<int>(i);
        }
        if (pick < 0) {
            host_log("wex: no open area in %s; the sign stays in %s", g_wex_file.c_str(), area_name(own).c_str());
            g_wex_current = -1;
            return copies;
        }
        g_wex_next = static_cast<std::size_t>(pick) + 1;
        g_wex_current = pick;
        const WexArea& a = g_wex_areas[static_cast<std::size_t>(pick)];
        place(text, a.area_id, a.region_id, a.pos);
        post.assign(text.begin(), text.end());
        host_log("wex: sign placed in %s (area %s region %s at %s %s %s), %d of %zu; the player is in %s",
                 area_name(a.area_id).c_str(), a.area_id.c_str(), a.region_id.c_str(), a.pos[0].c_str(), a.pos[1].c_str(),
                 a.pos[2].c_str(), pick + 1, n, area_name(own).c_str());
    } else if (invader_search && g_wex_current >= 0) {
        const WexArea& a = g_wex_areas[static_cast<std::size_t>(g_wex_current)];
        place(text, a.area_id, a.region_id, a.pos);
        post.assign(text.begin(), text.end());
    }
    return copies;
}

void perform(const std::string& url, int method, const Effective& eff, std::vector<std::uint8_t> post,
             std::uint64_t content_len, std::shared_ptr<Response> resp, bool rewritten) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::vector<std::uint8_t>> wex_copies = invade_rewrite(url, post);
#if defined(BBHOST_HAVE_CURL)
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    long status = 0;
    int err = 0;
    std::vector<std::uint8_t> body;
    if (!c) {
        err = kHttpNetwork;
    } else {
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        // The game's own connect timeout (its config value x1000, as usec) is
        // too short for a host across the internet: a loopback server connects
        // inside it, dev.thehuntersdream.com (23 ms away) never did - four
        // ss.info fetches timed out within seconds and nginx saw no request
        // (sceHttpSetConnectTimeOut logs the value it asks for). A PS4 on a real
        // network reached Sony's servers with the same value, so it is a floor
        // of what the game can wait, not a deadline; hold it to kMinConnectMs.
        constexpr long kMinConnectMs = 5000;
        const long want_ms = eff.connect_timeout_us ? static_cast<long>(eff.connect_timeout_us / 1000) : 10000;
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, want_ms < kMinConnectMs ? kMinConnectMs : want_ms);
        if (!config().online_verify_tls) {
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        }
#if defined(_WIN32)
        // schannel also asks the CA whether the certificate was revoked and fails
        // when that lookup cannot be made (firewalls, captive networks); the chain
        // and name are still checked.
        curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT | CURLSSLOPT_NATIVE_CA));
#endif
        // The game's own user agent with bbhost's name and version after it,
        // so a server can tell bbhost clients and their versions apart. The
        // game's text stays the prefix, which is what anything matching on
        // it looks at.
        const std::string ua = (eff.ua.empty() ? std::string() : eff.ua + " ") + "bbhost/" + BBHOST_VERSION;
        curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
        struct curl_slist* list = nullptr;
        for (const auto& h : eff.headers) {
            const bool is_ua = h.size() > 11 && std::equal(h.begin(), h.begin() + 11, "user-agent:", [](char a, char b) {
                                   return std::tolower(static_cast<unsigned char>(a)) == b;
                               });
            if (is_ua) {
                list = curl_slist_append(list, (h + " bbhost/" + BBHOST_VERSION).c_str());  // one the game set itself
                continue;
            }
            list = curl_slist_append(list, h.c_str());
        }
        list = curl_slist_append(list, "Expect:");
        // The game's own traffic carries the account's token too, so the
        // server's game sessions map to the account. Every host:
        // the game reaches the private server both through rewritten
        // official names and through the addresses ss.info hands it.
        (void)rewritten;
        if (const std::string token = net::account_token(); !token.empty()) {
            list = curl_slist_append(list, ("X-BB-Token: " + token).c_str());
        }
        // ... and the rules this session plays by (plugins_ruleset): the server
        // keeps a randomizer or boss-rush run off the normal map and stats, and
        // shows bloodstains and messages only within the same ruleset and seed.
        list = curl_slist_append(list, ("X-BBHost-Ruleset: " + plugins_ruleset()).c_str());
        // The plugins that can play another player's world (an adopting
        // guest may join a host of other rules).
        if (const std::string adopts = plugins_adopts(); !adopts.empty())
            list = curl_slist_append(list, ("X-BBHost-Adopt: " + adopts).c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
        // Methods as the game numbers them: 0 GET, 1 POST, 2 HEAD, 4 PUT (the
        // play-log upload). PUT sent as a GET lost its body.
        if (method == 1 || method == 4) {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            if (method == 4) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, post.data());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(post.empty() ? content_len : post.size()));
        }
        CURLcode rc = curl_easy_perform(c);
        if (rc != CURLE_OK) {
            err = rc == CURLE_OPERATION_TIMEDOUT ? kHttpTimeout : kHttpNetwork;
            static std::atomic<int> logs{0};
            if (logs.fetch_add(1) < 8) {
                host_log("http: %s -> %s", url.c_str(), curl_easy_strerror(rc));
            }
        } else {
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        }
        curl_slist_free_all(list);
        curl_easy_cleanup(c);
    }
#else
    (void)method;
    (void)eff;
    (void)post;
    (void)content_len;
    long status = 0;
    int err = kHttpNetwork;
    std::vector<std::uint8_t> body;
    host_log("http: no libcurl at build time; %s fails", url.c_str());
#endif
    if (g_http_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(g_http_delay_ms));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> logs{0};
    const int seq = logs.fetch_add(1);
    const char* method_name = method == 1 ? "POST" : method == 4 ? "PUT" : "GET";
    if (seq < 24 || !g_http_trace_dir.empty()) {
        host_log("http: #%d %s %s -> %ld (%zu bytes out, %zu in, %.0f ms)%s", seq, method_name, url.c_str(), status,
                 post.size(), body.size(), ms, err ? " error" : "");
    }
    if (!g_http_trace_dir.empty()) trace_bodies(seq, method_name, url, post, body);
    std::lock_guard<std::mutex> lk(resp->mu);
    resp->status = status;
    resp->error = err;
    resp->body = std::move(body);
    resp->done = true;
    resp->cv.notify_all();
    if (!wex_copies.empty()) {
        // BBHOST_WEX=all: the sign's copies, after the game has its own answer.
        std::thread([url, method, eff, copies = std::move(wex_copies)] {
            t_wex_copy = true;
            int ok = 0;
            for (const auto& copy : copies) {
                auto r = std::make_shared<Response>();
                perform(url, method, eff, copy, copy.size(), r, true);
                std::lock_guard<std::mutex> lk2(r->mu);
                const std::string reply(r->body.begin(), r->body.end());
                if (r->status == 200 && reply.find("\"ResKind\":0") != std::string::npos) ++ok;
            }
            host_log("wex: %d of %zu copies of the sign taken by the server", ok, copies.size());
        }).detach();
    }
}

void post_event(HttpEpoll* ep, int id, void* user, std::uint32_t events, std::uint32_t detail) {
    if (!ep) {
        return;
    }
    std::lock_guard<HostLock> lk(ep->mu);
    ep->events.push_back(NbEvent{events, detail, id, user});
    ep->cv.notify_all();
}

// ---- entry points
GUEST_ABI int hle_http_init(int net_mem, int ssl_ctx, std::uint64_t pool) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    const int id = http_alloc(HttpKind::Ctx, 0);
    host_log("sceHttpInit net=%d ssl=%d pool=%llu -> %d", net_mem, ssl_ctx, static_cast<unsigned long long>(pool), id);
    return id;
}
GUEST_ABI int hle_http_term() {
    std::lock_guard<std::mutex> lock(g_http_mu);
    g_http.clear();
    return 0;
}
GUEST_ABI int hle_http_create_template(int ctx, const char* ua, int ver, int proxy) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    if (!http_get(ctx, HttpKind::Ctx)) {
        return kHttpInvalidId;
    }
    const int id = http_alloc(HttpKind::Tmpl, ctx);
    g_http[id].ua = ua ? ua : "";
    host_log("sceHttpCreateTemplate ctx=%d ua=%s ver=%d proxy=%d -> %d", ctx, ua ? ua : "", ver, proxy, id);
    return id;
}
GUEST_ABI int hle_http_delete_template(int id) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    g_http.erase(id);
    return 0;
}
GUEST_ABI int hle_http_set_nonblock(int id, int enable) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    HttpObj* o = http_any(id);
    if (!o) {
        return kHttpInvalidId;
    }
    o->nonblock = enable ? 1 : 0;
    return 0;
}
GUEST_ABI int hle_http_create_epoll(int ctx, HttpEpoll** out) {
    if (!out) {
        return kHttpInvalidValue;
    }
    std::lock_guard<std::mutex> lock(g_http_mu);
    if (!http_get(ctx, HttpKind::Ctx)) {
        return kHttpInvalidId;
    }
    auto* ep = new HttpEpoll();
    g_http_eps.insert(ep);
    *out = ep;
    host_log("sceHttpCreateEpoll ctx=%d -> %p", ctx, static_cast<void*>(ep));
    return 0;
}
GUEST_ABI int hle_http_destroy_epoll(int, HttpEpoll* ep) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    if (!g_http_eps.erase(ep)) {
        return kHttpInvalidValue;
    }
    for (auto& kv : g_http) {
        if (kv.second.ep == ep) {
            kv.second.ep = nullptr;
        }
    }
    // Waiters hold g_http_mu only briefly; mark and leak rather than free under them.
    {
        std::lock_guard<HostLock> lk(ep->mu);
        ep->aborting = true;
        ep->cv.notify_all();
    }
    return 0;
}
// int sceHttpAddRequestHeader(int id, const char* name, const char* value, int mode)
// mode 0 (SCE_HTTP_HEADER_OVERWRITE) replaces a header of that name, 1
// (SCE_HTTP_HEADER_ADD) adds another. The play-log uploader sets its
// Authorization again before every PUT on one long-lived object: appending
// sent 2, 3, 4... Authorization lines, and nginx answers that with 400.
GUEST_ABI int hle_http_add_header(int id, const char* name, const char* value, unsigned mode) {
    if (!name) {
        return kHttpInvalidValue;
    }
    std::lock_guard<std::mutex> lock(g_http_mu);
    HttpObj* o = http_any(id);
    if (!o) {
        return kHttpInvalidId;
    }
    if (mode != 1) {
        const std::string n(name);
        std::erase_if(o->headers, [&](const std::string& h) { return header_named(h, n); });
    }
    o->headers.push_back(std::string(name) + ": " + (value ? value : ""));
    return 0;
}
// int sceHttpWaitRequest(SceHttpEpollHandle, SceHttpNBEvent* out, int maxevents, int timeout_us)
GUEST_ABI int hle_http_wait_request(HttpEpoll* ep, NbEvent* out, int max_events, int timeout) {
    if (!ep || !out || max_events <= 0) {
        return kHttpInvalidValue;
    }
    {
        std::lock_guard<std::mutex> lock(g_http_mu);
        if (!g_http_eps.count(ep)) {
            return kHttpInvalidValue;
        }
    }
    std::unique_lock<HostLock> lk(ep->mu);
    auto ready = [&] { return !ep->events.empty() || ep->aborting; };
    if (timeout < 0) {
        ep->cv.wait(lk, ready);
    } else if (timeout > 0) {
        ep->cv.wait_for(lk, std::chrono::microseconds(timeout), ready);
    }
    if (ep->aborting) {
        ep->aborting = false;
        return 0;
    }
    int n = 0;
    while (n < max_events && !ep->events.empty()) {
        out[n++] = ep->events.front();
        ep->events.pop_front();
    }
    return n;
}
GUEST_ABI int hle_http_abort_wait(HttpEpoll* ep) {
    if (!ep) {
        return kHttpInvalidValue;
    }
    std::lock_guard<HostLock> lk(ep->mu);
    ep->aborting = true;
    ep->cv.notify_all();
    return 0;
}
GUEST_ABI int hle_https_enable(int, unsigned) { return 0; }
GUEST_ABI int hle_https_disable(int, unsigned) { return 0; }

GUEST_ABI int hle_http_connect_url(int tmpl, const char* url, int) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    if (!http_get(tmpl, HttpKind::Tmpl)) {
        return kHttpInvalidId;
    }
    const int id = http_alloc(HttpKind::Conn, tmpl);
    g_http[id].url = url ? url : "";
    return id;
}
GUEST_ABI int hle_http_request_url(int conn, int method, const char* url, std::uint64_t len) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    if (!http_get(conn, HttpKind::Conn)) {
        return kHttpInvalidId;
    }
    const int id = http_alloc(HttpKind::Req, conn);
    HttpObj& r = g_http[id];
    r.method = method;
    r.url = url ? url : "";
    r.content_len = len;
    r.resp = std::make_shared<Response>();
    return id;
}
GUEST_ABI int hle_http_set_content_len(int id, std::uint64_t len) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    HttpObj* r = http_get(id, HttpKind::Req);
    if (!r) {
        return kHttpInvalidId;
    }
    r->content_len = len;
    return 0;
}
GUEST_ABI int hle_http_set_connect_timeout(int id, unsigned usec) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    HttpObj* o = http_any(id);
    if (!o) {
        return kHttpInvalidId;
    }
    o->connect_timeout_us = usec;
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 4) {
        host_log("sceHttpSetConnectTimeOut id=%d usec=%u", id, usec);
    }
    return 0;
}
GUEST_ABI int hle_http_set_epoll(int id, HttpEpoll* ep, void* user) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    HttpObj* o = http_any(id);
    if (!o) {
        return kHttpInvalidId;
    }
    o->ep = ep;
    o->user = user;
    return 0;
}
GUEST_ABI int hle_http_unset_epoll(int id) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    HttpObj* o = http_any(id);
    if (!o) {
        return kHttpInvalidId;
    }
    o->ep = nullptr;
    return 0;
}
GUEST_ABI int hle_http_delete_request(int id) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    HttpObj* r = http_get(id, HttpKind::Req);
    if (!r) {
        return kHttpInvalidId;
    }
    g_http.erase(id);
    return 0;
}
GUEST_ABI int hle_http_delete_connection(int id) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    g_http.erase(id);
    return 0;
}

// int sceHttpSendRequest(int reqId, const void* postData, size_t size)
GUEST_ABI int hle_http_send(int id, const void* post, std::uint64_t size) {
    std::string url;
    int method;
    Effective eff;
    std::uint64_t content_len;
    std::shared_ptr<Response> resp;
    bool rewritten = false;
    {
        std::lock_guard<std::mutex> lock(g_http_mu);
        HttpObj* r = http_get(id, HttpKind::Req);
        if (!r) {
            return kHttpInvalidId;
        }
        if (r->sent) {
            return kHttpBusy;
        }
        r->sent = true;
        url = rewrite_url(r->url);
        rewritten = url != r->url;
        method = r->method;
        content_len = r->content_len;
        eff = effective_locked(id);
        resp = r->resp;
    }
    std::vector<std::uint8_t> body;
    if (post && size) {
        body.assign(static_cast<const std::uint8_t*>(post), static_cast<const std::uint8_t*>(post) + size);
    }
    if (eff.nonblock) {
        HttpEpoll* ep = eff.ep;
        void* user = eff.user;
        std::thread([=] {
            perform(url, method, eff, body, content_len, resp, rewritten);
            std::uint32_t ev = kEvOut | kEvIn;
            std::uint32_t detail = 0;
            {
                std::lock_guard<std::mutex> lk(resp->mu);
                if (resp->error) {
                    ev = kEvSockErr | kEvHup;
                    detail = static_cast<std::uint32_t>(resp->error);
                }
            }
            post_event(ep, id, user, ev, detail);
        }).detach();
        return 0;
    }
    // Blocking: the whole request on the caller's thread.
    const auto t0 = std::chrono::steady_clock::now();
    perform(url, method, eff, body, content_len, resp, rewritten);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> logs{0};
    if (ms >= 5.0 && logs.fetch_add(1) < 32) {
        const std::string who = thread_name_now();
        host_log("http: a blocking sceHttpSendRequest (request %d) held thread %s for %.0f ms", id, who.c_str(), ms);
    }
    std::lock_guard<std::mutex> lk(resp->mu);
    return resp->error;
}

std::shared_ptr<Response> resp_of(int id, bool* nonblock = nullptr) {
    std::lock_guard<std::mutex> lock(g_http_mu);
    HttpObj* r = http_get(id, HttpKind::Req);
    if (nonblock) *nonblock = r && effective_locked(id).nonblock;
    return r && r->sent ? r->resp : nullptr;
}

// A request sent non-blocking answers SCE_HTTP_ERROR_EAGAIN here until its
// response has come, as the PS4's library does: the game sends its requests
// that way and asks for the status from the main loop, once a frame, until it
// has one. Waiting instead held the main loop for every request's round trip
// - each play-log upload (every 5 s since the private server's map wanted
// them often), message and ghost fetch: 111-145 ms frames against a server
// 23 ms and a TLS handshake away. A blocking request waits (up to 35 s); a
// wait of 5 ms or more is logged with the calling thread (the first 32).
void wait_done(std::unique_lock<std::mutex>& lk, const std::shared_ptr<Response>& resp, const char* fn, int id, bool nonblock) {
    if (resp->done || nonblock) return;
    const auto t0 = std::chrono::steady_clock::now();
    resp->cv.wait_for(lk, std::chrono::seconds(35), [&] { return resp->done; });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> logs{0};
    if (ms >= 5.0 && logs.fetch_add(1) < 32) {
        const std::string who = thread_name_now();
        host_log("http: %s waited %.0f ms for request %d's response on thread %s", fn, ms, id, who.c_str());
    }
}

GUEST_ABI int hle_http_status(int id, int* code) {
    if (!code) {
        return kHttpInvalidValue;
    }
    bool nonblock = false;
    auto resp = resp_of(id, &nonblock);
    if (!resp) {
        return kHttpBeforeSend;
    }
    std::unique_lock<std::mutex> lk(resp->mu);
    wait_done(lk, resp, "sceHttpGetStatusCode", id, nonblock);
    if (!resp->done) {
        return kHttpEagain;
    }
    if (resp->error) {
        return resp->error;
    }
    *code = static_cast<int>(resp->status);
    return 0;
}
GUEST_ABI int hle_http_resp_len(int id, std::uint64_t* len) {
    if (!len) {
        return kHttpInvalidValue;
    }
    bool nonblock = false;
    auto resp = resp_of(id, &nonblock);
    if (!resp) {
        return kHttpBeforeSend;
    }
    std::unique_lock<std::mutex> lk(resp->mu);
    wait_done(lk, resp, "sceHttpGetResponseContentLength", id, nonblock);
    if (!resp->done) {
        return kHttpEagain;
    }
    *len = resp->body.size();
    return 0;  // SCE_HTTP_CONTENTLEN_EXIST
}
GUEST_ABI int hle_http_read(int id, void* data, std::uint64_t size) {
    if (!data) {
        return kHttpInvalidValue;
    }
    bool nonblock = false;
    auto resp = resp_of(id, &nonblock);
    if (!resp) {
        return kHttpBeforeSend;
    }
    std::unique_lock<std::mutex> lk(resp->mu);
    wait_done(lk, resp, "sceHttpReadData", id, nonblock);
    if (!resp->done) {
        return kHttpEagain;
    }
    if (resp->error) {
        return resp->error;
    }
    const std::size_t avail = resp->body.size() - resp->read_off;
    const std::size_t n = static_cast<std::size_t>(size < avail ? size : avail);
    std::memcpy(data, resp->body.data() + resp->read_off, n);
    resp->read_off += n;
    return static_cast<int>(n);
}

}  // namespace

void hle_register_http() {
#define REG(name, fn) register_hle_fn(name, reinterpret_cast<void*>(fn))
    REG("sceSslInit", hle_ssl_init);
    REG("sceSslTerm", hle_ssl_term);
    REG("sceHttpInit", hle_http_init);
    REG("sceHttpTerm", hle_http_term);
    REG("sceHttpCreateTemplate", hle_http_create_template);
    REG("sceHttpDeleteTemplate", hle_http_delete_template);
    REG("sceHttpSetNonblock", hle_http_set_nonblock);
    REG("sceHttpCreateEpoll", hle_http_create_epoll);
    REG("sceHttpDestroyEpoll", hle_http_destroy_epoll);
    REG("sceHttpAddRequestHeader", hle_http_add_header);
    REG("sceHttpWaitRequest", hle_http_wait_request);
    REG("sceHttpAbortWaitRequest", hle_http_abort_wait);
    REG("sceHttpsEnableOption", hle_https_enable);
    REG("sceHttpsDisableOption", hle_https_disable);
    REG("sceHttpCreateConnectionWithURL", hle_http_connect_url);
    REG("sceHttpCreateRequestWithURL", hle_http_request_url);
    REG("sceHttpSetRequestContentLength", hle_http_set_content_len);
    REG("sceHttpSetConnectTimeOut", hle_http_set_connect_timeout);
    REG("sceHttpSetEpoll", hle_http_set_epoll);
    REG("sceHttpUnsetEpoll", hle_http_unset_epoll);
    REG("sceHttpDeleteRequest", hle_http_delete_request);
    REG("sceHttpDeleteConnection", hle_http_delete_connection);
    REG("sceHttpSendRequest", hle_http_send);
    REG("sceHttpGetStatusCode", hle_http_status);
    REG("sceHttpGetResponseContentLength", hle_http_resp_len);
    REG("sceHttpReadData", hle_http_read);
#undef REG
}
