#include "raft.h"
#include <chrono>
static int64_t raftTraceNowMs(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
#include "../include/xmex/runtime.h"
#include <cstring>
#include <sstream>

extern "C" {

// ============================================================================
// 节点生命周期
// ============================================================================

// raft.build(config_json) → 返回节点 ID 字符串
// config_json 格式: {"node_id":"...","bind_addr":"...","cluster_secret":"...","peers":[...],"data_dir":"..."}
xmex::Value raft_build(const char *config_json, int64_t jsonLen) {
    std::string json(config_json, jsonLen);

    // 简单 JSON 解析（生产环境应使用完整 JSON 库）
    RaftNodeConfig config;

    // 解析 node_id
    auto extract_str = [&](const std::string& key, std::string& out) {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);
        if (pos != std::string::npos) {
            pos += search.size();
            size_t end = json.find('"', pos);
            if (end != std::string::npos) {
                out = json.substr(pos, end - pos);
            }
        }
    };

    auto extract_int = [&](const std::string& key, int64_t& out) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos != std::string::npos) {
            pos += search.size();
            size_t end = json.find_first_of(",}", pos);
            if (end != std::string::npos) {
                out = std::stoll(json.substr(pos, end - pos));
            }
        }
    };

    extract_str("node_id", config.node_id);
    extract_str("bind_addr", config.bind_addr);
    extract_str("cluster_secret", config.cluster_secret);
    extract_str("data_dir", config.data_dir);

    // 解析 peers 数组
    size_t peers_pos = json.find("\"peers\":[");
    if (peers_pos != std::string::npos) {
        peers_pos += 9; // 跳过 "peers":[
        size_t end = json.find(']', peers_pos);
        if (end != std::string::npos) {
            std::string peers_str = json.substr(peers_pos, end - peers_pos);
            size_t start = 0;
            while (start < peers_str.size()) {
                size_t q1 = peers_str.find('"', start);
                if (q1 == std::string::npos) break;
                size_t q2 = peers_str.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                config.peers.push_back(peers_str.substr(q1 + 1, q2 - q1 - 1));
                start = q2 + 1;
            }
        }
    }

    extract_int("election_timeout_ms", config.election_timeout_ms);
    extract_int("heartbeat_interval_ms", config.heartbeat_interval_ms);

    if (config.node_id.empty()) {
        return str_new("{\"error\":\"node_id is required\"}", 27);
    }

    int64_t bt_ = raftTraceNowMs();
    RaftNode* node = RaftNode::build(config);
    if (trace_enabled()) trace_op("raft", "build", config.node_id.c_str(), node ? 1 : 0, raftTraceNowMs() - bt_);
    if (!node) {
        return str_new("{\"error\":\"build failed\"}", 22);
    }

    std::string result = "{\"node_id\":\"" + config.node_id + "\",\"status\":\"created\"}";
    return str_new(result.c_str(), result.size());
}

// raft.erase(node_id) → 删除节点
xmex::Value raft_erase(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    RaftNode::erase(id);
    std::string result = "{\"status\":\"erased\",\"node_id\":\"" + id + "\"}";
    return str_new(result.c_str(), result.size());
}

// raft.status(node_id) → 返回节点状态 JSON
xmex::Value raft_status(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    auto st = RaftNode::status(id);

    std::ostringstream json;
    json << "{";
    json << "\"node_id\":\"" << st.node_id << "\",";
    json << "\"role\":\"" << (st.role == RaftRole::LEADER ? "leader" :
                              st.role == RaftRole::CANDIDATE ? "candidate" : "follower") << "\",";
    json << "\"term\":" << st.term << ",";
    json << "\"leader_id\":\"" << st.leader_id << "\",";
    json << "\"commit_index\":" << st.commit_index << ",";
    json << "\"last_applied\":" << st.last_applied << ",";
    json << "\"num_peers\":" << st.num_peers << ",";
    json << "\"uptime_ms\":" << st.uptime_ms << ",";
    json << "\"peer_list\":[";
    for (size_t i = 0; i < st.peer_list.size(); ++i) {
        if (i > 0) json << ",";
        json << "\"" << st.peer_list[i] << "\"";
    }
    json << "]}";

    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// raft.list_nodes() → 返回所有节点 ID 列表 JSON
xmex::Value raft_list_nodes() {
    auto ids = RaftNode::list_nodes();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) json << ",";
        json << "\"" << ids[i] << "\"";
    }
    json << "]";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// ============================================================================
