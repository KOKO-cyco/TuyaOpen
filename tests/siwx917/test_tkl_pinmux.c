#include <stdio.h>
#include <stdarg.h>

#include "tuya_error_code.h"
#include "tkl_pinmux.h"
#include "tkl_pinmux_table.h"
#include "em_device.h"
#include "sl_driver_gpio.h"

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

static void test_table_shape(void)
{
    check("table is not empty", TKL_PINMUX_TABLE_LEN > 0, NULL);

    int dup = 0;
    for (unsigned i = 0; i < TKL_PINMUX_TABLE_LEN; i++) {
        for (unsigned j = i + 1; j < TKL_PINMUX_TABLE_LEN; j++) {
            if (g_tkl_pinmux_table[i].func == g_tkl_pinmux_table[j].func
                && g_tkl_pinmux_table[i].pin == g_tkl_pinmux_table[j].pin
                && g_tkl_pinmux_table[i].ulp == g_tkl_pinmux_table[j].ulp) {
                dup++;
            }
        }
    }
    check("no duplicate (function, pad, domain) rows", dup == 0, NULL);

    int bad = 0;
    for (unsigned i = 0; i < TKL_PINMUX_TABLE_LEN; i++) {
        if (g_tkl_pinmux_table[i].mux > 15) {
            bad++;
        }
    }
    check("every mux fits the 4-bit mode field", bad == 0, NULL);

    int ulp_bad = 0;
    for (unsigned i = 0; i < TKL_PINMUX_TABLE_LEN; i++) {
        if (g_tkl_pinmux_table[i].ulp && g_tkl_pinmux_table[i].pin > 11) {
            ulp_bad++;
        }
    }
    check("ULP pads are stored as domain-local indices", ulp_bad == 0,
          "RTE writes them as 64 + index");
}

static void test_table_matches_rte(void)
{

    int pwm0_pad6 = 0, pwm0_pad7 = 0, i2c1_pad50 = 0, i2c1_pad33 = 0;

    for (unsigned i = 0; i < TKL_PINMUX_TABLE_LEN; i++) {
        const tkl_pinmux_entry_t *e = &g_tkl_pinmux_table[i];
        if (e->func == TUYA_PWM0 && e->pin == 6 && e->mux == 10 && !e->ulp) pwm0_pad6 = 1;
        if (e->func == TUYA_PWM0 && e->pin == 7 && e->mux == 10 && !e->ulp) pwm0_pad7 = 1;
        if (e->func == TUYA_IIC1_SCL && e->pin == 50 && e->mux == 5  && !e->ulp) i2c1_pad50 = 1;
        if (e->func == TUYA_IIC1_SCL && e->pin == 33 && e->mux == 11 && !e->ulp) i2c1_pad33 = 1;
    }
    check("PWM0 reaches pad 6 at mux 10", pwm0_pad6, "PWM_1L");
    check("PWM0 also reaches pad 7 at mux 10", pwm0_pad7, "PWM_1H, same channel");
    check("I2C1_SCL reaches pad 50 at mux 5", i2c1_pad50, NULL);
    check("I2C1_SCL reaches pad 33 at mux 11", i2c1_pad33,
          "same signal, different pad, different mux");
}

static void test_config_hp(void)
{
    fake_gpio_reset();

    check("PWM0 on GPIO 6 accepted",
          tkl_io_pinmux_config(TUYA_GPIO_NUM_6, TUYA_PWM0) == OPRT_OK, NULL);
    check("the pad that was muxed is pad 6", g_fake_gpio.last_pin == 6, NULL);
    check("the mode written is 10", g_fake_gpio.last_mode == 10, "PWM_1L");

    check("I2C1_SCL on GPIO 33 accepted",
          tkl_io_pinmux_config(TUYA_GPIO_NUM_33, TUYA_IIC1_SCL) == OPRT_OK, NULL);
    check("GPIO 33 takes mux 11, not 5", g_fake_gpio.last_mode == 11,
          "the mux is per pad, not per signal");

    int before = g_fake_gpio.calls;
    check("a function the pad cannot serve is refused",
          tkl_io_pinmux_config(TUYA_GPIO_NUM_33, TUYA_PWM0) == OPRT_NOT_SUPPORTED, NULL);
    check("a refused request writes no register", g_fake_gpio.calls == before, NULL);

    g_fake_gpio.fail = SL_STATUS_FAIL;
    check("an SDK failure propagates",
          tkl_io_pinmux_config(TUYA_GPIO_NUM_6, TUYA_PWM0) != OPRT_OK, NULL);
    g_fake_gpio.fail = 0;
}

