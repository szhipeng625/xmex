#include "irgen.h"
#include "xmex/value_tags.h"  // 只要 tag 常量,理由见 value_tags.h

#include "frontend/lexer.h"  // SourceError

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

#include <cctype>
#include <string>

using namespace llvm;

namespace xmex {

namespace {

// 把运算符映射到运行时函数名。返回 nullptr 表示不是简单的一元/二元运算。
const char *runtimeForBinOp(const std::string &op) {
  if (op == "+")  return "add";
  if (op == "-")  return "sub";
  if (op == "*")  return "mul";
  if (op == "/")  return "div_";
  if (op == "//") return "floordiv";
  if (op == "%")  return "mod_";
  if (op == "**") return "pow_";
  if (op == "==") return "eq";
  if (op == "!=") return "ne";
  if (op == "<")  return "lt";
  if (op == "<=") return "le";
  if (op == ">")  return "gt";
  if (op == ">=") return "ge";
  return nullptr;
}

// 模块名/函数名里的非字母数字字符换成下划线,保证生成合法的 C 符号
std::string sanitize(const std::string &s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    r += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
  }
  if (r.empty() || std::isdigit(static_cast<unsigned char>(r[0]))) r.insert(r.begin(), '_');
  return r;
}

}  // namespace

// 注意这里必须写 llvm::Module。在 namespace xmex 里,不带限定的 `Module`
// 指的是 xmex::Module(AST 根节点),而不是 LLVM 的模块。
IRGen::IRGen(llvm::Module &M, std::string moduleName)
    : M_(M), Ctx_(M.getContext()), abi_(M), moduleName_(std::move(moduleName)),
      B_(M.getContext()) {}

std::string IRGen::symbolFor(const std::string &funcName) const {
  return "xmex_" + sanitize(moduleName_) + "_" + sanitize(funcName);
}

[[noreturn]] static void irError(int line, int col, const std::string &msg) {
  throw SourceError(line, col, "<irgen>:" + std::to_string(line) + ":" +
                                  std::to_string(col) + ": " + msg);
}

// ---------------------------------------------------------------------------
// 作用域
//
// Python 的变量是**函数作用域**的,不是块作用域 —— if/for/while 里赋值的
// 变量在外面也能看见。所以这里一个函数只有一个作用域,不随块进出压栈。
// 这既是正确语义,也省掉一堆边界情况。
// ---------------------------------------------------------------------------

AllocaInst *IRGen::lookupVar(const std::string &name) const {
  if (scopes_.empty()) return nullptr;
  auto it = scopes_.back().find(name);
  return it == scopes_.back().end() ? nullptr : it->second;
}

AllocaInst *IRGen::declareVar(const std::string &name) {
  // alloca 统一放在入口块的最前面。放在当前插入点会出问题:循环体里声明的
  // 变量每轮都会重新 alloca 一次,栈会一直涨(同 Abi::allocEntry 里的说明)。
  //
  // 与 allocEntry 的差别是这里还要**紧跟着**写一次 None,而且必须在入口块里
  // 紧挨着 alloca —— 变量可能在一个分支里声明、在另一个分支里被读,那时候
  // 写 None 的那条指令根本没执行过,读到的是未初始化的内存。所以 alloca 和
  // store 要成对地一起放进入口块。
  BasicBlock &entry = curFn_->getEntryBlock();
  IRBuilder<> tmp(&entry, entry.begin());
  auto *slot = tmp.CreateAlloca(abi_.valueTy(), nullptr, name);
  abi_.storeNone(tmp, slot);  // 默认 None,避免读到未初始化内存
  scopes_.back()[name] = slot;
  return slot;
}

// ---------------------------------------------------------------------------
// 表达式
//
// 全部返回"指向 xmex::Value 槽位的指针"。
// ---------------------------------------------------------------------------

Value *IRGen::genExpr(const Expr *e) {
  // --- 字面量 ---
  if (auto *x = dynamic_cast<const IntLit *>(e)) {
    Value *s = abi_.newSlot(B_, "int");
    abi_.storeInt(B_, s, x->value);
    return s;
  }
  if (auto *x = dynamic_cast<const FloatLit *>(e)) {
    Value *s = abi_.newSlot(B_, "flt");
    abi_.storeFloat(B_, s, x->value);
    return s;
  }
  if (auto *x = dynamic_cast<const BoolLit *>(e)) {
    Value *s = abi_.newSlot(B_, "bool");
    abi_.storeBool(B_, s, x->value);
    return s;
  }
  if (auto *x = dynamic_cast<const NoneLit *>(e)) {
    Value *s = abi_.newSlot(B_, "none");
    abi_.storeNone(B_, s);
    return s;
  }
  if (auto *x = dynamic_cast<const StringLit *>(e)) {
    // 字节放在一个模块级常量里,长度显式传给 str_new。
    // ⚠️ 必须用带长度的 StringRef,不能图省事走 .c_str() ——
    // 词法层把 "\0" 解码成了真正的 NUL 字节(lexer.cpp),按 C 字符串处理会
    // 在那里截断,而且 StringRef 从 std::string 构造本就是拿 data()/size()。
    llvm::GlobalVariable *bytes = B_.CreateGlobalString(llvm::StringRef(x->value));
    return abi_.callPtrI64(B_, "str_new", bytes,
                           B_.getInt64(static_cast<int64_t>(x->value.size())));
  }

  // --- 名字 ---
  if (auto *x = dynamic_cast<const Name *>(e)) {
    AllocaInst *slot = lookupVar(x->id);
    if (!slot) {
      // 也可能是"把一个函数当值用",这里同样不支持
      irError(x->line, x->col, "使用了未定义的变量 '" + x->id + "'");
    }
    return slot;
  }

  // --- 运算 ---
  if (auto *x = dynamic_cast<const BinOp *>(e)) return genBinOp(x);
  if (auto *x = dynamic_cast<const UnaryOp *>(e)) return genUnaryOp(x);
  if (auto *x = dynamic_cast<const Call *>(e)) return genCall(x);
  if (auto *x = dynamic_cast<const TupleLit *>(e)) return genTupleLit(x);
  if (auto *x = dynamic_cast<const ListLit *>(e)) return genListLit(x);
  if (auto *x = dynamic_cast<const DictLit *>(e)) return genDictLit(x);
  if (auto *x = dynamic_cast<const Subscript *>(e)) return genSubscript(x);
  if (auto *x = dynamic_cast<const SliceExpr *>(e)) return genSlice(x);

  if (auto *x = dynamic_cast<const RangeExpr *>(e)) {
    irError(x->line, x->col, "range() 只能直接用在 for 的迭代对象位置");
  }
  if (auto *x = dynamic_cast<const InputExpr *>(e)) return genInput(x);
  if (auto *x = dynamic_cast<const Lambda *>(e)) return genLambdaExpr(x);
  if (auto *x = dynamic_cast<const Attribute *>(e)) return genAttrGet(x);

  irError(e->line, e->col, "无法生成代码:不认识的表达式节点");
}

// ---------------------------------------------------------------------------
// 连续数组的构造
// ---------------------------------------------------------------------------

void IRGen::storeElement(Value *array, size_t i, Value *slot) {
  // 只能用**单索引**的 GEP。写成 {0, i} 是错的:源类型 %xmex::Value 是结构体,
  // 第二个索引会被解释成"结构体的第几个字段",而不是数组的第几个元素。
  // 单索引形式按 sizeof(%xmex::Value) 步进,正是数组取址要的语义。
  Value *elem = B_.CreateGEP(abi_.valueTy(), array,
                             B_.getInt64(static_cast<uint64_t>(i)), "elem.ptr");
  B_.CreateStore(B_.CreateLoad(abi_.valueTy(), slot), elem);
}

Value *IRGen::evalToArray(const std::vector<ExprPtr> &items, const Twine &name) {
  const size_t n = items.size();
  if (n == 0) {
    // 空数组传空指针。运行时不读它(元素个数另有一个 i64 参数)。
    return ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy()));
  }
  Value *arr = abi_.newSlotArray(B_, static_cast<int64_t>(n), name);
  for (size_t i = 0; i < n; ++i) storeElement(arr, i, genExpr(items[i].get()));
  return arr;
}

Value *IRGen::noneSlot() {
  Value *s = abi_.newSlot(B_, "none");
  abi_.storeNone(B_, s);
  return s;
}

// 构造一个真正的元组值(堆上分配)。用于 `return a, b` 这类场景。
// 这里逐个求值后立即写入新数组是安全的 —— 数组是全新的,不存在
// `a, b = b, a` 那种"写目标会覆盖读取来源"的问题。
Value *IRGen::genTupleLit(const TupleLit *e) {
  Value *arr = evalToArray(e->items, "tuple.vals");
  return abi_.callPtrI64(B_, "tuple_new", arr,
                         B_.getInt64(static_cast<uint64_t>(e->items.size())));
}

Value *IRGen::genListLit(const ListLit *e) {
  Value *arr = evalToArray(e->items, "list.vals");
  return abi_.callPtrI64(B_, "list_new", arr,
                         B_.getInt64(static_cast<uint64_t>(e->items.size())));
}

Value *IRGen::genDictLit(const DictLit *e) {
  const size_t n = e->items.size();

  // dict_new 收的是**两条平行数组**(键一条、值一条),不是交错的。
  // 空指针分别要单独申请:元素个数为 0 时两块指针都是空。
  Value *keys = ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy()));
  Value *vals = keys;
  if (n > 0) {
    keys = abi_.newSlotArray(B_, static_cast<int64_t>(n), "dict.keys");
    vals = abi_.newSlotArray(B_, static_cast<int64_t>(n), "dict.vals");
    for (size_t i = 0; i < n; ++i) {
      storeElement(keys, i, genExpr(e->items[i].first.get()));
      storeElement(vals, i, genExpr(e->items[i].second.get()));
    }
  }

  return abi_.callN(B_, "dict_new", {abi_.ptrTy(), abi_.ptrTy(), abi_.i64Ty()},
                    {keys, vals, B_.getInt64(static_cast<uint64_t>(n))});
}

// 下标。按 tag 分派到字符串/列表/元组/字典在运行时做 —— 编译期不知道
// 变量的类型(类型注解不检查),所以这里只能是统一入口。
Value *IRGen::genSubscript(const Subscript *e) {
  Value *obj = genExpr(e->obj.get());
  Value *idx = genExpr(e->index.get());
  return abi_.callN(B_, "index_", {abi_.ptrTy(), abi_.ptrTy()}, {obj, idx});
}

// 切片。省略的边界传 None 槽位,由运行时按默认值处理 ——
// 这样 IRGen 不必知道"省略上界"和"上界是 len"的区别(负步长下两者相反)。
Value *IRGen::genSlice(const SliceExpr *e) {
  Value *obj = genExpr(e->obj.get());
  Value *lo = e->lo ? genExpr(e->lo.get()) : noneSlot();
  Value *hi = e->hi ? genExpr(e->hi.get()) : noneSlot();
  Value *step = e->step ? genExpr(e->step.get()) : noneSlot();
  return abi_.callN(
      B_, "slice",
      {abi_.ptrTy(), abi_.ptrTy(), abi_.ptrTy(), abi_.ptrTy()},
      {obj, lo, hi, step});
}

