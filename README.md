# PyLite

一个用 LLVM 实现的 Python 风格小语言,能**在编译期编译成目标文件、与 C++ 一起链接**,
同一套前端也驱动 **ORC JIT** 做运行时执行。

```python
a, b = input(int, int)
for i in range(1, a):
    print(i)
```

这行代码不是合法 C++ —— 逗号表达式加 `=` 在语法分析阶段就不成立,模板元编程和
运算符重载都在它之后才生效,救不了。所以必须有一个前端把它降成 LLVM IR。

## 构建

需要 MSYS2 的 UCRT64 环境(不是 MSVCRT 的那个 MinGW):

```bash
cd /d/pylite
bash build-windows.sh              # 等价于 configure + 构建 + ctest,构建目录 build-win/
```

脚本会先把 PATH 修好再构建 —— 这一步不是可有可无的便利,见下。

想手工敲命令的话:

```bash
# 在 D:\msys64\ucrt64.exe 打开的 shell 里
cd /d/pylite
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
configure → 构建(要跑 pylitec.exe)→ ctest(要加载 libLLVM-22.dll)全程。

**Linux 构建不受影响**:`CMakeLists.txt` 里平台差异都在 `if(WIN32)`/`else()` 分支内,
`else()` 侧与原先一致;13 个 POSIX 专有模块(sandbox/cgroup/server/cluster 等)仅在
Linux 下参与构建。raft 三件(raft/raft_api/distributed_adapter)是自足的,两个平台都编。

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
./build-win/bin/pylitec.exe hello.pys -o hello.o
```

产出的是普通 COFF 目标文件。符号命名规则:

| 源文件里的东西 | 生成的符号 |
|---|---|
| 顶层语句 | `pylite_<模块名>_main` |
| `def foo(...)` | `pylite_<模块名>_foo` |

模块名默认取输入文件的主文件名,可用 `-I <名称>` 覆盖。写一个最小驱动就能跑:

```cpp
// main.cpp
extern "C" void pylite_hello_main();   // 按 ABI 约定,参数/返回值都是 lite::Value*
int main() {
  pylite_hello_main();
  return 0;
}
```

```bash
g++ -std=c++20 main.cpp hello.o -o hello.exe -L./build-win/bin -lpylite_runtime
./hello.exe
# 1 1
# 2 4
# 3 9
```

`-L./build-win/bin` 是为了让链接器找到导入库 `libpylite_runtime.dll.a`,
运行时则需要 `libpylite_runtime.dll` 在 `PATH` 上(它与 `pylitec.exe` 同在
`build-win/bin/`,把那个目录加进 PATH 最省事)。

调用带参数的 PyLite 函数时,所有 `lite::Value` 都**按指针**收发 ——
这是 Win64 ABI 的硬性要求,详见下面的"两个务必先读的点"。返回值也走第一个
指针参数(隐藏的 sret):

```cpp
#include "pylite/runtime.h"   // 编译时要加 -I./include

// pylitec 为 `def add(a, b): return a + b` 生成的符号
extern "C" void pylite_math_add(lite::Value *ret, lite::Value *a, lite::Value *b);

lite::Value a = int_(1), b = int_(2), r;
pylite_math_add(&r, &a, &b);     // r 里是 3
```

```bash
g++ -std=c++20 main.cpp math.o -o math.exe \
    -I./include -L./build-win/bin -lpylite_runtime
```

想直接看一个**完整的混合程序**(C++ 造数据 → PyLite 算 → C++ 收结果,
含字符串与多返回值),见 [examples/mixed/](examples/mixed/README.md)。

### 3. 查看中间产物

```bash
./build-win/bin/pylitec.exe hello.pys -o hello.o --emit-ir hello.ll  # LLVM IR
./build-win/bin/lex_dump.exe hello.pys                               # token 流
./build-win/bin/ast_dump.exe hello.pys                               # 语法树
```

排查代码生成问题时先看 `--emit-ir` 的输出。若结果在**开/关优化之间不一致**,
设 `PYLITE_NO_OPT=1` 再编一次 —— 两者行为不同说明问题出在 IR 本身,
而不是优化器算错:

