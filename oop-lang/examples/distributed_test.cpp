// 分布式节点创建测试
// 直接调用 Xmex 运行时的 Raft / Cluster / KvAdmin C API
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>

#include "../runtime/raft.h"
#include "../runtime/cluster.h"
#include "../runtime/kv_admin.h"

int main() {
    printf("=== 分布式节点创建测试 ===\n\n");

    // 1. 使用 KvAdmin 构建节点
    // build(node_id, binary, args, work_dir, addr, error)
    printf("1. 通过 KvAdmin 构建分布式节点\n");
    auto& admin = KvAdmin::instance();
    std::string err;
    std::vector<std::string> empty_args;

    bool ok1 = admin.build("node-1", "/usr/bin/raft-node", empty_args, "/tmp/raft-node-1", "127.0.0.1:9001", err);
    if (!ok1) printf("   node-1 构建失败: %s\n", err.c_str());
    else printf("   node-1 构建: 成功\n");

    bool ok2 = admin.build("node-2", "/usr/bin/raft-node", empty_args, "/tmp/raft-node-2", "127.0.0.1:9002", err);
    if (!ok2) printf("   node-2 构建失败: %s\n", err.c_str());
    else printf("   node-2 构建: 成功\n");

    bool ok3 = admin.build("node-3", "/usr/bin/raft-node", empty_args, "/tmp/raft-node-3", "127.0.0.1:9003", err);
    if (!ok3) printf("   node-3 构建失败: %s\n", err.c_str());
    else printf("   node-3 构建: 成功\n");

    // 2. 查看全局节点注册表 (RaftNode::list_nodes 返回 vector<string>)
    printf("\n2. 全局节点注册表\n");
    auto node_ids = RaftNode::list_nodes();
    printf("   已注册节点数: %zu\n", node_ids.size());
    for (auto& id : node_ids) {
        auto st = RaftNode::status(id);
        printf("   - %s: 任期=%lld, 领导=%s, 提交索引=%lld, 对等节点数=%lld\n",
               id.c_str(), (long long)st.term, st.leader_id.c_str(),
               (long long)st.commit_index, (long long)st.num_peers);
    }

    // 3. 使用 ClusterManager 组建集群
    // connect(cluster_name, remote_addr, secret)
    printf("\n3. Cluster 集群组建\n");
    auto& cluster = ClusterManager::instance();

    bool c1 = cluster.connect("my-cluster", "127.0.0.1:9001", "");
    bool c2 = cluster.connect("my-cluster", "127.0.0.1:9002", "");
    bool c3 = cluster.connect("my-cluster", "127.0.0.1:9003", "");

    printf("   node-1 加入集群: %s\n", c1 ? "成功" : "失败");
    printf("   node-2 加入集群: %s\n", c2 ? "成功" : "失败");
    printf("   node-3 加入集群: %s\n", c3 ? "成功" : "失败");

    // 4. 查看集群状态
    printf("\n4. 集群状态\n");
    auto clusters = cluster.list_clusters();
    printf("   集群数量: %zu\n", clusters.size());
    for (auto& cname : clusters) {
        printf("   集群名: %s\n", cname.c_str());
        auto cs = cluster.get_status(cname);
        printf("   连接状态: %s\n", cs.connected ? "已连接" : "未连接");
        printf("   健康状态: %s\n", cs.health.c_str());
        printf("   领导节点: %s\n", cs.leader_id.c_str());
        printf("   任期: %lld\n", (long long)cs.term);
        printf("   节点数: %lld\n", (long long)cs.num_nodes);
        for (auto& n : cs.nodes) {
            printf("     - %s @ %s [%s/%s] 任期=%lld\n",
                   n.node_id.c_str(), n.addr.c_str(),
                   n.role.c_str(), n.status.c_str(), (long long)n.term);
        }
    }

    // 5. KvAdmin 节点生命周期管理
    printf("\n5. KvAdmin 节点管理\n");
    auto kv_nodes = admin.list_nodes();
    printf("   KvAdmin 管理节点数: %zu\n", kv_nodes.size());
    for (auto& n : kv_nodes) {
        printf("   - %s: PID=%lld, 地址=%s, 重启次数=%lld\n",
               n.node_id.c_str(), (long long)n.pid,
               n.addr.c_str(), (long long)n.restart_count);
    }

    // 6. 健康检查
    printf("\n6. 健康检查\n");
    auto health_map = cluster.health_check_all();
    printf("   检查节点数: %zu\n", health_map.size());
    for (auto& [name, healthy] : health_map) {
        printf("   - %s: %s\n", name.c_str(), healthy ? "健康" : "异常");
    }

    printf("\n=== 测试完成 ===\n");
    printf("成功创建 3 个分布式 Raft 节点，组建 1 个集群，并通过 KvAdmin 完成节点生命周期管理。\n");

    return 0;
}
