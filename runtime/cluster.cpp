#include "cluster.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <mutex>

// 客户端 TLS 上下文(跨机通信加密,跳过 CA 验证但保留加密通道)
static SSL_CTX *g_clientTlsCtx = nullptr;
static bool g_clientTlsEnabled = false;
static void clientTlsInit() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        SSL_library_init();
        SSL_load_error_strings();
        g_clientTlsCtx = SSL_CTX_new(TLS_client_method());
        if (g_clientTlsCtx) {
            SSL_CTX_set_verify(g_clientTlsCtx, SSL_VERIFY_NONE, nullptr);
            g_clientTlsEnabled = true;
        }
    });
}
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <errno.h>

// ============================================================================
// 辅助函数
// ============================================================================

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 简易 URL 解析：从 "host:port/path" 中提取 host 和 port
static bool parse_url(const std::string& url, std::string& host, int& port, std::string& path) {
    std::string remaining = url;

    // 去掉 http:// 前缀
    if (remaining.rfind("http://", 0) == 0) {
        remaining = remaining.substr(7);
    }

    // 分离 path
    size_t slash = remaining.find('/');
    if (slash != std::string::npos) {
        path = remaining.substr(slash);
        remaining = remaining.substr(0, slash);
    } else {
        path = "/";
    }

    // 分离 host:port
    size_t colon = remaining.find(':');
    if (colon != std::string::npos) {
        host = remaining.substr(0, colon);
        try {
            port = std::stoi(remaining.substr(colon + 1));
        } catch (...) {
            return false;
        }
    } else {
        host = remaining;
        port = 80;
    }

    return true;
}

// 简易 TCP 连接（非阻塞 + 超时）
static int tcp_connect(const std::string& host, int port, int64_t timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    // 设置非阻塞
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        close(sock);
        return -1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }

    if (ret == 0) {
        // 立即连接成功
        fcntl(sock, F_SETFL, flags);  // 恢复阻塞
        return sock;
    }

    // 等待连接完成
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select(sock + 1, nullptr, &wfds, nullptr, &tv);
    if (ret <= 0) {
        close(sock);
        return -1;
    }

    // 检查连接是否成功
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        close(sock);
        return -1;
    }

    fcntl(sock, F_SETFL, flags);  // 恢复阻塞
    return sock;
}

// 简易 HTTP 请求
static RemoteResult http_request(const std::string& method,
                                  const std::string& url,
                                  const std::string& body,
                                  const std::string& secret,
                                  int64_t timeout_ms) {
    RemoteResult result{};
    result.success = false;
    int64_t start = now_ms();

    std::string host;
    int port;
    std::string path;
    if (!parse_url(url, host, port, path)) {
        result.error = "invalid url: " + url;
        result.elapsed_ms = now_ms() - start;
        return result;
    }

    int sock = tcp_connect(host, port, timeout_ms);
    if (sock < 0) {
        result.error = "connection failed: " + host + ":" + std::to_string(port);
        result.elapsed_ms = now_ms() - start;
        return result;
    }
    SSL *ssl = nullptr;
    if (!std::getenv("XMEX_NO_TLS")) {
        clientTlsInit();
        if (g_clientTlsEnabled) {
            ssl = SSL_new(g_clientTlsCtx);
            SSL_set_fd(ssl, sock);
            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl); ssl = nullptr;
                result.error = "TLS handshake failed";
                result.elapsed_ms = now_ms() - start;
                close(sock);
                return result;
            }
        }
    }

    // 设置读写超时
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // 构建 HTTP 请求
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "Content-Type: application/json\r\n";
    if (!secret.empty()) {
        req << "Authorization: Bearer " << secret << "\r\n";
    }
    req << "Connection: close\r\n";
    if (!body.empty()) {
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    if (!body.empty()) {
        req << body;
    }

    std::string req_str = req.str();
    ssize_t sent = ssl ? SSL_write(ssl, req_str.c_str(), static_cast<int>(req_str.size())) : send(sock, req_str.c_str(), req_str.size(), 0);
    if (sent < 0) {
        result.error = "send failed";
        close(sock);
        result.elapsed_ms = now_ms() - start;
        return result;
    }

    // 读取响应
    char buf[4096];
    std::string response;
    ssize_t n;
    while ((n = ssl ? SSL_read(ssl, buf, static_cast<int>(sizeof(buf) - 1)) : recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        response += buf;
    }
    close(sock);

    result.elapsed_ms = now_ms() - start;

    if (response.empty()) {
        result.error = "empty response";
        return result;
    }

    // 解析 HTTP 响应
    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        result.error = "invalid http response";
        return result;
    }

    std::string header = response.substr(0, header_end);
    result.body = response.substr(header_end + 4);

    // 解析状态码
    size_t sp1 = header.find(' ');
    if (sp1 != std::string::npos) {
        size_t sp2 = header.find(' ', sp1 + 1);
        std::string code_str = header.substr(sp1 + 1, sp2 - sp1 - 1);
        try {
            result.status_code = std::stoll(code_str);
        } catch (...) {
            result.status_code = 0;
        }
    }

    result.success = (result.status_code >= 200 && result.status_code < 300);
    if (!result.success) {
        result.error = "HTTP " + std::to_string(result.status_code);
    }

    return result;
}