Value *IRGen::genBinOp(const BinOp *e) {
  // and / or 需要短路,单独处理
  if (e->op == "and") return genAndOr(e, true);
  if (e->op == "or")  return genAndOr(e, false);

  const char *fn = runtimeForBinOp(e->op);
  if (!fn) irError(e->line, e->col, "无法生成代码:不支持的运算符 '" + e->op + "'");

  // 左右两侧都求值到槽位。注意求值顺序:先左后右。
  Value *a = genExpr(e->lhs.get());
  Value *b = genExpr(e->rhs.get());
  return abi_.callBinary(B_, fn, a, b);
}

// and / or 必须短路,并且返回的是**操作数本身**而不是布尔值
// (`1 and 2` 是 2,不是 True)。用 PHI 合并两个分支上产生的槽位指针。
Value *IRGen::genAndOr(const BinOp *e, bool isAnd) {
  Function *fn = curFn_;
  Value *a = genExpr(e->lhs.get());
  Value *aTruth = abi_.truthyAsI1(B_, a);
  BasicBlock *entryBB = B_.GetInsertBlock();

  BasicBlock *rhsBB = BasicBlock::Create(Ctx_, isAnd ? "and.rhs" : "or.rhs", fn);
  BasicBlock *endBB = BasicBlock::Create(Ctx_, isAnd ? "and.end" : "or.end", fn);

  // and:左侧为真才求右侧;or:左侧为假才求右侧
  if (isAnd) B_.CreateCondBr(aTruth, rhsBB, endBB);
  else       B_.CreateCondBr(aTruth, endBB, rhsBB);

  B_.SetInsertPoint(rhsBB);
  Value *b = genExpr(e->rhs.get());
  BasicBlock *rhsEndBB = B_.GetInsertBlock();
  B_.CreateBr(endBB);

  B_.SetInsertPoint(endBB);
  PHINode *phi = B_.CreatePHI(abi_.ptrTy(), 2, isAnd ? "and.val" : "or.val");
  phi->addIncoming(a, entryBB);   // 短路时结果就是左侧
  phi->addIncoming(b, rhsEndBB);  // 未短路时结果取右侧
  return phi;
}

Value *IRGen::genUnaryOp(const UnaryOp *e) {
  if (e->op == "-") {
    return abi_.callUnary(B_, "neg", genExpr(e->operand.get()));
  }
  if (e->op == "not") {
    return abi_.callUnary(B_, "not_", genExpr(e->operand.get()));
  }
  irError(e->line, e->col, "无法生成代码:不支持的一元运算符 '" + e->op + "'");
}

// len(x) —— 内建。运行时按 tag 分派到字符串/列表/字典/元组。
//
// len 返回的是 i64 而不是 xmex::Value,这里负责装箱。不把 len 的返回类型
// 定成 xmex::Value 是有意的:for 循环的条件要直接拿它跟下标比 i64;而且同一个
// 符号名如果出现两种签名,Abi 的按名字缓存会静默配错 FunctionType。
Value *IRGen::genLen(const Call *e) {
  if (e->args.size() != 1) {
    irError(e->line, e->col,
            "len() 需要 1 个参数,给了 " + std::to_string(e->args.size()) + " 个");
  }
  Value *v = genExpr(e->args[0].get());
  Value *slot = abi_.newSlot(B_, "len");
  abi_.storeI64AsInt(B_, slot, abi_.callUnaryI64(B_, "len", v));
  return slot;
}

// gc_collect() —— 手动触发一次垃圾回收。不返回值(语言里没有"空"的表达式,
// 所以给一个 None,这样 `x = gc_collect()` 也有确定结果)。
//
// 命名上有意用下划线而不是 gc.collect():语言里没有模块概念,`gc.collect()`
// 会被解析成"对变量 gc 做方法调用",而 gc 并不存在。
Value *IRGen::genGcCollect(const Call *e) {
  if (!e->args.empty()) {
    irError(e->line, e->col,
            "gc_collect() 不接受参数,给了 " + std::to_string(e->args.size()) + " 个");
  }
  abi_.callVoidN(B_, "gc_collect", {}, {});
  return noneSlot();
}

// sleep_ms(ms) —— 当前线程休眠指定毫秒数。分布式节点的轮询等待、
// 心跳间隔控制都要用它。无返回值,与 gc_collect 一样给 None。
Value *IRGen::genSleepMs(const Call *e) {
  if (e->args.size() != 1) {
    irError(e->line, e->col,
            "sleep_ms() 需要 1 个参数,给了 " + std::to_string(e->args.size()) + " 个");
  }
  Value *v = genExpr(e->args[0].get());
  Value *ms = abi_.callUnaryI64(B_, "to_int", v);
  abi_.callVoidN(B_, "sleep_ms", {abi_.i64Ty()}, {ms});
  return noneSlot();
}

// time_ms() / time_s() —— 当前 Unix 时间戳。分布式消息的时序控制、
// 选举超时的基准都靠它。
Value *IRGen::genTimeMs(const Call *e) {
  if (!e->args.empty()) {
    irError(e->line, e->col,
            "time_ms() 不接受参数,给了 " + std::to_string(e->args.size()) + " 个");
  }
  return abi_.callNullary(B_, "time_ms");
}
Value *IRGen::genTimeS(const Call *e) {
  if (!e->args.empty()) {
    irError(e->line, e->col,
            "time_s() 不接受参数,给了 " + std::to_string(e->args.size()) + " 个");
  }
  return abi_.callNullary(B_, "time_s");
}

// obj.method(args) —— 走运行时的统一分派入口。
//
// 为什么不在编译期解析成直接调用:本项目的类型注解不做检查,`x.upper()` 里的
// x 到编译期根本没有静态类型。于是方法名拼错是**运行时**错误,这一点在
// docs/language.md 里写明了。
Value *IRGen::genMethodCall(const Call *e, const Attribute *attr) {
  // 实例方法:调用者是记录了类名的变量时,解析成对该类方法的直接调用 ——
  // 实例作为第一个实参(与方法的第一个形参对应),其余实参按序跟在后面。
  // 先解析出调用接收者对应的类名:既支持 `obj.m(...)`(obj 是记录过类名的变量),
  // 也支持 `self.X.m(...)`(self.X 是记录过类型的属性 —— 组合结构)。
  std::string recvClass;
  if (auto *base = dynamic_cast<const Name *>(attr->obj.get())) {
    auto vc = varClasses_.find(curFn_->getName().str() + "::" + base->id);
    if (vc != varClasses_.end()) recvClass = vc->second;
  } else if (auto *inner = dynamic_cast<const Attribute *>(attr->obj.get())) {
    if (auto *base = dynamic_cast<const Name *>(inner->obj.get())) {
      auto vc = varClasses_.find(curFn_->getName().str() + "::" + base->id);
      if (vc != varClasses_.end()) {
        auto ac = classAttrClasses_.find(vc->second + "." + inner->name);
        if (ac != classAttrClasses_.end()) recvClass = ac->second;
      }
    }
  }
  if (!recvClass.empty()) {
      const std::string sym = "xmex_" + sanitize(moduleName_) + "_class_" +
                              sanitize(recvClass) + "_" + sanitize(attr->name);
      Function *f = M_.getFunction(sym);
      if (f == nullptr) {
        // 方法不存在时报清楚的错,而不是回退到内置方法给出误导性的提示
        irError(e->line, e->col,
                "类 '" + recvClass + "' 没有方法 '" + attr->name + "'");
      }
      Value *self = genExpr(attr->obj.get());
      std::vector<Value *> callArgs;
      callArgs.push_back(B_.CreateLoad(abi_.valueTy(), self));
      for (const auto &a : e->args) {
        Value *s = genExpr(a.get());
        callArgs.push_back(B_.CreateLoad(abi_.valueTy(), s));
      }
      Value *ret = abi_.newSlot(B_, attr->name + ".ret");
      Value *val = B_.CreateCall(f, callArgs, attr->name + ".r");
      B_.CreateStore(val, ret);
      return ret;
  }
  // 求值顺序与 Python 一致:先算对象,再算参数
  Value *obj = genExpr(attr->obj.get());
  Value *args = evalToArray(e->args, "call.args");

  GlobalVariable *name = B_.CreateGlobalString(llvm::StringRef(attr->name));
  return abi_.callN(
      B_, "call_method",
      {abi_.ptrTy(), abi_.ptrTy(), abi_.i64Ty(), abi_.ptrTy(), abi_.i64Ty()},
      {obj, name, B_.getInt64(static_cast<int64_t>(attr->name.size())), args,
       B_.getInt64(static_cast<int64_t>(e->args.size()))});
}

