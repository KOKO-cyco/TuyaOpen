#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "tuya_error_code.h"
#include "tkl_adc.h"
#include "tkl_adc_calc.h"
#include "sl_si91x_adc.h"

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

static TUYA_ADC_BASE_CFG_T cfg_for(uint32_t ch_mask, uint32_t ref_mv)
{
    TUYA_ADC_BASE_CFG_T c = {0};
    c.ch_list.data = ch_mask;
    c.ch_nums      = tkl_adc_count_channels(ch_mask);
    c.width        = 12;
    c.freq         = 100000;
    c.ref_vol      = ref_mv;
    return c;
}

static void fresh(void)
{
    tkl_adc_deinit(TUYA_ADC_NUM_0);
    fake_adc_reset();
}

static void test_code_to_mv(void)
{
    check("code 0 is 0 mV", tkl_adc_code_to_mv(0, 3300u) == 0, NULL);
    check("full scale is vref", tkl_adc_code_to_mv(4095, 3300u) == 3300, "4095 is the top code");
    check("half scale is ~vref/2", tkl_adc_code_to_mv(2047, 3300u) == 1649, "3300*2047/4095");
    check("over-range saturates", tkl_adc_code_to_mv(9999, 3300u) == 3300, "never exceeds vref");
    check("negative code floors at 0", tkl_adc_code_to_mv(-5, 3300u) == 0, NULL);
    check("vref 0 means the 3300 default", tkl_adc_code_to_mv(4095, 0u) == 3300, NULL);
    check("a 1800 mV reference scales", tkl_adc_code_to_mv(4095, 1800u) == 1800, NULL);

    int mono = 1;
    int32_t prev = -1;
    for (int32_t code = 0; code <= 4095; code++) {
        int32_t mv = tkl_adc_code_to_mv(code, 3300u);
        if (mv < prev) {
            mono = 0;
        }
        prev = mv;
    }
    check("code -> mV is monotonic over the whole range", mono, "swept 0..4095");
}

static void test_channel_list_helpers(void)
{
    check("empty list counts 0", tkl_adc_count_channels(0u) == 0, NULL);
    check("three bits count 3", tkl_adc_count_channels((1u << 0) | (1u << 3) | (1u << 7)) == 3, NULL);
    check("nth walks in index order",
          tkl_adc_nth_channel((1u << 0) | (1u << 3) | (1u << 7), 0) == 0
              && tkl_adc_nth_channel((1u << 0) | (1u << 3) | (1u << 7), 1) == 3
              && tkl_adc_nth_channel((1u << 0) | (1u << 3) | (1u << 7), 2) == 7, NULL);
    check("nth past the end reports none",
          tkl_adc_nth_channel((1u << 0), 1) == 0xFFu, "callers must not read it as channel 0");
    check("input select bounds", tkl_adc_input_sel_valid(15) && !tkl_adc_input_sel_valid(16),
          "the AUX ADC has 16 selects");
}

static void test_init(void)
{
    TUYA_ADC_BASE_CFG_T c = cfg_for(1u << 2, 3300u);
    fresh();

    check("init ok", tkl_adc_init(TUYA_ADC_NUM_0, &c) == OPRT_OK, NULL);
    check("static mode selected", g_fake_adc.mode_seen == SL_ADC_STATIC_MODE,
          "one-shot reads, no ping/pong DMA");
    check("vref passed in volts", g_fake_adc.vref_seen > 3.29f && g_fake_adc.vref_seen < 3.31f,
          "SDK takes a float in volts, TKL carries mV");
    check("callback registered before start", g_fake_adc.cb_calls == 1 && g_fake_adc.start_calls == 1,
          "the SDK requires one even when unused");

    check("exactly one slot installed, at index 0",
          g_fake_adc.last_chcfg.num_of_samples[0] == 1
              && g_fake_adc.last_chcfg.num_of_samples[1] == 0
              && g_fake_adc.last_chcfg.num_of_samples[15] == 0,
          "filling all 16 gave SL_STATUS_INVALID_CONFIGURATION on the part");
    check("channel field is the slot, not the input", g_fake_adc.last_chcfg.channel == 0, NULL);
    check("the requested input went into pos_inp_sel[0]",
          g_fake_adc.last_chcfg.pos_inp_sel[2 == 2 ? 0 : 0] == 2, "ch_list asked for input 2");
    check("single-ended input", g_fake_adc.last_chcfg.input_type[0] == SL_ADC_SINGLE_ENDED, NULL);

    check("port 1 refused", tkl_adc_init(TUYA_ADC_NUM_1, &c) == OPRT_NOT_SUPPORTED, "one AUX ADC");
    check("NULL cfg refused", tkl_adc_init(TUYA_ADC_NUM_0, NULL) == OPRT_INVALID_PARM, NULL);

    TUYA_ADC_BASE_CFG_T empty = cfg_for(0u, 3300u);
    fresh();
    check("empty ch_list refused", tkl_adc_init(TUYA_ADC_NUM_0, &empty) == OPRT_INVALID_PARM, NULL);
    check("nothing initialised on refusal", g_fake_adc.init_calls == 0, NULL);
}

