// Stage 0 门槛之二(最高风险):证明 ORC LLJIT 在 MinGW/COFF 上能把
// LLVM 生成的代码与 GCC 编译的运行时 DLL 接起来。
//
// 这个 spike 手工构造两个函数,刻意做成对照组:
//
//   define i64 @jit_add_plain(i64 %a, i64 %b)      ; 纯 LLVM 运算,不碰运行时
//     -> 验证 JIT 生成代码的"执行"路径本身通不通
//
//   declare lite::Value @add(lite::Value, lite::Value)      ; 来自 libpylite_runtime.dll
//   define i64 @jit_add_boxed(i64 %a, i64 %b)      ; 装箱后跨边界调用 add
//     -> 额外验证符号解析 + {i32,i64} 聚合体按值传参的 ABI 互操作
//
// 两个都过,才说明这条路真的通了。segfault 在哪一个上发生,直接指出问题层次。
#include "pylite/value.h"

#include "llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <memory>

using namespace llvm;

namespace {

// 把一个 i64 SSA 值装箱成 lite::Value 聚合体。
// 用 insertvalue 而不是调构造函数:不产生额外调用,且构造出的聚合体
// 直接就是 SSA 值,不碰内存。
Value *boxInt(IRBuilder<> &B, StructType *V, Value *N) {
  Value *S = UndefValue::get(V);
  S = B.CreateInsertValue(S, ConstantInt::get(B.getInt32Ty(), lite::kInt), 0, "tag");
  S = B.CreateInsertValue(S, N, 1, "pay");
  return S;
}

std::unique_ptr<Module> buildSpikeModule(LLVMContext &Ctx, const DataLayout &DL) {
  auto M = std::make_unique<Module>("jit_spike", Ctx);
  M->setDataLayout(DL);

  auto *I32 = Type::getInt32Ty(Ctx);
  auto *I64 = Type::getInt64Ty(Ctx);
  // 必须与 C++ 侧 sizeof(lite::Value)==16 的布局一致(i32 偏移 0,i64 偏移 8)。
  auto *V = StructType::create(Ctx, {I32, I64}, "lite::Value");
  auto *BinTy = FunctionType::get(I64, {I64, I64}, false);

  // ---- 对照组:纯 LLVM 运算 ----
  {
    auto *Fn = Function::Create(BinTy, Function::ExternalLinkage, "jit_add_plain", *M);
    auto *BB = BasicBlock::Create(Ctx, "entry", Fn);
    IRBuilder<> B(BB);
    B.CreateRet(B.CreateAdd(Fn->getArg(0), Fn->getArg(1), "r"));
  }

  // ---- 实验组:调用运行时 add ----
  auto *Ptr = PointerType::getUnqual(Ctx);
#ifdef _WIN32
  // 关键:必须按 Win64 ABI 显式声明边界 —— 16 字节聚合体按引用传递,
  // 返回值走隐藏的 sret 指针。落到寄存器就是:
  //     RCX = sret 返回缓冲区, RDX = &a, R8 = &b
  // 这正是 GCC 编译 `lite::Value add(lite::Value, lite::Value)` 时期望的约定。
  //
  // 反面教材:如果写成 `declare %lite::Value @add(%lite::Value, %lite::Value)` 按值声明,
  // LLVM 会挑另一套降低方式,与 GCC 不一致 —— 调用时直接段错误(已实测)。
  Attribute SretAttr = Attribute::getWithStructRetType(Ctx, V);
  Function::Create(FunctionType::get(Type::getVoidTy(Ctx), {Ptr, Ptr, Ptr}, false),
                   Function::ExternalLinkage, "add", *M)
      ->addParamAttr(0, SretAttr);
  {
    auto *Fn = Function::Create(BinTy, Function::ExternalLinkage, "jit_add_boxed", *M);
    auto *BB = BasicBlock::Create(Ctx, "entry", Fn);
    IRBuilder<> B(BB);
    auto *SlotA = B.CreateAlloca(V, nullptr, "a.slot");
    auto *SlotB = B.CreateAlloca(V, nullptr, "b.slot");
    auto *SlotR = B.CreateAlloca(V, nullptr, "r.slot");
    B.CreateStore(boxInt(B, V, Fn->getArg(0)), SlotA);
    B.CreateStore(boxInt(B, V, Fn->getArg(1)), SlotB);
    auto *C = B.CreateCall(M->getFunction("add"), {SlotR, SlotA, SlotB});
    C->addParamAttr(0, SretAttr);
    Value *Payload = B.CreateLoad(I64, B.CreateStructGEP(V, SlotR, 1), "r.i");
    B.CreateRet(Payload);
  }
#else
  // Linux System V ABI: 16 字节的 {i32, i64} 结构体通过 RAX+RDX 寄存器
  // 直接返回，不需要 sret 指针。函数声明为 %lite::Value @add(ptr, ptr)。
  // add 按值接收和返回 lite::Value 结构体
  Function::Create(FunctionType::get(V, {V, V}, false),
                   Function::ExternalLinkage, "add", *M);
  {
    auto *Fn = Function::Create(BinTy, Function::ExternalLinkage, "jit_add_boxed", *M);
    auto *BB = BasicBlock::Create(Ctx, "entry", Fn);
    IRBuilder<> B(BB);
    auto *ValA = boxInt(B, V, Fn->getArg(0));
    auto *ValB = boxInt(B, V, Fn->getArg(1));
    auto *RetVal = B.CreateCall(M->getFunction("add"), {ValA, ValB}, "r");
    Value *Payload = B.CreateExtractValue(RetVal, 1, "r.i");
    B.CreateRet(Payload);
  }
#endif

  return M;
}

// 用 int64_t 收发,避免在调用点引入聚合体 ABI 的额外变量
using BinFn = int64_t (*)(int64_t, int64_t);

int g_failures = 0;

void runCase(orc::LLJIT &J, const char *Name) {
  auto Sym = J.lookup(Name);
  if (!Sym) {
    std::printf("  [FAIL] %s: 符号未找到: %s\n", Name, toString(Sym.takeError()).c_str());
    ++g_failures;
    return;
  }
  auto *Fn = Sym->toPtr<BinFn>();
  std::printf("  [..]   %s @ %p,调用中...\n", Name, reinterpret_cast<void *>(Fn));

  int64_t R = Fn(1, 2);
  if (R == 3) {
    std::printf("  [ok]   %s(1, 2) == 3\n", Name);
  } else {
    std::printf("  [FAIL] %s(1, 2) == %lld (期望 3)\n", Name, static_cast<long long>(R));
    ++g_failures;
  }
}

}  // namespace

