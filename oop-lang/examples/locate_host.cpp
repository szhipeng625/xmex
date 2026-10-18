// 沙箱定位测试 —— C++ 宿主程序
#include <cstdio>

extern "C" void xmex_sandbox_locate_main();

int main() {
    xmex_sandbox_locate_main();
    return 0;
}