// ============================================================================
// ClusterManager 实现
// ============================================================================

ClusterManager& ClusterManager::instance() {
    static ClusterManager mgr;
    return mgr;
}

// ---- 连接管理 ----

bool ClusterManager::connect(const std::string& cluster_name,
                              const std::string& remote_addr,
                              const std::string& secret) {
    std::lock_guard<std::mutex> lk(mutex_);

    // 检查是否已连接
    if (connections_.find(cluster_name) != connections_.end()) {
        std::cerr << "[cluster] " << cluster_name << " already connected" << std::endl;
        return false;
    }

    ClusterConnection conn;
    conn.cluster_name = cluster_name;
    conn.remote_addr = remote_addr;
    conn.secret = secret;

    // 尝试建立连接（发送健康检查）
    std::string url = "http://" + remote_addr + "/status";
    RemoteResult result = http_get(url, secret, conn.timeout_ms);

    if (result.success) {
        conn.connected = true;
        conn.connected_at_ms = now_ms();
        conn.last_heartbeat_ms = now_ms();
        connections_[cluster_name] = conn;
        std::cout << "[cluster] connected to " << cluster_name
                  << " at " << remote_addr << " (" << result.elapsed_ms << "ms)" << std::endl;
        return true;
    }

    // 即使健康检查失败也记录连接（集群可能暂时不可达）
    conn.connected = false;
    conn.connected_at_ms = now_ms();
    connections_[cluster_name] = conn;
    std::cout << "[cluster] registered " << cluster_name
              << " at " << remote_addr << " (unreachable: " << result.error << ")" << std::endl;
    return true;
}

bool ClusterManager::disconnect(const std::string& cluster_name) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = connections_.find(cluster_name);
    if (it == connections_.end()) {
        return false;
    }
    connections_.erase(it);
    std::cout << "[cluster] disconnected from " << cluster_name << std::endl;
    return true;
}

bool ClusterManager::is_connected(const std::string& cluster_name) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = connections_.find(cluster_name);
    if (it == connections_.end()) return false;
    return it->second.connected;
}

std::vector<std::string> ClusterManager::list_clusters() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> names;
    for (const auto& [name, _] : connections_) {
        names.push_back(name);
    }
    return names;
}

// ---- 状态查询 ----

ClusterStatus ClusterManager::get_status(const std::string& cluster_name) {
    ClusterStatus st;
    st.cluster_name = cluster_name;
    st.connected = false;
    st.health = "unknown";

    std::lock_guard<std::mutex> lk(mutex_);
    auto it = connections_.find(cluster_name);
    if (it == connections_.end()) {
        st.health = "not_found";
        return st;
    }

    std::string url = "http://" + it->second.remote_addr + "/status";
    RemoteResult result = http_get(url, it->second.secret, it->second.timeout_ms);

    st.latency_ms = result.elapsed_ms;

    if (result.success) {
        st.connected = true;
        it->second.connected = true;
        it->second.last_heartbeat_ms = now_ms();
        return parse_status_json(cluster_name, result.body);
    }

    st.connected = false;
    it->second.connected = false;
    st.health = "down";
    st.error = result.error;
    return st;
}

std::vector<ClusterNodeInfo> ClusterManager::get_nodes(const std::string& cluster_name) {
    auto st = get_status(cluster_name);
    return st.nodes;
}

bool ClusterManager::health_check(const std::string& cluster_name) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = connections_.find(cluster_name);
    if (it == connections_.end()) return false;

    std::string url = "http://" + it->second.remote_addr + "/status";
    RemoteResult result = http_get(url, it->second.secret, it->second.timeout_ms);

    it->second.connected = result.success;
    if (result.success) {
        it->second.last_heartbeat_ms = now_ms();
    }
    return result.success;
}

// ---- 远程执行 ----

RemoteResult ClusterManager::exec(const std::string& cluster_name,
                                   const std::string& cmd_type,
                                   const std::string& payload) {
    RemoteResult result{};
    result.success = false;

    std::lock_guard<std::mutex> lk(mutex_);
    auto it = connections_.find(cluster_name);
    if (it == connections_.end()) {
        result.error = "cluster not found: " + cluster_name;
        return result;
    }

    std::string url = "http://" + it->second.remote_addr + "/exec";
    std::string body = "{\"type\":\"" + cmd_type + "\",\"payload\":" + payload + "}";
    return http_post(url, body, it->second.secret, it->second.timeout_ms);
}

