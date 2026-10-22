// Board bring-up for M5Stack StackChan (CoreS3).
// References (MIT, M5Stack): github.com/m5stack/StackChan firmware/main/hal/board/stackchan.cc
//   - AW9523B @0x58: P0.2 = AW88298 reset (ResetAw88298: 0x02 <- 0b011 then 0b111)
//   - PY32L020 IO expander @0x6F: IO0 = servo VM_EN (hal_io_expander.cpp)
#include "board.h"
#include "app.h"
#include <M5Unified.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char* TAG = "board";
static SemaphoreHandle_t s_i2c_mtx;
static bool s_display_ok = false;

namespace board {

void i2c_lock() { if (s_i2c_mtx) xSemaphoreTakeRecursive(s_i2c_mtx, portMAX_DELAY); }
void i2c_unlock() { if (s_i2c_mtx) xSemaphoreGiveRecursive(s_i2c_mtx); }

bool i2c_write(uint8_t a, uint8_t reg, const uint8_t* d, size_t n, uint32_t f)
{
    I2cGuard g;
    return M5.In_I2C.writeRegister(a, reg, d, n, f);
}
bool i2c_read(uint8_t a, uint8_t reg, uint8_t* d, size_t n, uint32_t f)
{
    I2cGuard g;
    return M5.In_I2C.readRegister(a, reg, d, n, f);
}
bool i2c_probe(uint8_t a)
{
    I2cGuard g;
    return M5.In_I2C.scanID(a, 100000);
}

void init_m5()
{
    s_i2c_mtx = xSemaphoreCreateRecursiveMutex();
    auto cfg = M5.config();
    cfg.clear_display = true;
    cfg.output_power = true;
    cfg.internal_imu = false;    // not used
    cfg.internal_rtc = false;
    cfg.internal_mic = false;    // we own I2S0 + ES7210 via esp_codec_dev
    cfg.internal_spk = false;    // we own AW88298 via esp_codec_dev
    cfg.led_brightness = 0;
    {
        I2cGuard g;
        M5.begin(cfg);
    }
    s_display_ok = (M5.Display.width() == 320 && M5.Display.height() == 240);
    ESP_LOGI(TAG, "M5 board=%d display %dx%d %s panel=%s", (int)M5.getBoard(), (int)M5.Display.width(),
             (int)M5.Display.height(), s_display_ok ? "OK" : "NG", panel_name());
    if (!s_display_ok) {
        ESP_LOGE(TAG, "LCD init failed: needs M5GFX>=0.2.33 (ILI9342E lots from 2026-08)");
    }
}

void aw88298_reset_release()
{
    I2cGuard g;
    // Same sequence as factory Aw9523::ResetAw88298(), but only touching bit2 of P0 output.
    M5.In_I2C.bitOff(0x58, 0x02, 0b100, 400000);
    vTaskDelay(pdMS_TO_TICKS(10));
    M5.In_I2C.bitOn(0x58, 0x02, 0b100, 400000);
    vTaskDelay(pdMS_TO_TICKS(50));
}

bool py32_servo_power(bool on)
{
    // PY32IOExpander registers (factory driver): GPIO_M_L=0x03 (dir, 1=out), GPIO_O_L=0x05, GPIO_PU_L=0x09
    I2cGuard g;
    constexpr uint8_t A = 0x6F;
    if (!M5.In_I2C.scanID(A, 100000)) {
        ESP_LOGW(TAG, "PY32 IO expander (0x6F) not found");
        return false;
    }
    bool ok = M5.In_I2C.bitOn(A, 0x03, 0x01, 100000);           // IO0 output
    ok &= M5.In_I2C.bitOn(A, 0x09, 0x01, 100000);               // pull-up
    ok &= on ? M5.In_I2C.bitOn(A, 0x05, 0x01, 100000) : M5.In_I2C.bitOff(A, 0x05, 0x01, 100000);
    ESP_LOGI(TAG, "servo VM_EN -> %d (%s)", on, ok ? "ok" : "fail");
    return ok;
}

const char* reset_reason_str()
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_USB: return "USB";
        case ESP_RST_EXT: return "EXT";
        default: return "OTHER";
    }
}

bool display_ok() { return s_display_ok; }

const char* panel_name()
{
    // M5GFX >=0.2.30 picks Panel_M5StackCoreS3E for ILI9342E. There is no public API
    // that returns the panel type name (未確認), so we only report the board here.
    switch (M5.getBoard()) {
        case m5::board_t::board_M5StackChan: return "StackChan(CoreS3)";
        case m5::board_t::board_M5StackCoreS3: return "CoreS3";
        case m5::board_t::board_M5StackCoreS3SE: return "CoreS3SE";
        default: return "unknown";
    }
}

}  // namespace board
