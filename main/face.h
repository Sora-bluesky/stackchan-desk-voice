#pragma once
#include <cstdint>
// Code-drawn face (M5GFX): blue cloud-shaped face built from 7 circles +
// two tilted white capsule eyes, no mouth. Draws into a 320x240 RGB565 sprite in PSRAM.
namespace face {

enum class State : uint8_t {
    Idle, Blink, Listening, Speaking, Petted, Sleepy, Sleep, Thinking,
    // not implemented yet (rendered as Idle): look_*, wink, surprised, wakeup, joy
    LookLeft, LookRight, LookUp, Wink, Surprised, Wakeup, Joy,
};
const char* name(State s);

bool init();                         // allocates the sprite (PSRAM)
void set_state(State s, uint32_t now_ms);
State state();
void note_activity(uint32_t now_ms);  // cancel idle flourishes, even without a state change
void hop(uint32_t now_ms);
void set_speech_level(float v);      // 0..1 (speaker RMS) for Speaking
void set_gaze_target(int x, int y);   // screen coordinates; display only
void clear_gaze_target();
// draws one frame; overlay lines may be null
void render(uint32_t now_ms, const char (*overlay)[52], int overlay_n, const char* status);

}  // namespace face
