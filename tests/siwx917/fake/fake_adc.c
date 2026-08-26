#include <string.h>
#include "sl_si91x_adc.h"

fake_adc_state_t g_fake_adc;

void fake_adc_reset(void) { memset(&g_fake_adc, 0, sizeof(g_fake_adc)); }

static sl_status_t fake_validate(const sl_adc_channel_config_t *ch, const sl_adc_config_t *cfg)
{
    uint8_t added = 0, biggest = 0;

    for (uint8_t i = 0; i < FAKE_ADC_CH; i++) {
        if (ch->num_of_samples[i] != 0) {
            added++;
            biggest = i;

            if (added > 1 && added != (uint8_t)(i + 1)) {
                return SL_STATUS_INVALID_CONFIGURATION;
            }
        }
    }
    if (added != cfg->num_of_channel_enable) {
        return SL_STATUS_INVALID_CONFIGURATION;
    }
    if (cfg->num_of_channel_enable == 1 && biggest != ch->channel) {
        return SL_STATUS_INVALID_CONFIGURATION;
    }
    return SL_STATUS_OK;
}

sl_status_t sl_si91x_adc_init(sl_adc_channel_config_t ch, sl_adc_config_t cfg, float vref)
{
    sl_status_t st = fake_validate(&ch, &cfg);

    g_fake_adc.init_calls++;
    g_fake_adc.vref_seen  = vref;
    g_fake_adc.mode_seen  = cfg.operation_mode;
    g_fake_adc.last_chcfg = ch;
    if (st != SL_STATUS_OK) {
        return st;
    }
    return g_fake_adc.fail_init ? g_fake_adc.fail_init : SL_STATUS_OK;
}

sl_status_t sl_si91x_adc_set_channel_configuration(sl_adc_channel_config_t ch, sl_adc_config_t cfg)
{
    sl_status_t st = fake_validate(&ch, &cfg);

    g_fake_adc.chcfg_calls++;
    g_fake_adc.last_chcfg = ch;
    if (st != SL_STATUS_OK) {
        return st;
    }

    g_fake_adc.muxed_input = ch.pos_inp_sel[0];
    return SL_STATUS_OK;
}

sl_status_t sl_si91x_adc_register_event_callback(sl_adc_callback_t cb)
{
    g_fake_adc.cb_calls++;
    return cb ? SL_STATUS_OK : SL_STATUS_FAIL;
}
sl_status_t sl_si91x_adc_start(sl_adc_config_t cfg)  { (void)cfg; g_fake_adc.start_calls++;  return SL_STATUS_OK; }
sl_status_t sl_si91x_adc_stop(sl_adc_config_t cfg)   { (void)cfg; g_fake_adc.stop_calls++;   return SL_STATUS_OK; }
sl_status_t sl_si91x_adc_deinit(sl_adc_config_t cfg) { (void)cfg; g_fake_adc.deinit_calls++; return SL_STATUS_OK; }

sl_status_t sl_si91x_adc_read_data_static(sl_adc_channel_config_t ch, sl_adc_config_t cfg, uint16_t *value)
{
    sl_status_t st = fake_validate(&ch, &cfg);

    g_fake_adc.read_calls++;
    if (st != SL_STATUS_OK) {
        return st;
    }
    if (g_fake_adc.fail_read) {
        return g_fake_adc.fail_read;
    }

    g_fake_adc.last_read_input = g_fake_adc.muxed_input;
    *value = (g_fake_adc.muxed_input < FAKE_ADC_CH) ? g_fake_adc.code[g_fake_adc.muxed_input] : 0u;
    return SL_STATUS_OK;
}