// 内置模块名 → 运行时函数前缀映射
// 让 Xmex 脚本能直接写 docker.run()、llm.chat() 等原生语法
static const char *builtinModulePrefix(const std::string &mod, const std::string &fn) {
  // Docker 模块
  if (mod == "docker") {
    if (fn == "run") return "docker_run";
    if (fn == "ps") return "docker_ps";
    if (fn == "stop") return "docker_stop";
    if (fn == "logs") return "docker_logs";
    if (fn == "pull") return "docker_pull";
    if (fn == "generate_dockerfile") return "docker_generate_dockerfile";
    if (fn == "build") return "docker_build";
    if (fn == "push") return "docker_push";
    if (fn == "project_package") return "docker_project_package";
  }
  // LLM 模块
  if (mod == "llm") {
    if (fn == "chat") return "llm_chat";
    if (fn == "create_network") return "llm_create_network";
    if (fn == "add_layer") return "llm_add_layer";
    if (fn == "network_summary") return "llm_network_summary";
    if (fn == "create_dataset") return "llm_create_dataset";
    if (fn == "upload_file") return "llm_upload_file";
    if (fn == "create_finetune") return "llm_create_finetune";
    if (fn == "finetune_status") return "llm_finetune_status";
    if (fn == "list_finetunes") return "llm_list_finetunes";
  }
  // CUDA 模块
  if (mod == "cuda") {
    if (fn == "info") return "cuda_info";
    if (fn == "compile_ptx") return "cuda_compile_ptx";
    if (fn == "launch_kernel") return "cuda_launch_kernel";
    if (fn == "alloc") return "cuda_alloc";
    if (fn == "free") return "cuda_free";
    if (fn == "memcpy_to_device") return "cuda_memcpy_to_device";
    if (fn == "memcpy_from_device") return "cuda_memcpy_from_device";
  }
  // Fusion 模块
  if (mod == "fusion") {
    if (fn == "create_graph") return "fusion_create_graph";
    if (fn == "add_op") return "fusion_add_op";
    if (fn == "apply_rules") return "fusion_apply_rules";
    if (fn == "generate_kernel") return "fusion_generate_kernel";
    if (fn == "autotune") return "fusion_autotune";
    if (fn == "compile_and_run") return "fusion_compile_and_run";
    if (fn == "benchmark") return "fusion_benchmark";
  }
  // Inference 模块
  if (mod == "inference") {
    if (fn == "load_model") return "inference_load_model";
    if (fn == "generate") return "inference_generate";
    if (fn == "kv_cache_stats") return "inference_kv_cache_stats";
    if (fn == "flash_attention_kernel") return "inference_flash_attention_kernel";
    if (fn == "multi_gpu_info") return "inference_multi_gpu_info";
    if (fn == "profile") return "inference_profile";
    if (fn == "unload") return "inference_unload";
  }
  // Tokenizer 模块
  if (mod == "tokenizer") {
    if (fn == "load") return "tokenizer_load";
    if (fn == "encode") return "tokenizer_encode";
    if (fn == "decode") return "tokenizer_decode";
    if (fn == "info") return "tokenizer_info";
  }
  // Server 模块
  if (mod == "server") {
    if (fn == "start") return "server_start";
    if (fn == "stop") return "server_stop";
    if (fn == "status") return "server_status";
    if (fn == "config") return "server_config";
    if (fn == "tls") return "server_tls";
  }
  // Quantize 模块
  if (mod == "quantize") {
    if (fn == "fp16_to_awq") return "quantize_fp16_to_awq";
    if (fn == "arch_template") return "quantize_arch_template";
    if (fn == "verify_precision") return "quantize_verify_precision";
    if (fn == "model_info") return "quantize_model_info";
  }
  // Plugin 模块
  if (mod == "plugin") {
    if (fn == "scan") return "plugin_scan";
    if (fn == "register") return "plugin_register";
    if (fn == "call") return "plugin_call";
    if (fn == "list") return "plugin_list";
  }
  // Engine 模块
  if (mod == "engine") {
    if (fn == "init") return "engine_init";
    if (fn == "add_request") return "engine_add_request";
    if (fn == "step") return "engine_step";
    if (fn == "status") return "engine_status";
    if (fn == "reset") return "engine_reset";
    if (fn == "serve") return "engine_serve";
  }
  // Sandbox 判题沙箱模块
  if (mod == "sandbox") {
    if (fn == "create") return "sandbox_create";
    if (fn == "set_cpu_limit") return "sandbox_set_cpu_limit_v";
    if (fn == "set_mem_limit") return "sandbox_set_mem_limit_v";
    if (fn == "set_max_pids") return "sandbox_set_max_pids_v";
    if (fn == "set_no_seccomp") return "sandbox_set_no_seccomp_v";
    if (fn == "set_work_dir") return "sandbox_set_work_dir_v";
    if (fn == "set_binary") return "sandbox_set_binary_v";
    if (fn == "add_arg") return "sandbox_add_arg_v";
    if (fn == "set_stdin") return "sandbox_set_stdin_v";
    if (fn == "set_stdout") return "sandbox_set_stdout_v";
    if (fn == "set_stderr") return "sandbox_set_stderr_v";
    if (fn == "exec") return "sandbox_exec_v";
    if (fn == "destroy") return "sandbox_destroy_v";
  }
  if (mod == "cluster") {
    if (fn == "connect") return "cluster_connect_v";
    if (fn == "disconnect") return "cluster_disconnect_v";
    if (fn == "list") return "cluster_list_v";
    if (fn == "is_connected") return "cluster_is_connected_v";
    if (fn == "status") return "cluster_status_v";
    if (fn == "nodes") return "cluster_nodes_v";
    if (fn == "health") return "cluster_health_v";
    if (fn == "exec") return "cluster_exec_v";
    if (fn == "query") return "cluster_query_v";
    if (fn == "broadcast") return "cluster_broadcast_v";
    if (fn == "health_all") return "cluster_health_all_v";
  }
  // KvAdmin 节点生命周期管理模块（通过 _v 适配器接收 xmex::Value* 参数）
  if (mod == "kv") {
    if (fn == "build") return "kv_build_v";
    if (fn == "add") return "kv_add_v";
    if (fn == "erase") return "kv_erase_v";
    if (fn == "skip") return "kv_skip_v";
    if (fn == "sleep") return "kv_sleep_v";
    if (fn == "wakeup") return "kv_wakeup_v";
    if (fn == "stop") return "kv_stop_v";
    if (fn == "restart") return "kv_restart_v";
    if (fn == "status") return "kv_status_v";
    if (fn == "list") return "kv_list_v";
    if (fn == "alive") return "kv_alive_v";
    if (fn == "persist") return "kv_persist_v";
    if (fn == "bind_sandbox") return "kv_bind_sandbox_v";
    if (fn == "get_sandbox") return "kv_get_sandbox_v";
    if (fn == "unbind_sandbox") return "kv_unbind_sandbox_v";
    if (fn == "add_box") return "kv_add_box_v";
    if (fn == "locate") return "kv_locate_v";
    if (fn == "self") return "kv_self_v";
    if (fn == "iter") return "kv_iter_v";
    if (fn == "next") return "kv_iter_next_v";
    if (fn == "rewind") return "kv_iter_rewind_v";
    if (fn == "iter_destroy") return "kv_iter_destroy_v";
    if (fn == "iter_live") return "kv_iter_live_v";
    // 声明式调谐方法(2026-10 增补):后台把 observed 拉回 desired,
    // 副本组补足到期望副本数。设计见 docs/distributed-reconcile-design.md
    // 容器节点(2026-10 增补):Docker 容器纳管与故障注入
    if (fn == "container_add") return "kv_container_add_v";
    if (fn == "container_pause") return "kv_container_pause_v";
    if (fn == "container_unpause") return "kv_container_unpause_v";
    if (fn == "container_crash") return "kv_container_crash_v";
    if (fn == "container_logs") return "kv_container_logs_v";
    if (fn == "reconcile_start") return "kv_reconcile_start_v";
    if (fn == "reconcile_stop") return "kv_reconcile_stop_v";
    if (fn == "reconcile_status") return "kv_reconcile_status_v";
    if (fn == "reconcile_once") return "kv_reconcile_once_v";
    if (fn == "unskip") return "kv_unskip_v";
    if (fn == "failures") return "kv_failures_v";
    // 副本组方法(2026-10 增补):声明副本数,调谐循环自动补足
    if (fn == "group_create") return "kv_group_create_v";
    if (fn == "group_add") return "kv_group_add_v";
    if (fn == "group_replicas") return "kv_group_replicas_v";
    if (fn == "group_status") return "kv_group_status_v";
    if (fn == "group_destroy") return "kv_group_destroy_v";
    // 动态库 FFI 方法(2026-10 增补):加载 .dll/.so 并按描述符调用其函数。
    // 详见 runtime/ffi.h 与 docs/ffi-design.md。
    if (fn == "ffi") return "kv_ffi_v";
    if (fn == "ffi_list") return "kv_ffi_list_v";
    // 隔离模式:把 DLL 加载到子进程,DLL 崩溃不影响宿主
    if (fn == "ffi_isolated") return "kv_ffi_isolated_v";
    // 节点内置 MQ 队列方法(2026-10 增补)
    if (fn == "enqueue") return "kv_enqueue_v";
    if (fn == "dequeue") return "kv_dequeue_v";
    if (fn == "done") return "kv_done_v";
    if (fn == "qlen") return "kv_qlen_v";
    if (fn == "set_quota") return "kv_set_quota_v";
    if (fn == "quota") return "kv_quota_v";
    if (fn == "kset") return "kv_kset_v";
    if (fn == "kget") return "kv_kget_v";
  }
  // 本地文件操作模块（节点落盘代码供编译,2026-10 增补）
  if (mod == "fio") {
    if (fn == "write") return "file_write_v";
    if (fn == "read") return "file_read_v";
  }
  // 字符串转换模块（任务参数解析,2026-10 增补）
  if (mod == "strc") {
    if (fn == "to_int") return "str_to_int_v";
    if (fn == "to_str") return "int_to_str_v";
  }
  return nullptr;
}

// 调用点的参数绑定:位置参数 -> 关键字参数 -> 缺省值 -> *args。
// params 为 nullptr 表示拿不到参数表,退回严格按位置匹配。
void IRGen::bindCallArgs(const std::vector<Param> *params, const Call *e,
                         Function *f, const std::string &calleeName,
                         std::vector<Value *> &out) {
  if (params == nullptr) {
    const size_t expected = f->arg_size();
    if (e->args.size() != expected || !e->kwargs.empty()) {
      irError(e->line, e->col,
              "函数 '" + calleeName + "' 需要 " + std::to_string(expected) +
                  " 个参数,给了 " + std::to_string(e->args.size()) + " 个");
    }
    for (const auto &a : e->args) {
      Value *slot = genExpr(a.get());
      out.push_back(B_.CreateLoad(abi_.valueTy(), slot));
    }
    return;
  }
  const auto &ps = *params;
  const size_t total = ps.size();
  bool hasVararg = false;
  for (size_t i = 0; i < total; ++i) {
    if (ps[i].isVararg) hasVararg = true;
  }
  std::vector<Value *> slots(total, nullptr);
  size_t pi = 0;
  for (; pi < e->args.size() && pi < total; ++pi) {
    if (ps[pi].isVararg) break;
    slots[pi] = genExpr(e->args[pi].get());
  }
  for (const auto &kw : e->kwargs) {
    size_t idx = total;
    for (size_t i = 0; i < total; ++i) {
      if (!ps[i].isVararg && ps[i].name == kw.first) { idx = i; break; }
    }
    if (idx == total) {
      irError(e->line, e->col,
              "函数 '" + calleeName + "' 没有名为 '" + kw.first + "' 的参数");
    }
    if (slots[idx]) {
      irError(e->line, e->col, "参数 '" + kw.first + "' 被重复传入");
    }
    slots[idx] = genExpr(kw.second.get());
  }
  std::vector<Value *> extra;
  for (; pi < e->args.size(); ++pi) extra.push_back(genExpr(e->args[pi].get()));
  if (!extra.empty() && !hasVararg) {
    irError(e->line, e->col, "函数 '" + calleeName + "' 收到了多余的位置参数");
  }
  for (size_t i = 0; i < total; ++i) {
    if (ps[i].isVararg || slots[i]) continue;
    if (ps[i].defaultValue) {
      slots[i] = genExpr(ps[i].defaultValue.get());
    } else {
      irError(e->line, e->col,
              "函数 '" + calleeName + "' 缺少必填参数 '" + ps[i].name + "'");
    }
  }
  for (size_t i = 0; i < total; ++i) {
    if (!ps[i].isVararg) continue;
    if (extra.empty()) {
      Value *np = ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy()));
      slots[i] = abi_.callPtrI64(B_, "list_new", np, B_.getInt64(0));
    } else {
      Value *arr = abi_.newSlotArray(B_, static_cast<int64_t>(extra.size()), "varargs");
      for (size_t k = 0; k < extra.size(); ++k) storeElement(arr, k, extra[k]);
      slots[i] = abi_.callPtrI64(B_, "list_new", arr,
                                B_.getInt64(static_cast<uint64_t>(extra.size())));
    }
  }
  for (size_t i = 0; i < total; ++i) {
    out.push_back(B_.CreateLoad(abi_.valueTy(), slots[i]));
  }
  if (out.size() != f->arg_size()) {
    irError(e->line, e->col, "函数 '" + calleeName + "' 的参数个数与声明不一致");
  }
}