RemoteResult ClusterManager::query(const std::string& cluster_name,
                                    const std::string& path) {
    RemoteResult result{};
    result.success = false;

    std::lock_guard<std::mutex> lk(mutex_);
    auto it = connections_.find(cluster_name);
    if (it == connections_.end()) {
        result.error = "cluster not found: " + cluster_name;
        return result;
    }

    std::string url = "http://" + it->second.remote_addr + path;
    return http_get(url, it->second.secret, it->second.timeout_ms);
}

// ---- 批量操作 ----

std::map<std::string, RemoteResult> ClusterManager::broadcast(
        const std::string& cmd_type, const std::string& payload) {
    std::map<std::string, RemoteResult> results;
    std::vector<std::string> clusters;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& [name, _] : connections_) {
            clusters.push_back(name);
        }
    }
    for (const auto& name : clusters) {
        results[name] = exec(name, cmd_type, payload);
    }
    return results;
}

std::map<std::string, bool> ClusterManager::health_check_all() {
    std::map<std::string, bool> results;
    std::vector<std::string> clusters;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& [name, _] : connections_) {
            clusters.push_back(name);
        }
    }
    for (const auto& name : clusters) {
        results[name] = health_check(name);
    }
    return results;
}

// ---- 内部方法 ----

RemoteResult ClusterManager::http_post(const std::string& url,
                                        const std::string& body,
                                        const std::string& secret,
                                        int64_t timeout_ms) {
    return http_request("POST", url, body, secret, timeout_ms);
}

RemoteResult ClusterManager::http_get(const std::string& url,
                                       const std::string& secret,
                                       int64_t timeout_ms) {
    return http_request("GET", url, "", secret, timeout_ms);
}

// 从单个 JSON 对象中提取字段（辅助函数）
static void extract_str_obj(const std::string& obj, const std::string& key, std::string& out) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = obj.find(search);
    if (pos != std::string::npos) {
        pos += search.size();
        size_t end = obj.find('"', pos);
        if (end != std::string::npos) {
            out = obj.substr(pos, end - pos);
        }
    }
}

static void extract_int_obj(const std::string& obj, const std::string& key, int64_t& out) {
    std::string search = "\"" + key + "\":";
    size_t pos = obj.find(search);
    if (pos != std::string::npos) {
        pos += search.size();
        size_t end = obj.find_first_of(",}", pos);
        if (end != std::string::npos) {
            try { out = std::stoll(obj.substr(pos, end - pos)); }
            catch (...) {}
        }
    }
}

ClusterStatus ClusterManager::parse_status_json(const std::string& cluster_name,
                                                  const std::string& json) {
    ClusterStatus st;
    st.cluster_name = cluster_name;
    st.connected = true;
    st.health = "ok";

    // 简易 JSON 解析（提取关键字段）
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
                try { out = std::stoll(json.substr(pos, end - pos)); }
                catch (...) {}
            }
        }
    };

    extract_str("leader_id", st.leader_id);
    extract_int("term", st.term);
    extract_int("num_nodes", st.num_nodes);
    extract_str("health", st.health);

    // 解析节点列表
    size_t nodes_pos = json.find("\"nodes\":[");
    if (nodes_pos != std::string::npos) {
        nodes_pos += 9;
        size_t end = json.find(']', nodes_pos);
        if (end != std::string::npos) {
            std::string nodes_str = json.substr(nodes_pos, end - nodes_pos);
            // 按 "},{ " 分割每个节点对象
            size_t obj_start = 0;
            while (obj_start < nodes_str.size()) {
                size_t obj_begin = nodes_str.find('{', obj_start);
                if (obj_begin == std::string::npos) break;
                size_t obj_end = nodes_str.find('}', obj_begin);
                if (obj_end == std::string::npos) break;

                std::string obj = nodes_str.substr(obj_begin, obj_end - obj_begin + 1);
                ClusterNodeInfo node;
                extract_str_obj(obj, "node_id", node.node_id);
                extract_str_obj(obj, "addr", node.addr);
                extract_str_obj(obj, "role", node.role);
                extract_str_obj(obj, "status", node.status);
                extract_int_obj(obj, "term", node.term);
                extract_int_obj(obj, "commit_index", node.commit_index);
                extract_int_obj(obj, "uptime_ms", node.uptime_ms);
                st.nodes.push_back(node);

                obj_start = obj_end + 1;
            }
        }
    }

    return st;
}

// 从单个 JSON 对象中提取字段（辅助函数）
