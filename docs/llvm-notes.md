# LLVM 集成笔记

本文记录在本机环境(Windows 11 + MSYS2 UCRT64 + LLVM 22.1.8)上实测得出的
结论。**每一条都是实测的,不是推测的** —— 与 LLVM 官方文档或第三方教程冲突时,
以本文为准。

## 1. 环境事实

| 项目 | 值 |
|---|---|
| 编译器 | `D:\msys64\ucrt64\bin\g++.exe`,**16.2.0**,target `x86_64-w64-mingw32` |
| LLVM | **22.1.8**(`mingw-w64-ucrt-x86_64-llvm`) |
| `llvm-config --cmakedir` | `D:/msys64/ucrt64/lib/cmake/llvm` |
| `llvm-config --shared-mode` | **shared** —— 只有 `libLLVM-22.dll`(141MB) |
| 构建 shell | MSYS2 **UCRT64**,即 `D:\msys64\ucrt64.exe` |

### 严禁混用工具链

`D:\mingw64` 上另有一个 g++ 13.2.0,它是 **MSVCRT** 版(configure 路径含
`msvcrt-rt_v11`)。它与 UCRT64 的 LLVM 包 ABI 不兼容 —— 混用轻则链接报错,
重则堆和 `std::string` 静默损坏。

判断自己在哪个环境:
```bash
g++ --version | head -1     # 必须是 16.2.0;若显示 13.2.0 说明在错误的 shell 里
```

## 2. ⚠️ 最重要的一条:lite::Value 绝不能按值跨 ABI 边界

**这是实测踩到的坑,会直接导致段错误。**

### 现象

在 IR 里按值声明并调用:

```llvm
; ❌ 错误写法 —— 调用时必然段错误
declare %lite::Value @add(%lite::Value, %lite::Value)
%sum = call %lite::Value @add(%lite::Value %a, %lite::Value %b)
```

`lookup` 能成功,JIT 链接也没问题,但**一执行就 segfault**。

### 原因

Win64 ABI 规定:大于 8 字节的聚合体**按引用传递**。GCC 编译
`lite::Value add(lite::Value a, lite::Value b)` 时严格遵循这条规则。反汇编
`libxmex_runtime.dll` 里的 `add` 序言可以看到:

```asm
add:
    sub  $0x28,%rsp
    mov  0x8(%rdx),%r9     ; 解引用 RDX —— 说明 RDX 是指向 lite::Value 的指针
    mov  (%r8),%r10        ; 解引用 R8
    mov  %rcx,%rax         ; RCX 是 sret 返回缓冲区指针
    mov  (%rdx),%rcx
```

即 GCC 侧的约定是:

| 寄存器 | 含义 |
|---|---|
| `RCX` | sret 返回缓冲区指针 |
| `RDX` | `&a`(lite::Value 的指针) |
| `R8`  | `&b`(lite::Value 的指针) |

而按值声明时,LLVM 选用了**另一套**降低方式,两边对不上,于是崩溃。

### 正确写法

按 Win64 ABI 显式声明边界,把"按引用"和"sret"写进 IR:

```llvm
; ✅ 正确写法
declare void @add(ptr sret(%lite::Value), ptr, ptr)

%a.slot = alloca %lite::Value, align 8
%b.slot = alloca %lite::Value, align 8
%r.slot = alloca %lite::Value, align 8
store %lite::Value %a, ptr %a.slot, align 8
store %lite::Value %b, ptr %b.slot, align 8
call void @add(ptr sret(%lite::Value) %r.slot, ptr %a.slot, ptr %b.slot)
; 结果从 %r.slot 里 load 出来
```

对应的 LLVM C++ API:

```cpp
auto *Ptr = PointerType::getUnqual(Ctx);
Attribute SretAttr = Attribute::getWithStructRetType(Ctx, V);  // V = {i32,i64}

// 函数声明:void(ptr sret(%lite::Value), ptr, ptr)
auto *Fn = Function::Create(
    FunctionType::get(Type::getVoidTy(Ctx), {Ptr, Ptr, Ptr}, false),
    Function::ExternalLinkage, "add", *M);
Fn->addParamAttr(0, SretAttr);

// 调用点也要重复标一次(属性不会自动从声明传播到调用)
auto *C = B.CreateCall(Fn, {SlotR, SlotA, SlotB});
C->addParamAttr(0, SretAttr);
```

