// 垃圾回收 —— 保守式标记-清除。
//
// 取代了原先只增不还的 bump 分配器(见 git 历史里的 runtime/arena.cpp)。
// 关于"为什么是保守式而不是引用计数",docs/language.md 里有完整论述;这里只记
// 实现要点:
//
//   * 只有 str/list/dict/tuple 是堆对象,int/float/bool/None 是内联标量、根本
//     不分配(见 include/xmex/value.h)。所以堆上全是容器,规模有限。
//   * 根 = **保守扫描本线程的栈与寄存器**,外加一个显式根栈。
//     扫栈之所以行得通,靠的是 IRGen 的既有约定:每个 xmex::Value 都住在栈上的
//     alloca 里,而且槽位地址几乎都会传给 DLL 里的不透明运行时函数 ——
//     于是 LLVM 的 mem2reg/SROA 提升不掉它们,值必须留在内存里。
//   * 不为每种容器写 tracer:xmex::List 的 items、xmex::Dict 的 keys/vals 都是指向**另外
//     的块**,保守扫描容器对象就能自动沿指针找到它们。以后加新容器类型不用改这里。
//
// ⚠️ 本文件是自举的:GC 内部任何路径都**不得**调用 gc_alloc / alloc。
// 块表、排序索引、标记栈一律用 malloc/realloc。
#include "xmex/runtime.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// 块头放在 payload **前面**。size 与 GCD 都取 16 的倍数对齐后,malloc 返回
// 16 字节对齐时 payload 必然是 16 字节对齐的 —— xmex::Value 要求 alignof == 8,
// 而 xmex::Str 的 data 紧跟在 int64 之后,所以 16 对齐比最低要求更宽裕。
struct alignas(16) GcBlock {
  GcBlock *next;        // 块表(单链表,头插,O(1))
  size_t size;          // payload 字节数(16 对齐后)
  uint32_t kind;        // xmex::GcScan
  uint32_t mark;        // 标记位
  uint32_t cellOffset;  // SCAN_VALUES 的单元格起始偏移(元组是 8,其余是 0)
  uint32_t refcount;    // 强引用计数(共享指针语义)
  uint32_t weakcount;   // 弱引用计数
  uint32_t destroyed;   // 强引用归零后置 1:内容已失效,控制块可能仍被弱引用观察
  uint32_t pad;         // 补到 16 的倍数,保持 payload 仍然 16 字节对齐
};

// 块表。不用 vector:保持 GC 完全不依赖 STL。
GcBlock **g_table = nullptr;
size_t g_tableLen = 0;
size_t g_tableCap = 0;

// 统计
int64_t g_allocBytes = 0;   // 历史累计分配(含块头)
int64_t g_freedBytes = 0;   // 历史累计回收
int64_t g_liveBytes = 0;    // 当前存活字节
int64_t g_collections = 0;

void tablePush(GcBlock *b) {
  if (g_tableLen == g_tableCap) {
    const size_t cap = g_tableCap ? g_tableCap * 2 : 256;
    auto **nt = static_cast<GcBlock **>(std::realloc(g_table, cap * sizeof(GcBlock *)));
    if (nt == nullptr) runtime_error("内存耗尽(GC 块表)");
    g_table = nt;
    g_tableCap = cap;
  }
  g_table[g_tableLen++] = b;
}

void tableRemoveAt(size_t i) {
  g_table[i] = g_table[--g_tableLen];
}

int64_t g_sinceGc = 0;        // 自上次回收以来分配的字节
int64_t g_lastLiveBytes = 0;  // 上次回收后的存活字节(用于自适应阈值)
bool g_inCollect = false;     // 回收期间禁止重入

