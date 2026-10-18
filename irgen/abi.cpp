#include "abi.h"

#include "pylite/value.h"  // lite::kInt 等 tag 常量,保证与 C++ 侧同源

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/ErrorHandling.h"  // report_fatal_error

#include <bit>  // std::bit_cast

using namespace llvm;

namespace pylite {

Abi::Abi(Module &M) : M_(M), Ctx_(M.getContext()) {
  i32Ty_ = Type::getInt32Ty(Ctx_);
  i64Ty_ = Type::getInt64Ty(Ctx_);
  f64Ty_ = Type::getDoubleTy(Ctx_);
  ptrTy_ = PointerType::getUnqual(Ctx_);
  voidTy_ = Type::getVoidTy(Ctx_);

  // 同一个 context 里可能建多个 Module(REPL 每次输入一个),
  // 所以先按名字查已有类型,避免反复 create 出 lite::Value.1 / lite::Value.2。
  valueTy_ = StructType::getTypeByName(Ctx_, "lite::Value");
  if (!valueTy_) {
    valueTy_ = StructType::create(Ctx_, {i32Ty_, i64Ty_}, "lite::Value");
  }

  sretAttr_ = Attribute::getWithStructRetType(Ctx_, valueTy_);
}

AllocaInst *Abi::allocEntry(IRBuilder<> &B, Type *ty, Value *count,
                            const Twine &name) {
  // ⚠️ 局部空间一律放在函数的**入口块**,这是正确性要求而不是优化。
  //
  // LLVM 只把入口块里的 alloca 当静态栈槽 —— AllocaInst::isStaticAlloca()
  // 明确要求 "在入口块"，否则会被降低成**每次执行都真的调整栈指针**的动态分配。
  // 于是循环体里只要生成过一个 alloca(比如 `total = total + i` 里那个存放
  // add 结果的槽位),每次迭代都会吃掉一份栈空间,几十万次之后栈溢出。
  //
  // 实测:同一段 `for i in range(N): total = total + i`,
  //   N = 50000   正常
  //   N = 200000  崩溃(进程异常退出,没有任何输出)
  //
  // 放进入口块后,空间在函数序言里一次分配、循环里反复复用,不累积。
  // 语义上也仍然正确:每次调用函数都会新建(循环里给槽位赋值不会串到下一轮,
  // 因为每次使用前都会先写)。
  BasicBlock *bb = B.GetInsertBlock();
  Function *fn = bb ? bb->getParent() : nullptr;
  if (fn && !fn->isDeclaration() && bb != &fn->getEntryBlock()) {
    IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    return tmp.CreateAlloca(ty, count, name);
  }
  return B.CreateAlloca(ty, count, name);
}

Value *Abi::newSlot(IRBuilder<> &B, const Twine &name) {
  return allocEntry(B, valueTy_, nullptr, name);
}

Value *Abi::newSlotArray(IRBuilder<> &B, int64_t count, const Twine &name) {
  return allocEntry(B, valueTy_, B.getInt32(static_cast<uint32_t>(count)), name);
}

Value *Abi::packAggregate(IRBuilder<> &B, Value *tag, Value *payloadI64) {
  Value *agg = UndefValue::get(valueTy_);
  agg = B.CreateInsertValue(agg, tag, 0);
  agg = B.CreateInsertValue(agg, payloadI64, 1);
  return agg;
}

void Abi::storeInt(IRBuilder<> &B, Value *slot, int64_t v) {
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, lite::kInt),
                              ConstantInt::get(i64Ty_, v)),
                slot);
}

void Abi::storeFloat(IRBuilder<> &B, Value *slot, double v) {
  // payload 字段在 LLVM 侧是 i64,所以要按位重新解释 double
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, lite::kFloat),
                              ConstantInt::get(i64Ty_, bits)),
                slot);
}

void Abi::storeBool(IRBuilder<> &B, Value *slot, bool v) {
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, lite::kBool),
                              ConstantInt::get(i64Ty_, v ? 1 : 0)),
                slot);
}

void Abi::storeNone(IRBuilder<> &B, Value *slot) {
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, lite::kNull),
                              ConstantInt::get(i64Ty_, 0)),
                slot);
}

void Abi::storeI64AsInt(IRBuilder<> &B, Value *slot, Value *i64v) {
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, lite::kInt), i64v), slot);
}

void Abi::copySlot(IRBuilder<> &B, Value *dst, Value *src) {
  B.CreateStore(B.CreateLoad(valueTy_, src), dst);
}

