// 算术、比较与逻辑运算。
//
// 全部走 C ABI、收发 lite::Value。数值类型提升规则与 Python 一致:
// int 与 float 混算时提升为 float。
//
// 除了数值,这里还负责三件容易做错的事:
//   * `+` 与 `*` 在字符串/列表上的拼接与重复(容器部分转发给 string.cpp /
//     list.cpp)
//   * `==` 的**按值**比较 —— 字符串按字节、容器逐元素递归。曾经这里走到
//     default 分支比较 payload(对字符串就是比指针),于是 "a" == "a" 是 False
//   * `+=` 的独立入口 iadd:列表要就地改,字符串/数值要新分配,两者语义
//     不同(见 iadd 的说明)
//
// 另外 truthy 对容器按元素个数判空 —— 早先"非空指针即真",空列表/空串
// 会被误判为真。
//
// 注意:C++ 侧的签名是按值的 `lite::Value f(lite::Value, lite::Value)`,而 IRGen 在 IR 里
// 把它声明成 `void @f(ptr sret(%lite::Value), ptr, ptr)`。两者在机器层面是一致的 ——
// Win64 ABI 规定 16 字节聚合体按引用传递,返回值走隐藏 sret 指针。
// tests/jit_spike.cpp 与 tests/abi_test.cpp 就是为了钉死这个约定。
#include "pylite/runtime.h"

#include <cmath>
#include <cstdio>

namespace {

  bool isNumeric(int32_t t) { return t == lite::kInt || t == lite::kFloat || t == lite::kBool; }

  // 能当重复次数的类型。Python 的 "ab" * True 是合法的(得 "ab"),
  // 所以 bool 也算。
  bool isCountLike(int32_t t) { return t == lite::kInt || t == lite::kBool; }

  bool wantFloat(lite::Value a, lite::Value b) {
    return a.tag == lite::kFloat || b.tag == lite::kFloat;
  }

  double toDouble(lite::Value v) {
    return v.tag == lite::kFloat ? v.as.f : static_cast<double>(v.as.i);
  }

  [[noreturn]] void badOperands(const char *op, lite::Value a, lite::Value b) {
    static thread_local char buf[192];
    std::snprintf(buf, sizeof(buf), "不支持的操作数类型: %s 用于 '%s' 和 '%s'",
                  op, tag_name(a.tag), tag_name(b.tag));
    runtime_error(buf);
  }

  void needNumeric(const char *op, lite::Value a, lite::Value b) {
    if (!isNumeric(a.tag) || !isNumeric(b.tag)) badOperands(op, a, b);
  }

  lite::Value boolOf(bool b) { return bool_(b); }

  // 列表与元组共用一套逐元素比较的取长/取值。两个类型在这些操作上完全一致,
  // 唯一区别是构造结果时的类型(见 index.cpp 的切片)。
  int64_t seqLen(const lite::Value &v) {
    return v.tag == lite::kList ? list_len(&v) : tuple_len(&v);
  }
  lite::Value seqAt(const lite::Value &v, int64_t i) {
    return v.tag == lite::kList ? list_get(&v, i) : tuple_get(&v, i);
  }

}  // namespace

// ---------------------------------------------------------------------------
// 算术
// ---------------------------------------------------------------------------

extern "C" lite::Value add(lite::Value a, lite::Value b) {
  // 字符串拼接与列表拼接各走各的:两者都返回**新**对象。
  // 列表的"就地"版本是 iadd,别搞混(见 iadd 的说明)。
  if (a.tag == lite::kStr && b.tag == lite::kStr) return str_concat(&a, &b);
  if (a.tag == lite::kList && b.tag == lite::kList) return list_concat(&a, &b);
  needNumeric("+", a, b);
  if (wantFloat(a, b)) return float_(toDouble(a) + toDouble(b));
  return int_(a.as.i + b.as.i);
}

// `a += b`。与 add 的差别**只在列表上**:
//
//   xs = [1]; ys = xs; xs += [2]      -> 就地 extend,ys 也是 [1, 2]
//   xs = [1]; ys = xs; xs = xs + [2]  -> 重新绑定,ys 还是 [1]
//
// 这与 Python 的 list.__iadd__ 一致,也正是运算符重载把 += 单独开出来教会
// 语言实现的经典理由。字符串与数值不可变,返回新值即可 —— 调用点会把结果写回
// 自己的槽位,指向同一字符串的别名不受影响,同样符合 Python。
//
// 注意 xs += xs:调用点两侧传进来的是同一个 alloca,指针相同。就地 extend 必须
// 能扛住自拼接 —— 这部分由 list_extend 负责(见 list.cpp)。
extern "C" lite::Value iadd(lite::Value *a, const lite::Value *b) {
  if (a->tag == lite::kList && b->tag == lite::kList) {
    list_extend(a, b);
    return *a;
  }
  return add(*a, *b);
}

