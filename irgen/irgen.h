// AST -> LLVM IR。
//
// 值的表示:每个 lite::Value 都住在栈上的一个 alloca 槽位里,genExpr 返回的是
// "指向槽位的指针"而非聚合体本身。这样传给运行时函数时天然就是指针,
// 不需要在每个调用点临时 spill —— 也和 docs/llvm-notes.md 里那条
// "聚合体不跨 ABI 边界按值传"的硬性要求天然吻合。
//
// 生成的符号约定:
//   每个 def  ->  void @pylite_<name>(ptr sret(%lite::Value), ptr arg0, ptr arg1, ...)
//   顶层语句  ->  void @pylite_module_main()   (无参数,无返回值)
// 调用方(手写 C++)拿到的是 lite::Value*,头文件由 aot/header_emit 生成。
#pragma once

#include "abi.h"
#include "ast.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace pylite {

class IRGen {
 public:
  IRGen(llvm::Module &M, std::string moduleName);

  // 开启插桩:在函数入口/退出与赋值处自动插入事件上报,供调试面板使用。
  void setTrace(bool on) { traceOn_ = on; }

  // 生成整个模块
  void generate(const Module &ast);

  Abi &abi() { return abi_; }
  const llvm::Module &module() const { return M_; }

  // 已生成的用户函数,供头文件生成使用
  const std::vector<const FuncDef *> &userFunctions() const { return funcs_; }

  // 符号名:pylite_<模块名>_<函数名>,模块名做了清洗
  std::string symbolFor(const std::string &funcName) const;

 private:
  // --- 作用域 ---
  llvm::AllocaInst *lookupVar(const std::string &name) const;
  llvm::AllocaInst *declareVar(const std::string &name);

  // --- 表达式:一律返回指向 lite::Value 槽位的指针 ---
  llvm::Value *genExpr(const Expr *e);
  llvm::Value *genBinOp(const BinOp *e);
  llvm::Value *genAndOr(const BinOp *e, bool isAnd);   // 短路,带 PHI
  llvm::Value *genUnaryOp(const UnaryOp *e);
  llvm::Value *genCall(const Call *e);
  llvm::Value *genPrint(const Call *e);                // print(...) 内建
  llvm::Value *genLen(const Call *e);                  // len(...) 内建
  llvm::Value *genGcCollect(const Call *e);            // gc_collect() 内建
  llvm::Value *genSleepMs(const Call *e);            // sleep_ms(ms) 内建
  llvm::Value *genTimeMs(const Call *e);             // time_ms() 内建
  llvm::Value *genTimeS(const Call *e);              // time_s() 内建
  llvm::Value *genMethodCall(const Call *e, const Attribute *attr);  // obj.m(...)
  llvm::Value *genInput(const InputExpr *e);           // input(int, int) 特殊形式
  llvm::Value *genListLit(const ListLit *e);
  llvm::Value *genDictLit(const DictLit *e);
  llvm::Value *genTupleLit(const TupleLit *e);
  llvm::Value *genSubscript(const Subscript *e);
  llvm::Value *genSlice(const SliceExpr *e);

  // 把一组表达式依次求值,塞进一块连续的 %lite::Value 数组,返回数组指针。
  // genTupleLit / genListLit / genDictLit / genPrint / 方法调用共用。
  //
  // 用一个**数组**而不是逐个传参,是因为数组是内存布局,不受
  // "聚合体不按值跨 ABI 边界"那条限制(见 docs/llvm-notes.md 第 2 节)。
  // 元素个数为 0 时返回空指针,运行时不读它。
  llvm::Value *evalToArray(const std::vector<ExprPtr> &items, const llvm::Twine &name);
  void storeElement(llvm::Value *array, size_t i, llvm::Value *slot);

  // 一个装着 None 的槽位。切片省略边界时用它当"没给"的哨兵。
  llvm::Value *noneSlot();

  // --- 语句 ---
  void genStmt(const Stmt *s);
  void genAssign(const Assign *s);
  void genAugAssign(const AugAssign *s);
  void genIf(const If *s);
  void genWhile(const While *s);
  void genFor(const For *s);
  void genForRange(const For *s, const RangeExpr *r);
  void genForIterableFromSlot(const For *s, llvm::Value *seq,
                              llvm::BasicBlock *mergeBB);   // 遍历 list / str / dict
  void genForIteratorProtocol(const For *s, llvm::Value *itObj,
                              llvm::BasicBlock *mergeBB);   // 迭代器协议:实例 next()
  void genReturn(const Return *s);
  void genBlock(const std::vector<StmtPtr> &body);

  // --- 插桩辅助 ---
  void traceEnterFn(const std::string &name);      // 函数入口:begin 并把帧标识存进槽位
  // 函数入口:上报形参名与实参值,便于面板节点显示为 combine(a=7, b=10)。
  void traceEnterFnArgs(const std::string &name,
                        const std::vector<std::string> &paramNames,
                        const std::vector<llvm::Value *> &paramSlots);
  void traceExitFn(const std::string &name, bool ok); // 函数退出:end
  void traceVar(const std::string &name, llvm::Value *slot); // 赋值后上报变量值
  // 当前函数的帧标识槽位(一个 i64 alloca),退出时从中读出标识。
  llvm::AllocaInst *traceFrameSlot_ = nullptr;
  // 当前正在生成的函数名,返回处理时按它上报退出事件。
  std::string curTraceName_;
  bool traceOn_ = false;

