# TuyaOpen SDK 全解（从小白到老员工）

这是一本关于 TuyaOpen SDK 的完整参考。它回答四类问题：

- **SDK 由什么构成**：分了哪些层、层与层靠什么约定衔接
- **跨平台怎么做到**：一份代码如何同时编到 T5AI、ESP32、Linux
- **硬件长什么样**：三大平台各自的内存、存储、启动方式
- **数据怎么流动**：从上电到连云、从麦克风到大模型、从 App 到继电器

读之前只需会看 C 代码、知道"编译"和"函数调用"是什么。

**关于可信度**：文中每个技术结论都标了文件与行号，你可以自己打开验证。**别信任何没有出处的技术描述，包括这篇。** 少数无法从本仓库验证的内容（例如未下载的平台适配层）会明确标注。

## 导读：怎么读这本书

- **第一次读**：一到三章建立地图，然后直接跳到你当前要做的那一章
- **当词典用**：附录 A 是目录速查，附录 B 是踩坑清单，附录 C 是自测题
- **对着日志读**：第四、五、七章配真机串口日志读，效果最好

## 第一章　先建立世界观

### 1.1 为什么需要 SDK

假设你要做一个智能开关。真正属于"开关"的逻辑可能只有十行，但你还得写：连 Wi-Fi 并断线重连、让 App 发现和绑定、和云建立加密长连接、把状态用云能懂的格式上报、解析下发命令、固件升级、时间同步、恢复出厂设置。

这些和"开关"没半点关系，却每个产品都要重写。**SDK 就是把这些与产品无关的部分做完**，让你只写那 5% 真正属于你产品的逻辑。

`apps/tuya_cloud/switch_demo` 整个应用的核心只有一句（`src/cli_cmd.c:54`）：

```c
if (0 == strcmp(argv[1], "on")) {
    sprintf(bool_value, "{\"1\": true}");
}
tuya_iot_dp_report_json(tuya_iot_client_get(), bool_value);
```

### 1.2 为什么要分层

因为**变化的东西和不变的东西速度不一样**。

"和云端用什么格式传数据"换芯片不会变；"怎么让某个引脚输出高电平"换芯片全变。写在一个文件里，换芯片就要重写业务逻辑。

分层的本质是**把变化关进笼子**：定义一组契约（函数签名），业务层只调契约，谁来实现由平台决定。

### 1.3 三个身份别搞混

这是新手最常见的混淆源，先钉死：

- **平台（platform）**：芯片是什么。`T5AI`（芯片型号 BK7258）、`ESP32`、`LINUX`
- **板子（board）**：这块板上焊了什么。`TUYA_T5AI_BOARD`、`DNESP32S3`、`TaishanPi_3`
- **应用（app）**：这个产品干什么。`switch_demo`、`camera_demo`、`your_chat_bot`

编译时这三个变量填进同一套骨架，产出不同固件。

另外三个**设备身份**同样容易混：

- **PID（产品 ID）**，如 `qhivvyqawogv04e4`。标识"哪一款产品"，同型号一万台共用一个。决定 App 显示什么面板、有哪些功能点
- **UUID + authkey**，如 `uuid0c83873343f39549`。标识"哪一台设备"，相当于身份证加密码，一机一份，从涂鸦平台购买授权获得
- **devId**，如 `6c881d022706616ac8r9u5`。**激活后云端分配**的运行时身份，MQTT 主题和 DP 上报都用它

**关系是**：拿 UUID+authkey 去激活一个 PID，云端返回一个 devId。

### 1.4 必须懂的词

- **固件**：烧进 Flash、上电就跑的程序镜像
- **线程**：操作系统里"同时推进"的一条执行流，各有自己的栈
- **栈**：函数调用和局部变量用的内存，太小会栈溢出死机
- **堆**：动态申请（malloc）的内存
- **阻塞 / yield**：卡住等 I/O，对比主动交回控制权让别人也能跑
- **回调**：事情发生时请调用我写的这个函数
- **事件**：系统广播"MQTT 连上了"，订阅者各自处理
- **KV**：Key-Value 存储，字符串键存一段数据，掉电还在
- **DP**：Data Point，App 与设备约定的一个控制点
- **激活**：设备第一次用 token 向云换取 devId 与密钥
- **配网**：把家里 Wi-Fi 的账号密码交给设备的过程
- **TLS**：传输层加密，MQTT 和 HTTPS 都靠它
- **OTA**：空中升级，云下发新固件，设备写 Flash 后重启切换
- **ASR / TTS / VAD / KWS**：语音转文字 / 文字转语音 / 检测有没有人说话 / 本地关键词唤醒

## 第二章　五层地图

### 2.1 整体形状

```
   应用      apps/tuya_cloud/switch_demo         你的产品逻辑
   ───────────────────────────────────────────────────────
   板级      boards/T5AI/TUYA_T5AI_BOARD         这块板焊了哪些器件
   ───────────────────────────────────────────────────────
   业务      src/tuya_cloud_service/             激活、MQTT、DP、LAN、OTA
   AI        src/tuya_ai_service/ ai_components/ 会话、播放器、UI
   外设      src/peripherals/  (TDL + TDD)       摄像头、屏、按键、LED
   系统      src/tal_*/                          线程、锁、存储、日志、事件
   ───────────────────────────────────────────────────────
   契约      tools/porting/adapter/  (TKL 头)    「芯片必须实现这些函数」
   ───────────────────────────────────────────────────────
   实现      platform/T5AI/tuyaos_adapter/       某颗芯片的具体实现
```

**关键理解：中间那条粗线（TKL 契约）是整个 SDK 的腰。** 线以上的代码永远不知道自己跑在什么芯片上，线以下的代码永远不知道自己在为什么业务服务。

### 2.2 TKL：平台契约层

TKL = Tuya Kernel Layer。**只有头文件，没有实现**——实现是各平台的事。

契约在 `tools/porting/adapter/`，33 个能力目录：

```
adc  bluetooth  dac  display  flash  gpio  hci  i2c  i2s  init  kws
mcu8080  media  network  pinmux  pm  pwm  qspi  register  rgb  rtc
security  spi  storage  system  timer  uart  utilities  vad  wakeup
watchdog  wifi  wired
```

**移植一颗新芯片，本质就是把这 33 个目录里你用得到的头文件逐个实现一遍**，实现放在 `platform/<芯片名>/tuyaos_adapter/`。

举例：网络。契约有 `tkl_net_socket_create`、`tkl_net_bind`、`tkl_net_send`。T5AI 的实现调 lwIP，Linux 的实现直接调 glibc 的 `socket()`。上层的 MQTT、HTTP、P2P 只认 `tkl_net_*`，所以一行不改就能跨平台。

**这层的价值在 P2P 移植里最明显**：pjproject 是个几万行的开源 ICE 库，原本直接调 BSD socket，改成调 TAL/TKL 后，同一份代码在 T5AI 的 lwIP 和 Linux 的 glibc 上都能跑。

### 2.3 TAL：系统能力层

TAL = Tuya Abstract Layer，在 TKL 之上做的好用封装，位于 `src/tal_*/`。

`src/tal_system/include/` 提供：

- **`tal_thread.h`**：线程创建与生命周期，比裸 `pthread_create` 多了名字、栈统计、状态机
- **`tal_mutex.h` / `tal_semaphore.h` / `tal_queue.h`**：互斥锁、信号量、消息队列
- **`tal_sw_timer.h`**：软件定时器。硬件定时器数量有限，软件定时器能开几十个
- **`tal_workqueue.h`**：工作队列，见第七章
- **`tal_event.h`**：发布订阅事件总线
- **`tal_log.h`**：分级日志
- **`tal_memory.h`**：`tal_malloc` / `tal_free`
- **`tal_fs.h` / `tal_ota.h` / `tal_time_service.h`**：文件系统、升级、时间

另有独立目录：`src/tal_kv/`（存储）、`src/tal_network/`、`src/tal_wifi/`、`src/tal_wired/`、`src/tal_bluetooth/`、`src/tal_security/`、`src/tal_cli/`。

**为什么不直接用 TKL？** TKL 是"够用就行"的最小契约，TAL 才是"人用着舒服"的接口。例如 `tal_thread_create_and_start` 自动登记线程名和栈深度：

```
[tal_thread.c:229] thread_create name:lan_sock_loop,stackDepth:4096,totalstackDepth:32768,priority:3
```

`totalstackDepth` 是累计值——**一行就知道整个系统吃掉了多少栈**。这类顺手的好处就是 TAL 存在的理由。

### 2.4 TDD 与 TDL：外设的两层

位于 `src/peripherals/`，覆盖：

```
audio_codecs  button  camera  display  encoder  imu  ir
joystick  led  leds_pixel  power  printer  tp  transport
```

每类外设拆成两层：

- **TDD**（Tuya Device Driver）：**具体某个型号**怎么驱动。GC2145 摄像头怎么初始化、SSD1306 屏怎么刷
- **TDL**（Tuya Device Layer）：**这类外设**统一长什么样。所有摄像头都得能"打开、设分辨率、给我帧"

两层之间是**注册–查找**范式，全仓统一：

```c
tdd_camera_v4l2_register("cam0", "/dev/video0");     // 板级：我这有个摄像头叫 cam0
TDL_CAMERA_HANDLE_T h = tdl_camera_find_dev("cam0"); // 应用：给我叫 cam0 的摄像头
tdl_camera_dev_open(h, &cfg);
```

全仓的 register/find 家族：

```
tdd_audio_alsa_register       tdd_gpio_button_register   tdd_led_pwm_register
tdd_disp_spi_device_register  tdd_ir_driver_register     tdd_printer_uart_register
tdl_camera_find_dev  tdl_disp_find_dev  tdl_led_find_dev  tdl_tp_find_dev
```

**这个范式解决了什么？** 应用层写"我要一个摄像头"而不是"我要 GC2145"。换型号只改板级那一行 register，应用一个字不动。

**注册在哪发生？** 板级的 `board_register_hardware()`，例如 `boards/LINUX/TaishanPi_3/board_com_api.c:268`。板上焊了什么就在这注册什么。

### 2.5 业务层

`src/tuya_cloud_service/`：