> **给 IRGen 的硬性要求**
>
> 1. 任何以 lite::Value 为参数或返回值的运行时函数,**一律**声明成
>    `void(ptr sret(%lite::Value)?, ptr..., ...)` —— 参数是"指向 lite::Value 的指针",
>    返回值走 sret。
> 2. 调用前把操作数 store 进 alloca,把 alloca 的指针传进去。
> 3. 标量参数(int64/指针/等)不受此影响,正常按值传。
> 4. 若某个函数要返回多个值,同样只用一个 sret 缓冲区,不要返回聚合体。
>
> 这条规则要固化进 `irgen/abi.h`,不能靠每个调用点自己记得。

### 为什么这个坑不容易发现

`lookup()` 成功、JIT 链接成功、IR 验证通过 —— 所有静态检查都显示"没问题",
只有真正执行时才炸。所以 `tests/jit_spike.cpp` 同时保留了一个不调用运行时的
对照函数 `jit_add_plain`:它证明 JIT 执行路径本身是好的,从而把问题精确锁定
在 ABI 边界上。

## 2.1 ⚠️ 一个运行时符号名只能对应一种签名

**与第 2 节同一类故障:静态检查全过,一执行就崩。**

`irgen/abi.h` 里的 `cache_` 是 `std::map<std::string, Function*>`,**只按函数名
缓存,不记签名**。于是:

```cpp
// 先在某个调用点按 (ptr, i64) -> void 建了 print_
// 之后另一个调用点又按 (ptr) -> i64 要它
// → cache_.find 命中,直接返回上面那个 Function*
// → 用错的 FunctionType 生成调用,而 LLVM 不报错
```

规避办法有两条,项目里都落实了:

1. **不要写死形状的 helper。** `Abi` 上那些 `callBinary` / `callUnary` /
   `callPtrI64` 只覆盖固定形状;混合指针与标量的签名走
   `callN(B, fn, paramTypes, args)`,**参数类型必须显式给出**。想省事拼一个
   "全是 ptr" 的版本是不行的 —— 例如 `call_method(obj, name, nameLen, args,
   nargs)` 里有两个 i64,`dict_new(keys, vals, n)` 里有一个 i64。
2. **统一 get-or-create 入口。** 所有声明都过 `Abi::getOrDeclare()`:先查
   `cache_`,再查当前 Module,都没有才新建,三条路径的结果都写回 `cache_`,
   并校验签名一致 —— 不一致直接 `report_fatal_error`,而不是等到运行时崩。
   早先 `callPrint` / `callUnpack` 是绕过 `cache_`、直接
   `M_.getFunction()` + `Function::Create()` 的,一旦有别的路径也声明这两个
   名字,LLVM 会把后建的那个改名成 `print_.1`,而 DLL 里并没有这个符号。

顺带一条相关的:`len` 刻意返回 `int64_t` 而不是 `lite::Value`。除了 for 循环的
条件要直接拿 i64 比之外,也是为了避免"同一个 `len` 既要 lite::Value 版本又要
i64 版本" —— 那就正好撞上上面这个缓存陷阱。`len(x)` 的调用点自己用
`storeI64AsInt` 装箱。

## 2.2 ⚠️ alloca 必须放在函数入口块

**症状:循环跑几十万次之后进程直接消失,没有任何输出。**

LLVM 只把**函数入口块里**的 alloca 当静态栈槽 —— `AllocaInst::isStaticAlloca()`
除了要求大小是常量,还明确要求 `Parent == 入口块`。放在别处(典型是循环体里)
的 alloca 会被降低成**真正调整栈指针**的动态分配,每次执行都吃掉一份栈空间,
而且函数返回前不会归还。

于是这样一行代码就能踩中:

```python
for i in range(200000):
    total = total + i
```

`add` 的结果需要一个 16 字节的 `%lite::Value` 槽位,slot 又是按需分配的
(`Abi::newSlot`),于是循环体里出现了一个 alloca。实测:

| 迭代次数 | 结果 |
|---|---|
| 50000 | 正常 |
| 200000 | 崩溃(进程异常退出,无输出) |

**硬性要求:所有局部空间的分配都走 `Abi::allocEntry()`**(以及它的两个包装
`newSlot` / `newSlotArray`),它会把 alloca 放进当前函数的入口块。语义上仍然
正确 —— 每次函数调用都会新建,只是空间在序言里一次分配、循环里反复复用。

两个例外,都必须成对处理:

- `IRGen::declareVar` 除了 alloca 还要**紧挨着**写一次 None 初始化,所以它自己
  用临时 IRBuilder 把 alloca 和 store 一起放进入口块。顺序不能反 —— 在同一个
  基本块里引用后面才定义的 alloca 违反 SSA 支配关系,IR 验证会直接报错。
