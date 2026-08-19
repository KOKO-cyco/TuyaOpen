# 嵌入式中用反汇编——用 newlib 字符串函数读懂 ARM Cortex-M 汇编

本文是一篇面向嵌入式 C 工程师的**实战进阶**反汇编教程。

和很多技术博客一样，我们不拿任何闭源/私有库当例子——而是**自己用开源的 newlib 字符串函数（`strlen` / `strcmp` / `strdup`）作示例**，用 `arm-none-eabi-gcc` 编译成 Cortex-M 的目标文件，再用 `objdump -S` 把 **C 源码和汇编逐行交错对照**着讲。文中**每一段汇编、每一个地址、每一个符号都来自真实编译产物**，你可以拿文末附录里的几行命令原样复现。

**阅读前置**：会写 C、大致知道"编译/链接"是怎么回事；不要求预先懂 ARM 汇编——本文会从指令讲起。

**环境**：`arm-none-eabi-gcc 10.3.1`（GNU Arm Embedded Toolchain），目标 `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft`（ARMv7E-M / Thumb-2）。不同 GCC 版本生成的指令可能略有差异，但套路一致。

## 0. 为什么嵌入式工程师要会读反汇编

日常开发里，绝大多数时候我们对着源码调试。但在下面这些场景，**反汇编是你唯一能抓住的线索**：

- **崩在某个 `.a` / `.o` 内部**：典型现象——HardFault，PC 指向一个没有源码的函数；反汇编能——把崩溃地址翻译回函数名 / 源码行，定位是参数错了还是栈炸了。
- **没有 symbols 的固件**：典型现象——只有 `.bin`，调试器里全是 `0x????????`；反汇编能——识别函数边界、调用关系、依赖的外部符号。
- **API 行为与文档/记忆不符**：典型现象——"这个函数明明该返回 0，却返回 -1"；反汇编能——看清它的参数校验、错误返回路径、真实调用链。
- **怀疑编译器优化改了语义**：典型现象——"我明明写了这行，汇编里却没有"；反汇编能——确认代码是否被优化掉、内联、重排。
- **排查调用约定 / ABI 不匹配**：典型现象——C 与汇编互调、新旧库混链时寄存器错乱；反汇编能——核对参数到底走寄存器还是栈。
- **安全 / 合规审计**：典型现象——第三方库到底链了什么、有没有隐藏行为；反汇编能——看依赖符号、看字符串、看可疑调用。

一句话：**源码是给编译器看的，反汇编才是程序"真正在做什么"的 ground truth。**

## 1. 先搞清楚：`.o` / `.a` 到底是什么

编译一个 `.c`，得到的是**可重定位目标文件 `.o`**——它是一个 ELF 文件，但里面的地址都还是**相对的（从 0 开始）**，外部函数（比如 `malloc`）的地址还没填上，要等**链接器**把它和别的 `.o`、库拼成最终固件时才确定。

`ar` 把一堆 `.o` 打包进一个文件，后缀就叫 `.a`（**ar archive**），也就是我们常说的**静态库**。所以：

**小结：`.a` 不是可执行文件，它是"半成品"**——一个装着若干 `.o` 的打包文件。

理解了这一点，你就理解了后文为什么反汇编里到处是 `bl 0 <某函数>`——因为还没链接，调用目标暂时填 0，旁边挂一个"重定位记号"等链接器来填。

### 1.1 动手打个最小的库看看

我们准备三个小源文件（开源实现，文末附录有全文），编译成 `.o`，再打包成 `libdemo.a`：

```bash
# 三个源文件：strlen.c / strcmp.c / strdup.c（开源，见附录）
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -g -o strlen.Os.o strlen.c
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -g -o strcmp.Os.o  strcmp.c
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -O0 -g -o strdup.O0.o strdup.c

# 用 ar 打包成静态库 libdemo.a
arm-none-eabi-ar rcs libdemo.a strlen.Os.o strcmp.Os.o strdup.O0.o
```

`file` 一眼就能看清它们的本质：

```
$ file libdemo.a strlen.Os.o
libdemo.a:   current ar archive          ← .a 就是个 ar 打包文件
strlen.Os.o: ELF 32-bit LSB relocatable, ARM, EABI5 version 1 (SYSV), with debug_info, not stripped
                                       ↑ 可重定位       ↑ ARM 架构   ↑ 带调试信息
```

看看 `.a` 里装了哪些成员、各自导出/依赖什么符号：

```
$ arm-none-eabi-ar t libdemo.a        ← 列成员
strlen.Os.o
strcmp.Os.o
strdup.O0.o

$ arm-none-eabi-nm libdemo.a          ← 看每个成员的符号表

strlen.Os.o:
00000000 T strlen                     ← T = 已定义的代码(Text)

strcmp.Os.o:
00000000 T strcmp

strdup.O0.o:
         U malloc                     ← U = 未定义(Undefined)，外部引用，等链接器解决
         U memcpy
00000000 T strdup
         U strlen
```

符号类型速记（`nm` 第一列字母）：

- **`T` / `t`**：已定义的代码（.text 段）。例：`T strlen`。
- **`D` / `d`**：已定义的已初始化数据（.data）。例：全局 `int x = 3;`。
- **`B` / `b`**：已定义的未初始化数据（.bss）。例：全局 `int y;`。
- **`R` / `r`**：只读数据（.rodata）。例：字符串字面量、`const` 表。
- **`U`**：**未定义**——本文件引用、别处定义。例：`U malloc`。
- **`W` / `w`**：弱符号（weak）。例：可被覆盖的默认实现。

大写 = 全局可见（外部链接），小写 = 本文件内可见（static）。

