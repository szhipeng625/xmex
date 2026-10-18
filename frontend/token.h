// Xmex 词法单元定义。
//
// 这是一门缩进敏感的语言,所以除了常见的 token 之外还有三个特殊标记:
//   NEWLINE —— 逻辑行结束
//   INDENT  —— 进入更深一层缩进块
//   DEDENT  —— 退出到更浅一层缩进块
// 语法分析器靠 INDENT/DEDENT 来确定块的边界,不需要显式的 end 关键字。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace xmex {

enum class Tok : uint8_t {
  EndOfFile,
  Newline,   // 逻辑行结束
  Indent,    // 缩进增加
  Dedent,    // 缩进减少

  Identifier,
  IntLit,
  FloatLit,
  StringLit,

  // --- 关键字 ---
  KwDef, KwReturn,
  KwIf, KwElif, KwElse,
  KwWhile, KwFor, KwIn,
  KwAnd, KwOr, KwNot,
  KwTrue, KwFalse, KwNone,
  KwLambda,
  KwClass, KwSelf,
  KwBreak, KwContinue,
  // 类型名。input(int, int) 里的 int 走这个 token;
  // 同时它们也是合法的类型注解。
  KwInt, KwFloat, KwBool, KwStr,

  // --- 标点 ---
  LParen, RParen,
  LBracket, RBracket,
  LBrace, RBrace,
  Comma, Colon, Dot, Arrow,     // ( ) [ ] { } , : . ->
  Assign,                        // =

  // --- 运算符 ---
  Plus, Minus, Star, Slash, DoubleSlash, Percent, StarStar,  // + - * / // % **
  EqEq, NotEq, Lt, Le, Gt, Ge,                               // == != < <= > >=
};

// 供报错信息与 --dump-tokens 使用
const char *tokName(Tok t);

struct Token {
  Tok kind = Tok::EndOfFile;
  std::string text;   // 标识符名、字面量原文、运算符符号
  int line = 1;       // 1 起
  int col = 1;        // 1 起

  // 字面量的值。用同一个 int64/double 联合体承载,避免下游再解析一次文本。
  int64_t intValue = 0;
  double floatValue = 0.0;

  bool is(Tok k) const { return kind == k; }
};

}  // namespace xmex
