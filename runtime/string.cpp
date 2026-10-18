// 字符串。
//
// 字符串**不可变**:拼接、切片、重复一律新分配,绝不就地改。这一点有意与列表
// 相反,也正是 add 与 iadd 在字符串上必须分道扬镳的原因
// (`s += "x"` 重新绑定,别名看不到变化;`xs += [1]` 就地改,别名看得见)。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstring>

// 与 value.h 里的前置声明 `struct lite::Str;` 对应,所以必须是全局作用域 ——
// 放进匿名命名空间会与那个声明构成歧义(编译期就报 ambiguous)。
// 定义只在本翻译单元内可见,外部一律通过 str_* 函数访问。
namespace lite {
struct Str {
  int64_t len;
  char data[1];  // 变长,末尾保证有 '\0'
};
}  // namespace lite

extern "C" lite::Value str_new(const char *bytes, int64_t len) {
  // SCAN_NONE:字符串块里只有长度和字节,没有任何指针。判成"要扫"会把文本内容
  // 当指针,平白保留一堆块。
  auto *s = static_cast<lite::Str *>(
      gc_alloc(sizeof(int64_t) + static_cast<size_t>(len) + 1, lite::kScanNone));
  s->len = len;
  if (len > 0) std::memcpy(s->data, bytes, static_cast<size_t>(len));
  s->data[len] = '\0';
  return ptr(lite::kStr, s);
}

extern "C" int64_t str_size(const lite::Value *v) {
  if (v->tag != lite::kStr) runtime_error("需要一个字符串");
  return reinterpret_cast<lite::Str *>(v->as.ptr)->len;
}

extern "C" const char *str_data(const lite::Value *v) {
  if (v->tag != lite::kStr) runtime_error("需要一个字符串");
  return reinterpret_cast<lite::Str *>(v->as.ptr)->data;
}

extern "C" lite::Value str_concat(const lite::Value *a, const lite::Value *b) {
  if (a->tag != lite::kStr || b->tag != lite::kStr) {
    runtime_error("只能拼接两个字符串");
  }
  auto *sa = reinterpret_cast<lite::Str *>(a->as.ptr);
  auto *sb = reinterpret_cast<lite::Str *>(b->as.ptr);
  auto *r = static_cast<lite::Str *>(gc_alloc(
      sizeof(int64_t) + static_cast<size_t>(sa->len + sb->len) + 1, lite::kScanNone));
  r->len = sa->len + sb->len;
  std::memcpy(r->data, sa->data, static_cast<size_t>(sa->len));
  std::memcpy(r->data + sa->len, sb->data, static_cast<size_t>(sb->len));
  r->data[r->len] = '\0';
  return ptr(lite::kStr, r);
}

// s[i] —— 取出一个长度为 1 的**新字符串**,不是字符码(与 Python 一致)。
extern "C" lite::Value str_get(const lite::Value *v, int64_t i) {
  if (v->tag != lite::kStr) runtime_error("下标访问需要一个字符串");
  auto *s = reinterpret_cast<lite::Str *>(v->as.ptr);
  if (i < 0) i += s->len;
  if (i < 0 || i >= s->len) {
    static thread_local char buf[128];
    std::snprintf(buf, sizeof(buf), "字符串下标越界: %lld(长度 %lld)",
                  static_cast<long long>(i), static_cast<long long>(s->len));
    runtime_error(buf);
  }
  return str_new(s->data + i, 1);
}

extern "C" lite::Value str_repeat(const lite::Value *v, int64_t times) {
  if (v->tag != lite::kStr) runtime_error("'*' 需要一个字符串");
  if (times <= 0) return str_new("", 0);  // Python: "ab" * -1 == ""
  auto *s = reinterpret_cast<lite::Str *>(v->as.ptr);
  const int64_t total = s->len * times;
  auto *r = static_cast<lite::Str *>(
      gc_alloc(sizeof(int64_t) + static_cast<size_t>(total) + 1, lite::kScanNone));
  r->len = total;
  for (int64_t k = 0; k < times; ++k) {
    std::memcpy(r->data + k * s->len, s->data, static_cast<size_t>(s->len));
  }
  r->data[total] = '\0';
  return ptr(lite::kStr, r);
}

// 按字节序比较。注意 C 的 strcmp 在遇到内嵌 '\0'(来自 "\0" 转义)时会提前
// 结束,所以这里必须用 memcmp + 长度,不能图省事调 strcmp。
extern "C" int32_t str_compare(const lite::Value *a, const lite::Value *b) {
  if (a->tag != lite::kStr || b->tag != lite::kStr) {
    runtime_error("字符串比较需要两个字符串");
  }
  auto *sa = reinterpret_cast<lite::Str *>(a->as.ptr);
  auto *sb = reinterpret_cast<lite::Str *>(b->as.ptr);
  const int64_t n = sa->len < sb->len ? sa->len : sb->len;
  if (n > 0) {
    const int c = std::memcmp(sa->data, sb->data, static_cast<size_t>(n));
    if (c != 0) return c < 0 ? -1 : 1;
  }
  if (sa->len == sb->len) return 0;
  return sa->len < sb->len ? -1 : 1;
}

// 子串查找,找不到返回 -1。from 允许负数(Python 的 str.find 语义)。
extern "C" int32_t str_find(const lite::Value *v, const lite::Value *needle,
                               int64_t from) {
  if (v->tag != lite::kStr || needle->tag != lite::kStr) {
    runtime_error("find() 需要两个字符串");
  }
  auto *s = reinterpret_cast<lite::Str *>(v->as.ptr);
  auto *n = reinterpret_cast<lite::Str *>(needle->as.ptr);
  if (from < 0) from += s->len;
  if (from < 0) from = 0;
  if (from > s->len) return -1;
  if (n->len == 0) return static_cast<int32_t>(from);

  for (int64_t i = from; i + n->len <= s->len; ++i) {
    if (std::memcmp(s->data + i, n->data, static_cast<size_t>(n->len)) == 0) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

extern "C" int32_t str_starts_with(const lite::Value *v, const lite::Value *prefix) {
  if (v->tag != lite::kStr || prefix->tag != lite::kStr) {
    runtime_error("startswith() 需要两个字符串");
  }
  auto *s = reinterpret_cast<lite::Str *>(v->as.ptr);
  auto *p = reinterpret_cast<lite::Str *>(prefix->as.ptr);
  if (p->len > s->len) return 0;
  return std::memcmp(s->data, p->data, static_cast<size_t>(p->len)) == 0 ? 1 : 0;
}

extern "C" int32_t str_ends_with(const lite::Value *v, const lite::Value *suffix) {
  if (v->tag != lite::kStr || suffix->tag != lite::kStr) {
    runtime_error("endswith() 需要两个字符串");
  }
  auto *s = reinterpret_cast<lite::Str *>(v->as.ptr);
  auto *p = reinterpret_cast<lite::Str *>(suffix->as.ptr);
  if (p->len > s->len) return 0;
  return std::memcmp(s->data + (s->len - p->len), p->data,
                     static_cast<size_t>(p->len)) == 0
             ? 1
             : 0;
}