static void test_read_single(void)
{
    TUYA_ADC_BASE_CFG_T c = cfg_for((1u << 1) | (1u << 4), 3300u);
    int32_t             v = 0;
    fresh();

    check("read before init refused",
          tkl_adc_read_single_channel(TUYA_ADC_NUM_0, 1, &v) == OPRT_RESOURCE_NOT_READY, NULL);

    tkl_adc_init(TUYA_ADC_NUM_0, &c);
    g_fake_adc.code[1] = 1000;
    g_fake_adc.code[4] = 2048;

    int cfgs = g_fake_adc.chcfg_calls;
    check("read returns the requested input's code",
          tkl_adc_read_single_channel(TUYA_ADC_NUM_0, 4, &v) == OPRT_OK && v == 2048, NULL);
    check("the pad actually converted was the one asked for",
          g_fake_adc.last_read_input == 4, "pos_inp_sel[0], not the channel field");

    check("switching input re-pushes the configuration",
          g_fake_adc.chcfg_calls > cfgs, "otherwise the pad never moves");
    check("reading the other input gives its code",
          tkl_adc_read_single_channel(TUYA_ADC_NUM_0, 1, &v) == OPRT_OK && v == 1000, NULL);
    check("NULL out refused",
          tkl_adc_read_single_channel(TUYA_ADC_NUM_0, 4, NULL) == OPRT_INVALID_PARM, NULL);
    check("channel 16 refused",
          tkl_adc_read_single_channel(TUYA_ADC_NUM_0, 16, &v) == OPRT_INVALID_PARM, NULL);

    g_fake_adc.fail_read = SL_STATUS_FAIL;
    check("SDK read failure propagates",
          tkl_adc_read_single_channel(TUYA_ADC_NUM_0, 4, &v) != OPRT_OK, NULL);
    g_fake_adc.fail_read = 0;
}

static void test_read_data_and_voltage(void)
{
    TUYA_ADC_BASE_CFG_T c = cfg_for((1u << 0) | (1u << 5), 3300u);
    int32_t             buf[4];
    fresh();
    tkl_adc_init(TUYA_ADC_NUM_0, &c);
    g_fake_adc.code[0] = 0;
    g_fake_adc.code[5] = 4095;

    memset(buf, 0x5A, sizeof(buf));
    check("read_data fills one per enabled channel",
          tkl_adc_read_data(TUYA_ADC_NUM_0, buf, 2) == OPRT_OK && buf[0] == 0 && buf[1] == 4095,
          "in ch_list index order");

    memset(buf, 0x5A, sizeof(buf));
    int32_t untouched = buf[2];
    check("a short ch_list leaves the tail alone",
          tkl_adc_read_data(TUYA_ADC_NUM_0, buf, 4) == OPRT_OK && buf[2] == untouched, NULL);

    memset(buf, 0, sizeof(buf));
    check("read_voltage converts to mV",
          tkl_adc_read_voltage(TUYA_ADC_NUM_0, buf, 2) == OPRT_OK
              && buf[0] == 0 && buf[1] == 3300, "0 and full scale");
    check("read_data NULL refused", tkl_adc_read_data(TUYA_ADC_NUM_0, NULL, 2) == OPRT_INVALID_PARM, NULL);
    check("read_data len 0 refused", tkl_adc_read_data(TUYA_ADC_NUM_0, buf, 0) == OPRT_INVALID_PARM, NULL);
}

static void test_accessors_and_deinit(void)
{
    TUYA_ADC_BASE_CFG_T c = cfg_for(1u << 3, 1800u);
    fresh();

    check("width is 12 bits", tkl_adc_width_get(TUYA_ADC_NUM_0) == 12, NULL);
    check("vref before init is the default",
          tkl_adc_ref_voltage_get(TUYA_ADC_NUM_0) == 3300, "never 0, callers divide by it");

    tkl_adc_init(TUYA_ADC_NUM_0, &c);
    check("vref after init reflects the config",
          tkl_adc_ref_voltage_get(TUYA_ADC_NUM_0) == 1800, NULL);
    check("die temperature reports unsupported",
          tkl_adc_temperature_get() == (int32_t)OPRT_NOT_SUPPORTED, "that is a separate BJT block");

    check("deinit ok", tkl_adc_deinit(TUYA_ADC_NUM_0) == OPRT_OK, NULL);
    check("deinit stops then powers down",
          g_fake_adc.stop_calls == 1 && g_fake_adc.deinit_calls == 1, NULL);
    int32_t v;
    check("read after deinit refused",
          tkl_adc_read_single_channel(TUYA_ADC_NUM_0, 3, &v) == OPRT_RESOURCE_NOT_READY, NULL);
    check("repeat deinit is a no-op",
          tkl_adc_deinit(TUYA_ADC_NUM_0) == OPRT_OK && g_fake_adc.deinit_calls == 1, NULL);
}

int main(void)
{
    printf("== SiWx917 tkl_adc ==\n");
    test_code_to_mv();
    test_channel_list_helpers();
    test_init();
    test_read_single();
    test_read_data_and_voltage();
    test_accessors_and_deinit();
    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
