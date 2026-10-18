// 输入输出。
//
// print_ 收的是 lite::Value 数组的指针 —— 聚合体本身不跨 ABI 边界按值传,
// 但"指向数组的指针"没问题,元素在内存里按 lite::Value 布局排列。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstdlib>  // strtod
#include <cstring>

namespace {

// 输出的目的地。同一套渲染代码要服务两个调用方:print_ 写 stdout、
// repr 拼成一个字符串。早先它们各自实现一遍的写法在加入列表/字典之后
// 会立刻变成两份要同步维护的递归打印代码,所以这里先抽出这个 sink。
struct Sink {
  std::FILE *f = nullptr;   // 非空则写文件
  char *buf = nullptr;      // 非空则写内存(由 gc_alloc 分配,SCAN_NONE)
  size_t len = 0;
  size_t cap = 0;
};

void sinkPut(Sink &s, const char *p, size_t n) {
  if (s.f) {
    std::fwrite(p, 1, n, s.f);
    return;
  }
  if (s.len + n > s.cap) {
    size_t cap = s.cap ? s.cap : 64;
    while (cap < s.len + n) cap *= 2;
    auto *buf = static_cast<char *>(gc_alloc(cap, lite::kScanNone));
    std::memcpy(buf, s.buf, s.len);
    s.buf = buf;
    s.cap = cap;
  }
  std::memcpy(s.buf + s.len, p, n);
  s.len += n;
}

void sinkPuts(Sink &s, const char *str) { sinkPut(s, str, std::strlen(str)); }
void sinkPutc(Sink &s, char c) { sinkPut(s, &c, 1); }

// 模仿 Python 的浮点显示:用能唯一还原该值的最短表示。
// 直接写 %.17g 会打出 0.10000000000000001 这种噪声,%.6g 又会丢精度,
// 所以从一个很短的精度开始试,直到字符串能精确往返为止。
void printDouble(Sink &s, double d) {
  char buf[64];
  int prec = 1;
  for (; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
    if (std::strtod(buf, nullptr) == d) break;
  }
  // 整数值要显示成 3.0 而不是 3,否则类型看起来像 int
  if (std::strpbrk(buf, ".eEnN") == nullptr) {
    std::strncat(buf, ".0", sizeof(buf) - std::strlen(buf) - 1);
  }
  sinkPuts(s, buf);
}

void writeRepr(Sink &s, lite::Value v);

// 字符串的 repr:带引号并转义。引号的选择与 Python 一致 ——
// 默认单引号,若内容含单引号而不含双引号则改用双引号,少一层转义。
void writeStrRepr(Sink &s, lite::Value v) {
  const char *data = str_data(&v);
  const int64_t n = str_size(&v);

  bool hasSingle = false, hasDouble = false;
  for (int64_t i = 0; i < n; ++i) {
    if (data[i] == '\'') hasSingle = true;
    if (data[i] == '"') hasDouble = true;
  }
  const char quote = (hasSingle && !hasDouble) ? '"' : '\'';

  sinkPutc(s, quote);
  for (int64_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(data[i]);
    if (c == static_cast<unsigned char>(quote) || c == '\\') {
      sinkPutc(s, '\\');
      sinkPutc(s, static_cast<char>(c));
    } else if (c == '\n') {
      sinkPuts(s, "\\n");
    } else if (c == '\r') {
      sinkPuts(s, "\\r");
    } else if (c == '\t') {
      sinkPuts(s, "\\t");
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      sinkPuts(s, buf);
    } else {
      sinkPutc(s, static_cast<char>(c));
    }
  }
  sinkPutc(s, quote);
}

// 容器内部的元素一律用 repr —— 所以 print(["hi"]) 是 ['hi'] 而不是 [hi],
// 而顶层的 print("hi") 不带引号。这正是 Python 的 str/repr 之分。
void writeRepr(Sink &s, lite::Value v) {
  switch (v.tag) {
    case lite::kNull:
      sinkPuts(s, "None");
      break;
    case lite::kBool:
      sinkPuts(s, v.as.i ? "True" : "False");
      break;
    case lite::kInt: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.as.i));
      sinkPuts(s, buf);
      break;
    }
    case lite::kFloat:
      printDouble(s, v.as.f);
      break;
    case lite::kStr:
      writeStrRepr(s, v);
      break;
    case lite::kList: {
      const int64_t n = list_len(&v);
      sinkPutc(s, '[');
      for (int64_t i = 0; i < n; ++i) {
        if (i) sinkPuts(s, ", ");
        writeRepr(s, list_get(&v, i));
      }
      sinkPutc(s, ']');
      break;
    }
    case lite::kTuple: {
      const int64_t n = tuple_len(&v);
      sinkPutc(s, '(');
      for (int64_t i = 0; i < n; ++i) {
        if (i) sinkPuts(s, ", ");
        writeRepr(s, tuple_get(&v, i));
      }
      // 单元素元组要写成 (x,),与 Python 一致
      if (n == 1) sinkPutc(s, ',');
      sinkPutc(s, ')');
      break;
    }
    case lite::kDict: {
      const int64_t n = dict_len(&v);
      sinkPutc(s, '{');
      for (int64_t i = 0; i < n; ++i) {
        if (i) sinkPuts(s, ", ");
        writeRepr(s, dict_key_at(&v, i));
        sinkPuts(s, ": ");
        writeRepr(s, dict_val_at(&v, i));
      }
      sinkPutc(s, '}');
      break;
    }
    default:
      sinkPuts(s, "<");
      sinkPuts(s, tag_name(v.tag));
      sinkPuts(s, ">");
      break;
  }
}

