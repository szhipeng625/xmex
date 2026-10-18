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
