// platform_proc.cpp —— 管道与子进程 spawn 的跨平台实现
//
// 对应 include/xmex/platform.h 里声明的那组函数。
//
// Windows 用 CreatePipe + CreateProcess(句柄显式继承),
// POSIX 用 pipe + fork + execv。
//
// ⚠️ Windows 上 CreateProcess 的句柄继承有个坑:所有可继承的句柄都会
// 被继承给子进程,包括那些我们**不想**它继承的。所以这里在创建管道时
// 就把它们设成不可继承(XMEX_PIPE_INHERITABLE 只在 spawn 前那一瞬间打开),
// 避免子进程攥着多余的句柄导致父进程的管道永远等不到 EOF。

#include "xmex/platform.h"

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>

#ifdef XMEX_WINDOWS
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
#else
  #include <unistd.h>
  #include <sys/wait.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <errno.h>
#endif

extern "C" {

#ifdef XMEX_WINDOWS

// Windows 的管道端用 HANDLE 存。这里额外记一份"读还是写",
// 因为 HANDLE 本身不区分方向,而 read/write 语义要区分。
namespace {
struct PipeEnd {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool is_read = false;
};
// 管道端 → 方向的映射。Windows 上 HANDLE 值可复用(会骗人),
// 所以按值查表时同时比地址与方向。
std::vector<std::pair<HANDLE, bool>> g_pipe_ends;
std::mutex g_pipe_mu;

void remember_pipe(HANDLE h, bool is_read) {
    std::lock_guard<std::mutex> lk(g_pipe_mu);
    g_pipe_ends.emplace_back(h, is_read);
}

bool lookup_pipe(HANDLE h, bool* is_read) {
    std::lock_guard<std::mutex> lk(g_pipe_mu);
    for (const auto& [hh, r] : g_pipe_ends) {
        if (hh == h) { *is_read = r; return true; }
    }
    return false;
}

void forget_pipe(HANDLE h) {
    std::lock_guard<std::mutex> lk(g_pipe_mu);
    for (size_t i = 0; i < g_pipe_ends.size(); ++i) {
        if (g_pipe_ends[i].first == h) {
            g_pipe_ends.erase(g_pipe_ends.begin() + (long)i);
            return;
        }
    }
}
}  // namespace

int xmex_pipe_pair(xmex_pipe_t* out_read, xmex_pipe_t* out_write) {
    if (out_read) *out_read = XMEX_PIPE_INVALID;
    if (out_write) *out_write = XMEX_PIPE_INVALID;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    // bInheritHandle = TRUE:管道要传给子进程
    sa.bInheritHandle = TRUE;

    HANDLE rd = INVALID_HANDLE_VALUE, wr = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;

    remember_pipe(rd, true);
    remember_pipe(wr, false);
    if (out_read) *out_read = (xmex_pipe_t)rd;
    if (out_write) *out_write = (xmex_pipe_t)wr;
    return 1;
}

int xmex_spawn(const char* exe_path, const char* const* argv, int argc,
               xmex_pipe_t in_pipe, xmex_pipe_t out_pipe,
               xmex_proc_t* out_proc) {
    if (out_proc) *out_proc = XMEX_PROC_INVALID;

    // 拼命令行: "exe" "arg1" "arg2" ...
    //
    // ⚠️ 必须用**可写**的缓冲区:CreateProcessA 会就地修改命令行。
    // std::string 的 operator[] 在 C++11 之后不保证可写(&s[0] 只在
    // s[s.size()] == '\0' 时才合法),所以用 std::vector<char> 显式留出
    // 结尾的 '\0'。
    std::string cmd_str = "\"";
    cmd_str += exe_path;
    cmd_str += "\"";
    for (int i = 0; i < argc; ++i) {
        cmd_str += " \"";
        cmd_str += argv[i];
        cmd_str += "\"";
    }
    std::vector<char> cmd(cmd_str.begin(), cmd_str.end());
    cmd.push_back('\0');

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof(si));
    std::memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    // 管道的另一端要在 spawn 前临时设为可继承,spawn 完立刻改回 ——
    // 否则子进程会攥着多余的管道端,父进程等不到 EOF。
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    // GetHandleInformation 要的是 DWORD*,不是 bool*
    DWORD in_was_inheritable = 0, out_was_inheritable = 0;
    BOOL in_flags = 0, out_flags = 0;

    if (in_pipe != XMEX_PIPE_INVALID) {
        HANDLE h = (HANDLE)in_pipe;
        in_flags = GetHandleInformation(h, &in_was_inheritable);
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        hStdIn = h;
    }
    if (out_pipe != XMEX_PIPE_INVALID) {
        HANDLE h = (HANDLE)out_pipe;
        out_flags = GetHandleInformation(h, &out_was_inheritable);
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        hStdOut = h;
    }

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdIn;
    si.hStdOutput = hStdOut;
    si.hStdError = hStdOut;   // stderr 也并到同一条管道,免得子进程的诊断信息丢失

    BOOL ok = CreateProcessA(exe_path, cmd.data(), nullptr, nullptr, TRUE, 0,
                             nullptr, nullptr, &si, &pi);

    // 恢复继承标志
    if (in_pipe != XMEX_PIPE_INVALID && in_flags) {
        SetHandleInformation((HANDLE)in_pipe, HANDLE_FLAG_INHERIT,
                             in_was_inheritable ? HANDLE_FLAG_INHERIT : 0);
    }
    if (out_pipe != XMEX_PIPE_INVALID && out_flags) {
        SetHandleInformation((HANDLE)out_pipe, HANDLE_FLAG_INHERIT,
                             out_was_inheritable ? HANDLE_FLAG_INHERIT : 0);
    }

    if (!ok) return 0;

    CloseHandle(pi.hThread);
    if (out_proc) *out_proc = (xmex_proc_t)pi.hProcess;
    return 1;
}