Value *Abi::loadTag(IRBuilder<> &B, Value *slot) {
  return B.CreateLoad(i32Ty_, B.CreateStructGEP(valueTy_, slot, 0), "tag");
}

Value *Abi::loadInt(IRBuilder<> &B, Value *slot) {
  return B.CreateLoad(i64Ty_, B.CreateStructGEP(valueTy_, slot, 1), "int");
}

Value *Abi::loadFloat(IRBuilder<> &B, Value *slot) {
  Value *bits = B.CreateLoad(i64Ty_, B.CreateStructGEP(valueTy_, slot, 1), "fbits");
  return B.CreateBitCast(bits, f64Ty_, "flt");
}

// ---------------------------------------------------------------------------
// 运行时函数声明
//
// 这些 declare* 是"聚合体如何跨 ABI 边界"这条约定的唯一落地点,按平台分两种:
//
//   Win64 :聚合体(>8 字节)一律按引用传递,返回值走隐藏的 sret 指针。
//          → void @name(ptr sret(%lite::Value), ptr %a, ptr %b)
//   SysV  :聚合体按值拆分到寄存器,返回值用 RAX:RDX。
//          → %lite::Value @name(%lite::Value %a, %lite::Value %b)
//
// 两侧都必须与 g++ 编译 runtime/*.cpp 时的实际降低一致。写错的表现是
// "链接通过、lookup 成功、一执行就段错误",且几乎无法从崩溃点反推回来
// (见 docs/llvm-notes.md 第 2 节)。
//
// 注意每个函数只声明一次并缓存 —— 重复创建同名函数会让 LLVM 把后来的
// 改名成 add.1,而 DLL 里没有这个符号,症状同上。
// ---------------------------------------------------------------------------

Function *Abi::getOrDeclare(StringRef name, FunctionType *ft, bool hasSret) {
  auto it = cache_.find(name.str());
  if (it != cache_.end()) return it->second;

  // Module 里可能已经有这个名字:例如某次 callPrint 先建了 print_,
  // 之后又有代码按另一条路径要它。把已有的收进 cache_,避免重复声明。
  Function *f = M_.getFunction(name);
  if (!f) {
    f = Function::Create(ft, Function::ExternalLinkage, name, M_);
  } else if (f->getFunctionType() != ft) {
    // 同一个符号名被两种签名声明过。真跑起来会得到一个按错误签名降低的调用 ——
    // 链接能过,执行时段错误,而且几乎不可能从崩溃点反推回这里。宁可现在就炸。
    report_fatal_error(Twine("运行时函数 '") + name +
                       "' 被以两种不同的签名声明过一次;"
                       "每个符号名只能对应一种签名(见 irgen/abi.h 的 callN 说明)");
  }

  if (hasSret) f->addParamAttr(0, sretAttr_);
  cache_[name.str()] = f;
  return f;
}

Function *Abi::declareBinRet(StringRef name) {
#ifdef _WIN32
  // void @name(ptr sret(%lite::Value), ptr %a, ptr %b)
  return getOrDeclare(name,
                      FunctionType::get(voidTy_, {ptrTy_, ptrTy_, ptrTy_}, false),
                      /*hasSret=*/true);
#else
  // Linux System V ABI: %lite::Value @name(%lite::Value %a, %lite::Value %b)
  // LLVM 自动将 {i32,i64} 拆分为寄存器对 (rdi,rsi) 和 (rdx,rcx)
  return getOrDeclare(name,
                      FunctionType::get(valueTy_, {valueTy_, valueTy_}, false),
                      /*hasSret=*/false);
#endif
}

Function *Abi::declareUnaryRet(StringRef name) {
#ifdef _WIN32
  // void @name(ptr sret(%lite::Value), ptr %a)
  return getOrDeclare(name, FunctionType::get(voidTy_, {ptrTy_, ptrTy_}, false),
                      /*hasSret=*/true);
#else
  // Linux: %lite::Value @name(%lite::Value %a)
  return getOrDeclare(name, FunctionType::get(valueTy_, {valueTy_}, false),
                      /*hasSret=*/false);
#endif
}

Function *Abi::declareUnaryPred(StringRef name) {
#ifdef _WIN32
  // i32 @name(ptr %a)  —— 谓词返回 i32 而非 i1,见 value.h
  return getOrDeclare(name, FunctionType::get(i32Ty_, {ptrTy_}, false),
                      /*hasSret=*/false);
#else
  // i32 @name(%lite::Value %a)  —— 谓词返回 i32 而非 i1,见 value.h
  return getOrDeclare(name, FunctionType::get(i32Ty_, {valueTy_}, false),
                      /*hasSret=*/false);
#endif
}

