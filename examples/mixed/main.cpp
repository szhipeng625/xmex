// C++ 侧 —— 宿主程序。
//
// 这个文件**完全不知道 PyLite 的语法**,它只是调用几个 extern "C" 函数,
// 而那些函数的实现由 scores.pys 编译而来。这正是本项目想要的形态:
// 用 C++ 写底层与 I/O,用 PyLite 写业务逻辑,两者在编译期链接成一个可执行文件。
//
// 运行期没有任何解释器、没有脚本文件需要分发 —— scores.pys 已经被编进了
// main.exe 里。
#include "pylite/runtime.h"

#include <cstdio>

// ---------------------------------------------------------------------------
// pylitec 为 scores.pys 生成的符号。
//
// 命名规则:pylite_<模块名>_<函数名>,模块名取输入文件的主文件名。
// 顶层语句进 pylite_scores_main()。
//
// ⚠️ ABI:所有 lite::Value 都**按指针**收发,返回值写在第一个参数里(隐藏的 sret)。
// 这是 Win64 规定的 —— 大于 8 字节的聚合体按引用传递。写成按值形式
// (`lite::Value f(lite::Value, lite::Value)`)会链接通过、一跑就段错误,原因见
// docs/llvm-notes.md 第 2 节。
// ---------------------------------------------------------------------------
extern "C" void pylite_scores_main();

// def clamp_all(xs, lo, hi)  →  3 个参数
extern "C" void pylite_scores_clamp_all(lite::Value *ret, lite::Value *xs, lite::Value *lo,
                                        lite::Value *hi);

// def total_and_count(xs)    →  返回元组
extern "C" void pylite_scores_total_and_count(lite::Value *ret, lite::Value *xs);

// def grade(name, score)     →  字符串进、字符串出
extern "C" void pylite_scores_grade(lite::Value *ret, lite::Value *name, lite::Value *score);

// def average(total, count)  →  返回浮点
extern "C" void pylite_scores_average(lite::Value *ret, lite::Value *total,
                                      lite::Value *count);

namespace {

// 把单个 lite::Value 打到 stdout。print_ 收的是数组指针,所以套个 1 元数组。
void show(const char *label, lite::Value v) {
  std::printf("%-22s ", label);
  lite::Value one[1] = {v};
  print_(one, 1);
}

// 造一个 lite::Value 的字符串。注意要显式给长度 —— 字符串里可能有 '\0'。
lite::Value str(const char *s, int64_t len) { return str_new(s, len); }

}  // namespace

int main() {
  std::printf("=== C++ 侧开始 ===\n\n");

  // ---------------------------------------------------------------------
  // 1. 跑 PyLite 的顶层语句。顶层语句被编译成 pylite_scores_main()。
  // ---------------------------------------------------------------------
  pylite_scores_main();

  // ---------------------------------------------------------------------
  // 2. C++ 造数据 —— 一个装着原始分数的列表,故意混入越界值。
  //
  //    lite::Value 都住在栈上的局部变量里,这一点很重要:垃圾回收是靠扫描
  //    当前线程的栈来找活对象的,**全局/静态变量里的 lite::Value 不算根**,
  //    指向的对象会被回收掉(见 docs/language.md)。
  // ---------------------------------------------------------------------
  lite::Value raw[6] = {int_(55),  int_(120), int_(88),
                    int_(-10), int_(93),  int_(61)};
  lite::Value xs = list_new(raw, 6);
  show("原始分数:", xs);

  // ---------------------------------------------------------------------
  // 3. 交给 PyLite 处理:把越界值夹到 [0, 100]。
  //    返回值写在 &clamped 里。
  // ---------------------------------------------------------------------
  lite::Value lo = int_(0);
  lite::Value hi = int_(100);
  lite::Value clamped;
  pylite_scores_clamp_all(&clamped, &xs, &lo, &hi);
  show("PyLite 夹取后:", clamped);
  // clamp_all 里是 `out = []` 再 append,建的是**新列表** —— 原来的没被就地改。
  // (对比 list.sort() / append() 那种就地修改的方法,那些会改到同一个对象。)
  show("原始列表(未改动):", xs);

  // ---------------------------------------------------------------------
  // 4. 多返回值:PyLite 返回元组,C++ 用 unpack 拆开。
  // ---------------------------------------------------------------------
  lite::Value summary;
  pylite_scores_total_and_count(&summary, &clamped);

  lite::Value parts[2];
  unpack(&summary, parts, 2);
  const int64_t total = as_int(parts[0]);
  const int64_t count = as_int(parts[1]);
  std::printf("PyLite 返回的元组:      total=%lld count=%lld\n",
              static_cast<long long>(total), static_cast<long long>(count));

  // ---------------------------------------------------------------------
  // 5. 把 C++ 算出来的值再喂回 PyLite,换回一个浮点。
  // ---------------------------------------------------------------------
  lite::Value totalV = int_(total);
  lite::Value countV = int_(count);
  lite::Value avg;
  pylite_scores_average(&avg, &totalV, &countV);
  std::printf("平均分(float):          %.4f\n", as_float(avg));

  // ---------------------------------------------------------------------
  // 6. 字符串进出:名字传进去,等第串传回来。
  // ---------------------------------------------------------------------
  const char *names[3] = {"alice", "bob", "carol"};
  const int64_t nameLens[3] = {5, 3, 5};
  const int64_t scores[3] = {93, 61, 55};

  std::printf("\n");
  for (int i = 0; i < 3; ++i) {
    lite::Value name = str(names[i], nameLens[i]);
    lite::Value score = int_(scores[i]);
    lite::Value line;
    pylite_scores_grade(&line, &name, &score);

    // 取回字符串:str_data 给指针、str_size 给长度。
    // 长度必须用 str_size,不能靠 strlen —— 字符串里可能有 '\0'。
    std::printf("  %s(%lld) → ", names[i], static_cast<long long>(scores[i]));
    std::fwrite(str_data(&line), 1, static_cast<size_t>(str_size(&line)),
                stdout);
    std::fputc('\n', stdout);
  }

  std::printf("\n=== C++ 侧结束 ===\n");
  return 0;
}
