# 混合语言示例:C++ 宿主 + Xmex 业务逻辑

写给想把 Xmex 嵌入自己 C++ 程序的人。

这个目录演示本项目的核心诉求:**把 Xmex 代码在编译期编进 C++ 程序**。
`main.cpp` 完全不知道 Xmex 的语法,它只是调用几个 `extern "C"` 函数,而那些
函数的实现由 `scores.pys` 编译而来。产出是一个普通的 `.exe`,**运行时不需要
解释器,也不需要分发 `.pys` 文件**。

| 文件 | 角色 |
|---|---|
| [scores.pys](scores.pys) | 业务逻辑:夹取越界值、求和计数、评级、求平均 |
| [main.cpp](main.cpp) | 宿主:C++ 造数据 → 调 Xmex → 回收结果并打印 |

## 构建与运行

在 **UCRT64 shell** 里(见根目录 README 的构建说明):

```bash
# 1. 把 .pys 编成目标文件
./build-win/bin/xmexc.exe examples/mixed/scores.pys -o examples/mixed/scores.o

# 2. 编译并链接 C++ 宿主
g++ -std=c++20 examples/mixed/main.cpp examples/mixed/scores.o \
    -o examples/mixed/mixed.exe -I./include -L./build-win/bin -lxmex_runtime

# 3. 把运行时 DLL 放到 exe 旁边
#    ⚠️ 不做这一步的话,直接运行会**静默退出、没有任何输出** ——
#    Windows 找不到 libxmex_runtime.dll,而它是运行时库,缺了连启动都过不去。
cp ./build-win/bin/libxmex_runtime.dll examples/mixed/

# 4. 运行
./examples/mixed/mixed.exe
```

产物(`scores.o`、`mixed.exe`、`libxmex_runtime.dll`)都落在 `examples/mixed/`
下,和源码放一起。`*.o` / `*.exe` / `*.dll` 都在 .gitignore 里,不会误提交。

> **为什么第 3 步不能省**:`g++` 链接时用 `-L./build-win/bin` 找到了导入库
> `libxmex_runtime.dll.a`(那只是链接期的符号表),但**运行时**需要的是
> `libxmex_runtime.dll` 本身。Windows 只在 exe 所在目录和 PATH 里找 DLL,
> 而 `examples/mixed/` 两者都没有 —— 于是进程在加载阶段就死了。
>
> 这也是为什么 CMake 那条路线没这个问题:CMake 把 exe 和 DLL 都放在
> `build-win/bin/` 里。

`-I./include` 是为了 `#include "xmex/runtime.h"`,`-L./build-win/bin` 是为了找到
导入库 `libxmex_runtime.dll.a`,运行期还需要 `libxmex_runtime.dll` 本身 ——
把 `./build-win/bin` 加进 PATH 最省事。

### 两条路线,同一个程序

上面三条命令是**手工路线**,`xmexc` 和 `g++` 各跑一次,产物叫
`examples/mixed/mixed.exe`。

这个示例同时也接进了项目的 CMake,所以还有**自动路线**:

```bash
cmake --build build-win && ./build-win/bin/mixed_demo.exe
```

两条路线编出来的是**同一个程序**(输出逐字节一致),只是产物名字和位置不同:

| 路线 | 产物 | 适合 |
|---|---|---|
| 手工(上面三步) | `examples/mixed/mixed.exe` | 只想单独试这个例子,不碰项目其余部分 |
| CMake 目标 | `build-win/bin/mixed_demo.exe` | 改完源码一条命令重建,也随 `ctest` 一起验证 |

## 运行结果

```
=== C++ 侧开始 ===

[pys] scores.pys 顶层执行
原始分数:          [55, 120, 88, -10, 93, 61]
Xmex 夹取后:      [55, 100, 88, 0, 93, 61]
原始列表(未改动): [55, 120, 88, -10, 93, 61]
Xmex 返回的元组:      total=397 count=6
平均分(float):          66.1667

  alice(93) → alice -> A
  bob(61) → bob -> B
  carol(55) → carol -> C

=== C++ 侧结束 ===
```

## 符号命名规则

| `scores.pys` 里的东西 | 生成的符号 |
|---|---|
| `def clamp_all(xs, lo, hi)` | `xmex_scores_clamp_all` |
| 顶层语句 | `xmex_scores_main` |

模块名默认取输入文件的主文件名,可用 `xmexc -I <名称>` 覆盖。符号名里的
非字母数字字符会被换成下划线。

## ⚠️ ABI:所有 lite::Value 都按指针收发

这是**唯一一处必须照着写**的地方,写错了会「编译通过、链接通过、一运行就段错误」。

```cpp
// ✅ 正确
extern "C" void xmex_scores_clamp_all(lite::Value *ret, lite::Value *xs,
                                        lite::Value *lo, lite::Value *hi);
lite::Value clamped;
xmex_scores_clamp_all(&clamped, &xs, &lo, &hi);

// ❌ 错误:按值声明
extern "C" lite::Value xmex_scores_clamp_all(lite::Value xs, lite::Value lo, lite::Value hi);
```

原因是 Win64 ABI 规定**大于 8 字节的聚合体按引用传递**,`lite::Value` 是 16 字节。
GCC 编译运行时时遵循这条规则(参数走 `RDX`/`R8`,返回值走隐藏的 `RCX` sret 指针),
而 LLVM 按值声明会挑另一套降低方式,两边对不上。

