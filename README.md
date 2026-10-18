# Xmex

一个用 LLVM 实现的 Python 风格小语言,能**在编译期编译成目标文件、与 C++ 一起链接**,
同一套前端也驱动 **ORC JIT** 做运行时执行。

```python
a, b = input(int, int)
for i in range(1, a):
    print(i)
```

这行代码不是合法 C++ —— 逗号表达式加 `=` 在语法分析阶段就不成立,模板元编程和
运算符重载都在它之后才生效,救不了。所以必须有一个前端把它降成 LLVM IR。

除了语言本身,Xmex 还带三块运行时能力:

| 能力 | 入口 | 平台 |
|---|---|---|
| **动态库调用** | `kv.ffi(path)` / `kv.ffi_isolated(path)` | 跨平台 |
| **进程纳管与故障注入** | `kv.add(...)` / `kv.sleep(...)` | Linux |
| **声明式调谐与副本组** | `kv.reconcile_start()` / `kv.group_create(...)` | Linux |

## 构建

需要 MSYS2 的 UCRT64 环境(不是 MSVCRT 的那个 MinGW):

```bash
cd /d/xmex
bash build-windows.sh              # 等价于 configure + 构建 + ctest,构建目录 build-win/
```

脚本会先把 PATH 修好再构建 —— 这一步不是可有可无的便利,见下。

想手工敲命令的话:

```bash
# 在 D:\msys64\ucrt64.exe 打开的 shell 里
cd /d/xmex
cmake -G Ninja -S . -B build-win -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build-win
cd build-win && ctest --output-on-failure
```

依赖:`mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,llvm,llvm-tools}`,LLVM 22.1.8。

**不要在别的 shell 里构建,也不要让 `/mingw64/bin` 抢在 `/d/msys64/ucrt64/bin` 前面。**
如果 PATH 里 MSVCRT 版 MinGW 排在前面,UCRT64 的 `cc1plus.exe` 会加载 MSVCRT 的
`libgcc_s_seh-1.dll`,于是**任何编译都静默失败** —— 退出码 1、没有任何错误输出,
连 `int main(){}` 都编不出来。`g++ --version` 照样打印 16.x,因为那条路径不经过
cc1plus,所以它不是有效的判断方式;要判断就直接编一个 hello world。

同样的错配在**运行期**是另一种症状:`libstdc++-6.dll` 这类名字在两套工具链里
都存在,加载到 MSVCRT 那份就是堆静默损坏(而不是报错)。所以修 PATH 要贯穿
configure → 构建(要跑 xmexc.exe)→ ctest(要加载 libLLVM-22.dll)全程。

**Linux 构建不受影响**:`CMakeLists.txt` 里平台差异都在 `if(WIN32)`/`else()` 分支内,
`else()` 侧与原先一致。

### 平台差异(2026-10)

**跨平台核心**(Windows 与 Linux 都编,23 个源文件):`error` `gc` `arith` `string` `tuple`
`list` `dict` `index` `method` `io` `docker` `llm` `fusion` `inference` `tokenizer`
`quantize` `concurrency` `object` `trace` `ffi` `ffi_api` `platform_proc`
`distributed_adapter`。

**仅 Linux**(依赖 POSIX socket / fork / seccomp / adviskv):`sandbox` `cgroup` `sysmon`
`server` `engine` `cuda` `cluster` `kv_admin` `kv_queue` `plugin_api` `lsm_client` 等。

⚠️ **这意味着 `kv.add`(进程纳管)、`kv.reconcile_*`(调谐循环)、`kv.group_*`(副本组)
在 Windows 上不存在** —— 它们实现在 `runtime/kv_admin.cpp`,而那个文件是 Linux-only。
**`kv.ffi` / `kv.ffi_isolated` 才是跨平台的**(它们在 `runtime/ffi*.cpp`)。

> 自研 raft 已于 2026-10 移除(它是模拟实现,不是共识实现)。
> 理由、替代方案与恢复方式见 [docs/raft-removal.md](docs/raft-removal.md)。

## 用法

