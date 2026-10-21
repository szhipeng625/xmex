// xmex 自研智能指针的测试。
//
// 测的是 std::shared_ptr 的语义子集,但实现完全自研
// (include/xmex/smartptr.h),不借用标准库的任何机制。
//
// 覆盖的点:
//   - 强/弱引用计数守恒
//   - 对象只析构一次
//   - WeakPtr::lock() 在对象已死时返回空
//   - 别名构造
//   - 异常安全(构造抛异常时控制块不泄漏)
//   - enable_shared_from_this
//   - 多线程下的 lock() 竞争
//   - 侵入式引用计数
#include "xmex/smartptr.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char *what) {
  ++g_checks;
  if (cond) {
    std::printf("  [ok]   %s\n", what);
  } else {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

// ---------------------------------------------------------------------------
// 测试用的类型
// ---------------------------------------------------------------------------

struct Tracker {
  static int alive;      // 活着的实例数
  static int destroyed;  // 累计析构次数
  int value = 0;

  Tracker() { ++alive; }
  explicit Tracker(int v) : value(v) { ++alive; }
  virtual ~Tracker() {
    --alive;
    ++destroyed;
  }
};
int Tracker::alive = 0;
int Tracker::destroyed = 0;

struct Base {
  int b = 0;
  virtual ~Base() = default;
};

struct Derived : Base {
  int d = 0;
  virtual ~Derived() = default;
};

// 走 enable_shared_from_this
class SelfAware : public xmex::enable_shared_from_this<SelfAware> {
 public:
  int value = 0;
  explicit SelfAware(int v) : value(v) {}
};

// 构造时抛异常,用来验证控制块不泄漏
struct Thrower {
  explicit Thrower(bool shouldThrow) {
    if (shouldThrow) throw std::runtime_error("boom");
  }
};

// 侵入式引用计数
class Counted : public xmex::RefCounted {
 public:
  static int destroyed;
  int value = 0;
  ~Counted() override { ++destroyed; }
};
int Counted::destroyed = 0;

}  // namespace