  // --- 函数 ---
  void genFunction(const FuncDef *fd);
  // 匿名函数:生成一个模块内部函数并返回 None 槽位(函数值在运行时没有表示)。
  llvm::Value *genLambdaExpr(const Lambda *lam);
  // 调用点的参数绑定:位置参数 -> 关键字参数 -> 缺省值 -> *args。
  // params 为空表示拿到不到参数表,退回严格按位置匹配。具名与匿名函数共用。
  void bindCallArgs(const std::vector<Param> *params, const Call *e,
                    llvm::Function *f, const std::string &calleeName,
                    std::vector<llvm::Value *> &out);
  // 匿名函数体:按给定的函数(Function)生成,与具名函数同一套参数绑定规则。
  void genLambdaDef(const Lambda *lam, llvm::Function *fn);
  // 嵌套函数:在函数体里写的 def,同样提升为内部函数并捕获外层变量。
  void genNestedFuncDef(const FuncDef *fd);
  // 类定义:把每个成员方法生成成带实例参数的普通函数,并登记类信息。
  void genClassDef(const ClassDef *cd);
  // 实例化(类名(...)):建一个空属性字典,交给运行时造实例。
  llvm::Value *genInstanceNew(const std::string &className, const Call *e);
  // 实例属性读写:obj.attr 与 obj.attr = v。
  llvm::Value *genAttrGet(const Attribute *a);
  void genAttrSet(const Attribute *a, llvm::Value *val);
  // 最近一次生成的内部函数符号。genAssign 用它把变量名绑定到内部函数。
  std::string lastLambdaSym_;
  void genTopLevel(const Module &ast);

  // 先全部求值到临时槽位,再统一写目标。
  // `a, b = b, a` 必须这样,否则第二次求值读到的是已被覆盖的 a。
  std::vector<llvm::Value *> evalAll(const std::vector<ExprPtr> &items);

  // 把若干已知的值按顺序写进赋值目标(目前目标只能是变量)
  void assignToTargets(const Assign *s, const std::vector<llvm::Value *> &vals);

  std::string mangle(const std::string &module, const std::string &fn);

  struct LoopTarget {
    llvm::BasicBlock *contBB = nullptr;
    llvm::BasicBlock *breakBB = nullptr;
  };

  llvm::Module &M_;
  llvm::LLVMContext &Ctx_;
  Abi abi_;
  std::string moduleName_;

  llvm::IRBuilder<> B_;
  llvm::Function *curFn_ = nullptr;
  llvm::Value *retSlot_ = nullptr;  // 当前函数的 sret 槽位

  std::vector<std::map<std::string, llvm::AllocaInst *>> scopes_;
  std::vector<LoopTarget> loops_;
  std::vector<const FuncDef *> funcs_;
  // 函数名 -> 定义。调用点用它重排关键字参数、补默认值、打包可变参数。
  std::map<std::string, const FuncDef *> funcMap_;
  // --- 匿名函数 ---
  // 值类型(lite::Value)的布局是 ABI 契约,不能动,所以匿名函数走"提升为模块内部
  // 函数 + 记录变量到函数的对应"这条路:把 lambda 赋给一个变量后,按该变量名
  // 调用会被解析成对内部函数的直接调用。
  // 键带上了所属函数名,避免不同函数里的同名变量互相干扰。
  std::map<std::string, std::string> lambdaVars_;     // "函数::变量" -> 内部符号
  std::map<std::string, const Lambda *> lambdaDefs_;  // 内部符号 -> 定义
  // 内部符号 -> 需要捕获的外层变量名(按签名顺序)。闭包靠它把外层数据带进去。
  std::map<std::string, std::vector<std::string>> lambdaCaptures_;
  // 嵌套函数:内部符号 -> 形参表(指针)。调用点据此做参数绑定。
  // ⚠️ 存指针而不是拷贝:Param 里有默认值表达式这种不可拷贝的成员,而函数定义
  // 一直挂在语法树上、生命周期足够长,没必要复制一份。
  std::map<std::string, const std::vector<Param> *> nestedParams_;
  // --- 类 ---
  // 类名 -> 定义。实例化与方法解析都用它。
  std::map<std::string, const ClassDef *> classMap_;
  // 类名 -> 类标识(整数)。运行时用它区分不同类的实例。
  std::map<std::string, int64_t> classIds_;
  int64_t classCounter_ = 1;
  // "函数::变量" -> 类名。靠它把变量解析成实例,从而判定 obj.attr 是属性还是方法。
  std::map<std::string, std::string> varClasses_;
  // "类名.属性名" -> 类名。记录某个属性持有的是哪个类的实例,
  // 这样 self.tree.insert(...) 这类组合结构的方法调用才能解析。
  std::map<std::string, std::string> classAttrClasses_;
  int lambdaCounter_ = 0;
  std::set<std::string> declaredSyms_;   // 已生成过声明的用户函数
};

}  // namespace pylite
