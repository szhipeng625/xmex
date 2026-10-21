// xmex 自研的引用计数智能指针。
//
// ⚠️ 为什么不用 std::shared_ptr:
//   本文件刻意**不依赖** std::shared_ptr / std::weak_ptr 的任何机制 ——
//   控制块、引用计数、弱引用提升、别名构造、一次性分配全部自己实现。
//   理由有三:
//     1. std::shared_ptr 的控制块类型是实现细节,不同 libstdc++ 版本之间
//        不保证二进制兼容。本项目要把运行时编成 DLL 给 C++ 宿主用,
//        宿主和 DLL 可能用不同的工具链 —— 那种情况下 std::shared_ptr
//        跨模块传递是未定义行为。
//     2. 本项目需要**可审计**的生命周期语义。GC(runtime/gc.cpp)那边的
//        引用计数是手写的、带"未登记强引用则交回标记-清除"这种特殊
//        约定的;再引入一个语义不透明的 std 容器会让两套规则打架。
//     3. 想要"一次分配"(控制块与对象同处一块内存)。这是 makeShared
//        相对 make_shared 的真实优势:省一次 malloc,且控制块与对象
//        落在同一批缓存行上,引用计数变动时不会多一次 cache miss。
//
// 四种所有权形态:
//   SharedPtr<T>  —— 共享所有权,带弱引用(WeakPtr<T>)可观察存活状态
//   UniquePtr<T>  —— 独占所有权,可移动,不可拷贝
//   RefCounted    —— 侵入式基类,对象自己带计数(零额外分配)
//   RefPtr<T>     —— 指向 RefCounted 派生类的轻量指针
//
// 线程安全:
//   引用计数是原子的,所以 SharedPtr 的**不同实例**可以在不同线程上
//   增减同一个对象的引用。但同一个 SharedPtr 实例本身不是线程安全的
//   (和 std::shared_ptr 一样)—— 共享一个实例变量需要外部同步。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace xmex {

template <class T>
class SharedPtr;

template <class T>
class WeakPtr;

template <class T>
class enable_shared_from_this;

namespace detail {

// ---------------------------------------------------------------------------
// 控制块
// ---------------------------------------------------------------------------
//
// 控制块只管两个原子计数,不管被管理对象的类型 —— 这样强引用和弱引用
// 可以指向同一个控制块而不需要模板体操。
//
// 两个计数的语义:
//   strong —— 强引用数。对象还活着时 >= 1,归零时**被管理对象被销毁**。
//   weak   —— 弱引用数。**含 1 个由控制块自己持有的隐式弱引用**。
//              所以 strong 归零 ≠ 控制块被回收:对象死了,但只要还有
//              弱引用活着,控制块就得留着(弱引用要能回答"它死过没有")。
//              最后一个弱引用消失时,连同隐式那个一起归零,控制块才被回收。
class RefCountBase {
 public:
  // 强引用数。对象还活着时 >= 1。
  std::atomic<std::uint32_t> strong{1};
  // 弱引用数(含 1 个隐式弱引用)。控制块活着时 >= 1。
  std::atomic<std::uint32_t> weak{1};

  void* object() const noexcept { return object_; }

  void retainStrong() noexcept {
    strong.fetch_add(1, std::memory_order_relaxed);
  }
  void retainWeak() noexcept {
    weak.fetch_add(1, std::memory_order_relaxed);
  }

  // 释放一个强引用。返回 true 表示"对象刚刚在这里被销毁"。
  //
  // ⚠️ 顺序不能反:先销毁对象,再放掉隐式弱引用。反过来的话,对象的
  // 析构函数里如果还持有指向自己的 WeakPtr,那个 WeakPtr 的析构会
  // 访问一个计数已经归零的控制块。
  bool releaseStrong() noexcept {
    if (strong.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      destroyObject();
      releaseWeakInternal();
      return true;
    }
    return false;
  }

  // 释放一个弱引用。返回 true 表示"控制块刚刚在这里被回收"。
  bool releaseWeak() noexcept { return releaseWeakInternal(); }