int main(int argc, char **argv) {
  // 不缓冲:这个 spike 是拿来定位崩溃的,段错误发生时缓冲区里的诊断信息
  // 会一起丢掉,那样就只能看到"什么都没有"。
  std::setvbuf(stdout, nullptr, _IONBF, 0);

#ifdef _WIN32
  const char *RuntimeDll = (argc > 1) ? argv[1] : "libpylite_runtime.dll";
#else
  const char *RuntimeDll = (argc > 1) ? argv[1] : "libpylite_runtime.so";
#endif

  std::printf("=== JIT spike ===\n");
  std::printf("runtime dll: %s\n", RuntimeDll);

  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  ExitOnError ExitOnErr("jit_spike: ");

  auto JIT = ExitOnErr(orc::LLJITBuilder().create());
  orc::LLJIT &J = *JIT;

  std::printf("JIT triple     : %s\n", J.getTargetTriple().str().c_str());
  std::printf("JIT data layout: %s\n", J.getDataLayout().getStringRepresentation().c_str());

  // JIT 生成的代码要调用 add,而 add 住在 libpylite_runtime.dll 里。
  // Windows 上不能靠"搜索当前进程导出符号" —— MinGW 链接出的 exe 几乎不导出
  // 任何东西。必须显式按名字加载那个 DLL 并从中建搜索生成器。
  auto &ES = J.getExecutionSession();
  auto Gen = ExitOnErr(orc::EPCDynamicLibrarySearchGenerator::Load(ES, RuntimeDll));
  J.getMainJITDylib().addGenerator(std::move(Gen));

  auto Ctx = std::make_unique<LLVMContext>();
  auto M = buildSpikeModule(*Ctx, J.getDataLayout());

  std::string Err;
  raw_string_ostream ErrOS(Err);
  if (verifyModule(*M, &ErrOS)) {
    std::printf("FAILED: 生成的 IR 就没通过验证:\n%s\n", Err.c_str());
    return 1;
  }

  std::printf("\n--- 模块 IR ---\n");
  M->print(outs(), nullptr);
  std::printf("----------------\n\n");

  if (auto E = J.addIRModule(orc::ThreadSafeModule(std::move(M), std::move(Ctx)))) {
    std::printf("FAILED: addIRModule 失败: %s\n", toString(std::move(E)).c_str());
    return 1;
  }

  runCase(J, "jit_add_plain");
  runCase(J, "jit_add_boxed");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
