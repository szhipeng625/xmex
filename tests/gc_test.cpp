// 垃圾回收的专项测试。
//
// 三条前提决定了这里的断言写法:
//
//   1. **不断言精确的回收数字。** 保守扫描 + 栈上残留(旧栈槽、非易失寄存器快照)
//      决定了"某个具体对象必然被回收"是**不可断言**的 —— 它的指针可能还躺在
//      某个地方。只能断言不等式与守恒量。
//   2. **必须靠"下毒"才有检出能力。** 运行时在回收时把块填成 0xDD,否则
//      use-after-free 会读到"看起来还挺对"的旧内容。
//   3. **造垃圾的函数必须 noinline,** 否则它的栈帧可能还在扫描区间里,
//      把本该回收的对象留住,断言就成了碰运气。
//
// 最高价值的是第三组(不误回收):任何一个块的扫描方式标错,那里就会读到毒字节。
#include "pylite/runtime.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const std::string &what) {
  std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what.c_str());
  if (!cond) ++g_failures;
}

std::string rep(lite::Value v) {
  lite::Value s = repr(&v);
  return std::string(str_data(&s), static_cast<size_t>(str_size(&s)));
}

void checkRep(lite::Value v, const std::string &want, const std::string &what) {
  const std::string got = rep(v);
  if (got == want) {
    std::printf("  [ok]   %s → %s\n", what.c_str(), got.c_str());
  } else {
    std::printf("  [FAIL] %s → 得到 %s,期望 %s\n", what.c_str(), got.c_str(),
                want.c_str());
    ++g_failures;
  }
}

lite::Value str(const char *s) {
  return str_new(s, static_cast<int64_t>(std::char_traits<char>::length(s)));
}

lite::Value list(std::initializer_list<lite::Value> xs) {
  std::vector<lite::Value> v(xs);
  return list_new(v.data(), static_cast<int64_t>(v.size()));
}

// 消耗内存的垃圾:函数一返回,它的栈帧就离开扫描区间了
__attribute__((noinline)) void makeGarbage(int count, int size) {
  std::vector<char> blob(static_cast<size_t>(size), 'x');
  for (int i = 0; i < count; ++i) {
    lite::Value s = str_new(blob.data(), size);
    (void)s;
  }
}

// 造一个环:x 引用 y,y 引用 x。引用计数会在这里泄漏,标记-清除不会。
__attribute__((noinline)) void makeCycle() {
  lite::Value x = list_new(nullptr, 0);
  lite::Value y = list_new(nullptr, 0);
  list_append(&x, &y);
  list_append(&y, &x);
}

// 反复造短命对象,中途**不做任何显式回收** —— 验的是阈值自动触发。
// 每轮丢掉字符串和列表,存活量应当稳定在阈值量级,而不是随累计分配量线性上涨。
__attribute__((noinline)) void churn(int rounds, int size) {
  std::vector<char> blob(static_cast<size_t>(size), 'y');
  for (int i = 0; i < rounds; ++i) {
    lite::Value s = str_new(blob.data(), size);
    lite::Value l = list_new(nullptr, 0);
    list_append(&l, &s);
    (void)l;
  }
}