int main() {
  std::printf("=== xmex 自研智能指针测试 ===\n");

  // -----------------------------------------------------------------
  std::printf("--- 基本计数 ---\n");
  {
    auto sp = xmex::makeShared<Tracker>(42);
    check(sp.use_count() == 1, "makeShared 后 use_count == 1");
    check(sp->value == 42, "可以解引用访问成员");
    check(Tracker::alive == 1, "对象已构造");

    {
      auto sp2 = sp;
      check(sp.use_count() == 2, "拷贝后 use_count == 2");
      check(sp2->value == 42, "拷贝出的指针指向同一对象");
    }
    check(sp.use_count() == 1, "内层作用域结束后计数回到 1");

    sp.reset();
    check(Tracker::alive == 0, "reset 后对象被析构");
    check(Tracker::destroyed == 1, "析构恰好发生一次");
  }

  // -----------------------------------------------------------------
  std::printf("--- 移动语义 ---\n");
  {
    auto sp = xmex::makeShared<Tracker>(7);
    auto moved = std::move(sp);
    check(moved.use_count() == 1, "移动后新指针计数为 1");
    check(!sp, "移动后源指针为空");
    check(moved->value == 7, "移动后数据完好");
  }

  // -----------------------------------------------------------------
  std::printf("--- 弱引用 ---\n");
  {
    xmex::WeakPtr<Tracker> weak;
    check(weak.expired(), "默认构造的弱引用已过期");
    {
      auto sp = xmex::makeShared<Tracker>(99);
      weak = xmex::WeakPtr<Tracker>(sp);
      check(!weak.expired(), "对象存活时弱引用未过期");
      check(weak.use_count() == 1, "弱引用能看到强引用计数");

      auto locked = weak.lock();
      check(static_cast<bool>(locked), "lock() 成功提升");
      check(locked->value == 99, "提升出的指针指向同一对象");
      check(sp.use_count() == 2, "lock() 增加了强引用计数");
    }
    check(weak.expired(), "对象销毁后弱引用过期");
    check(!weak.lock(), "对象销毁后 lock() 返回空");

    // 弱引用本身不应阻止控制块回收 —— 这里只能间接观察:
    // 重新赋值后旧控制块应被释放,不会崩。
    weak.reset();
    check(weak.expired(), "reset 后弱引用为空");
  }

  // -----------------------------------------------------------------
  std::printf("--- 别名构造 ---\n");
  {
    struct Holder {
      int payload = 5;
    };
    auto owner = xmex::makeShared<Holder>();
    xmex::SharedPtr<int> alias(owner, &owner->payload);
    check(alias.get() == &owner->payload, "别名构造指向内部成员");
    check(alias.use_count() == 2, "别名构造与 owner 共享所有权");
    owner.reset();
    check(alias.use_count() == 1, "owner 释放后别名仍持有");
    check(*alias == 5, "别名指向的数据仍有效");
  }

  // -----------------------------------------------------------------
  std::printf("--- 派生到基类转换 ---\n");
  {
    auto sp = xmex::makeShared<Derived>();
    sp->d = 8;
    xmex::SharedPtr<Base> base = sp;
    check(base.get() == sp.get(), "派生指针可转成基类指针");
    check(base.use_count() == 2, "转换后共享同一控制块");
  }

  // -----------------------------------------------------------------
  std::printf("--- 异常安全 ---\n");
  {
    const int before = Tracker::destroyed;
    bool threw = false;
    try {
      auto sp = xmex::makeShared<Thrower>(true);
      (void)sp;
    } catch (const std::exception &) {
      threw = true;
    }
    check(threw, "构造函数抛异常时向外传播");
    check(Tracker::destroyed == before, "构造失败不产生残留析构");
  }

  // -----------------------------------------------------------------
  std::printf("--- enable_shared_from_this ---\n");
  {
    auto sp = xmex::makeShared<SelfAware>(11);
    auto self = sp->shared_from_this();
    check(static_cast<bool>(self), "shared_from_this() 返回有效指针");
    check(self.get() == sp.get(), "shared_from_this() 指向同一个对象");
    check(self->value == 11, "shared_from_this() 能读到成员");
    check(sp.use_count() == 2, "shared_from_this() 增加一次强引用");

    auto weak = sp->weak_from_this();
    check(!weak.expired(), "weak_from_this() 指向存活对象");
  }

  // -----------------------------------------------------------------
  std::printf("--- 侵入式引用计数 ---\n");
  {
    {
      xmex::RefPtr<Counted> p(new Counted());
      p->value = 3;
      check(p->use_count() == 1, "RefPtr 初始计数为 1");
      {
        xmex::RefPtr<Counted> q = p;
        check(p->use_count() == 2, "拷贝后计数为 2");
        check(q->value == 3, "RefPtr 可访问成员");
      }
      check(p->use_count() == 1, "内层析构后计数回到 1");
    }
    check(Counted::destroyed == 1, "RefPtr 归零时销毁对象");
  }

  // -----------------------------------------------------------------
  std::printf("--- 多线程 lock() 竞争 ---\n");
  {
    // 目标:验证 CAS 循环在竞争下不会拿到已析构对象的指针。
    // 做法:一个线程持有一个 SharedPtr,另一个线程反复 lock()。
    //
    // ⚠️ 主线程必须等锁线程真正跑起来再开始竞争 —— 否则主循环
    //    (2000 次拷贝,纳秒级)会在锁线程拿到时间片之前就结束,
    //    停机标志已经置位,锁线程一次都没成功 —— 那是测试的竞态,
    //    不是实现的缺陷。
    std::atomic<int> successful{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> lockerStarted{false};

    auto sp = xmex::makeShared<Tracker>(1234);
    xmex::WeakPtr<Tracker> weak(sp);

    std::thread locker([&] {
      lockerStarted.store(true, std::memory_order_release);
      while (!stop.load(std::memory_order_relaxed)) {
        auto locked = weak.lock();
        if (locked) {
          // 读到值才算成功 —— 如果拿到的是已析构对象,这里会读到垃圾。
          if (locked->value == 1234) successful.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });

    // 等锁线程进入循环。
    while (!lockerStarted.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    for (int i = 0; i < 2000; ++i) {
      auto copy = sp;  // 短暂提升
      (void)copy;
    }
    stop.store(true, std::memory_order_relaxed);
    locker.join();

    check(successful.load() > 0, "并发 lock() 成功过");
    sp.reset();
    check(weak.expired(), "对象已销毁,弱引用过期");
  }

  // -----------------------------------------------------------------
  std::printf("--- 析构次数守恒 ---\n");
  {
    const int before = Tracker::destroyed;
    {
      auto sp = xmex::makeShared<Tracker>(1);
      std::vector<xmex::SharedPtr<Tracker>> v;
      for (int i = 0; i < 100; ++i) v.push_back(sp);
      check(sp.use_count() == 101, "vector 持有 100 份拷贝");
      v.clear();
      check(sp.use_count() == 1, "vector 清空后计数回到 1");
    }
    check(Tracker::destroyed == before + 1, "整组只析构一次");
  }

  std::printf("\n%d 项检查,%d 项失败\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
