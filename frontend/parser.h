// 递归下降语法分析器。
//
// 块的边界完全由词法层的 INDENT/DEDENT 决定 —— 这里不需要处理缩进,
// 只要在需要块的地方 expect(Indent) ... expect(Dedent) 即可。
//
// 运算符优先级(低到高),与 Python 一致:
//   or  ->  and  ->  not  ->  比较  ->  + -  ->  * / // %  ->  一元 -  ->  **  ->  后缀
// 注意两个细节:
//   * not 的优先级低于比较,所以 `not a == b` 等价于 `not (a == b)`
//   * ** 右结合,且一元负号比它低,所以 `-2 ** 2` 是 `-(2 ** 2)`
#pragma once

#include "ast.h"
#include "token.h"

#include <string>
#include <vector>

namespace xmex {

class Parser {
 public:
  Parser(std::vector<Token> toks, std::string filename);

  // 解析整个文件。出错时抛 SourceError。
  std::unique_ptr<Module> parseModule();

 private:
  const Token &peek(size_t ahead = 0) const;
  const Token &cur() const { return peek(0); }
  bool at(Tok k) const { return cur().kind == k; }
  bool accept(Tok k);
  const Token &expect(Tok k, const char *what);
  [[noreturn]] void fail(const std::string &msg) const;
  [[noreturn]] void failAt(const Token &t, const std::string &msg) const;

  // --- 语句 ---
  std::vector<StmtPtr> parseBlock();       // 消费 INDENT ... DEDENT
  StmtPtr parseStmt();
  StmtPtr parseSimpleStmt();               // 赋值 / 表达式语句(到 NEWLINE 为止)
  StmtPtr parseFuncDef();
  // 类定义:class 名字[(父类)]: 后跟若干成员方法。
  StmtPtr parseClassDef();
  // 形参列表:(name[:type] [= default] | *args) —— 供函数定义与匿名函数共用。
  // allowTypes=false 时不吃冒号:匿名函数的冒号是函数体的分隔符,不是类型注解。
  std::vector<Param> parseParams(bool allowTypes = true);
  StmtPtr parseIf();
  StmtPtr parseWhile();
  StmtPtr parseFor();
  StmtPtr parseReturn();

  // --- 表达式 ---
  // 逗号分隔。outEndedWithComma(可为 nullptr)报告列表是否**以逗号收尾** ——
  // 这个信息不能丢:`(1,)` 是单元素元组,而 `(1)` 就是 `1`,区别全在尾随逗号上。
  std::vector<ExprPtr> parseExprList(bool *outEndedWithComma = nullptr);
  ExprPtr parseExpr();
  ExprPtr parseOr();
  ExprPtr parseAnd();
  ExprPtr parseNot();
  ExprPtr parseComparison();
  ExprPtr parseAdd();
  ExprPtr parseMul();
  ExprPtr parseUnary();
  ExprPtr parsePower();
  ExprPtr parsePostfix();
  ExprPtr parseAtom();
  // lambda 形参: 单表达式体,解析后包成一条 return
  ExprPtr parseLambda();

  // a[...] 的两种形态:下标 a[i] 或切片 a[lo:hi:step]
  ExprPtr parseSubscriptOf(ExprPtr obj);

  // 把逗号分隔的表达式列表包装成单个节点:
  // 多个元素包成 TupleLit;只有一个元素时**只有列表以逗号收尾**才包 ——
  // `(1,)` / `x = 1,` / `return 1,` 在 Python 里都是单元素元组。
  // `a, b = b, a` 的右侧也走这条路径。
  ExprPtr wrapList(std::vector<ExprPtr> items, int line, int col,
                   bool endedWithComma = false);

  TypeName parseTypeAnnotation();          // int / float / bool / str

  std::vector<Token> toks_;
  std::string file_;
  size_t pos_ = 0;
};

}  // namespace xmex