完整分析见 [docs/llvm-notes.md](../../docs/llvm-notes.md) 第 2 节。**规律很简单:
`lite::Value` 出现在参数或返回值里,一律是指针;返回值写在第一个参数里。**

## 数据怎么进去、怎么出来

`lite::Value` 是 16 字节的 `{int32 tag; int64 payload}`([include/xmex/value.h](../../include/xmex/value.h))。

**构造标量**(`value.h` 里的 inline 函数,不分配内存):

```cpp
lite::Value a = int_(42);
lite::Value b = float_(3.14);
lite::Value c = bool_(true);
lite::Value d = none_();
```

**构造容器**(运行时函数,会分配内存 —— 记得看下面的 GC 一节):

```cpp
lite::Value elems[3] = {int_(1), int_(2), int_(3)};
lite::Value xs = list_new(elems, 3);

const char *s = "hello";
lite::Value str = str_new(s, 5);          // ⚠️ 长度必须显式给
```

**读取标量**(inline,调用方负责确认 tag 对 —— 运行时不做隐式类型检查):

```cpp
int64_t n = as_int(v);
double  f = as_float(v);
bool    b = as_bool(v);
```

**读取字符串**:

```cpp
const char *p = str_data(&v);
int64_t len   = str_size(&v);         // ⚠️ 长度用这个,不要用 strlen
std::fwrite(p, 1, len, stdout);
```

> 字符串里可以含内嵌的 `'\0'`(来自 `"\0"` 转义),用 `strlen` 会在那里截断。
> `str_size` 才是权威长度。

**解包多返回值**。Xmex 的 `return a, b` 返回一个元组:

```cpp
lite::Value summary;
xmex_scores_total_and_count(&summary, &clamped);

lite::Value parts[2];
unpack(&summary, parts, 2);           // 元素个数不匹配会报错并退出
int64_t total = as_int(parts[0]);
int64_t count = as_int(parts[1]);
```

**打印**。`print_` 收的是数组指针,和语言里的 `print(a, b)` 对应:

```cpp
lite::Value one[1] = {v};
print_(one, 1);
```

更多访问器见 [include/xmex/runtime.h](../../include/xmex/runtime.h):
`list_get` / `list_len` / `dict_get` / `len` / `index_` / `slice` …

## 内存:GC 只认栈上的引用

运行时带一个保守式标记-清除回收器,**根就是当前线程的栈与寄存器**。所以:

```cpp
// ✅ 局部变量 —— 在栈上,会被当作根
lite::Value xs = list_new(raw, 6);

// ❌ 全局 / static / thread_local —— 不在栈上,**不算根**
static lite::Value g_xs;          // 它指向的对象会被回收掉
```

如果你确实需要把引用放在别处(比如自己 malloc 的结构里),用
`gc_root_push(&slot)` / `gc_root_pop(1)` 显式登记。细节见
[docs/language.md](../../docs/language.md) 的「内存回收」一节。

顺带一提,这也意味着:**Xmex 侧不要用顶层语句存放长期数据**。顶层语句编译进
`xmex_scores_main()`,它的局部变量在函数返回后就没了 —— 本示例的 `scores.pys`
顶层只打了一行日志。

## 能力边界

**能做到:**

- C++ 造 `lite::Value`(标量、字符串、列表、字典、元组)、传给 Xmex 函数
- Xmex 返回任意值,包括多返回值(元组)与字符串,C++ 侧取出来随便用
- 两边共享同一个堆与同一个 GC,传递的是值、没有序列化开销
- 运行时错误(除零、下标越界、字典缺键)会打印消息到 stderr 并以退出码 1 结束
  进程,不会把栈破坏掉再继续

**做不到:**

- **Xmex 不能反过来调用 C++ 函数。** 语言里没有 `extern` / `import` /
  函数指针,`genCall` 只解析本模块 `def` 出来的符号(见
  [irgen.cpp](../../irgen/irgen.cpp) 的 `symbolFor`)。所以调用方向是单向的:
  C++ 驱动、Xmex 计算。要双向,得在语言层加一个声明外部函数的语法。
- **类型注解不做检查。** `def f(a: int)` 里的 `int` 只存进 AST,不影响代码生成。
- **没有异常。** 运行时错误直接结束进程,`try` / `except` 不存在。

## 常见坑

| 现象 | 原因 |
|---|---|
| 启动时报缺少 `libxmex_runtime.dll` | 没把 `./build-win/bin` 加进 PATH |
| `undefined reference to 'xmex_scores_clamp_all'` | 忘了把 `scores.o` 加进链接命令;或 `-I` 指定的模块名和符号对不上 |
| 编译报找不到 `xmex/runtime.h` | 少了 `-I./include` |
| 段错误,且发生在第一次调用时 | ABI 写错了 —— 检查是不是把 `lite::Value` 按值声明了 |
| Xmex 返回的字符串被截断 | 用了 `strlen` 而不是 `str_size` |
| 全局变量里的 `lite::Value` 突然变成垃圾 | GC 不认全局变量当根,见上文 |

## 另见

- [tests/linktest/](../../tests/linktest/) —— 同样的思路,但只是最小冒烟测试
- [docs/language.md](../../docs/language.md) —— Xmex 语言参考与已知限制
- [docs/llvm-notes.md](../../docs/llvm-notes.md) —— ABI 约定与 LLVM 集成的实测结论