// 状态机命令
// ============================================================================

// raft.add(node_id, cmd_type, payload) → 提交命令到集群
xmex::Value raft_add(const char *node_id, int64_t idLen,
                    const char *cmd_type, int64_t typeLen,
                    const char *payload, int64_t payloadLen) {
    std::string id(node_id, idLen);
    std::string type(cmd_type, typeLen);
    std::string pl(payload, payloadLen);

    // 查找节点
    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end()) {
        return str_new("{\"error\":\"node not found\"}", 26);
    }

    std::string result = it->second->propose(type, pl);
    return str_new(result.c_str(), result.size());
}

// raft.register_handler(node_id, cmd_type) → 注册命令处理器（返回成功标志）
int64_t raft_register_handler(const char *node_id, int64_t idLen,
                                  const char *cmd_type, int64_t typeLen) {
    std::string id(node_id, idLen);
    std::string type(cmd_type, typeLen);

    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end()) return 0;

    // 注册一个默认处理器（记录命令并返回确认）
    it->second->register_handler(type, [type](const RaftCommand& cmd) -> std::string {
        std::ostringstream json;
        json << "{";
        json << "\"status\":\"applied\",";
        json << "\"cmd_type\":\"" << type << "\",";
        json << "\"from\":\"" << cmd.from_node << "\",";
        json << "\"index\":" << cmd.timestamp_ms;
        json << "}";
        return json.str();
    });

    return 1;
}

// ============================================================================
// 节点管理
// ============================================================================

// raft.register_node(node_id, role, addr) → 注册节点角色
xmex::Value raft_register_node(const char *node_id, int64_t idLen,
                               const char *role, int64_t roleLen,
                               const char *addr, int64_t addrLen) {
    std::string id(node_id, idLen);
    std::string r(role, roleLen);
    std::string a(addr, addrLen);

    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end()) {
        return str_new("{\"error\":\"node not found\"}", 26);
    }

    it->second->register_node_role(r, a);
    std::string result = "{\"status\":\"registered\",\"node\":\"" + a + "\",\"role\":\"" + r + "\"}";
    return str_new(result.c_str(), result.size());
}

// raft.cluster_nodes(node_id) → 返回集群节点列表 JSON
xmex::Value raft_cluster_nodes(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);

    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end()) {
        return str_new("[]", 2);
    }

    auto nodes = it->second->get_cluster_nodes();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i > 0) json << ",";
        json << "{";
        json << "\"node_id\":\"" << nodes[i].node_id << "\",";
        json << "\"addr\":\"" << nodes[i].addr << "\",";
        json << "\"role\":\"" << nodes[i].role << "\",";
        json << "\"status\":\"" << nodes[i].status << "\",";
        json << "\"term\":" << nodes[i].term;
        json << "}";
    }
    json << "]";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// raft.mount_storage(node_id, name, path) → 注册共享存储
xmex::Value raft_mount_storage(const char *node_id, int64_t idLen,
                               const char *name, int64_t nameLen,
                               const char *path, int64_t pathLen) {
    std::string id(node_id, idLen);
    std::string n(name, nameLen);
    std::string p(path, pathLen);

    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end()) {
        return str_new("{\"error\":\"node not found\"}", 26);
    }

    it->second->mount_storage(n, p);
    std::string result = "{\"status\":\"mounted\",\"name\":\"" + n + "\",\"path\":\"" + p + "\"}";
    return str_new(result.c_str(), result.size());
}