Value *IRGen::genCall(const Call *e) {
  // 检查是否是内置模块调用: docker.run(...) / llm.chat(...) 等
  if (auto *attr = dynamic_cast<const Attribute *>(e->callee.get())) {
    if (auto *modName = dynamic_cast<const Name *>(attr->obj.get())) {
      const char *rtFn = builtinModulePrefix(modName->id, attr->name);
      if (rtFn) {
        // 内置模块调用: 直接映射到运行时函数
        // 所有参数都是 xmex::Value* 指针，通过 callN 传递
        std::vector<Value *> argSlots;
        std::vector<Type *> argTypes;
        argSlots.reserve(e->args.size());
        argTypes.reserve(e->args.size());
        for (const auto &a : e->args) {
          Value *slot = genExpr(a.get());
          argSlots.push_back(slot);
          argTypes.push_back(abi_.ptrTy());
        }

        // 对于返回 void 的函数（docker_stop, cuda_free, cuda_memcpy_to_device）
        if (std::string(rtFn) == "docker_stop" ||
            std::string(rtFn) == "cuda_free" ||
            std::string(rtFn) == "cuda_memcpy_to_device") {
          abi_.callVoidN(B_, rtFn, argTypes, argSlots);
          Value *res = abi_.newSlot(B_, "mod.ret");
          abi_.storeNone(B_, res);
          return res;
        }

        return abi_.callN(B_, rtFn, argTypes, argSlots);
      }
    }
    return genMethodCall(e, attr);
  }

  auto *name = dynamic_cast<const Name *>(e->callee.get());
  if (!name) {
    irError(e->line, e->col, "只支持调用具名函数或 obj.method(...) 形式的方法");
  }

  if (name->id == "print") return genPrint(e);
  if (name->id == "len") return genLen(e);
  if (name->id == "gc_collect") return genGcCollect(e);
  if (name->id == "sleep_ms") return genSleepMs(e);
  if (name->id == "time_ms") return genTimeMs(e);
  if (name->id == "time_s") return genTimeS(e);
  // 类名(...) —— 实例化
  if (classMap_.count(name->id)) return genInstanceNew(name->id, e);

  // 匿名函数:变量名绑定了内部函数时,解析成对该内部函数的直接调用。
  std::string sym;
  const std::vector<Param> *lamParams = nullptr;
  auto lamIt = lambdaVars_.find(curFn_->getName().str() + "::" + name->id);
  if (lamIt != lambdaVars_.end()) {
    sym = lamIt->second;
    auto dIt = lambdaDefs_.find(sym);
    if (dIt != lambdaDefs_.end()) lamParams = &dIt->second->params;
    auto nIt = nestedParams_.find(sym);
    if (nIt != nestedParams_.end()) lamParams = nIt->second;
  } else {
    sym = symbolFor(name->id) + "__impl";
  }
  Function *f = M_.getFunction(sym);
  if (!f) {
    irError(e->line, e->col, "调用了未定义的函数 '" + name->id + "'");
  }

  Value *ret = abi_.newSlot(B_, name->id + ".ret");
  std::vector<Value *> callArgs;
  callArgs.reserve(f->arg_size());
  if (lamParams != nullptr) {
    // 先传捕获的外层变量(与内部函数的前置参数一一对应),再绑实参。
    auto capIt = lambdaCaptures_.find(sym);
    if (capIt != lambdaCaptures_.end()) {
      for (const auto &cn : capIt->second) {
        AllocaInst *cs = lookupVar(cn);
        if (cs != nullptr) {
          callArgs.push_back(B_.CreateLoad(abi_.valueTy(), cs));
        } else {
          Value *ns = abi_.newSlot(B_, "cap.none");
          abi_.storeNone(B_, ns);
          callArgs.push_back(B_.CreateLoad(abi_.valueTy(), ns));
        }
      }
    }
    bindCallArgs(lamParams, e, f, name->id, callArgs);
    Value *lval = B_.CreateCall(f, callArgs, name->id + ".r");
    B_.CreateStore(lval, ret);
    return ret;
  }
  // 有函数定义时按名字绑定:位置参数 -> 关键字参数 -> 缺省值 -> *args。
  // 拿不到定义时(只声明未定义等)退回严格按位置匹配。
  auto defIt = funcMap_.find(name->id);
  if (defIt == funcMap_.end()) {
    const size_t expected = f->arg_size();
    if (e->args.size() != expected || !e->kwargs.empty()) {
      irError(e->line, e->col,
              "函数 '" + name->id + "' 需要 " + std::to_string(expected) +
                  " 个参数,给了 " + std::to_string(e->args.size()) + " 个");
    }
    for (const auto &a : e->args) {
      Value *slot = genExpr(a.get());
      callArgs.push_back(B_.CreateLoad(abi_.valueTy(), slot));
    }
  } else {
    const FuncDef *fd = defIt->second;
    const size_t total = fd->params.size();
    bool hasVararg = false;
    for (size_t i = 0; i < total; ++i) {
      if (fd->params[i].isVararg) hasVararg = true;
    }
    std::vector<Value *> slots(total, nullptr);
    // 位置参数按顺序占前面的形参,遇到 *args 就停下
    size_t pi = 0;
    for (; pi < e->args.size() && pi < total; ++pi) {
      if (fd->params[pi].isVararg) break;
      slots[pi] = genExpr(e->args[pi].get());
    }
    // 关键字参数按名字绑定
    for (const auto &kw : e->kwargs) {
      size_t idx = total;
      for (size_t i = 0; i < total; ++i) {
        if (!fd->params[i].isVararg && fd->params[i].name == kw.first) {
          idx = i;
          break;
        }
      }
      if (idx == total) {
        irError(e->line, e->col,
                "函数 '" + name->id + "' 没有名为 '" + kw.first + "' 的参数");
      }
      if (slots[idx]) {
        irError(e->line, e->col, "参数 '" + kw.first + "' 被重复传入");
      }
      slots[idx] = genExpr(kw.second.get());
    }
    // 剩余位置参数交给 *args;没有 *args 就是给多了
    std::vector<Value *> extra;
    for (; pi < e->args.size(); ++pi) extra.push_back(genExpr(e->args[pi].get()));
    if (!extra.empty() && !hasVararg) {
      irError(e->line, e->col, "函数 '" + name->id + "' 收到了多余的位置参数");
    }
    // 补缺省值 / 检查缺参
    for (size_t i = 0; i < total; ++i) {
      if (fd->params[i].isVararg || slots[i]) continue;
      if (fd->params[i].defaultValue) {
        slots[i] = genExpr(fd->params[i].defaultValue.get());
      } else {
        irError(e->line, e->col,
                "函数 '" + name->id + "' 缺少必填参数 '" + fd->params[i].name + "'");
      }
    }
    // *args 打包成列表
    for (size_t i = 0; i < total; ++i) {
      if (!fd->params[i].isVararg) continue;
      if (extra.empty()) {
        Value *np = ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy()));
        slots[i] = abi_.callPtrI64(B_, "list_new", np, B_.getInt64(0));
      } else {
        Value *arr =
            abi_.newSlotArray(B_, static_cast<int64_t>(extra.size()), "varargs");
        for (size_t k = 0; k < extra.size(); ++k) storeElement(arr, k, extra[k]);
        slots[i] = abi_.callPtrI64(B_, "list_new", arr,
                                  B_.getInt64(static_cast<uint64_t>(extra.size())));
      }
    }
    for (size_t i = 0; i < total; ++i) {
      callArgs.push_back(B_.CreateLoad(abi_.valueTy(), slots[i]));
    }
    if (callArgs.size() != f->arg_size()) {
      irError(e->line, e->col, "函数 '" + name->id + "' 的参数个数与声明不一致");
    }
  }

  Value *val = B_.CreateCall(f, callArgs, name->id + ".r");
  B_.CreateStore(val, ret);
  return ret;
}

// input(int, int) —— 特殊形式。
//
// 按给定的类型列表逐个从 stdin 读一个值,打包成元组返回。
// 之所以做成"返回元组"而不是别的形式:`a, b = input(int, int)` 于是和
// `a, b = f()`、`a, b = b, a` 走完全同一条解包路径,不必为它单开特例。
Value *IRGen::genInput(const InputExpr *e) {
  const size_t n = e->kinds.size();

  // 只读一个值时直接返回它本身,不包成 1 元组。
  // 否则 `x = input(float)` 会让 x 拿到一个元组而不是那个浮点数。
  auto readerFor = [&](TypeName t) -> const char * {
    switch (t) {
      case TypeName::Int:   return "read_int";
      case TypeName::Float: return "read_float";
      case TypeName::Bool:  return "read_bool";
      case TypeName::Str:   return "read_str";
      default: return nullptr;
    }
  };
  if (n == 1) {
    const char *fn = readerFor(e->kinds[0]);
    if (!fn) irError(e->line, e->col, "input() 的类型参数必须是 int/float/bool/str");
    return abi_.callNullary(B_, fn);
  }

  // 把各次读取的结果堆进一块连续内存,再交给 tuple_new
  Value *arr = abi_.newSlotArray(B_, static_cast<int64_t>(n), "input.vals");

  for (size_t i = 0; i < n; ++i) {
    const char *fn = readerFor(e->kinds[i]);
    if (!fn) irError(e->line, e->col, "input() 的类型参数必须是 int/float/bool/str");
    storeElement(arr, i, abi_.callNullary(B_, fn));
  }

  return abi_.callPtrI64(B_, "tuple_new", arr, B_.getInt64(static_cast<uint64_t>(n)));
}

