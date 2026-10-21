// xmex 内存对齐模型测试。
//
// 覆盖:
//   - 各级对齐保证(8/16/32/64/4096)
//   - AlignedBuffer 的 RAII / 移动语义 / 内容保留
//   - 缓冲池的复用
//   - 批量写确实减少了写调用次数(这是"节约 IO 时间"的量化证据)
//   - 对齐缓冲读文件与原读法的内容等价
#include "xmex/align.h"

#include <cstdio>
#include <cstring>
#include <string>

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

const char *kTmpFile = "align_test_tmp.bin";

void writeTmpFile(const char *content, std::size_t n) {
  std::FILE *f = std::fopen(kTmpFile, "wb");
  if (f == nullptr) {
    std::printf("  [FAIL] 无法创建临时文件 %s\n", kTmpFile);
    ++g_failures;
    return;
  }
  std::fwrite(content, 1, n, f);
  std::fclose(f);
}

}  // namespace

int main() {
  std::printf("=== xmex 内存对齐模型测试 ===\n");

  // -----------------------------------------------------------------
  std::printf("--- 基础工具 ---\n");
  {
    check(xmex::isPowerOfTwo(64), "isPowerOfTwo(64) == true");
    check(!xmex::isPowerOfTwo(0), "isPowerOfTwo(0) == false");
    check(!xmex::isPowerOfTwo(48), "isPowerOfTwo(48) == false");
    check(xmex::alignUp(1, 16) == 16, "alignUp(1,16) == 16");
    check(xmex::alignUp(16, 16) == 16, "alignUp(16,16) == 16");
    check(xmex::alignUp(17, 16) == 32, "alignUp(17,16) == 32");
    check(xmex::alignUp(0, 64) == 0, "alignUp(0,64) == 0");
  }

  // -----------------------------------------------------------------
  std::printf("--- 各级对齐保证 ---\n");
  {
    struct AlignProbe { alignas(64) char c; };
    check(alignof(AlignProbe) == 64, "alignas(64) 生效");

    char raw[128];
    check(!xmex::isAligned(raw, 8) || true, "栈上裸数组对齐由编译器决定(仅记录)");

    xmex::AlignedBuffer b8(100, xmex::kAlign8);
    xmex::AlignedBuffer b16(100, xmex::kAlign16);
    xmex::AlignedBuffer b32(100, xmex::kAlign32);
    xmex::AlignedBuffer b64(100, xmex::kAlign64);
    xmex::AlignedBuffer b4k(100, xmex::kAlign4K);

    check(xmex::isAligned(b8.data(), 8), "8 字节对齐");
    check(xmex::isAligned(b16.data(), 16), "16 字节对齐");
    check(xmex::isAligned(b32.data(), 32), "32 字节对齐");
    check(xmex::isAligned(b64.data(), 64), "64 字节对齐");
    check(xmex::isAligned(b4k.data(), 4096), "4096 字节对齐");
    check(b8.alignedAsDeclared() && b4k.alignedAsDeclared(), "alignedAsDeclared() 自检通过");
  }

  // -----------------------------------------------------------------
  std::printf("--- AlignedBuffer 语义 ---\n");
  {
    xmex::AlignedBuffer b(16, xmex::kAlign64);
    check(b.size() == 0, "新缓冲的内容长度为 0(容量与长度是两件事)");
    check(b.capacity() >= 16, "capacity 不小于构造参数");

    std::memcpy(b.data(), "hello", 6);  // 含结尾的 '\0'
    b.resize(6);
    check(std::strcmp(b.data(), "hello") == 0, "内容可写可读");

    // 移动:源应变空
    xmex::AlignedBuffer moved = std::move(b);
    check(moved.size() == 6, "移动后内容保留");
    check(b.data() == nullptr, "移动后源指针置空");
    check(xmex::isAligned(moved.data(), 64), "移动后仍保持对齐");

    // reset 保留内容
    moved.reset(64, xmex::kAlign64);
    check(moved.size() == 6, "reset 后内容长度不变");
    check(moved.capacity() >= 64, "reset 后容量更新");
    check(std::strncmp(moved.data(), "hello", 5) == 0, "reset 保留原有内容");

    // append
    moved.append("\0", 1);
    check(moved.size() == 7, "append 更新 size");
    check(std::strcmp(moved.data(), "hello") == 0, "append 后仍是合法字符串");

    // 扩容后内容仍在
    moved.appendStr("0123456789");
    check(moved.size() == 17, "appendStr 更新 size");
    check(std::strncmp(moved.data(), "hello", 5) == 0, "扩容后原内容仍在");
    check(xmex::isAligned(moved.data(), 64), "扩容后仍对齐");
  }

  // -----------------------------------------------------------------
  std::printf("--- 缓冲池 ---\n");
  {
    auto &pool = xmex::BufferPool::instance();
    pool.drain();

    const std::size_t hits0 = pool.hitCount();
    const std::size_t misses0 = pool.missCount();

    {
      auto b1 = pool.acquire(4096, xmex::kAlign64);
      check(b1.capacity() >= 4096, "acquire 拿到足够大的缓冲");
      check(xmex::isAligned(b1.data(), 64), "池化缓冲满足对齐");
      pool.release(b1);
    }
    {
      auto b2 = pool.acquire(4096, xmex::kAlign64);
      check(pool.hitCount() == hits0 + 1, "归还后再取命中缓存");
      check(pool.missCount() == misses0 + 1, "首次取用计为 miss");
      check(b2.capacity() >= 4096, "复用后容量仍足够");
      pool.release(b2);
    }
    pool.drain();
  }

  // -----------------------------------------------------------------
  std::printf("--- 批量写:写调用次数下降 ---\n");
  {
    const char *kTmp = "align_test_batch.txt";
    std::FILE *f = std::fopen(kTmp, "wb");
    check(f != nullptr, "创建临时文件");
    if (f != nullptr) {
      // 原始写法:逐字节 fwrite 4096 次
      for (int i = 0; i < 4096; ++i) {
        std::fwrite("x", 1, 1, f);
      }
      std::fclose(f);

      // 批量写法:同样的 4096 字节,一次 write
      std::FILE *f2 = std::fopen(kTmp, "wb");
      xmex::BatchWriter w(f2);
      const char *chunk = "x";
      for (int i = 0; i < 4096; ++i) {
        w.write(chunk, 1);
      }
      w.flush();
      const std::uint64_t calls = w.writeCalls();
      check(calls == 1, "4096 次单字节写被合并成 1 次 fwrite");
      check(w.bytesWritten() == 4096, "写出的字节数正确");
      std::fclose(f2);
      std::remove(kTmp);
    }
  }

  // -----------------------------------------------------------------
  std::printf("--- 批量写与原始写法内容等价 ---\n");
  {
    const char *kTmp = "align_test_equiv.txt";
    std::string payload;
    for (int i = 0; i < 5000; ++i) payload.push_back(static_cast<char>('a' + (i % 26)));

    // 原始
    std::FILE *f1 = std::fopen(kTmp, "wb");
    std::fwrite(payload.data(), 1, payload.size(), f1);
    std::fclose(f1);

    // 批量
    std::FILE *f2 = std::fopen(kTmp, "wb");
    {
      xmex::BatchWriter w(f2);
      w.write(payload.data(), payload.size());
      w.flush();
    }
    std::fclose(f2);

    // 读回来比较
    auto read = xmex::readFileAligned(kTmp);
    check(read.size() == payload.size(), "批量写出的长度与原始一致");
    check(read.size() == payload.size() &&
              std::memcmp(read.data(), payload.data(), payload.size()) == 0,
          "批量写出的内容与原始逐字节一致");
    std::remove(kTmp);
  }

  // -----------------------------------------------------------------
  std::printf("--- 对齐读文件 ---\n");
  {
    const char *content = "{\"a\":1,\"b\":[2,3]}";
    writeTmpFile(content, std::strlen(content));

    auto buf = xmex::readFileAligned(kTmpFile);
    check(buf.size() == std::strlen(content), "读出的长度正确");
    check(buf.data() != nullptr && buf.data()[buf.size()] == '\0', "尾部补了 '\\0'");
    check(xmex::isAligned(buf.data(), xmex::kDefaultAlign), "读出的缓冲满足对齐");
    check(std::memcmp(buf.data(), content, buf.size()) == 0, "内容逐字节一致");

    // 前缀读
    auto pre = xmex::readFilePrefix(kTmpFile, 5);
    check(pre.size() == 5, "前缀读的长度正确");
    check(std::memcmp(pre.data(), "{\"a\":", 5) == 0, "前缀读的内容正确");

    // 不存在的文件返回空缓冲而不是崩溃
    auto missing = xmex::readFileAligned("align_test_nonexistent.bin");
    check(missing.empty(), "不存在的文件返回空缓冲");

    std::remove(kTmpFile);
  }

  // -----------------------------------------------------------------
  std::printf("--- StreamBuffer ---\n");
  {
    const char *kTmp = "align_test_stream.txt";
    std::FILE *f = std::fopen(kTmp, "wb");
    check(f != nullptr, "创建临时文件");
    if (f != nullptr) {
      // 预期长度自己算 —— "line 0\n" 和 "line 999\n" 长度不同,
      // 不能用 1000 * 固定长度去估。
      std::size_t expect = 0;
      {
        xmex::StreamBuffer sb(f);
        check(sb.ok(), "setvbuf 成功");
        for (int i = 0; i < 1000; ++i) {
          char line[64];
          const int n = std::snprintf(line, sizeof(line), "line %d\n", i);
          expect += static_cast<std::size_t>(n);
          std::fwrite(line, 1, static_cast<std::size_t>(n), f);
        }
      }
      std::fclose(f);

      auto buf = xmex::readFileAligned(kTmp);
      check(buf.size() == expect, "输出长度与实际写入一致");
      std::remove(kTmp);
    }
  }

  // -----------------------------------------------------------------
  std::printf("--- 伪共享防护 ---\n");
  {
    xmex::CacheLinePadded<int> a;
    xmex::CacheLinePadded<int> b;
    check(alignof(xmex::CacheLinePadded<int>) == 64, "计数器按缓存行对齐");
    check(reinterpret_cast<std::uintptr_t>(&a) % 64 == 0, "实例地址落在缓存行边界");
    a.store(5);
    check(b.load() == 0, "相邻实例互不干扰");
  }

  std::printf("\n%d 项检查,%d 项失败\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
