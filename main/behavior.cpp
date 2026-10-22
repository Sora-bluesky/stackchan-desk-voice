#include "behavior.h"
#include "app.h"
#include "board.h"
#include "audio.h"
#include "face.h"
#include "head_touch.h"
#include "servo.h"
#include "usb_hid.h"
#include "log_overlay.h"
#include <M5Unified.h>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char* TAG = "behav";

namespace behavior {
namespace {

using face::State;

constexpr uint32_t SPK_ON_MS = 60, SPK_OFF_MS = 600, SPEAK_HOLD_MS = 2000;
constexpr uint32_t LISTEN_TO_THINK_MS = 600, THINK_TIMEOUT_MS = 5000;
constexpr uint32_t SLEEPY_MS = 3 * 60 * 1000, SLEEP_MS = 10 * 60 * 1000;
constexpr uint32_t PETTED_MS = 3000;
constexpr uint32_t HEAD_POLL_MS = 15;
constexpr int LOOK_CX = 160, LOOK_CY = 120, LOOK_DX = 80, LOOK_DY = 50;   // servo look -> gaze offset (px)
QueueHandle_t s_head_events = nullptr;

struct Ctx {
    State base = State::Idle;
    bool speaking = false;
    uint32_t spk_above_ms = 0, spk_below_ms = 0, speak_end_ms = 0;
    uint32_t voice_end_ms = 0, think_start_ms = 0;
    uint32_t last_activity = 0, call_end_ms = 0;
    bool call_end_blink = false;
    uint32_t petted_until = 0;
    uint32_t listen_force_until = 0;
    uint32_t corner_press_ms = 0, gaze_release_ms = 0;
    bool was_touching = false, gaze_hold = false;
    bool servo_gaze = false;   // gaze currently driven by the servo look
    int fps = 0, frames = 0;
    uint32_t fps_t0 = 0;
} c;

void apply_call_state(bool on, uint32_t now)
{
    if (app::call_on.exchange(on) == on) return;
    ESP_LOGI(TAG, "call %s", on ? "ON" : "OFF");
    c.last_activity = now;
    face::hop(now);
    c.call_end_blink = !on;
    if (on) {
        c.listen_force_until = now + 1500;     // listening face, no sound (mic is live)
    } else {
        c.listen_force_until = 0;
        audio::play_tone(880, 80, 20);         // short "pi" after voice mode stopped
        c.call_end_ms = now;
        c.base = State::Idle;
    }
}

void update_speaking(uint32_t now, uint32_t dt)
{
    bool fresh = now - app::spk_last_ms.load() < 100;
    bool above = fresh && app::spk_dbfs.load() > (float)CONFIG_STACKCHAN_SPK_DBFS_ON;
    if (above) { c.spk_above_ms += dt; c.spk_below_ms = 0; }
    else { c.spk_below_ms += dt; c.spk_above_ms = 0; }
    if (!c.speaking && c.spk_above_ms >= SPK_ON_MS) c.speaking = true;
    if (c.speaking && c.spk_below_ms >= SPK_OFF_MS) { c.speaking = false; c.speak_end_ms = now; }
    face::set_speech_level(fresh ? app::spk_level.load() : 0.f);
}

State decide(uint32_t now)
{
    bool voice = app::mic_voice.load();
    if (voice || c.speaking) c.last_activity = now;
    if (voice) servo::abort_motion();

    State s = c.base;
    switch (s) {
        case State::Sleep:
        case State::Sleepy:
            if (voice || c.speaking || now - c.last_activity < 1000) s = State::Idle;
            break;
        default: break;
    }
    if (voice) {
        s = State::Listening;
        c.voice_end_ms = 0;
    } else if (c.speaking) {
        s = State::Speaking;
    } else if (s == State::Listening) {
        if (!c.voice_end_ms) c.voice_end_ms = now;
        if (now - c.voice_end_ms > LISTEN_TO_THINK_MS) { s = State::Thinking; c.think_start_ms = now; }
    } else if (s == State::Thinking) {
        if (now - c.think_start_ms > THINK_TIMEOUT_MS) s = State::Idle;
    } else if (s == State::Speaking) {
        if (now - c.speak_end_ms > SPEAK_HOLD_MS) s = State::Idle;
    } else if (s == State::Idle || s == State::Blink) {
        s = State::Idle;
        if (now - c.last_activity > SLEEPY_MS) s = State::Sleepy;
    }
    if (s == State::Sleepy && now - c.last_activity > SLEEP_MS) s = State::Sleep;
    c.base = s;

    State shown = s;
    if (now < c.listen_force_until && s != State::Speaking) shown = State::Listening;
    if (now < c.petted_until) shown = State::Petted;   // overlay reaction, then back
    if (c.call_end_blink && now - c.call_end_ms >= 800) c.call_end_blink = false;
    if (c.call_end_blink && shown == State::Idle) shown = State::Blink;
    return shown;
}

void handle_screen_touch(uint32_t now)
{
    auto t = M5.Touch.getDetail();
    bool touching = M5.Touch.getCount() > 0 && t.isPressed();
    if (touching) {
        head_touch::note_screen_touch(now);
        c.last_activity = now;
        face::note_activity(now);
        face::set_gaze_target(t.x, t.y);
        c.gaze_hold = false;
    } else if (c.was_touching) {
        c.gaze_release_ms = now;
        c.gaze_hold = true;
    } else if (c.gaze_hold && now - c.gaze_release_ms >= 1000) {
        face::clear_gaze_target();
        c.gaze_hold = false;
    }
    // top-right long press (3 s) toggles the log overlay
    if (touching && t.x > 250 && t.y < 60) {
        if (!c.corner_press_ms) c.corner_press_ms = now;
        else if (c.corner_press_ms != UINT32_MAX / 2 && now - c.corner_press_ms > 3000) {
            app::overlay_on.store(!app::overlay_on.load());
            c.corner_press_ms = UINT32_MAX / 2;   // once per press
        }
    } else {
        c.corner_press_ms = 0;
    }
    c.was_touching = touching;
}

// eyes lead the head: follow the servo's published look unless a finger holds the gaze
void apply_servo_look()
{
    bool finger = c.was_touching || c.gaze_hold;
    if (app::look_active.load()) {
        if (!finger) {
            face::set_gaze_target(LOOK_CX + (int)(app::look_x.load() * LOOK_DX),
                                  LOOK_CY + (int)(app::look_y.load() * LOOK_DY));
        }
        c.servo_gaze = true;
    } else if (c.servo_gaze) {
        if (!finger) face::clear_gaze_target();
        c.servo_gaze = false;
    }
}

void head_task(void*)
{
    TickType_t last = xTaskGetTickCount();
    while (true) {
        auto ev = head_touch::poll(app::millis(), servo::busy_recent());
        if (ev != head_touch::Event::None && xQueueSend(s_head_events, &ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "head event queue full");
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(HEAD_POLL_MS));
    }
}

void ui_task(void*)
{
    TickType_t last = xTaskGetTickCount();
    uint32_t prev = app::millis();
    c.last_activity = prev;
    c.fps_t0 = prev;
    char ov[log_overlay::kLines][log_overlay::kLineLen];
    char status[96];
    while (true) {
        uint32_t now = app::millis();
        uint32_t dt = now - prev;
        prev = now;
        {
            board::I2cGuard g;
            M5.update();
        }
        handle_screen_touch(now);
        apply_servo_look();
        if (head_touch::touched()) c.last_activity = now;
        head_touch::Event ev;
        while (s_head_events && xQueueReceive(s_head_events, &ev, 0) == pdTRUE) {
            if (ev == head_touch::Event::DoubleTap) {
                bool sent = usb_hid::send_call_key();
                ESP_LOGI(TAG, "double tap (key %s)", sent ? "sent" : "NOT sent");
#if !CONFIG_STACKCHAN_USB_HID
                apply_call_state(!app::call_on.load(), now);
#endif
            } else if (ev == head_touch::Event::Stroke) c.petted_until = now + PETTED_MS;
        }
#if CONFIG_STACKCHAN_USB_HID
        apply_call_state(app::pc_call_active.load(), now);
#endif
        if (head_touch::touched() || app::mic_voice.load() || c.speaking) face::note_activity(now);
        audio::service();
        update_speaking(now, dt);
        State shown = decide(now);
        face::set_state(shown, now);
        servo::set_idle_allowed(c.base == State::Idle && shown == State::Idle);
        servo::set_sleep(c.base == State::Sleep);

        int n = 0;
        const char* st = nullptr;
        if (app::overlay_on.load()) {
            n = log_overlay::snapshot(ov);
            static const char* names[] = {"idle", "blnk", "lstn", "spk", "pet", "slpy", "slp", "thnk",
                                          "left", "rght", "up", "wink", "surp", "wake", "joy"};
            snprintf(status, sizeof(status), "%s aud:%s %s call:%s v%d %df s%.0f m%.0f/%.0f", FW_VERSION,
                     audio::ok() ? "OK" : "NG", names[(int)shown], app::call_on.load() ? "ON" : "off", audio::volume_pct(),
                     c.fps, app::spk_dbfs.load(), app::mic_dbfs.load(), app::mic_floor_dbfs.load());
            st = status;
        }
        face::render(now, ov, n, st);

        c.frames++;
        if (now - c.fps_t0 >= 1000) { c.fps = c.frames; c.frames = 0; c.fps_t0 = now; }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(33));   // ~30 fps cap
    }
}

}  // namespace

void start()
{
    s_head_events = xQueueCreate(8, sizeof(head_touch::Event));
    if (!s_head_events) {
        ESP_LOGE(TAG, "head event queue allocation failed");
    } else if (xTaskCreatePinnedToCore(head_task, "head", 3072, nullptr, 2, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "head task creation failed");
        vQueueDelete(s_head_events);
        s_head_events = nullptr;
    }
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, nullptr, 3, nullptr, 1);
}

}  // namespace behavior
