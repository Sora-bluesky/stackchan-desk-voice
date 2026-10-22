#pragma once
#include <cstddef>
// Captures ESP_LOG output (still forwarded to the UART console on PORT.C G17/G18)
// and keeps the last N lines for the on-screen overlay.
namespace log_overlay {
constexpr int kLines = 4;
constexpr int kLineLen = 52;
void init();
// copies the last lines (oldest first) into out; returns count
int snapshot(char out[kLines][kLineLen]);
}