// --- 环境变量,读一次就缓存 ---------------------------------------------
// 沿用项目已有的 XMEX_NO_OPT=1 风格(见 aot/compiler.cpp)。
struct Options {
  bool off = false;        // 完全关闭回收,只增不还(对照用)
  bool stress = false;     // 每次分配都回收(压测用)
  bool stats = false;      // 退出时打印统计
  bool dryRun = false;     // 只标记 + 对未标记块下毒,**不真正 free**
  int64_t threshold = 0;   // 显式阈值,0 表示按存活量自适应
  bool loaded = false;
};

Options g_opt;

bool envOn(const char *name) {
  const char *v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

void reportAtExit();
void maybeCollect();   // 定义在下面的匿名命名空间里

void loadOptions() {
  if (g_opt.loaded) return;
  g_opt.loaded = true;
  g_opt.off = envOn("XMEX_GC_OFF");
  g_opt.stress = envOn("XMEX_GC_STRESS");
  g_opt.stats = envOn("XMEX_GC_STATS");
  g_opt.dryRun = envOn("XMEX_GC_DRYRUN");
  if (const char *t = std::getenv("XMEX_GC_THRESHOLD")) {
    g_opt.threshold = std::strtoll(t, nullptr, 10);
  }
  // 统计要打到进程退出为止,所以注册在这里(第一次分配时)而不是要求调用方
  // 记得先调 gc_init —— 那样太容易漏。
  if (g_opt.stats) std::atexit(reportAtExit);
}

void printStats(const char *why) {
  std::fprintf(stderr,
               "[gc] %s: 累计分配 %lld 字节,累计回收 %lld,存活 %lld 字节 / %zu 块,"
               "回收次数 %lld\n",
               why, static_cast<long long>(g_allocBytes),
               static_cast<long long>(g_freedBytes),
               static_cast<long long>(g_liveBytes), g_tableLen,
               static_cast<long long>(g_collections));
}

void reportAtExit() {
  if (g_opt.stats) printStats("退出");
}

}  // namespace

extern "C" void *gc_alloc(size_t n, xmex::GcScan kind) {
  loadOptions();

  // ⚠️ 顺序钉死:**先回收、再取块**。反过来的话,刚拿到但还没被任何人引用的
  // 新块会在同一次分配返回前被自己收掉。
  maybeCollect();

  // 16 字节对齐:xmex::Value 需要 alignof == 8,字符串的 data 紧跟 int64,
  // 16 对齐比最低要求更宽裕,也让"指针 16 对齐"这类廉价过滤成为可能。
  const size_t size = (n + 15) & ~static_cast<size_t>(15);

  if (kind == xmex::kScanValues && (size % 16) != 0) {
    // size 已经向上取整到 16 的倍数,这里只可能在溢出等病态情况下触发
    runtime_error("GC 内部错误:SCAN_VALUES 的长度必须是 16 的倍数");
  }

  auto *b = static_cast<GcBlock *>(std::malloc(sizeof(GcBlock) + size));
  if (b == nullptr) runtime_error("内存耗尽");
  b->next = nullptr;
  b->size = size;
  b->kind = static_cast<uint32_t>(kind);
  b->mark = 0;
  b->cellOffset = 0;
  b->refcount = 0;    // 0 表示尚无强引用登记,交给标记-清除
  b->weakcount = 0;
  b->destroyed = 0;
  b->pad = 0;

  tablePush(b);

  const int64_t charged = static_cast<int64_t>(sizeof(GcBlock) + size);
  g_allocBytes += charged;
  g_liveBytes += charged;
  g_sinceGc += charged;

  return reinterpret_cast<char *>(b) + sizeof(GcBlock);
}

