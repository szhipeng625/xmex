// reconcile_test.cpp —— 调谐循环与副本组的独立验证
//
// 不依赖 LLVM/PyLite 运行时,只链 kv_admin.cpp。
//
// 关键设计:用 set_spawn_hook 注入假的 spawn —— 调谐的核心是
// 「观测 → 判断 → 动作」这套逻辑,不必真的 fork 进程。这样测试
// 在任何平台都能跑(Windows 上也能跑),而且能精确控制「进程崩溃」
// 这个时机,不用 sleep 等它死。
//
// 用法(Linux):
//   g++ -std=c++20 -I include -I runtime tests/reconcile_test.cpp \
//       runtime/kv_admin.cpp runtime/lsm_client.cpp -o t && ./t

#include "kv_admin.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <atomic>

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        g_pass++;
        std::printf("  [PASS] %s\n", what);
    } else {
        g_fail++;
        std::printf("  [FAIL] %s\n", what);
    }
}

// ---- 假的 spawn 环境 ----
static std::atomic<int64_t> g_next_pid{1000};
// 可执行体 -> 是否 spawn 失败(用来测退避)
static std::set<std::string> g_bad_binaries;

static int64_t fake_spawn(const KvNode& node, std::string& error) {
    if (g_bad_binaries.count(node.binary)) {
        error = "fake spawn failure for " + node.binary;
        return -1;
    }
    return g_next_pid.fetch_add(1);
}

static void install_fake(KvAdmin& m) {
    g_bad_binaries.clear();
    m.set_spawn_hook(fake_spawn);
}

// ---- 用例 ----

