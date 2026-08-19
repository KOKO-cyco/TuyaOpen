/**
 * @file tdd_camera_v4l2.c
 * @brief V4L2 camera driver implementation shared by all LINUX boards
 *
 * This file implements the V4L2 camera driver adapter on Linux, including:
 * - V4L2 device open/start/stop/close via TKL layer
 * - Frame capture thread (dequeue/queue)
 * - Frame buffer copy and posting to TDL camera manager
 * - Camera device registration for upper-layer discovery
 *
 * @note Lives in boards/LINUX/common/camera so every Linux board links the
 *       same implementation. Each board pulls it in from its CMakeLists when
 *       CONFIG_ENABLE_CAMERA_V4L2 is set, and passes its own device node to
 *       tdd_camera_v4l2_register().
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 *
 */
#include "tdd_camera_v4l2.h"

#include "tal_api.h"
#include "tuya_cloud_types.h"
#include "tuya_error_code.h"

#include "tdl_camera_driver.h"

#include "camera/tkl_camera_v4l2.h"
#include "jpeg_codec/tkl_jpeg_codec.h"

#if defined(ENABLE_TKL_VENC_MPP) && (ENABLE_TKL_VENC_MPP == 1)
#include "media/tkl_venc_mpp.h"
#endif

#define V4L2_DEVNODE_MAX_LEN 128

typedef struct {
    char devnode[V4L2_DEVNODE_MAX_LEN];

    TKL_CAMERA_V4L2_HANDLE_T tkl_hdl;

    THREAD_HANDLE thread;
    volatile bool running;

    uint16_t width;
    uint16_t height;
    uint16_t fps;

    /* Formats the node reported at register time (TKL_CAMERA_V4L2_PIXFMT_BIT
     * mask). UVC cameras give YUYV/MJPEG, SoC ISP pipelines give UYVY/NV12. */
    uint32_t pixfmt_mask;

    TKL_CAMERA_V4L2_PIXFMT_E pixfmt;

    bool need_raw;
    bool need_encoded;
    TUYA_FRAME_FMT_E encoded_post_fmt;

    bool jpeg_codec_inited;
    uint32_t frame_id;

    uint32_t dq_fail_run; /* consecutive empty dequeues, for the idle warning */

#if defined(ENABLE_TKL_VENC_MPP) && (ENABLE_TKL_VENC_MPP == 1)
    /* Capture nodes only produce raw YUV; H264 comes from the SoC encoder fed
     * with the NV12 the ISP already outputs, so nothing converts colour. */
    TKL_VENC_MPP_HANDLE_T venc;
    uint32_t venc_bitrate_kbps; /* what the encoder was asked for, for DBG stats */
    bool venc_fps_corrected;    /* rate control already moved to the measured fps */
    /* Rolling one-second window used for the encoder output stats. */
    uint64_t win_start_ms;
    uint32_t win_bytes, win_frames;
    uint32_t win_i_cnt, win_i_bytes, win_i_max;
#endif
} CAMERA_V4L2_DEV_T;