extern "C" void *gc_alloc_values(size_t n, size_t firstCellOffset) {
  // 先按 VALUES 分配再补 cellOffset:gc_alloc 只校验长度,
  // 偏移的合法性在这里查,报错信息能带上实际值。
  void *p = gc_alloc(n, xmex::kScanValues);

  const size_t size = (n + 15) & ~static_cast<size_t>(15);
  // 只要求偏移按 8 对齐、且落在块内。**不要求余下的字节正好是整数个单元格**:
  // 内存已经向上取整到 16,元组(s {int64 len; xmex::Value items[]})在 n=0 时
  // size=16、偏移 8,尾部那半个格子是块内的已分配内存,按 tag 读一下无害
  // (tag 判定的方向是"只跳过明确是标量的",最坏也只是多保留)。
  if ((firstCellOffset % 8) != 0 || firstCellOffset > size) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "GC 内部错误:SCAN_VALUES 的单元格偏移 %zu 不合法(块长 %zu)",
                  firstCellOffset, size);
    runtime_error(buf);
  }

  // 块头就在 payload 前面,回退一步改掉 cellOffset
  auto *b = reinterpret_cast<GcBlock *>(static_cast<char *>(p) - sizeof(GcBlock));
  b->cellOffset = static_cast<uint32_t>(firstCellOffset);
  return p;
}

// --- 兼容旧 ABI ---------------------------------------------------------
// alloc / arena_reset 是公开头里的历史符号,用户手写 C++ 会直接调,
// 签名不能改(见 include/xmex/runtime.h)。转发时按最保守的方式扫描 ——
// 宁可多保留,不可漏根。
extern "C" void *alloc(size_t n) {
  return gc_alloc(n, xmex::kScanWords);
}
// --- 引用计数(共享指针 / 弱指针语义) -----------------------------------
// 计数位记在**块头**里,对象自身数据布局与 xmex::Value 布局都不动 —— 不触碰
// ABI 契约,已编译的目标文件不受影响。
//
// 生命周期与 std::shared_ptr / std::weak_ptr 对齐:
//   * 强引用(refcount)归零         -> 对象内容失效(destroyed=1)并下毒
//   * 强引用归零且已无弱引用       -> 此时才真正归还内存
//   * 弱引用(weakcount)归零        -> 若对象早已失效,此刻归还内存
//
// ⚠️ refcount 为 0 表示"尚无强引用登记":此时减引用不做任何事,交回标记-清除。
// 于是即使生成代码没有配套插入计数调用,行为也只是退回原来的回收方式,
// 绝不会误释放仍然活着的对象。
//
// ⚠️ 强引用归零时**不递归递减子对象**:深链递归会爆 C 栈(同类故障见
// docs/llvm-notes.md 第 2.2 节),而各容器的真实布局只在各自的翻译单元里,
// 这里看不到。子对象交给标记-清除按"从根可达"回收,不会泄漏。
namespace {
inline GcBlock *blockOf(void *payload) {
  return reinterpret_cast<GcBlock *>(static_cast<char *>(payload) - sizeof(GcBlock));
}
// 从块表摘除并归还内存
void releaseBlock(GcBlock *b) {
  for (size_t i = 0; i < g_tableLen; ++i) {
    if (g_table[i] != b) continue;
    const int64_t charged = static_cast<int64_t>(sizeof(GcBlock) + b->size);
    g_liveBytes -= charged;
    g_freedBytes += charged;
    tableRemoveAt(i);
    std::free(b);
    return;
  }
}
// 对象内容失效:只下毒、不归还内存(控制块可能仍被弱引用观察)
void destroyPayload(GcBlock *b) {
  b->destroyed = 1;
  b->refcount = 0;
  std::memset(reinterpret_cast<char *>(b) + sizeof(GcBlock), 0xDD, b->size);
}
}  // namespace
extern "C" void incref(void *payload) {
  if (payload == nullptr) return;
  ++blockOf(payload)->refcount;
}
extern "C" void decref(void *payload) {
  if (payload == nullptr) return;
  GcBlock *b = blockOf(payload);
  if (b->refcount == 0) return;   // 未登记强引用,交给标记-清除
  if (--b->refcount > 0) return;
  // 强引用归零:内容立即失效。没有弱引用在观察时才归还内存。
  if (b->weakcount == 0) {
    releaseBlock(b);
  } else {
    destroyPayload(b);
  }
}
// 弱引用:只观察对象存活,不影响其生命周期
extern "C" void weakref(void *payload) {
  if (payload == nullptr) return;
  ++blockOf(payload)->weakcount;
}
extern "C" void weak_release(void *payload) {
  if (payload == nullptr) return;
  GcBlock *b = blockOf(payload);
  if (b->weakcount == 0) return;
  if (--b->weakcount > 0) return;
  // 最后一个弱引用也走了:若对象早已失效,此刻归还内存
  if (b->destroyed) releaseBlock(b);
}
// 弱引用升级为强引用:对象已失效返回 0,否则加引用并返回 1。
// 绝不返回悬空指针 —— 要么拿到有效对象,要么明确失败。
extern "C" int32_t weak_upgrade(void *payload) {
  if (payload == nullptr) return 0;
  GcBlock *b = blockOf(payload);
  if (b->destroyed || b->refcount == 0) return 0;
  ++b->refcount;
  return 1;
}
// 弱引用是否仍指向活对象
extern "C" int32_t weak_alive(void *payload) {
  if (payload == nullptr) return 0;
  GcBlock *b = blockOf(payload);
  return (b->destroyed == 0 && b->refcount > 0) ? 1 : 0;
}
// 读取强引用计数(供内建函数与测试使用)
extern "C" int64_t refcount(void *payload) {
  if (payload == nullptr) return 0;
  return blockOf(payload)->refcount;
}