// 1. desired/observed 基本语义
static void test_desired_observed() {
    std::printf("\n=== 1. desired / observed 分离 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    check(m.build("t1", "/fake/worker", {}, "/tmp", "127.0.0.1:1", err), "build 成功");
    KvNode n;
    check(m.get_node("t1", n), "节点在注册表里");
    check(n.desired == NodeDesired::PRESENT, "desired = PRESENT");
    check(n.pid > 0, "拿到了 pid");
    check(n.observed == NodeObserved::ALIVE, "observed = ALIVE");
    m.erase("t1", err);
}

// 2. 幂等性
static void test_idempotent() {
    std::printf("\n=== 2. 调谐幂等性 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    m.build("t2", "/fake/worker", {}, "/tmp", "", err);

    m.reconcile_report_once();
    int64_t pid_after_first;
    { KvNode n; m.get_node("t2", n); pid_after_first = n.pid; }

    ReconcileReport second = m.reconcile_report_once();
    check(second.actions == 0, "第二轮 actions == 0(已收敛)");
    { KvNode n; m.get_node("t2", n);
      check(n.pid == pid_after_first, "pid 未变(没有反复重启)"); }
    m.erase("t2", err);
}

// 3. 崩溃恢复
static void test_crash_recovery() {
    std::printf("\n=== 3. 崩溃自动恢复 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    m.build("t3", "/fake/worker", {}, "/tmp", "", err);

    int64_t old_pid = -1, old_rc = 0;
    { KvNode n; m.get_node("t3", n); old_pid = n.pid; old_rc = n.restart_count; }

    m.test_kill_fake(old_pid);          // 模拟进程崩溃

    ReconcileReport rep = m.reconcile_report_once();
    KvNode n;
    m.get_node("t3", n);
    check(rep.actions >= 1, "调谐执行了修复动作");
    check(n.pid > 0 && n.pid != old_pid, "pid 已变化(进程被重新拉起)");
    check(n.observed == NodeObserved::ALIVE, "observed 回到 ALIVE");
    check(n.restart_count > old_rc, "restart_count 增加了");
    m.erase("t3", err);
}

// 4. erase 之后不被复活 —— 最容易写错的用例
static void test_no_resurrect() {
    std::printf("\n=== 4. erase 后不被复活 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    m.build("t4", "/fake/worker", {}, "/tmp", "", err);
    check(m.erase("t4", err), "erase 成功");

    ReconcileReport rep = m.reconcile_report_once();
    KvNode n;
    check(!m.get_node("t4", n), "两轮调谐后节点仍不在注册表(没被复活)");
    check(rep.actions == 0, "调谐没有产生任何动作");
}

// 5. sleep 的节点不该被误判为掉线
static void test_sleep_not_repairs() {
    std::printf("\n=== 5. 暂停节点不被误修复 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    m.build("t5", "/fake/worker", {}, "/tmp", "", err);
    int64_t pid;
    { KvNode n; m.get_node("t5", n); pid = n.pid; }

    check(m.sleep_node("t5", err), "sleep_node 成功");
    m.reconcile_report_once();
    { KvNode n; m.get_node("t5", n);
      check(n.pid == pid, "进程没被重新拉起(故障注入未被打破)");
      check(n.observed == NodeObserved::SUSPENDED, "observed = SUSPENDED"); }
    check(m.wakeup("t5", err), "wakeup 成功");
    m.erase("t5", err);
}

// 6. skip 语义:保留进程,desired 仍 PRESENT
static void test_skip_semantics() {
    std::printf("\n=== 6. skip 语义 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    m.build("t6", "/fake/worker", {}, "/tmp", "", err);
    int64_t pid;
    { KvNode n; m.get_node("t6", n); pid = n.pid; }

    check(m.skip("t6", err), "skip 成功");
    { KvNode n; m.get_node("t6", n);
      check(n.state == KvNodeState::LOST, "state = LOST");
      check(n.pid == pid, "进程保留(数据不丢)");
      check(n.desired == NodeDesired::PRESENT, "desired 仍为 PRESENT(skip 不是下线)"); }

    check(m.unskip("t6", err), "unskip 成功");
    { KvNode n; m.get_node("t6", n);
      check(n.state != KvNodeState::LOST, "LOST 标记已清除"); }
    m.erase("t6", err);
}

// 7. 失败退避
static void test_backoff() {
    std::printf("\n=== 7. 失败退避 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    g_bad_binaries.insert("/fake/broken");
    m.build("t7", "/fake/broken", {}, "/tmp", "", err);
    check(!err.empty(), "build 报告了失败原因");

    int64_t f1 = m.consecutive_failures("t7");
    check(f1 >= 1, "失败计数已记录");
    // 再跑一轮:应进退避窗口,不再重试
    ReconcileReport r1 = m.reconcile_report_once();
    int64_t f2 = m.consecutive_failures("t7");
    check(f2 == f1, "退避期内不再增加失败计数(不反复重试)");
    check(r1.actions == 0, "退避期内没有产生动作");
    m.erase("t7", err);
    g_bad_binaries.clear();
}

// 8. 副本组:崩溃成员原地重启,skip 成员则补替补
static void test_replica_group() {
    std::printf("\n=== 8. 副本组:崩溃重启与替补补足 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;

    check(m.group_create("g1", 2, "/fake/worker", std::vector<std::string>{},
                         "/tmp/xmex_test", "127.0.0.1:9300", err),
          "创建期望 2 副本的组");

    check(m.build("g1-a", "/fake/worker", {}, "/tmp/xmex_test", "127.0.0.1:9300", err), "成员 a 已建");
    check(m.build("g1-b", "/fake/worker", {}, "/tmp/xmex_test", "127.0.0.1:9301", err), "成员 b 已建");
    check(m.group_add_member("g1", "g1-a", err), "a 加入组");
    check(m.group_add_member("g1", "g1-b", err), "b 加入组");

    ReconcileReport r0 = m.reconcile_report_once();
    check(r0.replaced.empty(), "两个成员健康时不补副本");
    check(r0.repaired.empty(), "两个成员健康时不重启");

    // --- 场景一:成员崩溃 → 原地重启,不补替补 ---
    int64_t b_pid;
    { KvNode n; m.get_node("g1-b", n); b_pid = n.pid; }
    m.test_kill_fake(b_pid);

    ReconcileReport r1 = m.reconcile_report_once();
    check(!r1.repaired.empty(), "崩溃成员被原地重启");
    check(r1.replaced.empty(), "原地重启后不补替补(副本数仍满)");
    { KvNode n; m.get_node("g1-b", n);
      check(n.pid > 0 && n.pid != b_pid, "b 换了新 pid");
      check(n.observed == NodeObserved::ALIVE, "b 恢复健康"); }
    { ReplicaGroup g; m.group_get("g1", g);
      check(g.member_ids.size() == 2, "成员数仍是 2(没多出替补)"); }

    // --- 场景二:成员被 skip → 补替补 ---
    check(m.skip("g1-b", err), "skip 成员 b");
    ReconcileReport r2 = m.reconcile_report_once();
    check(!r2.replaced.empty(), "skip 的成员触发了补替补");
    if (!r2.replaced.empty()) {
        std::printf("         替补节点: %s\n", r2.replaced[0].c_str());
        KvNode nn;
        check(m.get_node(r2.replaced[0], nn), "替补节点在注册表里");
        check(nn.observed == NodeObserved::ALIVE, "替补已拉起");
        check(nn.group_id == "g1", "替补归属本组");
    }
    { ReplicaGroup g; m.group_get("g1", g);
      check(g.member_ids.size() == 3, "成员列表含替补(2 成员 + 1 替补)"); }

    for (const auto& id : {std::string("g1-a"), std::string("g1-b")}) m.erase(id, err);
    { ReplicaGroup g; m.group_get("g1", g);
      for (const auto& id : g.member_ids) m.erase(id, err); }
    m.group_destroy("g1", err);
}

// 9. 声明缩容
static void test_scale_down_declared() {
    std::printf("\n=== 9. 声明缩容 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    check(m.group_create("g2", 1, "/fake/worker", std::vector<std::string>{}, "", "", err),
          "创建期望 1 副本的组");
    check(m.group_set_replicas("g2", 3, err), "期望改为 3");
    ReplicaGroup g;
    m.group_get("g2", g);
    check(g.desired_replica_count == 3, "期望副本数已更新为 3");
    m.group_destroy("g2", err);
}

// 10. 调谐线程生命周期
static void test_reconcile_thread() {
    std::printf("\n=== 10. 调谐线程生命周期 ===\n");
    KvAdmin& m = KvAdmin::instance();
    check(!m.reconcile_running(), "初始未运行");
    m.start_reconcile(100);
    check(m.reconcile_running(), "启动后运行中");
    m.start_reconcile(100);
    check(m.reconcile_running(), "重复启动不报错(幂等)");
    m.stop_reconcile();
    check(!m.reconcile_running(), "停止后不再运行");
    m.stop_reconcile();
    check(!m.reconcile_running(), "重复停止不报错(幂等)");
}

// 11. 副本组在成员全部健康时,反复调谐不重复补 —— 幂等性的组版本
static void test_group_idempotent() {
    std::printf("\n=== 11. 副本组调谐幂等 ===\n");
    KvAdmin& m = KvAdmin::instance();
    install_fake(m);
    std::string err;
    m.group_create("g3", 1, "/fake/worker", std::vector<std::string>{}, "", "", err);
    m.build("g3-a", "/fake/worker", {}, "", "", err);
    m.group_add_member("g3", "g3-a", err);

    m.reconcile_report_once();
    size_t after_first;
    { ReplicaGroup g; m.group_get("g3", g); after_first = g.member_ids.size(); }

    ReconcileReport r = m.reconcile_report_once();
    { ReplicaGroup g; m.group_get("g3", g);
      check(g.member_ids.size() == after_first, "第二轮成员数没变(没重复补)"); }
    check(r.replaced.empty(), "第二轮没有补副本");

    { ReplicaGroup g; m.group_get("g3", g);
      for (const auto& id : g.member_ids) m.erase(id, err); }
    m.erase("g3-a", err);
    m.group_destroy("g3", err);
}

int main() {
    std::printf("=== xmex 调谐循环验证(注入模式,不 fork 真实进程) ===\n");

    test_desired_observed();
    test_idempotent();
    test_crash_recovery();
    test_no_resurrect();
    test_sleep_not_repairs();
    test_skip_semantics();
    test_backoff();
    test_replica_group();
    test_scale_down_declared();
    test_reconcile_thread();
    test_group_idempotent();

    KvAdmin::instance().stop_reconcile();
    KvAdmin::instance().clear_spawn_hook();

    std::printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