- 初始化必须是入口块里的那条 store,不能挪到当前插入点:变量可能在一个分支里
  声明、在另一个分支里被读,那时写 None 的指令根本没执行过。

回归用例是 `tests/e2e/long_loop.pys`(20 万次迭代,比 1MB 默认栈能撑住的量级大)。

## 2.3 垃圾回收为什么不需要 IRGen 配合

`runtime/gc.cpp` 是保守式标记-清除:根靠**扫描当前线程的栈与寄存器**找到,
IRGen 不需要生成任何根登记代码,ABI 一个字没动。这在这里行得通,靠的是本项目
本来就有的两条约定:

1. **只有容器是堆对象**,`int`/`float`/`bool`/`None` 是内联标量、根本不分配
   (`value.h`)。堆上全是 `lite::Str`/`lite::List`/`lite::Dict`/`lite::Tuple`。
2. **每个 lite::Value 都住在栈上的 alloca 里**,而且槽位地址几乎都会传给 DLL 里的
   不透明运行时函数(`add`、`print_`、`call_method`…)。

第 2 条是关键。`mem2reg`/`SROA` 只能提升"地址不逃逸"的 alloca,而这里槽位地址
**逃逸到了外部函数**,所以 LLVM 提升不掉它们 —— 在任何会触发回收的调用点上,
活着的值一定在内存里(调用者的栈槽,或调用者栈上的数组),而不是只在某个 SSA
寄存器里。

### 因此有三条不能破的约束

1. **不要给运行时函数加 `nounwind` / `readnone` / `willreturn` 等属性。**
   现在除了 `runtime_error` 标了 `NoReturn`,其余一律无属性,LLVM 必须把它们
   当成读写内存的不透明屏障。这正是保守式 GC 需要的。为了跑分去加属性会破坏它。
2. **所有局部空间仍然必须放进入口块**(见 2.2 节)。既是栈不涨的要求,也保证了
   槽位地址稳定、可被扫到。
3. **runtime 侧的 C++ 局部指针是薄弱环节。** 一个刚分配、只被 C++ 局部变量持有的
   对象跨过一次嵌套分配时,它既不在容器里、也不在 IRGen 的 alloca 保证范围内,
   只能靠"编译器恰好把它 spill 到栈上"——那是**每十万次分配中一次**的静默内存
   损坏。这类位置一律用 `gc_root_push` / `gc_root_pop` 显式登记(约十处,
   如 `list.cpp` 的 `newList`、`dict.cpp` 的 `reserve`)。

### 寄存器是怎么抓到的

在回收函数里放一个 `jmp_buf` 局部变量,用 `setjmp` 把非易失寄存器溢写到这块栈
内存上,再把 `jmp_buf` 一起扫。本机 `D:\msys64\ucrt64\include\setjmp.h` 的
`_JUMP_BUFFER` 布局是 `Rbx/Rsi/Rdi/R12-R15/Rsp/Rbp/Rip` 等全部非易失寄存器
(实测七个寄存器逐个验证过)。跨调用仍活着的值必然在 callee-saved 寄存器或栈上,
所以这个快照够用。

两个注意点:

- **不读 `setjmp` 的返回值做分支,更不要调 `longjmp`。** 它在这里只是"溢写寄存器"
  的手段;读返回值会让 GCC 真按"返回两次"构造 CFG。
- **栈的上界用 `NT_TIB.StackBase`(x64 上是 `gs:0x08`),绝不能用 `StackLimit`。**
  Windows 栈按需 commit,从 `StackLimit` 往上扫会碰到未提交页 → 访问违例。
  用内联汇编读 `gs:0x08` 而不是 `<windows.h>`:运行时的很多 TU 只有几行 include,
  引入 windows.h 的 `min`/`max`/`ERROR` 宏会显著改变编译结果。

## 3. LLVM 22 的 API 与常见教程的差异

以下都是在本机 LLVM 22.1.8 头文件里核对过的,与多数网上教程(基于 LLVM 14–17)不同。

### 3.1 符号搜索生成器改名了

```cpp
// ❌ LLVM 22 中不存在,头文件也没有
#include "llvm/ExecutionEngine/Orc/DynamicLibrarySearchGenerator.h"
DynamicLibrarySearchGenerator::Load(...)

// ✅ LLVM 22 的正确形式
#include "llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h"
auto Gen = ExitOnErr(orc::EPCDynamicLibrarySearchGenerator::Load(
    ES, "libxmex_runtime.dll"));   // 注意第一个参数是 ExecutionSession&
J.getMainJITDylib().addGenerator(std::move(Gen));
```

