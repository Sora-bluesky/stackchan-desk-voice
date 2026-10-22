#pragma once
#include <cstdint>
#include <cstddef>
// CoreS3 audio: ES7210 (mic, I2S RX TDM) + AW88298 (speaker amp, I2S TX) on one full-duplex
// I2S0 (MCLK G0, BCK G34, WS G33, DOUT G13 -> AW88298, DIN G14 <- ES7210), 48 kHz / 16 bit.
namespace audio {
constexpr int kRate = 48000;
bool init();                 // codecs + I2S, after board::init_m5()
bool start_uac();            // usb_device_uac (UAC2 [+HID if composite])
void service();              // call periodically from the UI task: applies deferred volume (I2C)
void set_volume_pct(int host_pct);   // 0..100 from host, capped by app::volume_cap
int  volume_pct();
// blocking tone (sine), used for the safe-mode speaker test and the call-end beep
void play_tone(int freq_hz, int ms, int amp_pct);
// safe mode only (no UAC): read one 10 ms block and return mic dBFS
float read_mic_dbfs_block();
bool ok();
}
