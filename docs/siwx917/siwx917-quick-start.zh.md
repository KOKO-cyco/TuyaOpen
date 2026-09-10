---
title: "SiWx917 快速开始"
description: "在 SiWx917 开发板上构建、烧录并配网你的第一个 TuyaOpen 应用:覆盖 SLC 代码生成注意事项、TA 固件检查、日志串口接线与 BLE 配网。"
keywords:
  - SiWx917
  - 快速开始
  - 烧录
  - 配网
  - TuyaOpen 硬件
---

在 SiWx917 开发板上构建、烧录并运行你的第一个 TuyaOpen 应用。

## 前提条件

- 已完成[环境搭建](../../quick-start/enviroment-setup)
- 了解基本的 C 语言开发和串口终端使用

## 准备硬件

- 一块 SiWx917 开发板(`SIWX917_AI_DEV_KIT` 或 `BRD2605A`)
- 一个 J-Link 调试器(SWD 烧录),**或**一个 USB 串口适配器(ISP 串口烧录)
- 调试器 / 串口适配器对应的 USB 数据线
- 一台 Linux 或 macOS 电脑(已在 Ubuntu 22.04 和 macOS 15 Intel 上验证;Apple Silicon 和 Windows 未验证)
- 约 5 GB 空闲磁盘(首次构建要拉 Simplicity SDK 和 SLC)
- Wi-Fi 网络(2.4 GHz)

:::info
如果你的应用使用 Tuya Cloud 功能(远程控制、AI Agent、OTA),还需要一个 [Tuya Cloud 授权码](../../quick-start/equipment-authorization)。仅本地运行的项目(GPIO、UART、显示示例)不需要授权码。
:::

## 步骤

### 1. 克隆 TuyaOpen 并配置环境

```bash
git clone https://github.com/tuya/TuyaOpen.git
cd TuyaOpen
git submodule update --init
```

激活 TuyaOpen 环境:

```bash
source export.sh
```

确认工具可用:

```bash
tos.py version
tos.py check
```

SiWx917 是一等公民平台:它已列在 `platform/platform_config.yaml` 中,板级支持位于 `boards/SIWX917/`。第一次构建 SiWx917 会把 Silicon Labs SLC 和 Simplicity Commander 下载到 `platform/SIWX917/tools/`——不会装进你的系统。

### 2. 选择要构建的工程

第一次构建建议用 **switch demo**(一个简单的云连接开关):

```bash
cd apps/tuya_cloud/switch_demo
```

### 3. 选择板级配置

```bash
tos.py config choice -c SIWX917
```

