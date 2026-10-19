// unit_middleware_main.cpp -- host entry.
#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstdio>
extern "C" void xmex_unit_middleware_main();
int main() {
    xmex_unit_middleware_main();
    return 0;
}
