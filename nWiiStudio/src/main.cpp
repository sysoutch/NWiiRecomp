#include "raylib.h"
#include "rlImGui.h"
#include "imgui.h"
#include "GUI.hpp"
#include "StudioState.hpp"
#include <iostream>
#include <vector>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "pl_mpeg.h"

struct VideoState {
    std::vector<uint8_t> rgbBuffer;
    bool frameReady = false;
};

#include <mutex>

std::vector<float> globalAudioBuffer;
std::mutex globalAudioMutex;

void on_video_frame(plm_t *plm, plm_frame_t *frame, void *user) {
    VideoState *state = (VideoState *)user;
    plm_frame_to_rgb(frame, state->rgbBuffer.data(), plm_get_width(plm) * 3);
    state->frameReady = true;
}

void on_audio_frame(plm_t *plm, plm_samples_t *frame, void *user) {
    std::lock_guard<std::mutex> lock(globalAudioMutex);
    globalAudioBuffer.insert(globalAudioBuffer.end(), frame->interleaved, frame->interleaved + (frame->count * 2));
}

void AudioStreamCallback(void *bufferData, unsigned int frames) {
    float *out = (float *)bufferData;
    int requiredFloats = frames * 2;
    
    std::lock_guard<std::mutex> lock(globalAudioMutex);
    
    if (globalAudioBuffer.size() >= requiredFloats) {
        memcpy(out, globalAudioBuffer.data(), requiredFloats * sizeof(float));
        globalAudioBuffer.erase(globalAudioBuffer.begin(), globalAudioBuffer.begin() + requiredFloats);
    } else {
        memcpy(out, globalAudioBuffer.data(), globalAudioBuffer.size() * sizeof(float));
        memset(out + globalAudioBuffer.size(), 0, (requiredFloats - globalAudioBuffer.size()) * sizeof(float));
        globalAudioBuffer.clear();
    }
}

int main(int argc, char** argv) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(1280, 720, "nWiiStudio - PowerPC Analyzer");
    InitAudioDevice();
    SetTargetFPS(60);

    const char* paths[] = {
        "/Users/vovavovchok/NWiiRecomp/nWiiStudio/assets/logo-nwiirecomp.mpg",
        "assets/logo-nwiirecomp.mpg",
        "nWiiStudio/assets/logo-nwiirecomp.mpg",
        "../nWiiStudio/assets/logo-nwiirecomp.mpg",
        "../../nWiiStudio/assets/logo-nwiirecomp.mpg",
        "../../../nWiiStudio/assets/logo-nwiirecomp.mpg"
    };
    
    plm_t *plm = nullptr;
    for (const char* path : paths) {
        if (FileExists(path)) {
            plm = plm_create_with_filename(path);
            if (plm) break;
        }
    }

    if (plm) {
        int width = plm_get_width(plm);
        int height = plm_get_height(plm);
        plm_set_loop(plm, 0); // Play only once
        
        VideoState vstate;
        vstate.rgbBuffer.resize(width * height * 3);
        plm_set_video_decode_callback(plm, on_video_frame, &vstate);
        
        Image image = {
            .data = vstate.rgbBuffer.data(),
            .width = width,
            .height = height,
            .mipmaps = 1,
            .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8
        };
        Texture2D texture = LoadTextureFromImage(image);
        
        AudioStream audioStream = {0};
        
        if (plm_get_num_audio_streams(plm) > 0) {
            SetAudioStreamBufferSizeDefault(4096);
            audioStream = LoadAudioStream(plm_get_samplerate(plm), 32, 2);
            SetAudioStreamCallback(audioStream, AudioStreamCallback);
            PlayAudioStream(audioStream);
            plm_set_audio_decode_callback(plm, on_audio_frame, nullptr);
        }
        
        while (!WindowShouldClose() && !plm_has_ended(plm)) {
            float dt = GetFrameTime();
            if (dt > 0.1f) dt = 0.016f; // Clamp delta time to avoid skipping on lag
            plm_decode(plm, dt);
            
            if (vstate.frameReady) {
                UpdateTexture(texture, vstate.rgbBuffer.data());
                vstate.frameReady = false;
            }
            
            BeginDrawing();
            ClearBackground(BLACK);
            DrawTexture(texture, (GetScreenWidth() - width) / 2, (GetScreenHeight() - height) / 2, WHITE);
            EndDrawing();
        }
        
        if (plm_get_num_audio_streams(plm) > 0) {
            UnloadAudioStream(audioStream);
        }
        UnloadTexture(texture);
        plm_destroy(plm);
    }

    rlImGuiBeginInitImGui();
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    StudioState state;
    GUI::ApplySettings(state);
    
    rlImGuiEndInitImGui();

    if (argc > 1) {
        state.LoadUnpackedGame(argv[1]);
        state.StartAnalysis();
    }

    while (!WindowShouldClose() && !GUI::WantsQuit()) {
        if (state.pendingFontRebuild.load()) {
            state.pendingFontRebuild = false;
        }

        BeginDrawing();
        ClearBackground(GetColor(0x1a1a1aff));

        rlImGuiBegin();

        GUI::DrawStudio(state);

        rlImGuiEnd();
        EndDrawing();
    }

    state.SaveSettings();

    if (state.workerThread.valid()) {
        state.workerThread.wait();
    }

    rlImGuiShutdown();
    CloseAudioDevice();
    CloseWindow();

    return 0;
}