以下命令都在 **UCRT64 shell** 里、于仓库根目录执行(`build-win/bin/` 里已放着
所有可执行文件)。

### 1. 先写一个 .pys

```python
# hello.pys
xs = [3, 1, 2]
xs.sort()
for x in xs:
    print(x, x * x)
```

### 2. 编译成目标文件,与 C++ 链接

```bash
./build-win/bin/xmexc.exe hello.pys -o hello.o
```

产出的是普通 COFF 目标文件。符号命名规则:

| 源文件里的东西 | 生成的符号 |
|---|---|
| 顶层语句 | `xmex_<模块名>_main` |
| `def foo(...)` | `xmex_<模块名>_foo` |

模块名默认取输入文件的主文件名,可用 `-I <名称>` 覆盖。写一个最小驱动就能跑:

```cpp
// main.cpp
extern "C" void xmex_hello_main();   // 按 ABI 约定,参数/返回值都是 xmex::Value*
int main() {
  xmex_hello_main();
  return 0;
}
```

```bash
g++ -std=c++20 main.cpp hello.o -o hello.exe -L./build-win/bin -lxmex_runtime
./hello.exe
# 1 1
# 2 4
# 3 9
```

`-L./build-win/bin` 是为了让链接器找到导入库 `libxmex_runtime.dll.a`,
运行时则需要 `libxmex_runtime.dll` 在 `PATH` 上(它与 `xmexc.exe` 同在
`build-win/bin/`,把那个目录加进 PATH 最省事)。

调用带参数的 Xmex 函数时,所有 `xmex::Value` 都**按指针**收发 ——
这是 Win64 ABI 的硬性要求,详见下面的"两个务必先读的点"。返回值也走第一个
指针参数(隐藏的 sret):

```cpp
#include "xmex/runtime.h"   // 编译时要加 -I./include

// xmexc 为 `def add(a, b): return a + b` 生成的符号
extern "C" void xmex_math_add(xmex::Value *ret, xmex::Value *a, xmex::Value *b);

xmex::Value a = int_(1), b = int_(2), r;
xmex_math_add(&r, &a, &b);     // r 里是 3
```

```bash
g++ -std=c++20 main.cpp math.o -o math.exe \
    -I./include -L./build-win/bin -lxmex_runtime
```

想直接看一个**完整的混合程序**(C++ 造数据 → Xmex 算 → C++ 收结果,
含字符串与多返回值),见 [examples/mixed/](examples/mixed/README.md)。

### 3. 查看中间产物

```bash
./build-win/bin/xmexc.exe hello.pys -o hello.o --emit-ir hello.ll  # LLVM IR
./build-win/bin/lex_dump.exe hello.pys                               # token 流
./build-win/bin/ast_dump.exe hello.pys                               # 语法树
```

排查代码生成问题时先看 `--emit-ir` 的输出。若结果在**开/关优化之间不一致**,
设 `XMEX_NO_OPT=1` 再编一次 —— 两者行为不同说明问题出在 IR 本身,
而不是优化器算错:

```bash
XMEX_NO_OPT=1 ./build-win/bin/xmexc.exe hello.pys -o hello-noopt.o
```

### 4. 跑测试

```bash
cd build && ctest --output-on-failure
```

用例分三层,失败在哪一层直接指出问题范围:

| 层 | 测什么 | 失败意味着 |
|---|---|---|
| `runtime_test` / `abi_test` / `jit_spike` | 运行时语义、ABI 边界 | 与代码生成无关,是运行时或 ABI 的问题 |
| `frontend_test` / `parse_*` | 词法、语法、AST | 前端问题 |
| `e2e_*` / `linktest` | 编译 → 链接 → 运行 → 比对 stdout | 唯一能暴露 IRGen 与 ABI 集成问题的一层 |

另有三组独立测试:

| 测试 | 测什么 | 特点 |
|---|---|---|
| `ffi_test` | 动态库 FFI 的 C++ 层 | 测一个**真实**的动态库(`tests/ffi_test_dll.cpp`),不是桩 |
| `ffi_demo` | FFI 端到端 | PyLite 脚本 → 编译 → 链接 → 加载 DLL → 调用 |
| `ffi_isolated_demo` | 隔离模式 | 核心验证点:DLL 段错误后**宿主必须活着** |
| `reconcile_test` | 调谐循环与副本组 | 用 `set_spawn_hook` 注入假 spawn,不 fork 真实进程,**Windows 上也能跑** |

加一个 e2e 用例:在 `tests/e2e/` 放 `名字.pys` 与 `名字.expected`,
再把名字加进 `CMakeLists.txt` 的 `E2E_CASES`。

### 5. 调用本机动态库(FFI)

`kv.ffi(path)` 在**当前进程**里加载一个 `.dll` / `.so`,并按动态库自己声明的签名调用
其中的函数。

```python
lib = kv.ffi("./xx.dll")

print(lib.symbols())            # 有哪些函数
print(lib.describe("init"))     # 某个函数的参数类型与返回类型
r = lib.call("init", [1, "abc"]) # {"ok":true,"value":42}
print(lib.path())
lib.unload()

print(kv.ffi_list())            # 所有已加载的库
```

**动态库侧要写两样东西**:一个描述符(声明有哪些函数、参数与返回类型)和一个
按固定调用约定导出的函数。

```c
// 1) 描述符:返回 JSON 文本的长度
int64_t xmex_ffi_describe(char* buf, int64_t cap);
//    {"functions":[{"name":"add","params":["int","int"],"ret":"int"}]}

// 2) 被调函数:统一调用约定
int64_t add(int64_t* args, int64_t nargs, int64_t* out) {
    out[0] = args[0] + args[1];
    return 0;    // 0 成功,非 0 是错误码
}
```

**为什么固定一个原型**:`dlsym` 拿到的裸指针没有类型信息,只能按已知原型调用。
要求对方按约定导出,就把「任意 C 签名」的不确定性挡在门外。代价是对方要写一层
薄封装 —— 一次性的,换来宿主侧不用为每个函数写胶水。

没有描述符时退化为「裸调用」模式:调用方自己给函数名,参数一律当 int64 传,
类型信息全丢。

完整契约(参数类型编码、字符串有效期、错误处理)见
[docs/ffi-design.md](docs/ffi-design.md)。可运行的例子:
[examples/ffi_demo.pys](examples/ffi_demo.pys)。

### 6. 隔离模式:DLL 崩溃不带崩宿主

`kv.ffi_isolated(path)` 把 DLL 加载到**子进程**,宿主只发消息收消息。

```python
lib = kv.ffi_isolated("./xx.dll")
r = lib.call("add", [1, 2])       # {"ok":true,"value":3}
r = lib.call("crash", [])         # {"ok":false,"error":"worker crashed (exit code 139)"}
print("宿主还活着")                # ← 确实还活着
```

| | `kv.ffi` | `kv.ffi_isolated` |
|---|---|---|
| DLL 加载在哪 | 宿主进程 | 子进程 |
| DLL 崩溃 | **宿主一起死** | 只有子进程死,宿主拿到退出码 |
| 性能 | 直接调用,无序列化 | 每次调用一次管道往返 + JSON 序列化 |
| 回调 | 支持 | **不支持**(2026-10 第一版) |

隔离模式用独立可执行文件 `xmex_ffi_agent` 做代理,两边用管道通信、一行一条 JSON。
这个协议与将来的**远程 agent 完全一致** —— 远程只是换个连接方式。

例子:[examples/ffi_isolated_demo.pys](examples/ffi_isolated_demo.pys)。

**把 agent 当成被纳管的进程**(Linux,与调谐循环组合):

```python
kv.add("ffi-worker-1", "/path/bin/xmex_ffi_agent",
       ["/path/to/lib.so", "--ready-file", "/tmp/ffi.ready"], "/tmp/ffi", "")
kv.reconcile_start(1000)
```

agent 启动时先加载 DLL 再进请求循环,加载失败以退出码 3 退出 —— 调谐下一轮
就会看到「进程没了」并重新拉起。`--ready-file` 让纳管方能判断「DLL 已就绪」
而不只是「进程还活着」。

