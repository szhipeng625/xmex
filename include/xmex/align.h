// Xmex 内存对齐模型
//
// 这个文件回答三个问题:
//
//   1. xmex 里的"对齐"到底由什么保证?  —— 分级常量 + 一处统一分配入口。
//   2. 为什么要分级?                       —— 不同的消费者要的粒度不同,
//      全部按 4096 对齐会让分配器浪费内存,全部按 8 对齐会让 SIMD 慢。
//   3. 怎么用对齐来省 IO 时间?             —— 对齐缓冲 + 缓冲池 + 批量写。
//
// 与既有代码的关系:
//   include/xmex/value.h 里 xmex::Value 的 alignof 是 8(ABI 契约),
//   runtime/gc.cpp 的保守 GC 按 8 字节步长扫栈 —— 所以 kAlign8 是硬下限,
//   不是优化建议。本文件的所有缓冲区都至少满足 kAlign8。
//
// 不依赖 std::shared_ptr / std::vector 之外的设施,只依赖 <cstdio>/<mutex>。
#pragma once

#include "xmex/platform.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#if defined(XMEX_WINDOWS)
  #include <malloc.h>
#endif

namespace xmex {

// ===========================================================================
// 对齐级别
//
// kAlign8   —— xmex::Value 的 ABI 对齐(runtime/gc.cpp 按 8 字节步长扫栈),
//              这是"能被 GC 扫到"的最低要求。
// kAlign16  —— SSE 寄存器宽度。x86 的 movaps 要求 16 字节对齐,
//              未对齐的 movaps 会触发 #GP 异常而不是慢一点。
// kAlign32  —— AVX 寄存器宽度。vmovups 之类不要求,但 ymm 载入快。
// kAlign64  —— 缓存行(x86-64 上是 64 字节)。两个线程写同一缓存行的
//              不同变量会产生伪共享(false sharing),吞吐掉一半。
// kAlign4K  —— 页面大小。配合 mmap 可以做到按页归还给 OS。
//
// 选级原则:取"消费者要求的最大值",不要无脑 4096。
// ===========================================================================
inline constexpr std::size_t kAlign1 = 1;
inline constexpr std::size_t kAlign8 = 8;
inline constexpr std::size_t kAlign16 = 16;
inline constexpr std::size_t kAlign32 = 32;
inline constexpr std::size_t kAlign64 = 64;
inline constexpr std::size_t kAlign4K = 4096;

// 默认对齐:64 字节。覆盖绝大多数现代 x86-64 的缓存行粒度,
// 又不会让小缓冲浪费太多。
inline constexpr std::size_t kDefaultAlign = kAlign64;

// 批量 IO 的默认缓冲大小。64KB 是传统上的 L1/L2 之间比较合适的
// 批量大小,再大收益递减(超过管道缓冲后写 syscall 就不再被合并)。
inline constexpr std::size_t kDefaultBatchSize = 64 * 1024;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

// n 是不是 2 的幂。0 视为不是。
inline constexpr bool isPowerOfTwo(std::size_t n) {
  return n != 0 && (n & (n - 1)) == 0;
}

// 把 n 向上取整到 alignment 的倍数。alignment 必须是 2 的幂。
inline constexpr std::size_t alignUp(std::size_t n, std::size_t alignment) {
  return (n + alignment - 1) & ~(alignment - 1);
}

// p 相对 alignment 的偏移。
inline constexpr std::size_t alignOffset(const void *p, std::size_t alignment) {
  return reinterpret_cast<std::uintptr_t>(p) & (alignment - 1);
}

// p 是否满足 alignment 对齐。
inline constexpr bool isAligned(const void *p, std::size_t alignment) {
  return alignOffset(p, alignment) == 0;
}

// ---------------------------------------------------------------------------
// 底层对齐分配
//
// 不用 std::aligned_alloc / 过度对齐 operator new 的原因:
//   * std::aligned_alloc 要求 size 必须是 alignment 的整数倍,调用点很容易忘;
//   * ::operator new(size, align_val_t) 必须与 ::operator delete(p, align_val_t)
//     严格配对,一旦有一处走了普通 delete 就是未定义行为。
// 这里统一成"分配/释放成对出现"的一对函数,调用方不会出错。
// ---------------------------------------------------------------------------

// 分配至少 size 字节、且首地址满足 alignment 对齐的内存。
// 返回 nullptr 表示失败(不抛异常,便于在 C 风格的运行时里使用)。
inline void *alignedAllocRaw(std::size_t size, std::size_t alignment) {
  if (alignment < sizeof(void *)) alignment = sizeof(void *);
  if (!isPowerOfTwo(alignment)) return nullptr;
  if (size == 0) size = 1;
  // 向上取整,让底层实现(尤其是 posix_memalign 之外的路径)也满足
  // "size 是 alignment 倍数"的要求。
  const std::size_t rounded = alignUp(size, alignment);
#if defined(XMEX_WINDOWS)
  return _aligned_malloc(rounded, alignment);
#else
  void *p = nullptr;
  if (posix_memalign(&p, alignment, rounded) != 0) return nullptr;
  return p;
#endif
}

// 释放 alignedAllocRaw 得到的内存。p 为 nullptr 时是空操作。
inline void alignedFreeRaw(void *p) {
  if (!p) return;
#if defined(XMEX_WINDOWS)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

// ===========================================================================
// AlignedBuffer —— 对齐缓冲的 RAII 封装
//
// 语义上和 std::vector<char> 类似,但:
//   - 首地址按 alignment 对齐(可以安全地 reinterpret_cast 成需要对齐的类型);
//   - 移动构造/移动赋值便宜(只换指针),拷贝被禁用 —— 缓冲不该被隐式复制;
//  * resize 保留原有内容(和 vector 一样),扩容时按 2 的幂增长,
//    避免反复 realloc。
// ===========================================================================
class AlignedBuffer {
 public:
  AlignedBuffer() = default;

  // 分配 capacity 字节的存储。**内容长度为 0** —— 容量和长度是两件事,
  // 混在一起会让"写 4096 字节"变成"写 4096+capacity 字节"(里面还夹着
  // 未初始化的垃圾)。这是本类第一版真实踩过的坑。
  explicit AlignedBuffer(std::size_t capacity, std::size_t alignment = kDefaultAlign) {
    reserve(capacity, alignment);
  }

  ~AlignedBuffer() { release(); }

  AlignedBuffer(AlignedBuffer &&other) noexcept
      : data_(other.data_), size_(other.size_), cap_(other.cap_), align_(other.align_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.cap_ = 0;
  }

  AlignedBuffer &operator=(AlignedBuffer &&other) noexcept {
    if (this != &other) {
      release();
      data_ = other.data_;
      size_ = other.size_;
      cap_ = other.cap_;
      align_ = other.align_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.cap_ = 0;
    }
    return *this;
  }

  // 拷贝被禁用:一个缓冲被两个所有者持有时,释放时机无法确定。
  AlignedBuffer(const AlignedBuffer &) = delete;
  AlignedBuffer &operator=(const AlignedBuffer &) = delete;

  // 确保至少有 capacity 字节的存储。**不改动内容长度**。
  // 对齐级别变化时重新分配(内容照搬)。
  void reserve(std::size_t capacity, std::size_t alignment = kDefaultAlign) {
    if (alignment < kAlign8) alignment = kAlign8;  // GC 硬下限
    if (!isPowerOfTwo(alignment)) alignment = kDefaultAlign;
    if (data_ != nullptr && alignment == align_ && cap_ >= capacity) {
      return;
    }
    void *fresh = alignedAllocRaw(capacity, alignment);
    if (fresh == nullptr) {
      // 分配失败时保留旧缓冲,但把内容长度置 0 —— 让调用方的边界检查
      // 仍然生效,不会读到未初始化的内存。
      size_ = 0;
      return;
    }
    if (data_ != nullptr && size_ > 0) {
      const std::size_t keep = size_ < capacity ? size_ : capacity;
      std::memcpy(fresh, data_, keep);
      size_ = keep;
    } else {
      size_ = 0;
    }
    alignedFreeRaw(data_);
    data_ = static_cast<char *>(fresh);
    cap_ = capacity;
    align_ = alignment;
  }

  // 重新分配为 capacity 字节,保留前 min(内容长度, capacity) 字节的内容。
  // 内容长度取保留下来的长度 —— 不是 capacity。
  void reset(std::size_t capacity, std::size_t alignment = kDefaultAlign) {
    reserve(capacity, alignment);
  }

  // 释放所有内存,回到空状态。
  void release() {
    alignedFreeRaw(data_);
    data_ = nullptr;
    size_ = 0;
    cap_ = 0;
  }

  // 把内容长度设为 n。n 超过 capacity 时先扩容。
  void resize(std::size_t n) {
    if (n > cap_) grow(n);
    size_ = n;
  }

  // 在尾部追加 n 字节(内容未定义)。
  char *extend(std::size_t n) {
    if (size_ + n > cap_) grow(size_ + n);
    char *p = data_ + size_;
    size_ += n;
    return p;
  }

  // 追加一段字节。
  void append(const void *p, std::size_t n) {
    if (n == 0) return;
    if (size_ + n > cap_) grow(size_ + n);
    std::memcpy(data_ + size_, p, n);
    size_ += n;
  }

  void appendStr(const char *s) { append(s, std::strlen(s)); }

  // 清空内容但保留内存 —— 这是缓冲池能复用的前提。
  void clear() { size_ = 0; }

  char *data() { return data_; }
  const char *data() const { return data_; }
  std::size_t size() const { return size_; }
  std::size_t capacity() const { return cap_; }
  std::size_t alignment() const { return align_; }
  bool empty() const { return size_ == 0; }
  explicit operator bool() const { return data_ != nullptr; }

  // 首地址是否真的满足声明的对齐级别。
  bool alignedAsDeclared() const {
    return data_ == nullptr || isAligned(data_, align_);
  }

 private:
  void grow(std::size_t need) {
    std::size_t cap = cap_ ? cap_ : kDefaultBatchSize;
    while (cap < need) cap *= 2;
    const std::size_t keep = size_;
    char *fresh = static_cast<char *>(alignedAllocRaw(cap, align_));
    if (fresh == nullptr) {
      // 内存不足:截断到当前容量,让调用方的边界检查仍然生效。
      size_ = cap_;
      return;
    }
    if (data_ != nullptr && keep > 0) std::memcpy(fresh, data_, keep);
    alignedFreeRaw(data_);
    data_ = fresh;
    cap_ = cap;
    size_ = keep;
  }

  char *data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t cap_ = 0;
  std::size_t align_ = kDefaultAlign;
};

// ===========================================================================
// BufferPool —— 按大小档位复用的缓冲池
//
// 为什么需要它:对齐分配比普通 malloc 贵(要走 posix_memalign 或
// _aligned_malloc,后者在 Windows 上还会绕过 CRT 的小块堆)。
// 如果每次 IO 都新建+销毁一个 64KB 对齐缓冲,开销就吃掉了收益。
//
// 策略:按 2 的幂分档,每档保留若干个空闲缓冲。
// 归还时按"该档的当前空闲数"决定是否真的留着 —— 防止池无限膨胀。
// ===========================================================================
class BufferPool {
 public:
  static constexpr std::size_t kMaxPooled = 8;      // 每档最多留几个
  static constexpr std::size_t kMaxPooledBytes = 4u << 20;  // 单个缓冲上限 4MB

  static BufferPool &instance() {
    static BufferPool pool;
    return pool;
  }

  // 借一个至少 n 字节、alignment 对齐的缓冲。内容未定义。
  AlignedBuffer acquire(std::size_t n, std::size_t alignment = kDefaultAlign) {
    if (alignment < kAlign8) alignment = kAlign8;
    if (!isPowerOfTwo(alignment)) alignment = kDefaultAlign;

    const std::size_t want = alignUp(n < 64 ? 64 : n, alignment);
    const std::size_t bucket = bucketOf(want);

    {
      std::lock_guard<std::mutex> lock(mu_);
      auto &vec = free_[bucket];
      if (!vec.empty()) {
        AlignedBuffer buf = std::move(vec.back());
        vec.pop_back();
        ++hits_;
        // 复用的缓冲内容长度必须归零 —— 上一个借用者留下的内容
        // 不该被下一个借用者读到。
        buf.clear();
        // 同一档里的缓冲容量可能小于本次需求(档是按 2 的幂分的,
        // 同一档内尺寸并不相同),不够就重新分配。
        if (buf.capacity() < want) buf.reserve(want, alignment);
        return buf;
      }
    }
    ++misses_;
    return AlignedBuffer(want, alignment);
  }

  // 归还一个缓冲。容量太小或太大的不回收。
  void release(AlignedBuffer &buf) {
    if (!buf) return;
    const std::size_t cap = buf.capacity();
    if (cap > kMaxPooledBytes) return;
    const std::size_t bucket = bucketOf(cap);
    std::lock_guard<std::mutex> lock(mu_);
    auto &vec = free_[bucket];
    if (vec.size() < kMaxPooled) {
      buf.clear();
      vec.push_back(std::move(buf));
    }
  }

  // 池里当前空闲缓冲的字节总数。
  std::size_t pooledBytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t total = 0;
    for (const auto &vec : free_) {
      for (const auto &b : vec) total += b.capacity();
    }
    return total;
  }

  std::size_t hitCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
  }
  std::size_t missCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return misses_;
  }

  // 清空池(测试用)。
  void drain() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto &vec : free_) vec.clear();
  }

 private:
  static constexpr int kBuckets = 24;  // 2^0 .. 2^23

  static std::size_t bucketOf(std::size_t n) {
    int b = 0;
    while (b + 1 < kBuckets && (std::size_t(1) << (b + 1)) <= n) ++b;
    return static_cast<std::size_t>(b);
  }

  mutable std::mutex mu_;
  std::vector<AlignedBuffer> free_[kBuckets];
  std::size_t hits_ = 0;
  std::size_t misses_ = 0;
};

