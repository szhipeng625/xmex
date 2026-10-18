#include "cluster.h"
#include "../include/xmex/runtime.h"
#include <cstring>
#include <sstream>

extern "C" void trace_op(const char *category, const char *op,
                            const char *detail, int ok, int64_t elapsed);
extern "C" int32_t trace_enabled();
#include <chrono>
static int64_t traceNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
extern "C" {

// ============================================================================
// 连接管理
// ============================================================================

// cluster.connect(name, addr, secret) → 连接到远程集群
xmex::Value cluster_connect(const char *name, int64_t nameLen,
                            const char *addr, int64_t addrLen,
                            const char *secret, int64_t secretLen) {
    std::string n(name, nameLen);
    std::string a(addr, addrLen);
    std::string s(secret, secretLen);

    bool ok = ClusterManager::instance().connect(n, a, s);

    std::ostringstream json;
    json << "{";
    json << "\"cluster\":\"" << n << "\",";
    json << "\"addr\":\"" << a << "\",";
    json << "\"status\":\"" << (ok ? "connected" : "failed") << "\"";
    json << "}";
    std::string result = json.str();
    return str_new(result.c_str(), result.size());
}

// cluster.disconnect(name) → 断开集群连接
xmex::Value cluster_disconnect(const char *name, int64_t nameLen) {
    std::string n(name, nameLen);
    bool ok = ClusterManager::instance().disconnect(n);

    std::string result = ok
        ? "{\"status\":\"disconnected\",\"cluster\":\"" + n + "\"}"
        : "{\"error\":\"not found\",\"cluster\":\"" + n + "\"}";
    return str_new(result.c_str(), result.size());
}

// cluster.list() → 列出所有已连接集群
xmex::Value cluster_list() {
    auto clusters = ClusterManager::instance().list_clusters();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < clusters.size(); ++i) {
        if (i > 0) json << ",";
        json << "\"" << clusters[i] << "\"";
    }
    json << "]";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// cluster.is_connected(name) → 检查是否已连接
int64_t cluster_is_connected(const char *name, int64_t nameLen) {
    std::string n(name, nameLen);
    return ClusterManager::instance().is_connected(n) ? 1 : 0;
}

// ============================================================================
// 状态查询
// ============================================================================

// cluster.status(name) → 获取集群状态 JSON
xmex::Value cluster_status(const char *name, int64_t nameLen) {
    std::string n(name, nameLen);
    auto st = ClusterManager::instance().get_status(n);

    std::ostringstream json;
    json << "{";
    json << "\"cluster_name\":\"" << st.cluster_name << "\",";
    json << "\"connected\":" << (st.connected ? "true" : "false") << ",";
    json << "\"leader_id\":\"" << st.leader_id << "\",";
    json << "\"term\":" << st.term << ",";
    json << "\"num_nodes\":" << st.num_nodes << ",";
    json << "\"health\":\"" << st.health << "\",";
    json << "\"latency_ms\":" << st.latency_ms << ",";
    json << "\"nodes\":[";
    for (size_t i = 0; i < st.nodes.size(); ++i) {
        if (i > 0) json << ",";
        json << "{";
        json << "\"node_id\":\"" << st.nodes[i].node_id << "\",";
        json << "\"addr\":\"" << st.nodes[i].addr << "\",";
        json << "\"role\":\"" << st.nodes[i].role << "\",";
        json << "\"status\":\"" << st.nodes[i].status << "\",";
        json << "\"term\":" << st.nodes[i].term << ",";
        json << "\"commit_index\":" << st.nodes[i].commit_index << ",";
        json << "\"uptime_ms\":" << st.nodes[i].uptime_ms;
        json << "}";
    }
    json << "]}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// cluster.nodes(name) → 获取集群节点列表 JSON
xmex::Value cluster_nodes(const char *name, int64_t nameLen) {
    std::string n(name, nameLen);
    auto nodes = ClusterManager::instance().get_nodes(n);

    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i > 0) json << ",";
        json << "{";
        json << "\"node_id\":\"" << nodes[i].node_id << "\",";
        json << "\"addr\":\"" << nodes[i].addr << "\",";
        json << "\"role\":\"" << nodes[i].role << "\",";
        json << "\"status\":\"" << nodes[i].status << "\"";
        json << "}";
    }
    json << "]";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// cluster.health(name) → 健康检查
int64_t cluster_health(const char *name, int64_t nameLen) {
    std::string n(name, nameLen);
    return ClusterManager::instance().health_check(n) ? 1 : 0;
}

// ============================================================================
// 远程执行
// ============================================================================

// cluster.exec(name, cmd_type, payload) → 向集群发送命令
xmex::Value cluster_exec(const char *name, int64_t nameLen,
                         const char *cmd_type, int64_t typeLen,
                         const char *payload, int64_t payloadLen) {
    std::string n(name, nameLen);
    std::string t(cmd_type, typeLen);
    std::string p(payload, payloadLen);

    int64_t t0_ = traceNowMs();
    auto result = ClusterManager::instance().exec(n, t, p);
    if (trace_enabled()) { std::string d_ = n + "/" + t; trace_op("cluster", "exec", d_.c_str(), result.success ? 1 : 0, traceNowMs() - t0_); }

    std::ostringstream json;
    json << "{";
    json << "\"success\":" << (result.success ? "true" : "false") << ",";
    json << "\"status_code\":" << result.status_code << ",";
    json << "\"elapsed_ms\":" << result.elapsed_ms << ",";
    if (!result.error.empty()) {
        json << "\"error\":\"" << result.error << "\",";
    }
    json << "\"body\":" << (result.body.empty() ? "null" : result.body);
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// cluster.query(name, path) → 向集群发送 GET 请求
xmex::Value cluster_query(const char *name, int64_t nameLen,
                          const char *path, int64_t pathLen) {
    std::string n(name, nameLen);
    std::string p(path, pathLen);

    auto result = ClusterManager::instance().query(n, p);

    std::ostringstream json;
    json << "{";
    json << "\"success\":" << (result.success ? "true" : "false") << ",";
    json << "\"status_code\":" << result.status_code << ",";
    json << "\"elapsed_ms\":" << result.elapsed_ms << ",";
    if (!result.error.empty()) {
        json << "\"error\":\"" << result.error << "\",";
    }
    json << "\"body\":" << (result.body.empty() ? "null" : result.body);
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// ============================================================================
// 批量操作
// ============================================================================

// cluster.broadcast(cmd_type, payload) → 向所有集群广播命令
xmex::Value cluster_broadcast(const char *cmd_type, int64_t typeLen,
                              const char *payload, int64_t payloadLen) {
    std::string t(cmd_type, typeLen);
    std::string p(payload, payloadLen);

    auto results = ClusterManager::instance().broadcast(t, p);

    std::ostringstream json;
    json << "{";
    bool first = true;
    for (const auto& [name, result] : results) {
        if (!first) json << ",";
        first = false;
        json << "\"" << name << "\":{";
        json << "\"success\":" << (result.success ? "true" : "false") << ",";
        json << "\"elapsed_ms\":" << result.elapsed_ms;
        if (!result.error.empty()) {
            json << ",\"error\":\"" << result.error << "\"";
        }
        json << "}";
    }
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// cluster.health_all() → 所有集群健康检查
xmex::Value cluster_health_all() {
    auto results = ClusterManager::instance().health_check_all();

    std::ostringstream json;
    json << "{";
    bool first = true;
    for (const auto& [name, ok] : results) {
        if (!first) json << ",";
        first = false;
        json << "\"" << name << "\":" << (ok ? "true" : "false");
    }
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

}  // extern "C"
