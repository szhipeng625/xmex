// lsm_client.h —— LSM (Redis 协议) 极简客户端,供节点注册表持久化
#pragma once
#include <string>
#include <vector>

namespace xmex_lsm {

// 极简 Redis(RESP) 客户端:支持 SET/GET/DEL/KEYS,同步阻塞,超时短
// 用法: LsmClient c("43.157.39.171", 6380); c.set("k","v"); auto v = c.get("k");
class LsmClient {
public:
    LsmClient(const std::string& host, int port, int timeout_ms = 3000);
    ~LsmClient();
    bool ok() const { return fd_ >= 0; }
    bool set(const std::string& key, const std::string& val);
    bool get(const std::string& key, std::string& val);
    bool del(const std::string& key);
    bool keys(const std::string& pattern, std::vector<std::string>& out);
    // ---- 队列原语(节点内置 MQ 扩展,2026-10) ----
    long long lpush(const std::string& key, const std::string& val);   // 入队,返回队列长度,-1 失败
    bool rpop(const std::string& key, std::string& val);               // 出队(FIFO),空队列返回 false
    long long llen(const std::string& key);                            // 队列长度,-1 失败
    long long incr(const std::string& key);                            // 原子自增,返回新值,-1 失败
    long long decr(const std::string& key);                            // 原子自减,返回新值,-1 失败
private:
    int fd_{-1};
    int timeout_ms_;
    bool cmd(const std::vector<std::string>& parts, std::string& reply);
};

}  // namespace xmex_lsm
