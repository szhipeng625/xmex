// xmexc —— Xmex 的 AOT 编译器。
//
//   xmexc <输入.pys> -o <输出.o> [--emit-ir <输出.ll>]
//
// 产出的是普通 COFF 目标文件,可以直接和 C++ 编译出的目标文件一起链接。
// 生成的符号是 xmex_<模块名>_<函数名>,以及顶层的 xmex_<模块名>_main。
#include "aot/compiler.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "irgen/irgen.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace {

void usage() {
  std::fprintf(stderr,
               "用法: xmexc <输入.pys> -o <输出.o> [-I 名称] [--emit-ir <输出.ll>]\n"
               "\n"
               "  -I <名称>        指定模块名(默认取输入文件的主文件名)\n"
               "  --emit-ir <路径> 同时把 LLVM IR 文本写出来,便于检查代码生成\n"
               "  --trace          (默认开启)插入运行时插桩:函数计时/变量上报,经 SSE(18900) 推送并落盘\n"
               "  --no-trace       关闭插桩(正式压测时用)\n");
}

// 从 "path/to/foo.pys" 取出 "foo"
std::string moduleNameFromPath(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
  return base.empty() ? "unknown" : base;
}

}  // namespace

int main(int argc, char **argv) {
  std::string input, output, emitIRPath, moduleName;
  bool haveOutput = false;
  bool trace = true;  // 默认开启 trace,--no-trace 关闭

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-o") {
      if (++i >= argc) { usage(); return 2; }
      output = argv[i];
      haveOutput = true;
    } else if (a == "-I") {
      if (++i >= argc) { usage(); return 2; }
      moduleName = argv[i];
    } else if (a == "--emit-ir") {
      if (++i >= argc) { usage(); return 2; }
      emitIRPath = argv[i];
    } else if (a == "--trace") {
      trace = true;
    } else if (a == "--no-trace") {
      trace = false;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "未知选项: %s\n", a.c_str());
      usage();
      return 2;
    } else if (input.empty()) {
      input = a;
    } else {
      std::fprintf(stderr, "只能指定一个输入文件\n");
      return 2;
    }
  }

  if (input.empty() || !haveOutput) {
    usage();
    return 2;
  }
  if (moduleName.empty()) moduleName = moduleNameFromPath(input);

  std::ifstream in(input, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "打不开输入文件: %s\n", input.c_str());
    return 2;
  }
  std::stringstream buf;
  buf << in.rdbuf();

  try {
    // --- 前端 ---
    xmex::Lexer lexer(buf.str(), input);
    xmex::Parser parser(lexer.tokenize(), input);
    auto ast = parser.parseModule();

    // --- IRGen ---
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>(moduleName, ctx);
    xmex::IRGen gen(*module, moduleName);
    gen.setTrace(trace);
    if (trace) {
      // 运行时只负责 fopen(\"w\"),目录要由驱动侧先建好;建失败也不致命,
      // 落盘会静默缺席,但 SSE 推送仍然可用。
      //
      // 不能用 std::system("mkdir -p ..."):Windows 上它经 cmd.exe 执行,
      // cmd 的 mkdir 不认 -p,会在当前目录建一个名为 "-p" 的垃圾目录。
      // create_directories 的路径解析与运行期 fopen 一致。
      std::error_code ec;
      std::filesystem::create_directories("/tmp/xmex_trace", ec);
      if (ec) {
        std::fprintf(stderr, "警告: 无法创建 /tmp/xmex_trace,事件落盘不可用\n");
      }
    }
    gen.generate(*ast);

    // 出目标文件之前先验证 IR。这一步能把"前端生成的 IR 本身就不合法"
    // 和"后端/链接阶段的问题"区分开,省掉大量排查时间。
    std::string verr;
    llvm::raw_string_ostream verrOS(verr);
    if (llvm::verifyModule(*module, &verrOS)) {
      std::fprintf(stderr, "错误: 生成的 LLVM IR 不合法:\n%s\n", verr.c_str());
      return 1;
    }

    if (!emitIRPath.empty()) {
      std::string err;
      if (!xmex::emitIRText(*module, emitIRPath, &err)) {
        std::fprintf(stderr, "错误: %s\n", err.c_str());
        return 1;
      }
      std::fprintf(stderr, "已写出 IR: %s\n", emitIRPath.c_str());
    }

    std::string err;
    if (!xmex::emitObjectFile(*module, output, &err)) {
      std::fprintf(stderr, "错误: %s\n", err.c_str());
      return 1;
    }
    std::fprintf(stderr, "已生成目标文件: %s\n", output.c_str());
    return 0;

  } catch (const xmex::SourceError &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
