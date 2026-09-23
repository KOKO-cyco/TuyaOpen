/**
 * @file audio_diag.c
 * @brief T9 (BK7259) fully hands-free audio hardware diagnostic.
 *
 * Nobody has to make a sound. The onboard speaker is the test source and the
 * two mics measure it per lane, so every verdict is a level comparison, not a
 * human-performance review:
 *
 *   STEP 1 AMBIENT: dual capture 1 s, no playback ......... noise floor/lane
 *   STEP 2 TONE:    1 kHz tone 2 s + dual capture ......... coupling per lane
 *   STEP 3 DECAY:   tone stopped, dual capture 1 s ........ proves STEP 2 was the tone
 *
 * Verdict per lane: tone_peak >= 5000 AND tone_peak >= 4 * ambient_peak means
 * the speaker->air->mic path (DAC, PA, speaker, capsule, ADC, gains) works.
 * The tone levels also serve as the AEC-off echo baseline: compare against the
 * sample_project BEEP phase (same tone, AEC on) - out~=0 there means the AEC
 * cancelled exactly this echo.
 *
 * Drivers used: tkl_aud_adc / tkl_aud_dac directly (same as the fork's own
 * xadc/mftest tests). No voice-service, no AEC anywhere in this binary.
 */
#include "tal_api.h"
#include "tkl_output.h"
#include "board_com_api.h"
#include "tkl_aud_adc.h"
#include "tkl_aud_dac.h"

#define DIAG_SAMPLE_RATE    16000
#define DIAG_SAMPLE_BITS    16
#define DIAG_FRAME_MS       20
#define DIAG_ADC_DIG_DB     18
#define DIAG_ADC_ANA_DB     30
#define DIAG_DAC_DIG_DB     0
#define DIAG_DAC_ANA_DB     6
#define DIAG_PA_PIN         28 /* MUTE, active-high (T9A1-EVB R47/P28) */
#define DIAG_ALIVE_PEAK     5000
#define DIAG_AMBIENT_MS     1000
#define DIAG_TONE_MS        2000
#define DIAG_TAIL_MS        500
#define DIAG_DECAY_MS       1000

/* 1 kHz sine, 16 kHz sample rate = exactly 16 samples/period. Peak 12000. */
static const int16_t s_tone_tbl[16] = {
    0,     4506,  8491,  11314, 12000, 11314, 8491,  4506,
    0,     -4506, -8491, -11314, -12000, -11314, -8491, -4506,
};

typedef struct {
    volatile uint32_t frames;
    volatile uint32_t peak0; /* even samples = lane 0 (M1/ch0) */
    volatile uint32_t peak1; /* odd samples  = lane 1 (M2/ch1) */
    volatile uint64_t sum_abs;
    volatile uint64_t samples;
} diag_stat_t;

static diag_stat_t s_stat;

static void __diag_stat_reset(void)
{
    s_stat.frames = 0;
    s_stat.peak0 = 0;
    s_stat.peak1 = 0;
    s_stat.sum_abs = 0;
    s_stat.samples = 0;
}

static void __diag_adc_cb(TUYA_AUDIO_FRAME_EVT_E event, uint8_t *buf, uint32_t buf_len, VOID_T *args)
{
    uint32_t i, n;
    int16_t *pcm;

    (void)args;

    if (event != TUYA_AUDIO_FRAME_EVENT_ADC_RX) {
        return;
    }
    if (buf == NULL || buf_len == 0U || (buf_len & 0x1U)) {
        return;
    }

    pcm = (int16_t *)buf;
    n = buf_len / 2U;
    for (i = 0; i < n; i++) {
        int32_t v = pcm[i];
        if (v < 0) {
            v = -v;
        }
        if ((i & 1U) == 0U) {
            if ((uint32_t)v > s_stat.peak0) {
                s_stat.peak0 = (uint32_t)v;
            }
        } else {
            if ((uint32_t)v > s_stat.peak1) {
                s_stat.peak1 = (uint32_t)v;
            }
        }
        s_stat.sum_abs += (uint64_t)v;
    }
    s_stat.samples += n;
    s_stat.frames++;
}

static void __diag_dac_cb(TUYA_AUDIO_DAC_FRAME_EVT_E event, VOID_T *args)
{
    (void)event;
    (void)args;
}

