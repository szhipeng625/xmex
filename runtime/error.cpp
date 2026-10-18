// 运行时错误处理。
//
// v1 没有异常、没有栈展开 —— IRGen 不会生成 landing pad,运行时报错直接
// 打印并 exit(1)。这是刻意的简化:异常会要求 JIT 生成的代码带 SEH 表,
// 而 ORC 的 MinGW 支持目前很不完整(见 docs/llvm-notes.md 的 R1)。
#include "xmex/value.h"

#include <cstdio>
#include <cstdlib>

extern "C" {

// 通信模块的前置声明:避免 error.cpp 依赖 trace.cpp 的头文件。
// 链接时由运行时库统一解析。
extern void trace_error(const char *msg);

[[noreturn]] void runtime_error(const char *msg) {
  // 先冲 stdout:报错信息不能插在用户程序输出中间
  std::fflush(stdout);
  // 把错误推送到前端(若通信模块已启用)
  trace_error(msg);
  std::fprintf(stderr, "Xmex error: %s\n", msg);
  std::fflush(stderr);
  std::exit(1);
}

// 供 IRGen 在无法静态判定类型时使用
const char *tag_name(int32_t tag) {
  switch (tag) {
    case xmex::kNull:  return "None";
    case xmex::kBool:  return "bool";
    case xmex::kInt:   return "int";
    case xmex::kFloat: return "float";
    case xmex::kStr:   return "str";
    case xmex::kList:  return "list";
    case xmex::kDict:  return "dict";
    case xmex::kTuple: return "tuple";
    default:       return "<unknown>";
  }
}

}  // extern "C"