// ===========================================================================
// IO 原语
// ===========================================================================

// 读整个文件到一个对齐缓冲里。
//
// 相比"malloc + fread"的区别有两点:
//   1. 缓冲首地址按 alignment 对齐 —— 后续对内容做 SIMD 扫描(找分隔符、
//      找关键字)不会因为跨缓存行而慢;
//   2. 内部用一块更大的用户态缓冲喂 stdio,减少 read 系统调用次数。
// 失败返回空缓冲(而不是抛异常),调用方检查 empty()。
inline AlignedBuffer readFileAligned(const char *path,
                                     std::size_t alignment = kDefaultAlign) {
  AlignedBuffer out;
  if (path == nullptr) return out;

  std::FILE *fp = std::fopen(path, "rb");
  if (fp == nullptr) return out;

  if (std::fseek(fp, 0, SEEK_END) != 0) {
    std::fclose(fp);
    return out;
  }
  const long fsize = std::ftell(fp);
  if (fsize < 0) {
    std::fclose(fp);
    return out;
  }
  std::rewind(fp);

  const std::size_t size = static_cast<std::size_t>(fsize);
  out.reserve(size + 1, alignment);  // +1 留给 '\0'
  if (!out) {
    std::fclose(fp);
    return out;
  }

  // 给 stdio 一块大缓冲,减少 read 系统调用。
  std::vector<char> iobuf(kDefaultBatchSize);
  std::setvbuf(fp, iobuf.data(), _IOFBF, iobuf.size());

  const std::size_t got = std::fread(out.data(), 1, size, fp);
  std::fclose(fp);

  out.resize(got);
  out.data()[got] = '\0';
  return out;
}

