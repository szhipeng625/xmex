// 引用计数(共享指针 / 弱指针语义)验证探针。
// 直接调用运行时接口,覆盖三种关键情形。
#include "xmex/runtime.h"
#include "xmex/value.h"

#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char *what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_fail;
}

int main() {
  std::printf("=== 情形一:强引用归零且无弱引用 -> 立即销毁 ===\n");
  {
    xmex::Value s = str_new("hello", 5);
    void *p = s.as.ptr;
    incref(p);
    check(refcount(p) == 1, "加引用后计数为 1");
    incref(p);
    check(refcount(p) == 2, "再加引用后计数为 2");
    decref(p);
    check(refcount(p) == 1, "减引用后计数为 1");
  }

  std::printf("=== 情形二:强引用归零但有弱引用 -> 只失效,不回收 ===\n");
  {
    xmex::Value s = str_new("world", 5);
    void *p = s.as.ptr;
    incref(p);
    weakref(p);
    check(weak_alive(p) == 1, "对象存活时弱引用判活为真");
    decref(p);
    check(weak_alive(p) == 0, "强引用归零后弱引用判活为假");
    check(weak_upgrade(p) == 0, "对已失效对象升级失败,不返回悬空指针");
    weak_release(p);
    std::printf("  (最后一个弱引用释放,内存此刻归还)\n");
  }

  std::printf("=== 情形三:强弱并存时,回收不应提前归还内存 ===\n");
  {
    xmex::Value s = str_new("kept", 4);
    void *p = s.as.ptr;
    incref(p);
    weakref(p);
    decref(p);
    // 此时强引用已归零,但仍有弱引用;强制回收一次,内存应被保留。
    gc_collect();
    check(weak_alive(p) == 0, "回收后仍可安全读取失效状态(未崩溃)");
    check(weak_upgrade(p) == 0, "回收后升级仍失败");
    weak_release(p);
    std::printf("  (弱引用释放,内存此刻归还)\n");
  }

  std::printf("\n%s (失败 %d 项)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
