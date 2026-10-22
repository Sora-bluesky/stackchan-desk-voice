#include "aec_hook.h"
#include <cstring>
#include "sdkconfig.h"
#include "esp_log.h"

namespace aec {

void init()
{
#if CONFIG_STACKCHAN_AEC
    // TODO: 48k->16k decimate (FIR LPF, /3), esp-sr afe/aec (AEC_MODE_SR_LOW_COST,
    // 16 kHz only), 16k->48k interpolate. Add espressif/esp-sr to idf_component.yml then.
    ESP_LOGW("aec", "CONFIG_STACKCHAN_AEC=y but AEC is not implemented yet: pass-through");
#endif
}

bool enabled() { return false; }

void process(const int16_t* mic, const int16_t* ref, int16_t* out, size_t n)
{
    (void)ref;
    if (out != mic) memcpy(out, mic, n * sizeof(int16_t));
}

}  // namespace aec
