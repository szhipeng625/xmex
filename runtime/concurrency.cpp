// 线程 / 进程创建与资源管理。
//
// 与项目其它运行时模块一致:对外是 extern "C" 函数,参数按 xmex::Value 指针约定
// 交换,出错走 runtime_error(打印后退出)。
//
// 设计要点:
//   1. 线程任务用"注册名 -> C 函数指针"的注册表管理。这门语言目前还没有一等
//      函数,线程入口无法在语言里直接写;由宿主 C++ 用任务名注册,语言层按名字
//      启动。等语言层有了一等函数,再补一个直接收函数值的入口即可。
//   2. 线程与进程句柄都登记在同一张带锁的表里,线程结束后可回收条目,
//      避免句柄长期堆积。
//   3. 所有跨线程共享的结构都由一把互斥锁保护;持锁期间不做阻塞等待,
//      否则 join 会把别的线程挡在外面。
#include "xmex/runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#endif

namespace {
struct ThreadEntry {
  int64_t id = 0;
  bool running = false;
  bool joinable = true;
  int64_t exitCode = 0;
};
struct ProcessEntry {
  int64_t id = 0;      // Linux: pid;Windows: 进程句柄值
  bool alive = true;
  int64_t exitCode = 0;
};
// 一把锁保护下面所有共享状态。持锁期间只做常数时间操作,不做阻塞等待。
// ⚠️ 必须是**同一个**锁对象:加锁与解锁若各自持有独立对象,保护完全失效。
#ifdef _WIN32
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;
void lockAcq() {
  if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }
  EnterCriticalSection(&g_lock);
}
void lockRel() { LeaveCriticalSection(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
void lockAcq() { pthread_mutex_lock(&g_lock); }
void lockRel() { pthread_mutex_unlock(&g_lock); }
#endif
std::vector<ThreadEntry> &threads() { static std::vector<ThreadEntry> v; return v; }
std::vector<ProcessEntry> &procs() { static std::vector<ProcessEntry> v; return v; }
int64_t &nextTid() { static int64_t n = 1; return n; }
}  // namespace

// ---------------------------------------------------------------------------
// 线程
// ---------------------------------------------------------------------------
// 任务注册表的元素。用名字索引,避免把 C 函数指针暴露给语言层。
namespace {
struct TaskEntry {
  std::string name;
  void *fn = nullptr;   // void (*)(void *)
  void *arg = nullptr;
};
std::vector<TaskEntry> &taskTable() { static std::vector<TaskEntry> v; return v; }
struct ThreadStart {
  void (*fn)(void *);
  void *arg;
  int64_t id;
};
// 线程体:执行任务,结束后把自己的条目标记为已结束(这样 join 之外,
// 语言层也能通过 alive 查询到状态 —— 不需要额外的结束通知机制)。
#ifdef _WIN32
unsigned __stdcall trampoline(void *p) {
#else
void *trampoline(void *p) {
#endif
  auto *ts = static_cast<ThreadStart *>(p);
  ts->fn(ts->arg);
  lockAcq();
  for (auto &t : threads()) {
    if (t.id == ts->id) { t.running = false; break; }
  }
  lockRel();
  delete ts;
#ifdef _WIN32
  return 0;
#else
  return nullptr;
#endif
}
}  // namespace

// 注册(或覆盖)一个命名线程任务。fn 的实际签名是 void (*)(void *),arg 原样透传。
extern "C" int64_t thread_register_task(const char *name, void *fn, void *arg) {
  if (name == nullptr || fn == nullptr) runtime_error("线程任务需要名字和函数");
  lockAcq();
  for (auto &t : taskTable()) {
    if (t.name == name) { t.fn = fn; t.arg = arg; lockRel(); return 1; }
  }
  taskTable().push_back({name, fn, arg});
  lockRel();
  return 1;
}

// 按名字启动一个线程,返回线程号;找不到任务名则报错。
extern "C" xmex::Value thread_start(const char *taskName) {
  using Fn = void (*)(void *);
  if (taskName == nullptr) runtime_error("线程任务名不能为空");
  Fn fn = nullptr;
  void *arg = nullptr;
  lockAcq();
  for (auto &t : taskTable()) {
    if (t.name == taskName) { fn = reinterpret_cast<Fn>(t.fn); arg = t.arg; break; }
  }
  lockRel();
  if (fn == nullptr) runtime_error("未注册的线程任务名");
  auto *ts = new ThreadStart{fn, arg, 0};
  lockAcq();
  const int64_t id = nextTid()++;
  ts->id = id;
  lockRel();
  ThreadEntry e;
  e.id = id;
  e.running = true;
  // 先把条目标记入表,**再**创建线程:否则极短的任务可能在线程创建完成前
  // 就执行完毕,结束通知找不到对应条目,running 会一直停在真。
  lockAcq();
  threads().push_back(e);
  lockRel();
  int64_t handle = 0;
#ifdef _WIN32
  uintptr_t h = _beginthreadex(nullptr, 0, trampoline, ts, 0, nullptr);
  if (h == 0) { delete ts; runtime_error("创建线程失败"); }
  handle = static_cast<int64_t>(h);   // 复用 exitCode 存句柄
#else
  pthread_t th;
  if (pthread_create(&th, nullptr, trampoline, ts) != 0) {
    delete ts;
    runtime_error("创建线程失败");
  }
  handle = static_cast<int64_t>(th);  // 复用 exitCode 存句柄
#endif
  lockAcq();
  for (auto &t : threads()) {
    if (t.id == id) { t.exitCode = handle; break; }
  }
  lockRel();
  return int_(id);
}

// 等待线程结束,并从表里回收它的条目。
extern "C" int64_t thread_join(int64_t id) {
  int64_t handle = 0;
  bool found = false;
  lockAcq();
  for (auto &t : threads()) {
    if (t.id == id) { handle = t.exitCode; found = true; break; }
  }
  lockRel();
  if (!found) return -1;
#ifdef _WIN32
  WaitForSingleObject(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)), INFINITE);
  CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)));