### 7. 测试 Docker 部署的服务(Linux)

容器节点与 `kv.add` 平行,但作用于 Docker 容器。三种故障注入:

```python
# 拉起容器(可带资源限额),纳入节点表
kv.container_add("mq-1", "rabbitmq:3-management", "rabbitmq-server", "0.5", "256m")
kv.reconcile_start(2000)

kv.container_crash("mq-1")     # docker kill → 调谐自动重新 run
kv.container_pause("mq-1")     # docker pause → 冻结,模拟服务不可用
kv.container_unpause("mq-1")   # 解冻
kv.container_logs("mq-1")      # 容器日志
kv.erase("mq-1")
```

| 手段 | 进程节点 | 容器节点 |
|---|---|---|
| 拉起 | `fork` + `execv` | `docker run -d` |
| 判活 | `waitpid(WNOHANG)` | `docker inspect --format {{.State.Status}}` |
| 暂停 | `SIGSTOP` | `docker pause` |
| 崩溃 | `kill(pid, SIGKILL)` | `docker kill` |
| 限额 | 未实现 | `--cpus` / `--memory` |

**为什么不能用进程那套去操作容器**:容器里的进程宿主机 kill 不到。拿到容器
init 的 pid 也没用——那是容器命名空间里的 pid,SIGKILL 它要么无效要么误杀同名
进程。用错手段会得到误导性的结论(`SIGSTOP` 后容器照常服务,看着像"注入失败",
其实是信号发错了地方)。

`docker pause` 冻结容器时报 `observed=suspended` 而**不是** `crashed`——否则
调谐会把它重启掉,故障注入的现场就被打破了。

完整例子:[examples/container_test_demo.pys](examples/container_test_demo.pys)。

### 8. 测接口(跨平台)

`http.*` 能打任意 URL,配合容器做故障注入:

```python
r = http.get("http://api.example.com:8080/health", 3000)
print(r)                  # {"ok":true,"status":200,"elapsed_ms":12,"body":"..."}

http.post(url, "{\"name\":\"alice\"}", 3000)
http.head(url, 2000)                            # 探活:有状态码与头,无 body
http.request("DELETE", url, "", 3000)
http.request("GET", url, "", 5000, "token", {"X-Api-Key": "k"})   # 自定义头
```

| | 进程 | 容器 |
|---|---|---|
| 判活 | `waitpid(WNOHANG)` | `docker inspect {{.State.Status}}` |
| 暂停 | `SIGSTOP` | `docker pause` |
| 崩溃 | `kill(pid, SIGKILL)` | `docker kill` |
| 限额 | 未实现 | `--cpus` / `--memory` |

配起来测(见 `examples/http_demo.pys`):

```python
kv.container_add("api", "nginx:alpine", "", "0.5", "128m")
kv.reconcile_start(1000)
print(http.get("http://127.0.0.1:8080/ping"))   # 200
kv.container_pause("api")
print(http.get("http://127.0.0.1:8080/ping"))   # 超时(服务冻结了)
kv.container_unpause("api")
kv.container_crash("api")
sleep_ms(3000)
print(http.get("http://127.0.0.1:8080/ping"))   # 调谐重启后恢复 200
```

**刻意不做**:连接池、异步、**自动重试**、**重定向跟随**。测试要"每一步看得见"
——自动重试会把刚注入的故障冲掉,重定向跟随会把"返回了 302"这个事实藏起来。

细节见 [docs/http-design.md](docs/http-design.md)。

### 9. 进程纳管与调谐(Linux)

`kv.add` 纳管一个**可执行文件** —— 和 `kv.ffi` 加载动态库是两回事,别混用。

```python
kv.add("node-1", "/path/to/worker", ["--port", "9001"], "/tmp/wd", "127.0.0.1:9001")
kv.sleep("node-1")      # SIGSTOP —— 模拟卡死/网络分区
kv.wakeup("node-1")     # SIGCONT
kv.skip("node-1")       # 标记 LOST,进程保留(数据不丢)
kv.erase("node-1")      # 彻底下线
```

