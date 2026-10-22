// Audio path. Codec/I2S setup mirrors the factory StackChan firmware (MIT, M5Stack):
//   github.com/m5stack/StackChan firmware/main/hal/board/cores3_audio_codec.cc
//   - one I2S0 in full duplex: TX = STD stereo 16 bit (AW88298), RX = TDM slots 0..3 (ES7210)
//   - ES7210 mic_selected = MIC1|MIC2|MIC3, input opened as 2 ch: ch0 = mic, ch1 = reference
//     ("AUDIO_INPUT_REFERENCE true" in board/config.h -> echo-cancel reference)
//   - AW88298: hw_gain pa_voltage 5.0 / dac 3.3 / pa_gain 1, reset via AW9523B (board.cpp)
// Factory runs 24 kHz; we run 48 kHz to match the USB audio (UAC) format.
#include "audio.h"
#include "app.h"
#include "board.h"
#include "aec_hook.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "usb_device_uac.h"

static const char* TAG = "audio";

namespace {

// ---- I2C control interface for esp_codec_dev on top of M5.In_I2C (one bus owner) ----
struct M5CtrlIf {
    audio_codec_ctrl_if_t base;
    uint8_t addr7;
    bool opened;
};

int m5ctrl_open(const audio_codec_ctrl_if_t* c, void*, int) { ((M5CtrlIf*)c)->opened = true; return 0; }
bool m5ctrl_is_open(const audio_codec_ctrl_if_t* c) { return ((M5CtrlIf*)c)->opened; }
int m5ctrl_close(const audio_codec_ctrl_if_t* c) { ((M5CtrlIf*)c)->opened = false; return 0; }
int m5ctrl_read(const audio_codec_ctrl_if_t* c, int reg, int reg_len, void* data, int data_len)
{
    auto* m = (M5CtrlIf*)c;
    if (reg_len != 1) return ESP_CODEC_DEV_NOT_SUPPORT;
    return board::i2c_read(m->addr7, (uint8_t)reg, (uint8_t*)data, data_len) ? 0 : ESP_CODEC_DEV_READ_FAIL;
}
int m5ctrl_write(const audio_codec_ctrl_if_t* c, int reg, int reg_len, void* data, int data_len)
{
    auto* m = (M5CtrlIf*)c;
    if (reg_len != 1) return ESP_CODEC_DEV_NOT_SUPPORT;
    return board::i2c_write(m->addr7, (uint8_t)reg, (const uint8_t*)data, data_len) ? 0 : ESP_CODEC_DEV_WRITE_FAIL;
}
M5CtrlIf make_ctrl(uint8_t addr7)
{
    M5CtrlIf m{};
    m.base.open = m5ctrl_open;
    m.base.is_open = m5ctrl_is_open;
    m.base.read_reg = m5ctrl_read;
    m.base.write_reg = m5ctrl_write;
    m.base.close = m5ctrl_close;
    m.addr7 = addr7;
    m.opened = true;  // M5Unified owns and has already initialized the I2C bus.
    return m;
}

M5CtrlIf s_ctrl_aw = make_ctrl(0x36);   // AW88298 (8-bit 0x6C)
M5CtrlIf s_ctrl_es = make_ctrl(0x40);   // ES7210  (8-bit 0x80)

i2s_chan_handle_t s_tx, s_rx;
esp_codec_dev_handle_t s_out, s_in;
SemaphoreHandle_t s_tx_mtx;
bool s_ok = false;
std::atomic<int> s_pending_vol{-1};
int s_vol = 50;

#if CONFIG_STACKCHAN_SPK_BOOST_DB > 0
// USB speaker limiter; buffer matches usb_device_uac's full speaker FIFO.
constexpr float kSpkCeiling = 0.89f * 32767.f;
const float kSpkBoost = std::pow(10.f, CONFIG_STACKCHAN_SPK_BOOST_DB / 20.f);
const float kSpkRelease = 1.f - std::exp(-1.f / (0.100f * audio::kRate));
float s_spk_gain = kSpkBoost;
int16_t s_spk_buf[(CONFIG_UAC_SAMPLE_RATE / 1000 + 1) * (CONFIG_UAC_SPK_INTERVAL_MS + 1)];
#endif

// mic processing state
float s_mic_gain = 1.0f;          // 0..1 (mute window with 20 ms fade-in)
int s_voice_on_blocks = 0, s_voice_off_blocks = 0;
int16_t s_rx_buf[480 * 2];
int16_t s_mic[480], s_ref[480], s_out_buf[480];

constexpr int kDmaDesc = 6;
constexpr int kDmaFrame = 240;

bool create_duplex()
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = kDmaDesc,
        .dma_frame_num = kDmaFrame,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    if (i2s_new_channel(&chan_cfg, &s_tx, &s_rx) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = audio::kRate;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
    std_cfg.slot_cfg.left_align = true;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
    std_cfg.gpio_cfg.mclk = GPIO_NUM_0;
    std_cfg.gpio_cfg.bclk = GPIO_NUM_34;
    std_cfg.gpio_cfg.ws = GPIO_NUM_33;
    std_cfg.gpio_cfg.dout = GPIO_NUM_13;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;

    i2s_tdm_config_t tdm_cfg = {};
    tdm_cfg.clk_cfg.sample_rate_hz = audio::kRate;
    tdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tdm_cfg.clk_cfg.bclk_div = 8;
    tdm_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    tdm_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tdm_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    tdm_cfg.slot_cfg.slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
    tdm_cfg.slot_cfg.ws_width = I2S_TDM_AUTO_WS_WIDTH;
    tdm_cfg.slot_cfg.ws_pol = false;
    tdm_cfg.slot_cfg.bit_shift = true;
    tdm_cfg.slot_cfg.left_align = false;
    tdm_cfg.slot_cfg.big_endian = false;
    tdm_cfg.slot_cfg.bit_order_lsb = false;
    tdm_cfg.slot_cfg.skip_mask = false;
    tdm_cfg.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;
    tdm_cfg.gpio_cfg.mclk = GPIO_NUM_0;
    tdm_cfg.gpio_cfg.bclk = GPIO_NUM_34;
    tdm_cfg.gpio_cfg.ws = GPIO_NUM_33;
    tdm_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    tdm_cfg.gpio_cfg.din = GPIO_NUM_14;

    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_init_tdm_mode(s_rx, &tdm_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(s_tx) != ESP_OK) return false;
    if (i2s_channel_enable(s_rx) != ESP_OK) return false;
    return true;
}

inline float dbfs_of(const int16_t* s, size_t n)
{
    if (!n) return -100.f;
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)s[i] * s[i];
    double rms = std::sqrt(acc / n) / 32768.0;
    return rms < 1e-5 ? -100.f : (float)(20.0 * std::log10(rms));
}

