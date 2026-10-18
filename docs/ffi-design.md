# 动态库 FFI 设计

`kv.ffi(path)` 在**本进程内**加载一个 `.dll` / `.so`，按动态库自己声明的签名
调用其中的函数。

本文写清楚：为什么这么设计、动态库侧要写什么、PyLite 侧怎么用、
以及哪些还没做（回调、远程）。

---

## 一、它与 kv.add() 的分工

两者名字都带 "add"，但**语义完全不同**，不要混用：

| | `kv.add(node_id, binary, args, work_dir, addr)` | `kv.ffi(path)` |
|---|---|---|
| 加载的东西 | 一个**可执行文件** | 一个**动态库** |
| 怎么跑 | fork/exec 起独立进程 | 在当前进程内 `dlopen` |
| 崩了会怎样 | 只影响那个进程 | **带崩整个宿主** |
| 生命周期 | 由 kv_admin 纳管、注册表持久化 | 由 lib.unload() 显式卸载 |
| 用途 | 纳管 adviskv 这类服务节点 | 调用本机的算法/驱动/系统 DLL |

**远程动态库**走「远程进程 + agent」路线：由 `kv.add` 纳管远端的 agent 进程，
agent 内部再用 FFI 加载远端的 `.so`。这不在本文范围（见第六节）。

---

## 二、两种模式：进程内 与 隔离

`kv.ffi(path)` 把 DLL 加载在**当前进程**里。`kv.ffi_isolated(path)` 则起一个
**子进程**加载它，宿主只负责发消息收消息。

| | `kv.ffi(path)` | `kv.ffi_isolated(path)` |
|---|---|---|
| DLL 加载在哪 | 宿主进程 | 子进程 |
| DLL 崩溃 | **宿主一起死** | 只有子进程死，宿主拿到 `"worker crashed (exit code 139)"` |
| 性能 | 直接调用，无序列化 | 每次调用一次管道往返 + JSON 序列化 |
| 回调 | 支持（函数指针直接给） | **不支持**（2026-10 第一版） |
| 未来 | — | 协议与远程 agent 一致，换连接方式即可 |

**实测**（`examples/ffi_isolated_demo.pys`）：

```
isolated dll_add(3,4): {"ok":true,"value":7}
isolated dll_crash: {"ok":false,"error":"worker crashed (exit code 139)"}
after crash: {"ok":false,"error":"worker already dead (exit code 139)"}
=== host is still alive, demo done ===
```

退出码 139 = 128 + SIGSEGV，与 POSIX 语义一致（Windows 上 `GetExitCodeProcess`
给的是 NTSTATUS `0xC0000005`，已折算成 POSIX 风格）。

### 2.1 隔离模式的结构

```
宿主(PyLite)                          子进程(xmex_ffi_agent)
   │ 写请求(管道)                          │
   ├────────── {"op":"call",...} ────────▶ │ 加载 DLL
   │                                         │ 调函数
   │ ◀────────── {"ok":true,"value":7} ─────┤
   │                                         │
   │  ← DLL 段错误 → 子进程死 →             │
   │     宿主读到 EOF → waitpid → 139        │
```

**为什么 agent 是独立可执行文件**，而不是直接 fork 宿主：

1. 子进程是干净的，只加载目标 DLL —— 不会把宿主已加载的运行时、
   已注册的 GC 根、已打开的套接字都带过去
2. DLL 崩了时，退出码干净地反映在 agent 的退出状态上
3. 将来做远程 agent 时，同一个可执行文件换个连接方式即可

### 2.2 协议

一行一条 JSON。请求：

```json
{"op":"call","fn":"dll_add","args":[3,4]}
{"op":"symbols"}
{"op":"describe","fn":"dll_add"}
{"op":"quit"}
```

响应：

```json
{"ok":true,"value":7}
{"ok":true,"symbols":["dll_add",...]}
{"ok":false,"error":"..."}
```

握手：agent 启动后先发一行 `{"ok":true,"loaded":"ffi-1"}`，表示 DLL 加载成功；
加载失败则发 `{"ok":false,"error":"..."}` 并退出。

### 2.3 agent 在哪

宿主按「与宿主可执行文件同目录」推断：`bin/xmex_ffi_agent`。也可以用环境变量
`XMEX_FFI_AGENT` 指定绝对路径（部署到别处时用）。