static void __camera_v4l2_capture_task(void *args)
{
    CAMERA_V4L2_DEV_T *dev = (CAMERA_V4L2_DEV_T *)args;
    if (!dev) {
        return;
    }

    while (dev->running) {
        uint8_t *v4l2_data = NULL;
        uint32_t v4l2_len = 0;
        uint32_t v4l2_index = 0;

        OPERATE_RET rt = tkl_camera_v4l2_dequeue(dev->tkl_hdl, &v4l2_data, &v4l2_len, &v4l2_index);
        if (rt != OPRT_OK) {
            /* A pipeline that accepts STREAMON and then delivers nothing looks
             * exactly like a healthy idle camera from here, so say so rather
             * than retry in silence. */
            if (++dev->dq_fail_run == 1 || dev->dq_fail_run % 500 == 0) {
                PR_WARN("camera delivered no frame for %u tries (dequeue %d); pipeline is armed but idle",
                        dev->dq_fail_run, rt);
            }
            tal_system_sleep(10);
            continue;
        }
        if (dev->dq_fail_run) {
            PR_INFO("camera recovered after %u empty tries", dev->dq_fail_run);
            dev->dq_fail_run = 0;
        }

#if defined(ENABLE_TKL_VENC_MPP) && (ENABLE_TKL_VENC_MPP == 1)
        // 0) Hardware H264: the captured NV12 goes straight into the encoder.
        if (dev->venc && dev->pixfmt == TKL_CAMERA_V4L2_PIXFMT_NV12) {
            uint8_t *es = NULL;
            uint32_t es_len = 0;
            BOOL_T is_key = FALSE;

            OPERATE_RET ert = tkl_venc_mpp_encode(dev->venc, v4l2_data, v4l2_len, &es, &es_len, &is_key);
            if (ert == OPRT_OK && es && es_len) {
                /*
                 * DBG: what the encoder really produces, once a second. The
                 * configured target is only a request - if the measured rate
                 * sits above it, or single I-frames take a large share of one
                 * second's budget, no amount of queue or congestion tuning
                 * downstream can carry the stream.
                 */
                {
                    /* Per-device, not static: a second stream must not inherit
                     * counters from the previous one. */
                    uint64_t now_ms = (uint64_t)tal_system_get_millisecond();
                    /* One second of the requested bitrate, in bytes. */
                    uint32_t budget = dev->venc_bitrate_kbps ? (dev->venc_bitrate_kbps * 1024u / 8u) : 1u;

                    dev->win_bytes += es_len;
                    dev->win_frames++;
                    if (is_key) {
                        dev->win_i_cnt++;
                        dev->win_i_bytes += es_len;
                        if (es_len > dev->win_i_max) {
                            dev->win_i_max = es_len;
                        }
                    }
                    if (dev->win_start_ms == 0) {
                        dev->win_start_ms = now_ms;
                    } else if (now_ms - dev->win_start_ms >= 1000) {
                        uint32_t ms = (uint32_t)(now_ms - dev->win_start_ms);
                        PR_DEBUG("DBG venc 1s: %u kbps (target %u), %u fps, avg %u B/frame; "
                                 "I:%u frames max %u B (%u%% of 1s budget)",
                                 (uint32_t)((uint64_t)dev->win_bytes * 8 * 1000 / ms / 1000),
                                 dev->venc_bitrate_kbps, dev->win_frames * 1000 / ms,
                                 dev->win_frames ? dev->win_bytes / dev->win_frames : 0, dev->win_i_cnt, dev->win_i_max,
                                 dev->win_i_max * 100 / budget);
                        /*
                         * Correct rate control to the rate actually coming out
                         * of the sensor. This node rejects both VIDIOC_S_PARM
                         * and VIDIOC_G_PARM, so measuring is the only way to
                         * learn it, and getting it wrong makes the encoder miss
                         * its bitrate target by requested/actual - which is the
                         * whole overshoot. Done once, off the second full
                         * window so the figure is settled.
                         */
                        if (!dev->venc_fps_corrected) {
                            uint32_t measured = dev->win_frames * 1000 / ms;

                            dev->venc_fps_corrected = true;
                            if (measured > 0 && measured != dev->fps) {
                                PR_WARN("camera measured at %ufps, not the configured %u; correcting rate control",
                                        measured, dev->fps);
                                if (tkl_venc_mpp_set_fps(dev->venc, measured) == OPRT_OK) {
                                    dev->fps = (uint16_t)measured;
                                }
                            }
                        }
                        dev->win_start_ms = now_ms;
                        dev->win_bytes = dev->win_frames = 0;
                        dev->win_i_cnt = dev->win_i_bytes = dev->win_i_max = 0;
                    }
                }
                TDD_CAMERA_FRAME_T *enc = tdl_camera_create_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, TUYA_FRAME_FMT_H264);
                if (enc) {
                    if (es_len <= enc->frame.data_len) {
                        memcpy(enc->frame.data, es, es_len);

                        enc->frame.id = (uint16_t)(dev->frame_id++);
                        enc->frame.is_i_frame = is_key ? 1 : 0;
                        enc->frame.is_complete = 1;
                        enc->frame.width = dev->width;
                        enc->frame.height = dev->height;
                        enc->frame.data_len = es_len;
                        enc->frame.total_frame_len = es_len;

                        if (tdl_camera_post_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, enc) != OPRT_OK) {
                            tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, enc);
                        }
                    } else {
                        static bool h264_warned = false;
                        if (!h264_warned) {
                            h264_warned = true;
                            PR_WARN("h264 frame too large: %u > %u, drop", es_len, enc->frame.data_len);
                        }
                        tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, enc);
                    }
                }
            } else if (ert != OPRT_OK && ert != OPRT_RESOURCE_NOT_READY) {
                static uint32_t enc_err_cnt = 0;
                if ((enc_err_cnt++ % 100) == 0) {
                    PR_ERR("mpp encode failed: %d (count %u)", ert, enc_err_cnt);
                }
            }
        }
