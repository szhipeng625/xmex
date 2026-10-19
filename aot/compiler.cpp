#include "compiler.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>  // getenv
#include <memory>
#include <optional>

using namespace llvm;

namespace xmex {

namespace {

std::unique_ptr<TargetMachine> makeTargetMachine(const Triple &TT, std::string *error) {
  // 初始化放在这里而不是各个导出函数里 —— 这是所有需要 TargetMachine 的
  // 路径的唯一汇合点。之前 emitObjectFile 里有、emitIRText 里没有,
  // 结果 `--emit-ir` 会先走到未初始化的那条路,报 "no targets are registered"。
  initializeCodeGenTargets();

  std::string err;
  const Target *T = TargetRegistry::lookupTarget(TT.str(), err);
  if (!T) {
    if (error) *error = "找不到目标平台 " + TT.str() + ": " + err;
    return nullptr;
  }

  TargetOptions opt;
  // PIC 是 MinGW 的默认约定;显式给出来避免和 g++ 链接时出现重定位问题
  auto relocModel = std::optional<Reloc::Model>(Reloc::PIC_);
  // LLVM 20 的 createTargetMachine 收 StringRef,LLVM 22 收 Triple;
  // 传 TT.str()(std::string)对两者都能隐式转换,版本间都编得过。
  auto TM = std::unique_ptr<TargetMachine>(T->createTargetMachine(
      TT.str(), /*CPU=*/"generic", /*Features=*/"", opt, relocModel,
      std::nullopt, CodeGenOptLevel::Default));
  if (!TM && error) *error = "无法为目标平台创建 TargetMachine";
  return TM;
}

// 跑一遍标准优化流水线。IRGen 里每个值都住在 alloca 里,
// 不优化的话生成的代码会充满 load/store;这里把它们清理掉。
//
// 设 XMEX_NO_OPT=1 可跳过。这是排查手段:行为在开/关优化之间不一致时,
// 说明问题出在 IR 本身(比如缺少别名信息、可疑的 align),而不是优化器算错。
void runOptimizationPipeline(Module &M, TargetMachine &TM) {
  if (std::getenv("XMEX_NO_OPT") != nullptr) return;

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  PassBuilder PB(&TM);
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
  MPM.run(M, MAM);
}

}  // namespace

void initializeCodeGenTargets() {
  // 刻意**不用** InitializeNativeTarget() / InitializeNativeTargetAsmPrinter()
  // 那一组。它们在 LLVM 22 里是基于 LLVM_NATIVE_TARGET 等宏的条件编译
  // (见 llvm/Support/TargetSelect.h:126),宏没配对时会静默退化成空操作,
  // 只在返回值里体现为 true —— 而返回值极易被忽略,症状就是运行时才报
  // "no targets are registered"。踩过这个坑。
  //
  // 我们本来就钉死了 x86_64-w64-windows-gnu,直接显式初始化 X86 更可控,
  // 依赖关系也一眼可见。这些函数在进程内幂等,可以反复调用。
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmPrinter();
  LLVMInitializeX86AsmParser();
}

bool emitObjectFile(Module &M, const std::string &outPath, std::string *error) {
  const Triple TT(kTargetTriple);
  auto TM = makeTargetMachine(TT, error);
  if (!TM) return false;

  M.setTargetTriple(TT.str());   // LLVM 20 收 StringRef;Triple.str() 兼容各版本
  M.setDataLayout(TM->createDataLayout());

  runOptimizationPipeline(M, *TM);

  // 先写到内存再落盘:避免 raw_fd_ostream 与 PassManager 的生命周期纠缠
  SmallVector<char, 0> buffer;
  raw_svector_ostream os(buffer);
  legacy::PassManager pm;
  // 注意返回 true 表示失败
  if (TM->addPassesToEmitFile(pm, os, /*DwoOut=*/nullptr,
                              CodeGenFileType::ObjectFile)) {
    if (error) *error = "目标平台不支持生成目标文件";
    return false;
  }
  pm.run(M);

  std::error_code ec;
  raw_fd_ostream out(outPath, ec, sys::fs::OF_None);
  if (ec) {
    if (error) *error = "无法写入 " + outPath + ": " + ec.message();
    return false;
  }
  out.write(buffer.data(), buffer.size());
  return true;
}

bool emitIRText(Module &M, const std::string &outPath, std::string *error) {
  const Triple TT(kTargetTriple);
  auto TM = makeTargetMachine(TT, error);
  if (!TM) return false;

  M.setTargetTriple(TT.str());   // LLVM 20 收 StringRef;Triple.str() 兼容各版本
  M.setDataLayout(TM->createDataLayout());

  std::error_code ec;
  raw_fd_ostream out(outPath, ec, sys::fs::OF_None);
  if (ec) {
    if (error) *error = "无法写入 " + outPath + ": " + ec.message();
    return false;
  }
  M.print(out, nullptr);
  return true;
}

}  // namespace xmex
