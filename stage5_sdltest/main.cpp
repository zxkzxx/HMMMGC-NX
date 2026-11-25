#include <SDL3/SDL.h>
#include <switch.h>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>
#include <vector>

static void push_beep(SDL_AudioStream* s, float hz, float ratio) {
    if (!s) return;
    SDL_SetAudioStreamFrequencyRatio(s, ratio);
    std::vector<int16_t> pcm(8000 * 2);  // 0.25 detik @ 32 kHz stereo
    for (size_t i = 0; i < 8000; ++i) {
        int16_t v = static_cast<int16_t>(std::sin(2.0 * 3.14159265 * hz * i / 32000.0) * 14000.0);
        pcm[i * 2] = v;
        pcm[i * 2 + 1] = v;
    }
    SDL_PutAudioStreamData(s, pcm.data(), static_cast<int>(pcm.size() * sizeof(int16_t)));
    SDL_ResumeAudioStreamDevice(s);
}

int main() {
    consoleInit(nullptr);
    mkdir("/switch", 0777);
    mkdir("/switch/hmmmgc", 0777);
    FILE* logf = fopen("/switch/hmmmgc/sdltest.log", "w");

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD);
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    SDL_AudioSpec dev{};
    int dev_frames = 0;
    if (stream) SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &dev, &dev_frames);

    printf("=== HMMMGC Stage 5: SDL3 Audio + Input Test ===\n");
    printf("Driver : %s | Device: %d Hz (%d frames)\n", SDL_GetCurrentAudioDriver(), dev.freq, dev_frames);
    printf("[A] Bunyi 32kHz (1.0x) | [B] Bunyi Cepat (1.5x)\n");
    printf("[X] Volume 100%%/30%%    | [Y] Tes Getar (HD Rumble)\n");
    printf("[+] Keluar\n\n");
    if (logf) {
        fprintf(logf, "SDL3 AudioStream: %s | driver=%s | dev=%dHz/%d\n",
                stream ? "OK" : "FAIL", SDL_GetCurrentAudioDriver(), dev.freq, dev_frames);
        fflush(logf);
    }

    SDL_Gamepad* pad = nullptr;
    bool running = true, low_vol = false;
    bool prev_a = false, prev_b = false, prev_x = false, prev_y = false;
    int a_hits = 0, b_hits = 0, y_hits = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            else if (ev.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(ev.gdevice.which);
            else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED && pad) { SDL_CloseGamepad(pad); pad = nullptr; }
        }
        if (pad) {
            bool a = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
            bool b = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
            bool x = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
            bool y = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) running = false;
            if (a && !prev_a) { push_beep(stream, 440.0f, 1.0f); ++a_hits; }
            if (b && !prev_b) { push_beep(stream, 440.0f, 1.5f); ++b_hits; }
            if (x && !prev_x) { low_vol = !low_vol; SDL_SetAudioStreamGain(stream, low_vol ? 0.3f : 1.0f); }
            if (y && !prev_y) { SDL_RumbleGamepad(pad, 30000, 30000, 250); ++y_hits; }
            prev_a = a; prev_b = b; prev_x = x; prev_y = y;

            printf("\rPad: OK | LStick:(%6d,%6d) ZL:%5d ZR:%5d Vol:%3d%%   ",
                   SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                   SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY),
                   SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                   SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
                   low_vol ? 30 : 100);
        }
        consoleUpdate(nullptr);
        SDL_Delay(16);
    }

    if (logf) {
        fprintf(logf, "Pad=%s | A=%d B=%d Y=%d\n", pad ? "OK" : "NONE", a_hits, b_hits, y_hits);
        fclose(logf);
    }
    if (pad) SDL_CloseGamepad(pad);
    if (stream) SDL_DestroyAudioStream(stream);
    SDL_Quit();
    consoleExit(nullptr);
    return 0;
}