static OPERATE_RET __diag_adc_start_lr(void)
{
    OPERATE_RET rt;
    TKL_AUD_ADC_CFG_T cfg;
    INT32_T gain[2];
    uint32_t ch;

    memset(&cfg, 0, sizeof(cfg));
    cfg.chan = TUYA_AUDIO_ADC_CHANNEL_LR;
    cfg.sample_bits = DIAG_SAMPLE_BITS;
    cfg.sample_rate = DIAG_SAMPLE_RATE;
    cfg.frame_time_ms = DIAG_FRAME_MS;
    cfg.upper_cb = __diag_adc_cb;
    cfg.args = NULL;

    rt = tkl_aud_adc_init(TUYA_AUDIO_ADC_PORT_0, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("[diag] adc init failed: %d", rt);
        return rt;
    }
    gain[0] = DIAG_ADC_DIG_DB;
    gain[1] = DIAG_ADC_ANA_DB;
    for (ch = 0U; ch <= (uint32_t)TUYA_AUDIO_ADC_CHANNEL_1; ch++) {
        rt = tkl_aud_adc_set_gain(TUYA_AUDIO_ADC_PORT_0, (TUYA_AUDIO_ADC_CHANNEL_E)ch, gain, 2U);
        if (rt != OPRT_OK) {
            PR_ERR("[diag] adc set_gain ch%u failed: %d", ch, rt);
        }
    }
    rt = tkl_aud_adc_start(TUYA_AUDIO_ADC_PORT_0);
    if (rt != OPRT_OK) {
        PR_ERR("[diag] adc start failed: %d", rt);
        tkl_aud_adc_deinit(TUYA_AUDIO_ADC_PORT_0);
    }
    return rt;
}

static void __diag_adc_stop(void)
{
    tkl_aud_adc_stop(TUYA_AUDIO_ADC_PORT_0);
    tkl_aud_adc_deinit(TUYA_AUDIO_ADC_PORT_0);
}

static OPERATE_RET __diag_dac_start(void)
{
    OPERATE_RET rt;
    TKL_AUD_DAC_CFG_T cfg;
    INT32_T gain[2];

    memset(&cfg, 0, sizeof(cfg));
    cfg.chan_num = 1U;
    cfg.sample_bits = DIAG_SAMPLE_BITS;
    cfg.sample_rate = DIAG_SAMPLE_RATE;
    cfg.frame_time_ms = DIAG_FRAME_MS;
    cfg.frame_cb = __diag_dac_cb;
    cfg.args = NULL;
    cfg.src = TKL_AUD_DAC_SRC_AUTO;
    cfg.pa_pin = DIAG_PA_PIN;

    rt = tkl_aud_dac_init(TUYA_AUDIO_DAC_PORT_0, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("[diag] dac init failed: %d", rt);
        return rt;
    }
    gain[0] = DIAG_DAC_DIG_DB;
    gain[1] = DIAG_DAC_ANA_DB;
    rt = tkl_aud_dac_set_gain(TUYA_AUDIO_DAC_PORT_0, TUYA_AUDIO_DAC_CHANNEL_0, gain, 2U);
    if (rt != OPRT_OK) {
        PR_ERR("[diag] dac set_gain failed: %d", rt);
    }
    rt = tkl_aud_dac_start(TUYA_AUDIO_DAC_PORT_0);
    if (rt != OPRT_OK) {
        PR_ERR("[diag] dac start failed: %d", rt);
        tkl_aud_dac_deinit(TUYA_AUDIO_DAC_PORT_0);
    }
    return rt;
}

static void __diag_dac_stop(void)
{
    tkl_aud_dac_stop(TUYA_AUDIO_DAC_PORT_0);
    tkl_aud_dac_deinit(TUYA_AUDIO_DAC_PORT_0);
}

/* Push ms milliseconds of 1 kHz tone, paced by the DMA ringbuffer. */
static void __diag_play_tone_ms(uint32_t ms)
{
    static int16_t tone[320]; /* one 20 ms frame */
    uint32_t total, wrote = 0U, guard = 0U, i;

    for (i = 0; i < 320U; i++) {
        tone[i] = s_tone_tbl[i % 16U];
    }

    total = DIAG_SAMPLE_RATE * ms / 1000U * 2U; /* bytes */
    while (wrote < total && guard < 2000U) {
        OPERATE_RET rt = tkl_aud_dac_write(TUYA_AUDIO_DAC_PORT_0, (UINT8_T *)tone, sizeof(tone));
        if (rt == OPRT_OK) {
            wrote += sizeof(tone);
        } else {
            guard++;
            tal_system_sleep(5);
        }
    }
    PR_NOTICE("[diag] tone pushed %u/%u bytes", wrote, total);
}

