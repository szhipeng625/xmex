// Xmex 跨平台兼容层
//
// 统一 Linux/macOS/Windows 的 API 差异，让运行时模块无需关心平台细节。
// 所有平台相关的函数都通过这里的宏和内联函数封装。
//
// 支持的平台：
//   - Linux (__linux__)
//   - macOS (__APPLE__)
//   - Windows (_WIN32)
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ===========================================================================
// 平台检测
// ===========================================================================
#if defined(_WIN32) || defined(_WIN64)
  #define XMEX_WINDOWS 1
  #define XMEX_PLATFORM "Windows"
#elif defined(__APPLE__)
  #define XMEX_APPLE 1
  #define XMEX_PLATFORM "macOS"
#elif defined(__linux__)
  #define XMEX_LINUX 1
  #define XMEX_PLATFORM "Linux"
#else
  #error "Unsupported platform"
#endif

// ===========================================================================
// 动态库加载
// ===========================================================================
#ifdef XMEX_WINDOWS
  #include <windows.h>
  #define XMEX_DLOPEN(path)   (void*)LoadLibraryA(path)
  #define XMEX_DLSYM(handle, name) (void*)GetProcAddress((HMODULE)(handle), name)
  #define XMEX_DLCLOSE(handle) FreeLibrary((HMODULE)(handle))
  #define XMEX_DLERROR()      ""
#else
  #include <dlfcn.h>
  #define XMEX_DLOPEN(path)   dlopen(path, RTLD_LAZY)
  #define XMEX_DLSYM(handle, name) dlsym(handle, name)
  #define XMEX_DLCLOSE(handle) dlclose(handle)
  #define XMEX_DLERROR()      dlerror()
#endif

// ===========================================================================
// 进程管道（popen/pclose）
// ===========================================================================
#ifdef XMEX_WINDOWS
  #define XMEX_POPEN(cmd, mode)  _popen(cmd, mode)
  #define XMEX_PCLOSE(fp)        _pclose(fp)
#else
  #define XMEX_POPEN(cmd, mode)  popen(cmd, mode)
  #define XMEX_PCLOSE(fp)        pclose(fp)
#endif

// ===========================================================================
// 匿名管道 + 子进程 spawn
//
// 给 FFI 的隔离模式用:宿主起一个子进程加载 DLL,两边用管道通信。
// DLL 崩了只死子进程,宿主从管道读到 EOF,waitpid 拿到退出码。
//
// 为什么需要这层抽象:Windows 没有 fork()。POSIX 用 pipe+fork+execv,
// Windows 用 CreatePipe+CreateProcess(句柄要显式继承给子进程)。
// 两边语义对齐成同一组调用。
// ===========================================================================

#ifdef XMEX_WINDOWS

// 管道句柄。Windows 上是 HANDLE(POSIX 上是 int fd)。
typedef void* xmex_pipe_t;
#define XMEX_PIPE_INVALID ((xmex_pipe_t)INVALID_HANDLE_VALUE)

// 子进程句柄与 pid
typedef void* xmex_proc_t;
#define XMEX_PROC_INVALID ((xmex_proc_t)INVALID_HANDLE_VALUE)

#else

typedef int xmex_pipe_t;
#define XMEX_PIPE_INVALID (-1)
typedef int xmex_proc_t;
#define XMEX_PROC_INVALID (-1)

#endif

// 管道方向
#define XMEX_PIPE_READ  0
#define XMEX_PIPE_WRITE 1

// 建一对管道。out_read 拿到读端,out_write 拿到写端。
// 失败返回 0,并把两端置为 XMEX_PIPE_INVALID。
// 管道是"继承型"的:spawn 时子进程会拿到它们。
extern "C" int xmex_pipe_pair(xmex_pipe_t* out_read, xmex_pipe_t* out_write);