// 读文件的前 maxBytes 字节(用于"文件很大但只需要开头"的场景,
// 比如 tokenizer.json 里的 vocab 段)。
inline AlignedBuffer readFilePrefix(const char *path, std::size_t maxBytes,
                                    std::size_t alignment = kDefaultAlign) {
  AlignedBuffer out;
  if (path == nullptr) return out;

  std::FILE *fp = std::fopen(path, "rb");
  if (fp == nullptr) return out;

  std::vector<char> iobuf(kDefaultBatchSize);
  std::setvbuf(fp, iobuf.data(), _IOFBF, iobuf.size());

  // 先读满，再判断是否超限。
  AlignedBuffer tmp(maxBytes + 1, alignment);
  if (!tmp) {
    std::fclose(fp);
    return out;
  }
  const std::size_t got = std::fread(tmp.data(), 1, maxBytes, fp);
  std::fclose(fp);

  tmp.resize(got);
  tmp.data()[got] = '\0';
  out = std::move(tmp);
  return out;
}

// ---------------------------------------------------------------------------
// BatchWriter —— 把小写入合并成一次落盘
//
// 这是"节约 IO 时间"最直接的一处。
//
// 原来的写法(见 runtime/io.cpp 的 sinkPutc)是一次 fwrite 写 1 字节。
// 即使 stdio 内部有缓冲,每次调用仍有函数调用开销 + 缓冲状态维护;
// 而 render 一个 repr 可能有成千上万个字符要写,也就是成千上万次调用。
//
// BatchWriter 的做法:先在内存里拼,攒够一个 batch(默认 64KB)才 fwrite 一次。
// 拼接阶段还顺带保证了内存缓冲区是对齐的。
// ---------------------------------------------------------------------------
class BatchWriter {
 public:
  explicit BatchWriter(std::FILE *f, std::size_t batch = kDefaultBatchSize)
      : f_(f), batch_(batch), buf_(batch + kAlign64, kDefaultAlign) {
    // 缓冲容量固定为 batch + 一点余量,内容长度从 0 开始。
    // ⚠️ 批量阈值必须记在独立的成员里,不能从 buf_.capacity() 反推 ——
    //    缓冲扩容后 capacity 会变,阈值跟着变就成了增长反馈环
    //    (写多少都追不上阈值,最后永远不 flush)。
  }

