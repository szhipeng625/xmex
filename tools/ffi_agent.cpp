// ffi_agent.cpp —— FFI 隔离模式的 agent 可执行文件
//
// 用法:
//   xmex_ffi_agent <动态库路径> [选项]
//
// 选项:
//   --ready-file <路径>   加载成功后往该文件写一行 "ready" 并关闭。
//                         纳管方(kv.add)据此判断"DLL 已就绪",而不只是
//                         "进程还活着" —— 后者区分不了"服务就绪"与"进程空转"。
//
// 宿主(xmex_runtime)起这个进程,两边用管道通信:
//   宿主 → agent 的 stdin : 一行一条 JSON 请求
//   agent → 宿主的 stdout : 一行一条 JSON 响应
//
// 启动行为:**先加载 DLL,再进请求循环**。加载失败以退出码 3 退出 ——
// 纳管方(kv_admin 调谐循环)下一轮就会看到"进程没了"并重新拉起,
// 而不会留下一个假装活着的空壳进程。
//
// 之所以做成**独立可执行文件**而不是直接 fork 宿主:
//   1. 子进程是干净的,只加载目标 DLL —— 不会把宿主已加载的运行时、
//      已注册的 GC 根、已打开的套接字都带过去
//   2. DLL 崩了时,退出码干净地反映在 agent 的退出状态上
//   3. 将来做远程 agent 时,同一个可执行文件换个连接方式即可

#include <cstdio>
#include <cstring>

// ready_file 为 nullptr 时不写就绪文件(宿主用管道握手,不需要它)
extern "C" int ffi_worker_main(const char* lib_path, const char* ready_file);

int main(int argc, char** argv) {
    const char* lib = nullptr;
    const char* ready_file = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ready-file") == 0 && i + 1 < argc) {
            ready_file = argv[++i];
        } else if (!lib) {
            lib = argv[i];
        }
        // 未知参数忽略:向前兼容,别因为多传一个选项就起不来
    }

    if (!lib) {
        std::fprintf(stderr,
                     "用法: %s <动态库路径> [--ready-file <路径>]\n", argv[0]);
        return 2;
    }
    return ffi_worker_main(lib, ready_file);
}