**一句话：** `T` 是"我能提供"，`U` 是"我需要别人提供"。一个库的 `U` 列表，就是它的"依赖清单"——审计第三方库时先看这个。

## 2. 武器库：`arm-none-eabi-*` 工具链

ARM Cortex-M 用的是 `arm-none-eabi-` 前缀的工具链（`none` = 裸机无操作系统，`eabi` = 嵌入式 ABI）。核心几把刀：

- **`ar`**：打包/解包 `.a`、列成员。用法：`ar t` 列成员，`ar x` 提取成员。
- **`nm`**：列**符号表**（函数/变量/外部依赖）。用法：`nm xxx.o` / `nm xxx.a`。
- **`readelf`**：读 ELF **结构**（头/段/重定位/DWARF）。用法：`readelf -h` / `-S` / `-r`。
- **`objdump`**：**反汇编** + 看 DWARF + dump 段数据。用法：`objdump -d -S`（源码交错反汇编）。
- **`size`**：看 `.text/.data/.bss` 体积。用法：`size xxx.o`。
- **`addr2line`**：把**机器地址翻译回源码行**。用法：`addr2line -f -C -e xxx 0xADDR`。
- **`strings`**：扒字符串（线索金矿）。用法：`strings xxx.o`。

### ⚠️ 第一个坑：别用系统自带的 `objdump`

这是个几乎所有新手都会踩的坑。你的开发机（x86 Linux）自带一个 `objdump`，它是给本机架构用的。拿它去反汇编 ARM 目标，会直接报错：

```
$ objdump -d strlen.Os.o          ← 注意：这是 /usr/bin/objdump，宿主机自带的
strlen.Os.o：     文件格式 elf32-little
objdump: can't disassemble for architecture UNKNOWN !
```

它连架构都识别成 `UNKNOWN`，格式名也只剩 `elf32-little`（丢掉了 `arm`）。

**正确做法**：始终用和目标架构配套的 `arm-none-eabi-objdump`。一个可靠的判断标准——`objdump -d` 报 `UNKNOWN`，就说明你用错工具了，换带 `arm-none-eabi-` 前缀的那个。

```
$ arm-none-eabi-objdump -d strlen.Os.o   ← 正确：和目标架构配套
strlen.Os.o:     file format elf32-littlearm
Disassembly of section .text:
00000000 <strlen>:
   0:	4603      	mov	r3, r0
   ...
```

## 3. 摸清单个 `.o` 的家底：`readelf` / `size` / `nm`

动手反汇编前，先用三个命令把目标的"身份证"摸清楚。

**`readelf -h`——ELF 文件头（架构、类型、ABI）：**

```
$ arm-none-eabi-readelf -h strlen.Os.o
ELF Header:
  Class:                             ELF32              ← 32 位
  Data:                              2's complement, little endian   ← 小端
  Type:                              REL (Relocatable file)          ← 可重定位（半成品）
  Machine:                           ARM                             ← ARM 架构
  Flags:                             0x5000000, Version5 EABI        ← EABI v5
```

`Type: REL` 印证了第 1 节说的：这是还没链接的"半成品"，地址都从 0 开始。

**`size`——各段体积：**

```
$ arm-none-eabi-size *.o
   text	   data	    bss	    dec	    hex	filename
     22	      0	      0	     22	     16	strcmp.Os.o
     56	      0	      0	     56	     38	strdup.O0.o
     44	      0	      0	     44	     2c	strlen.O0.o
     16	      0	      0	     16	     10	strlen.Os.o
```

注意 `strlen`：**`-O0` 版 44 字节，`-Os` 版只有 16 字节**——同一个函数，优化让它缩到约 1/3。优化级别对体积的影响，`size` 一眼可见（第 10 节会深入）。

**`nm`——符号表**（前面已演示），看它**导出了什么**（`T`）、**依赖什么**（`U`）。

把这三样摸清，你对这个 `.o` "是什么、多大、靠谁"就有了底，再进反汇编就不慌了。

## 4. 核心：用 `objdump -S` 做"C 源码 ↔ 汇编"逐行对照

这是本教程最关键的一招，也是博客圈讲反汇编的主流手法。诀窍在于**编译时加 `-g`**（带调试信息），这样 `objdump -S`（大写 S）会把 **C 源码行交错插进反汇编**里，让你看到"这一行 C，到底变成了哪几条指令"：

```bash
arm-none-eabi-objdump -d -S strlen.Os.o      # -d 反汇编, -S 交错显示源码
```

下面三个案例，从简到繁，把 Thumb-2 指令、栈帧、调用约定一次讲透。

### 4.1 案例 A：`strlen` —— 叶子函数，`-Os` 与 `-O0` 全对比

`strlen` 的 C 实现（newlib 便携版，BSD 许可）朴素到不能再朴素：

```c
size_t strlen(const char *str)
{
    const char *start = str;
    while (*str)
        str++;
    return str - start;
}
```

#### 4.1.1 先看 `-Os`（体积优化）版

```
size_t strlen(const char *str)
{
    const char *start = str;

    while (*str)
   0:	4603      	mov	r3, r0          // r3 = str（保存起始指针的"游标"）
   2:	461a      	mov	r2, r3          // r2 = r3（r2 用来取当前字节）
   4:	3301      	adds	r3, #1          // r3++（游标前进一步）
   6:	7811      	ldrb	r1, [r2, #0]    // r1 = *r2（取一个字节，零扩展）
   8:	2900      	cmp	r1, #0          // 和 0 比
   a:	d1fa      	bne.n	2 <strlen+0x2>  // 不为 0 就回到 0x2，继续循环
        str++;
    return str - start;
}
   c:	1a10      	subs	r0, r2, r0      // r0 = r2 - r0（当前指针 - 起始指针 = 长度）
   e:	4770      	bx	lr              // 返回（结果已在 r0）
```