// ---------------------------------------------------------------------------
// 标记
// ---------------------------------------------------------------------------

namespace {

// 按地址有序的区间表,每次回收重建一次。标记阶段用它做
// "这个指针落在哪个块里"的二分查找 —— 因此不需要页表、size class 或
// VirtualAlloc 保留区。块表本身是链表(插入 O(1)),排序只在回收时做一次。
struct Span {
  uintptr_t start;
  uintptr_t end;
  GcBlock *block;
};

Span *g_spans = nullptr;
size_t g_spanLen = 0;
size_t g_spanCap = 0;

int spanCmp(const void *a, const void *b) {
  const auto *x = static_cast<const Span *>(a);
  const auto *y = static_cast<const Span *>(b);
  if (x->start < y->start) return -1;
  if (x->start > y->start) return 1;
  return 0;
}

void buildSpans() {
  if (g_tableLen > g_spanCap) {
    const size_t cap = g_tableLen * 2;
    auto *ns = static_cast<Span *>(std::realloc(g_spans, cap * sizeof(Span)));
    if (ns == nullptr) runtime_error("内存耗尽(GC 区间表)");
    g_spans = ns;
    g_spanCap = cap;
  }
  for (size_t i = 0; i < g_tableLen; ++i) {
    char *p = reinterpret_cast<char *>(g_table[i]) + sizeof(GcBlock);
    g_spans[i] = {reinterpret_cast<uintptr_t>(p),
                  reinterpret_cast<uintptr_t>(p) + g_table[i]->size, g_table[i]};
  }
  g_spanLen = g_tableLen;
  std::qsort(g_spans, g_spanLen, sizeof(Span), spanCmp);
}

// 地址落在哪个块里。允许**指向块内部的指针**(字符串的 data 就是块内部的地址)。
GcBlock *findBlock(uintptr_t addr) {
  size_t lo = 0, hi = g_spanLen;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (g_spans[mid].start <= addr) lo = mid + 1;
    else hi = mid;
  }
  if (lo == 0) return nullptr;
  const Span &s = g_spans[lo - 1];
  return (addr < s.end) ? s.block : nullptr;
}

// 标记待处理栈。**显式栈,不用递归** —— 递归会以对象图深度消耗 C 栈,
// 与 docs/llvm-notes.md 第 2.2 节那次循环 alloca 的故障是同一类。
GcBlock **g_work = nullptr;
size_t g_workLen = 0;
size_t g_workCap = 0;