extern "C" lite::Value sub(lite::Value a, lite::Value b) {
  needNumeric("-", a, b);
  if (wantFloat(a, b)) return float_(toDouble(a) - toDouble(b));
  return int_(a.as.i - b.as.i);
}

extern "C" lite::Value mul(lite::Value a, lite::Value b) {
  // 重复:字符串与列表都能乘整数,两个方向都要认("ab" * 2 与 2 * "ab")
  if (a.tag == lite::kStr && isCountLike(b.tag)) return str_repeat(&a, b.as.i);
  if (b.tag == lite::kStr && isCountLike(a.tag)) return str_repeat(&b, a.as.i);
  if (a.tag == lite::kList && isCountLike(b.tag)) return list_repeat(&a, b.as.i);
  if (b.tag == lite::kList && isCountLike(a.tag)) return list_repeat(&b, a.as.i);

  needNumeric("*", a, b);
  if (wantFloat(a, b)) return float_(toDouble(a) * toDouble(b));
  return int_(a.as.i * b.as.i);
}

// Python 的 / 是真除法,结果总是 float(7 / 2 == 3.5)
extern "C" lite::Value div_(lite::Value a, lite::Value b) {
  needNumeric("/", a, b);
  const double d = toDouble(b);
  if (d == 0.0) runtime_error("除以零");
  return float_(toDouble(a) / d);
}

// // 是向下取整除法:整数相除结果仍是整数,浮点相除结果是浮点数
extern "C" lite::Value floordiv(lite::Value a, lite::Value b) {
  needNumeric("//", a, b);
  if (wantFloat(a, b)) {
    const double d = toDouble(b);
    if (d == 0.0) runtime_error("除以零");
    return float_(std::floor(toDouble(a) / d));
  }
  if (b.as.i == 0) runtime_error("整数除以零");
  // C++ 的 / 向零取整,Python 的 // 向下取整,负数时结果不同:
  //   -7 // 2 == -4(Python)而 C++ 给 -3。这里按 Python 语义修正。
  int64_t q = a.as.i / b.as.i;
  const int64_t r = a.as.i % b.as.i;
  if (r != 0 && ((r < 0) != (b.as.i < 0))) --q;
  return int_(q);
}

// 取模。Python 的结果符号跟随除数(与 C++ 的 % 不同)。
extern "C" lite::Value mod_(lite::Value a, lite::Value b) {
  needNumeric("%", a, b);
  if (wantFloat(a, b)) {
    const double d = toDouble(b);
    if (d == 0.0) runtime_error("取模的除数为零");
    double r = std::fmod(toDouble(a), d);
    if (r != 0.0 && ((r < 0.0) != (d < 0.0))) r += d;
    return float_(r);
  }
  if (b.as.i == 0) runtime_error("整数取模的除数为零");
  int64_t r = a.as.i % b.as.i;
  if (r != 0 && ((r < 0) != (b.as.i < 0))) r += b.as.i;
  return int_(r);
}

// 幂。整数底数与非负整数指数时结果保持整数,否则退化为浮点。
// 这里用朴素的累乘实现;大指数会慢,但对 v1 的用途足够了。
extern "C" lite::Value pow_(lite::Value a, lite::Value b) {
  needNumeric("**", a, b);
  if (!wantFloat(a, b) && b.as.i >= 0) {
    int64_t base = a.as.i;
    int64_t exp = b.as.i;
    int64_t result = 1;
    while (exp > 0) {
      if (exp & 1) result *= base;
      base *= base;
      exp >>= 1;
    }
    return int_(result);
  }
  return float_(std::pow(toDouble(a), toDouble(b)));
}

// 取整数负载。range 的边界用它 —— 顺便在这里做类型检查,
// 让 `range("a")` 之类能在运行时给出像样的报错,而不是把指针当数字用。
extern "C" int64_t to_int(lite::Value v) {
  if (v.tag == lite::kInt || v.tag == lite::kBool) return v.as.i;
  static thread_local char buf[128];
  std::snprintf(buf, sizeof(buf), "需要整数,实际得到 '%s'", tag_name(v.tag));
  runtime_error(buf);
}

extern "C" lite::Value neg(lite::Value a) {
  if (!isNumeric(a.tag)) {
    static thread_local char buf[128];
    std::snprintf(buf, sizeof(buf), "不支持的操作数类型: 一元 '-' 用于 '%s'",
                  tag_name(a.tag));
    runtime_error(buf);
  }
  if (a.tag == lite::kFloat) return float_(-a.as.f);
  return int_(-a.as.i);
}

// ---------------------------------------------------------------------------
// 比较
//
// 全部返回 bool 值的 lite::Value。类型不匹配时 ==/!= 返回 False/True,
// 其余比较报错 —— 与 Python 一致。
// ---------------------------------------------------------------------------

