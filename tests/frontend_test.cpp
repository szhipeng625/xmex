// 前端单元测试。
//
// 重点覆盖"写错了不会立刻发现、但语义会悄悄跑偏"的地方:
//   * 运算符优先级与结合性(-2 ** 2、2 ** 3 ** 2、not a == b)
//   * 多目标赋值与元组解包共用一条路径
//   * 复合赋值 += 的词法歧义(op 后面紧跟 =)
//   * 缩进块边界(INDENT/DEDENT),包括错位缩进的报错
//   * range / input 这两个特殊形式
#include "lexer.h"
#include "parser.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace xmex;

namespace {

int g_failures = 0;

void check(bool cond, const std::string &what) {
  if (cond) {
    std::printf("  [ok]   %s\n", what.c_str());
  } else {
    std::printf("  [FAIL] %s\n", what.c_str());
    ++g_failures;
  }
}

std::unique_ptr<Module> parse(const std::string &src) {
  Lexer lex(src, "<test>");
  Parser p(lex.tokenize(), "<test>");
  return p.parseModule();
}

// 解析应当失败的情形,返回是否真的抛了 SourceError
bool parseFails(const std::string &src, std::string *msg = nullptr) {
  try {
    parse(src);
  } catch (const SourceError &e) {
    if (msg) *msg = e.what();
    return true;
  }
  return false;
}

const Stmt *stmt(const Module &m, size_t i) {
  return i < m.body.size() ? m.body[i].get() : nullptr;
}

// 取单条语句的表达式(用于 ExprStmt)
const Expr *exprOf(const Stmt *s) {
  if (auto *es = dynamic_cast<const ExprStmt *>(s)) return es->expr.get();
  return nullptr;
}

}  // namespace

