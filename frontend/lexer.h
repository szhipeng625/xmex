// 缩进敏感的词法分析器。
//
// 难点在于 Python 式的块结构:词法层要负责把缩进变化翻译成 INDENT/DEDENT
// 标记,语法层才能像处理普通花括号语言那样递归下降。
//
// 几条容易写错、这里刻意处理了的规则:
//   * 空行和纯注释行不产生 NEWLINE,也不参与缩进比较
//   * 圆括号/方括号/花括号内部换行不产生 NEWLINE(隐式续行)
//   * 制表符按 8 的倍数对齐,与 Python 一致
//   * 文件末尾要补出缺失的 NEWLINE,以及把所有未闭合的缩进 DEDENT 掉
#pragma once

#include "token.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace xmex {

// 词法/语法错误统一用这个异常抛出。编译器自身可以用异常 ——
// "不用异常"的限制只针对 IRGen 生成的目标代码(见 docs/llvm-notes.md R1)。
class SourceError : public std::runtime_error {
 public:
  SourceError(int line, int col, const std::string &msg);
  int line() const { return line_; }
  int col() const { return col_; }
 private:
  int line_, col_;
};

class Lexer {
 public:
  explicit Lexer(std::string source, std::string filename = "<input>");

  // 全量分词。返回的序列一定以 EndOfFile 结尾。
  std::vector<Token> tokenize();

 private:
  bool atEnd() const { return pos_ >= src_.size(); }
  char peek(size_t ahead = 0) const {
    return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
  }
  char advance();

  [[noreturn]] void fail(const std::string &msg) const;
  [[noreturn]] void failAt(int line, int col, const std::string &msg) const;

  void emit(Tok kind, const std::string &text, int line, int col);

  // 扫描一行开头,决定要不要产出 INDENT/DEDENT。
  // 返回 false 表示这一行是空行或纯注释行,应当整行跳过。
  bool handleLineStart();

  void lexNumber();
  void lexIdentifier();
  void lexString(char quote);
  void lexOperator();

  std::string src_;
  std::string file_;
  size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;

  std::vector<Token> out_;
  std::vector<int> indents_{0};   // 缩进栈,栈底恒为 0
  int bracketDepth_ = 0;          // > 0 时换行不产生 NEWLINE
  bool atLineStart_ = true;
};

}  // namespace xmex