#endif

        // 1) Post encoded JPEG (MJPEG) frame directly if requested.
        if (dev->need_encoded && dev->pixfmt == TKL_CAMERA_V4L2_PIXFMT_MJPEG) {
            TDD_CAMERA_FRAME_T *enc_frame = tdl_camera_create_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, dev->encoded_post_fmt);
            if (enc_frame) {
                if (v4l2_len <= enc_frame->frame.data_len) {
                    memcpy(enc_frame->frame.data, v4l2_data, v4l2_len);

                    enc_frame->frame.id = (uint16_t)(dev->frame_id++);
                    enc_frame->frame.is_i_frame = 1;
                    enc_frame->frame.is_complete = 1;
                    enc_frame->frame.width = dev->width;
                    enc_frame->frame.height = dev->height;
                    enc_frame->frame.data_len = v4l2_len;
                    enc_frame->frame.total_frame_len = v4l2_len;

                    rt = tdl_camera_post_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, enc_frame);
                    if (rt != OPRT_OK) {
                        tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, enc_frame);
                    }
                } else {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        PR_WARN("v4l2 jpeg frame too large: %u > %u, drop", v4l2_len, enc_frame->frame.data_len);
                    }
                    tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, enc_frame);
                }
            }
        }

        // 2) Post raw YUV422 captured natively (YUYV or UYVY, no conversion).
        if (dev->need_raw &&
            (dev->pixfmt == TKL_CAMERA_V4L2_PIXFMT_YUYV || dev->pixfmt == TKL_CAMERA_V4L2_PIXFMT_UYVY)) {
            TDD_CAMERA_FRAME_T *raw_frame = tdl_camera_create_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, TUYA_FRAME_FMT_YUV422);
            if (raw_frame) {
                if (v4l2_len <= raw_frame->frame.data_len) {
                    memcpy(raw_frame->frame.data, v4l2_data, v4l2_len);

                    raw_frame->frame.id = (uint16_t)(dev->frame_id++);
                    raw_frame->frame.is_i_frame = 1;
                    raw_frame->frame.is_complete = 1;
                    raw_frame->frame.width = dev->width;
                    raw_frame->frame.height = dev->height;
                    raw_frame->frame.data_len = v4l2_len;
                    raw_frame->frame.total_frame_len = v4l2_len;

                    rt = tdl_camera_post_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, raw_frame);
                    if (rt != OPRT_OK) {
                        tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, raw_frame);
                    }
                } else {
                    tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, raw_frame);
                }
            }
        }

        // 3) Decode MJPEG to YUV422(UYVY) and post raw frame if requested.
        if (dev->need_raw && dev->pixfmt == TKL_CAMERA_V4L2_PIXFMT_MJPEG) {
            TDD_CAMERA_FRAME_T *raw_frame = tdl_camera_create_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, TUYA_FRAME_FMT_YUV422);
            if (raw_frame) {
                TKL_JPEG_CODEC_INFO_T info;
                memset(&info, 0, sizeof(info));
                if (tkl_jpeg_codec_img_info_get(v4l2_data, v4l2_len, &info) == OPRT_OK) {
                    const uint32_t out_len = (uint32_t)info.out_width * (uint32_t)info.out_height * 2;
                    if (out_len <= raw_frame->frame.data_len) {
                        info.in_size = v4l2_len;
                        if (tkl_jpeg_codec_convert(v4l2_data, raw_frame->frame.data, &info, JPEG_DEC_OUT_YUV422) == OPRT_OK) {
                            raw_frame->frame.id = (uint16_t)(dev->frame_id++);
                            raw_frame->frame.is_i_frame = 1;
                            raw_frame->frame.is_complete = 1;
                            raw_frame->frame.width = info.out_width;
                            raw_frame->frame.height = info.out_height;
                            raw_frame->frame.data_len = out_len;
                            raw_frame->frame.total_frame_len = out_len;

                            rt = tdl_camera_post_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, raw_frame);
                            if (rt != OPRT_OK) {
                                tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, raw_frame);
                            }
                        } else {
                            tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, raw_frame);
                        }
                    } else {
                        tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, raw_frame);
                    }
                } else {
                    tdl_camera_release_tdd_frame((TDD_CAMERA_DEV_HANDLE_T)dev, raw_frame);
                }
            }
        }

        (void)tkl_camera_v4l2_queue(dev->tkl_hdl, v4l2_index);
    }
}