 protected:
  RefCountBase() = default;
  virtual ~RefCountBase() = default;

  // 销毁被管理的对象,但**不**释放控制块自身的内存。
  virtual void destroyObject() noexcept = 0;
  // 释放控制块自身的内存。调用后 this 立刻失效。
  virtual void destroyBlock() noexcept = 0;

  void* object_ = nullptr;

 private:
  bool releaseWeakInternal() noexcept {
    if (weak.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      destroyBlock();
      return true;
    }
    return false;
  }
};

// 默认删除器:delete p。
template <class T>
struct DefaultDeleter {
  void operator()(T* p) const noexcept { delete p; }
};

// 控制块 + 被管理对象**分开**分配。用于"接管一个已存在的裸指针"。
template <class T, class D>
class ManagedBlock final : public RefCountBase {
 public:
  ManagedBlock(T* p, D d) : deleter_(std::move(d)) { object_ = p; }

 protected:
  void destroyObject() noexcept override {
    deleter_(static_cast<T*>(object_));
    object_ = nullptr;
  }
  void destroyBlock() noexcept override { delete this; }

 private:
  D deleter_;
};

// 控制块 + 被管理对象**同处一块内存**。makeShared 走这条路径。
//
// ⚠️ 对象放在原始存储里(而不是直接写成成员),是因为 destroyObject
// 要能显式调用 T 的析构函数;如果 T 是直接成员,那之后再析构 FusedBlock
// 就会二次析构。用原始存储 + 显式 placement new / 显式析构可以避免。
template <class T>
class FusedBlock final : public RefCountBase {
 public:
  template <class... A>
  explicit FusedBlock(void* allocBase, A&&... args) : allocBase_(allocBase) {
    object_ = static_cast<void*>(&storage_);
    ::new (static_cast<void*>(&storage_)) T(std::forward<A>(args)...);
  }

  void* objectPtr() const noexcept { return object_; }

 protected:
  void destroyObject() noexcept override {
    static_cast<T*>(object_)->~T();
    object_ = nullptr;
  }
  void destroyBlock() noexcept override {
    // 用 allocBase_ 而不是 this:operator delete 要求的必须**当初传给
    // operator new 的那个指针**。虽然 RefCountBase 是唯一基类、偏移为 0,
    // 理论上 this == allocBase_,但依赖"唯一基类偏移为 0"这种隐含约定
    // 太脆 —— 基类顺序一变就是 UB。这里显式记下来。
    void* p = allocBase_;
    this->~FusedBlock();
    ::operator delete(p, std::align_val_t(alignof(FusedBlock)));
  }

 private:
  alignas(T) unsigned char storage_[sizeof(T)];
  void* object_ = nullptr;
  void* allocBase_ = nullptr;
};

// 标记:接管已有控制块(计数已由调用方处理好,不增减)。
struct AdoptTag {};
// 标记:登记一个真实的弱引用(会 +1 弱引用计数)。
struct AttachTag {};

// --- enable_shared_from_this 的钩子 ----------------------------------------
//
// 检测 T 是否继承自 enable_shared_from_this<X>。若是,在创建 SharedPtr
// 时把控制块登记到对象内部,对象就能 later 用 shared_from_this()
// 拿回一个共享自己所有权的 SharedPtr。
//
// 用嵌套 typedef xmex_self_type 而不是重载解析来检测,是因为要同时
// 拿到基类 X 的类型 —— 有了它才能做**正确的**派生到基类转换。
template <class T, class = void>
struct SelfHook {
  template <class U>
  static void apply(U*, RefCountBase*) {}
};

template <class T>
struct SelfHook<T, std::void_t<typename T::xmex_self_type>> {
  static void apply(T* p, RefCountBase* b) {
    // ⚠️ 模板参数必须是最派生类自己。这不是"限制",是**正确性要求**:
    //    存进弱引用的那个指针以后要当 T* 用并被 lock() 出来,若 X 是
    //    基类而非 T,基类子对象的地址和 T 的地址不同,类型就是错的。
    //    宁可编译期报错,也不能在运行期存一个会算错地址的指针。
    static_assert(std::is_same_v<typename T::xmex_self_type, T>,
                  "enable_shared_from_this<T> 的模板参数必须是最派生类自己"
                  "(例如 class Foo : public xmex::enable_shared_from_this<Foo>)");
    p->__xmex_attach_self(b, static_cast<void*>(p));
  }
};

}  // namespace detail

