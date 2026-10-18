// 字典。
//
// **插入有序 + 线性扫描**,刻意不用哈希表:
//
//   * 线性扫描保住了 Python 3.7 起可观测的插入序 —— 遍历 dict 按插入顺序、
//     `==` 比较与 repr 的输出稳定,这些行为不依赖哈希实现也不需要额外维护
//     一条顺序链。
//   * 哈希表要为每种键类型(int/float/str/bool/tuple)写一份 hash,还得处理
//     冲突与再散列;而本项目的用途下字典都不大。
//
// 代价是查找 O(n),README 与 docs/language.md 里都写明了。
//
// 键的相等判定复用 eq —— 单一事实来源。特别注意 eq 对字符串必须按
// **值**比较(见 arith.cpp),否则这里会退化成按指针认键,`d["a"]` 永远查不到。
#include "xmex/runtime.h"

#include <cstdio>
#include <cstring>

// 与 value.h 里的前置声明对应,必须是全局作用域。
namespace xmex {
struct Dict {
  int64_t len;
  int64_t cap;
  Value *keys;
  Value *vals;
};
}  // namespace xmex

namespace {

xmex::Dict *asDict(const xmex::Value *v) { return static_cast<xmex::Dict *>(v->as.ptr); }

void needDict(const xmex::Value *v, const char *what) {
  if (v->tag != xmex::kDict) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "%s 需要字典,实际得到 '%s'", what,
                  tag_name(v->tag));
    runtime_error(buf);
  }
}

void reserve(xmex::Dict *d, int64_t need) {
  if (need <= d->cap) return;
  int64_t newCap = d->cap ? d->cap : 8;
  while (newCap < need) newCap *= 2;

  // ⚠️ 显式根:`keys` 必须在 `vals` 的分配期间活下来 —— 那两次分配之间它只被这个
  // C++ 局部持有,没有任何容器指得到它(理由同 list.cpp 的 newList)。
  xmex::Value *keys = nullptr;
  gc_root_push(reinterpret_cast<void **>(&keys));
  keys = static_cast<xmex::Value *>(gc_alloc_values(
      static_cast<size_t>(newCap) * sizeof(xmex::Value), 0));

  auto *vals = static_cast<xmex::Value *>(gc_alloc_values(
      static_cast<size_t>(newCap) * sizeof(xmex::Value), 0));
  gc_root_pop(1);
  if (d->len > 0) {
    std::memcpy(keys, d->keys, static_cast<size_t>(d->len) * sizeof(xmex::Value));
    std::memcpy(vals, d->vals, static_cast<size_t>(d->len) * sizeof(xmex::Value));
  }
  d->keys = keys;
  d->vals = vals;
  d->cap = newCap;
}

xmex::Dict *newDict(int64_t cap) {
  // ⚠️ 显式根:`d` 跨过 reserve 里的两次分配(理由同 list.cpp 的 newList)
  xmex::Dict *d = nullptr;
  gc_root_push(reinterpret_cast<void **>(&d));

  // SCAN_WORDS:xmex::Dict 的后两个字是 keys / vals 指针
  d = static_cast<xmex::Dict *>(gc_alloc(sizeof(xmex::Dict), xmex::kScanWords));
  d->len = 0;
  d->cap = 0;
  d->keys = nullptr;
  d->vals = nullptr;
  reserve(d, cap);

  gc_root_pop(1);
  return d;
}

// 按键查找,返回下标;找不到返回 -1
int64_t findIndex(xmex::Dict *d, const xmex::Value *key) {
  for (int64_t i = 0; i < d->len; ++i) {
    if (eq(d->keys[i], *key).as.i != 0) return i;
  }
  return -1;
}

void needIndex(int64_t i, int64_t len) {
  if (i < 0 || i >= len) {
    static thread_local char buf[128];
    std::snprintf(buf, sizeof(buf), "字典下标越界: %lld(长度 %lld)",
                  static_cast<long long>(i), static_cast<long long>(len));
    runtime_error(buf);
  }
}

}  // namespace

