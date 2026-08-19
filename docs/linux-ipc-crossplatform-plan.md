# camera_demo 接入东山派 A1 / 泰山派 3 + P2P、switch_demo 跨平台 — 实施方案

> 目标：camera_demo 在 DshanPi A1、TaishanPi 3 上跑真实摄像头采集 → H264 → P2P 直播/本地回放；
> P2P 组件与 switch_demo 完成跨平台。
> 编码路线已定：**V4L2 M2M 硬编**（不引入厂商闭源 SDK）。

---

## 0. 结论先行

现有代码离目标比预期近，原因是 **T5AI 的直播链路本身已经是平台无关的**：

```c
// apps/tuya_cloud/camera_demo/src/tuya_ipc_demo.c
__demo_open_camera()        tdl_camera_find_dev(CAMERA_NAME)
                            cfg.out_fmt = TDL_CAMERA_FMT_H264
                            cfg.get_encoded_frame_cb = __demo_encoded_frame_cb
__demo_encoded_frame_cb()   tuya_ipc_ring_buffer_append_data_with_timestamp(...)
demo_on_get_video_frame_cb  tuya_ipc_ring_buffer_get_frame(s_ring_r, ...)
```

只依赖 TDL camera + ring buffer，两者 Linux 上都有。**所以不要在 Linux 上另写第三条媒体链路**，
真正缺的是两块：

1. **Linux 侧没有 H264 编码能力** — V4L2 TDD 显式拒绝：
   `boards/LINUX/TaishanPi_3/tdd_camera_v4l2.c:175` → `PR_ERR("V4L2 camera H264 output not supported")`
2. **camera_demo 把"媒体来源"和"操作系统"绑死了** — `#if OPERATING_SYSTEM == SYSTEM_LINUX` 直接决定
   走文件还是走摄像头，导致 Linux 板子即使有摄像头也只能读 `demo_video.264`。

---

## 1. 现状盘点（含证据）

### 板级

| 板子 | board_com_api | 按键 | 摄像头 | 显示 | 备注 |
|---|---|---|---|---|---|
| `boards/LINUX/Ubuntu` | — | — | — | — | 只有 Kconfig |
| `boards/LINUX/Raspberry_Pi` | ✅ 326 行 | ✅ | ✅ V4L2 | ✅ SDL | 最完整 |
| `boards/LINUX/TaishanPi_3` | ✅ 406 行 | ✅ | ✅ V4L2 | ✅ SDL | 驱动齐但 camera_demo 无对应 config |
| `boards/LINUX/DshanPi_A1` | ✅ 215 行 | ✅ | ❌ | ❌ | **缺摄像头** |

### 已确认的缺陷

- `boards/LINUX/Raspberry_Pi/tdd_camera_v4l2.c` 与 `boards/LINUX/TaishanPi_3/tdd_camera_v4l2.c`
  **逐字节相同**（各 334 行），纯复制粘贴。
- `boards/LINUX/DshanPi_A1/CMakeLists.txt:10` → `set(MODULE_NAME "Raspberry_Pi")`，
  编 A1 会产出名为 Raspberry_Pi 的库。正确写法见 `boards/ESP32/DNESP32S3/CMakeLists.txt:10`
  的 `get_filename_component(MODULE_NAME ${MODULE_PATH} NAME)`。三块 Linux 板都是硬编码 MODULE_NAME。
- `boards/LINUX/Raspberry_Pi/CMakeLists.txt` 与 `DshanPi_A1/CMakeLists.txt` 的注释同样残留
  "Raspberry Pi board configuration"。

### TKL 层能力

`platform/LINUX/tuyaos_adapter/include/camera/tkl_camera_v4l2.h` 只提供采集：

```c
TKL_CAMERA_V4L2_PIXFMT_YUYV / TKL_CAMERA_V4L2_PIXFMT_MJPEG   // 仅两种
tkl_camera_v4l2_open/start/dequeue/queue/stop/close
```

**没有任何编码器接口。**

### camera_demo 现有三条媒体路径

`apps/tuya_cloud/camera_demo/src/tuya_ipc_demo.c`：

| 行号 | 条件 | 媒体来源 |
|---|---|---|
| 16–256 | `#if OPERATING_SYSTEM == SYSTEM_LINUX` | 磁盘 `demo_video.264` |
| 259–476 | `#else` + `CAMERA_DEMO_P2P_FILE_H264` | 固件内置 `.264` blob |
| 477–1503 | `#else` | GC2145 DVP 实时 H264 |

