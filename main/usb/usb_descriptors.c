/*
 * Composite descriptors: UAC2 (speaker 1ch + mic 1ch, 48 kHz/16 bit) + HID boot keyboard.
 * Built into the TinyUSB library only when CONFIG_STACKCHAN_USB_HID=y.
 * UAC part is the stock usb_device_uac 1.3.2 descriptor macro (Apache-2.0).
 */
#include <string.h>
#include "tusb.h"
#include "uac_descriptors.h"

/* usb_device_uac hides its UAC_TUSB_* options when USB_DEVICE_UAC_AS_PART=y */
#ifndef CONFIG_UAC_TUSB_VID
#define CONFIG_UAC_TUSB_VID          0x303A
#endif
#ifndef CONFIG_UAC_TUSB_PID
#define CONFIG_UAC_TUSB_PID          0x8000
#endif
#ifndef CONFIG_UAC_TUSB_MANUFACTURER
#define CONFIG_UAC_TUSB_MANUFACTURER "M5Stack-StackChan(custom)"
#endif
#ifndef CONFIG_UAC_TUSB_PRODUCT
#define CONFIG_UAC_TUSB_PRODUCT      "StackChan"
#endif
#ifndef CONFIG_UAC_TUSB_SERIAL_NUM
#define CONFIG_UAC_TUSB_SERIAL_NUM   "SC-DESKVOICE-01"
#endif

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_UAC_TUSB_VID,
    /* different PID from the UAC-only build so Windows does not reuse a cached descriptor */
    .idProduct          = CONFIG_UAC_TUSB_PID + 1,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + CFG_TUD_AUDIO * TUD_AUDIO_DEVICE_DESC_LEN + TUD_HID_DESC_LEN)

#define EPNUM_AUDIO_OUT   0x01
#define EPNUM_AUDIO_FB    0x81
#define EPNUM_AUDIO_IN    0x82
#define EPNUM_HID         0x83

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    /* strings 4,5,6 = AC, speaker, microphone */
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 4, EPNUM_AUDIO_OUT, EPNUM_AUDIO_IN, EPNUM_AUDIO_FB),
    /* string 7 = HID */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 7, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 10),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },
    CONFIG_UAC_TUSB_MANUFACTURER,
    CONFIG_UAC_TUSB_PRODUCT,
    CONFIG_UAC_TUSB_SERIAL_NUM,
    "StackChan Audio",
    "StackChan Speaker",
    "StackChan Mic",
    "StackChan Call Key",
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