// print(a, b, ...) —— 内建。把各参数求值后放进一个 xmex::Value 数组,
// 把数组指针交给运行时。数组是内存布局,不受"聚合体不按值传"的限制。
Value *IRGen::genPrint(const Call *e) {
  const size_t n = e->args.size();
  // 数组元素个数为 0 时 evalToArray 给空指针,print_ 也不会去读它。
  // 这里曾经有个坑:用 {0, i} 的双索引 GEP 取数组元素是错的(第二个索引会被
  // 当成"结构体的第几个字段"),现在统一由 storeElement 用单索引处理。
  Value *arr = evalToArray(e->args, "print.args");

  abi_.callPrint(B_, arr, static_cast<int64_t>(n));

  // print 的返回值是 None,这样 `x = print(...)` 也能有个确定的结果
  Value *res = abi_.newSlot(B_, "print.ret");
  abi_.storeNone(B_, res);
  return res;
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

void IRGen::genBlock(const std::vector<StmtPtr> &body) {
  for (const auto &s : body) {
    // 前面的语句已经终止了当前块(return/break/continue),后面的代码不可达。
    // 仍然要生成 —— 否则变量声明会缺失,但要让它们落在新的死块里。
    if (B_.GetInsertBlock()->getTerminator()) {
      BasicBlock *dead = BasicBlock::Create(Ctx_, "dead", curFn_);
      B_.SetInsertPoint(dead);
    }
    genStmt(s.get());
  }
}

void IRGen::genStmt(const Stmt *s) {
  if (auto *x = dynamic_cast<const Assign *>(s))    return genAssign(x);
  if (auto *x = dynamic_cast<const AugAssign *>(s)) return genAugAssign(x);
  if (auto *x = dynamic_cast<const If *>(s))        return genIf(x);
  if (auto *x = dynamic_cast<const While *>(s))     return genWhile(x);
  if (auto *x = dynamic_cast<const For *>(s))       return genFor(x);
  if (auto *x = dynamic_cast<const Return *>(s))    return genReturn(x);

  if (dynamic_cast<const Break *>(s)) {
    if (loops_.empty()) irError(s->line, s->col, "break 只能出现在循环里");
    B_.CreateBr(loops_.back().breakBB);
    return;
  }
  if (dynamic_cast<const Continue *>(s)) {
    if (loops_.empty()) irError(s->line, s->col, "continue 只能出现在循环里");
    B_.CreateBr(loops_.back().contBB);
    return;
  }

  if (auto *x = dynamic_cast<const ExprStmt *>(s)) {
    genExpr(x->expr.get());  // 求值即可,丢弃结果
    return;
  }
  if (auto *x = dynamic_cast<const FuncDef *>(s)) {
    // 顶层的函数定义在 generate() 的两趟流程里已经处理过;能走到这里的
    // 都是写在函数体里的嵌套定义,单独提升。
    genNestedFuncDef(x);
    return;
  }
  if (dynamic_cast<const ClassDef *>(s)) {
    // 类定义在 generate() 的趟次里已经处理过了
    return;
  }

  irError(s->line, s->col, "无法生成代码:不认识的语句节点");
}

void IRGen::genAssign(const Assign *s) {
  const size_t n = s->targets.size();

  // 单目标:`a = expr`
  if (n == 1) {
    // 下标赋值 `a[i] = v`。求值顺序与 Python 一致:先算对象和下标,再算右侧 ——
    // 反过来的话 `xs[0] = xs.pop()` 之类的写法会读到错的元素。
    if (auto *sub = dynamic_cast<const Subscript *>(s->targets[0].get())) {
      Value *obj = genExpr(sub->obj.get());
      Value *idx = genExpr(sub->index.get());
      Value *val = genExpr(s->value.get());
      abi_.callVoidN(B_, "setindex", {abi_.ptrTy(), abi_.ptrTy(), abi_.ptrTy()},
                     {obj, idx, val});
      return;
    }

    // 属性赋值 `obj.attr = v`:先算对象与右侧,再写属性。
    if (auto *at = dynamic_cast<const Attribute *>(s->targets[0].get())) {
      Value *val = genExpr(s->value.get());
      genAttrSet(at, val);
      return;
    }
    Value *v = genExpr(s->value.get());
    auto *name = dynamic_cast<const Name *>(s->targets[0].get());
    if (!name) {
      irError(s->line, s->col, "赋值目标只能是变量或下标(如 a[i] = ...)");
    }
    AllocaInst *slot = lookupVar(name->id);
    if (!slot) slot = declareVar(name->id);
    abi_.copySlot(B_, slot, v);
    traceVar(name->id, slot);
    // 右侧是匿名函数:记下『该变量 -> 内部函数』,调用点据此解析成直接调用。
    if (dynamic_cast<const Lambda *>(s->value.get()) && !lastLambdaSym_.empty()) {
      lambdaVars_[curFn_->getName().str() + "::" + name->id] = lastLambdaSym_;
      lastLambdaSym_.clear();
    }
    // 右侧是类名实例化:记下『该变量 -> 类』,后续 obj.m(...) 才能解析成方法调用。
    if (auto *c = dynamic_cast<const Call *>(s->value.get())) {
      if (auto *cn = dynamic_cast<const Name *>(c->callee.get())) {
        if (classMap_.count(cn->id)) {
          varClasses_[curFn_->getName().str() + "::" + name->id] = cn->id;
        }
      }
    }
    return;
  }

  // --- 右侧是元组字面量:`a, b = b, a` ---
  //
  // 必须先全部求值并复制进独立临时槽位,再写目标。原因是 genExpr 遇到
  // 变量名时返回的是**变量自己的槽位指针**,不是拷贝 —— 直接拿这些指针
  // 去写目标的话,先写 a 就会把 vals[1] 的来源改掉,交换退化成两次自赋值。
  if (auto *tup = dynamic_cast<const TupleLit *>(s->value.get())) {
    if (tup->items.size() != n) {
      irError(s->line, s->col,
              "赋值目标有 " + std::to_string(n) + " 个,右侧元组有 " +
                  std::to_string(tup->items.size()) + " 个");
    }
    auto vals = evalAll(tup->items);
    std::vector<Value *> temps;
    temps.reserve(vals.size());
    for (Value *v : vals) {
      Value *t = abi_.newSlot(B_, "rhs.tmp");
      abi_.copySlot(B_, t, v);
      temps.push_back(t);
    }
    assignToTargets(s, temps);
    return;
  }

  // --- 其余情况:右侧求值成一个元组,运行时解包 ---
  //
  // 覆盖 `a, b = f()`(函数多返回值)和 `a, b = input(int, int)`。
  // 元素个数不匹配由 unpack 在运行时给出明确报错。
  Value *tup = genExpr(s->value.get());
  Value *out = abi_.newSlotArray(B_, static_cast<int64_t>(n), "unpack.vals");
  abi_.callUnpack(B_, tup, out, static_cast<int64_t>(n));

  std::vector<Value *> slots;
  slots.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    // out 是 %xmex::Value 数组,用单索引 GEP 取第 i 个元素
    slots.push_back(
        B_.CreateGEP(abi_.valueTy(), out, B_.getInt64(static_cast<uint64_t>(i))));
  }
  assignToTargets(s, slots);
}

void IRGen::assignToTargets(const Assign *s, const std::vector<Value *> &vals) {
  // 多目标赋值的目标只能是变量:`xs[0], xs[1] = 1, 2` 这种没实现。
  // 单目标的下标赋值在 genAssign 里单独处理。
  for (size_t i = 0; i < s->targets.size(); ++i) {
    auto *name = dynamic_cast<const Name *>(s->targets[i].get());
    if (!name) {
      irError(s->line, s->col, "多目标赋值的目标只能是变量(下标赋值请分开写)");
    }
    AllocaInst *slot = lookupVar(name->id);
    if (!slot) slot = declareVar(name->id);
    abi_.copySlot(B_, slot, vals[i]);
  }
}

std::vector<Value *> IRGen::evalAll(const std::vector<ExprPtr> &items) {
  std::vector<Value *> out;
  out.reserve(items.size());
  for (const auto &it : items) out.push_back(genExpr(it.get()));
  return out;
}

void IRGen::genAugAssign(const AugAssign *s) {
  auto *name = dynamic_cast<const Name *>(s->target.get());
  if (!name) {
    irError(s->line, s->col, "复合赋值目前只支持变量(下标形式如 a[i] += 1 尚未支持)");
  }

  AllocaInst *slot = lookupVar(name->id);
  if (!slot) irError(s->line, s->col, "使用了未定义的变量 '" + name->id + "'");

  Value *rhs = genExpr(s->value.get());

  // `+=` 走 iadd 而不是 add:列表的 `xs += [x]` 必须**就地** extend
  // (别名看得见),与 `xs = xs + [x]` 的重新绑定语义不同 —— 见 runtime/arith.cpp。
  // 字符串与数值不可变,iadd 内部退回 add,写回本地槽位即可。
  if (s->op == "+") {
    Value *result = abi_.callN(B_, "iadd", {abi_.ptrTy(), abi_.ptrTy()}, {slot, rhs});
    abi_.copySlot(B_, slot, result);
    return;
  }

  const char *fn = runtimeForBinOp(s->op);
  if (!fn) irError(s->line, s->col, "复合赋值不支持运算符 '" + s->op + "'");

  Value *result = abi_.callBinary(B_, fn, slot, rhs);
  abi_.copySlot(B_, slot, result);  // 结果写回原变量
}

void IRGen::genIf(const If *s) {
  Function *fn = curFn_;
  BasicBlock *endBB = BasicBlock::Create(Ctx_, "if.end", fn);

  struct Branch {
    const Expr *cond;
    const std::vector<StmtPtr> *body;
  };
  std::vector<Branch> branches;
  branches.push_back({s->cond.get(), &s->body});
  for (const auto &e : s->elifs) branches.push_back({e.first.get(), &e.second});

  for (size_t i = 0; i < branches.size(); ++i) {
    Value *c = abi_.truthyAsI1(B_, genExpr(branches[i].cond));
    BasicBlock *thenBB = BasicBlock::Create(Ctx_, "if.then", fn);

    // 后面还有 elif 或 else 才需要单独的"假"分支;否则假分支直接去 end
    const bool hasMore = (i + 1 < branches.size()) || !s->orelse.empty();
    BasicBlock *falseBB = hasMore ? BasicBlock::Create(Ctx_, "if.next", fn) : endBB;

    B_.CreateCondBr(c, thenBB, falseBB);

    B_.SetInsertPoint(thenBB);
    genBlock(*branches[i].body);
    if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(endBB);

    if (hasMore) B_.SetInsertPoint(falseBB);
  }

  if (!s->orelse.empty()) {
    genBlock(s->orelse);
    if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(endBB);
  }

  B_.SetInsertPoint(endBB);
}

