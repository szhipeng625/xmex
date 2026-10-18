// 下标、切片与长度 —— 按 tag 分派到各容器的访问器。
//
// 把这些集中在一处,是为了让"XX 不支持下标""XX 不支持 len()"这类错误只有
// 一个产生点:错误信息统一,而且新增容器类型时不会漏掉某一条路径。
//
// 注意本文件**看不见** xmex::List / xmex::Dict / xmex::Str 的内部布局 —— 那些结构体是各
// 自翻译单元局部的(与 tuple.cpp 同一模式),这里只能组合 list_len 这类
// 导出的访问器。
#include "xmex/runtime.h"

#include <cstdio>
#include <cstring>

namespace {

// 报错主语放在类型上:"'int' 不支持下标访问" 比 "下标访问 不支持 'int'" 好读
[[noreturn]] void badType(const char *what, const xmex::Value *v) {
  static thread_local char buf[160];
  std::snprintf(buf, sizeof(buf), "'%s' 不支持%s", tag_name(v->tag), what);
  runtime_error(buf);
}

// 切片边界:省略(None)时按方向取默认值,给了就归一化并夹到合法区间。
// 这一段是 Python 的 xmex::Slice_GetIndicesEx 语义,负步长下默认值恰好相反 ——
// 这是最容易写错的地方。
void sliceBounds(int64_t len, const xmex::Value *lo, const xmex::Value *hi,
                 const xmex::Value *step, int64_t *start, int64_t *count) {
  int64_t st = (step->tag == xmex::kNull) ? 1 : to_int(*step);
  if (st == 0) runtime_error("切片的步长不能为 0");

  const int64_t lower = (st > 0) ? 0 : -1;
  const int64_t upper = (st > 0) ? len : len - 1;

  int64_t a;
  if (lo->tag == xmex::kNull) {
    a = (st > 0) ? lower : upper;
  } else {
    a = to_int(*lo);
    if (a < 0) a += len;
    if (a < lower) a = lower;
    if (a > upper) a = upper;
  }

  int64_t b;
  if (hi->tag == xmex::kNull) {
    b = (st > 0) ? upper : lower;
  } else {
    b = to_int(*hi);
    if (b < 0) b += len;
    if (b < lower) b = lower;
    if (b > upper) b = upper;
  }

  // 数出会走到多少个元素。用算式而不是循环,免得 [:] 在大容器上退化成 O(n)
  // 的额外一趟。
  int64_t n = 0;
  if (st > 0) {
    if (b > a) n = (b - a + st - 1) / st;
  } else {
    if (a > b) n = (a - b + (-st) - 1) / (-st);
  }

  *start = a;
  *count = n;
}

// 按下标取元素。切片与遍历都要用,所以单独抽出来 —— 但它**不是**对外的
// index_:那一个还要处理字典的按键查找。
xmex::Value elementAt(const xmex::Value *obj, int64_t i) {
  switch (obj->tag) {
    case xmex::kList:  return list_get(obj, i);
    case xmex::kTuple: return tuple_get(obj, i);
    case xmex::kStr:   return str_get(obj, i);
    default:       badType("下标访问", obj);
  }
}

}  // namespace

// 长度。返回 i64 而非 xmex::Value:for 循环的条件要直接拿它比,
// 而 `len(x)` 的调用点自己装箱(见 irgen.cpp)。
extern "C" int64_t len(const xmex::Value *v) {
  switch (v->tag) {
    case xmex::kList:  return list_len(v);
    case xmex::kDict:  return dict_len(v);
    case xmex::kTuple: return tuple_len(v);
    case xmex::kStr:   return str_size(v);
    default:       badType("len()", v);
  }
}

extern "C" xmex::Value index_(const xmex::Value *obj, const xmex::Value *idx) {
  switch (obj->tag) {
    case xmex::kList:
    case xmex::kTuple:
    case xmex::kStr:
      return elementAt(obj, to_int(*idx));
    case xmex::kDict:
      // 字典是按键查找,不是"第 i 个" —— 遍历字典要的是键,走 iter_at
      return dict_get(obj, idx);
    default:
      badType("下标访问", obj);
  }
}

extern "C" void setindex(const xmex::Value *obj, const xmex::Value *idx,
                            const xmex::Value *val) {
  switch (obj->tag) {
    case xmex::kList:
      list_set(obj, to_int(*idx), val);
      return;
    case xmex::kDict:
      dict_set(obj, idx, val);
      return;
    default:
      // 字符串/元组不可变 —— 与 Python 报 TypeError 的判定一致
      badType("下标赋值", obj);
  }
}

extern "C" xmex::Value iter_at(const xmex::Value *obj, int64_t i) {
  // 遍历字典产出的是**键**,不能走 index_(那是按键查找,语义完全不同)
  if (obj->tag == xmex::kDict) return dict_key_at(obj, i);
  return elementAt(obj, i);
}

extern "C" xmex::Value slice(const xmex::Value *obj, const xmex::Value *lo,
                            const xmex::Value *hi, const xmex::Value *step) {
  const int64_t n = len(obj);

  int64_t start = 0, count = 0;
  sliceBounds(n, lo, hi, step, &start, &count);

  const int64_t st = (step->tag == xmex::kNull) ? 1 : to_int(*step);

  if (obj->tag == xmex::kStr) {
    // 步长为 1 时是连续区间,直接一次拷贝;否则逐字符拼。
    const char *data = str_data(obj);
    if (st == 1) return str_new(data + start, count);
    // ⚠️ 显式根:buf 要在随后的 str_new 分配期间活下来
    char *buf = nullptr;
    gc_root_push(reinterpret_cast<void **>(&buf));
    buf = static_cast<char *>(
        gc_alloc(static_cast<size_t>(count) + 1, xmex::kScanNone));
    for (int64_t k = 0; k < count; ++k) buf[k] = data[start + k * st];
    xmex::Value out = str_new(buf, count);
    gc_root_pop(1);
    return out;
  }

  if (obj->tag == xmex::kList || obj->tag == xmex::kTuple) {
    xmex::Value *elems = count > 0 ? static_cast<xmex::Value *>(gc_alloc_values(
                                     static_cast<size_t>(count) * sizeof(xmex::Value), 0))
                               : nullptr;
    for (int64_t k = 0; k < count; ++k) elems[k] = elementAt(obj, start + k * st);
    // 切片的类型跟着原对象:列表切出列表,元组切出元组(与 Python 一致)
    return obj->tag == xmex::kList ? list_new(elems, count)
                               : tuple_new(elems, count);
  }

  badType("切片", obj);
}
