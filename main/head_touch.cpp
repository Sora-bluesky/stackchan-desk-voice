#include "head_touch.h"
#include "board.h"
#include "esp_log.h"
#include <atomic>
#include <cstdlib>

static const char* TAG = "head";

namespace head_touch {
namespace {
constexpr uint8_t ADDR = 0x68;
constexpr uint32_t F = 100000;
constexpr uint32_t TAP_MAX_MS = 500;
constexpr uint32_t DOUBLE_TAP_GAP_MS = 600;
constexpr int SWIPE_MIN_MOVE = 80;
constexpr uint32_t SWIPE_MIN_MS = 250;
constexpr uint32_t TOGGLE_LOCKOUT_MS = 1000;
constexpr uint32_t BOOT_IGNORE_MS = 3000;
constexpr uint32_t DOUBLE_TAP_QUIET_MS = 800;
constexpr uint32_t LONG_CONTACT_MS = 300;
constexpr uint32_t SCREEN_GUARD_MS = 1000;
constexpr uint8_t SENSITIVITY = 0x11;   // SI12T_TYPE_LOW level 1 (factory 0x33 = level 3); lower level = more sensitive
constexpr bool RAW_LOG = false;  // log OUTPUT1 changes (double-tap diagnosis, off since v0.9)
bool s_present = false;
int s_raw_prev = -1;             // RAW_LOG: last OUTPUT1 byte (-1 = none yet)
bool s_raw_failing = false;      // RAW_LOG: inside a read-failure streak
std::atomic<bool> s_touched{false};
uint32_t s_contact_start_ms = 0;
std::atomic<int> s_position{0};

// gesture state
bool s_down = false;
uint32_t s_down_ms = 0;
int s_start_pos = 0, s_last_pos = 0;
bool s_swiped = false;
uint32_t s_last_tap_up_ms = 0;
bool s_have_tap = false;
uint32_t s_cooldown_until = 0;
uint32_t s_last_contact_ms = 0, s_last_screen_touch_ms = 0;
bool s_have_screen_touch = false, s_down_quiet = false;
std::atomic<uint32_t> s_pending_screen_touch_ms{0};
std::atomic<bool> s_screen_touch_pending{false};
const char* s_down_reject_reason = nullptr;
const char* s_tap_reject_reason = nullptr;

bool w(uint8_t reg, uint8_t v) { return board::i2c_write(ADDR, reg, &v, 1, F); }
}  // namespace

bool present() { return s_present; }
bool touched() { return s_touched.load(); }
int position() { return s_position.load(); }

void note_screen_touch(uint32_t now)
{
    s_pending_screen_touch_ms.store(now);
    s_screen_touch_pending.store(true);
}

bool init()
{
    s_present = board::i2c_probe(ADDR);
    if (!s_present) {
        ESP_LOGW(TAG, "Si12T not found at 0x68");
        return false;
    }
    bool ok = true;
    // si12t_enable_channel()
    ok &= w(0x0A, 0x00); ok &= w(0x0B, 0x00);   // REF_RST1/2
    ok &= w(0x0C, 0x00); ok &= w(0x0D, 0x00);   // CH_HOLD1/2
    ok &= w(0x0E, 0x00); ok &= w(0x0F, 0x00);   // CAL_HOLD1/2
    // si12t_set_ctrl2(), si12t_set_ctrl1()
    ok &= w(0x09, 0x0F); ok &= w(0x09, 0x07);
    ok &= w(0x08, 0x22);
    // sensitivity: SENSITIVITY1..5 (0x02..0x06); factory uses SI12T_TYPE_LOW / LEVEL_3 = 0x33, we use level 1 (more sensitive)
    for (uint8_t r = 0x02; r <= 0x06; r++) ok &= w(r, SENSITIVITY);
    ESP_LOGI(TAG, "Si12T init %s", ok ? "ok" : "partial");
    return ok;
}

Event poll(uint32_t now, bool ignore)
{
    if (!s_present) return Event::None;
    uint8_t res = 0;
    if (!board::i2c_read(ADDR, 0x10, &res, 1, F)) {   // OUTPUT1
        if (RAW_LOG && !s_raw_failing) ESP_LOGI(TAG, "raw rd fail");
        s_raw_failing = true;
        return Event::None;
    }
    s_raw_failing = false;
    uint8_t in[3];
    for (int i = 0; i < 3; i++) in[i] = (res >> (2 * i)) & 0x03;
    if (RAW_LOG && res != s_raw_prev) {
        s_raw_prev = res;
        ESP_LOGI(TAG, "raw %lu %u-%u-%u", (unsigned long)now, in[0], in[1], in[2]);
    }
    int total = in[0] + in[1] + in[2];
    bool t = total > 0;
    int pos = t ? (in[0] * -100 + in[2] * 100) / total : 0;
    bool quiet = now - s_last_contact_ms >= DOUBLE_TAP_QUIET_MS;
    // Quiet time follows long raw contact, including ignored gestures.
    if (t) {
        s_position.store(pos);
        if (!s_touched.load()) s_contact_start_ms = now;
    } else if (s_touched.load() && now - s_contact_start_ms >= LONG_CONTACT_MS) {
        s_last_contact_ms = now;
    }
    s_touched.store(t);
    bool screen_touch = s_screen_touch_pending.exchange(false);
    if (screen_touch) {
        s_have_screen_touch = true;
        s_last_screen_touch_ms = s_pending_screen_touch_ms.load();
        if (s_down && !s_down_reject_reason) s_down_reject_reason = "screen";
        if (s_have_tap && !s_tap_reject_reason) s_tap_reject_reason = "screen";
    }
    const char* blocked = now < BOOT_IGNORE_MS ? "boot" :
                          (screen_touch || (s_have_screen_touch && now - s_last_screen_touch_ms < SCREEN_GUARD_MS) ? "screen" : nullptr);
    if (blocked) {
        if (s_down && !s_down_reject_reason) s_down_reject_reason = blocked;
        if (s_have_tap && !s_tap_reject_reason) s_tap_reject_reason = blocked;
    }

    Event ev = Event::None;
    if (ignore || now < s_cooldown_until) {
        s_down = t; s_have_tap = false; s_swiped = true;   // swallow
        return Event::None;
    }
    if (t && !s_down) {
        s_down = true; s_down_ms = now; s_start_pos = s_last_pos = pos; s_swiped = false;
        s_down_quiet = quiet;
        s_down_reject_reason = blocked;
    } else if (t && s_down) {
        s_last_pos = pos;
        if (!s_swiped && std::abs(pos - s_start_pos) > SWIPE_MIN_MOVE &&
            now - s_down_ms >= SWIPE_MIN_MS) {
            s_swiped = true;
            s_have_tap = false;
            if (!s_down_reject_reason) {
                ev = Event::Stroke;
                ESP_LOGI(TAG, "Stroke");
            }
        }
    } else if (!t && s_down) {
        s_down = false;
        uint32_t held = now - s_down_ms;
        const char* result = s_swiped ? "swipe" : "long";
        if (!s_swiped && held < TAP_MAX_MS) {
            result = "tap";
            if (s_have_tap && (s_down_ms - s_last_tap_up_ms) <= DOUBLE_TAP_GAP_MS) {
                s_have_tap = false;
                const char* rejected = s_tap_reject_reason ? s_tap_reject_reason : s_down_reject_reason;
                if (rejected) {
                    ESP_LOGI(TAG, "DoubleTap rejected: %s", rejected);
                    result = "tap#2->rejected";
                } else {
                    s_cooldown_until = now + TOGGLE_LOCKOUT_MS; // 1 s lockout after toggle
                    ev = Event::DoubleTap;
                    ESP_LOGI(TAG, "DoubleTap");
                    result = "tap#2->double";
                }
            } else {
                s_have_tap = true;
                s_last_tap_up_ms = now;
                s_tap_reject_reason = s_down_reject_reason ? s_down_reject_reason : (s_down_quiet ? nullptr : "not-quiet");
            }
        } else {
            s_have_tap = false;
        }
        ESP_LOGI(TAG, "%lums %d>%d %s", (unsigned long)held, s_start_pos, s_last_pos, result);
    }
    if (s_have_tap && !s_down && now - s_last_tap_up_ms > DOUBLE_TAP_GAP_MS) s_have_tap = false;
    return ev;
}

}  // namespace head_touch
