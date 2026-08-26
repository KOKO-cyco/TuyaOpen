#ifndef FAKE_SL_SI91X_CALENDAR_H
#define FAKE_SL_SI91X_CALENDAR_H

#include <stdint.h>

typedef uint32_t sl_status_t;
#define SL_STATUS_OK                0x0000u
#define SL_STATUS_FAIL              0x0001u
#define SL_STATUS_INVALID_PARAMETER 0x0021u

typedef enum { KHZ_RO_CLK_SEL = 1, KHZ_RC_CLK_SEL = 2, KHZ_XTAL_CLK_SEL = 4 } sl_calendar_clock_t;

typedef struct {
    uint16_t MilliSeconds;
    uint8_t  Second;
    uint8_t  Minute;
    uint8_t  Hour;
    uint8_t  Day;
    uint8_t  DayOfWeek;
    uint8_t  Month;
    uint8_t  Year;
    uint8_t  Century;
} sl_calendar_datetime_config_t;

void        sl_si91x_calendar_init(void);
void        sl_si91x_calendar_deinit(void);
void        sl_si91x_calendar_rtc_start(void);
void        sl_si91x_calendar_rtc_stop(void);
sl_status_t sl_si91x_calendar_set_configuration(sl_calendar_clock_t clock_type);
sl_status_t sl_si91x_calendar_set_date_time(sl_calendar_datetime_config_t *config);
sl_status_t sl_si91x_calendar_get_date_time(sl_calendar_datetime_config_t *config);
sl_status_t sl_si91x_calendar_convert_unix_time_to_calendar_datetime(uint32_t unix_time,
                                                                     sl_calendar_datetime_config_t *cal);
sl_status_t sl_si91x_calendar_convert_calendar_datetime_to_unix_time(sl_calendar_datetime_config_t *cal,
                                                                     uint32_t *unix_time);

typedef struct {
    int      init_calls, deinit_calls, start_calls, stop_calls;
    int      set_cfg_calls, set_dt_calls, get_dt_calls;
    uint32_t last_unix_set;
    uint32_t next_get_unix;
    sl_status_t fail_set_cfg, fail_set_dt, fail_get_dt, fail_to_cal, fail_from_cal;
} fake_calendar_state_t;

extern fake_calendar_state_t g_fake_cal;
void fake_calendar_reset(void);

#endif
