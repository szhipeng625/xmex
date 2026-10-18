// 通信模块验证探针。
//
// 验证三件事:
//   1. 启动通信服务后,curl 能连上事件流
//   2. 函数进入/退出事件能被正确推送
//   3. 错误事件能被正确推送
#include "xmex/runtime.h"
#include "xmex/value.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

int main() {
  std::printf("=== 启动通信服务 ===\n");
  trace_start(18900);
  std::printf("通信服务已启动在端口 18900\n");
  std::printf("请用 curl http://localhost:18900/events 连接事件流\n");
  std::fflush(stdout);

  // 给 curl 一点时间连接
  usleep(500000);

  std::printf("=== 发送函数进入/退出事件 ===\n");
  int64_t id1 = trace_enter("outer", 0);
  usleep(100000);
  int64_t id2 = trace_enter("inner", id1);
  usleep(50000);
  trace_exit(id2, "inner", 1, 50);
  usleep(100000);
  trace_exit(id1, "outer", 1, 150);

  std::printf("=== 发送变量事件 ===\n");
  trace_vars("{\"x\":42,\"y\":\"hello\"}");

  std::printf("=== 发送错误事件 ===\n");
  trace_error("something went wrong in module X");

  std::printf("=== 事件已全部发送,服务保持运行 ===\n");
  std::printf("按 Ctrl+C 停止\n");
  std::fflush(stdout);

  // 保持运行,让 curl 有时间接收
  sleep(5);

  trace_stop();
  std::printf("通信服务已停止\n");
  return 0;
}