int main() {
  std::printf("=== 前端测试 ===\n\n");

  // ---------------------------------------------------------------- 优先级
  std::printf("-- 运算符优先级与结合性 --\n");
  {
    // -2 ** 2 必须是 -(2 ** 2) = -4,而不是 (-2) ** 2 = 4
    auto m = parse("-2 ** 2\n");
    auto *u = dynamic_cast<const UnaryOp *>(exprOf(stmt(*m, 0)));
    check(u && u->op == "-", "-2 ** 2 :最外层是一元负号");
    auto *b = u ? dynamic_cast<const BinOp *>(u->operand.get()) : nullptr;
    check(b && b->op == "**", "-2 ** 2 :负号的内部是 ** (即 -(2**2),与 Python 一致)");
  }
  {
    // ** 右结合:2 ** 3 ** 2 应当是 2 ** (3 ** 2)
    auto m = parse("2 ** 3 ** 2\n");
    auto *b = dynamic_cast<const BinOp *>(exprOf(stmt(*m, 0)));
    check(b && b->op == "**", "2 ** 3 ** 2 :最外层是 **");
    auto *rhs = b ? dynamic_cast<const BinOp *>(b->rhs.get()) : nullptr;
    check(rhs && rhs->op == "**", "2 ** 3 ** 2 :右侧还是 ** (右结合)");
  }
  {
    // not 优先级低于比较:not a == b 是 not (a == b)
    auto m = parse("not a == b\n");
    auto *u = dynamic_cast<const UnaryOp *>(exprOf(stmt(*m, 0)));
    check(u && u->op == "not", "not a == b :最外层是 not");
    auto *b = u ? dynamic_cast<const BinOp *>(u->operand.get()) : nullptr;
    check(b && b->op == "==", "not a == b :not 作用于整个比较 (优先级低于 ==)");
  }
  {
    // 乘法优先级高于加法
    auto m = parse("1 + 2 * 3\n");
    auto *b = dynamic_cast<const BinOp *>(exprOf(stmt(*m, 0)));
    check(b && b->op == "+", "1 + 2 * 3 :最外层是 +");
    auto *rhs = b ? dynamic_cast<const BinOp *>(b->rhs.get()) : nullptr;
    check(rhs && rhs->op == "*", "1 + 2 * 3 :+ 的右侧是 * (乘法优先)");
  }

  // ------------------------------------------------------------ 赋值与解包
  std::printf("\n-- 赋值与元组解包 --\n");
  {
    auto m = parse("a, b = input(int, int)\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    check(as && as->targets.size() == 2, "a, b = input(int, int) :两个赋值目标");
    auto *in = as ? dynamic_cast<const InputExpr *>(as->value.get()) : nullptr;
    check(in && in->kinds.size() == 2 && in->kinds[0] == TypeName::Int &&
              in->kinds[1] == TypeName::Int,
          "a, b = input(int, int) :右侧解析成 Input(int, int)");
  }
  {
    // a = 1 与 a, b = ... 共用同一条路径,单目标也走 Assign
    auto m = parse("a = 1\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    check(as && as->targets.size() == 1, "a = 1 :也是 Assign,单目标");
  }
  {
    auto m = parse("a, b = b, a\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    check(as && as->targets.size() == 2, "a, b = b, a :两个目标");
    auto *tup = as ? dynamic_cast<const TupleLit *>(as->value.get()) : nullptr;
    check(tup && tup->items.size() == 2, "a, b = b, a :右侧是 2 元素元组");
  }
  {
    auto m = parse("a += 1\n");
    auto *aa = dynamic_cast<const AugAssign *>(stmt(*m, 0));
    check(aa && aa->op == "+", "a += 1 :解析成复合赋值,运算符为 +");
  }
  {
    auto m = parse("a = -1\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    auto *u = as ? dynamic_cast<const UnaryOp *>(as->value.get()) : nullptr;
    check(u && u->op == "-", "a = -1 :这里的 - 是一元负号(未被误判为 -=)");
  }

  // -------------------------------------------------------------- 特殊形式
  std::printf("\n-- range / input 特殊形式 --\n");
  {
    auto m = parse("for i in range(1, a):\n    print(i)\n");
    auto *f = dynamic_cast<const For *>(stmt(*m, 0));
    check(f && f->var == "i", "for i in range(...) :循环变量是 i");
    auto *r = f ? dynamic_cast<const RangeExpr *>(f->iterable.get()) : nullptr;
    check(r != nullptr, "for i in range(...) :iterable 是 RangeExpr 而非普通调用");
    auto *start = r ? dynamic_cast<const IntLit *>(r->start.get()) : nullptr;
    auto *stop = r ? dynamic_cast<const Name *>(r->stop.get()) : nullptr;
    check(start && start->value == 1, "range(1, a) :start = 1");
    check(stop && stop->id == "a", "range(1, a) :stop = a");
    check(r && r->step == nullptr, "range(1, a) :step 未给出,为 nullptr");
    check(f && f->body.size() == 1, "for 的循环体解析出 1 条语句");
  }
  {
    auto m = parse("for i in range(n):\n    print(i)\n");
    auto *f = dynamic_cast<const For *>(stmt(*m, 0));
    auto *r = f ? dynamic_cast<const RangeExpr *>(f->iterable.get()) : nullptr;
    check(r && r->start == nullptr, "range(n) :只给一个参数时 start 为 nullptr(语义上等于 0)");
  }
  {
    bool ok = parseFails("for i in range(1, 2, 3, 4):\n    print(i)\n");
    check(ok, "range 给 4 个参数时报错");
  }
  {
    bool ok = parseFails("x = input()\n");
    check(ok, "input() 不带类型时报错");
  }

  // ---------------------------------------------------------------- 缩进块
  std::printf("\n-- 缩进块 --\n");
  {
    Lexer lex("if a:\n    b = 1\n    c = 2\nd = 3\n", "<test>");
    int indents = 0, dedents = 0;
    for (const auto &t : lex.tokenize()) {
      if (t.kind == Tok::Indent) ++indents;
      if (t.kind == Tok::Dedent) ++dedents;
    }
    check(indents == 1 && dedents == 1, "一层缩进块:1 个 INDENT、1 个 DEDENT");
  }
  {
    // 嵌套两层
    Lexer lex("if a:\n    if b:\n        c = 1\n", "<test>");
    int indents = 0, dedents = 0;
    for (const auto &t : lex.tokenize()) {
      if (t.kind == Tok::Indent) ++indents;
      if (t.kind == Tok::Dedent) ++dedents;
    }
    check(indents == 2 && dedents == 2, "两层嵌套:2 个 INDENT、2 个 DEDENT(文件末尾补齐)");
  }
  {
    // 空行和注释行不参与缩进
    Lexer lex("a = 1\n\n# 注释\n\nb = 2\n", "<test>");
    int indents = 0, dedents = 0;
    for (const auto &t : lex.tokenize()) {
      if (t.kind == Tok::Indent) ++indents;
      if (t.kind == Tok::Dedent) ++dedents;
    }
    check(indents == 0 && dedents == 0, "空行与纯注释行不产生 INDENT/DEDENT");
  }
  {
    // 括号内换行不结束逻辑行
    Lexer lex("a = [1,\n     2,\n     3]\n", "<test>");
    int newlines = 0;
    for (const auto &t : lex.tokenize()) {
      if (t.kind == Tok::Newline) ++newlines;
    }
    check(newlines == 1, "方括号内换行不产生 NEWLINE(隐式续行)");
  }

  // ------------------------------------------------------------ 错误处理
  std::printf("\n-- 错误处理 --\n");
  {
    std::string msg;
    bool ok = parseFails("if a:\n    b = 1\n  c = 2\n", &msg);
    check(ok, "错位的取消缩进会报错");
  }
  {
    std::string msg;
    bool ok = parseFails("a = $\n", &msg);
    check(ok, "无法识别的字符会报错");
  }
  {
    bool ok = parseFails("def f(:\n    return 1\n");
    check(ok, "参数列表语法错误会报错");
  }
  {
    bool ok = parseFails("a = (1 + 2\n");
    check(ok, "括号未闭合会报错");
  }

  // ------------------------------------------------------------- 容器与其余
  std::printf("\n-- 容器、字符串与其余语法 --\n");
  {
    auto m = parse("x = [1, 2, 3,]\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    auto *l = as ? dynamic_cast<const ListLit *>(as->value.get()) : nullptr;
    check(l && l->items.size() == 3, "列表字面量支持尾随逗号");
  }
  {
    auto m = parse("d = {\"a\": 1, \"b\": 2}\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    auto *d = as ? dynamic_cast<const DictLit *>(as->value.get()) : nullptr;
    check(d && d->items.size() == 2, "字典字面量");
  }
  {
    // (a,) 是单元素元组,(a) 就是 a —— 区别全在尾随逗号上。
    // 曾经解析器把尾随逗号丢掉了,于是 print((1,)) 打出 1(应为 (1,))。
    auto m = parse("(1,)\n");
    auto *t = dynamic_cast<const TupleLit *>(exprOf(stmt(*m, 0)));
    check(t && t->items.size() == 1, "(1,) 解析成单元素元组");

    auto m2 = parse("(1)\n");
    check(dynamic_cast<const IntLit *>(exprOf(stmt(*m2, 0))) != nullptr,
          "(1) 就是整数 1,不是元组");

    auto m3 = parse("x = 1,\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m3, 0));
    auto *tv = as ? dynamic_cast<const TupleLit *>(as->value.get()) : nullptr;
    check(tv && tv->items.size() == 1, "x = 1, 右侧是单元素元组");

    auto m4 = parse("def f():\n    return 1,\n");
    auto *fd = dynamic_cast<const FuncDef *>(stmt(*m4, 0));
    auto *ret = fd && !fd->body.empty()
                    ? dynamic_cast<const Return *>(fd->body[0].get())
                    : nullptr;
    check(ret && dynamic_cast<const TupleLit *>(ret->value.get()) != nullptr,
          "return 1, 返回的是单元素元组");
  }
  {
    auto m = parse("y = s[1:3]\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    auto *sl = as ? dynamic_cast<const SliceExpr *>(as->value.get()) : nullptr;
    check(sl && sl->lo && sl->hi && !sl->step, "切片 s[1:3]:lo/hi 有值,step 为空");
  }
  {
    auto m = parse("y = a[:]\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    auto *sl = as ? dynamic_cast<const SliceExpr *>(as->value.get()) : nullptr;
    check(sl && !sl->lo && !sl->hi, "切片 a[:]:两端都省略");
  }
  {
    auto m = parse("y = a[::-1]\n");
    auto *as = dynamic_cast<const Assign *>(stmt(*m, 0));
    auto *sl = as ? dynamic_cast<const SliceExpr *>(as->value.get()) : nullptr;
    auto *st = sl ? dynamic_cast<const UnaryOp *>(sl->step.get()) : nullptr;
    check(sl && st, "切片 a[::-1]:step 解析为 -1");
  }
  {
    auto m = parse("s.upper()\n");
    auto *c = dynamic_cast<const Call *>(exprOf(stmt(*m, 0)));
    auto *a = c ? dynamic_cast<const Attribute *>(c->callee.get()) : nullptr;
    check(a && a->name == "upper", "方法调用 s.upper() 解析成 Attribute");
  }
  {
    auto m = parse("def f(a: int, b: str) -> bool:\n    return True\n");
    auto *fd = dynamic_cast<const FuncDef *>(stmt(*m, 0));
    check(fd && fd->params.size() == 2, "函数定义:两个参数");
    check(fd && fd->params[0].type == TypeName::Int &&
              fd->params[1].type == TypeName::Str,
          "函数定义:参数类型注解正确");
    check(fd && fd->retType == TypeName::Bool, "函数定义:返回类型注解正确");
  }

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