Function *Abi::declareUnaryI64(StringRef name) {
#ifdef _WIN32
  // i64 @name(ptr %a)
  //
  // Win64 上按值聚合体一律降级成指针,于是 len(const lite::Value*) 与
  // to_int(lite::Value) 的实参在机器层是同一个东西 —— 统一按指针声明即可。
  return getOrDeclare(name, FunctionType::get(i64Ty_, {ptrTy_}, false),
                      /*hasSret=*/false);
#else
  // len 接收指针参数，to_int 按值接收 lite::Value
  if (name == "len") {
    return getOrDeclare(name, FunctionType::get(i64Ty_, {ptrTy_}, false),
                        /*hasSret=*/false);
  }
  return getOrDeclare(name, FunctionType::get(i64Ty_, {valueTy_}, false),
                      /*hasSret=*/false);
#endif
}

Function *Abi::declareNullaryRet(StringRef name) {
#ifdef _WIN32
  // void @name(ptr sret(%lite::Value))
  return getOrDeclare(name, FunctionType::get(voidTy_, {ptrTy_}, false),
                      /*hasSret=*/true);
#else
  // Linux: %lite::Value @name()
  return getOrDeclare(name, FunctionType::get(valueTy_, {}, false),
                      /*hasSret=*/false);
#endif
}

Function *Abi::declarePtrI64Ret(StringRef name) {
#ifdef _WIN32
  // void @name(ptr sret(%lite::Value), ptr, i64)
  return getOrDeclare(name,
                      FunctionType::get(voidTy_, {ptrTy_, ptrTy_, i64Ty_}, false),
                      /*hasSret=*/true);
#else
  // Linux: %lite::Value @name(ptr, i64)  —— ptr/i64 参数不受聚合体规则影响
  return getOrDeclare(name,
                      FunctionType::get(valueTy_, {ptrTy_, i64Ty_}, false),
                      /*hasSret=*/false);
#endif
}

Value *Abi::callBinary(IRBuilder<> &B, StringRef fn, Value *a, Value *b) {
  Function *f = declareBinRet(fn);
  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
#ifdef _WIN32
  CallInst *call = B.CreateCall(f, {ret, a, b});
  // 属性不会自动从声明传播到调用点,必须再标一次
  call->addParamAttr(0, sretAttr_);
  return ret;
#else
  // 从栈槽 load 出 lite::Value 值，按值传递给运行时函数
  Value *va = B.CreateLoad(valueTy_, a);
  Value *vb = B.CreateLoad(valueTy_, b);
  Value *val = B.CreateCall(f, {va, vb}, (Twine(fn) + ".r").str());
  B.CreateStore(val, ret);
  return ret;
#endif
}

Value *Abi::callUnary(IRBuilder<> &B, StringRef fn, Value *a) {
  Function *f = declareUnaryRet(fn);
  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
#ifdef _WIN32
  CallInst *call = B.CreateCall(f, {ret, a});
  call->addParamAttr(0, sretAttr_);
  return ret;
#else
  Value *va = B.CreateLoad(valueTy_, a);
  Value *val = B.CreateCall(f, {va}, (Twine(fn) + ".r").str());
  B.CreateStore(val, ret);
  return ret;
#endif
}

Value *Abi::callPredicate(IRBuilder<> &B, StringRef fn, Value *a) {
  Function *f = declareUnaryPred(fn);
#ifdef _WIN32
  // 实参已是栈槽指针,Win64 下直接传
  return B.CreateCall(f, {a}, (Twine(fn) + ".r").str());
#else
  Value *va = B.CreateLoad(valueTy_, a);
  return B.CreateCall(f, {va}, (Twine(fn) + ".r").str());
#endif
}

Value *Abi::callNullary(IRBuilder<> &B, StringRef fn) {
  Function *f = declareNullaryRet(fn);
  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
#ifdef _WIN32
  CallInst *call = B.CreateCall(f, {ret});
  call->addParamAttr(0, sretAttr_);
  return ret;
#else
  Value *val = B.CreateCall(f, {}, (Twine(fn) + ".r").str());
  B.CreateStore(val, ret);
  return ret;
#endif
}