void workPush(GcBlock *b) {
  if (g_workLen == g_workCap) {
    const size_t cap = g_workCap ? g_workCap * 2 : 256;
    auto **nw = static_cast<GcBlock **>(std::realloc(g_work, cap * sizeof(GcBlock *)));
    if (nw == nullptr) runtime_error("内存耗尽(GC 标记栈)");
    g_work = nw;
    g_workCap = cap;
  }
  g_work[g_workLen++] = b;
}

// 候选指针:命中块且未标记就标记并压栈。
// **先标记再压栈**,这样同一个块不会被重复压入导致栈爆。
void markCandidate(uintptr_t w) {
  if (w == 0) return;
  GcBlock *b = findBlock(w);
  if (b == nullptr || b->mark) return;
  b->mark = 1;
  workPush(b);
}

void scanWordRange(uintptr_t lo, uintptr_t hi) {
  lo &= ~static_cast<uintptr_t>(7);  // 向下取整到 8 字节对齐
  for (uintptr_t p = lo; p + 8 <= hi; p += 8) {
    uintptr_t w;
    std::memcpy(&w, reinterpret_cast<const void *>(p), 8);
    markCandidate(w);
  }
}

// --- 显式根栈 -----------------------------------------------------------
// 运行时内部的 C++ 局部变量**不在** IRGen 的 alloca 保证范围内(那条只覆盖
// 生成代码),而且它们不在任何容器里,所以"保守扫描自动沿指针找"也帮不上忙。
// 只能靠编译器恰好把值 spill 到栈上 —— 那是"每十万次分配中一次"的静默内存损坏。
// 取局部变量地址这个动作会迫使该值留在内存里,再配合显式登记就成了硬契约。
void **g_roots = nullptr;
size_t g_rootLen = 0;
size_t g_rootCap = 0;

// 栈上界。
// Windows: x64 上 GS 指向 TEB,NT_TIB.StackBase 在 gs:0x08。
// Linux: 用 pthread_getattr_np 获取栈基址。
#ifdef __linux__
#include <pthread.h>
uintptr_t stackBaseFromTeb() {
  pthread_attr_t attr;
  void *stackaddr = nullptr;
  size_t stacksize = 0;
  pthread_getattr_np(pthread_self(), &attr);
  pthread_attr_getstack(&attr, &stackaddr, &stacksize);
  pthread_attr_destroy(&attr);
  // 栈在高地址向低地址增长，基址 = 栈顶 + 栈大小
  return reinterpret_cast<uintptr_t>(stackaddr) + stacksize;
}
#else
uintptr_t stackBaseFromTeb() {
  uintptr_t v;
  asm volatile("movq %%gs:0x08, %0" : "=r"(v));
  return v;
}
#endif

void markRoots() {
  jmp_buf jb;
  // 只把 setjmp 当"把非易失寄存器溢写到 jb 这块栈内存"的手段。
  //
  // 已核实本机 D:\msys64\ucrt64\include\setjmp.h 的 _JUMP_BUFFER 布局恰好是
  // Rbx/Rsi/Rdi/R12-R15/Rsp/Rbp/Rip 等全部非易失寄存器,jmp_buf 是 256 字节
  // 的 POD、必然在栈上、必然落在下面的扫描区间里(实测七个寄存器逐个验证过)。
  //
  // 这个 if 的分支**永远不会成立** —— 本文件里没有任何地方调 longjmp。
  // "返回两次"的 CFG 处理来自函数声明上的 returns_twice 属性,与我们读不读
  // 返回值无关,所以写成 if 只是把"这个调用有第二次返回路径"这件事写明白。
  if (setjmp(jb) != 0) std::abort();  // 不可能到达

  volatile char anchor[16];
  (void)anchor;

  uintptr_t a = reinterpret_cast<uintptr_t>(&jb);
  uintptr_t b = reinterpret_cast<uintptr_t>(const_cast<char *>(anchor));
  uintptr_t lo = a < b ? a : b;
  lo -= 64;  // 保险:本帧里可能还有比这两个更靠下的局部

  // ⚠️ 上界用 StackBase,**绝不能用 StackLimit** —— Windows 栈按需 commit,
  // 从 StackLimit 往上扫会碰到未提交页 → 访问违例。
  const uintptr_t hi = stackBaseFromTeb();
  if (hi > lo) scanWordRange(lo, hi);

  // 每个登记项是一个"指向局部指针变量"的地址,读出它当前指向的堆对象
  for (size_t i = 0; i < g_rootLen; ++i) {
    const void *p = *reinterpret_cast<void *const *>(g_roots[i]);
    markCandidate(reinterpret_cast<uintptr_t>(p));
  }
}