  void write(const void *p, std::size_t n) {
    if (f_ == nullptr || n == 0) return;
    buf_.append(p, n);
    if (buf_.size() >= batch_) flushBuf();
  }

  void writeStr(const char *s) {
    if (s == nullptr) return;
    write(s, std::strlen(s));
  }

  void writeChar(char c) {
    if (f_ == nullptr) return;
    buf_.append(&c, 1);
    if (buf_.size() >= batch_) flushBuf();
  }

  // 把残余内容写出去(不 fflush FILE,只保证自己的缓冲落地)。
  void flush() {
    flushBuf();
    if (f_ != nullptr) std::fflush(f_);
  }

  // 已经发生的 fwrite 次数 —— 用来验证"批量化"确实减少了写调用。
  std::uint64_t writeCalls() const { return writeCalls_; }
  std::uint64_t bytesWritten() const { return bytesWritten_; }

  std::size_t pending() const { return buf_.size(); }

 private:
  void flushBuf() {
    if (buf_.empty()) return;
    const std::size_t n = buf_.size();
    std::fwrite(buf_.data(), 1, n, f_);
    ++writeCalls_;
    bytesWritten_ += n;
    buf_.clear();
  }

  std::FILE *f_ = nullptr;
  std::size_t batch_ = kDefaultBatchSize;
  AlignedBuffer buf_;
  std::uint64_t writeCalls_ = 0;
  std::uint64_t bytesWritten_ = 0;
};

