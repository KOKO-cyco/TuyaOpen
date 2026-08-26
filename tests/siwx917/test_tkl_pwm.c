#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "tuya_error_code.h"
#include "tkl_pwm.h"
#include "tkl_pwm_calc.h"
#include "em_device.h"
#include "sl_si91x_pwm.h"

static int g_fail;

static void check(const char *what, int ok, const char *detail)
{
    printf("%-54s %s   %s\n", what, ok ? "ok  " : "FAIL", detail ? detail : "");
    if (!ok) {
        g_fail++;
    }
}

static int quiet_printf(const char *fmt, ...) { (void)fmt; return 0; }
int (*tkl_printf)(const char *format, ...) = quiet_printf;

static TUYA_PWM_BASE_CFG_T cfg_1khz(uint32_t duty)
{
    TUYA_PWM_BASE_CFG_T c = {0};
    c.polarity  = TUYA_PWM_POSITIVE;
    c.frequency = 1000u;
    c.cycle     = 10000u;
    c.duty      = duty;
    return c;
}

static void fresh(void)
{
    for (int i = 0; i < 4; i++) {
        tkl_pwm_deinit((TUYA_PWM_NUM_E)i);
    }
    fake_pwm_reset();
}

static void test_channel_range(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(5000u);
    fresh();
    check("channel 0 accepted", tkl_pwm_init(TUYA_PWM_NUM_0, &c) == OPRT_OK, NULL);
    check("channel 3 accepted", tkl_pwm_init(TUYA_PWM_NUM_3, &c) == OPRT_OK, NULL);
    check("channel 4 refused", tkl_pwm_init(TUYA_PWM_NUM_4, &c) == OPRT_NOT_SUPPORTED,
          "the MCPWM has four channels, not six");
    check("channel 5 refused", tkl_pwm_init(TUYA_PWM_NUM_5, &c) == OPRT_NOT_SUPPORTED, NULL);
    check("NULL cfg refused", tkl_pwm_init(TUYA_PWM_NUM_0, NULL) == OPRT_INVALID_PARM, NULL);
}

static void test_init_muxes_both_pads(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(0u);
    fresh();
    tkl_pwm_init(TUYA_PWM_NUM_0, &c);

    check("ch0 low side is pin 6 pad 1 mux 10",
          g_fake_pwm.last_init.pin_l == 6 && g_fake_pwm.last_init.pad_l == 1
              && g_fake_pwm.last_init.mux_l == 10, NULL);
    check("ch0 high side is pin 7 pad 2 mux 10",
          g_fake_pwm.last_init.pin_h == 7 && g_fake_pwm.last_init.pad_h == 2
              && g_fake_pwm.last_init.mux_h == 10, NULL);
    check("both sides on the HP port",
          g_fake_pwm.last_init.port_l == HP && g_fake_pwm.last_init.port_h == HP, NULL);
    check("output mode is independent", g_fake_pwm.mode[0] == SL_MODE_INDEPENDENT,
          "complementary would drive the paired pad inverted");

    fresh();
    tkl_pwm_init(TUYA_PWM_NUM_3, &c);
    check("ch3 maps to PWM_4L/4H (pins 12/15)",
          g_fake_pwm.last_init.pin_l == 12 && g_fake_pwm.last_init.pin_h == 15, NULL);
}

static void test_init_applies_timing(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(2500u);
    tkl_pwm_timing_t    t;
    fresh();
    tkl_pwm_init(TUYA_PWM_NUM_0, &c);

    tkl_pwm_solve_timing(1000u, &t);
    check("period pushed matches the solver", g_fake_pwm.period[0] == t.period, "1 kHz");
    check("prescale pushed matches the solver", g_fake_pwm.prescale[0] == t.prescale_sel, NULL);
    check("duty is 25% of the period in ticks",
          g_fake_pwm.duty[0] == tkl_pwm_duty_to_ticks(t.period, 2500u, 10000u),
          "not the SDK's integer-percent path");
}

static void test_unreachable_frequency(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(0u);
    fresh();
    c.frequency = 10u;
    check("10 Hz init refused", tkl_pwm_init(TUYA_PWM_NUM_0, &c) == OPRT_INVALID_PARM,
          "below 180e6/(64*65535)");
    check("nothing was initialised", g_fake_pwm.init_calls == 0, "refuse before touching hardware");

    c.frequency = 0u;
    check("0 Hz init refused", tkl_pwm_init(TUYA_PWM_NUM_0, &c) == OPRT_INVALID_PARM, NULL);
}

static void test_start_stop_guards(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(5000u);
    fresh();
    check("start before init refused", tkl_pwm_start(TUYA_PWM_NUM_0) == OPRT_RESOURCE_NOT_READY, NULL);
    check("stop before init refused", tkl_pwm_stop(TUYA_PWM_NUM_0) == OPRT_RESOURCE_NOT_READY, NULL);
    check("no channel was started", g_fake_pwm.start_calls[0] == 0, NULL);

    tkl_pwm_init(TUYA_PWM_NUM_0, &c);
    check("start after init works", tkl_pwm_start(TUYA_PWM_NUM_0) == OPRT_OK, NULL);
    check("the right channel started", g_fake_pwm.start_calls[0] == 1, NULL);
    check("stop works", tkl_pwm_stop(TUYA_PWM_NUM_0) == OPRT_OK && g_fake_pwm.stop_calls[0] == 1, NULL);
}

