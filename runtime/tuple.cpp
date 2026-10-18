// 元组。
//
// 存在的意义主要是承载"多值":input(int, int) 的返回值、函数的多返回值,
// 以及 a, b = ... 的右侧。IRGen 对多目标赋值统一走 unpack。
//
// 按指针收发(参数是 lite::Value*),避免聚合体跨 ABI 边界按值传递。
#include "pylite/runtime.h"

#include <cstdio>

// 与 value.h 里的前置声明 `struct lite::Tuple;` 对应,必须是全局作用域 ——
// 放进匿名命名空间会与那个声明构成歧义。定义只在本翻译单元可见。
namespace lite {
struct Tuple {
  int64_t len;
  Value items[1];  // 变长数组,实际分配 sizeof(int64_t) + len*sizeof(Value)
};
}  // namespace lite

namespace {
lite::Tuple *asTuple(const lite::Value *v) { return static_cast<lite::Tuple *>(v->as.ptr); }
}  // namespace

extern "C" lite::Value tuple_new(lite::Value *elems, int64_t n) {
  // ⚠️ 单元格从偏移 8 开始:lite::Tuple 是 {int64_t len; lite::Value items[];},
  // 从 0 开始会把 len 当成 tag、把 items[0].tag 当成 payload,整条链错位 ——
  // 那是真的会丢根,不是保守与否的问题。
  auto *t = static_cast<lite::Tuple *>(gc_alloc_values(
      sizeof(int64_t) + sizeof(lite::Value) * static_cast<size_t>(n), 8));
  t->len = n;
  for (int64_t i = 0; i < n; ++i) t->items[i] = elems[i];
  return ptr(lite::kTuple, t);
}

extern "C" int64_t tuple_len(const lite::Value *v) {
  if (v->tag != lite::kTuple) {
    static thread_local char buf[128];
    std::snprintf(buf, sizeof(buf), "len() 不支持 '%s'", tag_name(v->tag));
    runtime_error(buf);
  }
  return asTuple(v)->len;
}

extern "C" lite::Value tuple_get(const lite::Value *v, int64_t i) {
  lite::Tuple *t = asTuple(v);
  // 负索引从末尾数,与列表/字符串一致(Python 语义)。
  // 内部的 repr 打印总是传非负下标,不受影响。
  if (i < 0) i += t->len;
  if (i < 0 || i >= t->len) {
    static thread_local char buf[128];
    std::snprintf(buf, sizeof(buf), "元组下标越界: %lld(长度 %lld)",
                  static_cast<long long>(i), static_cast<long long>(t->len));
    runtime_error(buf);
  }
  return t->items[i];
}

// 把元组解包到 out 指向的 n 个连续槽位。
// 元素个数不匹配是常见错误(比如 a, b = 只有两个值的东西),这里给出
// 明确的报错而不是静默出错。
extern "C" void unpack(const lite::Value *v, lite::Value *out, int64_t n) {
  if (v->tag != lite::kTuple) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "无法解包:'%s' 不是元组", tag_name(v->tag));
    runtime_error(buf);
  }
  lite::Tuple *t = asTuple(v);
  if (t->len != n) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "解包元素个数不匹配:左侧要 %lld 个,右侧有 %lld 个",
                  static_cast<long long>(n), static_cast<long long>(t->len));
    runtime_error(buf);
  }
  for (int64_t i = 0; i < n; ++i) out[i] = t->items[i];
}
