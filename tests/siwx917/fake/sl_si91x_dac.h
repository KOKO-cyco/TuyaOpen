#ifndef FAKE_SL_SI91X_DAC_H
#define FAKE_SL_SI91X_DAC_H
#include <stdint.h>
typedef uint32_t sl_status_t;
#define SL_STATUS_OK   0x0000u
#define SL_STATUS_FAIL 0x0001u
#define SL_DAC_FIFO_MODE   0
#define SL_DAC_STATIC_MODE 1
typedef struct { uint16_t division_factor; uint32_t soc_pll_clock, soc_pll_reference_clock; } sl_dac_clock_config_t;
typedef struct { uint8_t operating_mode, dac_fifo_threshold, adc_channel; uint32_t dac_sample_rate;
                 uint8_t dac_pin, dac_port; } sl_dac_config_t;
typedef void (*sl_dac_callback_t)(uint8_t event);
sl_status_t sl_si91x_dac_init(sl_dac_clock_config_t *clk);
sl_status_t sl_si91x_dac_set_configuration(sl_dac_config_t cfg, float vref);
sl_status_t sl_si91x_dac_write_data(int16_t *data, uint16_t length);
sl_status_t sl_si91x_dac_register_event_callback(sl_dac_callback_t cb);
sl_status_t sl_si91x_dac_start(void);
sl_status_t sl_si91x_dac_stop(void);
sl_status_t sl_si91x_dac_deinit(void);
typedef struct {
    int init_calls, cfg_calls, cb_calls, start_calls, stop_calls, deinit_calls, write_calls;
    int16_t  last_sample; uint16_t last_len; uint8_t mode_seen; uint32_t rate_seen; float vref_seen;
    sl_status_t fail_write, fail_cfg;
} fake_dac_state_t;
extern fake_dac_state_t g_fake_dac;
void fake_dac_reset(void);
#endif