void IRGen::genWhile(const While *s) {
  Function *fn = curFn_;
  BasicBlock *condBB = BasicBlock::Create(Ctx_, "while.cond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(Ctx_, "while.body", fn);
  BasicBlock *exitBB = BasicBlock::Create(Ctx_, "while.exit", fn);

  B_.CreateBr(condBB);

  B_.SetInsertPoint(condBB);
  Value *c = abi_.truthyAsI1(B_, genExpr(s->cond.get()));
  B_.CreateCondBr(c, bodyBB, exitBB);

  B_.SetInsertPoint(bodyBB);
  // continue 在 while 里跳回条件判断(而不是循环末),这里正好是 condBB
  loops_.push_back({condBB, exitBB});
  genBlock(s->body);
  loops_.pop_back();
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(condBB);

  B_.SetInsertPoint(exitBB);
}

void IRGen::genFor(const For *s) {
  // range(...) 直接降成计数循环;实例对象(迭代器协议)走 next() 路径;
  // 其余可迭代对象(列表/字符串/字典)走通用计数路径。
  if (auto *r = dynamic_cast<const RangeExpr *>(s->iterable.get())) {
    return genForRange(s, r);
  }
  // 迭代目标只求值一次存进槽位,两个分支共用
  Value *probe = genExpr(s->iterable.get());
  Value *probeSlot = abi_.newSlot(B_, "for.probe");
  abi_.copySlot(B_, probeSlot, probe);
  Value *tag = abi_.loadTag(B_, probeSlot);
  Value *isInst = B_.CreateICmpEQ(tag, ConstantInt::get(abi_.i32Ty(), xmex::kInstance), "is.inst");
  Function *fn = curFn_;
  BasicBlock *iterBB = BasicBlock::Create(Ctx_, "for.iterproto", fn);
  BasicBlock *countBB = BasicBlock::Create(Ctx_, "for.count", fn);
  BasicBlock *mergeBB = BasicBlock::Create(Ctx_, "for.done", fn);
  B_.CreateCondBr(isInst, iterBB, countBB);
  B_.SetInsertPoint(iterBB);
  genForIteratorProtocol(s, probeSlot, mergeBB);
  B_.SetInsertPoint(countBB);
  genForIterableFromSlot(s, probeSlot, mergeBB);
  B_.SetInsertPoint(mergeBB);
}

// for x in <list|str|dict> —— 降成"下标从 0 数到 len"的计数循环。
//
// 不构造任何迭代器对象:运行时只需要 len 与 iter_at 两个原语。
// 两个细节是照着 Python 的语义来的:
//
//   * 可迭代对象在**循环外**只求值一次,绑进一个临时槽位。循环体里给 xs
//     重新赋值不该改变正在遍历的东西(Python 的迭代器在进入循环时就绑定了)。
//   * 长度在**条件里每轮重读**。于是循环体里 append 进去的元素也会被遍历到,
//     这也正是 Python 的行为。
void IRGen::genForIterableFromSlot(const For *s, Value *seq, BasicBlock *mergeBB) {
  Function *fn = curFn_;

  BasicBlock *preBB = B_.GetInsertBlock();

  BasicBlock *condBB = BasicBlock::Create(Ctx_, "for.cond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(Ctx_, "for.body", fn);
  BasicBlock *latchBB = BasicBlock::Create(Ctx_, "for.latch", fn);
  BasicBlock *exitBB = BasicBlock::Create(Ctx_, "for.exit", fn);
  B_.CreateBr(condBB);

  B_.SetInsertPoint(condBB);
  PHINode *i = B_.CreatePHI(abi_.i64Ty(), 2, "i");
  i->addIncoming(ConstantInt::get(abi_.i64Ty(), 0), preBB);
  Value *len = abi_.callUnaryI64(B_, "len", seq);
  B_.CreateCondBr(B_.CreateICmpSLT(i, len, "for.keep"), bodyBB, exitBB);

  B_.SetInsertPoint(bodyBB);
  AllocaInst *ivar = lookupVar(s->var);
  if (!ivar) ivar = declareVar(s->var);
  // 每轮重新绑定循环变量。iter_at 对字典取的是**键**,不是 d[i] 那种按键查找
  // (见 runtime/index.cpp 的说明)。
  abi_.copySlot(B_, ivar, abi_.callPtrI64(B_, "iter_at", seq, i));
  loops_.push_back({latchBB, exitBB});
  genBlock(s->body);
  loops_.pop_back();
  // continue 跳到 latch,所以自增不会被跳过
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(latchBB);

  B_.SetInsertPoint(latchBB);
  Value *next = B_.CreateAdd(i, ConstantInt::get(abi_.i64Ty(), 1), "next");
  i->addIncoming(next, latchBB);
  B_.CreateBr(condBB);

  B_.SetInsertPoint(exitBB);
  B_.CreateBr(mergeBB);
}

// 迭代器协议:for x in <实例> —— 每轮调用 next(),返回 None 时终止循环。
// 与 Python 的迭代协议同构,只是方法名固定为 next(语言的属性调用走统一分派)。
void IRGen::genForIteratorProtocol(const For *s, Value *itObj, BasicBlock *mergeBB) {
  Function *fn = curFn_;
  BasicBlock *condBB = BasicBlock::Create(Ctx_, "ip.cond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(Ctx_, "ip.body", fn);
  BasicBlock *exitBB = BasicBlock::Create(Ctx_, "ip.exit", fn);
  B_.CreateBr(condBB);
  B_.SetInsertPoint(condBB);
  // item = it.next() —— 空参数的方法调用
  GlobalVariable *mname = B_.CreateGlobalString("next");
  Value *nullArgs = ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy()));
  Value *item = abi_.callN(
      B_, "call_method",
      {abi_.ptrTy(), abi_.ptrTy(), abi_.i64Ty(), abi_.ptrTy(), abi_.i64Ty()},
      {itObj, mname, B_.getInt64(4), nullArgs, B_.getInt64(0)});
  // item 是槽位指针,取 tag 判 None
  Value *tag = abi_.loadTag(B_, item);
  Value *isNone = B_.CreateICmpEQ(tag, ConstantInt::get(abi_.i32Ty(), xmex::kNull), "is.none");
  B_.CreateCondBr(isNone, exitBB, bodyBB);
  B_.SetInsertPoint(bodyBB);
  AllocaInst *ivar = lookupVar(s->var);
  if (!ivar) ivar = declareVar(s->var);
  abi_.copySlot(B_, ivar, item);
  // 迭代器循环没有 latch:next 调用本身就在 cond 里,continue 直接回 cond
  loops_.push_back({condBB, exitBB});
  genBlock(s->body);
  loops_.pop_back();
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(condBB);
  B_.SetInsertPoint(exitBB);
  B_.CreateBr(mergeBB);
}
// for i in range(a, b, step) 降成带 PHI 的计数循环,不构造任何迭代器对象。
//
//   stepOk: 检查 step != 0
//   cond:   %i = phi [start, stepOk], [next, latch]
//           按 step 的正负选择 i < stop 或 i > stop
//   body:   每轮把 %i 装箱写进循环变量的槽位
//   latch:  %next = i + step, 跳回 cond
void IRGen::genForRange(const For *s, const RangeExpr *r) {
  Function *fn = curFn_;

  // 边界在循环外只求值一次 —— 与 Python 一致(循环途中改 a 不影响次数)
  Value *startSlot = r->start ? genExpr(r->start.get()) : nullptr;
  Value *stopSlot = genExpr(r->stop.get());
  Value *stepSlot = r->step ? genExpr(r->step.get()) : nullptr;

  Value *startI = startSlot ? abi_.callUnaryI64(B_, "to_int", startSlot)
                            : ConstantInt::get(abi_.i64Ty(), 0);
  Value *stopI = abi_.callUnaryI64(B_, "to_int", stopSlot);
  Value *stepI = stepSlot ? abi_.callUnaryI64(B_, "to_int", stepSlot)
                          : ConstantInt::get(abi_.i64Ty(), 1);

  // 步长为 0 会死循环,先拦下来
  BasicBlock *stepOkBB = BasicBlock::Create(Ctx_, "range.stepok", fn);
  BasicBlock *badStepBB = BasicBlock::Create(Ctx_, "range.badstep", fn);
  B_.CreateCondBr(B_.CreateICmpEQ(stepI, ConstantInt::get(abi_.i64Ty(), 0)),
                  badStepBB, stepOkBB);

  B_.SetInsertPoint(badStepBB);
  abi_.emitRuntimeError(B_, "range() 的步长不能为 0");

  B_.SetInsertPoint(stepOkBB);
  BasicBlock *condBB = BasicBlock::Create(Ctx_, "range.cond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(Ctx_, "range.body", fn);
  BasicBlock *latchBB = BasicBlock::Create(Ctx_, "range.latch", fn);
  BasicBlock *exitBB = BasicBlock::Create(Ctx_, "range.exit", fn);
  B_.CreateBr(condBB);

  B_.SetInsertPoint(condBB);
  PHINode *i = B_.CreatePHI(abi_.i64Ty(), 2, "i");
  i->addIncoming(startI, stepOkBB);

  Value *stepPositive = B_.CreateICmpSGT(stepI, ConstantInt::get(abi_.i64Ty(), 0), "steppos");
  Value *lessThan = B_.CreateICmpSLT(i, stopI, "lt");
  Value *greaterThan = B_.CreateICmpSGT(i, stopI, "gt");
  Value *keepGoing = B_.CreateSelect(stepPositive, lessThan, greaterThan, "keep");
  B_.CreateCondBr(keepGoing, bodyBB, exitBB);

  B_.SetInsertPoint(bodyBB);
  AllocaInst *ivar = lookupVar(s->var);
  if (!ivar) ivar = declareVar(s->var);
  abi_.storeI64AsInt(B_, ivar, i);  // 每轮把计数装箱写回循环变量
  loops_.push_back({latchBB, exitBB});
  genBlock(s->body);
  loops_.pop_back();
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(latchBB);

  B_.SetInsertPoint(latchBB);
  Value *next = B_.CreateAdd(i, stepI, "next");
  i->addIncoming(next, latchBB);
  B_.CreateBr(condBB);

  B_.SetInsertPoint(exitBB);
}

// --- 插桩辅助 ---
// 这些调用都走 trace_*;通信模块未启用时它们是空操作,成本极低。
void IRGen::traceEnterFn(const std::string &name) {
  if (!traceOn_) return;
  // 帧标识槽位放在入口块最前面,与变量声明同一个约定。
  if (traceFrameSlot_ == nullptr) {
    BasicBlock &entry = curFn_->getEntryBlock();
    IRBuilder<> tmp(&entry, entry.begin());
    traceFrameSlot_ = tmp.CreateAlloca(abi_.i64Ty(), nullptr, "trace.frame");
  }
  GlobalVariable *gname = B_.CreateGlobalString(llvm::StringRef(name));
  // trace_begin(const char*) -> i64:直接声明 —— callUnaryI64 对非 len
  // 的函数会按 (xmex::Value)->i64 声明并加载 valueTy,参数类型对不上。
  Function *beginFn = M_.getFunction("trace_begin");
  if (beginFn == nullptr) {
    beginFn = Function::Create(
        FunctionType::get(abi_.i64Ty(), {abi_.ptrTy()}, false),
        Function::ExternalLinkage, "trace_begin", M_);
  }
  Value *id = B_.CreateCall(beginFn, {gname}, "trace.id");
  B_.CreateStore(id, traceFrameSlot_);
}
// 进入:附带形参名与实参值,面板显示为 combine(a=7, b=10)。
void IRGen::traceEnterFnArgs(const std::string &name,
                             const std::vector<std::string> &paramNames,
                             const std::vector<llvm::Value *> &paramSlots) {
  if (!traceOn_) return;
  if (paramNames.empty()) { traceEnterFn(name); return; }
  if (traceFrameSlot_ == nullptr) {
    BasicBlock &entry = curFn_->getEntryBlock();
    IRBuilder<> tmp(&entry, entry.begin());
    traceFrameSlot_ = tmp.CreateAlloca(abi_.i64Ty(), nullptr, "trace.frame");
  }
  Type *ptrTy = abi_.ptrTy();
  const size_t n = paramNames.size();

  // 形参名:只读常量数组
  std::vector<Constant *> namePtrs;
  for (size_t i = 0; i < n; ++i)
    namePtrs.push_back(B_.CreateGlobalString(llvm::StringRef(paramNames[i])));
  ArrayType *nameArrTy = ArrayType::get(ptrTy, n);
  Constant *nameArr = ConstantArray::get(nameArrTy, namePtrs);
  GlobalVariable *gNameArr = new GlobalVariable(
      M_, nameArrTy, true, GlobalValue::InternalLinkage, nameArr, "trace.argnames");

  // 实参值:栈上指针数组,存放各参数槽位地址(运行时按 xmex::Value* 取值)。
  // 注意:运行时接口的最后一参是 xmex::Value*const*,所以这里传的必须是
  // 指针数组,不能是把值铺开的数组 —— 否则运行时会把首个值当成地址解引用。
  AllocaInst *valsArr = B_.CreateAlloca(ArrayType::get(abi_.ptrTy(), n),
                                        nullptr, "trace.argptrs");
  for (size_t i = 0; i < n; ++i) {
    Value *elemPtr = B_.CreateGEP(abi_.ptrTy(), valsArr,
                                  B_.getInt64(static_cast<uint64_t>(i)));
    B_.CreateStore(paramSlots[i], elemPtr);
  }

  Function *beginFn = M_.getFunction("trace_begin_args");
  if (beginFn == nullptr) {
    beginFn = Function::Create(
        FunctionType::get(abi_.i64Ty(), {ptrTy, abi_.i32Ty(), ptrTy, ptrTy}, false),
        Function::ExternalLinkage, "trace_begin_args", M_);
  }
  GlobalVariable *gname = B_.CreateGlobalString(llvm::StringRef(name));
  Value *id = B_.CreateCall(
      beginFn,
      {gname, B_.getInt32(static_cast<int32_t>(n)), gNameArr, valsArr},
      "trace.id");
  B_.CreateStore(id, traceFrameSlot_);
}
void IRGen::traceExitFn(const std::string &name, bool ok) {
  if (!traceOn_ || traceFrameSlot_ == nullptr) return;
  // 帧标识槽位必须属于当前函数 —— lambda/嵌套函数生成期间会切换 curFn_,
  // 误用外层函数的槽位会把外层函数错误上报为退出。
  if (traceFrameSlot_->getFunction() != curFn_) return;
  GlobalVariable *gname = B_.CreateGlobalString(llvm::StringRef(name));
  Value *id = B_.CreateLoad(abi_.i64Ty(), traceFrameSlot_);
  abi_.callVoidN(B_, "trace_end",
                 {abi_.i64Ty(), abi_.ptrTy(), abi_.i32Ty()},
                 {id, gname, B_.getInt32(ok ? 1 : 0)});
}
void IRGen::traceVar(const std::string &name, Value *slot) {
  if (!traceOn_) return;
  // trace_var_value(const char*, const xmex::Value*):由运行时负责 repr 转换,
  // 编译器侧不用关心字符串怎么取、怎么保证 NUL 结尾 —— 全部收敛到这一个入口。
  GlobalVariable *gname = B_.CreateGlobalString(llvm::StringRef(name));
  abi_.callVoidN(B_, "trace_var_value",
                 {abi_.ptrTy(), abi_.ptrTy()}, {gname, slot});
}

void IRGen::genReturn(const Return *s) {
  if (s->value) {
    Value *v = genExpr(s->value.get());
    abi_.copySlot(B_, retSlot_, v);
  } else {
    abi_.storeNone(B_, retSlot_);
  }
  traceExitFn(curTraceName_, true);
  Value *retVal = B_.CreateLoad(abi_.valueTy(), retSlot_);
  B_.CreateRet(retVal);
}

// ---------------------------------------------------------------------------
// 函数
// ---------------------------------------------------------------------------

void IRGen::genFunction(const FuncDef *fd) {
  const std::string sym = symbolFor(fd->name) + "__impl";
  Function *fn = M_.getFunction(sym);
  // generate() 的第一趟已经建好声明

  curFn_ = fn;
  scopes_.emplace_back();

  BasicBlock *entry = BasicBlock::Create(Ctx_, "entry", fn);
  B_.SetInsertPoint(entry);

  // Linux: 返回值槽改为本地 alloca，不再从 sret 参数获取
  retSlot_ = abi_.newSlot(B_, fd->name + ".ret");

  // 插桩:记录带形参签名的函数名,重置帧槽位。
  std::string traceDisplayName = fd->name + "(";
  for (size_t i = 0; i < fd->params.size(); ++i) {
    if (i > 0) traceDisplayName += ", ";
    traceDisplayName += fd->params[i].name;
  }
  traceDisplayName += ")";
  curTraceName_ = traceDisplayName;
  traceFrameSlot_ = nullptr;

  // 参数按值传入，需要 store 到本地栈槽;同时收集形参名与槽位用于进入上报。
  std::vector<std::string> traceParamNames;
  std::vector<Value *> traceParamSlots;
  for (size_t i = 0; i < fd->params.size(); ++i) {
    AllocaInst *slot = declareVar(fd->params[i].name);
    B_.CreateStore(fn->getArg(static_cast<unsigned>(i)), slot);
    traceParamNames.push_back(fd->params[i].name);
    traceParamSlots.push_back(slot);
  }

  traceEnterFnArgs(traceDisplayName, traceParamNames, traceParamSlots);

  // Python 的函数走到末尾会隐式返回 None。入口处先写一次 None,
  // 这样没有 return 的路径也有确定的结果。
  abi_.storeNone(B_, retSlot_);

  genBlock(fd->body);
  if (!B_.GetInsertBlock()->getTerminator()) {
    traceExitFn(curTraceName_, true);
    Value *retVal = B_.CreateLoad(abi_.valueTy(), retSlot_);
    B_.CreateRet(retVal);
  }

  scopes_.pop_back();
  curFn_ = nullptr;
  retSlot_ = nullptr;
  traceFrameSlot_ = nullptr;
  curTraceName_.clear();
}

// 匿名函数:值类型没有函数表示,所以这里把它"提升"成一个模块内部函数,
// 并记下最近生成的符号。赋值语句据此把变量名绑定到该内部函数,调用点再
// 把它解析成直接调用。全程只动编译期信息,不碰 xmex::Value 的布局。
Value *IRGen::genLambdaExpr(const Lambda *lam) {
  const std::string sym = "xmex_" + sanitize(moduleName_) + "_lambda" +
                          std::to_string(lambdaCounter_++);
  // 闭包捕获:把当前可见的外层变量记下来,作为内部函数的**前置参数**随调用传入。
  // 这样匿名函数用到的外层数据就一起带进来了,不需要运行时环境对象,
  // 也就不必给值类型新增函数表示。
  // ⚠️ 采取"捕获当前可见的全部外层局部变量"这一偏保守的策略:不做自由变量分析,
  // 多传几个用不上的参数,换取"绝不漏捕获"。
  std::vector<std::string> caps;
  if (!scopes_.empty()) {
    for (const auto &kv : scopes_.back()) {
      bool isParam = false;
      for (const auto &p : lam->params) {
        if (p.name == kv.first) { isParam = true; break; }
      }
      if (!isParam) caps.push_back(kv.first);
    }
  }
  lambdaCaptures_[sym] = caps;
  std::vector<Type *> params;
  for (size_t i = 0; i < caps.size(); ++i) params.push_back(abi_.valueTy());
  for (size_t i = 0; i < lam->params.size(); ++i) params.push_back(abi_.valueTy());
  auto *ft = FunctionType::get(abi_.valueTy(), params, false);
  auto *fn = Function::Create(ft, Function::InternalLinkage, sym, M_);
  lambdaDefs_[sym] = lam;
  lastLambdaSym_ = sym;
  // 立刻生成函数体 —— 匿名函数出现在表达式位置,不参与顶层那两趟流程。
  genLambdaDef(lam, fn);
  // 求值结果本身没有运行时表示,给一个确定的值(None)。
  Value *s = abi_.newSlot(B_, "lambda");
  abi_.storeNone(B_, s);
  return s;
}

// 匿名函数体。与具名函数走同一套参数绑定;生成期间要保存并恢复当前函数、
// 返回槽与插入点,否则外层函数的后续语句会被写进匿名函数里。
void IRGen::genLambdaDef(const Lambda *lam, Function *fn) {
  Function *savedFn = curFn_;
  Value *savedRet = retSlot_;
  const auto savedIP = B_.saveIP();
  curFn_ = fn;
  scopes_.emplace_back();
  BasicBlock *entry = BasicBlock::Create(Ctx_, "entry", fn);
  B_.SetInsertPoint(entry);
  retSlot_ = abi_.newSlot(B_, "lambda.ret");
  // 先绑捕获项(前置参数),再绑形参;顺序必须与调用点一致。
  unsigned argBase = 0;
  auto capIt = lambdaCaptures_.find(fn->getName().str());
  if (capIt != lambdaCaptures_.end()) {
    for (size_t i = 0; i < capIt->second.size(); ++i) {
      AllocaInst *slot = declareVar(capIt->second[i]);
      B_.CreateStore(fn->getArg(static_cast<unsigned>(i)), slot);
    }
    argBase = static_cast<unsigned>(capIt->second.size());
  }
  for (size_t i = 0; i < lam->params.size(); ++i) {
    AllocaInst *slot = declareVar(lam->params[i].name);
    B_.CreateStore(fn->getArg(argBase + static_cast<unsigned>(i)), slot);
  }
  abi_.storeNone(B_, retSlot_);
  genBlock(lam->body);
  if (!B_.GetInsertBlock()->getTerminator()) {
    Value *retVal = B_.CreateLoad(abi_.valueTy(), retSlot_);
    B_.CreateRet(retVal);
  }
  scopes_.pop_back();
  curFn_ = savedFn;
  retSlot_ = savedRet;
  B_.restoreIP(savedIP);
}

// 嵌套函数:写在函数体里的 def。处理方式与匿名函数完全一致 ——
// 提升为模块内部函数,并把外层变量作为前置参数捕获进来。
void IRGen::genNestedFuncDef(const FuncDef *fd) {
  const std::string sym = "xmex_" + sanitize(moduleName_) + "_lambda" +
                          std::to_string(lambdaCounter_++);
  std::vector<std::string> caps;
  if (!scopes_.empty()) {
    for (const auto &kv : scopes_.back()) {
      bool isParam = false;
      for (const auto &p : fd->params) {
        if (p.name == kv.first) { isParam = true; break; }
      }
      if (!isParam) caps.push_back(kv.first);
    }
  }
  lambdaCaptures_[sym] = caps;
  nestedParams_[sym] = &fd->params;
  std::vector<Type *> params;
  for (size_t i = 0; i < caps.size(); ++i) params.push_back(abi_.valueTy());
  for (size_t i = 0; i < fd->params.size(); ++i) params.push_back(abi_.valueTy());
  auto *ft = FunctionType::get(abi_.valueTy(), params, false);
  auto *fn = Function::Create(ft, Function::InternalLinkage, sym, M_);
  // 把名字绑到内部函数:函数体内(含递归)调用 fd->name(...) 时据此解析。
  lambdaVars_[curFn_->getName().str() + "::" + fd->name] = sym;
  // 生成函数体。与匿名函数一样,要保存并恢复当前函数、返回槽与插入点。
  Function *savedFn = curFn_;
  Value *savedRet = retSlot_;
  const auto savedIP = B_.saveIP();
  curFn_ = fn;
  scopes_.emplace_back();
  BasicBlock *entry = BasicBlock::Create(Ctx_, "entry", fn);
  B_.SetInsertPoint(entry);
  retSlot_ = abi_.newSlot(B_, "nested.ret");
  for (size_t i = 0; i < caps.size(); ++i) {
    AllocaInst *slot = declareVar(caps[i]);
    B_.CreateStore(fn->getArg(static_cast<unsigned>(i)), slot);
  }
  const unsigned argBase = static_cast<unsigned>(caps.size());
  for (size_t i = 0; i < fd->params.size(); ++i) {
    AllocaInst *slot = declareVar(fd->params[i].name);
    B_.CreateStore(fn->getArg(argBase + static_cast<unsigned>(i)), slot);
  }
  abi_.storeNone(B_, retSlot_);
  genBlock(fd->body);
  if (!B_.GetInsertBlock()->getTerminator()) {
    Value *retVal = B_.CreateLoad(abi_.valueTy(), retSlot_);
    B_.CreateRet(retVal);
  }
  scopes_.pop_back();
  curFn_ = savedFn;
  retSlot_ = savedRet;
  B_.restoreIP(savedIP);
}

// 类定义:类信息已在 generate() 的第一趟登记,这里把每个成员方法生成成
// 一个带实例参数的普通函数 —— 方法的第一个形参就是 self(由调用方传入实例)。
// 这样方法体与普通函数完全同构,参数绑定、返回、循环控制全部复用既有逻辑。
void IRGen::genClassDef(const ClassDef *cd) {
  // 先声明所有方法,这样方法之间互相调用(含递归)不受定义顺序限制。
  std::map<std::string, Function *> methodFns;
  for (const auto &m : cd->methods) {
    const std::string sym = "xmex_" + sanitize(moduleName_) + "_class_" +
                            sanitize(cd->name) + "_" + sanitize(m->name);
    // 方法的声明参数**就是它的形参列表** —— 代表实例的那个形参(惯例写 self)
    // 本就在列表里,不能再额外加一个,否则声明与调用会差一个参数。
    std::vector<Type *> params;
    for (size_t i = 0; i < m->params.size(); ++i) params.push_back(abi_.valueTy());
    auto *ft = FunctionType::get(abi_.valueTy(), params, false);
    methodFns[m->name] = Function::Create(ft, Function::ExternalLinkage, sym, M_);
  }
  // 扫一遍方法体,登记 `self.X = SomeClass(...)` 这类"属性持有的实例类型"。
  // 有了它,self.X.m(...) 这种组合结构上的方法调用才能解析出来。
  for (const auto &m : cd->methods) {
    for (const auto &st : m->body) {
      auto *as = dynamic_cast<const Assign *>(st.get());
      if (!as || as->targets.size() != 1) continue;
      auto *at = dynamic_cast<const Attribute *>(as->targets[0].get());
      if (!at) continue;
      if (!dynamic_cast<const Name *>(at->obj.get())) continue;
      auto *call = dynamic_cast<const Call *>(as->value.get());
      if (!call) continue;
      auto *cn = dynamic_cast<const Name *>(call->callee.get());
      if (!cn) continue;
      if (classMap_.count(cn->id)) {
        classAttrClasses_[cd->name + "." + at->name] = cn->id;
      }
    }
  }
  // 再逐个生成方法体。要保存并恢复当前函数、返回槽与插入点。
  for (const auto &m : cd->methods) {
    Function *fn = methodFns[m->name];
    Function *savedFn = curFn_;
    Value *savedRet = retSlot_;
    const auto savedIP = B_.saveIP();
    curFn_ = fn;
    scopes_.emplace_back();
    BasicBlock *entry = BasicBlock::Create(Ctx_, "entry", fn);
    B_.SetInsertPoint(entry);
    retSlot_ = abi_.newSlot(B_, m->name + ".ret");
    // 按约定,第一个形参写 self;它与第一个实参(实例)天然对应。
    for (size_t i = 0; i < m->params.size(); ++i) {
      AllocaInst *slot = declareVar(m->params[i].name);
      B_.CreateStore(fn->getArg(static_cast<unsigned>(i)), slot);
    }
    // 方法体里的第一个形参(惯例写 self)指向本类实例;据此把 obj.m(...)
    // 解析成对类方法的直接调用。按形参名记录,不硬编码 self 这个写法。
    if (!m->params.empty()) {
      varClasses_[fn->getName().str() + "::" + m->params[0].name] = cd->name;
    }
    abi_.storeNone(B_, retSlot_);
    genBlock(m->body);
    if (!B_.GetInsertBlock()->getTerminator()) {
      Value *retVal = B_.CreateLoad(abi_.valueTy(), retSlot_);
      B_.CreateRet(retVal);
    }
    scopes_.pop_back();
    curFn_ = savedFn;
    retSlot_ = savedRet;
    B_.restoreIP(savedIP);
  }
}

// 实例化:类名(...)。先建一张空属性字典,交给运行时造出实例;
// 若该类定义了 __init__,再把实例作为 self、连同实参一起调用它。
Value *IRGen::genInstanceNew(const std::string &className, const Call *e) {
  auto idIt = classIds_.find(className);
  if (idIt == classIds_.end()) {
    irError(e->line, e->col, "未定义的类 '" + className + "'");
  }
  Value *np = ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy()));
  Value *attrs = abi_.callN(B_, "dict_new",
                            {abi_.ptrTy(), abi_.ptrTy(), abi_.i64Ty()},
                            {np, np, B_.getInt64(0)});
  Value *inst = abi_.callN(B_, "instance_new", {abi_.i64Ty(), abi_.ptrTy()},
                           {B_.getInt64(idIt->second), attrs});
  auto cm = classMap_.find(className);
  if (cm != classMap_.end()) {
    for (const auto &m : cm->second->methods) {
      if (m->name != "__init__") continue;
      const std::string sym = "xmex_" + sanitize(moduleName_) + "_class_" +
                              sanitize(className) + "___init__";
      Function *f = M_.getFunction(sym);
      if (f == nullptr) break;
      std::vector<Value *> callArgs;
      callArgs.push_back(B_.CreateLoad(abi_.valueTy(), inst));
      for (const auto &a : e->args) {
        Value *s = genExpr(a.get());
        callArgs.push_back(B_.CreateLoad(abi_.valueTy(), s));
      }
      B_.CreateCall(f, callArgs);
      break;
    }
  }
  return inst;
}