- **`authorize/`**：读取设备授权（UUID、authkey）
- **`cloud/`**：设备激活、MQTT 服务、IoT 状态机，最核心
- **`lan/`**：局域网直连协议，见第十一章
- **`netcfg/`**：配网
- **`netmgr/`**：网络连接管理与重连
- **`protocol/`**：LPV3.5 帧的序列化与解析
- **`schema/`**：DP 定义与校验
- **`tls/`** / **`transport/`**：加密与传输通道
- **`file_storage/`** / **`weather/`**：文件上传、天气

`src/tuya_ai_service/` 与 `src/ai_components/`：AI 会话协议栈、播放器、UI，见第十三章。

`src/tuya_p2p/`：P2P 音视频，含 pjproject，见第十二章。

### 2.6 库与可选组件

`src/` 下还有一批第三方或可选库，按 Kconfig 启用：`libcjson`（JSON）、`liblwip`（TCP/IP 协议栈，MCU 平台用）、`libtls`（mbedtls）、`libmqtt`、`libhttp`、`liblvgl`（GUI）、`libjpegturbo`、`libu8g2`（单色屏）、`micropython`、`audio_player`、`image_album`。

## 第三章　跨平台是怎么做到的

本章技术密度最高，建议对着代码读。

### 3.1 八个平台，两种下载方式

`platform/platform_config.yaml` 登记了所有支持的平台，每个都是**独立 Git 仓库**，构建时按需下载：

- **T2** —— TuyaOpen-T2
- **T3** —— TuyaOpen-T3
- **T5AI** —— TuyaOpen-T5AI，BK7258 双核
- **ESP32** —— TuyaOpen-esp32，覆盖 ESP32/C3/C6/S3/P4
- **LINUX** —— TuyaOpen-ubuntu
- **LN882H** / **BK7231X** / **GD32**

**注意**：本地 `platform/` 目录只会有你编译过的平台。没编译过的平台代码不在仓库里，所以想读某平台适配层源码，得先构建一次或手动 clone。

### 3.2 Kconfig：配置怎么变成宏

机制借鉴自 Linux 内核，四步：

**第一步，收集。** 各层各自写 `Kconfig` 声明可配项。`boards/LINUX/Kconfig` 用 `choice` 列出所有 Linux 板；`src/peripherals/Kconfig` 声明外设开关。

**第二步，汇总。** 构建时把散落各处的 Kconfig 拼成总目录 `CatalogKconfig`（顶层 `CMakeLists.txt:97`）。

**第三步，求解。** kconfiglib 读入你的 `.config` 选择加各 Kconfig 的依赖规则（`select`、`depends on`、`default`），算出**完整**配置，写到 `.build/cache/using.config`。

**第四步，落地成两种产物：**

- `using.config` → 生成 C 头文件 `tuya_kconfig.h`（`CMakeLists.txt:95`），代码里 `#if defined(ENABLE_TUYA_P2P)` 用的就是它
- `using.cmake` → 被顶层 CMake `include`（`CMakeLists.txt:100`），控制哪些源文件参与编译

**实测踩过的坑**：`using.config` 是缓存，改了 `app_default.config` 不会自动重算，`tos.py clean` 也不清它。必须 `rm -rf .build/cache` 再 build，否则改了板型毫无效果。

### 3.3 CMake 怎么组装

顶层 `CMakeLists.txt:123` 有个反直觉设计：

```cmake
list_components(COMPONENT_LIST "${TOP_SOURCE_DIR}/src")
foreach(comp ${COMPONENT_LIST})
    add_subdirectory("${TOP_SOURCE_DIR}/src/${comp}")
endforeach(comp)
```

`list_components`（`tools/cmake/util.cmake:9`）的逻辑是：**只要 `src/` 下某目录里有 `CMakeLists.txt` 就无条件加进来。**

也就是说 SDK **不做"挑选组件"**，`src/` 下 29 个模块全部进入构建。没启用的怎么办？——**每个组件在自己的 CMakeLists 里自我裁剪**：读 Kconfig 变量，没开就不产出源文件。

**好处**：加新模块只要建目录加写 CMakeLists，不用改顶层任何清单。**代价**：配置错了不会报"找不到组件"，而是安静地少编一个库。

板级则是精确加载（`CMakeLists.txt:128`）：

```cmake
add_subdirectory("${TOP_SOURCE_DIR}/boards/${TOS_PROJECT_PLATFORM}/${TOS_PROJECT_BOARD}")
```

**只加载被选中的那一个板子目录。** 想让多块板共用驱动，得在各板 CMakeLists 里显式引入公共目录。

### 3.4 一个正反对照：共享驱动的两种写法

**ESP32 做对了。** `boards/ESP32/common/` 下有 audio、button、camera、io_expander、lcd、led、tp 七类共享驱动，`common/CMakeLists.txt` 还按芯片过滤源文件：

```cmake
# *_dpi.c 用 MIPI-DSI，只有 ESP32-P4 有
if(NOT IDF_TARGET STREQUAL "esp32p4")
    list(FILTER LCD_SRCS EXCLUDE REGEX ".*_dpi\\.c$")
endif()
# *_rgb.c 用 RGB LCD，只有 S3 和 P4 有
if(NOT IDF_TARGET STREQUAL "esp32s3" AND NOT IDF_TARGET STREQUAL "esp32p4")
    list(FILTER LCD_SRCS EXCLUDE REGEX ".*_rgb\\.c$")
endif()
```

**一份代码，按芯片能力裁剪**，这是正确姿势。

**Linux 做错了。** `boards/LINUX/Raspberry_Pi/tdd_camera_v4l2.c` 和 `boards/LINUX/TaishanPi_3/tdd_camera_v4l2.c` **逐字节完全相同**，各 334 行，纯复制粘贴。改一个 bug 要改两处，且必然漏。

复制粘贴还带来了第二个错误。ESP32 的模块名是自动推导的（`boards/ESP32/DNESP32S3/CMakeLists.txt:10`）：

```cmake
get_filename_component(MODULE_NAME ${MODULE_PATH} NAME)
```

而三块 Linux 板全是硬编码，其中 `boards/LINUX/DshanPi_A1/CMakeLists.txt:10` 写的是：

```cmake
set(MODULE_NAME "Raspberry_Pi")   # 从树莓派复制过来忘了改
```

编东山派 A1 会产出一个名叫 Raspberry_Pi 的库。**能自动推导就别手写**——这是个很小但很典型的教训。

### 3.5 tos.py build 的完整流程

`tools/cli_command/cli_build.py` 里的链条：

1. **`env_check()`** —— 检查 cmake / ninja
2. **`get_platform_info()` / `download_platform()`** —— 平台是独立仓库，按需下载
3. **`check_platform_commit()`** —— 校验平台版本与 SDK 要求是否一致
4. **`prepare_platform()`** —— 执行平台自己的 `platform_prepare.py`
5. **`init_using_config()`** —— 生成 `using.config` / `using.cmake` / `tuya_kconfig.h`
6. **`cmake_configure()`** —— `cmake -G Ninja`
7. **`ninja_build()`** —— 真正编译
8. **`check_bin_file()`** —— 打包固件、生成 OTA 文件

**两条实用经验：**

**第三步会卡交互。** 提示 `Update the platform to the required commit?` 时脚本停下等输入。自动化脚本里要 `echo "n" | tos.py build` 喂进去，否则挂死。

**必须在应用目录下执行。** 在仓库根跑会报 `TuyaOpen root cannot be regarded as project root`。

### 3.6 实战：新增一块板

以 Linux 平台为例：

1. 建目录 `boards/LINUX/<新板名>/`
2. 写 `Kconfig`：声明 `CHIP_CHOICE`、`BOARD_CHOICE` 及板子特有配置项
3. 在 `boards/LINUX/Kconfig` 的 `choice` 块里加一项指向它
4. 写 `CMakeLists.txt`，模块名用 `get_filename_component` 自动推导
5. 写 `board_com_api.c`，实现 `board_register_hardware()`，逐个 register 板上器件

## 第四章　硬件平台详解

三大平台的硬件模型差异极大。这章分别讲。

### 4.1 T5AI（BK7258）：双核 MCU

#### 为什么有两颗核

BK7258 是双核，可以想成公司两个部门：

- **CP（Co-Processor，协处理核）**：让设备**能连上世界**。Wi-Fi、BLE、射频、底层协议栈、电源协同。人话是"网管加射频工程师"。固件住在 Flash `0x00022000` 起约 1088KB
- **AP（Application Processor，应用核）**：连上之后**干什么**。业务逻辑、UI、云协议、AI、DP。人话是"产品经理加应用开发"。固件住在 `0x00132000` 起约 3808KB，**你的应用跑在这里**

启动顺序固定：**先 CP，再由 CP 把 AP 拉起来**。改业务逻辑改 AP 侧；Wi-Fi 驱动诡异、射频校准才碰 CP。

#### 三种存储

**SRAM，640KB，基址 `0x28000000`。** 芯片内部，快但小。切成 AP 自旋锁区、AP 通用区（OS、小对象、**必须放片内的线程栈**）、CP 通用区、极小的电源管理区。

**为什么主线程栈强调放片内？** 有些操作（Flash 映射、部分音频前端）要求当前任务的栈在"cache 关闭时仍合法"的片内区域。栈若在 PSRAM 可能触发断言。所以**主应用线程用 SRAM 栈，大缓冲用 PSRAM**。

**PSRAM，16MB，基址 `0x60000000`。** 外挂伪静态 RAM，慢一点但大得多。最重要的是 **AP 的 PSRAM 堆约 8.6MB**：对话缓冲、JSON、大块音频、多数业务 malloc 都在这。显示、编码、音频还各有按用途预留的大块（slab）。开了扩展内存时，cJSON 可配置成从 PSRAM 申请，避免吃光 640KB SRAM。

**Flash，8MB，`0x00000000` 到 `0x00800000`。** 掉电不丢，装启动程序、双核固件、升级包、配置、校准。物理布局：