以前编过另一块板或改过外设 Kconfig,先 `tos.py clean`,否则 SLC 会跳过生成,见[构建](#4-构建)。

AI 开发板的配置名叫 `SIWX917`——不是 `SIWX917_AI_DEV_KIT`,后者是板子的名字;Silicon Labs 板的配置名叫 `BRD2605A`。`-c NAME` 按名字选择、不弹菜单——写脚本请用这种形式,因为菜单的序号会随着新板型加入而变化。想看全部可选项:

```bash
tos.py config choice -l
```

:::note
`tos.py config choice` 会把选中的文件复制为应用的 `app_default.config`——这是工具记录当前选择的方式。持久的配置改动请放在 `config/<BOARD>.config`,`app_default.config` 里留下的变动不要提交:它是应用对全部平台的默认值,提交一份 SiWx917 版本会让别人未配置的构建默认指向 SiWx917。
:::

### 4. 构建

```bash
tos.py build
```

固件产物在应用的 `dist/` 和 `.build/` 目录下。首次构建还会拉取 Silicon Labs 的 SDK 和工具(数 GB),见[概述](overview-siwx917#平台层目录与首次构建)。

这一步里 CMake 之前会跑 SLC 代码生成。换板、改 UART / I2C / I2S 等 Kconfig 之后必须 `tos.py clean` 再编,否则会看到 `Skipping generation`,新配置进不了 `.build/slc/`。`No module named 'jinja2'`、跨树软链 `sdks/`、PATH 上另一份 `slc` 都是常见坑,完整清单见[概述里的 SLC 注意事项](overview-siwx917#slc-代码生成与编译注意事项)。

### 5. 烧录固件

```bash
tos.py flash
```

会依次出现两个提示:

- **通道**:`swd`(J-Link 调试器)或 `serial`(ISP 串口)。
- **目标**:`M4 ONLY`(你的应用)、`TA ONLY`(NWP 无线固件)或 `TA + M4`(两者都写)。

脚本里可用环境变量跳过提示——`SIWX917_CHANNEL` 取 `swd` 或 `serial`,`SIWX917_FLASH` 取 `app`、`ta` 或 `both`:

```bash
SIWX917_CHANNEL=swd SIWX917_FLASH=app tos.py flash
```

:::warning[TA 固件——先检查再烧写]
射频核心运行着自己的固件(TA),与你的应用相互独立,应用没有它就无法启动。**板子出厂通常已烧好。** 烧写 TA 会先擦掉射频的 flash,中途断开曾把一块板子变成需要救援的状态——所以只在设备确实报告没有 TA 时才烧写。`tos.py flash` 会读取设备的 TA 版本,并在缺失时提示你。
:::

烧写 TA 之后,先断电重新上电再下结论:安装动作在复位时才最终生效,调试器的复位不一定触发它。出现过一块板子读回了新 TA 版本却一行日志不打,断电重启后恢复正常。

### 6. 查看日志

应用日志从 **ULP UART** 输出,115200 8N1。

- **BRD2605A**:日志落在板载 J-Link VCOM 上,`tos.py monitor` 无需任何接线即可找到。
- **外部调试器**(AI 开发板):把 USB 串口适配器接成下面这样:

| 适配器 | → | 芯片信号 | 排针脚位 |
|--------|---|----------|----------|
| RX | ← | ULP 日志 TX(`GPIO_NUM_41`) | 75 |
| TX | → | ULP 日志 RX(`GPIO_NUM_39`) | 73 |
| GND | — | GND | |

只接 RX 和 GND 就足以看到日志。这组 UART 与串口烧录用的 ISP UART **不是同一对**,两者不能共线。

```bash
tos.py monitor
```

### 7. 配网并验证

用涂鸦智能 App 通过 **BLE** 配网——在 SiWx917 上 BLE 是唯一可用的配网方式(这颗芯片上 SoftAP 和 BLE 无法共存,适配层已自动强制)。详细步骤见[设备配网](../../quick-start/device-network-configuration)。

Wi-Fi 连上后,应当能看到 switch demo 开始向云端上报状态。

## 进阶:M4 flash 大小

M4 flash 区默认 2040 KB。`tos.py config menu` → **Core M4 Flash Size** 提供 3008 KB 选项,它需要对设备做 MBR 更新。用 3008 KB 构建后,构建过程会打印操作说明并生成 `mbr_config.json`,用平台自带的 Commander 写入:

```bash
platform/SIWX917/tools/commander/commander manufacturing write tambr  --data mbr_config.json -d SiWG917M111MGTBA
platform/SIWX917/tools/commander/commander manufacturing write m4mbrcf --data mbr_config.json -d SiWG917M111MGTBA
```

## 故障排查

**构建在 SLC 生成阶段失败。** 包装脚本只印 `Failed to generate <project>`,根因在它上面。先对号:

| 日志 | 原因与处理 |
|------|------------|
| `Skipping generation` 后配置没生效 | `.build/slc/` 已存在,SLC 被跳过。`tos.py clean` 再编 |
| `No module named 'jinja2'` / `Feature lack: 'apack.core:4'` | SLC 没用上自带 Python;不要往 `.venv` 装 jinja2,见[概述](overview-siwx917#slc-代码生成与编译注意事项) |
| extension 路径不在 SDK 根下 / trust 失败 | `sdks/` 被软链到另一棵树,SLC 的 `realpath` 对不上。用真实目录,不要跨 checkout 软链 |
| `python: not found` | 没 `source export.sh`,或已经 `deactivate`。平台脚本需要 `.venv` 里的 `python` |
| `No need prepare` 但找不到 `arm-none-eabi-gcc` | `tools/toolchain` 目录在、工具链不在。删掉该目录再构建 |
| `command -v slc` 用了系统那份 | PATH 上有 Simplicity Studio 的 `slc`。先 `which slc`,确认走的是 `platform/SIWX917/tools/slc/` |

**应用启动了,但射频起不来。** 典型日志:

```
[tuyaos][E][app_tuya.c] WiFi initialization error 16056    SL_STATUS_VALID_FIRMWARE_NOT_PRESENT
[tuyaos][E][app_tuya.c] WiFi initialization error 16059    SL_STATUS_CARD_READY_TIMEOUT
[tuyaos][I][app_tuya.c] Failed to bring m4_ta_secure_handshake: 0x7   (timeout)
```

**先断电重启**——TA 安装在复位时才最终生效,仅这一步就救回过看似死掉的板子。如果报错依旧:

1. 查设备里存了什么(读到版本号只说明元数据存在,**不代表镜像完好**):

   ```bash
   platform/SIWX917/tools/commander/commander mfg917 info -d SiWG917M111MGTBA --json
   ```

2. 明确地重写 TA 固件,然后再断电重启:

   ```bash
   SIWX917_FLASH=ta tos.py flash                 # SWD,接了调试器时
   SIWX917_FLASH=ta tos.py flash -p /dev/ttyUSB0 # 串口/ISP
   ```

如果这些都无法救回,下一站是芯片的 ROM bootloader——见下一节。`platform/SIWX917/` 平台仓库里的 `GETTING_STARTED.md` 有完整流程,包括 Kermit 的全部参数。

## 进阶:ROM bootloader 菜单

除了你的应用和 TA 固件,芯片里还有第三套固件:ROM bootloader,出厂时写死、不可擦除。它通过 ISP UART 交互,是射频起不来时该先问的对象——它能分辨一个槽位是"存了"还是"存了且完好",重指默认镜像只需几个字节,而重传 1.6 MB 没有这个必要。真实救援里出现过把 TA 换三种方式重写三遍都没用的情况,坏的其实是 bootloader 记录的"该加载哪个镜像"。

进菜单需要把 USB 串口适配器接到 **ISP UART**——它和第 6 步的日志 UART 不是同一对,两者不能共线:

| 适配器 | → | 芯片 | GPIO | 封装脚 |
|--------|---|------|------|--------|
| TX | → | RX | GPIO_8 | A20 |
| RX | ← | TX | GPIO_9 | A21 |
| GND | — | GND | — | — |

然后让芯片进 ISP 模式:把 GPIO_34(`JTAG_TDO_SWO`)拉低,点一下 Reset,松开。拉低这个脚**不会**关掉 SWD;反过来,正常启动时该脚在复位期间必须**悬空**。任意串口终端里先发 `Ctrl+\`(0x1C)唤醒 bootloader,再按 `U` 打印菜单。

值得关心的条目如下(转录自一台设备实际打印的菜单;`B`/`4`/`1` 也见于 Silicon Labs 的 AN1431):

| 按键 | 条目 | 写 flash? |
|------|------|-----------|
| `K` | Check Wireless Firmware Integrity(槽位 0–f) | 否 |
| `5` | Select Default Wireless Firmware(槽位 0–f) | 几字节 |
| `1` | Load Default Wireless Firmware | 否 |
| `B` | Burn Wireless Firmware(槽位 0–f) | 是,约 1.6 MB |
| `A` | Load Wireless Firmware(槽位 0–f) | 否 |
| `F` | Select M4 and Wireless Images Pair | 几字节 |
| `4` → `1` | Burn M4 Firmware,image 1 | 是 |
| `b` | Change UART Baud Rate | 否 |

按这个顺序操作——不同动作的代价差着几个数量级:

1. **对每个槽 0–f 逐个按 `K`。** 这是分辨"存了"和"存了且完好"的唯一办法;`mfg917 info` 读元数据,分不出这两者。
2. **动手之前先断电重启。** 暂存的镜像在复位时才最终生效,调试器的复位不一定触发生效。
3. **`5` + 槽位**:某个槽 `K` 通过、设备却仍报 16056 时——几个字节把默认槽重指过去。
4. **`B` + 槽位**:只有没有任何完好槽位时才用。镜像走 Kermit 传输,而 ROM 里的接收端是个极简实现:C-Kermit 需要 `packet-length 94`、`window 1`、`block-check 1`、`prefixing all` 等一整套参数(完整清单在 `GETTING_STARTED.md`)。921600 波特率下约 85 s 传完,115200 下要十几分钟,所以先用 `b` 把波特率提上去。

## 下一步

- [设备授权](../../quick-start/equipment-authorization):连接涂鸦云前先烧录 license。
- [设备配网](../../quick-start/device-network-configuration):BLE 配网详解。
- [SiWx917 概述](overview-siwx917):了解平台整体架构。
