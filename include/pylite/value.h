// Value —— 通用运行时值表示。
//
// ABI 基石：运行时、生成的代码、手写 C++ 三方都按这份布局交换数据。
// 布局必须与 LLVM 侧 {i32, i64} 逐字节一致，总计 16 字节、对齐 8。
#pragma once

#include <cstdint>
#include <cstddef>

namespace lite {

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

}  // namespace lite

// 构造函数与取值辅助放在全局作用域，调用处不必写命名空间限定。
inline lite::Value none_() {
  lite::Value r;
  r.tag = lite::kNull;
  r.as.i = 0;
  return r;
}

inline lite::Value int_(int64_t v) {
  lite::Value r;
  r.tag = lite::kInt;
  r.as.i = v;
  return r;
}

inline lite::Value float_(double v) {
  lite::Value r;
  r.tag = lite::kFloat;
  r.as.f = v;
  return r;
}

inline lite::Value bool_(bool v) {
  lite::Value r;
  r.tag = lite::kBool;
  r.as.i = v ? 1 : 0;
  return r;
}

inline lite::Value ptr(int32_t tag, void *p) {
  lite::Value r;
  r.tag = tag;
  r.as.ptr = p;
  return r;
}

inline int64_t as_int(lite::Value v)   { return v.as.i; }
inline double  as_float(lite::Value v) { return v.as.f; }
inline bool    as_bool(lite::Value v)  { return v.as.i != 0; }
inline void   *as_ptr(lite::Value v)   { return v.as.ptr; }