static void test_config_rejections(void)
{
    fake_gpio_reset();

    check("an unknown pin is refused",
          tkl_io_pinmux_config((TUYA_PIN_NAME_E)5, TUYA_PWM0) == OPRT_INVALID_PARM, NULL);

    check("a UULP pad is refused",
          tkl_io_pinmux_config(TUYA_GPIO_NUM_2, TUYA_IIC0_SCL) == OPRT_NOT_SUPPORTED,
          "UULP has its own mux call");

    int has_row = 0;
    for (unsigned i = 0; i < TKL_PINMUX_TABLE_LEN; i++) {
        if (g_tkl_pinmux_table[i].func == TUYA_IIC1_SCL
            && g_tkl_pinmux_table[i].pin == 6 && g_tkl_pinmux_table[i].ulp) {
            has_row = 1;
        }
    }
    check("the table does carry ULP pad 6 for I2C1_SCL", has_row,
          "otherwise the next check would test the wrong branch");
    check("a ULP pad with a row still reports unsupported",
          tkl_io_pinmux_config(TUYA_GPIO_NUM_36, TUYA_IIC1_SCL) == OPRT_NOT_SUPPORTED,
          "the ULP mux call is not wired up");

    check("a ULP pad with no row is also refused",
          tkl_io_pinmux_config(TUYA_GPIO_NUM_20, TUYA_UART2_RTS) == OPRT_NOT_SUPPORTED, NULL);

    check("nothing was written for any rejection", g_fake_gpio.calls == 0, NULL);
}

static void test_multi(void)
{
    TUYA_MUL_PIN_CFG_T ok2[2] = {
        {.pin = TUYA_GPIO_NUM_6, .pin_func = TUYA_PWM0},
        {.pin = TUYA_GPIO_NUM_7, .pin_func = TUYA_PWM0},
    };
    TUYA_MUL_PIN_CFG_T bad2[2] = {
        {.pin = TUYA_GPIO_NUM_6,  .pin_func = TUYA_PWM0},
        {.pin = TUYA_GPIO_NUM_33, .pin_func = TUYA_PWM0},
    };

    fake_gpio_reset();
    check("two good pins both routed",
          tkl_multi_io_pinmux_config(ok2, 2) == OPRT_OK && g_fake_gpio.calls == 2, NULL);

    fake_gpio_reset();
    check("a bad entry fails the call", tkl_multi_io_pinmux_config(bad2, 2) != OPRT_OK, NULL);
    check("it stops rather than routing the rest", g_fake_gpio.calls == 1,
          "a half-configured bus is harder to diagnose");
    check("NULL array refused", tkl_multi_io_pinmux_config(NULL, 2) == OPRT_INVALID_PARM, NULL);
}

static void test_pin_to_func(void)
{
    check("GPIO 6 reports a PWM function",
          tkl_io_pin_to_func(TUYA_GPIO_NUM_6, TUYA_IO_TYPE_PWM) == TUYA_PWM0, NULL);
    check("GPIO 33 reports an I2C function",
          (tkl_io_pin_to_func(TUYA_GPIO_NUM_33, TUYA_IO_TYPE_I2C) & 0xFF00) == 0x0000, NULL);
    check("GPIO 33 has no PWM function",
          tkl_io_pin_to_func(TUYA_GPIO_NUM_33, TUYA_IO_TYPE_PWM) == OPRT_NOT_SUPPORTED, NULL);
    check("an unknown pin is refused",
          tkl_io_pin_to_func(5, TUYA_IO_TYPE_PWM) == OPRT_INVALID_PARM, NULL);
    check("ADC has no mux-table entries",
          tkl_io_pin_to_func(TUYA_GPIO_NUM_20, TUYA_IO_TYPE_ADC) == OPRT_NOT_SUPPORTED,
          "analog inputs bypass the pin mux");
}

int main(void)
{
    printf("== SiWx917 tkl_pinmux ==\n");
    test_table_shape();
    test_table_matches_rte();
    test_config_hp();
    test_config_rejections();
    test_multi();
    test_pin_to_func();
    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