- **`0x00000000`，68KB，一阶 Bootloader** —— 芯片复位后最先跑的引导员
- **`0x00011000`，68KB，tuyaboot** —— 涂鸦二阶引导，决定用不用新固件、启动谁
- **`0x00022000`，1088KB，CP 应用** —— CP 整包固件
- **`0x00132000`，3808KB，AP 应用** —— 含你的应用的 AP 整包
- **`0x004ea000`，2940KB，ota** —— 下载新固件的暂存货架
- **`0x007c9000`，8KB，ota_mgr** —— 升级状态与指针
- **`0x007cb000`，8KB，usr_config** —— 平台/用户配置
- **`0x007cd000`，196KB，tuya_data** —— 涂鸦业务数据总槽
- **`0x007fe000`，4KB，sys_rf** —— 射频校准表
- **`0x007ff000`，4KB，sys_net** —— 快连等网络工厂数据

```
0x00000000 ████ bootloader
0x00011000 ████ tuyaboot
0x00022000 ████████████ CP
0x00132000 ████████████████████████████ AP ← 你的应用在这
0x004ea000 ████████████████████ ota 暂存
0x007c9000 ▏ ota_mgr
0x007cb000 ▏ usr_config
0x007cd000 ██ tuya_data
0x007fe000 ▏ rf
0x007ff000 ▏ net
0x00800000 结束
```

`tuya_data` 内部再切逻辑区：

- **`0x7cd000`，4KB，KV 保护区**（可选）—— 更敏感的键值
- **`0x7ce000`，60KB，USER** —— 用户自定义区
- **`0x7dd000`，64KB，KV 类型映射区** —— 传统 KV 类型地址
- **`0x7ed000`，64KB，UF（User File）** —— **SDK 的 KV 实际挂在这里的小文件系统上**
- **`0x7fd000`，4KB，KV Key** —— 密钥相关

**UF 不是 UVC。** UVC 是 USB 摄像头协议；UF 是 Flash 里给文件式存储用的区域。音量、对话模式、复位计数最终都是"某个键到一段字节"写在 UF 上的 littlefs 里。

**关于 CRC 与虚拟地址**：部分镜像在 Flash 里按规则插入 CRC，物理占用约"长度×34/32"。OTA 与启动代码里会出现物理地址与虚拟地址换算。你只要知道**烧录工具和 boot 用物理布局，应用读逻辑区用类型查询接口**，别自己算偏移去写 ota 大区。

### 4.2 ESP32 系列：单核到双核，多芯片家族

**本节说明**：ESP32 的平台适配层（`TuyaOpen-esp32` 仓库）默认不在本地，所以这里只写本仓库能验证的内容，不编造 Flash 分区表。

SDK 支持的 ESP32 芯片（`boards/ESP32/Kconfig`）：

- **ESP32** —— 经典款，双核 Xtensa
- **ESP32-C3** —— 单核 RISC-V，低成本
- **ESP32-C6** —— 单核 RISC-V，带 Wi-Fi 6 与 802.15.4
- **ESP32-S3** —— 双核 Xtensa，带向量指令，AI 场景常用
- **ESP32-P4** —— 高性能，带 MIPI-DSI，常与 C6 搭配做无线（`ESP32-P4-C6`）

已适配的成品板包括 `DNESP32S3` 系列、`M5STACK_STICKS3`、`WAVESHARE` 的 C6/P4/S3 触摸屏板、`SEEED_ESP32S3_XIAO_SENSE`、`XINGZHI` OLED 板等。

**和 T5AI 的关键差异：**

**外设能力按芯片型号分化，构建期裁剪。** 前面 3.4 节引的那两段 `list(FILTER ...)` 就是例子：MIPI-DSI 驱动只给 P4 编，RGB LCD 驱动只给 S3 和 P4 编。**这是多芯片家族的标准做法**——共享目录加能力过滤，而不是每颗芯片一份拷贝。

**板级 Kconfig 直接声明引脚。** 例如 `boards/ESP32/ESP32-S3/Kconfig`：

```
config UART_NUM0_TX_PIN
    int "UART_NUM0_TX_PIN"
    default 43       # esp32-s3 default 43
config UART_NUM0_RX_PIN
    default 44
```

引脚是配置项而不是硬编码常量，换板改配置不改代码。

**底层是 ESP-IDF。** `tos.py` 有个专门的 `idf` 子命令用来透传 idf.py 命令，说明 ESP32 平台构建最终落到乐鑫自己的构建系统上。

### 4.3 Linux：用文件模拟一切

`platform/LINUX/tuyaos_adapter/src/` 实现了这些 TKL：

```
tkl_audio  tkl_bt  tkl_camera  tkl_display  tkl_flash.c  tkl_fs.c
tkl_gpio.c  tkl_i2c.c  tkl_jpeg_codec  tkl_memory.c  tkl_mutex.c
tkl_ota.c  tkl_output.c  tkl_pinmux.c  tkl_pwm.c  tkl_queue.c
tkl_rtc.c  tkl_semaphore.c  tkl_spi.c  tkl_system.c  tkl_thread.c
tkl_uart.c  tkl_watchdog.c  tkl_wifi  tkl_wired.c
```

**线程直接映射 pthread**（`tkl_thread.c`）：`tkl_thread_get_id()` 返回 `pthread_self()`。互斥锁、信号量同理。这就是 TKL 契约的价值——上层 `tal_thread_create_and_start` 在 MCU 上落到 FreeRTOS，在 Linux 上落到 pthread，调用方毫无感知。

**最有教学价值的是伪 Flash**（`tkl_flash.c`）：

```c
#define FLASH_FILE_PATH  "./tuyadb"
#define FLASH_FILE_NAME  "./tuyadb/tuyadb"
#define FLASH_FILE_SIZE  (256 * 1024 * 1024)   // 256MB
#define PARTITION_SIZE   (1 << 12)             // 4KB，擦除单位
```

**一个 256MB 的普通文件当 Flash 用**，但它**精确模拟了真实 Flash 的语义**：

- 擦除单位是 4KB 扇区，和真实 NOR Flash 一致
- 擦除的实现是 `memset(data, 0xff, PARTITION_SIZE)`——**真实 Flash 擦除后就是全 1**，这个细节没有偷懒
- 分区布局也照搬：SIMPLE_FLASH 32KB → UF 分区 96KB → RCD 文件区 100KB → 可选的 KV 保护区 4KB

**为什么要这么较真？** 因为上层的 littlefs 是按真实 Flash 语义写的：它假设"只能把 1 写成 0，要把 0 变回 1 必须整块擦除"。如果 Linux 这层用普通文件读写随便糊弄，littlefs 的磨损均衡和掉电恢复逻辑就跑不出真实行为，**Linux 上测过的存储代码到了 MCU 上会出问题**。

这是嵌入式跨平台仿真的一条通用原则：**仿真层要模拟的是语义，不是接口**。

**Linux 平台的定位**：主要用于开发调试和无线场景之外的产品（网关、边缘盒子）。`camera_demo` 的 `Ubuntu.config` 就关掉 Wi-Fi 开有线：

```
CONFIG_ENABLE_WIRED=y
# CONFIG_ENABLE_WIFI is not set
```

已适配板子：`Ubuntu`（纯软件）、`Raspberry_Pi`、`TaishanPi_3`（泰山派 3）、`DshanPi_A1`（东山派 A1）。

## 第五章　上电：从复位到应用主线程

本章以 T5AI 为例，其他平台的引导细节不同，但**从 `tuya_app_main` 之后完全一致**。

### 5.1 Boot 逐步（精确到地址）

1. **复位向量指向 `0x00000000` 一阶 Bootloader** —— 硬件规定的入口，做最基本初始化后跳下一阶段
2. **`0x00011000` tuyaboot** —— 读 `ota_mgr`（`0x007c9000`）判断上次升级是否成功、要不要切新镜像，然后决定启动 CP 镜像
3. **`0x00022000` CP 跑起来** —— 初始化芯片平台（时钟、中断、RTOS），初始化连接底座，**拉起 AP 核**
4. **`0x00132000` AP 跑起来** —— 再 init 一遍 AP 侧平台与多媒体服务；若射频校准丢失，尝试从 OTP 或 `sys_rf` 恢复；最后调用 **`tuya_app_main()`**，这是 SDK 的应用约定入口

### 5.2 tuya_app_main 做了什么

在 MCU 上它**不直接跑你的业务循环**，而是：

1. 配置一个线程：名字 `tuya_app_main`，栈约 4KB，优先级中等，**栈在片内**
2. 线程入口里调用 `user_main()`
3. 若 `user_main` 返回（正常设计是永不返回）则删除线程

**为什么要多包一层线程？** 平台 `main` 所在上下文不一定适合长时间阻塞；应用云循环需要独立的栈与调度。

**Linux 上则简单得多**（`apps/tuya_cloud/switch_demo/src/tuya_main.c:315`）：

```c
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else
    /* MCU：创建线程再跑 user_main */
#endif
```

### 5.3 user_main：应用人生大纲

按真实调用顺序，每一步的含义：

**（1）配置 JSON 用哪块堆。** 开了扩展内存就告诉 cJSON 以后用 PSRAM 申请释放。含义是解析云下发的大 JSON 时别把 SRAM 撑爆。

**（2）`tal_log_init`。** 设定日志级别、缓冲区、输出回调。之后所有 `PR_INFO`、`PR_ERR` 都从这出去。**小白调试第一技能就是看串口。**

**（3）打印版本横幅。** 工程名、版本、编译日期、SDK 版本、芯片与板型。方便现场确认烧的是不是这份固件。真机日志长这样：

```
[tuya_main.c:233] Project name:        switch_demo
[tuya_main.c:236] TuyaOpen version:    v1.9.0
[tuya_main.c:237] TuyaOpen commit-id:  4844c66a...
[tuya_main.c:238] Platform chip:       T5AI
[tuya_main.c:239] Platform board:      TUYA_T5AI_BOARD
```

**（4）`tal_kv_init`。** 用种子和密钥初始化，在 UF 区挂上 littlefs。之后可以 `tal_kv_set` / `tal_kv_get`，掉电仍在。

**（5）`tal_sw_timer_init`。** 创建后台线程 `sys_timer`。可以"N 毫秒后调用我的回调"，不必自己开线程 sleep。

**（6）`tal_workq_init`。** 创建 `wq_system`、`wq_highpri` 线程。把不能在中断或紧要回调里做的重活丢进队列，由专门线程慢慢做。

**（7）`tal_time_service_init` / `tal_cli_init`。** 时间服务与命令行。

