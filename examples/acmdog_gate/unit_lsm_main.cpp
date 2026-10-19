// unit_lsm_main.cpp -- host entry for the Xmex lsm unit test.
#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstdio>
extern "C" void xmex_unit_lsm_main();
int main() {
    xmex_unit_lsm_main();
    return 0;
}
