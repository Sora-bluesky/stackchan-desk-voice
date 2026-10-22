// StackChan desk voice firmware (fan-made, unofficial) — shared state between tasks.
#pragma once
#include <atomic>
#include <cstdint>
#include "sdkconfig.h"

#define FW_VERSION "v0.9.2"

namespace app {

// ---- written by audio (core 0), read by face/behavior (core 1) ----
extern std::atomic<float> spk_level;      // 0..1, speaker RMS mapped from dBFS (20 ms window)
extern std::atomic<float> spk_dbfs;       // last speaker dBFS
extern std::atomic<float> mic_dbfs;       // last mic dBFS (after mute)
extern std::atomic<float> mic_floor_dbfs; // tracked noise floor
extern std::atomic<bool>  mic_voice;      // raw VAD (above floor+margin)
extern std::atomic<uint32_t> spk_last_ms; // last time output_cb delivered data
extern std::atomic<uint32_t> mic_last_ms; // last time host pulled mic data

// ---- written by servo/behavior, read by audio ----
extern std::atomic<bool> mic_mute_req;    // servo mute window active (zeros sent, stream kept)

// ---- written by servo, read by behavior (eyes lead the head) ----
extern std::atomic<float> look_x;         // -1..1, screen-relative, +x = viewer's right
extern std::atomic<float> look_y;         // -1..1, +y = down
extern std::atomic<bool>  look_active;    // servo look in progress (until back at the front)

// ---- call state (PC keyboard LED feedback; local toggle when HID is disabled) ----
extern std::atomic<bool> pc_call_active;
extern std::atomic<bool> call_on;

// ---- misc ----
extern std::atomic<bool> overlay_on;
extern std::atomic<int>  volume_cap;      // percent
extern bool safe_mode;
extern bool low_power_mode;               // previous reset was brownout

uint32_t millis();

}  // namespace app
