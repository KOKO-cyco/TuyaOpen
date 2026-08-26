#include <string.h>
#include "sl_si91x_calendar.h"

fake_calendar_state_t g_fake_cal;

void fake_calendar_reset(void)
{
    memset(&g_fake_cal, 0, sizeof(g_fake_cal));
}

void sl_si91x_calendar_init(void)   { g_fake_cal.init_calls++; }
void sl_si91x_calendar_deinit(void) { g_fake_cal.deinit_calls++; }
void sl_si91x_calendar_rtc_start(void) { g_fake_cal.start_calls++; }
void sl_si91x_calendar_rtc_stop(void)  { g_fake_cal.stop_calls++; }

sl_status_t sl_si91x_calendar_set_configuration(sl_calendar_clock_t clock_type)
{
    g_fake_cal.set_cfg_calls++;

    if (clock_type != KHZ_RC_CLK_SEL) {
        return SL_STATUS_INVALID_PARAMETER;
    }
    return g_fake_cal.fail_set_cfg ? g_fake_cal.fail_set_cfg : SL_STATUS_OK;
}

sl_status_t sl_si91x_calendar_convert_unix_time_to_calendar_datetime(uint32_t unix_time,
                                                                     sl_calendar_datetime_config_t *cal)
{
    if (g_fake_cal.fail_to_cal) {
        return g_fake_cal.fail_to_cal;
    }
    memset(cal, 0, sizeof(*cal));

    cal->Century = 2;
    cal->Year    = (uint8_t)(unix_time % 100u);
    cal->Second  = (uint8_t)(unix_time % 60u);
    g_fake_cal.last_unix_set = unix_time;
    return SL_STATUS_OK;
}

sl_status_t sl_si91x_calendar_set_date_time(sl_calendar_datetime_config_t *config)
{
    (void)config;
    g_fake_cal.set_dt_calls++;
    return g_fake_cal.fail_set_dt ? g_fake_cal.fail_set_dt : SL_STATUS_OK;
}

sl_status_t sl_si91x_calendar_get_date_time(sl_calendar_datetime_config_t *config)
{
    g_fake_cal.get_dt_calls++;
    if (g_fake_cal.fail_get_dt) {
        return g_fake_cal.fail_get_dt;
    }
    memset(config, 0, sizeof(*config));
    config->Century = 2;
    return SL_STATUS_OK;
}

sl_status_t sl_si91x_calendar_convert_calendar_datetime_to_unix_time(sl_calendar_datetime_config_t *cal,
                                                                     uint32_t *unix_time)
{
    (void)cal;
    if (g_fake_cal.fail_from_cal) {
        return g_fake_cal.fail_from_cal;
    }
    *unix_time = g_fake_cal.next_get_unix;
    return SL_STATUS_OK;
}