void apply_volume(int pct)
{
    if (!s_out) return;
    esp_codec_dev_set_out_vol(s_out, pct);
}

// ---------------- UAC callbacks (run in usb_device_uac tasks on core 0) ----------------
esp_err_t uac_output_cb(uint8_t* buf, size_t len, void*)
{
    const int16_t* pcm = (const int16_t*)buf;
    size_t n = len / 2;
    float db = dbfs_of(pcm, n);
    app::spk_dbfs.store(db);
    app::spk_level.store(std::clamp((db + 50.f) / 40.f, 0.f, 1.f));
    app::spk_last_ms.store(app::millis());
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
#if CONFIG_STACKCHAN_SPK_BOOST_DB > 0
    for (size_t pos = 0; pos < n;) {
        size_t count = std::min(n - pos, sizeof(s_spk_buf) / sizeof(s_spk_buf[0]));
        for (size_t i = 0; i < count; i++) {
            float sample = pcm[pos + i];
            s_spk_gain += (kSpkBoost - s_spk_gain) * kSpkRelease;
            float peak = std::fabs(sample);
            if (peak * s_spk_gain > kSpkCeiling) s_spk_gain = kSpkCeiling / peak;
            s_spk_buf[i] = (int16_t)std::clamp(sample * s_spk_gain, -kSpkCeiling, kSpkCeiling);
        }
        esp_codec_dev_write(s_out, s_spk_buf, count * sizeof(int16_t));
        pos += count;
    }
#else
    esp_codec_dev_write(s_out, buf, len);
#endif
    xSemaphoreGive(s_tx_mtx);
    return ESP_OK;
}

