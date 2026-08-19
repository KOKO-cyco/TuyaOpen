# TuyaOpen SDK cJSON 详解

这篇文档回答两类问题：

- **cJSON 这个库本身怎么用**：数据结构、解析、构建、内存规则
- **TuyaOpen SDK 里怎么用它**：真实调用点在哪、两种典型用法模式、SDK 特有的一个内存陷阱

**关于可信度**：文中每个结论都标了文件与行号，你可以自己打开验证。少数纯讲 cJSON 库本身用法、和这个仓库无关的部分不带行号。

读之前只需要知道 JSON 长什么样、会看 C 结构体和指针。

---

## 第一章　cJSON 是什么，SDK 里谁在用

### 1.1 一句话

cJSON 是一个只有一个头文件、一个源文件的 C 语言 JSON 解析/构建库（`src/libcjson/cJSON/cJSON.h` + `cJSON.c`，共 3429 行）。它把任意 JSON 文本解析成一棵由链表组成的树，也能反过来把这棵树序列化回文本。

### 1.2 为什么 TuyaOpen SDK 离不开它

设备和云端、设备和 App 之间几乎所有通信都是 JSON：

- 上云的 DP（数据点）上报、ATOP 云 API 请求体
- 云端下发的 DP schema（描述这个产品有哪些功能点）
- P2P 直播的信令（offer/answer/candidate 交换）
- MQTT 下发指令的分发（`reqType` 字段路由到不同处理函数）

这些都是"字符串 ↔ 结构化数据"的转换，cJSON 就是干这个的。

### 1.3 仓库里的真实分布

```
src/libcjson/cJSON/cJSON.c                              ← 上游 DaveGamble/cJSON 1.7.16，主线 Linux/嵌入式平台用这份
platform/T5AI/t5_os/{ap,cp}/components/json/            ← T5AI 平台自带的拷贝，1.7.18（比主线还新）
platform/T5AI/t5_os/{ap,cp}/components/demos/leagcy/.../json/  ← 更老的一份，连版本号宏都没有
```

**注意**：仓库里不是只有一份 cJSON，而且版本关系和直觉相反——T5AI 平台自带的 `components/json` 那份（1.7.18）反而比主线 `src/libcjson`（1.7.16）**更新**，只有目录名带 `leagcy`（legacy）的第三份才是真正意义上的老代码。链接哪一份取决于平台构建脚本怎么配 include 路径。写跨平台代码时不要假设行为和某一份完全一致——如果遇到诡异的解析差异，先确认这次编译实际链的是哪一份、版本号是多少。

### 1.4 SDK 里实际调用频次（真实统计，不含 cJSON 库自身和平台 vendor 拷贝）

对 `src/`、`apps/`、`examples/` 下所有 `.c` 文件做的调用计数：

| API | 调用次数 |
|---|---|
| `cJSON_GetObjectItem` | 572 |
| `cJSON_Delete` | 268 |
| `cJSON_IsString` | 184 |
| `cJSON_AddStringToObject` | 152 |
| `cJSON_CreateObject` | 117 |
| `cJSON_AddItemToObject` | 106 |
| `cJSON_free` | 86 |
| `cJSON_PrintUnformatted` | 85 |
| `cJSON_Parse` | 70 |
| `cJSON_IsNumber` | 62 |
| `cJSON_IsObject` | 48 |
| `cJSON_AddNumberToObject` | 43 |
| `cJSON_GetObjectItemCaseSensitive` | 42 |
| `cJSON_IsArray` | 36 |
| `cJSON_CreateString` | 36 |
| `cJSON_GetStringValue` | 35 |

这张表决定了下面该讲透哪些 API——排前面的都会在第三、四章展开。

---

## 第二章　数据结构：一切都是链表

### 2.1 唯一的结构体

cJSON 只有一种节点类型，数组、对象、字符串、数字全用它表示（`src/libcjson/cJSON/cJSON.h:103-123`）：

```c
typedef struct cJSON
{
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;

    int type;

    char *valuestring;
    int valueint;      // 已废弃，别用，用 cJSON_SetNumberValue
    double valuedouble;

    char *string;      // 这个节点在对象里的 key 名（如果它是某个对象的子项）
} cJSON;
```