逐条拆解：

- **没有 `push`、没有动 `sp`——整个函数不开栈帧。** 因为它是个**叶子函数**（leaf function，不调用任何别的函数），又不需要保存调用者的寄存器，编译器直接用寄存器搞定。
- **`r0` 进、`r0` 出**：第一个参数在 `r0`，返回值也在 `r0`——这就是 AAPCS 调用约定（第 5 节细讲）。
- `ldrb` = Load Register Byte，取一个字节并把高位补 0（`b` = byte，`l` 末尾暗示零扩展）。C 里的 `*str`（`char`）就这条。
- `bne.n` = Branch if Not Equal，`.n` 表示这是 16 位窄（narrow）指令。Thumb-2 是 16/32 位混合编码，能短的就用 16 位省空间。
- `subs r0, r2, r0` 是 `r2 - r0`：指针相减正好是元素个数（字符串长度）。
- `bx lr`：跳到链接寄存器 `lr`（即返回地址）——函数返回。叶子函数的典型收尾。

整个函数 16 字节，干干净净。

#### 4.1.2 再看 `-O0`（无优化）版——对比才有伤害

```
size_t strlen(const char *str)
{
   0:	b480      	push	{r7}            // ① 保存 r7（帧指针）
   2:	b085      	sub	sp, #20         // ② 栈上开 20 字节空间（局部变量区）
   4:	af00      	add	r7, sp, #0      // ③ r7 = sp，建立帧指针
   6:	6078      	str	r0, [r7, #4]    // 把入参 str 存到栈上 [r7,#4]
    const char *start = str;
   8:	687b      	ldr	r3, [r7, #4]
   a:	60fb      	str	r3, [r7, #12]   // start 存到 [r7,#12]

    while (*str)
   c:	e002      	b.n	14 <strlen+0x14> // 先跳到循环条件判断
        str++;
   e:	687b      	ldr	r3, [r7, #4]    // 取出 str
  10:	3301      	adds	r3, #1          // str++
  12:	607b      	str	r3, [r7, #4]    // 存回 str
    while (*str)
  14:	687b      	ldr	r3, [r7, #4]    // 取 str
  16:	781b      	ldrb	r3, [r3, #0]    // *str
  18:	2b00      	cmp	r3, #0
  1a:	d1f8      	bne.n	e <strlen+0xe>  // 非零继续
    return str - start;
  1c:	687a      	ldr	r2, [r7, #4]    // 取 str
  1e:	68fb      	ldr	r3, [r7, #12]   // 取 start
  20:	1ad3      	subs	r3, r2, r3      // str - start
}
  22:	4618      	mov	r0, r3          // 返回值放进 r0
  24:	3714      	adds	r7, #20         // ④ 回收栈帧
  26:	46bd      	mov	sp, r7
  28:	bc80      	pop	{r7}            // ⑤ 恢复 r7
  2a:	4770      	bx	lr              // 返回
```

`-O0` 的特征是**"老老实实按字面翻译"**——每个变量都老实地存到栈上，每次用到再取出来（看 `str` 在 `[r7,#4]` 和寄存器之间来来回回 str/ldr）。于是出现了经典的**函数序言/尾声（prologue/epilogue）**：

- **序言**（①②③）：`push {r7}` 保存帧指针 → `sub sp, #N` 在栈上开局部变量空间 → `add r7, sp, #0` 把 `r7` 设成帧指针。之后局部变量都用 `[r7, #偏移]` 访问。
- **尾声**（④⑤）：`adds r7, #N` + `mov sp, r7` 回收栈空间 → `pop {r7}` 恢复帧指针 → `bx lr` 返回。

把两个版本放一起看，就懂了优化的本质：**`-Os` 发现这个函数根本不需要栈帧（变量少、是叶子函数），于是把序言/尾声和所有栈访问全砍了，纯寄存器运算。** 这也是为什么 `-O0` 是 44 字节而 `-Os` 只有 16 字节。

**记忆点：** 看到一个函数**开头 `push {…, lr}`、结尾 `pop {…, pc}`**，多半开了栈帧（见 4.3）；如果开头直接干活、结尾就一句 `bx lr`，多半是被优化掉的叶子函数。

### 4.2 案例 B：`strcmp` —— 双指针与前缀变址、返回值技巧

```c
int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
```

`-Os` 编译：

```
int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
   0:	1e43      	subs	r3, r0, #1      // r3 = s1 - 1（技巧：从 -1 开始，下面用前缀自增）
   2:	3901      	subs	r1, #1          // r1 = s2 - 1
   4:	f813 2f01 	ldrb.w	r2, [r3, #1]!   // r3 += 1（即 s1++），再取字节 → r2 = *s1
   8:	f811 0f01 	ldrb.w	r0, [r1, #1]!   // r1 += 1（即 s2++），再取字节 → r0 = *s2
   c:	b10a      	cbz	r2, 12 <strcmp+0x12>  // *s1 == 0 就跳出（cbz = Compare & Branch if Zero）
   e:	4282      	cmp	r2, r0          // 比较 *s1 和 *s2
  10:	d0f8      	beq.n	4 <strcmp+0x4>  // 相等就继续循环
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
  12:	1a10      	subs	r0, r2, r0      // 返回 *s1 - *s2
  14:	4770      	bx	lr
```

三个值得品味的细节：

