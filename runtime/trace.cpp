// 运行时通信模块 —— 把程序运行情况实时推送到前端。
//
// 设计要点:
//   1. 事件队列:加锁保护,多线程安全。
//   2. HTTP 服务:在独立线程里跑一个极简的 HTTP 服务,只认 /events 这一个路径,
//      用服务器推送事件把事件流实时推给前端。
//   3. 发送接口:供其他运行时模块调用,把事件塞进队列。
//   4. 零外部依赖:只用标准库和系统调用,不引入任何第三方 HTTP 库。
//
// 事件格式(每行一条 JSON):
//   {"type":"enter","id":1,"name":"func","parent":0}
//   {"type":"exit","id":1,"name":"func","ok":true,"elapsed":12}
//   {"type":"vars","vars":{"x":42,"y":"hello"}}
//   {"type":"error","msg":"something went wrong"}

#include "pylite/runtime.h"

#include <chrono>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SockLen = int;
#define CLOSE_SOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
using SockLen = socklen_t;
#define CLOSE_SOCK close
#define SOCKET int
#define INVALID_SOCKET (-1)
#endif

namespace {

// --- 事件队列 ---
struct TraceEvent {
  std::string data;  // 一行 JSON,不含换行
};
std::vector<TraceEvent> g_events;
bool g_traceEnabled = false;
int g_tracePort = 18900;

#ifdef _WIN32
CRITICAL_SECTION g_traceLock;
bool g_traceLockInit = false;
void traceLock() {
  if (!g_traceLockInit) { InitializeCriticalSection(&g_traceLock); g_traceLockInit = true; }
  EnterCriticalSection(&g_traceLock);
}
void traceUnlock() { LeaveCriticalSection(&g_traceLock); }
#else
pthread_mutex_t g_traceLock = PTHREAD_MUTEX_INITIALIZER;
void traceLock() { pthread_mutex_lock(&g_traceLock); }
void traceUnlock() { pthread_mutex_unlock(&g_traceLock); }
#endif

// --- HTTP 服务 ---
volatile bool g_serverRunning = false;

// 极简 HTTP 响应:只支持 GET /events,返回服务器推送事件流
void handleClient(SOCKET client) {
  char buf[4096];
#ifdef _WIN32
  int n = recv(client, buf, sizeof(buf) - 1, 0);
#else
  ssize_t n = read(client, buf, sizeof(buf) - 1);
#endif
  if (n <= 0) { CLOSE_SOCK(client); return; }
  buf[n] = '\0';

  // 只认 GET /events
  if (std::strncmp(buf, "GET /events", 11) != 0) {
    const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
#ifdef _WIN32
    send(client, resp, static_cast<int>(std::strlen(resp)), 0);
#else
    write(client, resp, std::strlen(resp));
#endif
    CLOSE_SOCK(client);
    return;
  }

  // 服务器推送事件头
  const char *header =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "\r\n";
#ifdef _WIN32
  send(client, header, static_cast<int>(std::strlen(header)), 0);
#else
  write(client, header, std::strlen(header));
#endif

  // 把已有事件一次性发过去,然后持续轮询新事件
  size_t sent = 0;
  while (g_serverRunning) {
    traceLock();
    while (sent < g_events.size()) {
      std::string line = "data: " + g_events[sent].data + "\n\n";
      traceUnlock();
#ifdef _WIN32
      int wr = send(client, line.c_str(), static_cast<int>(line.size()), 0);
#else
      ssize_t wr = write(client, line.c_str(), line.size());
#endif
      if (wr <= 0) { CLOSE_SOCK(client); return; }
      traceLock();
      ++sent;
    }
    traceUnlock();
    // 没有新事件时短暂休眠,避免空转
#ifdef _WIN32
    Sleep(50);
#else
    usleep(50000);
#endif
  }
  CLOSE_SOCK(client);
}

// 每个客户端连接在独立线程中处理:SSE 是常驻长连接,若串行受理,
// 第一条连接会一直占住受理循环,后续连接(如浏览器)全部被阻塞。
#ifdef _WIN32
DWORD WINAPI clientThread(LPVOID arg) {
  handleClient(static_cast<SOCKET>(reinterpret_cast<intptr_t>(arg)));
  return 0;
}
#else
void *clientThread(void *arg) {
  handleClient(static_cast<SOCKET>(reinterpret_cast<intptr_t>(arg)));
  return nullptr;
}
#endif

#ifdef _WIN32
DWORD WINAPI serverThread(LPVOID) {
#else
void *serverThread(void *) {
#endif
  SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv == INVALID_SOCKET) {
#ifdef _WIN32
    return 1;
#else
    return nullptr;
#endif
  }
  int opt = 1;
#ifdef _WIN32
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
#else
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(g_tracePort));
  if (bind(srv, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    CLOSE_SOCK(srv);
#ifdef _WIN32
    return 2;
#else
    return nullptr;
#endif
  }
  listen(srv, 8);
  while (g_serverRunning) {
    SOCKET client = accept(srv, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
#ifdef _WIN32
    HANDLE h = CreateThread(nullptr, 0, clientThread,
                            reinterpret_cast<LPVOID>(static_cast<intptr_t>(client)),
                            0, nullptr);
    if (h) CloseHandle(h); else handleClient(client);
#else
    pthread_t th;
    if (pthread_create(&th, nullptr, clientThread,
                       reinterpret_cast<void *>(static_cast<intptr_t>(client))) == 0) {
      pthread_detach(th);
    } else {
      handleClient(client);
    }
#endif
  }
  CLOSE_SOCK(srv);
#ifdef _WIN32
  return 0;
#else
  return nullptr;
#endif
}

// 把事件塞进队列
// 事件落盘文件。程序退出后事件仍能被查看。
const char *g_traceDumpPath = nullptr;
FILE *g_traceDump = nullptr;

// 触发机器标识:所有 trace 事件自动带上来源主机名
static const std::string &machineTag() {
  static std::string name = [] {
    char buf[256] = {0};
    if (gethostname(buf, sizeof(buf) - 1) == 0) return std::string(buf);
    return std::string("unknown");
  }();
  return name;
}

void pushEvent(const std::string &json) {
  if (!g_traceEnabled) return;
  std::string tagged;
  if (!json.empty() && json[0] == 0x7b) {
    tagged = "{\"machine\":\"" + machineTag() + "\"," + json.substr(1);
  } else {
    tagged = json;
  }
  traceLock();
  g_events.push_back({tagged});
  if (g_traceDump) {
    std::fprintf(g_traceDump, "data: %s\n\n", tagged.c_str());
    std::fflush(g_traceDump);
  }
  traceUnlock();
}

// JSON 转义辅助
std::string jsonStr(const std::string &s) {
  std::string r = "\"";
  for (char c : s) {
    switch (c) {
      case '"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\r': r += "\\r"; break;
      case '\t': r += "\\t"; break;
      default: r += c;
    }
  }
  return r + "\"";
}

int g_nextId = 1;

// --- 耗时与调用层级(供编译期插桩使用) ---
// 帧:记录进入时刻,退出时算出耗时。用顺序号索引,不用指针,避免移动失效。
struct TraceFrame {
  std::chrono::steady_clock::time_point start;
};
std::vector<TraceFrame> g_frames;      // 所有进入过的帧
std::vector<int64_t> g_callStack;      // 当前未退出的帧标识(调用栈)
int64_t nowMillis(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t)
      .count();
}

}  // namespace