#else
  pthread_join(static_cast<pthread_t>(handle), nullptr);
#endif
  lockAcq();
  for (size_t i = 0; i < threads().size(); ++i) {
    if (threads()[i].id == id) { threads().erase(threads().begin() + static_cast<long>(i)); break; }
  }
  lockRel();
  return 0;
}

// 线程是否仍在运行。
extern "C" int64_t thread_alive(int64_t id) {
  lockAcq();
  int64_t r = 0;
  for (auto &t : threads()) {
    if (t.id == id) { r = t.running ? 1 : 0; break; }
  }
  lockRel();
  return r;
}

// 登记表中的线程总数。
extern "C" int64_t thread_count() {
  lockAcq();
  const int64_t n = static_cast<int64_t>(threads().size());
  lockRel();
  return n;
}

// ---------------------------------------------------------------------------
// 进程
// ---------------------------------------------------------------------------
// 同步执行一条 shell 命令,返回其标准输出(与 docker.cpp 的捕获方式一致)。
extern "C" xmex::Value process_run(const xmex::Value *cmd) {
  if (cmd == nullptr || cmd->tag != xmex::kStr) runtime_error("process_run 需要命令字符串");
  const char *c = str_data(cmd);
#ifdef _WIN32
  FILE *fp = _popen(c, "r");
#else
  FILE *fp = popen(c, "r");
#endif
  if (fp == nullptr) runtime_error("无法启动进程");
  std::string out;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
#ifdef _WIN32
  _pclose(fp);
#else
  pclose(fp);
#endif
  return str_new(out.data(), static_cast<int64_t>(out.size()));
}

