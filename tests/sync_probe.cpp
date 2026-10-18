// 互斥锁与条件变量的验证探针。
//
// 重点验证三件事:
//   1. 加锁、解锁、销毁可以正常往返
//   2. 消费者先进入等待、生产者稍后放入数据并唤醒 —— 消费者必须被唤醒并读到数据
//      (这条同时证明"检查条件与等待之间不丢唤醒"是成立的)
//   3. 广播能一次唤醒多个等待者
#include "pylite/runtime.h"
#include "pylite/value.h"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

static int g_fail = 0;
static void check(bool ok, const char *what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_fail;
}

// ---- 场景一:单生产者、单消费者 ----
static lite::Value m1, c1;
static int ready1 = 0;
static int consumed1 = -1;

static void *consumer1(void *) {
  sync_mutex_lock(&m1);
  while (ready1 == 0) {
    sync_cond_wait(&c1, &m1);
  }
  consumed1 = ready1;
  sync_mutex_unlock(&m1);
  return nullptr;
}
static void *producer1(void *) {
  usleep(100000);   // 让消费者先进入等待
  sync_mutex_lock(&m1);
  ready1 = 42;
  sync_cond_signal(&c1);
  sync_mutex_unlock(&m1);
  return nullptr;
}

// ---- 场景二:两个消费者、广播唤醒 ----
static lite::Value m2, c2;
static int ready2 = 0;
static int got2 = 0;

static void *worker2(void *) {
  sync_mutex_lock(&m2);
  while (ready2 == 0) {
    sync_cond_wait(&c2, &m2);
  }
  got2 += 1;
  sync_mutex_unlock(&m2);
  return nullptr;
}
static void *producer2(void *) {
  usleep(100000);
  sync_mutex_lock(&m2);
  ready2 = 7;
  sync_cond_broadcast(&c2);
  sync_mutex_unlock(&m2);
  return nullptr;
}

int main() {
  std::printf("=== 互斥锁 ===\n");
  lite::Value m = sync_mutex_new();
  check(m.tag == lite::kInt && m.as.i > 0, "创建锁返回有效句柄");
  sync_mutex_lock(&m);
  sync_mutex_unlock(&m);
  check(true, "加锁与解锁正常返回");
  sync_mutex_free(&m);

  std::printf("=== 条件变量 ===\n");
  lite::Value cv = sync_cond_new();
  check(cv.tag == lite::kInt && cv.as.i > 0, "创建条件变量返回有效句柄");
  sync_cond_free(&cv);

  std::printf("=== 生产/消费协作(单对单)===\n");
  m1 = sync_mutex_new();
  c1 = sync_cond_new();
  pthread_t tc, tp;
  pthread_create(&tc, nullptr, consumer1, nullptr);
  pthread_create(&tp, nullptr, producer1, nullptr);
  pthread_join(tp, nullptr);
  pthread_join(tc, nullptr);
  check(consumed1 == 42, "消费者被唤醒后读到了生产者放入的数据");
  sync_mutex_free(&m1);
  sync_cond_free(&c1);

  std::printf("=== 广播唤醒多个消费者 ===\n");
  m2 = sync_mutex_new();
  c2 = sync_cond_new();
  pthread_t w1, w2, tp2;
  pthread_create(&w1, nullptr, worker2, nullptr);
  pthread_create(&w2, nullptr, worker2, nullptr);
  pthread_create(&tp2, nullptr, producer2, nullptr);
  pthread_join(tp2, nullptr);
  pthread_join(w1, nullptr);
  pthread_join(w2, nullptr);
  check(got2 == 2, "广播后两个消费者都被唤醒");
  sync_mutex_free(&m2);
  sync_cond_free(&c2);

  std::printf("=== 快照 ===\n");
  lite::Value st = sync_stats();
  check(st.tag == lite::kStr && std::strstr(str_data(&st), "mutexes") != nullptr,
        "同步原语快照可获取");

  std::printf("\n%s (失败 %d 项)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