esp_err_t uac_input_cb(uint8_t* buf, size_t len, size_t* bytes_read, void*)
{
    size_t n = std::min(len / 2, (size_t)480);
    // ES7210 opened as 2 channels: interleaved [mic, ref]
    if (esp_codec_dev_read(s_in, s_rx_buf, n * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
        memset(buf, 0, len);
        *bytes_read = len;
        return ESP_OK;
    }
    for (size_t i = 0; i < n; i++) {
        s_mic[i] = s_rx_buf[2 * i];
        s_ref[i] = s_rx_buf[2 * i + 1];
    }
    aec::process(s_mic, s_ref, s_out_buf, n);

    // VAD on the (pre-mute) mic signal so "speech starts -> stop servo" works during mute
    float db = dbfs_of(s_out_buf, n);
    float floor = app::mic_floor_dbfs.load();
    if (db < floor) floor = floor * 0.9f + db * 0.1f;
    else floor += 0.02f;
    floor = std::clamp(floor, -80.f, -30.f);
    app::mic_floor_dbfs.store(floor);
    float margin = (float)CONFIG_STACKCHAN_VAD_MARGIN_DB;
    // echo guard (behaviors.md 4): while speaker active and 300 ms after, raise threshold
    if (app::millis() - app::spk_last_ms.load() < 300 && app::spk_dbfs.load() > CONFIG_STACKCHAN_SPK_DBFS_ON) {
        margin += 10.f;
    }
    bool above = db > floor + margin;
    if (above) { s_voice_on_blocks++; s_voice_off_blocks = 0; }
    else { s_voice_off_blocks++; s_voice_on_blocks = 0; }
    if (s_voice_on_blocks >= 15) app::mic_voice.store(true);        // 150 ms
    if (s_voice_off_blocks >= 60) app::mic_voice.store(false);      // 600 ms

    // servo mute window: zeros (stream continues), 20 ms linear fade-in on release
    const float step = 1.0f / (0.020f * audio::kRate);
    int16_t* out = (int16_t*)buf;
    bool mute = app::mic_mute_req.load();
    for (size_t i = 0; i < n; i++) {
        if (mute) s_mic_gain = 0.f;
        else if (s_mic_gain < 1.f) s_mic_gain = std::min(1.f, s_mic_gain + step);
        out[i] = (int16_t)(s_out_buf[i] * s_mic_gain);
    }
    app::mic_dbfs.store(mute ? -100.f : db);
    app::mic_last_ms.store(app::millis());
    *bytes_read = n * 2;
    return ESP_OK;
}

void uac_set_mute_cb(uint32_t mute, void*)
{
    ESP_LOGI(TAG, "host mute=%u", (unsigned)mute);
    s_pending_vol.store(mute ? 0 : s_vol);
}

void uac_set_volume_cb(uint32_t volume, void*)
{
    audio::set_volume_pct((int)volume);
}

}  // namespace