void drainWorklist() {
  while (g_workLen > 0) {
    GcBlock *blk = g_work[--g_workLen];
    const char *p = reinterpret_cast<const char *>(blk) + sizeof(GcBlock);
    const size_t size = blk->size;

    switch (static_cast<xmex::GcScan>(blk->kind)) {
      case xmex::kScanNone:
        // 纯字节缓冲(字符串内容、临时 char 缓冲),里面不可能有指针
        break;

      case xmex::kScanWords:
        // 保守逐字:容器对象头靠这条照到 items / keys / vals 三个指针。
        // 不需要为每种容器写 tracer —— 指针指向的那个块会继续被扫。
        for (size_t off = 0; off + 8 <= size; off += 8) {
          uintptr_t w;
          std::memcpy(&w, p + off, 8);
          markCandidate(w);
        }
        break;

      case xmex::kScanValues:
        // 按 xmex::Value 单元格走,tag 决定要不要跟随 payload。
        //
        // ⚠️ 判定方向:只把**明确是标量**的 tag(xmex::kNull/xmex::kBool/xmex::kInt/
        // xmex::kFloat,即 0..3)跳过,其余(含非法 tag、含未初始化的垃圾)一律跟随。
        // 反过来的"只跟随 4 种指针类型"一旦漏一种就是 use-after-free;
        // 这个方向最坏只是多保留。负数 tag 强转成无符号会变成大数 → 不跳过,
        // 正是安全的那一侧。
        for (size_t off = blk->cellOffset; off + 16 <= size; off += 16) {
          int32_t tag;
          std::memcpy(&tag, p + off, 4);
          if (static_cast<uint32_t>(tag) <= static_cast<uint32_t>(xmex::kFloat)) continue;
          uintptr_t w;
          std::memcpy(&w, p + off + 8, 8);
          markCandidate(w);
        }
        break;
    }
  }
}

void poisonBlock(GcBlock *b) {
  // 下毒这一步不能省:malloc/free 会立刻复用内存,use-after-free 常常
  // "看起来是对的"。填成 0xDD 能让读已释放对象确定性地变成错误 repr 或崩溃。
  std::memset(reinterpret_cast<char *>(b) + sizeof(GcBlock), 0xDD, b->size);
}

