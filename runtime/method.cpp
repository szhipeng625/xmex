// 方法调用:obj.method(args) 统一走 call_method。
//
// 为什么把方法表放在运行时、按名字字符串分派,而不是在 IRGen 里按静态类型
// 解析成直接调用:
//
//   * 本项目的类型注解**不做检查**(docs/language.md),变量没有静态类型,
//     `x.upper()` 里的 x 到编译期根本不知道是什么。Python 自己也是运行时查找。
//   * 代价是拼错方法名变成**运行时**错误而不是编译期错误。这一点在
//     docs/language.md 里写明了,不是遗漏。
//
// 三个 tag 各有一张表,用 string_view 比名字。方法集合刻意只取常用子集,
// 文档里列了清单;表里没有的一律给出"XX 没有方法 'yy'"的明确报错。
#include "xmex/runtime.h"
#include <map>
#include <mutex>
#include <string>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

std::string_view nm(const char *p, int64_t n) {
  return std::string_view(p, static_cast<size_t>(n));
}

bool isSpace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

[[noreturn]] void noSuchMethod(const xmex::Value *obj, std::string_view name) {
  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%s' 没有方法 '%.*s'", tag_name(obj->tag),
                static_cast<int>(name.size()), name.data());
  runtime_error(buf);
}

// 参数个数检查。min == max 时只报一个数字,别写成 "需要 1..1 个"。
void needArgs(std::string_view method, int64_t got, int64_t min, int64_t max) {
  if (got >= min && got <= max) return;
  static thread_local char buf[192];
  if (min == max) {
    std::snprintf(buf, sizeof(buf), "'%.*s' 需要 %lld 个参数,给了 %lld 个",
                  static_cast<int>(method.size()), method.data(),
                  static_cast<long long>(min), static_cast<long long>(got));
  } else {
    std::snprintf(buf, sizeof(buf),
                  "'%.*s' 需要 %lld 到 %lld 个参数,给了 %lld 个",
                  static_cast<int>(method.size()), method.data(),
                  static_cast<long long>(min), static_cast<long long>(max),
                  static_cast<long long>(got));
  }
  runtime_error(buf);
}

void needStrArg(std::string_view method, const xmex::Value &v, const char *what) {
  if (v.tag == xmex::kStr) return;
  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%.*s' 的%s必须是字符串,实际得到 '%s'",
                static_cast<int>(method.size()), method.data(), what,
                tag_name(v.tag));
  runtime_error(buf);
}

void needListArg(std::string_view method, const xmex::Value &v, const char *what) {
  if (v.tag == xmex::kList) return;
  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%.*s' 的%s必须是列表,实际得到 '%s'",
                static_cast<int>(method.size()), method.data(), what,
                tag_name(v.tag));
  runtime_error(buf);
}

void needDictArg(std::string_view method, const xmex::Value &v) {
  if (v.tag == xmex::kDict) return;
  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%.*s' 的参数必须是字典,实际得到 '%s'",
                static_cast<int>(method.size()), method.data(),
                tag_name(v.tag));
  runtime_error(buf);
}

// ---------------------------------------------------------------------------
// 字符串方法
// ---------------------------------------------------------------------------

