// 分布式节点测试 —— C++ 宿主程序
// 调用 Xmex 编译 distributed.pys 生成的入口函数
#include <cstdio>

// 由 xmexc 编译 distributed.pys 生成
// 符号名规则: xmex_<模块名>_main
extern "C" void xmex_distributed_main();

int main() {
    xmex_distributed_main();
    return 0;
}
