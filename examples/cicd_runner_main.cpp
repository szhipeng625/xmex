// cicd_runner_main.cpp -- host entry for the Xmex CI/CD runner.
// The Xmex side holds all the pipeline logic; this file only starts it.
#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstdio>
extern "C" void xmex_cicd_runner_main();
int main() {
    xmex_cicd_runner_main();
    return 0;
}
