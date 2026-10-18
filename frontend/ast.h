// Xmex 抽象语法树。
//
// 用继承 + 虚函数表达节点:节点种类不多(二十来个),虚函数带来的开销在前端
// 阶段完全无所谓,换来的是 dump/遍历写法最直白。IRGen 那边用 dynamic_cast
// 或自建的 kind 标签都可以。
//
// 所有节点都记录 line/col —— 报错时能指回源码位置,这在写 IRGen 时很省事。
#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xmex {

// 语言里出现的类型名。Any 作为"未标注"的默认值。
enum class TypeName { Any, Int, Float, Bool, Str };

const char *typeNameStr(TypeName t);

// ---------------------------------------------------------------------------
// 表达式
// ---------------------------------------------------------------------------

struct Expr {
  int line = 0;
  int col = 0;
  virtual ~Expr() = default;
  virtual void dump(std::ostream &os, int indent) const = 0;
};

using ExprPtr = std::unique_ptr<Expr>;

struct IntLit : Expr {
  int64_t value = 0;
  void dump(std::ostream &, int) const override;
};

struct FloatLit : Expr {
  double value = 0.0;
  void dump(std::ostream &, int) const override;
};

struct StringLit : Expr {
  std::string value;  // 已解码转义序列
  void dump(std::ostream &, int) const override;
};

struct BoolLit : Expr {
  bool value = false;
  void dump(std::ostream &, int) const override;
};

struct NoneLit : Expr {
  void dump(std::ostream &, int) const override;
};

struct Name : Expr {
  std::string id;
  void dump(std::ostream &, int) const override;
};

// 二元运算。op 存的是源码里的符号(+ - * / // % ** == != < <= > >= and or)
struct BinOp : Expr {
  std::string op;
  ExprPtr lhs, rhs;
  void dump(std::ostream &, int) const override;
};

struct UnaryOp : Expr {
  std::string op;  // "-" | "not"
  ExprPtr operand;
  void dump(std::ostream &, int) const override;
};

struct Call : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;                            // 位置参数
  // 关键字参数 name=value。按名字绑定,调用点重排成位置顺序后交给被调函数。
  std::vector<std::pair<std::string, ExprPtr>> kwargs;
  void dump(std::ostream &, int) const override;
};

// range(a) / range(a, b) / range(a, b, step) —— 单独建节点而不是当普通 Call,
// 因为 IRGen 要把它直接降成计数循环 + PHI,不走迭代器协议。
// 未提供的参数为 nullptr。
struct RangeExpr : Expr {
  ExprPtr start, stop, step;
  void dump(std::ostream &, int) const override;
};

// input(int, int) —— 特殊形式:按给定类型从 stdin 读若干个值,返回元组。
// 只存类型,不存参数表达式。
struct InputExpr : Expr {
  std::vector<TypeName> kinds;
  void dump(std::ostream &, int) const override;
};

struct Subscript : Expr {
  ExprPtr obj;
  ExprPtr index;
  void dump(std::ostream &, int) const override;
};

// a[lo:hi:step],省略的部分为 nullptr
struct SliceExpr : Expr {
  ExprPtr obj;
  ExprPtr lo, hi, step;
  void dump(std::ostream &, int) const override;
};

struct ListLit : Expr {
  std::vector<ExprPtr> items;
  void dump(std::ostream &, int) const override;
};

struct DictLit : Expr {
  std::vector<std::pair<ExprPtr, ExprPtr>> items;
  void dump(std::ostream &, int) const override;
};

// a, b —— 元组构造。也用于 `a, b = <多值表达式>` 的右侧。
struct TupleLit : Expr {
  std::vector<ExprPtr> items;
  void dump(std::ostream &, int) const override;
};

// x.attr 形式的方法调用:m.upper() 解析成 Call{ callee=Attribute{obj=m,name=upper} }
struct Attribute : Expr {
  ExprPtr obj;
  std::string name;
  void dump(std::ostream &, int) const override;
};

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

struct Stmt {
  int line = 0;
  int col = 0;
  virtual ~Stmt() = default;
  virtual void dump(std::ostream &os, int indent) const = 0;
};

using StmtPtr = std::unique_ptr<Stmt>;

struct ExprStmt : Stmt {
  ExprPtr expr;
  void dump(std::ostream &, int) const override;
};

// a = v / a, b = v —— targets 可以有多个,右侧统一按"多值解包"处理。
// 即使是单目标赋值也走这条路径,避免两套代码。
struct Assign : Stmt {
  std::vector<ExprPtr> targets;
  ExprPtr value;
  void dump(std::ostream &, int) const override;
};

struct AugAssign : Stmt {
  ExprPtr target;
  std::string op;  // "+=" 等(存的是去掉 = 的运算符,如 "+")
  ExprPtr value;
  void dump(std::ostream &, int) const override;
};

struct If : Stmt {
  ExprPtr cond;
  std::vector<StmtPtr> body;
  std::vector<std::pair<ExprPtr, std::vector<StmtPtr>>> elifs;
  std::vector<StmtPtr> orelse;
  void dump(std::ostream &, int) const override;
};

struct While : Stmt {
  ExprPtr cond;
  std::vector<StmtPtr> body;
  void dump(std::ostream &, int) const override;
};

// for <var> in <iterable>:  —— iterable 为 RangeExpr 时降成计数循环,
// 否则走通用迭代(遍历 list)。
struct For : Stmt {
  std::string var;
  ExprPtr iterable;
  std::vector<StmtPtr> body;
  void dump(std::ostream &, int) const override;
};

struct Return : Stmt {
  ExprPtr value;  // 可为 nullptr
  void dump(std::ostream &, int) const override;
};

struct Break : Stmt {
  void dump(std::ostream &, int) const override;
};

struct Continue : Stmt {
  void dump(std::ostream &, int) const override;
};

// 函数形参。
//   defaultValue != nullptr  -> 默认参数,调用时可省略
//   isVararg == true         -> *args,收集多余的未知位置参数
// 必填参数必须排在默认参数之前,可变参数排在最后 —— 由解析器保证。
struct Param {
  std::string name;
  TypeName type = TypeName::Any;
  ExprPtr defaultValue;   // 默认值表达式;nullptr 表示必填
  bool isVararg = false;  // *args
};

struct FuncDef : Stmt {
  std::string name;
  std::vector<Param> params;
  TypeName retType = TypeName::Any;
  std::vector<StmtPtr> body;
  void dump(std::ostream &, int) const override;
};

// 匿名函数(lambda)。解析完得到一个可与其他值同样传递的第一类函数值。
// 单表达式体会被解析器包成一条 return。
struct Lambda : Expr {
  std::vector<Param> params;
  std::vector<StmtPtr> body;
  void dump(std::ostream &, int) const override;
};

// 类定义。
//   name       —— 类名
//   baseName   —— 父类名(空表示没有父类);当前支持单继承、方法可重写
//   methods    —— 成员方法,每个方法都是带 self 形参的普通函数体
// 属性不在这里声明:属性在实例化后按需产生(与 Python 一致的写法)。
struct ClassDef : Stmt {
  std::string name;
  std::string baseName;
  std::vector<std::unique_ptr<FuncDef>> methods;
  void dump(std::ostream &, int) const override;
};

struct Module {
  std::vector<StmtPtr> body;
  void dump(std::ostream &os) const;
};

}  // namespace xmex