签名:`Load(ExecutionSession &ES, const char *LibraryPath, SymbolPredicate Allow = {}, AddAbsoluteSymbolsFn = nullptr)`

`ES` 由 `JIT.getExecutionSession()` 取得。

### 3.2 没有 `setUseJITLink`

`LLJITBuilder` 上**不存在** `setUseJITLink`。LLVM 22 已把 Windows/COFF 的原生
JIT 支持做进默认路径(头文件里有 `COFFPlatform.h`、`COFFVCRuntimeSupport.h`),
不需要手动切换链接器。实测默认配置直接可用。

### 3.3 `lookup` 返回 `ExecutorAddr`

```cpp
// ✅ LLVM 22
llvm::orc::ExecutorAddr A = ExitOnErr(J.lookup("jit_test"));
auto *Fn = A.toPtr<int64_t(*)(int64_t, int64_t)>();
```

`JITEvaluatedSymbol` / `jitTargetAddressToFunction` 都已移除。

### 3.4 CMake:shared 模式下链接极简

因为是 shared 模式,`LLVMConfig.cmake` 导出了一个名为 `LLVM` 的 imported
target(`add_library(LLVM SHARED IMPORTED)`)。链接它就够了:

```cmake
find_package(LLVM REQUIRED CONFIG)
target_include_directories(目标 PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(目标 PRIVATE LLVM)
```

**不要**去枚举组件名。实测:
- 组件名在本 build 里**全小写**(`x86codegen` 存在,`X86CodeGen` 不存在)
- `llvm_map_components_to_libnames` 在这种配置下没必要
- `llvm-config --libs orcjit jitlink native` 只返回 `-lLLVM-22`

`LLVM_DEFINITIONS` 是含 `-D` 前缀的空格分隔字符串,要用
`add_compile_options` + `separate_arguments`,不能用 `target_compile_definitions`。

### 3.5 代码生成文件类型

```cpp
// ✅ LLVM 22(enum class,在 llvm/Support/CodeGen.h)
llvm::CodeGenFileType::ObjectFile

// ❌ 旧写法,已废弃
llvm::CGFT_ObjectFile / llvm::TargetMachine::CGFT_ObjectFile
```

`addPassesToEmitFile` 签名(**失败返回 true**):

```cpp
bool addPassesToEmitFile(PassManagerBase &PM, raw_pwrite_stream &Out,
                         raw_pwrite_stream *DwoOut, CodeGenFileType FileType,
                         bool DisableVerify = true,
                         MachineModuleInfoWrapperPass *MMIWP = nullptr);
```

## 4. 运行时库必须是 DLL

JIT 生成的代码要在运行时解析 `add` 这类符号。Windows 上**不能**靠
"搜索当前进程导出符号"(`GetForCurrentProcess`)—— MinGW 链接出的 exe
几乎不导出任何东西。

所以 `xmex_runtime` 必须是 `SHARED`(DLL),并用
`EPCDynamicLibrarySearchGenerator::Load` 按名字显式加载。

### 不要把 __declspec(dllexport) 撒进运行时

MinGW 对 `-shared` 库**默认自动导出全部符号**。而一旦出现第一个
`__declspec(dllexport)`,后续符号就不再自动导出 —— 反而会漏掉一批,
让 JIT 找不到 `add`。保持默认即可。

## 5. 运行时 DLL 的依赖

- `jit_spike.exe` / `xmex_repl.exe` 需要 `libLLVM-22.dll` 在 PATH 上。
  从 **UCRT64 shell** 运行即可(`/ucrt64/bin` 已在 PATH);从别处运行会因
  找不到 DLL 而启动失败。
- `libxmex_runtime.dll` 与 exe 同在 `build/bin/` 下,自动能找到。

## 6. 谓词返回 i32,不返回 i1

`truthy` 等判定函数一律返回 `int32_t`。GCC 与 Clang 对 `bool` 返回值的
ABI 标注不一致(`i1` vs `zeroext i1`),跨 DLL 边界不可靠。

## 7. 复现验证

```bash
# 从 UCRT64 shell
cd /d/xmex
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build
cd build && ctest --output-on-failure
```

`tests/abi_test.cpp` 与 `tests/jit_spike.cpp` 全过,说明 ABI 边界是通的。
**在写任何前端代码之前必须先过这两个测试。**
