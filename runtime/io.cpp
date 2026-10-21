// 输入输出。
//
// print_ 收的是 xmex::Value 数组的指针 —— 聚合体本身不跨 ABI 边界按值传,
// 但"指向数组的指针"没问题,元素在内存里按 xmex::Value 布局排列。
#include "xmex/runtime.h"

#include "xmex/align.h"

#include <cstdio>
#include <cstdlib>  // strtod
#include <cstring>
#include <memory>   // unique_ptr / make_unique

namespace {

// 输出的目的地。同一套渲染代码要服务两个调用方:print_ 写 stdout、
// repr 拼成一个字符串。早先它们各自实现一遍的写法在加入列表/字典之后
// 会立刻变成两份要同步维护的递归打印代码,所以这里先抽出这个 sink。
//
// ⚠️ 两条路径的 IO 策略完全不同,不能混:
//
//   文件路径(写 stdout / 磁盘文件):
//     早先的实现是 sinkPutc 一次 fwrite 写 **1 个字节**。渲染一个
//     repr 可能要写上千个字符,也就是上千次 fwrite 调用 —— 每次调用
//     都有函数调用开销和 stdio 缓冲状态维护。
//     现在改成 xmex::BatchWriter:先在内存里拼,攒够一批才 fwrite 一次。
//     写调用次数从 O(字符数) 降到 O(字符数 / 批大小)。
//
//   内存路径(repr 拼字符串):
//     早先用 gc_alloc 每次扩容都重新分配并拷贝一遍,而旧块进了 GC
//     的垃圾列表 —— 渲染一次 repr 会制造 O(log n) 个 GC 垃圾块。
//     现在用 xmex::AlignedBuffer:按 2 的幂增长、对齐分配、出了作用域
//     直接释放,完全不惊动 GC。最后 str_new 会拷贝一份,原缓冲随之消失。
struct Sink {
  // 内存路径的缓冲。
  xmex::AlignedBuffer buf;