- **前缀变址 `[r3, #1]!`**：末尾的 `!` 表示"先把基地址加上偏移、再访问"。所以 `ldrb.w r2, [r3, #1]!` 等于"r3 自增 1，再取 `*r3`"。编译器先把指针初始化成 `-1`，再用前缀自增，**把 C 里的 `s1++` 和 `*s1` 两步合并成一条访存指令**——这就是优化的巧劲。`.w` 表示这是 32 位宽（wide）指令。
- **`cbz r2, 12`**：Compare-and-Branch-if-Zero，**一条指令同时完成"判零 + 跳转"**，是 Thumb-2 的高效指令（比 `cmp + beq` 省一条）。对应 C 的 `*s1` 为真判断。
- **返回值技巧 `subs r0, r2, r0`**：C 写的是 `*s1 - *s2`，正好一条减法搞定，结果落在 `r0` 直接返回。遇到不相等的字符或末尾 `\0`，差的正负号就是比较结果。

### 4.3 案例 C：`strdup` —— 非叶子函数：为什么序言要存 `lr`？

```c
char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char  *p   = malloc(len);
    if (p)
        memcpy(p, s, len);
    return p;
}
```

`strdup` 会调用 `strlen` / `malloc` / `memcpy` 三个函数——它是**非叶子函数**。`-O0` 反汇编：

```
char *strdup(const char *s)
{
   0:	b580      	push	{r7, lr}        // ① 注意：这里 push 了 lr！
   2:	b084      	sub	sp, #16
   4:	af00      	add	r7, sp, #0
   6:	6078      	str	r0, [r7, #4]
    size_t len = strlen(s) + 1;
   8:	6878      	ldr	r0, [r7, #4]    // 入参 s 放进 r0（给 strlen 的参数）
   a:	f7ff fffe 	bl	0 <strlen>      // ② 调用 strlen；目标显示 0，原因见第 6 节
   e:	4603      	mov	r3, r0          // 取 strlen 的返回值（在 r0）
  10:	3301      	adds	r3, #1          // +1
  12:	60fb      	str	r3, [r7, #12]   // 存到 len
    char  *p   = malloc(len);
  14:	68f8      	ldr	r0, [r7, #12]   // len 放进 r0（给 malloc 的参数）
  16:	f7ff fffe 	bl	0 <malloc>
  1a:	4603      	mov	r3, r0
  1c:	60bb      	str	r3, [r7, #8]    // 存到 p
    if (p)
  1e:	68bb      	ldr	r3, [r7, #8]
  20:	2b00      	cmp	r3, #0
  22:	d004      	beq.n	2e <strdup+0x2e> // p == NULL 就跳过 memcpy
        memcpy(p, s, len);
  24:	68fa      	ldr	r2, [r7, #12]   // ③ 第 3 个参数 len → r2
  26:	6879      	ldr	r1, [r7, #4]    //    第 2 个参数 s  → r1
  28:	68b8      	ldr	r0, [r7, #8]    //    第 1 个参数 p  → r0
  2a:	f7ff fffe 	bl	0 <memcpy>
    return p;
  2e:	68bb      	ldr	r3, [r7, #8]
}
  30:	4618      	mov	r0, r3          // 返回值 p → r0
  32:	3710      	adds	r7, #16
  34:	46bd      	mov	sp, r7
  36:	bd80      	pop	{r7, pc}        // ④ pop 进 pc，等价于返回
```

和叶子函数 `strlen` 对照，最关键的两点：

- **① 序言 `push {r7, lr}`——多了个 `lr`。** 为什么？因为 `strdup` 要调用别的函数（`bl`），而 `bl` 会把返回地址写进 `lr`，**覆盖掉** `strdup` 自己的返回地址。所以必须在调用前把当前的 `lr` 存进栈保护起来，否则等它调完别人就找不到回家的路了。叶子函数不调用别人、不动 `lr`，所以不用存。
- **④ 尾声 `pop {r7, pc}`——直接 `pop` 进 `pc`。** 因为返回地址当时被存进了栈，现在一把弹进 `pc`，CPU 跳到那个地址 = 返回。这和叶子函数的 `bx lr` 是两种典型收尾：存了 `lr` 的用 `pop {…, pc}`，没存的用 `bx lr`。
- **③ 看 `memcpy` 调用前的参数装载**：`r0=p`、`r1=s`、`r2=len`，正好是前三个参数依次进 `r0/r1/r2`——这是 AAPCS 的现场示范（下一节）。
- **② `bl 0 <strlen>`**：调用指令是 `bl`（Branch with Link），目标却显示 `0`。这不是 bug，是"还没链接"——第 6 节解释。

## 5. AAPCS 速查：参数怎么传、谁负责存寄存器

AAPCS（ARM Architecture Procedure Call Standard，ARM 过程调用标准）规定了函数间怎么传参、谁保存寄存器。看懂它，汇编里的寄存器搬运就有了意义。

- **`r0–r3`（别名 `a1–a4`）**：**参数 / 返回值**（前 4 个参数；返回值在 `r0`）。保存责任：**调用者**（callee 可随便用，用前不存）。
- **`r4–r11`（别名 `v1–v8`）**：通用变量寄存器。保存责任：**被调用者**（要用就必须先 push、用完 pop 还原）。
- **`r12`（`ip`）**：临时/链接寄存器（intra-procedure）。保存责任：调用者。
- **`r13`（`sp`）**：栈指针（**8 字节对齐**）。
- **`r14`（`lr`）**：链接寄存器（返回地址）。保存责任：被调用者（如果它还要调别人）。
- **`r15`（`pc`）**：程序计数器。

要点：