// ---------------------------------------------------------------------------
// enable_shared_from_this
// ---------------------------------------------------------------------------
//
// 用法: class Foo : public xmex::enable_shared_from_this<Foo> { ... };
// 之后 Foo::shared_from_this() 返回一个 SharedPtr<Foo>。
//
// ⚠️ 约束:模板参数必须是**最派生类**自己(Foo 而不是基类 Base)。
//    这是刻意的 —— 模板参数决定了 shared_from_this() 返回什么类型的
//    指针,以及基类子对象地址怎么算。多继承下基类地址不唯一,
//    含糊的语义比不支持更危险。
template <class T>
class enable_shared_from_this {
 public:
  // ⚠️ 定义在 SharedPtr 之后(见文件末尾)。这里只声明 ——
  //    enable_shared_from_this 出现在 SharedPtr 之前,而它要返回
  //    SharedPtr<T>,那时 SharedPtr 还是不完整类型,无法定义成员函数体。
  SharedPtr<T> shared_from_this();
  WeakPtr<T> weak_from_this() const noexcept { return weak_this_; }

  // 内部使用:由 SharedPtr 在创建时调用。
  void __xmex_attach_self(detail::RefCountBase* b, void* obj) {
    // 走"从已有控制块造一个弱引用"的正规路径,而不是绕过计数的
    // 构造 —— attach_self 让 weak_this_ 持有一个真实的弱引用,对象的
    // 析构函数里它随之消失,那时才真正放掉一个弱引用计数。
    weak_this_ = WeakPtr<T>(detail::AttachTag{}, b, static_cast<T*>(obj));
  }

  using xmex_self_type = T;

 private:
  mutable WeakPtr<T> weak_this_;
};

// ---------------------------------------------------------------------------
// SharedPtr
// ---------------------------------------------------------------------------
template <class T>
class SharedPtr {
 public:
  using element_type = T;

  SharedPtr() noexcept = default;
  SharedPtr(std::nullptr_t) noexcept {}

  // 接管裸指针(用默认删除器 delete p)。
  template <class U,
            class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  SharedPtr(U* p) : SharedPtr(p, detail::DefaultDeleter<U>()) {}

  // 接管裸指针 + 自定义删除器。
  template <class U, class D,
            class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  SharedPtr(U* p, D d) : ptr_(p) {
    if (p != nullptr) {
      block_ = new detail::ManagedBlock<U, D>(p, std::move(d));
      detail::SelfHook<U>::apply(p, block_);
    }
  }

  // 转换构造:SharedPtr<Derived> -> SharedPtr<Base>。
  template <class U,
            class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  SharedPtr(const SharedPtr<U>& o) noexcept
      : ptr_(o.get()), block_(o.controlBlock()) {
    if (block_ != nullptr) block_->retainStrong();
  }

  SharedPtr(const SharedPtr& o) noexcept : ptr_(o.ptr_), block_(o.block_) {
    if (block_ != nullptr) block_->retainStrong();
  }

  SharedPtr(SharedPtr&& o) noexcept : ptr_(o.ptr_), block_(o.block_) {
    o.ptr_ = nullptr;
    o.block_ = nullptr;
  }

