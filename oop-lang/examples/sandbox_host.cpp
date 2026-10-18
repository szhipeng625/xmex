// 沙箱节点测试 —— C++ 宿主程序
#include <cstdio>

extern "C" void xmex_sandbox_node_main();

int main() {
    xmex_sandbox_node_main();
    return 0;
}
