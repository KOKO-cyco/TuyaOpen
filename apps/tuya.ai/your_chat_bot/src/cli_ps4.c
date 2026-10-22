/**
 * @file cli_ps4.c
 * @brief bring-up CLI: arm a timer wake source and enter PS4 deep sleep.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#include <stdlib.h>
#include <string.h>

#include "tal_api.h"
#include "tkl_sleep.h"
#include "tkl_wakeup.h"

static void ps4_cmd(int argc, char *argv[])
{
    TUYA_WAKEUP_SOURCE_BASE_CFG_T wk;
    uint32_t                      ms = 30000;

    if (argc > 1) {
        ms = (uint32_t)strtoul(argv[1], NULL, 0);
    }
    if (0 == ms) {
        PR_ERR("ps4: wake time must be > 0");
        return;
    }

    memset(&wk, 0, sizeof(wk));
    wk.source                            = TUYA_WAKEUP_SOURCE_TIMER;
    wk.wakeup_para.timer_param.timer_num = TUYA_TIMER_NUM_0;
    wk.wakeup_para.timer_param.mode      = TUYA_TIMER_MODE_ONCE;
    wk.wakeup_para.timer_param.ms        = ms;

    if (OPRT_OK != tkl_wakeup_source_set(&wk)) {
        PR_ERR("ps4: arm wake source failed");
        return;
    }

    PR_NOTICE("ps4: entering deep sleep, wake in %u ms", ms);

    tkl_cpu_sleep_mode_set(TRUE, TUYA_CPU_DEEP_SLEEP);
}

static cli_cmd_t s_cli_cmd[] = {
    {.name = "ps4", .func = ps4_cmd, .help = "ps4 [ms]: enter PS4 deep sleep, timer wake (default 30000)"},
};

void tuya_app_cli_init(void)
{
    tal_cli_cmd_register(s_cli_cmd, sizeof(s_cli_cmd) / sizeof(s_cli_cmd[0]));
}