**声明式调谐**:启动调谐循环后,进程崩了会自动拉起,`desired=ABSENT` 的会被摘除。
每一轮都是幂等的。

```python
kv.reconcile_start(1000)     # 周期 1 秒
kv.reconcile_status()        # {"running":true,"nodes":N,"groups":G}
kv.reconcile_once()          # 手动跑一轮,返回详细报告
```

**副本组**:声明「这一组要几个副本」,成员不健康时自动补足。

```python
kv.group_create("shard-1", 3, "/path/to/worker", "", "127.0.0.1:9200")
kv.group_add("shard-1", "node-a")     # 纳入已纳管的节点
kv.group_replicas("shard-1", 5)       # 改期望副本数
kv.group_status("shard-1")
```

设计与实现要点见 [docs/distributed-reconcile-design.md](docs/distributed-reconcile-design.md)。

### 常见问题

| 现象 | 原因 |
|---|---|
| 启动时报缺少 `libxmex_runtime.dll` | 没在 UCRT64 shell 里跑,或没把 `build-win/bin` 加进 PATH |
| 链接报 `undefined reference to 'add'` | 忘了 `-lxmex_runtime` |
| 编译成功、链接成功,一运行就段错误 | 几乎总是 ABI 边界问题 —— 见下节第 1 点 |
| 中文输出在测试里比对失败 | 比对框架只支持 ASCII 输出,见 `tests/compare_output.cmake` |
| `.pys` 里有中文注释就编译失败 | 词法器不接受非 ASCII 字符,`.pys` 的注释必须用英文 |
| `kv.ffi` 报 `symbol not found: xxx` | 动态库的符号被 C++ mangle 了,导出函数必须写在 `extern "C"` 里 |
| `kv.ffi` 报 `dlopen failed` | 路径不对,或依赖的 DLL 不在同目录(Windows 上要放在 exe 旁边) |
| `kv.ffi_isolated` 卡住不动 | 默认 5 秒超时。若仍卡住,检查是否在超时前就 spawn 失败(看 stderr) |
| `kv.ffi_isolated` 报 `worker crashed` | 这是**预期行为** —— DLL 崩了,只有子进程死,宿主活着 |
| `http.get` 报 `connection failed` 且耗时≈timeout | 正常,连不上要等满超时。端口不通的话立刻返回才是意外 |
| `http.*` 报 `https not supported` | Windows 构建没链 OpenSSL。用 http 代替,或把服务跑在 Linux 侧 |
| 加了可选参数后编译报 `Incorrect number of arguments` | 忘了在 `irgen.cpp` 的 `builtinOptionalArgs` 里登记元数 |
| `kv.add` / `kv.group_*` 报未定义 | 这些是 **Linux only**,Windows 上没有(见「平台差异」) |
| `kv.reconcile_*` 报未定义 | 同上,Linux only |
| `kv.container_*` 报未定义 | Linux only,且需要 docker CLI 与 daemon 权限 |
| `kv.container_pause` 报 `not a container node` | 该节点是用 `kv.add` 建的进程节点,不是容器 |
| `kv.container_*` 报 `docker run failed` | 看错误里的 docker 原始输出,常见是镜像名错或 daemon 没起 |

## 目录结构

