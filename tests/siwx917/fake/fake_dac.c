#include <string.h>
#include "sl_si91x_dac.h"
fake_dac_state_t g_fake_dac;
void fake_dac_reset(void) { memset(&g_fake_dac, 0, sizeof(g_fake_dac)); }
sl_status_t sl_si91x_dac_init(sl_dac_clock_config_t *clk) { (void)clk; g_fake_dac.init_calls++; return SL_STATUS_OK; }
sl_status_t sl_si91x_dac_set_configuration(sl_dac_config_t cfg, float vref)
{
    g_fake_dac.cfg_calls++; g_fake_dac.mode_seen = cfg.operating_mode;
    g_fake_dac.rate_seen = cfg.dac_sample_rate; g_fake_dac.vref_seen = vref;
    return g_fake_dac.fail_cfg ? g_fake_dac.fail_cfg : SL_STATUS_OK;
}
sl_status_t sl_si91x_dac_write_data(int16_t *data, uint16_t length)
{
    g_fake_dac.write_calls++; g_fake_dac.last_sample = data[0]; g_fake_dac.last_len = length;
    return g_fake_dac.fail_write ? g_fake_dac.fail_write : SL_STATUS_OK;
}
sl_status_t sl_si91x_dac_register_event_callback(sl_dac_callback_t cb)
{ g_fake_dac.cb_calls++; return cb ? SL_STATUS_OK : SL_STATUS_FAIL; }
sl_status_t sl_si91x_dac_start(void)  { g_fake_dac.start_calls++;  return SL_STATUS_OK; }
sl_status_t sl_si91x_dac_stop(void)   { g_fake_dac.stop_calls++;   return SL_STATUS_OK; }
sl_status_t sl_si91x_dac_deinit(void) { g_fake_dac.deinit_calls++; return SL_STATUS_OK; }
