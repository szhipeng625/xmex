// 列表。
//
// 列表是本项目里第一个**可变**对象:append/insert/pop 就地改元素数组,
// 持有同一个列表的别名会看到改动。这个语义是有意做出来的(与 Python 一致),
// 也是 iadd 与 add 必须分开的原因。
//
// 结构体定义只在本翻译单元内可见,与 runtime/tuple.cpp、string.cpp 同一模式:
// 外部一律经 list_* 函数访问,这样布局改动不会波及别的 TU。
//
// 增长策略是"新分配 + 拷贝",因为 bump 分配器没有 realloc(见 arena.cpp)。
// 旧数组就地泄漏 —— 与项目"内存只增不还"的整体取舍一致。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstring>

// 与 value.h 里的前置声明 `struct lite::List;` 对应,必须是全局作用域 ——
// 放进匿名命名空间会与那个声明构成歧义。
namespace lite {
struct List {
  int64_t len;
  int64_t cap;
  Value *items;
};
}  // namespace lite

namespace {

lite::List *asList(const lite::Value *v) { return static_cast<lite::List *>(v->as.ptr); }

void needList(const lite::Value *v, const char *what) {
  if (v->tag != lite::kList) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "%s 需要列表,实际得到 '%s'", what,
                  tag_name(v->tag));
    runtime_error(buf);
  }
}

// 负索引从末尾数,然后检查边界。所有按下标访问的路径都过这里,
// 保证报错信息一致。
int64_t normalize(int64_t i, int64_t len, const char *what) {
  if (i < 0) i += len;
  if (i < 0 || i >= len) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "%s下标越界: %lld(长度 %lld)", what,
                  static_cast<long long>(i), static_cast<long long>(len));
    runtime_error(buf);
  }
  return i;
}

// 保证至少能放 need 个元素。翻倍增长,避免逐个 append 时每次都要拷贝。
void reserve(lite::List *l, int64_t need) {
  if (need <= l->cap) return;
  int64_t newCap = l->cap ? l->cap : 8;
  while (newCap < need) newCap *= 2;
  // SCAN_VALUES:元素数组里装的就是 lite::Value,按 tag 判断是否跟随 payload
  auto *items = static_cast<lite::Value *>(gc_alloc_values(
      static_cast<size_t>(newCap) * sizeof(lite::Value), 0));
  if (l->len > 0) {
    std::memcpy(items, l->items, static_cast<size_t>(l->len) * sizeof(lite::Value));
  }
  l->items = items;
  l->cap = newCap;
}

lite::List *newList(int64_t cap) {
  // ⚠️ 显式根:`l` 在两次分配之间只被这个 C++ 局部持有 —— 它既不在任何容器里
  // (所以"保守扫描沿指针找"帮不上忙),也不在 IRGen 的 alloca 保证范围内
  // (那条只覆盖生成代码)。若第二次分配触发回收而编译器又没把 `l` spill 到栈上,
  // 刚建好的列表头就会被收掉,之后写 items 就写在已释放的块上。
  // 取地址这个动作本身迫使 `l` 留在内存里,配合登记就成了硬契约而不是运气。
  lite::List *l = nullptr;
  gc_root_push(reinterpret_cast<void **>(&l));

  // SCAN_WORDS:lite::List 的第三个字是 items 指针,必须照到 —— 靠它才能沿指针找到
  // 元素数组那个块(不需要为 lite::List 单独写 tracer)。
  l = static_cast<lite::List *>(gc_alloc(sizeof(lite::List), lite::kScanWords));
  l->len = 0;
  l->cap = cap;
  l->items = cap > 0 ? static_cast<lite::Value *>(gc_alloc_values(
                           static_cast<size_t>(cap) * sizeof(lite::Value), 0))
                     : nullptr;

  gc_root_pop(1);
  return l;
}

}  // namespace

extern "C" lite::Value list_new(lite::Value *elems, int64_t n) {
  if (n < 0) runtime_error("列表长度不能为负");
  lite::List *l = newList(n);
  l->len = n;
  if (n > 0) std::memcpy(l->items, elems, static_cast<size_t>(n) * sizeof(lite::Value));
  return ptr(lite::kList, l);
}

extern "C" int64_t list_len(const lite::Value *v) {
  needList(v, "len()");
  return asList(v)->len;
}

extern "C" lite::Value list_get(const lite::Value *v, int64_t i) {
  needList(v, "下标访问");
  lite::List *l = asList(v);
  return l->items[normalize(i, l->len, "列表")];
}

extern "C" void list_set(const lite::Value *v, int64_t i, const lite::Value *val) {
  needList(v, "下标赋值");
  lite::List *l = asList(v);
  l->items[normalize(i, l->len, "列表")] = *val;
}

extern "C" lite::Value list_concat(const lite::Value *a, const lite::Value *b) {
  needList(a, "'+'");
  needList(b, "'+'");
  lite::List *la = asList(a);
  lite::List *lb = asList(b);
  lite::List *r = newList(la->len + lb->len);
  r->len = la->len + lb->len;
  if (la->len > 0) {
    std::memcpy(r->items, la->items,
                static_cast<size_t>(la->len) * sizeof(lite::Value));
  }
  if (lb->len > 0) {
    std::memcpy(r->items + la->len, lb->items,
                static_cast<size_t>(lb->len) * sizeof(lite::Value));
  }
  return ptr(lite::kList, r);
}