### 2.4 隔离模式暂不支持回调

回调要跨进程反向通道（子进程 → 宿主），且要处理「宿主正在等调用结果、
同时又要执行回调」的重入问题。这是独立的一期工作。

---

## 三、为什么要求动态库导出「描述符」

PyLite 侧的调用是 `lib.init(1, "abc")` —— **参数个数与类型在编译期不知道**。
`init` 是运行时的名字，参数是运行时的值。

C 的函数没有自描述能力，`dlsym` 拿到的只是一个裸指针。所以动态库必须
自己声明有哪些函数、每个函数收什么参数、返回什么：

```c
// 动态库导出这个,返回 JSON 文本的长度,内容写进 buf
int64_t xmex_ffi_describe(char* buf, int64_t cap);
```

返回的 JSON 形如：

```json
{
  "functions": [
    {"name": "dll_add",   "params": ["int", "int"],     "ret": "int"},
    {"name": "dll_greet", "params": ["str"],             "ret": "str"},
    {"name": "dll_scale", "params": ["float", "int"],   "ret": "float"},
    {"name": "dll_flip",  "params": ["bool"],           "ret": "bool"}
  ]
}
```

**没有描述符会怎样**：退化为「裸调用」模式 —— 调用方自己给函数名，
参数一律当 int64 传，没有类型信息。此时返回的对象只有 `call` / `symbols`，
没有按描述符调用的能力。简单库不写描述符也能用，但类型全丢。

---

## 四、调用约定

所有被调函数按**同一个原型**导出：

```c
int64_t fn(int64_t* args, int64_t nargs, int64_t* out);
```

| 参数 | 含义 |
|---|---|
| `args` | 入参数组，顺序与描述符的 `params` 一致 |
| `nargs` | 入参个数 |
| `out` | 返回值槽，至少 1 个 `int64` |
| 返回值 | `0` 成功，非 0 是错误码 |

**为什么要固定成一个原型**：`dlsym` 返回的裸函数指针没有类型信息，
只能按一个已知原型调用。要求对方按约定导出，就把「任意 C 签名」的不确定性
挡在门外。代价是对方要写一层薄封装 —— 但这层封装是**一次性的**，
换来的是宿主侧不必为每个函数写胶水。

### 参数类型编码

| 描述符类型 | `args[i]` 里放什么 |
|---|---|
| `int` | 数值本身 |
| `bool` | `0` / `1` |
| `float` | `double` 的位模式（用 `memcpy` 传） |
| `str` | `char*`，指向**本次调用期间有效**的缓冲；函数返回后失效 |
| `ptr` | 裸指针（用于出参缓冲） |

**`str` 的有效期是硬约定**：宿主在调用前把字符串拷进一块临时缓冲，
传它的地址，函数返回后缓冲复用。想保存字符串的动态库必须自己拷贝。

同理，**返回的 `str` 也必须在宿主拷走之前有效**。测试 DLL 里用的是
静态缓冲，够用但不是线程安全的 —— 正式使用建议动态库自己管一块
per-call 的内存，或者约定由宿主提供输出缓冲。

---

## 五、PyLite 侧用法

```python
lib = kv.ffi("./xx.dll")          # 加载;失败返回 {"ok":false,"error":...}
lib2 = kv.ffi_isolated("./yy.dll")  # 隔离模式:加载到子进程

print(lib.symbols())              # '["dll_add","dll_greet",...]'
print(lib.describe("dll_add"))    # '{"name":"dll_add","params":["int","int"],"ret":"int"}'
print(lib.describe())             # 全部函数的描述数组

r = lib.call("dll_add", [3, 4])   # {"ok":true,"value":7}
r = lib.call("dll_greet", ["world"])  # {"ok":true,"value":"hello, world"}

print(lib.path())                 # 动态库路径
lib.unload()                      # 卸载
```

全局 introspection：

```python
print(kv.ffi_list())
# '[{"id":"ffi-1","path":"./xx.dll","functions":4,"has_descriptor":true}]'
```

**返回值一律是 JSON 字符串**，不是结构化值 —— 因为 PyLite 侧解析
JSON 字符串比解析嵌套结构省事得多（`kv.status` 等接口也是这个约定）。