namespace audio {

bool ok() { return s_ok; }

bool init()
{
    s_tx_mtx = xSemaphoreCreateMutex();
    board::aw88298_reset_release();
    if (!create_duplex()) {
        ESP_LOGE(TAG, "I2S duplex init failed");
        return false;
    }
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = s_rx;
    i2s_cfg.tx_handle = s_tx;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    aw88298_codec_cfg_t aw_cfg = {};
    aw_cfg.ctrl_if = &s_ctrl_aw.base;
    aw_cfg.gpio_if = gpio_if;
    aw_cfg.reset_pin = GPIO_NUM_NC;
    aw_cfg.hw_gain.pa_voltage = 5.0;
    aw_cfg.hw_gain.codec_dac_voltage = 3.3;
    aw_cfg.hw_gain.pa_gain = 1;
    const audio_codec_if_t* aw_if = aw88298_codec_new(&aw_cfg);

    es7210_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if = &s_ctrl_es.base;
    es_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
    const audio_codec_if_t* es_if = es7210_codec_new(&es_cfg);
    if (!data_if || !aw_if || !es_if) {
        ESP_LOGE(TAG, "codec if create failed (aw=%p es=%p)", aw_if, es_if);
        return false;
    }

    esp_codec_dev_cfg_t out_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = aw_if, .data_if = data_if};
    esp_codec_dev_cfg_t in_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es_if, .data_if = data_if};
    s_out = esp_codec_dev_new(&out_cfg);
    s_in = esp_codec_dev_new(&in_cfg);

    esp_codec_dev_sample_info_t ofs = {};
    ofs.bits_per_sample = 16;
    ofs.channel = 1;
    ofs.sample_rate = kRate;
    esp_codec_dev_sample_info_t ifs = {};
    ifs.bits_per_sample = 16;
    ifs.channel = 2;
    ifs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    ifs.sample_rate = kRate;
    int r1 = esp_codec_dev_open(s_out, &ofs);
    int r2 = esp_codec_dev_open(s_in, &ifs);
    esp_codec_dev_set_in_channel_gain(s_in, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), (float)CONFIG_STACKCHAN_MIC_GAIN_DB);
    // Host starts at 0 dB (100 %), so use the corresponding capped volume.
    s_vol = app::volume_cap.load();
    apply_volume(s_vol);
    aec::init();
    s_ok = (r1 == ESP_CODEC_DEV_OK && r2 == ESP_CODEC_DEV_OK);
    ESP_LOGI(TAG, "codec open out=%d in=%d, 48kHz/16bit, vol=%d cap=%d", r1, r2, s_vol, app::volume_cap.load());
    return s_ok;
}

bool start_uac()
{
    uac_device_config_t cfg = {};
    cfg.skip_tinyusb_init = false;
    cfg.output_cb = uac_output_cb;
    cfg.input_cb = uac_input_cb;
    cfg.set_mute_cb = uac_set_mute_cb;
    cfg.set_volume_cb = uac_set_volume_cb;
    cfg.cb_ctx = nullptr;
#if CONFIG_USB_DEVICE_UAC_AS_PART
    cfg.spk_itf_num = 1;   // ITF_NUM_AUDIO_STREAMING_SPK (main/usb/tusb_config.h)
    cfg.mic_itf_num = 2;   // ITF_NUM_AUDIO_STREAMING_MIC
#endif
    esp_err_t e = uac_device_init(&cfg);
    ESP_LOGI(TAG, "UAC start: %s (%s)", esp_err_to_name(e),
#if CONFIG_STACKCHAN_USB_HID
             "composite UAC+HID"
#else
             "UAC only"
#endif
    );
    return e == ESP_OK;
}

void set_volume_pct(int host_pct)
{
    host_pct = std::clamp(host_pct, 0, 100);
    int v = host_pct * app::volume_cap.load() / 100;   // cap (default 70 %)
    s_vol = v;
    s_pending_vol.store(v);
}

int volume_pct() { return s_vol; }

void service()
{
    int v = s_pending_vol.exchange(-1);
    if (v >= 0) {
        apply_volume(v);
        ESP_LOGI(TAG, "volume -> %d%%", v);
    }
}

void play_tone(int freq_hz, int ms, int amp_pct)
{
    if (!s_ok) return;
    static int16_t blk[480];
    int total = kRate * ms / 1000;
    float amp = 32767.f * std::clamp(amp_pct, 0, 100) / 100.f;
    int fade = kRate / 200;  // 5 ms
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    for (int pos = 0; pos < total;) {
        int n = std::min(480, total - pos);
        for (int i = 0; i < n; i++) {
            int k = pos + i;
            float env = std::min({1.f, (float)k / fade, (float)(total - k) / fade});
            blk[i] = (int16_t)(amp * env * std::sin(2.f * (float)M_PI * freq_hz * k / kRate));
        }
        esp_codec_dev_write(s_out, blk, n * 2);
        pos += n;
    }
    xSemaphoreGive(s_tx_mtx);
}

float read_mic_dbfs_block()
{
    if (!s_ok) return -100.f;
    if (esp_codec_dev_read(s_in, s_rx_buf, sizeof(s_rx_buf)) != ESP_CODEC_DEV_OK) return -100.f;
    for (int i = 0; i < 480; i++) s_mic[i] = s_rx_buf[2 * i];
    return dbfs_of(s_mic, 480);
}

}  // namespace audio