// ============================================================================
// 动态插件
// ============================================================================

// raft.plugin_load(node_id, name, so_path) → 加载插件
xmex::Value raft_plugin_load(const char *node_id, int64_t idLen,
                             const char *name, int64_t nameLen,
                             const char *so_path, int64_t pathLen) {
    std::string id(node_id, idLen);
    std::string n(name, nameLen);
    std::string p(so_path, pathLen);

    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end()) {
        return str_new("{\"error\":\"node not found\"}", 26);
    }

    bool ok = it->second->load_plugin(n, p);
    std::string result = ok
        ? "{\"status\":\"loaded\",\"name\":\"" + n + "\"}"
        : "{\"error\":\"load failed\"}";
    return str_new(result.c_str(), result.size());
}

// raft.plugin_unload(node_id, name) → 卸载插件
xmex::Value raft_plugin_unload(const char *node_id, int64_t idLen,
                               const char *name, int64_t nameLen) {
    std::string id(node_id, idLen);
    std::string n(name, nameLen);

    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end()) {
        return str_new("{\"error\":\"node not found\"}", 26);
    }

    bool ok = it->second->unload_plugin(n);
    std::string result = ok
        ? "{\"status\":\"unloaded\",\"name\":\"" + n + "\"}"
        : "{\"error\":\"plugin not found\"}";
    return str_new(result.c_str(), result.size());
}

// raft.plugin_list(node_id) → 列出已加载插件
xmex::Value raft_plugin_list(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);

    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(id);
    if (it == g_nodes.end()) {
        return str_new("[]", 2);
    }

    auto plugins = it->second->list_plugins();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < plugins.size(); ++i) {
        if (i > 0) json << ",";
        json << "{";
        json << "\"name\":\"" << plugins[i].name << "\",";
        json << "\"path\":\"" << plugins[i].path << "\",";
        json << "\"version\":\"" << plugins[i].version << "\",";
        json << "\"loaded\":" << (plugins[i].loaded ? "true" : "false");
        json << "}";
    }
    json << "]";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

}  // extern "C"

// ============================================================================
// 分布式便捷内建:选主查询与节点消息收件箱(2026-10 增补)
// ============================================================================
#include <queue>
#include <map>

// 每个 raft 节点一个简单消息队列(进程内),配合 server /exec 端点投递
static std::map<std::string, std::queue<std::string>> g_inbox;
static std::mutex g_inbox_mutex;

extern "C" xmex::Value raft_leader_info(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    auto st = RaftNode::status(id);
    std::ostringstream json;
    json << "{";
    json << "\"node_id\":\"" << st.node_id << "\",";
    json << "\"role\":\"" << (st.role == RaftRole::LEADER ? "leader" :
                              st.role == RaftRole::CANDIDATE ? "candidate" : "follower") << "\",";
    json << "\"leader_id\":\"" << st.leader_id << "\",";
    json << "\"is_leader\":" << (st.role == RaftRole::LEADER ? "true" : "false") << ",";
    json << "\"term\":" << st.term;
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

extern "C" xmex::Value raft_send(const char *node_id, int64_t idLen,
                     const char *msg, int64_t msgLen) {
    std::string id(node_id, idLen);
    std::string m(msg, msgLen);
    {
        std::lock_guard<std::mutex> lk(g_inbox_mutex);
        g_inbox[id].push(m);
    }
    std::string r = "{\"status\":\"queued\",\"node\":\"" + id + "\"}";
    return str_new(r.c_str(), r.size());
}

extern "C" xmex::Value raft_recv(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::string m;
    {
        std::lock_guard<std::mutex> lk(g_inbox_mutex);
        auto it = g_inbox.find(id);
        if (it != g_inbox.end() && !it->second.empty()) {
            m = it->second.front();
            it->second.pop();
        }
    }
    return str_new(m.c_str(), m.size());
}
