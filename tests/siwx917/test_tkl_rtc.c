#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "tuya_error_code.h"
#include "tkl_rtc.h"
#include "tkl_rtc_range.h"
#include "sl_si91x_calendar.h"

static int g_fail;

static void check(const char *what, int ok, const char *detail)
{
    printf("%-48s %s   %s\n", what, ok ? "ok  " : "FAIL", detail ? detail : "");
    if (!ok) {
        g_fail++;
    }
}

static int quiet_printf(const char *fmt, ...) { (void)fmt; return 0; }
int (*tkl_printf)(const char *format, ...) = quiet_printf;

static void fresh(void)
{
    tkl_rtc_deinit();
    fake_calendar_reset();
}

static void test_init_brings_up_the_block(void)
{
    fresh();
    check("init returns OK", tkl_rtc_init() == OPRT_OK, NULL);
    check("init powers up the calendar", g_fake_cal.init_calls == 1, NULL);
    check("init picks the RC oscillator", g_fake_cal.set_cfg_calls == 1,
          "set_configuration rejects anything but KHZ_RC_CLK_SEL");
    check("init starts the counter", g_fake_cal.start_calls == 1, "else time never advances");
}

static void test_init_is_idempotent(void)
{
    fresh();
    tkl_rtc_init();
    int first = g_fake_cal.init_calls;
    check("second init returns OK", tkl_rtc_init() == OPRT_OK, NULL);
    check("second init does not re-init hardware", g_fake_cal.init_calls == first,
          "re-running set_configuration would restart the counter");
}

static void test_init_failure_is_reported(void)
{
    fresh();
    g_fake_cal.fail_set_cfg = SL_STATUS_FAIL;
    check("clock config failure propagates", tkl_rtc_init() != OPRT_OK,
          "must not report a working RTC");
    check("counter not started after failure", g_fake_cal.start_calls == 0, NULL);
    g_fake_cal.fail_set_cfg = 0;
}

static void test_calls_before_init_are_refused(void)
{
    TIME_T t = 0;
    fresh();
    check("time_set before init refused", tkl_rtc_time_set(1787616000u) == OPRT_RESOURCE_NOT_READY, NULL);
    check("time_get before init refused", tkl_rtc_time_get(&t) == OPRT_RESOURCE_NOT_READY, NULL);
    check("no register access attempted", g_fake_cal.set_dt_calls == 0 && g_fake_cal.get_dt_calls == 0, NULL);
}

static void test_set_range(void)
{
    fresh();
    tkl_rtc_init();

    check("mid-range time accepted", tkl_rtc_time_set(1787616000u) == OPRT_OK, "2026-08-25");
    check("epoch accepted", tkl_rtc_time_set(0u) == OPRT_OK, "1970-01-01");
    check("SDK cap accepted", tkl_rtc_time_set(TKL_RTC_UNIX_MAX) == OPRT_OK, "2038-01-19");

    int before = g_fake_cal.set_dt_calls;
    check("cap+1 rejected", tkl_rtc_time_set(TKL_RTC_UNIX_MAX + 1u) == OPRT_INVALID_PARM,
          "TIME_T can hold values the SDK cannot");
    check("0xFFFFFFFF rejected", tkl_rtc_time_set(0xFFFFFFFFu) == OPRT_INVALID_PARM, NULL);
    check("rejected values never reach the SDK", g_fake_cal.set_dt_calls == before,
          "else the SDK would wrap them silently");
}

static void test_set_propagates_sdk_failure(void)
{
    fresh();
    tkl_rtc_init();
    g_fake_cal.fail_set_dt = SL_STATUS_FAIL;
    check("set_date_time failure propagates", tkl_rtc_time_set(1787616000u) != OPRT_OK, NULL);
    g_fake_cal.fail_set_dt = 0;
}

static void test_get(void)
{
    TIME_T t = 0;
    fresh();
    tkl_rtc_init();

    check("NULL out-param rejected", tkl_rtc_time_get(NULL) == OPRT_INVALID_PARM, NULL);

    g_fake_cal.next_get_unix = 1787616000u;
    check("time_get returns OK", tkl_rtc_time_get(&t) == OPRT_OK, NULL);
    check("time_get yields the stored value", t == 1787616000u, NULL);

    g_fake_cal.fail_get_dt = SL_STATUS_FAIL;
    check("get_date_time failure propagates", tkl_rtc_time_get(&t) != OPRT_OK, NULL);
    g_fake_cal.fail_get_dt = 0;

    g_fake_cal.fail_from_cal = SL_STATUS_INVALID_PARAMETER;
    check("calendar->unix failure propagates", tkl_rtc_time_get(&t) != OPRT_OK,
          "an unset RTC decodes to century 0 and must not yield a bogus time");
    g_fake_cal.fail_from_cal = 0;
}

static void test_deinit(void)
{
    TIME_T t = 0;
    fresh();
    tkl_rtc_init();
    check("deinit returns OK", tkl_rtc_deinit() == OPRT_OK, NULL);
    check("deinit stops the counter", g_fake_cal.stop_calls == 1, NULL);
    check("deinit powers down the block", g_fake_cal.deinit_calls == 1, NULL);
    check("calls after deinit refused", tkl_rtc_time_get(&t) == OPRT_RESOURCE_NOT_READY, NULL);
    check("second deinit is a no-op", tkl_rtc_deinit() == OPRT_OK && g_fake_cal.stop_calls == 1, NULL);
}

int main(void)
{
    printf("== SiWx917 tkl_rtc ==\n");
    test_init_brings_up_the_block();
    test_init_is_idempotent();
    test_init_failure_is_reported();
    test_calls_before_init_are_refused();
    test_set_range();
    test_set_propagates_sdk_failure();
    test_get();
    test_deinit();
    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