  // 别名构造:与 o 共享所有权,但指向 p。
  //
  // 用途:一个对象的所有权在别处,但你想通过 SharedPtr 暴露它的一部分
  // (比如"持有 owner 的同时指向 owner 内部的某个 buffer")。
  //
  // ⚠️ p 必须在 o 的对象**存活期间**有效 —— SharedPtr 保不住 p 的
  //    生命周期,只保得住 o 的。析构时销毁的仍是 o 原本管理的对象。
  //
  // 注意这里对 U **没有**可转换性要求:别名构造的用途正是"用 A 的
  // 所有权去指 B 的内部",两者类型本就无关(比如用 Holder 的所有权
  // 指向 Holder 里的一个 int 字段)。要求可转换会让这种用法编不过。
  template <class U>
  SharedPtr(const SharedPtr<U>& o, T* p) noexcept
      : ptr_(p), block_(o.controlBlock()) {
    if (block_ != nullptr) block_->retainStrong();
  }

  SharedPtr& operator=(const SharedPtr& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      block_ = o.block_;
      if (block_ != nullptr) block_->retainStrong();
    }
    return *this;
  }

  SharedPtr& operator=(SharedPtr&& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      block_ = o.block_;
      o.ptr_ = nullptr;
      o.block_ = nullptr;
    }
    return *this;
  }

  SharedPtr& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  ~SharedPtr() { reset(); }

  T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  // 当前强引用数。用来诊断"是不是多了一份拷贝"。
  long use_count() const noexcept {
    return block_ == nullptr
               ? 0
               : static_cast<long>(block_->strong.load(std::memory_order_relaxed));
  }

  // 内部:与 WeakPtr 互访。
  detail::RefCountBase* controlBlock() const noexcept { return block_; }

  void reset() noexcept {
    if (block_ == nullptr) return;
    detail::RefCountBase* b = block_;
    ptr_ = nullptr;
    block_ = nullptr;
    b->releaseStrong();
  }

  void swap(SharedPtr& o) noexcept {
    std::swap(ptr_, o.ptr_);
    std::swap(block_, o.block_);
  }

 private:
  // 接管已有控制块(计数已由调用方处理)。只给 WeakPtr::lock 用。
  SharedPtr(detail::AdoptTag, detail::RefCountBase* b, T* p) noexcept
      : ptr_(p), block_(b) {}

  template <class U>
  friend class WeakPtr;
  // makeShared 是自由函数,需要用私有构造接管刚建好的控制块。
  template <class U, class... A>
  friend SharedPtr<U> makeShared(A&&... args);

  T* ptr_ = nullptr;
  detail::RefCountBase* block_ = nullptr;
};

// makeShared —— 一次分配。
//
// 控制块与对象同处一块内存:比"先 new 对象再 new 控制块"少一次
// malloc,少一次指针跳转,而且两者落在相邻的缓存行上 —— 引用计数
// 增减时不会额外 miss 一次 cache。
template <class T, class... Args>
SharedPtr<T> makeShared(Args&&... args) {
  using Block = detail::FusedBlock<T>;

  // 手动分配而不是写 `new Block(...)`:Block 可能过度对齐
  // (alignas(T) 的成员会抬高整个 Block 的对齐要求),而
  // `new Block(...)` 只在 sizeof(Block) 超过默认 new 对齐时才用
  // 对齐版 operator new —— 那会让这里的 delete 形式对不上。
  // 统一走显式的 align_val_t 版本,分配与释放严格配对。
  void* mem = ::operator new(sizeof(Block), std::align_val_t(alignof(Block)));
  Block* blk = nullptr;
  try {
    blk = new (mem) Block(mem, std::forward<Args>(args)...);
  } catch (...) {
    ::operator delete(mem, std::align_val_t(alignof(Block)));
    throw;
  }

  SharedPtr<T> sp(detail::AdoptTag{}, blk, static_cast<T*>(blk->objectPtr()));
  detail::SelfHook<T>::apply(static_cast<T*>(blk->objectPtr()), blk);
  return sp;
}

// ---------------------------------------------------------------------------
// WeakPtr
// ---------------------------------------------------------------------------
//
// 只观察,不拥有。对象死了它还活着,用来回答"它还在吗"。
template <class T>
class WeakPtr {
 public:
  using element_type = T;

