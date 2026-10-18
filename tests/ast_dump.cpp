// 开发用工具:把 .pys 解析成 AST 并打印。
// 语法分析器的错误是最容易写错的地方(优先级、块边界、尾随逗号),
// 把树打出来检查比盯着 parser 代码看来得快。
#include "lexer.h"
#include "parser.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "用法: ast_dump <file.pys>\n");
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
    xmex::Parser parser(lexer.tokenize(), argv[1]);
    auto mod = parser.parseModule();
    mod->dump(std::cout);
  } catch (const xmex::SourceError &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return 0;
}