// --- 对外接口 ---

// 启动通信服务。port 为 0 表示用默认端口 18900。
// 指定落盘文件路径(可选)。必须在 trace_start 之前调用。
extern "C" void trace_set_dump(const char *path) {
  g_traceDumpPath = path;
}

// cuda / docker 操作事件。category 取 cuda 或 docker,op 为操作名,
// detail 为对象描述,ok 为是否成功,elapsed 为耗时(毫秒)。
extern "C" void trace_op(const char *category, const char *op,
                            const char *detail, int ok, int64_t elapsed) {
  if (!g_traceEnabled || category == nullptr) return;
  std::string json = "{\"type\":\"op\",\"category\":" +
                     jsonStr(category) + ",\"op\":" +
                     jsonStr(op ? op : "") + ",\"detail\":" +
                     jsonStr(detail ? detail : "") + ",\"ok\":" +
                     (ok ? "true" : "false") + ",\"elapsed\":" +
                     std::to_string(elapsed) + "}";
  pushEvent(json);
}

extern "C" void trace_start(int port) {
  if (g_traceEnabled) return;
  if (port > 0) g_tracePort = port;
  if (g_traceDumpPath) {
    g_traceDump = std::fopen(g_traceDumpPath, "w");
  }
  g_traceEnabled = true;
  g_serverRunning = true;
#ifdef _WIN32
  // Windows 上必须先初始化 winsock,否则下面线程里的 socket() 直接返回
  // INVALID_SOCKET(WSANOTINITIALISED),SSE 服务静默失效 —— 不崩溃,但整个
  // 服务等于不存在。gethostname 取机器标识同理。
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  CreateThread(nullptr, 0, serverThread, nullptr, 0, nullptr);
#else
  pthread_t t;
  pthread_create(&t, nullptr, serverThread, nullptr);
  pthread_detach(t);
#endif
}

