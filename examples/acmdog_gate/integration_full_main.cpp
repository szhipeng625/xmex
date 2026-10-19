// integration_full_main.cpp -- host entry.
#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstdio>
extern "C" void xmex_integration_full_main();
int main() {
    xmex_integration_full_main();
    return 0;
}
