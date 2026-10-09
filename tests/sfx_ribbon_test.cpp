// The effect ribbons' writers (decomp/sfx_ribbon.cpp) against the game's own
// code: the eboot's first segment - code and the constants it reads - is
// mapped, and sub_2cce7b0 and sub_2cceec0 run there on the same random
// strips as ours, every byte of the vertices compared. Both are leaves that
// reach only that segment, rip-relative, so they run wherever it lands.
// Skips without the 1.09 eboot (BBHOST_EBOOT, or eboot-109-decrypted.bin in
// the checkout).
#include "../src/decomp/sfx_ribbon.cpp"

#include "core/sha256.h"
#include "engine/addr.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>

#include <sys/mman.h>

void decomp_add(const DecompFunction&) {}

namespace {

constexpr int kTestSkip = 77;

std::mt19937_64 g_rng(0x5f3759df);

float uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }
bool chance(double p) { return std::uniform_real_distribution<double>(0, 1)(g_rng) < p; }

// Now and then a value no real ribbon has, where the bits are the question.
bool g_specials = false;
float value(float lo, float hi) {
    if (g_specials && chance(0.15)) {
        static const float kSpecial[] = {0.0f,     -0.0f,         1.0f,    -1.0f,   0.5f,    1e-40f,  -1e-40f, FLT_MAX,
                                         -FLT_MAX, 1e20f,         -1e20f,  127.0f,  255.0f,  2.0f,    -2.0f,
                                         std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                         std::numeric_limits<float>::quiet_NaN()};
        return kSpecial[std::uniform_int_distribution<std::size_t>(0, std::size(kSpecial) - 1)(g_rng)];
    }
    return uniform(lo, hi);
}

struct Strip {
    static constexpr int kPoints = 16;  // the writers read up to point n + 1
    float born[kPoints], px[kPoints], py[kPoints], pz[kPoints], u_offset[kPoints], nx[kPoints], ny[kPoints], nz[kPoints];
    u32 packed[kPoints];
    u32 n, first, total, per_point;
    bool offsets;
    float f[16];
};

Strip random_strip() {
    Strip s{};
    s.n = chance(0.9) ? static_cast<u32>(std::uniform_int_distribution<int>(1, 3)(g_rng))
                      : static_cast<u32>(std::uniform_int_distribution<int>(4, 7)(g_rng));
    const float now = value(0, 100);
    const float base = std::isfinite(now) ? now : 50.0f;
    for (int i = 0; i < Strip::kPoints; ++i) {
        s.born[i] = value(base - 6, base + 1);
        s.px[i] = value(-300, 300);
        s.py[i] = value(-300, 300);
        s.pz[i] = value(-300, 300);
        s.u_offset[i] = value(-1, 1);
        s.nx[i] = value(-1, 1);
        s.ny[i] = value(-1, 1);
        s.nz[i] = value(-1, 1);
        s.packed[i] = static_cast<u32>(g_rng());
    }
    if (chance(0.8)) {
        s.first = 4 * static_cast<u32>(std::uniform_int_distribution<int>(0, 40)(g_rng));
        s.total = s.first + s.n;
    } else {
        s.first = static_cast<u32>(g_rng());
        s.total = static_cast<u32>(g_rng());
    }
    s.per_point = chance(0.45) ? 1 : chance(0.9) ? 0 : 2;
    s.offsets = chance(0.7);
    s.f[0] = now;
    for (int i = 1; i < 16; ++i) s.f[i] = value(-2, 6);
    return s;
}

// Floats where both sides made a NaN, of whichever payload - an operand
// order the compiler was free to swap - count as the same.
bool same_vertices(const std::uint8_t* a, const std::uint8_t* b, std::size_t len, std::size_t* where) {
    for (std::size_t k = 0; k < len; k += 4) {
        u32 x, y;
        std::memcpy(&x, a + k, 4);
        std::memcpy(&y, b + k, 4);
        if (x == y) continue;
        const std::size_t field = k % kVertex;
        const bool is_float = field <= 0x08 || field == 0x20;
        if (is_float && std::isnan(std::bit_cast<float>(x)) && std::isnan(std::bit_cast<float>(y))) continue;
        *where = k;
        return false;
    }
    return true;
}

