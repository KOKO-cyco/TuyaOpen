# RK3576 + ES8388 Onboard Microphone Audio Setup Guide

## Background

When running TuyaOpen AI Chat Bot on the LiChuang RK3576 development board, the onboard microphone (ES8388 codec) exhibits the following issues under default configuration:

1. **Low onboard MIC sensitivity**: Datasheet specifies −42 dB, which makes far-field speech recognition difficult
2. **Insufficient ES8388 default PGA gain**: Only +9 dB, not aggressive enough
3. **Stereo phase inversion**: Left and right channels cancel each other out when downmixed to mono
4. **Incorrect ALSA configuration**: Wrong sample rate or device routing causes silence or distorted audio

## Solution

### 1. Increase ES8388 Analog Capture Gain

```bash
# Set left channel Capture Volume to maximum (+24 dB)
amixer -c 0 cset numid=52 8

# Set right channel Capture Volume to maximum (+24 dB)
amixer -c 0 cset numid=53 8
```

### 2. Fix Left/Right Phase Inversion

```bash
# Set ADC Data Select to "Left Left" to avoid phase cancellation
amixer -c 0 cset numid=61 1
```

### 3. Stop PipeWire (avoid audio routing conflicts)

```bash
systemctl --user stop pipewire pipewire-pulse wireplumber pipewire.socket pipewire-pulse.socket
```

To disable on boot permanently:

```bash
systemctl --user disable pipewire.socket pipewire-pulse.socket pipewire wireplumber
systemctl --user mask pipewire.socket pipewire-pulse.socket
```

### 4. Configure ALSA Default Device

Edit `/etc/asound.conf`:

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

Key points:
- ES8388 natively supports 48 kHz; forcing the slave to 48 kHz prevents audio distortion (high-pitched or pitch-shifted output)
- `type plug` automatically handles sample-rate/channel conversion between the application (16 kHz mono) and hardware (48 kHz stereo)
- PipeWire is bypassed; the program accesses the hardware device directly

### 5. Verify Configuration

```bash
# Confirm gain settings
amixer -c 0 cget numid=52  # should show values=8
amixer -c 0 cget numid=53  # should show values=8
amixer -c 0 cget numid=61  # should show values=1 (Left Left)

# Confirm sound card info
cat /proc/asound/cards
# Should display: 0 [rockchipes8388 ]: rockchip-es8388

# Recording test (speak into the onboard mic for 5 seconds)
arecord -D default -f S16_LE -r 16000 -c 1 -d 5 test.wav

# Playback test (plug in headphones)
aplay -D default test.wav
```

### 6. Run the AI Program

```bash
./your_chat_bot_x.x.x.elf
```

## Persistent Configuration on Boot

`amixer` settings are lost after reboot. Write them to a startup script:

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

Alternatively, save the current state with `alsactl store` (if supported by the system):

```bash
sudo alsactl store 0
```

## Notes

- Onboard MIC sensitivity is limited (−42 dB); best for near-field use (within 30 cm)
- For far-field speech recognition, consider replacing the microphone with a higher-sensitivity one (−26 dB class) or adding a pre-amplifier module
- Plugging/unplugging a 3.5 mm headphone jack may affect the microphone routing; verify with a live test after configuration
- VAD threshold is set to `TKL_AUDIO_VAD_LOW` (−80 dB); if false triggers occur due to ambient noise, change it back to `TKL_AUDIO_VAD_MID` (−60 dB)

---

# SDL Display and Camera Cross-Compilation Notes (RK3576)

## SDL Display

### Prebuilt Library Details

`platform/LINUX/tuyaos_adapter/src/tkl_display/libs/RK3576/SDL2/libSDL2.a` is reused from the Raspberry Pi build (AArch64 architecture). The following backends are confirmed present via symbol table inspection (`nm`):

| Backend | Status |
|---------|--------|
| X11 | ✓ Compiled in (shows a window under a desktop environment) |
| KMS/DRM | ✓ Compiled in (drives the display directly without a desktop) |
| OFFSCREEN | ✓ Compiled in |
| DUMMY | ✓ Compiled in |
| Wayland | ✗ Not compiled in |

> Note: The `SDL_config.h` header shows `#undef` for these backends, which is misleading — the symbol table (`nm`) is the authoritative source.

SDL selects a backend automatically based on priority: X11 is used when the `DISPLAY` environment variable is set; KMS/DRM is used when no desktop is present.

### Runtime Environment Variables

```bash
# Desktop (X11) — show a window (default behavior, usually no override needed)
export SDL_VIDEODRIVER=x11
./your_chat_bot_x.x.x.elf

# Headless — drive a KMS/DRM screen directly
export SDL_VIDEODRIVER=kmsdrm
./your_chat_bot_x.x.x.elf

# Fully offscreen — no rendering to any display
export SDL_VIDEODRIVER=offscreen
./your_chat_bot_x.x.x.elf
```

---

## Camera (V4L2)

### Device Node

The RK3576 ISP pipeline exposes multiple `/dev/videoX` nodes. The **video capture output node** is typically not `/dev/video0`. The default in `RK3576.config` is:

```
CONFIG_CAMERA_V4L2_DEVNODE="/dev/video73"
```

Confirm the actual node before running:

```bash
# List all video devices and their capabilities
v4l2-ctl --list-devices

# Check whether a node supports capture (has Video Capture capability)
v4l2-ctl -d /dev/video73 --all | grep -i "capture\|format"

# List supported formats and resolutions
v4l2-ctl -d /dev/video73 --list-formats-ext
```

If `/dev/video73` does not exist or lacks capture capability, update `RK3576.config`:

```
CONFIG_CAMERA_V4L2_DEVNODE="/dev/videoN"   # replace with the actual node
```

### Resolution Configuration

Camera output resolution in `RK3576.config`:

```
CONFIG_COMP_AI_VIDEO_WIDTH=1280
CONFIG_COMP_AI_VIDEO_HEIGHT=720
```

Adjust to match your camera's actual capability; a mismatch causes V4L2 to return `EINVAL`.

### Cross-Compilation Notes

V4L2 is a kernel interface using standard Linux UAPI headers — **no extra dependencies are required for cross-compilation**. The `aarch64-none-linux-gnu` toolchain includes all necessary headers. The only thing to confirm is that the target board kernel has the relevant driver enabled (`CONFIG_VIDEO_V4L2`).