**（8）`tuya_authorize_init` 加读授权。** 尝试读出 UUID 与 authkey。失败则用头文件里的占位字符串——**占位不能上生产**，云会拒绝。日志里会看到：

```
[tuya_main.c:260] Replace the TUYA_OPENSDK_UUID and TUYA_OPENSDK_AUTHKEY contents,
                  otherwise the demo cannot work.
```

**（9）`reset_netconfig_start`。** 启动连续重启计数，为"重启三次清配网"做准备。

**（10）`tuya_iot_init`。** 云客户端诞生。传入软件版本、PID、uuid、authkey、事件回调、网络探测函数。内部会做：清零并保存配置、`tuya_tls_init` 让 TLS 引擎就绪、注册中心与证书管理初始化、端点初始化（知道云域名从哪来）、**尝试从 KV 读激活数据**（读到且 schema 可用就标记 `is_activated`）、OTA 模块与健康监控初始化、状态设为 IDLE。

**此时还没连网也没 MQTT**，只是把身份证和工具箱备好。

**（11）网络栈与 netmgr。** 链了独立 lwIP 就初始化 TCP/IP；`netmgr_init(WIFI)` 管连接状态；`netmgr_conn_set` 声明启用哪些配网方式。

**（12）`board_register_hardware`。** 按板型注册麦克风、喇叭、按键、LED、屏。**应用认设备名，不认裸 GPIO 号。**

**（13）应用自身初始化。** 例如 AI 聊天子系统 `app_chat_bot_init`。注意此时通常**还不能真正连 AI 云会话**，要等 MQTT 连通后的回调。

**（14）`tuya_iot_start`。** 状态机从 IDLE 推到 START。真正的连网、激活、MQTT 在后面的 yield 循环里推进。

**（15）关掉 Wi-Fi 低功耗。** 对话类场景要稳定吞吐，避免省电导致卡顿。

**（16）`reset_netconfig_check`。** 看复位计数是否到阈值，到了就清配网数据。

**（17）死循环 `tuya_iot_yield`。** **这是设备活着的心跳。** 每一圈根据当前状态做一点事：检查网、激活、连 MQTT、收发包。**绝不能堵死这个循环太久**，否则云保活和消息都会停。

## 第六章　云状态机

把客户端想成自动售货机：每次 yield 看当前状态，做一步，设下一状态。

### 6.1 状态清单（按人生顺序）

- **IDLE** —— 睡一会，啥也不干
- **START** —— 出发。已激活就去检查网络，未激活就去加载或等待激活
- **DATA_LOAD** —— 确认激活数据，没有则进入等 token
- **TOKEN_PENDING** —— 等 App 配网给出 token，同时给应用发 BIND_START
- **NETWORK_CHECK** —— 调你传入的 `user_network_check`，看 Wi-Fi 是否 Link Up
- **ENDPOINT_GET / UPDATE** —— 向云问"我该连哪个机房"，更新本地端点
- **ACTIVATING** —— 用 token 调 ATOP 激活，结果写入 KV
- **STARTUP_UPDATE** —— 激活后刷新配置、准备 MQTT 参数
- **MQTT_CONNECT_START** —— `tuya_mqtt_start`，注册 DP、复位、升级等协议回调
- **MQTT_CONNECTING** —— 等 TLS 加 MQTT 握手完成
- **MQTT_YIELD** —— **日常态**，收发 MQTT、处理 MATOP
- **MQTT_RECONNECT / NETWORK_RECONNECT** —— 断线重连路径
- **RESET / RESTART / STOP / EXIT** —— 复位清理、重启、停止

### 6.2 两条路

**老设备（KV 里已有激活信息）**：START → 检查网络 → 拿端点 → 更新启动配置 → 连 MQTT → 长期 YIELD。

**新设备**：START → DATA_LOAD 发现没激活 → TOKEN_PENDING 等配网拿 token → 有网后更新端点 → ACTIVATING → 再走上电后半程 → MQTT。

### 6.3 应用事件回调：云在喊你

`user_event_handler_on` 里常见事件：

- **BIND_START** —— 开始绑定配网流程。典型反应是播提示音
- **DIRECT_MQTT_CONNECTED** —— 直连相关节点就绪，可打印绑定二维码
- **MQTT_CONNECTED** —— **业务 MQTT 已在线**。上传初始 DP、UI 显示网络正常、**AI 模块在此初始化 agent**
- **MQTT_DISCONNECT** —— 掉线，UI 可变灰
- **UPGRADE_NOTIFY** —— 云要你升级，后续 OTA 线程干活
- **TIMESTAMP_SYNC** —— 云给了 Unix 时间，发布给需要校时的模块
- **RESET / RESET_COMPLETE** —— 复位类型处理，完成后整机软件复位
- **DP_RECEIVE_OBJ / DP_RECEIVE_RAW** —— App 下发对象型或原始 DP

内部还会通过事件总线发 `EVENT_MQTT_CONNECTED`，模块订阅的是总线事件。

## 第七章　并发模型与线程百科

嵌入式最容易出事的地方。

### 7.1 线程

`tal_thread_create_and_start()` 要填名字、栈深度、优先级。日志打印累计栈占用，是排查"内存去哪了"的第一手线索。

**栈深度怎么定？** 没有公式，只能实测：先给大值跑通，再用平台工具看高水位往下压。给小了会栈溢出，症状是莫名其妙的 HardFault 或数据被踩。

### 7.2 工作队列：最该养成的习惯

**核心用途：把耗时的活从回调里挪走。**

为什么重要？网络数据到达时会触发回调，如果你在回调里写 flash、解析大 JSON，整个网络线程就被堵住，后续数据全部延迟，严重时丢包或断连。

正确做法是回调里只把任务丢进工作队列然后立刻返回，真正的活在工作队列线程里干。SDK 自己开了两条：

```
thread_create name:wq_system,stackDepth:6144
thread_create name:wq_highpri,stackDepth:5120
```

**回调里绝不干重活**——这是嵌入式开发最值钱的习惯之一。

### 7.3 事件总线

`src/tal_system/include/tal_event.h`：

```c
tal_event_publish(const char *name, void *data);
tal_event_subscribe(const char *name, const char *desc, EVENT_SUBSCRIBE_CB cb, SUBSCRIBE_TYPE_E type);
```

**事件名是字符串**，集中定义在 `tal_event_info.h`：

```c
#define EVENT_MQTT_CONNECTED    "mqtt.con"      // MQTT 连上了
#define EVENT_MQTT_DISCONNECTED "mqtt.disc"     // MQTT 断了
#define EVENT_LINK_STATUS_CHG   "link.status"   // 网络状态变了
#define EVENT_LINK_ACTIVATE     "link.activate" // 拿到激活信息
#define EVENT_RESET             "dev.reset"     // 设备被重置
#define EVENT_TIME_SYNC         "time.sync"     // 时间同步完成
#define EVENT_LAN_CLIENT_CLOSE  "lan.cli.close" // 局域网客户端断开
```

**用字符串而非枚举，好处是解耦**：发布者和订阅者不需要共享枚举，新增事件不引起全仓重编。**代价是编译期不检查**，名字打错不会报错，只会静默地永远收不到。

`SUBSCRIBE_TYPE_ONETIME` 表示触发一次自动退订，适合"等激活完成"这种一次性等待。

### 7.4 定时器

`tal_sw_timer.h`。基于一个硬件定时器加链表实现，可以开很多个。**定时器回调同样运行在定时器线程里，同样不能干重活。**

### 7.5 线程百科：谁全天在干什么

把设备想成车间，每个线程是一名工人。

**一上电就在的：**

- **RTOS 空闲与系统线程** —— 调度、滴答
- **`sys_timer`** —— 到点执行软件定时器回调
- **`wq_system` / `wq_highpri`** —— 消化投递来的延后重活
- **`tuya_app_main`** —— 跑 `user_main`，核心是不停 yield 云状态机
- **`cli`** —— 串口命令行（启用时）
- **`health_monitor`** —— 健康检查与喂狗

**网络与云相关（按需出现）：**

- **`ap_cfg_task`** —— AP 热点配网会话
- **`mqtt_bind`** —— 绑定阶段取 token
- **`lan_sock_loop`** —— 局域网协议循环
- **`tuya_ota`** —— 按 URL 下载固件写入 ota 区

**AI、音频、UI（AI 类产品才有）：**

- **`ai_chat_mode`** —— 每 20ms 跑对话模式状态机
- **`record_task`** —— 录音加 VAD
- **`ai_player`** —— TTS、提示音、音乐播放流水线
- **`ai_client`** —— AI 云连接与鉴权状态机
- **`ai_biz_thread`** —— AI 业务与技能处理
- **`ai_agent_input` / `ai_agent_output`** —— 上行送云、下行拆给播放与 UI
- **`ai_ui` / `ai_ui_action` / `app_ui_msg`** —— 界面

**优先级直觉**：录音、播放、云 yield 不能被慢 UI 长期饿死；模式轮询线程可以低优先级。调参前先抓"卡顿时谁占 CPU"。

## 第八章　存储

### 8.1 tal_kv 与 littlefs

`src/tal_kv/` 底层是 **littlefs**（专为 flash 设计的掉电安全文件系统）外加 FlashDB。

**为什么不直接写 flash 地址？** 三个原因：

- **磨损均衡**：flash 每块擦写次数有限（典型 10 万次），littlefs 自动把写入分散，避免某块被写穿
- **掉电安全**：写到一半断电能回滚到上一个完整状态，不留半截数据
- **不用自己管布局**：直接按 key 存取

日志里 `lfs key not found: UUID_TUYAOPEN` **不是错误**，只是"这个 key 还没写过"。

### 8.2 三个平台的存储落点对比

- **T5AI** —— 真实 8MB SPI NOR Flash，KV 挂在 `0x7ed000` 起 64KB 的 UF 区
- **Linux** —— 256MB 普通文件 `./tuyadb/tuyadb` 模拟，4KB 扇区语义，见 4.3 节
- **ESP32** —— 由 ESP-IDF 的分区表管理（平台层未下载，不展开）

### 8.3 常用 key

- **`<uuid>`** —— 激活记录 JSON，含 devId、localKey、secKey、schemaId
- **`<uuid>.devid`** —— 单独存的 devId
- **`rst_cnt`** —— 复位计数，用于"重启三次清配网"
- **`device_timer_tasks`** —— 云下发的定时任务
- **`tuya_seed`** —— KV 加密种子

