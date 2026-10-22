#include "log_overlay.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace log_overlay {

static char s_ring[kLines][kLineLen];
static int s_head = 0, s_count = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev = nullptr;

static int hook(const char* fmt, va_list ap)
{
    char tmp[160];
    va_list cp;
    va_copy(cp, ap);
    vsnprintf(tmp, sizeof(tmp), fmt, cp);
    va_end(cp);
    // strip ANSI colour codes and newline
    char clean[kLineLen];
    int j = 0;
    for (int i = 0; tmp[i] && j < kLineLen - 1; i++) {
        if (tmp[i] == '\033') {
            while (tmp[i] && tmp[i] != 'm') i++;
            if (!tmp[i]) break;
            continue;
        }
        if (tmp[i] == '\n' || tmp[i] == '\r') continue;
        clean[j++] = tmp[i];
    }
    clean[j] = 0;
    if (j > 0) {
        portENTER_CRITICAL(&s_mux);
        memcpy(s_ring[s_head], clean, j + 1);
        s_head = (s_head + 1) % kLines;
        if (s_count < kLines) s_count++;
        portEXIT_CRITICAL(&s_mux);
    }
    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void init() { s_prev = esp_log_set_vprintf(hook); }

int snapshot(char out[kLines][kLineLen])
{
    portENTER_CRITICAL(&s_mux);
    int n = s_count;
    for (int i = 0; i < n; i++) {
        int idx = (s_head - n + i + kLines) % kLines;
        memcpy(out[i], s_ring[idx], kLineLen);
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

}  // namespace log_overlay
