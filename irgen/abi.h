// LLVM 侧 xmex::Value 的 ABI 封装。
//
// 全项目**只有这一处**把"聚合体按引用传参 + sret 返回"这条约定写下来。
// 每个调用点自己记这件事迟早会漏,而这个错误的表现形式是"链接通过、lookup
// 成功、一执行就段错误",极难定位。详见 docs/llvm-notes.md 第 2 节。
//
// 值的表示约定:IRGen 里每个 xmex::Value 都住在栈上的一个 alloca 槽位里,
// genExpr 返回的是"指向槽位的指针"而不是聚合体本身。
// 这样做的好处是传给运行时函数时天然就是指针,不需要在每个调用点临时 spill;
// 代价是多了一些栈流量,靠 LLVM 的优化去消。
#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <map>
#include <string>

namespace xmex {

class Abi {
 public:
  explicit Abi(llvm::Module &M);

  llvm::StructType *valueTy() const { return valueTy_; }
  llvm::Type *i32Ty() const { return i32Ty_; }
  llvm::Type *i64Ty() const { return i64Ty_; }
  llvm::Type *f64Ty() const { return f64Ty_; }
  llvm::Type *ptrTy() const { return ptrTy_; }
  llvm::Type *voidTy() const { return voidTy_; }
  llvm::Attribute sretAttr() const { return sretAttr_; }

  // 把 alloca 放进**函数入口块**。所有局部空间的分配都必须走这里 ——
  // 这是硬性要求,不是优化,见 abi.cpp 里 allocEntry 的说明。
  llvm::AllocaInst *allocEntry(llvm::IRBuilder<> &B, llvm::Type *ty,
                               llvm::Value *count, const llvm::Twine &name);

  // 分配一个 xmex::Value 槽位
  llvm::Value *newSlot(llvm::IRBuilder<> &B, const llvm::Twine &name = "v");

  // 分配一块 count 个 xmex::Value 的连续数组(用于给运行时传元素数组)
  llvm::Value *newSlotArray(llvm::IRBuilder<> &B, int64_t count,
                            const llvm::Twine &name);

  // 往槽位里写装箱的常量
  void storeInt(llvm::IRBuilder<> &B, llvm::Value *slot, int64_t v);
  void storeFloat(llvm::IRBuilder<> &B, llvm::Value *slot, double v);
  void storeBool(llvm::IRBuilder<> &B, llvm::Value *slot, bool v);
  void storeNone(llvm::IRBuilder<> &B, llvm::Value *slot);

  // 把一个未装箱的 i64 SSA 值装箱后写入槽位(循环变量每轮要重新装箱)
  void storeI64AsInt(llvm::IRBuilder<> &B, llvm::Value *slot, llvm::Value *i64v);

  // 把槽位里的值原样复制到另一个槽位
  void copySlot(llvm::IRBuilder<> &B, llvm::Value *dst, llvm::Value *src);

  // 从槽位里读
  llvm::Value *loadTag(llvm::IRBuilder<> &B, llvm::Value *slot);
  llvm::Value *loadInt(llvm::IRBuilder<> &B, llvm::Value *slot);
  llvm::Value *loadFloat(llvm::IRBuilder<> &B, llvm::Value *slot);

  // --- 运行时调用 ---
  // 自动按正确 ABI 声明函数、投入槽位指针、把结果放进新槽位。

  // (xmex::Value, xmex::Value) -> xmex::Value
  llvm::Value *callBinary(llvm::IRBuilder<> &B, llvm::StringRef fn,
                          llvm::Value *a, llvm::Value *b);
  // (xmex::Value) -> xmex::Value
  llvm::Value *callUnary(llvm::IRBuilder<> &B, llvm::StringRef fn, llvm::Value *a);
  // () -> xmex::Value   —— 如 read_int()
  llvm::Value *callNullary(llvm::IRBuilder<> &B, llvm::StringRef fn);
  // (ptr, i64) -> xmex::Value —— 如 tuple_new(elems, n)、str_new(bytes, len)
  llvm::Value *callPtrI64(llvm::IRBuilder<> &B, llvm::StringRef fn,
                          llvm::Value *p, llvm::Value *i);
  // (xmex::Value) -> i32
  llvm::Value *callPredicate(llvm::IRBuilder<> &B, llvm::StringRef fn, llvm::Value *a);