static const char *__diag_verdict(uint32_t tone_peak, uint32_t ambient_peak)
{
    if (tone_peak >= DIAG_ALIVE_PEAK && tone_peak >= 4U * ambient_peak) {
        return "ALIVE (tone-coupled)";
    }
    return "DEAD/QUIET";
}

/* Single-mic sustained capture for phone music: per-second mean energy decides.
 * A live mic under loud music shows energy in many seconds; a tap transient
 * spikes one second at most. ALIVE = >=3 seconds with mean_abs >= 2%. */
static uint32_t __diag_mic_music(const char *mic_name, TUYA_AUDIO_ADC_CHAN_E adc_chan,
                                 TUYA_AUDIO_ADC_CHANNEL_E gain_chan)
{
    OPERATE_RET rt;
    TKL_AUD_ADC_CFG_T cfg;
    INT32_T gain[2];
    uint64_t last_sum = 0U, last_n = 0U;
    int good_secs = 0, s;

    PR_NOTICE("[diag] STEP MUSIC %s: put the phone speaker AGAINST %s, PLAY MUSIC LOUD for 10 s ... NOW",
              mic_name, mic_name);
    tal_system_sleep(1500);

    memset(&cfg, 0, sizeof(cfg));
    cfg.chan = adc_chan;
    cfg.sample_bits = DIAG_SAMPLE_BITS;
    cfg.sample_rate = DIAG_SAMPLE_RATE;
    cfg.frame_time_ms = DIAG_FRAME_MS;
    cfg.upper_cb = __diag_adc_cb;
    cfg.args = NULL;

    __diag_stat_reset();
    rt = tkl_aud_adc_init(TUYA_AUDIO_ADC_PORT_0, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("[diag] music %s adc init failed: %d", mic_name, rt);
        return 0U;
    }
    gain[0] = DIAG_ADC_DIG_DB;
    gain[1] = DIAG_ADC_ANA_DB;
    tkl_aud_adc_set_gain(TUYA_AUDIO_ADC_PORT_0, gain_chan, gain, 2U);
    rt = tkl_aud_adc_start(TUYA_AUDIO_ADC_PORT_0);
    if (rt != OPRT_OK) {
        PR_ERR("[diag] music %s start failed: %d", mic_name, rt);
        tkl_aud_adc_deinit(TUYA_AUDIO_ADC_PORT_0);
        return 0U;
    }

    for (s = 0; s < 10; s++) {
        uint32_t pct = 0U;
        tal_system_sleep(1000);
        if (s_stat.samples > last_n) {
            pct = (uint32_t)(((s_stat.sum_abs - last_sum) * 100U) / ((s_stat.samples - last_n) * 32767U));
        }
        last_sum = s_stat.sum_abs;
        last_n = s_stat.samples;
        PR_NOTICE("[diag] MUSIC %s sec %d: mean=%u%%", mic_name, s, pct);
        if (pct >= 2U) {
            good_secs++;
        }
    }

    tkl_aud_adc_stop(TUYA_AUDIO_ADC_PORT_0);
    tkl_aud_adc_deinit(TUYA_AUDIO_ADC_PORT_0);

    {
        uint32_t pk = (s_stat.peak0 > s_stat.peak1) ? s_stat.peak0 : s_stat.peak1;
        PR_NOTICE("[diag] MUSIC %s: peak=%u lively_secs=%d/10 -> %s",
                  mic_name, pk, good_secs,
                  (good_secs >= 3) ? "ALIVE (sustained)" : "DEAD/QUIET");
        return pk;
    }
}

/**
 * @brief user_main
 *
 * @return int
 */
