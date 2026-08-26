#include <string.h>
#include "sl_si91x_pwm.h"

fake_pwm_state_t g_fake_pwm;

void fake_pwm_reset(void) { memset(&g_fake_pwm, 0, sizeof(g_fake_pwm)); }

static int ok_ch(sl_pwm_channel_t c) { return (int)c >= 0 && (int)c < FAKE_PWM_CHANNELS; }

sl_status_t sl_si91x_pwm_init(sl_pwm_init_t *p)
{
    g_fake_pwm.init_calls++;
    g_fake_pwm.last_init = *p;
    return g_fake_pwm.fail_init ? g_fake_pwm.fail_init : SL_STATUS_OK;
}
void sl_si91x_pwm_deinit(void) { g_fake_pwm.global_deinit_calls++; }

sl_status_t sl_si91x_pwm_start(sl_pwm_channel_t c)
{
    if (!ok_ch(c)) return SL_STATUS_FAIL;
    g_fake_pwm.start_calls[c]++;
    return g_fake_pwm.fail_start ? g_fake_pwm.fail_start : SL_STATUS_OK;
}
sl_status_t sl_si91x_pwm_stop(sl_pwm_channel_t c)
{
    if (!ok_ch(c)) return SL_STATUS_FAIL;
    g_fake_pwm.stop_calls[c]++;
    return SL_STATUS_OK;
}
sl_status_t sl_si91x_pwm_set_time_period(sl_pwm_channel_t c, uint32_t period, uint32_t init_val)
{
    (void)init_val;
    if (!ok_ch(c)) return SL_STATUS_FAIL;
    g_fake_pwm.period[c] = period;
    return SL_STATUS_OK;
}
sl_status_t sl_si91x_pwm_set_duty_cycle(uint32_t duty, sl_pwm_channel_t c)
{
    if (!ok_ch(c)) return SL_STATUS_FAIL;
    g_fake_pwm.duty[c] = duty;
    return g_fake_pwm.fail_duty ? g_fake_pwm.fail_duty : SL_STATUS_OK;
}
sl_status_t sl_si91x_pwm_set_output_mode(sl_pwm_mode_t m, sl_pwm_channel_t c)
{
    if (!ok_ch(c)) return SL_STATUS_FAIL;
    g_fake_pwm.mode[c] = (int)m;
    return SL_STATUS_OK;
}
sl_status_t sl_si91x_pwm_set_base_timer_mode(sl_pwm_base_timer_mode_t m, sl_pwm_channel_t c)
{
    (void)m;
    return ok_ch(c) ? SL_STATUS_OK : SL_STATUS_FAIL;
}
sl_status_t sl_si91x_pwm_control_period(sl_pwm_post_t post, sl_pwm_pre_t pre, sl_pwm_channel_t c)
{
    (void)post;
    if (!ok_ch(c)) return SL_STATUS_FAIL;
    g_fake_pwm.prescale[c] = (int)pre;
    return SL_STATUS_OK;
}