// 深一层:列表套列表
__attribute__((noinline)) lite::Value makeNested() {
  lite::Value inner = list({str("aaaa"), str("bbbb")});
  lite::Value tail = str("cccc");
  lite::Value outer = list_new(nullptr, 0);
  list_append(&outer, &inner);
  list_append(&outer, &tail);
  return outer;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // 崩了也能看到进度

  std::printf("=== GC 测试 ===\n\n");

  lite::GcStats mode;
  gc_stats(&mode);
  // 干跑模式只下毒、不归还内存,所以"回收了多少字节""存活块数下降"这两类
  // 断言在它下面不成立 —— 它要验的是另一件事:内容有没有被毒坏。
  const bool reclaims = (mode.dry_run == 0) && (mode.disabled == 0);
  if (!reclaims) {
    std::printf("  (当前是 %s 模式,与内存归还有关的断言跳过;"
                "内容完整性断言照常跑)\n\n",
                mode.disabled ? "PYLITE_GC_OFF" : "PYLITE_GC_DRYRUN");
  }

  // ---------------------------------------------------------- 守恒律
  std::printf("-- 守恒律(内部自洽,恒成立)--\n");
  {
    lite::GcStats st;
    gc_stats(&st);
    check(st.live_bytes + st.freed_bytes_total == st.alloc_bytes_total,
          "live + freed == alloc");
    std::printf("      分配 %lld / 回收 %lld / 存活 %lld 字节,存活 %lld 块\n",
                static_cast<long long>(st.alloc_bytes_total),
                static_cast<long long>(st.freed_bytes_total),
                static_cast<long long>(st.live_bytes),
                static_cast<long long>(st.live_blocks));
  }

  // ------------------------------------------------------ 真的回收了
  std::printf("\n-- 真的回收了 --\n");
  {
    const int kCount = 64, kSize = 1 << 20;   // 64 × 1MB
    lite::GcStats before, after;
    gc_stats(&before);

    makeGarbage(kCount, kSize);   // 函数返回后,这些字符串没有任何引用
    gc_collect();

    gc_stats(&after);
    const int64_t freedDelta = after.freed_bytes_total - before.freed_bytes_total;
    const int64_t made = static_cast<int64_t>(kCount) * kSize;

    std::printf("      造了约 %lld 字节垃圾,回收了 %lld 字节\n",
                static_cast<long long>(made), static_cast<long long>(freedDelta));
    // 不断言"全部回收":栈上残留的寄存器快照可能留住最后几个。
    // 断言"至少回收一半" —— 粒度是 1MB,不会被小对象噪声淹没。
    if (reclaims) {
      check(freedDelta >= made / 2, "至少回收了一半的垃圾");
    }
    // 守恒律在**任何**模式下都必须成立(干跑也要照实记账)
    check(after.live_bytes + after.freed_bytes_total == after.alloc_bytes_total,
          "守恒律成立");
  }

  // ------------------------------------------------ 不误回收(最高价值)
  std::printf("\n-- 不误回收:只留一条根,内容必须完好 --\n");
  {
    // 列表:items 数组若被标成 SCAN_NONE,元素会被回收,这里就读到 0xDD
    lite::Value xs = list({str("aaaa"), str("bbbb")});
    gc_collect();
    checkRep(xs, "['aaaa', 'bbbb']", "列表:元素数组被正确扫描");

    // 元组:单元格从偏移 8 开始,标错偏移整条链就错位
    lite::Value tupElems[2] = {str("aaaa"), str("bbbb")};
    lite::Value tup = tuple_new(tupElems, 2);
    gc_collect();
    checkRep(tup, "('aaaa', 'bbbb')", "元组:单元格偏移 8");

    // 字典:keys 与 vals 是两个独立的块
    lite::Value keys[1] = {str("kk")};
    lite::Value vals[1] = {str("vvvv")};
    lite::Value d = dict_new(keys, vals, 1);
    gc_collect();
    checkRep(d, "{'kk': 'vvvv'}", "字典:keys/vals 两个数组都被扫描");

    // 切片:结果列表的 elems 数组
    lite::Value lo = int_(0), hi = int_(2), noStep = none_();
    lite::Value sliced = slice(&xs, &lo, &hi, &noStep);
    gc_collect();
    checkRep(sliced, "['aaaa', 'bbbb']", "切片:结果数组被正确扫描");

    // 列表套列表:层与层之间靠保守扫描自动传递
    lite::Value nested = makeNested();
    gc_collect();
    checkRep(nested, "[['aaaa', 'bbbb'], 'cccc']", "嵌套列表:逐层可达");
  }

  // ------------------------------------------------------------ 环
  std::printf("\n-- 环(引用计数会在这里泄漏)--\n");
  {
    lite::GcStats before, after;
    // 先造一轮,让堆长起来,避免把"首次分配"的噪声算进来
    makeCycle();
    gc_collect();
    gc_stats(&before);

    for (int i = 0; i < 50; ++i) makeCycle();
    gc_stats(&after);
    check(after.live_blocks > before.live_blocks, "50 个环确实占用着堆");

    gc_collect();
    lite::GcStats swept;
    gc_stats(&swept);
    std::printf("      回收前 %lld 块 → 回收后 %lld 块\n",
                static_cast<long long>(after.live_blocks),
                static_cast<long long>(swept.live_blocks));
    // 干跑不把块从表里摘掉,所以块数不会降 —— 但它照样把那些块判死并下毒:
    // 如果真的误判成"活",同一次压测里早就读到毒字节了。
    if (reclaims) {
      check(swept.live_blocks < after.live_blocks, "环被回收了(标记-清除能处理环)");
    }
  }

  // ------------------------------------------------ 自动触发(阈值)
  std::printf("\n-- 自动回收:阈值触发,存活量有界 --\n");
  {
    lite::GcStats before, after;
    gc_stats(&before);

    const int kRounds = 400, kSize = 256 * 1024;   // 约 100MB 累计分配
    churn(kRounds, kSize);                          // 全程不显式回收

    gc_stats(&after);
    const int64_t made = after.alloc_bytes_total - before.alloc_bytes_total;
    std::printf("      本轮累计分配约 %lld MB,回收后存活 %lld MB\n",
                static_cast<long long>(made >> 20),
                static_cast<long long>(after.live_bytes >> 20));

    // 自动触发在"关闭回收"之外的所有模式下都该发生(含干跑)
    if (!mode.disabled) {
      check(after.collections > before.collections, "分配量越过阈值后自动触发了回收");
    }
    // 但这句说的是"内存有没有真的还回去",干跑模式按定义不还,所以要分开判
    if (reclaims) {
      check(after.live_bytes < made / 4, "存活量远小于累计分配量(内存没有只增不还)");
    }
  }

  // ------------------------------------------------- 手动入口与幂等
  std::printf("\n-- 手动回收 --\n");
  {
    lite::GcStats a, b;
    gc_collect();
    gc_stats(&a);
    gc_collect();          // 连着收两次不应出错
    gc_stats(&b);
    check(a.collections + 1 == b.collections, "每次调用计数 +1");
    if (reclaims) {
      check(b.live_bytes <= a.live_bytes, "重复回收不会把存活量收上去");
    }
  }

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
