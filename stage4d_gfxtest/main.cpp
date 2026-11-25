#include <switch.h>
#include <pthread.h>
#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "dk.h"
#include "dk_aurora.h"

void JKRPcBeginHostAlloc() {}

extern "C" {
void cos_switch_flush_logs(void) {
    fflush(stdout);
    fflush(stderr);
}
void cos_switch_thread_role(int) {}

int __real_pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg);
int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    pthread_attr_t local;
    if (attr) {
        local = *attr;
    } else {
        pthread_attr_init(&local);
    }
    size_t sz = 0;
    pthread_attr_getstacksize(&local, &sz);
    if (sz < 2u * 1024u * 1024u) {
        pthread_attr_setstacksize(&local, 2u * 1024u * 1024u);
    }
    int r = __real_pthread_create(t, &local, fn, arg);
    if (!attr) pthread_attr_destroy(&local);
    return r;
}
}

struct TestState {
    uint64_t frames = 0;
    double fps = 0.0;
    bool uam_ok = false;
    double uam_ms = 0.0;
};

static void picture_cb(void* user, int stage) {
    if (stage != 1) return;
    auto* st = static_cast<TestState*>(user);

    const float t = static_cast<float>(st->frames) * 0.03f;
    const float r = 0.08f + 0.06f * std::sin(t);
    const float g = 0.16f + 0.10f * std::sin(t + 2.0f);
    const float b = 0.30f + 0.15f * std::cos(t * 0.7f);

    swdk::set_view(0, 0, swdk::R.width, swdk::R.height);
    dkCmdBufClearColorFloat(swdk::R.cmd, 0, DkColorMask_RGBA, r, g, b, 1.0f);

    char l0[96], l1[96], l2[96], l3[96];
    std::snprintf(l0, sizeof(l0), "HMMMGC SWITCH - STAGE 4D DEKO3D OK");
    std::snprintf(l1, sizeof(l1), "RES: %ux%u | FRAME: %llu | FPS: %.1f",
                  swdk::R.width, swdk::R.height,
                  static_cast<unsigned long long>(st->frames), st->fps);
    std::snprintf(l2, sizeof(l2), "UAM LIVE SHADER: %s (%.1f MS)",
                  st->uam_ok ? "PASS" : "FAIL", st->uam_ms);
    std::snprintf(l3, sizeof(l3), "PRESS [+] BUTTON TO EXIT");

    const char* lines[] = {l0, l1, l2, l3};
    const float fg[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float bg[4] = {0.0f, 0.0f, 0.0f, 0.75f};
    swdk::draw_text(48, 48, 4, lines, 4, fg, bg);
}

int main() {
    mkdir("/switch", 0777);
    mkdir("/switch/hmmmgc", 0777);
    freopen("/switch/hmmmgc/gfxtest.log", "w", stderr);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    swdk::dklog("=== HMMMGC Switch Stage 4D: deko3d + uamlib Test ===\n");
    aurora_switch_dk_init(1);

    TestState st{};
    const std::string test_vs =
        "#version 460\n"
        "layout(location = 0) in vec2 aPos;\n"
        "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    const u64 t_uam0 = armGetSystemTick();
    swdk::ShaderEntry* e = swdk::shader_entry(swdk::ShaderStage::Vertex, 0x1122334455667788ULL);
    swdk::shader_compile(e, test_vs, true);
    st.uam_ok = swdk::shader_wait(e);
    st.uam_ms = static_cast<double>(armTicksToNs(armGetSystemTick() - t_uam0)) / 1e6;
    swdk::dklog("UAM live compile: %s (%.2f ms)\n", st.uam_ok ? "PASS" : "FAIL", st.uam_ms);

    swdk::set_picture(picture_cb, &st);

    const u64 t_start = armGetSystemTick();
    u64 t_fps = t_start;
    uint64_t fps_frames = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        aurora_switch_dk_present(nullptr);
        ++st.frames;
        ++fps_frames;

        const u64 now = armGetSystemTick();
        const double dt = static_cast<double>(armTicksToNs(now - t_fps)) / 1e9;
        if (dt >= 0.5) {
            st.fps = static_cast<double>(fps_frames) / dt;
            fps_frames = 0;
            t_fps = now;
        }
    }

    const double total_s = static_cast<double>(armTicksToNs(armGetSystemTick() - t_start)) / 1e9;
    swdk::dklog("DONE: frames=%llu total=%.2fs avg_fps=%.1f uam=%s\n",
                static_cast<unsigned long long>(st.frames),
                total_s,
                total_s > 0.0 ? st.frames / total_s : 0.0,
                st.uam_ok ? "PASS" : "FAIL");
    aurora_switch_dk_shutdown();
    cos_switch_flush_logs();
    return 0;
}