  WeakPtr() noexcept = default;
  WeakPtr(std::nullptr_t) noexcept {}

  template <class U,
            class = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  WeakPtr(const SharedPtr<U>& o) noexcept
      : ptr_(o.get()), block_(o.controlBlock()) {
    if (block_ != nullptr) block_->retainWeak();
  }

  WeakPtr(const WeakPtr& o) noexcept : ptr_(o.ptr_), block_(o.block_) {
    if (block_ != nullptr) block_->retainWeak();
  }

  WeakPtr(WeakPtr&& o) noexcept : ptr_(o.ptr_), block_(o.block_) {
    o.ptr_ = nullptr;
    o.block_ = nullptr;
  }

  WeakPtr& operator=(const WeakPtr& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      block_ = o.block_;
      if (block_ != nullptr) block_->retainWeak();
    }
    return *this;
  }

  WeakPtr& operator=(WeakPtr&& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      block_ = o.block_;
      o.ptr_ = nullptr;
      o.block_ = nullptr;
    }
    return *this;
  }

  ~WeakPtr() { reset(); }

  // 尝试提升为强引用。对象已死则返回空。
  //
  // ⚠️ 必须用 CAS 循环,不能先读 strong 再无条件 +1:
  //    读到的 1 可能正好是最后一个强引用,那个线程随时会把它减到 0
  //    并销毁对象。此时若无脑 +1,拿到的就是**已经析构过的对象**的指针
  //    —— use-after-free。CAS 失败时 strong 会被自动刷新,循环重试即可。
  SharedPtr<T> lock() const noexcept {
    detail::RefCountBase* b = block_;
    if (b == nullptr) return SharedPtr<T>();

    std::uint32_t count = b->strong.load(std::memory_order_acquire);
    for (;;) {
      if (count == 0) return SharedPtr<T>();  // 已经死了
      if (b->strong.compare_exchange_weak(count, count + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
        return SharedPtr<T>(detail::AdoptTag{}, b, ptr_);
      }
      // count 已被 compare_exchange_weak 刷新成当前真实值,继续循环。
    }
  }

  bool expired() const noexcept {
    if (block_ == nullptr) return true;
    return block_->strong.load(std::memory_order_acquire) == 0;
  }

  long use_count() const noexcept {
    return block_ == nullptr
               ? 0
               : static_cast<long>(block_->strong.load(std::memory_order_relaxed));
  }

  void reset() noexcept {
    if (block_ == nullptr) return;
    detail::RefCountBase* b = block_;
    ptr_ = nullptr;
    block_ = nullptr;
    b->releaseWeak();
  }

  void swap(WeakPtr& o) noexcept {
    std::swap(ptr_, o.ptr_);
    std::swap(block_, o.block_);
  }

 private:
  // 从已有控制块造一个弱引用。**必须**真的持有一个弱引用计数,
  // 否则对象的析构函数里 weak_this_ 消失时不会放掉计数,控制块泄漏。
  WeakPtr(detail::AttachTag, detail::RefCountBase* b, T* p) noexcept
      : ptr_(p), block_(b) {
    if (block_ != nullptr) block_->retainWeak();
  }

  template <class U>
  friend class SharedPtr;
  template <class U>
  friend class enable_shared_from_this;

  T* ptr_ = nullptr;
  detail::RefCountBase* block_ = nullptr;
};

// ---------------------------------------------------------------------------
// enable_shared_from_this 的成员函数定义
// ---------------------------------------------------------------------------
//
// 放在这里是因为它要返回 SharedPtr<T> —— enable_shared_from_this 本身
// 声明在 SharedPtr 之前,那时 SharedPtr 不完整,无法定义函数体。
template <class T>
inline SharedPtr<T> enable_shared_from_this<T>::shared_from_this() {
  return weak_this_.lock();
}

