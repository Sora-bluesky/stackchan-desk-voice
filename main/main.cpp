// StackChan desk voice firmware (fan-made, unofficial)
// M5Stack StackChan (CoreS3, ESP32-S3, 16MB flash / 8MB PSRAM) as a USB audio device
// (UAC2 mic+speaker, 48 kHz/16 bit/1 ch) for a desktop AI chat app's voice mode on the local PC,
// with a code-drawn face, head-touch call toggle (HID key), and
// optional neck servos.
#include "app.h"
#include "board.h"
#include "audio.h"
#include "face.h"
#include "head_touch.h"
#include "servo.h"
#include "behavior.h"
#include "log_overlay.h"
#include <M5Unified.h>
#include <cstdio>
#include <algorithm>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "nvs_flash.h"

static const char* TAG = "main";

namespace app {
std::atomic<float> spk_level{0}, spk_dbfs{-100}, mic_dbfs{-100}, mic_floor_dbfs{-60};
std::atomic<bool> mic_voice{false};
std::atomic<uint32_t> spk_last_ms{0}, mic_last_ms{0};
std::atomic<bool> mic_mute_req{false};
std::atomic<float> look_x{0}, look_y{0};
std::atomic<bool> look_active{false};
std::atomic<bool> pc_call_active{false};
std::atomic<bool> call_on{false};
std::atomic<bool> overlay_on{CONFIG_STACKCHAN_DEBUG_OVERLAY};
std::atomic<int> volume_cap{CONFIG_STACKCHAN_VOLUME_CAP};
bool safe_mode = false;
bool low_power_mode = false;
uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }
}  // namespace app

static bool boot_touch_held()
{
    // safe mode: screen or head touch held during boot (sampled for ~400 ms)
    int hits = 0;
    for (int i = 0; i < 8; i++) {
        {
            board::I2cGuard g;
            M5.update();
        }
        bool t = M5.Touch.getCount() > 0;
        head_touch::poll(app::millis(), true);
        if (t || head_touch::touched()) hits++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return hits >= 6;
}

static void safe_mode_loop()
{
    auto& d = M5.Display;
    uint32_t flash = 0;
    esp_flash_get_size(nullptr, &flash);
    bool audio_ready = false;
    while (true) {
        d.startWrite();
        d.fillScreen(TFT_BLACK);
        d.setFont(&fonts::Font2);
        d.setTextColor(TFT_YELLOW, TFT_BLACK);
        d.setCursor(4, 4);
        d.printf("SAFE MODE  StackChan fw %s\n", FW_VERSION);
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.printf("USB audio (UAC) NOT started\n");
        d.printf("board: %s  lcd: %s %dx%d\n", board::panel_name(), board::display_ok() ? "OK" : "NG", (int)d.width(),
                 (int)d.height());
        d.printf("reset: %s  psram: %u KB  flash: %u MB\n", board::reset_reason_str(),
                 (unsigned)(esp_psram_get_size() / 1024), (unsigned)(flash >> 20));
        d.printf("i2c: AXP %c AW9523 %c ES7210 %c AW88298 %c\n", board::i2c_probe(0x34) ? 'o' : 'x',
                 board::i2c_probe(0x58) ? 'o' : 'x', board::i2c_probe(0x40) ? 'o' : 'x', board::i2c_probe(0x36) ? 'o' : 'x');
        d.printf("     Si12T %c FT6336 %c PY32 %c\n", head_touch::present() ? 'o' : 'x', board::i2c_probe(0x38) ? 'o' : 'x',
                 board::i2c_probe(0x6F) ? 'o' : 'x');
        d.printf("log: UART PORT.C TX=G17 RX=G18 115200\n");
        d.printf("RST 3s -> download mode / reset to exit\n");
        // buttons
        d.fillRoundRect(10, 180, 140, 50, 8, TFT_DARKGREEN);
        d.fillRoundRect(170, 180, 140, 50, 8, TFT_NAVY);
        d.setTextColor(TFT_WHITE);
        d.drawCenterString("SPK 1kHz", 80, 196);
        d.drawCenterString("MIC level", 240, 196);
        d.endWrite();

        // wait for a button
        int which = 0;
        while (!which) {
            {
                board::I2cGuard g;
                M5.update();
            }
            auto t = M5.Touch.getDetail();
            if (t.wasClicked() && t.y > 170) which = t.x < 160 ? 1 : 2;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!audio_ready) audio_ready = audio::init();
        if (which == 1) {
            audio::play_tone(1000, 1000, 30);   // 30 % amplitude, codec vol <= cap
        } else {
            uint32_t t0 = app::millis();
            while (app::millis() - t0 < 5000) {
                float db = audio::read_mic_dbfs_block();
                int w = (int)((db + 80.f) / 80.f * 300.f);
                w = w < 0 ? 0 : (w > 300 ? 300 : w);
                d.fillRect(10, 160, 300, 12, TFT_DARKGREY);
                d.fillRect(10, 160, w, 12, TFT_GREEN);
            }
        }
    }
}

extern "C" void app_main(void)
{
    log_overlay::init();
    ESP_LOGI(TAG, "StackChan desk voice fw %s, reset=%s", FW_VERSION, board::reset_reason_str());

    // NVS: never erase (factory NVS keeps servo calibration etc.). If it fails, just go on.
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs init: %s (not erasing)", esp_err_to_name(e));

    if (esp_reset_reason() == ESP_RST_BROWNOUT) {
        app::low_power_mode = true;
        app::volume_cap.store(std::min(50, (int)CONFIG_STACKCHAN_VOLUME_CAP));
        ESP_LOGW(TAG, "previous reset = BROWNOUT -> volume cap 50%%, servos off");
    }

    board::init_m5();
    head_touch::init();

    app::safe_mode = boot_touch_held();
    if (app::safe_mode) {
        ESP_LOGW(TAG, "SAFE MODE (touch held at boot): UAC not started");
        safe_mode_loop();
    }

    bool face_ok = face::init();
    if (!face_ok) ESP_LOGE(TAG, "face disabled (sprite alloc failed); audio continues");
    if (!audio::init()) ESP_LOGE(TAG, "audio codec init failed");
    audio::start_uac();      // even if the screen fails, audio should still work
    servo::init();
    behavior::start();
    ESP_LOGI(TAG, "boot done, free heap %u, psram %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