static OPERATE_RET __tdd_camera_v4l2_open(TDD_CAMERA_DEV_HANDLE_T device, TDD_CAMERA_OPEN_CFG_T *cfg)
{
    CAMERA_V4L2_DEV_T *dev = (CAMERA_V4L2_DEV_T *)device;
    if (dev == NULL || cfg == NULL) {
        return OPRT_INVALID_PARM;
    }

    if (dev->running) {
        return OPRT_OK;
    }

    bool need_raw = (cfg->out_fmt & TDL_IMG_FMT_RAW_MASK) ? true : false;
    bool need_encoded = (cfg->out_fmt & TDL_IMG_FMT_ENCODED_MASK) ? true : false;
    bool need_h264 = (cfg->out_fmt == TDL_CAMERA_FMT_H264 || cfg->out_fmt == TDL_CAMERA_FMT_H264_YUV422_BOTH);

#if !defined(ENABLE_TKL_VENC_MPP) || (ENABLE_TKL_VENC_MPP != 1)
    if (need_h264) {
        PR_ERR("H264 output needs a hardware encoder, none built for this chip");
        return OPRT_NOT_SUPPORTED;
    }
#endif

    dev->need_raw = need_raw;
    dev->need_encoded = need_encoded;
    dev->encoded_post_fmt = TUYA_FRAME_FMT_JPEG;
    dev->jpeg_codec_inited = false;

    dev->width = cfg->width;
    dev->height = cfg->height;
    dev->fps = cfg->fps;

    TKL_CAMERA_V4L2_CFG_T v4l2_cfg = {0};
    v4l2_cfg.devnode = dev->devnode;
    v4l2_cfg.width = cfg->width;
    v4l2_cfg.height = cfg->height;
    v4l2_cfg.fps = cfg->fps;
    v4l2_cfg.buffer_count = 4;

    OPERATE_RET rt = OPRT_COM_ERROR;

    /* Try native YUV422 first (no conversion at all), then MJPEG plus a
     * software decode. Which native order exists depends on the hardware:
     * UVC webcams give YUYV, Rockchip rkisp/rkvpss give UYVY and never YUYV. */
    static const TKL_CAMERA_V4L2_PIXFMT_E raw_pref[] = {
        TKL_CAMERA_V4L2_PIXFMT_YUYV,
        TKL_CAMERA_V4L2_PIXFMT_UYVY,
    };

    if (need_h264) {
        /* Feed the encoder the format it wants natively; the ISP emits NV12, so
         * capture and encode agree and no conversion sits between them. */
        v4l2_cfg.pixfmt = TKL_CAMERA_V4L2_PIXFMT_NV12;
        rt = tkl_camera_v4l2_open(&dev->tkl_hdl, &v4l2_cfg);
        if (rt != OPRT_OK) {
            PR_ERR("tkl_camera_v4l2_open(NV12) failed: %d, cannot feed H264 encoder", rt);
            return rt;
        }
    } else if (need_raw && !need_encoded) {
        for (size_t i = 0; i < CNTSOF(raw_pref); i++) {
            if (dev->pixfmt_mask && !(dev->pixfmt_mask & TKL_CAMERA_V4L2_PIXFMT_BIT(raw_pref[i]))) {
                continue; /* probe says the node does not have it */
            }
            v4l2_cfg.pixfmt = raw_pref[i];
            rt = tkl_camera_v4l2_open(&dev->tkl_hdl, &v4l2_cfg);
            if (rt == OPRT_OK) {
                break;
            }
        }

        if (rt != OPRT_OK) {
            PR_WARN("no native YUV422 format usable, fallback to MJPEG+decode");
            v4l2_cfg.pixfmt = TKL_CAMERA_V4L2_PIXFMT_MJPEG;
            rt = tkl_camera_v4l2_open(&dev->tkl_hdl, &v4l2_cfg);
            if (rt == OPRT_OK) {
                if (tkl_jpeg_codec_init() == OPRT_OK) {
                    dev->jpeg_codec_inited = true;
                } else {
                    PR_WARN("tkl_jpeg_codec_init failed, raw decode may fail");
                }
            }
        }
    } else {
        // For JPEG/BOTH mode, capture MJPEG and (optionally) decode to YUV422 in software.
        v4l2_cfg.pixfmt = TKL_CAMERA_V4L2_PIXFMT_MJPEG;
        rt = tkl_camera_v4l2_open(&dev->tkl_hdl, &v4l2_cfg);
        if (rt == OPRT_OK && need_raw) {
            if (tkl_jpeg_codec_init() == OPRT_OK) {
                dev->jpeg_codec_inited = true;
            } else {
                PR_WARN("tkl_jpeg_codec_init failed, raw decode may fail");
            }
        }
    }

    dev->pixfmt = v4l2_cfg.pixfmt;

#if defined(ENABLE_TKL_VENC_MPP) && (ENABLE_TKL_VENC_MPP == 1)
    if (rt == OPRT_OK && need_h264) {
        TKL_VENC_MPP_CFG_T venc_cfg = {0};
        uint32_t real_fps = cfg->fps;

        /*
         * Rate control divides the bitrate by the frame rate to get a
         * per-frame bit budget, so it needs the rate the sensor actually
         * delivers. This ISP pipeline rejects VIDIOC_S_PARM outright and runs
         * at a fixed rate; feeding the encoder the requested rate instead
         * overshoots the target bitrate by exactly requested/actual.
         */
        if (tkl_camera_v4l2_get_fps(dev->tkl_hdl, &real_fps) != OPRT_OK || real_fps == 0) {
            real_fps = cfg->fps;
        }
        if (real_fps != cfg->fps) {
            PR_WARN("camera runs at %ufps not the requested %ufps; rate control follows the hardware", real_fps,
                    cfg->fps);
        }
        dev->fps = (uint16_t)real_fps;

        venc_cfg.width = cfg->width;
        venc_cfg.height = cfg->height;
        venc_cfg.fps = real_fps;
        /* Bitrate comes from the caller because it has to fit that caller's
         * transport budget; MPP's resolution-derived default is several times
         * higher than a P2P live stream can carry. 0 falls back to it. */
        venc_cfg.bitrate_kbps = cfg->bitrate_kbps;
        /* GOP is expressed in frames, so scale it with the real rate to keep
         * the interval between I-frames the caller asked for in seconds. */
        venc_cfg.gop = (cfg->gop && cfg->fps) ? (cfg->gop * real_fps / cfg->fps) : cfg->gop;
        dev->venc_bitrate_kbps = cfg->bitrate_kbps;
        dev->venc_fps_corrected = false; /* re-measure for every new encoder */
        dev->win_start_ms = 0;
        dev->win_bytes = dev->win_frames = 0;
        dev->win_i_cnt = dev->win_i_bytes = dev->win_i_max = 0;
        OPERATE_RET vrt = tkl_venc_mpp_open(&dev->venc, &venc_cfg);
        if (vrt != OPRT_OK) {
            PR_ERR("tkl_venc_mpp_open failed: %d", vrt);
            (void)tkl_camera_v4l2_close(dev->tkl_hdl);
            dev->tkl_hdl = NULL;
            return vrt;
        }
        PR_INFO("MPP H264 encoder ready: %ux%u @%ufps gop=%u", cfg->width, cfg->height, real_fps, venc_cfg.gop);
    }
#endif

    if (rt != OPRT_OK) {
        PR_ERR("tkl_camera_v4l2_open failed: %d", rt);
        return rt;
    }

    rt = tkl_camera_v4l2_start(dev->tkl_hdl);
    if (rt != OPRT_OK) {
        PR_ERR("tkl_camera_v4l2_start failed: %d", rt);
        (void)tkl_camera_v4l2_close(dev->tkl_hdl);
        dev->tkl_hdl = NULL;
        if (dev->jpeg_codec_inited) {
            (void)tkl_jpeg_codec_deinit();
            dev->jpeg_codec_inited = false;
        }
        return rt;
    }

    dev->running = true;
    THREAD_CFG_T thread_cfg = {8192, THREAD_PRIO_1, "v4l2_cam"};
    rt = tal_thread_create_and_start(&dev->thread, NULL, NULL, __camera_v4l2_capture_task, dev, &thread_cfg);
    if (rt != OPRT_OK) {
        dev->running = false;
        (void)tkl_camera_v4l2_stop(dev->tkl_hdl);
        (void)tkl_camera_v4l2_close(dev->tkl_hdl);
        dev->tkl_hdl = NULL;
        if (dev->jpeg_codec_inited) {
            (void)tkl_jpeg_codec_deinit();
            dev->jpeg_codec_inited = false;
        }
        return rt;
    }

    return OPRT_OK;
}