// ---------------------------------------------------------------------------
// StreamBuffer —— 给 FILE* 挂一块大的用户态缓冲
//
// 用途:当一个文件被反复做小写入(日志、逐行输出、序列化)时,stdio 默认的
// 缓冲通常是 4KB,写系统调用比较频繁。换成 64KB 后,同样大小的输出
// 写系统调用数量减少一个数量级。
//
// 注意:setvbuf 必须在该流上发生任何 I/O 之前调用,且这个对象必须活得
// 比那个流更久(缓冲的生命周期由 setvbuf 接管,流关闭后不能再访问)。
// ---------------------------------------------------------------------------
class StreamBuffer {
 public:
  StreamBuffer() = default;

  explicit StreamBuffer(std::FILE *f, std::size_t size = kDefaultBatchSize,
                        std::size_t alignment = kDefaultAlign)
      : buf_(size, alignment) {
    if (f != nullptr && buf_ && std::setvbuf(f, buf_.data(), _IOFBF, buf_.capacity()) == 0) {
      f_ = f;
      ok_ = true;
    }
  }

  ~StreamBuffer() { release(); }

  StreamBuffer(StreamBuffer &&other) noexcept
      : buf_(std::move(other.buf_)), f_(other.f_), ok_(other.ok_) {
    other.f_ = nullptr;
    other.ok_ = false;
  }
  StreamBuffer &operator=(StreamBuffer &&other) noexcept {
    if (this != &other) {
      release();
      buf_ = std::move(other.buf_);
      f_ = other.f_;
      ok_ = other.ok_;
      other.f_ = nullptr;
      other.ok_ = false;
    }
    return *this;
  }
  StreamBuffer(const StreamBuffer &) = delete;
  StreamBuffer &operator=(const StreamBuffer &) = delete;

  bool ok() const { return ok_; }
  std::FILE *file() const { return f_; }

  void release() {
    if (f_ != nullptr) {
      // 让 stdio 把缓冲刷出去,再交还内存。
      std::fflush(f_);
      std::setvbuf(f_, nullptr, _IONBF, 0);
      f_ = nullptr;
    }
    ok_ = false;
  }

 private:
  AlignedBuffer buf_;
  std::FILE *f_ = nullptr;
  bool ok_ = false;
};

// ===========================================================================
// 伪共享防护
//
// 多个线程写同一缓存行的不同变量会让缓存行在核间反复失效。
// 把热点计数器放进 CacheLinePadded 里,保证不同计数器落在不同缓存行。
// ===========================================================================
template <class T>
struct alignas(kAlign64) CacheLinePadded {
  CacheLinePadded() = default;
  explicit CacheLinePadded(T v) : value(v) {}

  T load(std::memory_order order = std::memory_order_relaxed) const {
    return value.load(order);
  }
  void store(T v, std::memory_order order = std::memory_order_relaxed) {
    value.store(v, order);
  }

 private:
  // 填充到缓存行边界,保证相邻的两个实例不共享缓存行。
  std::atomic<T> value{};
  char pad[sizeof(T) % kAlign64 == 0 ? 1 : kAlign64 - (sizeof(T) % kAlign64)];
};

}  // namespace xmex