extern "C" xmex::Value dict_new(xmex::Value *keys, xmex::Value *vals, int64_t n) {
  // ⚠️ 显式根:循环里每一次 reserve 都可能触发回收,而 `d` 只被这个局部持有
  xmex::Dict *d = nullptr;
  gc_root_push(reinterpret_cast<void **>(&d));

  d = newDict(n);
  // 字面量里出现重复键时:值取**后**到的,位置取**先**到的 —— 与 Python 一致
  // (`{"a": 1, "b": 2, "a": 3}` 得到 `{'a': 3, 'b': 2}`,'a' 仍在首位)。
  for (int64_t i = 0; i < n; ++i) {
    const int64_t at = findIndex(d, &keys[i]);
    if (at >= 0) {
      d->vals[at] = vals[i];
    } else {
      reserve(d, d->len + 1);
      d->keys[d->len] = keys[i];
      d->vals[d->len] = vals[i];
      ++d->len;
    }
  }

  gc_root_pop(1);
  return ptr(xmex::kDict, d);
}

extern "C" int64_t dict_len(const xmex::Value *v) {
  needDict(v, "len()");
  return asDict(v)->len;
}

extern "C" int32_t dict_has(const xmex::Value *v, const xmex::Value *key) {
  needDict(v, "'in'");
  return findIndex(asDict(v), key) >= 0 ? 1 : 0;
}

extern "C" xmex::Value dict_get(const xmex::Value *v, const xmex::Value *key) {
  needDict(v, "下标访问");
  xmex::Dict *d = asDict(v);
  const int64_t at = findIndex(d, key);
  if (at < 0) {
    // 用 repr 显示键:字符串键能带出引号,数字键与字符串键不会被混为一谈
    xmex::Value shown = repr(key);
    static thread_local char buf[256];
    std::snprintf(buf, sizeof(buf), "字典里没有键 %.*s",
                  static_cast<int>(str_size(&shown)), str_data(&shown));
    runtime_error(buf);
  }
  return d->vals[at];
}

extern "C" xmex::Value dict_get_default(const xmex::Value *v, const xmex::Value *key,
                                       const xmex::Value *def) {
  needDict(v, "get()");
  xmex::Dict *d = asDict(v);
  const int64_t at = findIndex(d, key);
  return at < 0 ? *def : d->vals[at];
}

extern "C" void dict_set(const xmex::Value *v, const xmex::Value *key,
                            const xmex::Value *val) {
  needDict(v, "下标赋值");
  xmex::Dict *d = asDict(v);
  const int64_t at = findIndex(d, key);
  if (at >= 0) {
    d->vals[at] = *val;
    return;
  }
  reserve(d, d->len + 1);
  d->keys[d->len] = *key;
  d->vals[d->len] = *val;
  ++d->len;
}

extern "C" xmex::Value dict_key_at(const xmex::Value *v, int64_t i) {
  needDict(v, "遍历");
  xmex::Dict *d = asDict(v);
  needIndex(i, d->len);
  return d->keys[i];
}

extern "C" xmex::Value dict_val_at(const xmex::Value *v, int64_t i) {
  needDict(v, "遍历");
  xmex::Dict *d = asDict(v);
  needIndex(i, d->len);
  return d->vals[i];
}

extern "C" xmex::Value dict_pop(const xmex::Value *v, const xmex::Value *key) {
  needDict(v, "pop()");
  xmex::Dict *d = asDict(v);
  const int64_t at = findIndex(d, key);
  if (at < 0) runtime_error("pop(): 字典里没有这个键");
  xmex::Value r = d->vals[at];
  // 后面的元素整体前移,保持插入序
  std::memmove(d->keys + at, d->keys + at + 1,
               static_cast<size_t>(d->len - at - 1) * sizeof(xmex::Value));
  std::memmove(d->vals + at, d->vals + at + 1,
               static_cast<size_t>(d->len - at - 1) * sizeof(xmex::Value));
  --d->len;
  return r;
}

extern "C" void dict_clear(const xmex::Value *v) {
  needDict(v, "clear()");
  asDict(v)->len = 0;
}
