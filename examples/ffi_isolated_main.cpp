// ffi_isolated_main.cpp —— FFI 隔离模式演示的 C++ 宿主
//
// 与 ffi_main.cpp 的区别:生成的符号名不同(模块名决定)。
// pylitec 为模块 ffi_isolated_demo 生成的入口是
//   xmex_ffi_isolated_demo_main

#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstdio>

// 按 ABI 约定,参数/返回值都是 xmex::Value*
extern "C" void xmex_ffi_isolated_demo_main();

int main() {
    xmex_ffi_isolated_demo_main();
    return 0;
}
