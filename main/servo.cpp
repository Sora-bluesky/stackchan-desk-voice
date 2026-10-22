#include "servo.h"
#include "app.h"
#include "board.h"
#include <atomic>
#include <initializer_list>
#include <cstdlib>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

static const char* TAG = "servo";

namespace servo {
namespace {

constexpr uart_port_t UART = UART_NUM_1;
constexpr int PIN_TX = 6, PIN_RX = 7;
constexpr int BAUD = 1000000;
// SCSCL memory table
constexpr uint8_t REG_MIN_ANGLE = 9, REG_MAX_ANGLE = 11, REG_TORQUE = 40, REG_GOAL_POS = 42, REG_POS = 56,
                  REG_LOAD = 60, REG_MOVING = 66;
constexpr uint8_t INST_READ = 0x02, INST_WRITE = 0x03;

struct Axis {
    uint8_t id;
    int zero;            // raw position of 0 deg
    int target_raw;
    int cur_raw = -1;
    bool torque = false;
    bool usable = false; // false if not responding or in PWM/wheel mode
    int off_count = 0;   // forcing detector (20 Hz samples)
    int still_count = 0;
    int last_raw = -1;
    bool forced = false;
};

Axis s_yaw{1, 460, 460};
Axis s_pitch{2, 620, 620};

std::atomic<bool> s_allowed{false}, s_abort{false}, s_sleep{false};
std::atomic<uint32_t> s_busy_until{0};
std::atomic<bool> s_forced{false};
bool s_enabled = false;

uint8_t checksum(const uint8_t* p, int n)
{
    uint8_t s = 0;
    for (int i = 2; i < n; i++) s += p[i];
    return ~s;
}

bool write_reg(uint8_t id, uint8_t addr, const uint8_t* d, uint8_t n)
{
    uint8_t pkt[16];
    pkt[0] = 0xFF; pkt[1] = 0xFF; pkt[2] = id; pkt[3] = n + 3; pkt[4] = INST_WRITE; pkt[5] = addr;
    memcpy(pkt + 6, d, n);
    pkt[6 + n] = checksum(pkt, 6 + n);
    uart_flush_input(UART);
    uart_write_bytes(UART, pkt, 7 + n);
    uart_wait_tx_done(UART, pdMS_TO_TICKS(5));
    uint8_t ack[6];
    int r = uart_read_bytes(UART, ack, sizeof(ack), pdMS_TO_TICKS(4));  // status packet
    return r == 6 && ack[2] == id;
}

bool read_reg(uint8_t id, uint8_t addr, uint8_t* out, uint8_t n)
{
    uint8_t pkt[8] = {0xFF, 0xFF, id, 4, INST_READ, addr, n, 0};
    pkt[7] = checksum(pkt, 7);
    uart_flush_input(UART);
    uart_write_bytes(UART, pkt, 8);
    uart_wait_tx_done(UART, pdMS_TO_TICKS(5));
    uint8_t rx[16];
    int want = 6 + n;
    int r = uart_read_bytes(UART, rx, want, pdMS_TO_TICKS(6));
    if (r != want || rx[0] != 0xFF || rx[1] != 0xFF || rx[2] != id || rx[3] != n + 2) return false;
    if (checksum(rx, want - 1) != rx[want - 1]) return false;
    memcpy(out, rx + 5, n);
    return true;
}

int read_word(uint8_t id, uint8_t addr)
{
    uint8_t b[2];
    if (!read_reg(id, addr, b, 2)) return -1;
    return (b[0] << 8) | b[1];   // SCS = big endian (SCSCL End=1)
}

bool set_torque(Axis& a, bool on)
{
    uint8_t v = on ? 1 : 0;
    bool ok = write_reg(a.id, REG_TORQUE, &v, 1);
    if (ok) a.torque = on;
    return ok;
}

bool write_pos(Axis& a, int raw, int time_ms)
{
    raw = raw < 0 ? 0 : (raw > 1000 ? 1000 : raw);
    uint8_t b[6] = {(uint8_t)(raw >> 8), (uint8_t)raw, (uint8_t)(time_ms >> 8), (uint8_t)time_ms, 0, 0};
    a.target_raw = raw;
    return write_reg(a.id, REG_GOAL_POS, b, 6);
}

int deg_to_raw(const Axis& a, float deg) { return a.zero + (int)(deg * 16.f / 5.f); }   // 0.3125 deg/step

bool is_moving(Axis& a)
{
    uint8_t m = 0;
    return read_reg(a.id, REG_MOVING, &m, 1) && m != 0;
}

void load_zero_from_nvs()
{
    // factory firmware stores zero positions in NVS namespace "servo" (zero_pos_1 / zero_pos_2).
    // Only present when NVS was preserved (app-only flashing, README). Read-only access.
    nvs_handle_t h;
    if (nvs_open("servo", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "zero_pos_1", &v) == ESP_OK && v >= 0 && v <= 1000) s_yaw.zero = v;
    if (nvs_get_i32(h, "zero_pos_2", &v) == ESP_OK && v >= 0 && v <= 1000) s_pitch.zero = v;
    nvs_close(h);
    ESP_LOGI(TAG, "zero from NVS: yaw=%d pitch=%d", s_yaw.zero, s_pitch.zero);
}

void probe(Axis& a)
{
    int pos = read_word(a.id, REG_POS);
    int mn = read_word(a.id, REG_MIN_ANGLE), mx = read_word(a.id, REG_MAX_ANGLE);
    a.cur_raw = pos;
    // min=max=0 means wheel/PWM mode (factory switches yaw to PWM sometimes) -> don't drive it
    a.usable = pos >= 0 && !(mn == 0 && mx == 0);
    ESP_LOGI(TAG, "id%d p%d %d..%d u%d", a.id, pos, mn, mx, a.usable);
}

// eyes lead the head: look direction is published this long before the goal positions are written
constexpr int LOOK_LEAD_MS = 150;
constexpr int PRE_MOVE_MS = CONFIG_STACKCHAN_MIC_MUTE_PRE_MS > LOOK_LEAD_MS ? CONFIG_STACKCHAN_MIC_MUTE_PRE_MS : LOOK_LEAD_MS;
// +1: positive yaw = viewer's right on screen. UNVERIFIED on the device; flip to -1 if the eyes look the wrong way
constexpr float YAW_TO_SCREEN_SIGN = 1.f;
constexpr float HOME_YAW = 0.f, HOME_PITCH = 15.f;

void publish_look(float yaw_deg, float pitch_deg)
{
    float x = YAW_TO_SCREEN_SIGN * yaw_deg / 25.f;              // yaw +-25 deg -> +-1
    float y = -(pitch_deg - HOME_PITCH) / 10.f;                  // head up (higher pitch) -> look up (y < 0)
    app::look_x.store(x < -1.f ? -1.f : (x > 1.f ? 1.f : x));
    app::look_y.store(y < -1.f ? -1.f : (y > 1.f ? 1.f : y));
    app::look_active.store(true);
}

// move both axes inside a mic-mute window so motor noise is not sent: mute 80 ms before,
// unmute 200 ms after the move is confirmed finished, 20 ms fade-in is done in audio.cpp.
// The pre-move wait is stretched to LOOK_LEAD_MS so the eyes move first.
void move_with_mute(float yaw_deg, float pitch_deg, int time_ms)
{
    if (!s_yaw.usable && !s_pitch.usable) return;
    bool home = yaw_deg == HOME_YAW && pitch_deg == HOME_PITCH;
    publish_look(yaw_deg, pitch_deg);
    app::mic_mute_req.store(true);
    s_busy_until.store(app::millis() + time_ms + 1500);
    vTaskDelay(pdMS_TO_TICKS(PRE_MOVE_MS));
    if (s_abort.load()) { app::mic_mute_req.store(false); app::look_active.store(false); return; }
    if (s_yaw.usable) write_pos(s_yaw, deg_to_raw(s_yaw, yaw_deg), time_ms);
    if (s_pitch.usable) write_pos(s_pitch, deg_to_raw(s_pitch, (float)clamp_tilt((int)pitch_deg)), time_ms);
    uint32_t t0 = app::millis();
    while (app::millis() - t0 < 1500) {          // one move <= 1.5 s
        vTaskDelay(pdMS_TO_TICKS(50));
        if (s_abort.load()) {                    // speech: stop where we are
            int py = read_word(s_yaw.id, REG_POS), pp = read_word(s_pitch.id, REG_POS);
            if (s_yaw.usable && py >= 0) write_pos(s_yaw, py, 0);
            if (s_pitch.usable && pp >= 0) write_pos(s_pitch, pp, 0);
            app::mic_mute_req.store(false);       // unmute immediately on speech
            app::look_active.store(false);
            s_busy_until.store(app::millis() + 300);
            return;
        }
        bool mv = (s_yaw.usable && is_moving(s_yaw)) || (s_pitch.usable && is_moving(s_pitch));
        if (!mv && app::millis() - t0 > 100) break;
    }
    if (home) app::look_active.store(false);     // back at the front: hand the eyes back to the face
    vTaskDelay(pdMS_TO_TICKS(CONFIG_STACKCHAN_MIC_MUTE_POST_MS));
    app::mic_mute_req.store(false);
    s_busy_until.store(app::millis() + 300);
}

// manual forcing detector: |pos - target| >= 8 steps for 150 ms -> torque off;
// released and still for 1 s -> new target = current, torque on (no snap back)
void check_forcing(Axis& a)
{
    if (!a.usable) return;
    int pos = read_word(a.id, REG_POS);
    if (pos < 0) return;
    a.cur_raw = pos;
    if (a.torque) {
        if (std::abs(pos - a.target_raw) >= 8) a.off_count++;
        else a.off_count = 0;
        if (a.off_count >= 3) {
            set_torque(a, false);
            a.forced = true;
            a.still_count = 0;
            s_forced.store(true);
            ESP_LOGW(TAG, "id%d forced by hand -> torque OFF", a.id);
        }
    } else if (a.forced) {
        if (a.last_raw >= 0 && std::abs(pos - a.last_raw) <= 2) a.still_count++;
        else a.still_count = 0;
        if (a.still_count >= 20) {
            int p = pos;
            if (&a == &s_pitch) {   // keep pitch inside 5..85 deg
                float deg = (p - a.zero) * 5.f / 16.f;
                p = deg_to_raw(a, (float)clamp_tilt((int)deg));
            }
            write_pos(a, p, 300);
            set_torque(a, true);
            a.forced = false;
            a.off_count = 0;
            s_forced.store(s_yaw.forced || s_pitch.forced);
            ESP_LOGI(TAG, "id%d re-hold at %d", a.id, p);
        }
    }
    a.last_raw = pos;
}

[[maybe_unused]] void task(void*)
{
    uart_config_t uc = {};
    uc.baud_rate = BAUD;
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;
    uart_driver_install(UART, 256, 0, 0, nullptr, 0);
    uart_param_config(UART, &uc);
    uart_set_pin(UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    load_zero_from_nvs();
    board::py32_servo_power(true);
    vTaskDelay(pdMS_TO_TICKS(300));
    probe(s_yaw);
    probe(s_pitch);
    // hold current position (no jump at boot); pitch is clamped into 5..85 deg slowly
    for (Axis* a : {&s_yaw, &s_pitch}) {
        if (!a->usable) continue;
        int p = a->cur_raw;
        if (a == &s_pitch) p = deg_to_raw(*a, (float)clamp_tilt((int)((p - a->zero) * 5.f / 16.f)));
        write_pos(*a, p, 800);
        set_torque(*a, true);
    }

    uint32_t next_look = app::millis() + 15000;
    bool slept = false;
    while (true) {
        uint32_t now = app::millis();
        if (s_sleep.load()) {
            if (!slept) { set_torque(s_yaw, false); set_torque(s_pitch, false); slept = true; }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        } else if (slept) {
            for (Axis* a : {&s_yaw, &s_pitch}) {
                if (!a->usable) continue;
                int p = read_word(a->id, REG_POS);
                if (p >= 0) write_pos(*a, p, 0);
                set_torque(*a, true);
            }
            slept = false;
        }
        check_forcing(s_yaw);
        check_forcing(s_pitch);
        s_abort.store(false);

        if (s_allowed.load() && !s_forced.load() && now >= next_look && !app::mic_voice.load()) {
            // slow look-around: small yaw +-25 deg, pitch 10..25 deg; then back to front
            float yaw = (float)((int)(esp_random() % 51) - 25);
            float pitch = 10.f + (float)(esp_random() % 16);
            move_with_mute(yaw, pitch, 1200);
            if (!s_abort.load()) move_with_mute(0.f, 15.f, 1200);
            uint32_t span = (CONFIG_STACKCHAN_SERVO_LOOK_MAX_S - CONFIG_STACKCHAN_SERVO_LOOK_MIN_S) * 1000;
            next_look = app::millis() + CONFIG_STACKCHAN_SERVO_LOOK_MIN_S * 1000 + (span ? esp_random() % span : 0);
        }
        vTaskDelay(pdMS_TO_TICKS(50));   // 20 Hz
    }
}

}  // namespace

void init()
{
#if CONFIG_STACKCHAN_SERVO
    if (app::low_power_mode) {
        ESP_LOGW(TAG, "previous reset was BROWNOUT: servos disabled this boot");
        return;
    }
    s_enabled = true;
    xTaskCreatePinnedToCore(task, "servo", 4096, nullptr, 4, nullptr, 1);
#else
    ESP_LOGI(TAG, "servos disabled (CONFIG_STACKCHAN_SERVO=n)");
#endif
}

bool enabled() { return s_enabled; }
void set_idle_allowed(bool a) { s_allowed.store(a); }
void abort_motion() { s_abort.store(true); }
void set_sleep(bool s) { s_sleep.store(s); }
bool busy_recent() { return app::mic_mute_req.load() || app::millis() < s_busy_until.load(); }
bool forced_free() { return s_forced.load(); }

}  // namespace servo
