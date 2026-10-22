/*
 * TinyUSB config for the composite device: UAC2 (speaker+mic) + HID keyboard.
 * Only used when CONFIG_STACKCHAN_USB_HID=y (=> CONFIG_USB_DEVICE_UAC_AS_PART=y).
 * Based on espressif/usb_device_uac 1.3.2 tusb/tusb_config.h (Apache-2.0).
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"
#include "uac_config.h"

/* Interface numbers. UAC uses AC + SPK-AS + MIC-AS (3 interfaces), then HID. */
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_SPK,
    ITF_NUM_AUDIO_STREAMING_MIC,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

#include "uac_descriptors.h"
#include "tusb_config_uac.h"

#define CFG_TUSB_RHPORT0_MODE    (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CONFIG_USB_HS            0

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS              OPT_OS_FREERTOS
#endif
#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG           0
#endif
#define CFG_TUSB_OS_INC_PATH     freertos/
#define CFG_TUD_ENABLED          1
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN       __attribute__ ((aligned(4)))
#endif
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE   64
#endif

/* HID keyboard (call toggle key) */
#define CFG_TUD_HID              1
#define CFG_TUD_HID_EP_BUFSIZE   8

#ifdef __cplusplus
}
#endif
#endif /* _TUSB_CONFIG_H_ */