1. **前 4 个参数走 `r0–r3`，第 5 个及以后压栈。** 回看 `strdup` 里给 `memcpy` 装参数：`r0=p`(第1)、`r1=s`(第2)、`r2=len`(第3)——教科书级的 AAPCS。
2. **`r0–r3` 是"易失"的**（caller-saved）：函数调用后这 4 个值不保证还在。所以 `strdup` 在 `bl malloc` 之后立刻 `mov r3, r0` 把返回值挪到 `r3` 再存栈——因为接下来要调 `memcpy`，`r0` 会被覆盖。
3. **`r4–r11` 是"非易失"的**（callee-saved）：函数想用就必须保护。所以序言里 `push {r7, lr}`、`push {r4,r5,…}` 保护的就是这些。
4. **栈 8 字节对齐**：AAPCS 要求函数调用边界上 `sp` 是 8 字节对齐的——这也是序言里 `sub sp, #N` 的 `N` 经常凑成 8 的倍数的原因。

**调试小贴士：** C 和汇编互调、或新老库混链出诡异问题时，十有八九是 AAPCS 不匹配（参数个数、寄存器保存、栈对齐）。对着反汇编数一遍 `r0–r3` 和 `push/pop`，往往一眼就看出来。

## 6. 重定位：为什么 `bl` 的目标是 `0`

第 4.3 节里 `bl 0 <strlen>` 让人困惑：明明是调用 `strlen`，目标怎么是 `0`？

答案就在第 1 节埋的伏笔——`.o` 是**还没链接的半成品**，`strlen` 的最终地址此时谁也不知道，编译器只能先填 `0`，再在旁边挂一张"重定位表（relocation table）"，告诉链接器："**这个位置的 `bl`，等你知道 `strlen` 在哪了，记得回来把正确地址填上。**"

用 `readelf -r` 就能看到这张表：

```
$ arm-none-eabi-readelf -r strdup.O0.o

Relocation section '.rel.text' contains 3 entries:
 Offset     Info    Type            Sym.Value  Sym. Name
0000000a  00000f0a R_ARM_THM_CALL    00000000   strlen
00000016  0000100a R_ARM_THM_CALL    00000000   malloc
0000002a  0000110a R_ARM_THM_CALL    00000000   memcpy
```

把这张表和 `strdup` 的反汇编对上号：

- `a: bl 0 <strlen>` → 重定位 Offset `0x0000000a`，类型 `R_ARM_THM_CALL`，要填的符号 `strlen`。
- `16: bl 0 <malloc>` → Offset `0x00000016`，类型 `R_ARM_THM_CALL`，符号 `malloc`。
- `2a: bl 0 <memcpy>` → Offset `0x0000002a`，类型 `R_ARM_THM_CALL`，符号 `memcpy`。

**一一对应！** 三个 `bl 0` 各自挂了一个 `R_ARM_THM_CALL` 重定位项。`R_ARM_THM_CALL` 专门用于 Thumb 的 `bl/blx` 调用——链接器会根据这条记录，把目标函数的真实地址编码进那条 `bl` 指令里（Thumb 的 `bl` 是两条 16 位半字拼成的 32 位指令，编码比较巧妙）。

常见的几种重定位类型：

- **`R_ARM_THM_CALL`**：用于 Thumb 的 `bl/blx` 调用。典型场景：函数调用。
- **`R_ARM_CALL`**：用于 ARM（32 位）的 `bl/blx`。典型场景：非 Thumb 的调用。
- **`R_ARM_ABS32`**：32 位绝对地址。典型场景：访问全局变量、字面量池、调试段。

**顺带一提：** `readelf -r` 里还会看到一堆针对 `.debug_*` 段的 `R_ARM_ABS32`——那是调试信息内部用的地址引用，正常运行不参与，只有 `addr2line`/`gdb` 会用到。

**所以："`.o` 反汇编里看到 `bl 0 <某函数>`，不是写错了，是还没链接。"** 等链接成最终固件再反汇编，这些 `0` 就会变成真实地址（或就近的相对偏移）。

## 7. `addr2line` + DWARF：把机器地址翻译回源码行

崩溃地址是 `0x080012a6`，怎么知道它对应哪一行 C 代码？靠编译时加 `-g` 嵌入的 **DWARF 调试信息**。两个利器：

**`objdump --dwarf=decodedline`——列出"地址 ↔ 源码行"映射：**

```
$ arm-none-eabi-objdump --dwarf=decodedline strlen.Os.o

CU: ./strlen.c:
File name           Line number    Starting address
strlen.c            5                   0x0
strlen.c            8                   0x0
strlen.c            8                   0x2
strlen.c            8                   0x4       ← 偏移 0x4 对应第 8 行
strlen.c           10                   0xc
strlen.c           11                   0xe
```

**`addr2line`——直接问"这个地址出自哪一行、哪个函数"：**

```
$ arm-none-eabi-addr2line -e strlen.Os.o -f -C 0x4
strlen                   ← 所在函数
/tmp/disasm_libc/strlen.c:8   ← 源文件:行号
```

实战中拿到崩溃 PC，扔给 `addr2line`（或带调试信息的 `addr2line -e firmware.elf 0x080012a6`），立刻就知道崩在哪个函数哪一行——这是排查 HardFault 的标准动作（第 9 节）。

**前提：** 固件**带调试信息**（`-g`）且**没 strip**。如果 `.a` 是 stripped 的，`addr2line` 只能回退到函数名（靠符号表），给不出行号。

## 8. 从 `.a` / `.o` 里还能挖出什么情报：`strings` 与依赖

`strings` 能扒出文件里的可见字符串，是个情报金矿。对一个**带调试信息**的 `.o`，它能暴露不少东西（节选）：

```
$ arm-none-eabi-strings strlen.Os.o
strlen
strlen.c
size_t
GNU C17 10.3.1 20210824 (release) -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -march=armv7e-m -g -Os
GCC: (GNU Arm Embedded Toolchain 10.3-2021.10) 10.3.1 20210824 (release)
...
```

