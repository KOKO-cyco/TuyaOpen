#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "tkl_pwm_calc.h"

static int g_fail;

static void check(const char *what, int ok, const char *detail)
{
    printf("%-52s %s   %s\n", what, ok ? "ok  " : "FAIL", detail ? detail : "");
    if (!ok) {
        g_fail++;
    }
}

static double emitted_hz(const tkl_pwm_timing_t *t)
{
    return (double)TKL_PWM_SOC_CLK_HZ / ((double)(1u << t->prescale_sel) * (double)t->period);
}

static void test_common_frequencies(void)
{
    struct { uint32_t hz; const char *why; } cases[] = {
        { 50u,     "hobby servo" },
        { 100u,    "slow dimming" },
        { 1000u,   "LED PWM" },
        { 4000u,   "buzzer" },
        { 20000u,  "above audible" },
        { 100000u, "fast" },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        tkl_pwm_timing_t t;
        char label[96], detail[96];
        bool ok = tkl_pwm_solve_timing(cases[i].hz, &t);

        snprintf(label, sizeof(label), "%u Hz solvable (%s)", cases[i].hz, cases[i].why);
        check(label, ok, NULL);
        if (!ok) {
            continue;
        }

        double got = emitted_hz(&t);
        double err = (got - (double)cases[i].hz) / (double)cases[i].hz;
        snprintf(detail, sizeof(detail), "prescale=%u period=%u -> %.1f Hz",
                 1u << t.prescale_sel, t.period, got);
        snprintf(label, sizeof(label), "%u Hz accurate to 1%%", cases[i].hz);
        check(label, err > -0.01 && err < 0.01, detail);
    }
}

static void test_prescale_is_minimal(void)
{

    tkl_pwm_timing_t t;
    check("2800 Hz uses no prescaling",
          tkl_pwm_solve_timing(2800u, &t) && t.prescale_sel == 0u, "period fits in 16 bits");
    check("1000 Hz needs a prescaler",
          tkl_pwm_solve_timing(1000u, &t) && t.prescale_sel > 0u, "180e6/1000 overflows 16 bits");

    int minimal = 1;
    for (uint32_t hz = 45u; hz <= 200000u; hz += 37u) {
        tkl_pwm_timing_t s;
        if (!tkl_pwm_solve_timing(hz, &s)) {
            continue;
        }
        if (s.prescale_sel > 0u) {
            uint32_t smaller = 1u << (s.prescale_sel - 1u);
            uint32_t period  = (uint32_t)(TKL_PWM_SOC_CLK_HZ / (smaller * (uint64_t)hz));
            if (period <= TKL_PWM_PERIOD_MAX) {
                minimal = 0;
            }
        }
    }
    check("prescale is always the smallest that fits", minimal, "period is the duty resolution");
}

static void test_range_limits(void)
{
    tkl_pwm_timing_t t;

    check("43 Hz reachable", tkl_pwm_solve_timing(43u, &t), "just above the floor");
    check("42 Hz refused", !tkl_pwm_solve_timing(42u, &t), "would need period > 65535");
    check("10 Hz refused", !tkl_pwm_solve_timing(10u, &t), NULL);
    check("0 Hz refused", !tkl_pwm_solve_timing(0u, &t), "no divide by zero");

    check("90 MHz reachable at the 2-tick floor",
          tkl_pwm_solve_timing(90000000u, &t) && t.period == 2u, "0/50/100% only");
    check("91 MHz refused", !tkl_pwm_solve_timing(91000000u, &t), "period would truncate to 1");
    check("45 MHz reachable", tkl_pwm_solve_timing(45000000u, &t) && t.period == 4u, NULL);

    int sane = 1;
    uint32_t worst_hz = 0;
    double worst_err = 0.0;
    for (uint32_t hz = 43u; hz <= 1000000u; hz += 101u) {
        tkl_pwm_timing_t s;
        if (!tkl_pwm_solve_timing(hz, &s)) {
            continue;
        }
        if (s.period < TKL_PWM_PERIOD_MIN) {
            sane = 0;
        }
        double err = (emitted_hz(&s) - (double)hz) / (double)hz;
        if (err < 0.0) {
            err = -err;
        }
        if (err > worst_err) {
            worst_err = err;
            worst_hz  = hz;
        }
    }
    check("solved period never below the 2-tick floor", sane, "swept 43 Hz .. 1 MHz");
    {
        char detail[96];
        snprintf(detail, sizeof(detail), "worst %.2f%% at %u Hz", worst_err * 100.0, worst_hz);

        check("emitted frequency tracks the request", worst_err < 0.05, detail);
    }
}

static void test_duty(void)
{
    check("0%% -> 0 ticks", tkl_pwm_duty_to_ticks(1000u, 0u, 10000u) == 0u, NULL);
    check("50%% -> half", tkl_pwm_duty_to_ticks(1000u, 5000u, 10000u) == 500u, NULL);
    check("100%% -> full period", tkl_pwm_duty_to_ticks(1000u, 10000u, 10000u) == 1000u, NULL);
    check("duty above cycle clamps", tkl_pwm_duty_to_ticks(1000u, 99999u, 10000u) == 1000u,
          "must not wrap past the period");

    check("cycle 0 means the 10000 default", tkl_pwm_duty_to_ticks(1000u, 5000u, 0u) == 500u,
          "matches tdd_led_pwm.c and the T5AI adapter");
    check("a non-10000 cycle is honoured", tkl_pwm_duty_to_ticks(1000u, 50u, 100u) == 500u,
          "LINUX adapter does the same; T5AI ignores cycle");

    check("tiny duty never rounds to off", tkl_pwm_duty_to_ticks(100u, 1u, 10000u) == 1u, NULL);
    check("genuine zero stays off", tkl_pwm_duty_to_ticks(100u, 0u, 10000u) == 0u, NULL);

    int mono = 1;
    uint16_t prev = 0u;
    for (uint32_t d = 0u; d <= 10000u; d += 7u) {
        uint16_t ticks = tkl_pwm_duty_to_ticks(65535u, d, 10000u);
        if (ticks < prev) {
            mono = 0;
        }
        prev = ticks;
    }
    check("duty -> ticks is monotonic", mono, "swept 0..10000 at max period");
}

int main(void)
{
    printf("== SiWx917 tkl_pwm timing/duty arithmetic ==\n");
    test_common_frequencies();
    test_prescale_is_minimal();
    test_range_limits();
    test_duty();
    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
