// 开发用工具:把 .pys 的 token 流打出来,用于验证词法分析器。
// 缩进敏感的词法器很难靠肉眼读代码确认正确性,把 INDENT/DEDENT 打出来最直观。
#include "lexer.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// 把换行/制表符转义掉,否则 dump 出来的对齐会乱掉
std::string visible(const std::string &s) {
  std::string r;
  for (char c : s) {
    switch (c) {
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\0': r += "\\0"; break;
      default:   r += c;
    }
  }
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "用法: lex_dump <file.pys>\n");
    return 2;
  }

  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "打不开文件: %s\n", argv[1]);
    return 2;
  }
  std::stringstream buf;
  buf << in.rdbuf();

  try {
    xmex::Lexer lexer(buf.str(), argv[1]);
    for (const auto &t : lexer.tokenize()) {
      std::printf("%4d:%-3d  %-8s  %s\n", t.line, t.col, xmex::tokName(t.kind),
                  visible(t.text).c_str());
    }
  } catch (const xmex::SourceError &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return 0;
}