第三条里混入了 T5 专属调用：`tkl_ai_*`(音频输入,6+ 处)、`tkl_ao_*`(音频输出)、`tkl_dvp_*`、
`tkl_gpio_write`、`tkl_fs_mount`。这些是**下沉到板级**的候选，不是 OS 差异。

### switch_demo

`apps/tuya_cloud/switch_demo/src/tuya_main.c:315` 已经有 Linux 版 `main()`，
缺的只是板级 `.config`（当前 `app_default.config` 仅 `CONFIG_BOARD_CHOICE_T5AI=y`）。

---

## 2. 目标架构

```
camera_demo (媒体来源无关)
        │  tdl_camera_dev_open(out_fmt = TDL_CAMERA_FMT_H264)
        │  get_encoded_frame_cb ──> ring buffer ──> P2P / local_store
        ▼
TDL  src/peripherals/camera/tdl_camera/          ← 契约不变
        ▼
TDD  boards/LINUX/common/camera/tdd_camera_v4l2.c   ← 一份实现，三块板共用
        │   采集(YUYV/MJPEG) ──[必要时转 NV12]──> 编码 ──> 编码帧回调
        ▼
TKL  platform/LINUX/.../tkl_camera_v4l2.c   (采集，已有)
     platform/LINUX/.../tkl_venc_v4l2m2m.c  (编码，新增)
```

**关键决策：编码器塞在 TDD 层内部，不改 TDL 契约。**
理由：`TDL_CAMERA_FMT_H264` 枚举本来就存在（`tdl_camera_manage.h:40`），T5AI 上 GC2145 也是
"TDD 内部出 H264"。这样 camera_demo 一行不用改就能在两个平台拿到 H264。

---

## 3. 分阶段实施

### Phase 0 — 地基清理（低风险，可独立提 PR）

- 新建 `boards/LINUX/common/camera/tdd_camera_v4l2.{c,h}`，删除 Raspberry_Pi / TaishanPi_3 下的两份副本。
- 三块板 CMakeLists 改用 `get_filename_component(MODULE_NAME ...)`，修掉 A1 的 `"Raspberry_Pi"`。
- 各板 CMakeLists 把 `${MODULE_PATH}/../common/camera` 加进 `LIB_SRCS` 与 include。
  （顶层 `CMakeLists.txt:128` 只 `add_subdirectory(boards/<平台>/<板>)`，common 需各板显式引入。）
- 给 DshanPi_A1 补 camera：`Kconfig` 加 `ENABLE_CAMERA_V4L2`/`CAMERA_NAME`/`CAMERA_V4L2_DEVNODE`，
  `board_com_api.c` 加 `__board_register_camera()`（照抄 TaishanPi_3:266–290 的形状）。
- **验收**：四块 Linux 板各自 `tos.py config choice` + `build` 通过。

### Phase 1 — TKL：V4L2 M2M 编码器（核心新增）

新增：
- `platform/LINUX/tuyaos_adapter/include/camera/tkl_venc_v4l2m2m.h`
- `platform/LINUX/tuyaos_adapter/src/tkl_camera/tkl_venc_v4l2m2m.c`

接口对齐现有 `tkl_camera_v4l2` 风格：
```c
tkl_venc_v4l2m2m_open(&hdl, &cfg)      // cfg: w/h/fps/bitrate/gop/in_pixfmt
tkl_venc_v4l2m2m_start/stop/close
tkl_venc_v4l2m2m_queue_input(hdl, yuv, len)
tkl_venc_v4l2m2m_dequeue_output(hdl, &data, &len, &is_keyframe, &index)
tkl_venc_v4l2m2m_release_output(hdl, index)
```

实现要点：
- `VIDIOC_QUERYCAP` 认 `V4L2_CAP_VIDEO_M2M` / `V4L2_CAP_VIDEO_M2M_MPLANE`
- OUTPUT 队列设输入 YUV（NV12 优先），CAPTURE 队列设 `V4L2_PIX_FMT_H264`
- `VIDIOC_ENUM_FMT` 探测输入格式，优先选**零转换**的那个（省 CPU）
- 码率/GOP 走 controls：`V4L2_CID_MPEG_VIDEO_BITRATE`、`_GOP_SIZE`、`_H264_PROFILE`、
  `_FORCE_KEY_FRAME`（P2P 首帧和 App 请求 I 帧时用）
