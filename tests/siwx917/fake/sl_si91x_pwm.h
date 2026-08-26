#ifndef FAKE_SL_SI91X_PWM_H
#define FAKE_SL_SI91X_PWM_H

#include <stdint.h>

typedef uint32_t sl_status_t;
#define SL_STATUS_OK   0x0000u
#define SL_STATUS_FAIL 0x0001u

typedef enum { SL_CHANNEL_1 = 0, SL_CHANNEL_2, SL_CHANNEL_3, SL_CHANNEL_4, SL_CHANNEL_LAST } sl_pwm_channel_t;
typedef enum { SL_MODE_INDEPENDENT = 0, SL_MODE_COMPLEMENTARY, SL_MODE_LAST } sl_pwm_mode_t;
typedef enum { SL_FREE_RUN_MODE = 0, SL_SINGLE_EVENT_MODE } sl_pwm_base_timer_mode_t;
typedef enum { SL_TIME_PERIOD_POSTSCALE_1_1 = 0 } sl_pwm_post_t;
typedef enum {
    SL_TIME_PERIOD_PRESCALE_1 = 0, SL_TIME_PERIOD_PRESCALE_2, SL_TIME_PERIOD_PRESCALE_4,
    SL_TIME_PERIOD_PRESCALE_8, SL_TIME_PERIOD_PRESCALE_16, SL_TIME_PERIOD_PRESCALE_32,
    SL_TIME_PERIOD_PRESCALE_64, SL_TIME_PERIOD_PRESCALE_LAST
} sl_pwm_pre_t;

typedef struct {
    uint8_t port_l, pin_l, port_h, pin_h, mux_l, mux_h, pad_l, pad_h;
} sl_pwm_init_t;

sl_status_t sl_si91x_pwm_init(sl_pwm_init_t *pwm_init);
void        sl_si91x_pwm_deinit(void);
sl_status_t sl_si91x_pwm_start(sl_pwm_channel_t channel);
sl_status_t sl_si91x_pwm_stop(sl_pwm_channel_t channel);
sl_status_t sl_si91x_pwm_set_time_period(sl_pwm_channel_t channel, uint32_t period, uint32_t init_val);
sl_status_t sl_si91x_pwm_set_duty_cycle(uint32_t duty_cycle, sl_pwm_channel_t channel);
sl_status_t sl_si91x_pwm_set_output_mode(sl_pwm_mode_t mode, sl_pwm_channel_t channel);
sl_status_t sl_si91x_pwm_set_base_timer_mode(sl_pwm_base_timer_mode_t mode, sl_pwm_channel_t channel);
sl_status_t sl_si91x_pwm_control_period(sl_pwm_post_t post, sl_pwm_pre_t pre, sl_pwm_channel_t channel);

#define FAKE_PWM_CHANNELS 4
typedef struct {
    int           init_calls, global_deinit_calls;
    sl_pwm_init_t last_init;
    int           start_calls[FAKE_PWM_CHANNELS], stop_calls[FAKE_PWM_CHANNELS];
    uint32_t      period[FAKE_PWM_CHANNELS], duty[FAKE_PWM_CHANNELS];
    int           prescale[FAKE_PWM_CHANNELS], mode[FAKE_PWM_CHANNELS];
    sl_status_t   fail_init, fail_start, fail_duty;
} fake_pwm_state_t;

extern fake_pwm_state_t g_fake_pwm;
void fake_pwm_reset(void);

#endif