static void test_duty_set_uses_cached_cycle(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(0u);
    tkl_pwm_timing_t    t;
    fresh();

    c.cycle = 1000u;
    tkl_pwm_init(TUYA_PWM_NUM_1, &c);
    tkl_pwm_solve_timing(1000u, &t);

    check("duty_set honours the cycle from init",
          tkl_pwm_duty_set(TUYA_PWM_NUM_1, 500u) == OPRT_OK
              && g_fake_pwm.duty[1] == tkl_pwm_duty_to_ticks(t.period, 500u, 1000u),
          "500/1000 is 50%, not 5%");
    check("duty_set before init refused",
          tkl_pwm_duty_set(TUYA_PWM_NUM_2, 100u) == OPRT_RESOURCE_NOT_READY, NULL);
}

static void test_frequency_set_reapplies_duty(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(5000u);
    tkl_pwm_timing_t    t_new;
    fresh();
    tkl_pwm_init(TUYA_PWM_NUM_0, &c);

    check("frequency_set accepted", tkl_pwm_frequency_set(TUYA_PWM_NUM_0, 20000u) == OPRT_OK, NULL);
    tkl_pwm_solve_timing(20000u, &t_new);
    check("period followed the new frequency", g_fake_pwm.period[0] == t_new.period, NULL);

    check("duty re-scaled to the new period",
          g_fake_pwm.duty[0] == tkl_pwm_duty_to_ticks(t_new.period, 5000u, 10000u),
          "still 50%");
    check("unreachable frequency refused",
          tkl_pwm_frequency_set(TUYA_PWM_NUM_0, 5u) == OPRT_INVALID_PARM, NULL);
}

static void test_deinit_is_refcounted(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(1000u);
    fresh();
    tkl_pwm_init(TUYA_PWM_NUM_0, &c);
    tkl_pwm_init(TUYA_PWM_NUM_1, &c);

    tkl_pwm_deinit(TUYA_PWM_NUM_0);
    check("one channel down does not kill the block", g_fake_pwm.global_deinit_calls == 0,
          "sl_si91x_pwm_deinit() takes no channel");
    check("that channel was stopped", g_fake_pwm.stop_calls[0] == 1, NULL);

    tkl_pwm_deinit(TUYA_PWM_NUM_1);
    check("last channel down powers off the block", g_fake_pwm.global_deinit_calls == 1, NULL);
    check("repeat deinit is a no-op",
          tkl_pwm_deinit(TUYA_PWM_NUM_1) == OPRT_OK && g_fake_pwm.global_deinit_calls == 1, NULL);
}

static void test_info_and_unsupported(void)
{
    TUYA_PWM_BASE_CFG_T c = cfg_1khz(3333u), got = {0};
    fresh();
    tkl_pwm_init(TUYA_PWM_NUM_2, &c);

    check("info_get returns what init was given",
          tkl_pwm_info_get(TUYA_PWM_NUM_2, &got) == OPRT_OK
              && got.frequency == 1000u && got.duty == 3333u && got.cycle == 10000u, NULL);
    check("info_get NULL refused", tkl_pwm_info_get(TUYA_PWM_NUM_2, NULL) == OPRT_INVALID_PARM, NULL);

    check("polarity_set reports unsupported",
          tkl_pwm_polarity_set(TUYA_PWM_NUM_2, TUYA_PWM_NEGATIVE) == OPRT_NOT_SUPPORTED,
          "sl_si91x_pwm_set_output_polarity takes no channel");
    check("capture reports unsupported",
          tkl_pwm_cap_start(TUYA_PWM_NUM_2, NULL) == OPRT_NOT_SUPPORTED, "capture is QEI/SCT");
}

static void test_multichannel(void)
{
    TUYA_PWM_BASE_CFG_T c  = cfg_1khz(5000u);
    TUYA_PWM_NUM_E      ch[2] = {TUYA_PWM_NUM_0, TUYA_PWM_NUM_1};
    fresh();
    tkl_pwm_init(TUYA_PWM_NUM_0, &c);
    tkl_pwm_init(TUYA_PWM_NUM_1, &c);

    check("multichannel_start starts both",
          tkl_pwm_multichannel_start(ch, 2) == OPRT_OK
              && g_fake_pwm.start_calls[0] == 1 && g_fake_pwm.start_calls[1] == 1, NULL);
    check("multichannel_stop stops both",
          tkl_pwm_multichannel_stop(ch, 2) == OPRT_OK
              && g_fake_pwm.stop_calls[0] == 1 && g_fake_pwm.stop_calls[1] == 1, NULL);
    check("NULL array refused", tkl_pwm_multichannel_start(NULL, 2) == OPRT_INVALID_PARM, NULL);
}

int main(void)
{
    printf("== SiWx917 tkl_pwm ==\n");
    test_channel_range();
    test_init_muxes_both_pads();
    test_init_applies_timing();
    test_unreachable_frequency();
    test_start_stop_guards();
    test_duty_set_uses_cached_cycle();
    test_frequency_set_reapplies_duty();
    test_deinit_is_refcounted();
    test_info_and_unsupported();
    test_multichannel();
    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