// 后台启动一条命令,返回进程号;进程号登记进句柄表以便查询与回收。
extern "C" int64_t process_spawn(const xmex::Value *cmd) {
  if (cmd == nullptr || cmd->tag != xmex::kStr) runtime_error("process_spawn 需要命令字符串");
  const char *c = str_data(cmd);
  int64_t id = 0;
#ifdef _WIN32
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  std::memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  std::memset(&pi, 0, sizeof(pi));
  std::string line = std::string("cmd /c ") + c;
  if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      nullptr, &si, &pi)) {
    runtime_error("创建进程失败");
  }
  CloseHandle(pi.hThread);
  id = static_cast<int64_t>(reinterpret_cast<uintptr_t>(pi.hProcess));
#else
  const pid_t pid = fork();
  if (pid < 0) runtime_error("fork 失败");
  if (pid == 0) {
    // 子进程:换成 shell 执行命令。失败时用 127 退出(fork 后不能走报错路径)。
    execl("/bin/sh", "sh", "-c", c, static_cast<char *>(nullptr));
    _exit(127);
  }
  id = static_cast<int64_t>(pid);
#endif
  ProcessEntry e;
  e.id = id;
  e.alive = true;
  lockAcq();
  procs().push_back(e);
  lockRel();
  return id;
}

// 等待进程结束,返回其退出码;同时回收条目。
extern "C" int64_t process_wait(int64_t id) {
  int64_t code = -1;
#ifdef _WIN32
  auto h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(id));
  WaitForSingleObject(h, INFINITE);
  DWORD ec = 0;
  GetExitCodeProcess(h, &ec);
  code = static_cast<int64_t>(ec);
  CloseHandle(h);
#else
  int status = 0;
  if (waitpid(static_cast<pid_t>(id), &status, 0) < 0) return -1;
  if (WIFEXITED(status)) code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
#endif
  lockAcq();
  for (size_t i = 0; i < procs().size(); ++i) {
    if (procs()[i].id == id) { procs().erase(procs().begin() + static_cast<long>(i)); break; }
  }
  lockRel();
  return code;
}

// 进程是否仍存活(不阻塞,不回收)。
extern "C" int64_t process_alive(int64_t id) {
#ifdef _WIN32
  auto h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(id));
  DWORD ec = 0;
  if (!GetExitCodeProcess(h, &ec)) return 0;
  return ec == STILL_ACTIVE ? 1 : 0;
#else
  if (kill(static_cast<pid_t>(id), 0) == 0) return 1;
  return 0;
#endif
}

// 终止进程。
extern "C" int64_t process_kill(int64_t id) {
#ifdef _WIN32
  auto h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(id));
  return TerminateProcess(h, 1) ? 0 : -1;
#else
  return kill(static_cast<pid_t>(id), SIGTERM) == 0 ? 0 : -1;
#endif
}

// ---------------------------------------------------------------------------
// 资源管理
// ---------------------------------------------------------------------------
// 资源快照:存活线程数、运行中线程数、登记进程数。
extern "C" xmex::Value resource_stats() {
  lockAcq();
  int64_t total = 0, running = 0;
  for (auto &t : threads()) { ++total; if (t.running) ++running; }
  const int64_t p = static_cast<int64_t>(procs().size());
  lockRel();
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "{\"threads\":%lld,\"threads_running\":%lld,\"processes\":%lld}",
                static_cast<long long>(total), static_cast<long long>(running),
                static_cast<long long>(p));
  return str_new(buf, static_cast<int64_t>(std::strlen(buf)));
}

// 回收已经结束的线程条目(不阻塞)。
extern "C" int64_t resource_reap() {
  int64_t n = 0;
  lockAcq();
  for (size_t i = 0; i < threads().size();) {
    if (!threads()[i].running) {
      threads().erase(threads().begin() + static_cast<long>(i));
      ++n;
    } else {
      ++i;
    }
  }
  lockRel();
  return n;
}

