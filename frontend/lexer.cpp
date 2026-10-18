#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace xmex {

const char *tokName(Tok t) {
  switch (t) {
    case Tok::EndOfFile: return "EOF";
    case Tok::Newline:   return "NEWLINE";
    case Tok::Indent:    return "INDENT";
    case Tok::Dedent:    return "DEDENT";
    case Tok::Identifier: return "IDENT";
    case Tok::IntLit:    return "INT";
    case Tok::FloatLit:  return "FLOAT";
    case Tok::StringLit: return "STRING";
    case Tok::KwDef:     return "def";
    case Tok::KwReturn:  return "return";
    case Tok::KwIf:      return "if";
    case Tok::KwElif:    return "elif";
    case Tok::KwElse:    return "else";
    case Tok::KwWhile:   return "while";
    case Tok::KwFor:     return "for";
    case Tok::KwIn:      return "in";
    case Tok::KwAnd:     return "and";
    case Tok::KwOr:      return "or";
    case Tok::KwNot:     return "not";
    case Tok::KwTrue:    return "True";
    case Tok::KwFalse:   return "False";
    case Tok::KwNone:    return "None";
    case Tok::KwLambda:  return "lambda";
    case Tok::KwClass:   return "class";
    case Tok::KwSelf:    return "self";   // 已不产出,保留以维持枚举完整
    case Tok::KwBreak:   return "break";
    case Tok::KwContinue:return "continue";
    case Tok::KwInt:     return "int";
    case Tok::KwFloat:   return "float";
    case Tok::KwBool:    return "bool";
    case Tok::KwStr:     return "str";
    case Tok::LParen:    return "(";
    case Tok::RParen:    return ")";
    case Tok::LBracket:  return "[";
    case Tok::RBracket:  return "]";
    case Tok::LBrace:    return "{";
    case Tok::RBrace:    return "}";
    case Tok::Comma:     return ",";
    case Tok::Colon:     return ":";
    case Tok::Dot:       return ".";
    case Tok::Arrow:     return "->";
    case Tok::Assign:    return "=";
    case Tok::Plus:      return "+";
    case Tok::Minus:     return "-";
    case Tok::Star:      return "*";
    case Tok::Slash:     return "/";
    case Tok::DoubleSlash:return "//";
    case Tok::Percent:   return "%";
    case Tok::StarStar:  return "**";
    case Tok::EqEq:      return "==";
    case Tok::NotEq:     return "!=";
    case Tok::Lt:        return "<";
    case Tok::Le:        return "<=";
    case Tok::Gt:        return ">";
    case Tok::Ge:        return ">=";
  }
  return "<?>";
}

SourceError::SourceError(int line, int col, const std::string &msg)
    : std::runtime_error(msg), line_(line), col_(col) {}

Lexer::Lexer(std::string source, std::string filename)
    : src_(std::move(source)), file_(std::move(filename)) {}

char Lexer::advance() {
  char c = src_[pos_++];
  if (c == '\n') {
    ++line_;
    col_ = 1;
  } else {
    ++col_;
  }
  return c;
}