`lib.call` 的实参只支持标量（int/float/bool/str）和标量组成的列表。
传 dict 或嵌套结构会被拒绝并给出明确错误，而不是默默出错。

---

## 六、实现要点

### 6.1 加载与查找

用 `include/xmex/platform.h` 里已有的跨平台抽象：

```
XMEX_DLOPEN  →  Windows: LoadLibraryA   /  Linux: dlopen(RTLD_LAZY)
XMEX_DLSYM   →  Windows: GetProcAddress /  Linux: dlsym
XMEX_DLCLOSE →  Windows: FreeLibrary     /  Linux: dlclose
```

所以 `ffi.cpp` **没有 POSIX 专有依赖，跨平台编译** —— 这点是刻意的：
加载 Windows DLL 正是它的主要用途，放进 `if(NOT WIN32)` 就等于在
Windows 上把这个功能阉掉。

### 6.2 描述符读取

描述符可能返回长度，也可能把内容写进 buf。实现用两段式：
先给 64KB 缓冲，不够再按返回的长度重试一次。

### 6.3 JSON 解析

**没有引入通用 JSON 解析器**。描述符格式是我们自己定的，用专用的
`JsonParser`（约 150 行）解析 —— 比通用解析器快，也更少出错。

项目里现有的 JSON 处理（`cluster.cpp` 的 `extractJsonString`）都是
「按 key 抠字符串」，不是解析器，不能直接复用。

### 6.4 方法注册

`lib` 是 `PY_INSTANCE`，类型名 `"ffi"`。方法通过
`instance_register_method("ffi", 3, "symbols", 7, fn)` 注册，
`call_method` 查表分派。**前端与 irgen 完全不用改** —— 这跟
`kv.iter()` 那套迭代器是同一个机制。

---

## 七、还没做的

### 7.1 反向回调（`lib.on("init", handler)`）

把 PyLite 函数的地址交给动态库，让它回调。PyLite 的函数符号是
`xmex_<模块>_<函数>`，可以 `dlsym` 反查自身，所以技术上可行。

但有两个必须先解决的问题：

1. **GC 根登记**：`gc_root_push` 的实现是裸 `malloc` 的全局数组，
   **没有锁**。DLL 在自己创建的线程上回调时，跨线程操作这个根栈会
   数据竞争。要先给它加锁。
2. **GC 只扫当前线程**：`gc.cpp` 用 `pthread_getattr_np(pthread_self())`
   获取栈基址，只扫触发回收的那个线程。其他线程栈上的 `PyValue`
   不在扫描范围内 → 回调参数构造出的对象会被立即回收 → use-after-free。

所以回调这一期要连带改 GC，不是纯 FFI 层的事。

### 7.2 远程动态库

形态必然是：

```
PyLite 侧                    远端
  ↓ HTTP/TCP
  ↓ 客户端桩
  ↓ ──────────────→  agent 进程 → dlopen(remote.so) → 调用
  ↑ ←──────────────  返回值
```

`dlopen` 只能加载本地文件，所以远端必须有个 agent 进程。
协议格式（JSON over HTTP 还是自定义二进制）需要单独设计，
它决定了后续所有兼容性。

### 7.3 中间件

中间件也是动态库，复用同一套 FFI 机制。区别在于：中间件通常需要
**进程隔离**（它崩了不该带崩宿主），所以要用「独立进程 + agent」的形态，
而不是进程内 `dlopen`。

现有的 `runtime/plugin_api.cpp` 有 `plugin_scan` / `plugin_register` /
`plugin_call` / `plugin_list` 四个函数，但**全是桩** —— 整个文件没有一处
`dlopen`。要变成真的，得接到 `ffi.cpp` 这套机制上。

---

## 八、测试

`tests/ffi_test.cpp` 测一个**真实的**动态库（`tests/ffi_test_dll.cpp`），
不是桩。覆盖：加载、符号列表、签名查询、int/float/bool/str 四种类型的
调用、错误路径（函数返回非 0、符号不存在）、全局列表、卸载。

`examples/ffi_demo.pys` + `examples/ffi_main.cpp` 是**端到端**用例：
C++ 宿主 + PyLite 脚本，脚本里 `kv.ffi()` 加载 DLL 并调用其函数。
这是唯一能验证「PyLite 源码 → 编译 → 链接 → 加载 DLL → 调用」整条链路的
用例——`ffi_test` 只测 C++ 层的 `ffi.cpp`，测不到 irgen 的映射与 ABI 约定。

