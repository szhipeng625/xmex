// http_demo_main.cpp —— HTTP 演示的 C++ 宿主
//
// 与 ffi_main.cpp 同样的模式:PyLite 只负责业务逻辑,C++ 侧提供入口。
// 模块名决定生成的符号名(模块 http_demo → xmex_http_demo_main)。

#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstdio>

// 按 ABI 约定,参数/返回值都是 xmex::Value*
extern "C" void xmex_http_demo_main();

int main() {
    xmex_http_demo_main();
    return 0;
}