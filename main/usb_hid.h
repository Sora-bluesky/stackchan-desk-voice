#pragma once
// HID keyboard "call toggle" key (F13 by default). No-op when CONFIG_STACKCHAN_USB_HID=n.
namespace usb_hid {
bool available();
bool send_call_key();   // press + release; returns false if not mounted / not ready
}