xmex::Value strUpper(const xmex::Value &s) {
  const int64_t n = str_size(&s);
  const char *data = str_data(&s);
  // ⚠️ 显式根:buf 要在随后的 str_new 分配期间活下来
  // (它只被这个局部持有,没有任何容器指得到它)
  char *buf = nullptr;
  gc_root_push(reinterpret_cast<void **>(&buf));
  buf = static_cast<char *>(
      gc_alloc(static_cast<size_t>(n) + 1, xmex::kScanNone));
  for (int64_t i = 0; i < n; ++i) {
    const char c = data[i];
    buf[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  // 先把结果串造出来(这一步可能触发回收,buf 仍被根保护),再退根。
  // 反过来先退根的话,根就白加了。
  xmex::Value out = str_new(buf, n);
  gc_root_pop(1);
  return out;
}

xmex::Value strLower(const xmex::Value &s) {
  const int64_t n = str_size(&s);
  const char *data = str_data(&s);
  // ⚠️ 显式根:buf 要在随后的 str_new 分配期间活下来
  // (它只被这个局部持有,没有任何容器指得到它)
  char *buf = nullptr;
  gc_root_push(reinterpret_cast<void **>(&buf));
  buf = static_cast<char *>(
      gc_alloc(static_cast<size_t>(n) + 1, xmex::kScanNone));
  for (int64_t i = 0; i < n; ++i) {
    const char c = data[i];
    buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  // 先把结果串造出来(这一步可能触发回收,buf 仍被根保护),再退根。
  // 反过来先退根的话,根就白加了。
  xmex::Value out = str_new(buf, n);
  gc_root_pop(1);
  return out;
}

// strip() 去空白,strip(chars) 去掉两端出现在 chars 里的任意字符
xmex::Value strStrip(const xmex::Value &s, const xmex::Value *chars) {
  const int64_t n = str_size(&s);
  const char *data = str_data(&s);

  const char *set = nullptr;
  int64_t setLen = 0;
  if (chars) {
    set = str_data(chars);
    setLen = str_size(chars);
  }

  auto inSet = [&](char c) {
    if (!set) return isSpace(static_cast<unsigned char>(c));
    for (int64_t i = 0; i < setLen; ++i) {
      if (set[i] == c) return true;
    }
    return false;
  };

  int64_t lo = 0, hi = n;
  while (lo < hi && inSet(data[lo])) ++lo;
  while (hi > lo && inSet(data[hi - 1])) --hi;
  return str_new(data + lo, hi - lo);
}

// 在 sep 的每次出现处切开。sep 为空串是 Python 里的 ValueError,这里直接报错;
// 空串作为分隔符在我们这套 C 实现的循环里会死循环,不拦下来很危险。
xmex::Value strSplit(const xmex::Value &s, const xmex::Value *sep) {
  const char *data = str_data(&s);
  const int64_t n = str_size(&s);

  xmex::Value out = list_new(nullptr, 0);

  if (!sep) {
    // 不给分隔符:按空白切,并丢掉空片段(与 Python 一致)
    int64_t i = 0;
    while (i < n) {
      while (i < n && isSpace(static_cast<unsigned char>(data[i]))) ++i;
      if (i >= n) break;
      const int64_t start = i;
      while (i < n && !isSpace(static_cast<unsigned char>(data[i]))) ++i;
      xmex::Value piece = str_new(data + start, i - start);
      list_append(&out, &piece);
    }
    return out;
  }

  const char *sd = str_data(sep);
  const int64_t sn = str_size(sep);
  if (sn == 0) runtime_error("split() 的分隔符不能是空字符串");

  int64_t start = 0;
  for (int64_t i = 0; i + sn <= n;) {
    if (std::memcmp(data + i, sd, static_cast<size_t>(sn)) == 0) {
      xmex::Value piece = str_new(data + start, i - start);
      list_append(&out, &piece);
      i += sn;
      start = i;
    } else {
      ++i;
    }
  }
  xmex::Value tail = str_new(data + start, n - start);
  list_append(&out, &tail);
  return out;
}

xmex::Value strJoin(const xmex::Value &sep, const xmex::Value &items) {
  needListArg("join", items, "参数");
  const int64_t n = list_len(&items);

  // 先量一遍总长,一次分配,避免逐个 concat 造成 O(n²) 的拷贝
  int64_t total = 0;
  for (int64_t i = 0; i < n; ++i) {
    xmex::Value e = list_get(&items, i);
    needStrArg("join", e, "列表元素");
    total += str_size(&e);
  }
  const int64_t sepLen = str_size(&sep);
  if (n > 1) total += sepLen * (n - 1);

  // ⚠️ 显式根:buf 要在随后的 str_new 分配期间活下来
  // (它只被这个局部持有,没有任何容器指得到它)
  char *buf = nullptr;
  gc_root_push(reinterpret_cast<void **>(&buf));
  buf = static_cast<char *>(
      gc_alloc(static_cast<size_t>(total) + 1, xmex::kScanNone));
  int64_t at = 0;
  for (int64_t i = 0; i < n; ++i) {
    if (i) {
      std::memcpy(buf + at, str_data(&sep), static_cast<size_t>(sepLen));
      at += sepLen;
    }
    xmex::Value e = list_get(&items, i);
    const int64_t en = str_size(&e);
    std::memcpy(buf + at, str_data(&e), static_cast<size_t>(en));
    at += en;
  }
  // 先把结果串造出来(这一步可能触发回收,buf 仍被根保护),再退根。
  // 反过来先退根的话,根就白加了。
  xmex::Value out = str_new(buf, total);
  gc_root_pop(1);
  return out;
}

xmex::Value strReplace(const xmex::Value &s, const xmex::Value &oldS, const xmex::Value &newS) {
  const char *data = str_data(&s);
  const int64_t n = str_size(&s);
  const char *od = str_data(&oldS);
  const int64_t on = str_size(&oldS);
  const char *nd = str_data(&newS);
  const int64_t nn = str_size(&newS);

  // Python 里 "abc".replace("", "x") 是在每个字符前后插入,规则很绕。
  // 这里把空旧串当成"没什么可替换"直接返回原串 —— 明确、可预期,
  // 也在 docs/language.md 里注明了与 Python 的差异。
  if (on == 0) return str_new(data, n);

  const int64_t cap = n + (nn > on ? (nn - on) * n : 0) + 1;
  // ⚠️ 显式根:buf 要在随后的 str_new 分配期间活下来
  // (它只被这个局部持有,没有任何容器指得到它)
  char *buf = nullptr;
  gc_root_push(reinterpret_cast<void **>(&buf));
  buf = static_cast<char *>(
      gc_alloc(static_cast<size_t>(cap) + 1, xmex::kScanNone));
  int64_t at = 0;
  for (int64_t i = 0; i < n;) {
    if (i + on <= n && std::memcmp(data + i, od, static_cast<size_t>(on)) == 0) {
      std::memcpy(buf + at, nd, static_cast<size_t>(nn));
      at += nn;
      i += on;
    } else {
      buf[at++] = data[i++];
    }
  }
  // 先把结果串造出来(这一步可能触发回收,buf 仍被根保护),再退根。
  // 反过来先退根的话,根就白加了。
  xmex::Value out = str_new(buf, at);
  gc_root_pop(1);
  return out;
}

xmex::Value strCount(const xmex::Value &s, const xmex::Value &sub) {
  const char *data = str_data(&s);
  const int64_t n = str_size(&s);
  const char *sd = str_data(&sub);
  const int64_t sn = str_size(&sub);
  if (sn == 0) runtime_error("count(): 子串不能为空");

  int64_t cnt = 0;
  for (int64_t i = 0; i + sn <= n;) {
    if (std::memcmp(data + i, sd, static_cast<size_t>(sn)) == 0) {
      ++cnt;
      i += sn;  // 不重叠地往后走,与 Python 一致
    } else {
      ++i;
    }
  }
  return int_(cnt);
}

// ---------------------------------------------------------------------------
// 列表方法
// ---------------------------------------------------------------------------

xmex::Value listPop(const xmex::Value &l, const xmex::Value *idx) {
  xmex::Value none = none_();
  return list_pop(&l, idx ? idx : &none);
}

xmex::Value dictGet(const xmex::Value &d, const xmex::Value &key, const xmex::Value *def) {
  if (def) return dict_get_default(&d, &key, def);
  xmex::Value none = none_();
  return dict_get_default(&d, &key, &none);
}

// keys()/values()/items() 都由"按下标取第 i 项"拼出来
xmex::Value dictProjection(const xmex::Value &d, bool wantKeys, bool wantVals) {
  const int64_t n = dict_len(&d);
  // 元素数组里装的是键/值/新元组 —— 标成 SCAN_NONE 会把它们全放死。
  // 这里是 method.cpp 里唯一需要扫描的缓冲,其余 4 处 char 缓冲都是 SCAN_NONE。
  //
  // ⚠️ 显式根:`items` 里已经填好的那些元组/字符串,在下一轮 tuple_new
  // 触发回收时只被这个局部数组引用着 —— 不登记就会被收掉。
  xmex::Value *items = nullptr;
  gc_root_push(reinterpret_cast<void **>(&items));
  items = n > 0 ? static_cast<xmex::Value *>(
                      gc_alloc_values(static_cast<size_t>(n) * sizeof(xmex::Value), 0))
                : nullptr;

  for (int64_t i = 0; i < n; ++i) {
    if (wantKeys && wantVals) {
      xmex::Value pair[2] = {dict_key_at(&d, i), dict_val_at(&d, i)};
      items[i] = tuple_new(pair, 2);
    } else if (wantKeys) {
      items[i] = dict_key_at(&d, i);
    } else {
      items[i] = dict_val_at(&d, i);
    }
  }

  xmex::Value out = list_new(items, n);   // 此时元素已经在结果列表里了
  gc_root_pop(1);
  return out;
}

}  // namespace

// ---- 实例方法类型化注册表(迭代协议等实例方法的分派入口) ----
// 按 (类型名, 方法名) 注册处理函数。迭代器等内置实例在创建处注册方法,
// call_method 遇到 xmex::kInstance 时按实例的 type 属性查表分派。
using InstanceMethodFn = xmex::Value (*)(const xmex::Value *inst, xmex::Value *args, int64_t nargs);
static std::map<std::string, InstanceMethodFn> g_instance_methods;
static std::mutex g_instance_methods_mutex;

extern "C" void instance_register_method(const char *type_name, int64_t typeLen,
                                               const char *method, int64_t mLen,
                                               InstanceMethodFn fn) {
    std::lock_guard<std::mutex> lk(g_instance_methods_mutex);
    std::string key = std::string(type_name, typeLen) + "::" + std::string(method, mLen);
    g_instance_methods[key] = fn;
}

// 取实例的 type 属性(无则返回空)
static std::string instanceTypeOf(const xmex::Value *inst) {
    xmex::Value k = str_new("type", 4);
    if (inst->tag != xmex::kInstance) return "";
    struct InstView { int64_t classId; xmex::Value attrs; };
    auto *iv = reinterpret_cast<InstView *>(inst->as.ptr);
    if (!dict_has(&iv->attrs, &k)) return "";
    xmex::Value t = dict_get(&iv->attrs, &k);
    if (t.tag != xmex::kStr) return "";
    return std::string(str_data(&t), str_size(&t));
}

extern "C" xmex::Value call_method(const xmex::Value *obj, const char *name,
                                  int64_t nameLen, xmex::Value *args, int64_t nargs) {
  const std::string_view m = nm(name, nameLen);

  // ---- 实例对象:查类型化方法注册表 ----
  if (obj->tag == xmex::kInstance) {
    std::string tname = instanceTypeOf(obj);
    std::string key = tname + "::" + std::string(m);
    InstanceMethodFn fn = nullptr;
    {
      std::lock_guard<std::mutex> lk(g_instance_methods_mutex);
      auto it = g_instance_methods.find(key);
      if (it != g_instance_methods.end()) fn = it->second;
    }
    if (fn) return fn(obj, args, nargs);
    static thread_local char ibuf[192];
    std::snprintf(ibuf, sizeof(ibuf), "'%s' 实例没有方法 '%.*s'",
                  tname.c_str(), static_cast<int>(nameLen), name);
    runtime_error(ibuf);
  }

  // 便于书写:下标访问比 args[0] 清楚
  auto arg = [&](int64_t i) -> const xmex::Value & { return args[i]; };

  // ------------------------------------------------------------------ str
  if (obj->tag == xmex::kStr) {
    if (m == "upper") {
      needArgs(m, nargs, 0, 0);
      return strUpper(*obj);
    }
    if (m == "lower") {
      needArgs(m, nargs, 0, 0);
      return strLower(*obj);
    }
    if (m == "strip") {
      needArgs(m, nargs, 0, 1);
      if (nargs == 1) needStrArg(m, arg(0), "参数");
      return strStrip(*obj, nargs == 1 ? &arg(0) : nullptr);
    }
    if (m == "split") {
      needArgs(m, nargs, 0, 1);
      if (nargs == 1) needStrArg(m, arg(0), "分隔符");
      return strSplit(*obj, nargs == 1 ? &arg(0) : nullptr);
    }
    if (m == "join") {
      needArgs(m, nargs, 1, 1);
      return strJoin(*obj, arg(0));
    }
    if (m == "startswith") {
      needArgs(m, nargs, 1, 1);
      needStrArg(m, arg(0), "参数");
      return bool_(str_starts_with(obj, &arg(0)) != 0);
    }
    if (m == "endswith") {
      needArgs(m, nargs, 1, 1);
      needStrArg(m, arg(0), "参数");
      return bool_(str_ends_with(obj, &arg(0)) != 0);
    }
    if (m == "replace") {
      needArgs(m, nargs, 2, 2);
      needStrArg(m, arg(0), "第一个参数");
      needStrArg(m, arg(1), "第二个参数");
      return strReplace(*obj, arg(0), arg(1));
    }
    if (m == "find") {
      needArgs(m, nargs, 1, 2);
      needStrArg(m, arg(0), "参数");
      const int64_t from = nargs == 2 ? to_int(arg(1)) : 0;
      return int_(str_find(obj, &arg(0), from));
    }
    if (m == "count") {
      needArgs(m, nargs, 1, 1);
      needStrArg(m, arg(0), "参数");
      return strCount(*obj, arg(0));
    }
    noSuchMethod(obj, m);
  }

  // ----------------------------------------------------------------- list
  if (obj->tag == xmex::kList) {
    if (m == "append") {
      needArgs(m, nargs, 1, 1);
      list_append(obj, &arg(0));
      return none_();
    }
    if (m == "extend") {
      needArgs(m, nargs, 1, 1);
      needListArg(m, arg(0), "参数");
      list_extend(obj, &arg(0));
      return none_();
    }
    if (m == "insert") {
      needArgs(m, nargs, 2, 2);
      list_insert(obj, to_int(arg(0)), &arg(1));
      return none_();
    }
    if (m == "remove") {
      needArgs(m, nargs, 1, 1);
      list_remove(obj, &arg(0));
      return none_();
    }
    if (m == "pop") {
      needArgs(m, nargs, 0, 1);
      return listPop(*obj, nargs == 1 ? &arg(0) : nullptr);
    }
    if (m == "index") {
      needArgs(m, nargs, 1, 1);
      return int_(list_index(obj, &arg(0)));
    }
    if (m == "count") {
      needArgs(m, nargs, 1, 1);
      return int_(list_count(obj, &arg(0)));
    }
    if (m == "reverse") {
      needArgs(m, nargs, 0, 0);
      list_reverse(obj);
      return none_();
    }
    if (m == "sort") {
      needArgs(m, nargs, 0, 0);
      list_sort(obj);
      return none_();
    }
    if (m == "clear") {
      needArgs(m, nargs, 0, 0);
      list_clear(obj);
      return none_();
    }
    noSuchMethod(obj, m);
  }

  // ----------------------------------------------------------------- dict
  if (obj->tag == xmex::kDict) {
    if (m == "get") {
      needArgs(m, nargs, 1, 2);
      return dictGet(*obj, arg(0), nargs == 2 ? &arg(1) : nullptr);
    }
    if (m == "keys") {
      needArgs(m, nargs, 0, 0);
      return dictProjection(*obj, true, false);
    }
    if (m == "values") {
      needArgs(m, nargs, 0, 0);
      return dictProjection(*obj, false, true);
    }
    if (m == "items") {
      needArgs(m, nargs, 0, 0);
      return dictProjection(*obj, true, true);
    }
    if (m == "pop") {
      needArgs(m, nargs, 1, 1);
      return dict_pop(obj, &arg(0));
    }
    if (m == "update") {
      needArgs(m, nargs, 1, 1);
      needDictArg(m, arg(0));
      const int64_t n = dict_len(&arg(0));
      for (int64_t i = 0; i < n; ++i) {
        xmex::Value k = dict_key_at(&arg(0), i);
        xmex::Value v = dict_val_at(&arg(0), i);
        dict_set(obj, &k, &v);
      }
      return none_();
    }
    if (m == "clear") {
      needArgs(m, nargs, 0, 0);
      dict_clear(obj);
      return none_();
    }
    noSuchMethod(obj, m);
  }

  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%s' 类型的值没有方法(方法调用只支持 str/list/dict)",
                tag_name(obj->tag));
  runtime_error(buf);
}
