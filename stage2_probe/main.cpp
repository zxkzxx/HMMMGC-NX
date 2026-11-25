#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

extern "C" char* fake_heap_end;

namespace {

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

double read_clock_mhz(PcvModuleId mod, bool clkrst_ok) {
    if (!clkrst_ok) return -1.0;
    ClkrstSession session{};
    if (R_FAILED(clkrstOpenSession(&session, mod, 3))) {
        return -1.0;
    }
    u32 hz = 0;
    const Result rc = clkrstGetClockRate(&session, &hz);
    clkrstCloseSession(&session);
    return R_SUCCEEDED(rc) ? static_cast<double>(hz) / 1000000.0 : -1.0;
}

}  // namespace

int main(int argc, char** argv) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    mkdir("/switch", 0777);
    mkdir("/switch/hmmmgc", 0777);
    g_log = fopen("/switch/hmmmgc/probe.log", "w");

    log_both("=== HMMMGC Switch Stage 2 Probe ===\n");
    log_both("argv[0]       : %s\n", (argc > 0 && argv[0]) ? argv[0] : "(none)");

    const AppletType atype = appletGetAppletType();
    const bool title_mode = (atype == AppletType_Application || atype == AppletType_SystemApplication);
    log_both("Applet mode   : %s (type=%d)\n", title_mode ? "TITLE MODE (OK)" : "APPLET MODE (RAM KECIL!)", static_cast<int>(atype));

    const AppletOperationMode op_mode = appletGetOperationMode();
    log_both("Console mode  : %s\n", op_mode == AppletOperationMode_Console ? "DOCKED" : "HANDHELD");

    char* heap_top = static_cast<char*>(sbrk(0));
    size_t free_mib = 0;
    if (fake_heap_end != nullptr && heap_top != nullptr && heap_top != reinterpret_cast<char*>(-1) && fake_heap_end > heap_top) {
        free_mib = static_cast<size_t>(fake_heap_end - heap_top) >> 20;
    }
    log_both("Free heap     : %zu MiB\n", free_mib);

    const bool clkrst_ok = R_SUCCEEDED(clkrstInitialize());
    const double cpu_mhz = read_clock_mhz(PcvModuleId_CpuBus, clkrst_ok);
    const double gpu_mhz = read_clock_mhz(PcvModuleId_GPU, clkrst_ok);
    const double emc_mhz = read_clock_mhz(PcvModuleId_EMC, clkrst_ok);
    if (clkrst_ok) {
        clkrstExit();
    }
    log_both("Stock clocks  : CPU %.1f MHz | GPU %.1f MHz | RAM %.1f MHz\n", cpu_mhz, gpu_mhz, emc_mhz);
    log_both("SD log file   : %s\n", g_log ? "OK (/switch/hmmmgc/probe.log)" : "GAGAL DIBUKA");
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