注意那一长串：**编译器把"完整的编译选项"也塞进了 DWARF**——`-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -march=armv7e-m -g -Os`。也就是说，**从一个带调试信息的 `.o`，你能反推出它用什么工具链、什么优化级别编译的**。审计第三方库时这是很有用的线索。

结合第 1 节的 `nm`，拿到一个陌生 `.a`，标准"情报三板斧"是：

1. `ar t lib.a` —— 它由哪些模块组成；
2. `nm lib.a` —— 它**提供**什么（`T/D/B/R`）、**依赖**什么（`U`）；
3. `strings lib.a` —— 编译环境、版本、内嵌字符串。

这三步做完，你对这个库"是什么、靠谁、怎么编的"就有了全貌——常常比读文档还快。

## 9. HardFault 与栈回溯：拿到崩溃地址之后

嵌入式最头疼的 HardFault，反汇编是救命稻草。Cortex-M 进异常时，硬件会自动把一组寄存器压到当前栈上（**异常栈帧**），布局如下（无 FPU 时 8 个字 = 0x20 字节）：

```
栈顶 (sp) →  xPSR        ← 异常发生时的程序状态
             Return_addr ← ★ 故障点的返回地址（通常是 PC）
             LR (r14)
             R12
             R3
             R2
             R1
             R0
```

排查流程（典型）：

1. 进 HardFault_Handler，读当前 `sp`（先判断是 MSP 还是 PSP）；
2. 从栈上第 2 个字取出 `Return_addr`（即故障 PC）；
3. 若有 `SCB->CFSR`/`BFAR`/`MMFAR`，一起读出来判断是访存错、对齐错还是总线错；
4. 把 `Return_addr` 喂给 `addr2line -e firmware.elf <addr>`，定位到函数名和源码行；
5. 再 `objdump -d` 那个函数，看故障地址那一条指令在访问什么，进而推断是空指针、野指针、栈溢出还是数组越界。

下面是一段**示意性**的故障栈（数值仅作格式演示）：

```
sp -> 0x21000000   xPSR
      0x080012a7   Return_addr  ← ★ 喂给 addr2line
      0x080014ed   LR
      0x00000000   R12
      ...          R3/R2/R1/R0
```

```
$ arm-none-eabi-addr2line -e firmware.elf -f -C 0x080012a6
parse_packet                                   ← 崩在 parse_packet
src/net/protocol.c:142                         ← 第 142 行
```

拿到"崩在 `protocol.c:142`"，再对照源码和反汇编，问题往往就现形了。

**进阶：** 要还原完整调用栈（而不仅是故障点），需要沿帧指针 `r7`（或用 `.ARM.exidx` / `-funwind-tables` 的展开表）逐层回溯。叶子函数被优化掉栈帧时（如 4.1 的 `-Os` strlen）没有 `r7` 可跟，这时靠 ARM 的展开段（`.ARM.exidx`）更可靠——`arm-none-eabi-addr2line` 配合带调试信息的 ELF 已经能做不错的回溯。

## 10. 优化级别识别：同一个 `strlen`，三种指纹

前面已经看到 `strlen` 在 `-O0` 和 `-Os` 下的天壤之别。把 `-O2` 也加进来对比，就总结出"识别优化级别"的指纹：

**`-O2` 版：**

```
00000000 <strlen>:
   0:	7803      	ldrb	r3, [r0, #0]     // 先单独取第一个字节
   2:	b133      	cbz	r3, 12 <strlen+0x12>  // 空：直接跳到返回路径
   4:	4603      	mov	r3, r0
   6:	f813 2f01 	ldrb.w	r2, [r3, #1]!   // 循环体用前缀变址
   a:	2a00      	cmp	r2, #0
   c:	d1fb      	bne.n	6 <strlen+0x6>
   e:	1a18      	subs	r0, r3, r0
  10:	4770      	bx	lr
  12:	4618      	mov	r0, r3           // 空串的快速返回路径
  14:	4770      	bx	lr
  16:	bf00      	nop
```

三个级别的指纹对比：

- **`-O0`（text 44 B）**：有完整栈帧（`push {r7}` / `sub sp` / `add r7`）；**每个变量都溢到栈**，反复 `str/ldr`；指令最多、最直白。
- **`-Os`（16 B）**：无栈帧、纯寄存器；尽量用 16 位窄指令；**体积最小**，逻辑紧凑。
- **`-O2`（18 B）**：无栈帧；**会把循环"旋转"**（先判首字节走快速路径）、为速度牺牲一点体积；可能见到分支预测友好的布局。

识别口诀：

- **看到一堆 `str rN,[r7,#x]` / `ldr rN,[r7,#x]` 来回搬运** → 多半是 `-O0`（或 `-Og`）。
- **函数无栈帧、指令精简、偏爱 16 位窄指令** → `-Os`。
- **循环被旋转/展开、出现多条快速路径、为速度多发几条指令** → `-O2`/`-O3`。
- 再用 `strings` 里那条编译选项 `-O?` 直接确认（带调试信息时）。

**小结：** 这套识别在做"逆向推断别人用什么选项编译的"、"解释为什么同一份代码行为/体积不同"时特别有用。

## 11. 反汇编能还原多少源码？谈"伪代码 ≈ 百分之几源码"

我们整篇在做的事——把汇编旁边写上等价 C——本质就是"源码还原"。自然有个问题：**如果手上是个没有源码的二进制，反汇编到底能还原出百分之几的源码？**

答案是：**取决于你说的"还原"到哪个层面**。源码里其实叠了好几层信息，而编译是**单向有损**的——有些信息一旦编译就永远回不来。

### 11.1 哪些信息编译时就丢了（无法从二进制恢复）