// 属性取值:obj.attr
Value *IRGen::genAttrGet(const Attribute *a) {
  Value *obj = genExpr(a->obj.get());
  GlobalVariable *name = B_.CreateGlobalString(llvm::StringRef(a->name));
  return abi_.callN(B_, "instance_get_attr",
                    {abi_.ptrTy(), abi_.ptrTy(), abi_.i64Ty()},
                    {obj, name, B_.getInt64(static_cast<int64_t>(a->name.size()))});
}

// 属性赋值:obj.attr = v
void IRGen::genAttrSet(const Attribute *a, Value *val) {
  Value *obj = genExpr(a->obj.get());
  GlobalVariable *name = B_.CreateGlobalString(llvm::StringRef(a->name));
  abi_.callVoidN(B_, "instance_set_attr",
                 {abi_.ptrTy(), abi_.ptrTy(), abi_.i64Ty(), abi_.ptrTy()},
                 {obj, name, B_.getInt64(static_cast<int64_t>(a->name.size())), val});
}

void IRGen::genTopLevel(const Module &ast) {
  const std::string sym = symbolFor("main");
  auto *ft = FunctionType::get(abi_.voidTy(), {}, false);
  auto *fn = Function::Create(ft, Function::ExternalLinkage, sym, M_);

  curFn_ = fn;
  retSlot_ = nullptr;
  scopes_.emplace_back();

  BasicBlock *entry = BasicBlock::Create(Ctx_, "entry", fn);
  B_.SetInsertPoint(entry);

  // 插桩:先把事件落盘路径挂上,再启动 SSE 服务(18900)。
  if (traceOn_) {
    Function *dumpFn = M_.getFunction("trace_set_dump");
    if (dumpFn == nullptr) {
      dumpFn = Function::Create(
          FunctionType::get(abi_.voidTy(), {abi_.ptrTy()}, false),
          Function::ExternalLinkage, "trace_set_dump", M_);
    }
    GlobalVariable *dumpPath = B_.CreateGlobalString(
        llvm::StringRef("/tmp/xmex_trace/events.sse"));
    B_.CreateCall(dumpFn, {dumpPath});
    Function *startFn = M_.getFunction("trace_start");
    if (startFn == nullptr) {
      startFn = Function::Create(
          FunctionType::get(abi_.voidTy(), {abi_.i32Ty()}, false),
          Function::ExternalLinkage, "trace_start", M_);
    }
    B_.CreateCall(startFn, {B_.getInt32(18900)});
  }

  for (const auto &s : ast.body) {
    if (dynamic_cast<const FuncDef *>(s.get())) continue;  // 已单独生成
    if (dynamic_cast<const ClassDef *>(s.get())) continue; // 类已单独生成
    if (B_.GetInsertBlock()->getTerminator()) {
      BasicBlock *dead = BasicBlock::Create(Ctx_, "dead", fn);
      B_.SetInsertPoint(dead);
    }
    genStmt(s.get());
  }
  // 插桩:停掉 SSE 服务并关闭落盘文件。
  if (traceOn_) {
    abi_.callVoidN(B_, "trace_stop", {}, {});
  }
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateRetVoid();

  scopes_.pop_back();
  curFn_ = nullptr;
}

