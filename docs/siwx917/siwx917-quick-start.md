---
title: "SiWx917 Quick Start"
description: "SiWx917 quick start for TuyaOpen — build, flash, and provision your first application on the SIWX917_AI_DEV_KIT, including SLC generate caveats, TA firmware checks, and log UART wiring."
keywords:
  - SiWx917
  - quick start
  - TuyaOpen hardware
  - flashing
  - SIWX917_AI_DEV_KIT
---

Build, flash, and run your first TuyaOpen application on a SiWx917 board.

## Prerequisites

- Completed [Environment Setup](../../quick-start/enviroment-setup)
- Basic familiarity with C development and serial terminals

## Requirements

- A SiWx917 development board (`SIWX917_AI_DEV_KIT` or `BRD2605A`)
- A J-Link probe for flashing over SWD, **or** a USB-serial adapter for flashing over the ISP UART
- USB cables for the probe / serial adapter
- Computer running Linux or macOS (verified on Ubuntu 22.04 and macOS 15 on Intel; Apple Silicon and Windows are untested)
- About 5 GB of free disk (the first build pulls Simplicity SDK and SLC)
- Wi-Fi network (2.4 GHz)

:::info
If your application uses Tuya Cloud features (remote control, AI Agent, OTA), you also need a [Tuya Cloud license key](../../quick-start/equipment-authorization). Local-only projects (GPIO, UART, display demos) do not require a license.
:::

## Steps

### 1. Clone TuyaOpen and set up the environment

```bash
git clone https://github.com/tuya/TuyaOpen.git
cd TuyaOpen
git submodule update --init
```

Activate the TuyaOpen environment:

```bash
source export.sh
```

Verify the tools are working:

```bash
tos.py version
tos.py check
```

SiWx917 is a first-class platform: it is listed in `platform/platform_config.yaml` and its board support lives in `boards/SIWX917/`. The first SiWx917 build downloads Silicon Labs SLC and Simplicity Commander into `platform/SIWX917/tools/` — nothing is installed into your system.

### 2. Choose a project to build

For your first build, use the **switch demo** (a simple cloud-connected switch):

```bash
cd apps/tuya_cloud/switch_demo
```

### 3. Select the board config

```bash
tos.py config choice -c SIWX917
```

