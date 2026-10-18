// ffi_agent.cpp —— FFI 隔离模式的 agent 可执行文件
//
// 用法: xmex_ffi_agent <动态库路径>
//
// 宿主(xmex_runtime)起这个进程,两边用管道通信:
//   宿主 → agent 的 stdin : 一行一条 JSON 请求
//   agent → 宿主的 stdout : 一行一条 JSON 响应
//
// 之所以做成**独立可执行文件**而不是直接 fork 宿主:
//   1. 子进程是干净的,只加载目标 DLL —— 不会把宿主已加载的运行时、
//      已注册的 GC 根、已打开的套接字都带过去
//   2. DLL 崩了时,退出码干净地反映在 agent 的退出状态上
//   3. 将来做远程 agent 时,同一个可执行文件换个连接方式即可

#include <cstdio>
#include <cstring>

extern "C" int ffi_worker_main(const char* lib_path);

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <动态库路径>\n", argv[0]);
        return 2;
    }
    return ffi_worker_main(argv[1]);
}