```bash
PYLITE_NO_OPT=1 ./build-win/bin/pylitec.exe hello.pys -o hello-noopt.o
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

加一个 e2e 用例:在 `tests/e2e/` 放 `名字.pys` 与 `名字.expected`,
再把名字加进 `CMakeLists.txt` 的 `E2E_CASES`。

### 常见问题

| 现象 | 原因 |
|---|---|
| 启动时报缺少 `libpylite_runtime.dll` | 没在 UCRT64 shell 里跑,或没把 `build-win/bin` 加进 PATH |
| 链接报 `undefined reference to 'add'` | 忘了 `-lpylite_runtime` |
| 编译成功、链接成功,一运行就段错误 | 几乎总是 ABI 边界问题 —— 见下节第 1 点 |
| 中文输出在测试里比对失败 | 比对框架只支持 ASCII 输出,见 `tests/compare_output.cmake` |

## 目录结构

| 路径 | 内容 |
|---|---|
| `frontend/` | 词法、语法、AST —— **不依赖 LLVM**,可独立测试 |
| `irgen/` | AST → LLVM IR,含集中管理 ABI 约定的 `abi.h` |
| `aot/` | 目标文件 / IR 文本生成 |
| `runtime/` | 运行时库(`libpylite_runtime.dll`),由 GCC 编译 |
| `include/pylite/value.h` | `lite::Value` —— 整个项目的 ABI 基石 |
| `docs/llvm-notes.md` | LLVM 集成的实测结论,**改代码前先读** |
| `docs/language.md` | 支持的语言子集与已知限制 |
| `examples/mixed/` | **混合语言完整示例**:C++ 宿主 + PyLite 业务逻辑,附[说明文档](examples/mixed/README.md) |
| `tests/e2e/` | 端到端用例:`*.pys` + `*.expected`(可选 `*.in`) |
| `tests/linktest/` | 手写 C++ 链接编译产物的集成测试 |

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

## 两个务必先读的点

1. **[docs/llvm-notes.md](docs/llvm-notes.md) 第 2 节:lite::Value 绝不能按值跨 ABI 边界。**
   Win64 规定大于 8 字节的聚合体按引用传递。在 IR 里按值声明会**链接通过、
   lookup 成功、一执行就段错误**。所有运行时调用都要经过 `irgen/abi.h`。

2. **运行时必须是 DLL,不能是静态库。** JIT 生成的代码要在运行时解析 `add`
   这类符号,而 Windows 上 MinGW 链接出的 exe 几乎不导出任何符号。

## 内存回收

运行时带一个**保守式标记-清除**回收器(`runtime/gc.cpp`)。分配量越过阈值时
自动回收(阈值按上次存活量自适应,下限 1MB),也能用 `gc_collect()` 手动触发;
`PYLITE_GC_STATS=1` 打印统计,`PYLITE_GC_STRESS=1` 每次分配都回收(压测用)。

它不需要 IRGen 做任何配合 —— 根就是**栈与寄存器**,靠保守扫描找到。这能成立,
是因为本项目本来就有的两条约定:

1. 只有容器是堆对象,标量是内联的、根本不分配;
2. 每个 `lite::Value` 都住在栈上的 alloca 里,而且槽位地址几乎都会传给 DLL 里的
   不透明运行时函数,所以 LLVM 的 memopt 提升不掉它们。

代价写在 [docs/language.md](docs/language.md) 里:不压缩(会有碎片)、偶尔误保留、
**根必须放在栈上**(全局 / `static` / `thread_local` 里放 `lite::Value` 不算根)。

## 已知限制

- 没有异常:运行时错误打印消息后 `exit(1)`
- 垃圾回收不压缩,长时间运行会有内存碎片
- 字典是线性扫描,查找 O(n)
- 方法名拼错是**运行时**错误(类型注解不做检查,`x.upper()` 里的 x 编译期不知道类型)
- 比较不支持 Python 的链式写法(`a < b < c`)
- `a[i] += 1` 这类复合赋值尚未支持(`a[i] = ...` 支持)
- 整数是 64 位,不会自动提升为任意精度
- 支持类（`class`）：单继承、`__init__`、属性读写、方法调用，实例属性表复用字典实现
- 没有异常、`import`、生成器、推导式、JIT REPL

完整的支持清单与语义细节见 [docs/language.md](docs/language.md)。