`type` 是个位标记（`cJSON.h:89-97`）：

```c
#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)
```

### 2.2 数组和对象怎么用同一套字段表达

一个 JSON 对象 `{"a":1,"b":2}` 在内存里是这样的：

```
root (type=Object)
  └─ child ──▶ [string="a", type=Number, valuedouble=1]
                    │ next
                    ▼
               [string="b", type=Number, valuedouble=2]
                    │ next
                    ▼
                  NULL
```

数组也是同一套（子项没有 `string` 字段，靠位置区分而不是 key）。所以"数组"和"对象"根本不是两种数据结构，只是同一个链表节点，`type` 不同、`string` 字段用不用而已。

### 2.3 GetObjectItem 是线性扫描

找一个 key，就是从 `object->child` 开始沿 `next` 一个个比较字符串，不是哈希表。JSON 对象字段少的时候（几个到几十个，DP schema、P2P 信令都是这个量级）无所谓；如果哪天要解析几百个字段的大对象，重复 `GetObjectItem` 会是 O(n²)，得自己拿到 `child` 后手动遍历一次。

---

## 第三章　解析：从字符串到树

### 3.1 基本用法

```c
cJSON *root = cJSON_Parse(json_str);
if (root == NULL) {
    // 解析失败，root 是 NULL，不需要 Delete
}
```

### 3.2 铁律：拿到节点先判空，再判类型

`cJSON_GetObjectItem` 找不到 key 会返回 `NULL`；就算找到了，也不保证类型是你以为的那种——**JSON 本身没有 schema 约束，云端/对端传什么都合法**。这条铁律在仓库里被违反的后果不难猜：空指针解引用崩溃。

**真实的类型不一致案例**（`src/tuya_cloud_service/schema/dp_schema.c:1382-1392`）：

```c
item = cJSON_GetObjectItem(cjson, "id");
if (NULL == item) {
    op_ret = OPRT_CJSON_GET_ERR;
    goto __exit;
}
if (item->type == cJSON_String) {
    dp_desc->id = atoi(item->valuestring);   // 有的固件/工具传的是字符串 "123"
} else {
    dp_desc->id = item->valueint;            // 有的传数字 123
}
```

同一个字段 `id`，不同来源可能传字符串也可能传数字，这段代码显式判断 `item->type` 后分别处理——这不是防御性编程过度，是真出现过这种情况才这么写的。**拿到一个字段，永远不要假设它的类型，用 `cJSON_IsString`/`cJSON_IsNumber`/`cJSON_IsObject`/`cJSON_IsArray` 先确认。**

### 3.3 "判空判类型"落到实处的完整例子

P2P 信令解析整段都遵守这条铁律，而且**每一个失败分支都会释放 `root`**（`src/tuya_p2p/base_ice/src/tuya_media_service_rtc.c:492-530`）：

```c
cJSON *root = cJSON_Parse(msg);
if (root == NULL) {
    return -1;                          // Parse 失败，root 已经是 NULL，不用 Delete
}

cJSON *el_header = cJSON_GetObjectItemCaseSensitive(root, "header");
if (!cJSON_IsObject(el_header)) {
    cJSON_Delete(root);                 // header 字段不存在或类型不对，释放整棵树再退出
    return -1;
}

cJSON *el_from = cJSON_GetObjectItemCaseSensitive(el_header, "from");
cJSON *el_to   = cJSON_GetObjectItemCaseSensitive(el_header, "to");
cJSON *el_sessionid = cJSON_GetObjectItemCaseSensitive(el_header, "sessionid");
cJSON *el_type = cJSON_GetObjectItemCaseSensitive(el_header, "type");
if ((!cJSON_IsString(el_from)) || (!cJSON_IsString(el_to)) ||
    (!cJSON_IsString(el_sessionid)) || (!cJSON_IsString(el_type))) {
    cJSON_Delete(root);                 // 必填字段缺一个或类型不对，同样释放后退出
    return -1;
}

// 可选字段：不存在就给默认值，不强求
char *trace_id = cJSON_IsString(el_trace_id) ? el_trace_id->valuestring : "";
```