// ---------------------------------------------------------------------------
// UniquePtr
// ---------------------------------------------------------------------------
//
// 独占所有权。可移动、不可拷贝。适合"一定只有一个主人"的资源:
// 文件句柄、CUDA stream、编译期临时对象。
template <class T, class D = detail::DefaultDeleter<T>>
class UniquePtr {
 public:
  using element_type = T;

  UniquePtr() noexcept = default;
  UniquePtr(std::nullptr_t) noexcept {}
  explicit UniquePtr(T* p) noexcept : ptr_(p) {}
  UniquePtr(T* p, D d) : ptr_(p), deleter_(std::move(d)) {}

  UniquePtr(UniquePtr&& o) noexcept
      : ptr_(o.ptr_), deleter_(std::move(o.deleter_)) {
    o.ptr_ = nullptr;
  }

  UniquePtr& operator=(UniquePtr&& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      deleter_ = std::move(o.deleter_);
      o.ptr_ = nullptr;
    }
    return *this;
  }

  UniquePtr(const UniquePtr&) = delete;
  UniquePtr& operator=(const UniquePtr&) = delete;

  ~UniquePtr() { reset(); }

  T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  // 放弃所有权,把裸指针交出去(调用者负责之后怎么处置)。
  T* release() noexcept {
    T* p = ptr_;
    ptr_ = nullptr;
    return p;
  }

  void reset(T* p = nullptr) noexcept {
    if (ptr_ != nullptr) deleter_(ptr_);
    ptr_ = p;
  }

  void swap(UniquePtr& o) noexcept {
    std::swap(ptr_, o.ptr_);
    deleter_.swap(o.deleter_);
  }

 private:
  T* ptr_ = nullptr;
  D deleter_{};
};

template <class T, class... Args>
UniquePtr<T> makeUnique(Args&&... args) {
  return UniquePtr<T>(new T(std::forward<Args>(args)...));
}

// ---------------------------------------------------------------------------
// 侵入式引用计数
// ---------------------------------------------------------------------------
//
// 对象自己带一个计数,指针本身不占额外内存 —— 适合"对象已经有一堆成员
// 和虚函数,再套一层控制块不划算"的场景。代价:必须继承本基类。
//
// ⚠️ 虚析构是必须的 —— release() 归零时执行 delete this,而静态类型
//    是基类指针,没有虚析构就会只跑基类析构、派生部分的资源全泄漏。
class RefCounted {
 public:
  void retain() const noexcept {
    count_.fetch_add(1, std::memory_order_relaxed);
  }

  // 归零时删除自身。
  void release() const noexcept {
    if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }

  long use_count() const noexcept {
    return static_cast<long>(count_.load(std::memory_order_relaxed));
  }

 protected:
  RefCounted() = default;
  virtual ~RefCounted() = default;

 private:
  mutable std::atomic<std::uint32_t> count_{1};
};

// 指向 RefCounted 派生类的指针。
template <class T>
class RefPtr {
 public:
  using element_type = T;

  RefPtr() noexcept = default;
  RefPtr(std::nullptr_t) noexcept {}

  // 接管一个新建的对象(计数从 1 开始)。
  explicit RefPtr(T* p) noexcept : ptr_(p) {}

  RefPtr(const RefPtr& o) noexcept : ptr_(o.ptr_) {
    if (ptr_ != nullptr) ptr_->retain();
  }

  RefPtr(RefPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }

  RefPtr& operator=(const RefPtr& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      if (ptr_ != nullptr) ptr_->retain();
    }
    return *this;
  }

  RefPtr& operator=(RefPtr&& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = o.ptr_;
      o.ptr_ = nullptr;
    }
    return *this;
  }

  ~RefPtr() { reset(); }

  T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  long use_count() const noexcept {
    return ptr_ == nullptr ? 0 : ptr_->use_count();
  }

  void reset() noexcept {
    if (ptr_ == nullptr) return;
    T* p = ptr_;
    ptr_ = nullptr;
    p->release();
  }

  void swap(RefPtr& o) noexcept { std::swap(ptr_, o.ptr_); }

 private:
  T* ptr_ = nullptr;
};

}  // namespace xmex