  // 通用形式:(xmex::Value*, <paramTypes...>) -> xmex::Value,返回值走 sret。
  // 参数类型必须显式给出 —— 上面那些 call* 只覆盖了固定形状,而像
  // slice(obj, lo, hi, step)、call_method(obj, name, len, args, n) 这种
  // 混合指针与标量的签名没法用一个"全是 ptr"的 helper 表达。
  //
  // ⚠️ 不要省掉 paramTypes 自己拼一个全 ptr 的版本:cache_ 只按**函数名**缓存,
  // 同一个名字被两种签名声明两次时,第二次会静默复用第一次的 Function* 配上
  // 错误的 FunctionType,最终表现又是"链接通过、一执行就段错误"。
  // getOrDeclare() 里做了签名校验,写错会立刻报错而不是等到运行时。
  llvm::Value *callN(llvm::IRBuilder<> &B, llvm::StringRef fn,
                     llvm::ArrayRef<llvm::Type *> paramTypes,
                     llvm::ArrayRef<llvm::Value *> args);

  // 不返回值的运行时函数(如 setindex)。参数全是普通指针/标量,
  // 不涉及聚合体按值传递,所以没有 sret 那一位。
  void callVoidN(llvm::IRBuilder<> &B, llvm::StringRef fn,
                 llvm::ArrayRef<llvm::Type *> paramTypes,
                 llvm::ArrayRef<llvm::Value *> args);

  // print_(xmex::Value* vals, i64 n) —— 数组参数是普通指针,不受聚合体规则影响
  void callPrint(llvm::IRBuilder<> &B, llvm::Value *slotArray, int64_t n);

  // unpack(const xmex::Value* t, xmex::Value* out, i64 n) —— 把元组解包到 out
  void callUnpack(llvm::IRBuilder<> &B, llvm::Value *tupleSlot,
                  llvm::Value *outArray, int64_t n);

  // (xmex::Value) -> i64。取出整数负载(供 range 的边界用);非整数会报运行时错误。
  llvm::Value *callUnaryI64(llvm::IRBuilder<> &B, llvm::StringRef fn, llvm::Value *a);

  // 把槽位里的值当布尔用,返回 i1
  llvm::Value *truthyAsI1(llvm::IRBuilder<> &B, llvm::Value *slot);

  // 生成 runtime_error(msg) 并接 unreachable。
  // 调用方负责先建好错误分支的基本块。
  void emitRuntimeError(llvm::IRBuilder<> &B, const llvm::Twine &msg);

 private:
  // 按名字取回运行时函数:先查 cache_,再查当前 Module(可能是别处已经建过的),
  // 都没有才按给定签名新建。三条路径的结果都会写回 cache_。
  //
  // 统一走这一个入口是有原因的:callPrint / callUnpack 早先是直接
  // M_.getFunction() + Function::Create()、完全绕过 cache_ 的。那样一旦同一个
  // 名字从两条路都被声明,LLVM 会把后建的那个改名成 print_.1,而 DLL 里并没有
  // 这个符号 —— 又是一次"链接通过、lookup 失败"。
  //
  // hasSret 为真时给第一个参数补 sret(%xmex::Value) 属性。签名不一致会直接
  // report_fatal_error:这是编译器自身的内部约定被破坏,不能静默带过。
  llvm::Function *getOrDeclare(llvm::StringRef name, llvm::FunctionType *ft,
                               bool hasSret);

  llvm::Function *declareBinRet(llvm::StringRef name);     // (xmex::Value,xmex::Value)->xmex::Value
  llvm::Function *declareUnaryRet(llvm::StringRef name);   // (xmex::Value)->xmex::Value
  llvm::Function *declareNullaryRet(llvm::StringRef name); // ()->xmex::Value
  llvm::Function *declarePtrI64Ret(llvm::StringRef name);  // (ptr,i64)->xmex::Value
  llvm::Function *declareUnaryPred(llvm::StringRef name);  // (xmex::Value)->i32
  llvm::Function *declareUnaryI64(llvm::StringRef name);   // (xmex::Value)->i64

  llvm::Value *packAggregate(llvm::IRBuilder<> &B, llvm::Value *tag, llvm::Value *payloadI64);

  llvm::Module &M_;
  llvm::LLVMContext &Ctx_;
  llvm::StructType *valueTy_ = nullptr;
  llvm::Type *i32Ty_ = nullptr;
  llvm::Type *i64Ty_ = nullptr;
  llvm::Type *f64Ty_ = nullptr;
  llvm::Type *ptrTy_ = nullptr;
  llvm::Type *voidTy_ = nullptr;
  llvm::Attribute sretAttr_;
  std::map<std::string, llvm::Function *> cache_;
};

}  // namespace xmex