用串口 CLI 可以直接读：`kv get <你的uuid>`。

## 第九章　设备与云

### 9.1 激活流程

设备第一次上电并不知道自己是谁的：

1. **配网** —— 手机 App 把 Wi-Fi 账号密码和一个 **token** 交给设备（token 是 App 从云端申请的一次性凭证）
2. **联网** —— 设备连上 Wi-Fi
3. **激活** —— 设备拿 UUID + authkey + PID + token 走 ATOP（HTTPS 加密）向云换身份
4. **落盘** —— 云端返回 JSON，写进 KV

存的内容：

```json
{"capability":1025,"devId":"6c881d0227...","localKey":"n7D;;Pi)41[R9cp;",
 "schemaId":"000002qtv8","secKey":"...","stdTimeZone":"+08:00"}
```

**存在哪个 key 下？** 默认就是 UUID（`src/tuya_cloud_service/cloud/tuya_iot.c:612`）：

```c
/* Default storage namespace */
if (client->config.storage_namespace == NULL) {
    client->config.storage_namespace = client->config.uuid;
}
```

**三把密钥的分工**（理解后面所有加密的基础）：

- **localKey** —— 局域网通信用，16 字节。**只有云、App、设备三方知道**，这是 LAN 直连能保证安全的根本
- **secKey** —— MQTT 报文加密用
- **schemaId** —— 指向这个产品的 DP 定义

### 9.2 配网的几种方式

**为什么要配网？** 芯片出厂不知道你家路由器密码，必须有安全方式把 SSID、密码、绑定 token 交给设备。

- **BLE 配网** —— 手机经蓝牙把凭证传给设备，设备再去连 Wi-Fi。体验最快
- **AP 配网** —— 设备自己开热点（可带 PIN 码校验），手机连上热点后把 Wi-Fi 信息传下来。会起 `ap_cfg_task` 线程伺候
- **有线** —— Linux 网关类产品直接走 `ENABLE_WIRED`，不需要配网

几种可并行注册，实际走哪条由 App 与现场决定。

**重启三次清配网**：KV 里存复位计数 `rst_cnt`，每次上电早期加一，正常跑一段时间后清零。计数达到阈值就判定用户在狂按复位，清除配网与激活数据回到出厂态。

### 9.3 MQTT 通道

激活后建立 MQTT over TLS 长连接。订阅 `smart/device/in/<devId>`，上行发到对应 out 主题。

收到消息进入分发器（`tuya_mqtt_dispatch.c`），按报文里的 `reqType` 路由：

```
mqtt dispatch >> reqType=timer_sync taskId=1786502281332
mqtt dispatch >> matched: timer_sync (device timer sync)
mqtt dispatch << done
```

**这个分发表是可注册的**，模块用 `protocol_id` 登记自己关心的报文类型——和 2.4 节的 register 范式是同一个思路。

### 9.4 DP：设备和云的共同语言

DP = Data Point。云、App、设备三方约定：**每个功能是一个编号加一个类型**。

开关产品的 schema 简单到只有一条：

```
schema_json [{"mode":"rw","property":{"type":"bool"},"id":1,"type":"obj"}]
```

1 号 DP，布尔型，可读可写。于是"开灯"在全链路里就是 `{"1":true}`：

```
[tuya_main.c:151] idx:0 dpid:1 type:0 ts:1786502289
[tuya_main.c:154] bool value:1
[dp_schema.c:1009] dp rept out: {"1":true}
```

**为什么不直接传 "turn_on"？** 编号省流量、类型可校验、与语言无关。`src/tuya_cloud_service/schema/` 负责按 schema 校验：类型不对、DP 不存在、只读 DP 被写，都在这层挡掉。

### 9.5 TLS

**没有 TLS 会怎样？** 中间人可窃听 token 与业务流量，甚至伪造云端。IoT 必须加密。

- `tuya_tls_init` 准备加密引擎（基于 mbedtls）
- 模式可有 PSK、校验证书、双向证书
- 证书管理负责装 CA、处理过期
- **MQTT** 与 **HTTPS**（ATOP 激活、TTS 拉流、OTA 下载）都走带 TLS 的传输封装

真机日志能看到握手结果：

```
TUYA_TLS Success Connect m2.tuyacn.com:8883 Suit:TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256
```

### 9.6 OTA

- 云通过 MQTT 下发升级通知，带版本、大小、MD5、URL
- OTA 线程下载写入 **ota 大区**（T5AI 上是 `0x004ea000`），管理信息进 **ota_mgr**
- 校验通过后重启，**tuyaboot 读 ota_mgr 决定切到新镜像**
- 失败则保留旧固件，避免变砖

### 9.7 时间与安全

**时间**：云下发时间戳后，依赖绝对时间的模块（日志、证书校验、定时任务）才能正常工作。

**安全三条铁律**：不要把 authkey 打进日志或提交到公开仓库；生产环境不要关 TLS 校验；复位清数据要按复位类型处理，避免"假复位"残留绑定。

## 第十章　外设抽象实战

用摄像头串一遍四层，这是全 SDK 最完整的一条外设链路。

### 10.1 TDL 定义了什么

`src/peripherals/camera/tdl_camera/include/tdl_camera_manage.h`：

```c
typedef enum {
    TDL_CAMERA_FMT_YUV422 = 1,
    TDL_CAMERA_FMT_JPEG   = ENCODED_SHIFT(1),
    TDL_CAMERA_FMT_H264   = ENCODED_SHIFT(2),
} TDL_CAMERA_FMT_E;

typedef struct {
    uint16_t fps, width, height;
    TDL_CAMERA_FMT_E out_fmt;
    TDL_CAMERA_GET_FRAME_CB get_frame_cb;          // 原始帧
    TDL_CAMERA_GET_FRAME_CB get_encoded_frame_cb;  // 编码帧
} TDL_CAMERA_CFG_T;
```

**注意两个回调**：一路原始 YUV 给本地显示或算法，一路编码后的 H264/JPEG 给网络传输。同一次采集可以同时出两种。

### 10.2 应用怎么用

`apps/tuya_cloud/camera_demo/src/tuya_ipc_demo.c` 只有四步：

```c
s_cam = tdl_camera_find_dev(CAMERA_NAME);
cfg.out_fmt = TDL_CAMERA_FMT_H264;
cfg.get_encoded_frame_cb = __demo_encoded_frame_cb;
tdl_camera_dev_open(s_cam, &cfg);
```

编码帧回调把帧塞进环形缓冲，P2P 线程再从环形缓冲取。**中间加一层环形缓冲是关键**：采集和发送速率不一致，直接对接会互相拖累。

**这段代码里没有任何芯片相关内容**，所以它在 T5AI 和 Linux 上是同一份。

### 10.3 板级怎么接

以 Linux 的 V4L2 摄像头为例：

```
tkl_camera_v4l2_*        平台层：ioctl 操作 /dev/videoX
        ↑
tdd_camera_v4l2.c        驱动层：把 V4L2 数据整理成 TDL 帧格式
        ↑  tdd_camera_v4l2_register("cam0", "/dev/video0")
board_com_api.c          板级：启动时注册
```

**给一块板加摄像头，就是在 `board_register_hardware()` 里加一行 register。**

### 10.4 一个重要的设计原则

现有 V4L2 驱动有一行值得玩味（`boards/LINUX/TaishanPi_3/tdd_camera_v4l2.c:175`）：

```c
if (cfg->out_fmt == TDL_CAMERA_FMT_H264 || cfg->out_fmt == TDL_CAMERA_FMT_H264_YUV422_BOTH) {
    PR_ERR("V4L2 camera H264 output not supported");
```

**这说明契约和实现可以不对齐**：TDL 定义了 H264 能力，但某个 TDD 可以不实现。这不是缺陷，是分层的正常状态——**接口定义"可能有什么"，实现声明"我有什么"**。

所以 TDL 提供了能力查询：`TDL_CAMERA_DEV_INFO_T.supported_fmts` 是位掩码，应用应该**先查询再决定**而不是假设。**能力协商优于假设**——这是写健壮驱动的通用原则。

## 第十一章　局域网直连（LAN）

手机和设备在同一 Wi-Fi 下时没必要绕云端。看完这章你能自己写一个客户端。

### 11.1 两个端口

- **TCP 6668** —— 设备监听，App 连上来下发命令、查询状态
- **UDP 7000** —— 广播发现

依据在 `src/tuya_cloud_service/lan/tuya_lan.c:35`。

### 11.2 LPV3.5 帧格式（逐字节）

实现在 `src/tuya_cloud_service/protocol/tuya_protocol.c`。一帧：

```
+--------+------------------+----------+-----------+--------+--------+
| HEAD 4 |      AD 14       | NONCE 12 |  密文 N   | TAG 16 | TAIL 4 |
+--------+------------------+----------+-----------+--------+--------+
 00006699                                                    00009966
```

**AD 那 14 字节的结构**（`tuya_protocol.h:67`）：

- 第 1 字节：高 4 位版本号，低 4 位保留
- 第 2 字节：保留
- 第 3 到 6 字节：序列号，大端
- 第 7 到 10 字节：帧类型，大端
- 第 11 到 14 字节：长度，大端，值等于 12 加明文长度加 16

**加密方式是 AES-128-GCM。** 妙处在于 **AD 这 14 字节同时作为 GCM 的附加认证数据**——帧头被篡改（比如改帧类型）会导致解密失败，头和体是绑定的。

**一个容易踩的细节**：设备回给 App 的帧，明文开头多 4 字节 `ret_code`（`tuya_lan.c:474`）；App 发给设备的帧没有。**方向不对称**，写客户端时要注意。

### 11.3 握手与会话密钥

App 连上 6668 后先做三步握手，全部用 **localKey** 加密：

1. **App 到设备，帧类型 0x03** —— 随机数 randA（16 字节）
2. **设备到 App，帧类型 0x04** —— ret_code(4) + randB(16) + HMAC-SHA256(localKey, randA)(32)
3. **App 到设备，帧类型 0x05** —— HMAC-SHA256(localKey, randB)

双方各自推导会话密钥：