| 路径 | 内容 |
|---|---|
| `frontend/` | 词法、语法、AST —— **不依赖 LLVM**,可独立测试 |
| `irgen/` | AST → LLVM IR,含集中管理 ABI 约定的 `abi.h` |
| `aot/` | 目标文件 / IR 文本生成 |
| `runtime/` | 运行时库(`libxmex_runtime.dll`),由 GCC 编译 |
| `include/xmex/value.h` | `xmex::Value` —— 整个项目的 ABI 基石 |
| `include/xmex/value_tags.h` | tag 常量(与 value.h 分开,理由见该文件) |
| `include/xmex/platform.h` | 跨平台兼容层:动态库、管道、socket、线程、原子 |
| `docs/llvm-notes.md` | LLVM 集成的实测结论,**改代码前先读** |
| `docs/language.md` | 支持的语言子集与已知限制 |
| `docs/ffi-design.md` | 动态库 FFI 的契约与设计 |
| `docs/distributed-reconcile-design.md` | 调谐循环与副本组的设计 |
| `docs/raft-removal.md` | 为什么移除了自研 raft,以及怎么拿回来 |
| `examples/mixed/` | **混合语言完整示例**:C++ 宿主 + Xmex 业务逻辑,附[说明文档](examples/mixed/README.md) |
| `examples/ffi_demo.pys` | FFI 端到端示例(进程内) |
| `examples/ffi_isolated_demo.pys` | FFI 隔离模式示例(DLL 崩溃不带崩宿主) |
| `tests/e2e/` | 端到端用例:`*.pys` + `*.expected`(可选 `*.in`) |
| `tests/linktest/` | 手写 C++ 链接编译产物的集成测试 |
| `tests/ffi_test.cpp` | FFI 单元测试(测一个**真实**的动态库,不是桩) |
| `tests/reconcile_test.cpp` | 调谐循环与副本组测试(注入模式,不 fork 真实进程) |
| `xmex_web/` | 通用挂载协议:MySQL / OJ / Docker / MQ / MID / CloudJudge 六后端统一接口 |
| `third_party/adviskv/` | 内置的分布式 KV 存储(C++17,含真 Raft) |

运行时按关注点分文件,每个文件一个对象类型或一类操作:

| 文件 | 内容 |
|---|---|
| `runtime/gc.cpp` | 保守式标记-清除垃圾回收器(取代了原先的 bump 分配器) |
| `runtime/arith.cpp` | 算术、比较、真值判定(含字符串/列表的比较与拼接) |
| `runtime/string.cpp` | 不可变字符串 |
| `runtime/list.cpp` | 可变列表 |
| `runtime/dict.cpp` | 插入有序字典 |
| `runtime/tuple.cpp` | 元组(承载多返回值) |
| `runtime/index.cpp` | 下标/切片/`len` 的按类型分派 |
| `runtime/method.cpp` | 方法表与 `call_method` 分派 |
| `runtime/io.cpp` | `print`/`input` 与 repr 渲染 |
| `runtime/ffi.cpp` | 动态库加载、描述符解析、类型化调用 |
| `runtime/ffi_api.cpp` | `kv.ffi()` / `kv.ffi_isolated()` 与实例方法注册 |
| `runtime/ffi_worker.cpp` | 隔离模式的 agent 请求循环 |
| `runtime/platform_proc.cpp` | 跨平台管道与子进程 spawn |
| `runtime/kv_admin.cpp` | 节点生命周期、故障注入、调谐循环、副本组(**Linux only**) |

## 两个务必先读的点

1. **[docs/llvm-notes.md](docs/llvm-notes.md) 第 2 节:`xmex::Value` 绝不能按值跨 ABI 边界。**
   Win64 规定大于 8 字节的聚合体按引用传递。在 IR 里按值声明会**链接通过、
   lookup 成功、一执行就段错误**。所有运行时调用都要经过 `irgen/abi.h`。

2. **运行时必须是 DLL,不能是静态库。** JIT 生成的代码要在运行时解析 `add`
   这类符号,而 Windows 上 MinGW 链接出的 exe 几乎不导出任何符号。

## 内存回收

运行时带一个**保守式标记-清除**回收器(`runtime/gc.cpp`)。分配量越过阈值时
自动回收(阈值按上次存活量自适应,下限 1MB),也能用 `gc_collect()` 手动触发;
`XMEX_GC_STATS=1` 打印统计,`XMEX_GC_STRESS=1` 每次分配都回收(压测用)。

它不需要 IRGen 做任何配合 —— 根就是**栈与寄存器**,靠保守扫描找到。这能成立,
是因为本项目本来就有的两条约定:

1. 只有容器是堆对象,标量是内联的、根本不分配;
2. 每个 `xmex::Value` 都住在栈上的 alloca 里,而且槽位地址几乎都会传给 DLL 里的
   不透明运行时函数,所以 LLVM 的 memopt 提升不掉它们。