// 相等。这里有两个曾经是 bug、必须按值比较的地方:
//
//   * 字符串:payload 存的是 lite::Str*,早先走到 default 分支比的是**指针**,
//     于是 "a" == "a" 是 False。现在按字节比。
//   * int 与 float 跨类型:1 == 1.0 在 Python 里是 True。这同时也是字典键
//     语义的一部分 —— Python 认为 1 与 1.0 是同一个键。
extern "C" lite::Value eq(lite::Value a, lite::Value b) {
  if (a.tag != b.tag) {
    // 数值之间跨类型比较有意义;其余类型不同一律不等(不报错,与 Python 一致)
    if (isNumeric(a.tag) && isNumeric(b.tag)) {
      return boolOf(toDouble(a) == toDouble(b));
    }
    return boolOf(false);
  }
  switch (a.tag) {
    case lite::kNull:
      return boolOf(true);
    case lite::kFloat:
      return boolOf(a.as.f == b.as.f);
    case lite::kStr:
      return boolOf(str_compare(&a, &b) == 0);
    case lite::kList:
    case lite::kTuple: {
      const int64_t n = seqLen(a);
      if (n != seqLen(b)) return boolOf(false);
      for (int64_t i = 0; i < n; ++i) {
        if (eq(seqAt(a, i), seqAt(b, i)).as.i == 0) return boolOf(false);
      }
      return boolOf(true);
    }
    case lite::kDict: {
      // 字典相等与插入序**无关**:逐键查过去、值和键都相等即可
      const int64_t n = dict_len(&a);
      if (n != dict_len(&b)) return boolOf(false);
      for (int64_t i = 0; i < n; ++i) {
        lite::Value k = dict_key_at(&a, i);
        if (!dict_has(&b, &k)) return boolOf(false);
        if (eq(dict_get(&b, &k), dict_val_at(&a, i)).as.i == 0) {
          return boolOf(false);
        }
      }
      return boolOf(true);
    }
    default:
      return boolOf(a.as.i == b.as.i);
  }
}

extern "C" lite::Value ne(lite::Value a, lite::Value b) {
  lite::Value e = eq(a, b);
  return boolOf(e.as.i == 0);
}

// 三路比较。数值之间比大小、两个字符串按字典序比,其余一律报错 ——
// 与 Python 的行为一致(`1 < "a"` 在 Python 里是 TypeError)。
extern "C" int32_t compare_(const lite::Value *a, const lite::Value *b) {
  if (a->tag == lite::kStr && b->tag == lite::kStr) return str_compare(a, b);
  if (isNumeric(a->tag) && isNumeric(b->tag)) {
    if (wantFloat(*a, *b)) {
      const double x = toDouble(*a), y = toDouble(*b);
      return x < y ? -1 : (x > y ? 1 : 0);
    }
    return a->as.i < b->as.i ? -1 : (a->as.i > b->as.i ? 1 : 0);
  }
  badOperands("<", *a, *b);
}

extern "C" lite::Value lt(lite::Value a, lite::Value b) {
  return boolOf(compare_(&a, &b) < 0);
}

extern "C" lite::Value le(lite::Value a, lite::Value b) {
  return boolOf(compare_(&a, &b) <= 0);
}

extern "C" lite::Value gt(lite::Value a, lite::Value b) {
  return boolOf(compare_(&a, &b) > 0);
}

extern "C" lite::Value ge(lite::Value a, lite::Value b) {
  return boolOf(compare_(&a, &b) >= 0);
}

// ---------------------------------------------------------------------------
// 逻辑
//
// and / or 的短路由 IRGen 用 PHI 实现(且要返回操作数本身,不是布尔值),
// 所以这里只有 not。
// ---------------------------------------------------------------------------

extern "C" lite::Value not_(lite::Value a) { return boolOf(truthy(a) == 0); }

// 返回 i32 而非 i1:见 value.h 的说明,GCC 与 Clang 对 bool 返回值的
// ABI 标注不一致,跨 DLL 边界不可靠。
extern "C" int32_t truthy(lite::Value v) {
  switch (v.tag) {
    case lite::kNull:  return 0;
    case lite::kBool:
    case lite::kInt:   return v.as.i != 0;
    case lite::kFloat: return v.as.f != 0.0;
    // 容器按元素个数判空:空串、空列表、空字典、空元组都是假。
    // 早先这里是"非空指针即真",于是 `if []:` 会走真分支。
    case lite::kStr:   return str_size(&v) != 0;
    case lite::kList:  return list_len(&v) != 0;
    case lite::kDict:  return dict_len(&v) != 0;
    case lite::kTuple: return tuple_len(&v) != 0;
    default:       return v.as.ptr != nullptr;
  }
}