// ---------------------------------------------------------------------------
// 互斥锁与条件变量
// ---------------------------------------------------------------------------
// 对外用 xmex::Value 交换(与内置模块调用约定一致):句柄是一个整数,实际对象
// 登记在带锁的表里,靠句柄访问 —— 语言层因此不需要认识原生指针。
//
// ⚠️ 取到原生指针后**先释放表锁再操作**。条件变量的等待会阻塞,若握着表锁
// 等待,别的线程连创建锁都做不了,会直接死锁。
namespace {
#ifdef _WIN32
using MutexType = CRITICAL_SECTION;
using CondType = CONDITION_VARIABLE;
#else
using MutexType = pthread_mutex_t;
using CondType = pthread_cond_t;
#endif
std::vector<MutexType *> &mutexTable() { static std::vector<MutexType *> v; return v; }
std::vector<CondType *> &condTable() { static std::vector<CondType *> v; return v; }
// 从句柄取出原生指针;句柄无效返回 nullptr
MutexType *mutexOf(int64_t id) {
  if (id < 1 || id > static_cast<int64_t>(mutexTable().size())) return nullptr;
  return mutexTable()[static_cast<size_t>(id - 1)];
}
CondType *condOf(int64_t id) {
  if (id < 1 || id > static_cast<int64_t>(condTable().size())) return nullptr;
  return condTable()[static_cast<size_t>(id - 1)];
}
}  // namespace

// 创建一个互斥锁,返回其句柄。
extern "C" xmex::Value sync_mutex_new() {
#ifdef _WIN32
  auto *m = new MutexType();
  InitializeCriticalSection(m);
#else
  auto *m = new MutexType();
  pthread_mutex_init(m, nullptr);
#endif
  lockAcq();
  mutexTable().push_back(m);
  const int64_t h = static_cast<int64_t>(mutexTable().size());
  lockRel();
  return int_(h);
}

// 加锁(阻塞直到拿到)。
extern "C" xmex::Value sync_mutex_lock(const xmex::Value *h) {
  if (h == nullptr || h->tag != xmex::kInt) runtime_error("互斥锁需要一个句柄");
  const int64_t id = h->as.i;
  lockAcq();
  MutexType *m = mutexOf(id);
  lockRel();
  if (m == nullptr) runtime_error("无效的互斥锁句柄");
#ifdef _WIN32
  EnterCriticalSection(m);
#else
  pthread_mutex_lock(m);
#endif
  return none_();
}

// 解锁。
extern "C" xmex::Value sync_mutex_unlock(const xmex::Value *h) {
  if (h == nullptr || h->tag != xmex::kInt) runtime_error("互斥锁需要一个句柄");
  const int64_t id = h->as.i;
  lockAcq();
  MutexType *m = mutexOf(id);
  lockRel();
  if (m == nullptr) runtime_error("无效的互斥锁句柄");
#ifdef _WIN32
  LeaveCriticalSection(m);
#else
  pthread_mutex_unlock(m);
#endif
  return none_();
}

// 销毁互斥锁。槽位置空,重复销毁是安全的空操作。
extern "C" xmex::Value sync_mutex_free(const xmex::Value *h) {
  if (h == nullptr || h->tag != xmex::kInt) runtime_error("互斥锁需要一个句柄");
  const int64_t id = h->as.i;
  lockAcq();
  MutexType *m = nullptr;
  if (id >= 1 && id <= static_cast<int64_t>(mutexTable().size())) {
    m = mutexTable()[static_cast<size_t>(id - 1)];
    mutexTable()[static_cast<size_t>(id - 1)] = nullptr;
  }
  lockRel();
  if (m != nullptr) {
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
    delete m;
  }
  return none_();
}

// 创建一个条件变量,返回其句柄。
extern "C" xmex::Value sync_cond_new() {
#ifdef _WIN32
  auto *c = new CondType();
  InitializeConditionVariable(c);
#else
  auto *c = new CondType();
  pthread_cond_init(c, nullptr);
#endif
  lockAcq();
  condTable().push_back(c);
  const int64_t h = static_cast<int64_t>(condTable().size());
  lockRel();
  return int_(h);
}

