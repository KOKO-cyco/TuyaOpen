# RK3576 + ES8388 板载麦克风音频配置指南

## 问题背景

在立创 RK3576 开发板上运行 TuyaOpen AI Chat Bot 时，板载麦克风（ES8388 codec）默认配置下存在以下问题：

1. **板载 MIC 灵敏度低**：数据手册标称 -42dB，远场语音识别偏吃力
2. **ES8388 默认 PGA 增益不足**：仅 +9dB，不够激进
3. **双声道反相**：左右声道下混为 mono 时信号抵消
4. **ALSA 配置不当**：采样率/设备路由问题导致无声或音色异常

## 解决方案

### 1. 提高 ES8388 采集模拟增益

```bash
# 左声道 Capture Volume 拉满（+24dB）
amixer -c 0 cset numid=52 8

# 右声道 Capture Volume 拉满（+24dB）
amixer -c 0 cset numid=53 8
```

### 2. 修正左右反相问题

```bash
# ADC Data Select 设为 "Left Left"，避免反相抵消
amixer -c 0 cset numid=61 1
```

### 3. 停止 PipeWire（避免音频路由冲突）

```bash
systemctl --user stop pipewire pipewire-pulse wireplumber pipewire.socket pipewire-pulse.socket
```

如需开机自动禁用：

```bash
systemctl --user disable pipewire.socket pipewire-pulse.socket pipewire wireplumber
systemctl --user mask pipewire.socket pipewire-pulse.socket
```

### 4. 配置 ALSA 默认设备

编辑 `/etc/asound.conf`：

```bash
sudo tee /etc/asound.conf << 'EOF'
pcm.!default {
    type plug
    slave {
        pcm "hw:0,0"
        rate 48000
        channels 2
        format S16_LE
    }
}

ctl.!default {
    type hw
    card 0
}
EOF
```

关键点：
- ES8388 原生支持 48kHz，强制 slave 跑在 48kHz 可避免音色异常（尖锐/变调）
- `type plug` 自动处理应用程序 16kHz mono 与硬件 48kHz stereo 之间的转换
- 不使用 PipeWire，程序直接访问硬件设备

### 5. 验证配置

```bash
# 确认增益设置
amixer -c 0 cget numid=52  # 应为 values=8
amixer -c 0 cget numid=53  # 应为 values=8
amixer -c 0 cget numid=61  # 应为 values=1 (Left Left)

# 确认声卡信息
cat /proc/asound/cards
# 应显示: 0 [rockchipes8388 ]: rockchip-es8388

# 录音测试（拔掉耳机，对着板载麦克风说话，5秒自动停止）
arecord -D default -f S16_LE -r 16000 -c 1 -d 5 test.wav

# 播放测试（插上耳机）
aplay -D default test.wav
```

### 6. 运行 AI 程序

```bash
# 确保无耳机插入时录音（板载麦克风不受耳机影响，配置正确后可同时使用）
./your_chat_bot_x.x.x.elf
```

## 开机持久化

amixer 设置重启后会丢失，需写入启动脚本：

```bash
sudo tee /etc/profile.d/es8388_audio.sh << 'EOF'
#!/bin/bash
# ES8388 audio gain and phase configuration for AI voice
amixer -c 0 cset numid=52 8 > /dev/null 2>&1  # Left Capture Volume max
amixer -c 0 cset numid=53 8 > /dev/null 2>&1  # Right Capture Volume max
amixer -c 0 cset numid=61 1 > /dev/null 2>&1  # ADC Data Select: Left Left
EOF
sudo chmod +x /etc/profile.d/es8388_audio.sh
```

或使用 `alsactl store` 保存当前状态（需系统支持）：

```bash
sudo alsactl store 0
```

## 注意事项

- 板载 MIC 灵敏度有限（-42dB），适合近场（30cm 内）对话
- 如需远场语音识别，建议硬件上更换高灵敏度麦克风（-26dB 级别）或加前置放大模块
- 3.5mm 耳机插拔可能影响麦克风路由，配置完成后实测确认
- VAD 阈值已设为 `TKL_AUDIO_VAD_LOW`（-80dB），如环境噪音导致误触发可改回 `TKL_AUDIO_VAD_MID`（-60dB）

---

# SDL 显示与摄像头交叉编译说明（RK3576）

## SDL 显示

### 预编译库说明

`platform/LINUX/tuyaos_adapter/src/tkl_display/libs/RK3576/SDL2/libSDL2.a` 复用自 Raspberry Pi 构建，架构为 AArch64。通过符号表实际确认编译进去的后端为：

| 后端 | 状态 |
|------|------|
| X11 | ✓ 编译进去（可在桌面环境显示窗口） |
| KMS/DRM | ✓ 编译进去（可直驱屏幕，无需桌面） |
| OFFSCREEN | ✓ 编译进去 |
| DUMMY | ✓ 编译进去 |
| Wayland | ✗ 未编译 |

> 注：`SDL_config.h` 头文件中的 `#undef` 与实际库不符，以 `nm` 符号表为准。

SDL 会按优先级自动选择后端：有 `DISPLAY` 环境变量时走 X11，无桌面时走 KMS/DRM。

### 运行时环境变量

```bash
# 桌面环境（X11）下显示窗口（默认行为，一般无需手动指定）
export SDL_VIDEODRIVER=x11
./your_chat_bot_x.x.x.elf

# 无桌面，直接驱动 KMS/DRM 屏幕
export SDL_VIDEODRIVER=kmsdrm
./your_chat_bot_x.x.x.elf

# 完全离屏，不渲染到任何显示器
export SDL_VIDEODRIVER=offscreen
./your_chat_bot_x.x.x.elf
```

---

## 摄像头（V4L2）

### 设备节点说明

RK3576 的 ISP 处理管线会在 `/dev/videoX` 下生成多个节点，**视频数据输出节点**通常不是 `/dev/video0`。RK3576 配置中默认使用：

```
CONFIG_CAMERA_V4L2_DEVNODE="/dev/video73"
```

运行前确认实际节点：

```bash
# 列出所有视频节点及其功能
v4l2-ctl --list-devices

# 查看指定节点是否可采集（有 Video Capture 能力）
v4l2-ctl -d /dev/video73 --all | grep -i "capture\|format"

# 枚举支持的格式和分辨率
v4l2-ctl -d /dev/video73 --list-formats-ext
```

如果 `/dev/video73` 不存在或无采集能力，修改 `RK3576.config`：

```
CONFIG_CAMERA_V4L2_DEVNODE="/dev/videoN"   # 替换为实际节点
```

### 分辨率配置

`RK3576.config` 中摄像头输出分辨率：

```
CONFIG_COMP_AI_VIDEO_WIDTH=1280
CONFIG_COMP_AI_VIDEO_HEIGHT=720
```

根据实际摄像头能力调整，不匹配时 V4L2 会报 `EINVAL`。

### 交叉编译注意事项

V4L2 是内核接口，使用标准 Linux UAPI 头文件，**交叉编译无额外依赖**，`aarch64-none-linux-gnu` 工具链自带所需头文件。唯一需要确认的是目标板内核是否启用了对应驱动（`CONFIG_VIDEO_V4L2`）。