```
session_key = AES-128-GCM(key=localKey, nonce=randA前12字节, 明文=randA XOR randB) 取前16字节
```

**这个设计好在哪？** 双方都贡献随机数，会话密钥每次连接都不同，录下整个会话也无法重放到下一次。而且**握手证明了双方都持有 localKey**，而 localKey 只有云端授权过的设备和 App 才有。

握手后所有帧改用会话密钥。密钥选择逻辑在 `tuya_lan.c:1026`：帧类型是 0x03/0x04/0x05 用 localKey，其余用会话密钥。

### 11.4 帧类型分发与扩展点

握手完成后所有帧进入 `lan_protocol_process()` 的 `switch (frame->type)`：

- **0x07 / 0x0d** —— 下发 DP 命令
- **0x0a / 0x10** —— 查询设备状态
- **0x03 / 0x05** —— 握手
- **0x20** —— P2P 信令
- **其他** —— 走扩展注册表

**扩展机制**：

```c
int tuya_lan_register_cb(uint32_t frame_type, lan_cmd_handler_cb handler);
int tuya_lan_unregister_cb(uint32_t frame_type);
```

业务模块可认领某个帧类型。回调返回的应答体由 LAN 层负责发送和释放（**内存所有权移交**），返回 NULL 且成功时 LAN 层仍会回一个空 ACK——**因为 App 端在等应答，不回会被当成超时并重试**。

### 11.5 动手验证

理解协议最快的方法是自己发一帧：

1. 从设备读出 localKey（串口 CLI `kv get <你的uuid>`）
2. TCP 连到设备 6668
3. 按 11.2 组帧，走完 11.3 握手
4. 发一个 0x0a 查询帧，明文放 `{"gwId":"","devId":""}`

设备会回 `{"dps":{"1":false},"devId":"..."}`，同时日志打印每帧的分发：

```
[tuya_lan.c:742] Process Data. FD:3, Num:3, Type:10, Len:22
[tuya_lan.c:879] Send Query To App:{"dps":{"1":false},...}
```

**这是验证 LAN 层改动最硬的证据**，比在手机 App 上点来点去可控得多——App 是否走局域网取决于它自己的选路策略，而你的客户端百分之百走 LAN。

## 第十二章　P2P 音视频

摄像头类产品用 P2P 传视频。这是 SDK 里最复杂的子系统，`src/tuya_p2p/` 下光非 pjproject 的源文件就有 84 个。本章按"从上到下"拆完整条链路。

### 12.1 模块地图

`src/tuya_p2p/` 的六个部分：

- **`svc_ipc_core/`** —— SDK 入口。`TUYA_APP_Start` 在这，负责填参数、初始化、起监听
- **`base_ice/`** —— 核心。信令队列、SDP、ICE 封装、**KCP 可靠传输**、内存池
- **`svc_streaming_p2p/`** —— 媒体会话。监听接入、每会话线程、RTP 打包、背压控制
- **`lib_rtp/`** —— 完整 RTP/RTCP 栈，64 个源文件。打包器覆盖 H264、H265、H266、AV1、VP8、VP9、AAC、MPEG4 等
- **`svc_ring_buffer/`、`local_store/`、`ipc_cloud_store/`** —— 缓冲与录像存储
- **`pjproject/`** —— pjlib 加 pjnath，提供 ICE、STUN、TURN

### 12.2 信令和媒体是两回事

**这是最容易搞混的一点。**

- **信令** —— 协商用的控制消息（我在哪、支持什么编码、密钥是什么），数据量极小
- **媒体** —— 真正的音视频流，数据量极大

信令有两条入口，**汇聚到同一个队列**：

```c
// MQTT 路径：云端下发 → svc_ipc_core/src/tuya_p2p_sdk.c:144
void gw_p2p_mqtt_data_cb(cJSON *root_json) {
    sendBuff = cJSON_PrintUnformatted(root_json);
    tuya_p2p_rtc_set_signaling(NULL, sendBuff, msg_len);
}

// LAN 路径：App 局域网直连 → 同一个函数
tuya_lan_register_cb(FRM_LAN_P2P_SIGNAL, ipc_lan_cmd_cb);   // 0x20
```

而 `tuya_p2p_rtc_set_signaling`（`base_ice/src/tuya_media_service_rtc.c:1007`）**只做一件事：入队**。

```c
/* Align OS: only enqueue; worker runs tuya_p2p_process_signal_msg */
ret = bc_msg_queue_push_back(s_msg_queue_incoming, CTX_SIG_MSG_TYPE_INCOMING, msg, (int)msglen);
```

**这是第七章"回调里不干重活"原则的教科书级实例。** 信令回调可能跑在 MQTT 线程或 LAN 线程上，ICE 协商涉及网络往返、可能耗时数秒。如果在回调里直接做，MQTT 心跳会断、LAN 会话会超时。所以无论从哪进来，一律入队，由专门的 worker 线程消费。

**媒体流的路由由 ICE 协商决定，和信令走哪条没有必然关系。** "信令走了云端"不代表"视频也走云端"。

### 12.3 传输栈：五层叠起来

这是本章最值得理解的部分。从下往上：

```
UDP                    最底层，不可靠、无序
  ↑
ICE (pjnath)           打洞与选路，产出一条能通的 UDP 通道
  ↑
KCP (ikcp.c)           在不可靠 UDP 上做可靠重传（ARQ）
  ↑
通道复用 (6 条)         命令/视频/音频/回放/相册 各走各的
  ↑
RTP (lib_rtp)          媒体分片打包、时间戳、序列号
  ↑
H264 / H265 / G711     编码后的裸流
```

**为什么 UDP 之上还要 KCP？** TCP 的重传和拥塞控制是为吞吐优化的，一旦丢包就会大幅降速并等待重排，对实时视频是灾难。KCP 是"用带宽换延迟"的 ARQ 协议：更激进的快速重传、更小的等待窗口。

配置在 `tuya_media_service_rtc.c:1911` 附近：

```c
chan->kcp = ikcp_create(channel_id, chan);
ikcp_wndsize(chan->kcp, send_buf_size / 1600, ...);
ikcp_nodelay(chan->kcp, 0, 5, 20, g_options.preconnect_enable ? 0 : 1);
```

`ikcp_nodelay` 的四个参数含义：nodelay=0、**内部时钟间隔 5ms**、**快速重传阈值 20**、最后一个是"是否关闭流控"。窗口大小按发送缓冲除以 1600 字节算。

**每条通道一个独立 KCP 实例**——视频卡顿不会拖累音频和命令通道。

### 12.4 六条通道

定义在 `svc_streaming_p2p/include/tuya_ipc_media_stream_common.h:16`：

- **0 `TUYA_CMD_CHANNEL`** —— 命令通道，App 的所有控制指令走这
- **1 `TUYA_VDATA_CHANNEL`** —— 视频数据
- **2 `TUYA_ADATA_CHANNEL`** —— 音频数据
- **3 `TUYA_TRANS_CHANNEL`** —— 回放视频下载
- **4 `TUYA_TRANS_CHANNEL4`** —— **已废弃**，注释写明 App 侧被别的功能占用了
- **5 `TUYA_TRANS_CHANNEL5`** —— 相册下载

IPC 产品用 6 条，XVR（录像机）产品编译时会变成 200 条。

命令通道上跑的是 `MEDIA_STREAM_*` 枚举（`tuya_ipc_media_stream_event.h`）：对讲开始、回放按时间查询、回放开始暂停继续、倍速、静音、删除录像、能力查询等几十条。

### 12.5 缓冲区怎么定大小（一个真实故障）

`svc_ipc_core/src/tuya_p2p_sdk.c:213`：

```c
uint32_t vsend = (bitrate * 1024u / 8u) * TUYA_P2P_SEND_BUFFER_SECONDS;  // 码率 × 4 秒
if (vsend > TUYA_P2P_SEND_BUFFER_SIZE_MAX) {        // 上限 800KB
    vsend = TUYA_P2P_SEND_BUFFER_SIZE_MAX;
} else if (vsend < TUYA_P2P_SEND_BUFFER_SIZE_MIN) { // 下限 500KB
    vsend = TUYA_P2P_SEND_BUFFER_SIZE_MIN;
}
```

**视频发送缓冲等于"码率乘以 4 秒"**，再夹在 500KB 到 800KB 之间。

代码上方的注释记录了一个真实故障：

```
It used to be passed as 0 here with send_buf_size hardcoded to 1.1 MB,
which is above the OS maximum and let the queue grow to about eight
seconds of video before anything was dropped.
```

硬编码 1.1MB 时，网络一慢队列就能堆到 **8 秒的视频**才开始丢帧——用户看到的就是"画面延迟好几秒且怎么都追不上"。

**这是实时流媒体的一条通用原则：发送缓冲不是越大越好。** 缓冲的作用是吸收抖动，超过这个限度就纯粹是在累积延迟。按"码率乘以可接受延迟秒数"来算才对。

### 12.6 发送循环：优先级、背压与丢帧策略

`svc_streaming_p2p/src/tuya_ipc_p2p.c:2015` 的 `__p2p_media_send_proc` 是媒体发送主循环，四条策略都很值得学。

**策略一：音频优先，且绝不被视频饿死。**

```c
/* Align TuyaOS push path: audio must not be starved by video sleep/backoff.
 * Drain uplink audio first (up to a few frames), then try one video frame. */
if (P2P_AUDIO & cmd) {
    for (a_burst = 0; a_burst < 4; a_burst++) { ... }   // 先连抽最多 4 帧音频
}
if (P2P_VIDEO & cmd) { ... }                            // 再试一帧视频
```

原因很实际：**人对音频断续的容忍度远低于视频卡顿**。画面卡一下能忍，声音断一下就像通话故障。

**策略二：拥塞时丢到下一个 I 帧。**

```c
buf_ret = __p2p_check_free_buffer_size(index, TUYA_VDATA_CHANNEL, (int)pMediaFrame->size);
if (buf_ret != OPRT_OK) {
    /* Align live preview: drop until next I-frame when send queue full */
    pSession->video_need_iframe = TRUE;
    ...
}
```