Value *Abi::callPtrI64(IRBuilder<> &B, StringRef fn, Value *p, Value *i) {
  Function *f = declarePtrI64Ret(fn);
  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
#ifdef _WIN32
  CallInst *call = B.CreateCall(f, {ret, p, i});
  call->addParamAttr(0, sretAttr_);
  return ret;
#else
  Value *val = B.CreateCall(f, {p, i}, (Twine(fn) + ".r").str());
  B.CreateStore(val, ret);
  return ret;
#endif
}

void Abi::callPrint(IRBuilder<> &B, Value *slotArray, int64_t n) {
  auto *ft = FunctionType::get(voidTy_, {ptrTy_, i64Ty_}, false);
  Function *f = getOrDeclare("print_", ft, /*hasSret=*/false);
  B.CreateCall(f, {slotArray, ConstantInt::get(i64Ty_, n)});
}

void Abi::callUnpack(IRBuilder<> &B, Value *tupleSlot, Value *outArray, int64_t n) {
  // void @unpack(ptr %tuple, ptr %out, i64 %n)
  // 三个参数都是普通标量/指针,不涉及聚合体按值传递
  auto *ft = FunctionType::get(voidTy_, {ptrTy_, ptrTy_, i64Ty_}, false);
  Function *f = getOrDeclare("unpack", ft, /*hasSret=*/false);
  B.CreateCall(f, {tupleSlot, outArray, ConstantInt::get(i64Ty_, n)});
}

Value *Abi::callN(IRBuilder<> &B, StringRef fn, ArrayRef<Type *> paramTypes,
                  ArrayRef<Value *> args) {
#ifdef _WIN32
  // Win64:返回值走隐藏的 sret 首参,调用完直接读该槽位。
  SmallVector<Type *, 8> tys;
  tys.reserve(paramTypes.size() + 1);
  tys.push_back(ptrTy_);  // sret 槽位
  tys.append(paramTypes.begin(), paramTypes.end());

  Function *f = getOrDeclare(fn, FunctionType::get(voidTy_, tys, false),
                             /*hasSret=*/true);

  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
  SmallVector<Value *, 8> callArgs;
  callArgs.reserve(args.size() + 1);
  callArgs.push_back(ret);
  callArgs.append(args.begin(), args.end());

  CallInst *call = B.CreateCall(f, callArgs);
  call->addParamAttr(0, sretAttr_);
  return ret;
#else
  // callN 用于 slice/call_method 等接收指针参数的函数，
  // 参数已经是栈槽指针，直接传递即可
  Function *f = getOrDeclare(fn, FunctionType::get(valueTy_, paramTypes, false),
                             /*hasSret=*/false);

  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
  Value *val = B.CreateCall(f, args, (Twine(fn) + ".r").str());
  B.CreateStore(val, ret);
  return ret;
#endif
}

void Abi::callVoidN(IRBuilder<> &B, StringRef fn, ArrayRef<Type *> paramTypes,
                    ArrayRef<Value *> args) {
  Function *f =
      getOrDeclare(fn, FunctionType::get(voidTy_, paramTypes, false),
                   /*hasSret=*/false);
  B.CreateCall(f, args);
}

Value *Abi::truthyAsI1(IRBuilder<> &B, Value *slot) {
  Value *t = callPredicate(B, "truthy", slot);
  return B.CreateICmpNE(t, ConstantInt::get(i32Ty_, 0), "truthy");
}

Value *Abi::callUnaryI64(IRBuilder<> &B, StringRef fn, Value *a) {
  Function *f = declareUnaryI64(fn);
#ifdef _WIN32
  // Win64 下 len 与 to_int 的实参都是指针(按值聚合体降级成指针)
  return B.CreateCall(f, {a}, (Twine(fn) + ".r").str());
#else
  // to_int 按值接收 lite::Value，len 按指针接收
  // 通过函数名区分：len 传指针，其余传值
  if (fn == "len") {
    return B.CreateCall(f, {a}, (Twine(fn) + ".r").str());
  }
  Value *va = B.CreateLoad(valueTy_, a);
  return B.CreateCall(f, {va}, (Twine(fn) + ".r").str());
#endif
}

void Abi::emitRuntimeError(IRBuilder<> &B, const Twine &msg) {
  Function *f = M_.getFunction("runtime_error");
  if (!f) {
    auto *ft = FunctionType::get(voidTy_, {ptrTy_}, false);
    f = Function::Create(ft, Function::ExternalLinkage, "runtime_error", M_);
    f->addFnAttr(Attribute::NoReturn);
  }
  Value *str = B.CreateGlobalString(msg.str(), ".errmsg");
  B.CreateCall(f, {str});
  B.CreateUnreachable();
}

}  // namespace pylite