// 停止通信服务。
extern "C" void trace_stop() {
  g_serverRunning = false;
  g_traceEnabled = false;
  if (g_traceDump) {
    std::fclose(g_traceDump);
    g_traceDump = nullptr;
  }
}

// 函数进入事件。返回一个帧标识,退出时用它关联。
extern "C" int64_t trace_enter(const char *name, int64_t parentId) {
  if (!g_traceEnabled || name == nullptr) return 0;
  traceLock();
  int64_t id = g_nextId++;
  traceUnlock();
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "{\"type\":\"enter\",\"id\":%lld,\"name\":%s,\"parent\":%lld}",
                static_cast<long long>(id), jsonStr(name).c_str(),
                static_cast<long long>(parentId));
  pushEvent(buf);
  return id;
}

// 函数退出事件。ok 为 true 表示正常完成,false 表示失败。elapsed 是耗时(毫秒)。
extern "C" void trace_exit(int64_t id, const char *name, int ok, int64_t elapsed) {
  if (!g_traceEnabled || name == nullptr) return;
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "{\"type\":\"exit\",\"id\":%lld,\"name\":%s,\"ok\":%s,\"elapsed\":%lld}",
                static_cast<long long>(id), jsonStr(name).c_str(),
                ok ? "true" : "false", static_cast<long long>(elapsed));
  pushEvent(buf);
}