/**
 * @brief Produce a key frame on the next encode
 *
 * Only the hardware encoder path can honour this; a node that already hands us
 * encoded frames decides its own key frame placement.
 */
static OPERATE_RET __tdd_camera_v4l2_request_i_frame(TDD_CAMERA_DEV_HANDLE_T device)
{
    CAMERA_V4L2_DEV_T *dev = (CAMERA_V4L2_DEV_T *)device;

    if (dev == NULL) {
        return OPRT_INVALID_PARM;
    }
#if defined(ENABLE_TKL_VENC_MPP) && (ENABLE_TKL_VENC_MPP == 1)
    if (dev->venc) {
        return tkl_venc_mpp_request_idr(dev->venc);
    }
#endif
    return OPRT_NOT_SUPPORTED;
}

/**
 * @brief Move the encoder's target bitrate
 *
 * Keeps venc_bitrate_kbps in step so the per-second statistics stay measured
 * against what the encoder is actually being asked for.
 */
static OPERATE_RET __tdd_camera_v4l2_set_bitrate(TDD_CAMERA_DEV_HANDLE_T device, uint32_t kbps)
{
    CAMERA_V4L2_DEV_T *dev = (CAMERA_V4L2_DEV_T *)device;

    if (dev == NULL || kbps == 0) {
        return OPRT_INVALID_PARM;
    }
#if defined(ENABLE_TKL_VENC_MPP) && (ENABLE_TKL_VENC_MPP == 1)
    if (dev->venc) {
        OPERATE_RET rt = tkl_venc_mpp_set_bitrate(dev->venc, kbps);

        if (rt == OPRT_OK) {
            dev->venc_bitrate_kbps = kbps;
        }
        return rt;
    }
#endif
    (void)kbps;
    return OPRT_NOT_SUPPORTED;
}