void IRGen::generate(const Module &ast) {
  // 第零趟:先登记所有类。类要排在函数之前 —— 任何函数都可能实例化某个类。
  for (const auto &s : ast.body) {
    auto *cd = dynamic_cast<const ClassDef *>(s.get());
    if (!cd) continue;
    if (classMap_.count(cd->name)) {
      irError(cd->line, cd->col, "类 '" + cd->name + "' 重复定义");
    }
    classMap_[cd->name] = cd;
    classIds_[cd->name] = classCounter_++;   // 每个类一个整数标识
  }
  // 第一趟:登记所有用户函数的签名。
  // 必须先做这一趟,否则调用点无法知道参数个数,而且函数之间的相互调用
  // (包括递归)在定义顺序上会受限制。
  for (const auto &s : ast.body) {
    auto *fd = dynamic_cast<const FuncDef *>(s.get());
    if (!fd) continue;

    const std::string sym = symbolFor(fd->name) + "__impl";
    if (!declaredSyms_.insert(sym).second) {
      irError(fd->line, fd->col, "函数 '" + fd->name + "' 重复定义");
    }
    funcs_.push_back(fd);
    funcMap_[fd->name] = fd;   // 调用点用它重排关键字参数、补默认值、打包 *args

    std::vector<Type *> params;
    for (size_t i = 0; i < fd->params.size(); ++i) params.push_back(abi_.valueTy());
    // 内部实现:模块内按值收发 xmex::Value(符号带 __impl 后缀)
    auto *ft = FunctionType::get(abi_.valueTy(), params, false);
    Function::Create(ft, Function::ExternalLinkage, sym, M_);
    // 导出包装:C++ 宿主约定 —— void(ptr sret, ptr args...)
    // 第一个参数是返回值缓冲区,其余参数按指针传入(见 docs/llvm-notes.md 第2节)
    const std::string exportSym = symbolFor(fd->name);
    std::vector<Type *> wrapParams;
    wrapParams.push_back(abi_.ptrTy());  // sret 返回槽
    for (size_t i = 0; i < fd->params.size(); ++i) wrapParams.push_back(abi_.ptrTy());
    auto *wrapFt = FunctionType::get(abi_.voidTy(), wrapParams, false);
    auto *wrapFn = Function::Create(wrapFt, Function::ExternalLinkage, exportSym, M_);
    wrapFn->addParamAttr(0, abi_.sretAttr());
    // 生成包装函数体:从指针载入参数 -> 调用内部实现 -> 返回值写入 sret 槽
    {
      BasicBlock *entry = BasicBlock::Create(Ctx_, "entry", wrapFn);
      IRBuilder<> wb(entry);
      std::vector<Value *> callArgs;
      for (size_t i = 0; i < fd->params.size(); ++i) {
        callArgs.push_back(wb.CreateLoad(abi_.valueTy(), wrapFn->getArg(static_cast<unsigned>(i + 1)), "arg"));
      }
      Function *implFn = M_.getFunction(sym);
      Value *rv = wb.CreateCall(implFn, callArgs, "rv");
      wb.CreateStore(rv, wrapFn->getArg(0));
      wb.CreateRetVoid();
    }
  }

  // 第二趟:填函数体
  for (const auto &s : ast.body) {
    if (auto *fd = dynamic_cast<const FuncDef *>(s.get())) genFunction(fd);
  }

  // 补上类的方法体(类已在上面的趟次里登记)
  for (const auto &s : ast.body) {
    if (auto *cd = dynamic_cast<const ClassDef *>(s.get())) genClassDef(cd);
  }
  // 顶层语句
  genTopLevel(ast);
}

}  // namespace xmex