- **注释** — ❌ 100% 丢失：编译时直接丢弃，任何工具都救不回。
- **宏（`#define`）** — ❌ 已展开：编译期替换完毕，只剩展开后的结果。
- **内联函数边界** — ❌ 已融合：`-O2` 内联后，原函数边界消失。
- **局部变量名** — ⚠️ 看 DWARF：stripped 二进制丢失；带 `-g` 的 DWARF 里有。
- **结构体字段名** — ⚠️ 看 DWARF：同上；无 DWARF 时只能从访存偏移推断布局。
- **函数名** — ⚠️ 看符号表：stripped 后变成 `sub_0000012A`；有符号表则保留。
- **控制流结构（for/while/switch）** — ⚠️ 部分恢复：被拍平成跳转/比较，反编译器尝试重建但不完美。
- **类型信息** — ⚠️ 部分推断：从指令位宽、访存模式、传参方式推断。

### 11.2 "百分几源码"——分层诚实回答

下面百分比是工程经验估计，重点看**层级关系**而非绝对数字：

- **理解行为、能复现功能** — **80%~99%**：反汇编 + 反编译能搞清算法逻辑，足以写出行为等价的实现。
- **带 DWARF 的 C 还原** — **50%~85%**：`-g` 编译的 `.o`/elf，`addr2line`、`objdump -S` 给出函数名、变量名、类型、行号，接近原貌。
- **纯 stripped 反汇编 + 人工还原** — **30%~50%**：控制流/数据流能还原，但无注释/宏/原名，靠人工逐条推理。
- **字面级逐字还原原始源码文本** — **10%~30%**：注释、宏、格式、变量名基本全丢，只剩结构骨架。

### 11.3 一句话结论 + 工具上限

- 想"**搞清它干什么、能不能复现功能**" → 反汇编足够，行为层面能到 **80% 以上**。
- 想"**逐字还原原始源码**" → 别想了，编译有损，**注释和宏永远回不来**。
- **工具决定上限**：带 `-g` 调试信息 ≫ 只有符号表 ≫ stripped 裸二进制。
- **反汇编器 vs 反编译器**要分清：
  - **反汇编器**（disassembler，如 `objdump`）→ 输出**汇编**（本文讲的就是它 + 人工还原）。
  - **反编译器**（decompiler，如 **Ghidra / IDA Hex-Rays**）→ 直接输出 **C 伪代码**，连 stripped 二进制也能还原成可读伪代码——但仍是"行为近似"，不是"原样"。

**回看本文：** 我们用 `objdump -S` 看到 `strlen`/`strcmp` 的源码对照能接近 100%，恰恰是因为 **①有原始源码 ②带了 `-g`**。换成真正的闭源 stripped 库，就只能落到上面 30%~50% 那一档，靠 Ghidra 之类慢慢啃。

## 12. 进阶：让 AI Agent 直接帮你反汇编

现在的 AI 编程助手（Claude Code、Cursor 等）能在终端里直接调 `arm-none-eabi-*`，帮你反汇编、解释指令、还原伪代码。但有几个**不点破就跑不通**的坑——全是本教程前面踩过的。把它们写成给 Agent 的明确约束，效率会高很多。

### 12.1 七条实操技巧

**① 指定 `arm-none-eabi-` 前缀、最好给全路径（最重要的坑）**
Agent 默认可能调系统 `/usr/bin/objdump`，结果就是第 2 节那个 `can't disassemble for architecture UNKNOWN`。第一条指令就应写死："用 `<工具链>/bin/arm-none-eabi-objdump`"。给个变量更稳：先 `OBJDUMP=/path/arm-none-eabi-objdump`，后续都用 `$OBJDUMP`。

**② 先摸身份、再反汇编（三板斧顺序）**
让 Agent 按 `file` → `readelf -h`（确认架构/类型）→ `nm`（看符号/依赖）→ 最后才 `objdump -d -S` 的顺序来。一上来就 dump 容易用错工具或面对一堆无符号地址。

**③ 拆小：千万别一次 dump 整个 `.a`**
一个 `.a` 反汇编可能几十万行，直接撑爆 Agent 的上下文窗口。让它聚焦：
- 从 `.a` 提取单个 `.o`：`arm-none-eabi-ar x lib.a 某某.o`
- 只反汇编一个函数：`arm-none-eabi-objdump --disassemble=函数名 文件`
- 或截取一段：`arm-none-eabi-objdump -d 文件` 后只取 `<函数名>:` 到下一个函数之间的部分

**④ 要源码对照，强调 `-g`**
若能拿到源码或能重编一个探针，让它用 `-g` 编译，`objdump -S` 就能给出 C 交错对照（如本文全篇）。没有源码时，让它用"反编译器思维"直接给伪代码。

**⑤ 把伪代码当"假设"而非"事实"，要求它逐条举证**
Agent 给的伪代码常常"看起来对、其实错"。要求它：**每一条结论都标注对应哪几条汇编指令**（像本文第 4 节那样逐行注释）。能和指令对上的才采信，对不上的追问。

**⑥ 让 Agent 区分"未链接 `.o`" vs "已链接固件"**
`.o` 里 `bl 0 <xxx>` 是正常的（第 6 节重定位），不是 bug；已链接固件里就是真实地址。明确告诉 Agent 目标是哪种，免得它把 `bl 0` 当异常来分析。

**⑦ 让它主动报优化级别与调用约定**
这两件事 Agent 很擅长，且能解释大部分"行为反常"。直接让它判断：是否有栈帧（`push {…,lr}`）、叶子还是非叶子、像 `-O0` 还是 `-Os/-O2`（第 10 节指纹）、参数怎么传（AAPCS）。

### 12.2 一个可直接复制的 Prompt 模板