**为什么不是丢一帧就继续？** 因为 H264 的 P 帧依赖前面的帧。丢掉中间某个 P 帧后，后续 P 帧解出来全是花屏，直到下一个 I 帧才恢复。既然如此，**不如干脆丢到下一个 I 帧**，让接收端从干净的画面重新开始。这是视频传输的标准做法。

**策略三：退避时长看音频是否在跑。**

```c
backoff_ms = (P2P_AUDIO & cmd) ? 20 : 200;   // 缓冲满
backoff_ms = (P2P_AUDIO & cmd) ? 20 : 500;   // 发送失败
```

音频开着就只退 20ms，因为循环还要继续抽音频；纯视频场景才敢睡 200 到 500ms。

**策略四：按帧率控速，且取出的帧不丢。**

```c
video_fps = sg_p2p_session->av_Info.fps[0];
if (video_fps <= 0 || video_fps > 60) video_fps = 15;   // 兜底
pace_ms = 1000 / video_fps;
if (pace_ms < 20) pace_ms = 20;                          // 最快 50fps
tal_system_sleep(pace_ms);
```

另有 `video_frame_pending` 标志：从上层取出的帧如果这轮没发成功，**保留到下轮重试，不重新取**——避免上层的环形缓冲被无谓地推进。

背压检查本身留了 10% 余量（`tuya_ipc_p2p.c:658`）：

```c
int need_size = (int)((double)len * 1.1);
```

### 12.7 安全：两层加密

**第一层，DTLS 指纹。** 设备用 mbedtls 现场生成自签名证书（`base_ice/src/tuya_misc.c:223` 起，X.509 v3 加 SHA256），把证书指纹写进 SDP：

```
a=fingerprint:%s
a=setup:%s
```

对端拿指纹校验握手上来的证书，防止中间人替换。

**第二层，应用层 AES。** SDP 里还带一个十六进制编码的 AES 密钥（`base_ice/src/tuya_sdp.c:673` 的 `tuya_p2p_rtc_sdp_set_aes_key`）。真正的加解密由**应用注入的回调**完成：

```c
struct {
    tuya_p2p_rtc_aes_create_cb_t  on_create;
    tuya_p2p_rtc_aes_destroy_cb_t on_destroy;
    tuya_p2p_rtc_aes_encrypt_cb_t on_encrypt;
    tuya_p2p_rtc_aes_decrypt_cb_t on_decrypt;
} aes;
```

**为什么做成回调而不是写死？** 因为不同平台的 AES 加速硬件不一样。有硬件引擎的板子可以把加解密丢给硬件，没有的用软件实现，P2P 核心代码不用改。**这又是一次"把变化关进笼子"。**

### 12.8 ICE 的三类候选

底层是 pjproject 的 ICE，同时准备三种通路然后择优：

- **HOST** —— 本机地址直连。同局域网时用，延迟最低
- **SRFLX** —— 通过 STUN 问出"我在公网上长什么样"然后两边打洞。**这是跨公网的主力**，多数家用 NAT 能打通
- **RELAYED** —— 通过 TURN 中继。打不通时兜底，费流量但保证能通

STUN/TURN 的地址与账号密码由**云端通过信令下发**，解析在 `base_ice/src/pj_ice.c:580` 起。设备上报的能力位明确带中继：`TUYA_P2P_SDK_SKILL_UDP_TCP_RELAY`（`base_ice/src/tuya_misc.h:14`）。

**所以 P2P 不是只能局域网跑，它主要就是为跨公网设计的。**

### 12.9 三个实现细节

**域名会被跳过**（`pj_ice.c:591` 和 `622`）：

```c
if (!is_ipv4_n(paddr, addrlen) && !is_ipv6_n(paddr, addrlen)) {
    PJ_LOG(2, ("pj_ice", "- turn: %.*s is domain, ignore connect", ...));
    continue;
}
```

云端下发域名而非 IP 时这个服务器会被丢弃，因为 DNS 解析器是关着的（`pj_ice.c:479` 那两行 `pj_dns_resolver_create` 处于注释状态）。**排查"公网连不上"时这是第一个要看的地方。**

**成功直连后会主动拆 TURN**（`ice_strans.c:1917` 附近）。一旦 host 或 srflx 候选被提名成功就销毁未用的 TURN socket，原因写在注释里：TURN 的保活报文会把 lwIP 缓冲打爆（ENOBUFS）。所以日志里 `destroy unused TURN` 是**好消息**，代表直连成功。

**预连接当前是关闭的。** `tuya_media_service_rtc.c:469` 写死了：

```c
g_options.preconnect_enable = false; // Disable the use of pre-connection
```

预连接的思路是"App 还没点开直播就先把 ICE 通道建好"，能显著缩短首帧时间，代价是设备要长期维持一条通道。它同时影响 KCP 的流控开关（12.3 节那个 `nc` 参数）。

### 12.10 线程与会话生命周期

**监听线程**（`svc_streaming_p2p/src/tuya_ipc_p2p.c:189`）：

```c
while (1) {
    session_id = tuya_p2p_rtc_listen();     // 阻塞等一路接入
    if (session_id < 0) break;
    p2p_deal_with_listen(session_id);       // 起该会话的工作线程
}
```

线程栈 **128KB**，且在有外部内存的平台上强制放 PSRAM（`param.psram_mode = 1`）——ICE 协商和 SDP 解析吃栈很凶，片内 SRAM 放不下。

**每个会话再起三条线程：**

- **`cmd_recv_proc_thread`** —— 收命令通道，处理 App 的控制指令
- **`video_send_proc_thread`** —— 就是 12.6 那个媒体发送循环（音视频都在这条里）
- **`audio_downlink_thread`** —— 收 App 下行音频，送到喇叭（对讲）

**完整时间线**（以 App 点开实时预览为例）：

1. App 通过 MQTT 或 LAN 0x20 发来信令（含 SDP：ICE 候选、DTLS 指纹、AES 密钥）
2. `tuya_p2p_rtc_set_signaling` 入队，worker 线程取出处理
3. 双方交换候选，ICE 开始连通性检查
4. 某个候选对被提名 → DTLS 握手 → 通道就绪，未用的 TURN 被销毁
5. `tuya_p2p_rtc_listen()` 返回 session_id，监听线程起三条会话线程
6. App 从命令通道发 `MEDIA_STREAM_*` 指令请求开流
7. 发送循环开始工作：从上层回调取帧 → 检查缓冲 → RTP 打包 → KCP 发送
8. 会话结束，`__p2p_session_all_stop` 清理线程与缓冲

### 12.11 错误码是最好的架构文档

`base_ice/include/tuya_media_service_rtc.h:41` 起定义了 50 多个错误码。**通读一遍错误码，比读代码更快建立全局认知**——它把系统所有可能的失败点都列出来了：

- **连接类** —— `DEVICE_NOT_ONLINE`、`TIME_OUT_NO_ANSWER`、`REMOTE_NO_RESPONSE`、`TIME_OUT_LOCAL_NO_HOST_CAND`（本地连一个候选都没收集到）
- **中继类** —— `NO_RELAY_SERVER_AVAILABLE`
- **加密类** —— `DTLS_HANDSHAKE_FAILED`、`DTLS_HANDSHAKE_FAILED_FINGERPRINT`（指纹对不上，可能有中间人）、`DTLS_HANDSHAKE_TIMEOUT`、`INVALID_AES_KEY`
- **鉴权类** —— `INVALID_TOKEN`、`GET_TOKEN_TIMEOUT`、`AUTH_FAILED`、`INVALID_APILICENSE`
- **会话类** —— `MAX_SESSION`、`OUT_OF_SESSION`、`SESSION_CLOSED_REMOTE`、`SESSION_CLOSED_TIMEOUT`、`REMOTE_SITE_BUFFER_FULL`（对端缓冲满，即背压传导过来了）
- **预连接类** —— 七个 `PRE_SESSION_*`，说明预连接有完整的状态机
- **资源类** —— `OUT_OF_MEMORY`、`UDP_PORT_BIND_FAILED`、`FAIL_TO_CREATE_THREAD`

**排查现场问题时，先拿到错误码再看代码**，能省掉大量盲目搜索。

## 第十三章　AI 子系统

AI 类产品（如聊天机器人）在标准云链路之外还有一整套。

### 13.1 初始化顺序

`app_chat_bot_init` 做的事：

1. **`ai_chat_init`** —— 注册对话模式插件；读 KV 取上次音量与模式（默认按住说、音量 70）；注册用户事件入口；**订阅 MQTT 连通事件**（连通后才 `ai_agent_init`）；初始化 UI、麦克风路径、播放器、KWS；订阅 VAD；创建线程 `ai_chat_mode` 死循环跑模式状态机，每轮 sleep 20ms
2. 注册 UI 动作，可选视频、MCP、图片能力
3. 开两个周期定时器：打印剩余堆、刷新网络图标

**注意时序**：`ai_agent_init` 必须等 MQTT 连通之后，因为 AI 会话要用云端下发的参数。

### 13.2 对话模式

模式决定**什么时候开始送麦、什么时候停**：

- **HOLD（按住说）** —— 按下说话，松开结束
- **单击一轮** —— 点一下说一句
- **唤醒** —— 先 KWS 本地唤醒，再进入收听
- **自由说** —— 接近连续对话

双击按键的完整链路：停止播放 → 通知云 CHAT_BREAK 打断 → 切下一模式 → 写 KV → 播提示音。

### 13.3 上行：声音怎么变成云能懂的流

```
麦克风硬件
  → 板级音频驱动（采样率与 agent 配置对齐，常见 16kHz）
  → record_task 线程：取音、做 VAD
  → 按切片（如 80ms）回调到 ai_chat
  → agent 已初始化则带时间戳调用音频输入 API
  → ai_agent_input 线程打包
  → AI Client（Running 态）加密送出
  → 云端 ASR → 大模型 → 生成回复
```

细节：

- **VAD** 决定语音起止，模式层据此切换状态
- **AEC（回声消除）** 开了可以在播 TTS 时仍送麦，打断更自然；没开则播报时通常不送麦，防自激
- **KWS** 是本地唤醒，不替代云端 ASR

### 13.4 下行：云的话怎么从喇叭出来

**流式 TTS（会话中）**：agent 回调收到 START 就打开前景播放，中间的媒体数据回调把编码帧塞进播放器，END 收尾。收到 CHAT_BREAK 立刻停前景播放并通知 UI。

