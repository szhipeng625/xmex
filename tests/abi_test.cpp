// Stage 0 门槛之一:证明 C++(GCC)与运行时 DLL 之间的 xmex::Value ABI 自洽。
//
// 这个测试不做任何 LLVM 的事 —— 它只回答一个问题:
// "16 字节的 xmex::Value 按值跨 DLL 边界传递,能不能正常工作?"
// 如果这个都过不了,后面 LLVM 生成的代码更不可能过。
#include "xmex/runtime.h"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void check(bool cond, const char *what) {
  if (cond) {
    std::printf("  [ok]   %s\n", what);
  } else {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

}  // namespace

int main() {
  std::printf("=== xmex::Value ABI test ===\n");

  // 布局契约
  check(sizeof(xmex::Value) == 16, "sizeof(xmex::Value) == 16");
  check(alignof(xmex::Value) == 8, "alignof(xmex::Value) == 8");
  check(offsetof(xmex::Value, as) == 8, "payload 位于偏移 8");

  // tag 值必须与 IRGen 里硬编码的常量一致
  check(xmex::kNull == 0 && xmex::kBool == 1 && xmex::kInt == 2 && xmex::kFloat == 3 &&
            xmex::kStr == 4 && xmex::kList == 5 && xmex::kDict == 6 && xmex::kTuple == 7,
        "xmex::Tag 取值与 IRGen 常量一致");

  // 构造 + 取值往返
  check(as_int(int_(42)) == 42, "int_ 往返");
  check(as_float(float_(2.5)) == 2.5, "float_ 往返");
  check(as_bool(bool_(true)), "bool_(true) 往返");
  check(!as_bool(bool_(false)), "bool_(false) 往返");

  // 跨 DLL 调用的按值传参/返回 —— 这是本测试的真正目的
  xmex::Value s = add(int_(1), int_(2));
  check(s.tag == xmex::kInt && s.as.i == 3, "add(int 1, int 2) == 3 (跨 DLL 按值传递)");

  xmex::Value f = add(float_(1.5), float_(2.25));
  check(f.tag == xmex::kFloat && f.as.f == 3.75, "add(1.5, 2.25) == 3.75");

  xmex::Value mixed = add(int_(1), float_(0.5));
  check(mixed.tag == xmex::kFloat && mixed.as.f == 1.5, "int+float 提升为 float");

  xmex::Value neg = sub(int_(10), int_(4));
  check(neg.as.i == 6, "sub(10, 4) == 6");

  xmex::Value prod = mul(int_(6), int_(7));
  check(prod.as.i == 42, "mul(6, 7) == 42");

  // 谓词返回 i32
  check(truthy(int_(1)) == 1, "truthy(1) == 1 (返回 i32)");
  check(truthy(int_(0)) == 0, "truthy(0) == 0");
  check(truthy(none_()) == 0, "truthy(None) == 0");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