// 等待通知:等待期间原子地放开传入的锁,被唤醒后重新拿回它。
// 这正是"检查条件"与"等待"之间不丢唤醒的关键。
extern "C" xmex::Value sync_cond_wait(const xmex::Value *ch, const xmex::Value *mh) {
  if (ch == nullptr || ch->tag != xmex::kInt) runtime_error("条件变量需要一个句柄");
  if (mh == nullptr || mh->tag != xmex::kInt) runtime_error("等待需要一把互斥锁");
  const int64_t cid = ch->as.i;
  const int64_t mid = mh->as.i;
  lockAcq();
  CondType *c = condOf(cid);
  MutexType *m = mutexOf(mid);
  lockRel();
  if (c == nullptr) runtime_error("无效的条件变量句柄");
  if (m == nullptr) runtime_error("无效的互斥锁句柄");
#ifdef _WIN32
  SleepConditionVariableCS(c, m, INFINITE);
#else
  pthread_cond_wait(c, m);
#endif
  return none_();
}

// 唤醒一个等待者。
extern "C" xmex::Value sync_cond_signal(const xmex::Value *ch) {
  if (ch == nullptr || ch->tag != xmex::kInt) runtime_error("条件变量需要一个句柄");
  const int64_t cid = ch->as.i;
  lockAcq();
  CondType *c = condOf(cid);
  lockRel();
  if (c == nullptr) runtime_error("无效的条件变量句柄");
#ifdef _WIN32
  WakeConditionVariable(c);
#else
  pthread_cond_signal(c);
#endif
  return none_();
}

// 唤醒全部等待者。
extern "C" xmex::Value sync_cond_broadcast(const xmex::Value *ch) {
  if (ch == nullptr || ch->tag != xmex::kInt) runtime_error("条件变量需要一个句柄");
  const int64_t cid = ch->as.i;
  lockAcq();
  CondType *c = condOf(cid);
  lockRel();
  if (c == nullptr) runtime_error("无效的条件变量句柄");
#ifdef _WIN32
  WakeAllConditionVariable(c);
#else
  pthread_cond_broadcast(c);
#endif
  return none_();
}

// 销毁条件变量。
extern "C" xmex::Value sync_cond_free(const xmex::Value *ch) {
  if (ch == nullptr || ch->tag != xmex::kInt) runtime_error("条件变量需要一个句柄");
  const int64_t cid = ch->as.i;
  lockAcq();
  CondType *c = nullptr;
  if (cid >= 1 && cid <= static_cast<int64_t>(condTable().size())) {
    c = condTable()[static_cast<size_t>(cid - 1)];
    condTable()[static_cast<size_t>(cid - 1)] = nullptr;
  }
  lockRel();
  if (c != nullptr) {
#ifdef _WIN32
    // 条件变量无需显式销毁
#else
    pthread_cond_destroy(c);
#endif
    delete c;
  }
  return none_();
}

// 同步原语的数量快照(供排查与测试使用)。
extern "C" xmex::Value sync_stats() {
  lockAcq();
  const int64_t mcount = static_cast<int64_t>(mutexTable().size());
  const int64_t ccount = static_cast<int64_t>(condTable().size());
  lockRel();
  char buf[128];
  std::snprintf(buf, sizeof(buf), "{\"mutexes\":%lld,\"conds\":%lld}",
                static_cast<long long>(mcount), static_cast<long long>(ccount));
  return str_new(buf, static_cast<int64_t>(std::strlen(buf)));
}

// ============================================================================
// 分布式便捷内建:休眠与时间(2026-10 增补)
// ============================================================================
extern "C" void sleep_ms(int64_t ms) {
  if (ms <= 0) return;
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

extern "C" xmex::Value time_ms() {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  return int_(ms);
}

extern "C" xmex::Value time_s() {
  auto now = std::chrono::system_clock::now().time_since_epoch();
  int64_t s = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  return int_(s);
}