// 顶层用的 str 规则:只有字符串不加引号,其余与 repr 相同。
void writeStr(Sink &s, lite::Value v) {
  if (v.tag == lite::kStr) {
    sinkPut(s, str_data(&v), static_cast<size_t>(str_size(&v)));
    return;
  }
  writeRepr(s, v);
}

// 读一整行。
// Windows 上 stdin 是文本模式,会给出行尾的 \r\n,统一裁掉;
// 读之前先冲 stdout,否则提示语会卡在缓冲区里,用户看不到就被阻塞住。
bool readLine(char *buf, size_t cap) {
  std::fflush(stdout);
  if (std::fgets(buf, static_cast<int>(cap), stdin) == nullptr) return false;
  size_t len = std::strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[--len] = '\0';
  }
  return true;
}

[[noreturn]] void badNumber(const char *what, const char *text) {
  static thread_local char buf[256];
  std::snprintf(buf, sizeof(buf), "输入的不是合法的 %s: '%s'", what, text);
  runtime_error(buf);
}

}  // namespace

// print(a, b, c) —— 以空格分隔,末尾换行。与 Python 的 print 一致。
extern "C" void print_(lite::Value *vals, int64_t n) {
  Sink s;
  s.f = stdout;
  for (int64_t i = 0; i < n; ++i) {
    if (i) sinkPutc(s, ' ');
    writeStr(s, vals[i]);
  }
  sinkPutc(s, '\n');
}

// 按 repr 规则渲染成新字符串。字典查不到键时的报错信息要用它,
// 这样 `d["a"]` 报的是 字典里没有键 'a' 而不是一个裸指针值。
extern "C" lite::Value repr(const lite::Value *v) {
  Sink s;
  writeRepr(s, *v);
  return str_new(s.buf, static_cast<int64_t>(s.len));
}

// ---------------------------------------------------------------------------
// input(...)
//
// input(int, int) 在 IRGen 里被拆成若干个这里的具体读取函数,
// 结果再打包成元组交给多目标赋值去解包。
// ---------------------------------------------------------------------------

extern "C" lite::Value read_int() {
  char buf[256];
  if (!readLine(buf, sizeof(buf))) runtime_error("读取输入失败(输入已结束)");
  char *end = nullptr;
  const long long v = std::strtoll(buf, &end, 10);
  if (end == buf) badNumber("整数", buf);
  return int_(v);
}

extern "C" lite::Value read_float() {
  char buf[256];
  if (!readLine(buf, sizeof(buf))) runtime_error("读取输入失败(输入已结束)");
  char *end = nullptr;
  const double v = std::strtod(buf, &end);
  if (end == buf) badNumber("浮点数", buf);
  return float_(v);
}

extern "C" lite::Value read_bool() {
  char buf[256];
  if (!readLine(buf, sizeof(buf))) runtime_error("读取输入失败(输入已结束)");
  if (std::strcmp(buf, "True") == 0 || std::strcmp(buf, "true") == 0 ||
      std::strcmp(buf, "1") == 0) {
    return bool_(true);
  }
  if (std::strcmp(buf, "False") == 0 || std::strcmp(buf, "false") == 0 ||
      std::strcmp(buf, "0") == 0) {
    return bool_(false);
  }
  badNumber("布尔值(应为 True/False)", buf);
}

extern "C" lite::Value read_str() {
  char buf[1024];
  if (!readLine(buf, sizeof(buf))) runtime_error("读取输入失败(输入已结束)");
  return str_new(buf, static_cast<int64_t>(std::strlen(buf)));
}