// 起一个子进程:
//   exe_path   —— 可执行文件路径
//   argv       —— 参数数组(不含程序名,会自动补上 exe_path)
//   argc       —— argv 的个数
//   in_pipe    —— 给子进程的 stdin(读端);XMEX_PIPE_INVALID 表示 /dev/null
//   out_pipe   —— 给子进程的 stdout(写端);XMEX_PIPE_INVALID 表示丢弃
//   out_proc   —— 返回子进程句柄
// 返回 0 成功。
extern "C" int xmex_spawn(const char* exe_path, const char* const* argv, int argc,
                          xmex_pipe_t in_pipe, xmex_pipe_t out_pipe,
                          xmex_proc_t* out_proc);

// 等待子进程结束。返回退出码;若是被信号杀死,返回 128+signal。
extern "C" int xmex_proc_wait(xmex_proc_t proc);

// 强制终止子进程(不给它清理机会),然后回收。
//
// 什么时候需要:子进程里的 DLL 若死循环,关管道它也不退出 —— 它根本没在
// 读。此时必须硬杀,否则留下一个永远空转的僵尸进程,占着 CPU 与内存。
// 返回 1 表示杀掉了,0 表示已经退出了(不用杀)。
extern "C" int xmex_proc_kill(xmex_proc_t proc);

// 取子进程 pid(用于日志与诊断)。
extern "C" long xmex_proc_pid(xmex_proc_t proc);

// 关管道端。
extern "C" void xmex_pipe_close(xmex_pipe_t p);

// 从管道读 n 字节。返回实际读到的字节数;0 表示 EOF(对端已关闭);
// -1 表示错误。
extern "C" long xmex_pipe_read(xmex_pipe_t p, void* buf, unsigned long n);

// 从管道读**恰好一个字节**,最多等 timeout_ms 毫秒。
// 返回 1 = 读到一字节(写入 *out);0 = 超时(什么都没等到);
// -1 = EOF(对端关闭);-2 = 错误。
//
// 为什么需要它:子进程里的 DLL 若死循环,阻塞式读会让宿主**永久**卡住 ——
// 没有超时就没有任何机会恢复。调用方拿到 0 后可以杀掉 worker 再报错。
extern "C" int xmex_pipe_read_byte_timeout(xmex_pipe_t p, char* out,
                                           unsigned long timeout_ms);

// 往管道写 n 字节。返回实际写入的字节数;-1 表示错误。
extern "C" long xmex_pipe_write(xmex_pipe_t p, const void* buf, unsigned long n);

// ===========================================================================
// 路径分隔符
// ===========================================================================
#ifdef XMEX_WINDOWS
  #define XMEX_PATH_SEP '\\'
  #define XMEX_PATH_SEP_STR "\\"
  #define XMEX_TMP_DIR "C:/Windows/Temp"
#else
  #define XMEX_PATH_SEP '/'
  #define XMEX_PATH_SEP_STR "/"
  #define XMEX_TMP_DIR "/tmp"
#endif

// ===========================================================================
// 临时文件路径
// ===========================================================================
#ifdef XMEX_WINDOWS
  #include <windows.h>
  inline const char* xmex_tmp_dir() {
    static char buf[MAX_PATH];
    if (buf[0] == '\0') {
      GetTempPathA(MAX_PATH, buf);
      // 去掉末尾的反斜杠
      size_t len = strlen(buf);
      if (len > 0 && buf[len - 1] == '\\') buf[len - 1] = '\0';
    }
    return buf;
  }
#else
  inline const char* xmex_tmp_dir() { return "/tmp"; }
#endif

// ===========================================================================
// 文件操作（统一 fopen 的文本/二进制模式）
// ===========================================================================
// 在 Windows 上，fopen 默认是文本模式（会转换 \n -> \r\n），
// 我们需要始终使用二进制模式来读写模型文件和 PTX 代码。
#ifdef XMEX_WINDOWS
  #define XMEX_FOPEN_READ(path)  fopen(path, "rb")
  #define XMEX_FOPEN_WRITE(path) fopen(path, "wb")
#else
  #define XMEX_FOPEN_READ(path)  fopen(path, "r")
  #define XMEX_FOPEN_WRITE(path) fopen(path, "w")