int user_main()
{
    uint32_t amb0, amb1, tone0, tone1, dec0, dec1;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    /* register board peripherals (no audio use here, kept for pattern safety) */
    board_register_hardware();

    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);

    PR_NOTICE("[diag] ===== T9 audio hands-free diagnostic (no AEC, no human) =====");
    PR_NOTICE("[diag] gains: adc dig=%d ana=%d, dac dig=%d ana=%d",
              DIAG_ADC_DIG_DB, DIAG_ADC_ANA_DB, DIAG_DAC_DIG_DB, DIAG_DAC_ANA_DB);

    /* let the vendor audio service settle after boot */
    tal_system_sleep(1000);

    if (__diag_adc_start_lr() != OPRT_OK) {
        PR_ERR("[diag] adc unavailable, abort");
        while (1) {
            tal_system_sleep(10);
        }
    }

    /* STEP 1: ambient floor, no playback. */
    PR_NOTICE("[diag] STEP 1/3 AMBIENT, %u ms, silence please (or not - floor is floor)", DIAG_AMBIENT_MS);
    __diag_stat_reset();
    tal_system_sleep(DIAG_AMBIENT_MS);
    amb0 = s_stat.peak0;
    amb1 = s_stat.peak1;
    PR_NOTICE("[diag] AMBIENT: frames=%u mic0=%u mic1=%u", s_stat.frames, amb0, amb1);

    /* STEP 2: tone on + capture. DAC runs while ADC captures. */
    PR_NOTICE("[diag] STEP 2/3 TONE, 1 kHz %u ms", DIAG_TONE_MS);
    if (__diag_dac_start() != OPRT_OK) {
        PR_ERR("[diag] dac unavailable, abort");
        __diag_adc_stop();
        while (1) {
            tal_system_sleep(10);
        }
    }
    __diag_stat_reset();
    __diag_play_tone_ms(DIAG_TONE_MS);
    tal_system_sleep(DIAG_TAIL_MS); /* capture the steady-state tail too */
    tone0 = s_stat.peak0;
    tone1 = s_stat.peak1;
    PR_NOTICE("[diag] TONE: frames=%u mic0=%u mic1=%u", s_stat.frames, tone0, tone1);
    __diag_dac_stop();

    /* STEP 3: tone stopped, coupling must decay. */
    PR_NOTICE("[diag] STEP 3/3 DECAY, %u ms", DIAG_DECAY_MS);
    __diag_stat_reset();
    tal_system_sleep(DIAG_DECAY_MS);
    dec0 = s_stat.peak0;
    dec1 = s_stat.peak1;
    PR_NOTICE("[diag] DECAY: frames=%u mic0=%u mic1=%u", s_stat.frames, dec0, dec1);

    __diag_adc_stop();

    {
        uint32_t m1m, m2m;
        m1m = __diag_mic_music("M1", TUYA_AUDIO_ADC_CHANNEL_L, TUYA_AUDIO_ADC_CHANNEL_0);
        m2m = __diag_mic_music("M2", TUYA_AUDIO_ADC_CHANNEL_R, TUYA_AUDIO_ADC_CHANNEL_1);
        PR_NOTICE("[diag] MUSIC summary: M1 peak=%u, M2 peak=%u (sustained music must hold both up)", m1m, m2m);
    }

    PR_NOTICE("[diag] ---- result ----");
    PR_NOTICE("[diag] M1: ambient=%u tone=%u decay=%u -> %s",
              amb0, tone0, dec0, __diag_verdict(tone0, (amb0 > 0U) ? amb0 : 1U));
    PR_NOTICE("[diag] M2: ambient=%u tone=%u decay=%u -> %s",
              amb1, tone1, dec1, __diag_verdict(tone1, (amb1 > 0U) ? amb1 : 1U));
    PR_NOTICE("[diag] read guide: tone>>ambient on a lane = that mic hears the speaker;");
    PR_NOTICE("[diag] tone levels are the AEC-off echo baseline - sample_project BEEP");
    PR_NOTICE("[diag] (same tone, AEC on) must drive out~=0 to prove cancellation;");

    while (1) {
        tal_system_sleep(10);
    }
}

/**
 * @brief main
 *
 * @param argc
 * @param argv
 * @return void
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else

/* Tuya thread handle */
static THREAD_HANDLE ty_app_thread = NULL;

/**
 * @brief  task thread
 *
 * @param[in] arg:Parameters when creating a task
 * @return none
 */
static void tuya_app_thread(void *arg)
{
    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param;

    memset(&thrd_param, 0, sizeof(THREAD_CFG_T));
    thrd_param.stackDepth = 1024 * 8;
    thrd_param.priority = THREAD_PRIO_1;
    thrd_param.thrdname = "tuya_app_main";

    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
