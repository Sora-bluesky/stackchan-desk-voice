#include "usb_hid.h"
#include "app.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_STACKCHAN_USB_HID
#include "tusb.h"

static const char* TAG = "hid";

// --- TinyUSB HID callbacks (required) ---
extern "C" uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t*, uint16_t) { return 0; }
extern "C" void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                                      uint8_t const* buffer, uint16_t bufsize)
{
    // The keyboard descriptor has no report ID; its LED output is one byte.
    if (instance != 0 || report_id != 0 || report_type != HID_REPORT_TYPE_OUTPUT || !buffer || bufsize != 1) return;
    bool on = (buffer[0] & 0x04) != 0;   // Scroll Lock
    if (app::pc_call_active.exchange(on) != on) ESP_LOGI(TAG, "PC call %s", on ? "ON" : "OFF");
}

// --- device state callbacks (usb_device_uac does not define them when USB_DEVICE_UAC_AS_PART=y) ---
extern "C" void tud_mount_cb(void) { ESP_LOGI(TAG, "USB mounted (UAC+HID)"); }
extern "C" void tud_umount_cb(void) { ESP_LOGI(TAG, "USB unmounted"); }
extern "C" void tud_suspend_cb(bool) { ESP_LOGI(TAG, "USB suspended"); }
extern "C" void tud_resume_cb(void) { ESP_LOGI(TAG, "USB resumed"); }

namespace usb_hid {
bool available() { return tud_mounted(); }
bool send_call_key()
{
    if (!tud_mounted()) { ESP_LOGW(TAG, "not mounted"); return false; }
    if (tud_suspended()) tud_remote_wakeup();
    for (int i = 0; i < 20 && !tud_hid_ready(); i++) vTaskDelay(pdMS_TO_TICKS(2));
    if (!tud_hid_ready()) { ESP_LOGW(TAG, "HID not ready"); return false; }
    uint8_t keys[6] = {(uint8_t)CONFIG_STACKCHAN_HID_KEYCODE, 0, 0, 0, 0, 0};
    tud_hid_keyboard_report(0, (uint8_t)CONFIG_STACKCHAN_HID_MODIFIER, keys);
    vTaskDelay(pdMS_TO_TICKS(30));
    for (int i = 0; i < 20 && !tud_hid_ready(); i++) vTaskDelay(pdMS_TO_TICKS(2));
    tud_hid_keyboard_report(0, 0, nullptr);
    ESP_LOGI(TAG, "call key 0x%02X sent", CONFIG_STACKCHAN_HID_KEYCODE);
    return true;
}
}  // namespace usb_hid
#else
namespace usb_hid {
bool available() { return false; }
bool send_call_key()
{
    ESP_LOGW("hid", "HID disabled (CONFIG_STACKCHAN_USB_HID=n): press the voice button in the PC app manually");
    return false;
}
}  // namespace usb_hid
#endif