#endif

// ===========================================================================
// 线程（用于 REST API 服务器）
// ===========================================================================
#ifdef XMEX_WINDOWS
  #include <windows.h>
  #include <process.h>
  #define XMEX_THREAD_TYPE HANDLE
  #define XMEX_THREAD_CREATE(thread, func, arg) \
    ((*(thread) = (HANDLE)_beginthreadex(NULL, 0, (unsigned(__stdcall*)(void*))(func), arg, 0, NULL)) != NULL)
  #define XMEX_THREAD_JOIN(thread) WaitForSingleObject(thread, INFINITE)
  #define XMEX_THREAD_DETACH(thread) CloseHandle(thread)
#else
  #include <thread>
  #define XMEX_THREAD_TYPE std::thread
  #define XMEX_THREAD_CREATE(thread, func, arg) (*(thread) = std::thread(func, arg), true)
  #define XMEX_THREAD_JOIN(thread) (thread)->join()
  #define XMEX_THREAD_DETACH(thread) (thread)->detach()
#endif

// ===========================================================================
// 网络（socket）
// ===========================================================================
#ifdef XMEX_WINDOWS
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  #define XMEX_SOCKET_INIT() \
    do { WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa); } while(0)
  #define XMEX_SOCKET_CLEANUP() WSACleanup()
  #define XMEX_CLOSE_SOCKET(s) closesocket(s)
  typedef SOCKET xmex_socket_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #define XMEX_SOCKET_INIT()
  #define XMEX_SOCKET_CLEANUP()
  #define XMEX_CLOSE_SOCKET(s) close(s)
  typedef int xmex_socket_t;
#endif

// ===========================================================================
// 原子操作
// ===========================================================================
#ifdef XMEX_WINDOWS
  #include <windows.h>
  #define XMEX_ATOMIC_BOOL volatile LONG
  #define XMEX_ATOMIC_LOAD(p)    (InterlockedCompareExchange(p, 0, 0) != 0)
  #define XMEX_ATOMIC_STORE(p, v) InterlockedExchange(p, (v) ? 1 : 0)
#else
  #include <atomic>
  #define XMEX_ATOMIC_BOOL std::atomic<bool>
  #define XMEX_ATOMIC_LOAD(p)    (p)->load()
  #define XMEX_ATOMIC_STORE(p, v) (p)->store(v)
#endif

// ===========================================================================
// 栈基址获取（用于 GC 保守扫描）
// ===========================================================================
#ifdef XMEX_WINDOWS
  // Windows: x64 上 GS 指向 TEB，NT_TIB.StackBase 在 gs:0x08
  inline uintptr_t xmex_stack_base() {
    uintptr_t v;
    #ifdef _MSC_VER
      v = (uintptr_t)__readgsqword(0x08);
    #else
      asm volatile("movq %%gs:0x08, %0" : "=r"(v));
    #endif
    return v;
  }
#elif defined(XMEX_LINUX)
  #include <pthread.h>
  inline uintptr_t xmex_stack_base() {
    pthread_attr_t attr;
    void *stackaddr = nullptr;
    size_t stacksize = 0;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &stackaddr, &stacksize);
    pthread_attr_destroy(&attr);
    return reinterpret_cast<uintptr_t>(stackaddr) + stacksize;
  }
#elif defined(XMEX_APPLE)
  #include <pthread.h>
  inline uintptr_t xmex_stack_base() {
    return reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
  }
#endif

// ===========================================================================
// 可执行文件扩展名
// ===========================================================================
#ifdef XMEX_WINDOWS
  #define XMEX_EXE_EXT ".exe"
#else
  #define XMEX_EXE_EXT ""
#endif

// ===========================================================================
// 共享库扩展名
// ===========================================================================
#ifdef XMEX_WINDOWS
  #define XMEX_SO_EXT ".dll"
#elif defined(XMEX_APPLE)
  #define XMEX_SO_EXT ".dylib"
#else
  #define XMEX_SO_EXT ".so"
#endif
