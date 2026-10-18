// Value —— 通用运行时值表示。
//
// ABI 基石：运行时、生成的代码、手写 C++ 三方都按这份布局交换数据。
// 布局必须与 LLVM 侧 {i32, i64} 逐字节一致，总计 16 字节、对齐 8。
#pragma once

#include <cstdint>
#include <cstddef>

namespace xmex {

enum Tag : int32_t {
  kNull     = 0,
  kBool     = 1,
  kInt      = 2,
  kFloat    = 3,
  kStr      = 4,
  kList     = 5,
  kDict     = 6,
  kTuple    = 7,
  kFunc     = 8,
  kInstance = 9,
};

struct Str;
struct List;
struct Dict;
struct Tuple;

struct Value {
  int32_t tag;
  union {
    int64_t i;
    double  f;
    void   *ptr;
  } as;
};

static_assert(sizeof(Value) == 16, "Value layout is part of the ABI");
static_assert(alignof(Value) == 8, "Value layout is part of the ABI");

}  // namespace xmex

// 构造函数与取值辅助放在全局作用域，调用处不必写命名空间限定。
inline xmex::Value none_() {
  xmex::Value r;
  r.tag = xmex::kNull;
  r.as.i = 0;
  return r;
}

inline xmex::Value int_(int64_t v) {
  xmex::Value r;
  r.tag = xmex::kInt;
  r.as.i = v;
  return r;
}

inline xmex::Value float_(double v) {
  xmex::Value r;
  r.tag = xmex::kFloat;
  r.as.f = v;
  return r;
}

inline xmex::Value bool_(bool v) {
  xmex::Value r;
  r.tag = xmex::kBool;
  r.as.i = v ? 1 : 0;
  return r;
}

inline xmex::Value ptr(int32_t tag, void *p) {
  xmex::Value r;
  r.tag = tag;
  r.as.ptr = p;
  return r;
}

inline int64_t as_int(xmex::Value v)   { return v.as.i; }
inline double  as_float(xmex::Value v) { return v.as.f; }
inline bool    as_bool(xmex::Value v)  { return v.as.i != 0; }
inline void   *as_ptr(xmex::Value v)   { return v.as.ptr; }