// --- 供编译期插桩使用的便捷接口 ---
// 进入:自动记录进入时刻与调用层级,返回帧标识(>0)。
extern "C" int64_t trace_begin(const char *name) {
  if (!g_traceEnabled || name == nullptr) return 0;
  traceLock();
  int64_t id = g_nextId++;
  g_frames.push_back({std::chrono::steady_clock::now()});
  const int64_t parent = g_callStack.empty() ? 0 : g_callStack.back();
  g_callStack.push_back(id);
  traceUnlock();
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "{\"type\":\"enter\",\"id\":%lld,\"name\":%s,\"parent\":%lld}",
                static_cast<long long>(id), jsonStr(name).c_str(),
                static_cast<long long>(parent));
  pushEvent(buf);
  return id;
}
// 进入:附带形参名与实参值,显示名形如 combine(a=7, b=10)。
// paramNames 是形参名数组,paramValues 是各实参值指针数组。
extern "C" int64_t trace_begin_args(const char *name, int count,
                                       const char *const *paramNames,
                                       const lite::Value *const *paramValues) {
  if (!g_traceEnabled || name == nullptr) return 0;
  traceLock();
  int64_t id = g_nextId++;
  g_frames.push_back({std::chrono::steady_clock::now()});
  const int64_t parent = g_callStack.empty() ? 0 : g_callStack.back();
  g_callStack.push_back(id);
  traceUnlock();
  // 拼显示名:函数名(形参名=实参值, ...)。每一段的取值文本在拼接后即被消费,
  // 不会跨越下一次 repr 的分配,指针始终有效。
  // name 可能已带形参签名(如 combine(a, b)),这里先取纯函数名,
  // 再用『形参名=实参值』重新拼装,避免出现 combine(a, b)(a=7, b=10)。
  std::string display = name;
  size_t lp = display.find('(');
  if (lp != std::string::npos) display = display.substr(0, lp);
  display += "(";
  for (int i = 0; i < count; ++i) {
    if (i > 0) display += ", ";
    display += (paramNames && paramNames[i]) ? paramNames[i] : "?";
    display += "=";
    const char *text = "None";
    if (paramValues && paramValues[i] != nullptr) {
      lite::Value r = repr(paramValues[i]);
      if (r.tag == lite::kStr) text = str_data(&r);
    }
    display += text;
  }
  display += ")";
  std::string json = "{\"type\":\"enter\",\"id\":" + std::to_string(id) +
                     ",\"name\":" + jsonStr(display) +
                     ",\"parent\":" + std::to_string(parent) + "}";
  pushEvent(json);
  return id;
}
// 退出:自动算耗时,并从调用栈上弹掉。id 无效时是安全的空操作。
extern "C" void trace_end(int64_t id, const char *name, int ok) {
  if (!g_traceEnabled || name == nullptr) return;
  traceLock();
  int64_t elapsed = 0;
  // 帧的 start 按进入顺序存,id 从 1 起,所以 g_frames[id-1] 就是它
  if (id >= 1 && id <= static_cast<int64_t>(g_frames.size())) {
    elapsed = nowMillis(g_frames[static_cast<size_t>(id - 1)].start);
  }
  if (!g_callStack.empty()) g_callStack.pop_back();
  traceUnlock();
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "{\"type\":\"exit\",\"id\":%lld,\"name\":%s,\"ok\":%s,\"elapsed\":%lld}",
                static_cast<long long>(id), jsonStr(name).c_str(),
                ok ? "true" : "false", static_cast<long long>(elapsed));
  pushEvent(buf);
}
// 单变量上报:name 是变量名,value 是它的可读文本(由编译器先转成字符串)。
extern "C" void trace_var(const char *name, const char *value) {
  if (!g_traceEnabled || name == nullptr) return;
  const std::string v = value ? value : "None";
  char buf[512];
  std::snprintf(buf, sizeof(buf), "{\"type\":\"vars\",\"vars\":{%s:%s}}",
                jsonStr(name).c_str(), jsonStr(v).c_str());
  pushEvent(buf);
}

// 变量赋值事件。vars 是 JSON 格式的键值对,如 {"x":42,"y":"hello"}。
extern "C" void trace_vars(const char *varsJson) {
  if (!g_traceEnabled || varsJson == nullptr) return;
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "{\"type\":\"vars\",\"vars\":%s}", varsJson);
  pushEvent(buf);
}

// 错误事件。
extern "C" void trace_error(const char *msg) {
  if (!g_traceEnabled || msg == nullptr) return;
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                "{\"type\":\"error\",\"msg\":%s}", jsonStr(msg).c_str());
  pushEvent(buf);
}

// 是否已启用。
extern "C" int32_t trace_enabled() {
  return g_traceEnabled ? 1 : 0;
}

// 直接收『变量名 + 值指针』的上报接口。值的文本化在这里完成(用运行时的 repr),
// 编译器侧不用关心字符串怎么取、怎么保证 NUL 结尾 —— 全部收敛到这一个入口。
extern "C" void trace_var_value(const char *name, const lite::Value *v) {
  if (!g_traceEnabled || name == nullptr) return;
  const char *text = "None";
  if (v != nullptr) {
    lite::Value r = repr(v);
    if (r.tag == lite::kStr) text = str_data(&r);
  }
  // ⚠️ text 指向的字符串对象在这之后不能再触发分配:jsonStr / pushEvent 都不走
  // 堆对象分配,所以指针在本函数内始终有效。
  char buf[512];
  std::snprintf(buf, sizeof(buf), "{\"type\":\"vars\",\"vars\":{%s:%s}}",
                jsonStr(name).c_str(), jsonStr(text).c_str());
  pushEvent(buf);
}