int xmex_proc_wait(xmex_proc_t proc) {
    if (proc == XMEX_PROC_INVALID) return -1;
    HANDLE h = (HANDLE)proc;
    DWORD code = 0;
    // ⚠️ WaitForSingleObject 成功时返回 WAIT_OBJECT_0,它的值**就是 0**。
    // 写成 `if (!WaitForSingleObject(...))` 会把成功当成失败 —— 返回 -1。
    // 必须显式跟 WAIT_OBJECT_0 比。
    DWORD wr = WaitForSingleObject(h, INFINITE);
    if (wr != WAIT_OBJECT_0) return -1;
    if (!GetExitCodeProcess(h, &code)) return -1;
    CloseHandle(h);
    // 异常终止时 Windows 给的是 NTSTATUS(0xC0000005 = ACCESS_VIOLATION),
    // 与 POSIX 的 128+signal 不是一回事。统一折算成 POSIX 风格,
    // 让上层看到的退出码在两个平台上一致(崩溃 → 139 = 128+SIGSEGV)。
    if (code >= 0xC0000000u) {
        switch (code) {
            case 0xC0000005u: return 139;   // SIGSEGV
            case 0xC000001Du: return 132;   // SIGILL
            case 0xC0000094u: return 136;   // SIGFPE
            case 0xC00000FDu: return 134;   // SIGABRT
            case 0xC0000139u: return 133;   // SIGTRAP
            default:           return 128;   // 其它异常,笼统按"被杀"处理
        }
    }
    return (int)code;
}

long xmex_proc_pid(xmex_proc_t proc) {
    if (proc == XMEX_PROC_INVALID) return -1;
    return (long)GetProcessId((HANDLE)proc);
}

int xmex_proc_kill(xmex_proc_t proc) {
    if (proc == XMEX_PROC_INVALID) return 0;
    HANDLE h = (HANDLE)proc;
    DWORD code = 0;
    // 已经退出了就不用杀
    if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) {
        CloseHandle(h);
        return 0;
    }
    // 退出码 1 = 强杀。Windows 没有 SIGKILL 的"不给清理机会"语义,
    // TerminateProcess 就是硬杀 —— 与 POSIX 的 SIGKILL 对齐。
    BOOL ok = TerminateProcess(h, 1);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    return ok ? 1 : 0;
}

void xmex_pipe_close(xmex_pipe_t p) {
    if (p == XMEX_PIPE_INVALID) return;
    HANDLE h = (HANDLE)p;
    forget_pipe(h);
    CloseHandle(h);
}

long xmex_pipe_read(xmex_pipe_t p, void* buf, unsigned long n) {
    if (p == XMEX_PIPE_INVALID) return -1;
    HANDLE h = (HANDLE)p;
    bool is_read = false;
    if (!lookup_pipe(h, &is_read)) is_read = true;
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, nullptr)) {
        // 管道被写端关闭时 ReadFile 返回 FALSE 但 got==0,那是 EOF 不是错误
        if (got == 0) return 0;
        return -1;
    }
    return (long)got;
}

int xmex_pipe_read_byte_timeout(xmex_pipe_t p, char* out, unsigned long timeout_ms) {
    if (p == XMEX_PIPE_INVALID) return -2;
    HANDLE h = (HANDLE)p;

    // PeekNamedPipe 只能看"有没有数据",不能等。要等就得轮询 ——
    // 管道不支持 select/epoll 的等待语义,轮询间隔取 10ms 兼顾
    // 响应速度与 CPU 开销。
    const unsigned long kPollMs = 10;
    unsigned long waited = 0;
    while (true) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
            // 管道坏了或已关闭 —— 交给下面的 ReadFile 区分 EOF 与错误
            DWORD got = 0;
            if (!ReadFile(h, out, 1, &got, nullptr)) {
                return (got == 0) ? -1 : -2;
            }
            return 1;
        }
        if (avail > 0) {
            DWORD got = 0;
            if (!ReadFile(h, out, 1, &got, nullptr)) return -2;
            return 1;
        }
        if (waited >= timeout_ms) return 0;   // 超时
        Sleep(kPollMs);
        waited += kPollMs;
    }
}

