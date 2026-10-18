// ffi_main.cpp —— FFI 演示的 C++ 宿主
//
// 演示混合程序的标准形态:C++ 宿主 + PyLite 业务逻辑。
// PyLite 侧通过 kv.ffi() 加载动态库并调用其函数。

#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstdio>

// pylitec 为本模块生成的入口(按 ABI 约定,参数/返回值都是 xmex::Value*)
extern "C" void xmex_ffi_demo_main();

int main() {
    xmex_ffi_demo_main();
    return 0;
}
