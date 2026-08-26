#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "tuya_error_code.h"
#include "tkl_dac.h"
#include "tkl_dac_calc.h"
#include "sl_si91x_dac.h"

static int g_fail;
static void check(const char *what, int ok, const char *detail)
{
    printf("%-54s %s   %s\n", what, ok ? "ok  " : "FAIL", detail ? detail : "");
    if (!ok) g_fail++;
}
static int quiet_printf(const char *fmt, ...) { (void)fmt; return 0; }
int (*tkl_printf)(const char *format, ...) = quiet_printf;

static void fresh(void) { tkl_dac_deinit(TUYA_DAC_NUM_0); fake_dac_reset(); }

static void test_clamp_and_scale(void)
{
    check("negative clamps to 0", tkl_dac_clamp(-1) == 0, NULL);
    check("in range passes through", tkl_dac_clamp(512) == 512, NULL);
    check("full scale is 1023", tkl_dac_clamp(1023) == 1023, "the converter is 10-bit");
    check("a 12-bit value clamps, not wraps", tkl_dac_clamp(4095) == 1023,
          "easy mistake: the ADC on this part is 12-bit");

    check("0 mV -> code 0", tkl_dac_mv_to_code(0, 3300u) == 0, NULL);
    check("vref -> full scale", tkl_dac_mv_to_code(3300, 3300u) == 1023, NULL);
    check("above vref saturates", tkl_dac_mv_to_code(5000, 3300u) == 1023, NULL);
    check("half vref -> mid code", tkl_dac_mv_to_code(1650, 3300u) == 511, "1023*1650/3300");
    check("vref 0 uses the default", tkl_dac_mv_to_code(3300, 0u) == 1023, NULL);

    int mono = 1; int16_t prev = -1;
    for (int32_t mv = 0; mv <= 3300; mv++) {
        int16_t c = tkl_dac_mv_to_code(mv, 3300u);
        if (c < prev) mono = 0;
        prev = c;
    }
    check("mV -> code is monotonic", mono, "swept 0..3300 mV");
}

static void test_init_deinit(void)
{
    fresh();
    check("init ok", tkl_dac_init(TUYA_DAC_NUM_0) == OPRT_OK, NULL);
    check("callback registered", g_fake_dac.cb_calls == 1, "the SDK wants one before start");
    check("port 1 refused", tkl_dac_init(TUYA_DAC_NUM_1) == OPRT_NOT_SUPPORTED, "one AUX DAC");
    check("repeat init is a no-op",
          tkl_dac_init(TUYA_DAC_NUM_0) == OPRT_OK && g_fake_dac.init_calls == 1, NULL);

    check("deinit ok", tkl_dac_deinit(TUYA_DAC_NUM_0) == OPRT_OK && g_fake_dac.deinit_calls == 1, NULL);
    check("repeat deinit is a no-op",
          tkl_dac_deinit(TUYA_DAC_NUM_0) == OPRT_OK && g_fake_dac.deinit_calls == 1, NULL);
}

static void test_base_cfg(void)
{
    TUYA_DAC_BASE_CFG_T set = {0}, got = {0};
    fresh();

    check("config before init refused",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_SET_BASE_CFG, &set)
              == OPRT_RESOURCE_NOT_READY, NULL);

    tkl_dac_init(TUYA_DAC_NUM_0);
    set.width = 10; set.freq = 1000000u; set.ch_list.data = 1u; set.ch_nums = 1;
    check("base cfg accepted",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_SET_BASE_CFG, &set) == OPRT_OK, NULL);
    check("static mode selected", g_fake_dac.mode_seen == SL_DAC_STATIC_MODE,
          "a held level, not a streamed waveform");
    check("sample rate passed through", g_fake_dac.rate_seen == 1000000u, NULL);

    set.width = 12;
    check("a 12-bit width is refused",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_SET_BASE_CFG, &set) == OPRT_INVALID_PARM,
          "better than silently truncating");

    check("base_cfg_get always reports 10 bits",
          tkl_dac_base_cfg_get(TUYA_DAC_NUM_0, &got) == OPRT_OK && got.width == 10, NULL);
    check("base_cfg_get NULL refused",
          tkl_dac_base_cfg_get(TUYA_DAC_NUM_0, NULL) == OPRT_INVALID_PARM, NULL);
    check("NULL argu refused",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_SET_BASE_CFG, NULL) == OPRT_INVALID_PARM, NULL);
}

static void test_write(void)
{
    int16_t         samples[3] = {900, 100, 50};
    TKL_DAC_WRITE_T w          = {samples, 3};
    fresh();
    tkl_dac_init(TUYA_DAC_NUM_0);

    check("write accepted",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_WRITE_FIFO, &w) == OPRT_OK, NULL);
    check("the first sample is what reached the DAC", g_fake_dac.last_sample == 900,
          "static mode holds one value");
    check("length is one, not the caller's count", g_fake_dac.last_len == 1, NULL);

    int16_t         over[1] = {4095};
    TKL_DAC_WRITE_T w2      = {over, 1};
    tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_WRITE_FIFO, &w2);
    check("an out-of-range sample is clamped, not wrapped", g_fake_dac.last_sample == 1023, NULL);

    TKL_DAC_WRITE_T empty = {NULL, 0};
    check("NULL data refused",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_WRITE_FIFO, &empty) == OPRT_INVALID_PARM, NULL);
    TKL_DAC_WRITE_T zero_len = {samples, 0};
    check("zero length refused",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_WRITE_FIFO, &zero_len) == OPRT_INVALID_PARM, NULL);

    g_fake_dac.fail_write = SL_STATUS_FAIL;
    check("SDK write failure propagates",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, TUYA_DAC_WRITE_FIFO, &w) != OPRT_OK, NULL);
    g_fake_dac.fail_write = 0;

    check("an unknown command is refused",
          tkl_dac_controller_config(TUYA_DAC_NUM_0, (TUYA_DAC_CMD_E)99, &w) == OPRT_NOT_SUPPORTED, NULL);
}

static void test_start_stop_fifo(void)
{
    fresh();
    check("start before init refused", tkl_dac_start(TUYA_DAC_NUM_0) == OPRT_RESOURCE_NOT_READY, NULL);
    tkl_dac_init(TUYA_DAC_NUM_0);
    check("start ok", tkl_dac_start(TUYA_DAC_NUM_0) == OPRT_OK && g_fake_dac.start_calls == 1, NULL);
    check("stop ok", tkl_dac_stop(TUYA_DAC_NUM_0) == OPRT_OK && g_fake_dac.stop_calls == 1, NULL);

    check("fifo_reset reports unsupported",
          tkl_dac_fifo_reset(TUYA_DAC_NUM_0) == OPRT_NOT_SUPPORTED, "no FIFO in static mode");

    tkl_dac_start(TUYA_DAC_NUM_0);
    tkl_dac_deinit(TUYA_DAC_NUM_0);
    check("deinit stops a running DAC first", g_fake_dac.stop_calls == 2, NULL);
}

int main(void)
{
    printf("== SiWx917 tkl_dac ==\n");
    test_clamp_and_scale();
    test_init_deinit();
    test_base_cfg();
    test_write();
    test_start_stop_fifo();
    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