void Lexer::failAt(int line, int col, const std::string &msg) const {
  throw SourceError(line, col,
                    file_ + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " + msg);
}

void Lexer::fail(const std::string &msg) const { failAt(line_, col_, msg); }

void Lexer::emit(Tok kind, const std::string &text, int line, int col) {
  Token t;
  t.kind = kind;
  t.text = text;
  t.line = line;
  t.col = col;
  out_.push_back(std::move(t));
}

// 处理一行开头。返回 true 表示这一行有实际内容,调用方应继续扫描;
// 返回 false 表示这是空行或纯注释行,已经被整行消费掉(含换行符)。
bool Lexer::handleLineStart() {
  int indent = 0;
  for (;;) {
    char c = peek();
    if (c == ' ') {
      indent += 1;
      advance();
    } else if (c == '\t') {
      // 制表符对齐到下一个 8 的倍数,与 Python 一致
      indent += 8 - (indent % 8);
      advance();
    } else {
      break;
    }
  }

  if (atEnd()) return false;

  // 空行:吃掉换行,整行跳过(空行不产生 NEWLINE,也不影响缩进层级)
  if (peek() == '\r' || peek() == '\n') {
    advance();
    return false;
  }

  // 纯注释行:同样是整行跳过
  if (peek() == '#') {
    while (!atEnd() && peek() != '\n') advance();
    if (!atEnd()) advance();
    return false;
  }

  const int top = indents_.back();
  if (indent > top) {
    indents_.push_back(indent);
    emit(Tok::Indent, "", line_, col_);
  } else if (indent < top) {
    while (indents_.size() > 1 && indents_.back() > indent) {
      indents_.pop_back();
      emit(Tok::Dedent, "", line_, col_);
    }
    // 缩进必须回到某个已存在的层级,否则是"错位"的缩进
    if (indents_.back() != indent) {
      fail("取消缩进与任何外层缩进层级都不匹配");
    }
  }
  return true;
}

std::vector<Token> Lexer::tokenize() {
  for (;;) {
    if (atLineStart_) {
      if (!handleLineStart()) {
        if (atEnd()) break;
        continue;
      }
      atLineStart_ = false;
      if (atEnd()) break;
    }

    if (atEnd()) break;

    const char c = peek();
    if (c == '\n') {
      // 只有不在括号内时,换行才终止逻辑行
      if (bracketDepth_ == 0) {
        emit(Tok::Newline, "\n", line_, col_);
        atLineStart_ = true;
      }
      advance();
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') { advance(); continue; }
    if (c == '#') {
      while (!atEnd() && peek() != '\n') advance();
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) { lexNumber(); continue; }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') { lexIdentifier(); continue; }
    if (c == '"' || c == '\'') { lexString(c); continue; }
    lexOperator();
  }

  // 收尾:若最后一行没有换行结尾,补一个 NEWLINE,
  // 否则语法分析器会看到一个缺少语句终结符的块。
  if (!out_.empty() && out_.back().kind != Tok::Newline &&
      out_.back().kind != Tok::Indent) {
    emit(Tok::Newline, "\n", line_, col_);
  }
  // 把仍然开着的缩进块全部 DEDENT 掉
  while (indents_.size() > 1) {
    indents_.pop_back();
    emit(Tok::Dedent, "", line_, col_);
  }
  emit(Tok::EndOfFile, "", line_, col_);
  return std::move(out_);
}

void Lexer::lexNumber() {
  const int startLine = line_;
  const int startCol = col_;
  const size_t start = pos_;
  bool isFloat = false;

  while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();

  // 小数点后必须紧跟数字才算浮点,这样 `1.foo` 之类的写法不会被误吞
  if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
    isFloat = true;
    advance();
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
  }

  // 科学计数法。若 e 后面不是合法指数,则回退,把 e 留给标识符处理。
  if (peek() == 'e' || peek() == 'E') {
    const size_t savePos = pos_;
    const int saveLine = line_, saveCol = col_;
    advance();
    if (peek() == '+' || peek() == '-') advance();
    if (std::isdigit(static_cast<unsigned char>(peek()))) {
      isFloat = true;
      while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    } else {
      pos_ = savePos;
      line_ = saveLine;
      col_ = saveCol;
    }
  }

  const std::string text = src_.substr(start, pos_ - start);
  Token t;
  t.kind = isFloat ? Tok::FloatLit : Tok::IntLit;
  t.text = text;
  t.line = startLine;
  t.col = startCol;
  if (isFloat) t.floatValue = std::strtod(text.c_str(), nullptr);
  else         t.intValue = std::strtoll(text.c_str(), nullptr, 10);
  out_.push_back(std::move(t));
}

void Lexer::lexIdentifier() {
  const int startLine = line_;
  const int startCol = col_;
  const size_t start = pos_;
  while (!atEnd() &&
         (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
    advance();
  }
  const std::string text = src_.substr(start, pos_ - start);

  static const std::unordered_map<std::string, Tok> kKeywords = {
      {"def", Tok::KwDef},       {"return", Tok::KwReturn},
      {"if", Tok::KwIf},         {"elif", Tok::KwElif},
      {"else", Tok::KwElse},     {"while", Tok::KwWhile},
      {"for", Tok::KwFor},       {"in", Tok::KwIn},
      {"and", Tok::KwAnd},       {"or", Tok::KwOr},
      {"not", Tok::KwNot},       {"True", Tok::KwTrue},
      {"False", Tok::KwFalse},   {"None", Tok::KwNone},
      {"break", Tok::KwBreak},   {"continue", Tok::KwContinue},
      {"int", Tok::KwInt},       {"float", Tok::KwFloat},
      {"bool", Tok::KwBool},     {"str", Tok::KwStr},
      {"lambda", Tok::KwLambda},
      {"class", Tok::KwClass},
  };

  auto it = kKeywords.find(text);
  emit(it != kKeywords.end() ? it->second : Tok::Identifier, text, startLine, startCol);
}

void Lexer::lexString(char quote) {
  const int startLine = line_;
  const int startCol = col_;
  advance();  // 开引号

  std::string value;
  for (;;) {
    if (atEnd()) failAt(startLine, startCol, "字符串没有闭合的引号");
    const char c = peek();
    if (c == '\n') {
      failAt(startLine, startCol,
             "字符串不能跨行(起始于第 " + std::to_string(startLine) + " 行)");
    }
    if (c == quote) {
      advance();
      break;
    }
    if (c == '\\') {
      advance();
      if (atEnd()) failAt(startLine, startCol, "字符串末尾的转义符不完整");
      const int escLine = line_, escCol = col_;
      const char e = advance();
      switch (e) {
        case 'n':  value += '\n'; break;
        case 't':  value += '\t'; break;
        case 'r':  value += '\r'; break;
        case '\\': value += '\\'; break;
        case '\'': value += '\''; break;
        case '"':  value += '"';  break;
        case '0':  value += '\0'; break;
        default:
          failAt(escLine, escCol, std::string("无法识别的转义序列 \\") + e);
      }
    } else {
      value += advance();
    }
  }

  Token t;
  t.kind = Tok::StringLit;
  t.text = value;  // 存的是解码后的结果,IRGen 直接用,不必再解一遍转义
  t.line = startLine;
  t.col = startCol;
  out_.push_back(std::move(t));
}

void Lexer::lexOperator() {
  const int startLine = line_;
  const int startCol = col_;
  const char c = advance();

  // 先看下一个字符是不是能拼成双字符运算符;不能就退回单字符版本
  auto two = [&](char second, Tok combined, Tok single) {
    if (peek() == second) {
      advance();
      emit(combined, std::string(1, c) + second, startLine, startCol);
    } else {
      emit(single, std::string(1, c), startLine, startCol);
    }
  };

  switch (c) {
    case '(': ++bracketDepth_; emit(Tok::LParen, "(", startLine, startCol); return;
    case ')': if (bracketDepth_ > 0) --bracketDepth_; emit(Tok::RParen, ")", startLine, startCol); return;
    case '[': ++bracketDepth_; emit(Tok::LBracket, "[", startLine, startCol); return;
    case ']': if (bracketDepth_ > 0) --bracketDepth_; emit(Tok::RBracket, "]", startLine, startCol); return;
    case '{': ++bracketDepth_; emit(Tok::LBrace, "{", startLine, startCol); return;
    case '}': if (bracketDepth_ > 0) --bracketDepth_; emit(Tok::RBrace, "}", startLine, startCol); return;
    case ',': emit(Tok::Comma, ",", startLine, startCol); return;
    case ':': emit(Tok::Colon, ":", startLine, startCol); return;
    case '.': emit(Tok::Dot, ".", startLine, startCol); return;
    case '+': emit(Tok::Plus, "+", startLine, startCol); return;
    case '%': emit(Tok::Percent, "%", startLine, startCol); return;
    case '-': two('>', Tok::Arrow, Tok::Minus); return;
    // ** 必须比 * 先判断
    case '*': two('*', Tok::StarStar, Tok::Star); return;
    // // 是整除,不是注释(# 才是注释)
    case '/': two('/', Tok::DoubleSlash, Tok::Slash); return;
    case '=': two('=', Tok::EqEq, Tok::Assign); return;
    case '!':
      if (peek() == '=') {
        advance();
        emit(Tok::NotEq, "!=", startLine, startCol);
        return;
      }
      failAt(startLine, startCol, "意外的字符 '!'(不等号应写成 '!=')");
    case '<': two('=', Tok::Le, Tok::Lt); return;
    case '>': two('=', Tok::Ge, Tok::Gt); return;
    default:
      failAt(startLine, startCol, std::string("无法识别的字符 '") + c + "'");
  }
}

}  // namespace xmex