void sweepBlocks() {
  for (size_t i = 0; i < g_tableLen;) {
    GcBlock *b = g_table[i];
    if (b->mark) {
      b->mark = 0;
      ++i;
      continue;
    }
    // 有弱引用正在观察:不归还内存,只标记内容失效并下毒。
    // 这样弱引用仍能读到"对象已失效"这个确定状态,而不会指向已释放地址;
    // 最后一个弱引用释放时(refcount 已为 0 的路径)才真正归还内存。
    if (b->weakcount > 0) {
      b->destroyed = 1;
      b->refcount = 0;   // 已不可达,强引用视为失效
      poisonBlock(b);
      ++i;
      continue;
    }
    poisonBlock(b);

    if (g_opt.dryRun) {
      // 干跑:只下毒、不 free。任何"丢根"会立刻表现为读到自己下毒的字节,
      // 而内存从不归还分配器,所以不会叠加"复用已释放内存"这类干扰因素。
      //
      // 记账也**不动**:块还在表里,下次回收还会扫到它、又一次被下毒。
      // 若在这里减 liveBytes / 加 freedBytes,下一次就会重复扣减 ——
      // 守恒律(两边同步变化)掩盖了这种重复计数,别被它骗过去。
      ++i;
    } else {
      const int64_t charged = static_cast<int64_t>(sizeof(GcBlock) + b->size);
      g_liveBytes -= charged;
      g_freedBytes += charged;
      tableRemoveAt(i);   // 与末尾元素互换后覆盖,所以不用先清 g_table[i]
      std::free(b);
    }
  }
}

int64_t currentThreshold() {
  if (g_opt.threshold > 0) return g_opt.threshold;
  if (g_opt.stress) return 0;
  // 自适应:至少 1MB,且不低于上次存活量的两倍 —— 避免"堆一涨就狂收"
  const int64_t adaptive = g_lastLiveBytes * 2;
  return adaptive > (1 << 20) ? adaptive : (1 << 20);
}

void maybeCollect() {
  if (g_opt.off || g_inCollect) return;
  if (g_tableLen == 0) return;  // 空堆没什么可收
  if (g_sinceGc < currentThreshold()) return;
  gc_collect();
}

}  // namespace

extern "C" void gc_collect() {
  loadOptions();
  if (g_inCollect) return;   // 防重入:回收期间不做嵌套回收
  g_inCollect = true;

  buildSpans();
  markRoots();
  drainWorklist();
  sweepBlocks();

  g_sinceGc = 0;
  g_lastLiveBytes = g_liveBytes;
  ++g_collections;

  if (g_opt.stats) {
    std::fprintf(stderr,
                 "[gc] 第 %lld 次回收:存活 %lld 字节 / %zu 块,本次回收 %lld 字节\n",
                 static_cast<long long>(g_collections),
                 static_cast<long long>(g_liveBytes), g_tableLen,
                 static_cast<long long>(
                     g_allocBytes - g_freedBytes - g_liveBytes));
  }

  g_inCollect = false;
}

extern "C" void arena_reset() {
  // 原本是给 REPL 用的"整块丢弃"。语义改成"强制回收一次":
  // 在 GC 下"丢弃全部内存"是做不到的 —— 栈上还有活着的引用。
  gc_collect();
}

extern "C" void gc_stats(xmex::GcStats *out) {
  if (out == nullptr) return;
  out->alloc_bytes_total = g_allocBytes;
  out->freed_bytes_total = g_freedBytes;
  out->live_bytes = g_liveBytes;
  out->live_blocks = static_cast<int64_t>(g_tableLen);
  out->collections = g_collections;
  loadOptions();
  out->dry_run = g_opt.dryRun ? 1 : 0;
  out->disabled = g_opt.off ? 1 : 0;
}

extern "C" void gc_root_push(void **slot) {
  if (g_rootLen == g_rootCap) {
    const size_t cap = g_rootCap ? g_rootCap * 2 : 32;
    auto **nr = static_cast<void **>(std::realloc(g_roots, cap * sizeof(void *)));
    if (nr == nullptr) runtime_error("内存耗尽(GC 根栈)");
    g_roots = nr;
    g_rootCap = cap;
  }
  g_roots[g_rootLen++] = slot;
}

extern "C" void gc_root_pop(size_t n) {
  // 只减不释放:根栈的上限很小(运行时里同时活着的裸指针不超过十个),
  // 留着复用比反复 realloc 划算。
  g_rootLen -= (n <= g_rootLen) ? n : g_rootLen;
}

extern "C" void gc_init() { loadOptions(); }