If you previously built a different board or changed peripheral Kconfig, `tos.py clean` first — otherwise SLC skips generate; see [Build](#4-build).

The AI dev kit's config is named `SIWX917` — not `SIWX917_AI_DEV_KIT`, which is the board's name; the Silicon Labs kit's config is `BRD2605A`. `-c NAME` selects by name without opening the menu — use this form in scripts, because the menu's index numbers shift as new board configs are added. To see everything on offer:

```bash
tos.py config choice -l
```

:::note
`tos.py config choice` copies the selected file over the app's `app_default.config` — that is how the tool records the active selection. Keep durable settings in `config/<BOARD>.config` and do not commit the churn this leaves in `app_default.config`: it is the app's default for every platform, and committing a SiWx917 copy would make an unconfigured build target SiWx917 for everyone else.
:::

### 4. Build

```bash
tos.py build
```

Firmware artifacts land in the app's `dist/` and `.build/` directories. The first build also pulls the Silicon Labs SDKs and tools — several gigabytes; see [the overview](overview-siwx917#the-platform-layer-and-the-first-build).

SLC code generation runs before CMake. After switching boards or changing UART / I2C / I2S Kconfig you must `tos.py clean` and rebuild, or you will see `Skipping generation` and the new config never enters `.build/slc/`. `No module named 'jinja2'`, a cross-tree `sdks/` symlink, and another `slc` on PATH are the usual traps; the full list is in the overview's [SLC notes](overview-siwx917#slc-code-generation-and-build-notes).

### 5. Flash the firmware

```bash
tos.py flash
```

Two prompts appear:

- **Channel**: `swd` (J-Link probe) or `serial` (ISP UART).
- **Target**: `M4 ONLY` (your application), `TA ONLY` (NWP wireless firmware), or `TA + M4` (both).

Skip the prompts in scripts with environment variables — `SIWX917_CHANNEL` is `swd` or `serial`, `SIWX917_FLASH` is `app`, `ta`, or `both`:

```bash
SIWX917_CHANNEL=swd SIWX917_FLASH=app tos.py flash
```

:::warning[TA firmware — check before writing]
The radio runs its own firmware (TA), separate from your application, and the application cannot start without it. **Boards normally arrive with it already programmed.** Writing it erases the radio's flash, and an interrupted write has left a board needing recovery — so write TA only when the device actually reports having none. `tos.py flash` reads the device's TA version and tells you when it is missing.
:::

After writing TA, power-cycle the board before concluding anything failed: the install is finalized at reset, and a debug probe's reset does not always finalize it. A board has been seen read its new TA version back, print nothing, and then come up normally after a power-cycle.

### 6. Open the log

The application log leaves on the **ULP UART** at 115200 8N1.

- **BRD2605A**: the log lands on the on-board J-Link VCOM, so `tos.py monitor` finds it with no wiring.
- **External probe** (AI dev kit): wire a USB-serial adapter:

| Adapter | → | Chip signal | Flat pin |
|---------|---|-------------|----------|
| RX | ← | ULP log TX (`GPIO_NUM_41`) | 75 |
| TX | → | ULP log RX (`GPIO_NUM_39`) | 73 |
| GND | — | GND | |

Connecting RX and GND is enough to read the log. This is a *different* UART pair from the ISP UART used for serial flashing — the two cannot share wires.

```bash
tos.py monitor
```

### 7. Provision and verify

Provision the device over **BLE** with the Tuya Smart app — on SiWx917, BLE is the only working provisioning method (SoftAP and BLE cannot coexist on this chip, and the adapter enforces this automatically). Follow [Device Network Configuration](../../quick-start/device-network-configuration).

With Wi-Fi connected you should see the switch demo reporting state to the cloud.

## Advanced: M4 flash size

The M4 flash region defaults to 2040 KB. `tos.py config menu` → **Core M4 Flash Size** offers 3008 KB, which requires an MBR update on the device. After building with 3008 KB the build prints instructions and generates `mbr_config.json`; write it with the bundled Commander:

```bash
platform/SIWX917/tools/commander/commander manufacturing write tambr  --data mbr_config.json -d SiWG917M111MGTBA
platform/SIWX917/tools/commander/commander manufacturing write m4mbrcf --data mbr_config.json -d SiWG917M111MGTBA
```

## Troubleshooting

**The build fails during SLC generate.** The wrapper prints `Failed to generate <project>`; the cause is above that line. Match the log:

| Log | Cause and fix |
|-----|----------------|
| Config did not take effect after `Skipping generation` | `.build/slc/` already exists, so SLC was skipped. `tos.py clean` and rebuild |
| `No module named 'jinja2'` / `Feature lack: 'apack.core:4'` | SLC did not load its bundled Python; do not pip-install jinja2 into `.venv`. See the [overview](overview-siwx917#slc-code-generation-and-build-notes) |
| Extension path outside the SDK root / trust failed | `sdks/` is a symlink into another tree and SLC's `realpath` no longer matches. Use a real directory, do not cross-link checkouts |
| `python: not found` | `export.sh` was not sourced, or the venv was deactivated. Platform scripts need `python` from `.venv` |
| `No need prepare` but `arm-none-eabi-gcc` is missing | `tools/toolchain` exists, the toolchain does not. Delete that directory and rebuild |
| `command -v slc` picked a system copy | Simplicity Studio's `slc` is on PATH. `which slc` should point at `platform/SIWX917/tools/slc/` |

**The application boots but the radio does not start.** Typical log lines:


```
[tuyaos][E][app_tuya.c] WiFi initialization error 16056    SL_STATUS_VALID_FIRMWARE_NOT_PRESENT
[tuyaos][E][app_tuya.c] WiFi initialization error 16059    SL_STATUS_CARD_READY_TIMEOUT
[tuyaos][I][app_tuya.c] Failed to bring m4_ta_secure_handshake: 0x7   (timeout)
```

**Power-cycle first** — a staged TA install is finalized at reset, and this alone has recovered boards that looked dead. If the errors persist:

1. Check what the device has stored (a version being *stored* does not prove the image is *intact*, only that metadata exists):

   ```bash
   platform/SIWX917/tools/commander/commander mfg917 info -d SiWG917M111MGTBA --json
   ```

2. Write the TA firmware deliberately, then power-cycle again:

   ```bash
   SIWX917_FLASH=ta tos.py flash                 # over SWD, if a probe is attached
   SIWX917_FLASH=ta tos.py flash -p /dev/ttyUSB0 # over serial/ISP
   ```

If none of this recovers the board, the next stop is the chip's ROM bootloader — see the next section. `GETTING_STARTED.md` in the platform repository (`platform/SIWX917/`) carries the full procedures, including the exact Kermit configuration.

## Advanced: the ROM bootloader menu

Besides your application and the TA, the chip carries a third firmware: the ROM bootloader, written at the factory and not erasable. It talks over the ISP UART and is the tool to reach for when the radio will not start — it can tell a slot that is *stored* from one that is *stored and intact*, and repoint the default image in a few bytes instead of retransmitting 1.6 MB. Rewriting the TA three times over has fixed nothing on a real recovery, because what had broken was the bootloader's record of *which* image to load.

Entering it needs a USB-serial adapter on the **ISP UART** — a different pair from the log UART of step 6; the two cannot share wires:

| Adapter | → | Chip | GPIO | Package pin |
|---------|---|------|------|-------------|
| TX | → | RX | GPIO_8 | A20 |
| RX | ← | TX | GPIO_9 | A21 |
| GND | — | GND | — | — |

Then put the chip into ISP mode: hold GPIO_34 (`JTAG_TDO_SWO`) low, tap Reset, release. Holding it low does **not** disable SWD; conversely, normal boot requires the pin left *unconnected* during reset. In any serial terminal, send `Ctrl+\` (0x1C) to wake the bootloader, then `U` to print its menu.

The entries that matter (transcribed from a device's printed menu; `B`/`4`/`1` also appear in Silicon Labs' AN1431):

| Key | Entry | Writes flash? |
|-----|-------|---------------|
| `K` | Check Wireless Firmware Integrity (slot 0–f) | no |
| `5` | Select Default Wireless Firmware (slot 0–f) | a few bytes |
| `1` | Load Default Wireless Firmware | no |
| `B` | Burn Wireless Firmware (slot 0–f) | yes, ~1.6 MB |
| `A` | Load Wireless Firmware (slot 0–f) | no |
| `F` | Select M4 and Wireless Images Pair | a few bytes |
| `4` → `1` | Burn M4 Firmware, image 1 | yes |
| `b` | Change UART Baud Rate | no |

Work in this order — the cost differs by orders of magnitude:

1. **`K` on each slot 0–f.** The only way to tell *stored* from *stored and intact*; `mfg917 info` reads metadata and cannot make that distinction.
2. **Power-cycle before anything else.** A staged image finalises at reset, and a debug probe's reset may not finalise it.
3. **`5` + slot** when a slot passes `K` but the device still reports 16056 — repoints the default in a few bytes.
4. **`B` + slot** only when no slot holds a good image. The image moves over Kermit, and the ROM receiver is a minimal implementation: C-Kermit needs `packet-length 94`, `window 1`, `block-check 1`, `prefixing all` and more (full list in `GETTING_STARTED.md`). At 921600 baud the ~1.6 MB transfer takes about 85 s; at 115200 it runs over ten minutes, so raise the rate first (`b`).

## Next steps

- [Equipment Authorization](../../quick-start/equipment-authorization): Burn the license key before connecting to Tuya Cloud.
- [Device Network Configuration](../../quick-start/device-network-configuration): Pairing over BLE in detail.
- [SiWx917 Overview](overview-siwx917): How the platform stacks up.