```bash
cmake --build build && cd build && ctest -R ffi --output-on-failure
```

`examples/ffi_isolated_demo.pys` + `examples/ffi_isolated_main.cpp` 测**隔离模式**，
核心验证点是 `dll_crash` 让 DLL 段错误后**宿主必须活着**并继续往下跑。

跨平台：Windows 上编 `.dll`，Linux 上编 `.so`，产物名不同，测试按平台猜。

### 测试与端到端验证抓到的 bug

**1. `ffi_load` 返回局部 `std::string` 的 `c_str()` —— 悬空指针。**

```cpp
std::string id = make_id();
...
return id.c_str();     // ← 函数返回后 id 析构,指针失效
```

调用方 `kv_ffi` 拿到的 `id` 指向已释放的内存，`str_new` 复制到的是**空串**。
症状很隐蔽：DLL 确实加载成功了（`ffi_list` 能看到它），但**所有通过实例
发起的调用都报 "no handle"**——因为实例里的句柄是空的。

修法：`ffi_load` 改用输出参数回传句柄，不再返回裸指针。
教训：**C 接口返回 `const char*` 时，指向的内存必须在函数返回后依然有效**；
局部 `std::string` 的 `c_str()` 做不到这一点。

**2. `describe` 返回的 JSON 缺结尾的 `}`。**

子串匹配（`contains(desc, "\"ret\":\"int\"")`）看不出来，是后来加了
「花括号/方括号配平」断言才暴露的。教训：验证 JSON 时不能只做子串匹配，
要验结构完整性。

**3. FFI 适配器被放进了 `#ifndef _WIN32` 段。**

`kv_ffi_v` 落在 Linux-only 段里，Windows 上链接直接失败
（`undefined reference to 'kv_ffi_v'`）。FFI 是**跨平台**的，
加载 Windows DLL 正是它的主要用途。已移到条件块之外。

**4. irgen 里的 `Value` 名字冲突（改名遗留，与 FFI 无关但阻断了验证）。**

`irgen` 里裸写的 `Value` 一直指 `llvm::Value`（LLVM 的 SSA 值），
但改名把 PyLite 的值类型也命名成了 `xmex::Value`，两者同在 `namespace xmex` 里。
近邻查找规则让裸 `Value` 解析到 `xmex::Value`，于是：

```
error: no declaration matches 'xmex::Value* xmex::Abi::newSlot(...)'
```

修法不是改 200 多处 `Value`（风险太大），而是**消除根因**：
把 tag 枚举拆到独立头 `include/xmex/value_tags.h`，`irgen` 只引它，
不引 `value.h`——`xmex::Value` 就不进 irgen 的作用域，裸 `Value`
稳定解析到 `llvm::Value`。

> 教训：做全局改名时，**必须检查目标名字在各个作用域下是否已有含义**。
> `PyValue` → `xmex::Value` 这个改动在 runtime 里没问题，但在 `namespace xmex`
> 内部就撞上了 LLVM 的 `Value`。

**5. Windows 上 `WaitForSingleObject` 的返回值陷阱。**

```cpp
if (!WaitForSingleObject(h, INFINITE)) return -1;   // ✗ 错
```

`WAIT_OBJECT_0` 的值**就是 0**，所以 `!` 把成功当成了失败 —— 退出码永远是 -1，
崩溃隔离看起来"生效了"但退出码不对。必须显式比 `WAIT_OBJECT_0`。

**6. `CreateProcessA` 的命令行缓冲区必须可写。**

`std::string` 的 `operator[]` 在 C++11 之后不保证可写（`&s[0]` 只在
`s[s.size()] == '\0'` 时合法），而 `CreateProcessA` 会**就地修改**命令行。
用 `std::vector<char>` 显式留出结尾的 `\0`。

**7. 测试 DLL 的崩溃函数必须放在 `extern "C"` 里。**

写在块外会被 C++ mangle（导出成 `_Z9dll_crashPxxS_`），宿主 `dlsym`
按 `dll_crash` 找不到 —— 报 "symbol not found"，而不是段错误。
症状有欺骗性：看起来像"崩溃测试没生效"，其实是符号名不对。

