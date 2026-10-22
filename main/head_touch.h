#pragma once
#include <cstdint>
// Si12T 3-zone head touch (I2C 0x68 on the internal bus).
// Register init and the weighted position (-100..100) follow the factory firmware
// (MIT, M5Stack): firmware/main/hal/drivers/Si12T/Si12T.cpp, hal_head_touch.cpp.
namespace head_touch {
enum class Event : uint8_t { None, Stroke, DoubleTap };
bool init();
// poll every ~15 ms from the head task (boot also polls). ignore=true while servos move (+300 ms)
Event poll(uint32_t now_ms, bool ignore);
void note_screen_touch(uint32_t now_ms);
bool touched();        // current raw state
int position();        // last touched zone position, -100..100
bool present();
}