**URL 型 TTS（技能场景）**：云下发 JSON 含 TTS 的 HTTPS URL、格式、可选背景乐 URL。设备解析后让播放器去拉流（再次 TLS）、解码、送喇叭。

**播放器分前景背景**：前景播放器配短列表，负责提示音与会话 TTS；背景播放器配长列表，负责音乐故事。线程 `ai_player` 整天在取任务、拉流、解码、往 DAC 送 PCM。

### 13.5 AI Client 状态机

线程 `ai_client` 是另一台自动售货机：

- **IDLE** —— 空闲
- **SETUP** —— 准备连接参数
- **CONNECT** —— 建连
- **CLIENT_HELLO** —— 协议问候
- **AUTH_REQ / AUTH_RESP** —— 鉴权请求与应答
- **RUNNING** —— **可收发业务数据**，开 ping 保活
- **出错** —— 按策略退回 SETUP 或 IDLE

**为什么 MQTT 连通前不能真正 AI 对话？** 因为 agent 初始化依赖云端参数，而参数经 MQTT 下发；AI Client 还要走完自己的鉴权到 Running，麦克风数据才有地方去。

## 第十四章　调试手艺

### 14.1 日志

六个级别（`src/tal_system/include/tal_log.h:74`），从严重到啰嗦：

```
ERR → WARN → NOTICE → INFO → DEBUG → TRACE
```

对应宏 `PR_ERR` / `PR_WARN` / `PR_NOTICE` / `PR_INFO` / `PR_DEBUG` / `PR_TRACE`。日志自动带文件名与行号：

```
[08-12 10:55:27 ty D][tuya_lan.c:742] Process Data. FD:3, Num:3, Type:10, Len:22
```

`ty D` 里的 D 就是级别首字母。**这个行号是排查的黄金线索**——看到日志直接定位源码，不用猜。

### 14.2 串口 CLI

SDK 自带命令行（`src/tal_cli/`），需要 `CONFIG_ENABLE_SERIAL_CLI_CMD=y` 才注册完整命令集。常用：

- **`help`** —— 列出所有命令
- **`kv get <key>` / `kv set` / `kv del`** —— 直接读写设备存储
- **`netmgr`** —— 看网络连接状态
- **`reset`** —— 清除激活信息回到未配网
- **`mem`** —— 看剩余堆内存
- **`sys_reboot`** —— 重启
- **`auth` / `auth-read`** —— 运行时读写授权信息

**注意 CLI 和日志可能不在同一个串口上。** `tal_cli_init()` 固定用 UART0 @115200，而平台日志可能在另一个 UART、另一个波特率。T5AI 上就是这样：命令从一个口进、输出从另一个口出。查平台默认监控波特率看 `tools/cli_command/cli_monitor.py` 的 `_CHIP_MONITOR_BAUDRATE`。

### 14.3 用 compile_commands.json 精确编译单个文件

构建会在 `.build/` 下生成 `compile_commands.json`，记录**每个 .c 文件的完整编译命令**（含全部 -I 和 -D）。

**用途**：改了一个文件想快速验证语法，不用全量编译，抽出对应条目直接执行，几秒出结果。

**提醒**：不要自作主张加 `-Wextra`。项目的告警口径是 `-Wall -Werror`，老代码里 `sign-compare`、`unused-parameter` 遍地都是，加了会淹掉你真正关心的新问题。

### 14.4 怎么判断"某功能到底实现没有"

最实用也最多人栽跟头的一条。

**grep 搜不到不等于没实现。** grep 只能定位"可能在哪"，绝不能据此下结论。原因很实际：功能可能藏在宏或条件编译里、可能由另一个模块通过注册机制间接提供、关键词可能是你没想到的拼写。

**正确做法**：grep 定位 → 打开源码完整读一遍 → 必要时跑起来看运行时行为。

**本文里的实例**：判断"P2P 是否支持公网"。grep `stun` 只能搜到一堆错误码定义，什么也说明不了。真正的答案要打开 `pj_ice.c` 读到第 580 行那段服务器解析，才知道 STUN/TURN 是云端下发的、且域名会被跳过。**这个结论 grep 永远给不了你。**

## 第十五章　一次完整人生

用时间线把全书串起来（以 T5AI 上的 AI 聊天产品为例）：

1. **烧录** —— bootloader、tuyaboot、CP、AP 按地址写入 Flash
2. **上电** —— `0x0` → tuyaboot → CP@`0x22000` → AP@`0x132000` → `tuya_app_main` 线程 → `user_main`
3. **基础设施** —— 日志、KV(UF)、定时器、工作队列、读授权
4. **`tuya_iot_init`** —— TLS、证书、端点；有旧激活则标记已激活
5. **netmgr 加配网能力注册**；板级麦喇叭按键屏注册；AI 子系统待命
6. **`tuya_iot_start` 加 yield 循环** —— 新机走配网 → token → 激活写 KV → MQTT；老机直接连路由 → MQTT
7. **MQTT_CONNECTED** —— 上传初始 DP；`ai_agent_init`；AI Client 走到 Running
8. **用户按住说话** —— 模式层开送麦 → record_task → agent_input → 云端 ASR 与大模型
9. **云回 TTS** —— 流式或 URL → ai_player → 喇叭；文本上屏；表情变化
10. **用户双击** —— 打断、切模式、提示音
11. **App 改音量 DP** —— 回调设音量并上报
12. **云推升级** —— OTA 线程写入 `0x004ea000`，重启后 tuyaboot 读 ota_mgr 切新固件

## 附录 A　目录速查

- **`apps/`** —— 示例应用。`switch_demo` 最简单适合入门，`camera_demo` 最完整
- **`boards/<平台>/<板名>/`** —— 板级，回答"这块板焊了什么"
- **`boards/<平台>/common/`** —— 该平台多块板共享的驱动（ESP32 有，Linux 缺）
- **`platform/<平台>/`** —— 芯片适配，TKL 契约的实现。独立仓库，构建时按需下载
- **`platform/platform_config.yaml`** —— 所有支持平台的仓库地址与锁定 commit
- **`src/tal_*/`** —— 系统能力：线程、锁、存储、日志、事件
- **`src/peripherals/`** —— 外设的 TDD 加 TDL 两层
- **`src/tuya_cloud_service/`** —— 云业务：激活、MQTT、DP、LAN、配网
- **`src/tuya_ai_service/`、`src/ai_components/`** —— AI 会话、播放器、UI
- **`src/tuya_p2p/`** —— P2P 音视频，含 pjproject
- **`tools/porting/adapter/`** —— **TKL 契约头文件，移植新芯片的清单**
- **`tools/cli_command/`** —— tos.py 各子命令实现
- **`tools/cmake/`、`tools/kconfiglib/`** —— 构建与配置系统

## 附录 B　新手常踩的坑

- **改了 `app_default.config` 却没生效** —— `using.config` 是缓存，`tos.py clean` 不清它，要 `rm -rf .build/cache`
- **在仓库根目录跑 `tos.py build`** —— 必须 cd 到应用目录
- **`tos.py build` 挂住不动** —— 卡在平台版本确认的交互提示，用 `echo "n" |` 喂输入
- **CLI 敲命令没反应** —— 命令口和日志口可能是两个不同串口，波特率也不同
- **串口全是乱码** —— 波特率不对，日志口和下载口通常不一样
- **`lfs key not found` 以为是错误** —— 只是这个 key 还没写过
- **在回调里干重活** —— 会阻塞整条链路，用工作队列
- **事件订阅了收不到** —— 事件名是字符串，编译期不检查，先查拼写
- **占位 UUID 直接烧板** —— 云会拒绝激活，必须换成平台申请的真实授权
- **把 authkey 提交进公开仓库** —— 安全事故，`tuya_config_secrets.h` 类文件要进 `.gitignore`
- **以为 grep 搜不到就是没实现** —— 见 14.4，这条最伤

## 附录 C　自测题

答不出就回对应章节：

1. TKL 和 TAL 的区别是什么？为什么两层都要？
2. 移植一颗新芯片，你要做的事清单在哪个目录？
3. `src/` 下的组件是怎么被选中编译的？没启用的组件去哪了？
4. T5AI 的 CP 和 AP 谁管 Wi-Fi、谁跑你的应用？固件各在哪个地址？
5. Linux 平台上"Flash"到底是什么？为什么擦除要写 0xFF？
6. PID、UUID、devId 三者关系？localKey 从哪来、干什么用？
7. `tuya_iot_init` 与 `tuya_iot_start`、`yield` 的区别？
8. 未激活设备的状态机怎么一步步走到 MQTT？
9. LPV3.5 帧里 AD 那 14 字节为什么同时当 GCM 的 AAD？
10. LAN 会话密钥是怎么推导的？为什么要双方各出一个随机数？
11. P2P 的信令和媒体分别走哪条路？两者有关系吗？
12. ICE 三类候选各在什么场景生效？
13. TDD 和 TDL 分别解决什么问题？register/find 范式好在哪？
14. 为什么"回调里不能干重活"？该怎么办？
15. 怎么判断某个功能到底实现没有？

## 附录 D　本文的边界

- **不替代芯片数据手册**与射频校准工艺
- **不替代涂鸦平台**创建 PID、购买授权的操作文档
- **不展开每块板的引脚表**，板型差异太大
- **ESP32 的平台适配层细节未展开**，因为该仓库默认不在本地。想深入请先构建一次 ESP32 目标，再读 `platform/ESP32/`
- **AI 帧的字节级协议**、CP 内 Wi-Fi 全线程名，可作后续专题

## 结语

如果这本书只能记住三件事，我希望是：

**第一，分层的本质是把变化关进笼子。** TKL 契约那条线以上不知道芯片、以下不知道业务，所以两边可以独立演进。这也是为什么同一份 pjproject 能在 lwIP 和 glibc 上都跑起来。

**第二，注册–查找范式贯穿全 SDK。** 外设 register/find、LAN 帧类型注册、MQTT 报文分发注册——同一个思路反复出现。看懂一个就看懂全部，写新模块时也该优先照这个形状写。

**第三，一切结论都要落到源码里验证。** 本文每个断言都给了文件行号，就是为了让你能自己去查。技术判断的可靠性，取决于你是读了代码还是猜了代码。
