#include <switch.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace {

constexpr uint32_t kPageShift = 20;
constexpr uint32_t kPageCount = 1u << (32 - kPageShift);

constexpr uint32_t kMem1Size = 24u * 1024u * 1024u;
constexpr uint32_t kPhysBase = 0x00000000u;
constexpr uint32_t kCachedBase = 0x80000000u;
constexpr uint32_t kUncachedBase = 0xC0000000u;
constexpr uint32_t kRuntimeBase = 0x81800000u;
constexpr uint32_t kRuntimeSize = 1u * 1024u * 1024u;
constexpr uint32_t kLockedCacheBase = 0xE0000000u;
constexpr uint32_t kLockedCacheSize = 16u * 1024u;
constexpr uint32_t kMmioBase = 0xCC000000u;
constexpr uint32_t kMmioSize = 0x00010000u;

uintptr_t g_fullPageBias[kPageCount]{};
uint8_t* g_mem1 = nullptr;
uint8_t* g_runtimeMem = nullptr;
uint8_t* g_lockedCache = nullptr;

uint32_t g_mmioReads = 0;
uint32_t g_mmioWrites = 0;
uint32_t g_mmioLastWritten = 0;

FILE* g_log = nullptr;

void log_both(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_log != nullptr) {
        va_start(args, fmt);
        vfprintf(g_log, fmt, args);
        va_end(args);
        fflush(g_log);
    }
}

void publish_view(uint32_t guest_base, uint8_t* host_ptr, uint32_t size) {
    const uint32_t first_page = guest_base >> kPageShift;
    const uint32_t pages = size >> kPageShift;
    const uintptr_t bias = (reinterpret_cast<uintptr_t>(host_ptr) - guest_base) + 1u;
    for (uint32_t p = first_page; p < first_page + pages; ++p) {
        g_fullPageBias[p] = bias;
    }
}

__attribute__((noinline)) uint32_t read32_slow(uint32_t addr) {
    if (addr - kMmioBase < kMmioSize) {
        ++g_mmioReads;
        return g_mmioLastWritten ^ 0xA5A50000u;
    }
    if (addr - kLockedCacheBase <= kLockedCacheSize - 4) {
        uint32_t raw = 0;
        std::memcpy(&raw, g_lockedCache + (addr - kLockedCacheBase), 4);
        return __builtin_bswap32(raw);
    }
    return 0;
}

__attribute__((noinline)) void write32_slow(uint32_t addr, uint32_t val) {
    if (addr - kMmioBase < kMmioSize) {
        ++g_mmioWrites;
        g_mmioLastWritten = val;
        return;
    }
    if (addr - kLockedCacheBase <= kLockedCacheSize - 4) {
        const uint32_t swapped = __builtin_bswap32(val);
        std::memcpy(g_lockedCache + (addr - kLockedCacheBase), &swapped, 4);
    }
}

__attribute__((always_inline)) inline uint32_t guest_read32(uint32_t addr) {
    const uintptr_t bias = g_fullPageBias[addr >> kPageShift];
    if (bias == 0) [[unlikely]] {
        return read32_slow(addr);
    }
    uint32_t raw = 0;
    std::memcpy(&raw, reinterpret_cast<const uint8_t*>((bias - 1u) + addr), 4);
    return __builtin_bswap32(raw);
}

__attribute__((always_inline)) inline void guest_write32(uint32_t addr, uint32_t val) {
    const uintptr_t bias = g_fullPageBias[addr >> kPageShift];
    if (bias == 0) [[unlikely]] {
        write32_slow(addr, val);
        return;
    }
    const uint32_t swapped = __builtin_bswap32(val);
    std::memcpy(reinterpret_cast<uint8_t*>((bias - 1u) + addr), &swapped, 4);
}

}  // namespace

int main() {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    mkdir("/switch", 0777);
    mkdir("/switch/hmmmgc", 0777);
    g_log = fopen("/switch/hmmmgc/memtest.log", "w");

    log_both("=== HMMMGC Switch Stage 3: Guest Memory Test ===\n");

    g_mem1 = static_cast<uint8_t*>(std::calloc(1, kMem1Size + 4096));
    g_runtimeMem = static_cast<uint8_t*>(std::calloc(1, kRuntimeSize + 4096));
    g_lockedCache = static_cast<uint8_t*>(std::calloc(1, kLockedCacheSize));

    const bool alloc_ok = (g_mem1 != nullptr && g_runtimeMem != nullptr && g_lockedCache != nullptr);
    log_both("1. Heap alloc (24+1 MiB) : %s\n", alloc_ok ? "PASS" : "FAIL");

    if (alloc_ok) {
        publish_view(kPhysBase, g_mem1, kMem1Size);
        publish_view(kCachedBase, g_mem1, kMem1Size);
        publish_view(kUncachedBase, g_mem1, kMem1Size);
        publish_view(kRuntimeBase, g_runtimeMem, kRuntimeSize);

        guest_write32(0x80001234u, 0x12345678u);
        const uint32_t from_phys = guest_read32(0x00001234u);
        const uint32_t from_uncached = guest_read32(0xC0001234u);
        const bool big_endian_ok = (g_mem1[0x1234] == 0x12 && g_mem1[0x1237] == 0x78);
        const bool alias_ok = (from_phys == 0x12345678u && from_uncached == 0x12345678u && big_endian_ok);
        log_both("2. MEM1 3-way alias + BE : %s (phys=0x%08X uncached=0x%08X)\n",
                 alias_ok ? "PASS" : "FAIL", from_phys, from_uncached);

        guest_write32(0x81800100u, 0xCAFEBABEu);
        guest_write32(0xE0000040u, 0xDEADBEEFu);
        const bool extra_ok = (guest_read32(0x81800100u) == 0xCAFEBABEu &&
                               guest_read32(0xE0000040u) == 0xDEADBEEFu);
        log_both("3. RuntimeMem + LC       : %s\n", extra_ok ? "PASS" : "FAIL");

        guest_write32(0xCC006800u, 0x00001337u);
        const uint32_t mmio_val = guest_read32(0xCC006800u);
        const bool mmio_ok = (g_mmioWrites == 1 && g_mmioReads == 1 && mmio_val == 0xA5A51337u);
        log_both("4. MMIO slow-path        : %s (val=0x%08X)\n", mmio_ok ? "PASS" : "FAIL", mmio_val);

        constexpr uint32_t kIters = 10000000u;
        const u64 t0 = armGetSystemTick();
        uint32_t acc = 0;
        for (uint32_t i = 0; i < kIters; ++i) {
            const uint32_t addr = 0x80010000u + ((i & 0x3FFFu) << 2);
            guest_write32(addr, i ^ acc);
            acc += guest_read32(addr);
        }
        const u64 t1 = armGetSystemTick();
        const double ms = static_cast<double>(armTicksToNs(t1 - t0)) / 1e6;
        log_both("5. Bench 20M ops (R+W)   : %.2f ms (acc=0x%08X)\n", ms, acc);
        log_both("   Rata-rata per akses   : %.2f ns\n", (ms * 1e6) / (2.0 * kIters));
    }

    std::free(g_lockedCache);
    std::free(g_runtimeMem);
    std::free(g_mem1);

    log_both("\nTekan tombol + (Plus) untuk keluar.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        consoleUpdate(nullptr);
    }

    if (g_log != nullptr) {
        fclose(g_log);
        g_log = nullptr;
    }
    consoleExit(nullptr);
    return 0;
}
