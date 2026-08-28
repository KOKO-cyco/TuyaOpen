/**
 * @file tuya_ipc_demo.c
 * @brief Tuya IPC demo media callbacks: live camera over P2P, file playback as fallback
 * @version 2.0
 * @date 2026-08-28
 * @copyright Copyright (c) Tuya Inc.
 *
 * One implementation for every board. The camera, the frame queue, the P2P
 * callbacks and the stream parameters are the same code everywhere - they all
 * go through TDL, which is where the chip differences already live.
 *
 * Two things still differ by platform and say so through a feature switch
 * rather than by branching on the OS in the middle of a function:
 *
 *   DEMO_HAS_AUDIO      the mic/speaker path exists (embedded only for now;
 *                       the Linux side has no capture wired up yet)
 *   DEMO_NEEDS_FS_MOUNT the recording filesystem has to be mounted first
 *                       (SD card on embedded; Linux already has one)
 */
#include "tuya_ipc_demo.h"
#include "tuya_cloud_types.h"
#include "tal_log.h"
#include "tal_system.h"
#include "tal_mutex.h"
#include "tal_memory.h"
#include "tuya_ipc_p2p.h"
#include "demo_media_event.h"
#include <string.h>

extern uint64_t tuya_p2p_misc_get_current_time_ms(void);

/* ---------------------------------------------------------------------------
 * Platform feature switches
 * --------------------------------------------------------------------------- */
#if OPERATING_SYSTEM == SYSTEM_LINUX

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEMO_AUDIO_CAPABLE 0
#define DEMO_FS_IS_MOUNTED 1 /* a real filesystem is already there */

#else

#include "board_com_api.h"
#include "tkl_fs.h"

#define DEMO_AUDIO_CAPABLE 1
#define DEMO_FS_IS_MOUNTED 0 /* the SD card has to be mounted first */
#define DEMO_FS_MOUNT      "/sdcard"

#endif

/* Capable and wanted are different questions; both have to say yes. */
#if DEMO_AUDIO_CAPABLE && DEMO_ENABLE_AUDIO
#include "tkl_audio.h"
#include "tkl_vad.h"
#include "tkl_gpio.h"
#include "resample_fixed.h"
#include "modules/g711.h"
#define DEMO_HAS_AUDIO 1
#else
#define DEMO_HAS_AUDIO 0
#endif

/* Nothing to mount when nothing is going to be written. */
#if !DEMO_FS_IS_MOUNTED && DEMO_HAS_LOCAL_STORE
#define DEMO_NEEDS_FS_MOUNT 1
#else
#define DEMO_NEEDS_FS_MOUNT 0
#endif

#if defined(ENABLE_IPC_RING_BUFFER) && (ENABLE_IPC_RING_BUFFER == 1)
#include "tuya_ring_buffer.h"
#define DEMO_HAS_RING_BUFFER 1
#else
#define DEMO_HAS_RING_BUFFER 0
#endif

#if DEMO_HAS_LOCAL_STORE
#include "local_store.h"
#endif

/* A board without a camera is not a broken build: it streams the demo
 * bitstream instead, and the TDL camera library is not linked at all. */
#if defined(ENABLE_CAMERA) && (ENABLE_CAMERA == 1)
#include "tdl_camera_manage.h"
#define DEMO_HAS_CAMERA 1
#else
#define DEMO_HAS_CAMERA 0
#endif

/* Must match what the board's encoder actually emits: the codec is declared
 * once in av_info and never negotiated per frame. */
#if defined(CAMERA_V4L2_H265) && (CAMERA_V4L2_H265 == 1)
#define DEMO_VIDEO_CODEC TY_AV_CODEC_VIDEO_H265
#else
#define DEMO_VIDEO_CODEC TY_AV_CODEC_VIDEO_H264
#endif

#if DEMO_HAS_LOCAL_STORE && defined(CAMERA_DEMO_SD_LIVE_RECORD) && (CAMERA_DEMO_SD_LIVE_RECORD == 1)
#define DEMO_LIVE_RECORD 1
#ifndef CAMERA_DEMO_SD_RECORD_MAX_SEC
#define CAMERA_DEMO_SD_RECORD_MAX_SEC 120
#endif
#else
#define DEMO_LIVE_RECORD 0
#endif

/* ---------------------------------------------------------------------------
 * Stream parameters
 * --------------------------------------------------------------------------- */
#if !defined(CAMERA_DEMO_WIDTH) || !defined(CAMERA_DEMO_HEIGHT) || !defined(CAMERA_DEMO_FPS) ||                    \
    !defined(CAMERA_DEMO_KBPS)
#error "the board .config must set CONFIG_CAMERA_DEMO_{WIDTH,HEIGHT,FPS,KBPS} - Kconfig carries no default"
#endif

/*
 * Per board, because a resolution that streams on one link floods another: an
 * I-frame costing more than a second of the bitrate budget fills the send queue
 * and sheds the frames behind it. Measured: 480x480 at 1024 kbps is 16-19 KB
 * against 128 KB/s and streams clean; 1280x720 at 272 kbps was 42 KB against
 * 34 KB/s and shed 37%. KBPS also sizes the ring buffer,
 * max_frame_size = min(kbps*1024*3/16, 300KB), and is what the App is told.
 */
#define DEMO_CAM_WIDTH  CAMERA_DEMO_WIDTH
#define DEMO_CAM_HEIGHT CAMERA_DEMO_HEIGHT
#define DEMO_CAM_FPS    CAMERA_DEMO_FPS
#define DEMO_CAM_KBPS   CAMERA_DEMO_KBPS

/*
 * A board that cannot program the GOP does not offer the option, and the
 * encoder's own constant is what the App has to be told. On T5AI that is
 * H264_GOP_FRAME_CNT, fixed at build time with no tkl_dvp setter behind it.
 */
#ifndef CAMERA_DEMO_GOP
#define CAMERA_DEMO_GOP 30
#endif
#define DEMO_CAM_GOP CAMERA_DEMO_GOP

/* The sensor is opened at DEMO_CAM_FPS, but media_info advertises 25 the way
 * TuyaOS wukong does; the App paces from its own timestamps either way. */
#define DEMO_AV_FPS 25

#ifndef CAMERA_NAME
#define CAMERA_NAME "camera"
#endif
/* Boards name the same thing differently; try the other spelling before
 * deciding there is no camera. */
#define CAMERA_NAME_ALT "camera_dvp"

/* Must match p2p_init()'s media_frame buffer; do not read media_frame->size */
#define DEMO_P2P_FRAME_CAP    (300 * 1024)
#define DEMO_FRAME_BUF_SIZE   (256 * 1024)
#define DEMO_FRAME_LOG_PERIOD 30
/* Shallow live queue: keep latest, drop oldest - the App's ring jumps to the
 * newest frame when it falls behind, so a deep backlog only adds delay. */
#define DEMO_P2P_QUEUE_DEPTH 2

/* ---------------------------------------------------------------------------
 * File playback fallback
 * --------------------------------------------------------------------------- */
#if OPERATING_SYSTEM == SYSTEM_LINUX
#define DEMO_HAS_FILE_PLAYBACK 1
#define DEMO_FILE_PATH         "demo_video.264"
#elif defined(CAMERA_DEMO_P2P_FILE_H264) && (CAMERA_DEMO_P2P_FILE_H264 == 1)
#define DEMO_HAS_FILE_PLAYBACK 1
extern const uint8_t demo_video_264_start[];
extern const uint8_t demo_video_264_end[];
#else
#define DEMO_HAS_FILE_PLAYBACK 0
#endif

