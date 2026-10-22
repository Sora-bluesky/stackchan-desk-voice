#pragma once
#include <cstdint>
#include <cstddef>
// Hook for future echo cancellation (ESP-SR AFE/AEC). Currently a pass-through.
// mic:  48 kHz mono mic samples (ES7210 TDM slot 0)
// ref:  48 kHz mono reference (ES7210 TDM slot 1 = factory "AUDIO_INPUT_REFERENCE", 未確認 on device)
// out:  48 kHz mono samples to send to the PC
namespace aec {
void init();
bool enabled();
void process(const int16_t* mic, const int16_t* ref, int16_t* out, size_t n);
}
