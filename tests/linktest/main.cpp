// 端到端集成测试:手写 C++,链接 xmexc 编译出来的目标文件。
//
// 这是整个项目的核心诉求所在 —— "在编译期能够引用"。
// 这里的 main.cpp 完全不知道 Xmex 的存在,它只是调用一个 extern "C" 函数,
// 那个函数的实现由 arith.pys 编译而来。
#include "xmex/value.h"

#include <cstdio>

// 由 xmexc 编译 arith.pys 生成(符号名规则:xmex_<模块名>_<函数名>)
extern "C" void xmex_arith_main();

// 横幅刻意用 ASCII:这个程序的 stdout 要跟 arith.expected 逐字节比对,而
// tests/compare_output.cmake 在 Windows 上只能比 ASCII(execute_process 会用
// 当前代码页重新解码子进程输出,file(READ) 却不会 —— 中文横幅在两边会变成
// 不同的字节)。注释仍然用中文,受影响的只有程序真正打出来的字符。
int main() {
  std::printf("--- calling Xmex object ---\n");
  xmex_arith_main();
  std::printf("--- done ---\n");
  return 0;
}
