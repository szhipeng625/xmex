// 诊断工具:确认 LLVM 目标注册表的状态。
//
// 起因是 xmexc 报 "no targets are registered" —— 目标初始化那一组调用
// 明明链接通过、也确实执行了,注册表却是空的。这个探针把注册表的真实内容
// 打出来,避免继续靠猜。
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <string>

int main() {
  std::printf("=== 显式初始化 X86 ===\n");
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmPrinter();
  LLVMInitializeX86AsmParser();
  std::printf("  调用完成\n\n");

  std::printf("=== 注册表中的目标 ===\n");
  llvm::TargetRegistry::printRegisteredTargetsForVersion(llvm::errs());
  std::printf("\n");

  std::printf("=== lookupTarget 测试 ===\n");
  for (const char *triple : {"x86_64-w64-windows-gnu", "x86_64-pc-windows-msvc",
                             "x86_64-unknown-linux-gnu"}) {
    std::string err;
    // 用接受 Triple 的重载;收 StringRef 的那个在 LLVM 22 已弃用
    const llvm::Target *t = llvm::TargetRegistry::lookupTarget(triple, err);
    if (t) {
      std::printf("  [ok]   %-26s -> %s\n", triple, t->getName());
    } else {
      std::printf("  [FAIL] %-26s -> %s\n", triple, err.c_str());
    }
  }

  return 0;
}
