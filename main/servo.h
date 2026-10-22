#pragma once
#include <cstdint>
// Feetech SCS0009 x2 on the StackChan base (half-duplex TTL bus, Servo_TX G6 / Servo_RX G7, 1 Mbps).
// IDs / zero positions / 0.3125 deg per step / big-endian words follow the factory firmware
// (MIT, M5Stack): firmware/main/hal/hal_servo.cpp + drivers/FTServo_Arduino (SCSCL, End=1).
// Compiled always, active only with CONFIG_STACKCHAN_SERVO=y.
namespace servo {
constexpr int TILT_MIN_DEG = 5;
constexpr int TILT_MAX_DEG = 85;
inline int clamp_tilt(int deg) { return deg < TILT_MIN_DEG ? TILT_MIN_DEG : (deg > TILT_MAX_DEG ? TILT_MAX_DEG : deg); }

void init();                 // starts servo task on core 1 (no-op if disabled)
bool enabled();
void set_idle_allowed(bool allowed);   // behavior: true only in idle (not speaking/listening/call activity)
void abort_motion();         // speech detected: stop now, unmute mic
void set_sleep(bool sleep);  // sleep -> torque off
bool busy_recent();          // moving or within 300 ms after (for head-touch ignore)
bool forced_free();          // torque was cut because someone moved the head by hand
}
