---
title: "SIWX917_AI_DEV_KIT 开发板"
description: "世强 SK_SIWG917_AI_MB AI 开发套件在 TuyaOpen 上的板级说明:ST7789 显示屏、模拟麦克风与喇叭、按键与 RGB LED 的引脚映射、外设占用与已知冲突。"
keywords:
  - SIWX917_AI_DEV_KIT
  - SK_SIWG917_AI_MB
  - SiWx917
  - 世强
  - TuyaOpen 硬件
---

`SIWX917_AI_DEV_KIT` 是世强推出的 SiWx917 AI 开发套件(型号 `SK_SIWG917_AI_MB`),TuyaOpen 为它做了板级适配。整板围绕语音与显示交互:一块 ST7789 SPI 屏、模拟麦克风加喇叭(无外置 codec)、一个对话按键和 RGB LED,是跑 `your_chat_bot` 这类 AI Agent 应用的现成载体。

![SK_SIWG917_AI_MB AI 开发套件实物图](/img/hardware/siwx917/siwx917-ai-dev-kit.png)

## 板级配置

板级配置文件定义了外设驱动、引脚映射、BSP 和第三方库参数。用现成的板级配置构建,能省去大量硬件适配工作。

| 项 | 值 |
|----|----|
| 配置名(`tos.py config choice -c`) | `SIWX917` |
| Kconfig 板型(`BOARD_CHOICE`) | `SIWX917_AI_DEV_KIT` |
| 配置文件 | [`boards/SIWX917/config/SIWX917.config`](https://github.com/tuya/TuyaOpen/blob/master/boards/SIWX917/config/SIWX917.config) |
| BSP 源码 | [`boards/SIWX917/SIWX917_AI_DEV_KIT/`](https://github.com/tuya/TuyaOpen/tree/master/boards/SIWX917/SIWX917_AI_DEV_KIT) |

选择这块板:

```bash
tos.py config choice -c SIWX917
```

## 规格

| 特性 | 规格 |
|------|------|
| 模组 | SiWG917M111MGTBA |
| CPU | ARM Cortex-M4 @ 180 MHz |
| 无线 | Wi-Fi 6(802.11ax,2.4 GHz)+ Bluetooth LE |
| Flash | 8 MB |
| PSRAM | 8 MB 封装内(承载 `.text`/`.bss`/FreeRTOS 堆) |
| 显示 | ST7789 320×240 SPI,RGB565 |
| 音频 | 模拟麦克风 + 喇叭,无外置 codec(走片上 I2S) |
| 交互 | 对话按键 SW1、按键 SW2/SW3、RGB LED |
| 烧录/调试 | J-Link(SWD)或 ISP 串口;日志走 ULP UART |

## 板级层引脚(应用可见)

出处 [`SIWX917_AI_DEV_KIT/Kconfig`](https://github.com/tuya/TuyaOpen/blob/master/boards/SIWX917/SIWX917_AI_DEV_KIT/Kconfig) 与 `siwx917_ai_board.c`。TUYA GPIO 号到芯片 pad 的换算见 [两块板通用的编号换算](#芯片层编号换算两块板通用)。

| 功能 | Kconfig 项 | TUYA GPIO | 芯片位置 | 注册名 | 是否注册 |
|------|-----------|-----------|----------|--------|----------|
| 按键 1 | `BOARD_SW1_PIN` | 49 | HP 49 | `ai_chat_button` | 是 |
| 按键 2 | `BOARD_SW2_PIN` | 2 | UULP 2 | `SW2` | 是 |
| 按键 3 | `BOARD_SW3_PIN` | 3 | UULP 3 | `SW3` | 否(Kconfig 有,板级 .c 未注册) |
| LED R | `BOARD_LEDR_PIN` | 50 | HP 50 | `LEDR` | 是 |
| LED G | `BOARD_LEDG_PIN` | 51 | HP 51 | `LEDG` | 否(被拿去当屏 CS 的占位脚,见下) |
| LED B | `BOARD_LEDB_PIN` | 15 | HP 15 | `LEDB` | 是 |

按键统一 `LEVEL_LOW` + `PULLUP` + `TIMER_SCAN_MODE`;LED 统一 `LEVEL_LOW` + `PUSH_PULL`。

### 显示屏(只有这块板有)

出处 `siwx917_ai_board.c` 里写死的宏,**没有进 Kconfig**:

| 信号 | 宏 | TUYA GPIO | 备注 |
|------|----|-----------|------|
| SPI 端口 | `BOARD_LCD_SPI_PORT` | `TUYA_SPI_NUM_3` | = GSPI_MASTER |
| SPI 时钟 | `BOARD_LCD_SPI_CLK` | 40 MHz | |
| DC | `BOARD_LCD_SPI_DC_PIN` | 29 | |
| RST | `BOARD_LCD_SPI_RST_PIN` | 26 | 与 GSPI_MASTER MISO 同脚,见占用表 |
| CS | `BOARD_LCD_SPI_CS_PIN`(28) | 未使用 | 实际传入的是 `BOARD_LEDG_PIN`(51) 顶位;CS0(28) 由 GSPI 硬件自动驱动 |
| 背光 | `BOARD_LCD_BL_PIN` | 30 | GPIO 背光,高有效 |
| 电源 | `BOARD_LCD_POWER_PIN` | `TUYA_GPIO_NUM_MAX` | 不使用 |
| 分辨率 | | 320×240 RGB565 | |

`cs_pin` 字段填 51 而非 28,是因为 GSPI 硬件已自动在 CS0(28)上驱动片选,填个不用的脚顶位;定义好的 `BOARD_LCD_SPI_CS_PIN`(28)是死代码。

## SLC / SDK 层引脚(外设走线)

SiWx917 的外设引脚有两套独立来源:上面的 Tuya 板级 Kconfig,以及厂商 SDK 的 RTE / pin_config。二者之间没有交叉校验。SDK 层的生效机制是:`RTE_Device_917.h` 里每个信号先看 `pin_config.h` 有没有覆盖(`#ifndef <SIG>_LOC`),没覆盖就走 `PORT_ID` 默认值。

**这块板的 `pin_config.h` 只覆盖了 I2S0 一组信号**,其余外设(GSPI、I2C、USART…)全走 RTE 默认值。

### I2S0 — 音频(被 pin_config.h 覆盖)

| 信号 | TUYA GPIO / 芯片 | 用途 |
|------|------------------|------|
| SCLK | HP 46 | |
| WSCLK | HP 47 | |
| DOUT0 | HP 11 | 喇叭输出 |
| DIN0 | HP 48 | 麦克风输入 |

:::warning[SDK 里有一份错注释]
`sl_si91x_i2s_init_i2s0_config.h` 中同一组值的注释写着 `GPIO_25/26/28/27`,而 `#define` 的值是 `46/47/11/48`。注释里的号正好是 RTE 默认值,是改脚时留下的旧注释,**以 `#define` 的值为准**。同目录 `pin_config.h` 的注释是对的。
:::

### GSPI_MASTER — 屏的 SPI 总线(RTE 默认值)

| 信号 | TUYA GPIO / 芯片 |
|------|------------------|
| CLK | HP 25 |
| MISO | HP 26 |
| MOSI | HP 27 |
| CS0 | HP 28 |
| CS1 / CS2 | HP 29 / HP 30 |

### 其余外设(RTE 默认值)

`tkl_spi.c` 的 SPI 端口映射:`SPI_NUM_0`→SSI_ULP_MASTER、`SPI_NUM_1`→SSI_MASTER、`SPI_NUM_2`→SSI_SLAVE、`SPI_NUM_3`→GSPI_MASTER。

| 项 | 值 |
|----|----|
| 日志串口 | ULP_UART,TX = ULP 11 = **TUYA 41**,RX = ULP 9 = **TUYA 39**,115200 8N1 |
| SSI_MASTER | SCK 25 / MOSI 26 / MISO 12 / CS0 28 |
| I2C0 | SCL pad 65(ULP 区)/ SDA HP 6 |
| I2C1 | SCL HP 50 / SDA HP 51 |
| USART0 | TX HP 15 / RX HP 10 |
| PSRAM | **HP 52 – 57**(6 脚全占) |

## 引脚占用汇总

只列真正生效的。同一脚出现多次即为潜在冲突。

| TUYA GPIO | 芯片 | 占用方 | 状态 |
|-----------|------|--------|------|
| 2 | UULP 2 | 按键 SW2 | 生效 |
| 3 | UULP 3 | 按键 SW3(Kconfig 有,未注册) | 未生效 |
| 11 | HP 11 | I2S0 DOUT0(喇叭输出) | 生效 |
| 15 | HP 15 | LED B | 生效 |
| 25 | HP 25 | GSPI CLK(屏时钟) | 屏在用 |
| 26 | HP 26 | GSPI MISO **＋ 屏 RST** | ⚠️ 同脚两用 |
| 27 | HP 27 | GSPI MOSI(屏数据) | 屏在用 |
| 28 | HP 28 | GSPI CS0(硬件自动 CS) | 屏在用 |
| 29 | HP 29 | 屏 DC(也是 GSPI CS1,未用) | 生效 |
| 30 | HP 30 | 屏背光(也是 GSPI CS2,未用) | 生效 |
| 39 | ULP 9 | 日志串口 RX | 生效 |
| 41 | ULP 11 | 日志串口 TX | 生效 |
| 46 | HP 46 | I2S0 SCLK | 生效 |
| 47 | HP 47 | I2S0 WSCLK | 生效 |
| 48 | HP 48 | I2S0 DIN0(麦克风) | 生效 |
| 49 | HP 49 | 按键 SW1(`ai_chat_button`) | 生效 |
| 50 | HP 50 | LED R(也是 I2C1 SCL / SSI CS2,未用) | 生效 |
| 51 | HP 51 | 屏 CS 顶位脚(名义 LEDG;也是 I2C1 SDA) | ⚠️ 名不符实 |
| 52 – 57 | HP 52-57 | PSRAM(6 脚) | 生效 |

### 需要注意的点

1. **GPIO 26 同时是 GSPI MISO 和屏 RST**。屏是只写设备不需要 MISO,所以能跑,但代码里没有一处说明这是有意为之。
2. **GPIO 51 名义 LEDG、实际当屏 CS 占位**,真正的片选由 GSPI 硬件在 28 上驱动,所以 `boards` 配置里的 LEDG 注册被略过。
3. **PSRAM 吃掉 52–57**,但 `tkl_gpio.c` 的引脚表仍把 52–57 当普通可用 GPIO 暴露给应用,没有保护。
4. `Kconfig` 里的引脚和 SLC/RTE 里的引脚是**两套独立来源**,构建时不会交叉校验,上面这些重叠都是人工比对得出的。换板或改外设之后必须让 SLC 重新生成,否则 `.build/slc/` 里仍是旧 pin 头文件,见[概述里的 SLC 注意事项](overview-siwx917#slc-代码生成与编译注意事项)。

## 芯片层编号换算(两块板通用)

`tkl_gpio.c` 的 `SI91X_PIN_MAPPING`:

| TUYA GPIO 号 | 芯片端口 | pad 号 |
|--------------|----------|--------|
| 0 – 4 | UULP_VBAT | 同号 0 – 4 |
| 6 – 12, 15, 25 – 34, 46 – 57 | HP | 同号 |
| 20 – 24 | ULP | 0 – 4 |
| 35 – 41 | ULP | 5 – 11 |

SLC / RTE 里写的 `PIN` 是**端口内的 pad 号**,所以 `HP 46` = TUYA GPIO 46,`ULP 11` = TUYA GPIO 41。

:::note[GPIO 8/16 保留]
`tkl_gpio.c` 注明 `TUYA_GPIO_NUM_8` 和 `TUYA_GPIO_NUM_16` 用于 vcom,不要在应用里当普通 GPIO 用。
:::

## 接线与日志

- 烧录、TA 固件检查、BLE 配网的完整步骤见 [SiWx917 快速开始](siwx917-quick-start)。
- 用外部调试器看日志时,把 USB 串口的 RX 接到排针 **75 脚**(ULP 日志 TX),GND 接 GND 即可;这组口与 ISP 烧录口不能共线,细节见快速开始的[查看日志](siwx917-quick-start#6-查看日志)一节。

## 资源

- [SK_SiWx917_AI_MB 原理图 V1.1(PDF)](/docs/hardware/siwx917/SK_SiWx917_AI_MB_Schematic_V1.1.pdf)
- [SK_SIWG917_AI_MB 购买链接(世强)](https://www.sekorm.com/product/603942256.html)
- [SiWx917 快速开始](siwx917-quick-start)
- [SiWx917 概述](overview-siwx917)