```
工具链：/opt/gcc-arm-none-eabi/bin（含 arm-none-eabi-objdump / nm / readelf / addr2line）
目标：libdemo.a，重点看 strdup 这个函数
请按顺序执行并解释：
1) arm-none-eabi-readelf -h 确认架构与文件类型（是 REL 还是 EXEC？）
2) arm-none-eabi-nm 看 strdup 的符号类型与未定义依赖（U 符号）
3) 从 .a 提取 strdup 所在 .o，用 arm-none-eabi-objdump -d -S 反汇编该函数
4) 逐条解释指令，说清：调用约定(AAPCS)、是否开栈帧、叶子/非叶子、
   优化级别、bl 0 出现的原因
5) 给出等价 C 伪代码，每条标注对应的汇编偏移地址作为依据
约束：不要用系统 objdump（会对 ARM 目标报 architecture UNKNOWN）。
```

### 12.3 Agent 适合 / 不适合干什么

**✅ 适合交给 Agent：**

- 解释单个函数的指令与逻辑；
- 还原算法逻辑、给带举证的伪代码；
- 核对调用约定 / 栈帧 / 优化级别；
- 把崩溃地址翻译回函数名+行号。

**❌ 不适合：**

- 一次性逆向整个 stripped 固件（上下文装不下）；
- 100% 可靠的逐字源码还原（见第 11 节，本就不可能）；
- 替代 Ghidra 做大规模批量反编译。

一句话：**让 Agent 当你的"反汇编副驾驶"——它读指令、查工具、给假设、标依据；你来拍板真伪。**

## 13. 速查卡

```bash
# —— 看本质 ——
file xxx.a xxx.o                              # .a = ar archive; .o = ELF relocatable
arm-none-eabi-readelf -h xxx.o                # 架构/类型/ABI（Type: REL = 半成品）
arm-none-eabi-size xxx.o                      # text/data/bss 体积

# —— 看符号/依赖 ——
arm-none-eabi-nm xxx.o                        # T=已定义代码, U=未定义依赖, B/D/R=数据
arm-none-eabi-ar t xxx.a                      # 列出 .a 里的成员

# —— 反汇编（核心）——
arm-none-eabi-objdump -d -S xxx.o             # ★ 源码交错反汇编（需 -g 编译）
arm-none-eabi-objdump -d xxx.o                # 纯反汇编
arm-none-eabi-objdump --disassemble=strlen xxx.o   # 只看某个函数

# —— 重定位 ——
arm-none-eabi-readelf -r xxx.o                # 看 bl 0 为什么是 0（R_ARM_THM_CALL）

# —— 地址 ↔ 源码 ——
arm-none-eabi-objdump --dwarf=decodedline xxx.o
arm-none-eabi-addr2line -e xxx.o -f -C 0xADDR # 崩溃地址 → 函数名+行号

# —— 情报 ——
arm-none-eabi-strings xxx.o                   # 编译选项、版本、内嵌字符串
```

寄存器速记：`r0–r3` 参数/返回（caller 存）、`r4–r11` 通用（callee 存）、`r12 ip`、`r13 sp`、`r14 lr`、`r15 pc`。

序言/尾声速记：叶子函数常 `bx lr` 收尾；非叶子 `push {…,lr}` 开头、`pop {…,pc}` 收尾。

## 附录：完整复现步骤

**说明：** 三个示例源码均为开源实现：`strlen` / `strcmp` 取自 newlib 便携版（BSD-style license），`strdup` 为 POSIX 经典实现。下列命令在 `arm-none-eabi-gcc 10.3.1` 下逐条可复现。

**A.1 准备源文件**

`strlen.c`：

```c
#include <stddef.h>
size_t strlen(const char *str)
{
    const char *start = str;
    while (*str)
        str++;
    return str - start;
}
```

`strcmp.c`：

```c
#include <stddef.h>
int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
```

`strdup.c`（用 `extern` 声明，避免头文件内联，确保反汇编里看到真实 `bl` 调用）：

```c
#include <stddef.h>
extern size_t strlen(const char *s);
extern void  *malloc(size_t n);
extern void  *memcpy(void *dst, const void *src, size_t n);

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char  *p   = malloc(len);
    if (p)
        memcpy(p, s, len);
    return p;
}
```

**A.2 编译（关键参数 `-mcpu=cortex-m4 -mthumb -mfloat-abi=soft -g`）**

```bash
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -g -o strlen.Os.o strlen.c
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -O0 -g -o strlen.O0.o strlen.c
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -O2 -g -o strlen.O2.o strlen.c
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -g -o strcmp.Os.o  strcmp.c
arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -O0 -g -o strdup.O0.o strdup.c

# 打包成静态库
arm-none-eabi-ar rcs libdemo.a strlen.Os.o strcmp.Os.o strdup.O0.o
```

**A.3 复现本文每一段反汇编**

```bash
arm-none-eabi-objdump -d -S strlen.Os.o     # 第 4.1.1 节
arm-none-eabi-objdump -d -S strlen.O0.o     # 第 4.1.2 节
arm-none-eabi-objdump -d -S strcmp.Os.o      # 第 4.2 节
arm-none-eabi-objdump -d -S strdup.O0.o     # 第 4.3 节
arm-none-eabi-readelf -r strdup.O0.o        # 第 6 节
arm-none-eabi-addr2line -e strlen.Os.o -f -C 0x4   # 第 7 节
```

**提示：** 不同 GCC 版本生成的指令可能略有差异（寄存器分配、是否用某条 Thumb-2 指令等），但本文讲的所有套路（栈帧、AAPCS、重定位、优化指纹）都通用。

*示例源码 `strlen` / `strcmp` 源自 newlib（BSD-style license），`strdup` 为 POSIX 经典实现，仅作教学用途。*