这段代码值得学的地方：**必填字段用"判空+判类型"两道检查，可选字段用三元表达式给默认值，每条退出路径都配对释放**。这是解析不可信输入（对端传来的信令）该有的严谨度。

### 3.4 GetObjectItem 和 GetObjectItemCaseSensitive 的真实区别

两者实现只差一个参数（`src/libcjson/cJSON/cJSON.c` 内部 `get_object_item`）：大小写不敏感版本用 `case_insensitive_strcmp` 比较 key，敏感版本用普通 `strcmp`。JSON 规范里 key 是大小写敏感的，`cJSON_GetObjectItem` 之所以不敏感是历史遗留兼容行为。

仓库里两种写法都在用（572 次 vs 42 次）：新协议相关的代码（P2P 信令）明确用 `CaseSensitive`，符合规范；老代码（`dp_schema.c` 等）历史上一直用不区分大小写的版本，没有动机去改。**新写解析代码时用 `CaseSensitive` 版本**，除非要兼容一个已知会传错大小写 key 的老对端。

### 3.5 遍历数组

```c
cJSON *arr = cJSON_GetObjectItem(root, "candidates");
cJSON *item = NULL;
cJSON_ArrayForEach(item, arr) {
    // item 依次是数组里的每个元素
}
```

`cJSON_ArrayForEach` 是个宏（`cJSON.h:290`），本质就是 `for (item = arr->child; item != NULL; item = item->next)`，没有魔法。

---

## 第四章　构建：从数据到字符串

### 4.1 两种建节点的方式

`cJSON_CreateXxx` 系列（`CreateObject`/`CreateString`/`CreateNumber`/`CreateArray`）只是**造一个新的、独立的节点**，还没挂到任何树上：

```c
cJSON *root = cJSON_CreateObject();
cJSON *name = cJSON_CreateString("switch_demo");
```

### 4.2 AddXxxToObject 是"造节点 + 挂上去"的快捷方式，且转移所有权

```c
cJSON_AddStringToObject(root, "name", "switch_demo");
// 等价于：
// cJSON *v = cJSON_CreateString("switch_demo");
// cJSON_AddItemToObject(root, "name", v);
```

关键在于 `cJSON_AddItemToObject`（内部就是把节点链进 `object->child` 链表，源码可查 `add_item_to_array`，纯指针操作、不拷贝）：**挂上去之后，这个子节点的生命周期就归父节点管了**。父节点被 `cJSON_Delete` 时，子节点会被递归一起释放。这意味着：

- 挂上去的节点**不能自己再单独 `cJSON_Delete` 一次**——那是双重释放
- 同一个节点**不能挂到两个父节点下**——树的所有权是单一的，不是共享的

### 4.3 序列化：PrintUnformatted 是嵌入式场景的默认选择

```c
char *json_str = cJSON_PrintUnformatted(root);   // 无缩进无换行，最省字节
// char *json_str = cJSON_Print(root);           // 带缩进，人类好读，占字节多
```

上云、发 MQTT、传 P2P 信令这些场景，带宽和内存都金贵，仓库里 85 次 `PrintUnformatted` 对应几乎为 0 次的 `Print`（未在高频表里出现）——嵌入式代码基本不用带格式化的版本，只有调试打印才会考虑。

### 4.4 真实的"常驻树、增量改"模式

大多数场景是"造一棵树、序列化、扔掉"的一次性用法，但 `src/tuya_cloud_service/cloud/tuya_device_meta.c` 是个例外——它维护一棵**长期存活**的 JSON 树，每次上报新 meta 字段时增量修改而不是重建（`tuya_device_meta.c:151-186`）：

```c
if (s_meta.json == NULL) {
    s_meta.json = cJSON_CreateObject();
    cJSON_AddObjectToObject(s_meta.json, "metas");
    cJSON_AddNumberToObject(s_meta.json, "t", 0);
}
cJSON *metas = cJSON_GetObjectItem(s_meta.json, "metas");
cJSON *existing = cJSON_GetObjectItem(metas, key);
if (existing) {
    cJSON *new_val = cJSON_CreateString(value);
    cJSON_ReplaceItemInObject(metas, key, new_val);   // 换掉旧值，不整树重建
} else {
    cJSON_AddStringToObject(metas, key, value);
}
char *tmp = cJSON_PrintUnformatted(s_meta.json);      // 序列化用于本次上报
// ... 发送 tmp ...
cJSON_free(tmp);                                       // 只释放字符串，树还活着，下次接着用
```