  // 写文件路径的批量写器。为空则走内存路径。
  xmex::BatchWriter *batch = nullptr;
};

// stdout 的批量写器。
//
// ⚠️ 必须是**复用**的,不能每次 print_ 新建一个。
//    BatchWriter 内部有 64KB 的对齐缓冲;每次 print_ 新建就意味着
//    每次 print_ 都要 malloc + free 64KB —— 那比省下的 fwrite 调用
//    贵得多,实测端到端反而更慢。
//    thread_local 是因为 print_ 可能在不同线程被调用(服务端场景),
//    而 stdout 本身也不是线程安全的。
xmex::BatchWriter &stdoutWriter() {
  static thread_local xmex::BatchWriter writer(stdout);
  return writer;
}

void sinkPut(Sink &s, const char *p, size_t n) {
  if (n == 0) return;
  if (s.batch != nullptr) {
    s.batch->write(p, n);
    return;
  }
  s.buf.append(p, n);
}

void sinkPuts(Sink &s, const char *str) { sinkPut(s, str, std::strlen(str)); }
void sinkPutc(Sink &s, char c) { sinkPut(s, &c, 1); }

// 模仿 Python 的浮点显示:用能唯一还原该值的最短表示。
// 直接写 %.17g 会打出 0.10000000000000001 这种噪声,%.6g 又会丢精度,
// 所以要找一个刚好能精确往返的精度。
//
// ⚠️ 试探的档位是 15 → 16 → 17,而不是 1 → 17。
//    一个 double 的最短往返表示几乎总是 15~17 位有效数字:
//      1/3      → "0.3333333333333333"  (16 位)
//      0.1      → "0.1"                 (15 位就够)
//      0.1+0.2  → "0.30000000000000004"(17 位)
//    从 1 往上试要跑 15~17 次 snprintf 才命中;从 17 往下试则**永远**
//    停在 17 位(17 位必定往返),输出全是长噪声。
//    只试这三档,最多 3 次 snprintf,输出与 Python 的 repr 一致。
void printDouble(Sink &s, double d) {
  char buf[64];
  int prec = 17;
  for (int p : {15, 16, 17}) {
    std::snprintf(buf, sizeof(buf), "%.*g", p, d);
    if (std::strtod(buf, nullptr) == d) {
      prec = p;
      break;
    }
  }
  if (prec == 17) {
    // 三档都没命中(理论上 17 位必定往返,这里只是防御)。
    std::snprintf(buf, sizeof(buf), "%.17g", d);
  }
  // 整数值要显示成 3.0 而不是 3,否则类型看起来像 int
  if (std::strpbrk(buf, ".eEnN") == nullptr) {
    std::strncat(buf, ".0", sizeof(buf) - std::strlen(buf) - 1);
  }
  sinkPuts(s, buf);
}

void writeRepr(Sink &s, xmex::Value v);

// 字符串的 repr:带引号并转义。引号的选择与 Python 一致 ——
// 默认单引号,若内容含单引号而不含双引号则改用双引号,少一层转义。
void writeStrRepr(Sink &s, xmex::Value v) {
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
void writeRepr(Sink &s, xmex::Value v) {
  switch (v.tag) {
    case xmex::kNull:
      sinkPuts(s, "None");
      break;
    case xmex::kBool:
      sinkPuts(s, v.as.i ? "True" : "False");
      break;
    case xmex::kInt: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.as.i));
      sinkPuts(s, buf);
      break;
    }
    case xmex::kFloat:
      printDouble(s, v.as.f);
      break;
    case xmex::kStr:
      writeStrRepr(s, v);
      break;
    case xmex::kList: {
      const int64_t n = list_len(&v);
      sinkPutc(s, '[');
      for (int64_t i = 0; i < n; ++i) {
        if (i) sinkPuts(s, ", ");
        writeRepr(s, list_get(&v, i));
      }
      sinkPutc(s, ']');
      break;
    }
    case xmex::kTuple: {
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
    case xmex::kDict: {
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
void writeStr(Sink &s, xmex::Value v) {
  if (v.tag == xmex::kStr) {
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
extern "C" void print_(xmex::Value *vals, int64_t n) {
  Sink s;
  s.batch = &stdoutWriter();
  for (int64_t i = 0; i < n; ++i) {
    if (i) sinkPutc(s, ' ');
    writeStr(s, vals[i]);
  }
  sinkPutc(s, '\n');
  // 落盘:整条 print 只产生一次 fwrite,而不是每个字符一次。
  // 批量写器是复用的,这里 flush 的只是它内部那块 64KB 缓冲的内容。
  s.batch->flush();
}

// 按 repr 规则渲染成新字符串。字典查不到键时的报错信息要用它,
// 这样 `d["a"]` 报的是 字典里没有键 'a' 而不是一个裸指针值。
extern "C" xmex::Value repr(const xmex::Value *v) {
  Sink s;
  writeRepr(s, *v);
  // s.buf 是对齐的堆内存,str_new 会拷走一份;拷完这块内存随 Sink 析构释放,
  // 不进 GC —— 渲染一次 repr 不再制造 GC 垃圾块。
  return str_new(s.buf.data(), static_cast<int64_t>(s.buf.size()));
}

// ---------------------------------------------------------------------------
// input(...)
//
// input(int, int) 在 IRGen 里被拆成若干个这里的具体读取函数,
// 结果再打包成元组交给多目标赋值去解包。
// ---------------------------------------------------------------------------

extern "C" xmex::Value read_int() {
  char buf[256];
  if (!readLine(buf, sizeof(buf))) runtime_error("读取输入失败(输入已结束)");
  char *end = nullptr;
  const long long v = std::strtoll(buf, &end, 10);
  if (end == buf) badNumber("整数", buf);
  return int_(v);
}

extern "C" xmex::Value read_float() {
  char buf[256];
  if (!readLine(buf, sizeof(buf))) runtime_error("读取输入失败(输入已结束)");
  char *end = nullptr;
  const double v = std::strtod(buf, &end);
  if (end == buf) badNumber("浮点数", buf);
  return float_(v);
}

extern "C" xmex::Value read_bool() {
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

extern "C" xmex::Value read_str() {
  char buf[1024];
  if (!readLine(buf, sizeof(buf))) runtime_error("读取输入失败(输入已结束)");
  return str_new(buf, static_cast<int64_t>(std::strlen(buf)));
}
