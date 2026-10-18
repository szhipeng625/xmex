// lsm_client.cpp —— LSM (Redis 协议) 极简客户端实现
#include "lsm_client.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

namespace xmex_lsm {

LsmClient::LsmClient(const std::string& host, int port, int timeout_ms)
    : timeout_ms_(timeout_ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return;
    sockaddr_in addr = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    freeaddrinfo(res);
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return;
    timeval tv{timeout_ms_ / 1000, (timeout_ms_ % 1000) * 1000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd_);
        fd_ = -1;
    }
}

LsmClient::~LsmClient() { if (fd_ >= 0) close(fd_); }

// 发送 RESP 命令并读取回复
bool LsmClient::cmd(const std::vector<std::string>& parts, std::string& reply) {
    if (fd_ < 0) return false;
    std::ostringstream oss;
    oss << "*" << parts.size() << "\r\n";
    for (const auto& p : parts) {
        oss << "$" << p.size() << "\r\n" << p << "\r\n";
    }
    std::string req = oss.str();
    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = send(fd_, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    char buf[8192];
    ssize_t n = recv(fd_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = 0;
    reply.assign(buf, static_cast<size_t>(n));
    return true;
}

bool LsmClient::set(const std::string& key, const std::string& val) {
    std::string r;
    if (!cmd({"SET", key, val}, r)) return false;
    return r.rfind("+OK", 0) == 0;
}

bool LsmClient::get(const std::string& key, std::string& val) {
    std::string r;
    if (!cmd({"GET", key}, r)) return false;
    if (r.rfind("$-1", 0) == 0) return false;
    size_t p = r.find("\r\n");
    if (p == std::string::npos) return false;
    size_t s = p + 2;
    size_t e = r.find("\r\n", s);
    if (e == std::string::npos) e = r.size();
    val = r.substr(s, e - s);
    return true;
}

bool LsmClient::del(const std::string& key) {
    std::string r;
    return cmd({"DEL", key}, r);
}

// ---- 队列原语实现 ----
// 解析 RESP 整数回复(":123\r\n"),失败返回 -1
static long long parse_int_reply(const std::string& r) {
    if (r.empty() || r[0] != ':') return -1;
    return std::atoll(r.c_str() + 1);
}

long long LsmClient::lpush(const std::string& key, const std::string& val) {
    std::string r;
    if (!cmd({"LPUSH", key, val}, r)) return -1;
    return parse_int_reply(r);
}

bool LsmClient::rpop(const std::string& key, std::string& val) {
    std::string r;
    if (!cmd({"RPOP", key}, r)) return false;
    if (r.rfind("$-1", 0) == 0) return false;   // 空队列
    size_t p = r.find("\r\n");
    if (p == std::string::npos) return false;
    size_t s = p + 2;
    size_t e = r.find("\r\n", s);
    if (e == std::string::npos) e = r.size();
    val = r.substr(s, e - s);
    return true;
}

long long LsmClient::llen(const std::string& key) {
    std::string r;
    if (!cmd({"LLEN", key}, r)) return -1;
    return parse_int_reply(r);
}

long long LsmClient::incr(const std::string& key) {
    std::string r;
    if (!cmd({"INCR", key}, r)) return -1;
    return parse_int_reply(r);
}

long long LsmClient::decr(const std::string& key) {
    std::string r;
    if (!cmd({"DECR", key}, r)) return -1;
    return parse_int_reply(r);
}

bool LsmClient::keys(const std::string& pattern, std::vector<std::string>& out) {
    std::string r;
    if (!cmd({"KEYS", pattern}, r)) return false;
    out.clear();
    size_t pos = 0;
    while (true) {
        size_t d = r.find("$", pos);
        if (d == std::string::npos) break;
        size_t nl = r.find("\r\n", d);
        if (nl == std::string::npos) break;
        int len = std::atoi(r.substr(d + 1, nl - d - 1).c_str());
        if (len <= 0) { pos = nl + 2; continue; }
        size_t vs = nl + 2;
        out.push_back(r.substr(vs, static_cast<size_t>(len)));
        pos = vs + static_cast<size_t>(len) + 2;
    }
    return true;
}

}  // namespace xmex_lsm