`cJSON_ReplaceItemInObject` 会自动释放被替换下来的旧节点，调用者不用操心。这个模式的价值在于避免"每上报一次 meta 就整棵树重新 Create 一遍"的浪费——树的结构基本不变，变的只是值。

---

## 第五章　内存：这是全篇最容易出事的一章

### 5.1 谁该释放，释放到哪一层

`cJSON_Delete(root)` 递归释放 `root` 以及它 `child`/`next` 链上的一切。**前提是 `root` 是一棵你独立拥有的树**——按第四章的规则，只要没把它的任何子节点重复挂到别处、也没对子节点单独 Delete 过，这一步总是安全的。

### 5.2 PrintUnformatted 的返回值不能用 `free()`

```c
char *s = cJSON_PrintUnformatted(root);
// free(s);        ✗ 错误：这块内存不一定是 libc malloc 分配的
cJSON_free(s);      // ✓ 正确：永远配对使用 cJSON 自己的释放函数
```

`cJSON_free` 的实现只有一行（`src/libcjson/cJSON/cJSON.c:3126-3129`）：

```c
CJSON_PUBLIC(void) cJSON_free(void *object)
{
    global_hooks.deallocate(object);
}
```

它调用的是当前配置的分配器钩子，不是写死的 libc `free`。这就引出下一节 TuyaOpen SDK 特有的坑。

### 5.3 TuyaOpen 特有的坑：分配器钩子被换过

cJSON 默认用 libc 的 `malloc`/`free`（`cJSON.c:186` 的 `global_hooks` 初值）。但几乎每个 TuyaOpen app 的 `user_main()`（或等价入口）**第一行**都会调用 `cJSON_InitHooks` 把它换掉，例如 `apps/tuya_cloud/camera_demo/src/tuya_main.c:251`：

```c
void user_main(void)
{
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});
    tal_log_init(...);   // 连日志系统都是这行之后才初始化的
    ...
```

部分 AI 类 app（如 `apps/tuya.ai/your_chat_bot/src/tuya_main.c:291-293`）甚至按平台分两种钩子：有 PSRAM 的平台把 cJSON 的分配挂到 PSRAM 分配器上，省 SRAM。

这意味着几件事：

1. **这行代码执行之前解析的任何 JSON 用的是 libc 内存**——正常流程里不会发生，因为它就是入口函数的第一行，早于任何其他初始化
2. **`cJSON_free()` 在这个 SDK 里实际调用的是 `tal_free`**（或 PSRAM 版本），不是你以为的 libc free——这也是为什么规则 5.2 "别用 `free()`" 在这里格外重要：两个分配器的堆完全不是一回事，用错了直接破坏内存
3. **新建一个 app 模板时如果漏抄这一行**，cJSON 会默默退回用 libc `malloc`/`free`。在有完整 libc 的 Linux 平台上可能看不出问题，但在 RTOS/裸机平台上，libc 堆和 `tal_malloc` 管理的内存池是两套账本，混用会导致难以复现的内存损坏

**检查一个新 app 模板是否遗漏了这行的方法**：`grep -n cJSON_InitHooks apps/<你的app>/src/tuya_main.c`，确认它在 `user_main`/`tuya_app_main` 的最开头。

---

## 第六章　SDK 里两种真实用法模式的对照

| | 一次性应答式 | 常驻累积式 |
|---|---|---|
| 典型场景 | 解析云端/P2P 下发的一条消息 | 设备自己维护的、持续上报的状态 |
| 生命周期 | 函数内建、函数内销 | 跨调用存活，模块级静态变量持有 |
| 改动方式 | 不存在"改动"，每次都是全新解析 | `cJSON_ReplaceItemInObject` 增量替换 |
| 例子 | P2P 信令解析（第 3.3 节） | 设备 meta 上报（第 4.4 节） |
| 释放规则 | 用完立刻 `cJSON_Delete` | 常驻树活到进程结束或显式清理；每次序列化产生的字符串各自 `cJSON_free` |