#if DEMO_HAS_FILE_PLAYBACK
/* demo_video.264 in repo: 320x240 Annex-B, ~30fps */
#define DEMO_FILE_FPS    30
#define DEMO_FILE_WIDTH  320
#define DEMO_FILE_HEIGHT 240
#define DEMO_FILE_GOP    30
#define DEMO_FILE_KBPS   512
#endif

/* ---------------------------------------------------------------------------
 * Audio uplink / downlink (mic -> G.711U -> P2P, and back)
 * --------------------------------------------------------------------------- */
#if DEMO_HAS_AUDIO
#define DEMO_MIC_SAMPLE_RATE TKL_AUDIO_SAMPLE_16K
#define DEMO_MIC_DATABITS    TKL_AUDIO_DATABITS_16
#define DEMO_MIC_CHANNEL     TKL_AUDIO_CHANNEL_MONO
#define DEMO_MIC_CARD        TKL_AUDIO_TYPE_BOARD
/* Align boards/T5AI/TUYA_T5AI_BOARD: BOARD_SPEAKER_EN_PIN=GPIO28, high-enable */
#define DEMO_SPK_GPIO          TUYA_GPIO_NUM_28
#define DEMO_SPK_GPIO_POLARITY 0
#define DEMO_SPK_VOLUME        80
/* G.711 8k at 25 fps -> 40 ms/frame = 320 samples = 320 bytes */
#define DEMO_AUDIO_FRAME_BYTES   (8000 / DEMO_AV_FPS)
#define DEMO_AUDIO_RING_FRAMES   8
#define DEMO_AUDIO_RING_CAP      (DEMO_AUDIO_FRAME_BYTES * DEMO_AUDIO_RING_FRAMES)
#define DEMO_AUDIO_PCM_MAX       640
#define DEMO_DOWNLINK_G711_MAX   640
#define DEMO_DOWNLINK_PCM16K_MAX 1280
#endif

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    uint32_t len;
    BOOL_T   is_key;
    uint64_t ts_ms;
} DEMO_P2P_Q_SLOT_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
static MUTEX_HANDLE        s_frame_mutex = NULL;
static uint8_t            *s_q_pool[DEMO_P2P_QUEUE_DEPTH] = {0};
static DEMO_P2P_Q_SLOT_T   s_q_slot[DEMO_P2P_QUEUE_DEPTH];
static uint32_t            s_q_head = 0;
static uint32_t            s_q_tail = 0;
static uint32_t            s_q_count = 0;
static uint32_t            s_frame_slot_cap = 0;
static BOOL_T              s_media_ready = FALSE;
static BOOL_T              s_live_push_enable = FALSE;
static BOOL_T              s_queue_need_iframe = FALSE;
static BOOL_T              s_cam_running = FALSE;
#if DEMO_HAS_CAMERA
static TDL_CAMERA_HANDLE_T s_cam = NULL;
#endif
static uint64_t            s_frame_idx = 0;

#if DEMO_HAS_RING_BUFFER
static RING_BUFFER_USER_HANDLE_T s_ring_w = NULL;
static RING_BUFFER_USER_HANDLE_T s_ring_r = NULL;
static BOOL_T                    s_ring_ready = FALSE;
#endif

#if DEMO_LIVE_RECORD
static BOOL_T   s_rec_wait_iframe = FALSE;
static uint32_t s_rec_wr_fail = 0;
#endif

#if DEMO_HAS_FILE_PLAYBACK
static const uint8_t *s_file_h264 = NULL;
static uint32_t       s_file_size = 0;
static uint32_t       s_file_offset = 0;
static uint32_t       s_file_frame_len = 0;
static uint32_t       s_file_frame_start = 0;
static uint32_t       s_file_is_key = 0;
static uint64_t       s_file_pts_idx = 0;
#if OPERATING_SYSTEM == SYSTEM_LINUX
static uint8_t *s_file_buf = NULL; /* owned here; the embedded blob is not */
#endif
#endif

#if DEMO_HAS_AUDIO
static MUTEX_HANDLE s_audio_mutex = NULL;
static uint8_t      s_audio_ring[DEMO_AUDIO_RING_CAP];
static uint32_t     s_audio_head = 0, s_audio_tail = 0, s_audio_count = 0;
static BOOL_T       s_audio_inited = FALSE;
static BOOL_T       s_mic_running = FALSE;
static BOOL_T       s_spk_active = FALSE;

static OPERATE_RET __demo_audio_uplink_init(void);
static void        __demo_audio_uplink_deinit(void);
static OPERATE_RET __demo_mic_start(void);
static void        __demo_mic_stop(void);
#endif

/* ---------------------------------------------------------------------------
 * Live frame queue
 * --------------------------------------------------------------------------- */
/**
 * @brief Drop all queued P2P video frames
 * @return none
 */
static void __demo_p2p_queue_clear(void)
{
    s_q_head = 0;
    s_q_tail = 0;
    s_q_count = 0;
    s_queue_need_iframe = TRUE;
}

/**
 * @brief Push one encoded frame into the live FIFO; drop oldest when full
 * @param[in] data frame bytes
 * @param[in] len byte length
 * @param[in] is_key TRUE for I frame
 * @param[in] ts_ms capture time in ms (monotonic)
 * @return OPRT_OK on success
 * @note After a drop the queue waits for the next I-frame: handing the App a
 *       P-frame whose reference was thrown away decodes to garbage.
 */
static OPERATE_RET __demo_p2p_queue_push(const uint8_t *data, uint32_t len, BOOL_T is_key, uint64_t ts_ms)
{
    uint8_t *dst;

    if (data == NULL || len == 0 || len > s_frame_slot_cap) {
        return OPRT_INVALID_PARM;
    }
    if (s_q_count >= DEMO_P2P_QUEUE_DEPTH) {
        s_q_head = (s_q_head + 1U) % DEMO_P2P_QUEUE_DEPTH;
        s_q_count--;
        s_queue_need_iframe = TRUE;
    }
    if (s_queue_need_iframe && !is_key) {
        return OPRT_OK;
    }
    dst = s_q_pool[s_q_tail];
    if (dst == NULL) {
        return OPRT_COM_ERROR;
    }
    memcpy(dst, data, len);
    s_q_slot[s_q_tail].len = len;
    s_q_slot[s_q_tail].is_key = is_key;
    s_q_slot[s_q_tail].ts_ms = ts_ms;
    s_q_tail = (s_q_tail + 1U) % DEMO_P2P_QUEUE_DEPTH;
    s_q_count++;
    if (is_key) {
        s_queue_need_iframe = FALSE;
    }
    return OPRT_OK;
}

/**
 * @brief Pop the oldest frame for the P2P media thread
 * @param[out] media_frame output media frame
 * @return OPRT_OK if a frame was returned, OPRT_NOT_FOUND if the queue is empty
 */
