#ifndef FAKE_SL_SI91X_ADC_H
#define FAKE_SL_SI91X_ADC_H

#include <stdint.h>

typedef uint32_t sl_status_t;
#define SL_STATUS_OK   0x0000u
#define SL_STATUS_FAIL 0x0001u
#define SL_STATUS_INVALID_CONFIGURATION 0x0023u

#define SL_ADC_STATIC_MODE  0
#define SL_ADC_FIFO_MODE    1
#define SL_ADC_SINGLE_ENDED 0
#define SL_ADC_DIFFERENTIAL 1

#define FAKE_ADC_CH 16

typedef struct {
    uint8_t  channel;
    uint8_t  input_type[FAKE_ADC_CH];
    uint32_t sampling_rate[FAKE_ADC_CH];
    uint8_t  pos_inp_sel[FAKE_ADC_CH];
    uint8_t  neg_inp_sel[FAKE_ADC_CH];
    uint16_t num_of_samples[FAKE_ADC_CH];
} sl_adc_channel_config_t;

typedef struct {
    uint8_t operation_mode;
    uint8_t num_of_channel_enable;
} sl_adc_config_t;

typedef void (*sl_adc_callback_t)(uint8_t channel, uint8_t event);

sl_status_t sl_si91x_adc_init(sl_adc_channel_config_t ch, sl_adc_config_t cfg, float vref);
sl_status_t sl_si91x_adc_set_channel_configuration(sl_adc_channel_config_t ch, sl_adc_config_t cfg);
sl_status_t sl_si91x_adc_register_event_callback(sl_adc_callback_t cb);
sl_status_t sl_si91x_adc_start(sl_adc_config_t cfg);
sl_status_t sl_si91x_adc_stop(sl_adc_config_t cfg);
sl_status_t sl_si91x_adc_deinit(sl_adc_config_t cfg);
sl_status_t sl_si91x_adc_read_data_static(sl_adc_channel_config_t ch, sl_adc_config_t cfg, uint16_t *value);

typedef struct {
    int      init_calls, chcfg_calls, cb_calls, start_calls, stop_calls, deinit_calls;
    float    vref_seen;
    uint8_t  mode_seen;
    int      read_calls;
    uint8_t  last_read_input;
    uint8_t  muxed_input;
    uint16_t code[FAKE_ADC_CH];
    sl_adc_channel_config_t last_chcfg;
    sl_status_t fail_init, fail_read;
} fake_adc_state_t;

extern fake_adc_state_t g_fake_adc;
void fake_adc_reset(void);

#endif