long xmex_pipe_write(xmex_pipe_t p, const void* buf, unsigned long n) {
    if (p == XMEX_PIPE_INVALID) return -1;
    HANDLE h = (HANDLE)p;
    DWORD put = 0;
    if (!WriteFile(h, buf, (DWORD)n, &put, nullptr)) return -1;
    return (long)put;
}

#else   // ===== POSIX =====

int xmex_pipe_pair(xmex_pipe_t* out_read, xmex_pipe_t* out_write) {
    if (out_read) *out_read = XMEX_PIPE_INVALID;
    if (out_write) *out_write = XMEX_PIPE_INVALID;
    int fds[2];
    if (pipe(fds) != 0) return 0;
    if (out_read) *out_read = fds[0];
    if (out_write) *out_write = fds[1];
    return 1;
}

int xmex_spawn(const char* exe_path, const char* const* argv, int argc,
               xmex_pipe_t in_pipe, xmex_pipe_t out_pipe,
               xmex_proc_t* out_proc) {
    if (out_proc) *out_proc = XMEX_PROC_INVALID;

    // 拼 argv: argv[0] 是程序名(约定用 exe_path)
    std::vector<char*> cargv;
    std::vector<std::string> storage;
    storage.push_back(exe_path);
    for (int i = 0; i < argc; ++i) storage.push_back(argv[i]);
    for (auto& s : storage) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        // 子进程:接管管道的标准流
        if (in_pipe != XMEX_PIPE_INVALID) {
            dup2(in_pipe, STDIN_FILENO);
            close(in_pipe);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        }
        if (out_pipe != XMEX_PIPE_INVALID) {
            dup2(out_pipe, STDOUT_FILENO);
            dup2(out_pipe, STDERR_FILENO);
            close(out_pipe);
        }
        execv(exe_path, cargv.data());
        _exit(127);   // exec 失败
    }
    // 父进程:关掉自己那份多余端
    if (in_pipe != XMEX_PIPE_INVALID) close(in_pipe);
    if (out_pipe != XMEX_PIPE_INVALID) close(out_pipe);
    if (out_proc) *out_proc = (xmex_proc_t)pid;
    return 1;
}

int xmex_proc_wait(xmex_proc_t proc) {
    if (proc == XMEX_PROC_INVALID) return -1;
    int status = 0;
    if (waitpid((pid_t)proc, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

long xmex_proc_pid(xmex_proc_t proc) {
    if (proc == XMEX_PROC_INVALID) return -1;
    return (long)proc;
}

int xmex_proc_kill(xmex_proc_t proc) {
    if (proc == XMEX_PROC_INVALID) return 0;
    pid_t pid = (pid_t)proc;
    // 已经退出了(waitpid 立刻返回 pid)就不用杀
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) return 0;         // 已经退出并回收
    if (r < 0) return 0;            // 没这个子进程,视为已退出

    // SIGKILL = 不给清理机会,与 Windows 的 TerminateProcess 对齐。
    // 卡在死循环里的 agent 根本没在处理 SIGTERM,所以必须用 KILL。
    if (kill(pid, SIGKILL) != 0) return 0;
    waitpid(pid, &status, 0);
    return 1;
}

void xmex_pipe_close(xmex_pipe_t p) {
    if (p == XMEX_PIPE_INVALID) return;
    close(p);
}

long xmex_pipe_read(xmex_pipe_t p, void* buf, unsigned long n) {
    if (p == XMEX_PIPE_INVALID) return -1;
    ssize_t got = read(p, buf, n);
    if (got < 0) {
        // EINTR 是信号打断,重试
        if (errno == EINTR) return xmex_pipe_read(p, buf, n);
        return -1;
    }
    return (long)got;
}

int xmex_pipe_read_byte_timeout(xmex_pipe_t p, char* out, unsigned long timeout_ms) {
    if (p == XMEX_PIPE_INVALID) return -2;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(p, &rfds);
        struct timeval tv;
        tv.tv_sec = (time_t)(timeout_ms / 1000);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);

        int rc = select(p + 1, &rfds, nullptr, nullptr, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -2;
        }
        if (rc == 0) return 0;      // 超时

        ssize_t got = read(p, out, 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -2;
        }
        if (got == 0) return -1;    // EOF
        return 1;
    }
}

long xmex_pipe_write(xmex_pipe_t p, const void* buf, unsigned long n) {
    if (p == XMEX_PIPE_INVALID) return -1;
    ssize_t put = write(p, buf, n);
    if (put < 0) {
        if (errno == EINTR) return xmex_pipe_write(p, buf, n);
        return -1;
    }
    return (long)put;
}

#endif  // XMEX_WINDOWS

}  // extern "C"