static OPERATE_RET __demo_p2p_queue_pop(MEDIA_FRAME *media_frame)
{
    const DEMO_P2P_Q_SLOT_T *slot;
    const uint8_t           *src;

    if (media_frame == NULL || media_frame->data == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (s_q_count == 0) {
        return OPRT_NOT_FOUND;
    }
    slot = &s_q_slot[s_q_head];
    src = s_q_pool[s_q_head];
    if (src == NULL || slot->len == 0 || slot->len > DEMO_P2P_FRAME_CAP) {
        __demo_p2p_queue_clear();
        return OPRT_COM_ERROR;
    }
    memcpy(media_frame->data, src, slot->len);
    media_frame->size = slot->len;
    media_frame->type = slot->is_key ? eVideoIFrame : eVideoPBFrame;
    /* Same monotonic ms for pts and timestamp, as TuyaOS __p2p_h264_cb does */
    media_frame->pts = slot->ts_ms;
    media_frame->timestamp = (uint32_t)slot->ts_ms;
    s_q_head = (s_q_head + 1U) % DEMO_P2P_QUEUE_DEPTH;
    s_q_count--;
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Recording to local_store
 * --------------------------------------------------------------------------- */
#if DEMO_LIVE_RECORD
/**
 * @brief Start (or restart) a recording segment; it begins at the next I-frame
 * @return none
 */
static void __demo_rec_start(void)
{
    OPERATE_RET rt = local_store_rec_start("live");

    if (rt != OPRT_OK) {
        PR_ERR("live rec start failed: %d", rt);
        s_rec_wait_iframe = FALSE;
        return;
    }
    s_rec_wait_iframe = TRUE;
}

/**
 * @brief Stop recording and write the day index
 * @return none
 */
static void __demo_rec_stop(void)
{
    s_rec_wait_iframe = FALSE;
    (void)local_store_rec_stop();
}

/**
 * @brief Append one AU; roll the segment after MAX_SEC on an I-frame
 * @param[in] data Annex-B bytes
 * @param[in] len byte length
 * @param[in] is_key I-frame flag
 * @return none
 */
static void __demo_rec_on_frame(const uint8_t *data, uint32_t len, BOOL_T is_key)
{
    if (!local_store_rec_is_open()) {
        return;
    }
    if (s_rec_wait_iframe) {
        if (!is_key) {
            return;
        }
        s_rec_wait_iframe = FALSE;
    }
    if (is_key && local_store_rec_elapsed_sec() >= (uint32_t)CAMERA_DEMO_SD_RECORD_MAX_SEC) {
        __demo_rec_stop();
        __demo_rec_start();
        if (!local_store_rec_is_open()) {
            return;
        }
        /* The new segment wants an I-frame and this one is it. */
        s_rec_wait_iframe = FALSE;
    }
    if (local_store_rec_write(data, len) != OPRT_OK) {
        s_rec_wr_fail++;
        if ((s_rec_wr_fail % 30) == 1) {
            PR_WARN("live rec write fail cnt=%u", s_rec_wr_fail);
        }
        /* Give up on sustained ENOSPC/IO errors rather than storm the log */
        if (s_rec_wr_fail >= 5) {
            PR_ERR("live rec abort after %u write fails", s_rec_wr_fail);
            s_rec_wr_fail = 0;
            __demo_rec_stop();
        }
    } else {
        s_rec_wr_fail = 0;
    }
}
#endif /* DEMO_LIVE_RECORD */

/* ---------------------------------------------------------------------------
 * Camera
 * --------------------------------------------------------------------------- */
#if DEMO_HAS_CAMERA
/**
 * @brief Check that an Annex-B start code is present
 * @param[in] data encoded frame, H.264 or HEVC
 * @param[in] len byte length
 * @return TRUE if at least one 00 00 01 / 00 00 00 01 prefix exists
 * @note A DMA-truncated buffer still looks like a frame to the layer below;
 *       it is only recognisable as garbage here, before it costs bandwidth.
 */
static BOOL_T __demo_au_has_annexb(const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (data == NULL || len < 4) {
        return FALSE;
    }
    for (i = 0; i + 3 < len; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            return TRUE;
        }
        if (i + 4 < len && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief Encoded frame from the camera TDD
 * @param[in] hdl camera handle
 * @param[in] frame encoded frame
 * @return OPRT_OK
 * @note Runs on the capture thread: copy and return, the P2P sender drains the
 *       other end at its own pace.
 */
static OPERATE_RET __demo_encoded_frame_cb(TDL_CAMERA_HANDLE_T hdl, TDL_CAMERA_FRAME_T *frame)
{
    BOOL_T   is_key;
    uint64_t ts_ms;

    (void)hdl;
    if (frame == NULL || frame->data == NULL || frame->data_len == 0) {
        return OPRT_OK;
    }
    if (s_frame_mutex == NULL || s_frame_slot_cap == 0 || !s_live_push_enable) {
        /* No client, or not LIVE: do not feed P2P */
        return OPRT_OK;
    }
    if (frame->data_len > s_frame_slot_cap) {
        PR_WARN("encoded frame too large: %u > %u", (uint32_t)frame->data_len, (uint32_t)s_frame_slot_cap);
        return OPRT_OK;
    }
    if (!__demo_au_has_annexb((const uint8_t *)frame->data, frame->data_len)) {
        static uint32_t s_bad_au_cnt = 0;
        if ((s_bad_au_cnt++ % 30) == 0) {
            PR_NOTICE("drop AU without Annex-B start code len=%u cnt=%u", (uint32_t)frame->data_len, s_bad_au_cnt);
        }
        return OPRT_OK;
    }
    if (!frame->is_complete) {
        static uint32_t s_incomplete_cnt = 0;
        if ((s_incomplete_cnt++ % 30) == 0) {
            PR_DEBUG("drop incomplete AU len=%u total=%u cnt=%u", (uint32_t)frame->data_len,
                     (uint32_t)frame->total_frame_len, s_incomplete_cnt);
        }
        return OPRT_OK;
    }
    if (frame->total_frame_len > 0 && frame->data_len != frame->total_frame_len) {
        static uint32_t s_len_mismatch_cnt = 0;
        if ((s_len_mismatch_cnt++ % 10) == 0) {
            PR_DEBUG("drop len mismatch got=%u expect=%u cnt=%u", (uint32_t)frame->data_len,
                     (uint32_t)frame->total_frame_len, s_len_mismatch_cnt);
        }
        return OPRT_OK;
    }

    is_key = frame->is_i_frame ? TRUE : FALSE;
    ts_ms = tuya_p2p_misc_get_current_time_ms();
    s_frame_idx++;

    tal_mutex_lock(s_frame_mutex);
    (void)__demo_p2p_queue_push((const uint8_t *)frame->data, frame->data_len, is_key, ts_ms);
    tal_mutex_unlock(s_frame_mutex);

#if DEMO_HAS_RING_BUFFER
    if (s_ring_ready && s_ring_w != NULL) {
        (void)tuya_ipc_ring_buffer_append_data_with_timestamp(s_ring_w, (uint8_t *)frame->data, frame->data_len,
                                                             is_key ? E_VIDEO_I_FRAME : E_VIDEO_PB_FRAME,
                                                             ts_ms * 1000ULL, ts_ms);
    }
#endif

#if DEMO_LIVE_RECORD
    __demo_rec_on_frame((const uint8_t *)frame->data, frame->data_len, is_key);
#endif

    if (is_key || (s_frame_idx % DEMO_FRAME_LOG_PERIOD) == 0) {
        PR_NOTICE("enc frames=%llu len=%u i=%u q=%u", (unsigned long long)s_frame_idx, (uint32_t)frame->data_len,
                  (uint32_t)(is_key ? 1 : 0), (uint32_t)s_q_count);
    }
    return OPRT_OK;
}

/**
 * @brief Look up the registered camera without opening it
 * @return handle, or NULL when the board registered none
 */
static TDL_CAMERA_HANDLE_T __demo_camera_find(void)
{
    TDL_CAMERA_HANDLE_T hdl = tdl_camera_find_dev((char *)CAMERA_NAME);

    if (hdl == NULL) {
        hdl = tdl_camera_find_dev((char *)CAMERA_NAME_ALT);
    }
    return hdl;
}

/**
 * @brief Whether this board has a camera to stream from
 * @return TRUE when one is registered
 * @note Looking it up powers nothing on, so it is safe to ask at startup.
 */
static BOOL_T __demo_camera_present(void)
{
    return (__demo_camera_find() != NULL) ? TRUE : FALSE;
}

/**
 * @brief Open the camera and ask it for encoded video
 * @return OPRT_OK when live video is available
 * @note Opening on demand keeps the sensor and encoder idle until someone is
 *       actually watching.
 */
static OPERATE_RET __demo_camera_open(void)
{
    TDL_CAMERA_CFG_T      cfg;
    TDL_CAMERA_DEV_INFO_T info;
    OPERATE_RET           rt;

    if (s_cam_running) {
        return OPRT_OK;
    }

    s_cam = __demo_camera_find();
    if (s_cam == NULL) {
        PR_WARN("camera '%s' not registered", CAMERA_NAME);
        return OPRT_NOT_FOUND;
    }

    /* Ask before assuming: a node with no hardware encoder behind it cannot
     * give encoded video, and streaming nothing is worse than saying so. */
    memset(&info, 0, sizeof(info));
    if (tdl_camera_dev_get_info(s_cam, &info) == OPRT_OK && info.supported_fmts != 0 &&
        !(info.supported_fmts & TDL_CAMERA_FMT_H264)) {
        PR_WARN("camera '%s' has no encoded output (supported_fmts=0x%x)", CAMERA_NAME, info.supported_fmts);
        s_cam = NULL;
        return OPRT_NOT_SUPPORTED;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.width = DEMO_CAM_WIDTH;
    cfg.height = DEMO_CAM_HEIGHT;
    cfg.fps = DEMO_CAM_FPS;
    cfg.bitrate_kbps = DEMO_CAM_KBPS;
    cfg.gop = DEMO_CAM_GOP;
    /* TDL_CAMERA_FMT_E has no H.265 member, so this reads as "hardware-encoded
     * video"; which codec comes out is the board encoder's setting. */
    cfg.out_fmt = TDL_CAMERA_FMT_H264;
    cfg.get_encoded_frame_cb = __demo_encoded_frame_cb;

    rt = tdl_camera_dev_open(s_cam, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("tdl_camera_dev_open failed: %d", rt);
        s_cam = NULL;
        return rt;
    }

    s_cam_running = TRUE;
    PR_NOTICE("live camera started: %ux%u@%u %s", (uint32_t)DEMO_CAM_WIDTH, (uint32_t)DEMO_CAM_HEIGHT,
              (uint32_t)DEMO_CAM_FPS, DEMO_VIDEO_CODEC == TY_AV_CODEC_VIDEO_H265 ? "H265" : "H264");
    return OPRT_OK;
}

/**
 * @brief Stop the camera's output
 * @return none
 * @note A stop, not a teardown. close() reaches tkl_dvp_stop, which is what one
 *       viewer leaving is worth; the sensor registers and the frame pool stay
 *       up for the next one. A full bring-up per session is what made the layer
 *       above allocate a fresh megabyte each time.
 */
static void __demo_camera_close(void)
{
    if (s_cam != NULL && s_cam_running) {
        (void)tdl_camera_dev_close(s_cam);
        s_cam_running = FALSE;
        PR_NOTICE("live camera stopped");
    }
    s_cam = NULL;
}

/**
 * @brief Ask the encoder to emit an IDR now
 * @return OPRT_OK when the encoder accepted the request
 */
static OPERATE_RET __demo_camera_request_i_frame(void)
{
    if (s_cam == NULL) {
        return OPRT_NOT_SUPPORTED;
    }
    return tdl_camera_dev_request_i_frame(s_cam);
}

/**
 * @brief Move the encoder's target bitrate
 * @param[in] kbps requested bitrate
 * @return OPRT_OK when the encoder accepted the change
 */
static OPERATE_RET __demo_camera_set_bitrate(uint32_t kbps)
{
    if (s_cam == NULL) {
        return OPRT_NOT_SUPPORTED;
    }
    return tdl_camera_dev_set_bitrate(s_cam, kbps);
}

#else /* !DEMO_HAS_CAMERA - the demo bitstream is the only source */

static BOOL_T      __demo_camera_present(void)              { return FALSE; }
static OPERATE_RET __demo_camera_open(void)                 { return OPRT_NOT_SUPPORTED; }
static void        __demo_camera_close(void)                { }
static OPERATE_RET __demo_camera_request_i_frame(void)      { return OPRT_NOT_SUPPORTED; }
static OPERATE_RET __demo_camera_set_bitrate(uint32_t kbps) { (void)kbps; return OPRT_NOT_SUPPORTED; }

#endif /* DEMO_HAS_CAMERA */

/* ---------------------------------------------------------------------------
 * File playback
 * --------------------------------------------------------------------------- */
#if DEMO_HAS_FILE_PLAYBACK
/**
 * @brief Parse one H.264 AU out of an Annex-B buffer
 * @param[in] video_buf buffer base
 * @param[in] offset absolute offset of @p video_buf
 * @param[in] buf_size bytes from offset to the end
 * @param[out] is_key_frame keyframe flag
 * @param[out] frame_len AU length
 * @param[out] frame_start absolute start offset
 * @return 0 on success, -1 on failure
 */
static int __demo_read_one_au(const uint8_t *video_buf, uint32_t offset, uint32_t buf_size, uint32_t *is_key_frame,
                              uint32_t *frame_len, uint32_t *frame_start)
{
    uint32_t pos = 0;
    int      need_calc = 0;
    uint8_t  nal_type = 0;
    int      idx = 0;

    if (buf_size <= 5) {
        return -1;
    }
    for (pos = 0; pos <= buf_size - 5; pos++) {
        if (video_buf[pos] == 0x00 && video_buf[pos + 1] == 0x00 && video_buf[pos + 2] == 0x00 &&
            video_buf[pos + 3] == 0x01) {
            nal_type = (uint8_t)(video_buf[pos + 4] & 0x1f);
            if (nal_type == 0x7) {
                if (need_calc == 1) {
                    *frame_len = pos - (uint32_t)idx;
                    return 0;
                }
                *is_key_frame = 1;
                *frame_start = offset + pos;
                need_calc = 1;
                idx = (int)pos;
            } else if (nal_type == 0x1) {
                if (need_calc) {
                    *frame_len = pos - (uint32_t)idx;
                    return 0;
                }
                *frame_start = offset + pos;
                *is_key_frame = 0;
                idx = (int)pos;
                need_calc = 1;
            }
        }
    }
    *frame_len = buf_size;
    return 0;
}

/**
 * @brief Point the player at the demo bitstream
 * @return OPRT_OK when a stream is loaded
 * @note Where the bytes come from is the one thing the two builds disagree on:
 *       a file on a real filesystem, or a blob linked into the image.
 */
static OPERATE_RET __demo_file_load(void)
{
#if OPERATING_SYSTEM == SYSTEM_LINUX
    char  path[512] = {0};
    FILE *fp = NULL;
    long  size;

    if (getcwd(path, sizeof(path)) == NULL) {
        PR_ERR("getcwd failed");
        return OPRT_COM_ERROR;
    }
    strncat(path, "/" DEMO_FILE_PATH, sizeof(path) - strlen(path) - 1);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        PR_WARN("no demo video file at %s", path);
        return OPRT_NOT_FOUND;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 128) {
        PR_ERR("demo video file too small: %ld", size);
        fclose(fp);
        return OPRT_COM_ERROR;
    }
    s_file_buf = (uint8_t *)malloc((size_t)size);
    if (s_file_buf == NULL) {
        PR_ERR("malloc %ld for demo video failed", size);
        fclose(fp);
        return OPRT_MALLOC_FAILED;
    }
    if (fread(s_file_buf, 1, (size_t)size, fp) != (size_t)size) {
        PR_ERR("fread demo video incomplete");
        free(s_file_buf);
        s_file_buf = NULL;
        fclose(fp);
        return OPRT_COM_ERROR;
    }
    fclose(fp);
    s_file_h264 = s_file_buf;
    s_file_size = (uint32_t)size;
#else
    s_file_h264 = demo_video_264_start;
    s_file_size = (uint32_t)(demo_video_264_end - demo_video_264_start);
    if (s_file_h264 == NULL || s_file_size < 128) {
        PR_ERR("embedded demo_video.264 invalid size=%u", (uint32_t)s_file_size);
        s_file_h264 = NULL;
        s_file_size = 0;
        return OPRT_COM_ERROR;
    }
#endif
    s_file_offset = 0;
    s_file_frame_len = 0;
    s_file_frame_start = 0;
    s_file_is_key = 0;
    s_file_pts_idx = 0;
    PR_NOTICE("file playback ready: %u bytes %ux%u@%u", (uint32_t)s_file_size, (uint32_t)DEMO_FILE_WIDTH,
              (uint32_t)DEMO_FILE_HEIGHT, (uint32_t)DEMO_FILE_FPS);
    return OPRT_OK;
}

/**
 * @brief Release the loaded bitstream
 * @return none
 */
static void __demo_file_unload(void)
{
#if OPERATING_SYSTEM == SYSTEM_LINUX
    if (s_file_buf != NULL) {
        free(s_file_buf);
        s_file_buf = NULL;
    }
#endif
    s_file_h264 = NULL;
    s_file_size = 0;
}

/**
 * @brief Serve the next AU from the loaded bitstream, looping at the end
 * @param[in,out] media_frame media frame
 * @return 0 on success, -1 on failure
 */
static int __demo_file_get_frame(MEDIA_FRAME *media_frame)
{
    uint64_t pts_ms;
    int      ret;

    if (s_file_h264 == NULL || s_file_size == 0) {
        return -1;
    }

    s_file_offset = s_file_frame_start + s_file_frame_len;
    if (s_file_offset >= s_file_size) {
        s_file_offset = 0;
        s_file_frame_len = 0;
        s_file_frame_start = 0;
        s_file_is_key = 0;
        s_file_pts_idx = 0;
    }

    ret = __demo_read_one_au(s_file_h264 + s_file_offset, s_file_offset, s_file_size - s_file_offset, &s_file_is_key,
                             &s_file_frame_len, &s_file_frame_start);
    if (ret != 0 || s_file_frame_len == 0) {
        return -1;
    }
    if (s_file_frame_len > DEMO_P2P_FRAME_CAP) {
        PR_WARN("demo AU too large len=%u cap=%u", (uint32_t)s_file_frame_len, (uint32_t)DEMO_P2P_FRAME_CAP);
        return -1;
    }

    memcpy(media_frame->data, s_file_h264 + s_file_frame_start, s_file_frame_len);
    media_frame->size = s_file_frame_len;
    media_frame->type = s_file_is_key ? eVideoIFrame : eVideoPBFrame;
    pts_ms = (s_file_pts_idx * 1000ULL) / DEMO_FILE_FPS;
    s_file_pts_idx++;
    media_frame->timestamp = (uint32_t)pts_ms;
    media_frame->pts = pts_ms * 1000ULL;

    /* The file has no capture clock, so pace it here or the whole clip leaves
     * in one burst and the App has nowhere to put it. */
    tal_system_sleep(1000 / DEMO_FILE_FPS);
    return 0;
}
#endif /* DEMO_HAS_FILE_PLAYBACK */

/* ---------------------------------------------------------------------------
 * Stream parameters published to the App
 * --------------------------------------------------------------------------- */
/**
 * @brief Tell P2P what the stream actually is
 * @param[in] from_camera TRUE for the sensor stream, FALSE for the demo file
 * @return none
 * @note Without this the App renders at whatever default it assumes, which
 *       distorts the picture whenever that is not the sensor's aspect ratio.
 */
static void __demo_init_p2p_av_info(BOOL_T from_camera)
{
    TRANS_IPC_AV_INFO_T av_info;
    OPERATE_RET         rt;
    uint32_t            w, h, fps, gop, kbps;
    int                 i;

    if (from_camera) {
        w = DEMO_CAM_WIDTH;
        h = DEMO_CAM_HEIGHT;
        fps = DEMO_AV_FPS;
        gop = DEMO_CAM_GOP;
        kbps = DEMO_CAM_KBPS;
    } else {
#if DEMO_HAS_FILE_PLAYBACK
        w = DEMO_FILE_WIDTH;
        h = DEMO_FILE_HEIGHT;
        fps = DEMO_FILE_FPS;
        gop = DEMO_FILE_GOP;
        kbps = DEMO_FILE_KBPS;
#else
        return;
#endif
    }

    memset(&av_info, 0, sizeof(av_info));
    /* One sensor stream serves both clarity levels: HIGH -> main, STANDARD -> sub */
    for (i = 0; i < 2; i++) {
        int s = (i == 0) ? eIpcStreamVideoMain : eIpcStreamVideoSub;

        av_info.video_codec[s] = DEMO_VIDEO_CODEC;
        av_info.fps[s] = fps;
        av_info.gop[s] = gop;
        av_info.bitrate[s] = kbps;
        av_info.width[s] = w;
        av_info.height[s] = h;
    }
#if DEMO_HAS_AUDIO
    /* Uplink is G.711 mu-law 8k mono, as TuyaOS wukong advertises it */
    av_info.audio_codec = TY_AV_CODEC_AUDIO_G711U;
    av_info.audio_sample = TY_AUDIO_SAMPLE_8K;
    av_info.audio_databits = TY_AUDIO_DATABITS_16;
    av_info.audio_channel = TY_AUDIO_CHANNEL_MONO;
#endif

    rt = tuya_ipc_init_trans_av_info(&av_info);
    if (rt != OPRT_OK) {
        PR_ERR("tuya_ipc_init_trans_av_info failed: %d", rt);
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */
/**
 * @brief Set up buffers and publish stream parameters
 * @return none
 * @note p2p_init() already ran in TUYA_APP_Start(); the camera itself is not
 *       opened until the App asks for LIVE.
 */
void tuya_ipc_demo_start(void)
{
    OPERATE_RET rt;
    BOOL_T      have_camera;
    uint32_t    i;

    if (s_media_ready) {
        return;
    }

    rt = tal_mutex_create_init(&s_frame_mutex);
    if (rt != OPRT_OK) {
        PR_ERR("frame mutex create failed: %d", rt);
        return;
    }

    s_frame_idx = 0;
    s_live_push_enable = FALSE;
    __demo_p2p_queue_clear();

    for (i = 0; i < DEMO_P2P_QUEUE_DEPTH; i++) {
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
        s_q_pool[i] = (uint8_t *)tal_psram_malloc(DEMO_FRAME_BUF_SIZE);
#else
        s_q_pool[i] = (uint8_t *)tal_malloc(DEMO_FRAME_BUF_SIZE);
#endif
        if (s_q_pool[i] == NULL) {
            PR_ERR("alloc p2p queue slot %u failed", i);
            return;
        }
    }
    s_frame_slot_cap = DEMO_FRAME_BUF_SIZE;

    /* Looking the device up is cheap and does not power anything on, so the
     * App can be told the truth about which stream it is going to get. */
    have_camera = __demo_camera_present();

#if DEMO_HAS_FILE_PLAYBACK
    if (__demo_file_load() != OPRT_OK && !have_camera) {
        PR_ERR("neither a camera nor a demo bitstream is available");
    }
#else
    if (!have_camera) {
        PR_ERR("no camera registered and this build has no file playback");
    }
#endif

    __demo_init_p2p_av_info(have_camera);

#if DEMO_HAS_RING_BUFFER
    {
        RING_BUFFER_INIT_PARAM_T rp = {0};

        rp.bitrate = DEMO_CAM_KBPS;
        rp.fps = DEMO_AV_FPS;
        rp.max_buffer_seconds = 2;
        if (tuya_ipc_ring_buffer_init(0, 0, E_IPC_STREAM_VIDEO_MAIN, &rp) == OPRT_OK) {
            s_ring_w = tuya_ipc_ring_buffer_open(0, 0, E_IPC_STREAM_VIDEO_MAIN, E_RBUF_WRITE);
            s_ring_r = tuya_ipc_ring_buffer_open(0, 0, E_IPC_STREAM_VIDEO_MAIN, E_RBUF_READ);
            s_ring_ready = (s_ring_w != NULL && s_ring_r != NULL) ? TRUE : FALSE;
            PR_NOTICE("ring_buffer live video %s", s_ring_ready ? "ready" : "open fail");
        }
    }
#endif

#if DEMO_HAS_AUDIO
    if (__demo_audio_uplink_init() != OPRT_OK) {
        PR_ERR("audio uplink init failed");
    }
#endif

    /*
     * Ready before registering, and registered before the filesystem: the App
     * can ask for LIVE the instant the callbacks exist, and a card that is not
     * there costs a second of retries. Behind the mount, that second landed
     * between the session coming up and this app being able to serve it, and
     * the App got a stream that never produced a frame.
     */
    s_media_ready = TRUE;
    demo_media_event_register();

#if DEMO_NEEDS_FS_MOUNT
    if (tkl_fs_mount(DEMO_FS_MOUNT, DEV_SDCARD) != OPRT_OK) {
        PR_ERR("mount %s failed (FAT card? SDIO=P2/P3/...)", DEMO_FS_MOUNT);
    } else {
        PR_NOTICE("mount ok: %s", DEMO_FS_MOUNT);
    }
#endif

    PR_NOTICE("tuya_ipc_demo: ready (source=%s, sensor %ux%u@%u, av fps %u, live_rec=%d)",
              have_camera ? "camera" : "file", (uint32_t)DEMO_CAM_WIDTH, (uint32_t)DEMO_CAM_HEIGHT,
              (uint32_t)DEMO_CAM_FPS, (uint32_t)DEMO_AV_FPS, (int)DEMO_LIVE_RECORD);
}

/**
 * @brief Release everything this demo owns
 * @return none
 */
void tuya_ipc_demo_end(void)
{
    uint32_t i;

    s_live_push_enable = FALSE;
#if DEMO_HAS_AUDIO
    __demo_mic_stop();
#endif
    __demo_camera_close();
    s_media_ready = FALSE;

    if (s_frame_mutex != NULL) {
        tal_mutex_lock(s_frame_mutex);
        __demo_p2p_queue_clear();
        tal_mutex_unlock(s_frame_mutex);
    }
    for (i = 0; i < DEMO_P2P_QUEUE_DEPTH; i++) {
        if (s_q_pool[i] != NULL) {
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
            tal_psram_free(s_q_pool[i]);
#else
            tal_free(s_q_pool[i]);
#endif
            s_q_pool[i] = NULL;
        }
    }
    s_frame_slot_cap = 0;
    if (s_frame_mutex != NULL) {
        tal_mutex_release(s_frame_mutex);
        s_frame_mutex = NULL;
    }

#if DEMO_HAS_FILE_PLAYBACK
    __demo_file_unload();
#endif
#if DEMO_HAS_AUDIO
    __demo_audio_uplink_deinit();
#endif
}

/* ---------------------------------------------------------------------------
 * P2P callbacks
 * --------------------------------------------------------------------------- */
/**
 * @brief LIVE video start: open the camera and begin feeding P2P
 * @return 0 on success
 */
int demo_on_live_video_start_callback(void)
{
    OPERATE_RET rt;

    if (!s_media_ready) {
        return -1;
    }

    rt = __demo_camera_open();
    if (rt != OPRT_OK) {
#if DEMO_HAS_FILE_PLAYBACK
        /* Not fatal: the demo bitstream still plays, so the App gets a picture
         * either way. */
        PR_WARN("LIVE start: no camera (%d), falling back to file playback", rt);
#else
        PR_ERR("LIVE start: camera open failed: %d", rt);
        return -1;
#endif
    }

#if DEMO_LIVE_RECORD
    if (!local_store_rec_is_open()) {
        __demo_rec_start();
    }
#endif

    if (s_frame_mutex != NULL) {
        tal_mutex_lock(s_frame_mutex);
        __demo_p2p_queue_clear();
        tal_mutex_unlock(s_frame_mutex);
    }

#if DEMO_HAS_AUDIO
    if (!s_mic_running && __demo_mic_start() != OPRT_OK) {
        PR_ERR("LIVE start: mic start failed");
    }
#endif

    s_live_push_enable = TRUE;
    PR_NOTICE("LIVE video start: push enabled");
    return 0;
}

/**
 * @brief LIVE video stop: stop the feed and the camera
 * @return 0 on success
 * @note Camera first so the encode callback stops, then the recording, or a
 *       late frame writes into a closed file.
 */
int demo_on_live_video_stop_callback(void)
{
    s_live_push_enable = FALSE;
    if (s_frame_mutex != NULL) {
        tal_mutex_lock(s_frame_mutex);
        __demo_p2p_queue_clear();
        tal_mutex_unlock(s_frame_mutex);
    }
    __demo_camera_close();
#if DEMO_LIVE_RECORD
    __demo_rec_stop();
#endif
#if DEMO_HAS_AUDIO
    demo_mic_uplink_pause();
#endif
    PR_NOTICE("LIVE video stop: camera closed");
    return 0;
}

/**
 * @brief Session ended
 * @return 0 on success
 */
int demo_on_signal_disconnect_callback(void)
{
    (void)demo_on_live_video_stop_callback();
#if DEMO_HAS_AUDIO
    /* Release the mic: TuyaOS keeps an always-on AI path, this demo has none */
    __demo_mic_stop();
#endif
    return 0;
}

/**
 * @brief Hand one encoded frame to the P2P sender
 * @param[in,out] media_frame media frame (buffer owned by the P2P session)
 * @return 0 on success, -1 when nothing is ready yet
 */
int demo_on_get_video_frame_callback(MEDIA_FRAME *media_frame)
{
    OPERATE_RET rt;

    if (media_frame == NULL || media_frame->data == NULL || !s_media_ready) {
        tal_system_sleep(10);
        return -1;
    }

#if DEMO_HAS_FILE_PLAYBACK
    /* The camera wins whenever it is running; the file is only the fallback. */
    if (!s_cam_running) {
        return __demo_file_get_frame(media_frame);
    }
#endif

#if DEMO_HAS_RING_BUFFER
    if (s_ring_ready && s_ring_r != NULL) {
        RING_BUFFER_NODE_T *node = tuya_ipc_ring_buffer_get_frame(s_ring_r, FALSE);

        if (node != NULL && node->raw_data != NULL && node->size > 0 && node->size <= DEMO_P2P_FRAME_CAP) {
            memcpy(media_frame->data, node->raw_data, node->size);
            media_frame->size = node->size;
            media_frame->type = (node->type == E_VIDEO_I_FRAME) ? eVideoIFrame : eVideoPBFrame;
            media_frame->pts = node->timestamp;
            media_frame->timestamp = (uint32_t)node->timestamp;
            return 0;
        }
        tal_system_sleep(10);
        return -1;
    }
#endif

    tal_mutex_lock(s_frame_mutex);
    rt = __demo_p2p_queue_pop(media_frame);
    tal_mutex_unlock(s_frame_mutex);
    if (rt != OPRT_OK) {
        tal_system_sleep(10);
        return -1;
    }
    return 0;
}

/**
 * @brief Ask the encoder for a key frame now
 * @return 0 when the encoder accepted the request
 * @note File playback has no encoder to ask, and a sensor driver without one
 *       places key frames on its own schedule.
 */
int demo_on_request_i_frame_callback(void)
{
    return (__demo_camera_request_i_frame() == OPRT_OK) ? 0 : -1;
}

/**
 * @brief Move the encoder's target bitrate
 * @param[in] kbps requested bitrate
 * @return 0 when the encoder accepted the change
 */
int demo_on_set_video_bitrate_callback(uint32_t kbps)
{
    return (__demo_camera_set_bitrate(kbps) == OPRT_OK) ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * Audio uplink: mic (16k PCM) -> resample 8k -> G.711U -> P2P pull
 * Audio downlink: APP G.711U -> resample 16k -> speaker
 *
 * Still platform-gated: the Linux side has no capture path wired up yet, so
 * these callbacks are stubs there and the App is told there is no audio.
 * --------------------------------------------------------------------------- */
#if DEMO_HAS_AUDIO

/**
 * @brief G.711 mu-law encode
 * @param[in] pcm 16-bit samples
 * @param[in] n sample count
 * @param[out] out one byte per sample
 * @return none
 */
static void __demo_g711u_encode(const int16_t *pcm, size_t n, uint8_t *out)
{
    size_t i;

    for (i = 0; i < n; i++) {
        out[i] = (uint8_t)linear2ulaw((int)pcm[i]);
    }
}

/**
 * @brief Push G.711 bytes into the ring, dropping oldest when full
 * @param[in] data encoded bytes
 * @param[in] len byte count
 * @return none
 */
static void __demo_audio_ring_push(const uint8_t *data, size_t len)
{
    size_t i;

    tal_mutex_lock(s_audio_mutex);
    for (i = 0; i < len; i++) {
        if (s_audio_count >= DEMO_AUDIO_RING_CAP) {
            s_audio_head = (s_audio_head + 1U) % DEMO_AUDIO_RING_CAP;
            s_audio_count--;
        }
        s_audio_ring[s_audio_tail] = data[i];
        s_audio_tail = (s_audio_tail + 1U) % DEMO_AUDIO_RING_CAP;
        s_audio_count++;
    }
    tal_mutex_unlock(s_audio_mutex);
}

/**
 * @brief Mic frame callback: PCM 16k -> resample 8k -> G.711U -> ring
 * @param[in] pframe captured frame
 * @return 0
 */
static int __demo_mic_frame_put_cb(TKL_AUDIO_FRAME_INFO_T *pframe)
{
    static int16_t s_pcm8k[DEMO_AUDIO_PCM_MAX];
    static uint8_t s_g711[DEMO_AUDIO_PCM_MAX];
    size_t         in_frames, out_frames = 0;
    int            ret;

    if (!s_mic_running || pframe == NULL || pframe->pbuf == NULL || pframe->used_size == 0) {
        return 0;
    }
    in_frames = pframe->used_size / 2U; /* 16-bit mono -> samples */
    {
        static uint32_t s_put_cnt = 0;
        if ((s_put_cnt++ % 100) == 0) {
            PR_DEBUG("uplink mic put n=%u bytes=%u samples=%u", s_put_cnt, pframe->used_size, (uint32_t)in_frames);
        }
    }
    if (in_frames == 0 || in_frames > DEMO_AUDIO_PCM_MAX) {
        return 0;
    }
    ret = resample_to_8k_fixed((const int16_t *)pframe->pbuf, in_frames, 16000, 1, s_pcm8k, &out_frames);
    if (ret != 0 || out_frames == 0) {
        return 0;
    }
    __demo_g711u_encode(s_pcm8k, out_frames, s_g711);
    __demo_audio_ring_push(s_g711, out_frames);
    return 0;
}

/**
 * @brief Init and start mic capture (called on LIVE start)
 * @return OPRT_OK on success
 */
static OPERATE_RET __demo_mic_start(void)
{
    TKL_AUDIO_CONFIG_T cfg;
    OPERATE_RET        rt;

    memset(&cfg, 0, sizeof(cfg));
    /* enable=1 selects the vendor AFE. The enable=0 raw-capture path puts DMA
     * on the log UART and silences all logging after the sensor's set_ppi.
     * enable=1 also makes tkl_audio set chl_num=2, which hardware AEC needs. */
    cfg.enable = 1;
    cfg.card = DEMO_MIC_CARD;
    cfg.ai_chn = TKL_AI_0;
    cfg.sample = DEMO_MIC_SAMPLE_RATE;
    cfg.spk_sample = DEMO_MIC_SAMPLE_RATE;
    cfg.datebits = DEMO_MIC_DATABITS;
    cfg.channel = DEMO_MIC_CHANNEL;
    cfg.codectype = TKL_CODEC_AUDIO_PCM;
    cfg.spk_gpio = DEMO_SPK_GPIO;
    cfg.spk_gpio_polarity = DEMO_SPK_GPIO_POLARITY;
    cfg.spk_volume = DEMO_SPK_VOLUME;
    cfg.put_cb = __demo_mic_frame_put_cb;

    rt = tkl_ai_init(&cfg, 1);
    if (rt != OPRT_OK) {
        PR_ERR("tkl_ai_init failed: %d", rt);
        return rt;
    }
    rt = tkl_ai_start(cfg.card, TKL_AI_0);
    if (rt != OPRT_OK) {
        PR_ERR("tkl_ai_start failed: %d", rt);
        (void)tkl_ai_uninit();
        return rt;
    }
    /*
     * tkl_ai_stop() parks the amplifier pin at the mute level and tkl_ai_init()
     * only reconfigures that pin rather than re-asserting it, so every LIVE
     * restart leaves the speaker muted while tkl_ao_put_frame() keeps reporting
     * success. Drive it here rather than through tkl_ai_set_vol(), which would
     * also overwrite the mic gain.
     */
    rt = tkl_gpio_write(DEMO_SPK_GPIO, (DEMO_SPK_GPIO_POLARITY == 0) ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW);
    if (rt != OPRT_OK) {
        PR_ERR("speaker amplifier enable failed: %d", rt);
    }
    /*
     * cfg.spk_volume alone leaves the DAC muted: tdd_audio.c, the shared driver
     * every other speaker example goes through, does not set that field at all
     * and applies the gain with tkl_ao_set_vol() once capture is running.
     * Follow the same order, or the downlink reaches tkl_ao_put_frame() intact
     * and plays back silent.
     */
    rt = tkl_ao_set_vol(cfg.card, TKL_AO_0, NULL, DEMO_SPK_VOLUME);
    if (rt != OPRT_OK) {
        PR_ERR("tkl_ao_set_vol failed: %d", rt);
    }
    /*
     * Swap the vendor canceller for the Speex one that tkl_vad_init installs:
     * it registers __tkl_aec_vad_process through tkl_ai_set_vad_aec_algorithm,
     * and aec_v3_algorithm calls that instead of aec_proc from then on.
     *
     * It also settles the uplink dropouts. The vendor's own VAD runs in the arm
     * this replaces, so aec_vad_proc no longer updates vad_state and the gate
     * that swallowed frames - vad_enable && vad_state != VAD_NONE - stops being
     * true. The RNN VAD in the new path only reads the output to raise a flag.
     */
    {
        TKL_VAD_CONFIG_T vad_cfg;

        memset(&vad_cfg, 0, sizeof(vad_cfg));
        vad_cfg.sample_rate = DEMO_MIC_SAMPLE_RATE;
        vad_cfg.channel_num = 1;
        vad_cfg.speech_min_ms = 200;
        vad_cfg.noise_min_ms = 1000;
        vad_cfg.frame_duration_ms = 10;
        vad_cfg.scale = 1.0f;

        rt = tkl_vad_init(&vad_cfg);
        if (rt != OPRT_OK) {
            PR_ERR("tkl_vad_init failed: %d, keeping the vendor canceller", rt);
        } else {
            (void)tkl_vad_start();
        }
    }

    s_mic_running = TRUE;
    PR_NOTICE("mic started: 16k/16bit/mono PCM -> G.711U 8k (Speex AEC + RNN VAD)");
    return OPRT_OK;
}

/**
 * @brief Stop mic capture
 * @return none
 */
static void __demo_mic_stop(void)
{
    if (!s_mic_running) {
        return;
    }
    s_mic_running = FALSE;
    /* Unhooks __tkl_aec_vad_process, so a session that fails to install it
     * again gets the vendor canceller rather than a dangling one. */
    (void)tkl_vad_stop();
    (void)tkl_vad_deinit();
    (void)tkl_ai_stop(DEMO_MIC_CARD, TKL_AI_0);
    (void)tkl_ai_uninit();
    PR_NOTICE("mic stopped");
}

/**
 * @brief Init the uplink ring and its mutex
 * @return OPRT_OK on success
 */
static OPERATE_RET __demo_audio_uplink_init(void)
{
    OPERATE_RET rt;

    if (s_audio_inited) {
        return OPRT_OK;
    }
    rt = tal_mutex_create_init(&s_audio_mutex);
    if (rt != OPRT_OK) {
        return rt;
    }
    s_audio_head = s_audio_tail = s_audio_count = 0;
    s_audio_inited = TRUE;
    return OPRT_OK;
}

/**
 * @brief Release the uplink ring
 * @return none
 */
static void __demo_audio_uplink_deinit(void)
{
    if (s_audio_mutex != NULL) {
        tal_mutex_lock(s_audio_mutex);
        s_audio_head = s_audio_tail = s_audio_count = 0;
        tal_mutex_unlock(s_audio_mutex);
        tal_mutex_release(s_audio_mutex);
        s_audio_mutex = NULL;
    }
    s_audio_inited = FALSE;
}

void demo_mic_uplink_pause(void)
{
    __demo_mic_stop();
}

int demo_on_get_audio_frame_callback(MEDIA_FRAME *media_frame)
{
    uint32_t i;
    uint64_t now_ms;

    if (media_frame == NULL || media_frame->data == NULL || !s_audio_inited) {
        return -1;
    }
    tal_mutex_lock(s_audio_mutex);
    if (s_audio_count < DEMO_AUDIO_FRAME_BYTES) {
        tal_mutex_unlock(s_audio_mutex);
        return -1;
    }
    for (i = 0; i < DEMO_AUDIO_FRAME_BYTES; i++) {
        ((uint8_t *)media_frame->data)[i] = s_audio_ring[s_audio_head];
        s_audio_head = (s_audio_head + 1U) % DEMO_AUDIO_RING_CAP;
        s_audio_count--;
    }
    tal_mutex_unlock(s_audio_mutex);

    media_frame->size = DEMO_AUDIO_FRAME_BYTES;
    media_frame->type = eAudioFrame;
    now_ms = tuya_p2p_misc_get_current_time_ms();
    media_frame->pts = now_ms;
    media_frame->timestamp = (uint32_t)now_ms;
    {
        static uint32_t s_pull_cnt = 0;
        if ((s_pull_cnt++ % 100) == 0) {
            PR_DEBUG("uplink p2p pull n=%u bytes=%u ring_left=%u", s_pull_cnt, (uint32_t)DEMO_AUDIO_FRAME_BYTES,
                     s_audio_count);
        }
    }
    return 0;
}

/*
 * SPEAKER_START/STOP only gate the downlink; they do not touch volume or the PA
 * pin. Gain and spk_gpio are configured once by tkl_ai_init and left alone --
 * calling tkl_ao_set_vol(0) here would mute the DAC on every intercom press.
 */
int demo_on_live_audio_start_callback(void)
{
    s_spk_active = TRUE;
    PR_NOTICE("LIVE audio(speaker) start: downlink intercom on");
    return 0;
}

int demo_on_live_audio_stop_callback(void)
{
    s_spk_active = FALSE;
    PR_NOTICE("LIVE audio(speaker) stop: downlink intercom off");
    return 0;
}

int demo_on_recv_audio_frame_callback(MEDIA_FRAME *media_frame)
{
    static int16_t         s_pcm8k[DEMO_DOWNLINK_G711_MAX];
    static int16_t         s_pcm16k[DEMO_DOWNLINK_PCM16K_MAX];
    static uint32_t        s_dl_cnt = 0;
    TKL_AUDIO_FRAME_INFO_T frame;
    uint32_t               i, n;
    size_t                 out_frames = 0;
    int                    ret;
    OPERATE_RET            ao_ret;

    if (media_frame == NULL || media_frame->data == NULL || media_frame->size == 0 || !s_spk_active) {
        return 0;
    }
    n = media_frame->size;
    if (n > DEMO_DOWNLINK_G711_MAX) {
        n = DEMO_DOWNLINK_G711_MAX;
    }
    /* Take ulaw2linear()'s result as the finished sample, the way the vendor's
     * own g711_decoder.c does; scaling it up here only clips. */
    for (i = 0; i < n; i++) {
        s_pcm8k[i] = (int16_t)ulaw2linear((int)((const uint8_t *)media_frame->data)[i]);
    }
    ret = resample_to_16k_fixed(s_pcm8k, (size_t)n, 8000, 1, s_pcm16k, &out_frames);
    if (ret != 0 || out_frames == 0) {
        PR_ERR("resample fail ret=%d in=%u out=%u", ret, n, (uint32_t)out_frames);
        return 0;
    }
    /*
     * Describe the frame the way tdd_audio.c does. One carrying only pbuf and
     * used_size leaves type/codectype/sample/datebits/channel at zero, which
     * the DAC accepts with OPRT_OK and then plays as silence.
     */
    memset(&frame, 0, sizeof(frame));
    frame.type = TKL_AUDIO_FRAME;
    frame.codectype = TKL_CODEC_AUDIO_PCM;
    frame.sample = DEMO_MIC_SAMPLE_RATE;
    frame.datebits = DEMO_MIC_DATABITS;
    frame.channel = DEMO_MIC_CHANNEL;
    frame.pbuf = (char *)s_pcm16k;
    frame.used_size = (uint32_t)(out_frames * 2);
    ao_ret = tkl_ao_put_frame(0, 0, NULL, &frame);
    s_dl_cnt++;
    if (ao_ret != OPRT_OK) {
        PR_ERR("downlink play failed n=%u ret=%d", s_dl_cnt, ao_ret);
    }
    return 0;
}

#else /* !DEMO_HAS_AUDIO */

void demo_mic_uplink_pause(void)
{
}

int demo_on_get_audio_frame_callback(MEDIA_FRAME *media_frame)
{
    (void)media_frame;
    return -1;
}

int demo_on_live_audio_start_callback(void)
{
    return 0;
}

int demo_on_live_audio_stop_callback(void)
{
    return 0;
}

int demo_on_recv_audio_frame_callback(MEDIA_FRAME *media_frame)
{
    (void)media_frame;
    return 0;
}

#endif /* DEMO_HAS_AUDIO */