上报云端走的是"构建"那条链路的完整闭环：`cJSON_CreateObject` 组装 → `cJSON_PrintUnformatted` 序列化成字符串发出去 → 树本身 `cJSON_Delete`（一次性场景）或保留复用（常驻场景）→ 序列化出来的字符串单独 `cJSON_free`。**树和字符串是两块独立内存，各自释放，别漏一个。**

---

## 第七章　踩坑清单

- **字段类型不能假设一致**：同一个 JSON 字段，不同来源可能传字符串也可能传数字（3.2 节的 `id` 例子）。拿到节点先 `cJSON_Is*` 判类型，再决定读 `valuestring` 还是 `valuedouble`。
- **每条错误返回路径都要配对 `cJSON_Delete`**：解析函数里一旦提前 `return`，忘记释放已经 `Parse` 出来的树就是内存泄漏。看 3.3 节的例子照抄这个结构。
- **`cJSON_AddItemToObject` 之后不能再对子节点单独 `Delete`**：所有权已经转移给父节点，父节点释放时会连带释放它，重复释放是典型的 double-free。
- **同一个节点不能挂到两棵树上**：cJSON 的树不支持共享子节点，这么做会导致两次释放同一块内存。
- **`cJSON_PrintUnformatted`/`cJSON_Print` 的返回值只能用 `cJSON_free` 释放**，不能用 `free()`——在这个 SDK 里两者背后是完全不同的分配器（5.2、5.3 节）。
- **新 app 模板漏抄 `cJSON_InitHooks` 这一行**：cJSON 会静默退回 libc 分配器，在没有完整 libc 堆的平台上是定时炸弹（5.3 节）。
- **`cJSON_GetObjectItem` 是大小写不敏感的**：如果协议要求严格匹配 key（比如新写的协议解析代码），显式用 `cJSON_GetObjectItemCaseSensitive`（3.4 节）。
- **不要在大对象上重复调用 `cJSON_GetObjectItem` 查不同的 key**：它是线性扫描，字段一多就是 O(n²)（2.3 节）。

---

## 附录　高频 API 速查表

| API | 用途 | 真实调用次数 | 参考例子 |
|---|---|---|---|
| `cJSON_Parse` | 字符串 → 树 | 70 | `dp_schema.c:1376` |
| `cJSON_GetObjectItem` | 按 key 查子节点（大小写不敏感） | 572 | `dp_schema.c:1382` |
| `cJSON_GetObjectItemCaseSensitive` | 按 key 查子节点（大小写敏感） | 42 | `tuya_media_service_rtc.c:499` |
| `cJSON_IsString`/`IsNumber`/`IsObject`/`IsArray` | 判类型 | 184/62/48/36 | `tuya_media_service_rtc.c:500` |
| `cJSON_ArrayForEach` | 遍历数组 | 25 | 见 3.5 节 |
| `cJSON_CreateObject`/`CreateString`/`CreateNumber`/`CreateArray` | 造独立新节点 | 117/36/12/21 | `tuya_device_meta.c:151` |
| `cJSON_AddStringToObject`/`AddNumberToObject` | 造节点 + 挂到对象（转移所有权） | 152/43 | `tuya_device_meta.c:179` |
| `cJSON_AddItemToObject`/`AddItemToArray` | 挂已有节点到对象/数组（转移所有权） | 106/32 | 见 4.2 节 |
| `cJSON_ReplaceItemInObject` | 替换已有 key 的值，自动释放旧值 | 3 | `tuya_device_meta.c:177` |
| `cJSON_PrintUnformatted` | 树 → 紧凑字符串 | 85 | `tuya_device_meta.c:183` |
| `cJSON_free` | 释放 `Print*` 的返回值 | 86 | `tuya_device_meta.c:186` |
| `cJSON_Delete` | 递归释放整棵树 | 268 | `tuya_media_service_rtc.c:503` |
| `cJSON_InitHooks` | 换分配器（TuyaOpen 每个 app 入口第一行必调） | — | `tuya_main.c:251` |