static OPERATE_RET __tdd_camera_v4l2_close(TDD_CAMERA_DEV_HANDLE_T device)
{
    CAMERA_V4L2_DEV_T *dev = (CAMERA_V4L2_DEV_T *)device;
    if (dev == NULL) {
        return OPRT_INVALID_PARM;
    }

    if (!dev->running) {
        return OPRT_OK;
    }

    dev->running = false;

    if (dev->thread) {
        tal_thread_delete(dev->thread);
        dev->thread = NULL;
    }

    if (dev->tkl_hdl) {
        (void)tkl_camera_v4l2_stop(dev->tkl_hdl);
        (void)tkl_camera_v4l2_close(dev->tkl_hdl);
        dev->tkl_hdl = NULL;
    }

#if defined(ENABLE_TKL_VENC_MPP) && (ENABLE_TKL_VENC_MPP == 1)
    if (dev->venc) {
        (void)tkl_venc_mpp_close(dev->venc);
        dev->venc = NULL;
    }
#endif

    if (dev->jpeg_codec_inited) {
        (void)tkl_jpeg_codec_deinit();
        dev->jpeg_codec_inited = false;
    }

    return OPRT_OK;
}

OPERATE_RET tdd_camera_v4l2_register(const char *name, const char *devnode)
{
    if (name == NULL || devnode == NULL) {
        return OPRT_INVALID_PARM;
    }

    CAMERA_V4L2_DEV_T *dev = (CAMERA_V4L2_DEV_T *)tal_malloc(sizeof(CAMERA_V4L2_DEV_T));
    if (dev == NULL) {
        return OPRT_MALLOC_FAILED;
    }
    memset(dev, 0, sizeof(*dev));

    strncpy(dev->devnode, devnode, sizeof(dev->devnode) - 1);

    /* Ask the node what it has before advertising anything: a UVC webcam and a
     * Rockchip ISP pipeline expose disjoint format sets, and the YUV422 byte
     * order has to be declared here, long before open() negotiates a format. */
    if (tkl_camera_v4l2_probe(devnode, &dev->pixfmt_mask) != OPRT_OK) {
        PR_WARN("v4l2 probe failed on %s, falling back to open-time negotiation", devnode);
        dev->pixfmt_mask = 0;
    }

    TDD_CAMERA_DEV_INFO_T dev_info = {0};
    dev_info.type = TDL_CAMERA_DVP; /* Keep existing type for compatibility with apps expecting a camera device */
    dev_info.max_fps = 60;
    dev_info.max_width = 1920;
    dev_info.max_height = 1080;

    /* V4L2 capture gives raw YUV422 and/or MJPEG. H264 needs a separate encoder
     * block (Rockchip VEPU via MPP) and is not offered by any capture node. */
    dev_info.supported_fmts = 0;
    if (dev->pixfmt_mask == 0) {
        /* Probe unavailable: keep the previous optimistic advertisement. */
        dev_info.supported_fmts = TDL_CAMERA_FMT_YUV422 | TDL_CAMERA_FMT_JPEG;
        dev_info.yuv_order = TUYA_YUV422_UYVY;
    } else {
        if (dev->pixfmt_mask &
            (TKL_CAMERA_V4L2_PIXFMT_BIT(TKL_CAMERA_V4L2_PIXFMT_YUYV) |
             TKL_CAMERA_V4L2_PIXFMT_BIT(TKL_CAMERA_V4L2_PIXFMT_UYVY) |
             TKL_CAMERA_V4L2_PIXFMT_BIT(TKL_CAMERA_V4L2_PIXFMT_MJPEG))) {
            dev_info.supported_fmts |= TDL_CAMERA_FMT_YUV422; /* MJPEG decodes to YUV422 */
        }
        if (dev->pixfmt_mask & TKL_CAMERA_V4L2_PIXFMT_BIT(TKL_CAMERA_V4L2_PIXFMT_MJPEG)) {
            dev_info.supported_fmts |= TDL_CAMERA_FMT_JPEG;
        }
#if defined(ENABLE_TKL_VENC_MPP) && (ENABLE_TKL_VENC_MPP == 1)
        /* H264 needs both a hardware encoder and a node that can hand it NV12. */
        if (dev->pixfmt_mask & TKL_CAMERA_V4L2_PIXFMT_BIT(TKL_CAMERA_V4L2_PIXFMT_NV12)) {
            dev_info.supported_fmts |= TDL_CAMERA_FMT_H264;
        }
#endif
        /* Native YUYV wins only when the node has no UYVY, matching the order
         * __tdd_camera_v4l2_open() tries. MJPEG decode always yields UYVY. */
        dev_info.yuv_order = (!(dev->pixfmt_mask & TKL_CAMERA_V4L2_PIXFMT_BIT(TKL_CAMERA_V4L2_PIXFMT_UYVY)) &&
                              (dev->pixfmt_mask & TKL_CAMERA_V4L2_PIXFMT_BIT(TKL_CAMERA_V4L2_PIXFMT_YUYV)))
                                 ? TUYA_YUV422_YUYV
                                 : TUYA_YUV422_UYVY;
    }

    TDD_CAMERA_INTFS_T intfs = {
        .open = __tdd_camera_v4l2_open,
        .close = __tdd_camera_v4l2_close,
        .request_i_frame = __tdd_camera_v4l2_request_i_frame,
        .set_bitrate = __tdd_camera_v4l2_set_bitrate,
    };

    OPERATE_RET rt = tdl_camera_device_register((char *)name, (TDD_CAMERA_DEV_HANDLE_T)dev, &intfs, &dev_info);
    if (rt != OPRT_OK) {
        tal_free(dev);
        return rt;
    }

    PR_INFO("registered V4L2 camera: name=%s devnode=%s", name, devnode);
    return OPRT_OK;
}