代价写在 [docs/language.md](docs/language.md) 里:不压缩(会有碎片)、偶尔误保留、
**根必须放在栈上**(全局 / `static` / `thread_local` 里放 `xmex::Value` 不算根)。

⚠️ **多线程下 GC 不安全**:它只扫**当前线程**的栈与寄存器,其他线程持有的
`xmex::Value` 可能被误回收。`runtime/concurrency.cpp` 有 26 个线程/进程/同步原语,
但语言层没有入口 —— 用了它们要先解决 GC 的跨线程根登记。

## 已知限制

### 语言

- 没有异常:运行时错误打印消息后 `exit(1)`
- 垃圾回收不压缩,长时间运行会有内存碎片
- 字典是线性扫描,查找 O(n)
- 方法名拼错是**运行时**错误(类型注解不做检查,`x.upper()` 里的 x 编译期不知道类型)
- 比较不支持 Python 的链式写法(`a < b < c`)
- `a[i] += 1` 这类复合赋值尚未支持(`a[i] = ...` 支持)
- 整数是 64 位,不会自动提升为任意精度
- 没有 `import`、生成器、推导式、JIT REPL
- **`.pys` 源文件里不能出现非 ASCII 字符** —— 词法器不接受,中文注释会报
  「无法识别的字串」

支持类(`class`):单继承、`__init__`、属性读写、方法调用,实例属性表复用字典实现。

### 动态库 FFI

- **必须按固定调用约定导出**:`int64_t fn(int64_t*, int64_t, int64_t*)`。
  任意 C 签名不支持 —— `dlsym` 拿到的裸指针没有类型信息
- **隔离模式不支持回调**(2026-10 第一版)。回调要跨进程反向通道
- 隔离模式有 **5 秒超时**(编译期 `XMEX_FFI_TIMEOUT_MS` 可改),超时后硬杀 worker。
  退出码记 `-124` 以区别于真实崩溃码(139/134/...)
- **agent 被 `kv.add` 纳管时没有就绪判据的闭环**:调谐只看「pid 还活着」,
  要判「DLL 已就绪」得自己查 `--ready-file` 或发 `ping`
- 跨进程时字符串要序列化:进程内传的是指针,跨进程必须传内容
- 动态库崩了退出码统一成 POSIX 风格(128+signal),Windows 的 NTSTATUS 已折算

### HTTP 客户端

- **https 只在 Linux 可用**。OpenSSL 只在那个平台链接;Windows 上 https 会明确
  报"不支持"而不是静默降级成明文
- 超时覆盖连接与读响应两个阶段,但**不含 DNS 解析**(Windows 上 `getaddrinfo`
  是阻塞调用,域名解析慢时不受 timeout_ms 约束)
- 不支持 multipart/form-data;要传文件得自己拼 body 并设 `Content-Type`
- **加可选参数的内置函数时要同步改 `builtinOptionalArgs`**(见
  [docs/http-design.md](docs/http-design.md) 第 2.4 节)—— irgen 按调用点的
  实参个数生成 IR,不补齐会直接编译失败

### 分布式

- `kv.add` / 调谐循环 / 副本组**仅 Linux**(实现在 `runtime/kv_admin.cpp`)
- Redis 持久化仍存 `state` 而非 `desired` —— 进程重启会丢节点意图(已知待修)
- 副本组只支持"补足期望副本数",不支持缩容时自动摘除多余成员
- **容器节点只支持 `--cpus`/`--memory` 两种限额**,不支持 `--cpuset-cpus`
  / `--network` / `--volumes` 等参数(传了会被忽略)
- 容器节点需要 PATH 上有 docker CLI 且当前用户能访问 daemon;容器内进程
  的日志要通过 `docker logs` 拿,拿不到宿主机的 stdout
- `lsm_client` 名字叫 LSM,实际是手写的 Redis 协议客户端(历史命名债)

完整的支持清单与语义细节见 [docs/language.md](docs/language.md)。