int run(const char* name, void* game_fn, bool along, int cases) {
    int bad = 0;
    for (int c = 0; c < cases; ++c) {
        const Strip s = random_strip();
        const std::size_t len = static_cast<std::size_t>(s.n) * 2 * kVertex;
        std::uint8_t theirs[16 * kVertex], ours[16 * kVertex];
        std::memset(theirs, 0xa5, sizeof theirs);
        std::memset(ours, 0xa5, sizeof ours);
        const float* off = s.offsets ? s.u_offset : nullptr;
        const float* f = s.f;
        if (!along) {
            reinterpret_cast<FacingFn>(game_fn)(theirs, s.n, s.born, s.px, s.py, s.pz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                                                s.packed, off, f[8], f[9], f[10], f[11], f[12], s.first, s.total, f[13], f[14]);
            sfx_ribbon_facing(ours, s.n, s.born, s.px, s.py, s.pz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], s.packed, off, f[8],
                              f[9], f[10], f[11], f[12], s.first, s.total, f[13], f[14]);
        } else {
            reinterpret_cast<AlongFn>(game_fn)(theirs, s.n, s.born, s.px, s.py, s.pz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                                               s.packed, off, s.nx, s.ny, s.nz, s.per_point, f[8], f[9], s.first, s.total, f[10],
                                               f[11]);
            sfx_ribbon_along(ours, s.n, s.born, s.px, s.py, s.pz, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], s.packed, off, s.nx,
                             s.ny, s.nz, s.per_point, f[8], f[9], s.first, s.total, f[10], f[11]);
        }
        std::size_t k = 0;
        // Past the strip neither may have written.
        if (!same_vertices(theirs, ours, len, &k) || std::memcmp(theirs + len, ours + len, sizeof theirs - len) != 0) {
            if (++bad <= 8) {
                u32 a = 0, b = 0;
                std::memcpy(&a, theirs + k, 4);
                std::memcpy(&b, ours + k, 4);
                std::printf("%s: case %d (n %u, first %u, total %u%s) differs at vertex %zu +0x%zx: the game's 0x%08x, ours 0x%08x\n",
                            name, c, s.n, s.first, s.total, g_specials ? ", special values" : "", k / kVertex, k % kVertex, a, b);
            }
        }
    }
    return bad;
}

}  // namespace

int main() {
    const char* env = std::getenv("BBHOST_EBOOT");
    const std::string path = env && *env ? env : std::string(BBHOST_SOURCE_DIR) + "/eboot-109-decrypted.bin";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("sfx_ribbon_test skipped: no eboot at %s (BBHOST_EBOOT)\n", path.c_str());
        return kTestSkip;
    }
    const std::vector<std::uint8_t> elf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (sha256_hex(elf.data(), elf.size()) != kEboot109Sha256) {
        std::printf("sfx_ribbon_test skipped: %s is not the 1.09 eboot\n", path.c_str());
        return kTestSkip;
    }
    // The first PT_LOAD: code and read-only data from virtual address 0.
    std::uint64_t phoff = 0;
    std::uint16_t phnum = 0;
    std::memcpy(&phoff, elf.data() + 0x20, 8);
    std::memcpy(&phnum, elf.data() + 0x38, 2);
    std::uint64_t off = 0, filesz = 0;
    for (int i = 0; i < phnum; ++i) {
        const std::uint8_t* ph = elf.data() + phoff + 56 * i;
        std::uint32_t type = 0;
        std::uint64_t vaddr = 0;
        std::memcpy(&type, ph, 4);
        std::memcpy(&vaddr, ph + 16, 8);
        if (type == 1 && vaddr == 0) {
            std::memcpy(&off, ph + 8, 8);
            std::memcpy(&filesz, ph + 32, 8);
            break;
        }
    }
    if (!filesz || off + filesz > elf.size()) {
        std::printf("sfx_ribbon_test: no code segment at address 0 in %s\n", path.c_str());
        return 1;
    }
    const std::size_t size = (filesz + 0xfff) & ~std::size_t{0xfff};
    void* seg = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (seg == MAP_FAILED) return 1;
    std::memcpy(seg, elf.data() + off, filesz);
    if (mprotect(seg, size, PROT_READ | PROT_EXEC) != 0) return 1;
    auto* base = static_cast<std::uint8_t*>(seg);
    std::uint8_t* facing = base + (0x2cce7b0 - kPreferredGuestSlide);
    std::uint8_t* along = base + (0x2cceec0 - kPreferredGuestSlide);
    if (std::memcmp(facing, kFacingEntry, sizeof kFacingEntry) != 0 || std::memcmp(along, kAlongEntry, sizeof kAlongEntry) != 0) {
        std::printf("sfx_ribbon_test: the writers' entries are not where the decomp expects them\n");
        return 1;
    }
    int bad = 0;
    constexpr int kCases = 200000;
    for (const bool specials : {false, true}) {
        g_specials = specials;
        bad += run("sub_2cce7b0", facing, false, kCases);
        bad += run("sub_2cceec0", along, true, kCases);
    }
    std::printf("sfx_ribbon_test: %d strips each way (ordinary values, then special ones), %d differ\n", 2 * kCases, bad);
    return bad ? 1 : 0;
}
