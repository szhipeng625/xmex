// unit_cloud_judge_main.cpp -- host entry for the Xmex unit test.
#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstdio>
extern "C" void xmex_unit_cloud_judge_main();
int main() {
    xmex_unit_cloud_judge_main();
    return 0;
}