extern "C" lite::Value list_repeat(const lite::Value *a, int64_t times) {
  needList(a, "'*'");
  if (times < 0) times = 0;  // Python: [1] * -1 == []
  lite::List *la = asList(a);
  lite::List *r = newList(la->len * times);
  r->len = la->len * times;
  for (int64_t k = 0; k < times; ++k) {
    if (la->len > 0) {
      std::memcpy(r->items + k * la->len, la->items,
                  static_cast<size_t>(la->len) * sizeof(lite::Value));
    }
  }
  return ptr(lite::kList, r);
}

// ---------------------------------------------------------------------------
// 就地修改
// ---------------------------------------------------------------------------

extern "C" void list_append(const lite::Value *v, const lite::Value *item) {
  needList(v, "append()");
  lite::List *l = asList(v);
  reserve(l, l->len + 1);
  l->items[l->len++] = *item;
}

extern "C" void list_extend(const lite::Value *v, const lite::Value *other) {
  needList(v, "extend()");
  needList(other, "extend()");
  lite::List *l = asList(v);
  const lite::List *o = asList(other);
  // 先把长度和数组取出来再 reserve:reserve 会重新分配 l->items,
  // 而 `xs.extend(xs)` 时 o 与 l 是同一个对象,先读到的指针会失效。
  const int64_t n = o->len;
  lite::Value *src = o->items;
  reserve(l, l->len + n);
  for (int64_t k = 0; k < n; ++k) l->items[l->len + k] = src[k];
  l->len += n;
}

extern "C" void list_insert(const lite::Value *v, int64_t i, const lite::Value *item) {
  needList(v, "insert()");
  lite::List *l = asList(v);
  // insert 的索引是"夹取"而不是越界报错:超出范围就贴到两端,与 Python 一致
  if (i < 0) i += l->len;
  if (i < 0) i = 0;
  if (i > l->len) i = l->len;
  reserve(l, l->len + 1);
  std::memmove(l->items + i + 1, l->items + i,
               static_cast<size_t>(l->len - i) * sizeof(lite::Value));
  l->items[i] = *item;
  ++l->len;
}

extern "C" int64_t list_index(const lite::Value *v, const lite::Value *item) {
  needList(v, "index()");
  lite::List *l = asList(v);
  for (int64_t i = 0; i < l->len; ++i) {
    if (eq(l->items[i], *item).as.i != 0) return i;
  }
  runtime_error("index(): 列表中找不到该元素");
}

extern "C" int64_t list_count(const lite::Value *v, const lite::Value *item) {
  needList(v, "count()");
  lite::List *l = asList(v);
  int64_t n = 0;
  for (int64_t i = 0; i < l->len; ++i) {
    if (eq(l->items[i], *item).as.i != 0) ++n;
  }
  return n;
}

extern "C" void list_remove(const lite::Value *v, const lite::Value *item) {
  needList(v, "remove()");
  lite::List *l = asList(v);
  for (int64_t i = 0; i < l->len; ++i) {
    if (eq(l->items[i], *item).as.i != 0) {
      std::memmove(l->items + i, l->items + i + 1,
                   static_cast<size_t>(l->len - i - 1) * sizeof(lite::Value));
      --l->len;
      return;
    }
  }
  runtime_error("remove(): 列表中找不到该元素");
}

extern "C" lite::Value list_pop(const lite::Value *v, const lite::Value *idxOrNone) {
  needList(v, "pop()");
  lite::List *l = asList(v);
  if (l->len == 0) runtime_error("pop(): 空列表");
  int64_t i = (idxOrNone->tag == lite::kNull) ? l->len - 1
                                          : to_int(*idxOrNone);
  i = normalize(i, l->len, "列表");
  lite::Value r = l->items[i];
  std::memmove(l->items + i, l->items + i + 1,
               static_cast<size_t>(l->len - i - 1) * sizeof(lite::Value));
  --l->len;
  return r;
}

extern "C" void list_reverse(const lite::Value *v) {
  needList(v, "reverse()");
  lite::List *l = asList(v);
  for (int64_t a = 0, b = l->len - 1; a < b; ++a, --b) {
    lite::Value t = l->items[a];
    l->items[a] = l->items[b];
    l->items[b] = t;
  }
}

extern "C" void list_clear(const lite::Value *v) {
  needList(v, "clear()");
  asList(v)->len = 0;
}

// 朴素插入排序。元素少时足够,而且**稳定**(Python 的 sort 也是稳定的)——
// 交换式排序会破坏这一点,所以这里用"取出再后移插入"的写法。
extern "C" void list_sort(const lite::Value *v) {
  needList(v, "sort()");
  lite::List *l = asList(v);
  for (int64_t i = 1; i < l->len; ++i) {
    lite::Value key = l->items[i];
    int64_t j = i - 1;
    while (j >= 0 && compare_(&l->items[j], &key) > 0) {
      l->items[j + 1] = l->items[j];
      --j;
    }
    l->items[j + 1] = key;
  }
}