- 关键帧标记读 `V4L2_BUF_FLAG_KEYFRAME`，直接喂给 `TDL_CAMERA_FRAME_T.is_i_frame`

### Phase 2 — TDD 打通 H264

改 `boards/LINUX/common/camera/tdd_camera_v4l2.c`：
- 删掉 175 行那个 `H264 output not supported` 的拒绝分支
- open 时若请求 H264：探测 M2M 编码器 → 建管线 → 采集帧转 NV12（能零转换就零转换）→ 编码 →
  `get_encoded_frame_cb` 上抛
- `tdl_camera_dev_get_info()` 的 `supported_fmts` **仅在探测到编码器时**才置上 `TDL_CAMERA_FMT_H264`，
  探测不到就保持现状（优雅降级到 MJPEG，不要硬失败）

### Phase 3 — camera_demo 解耦 OS 与媒体来源（风险最高）

把 `#if OPERATING_SYSTEM == SYSTEM_LINUX` 的轴换成**媒体来源**：

- `MEDIA_SRC_CAMERA` — 走 TDL camera，任何注册了摄像头的板子（T5AI/A1/泰山派3 共用同一段代码）
- `MEDIA_SRC_FILE` — 磁盘 `.264` 或固件内置 blob（Ubuntu 无摄像头时的回退）

建议拆成两步降低风险：
- **3a**：只合并 video 路径，audio 保持现有 `#if` 不动 → 可独立验证
- **3b**：`tkl_ai_*`/`tkl_ao_*` 收敛到 TDL audio（Linux 侧走已有的 `tdd_audio_alsa_register`），
  `tkl_dvp_*`/`tkl_gpio_write` 下沉到 T5 板级

### Phase 4 — 板级 config

新增 `apps/tuya_cloud/camera_demo/config/DshanPi_A1.config`、`TaishanPi_3.config`，
以 `Ubuntu.config` 为模板，差异项：`CONFIG_BOARD_CHOICE_*`、`ENABLE_CAMERA_V4L2=y`、
`CAMERA_V4L2_DEVNODE`、关掉 `CAMERA_DEMO_P2P_FILE_H264`。

### Phase 5 — switch_demo 跨平台

Linux `main()` 已有。新增 `apps/tuya_cloud/switch_demo/config/Linux.config`
（`CONFIG_BOARD_CHOICE_LINUX` + 板选择 + `ENABLE_WIRED`），裁掉 T5 专属依赖。
顺手把 `app_default.config` 漏写的板型选择补上（当前只有 `CONFIG_BOARD_CHOICE_T5AI=y`，
落到 Kconfig 第一项 SPARKLEIOT_T5AI_DEV，而它却写了 TUYA_T5AI_BOARD 专属的 `EX_MODULE_NONE`）。

### Phase 6 — P2P Linux 实测

`Ubuntu.config` 已开 `CONFIG_ENABLE_TUYA_P2P=y` 但未在真机验证。
复用本仓已验证的 LAN 测试装置（见记忆 `t5ai-lan-hardware-test-rig`）做端到端验证。

---

## 4. 未确认盲点 — 需要上板确认

以下**无法从代码判断**，必须在两块板上实测，否则 Phase 1 的设计可能落空：

1. 摄像头是 **CSI 还是 USB UVC**？现有 TDD 头文件注释写的是 "V4L2 (UVC) camera"，
   若是 CSI 则节点、格式、是否需要 media-ctl 配置 pipeline 都不同。
2. **M2M 编码器节点是否存在**、叫什么名字。
3. 编码器**支持的输入像素格式**（决定要不要做色彩转换，以及转换的 CPU 开销）。
4. 摄像头本身支持的分辨率/帧率/格式。

请在两块板上各跑：

```sh
v4l2-ctl --list-devices
for d in /dev/video*; do echo "== $d"; v4l2-ctl -d $d --info --list-formats-ext; done
```

---

## 5. 建议的合入顺序

Phase 0 → 1 → 2 可各自独立成 PR（互不阻塞、都能单独验证）；
Phase 3 体量最大且触碰 1500 行的 `tuya_ipc_demo.c`，建议 3a/3b 分开；
Phase 5 与前面完全解耦，随时可插队做（最快见效）。
