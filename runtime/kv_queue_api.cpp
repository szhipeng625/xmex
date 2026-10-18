// kv_queue_api.cpp —— 节点内置 MQ 队列 API(2026-10 新增)
// 设计:以共享 LSM 存储为介质,用 LPUSH/RPOP 实现 FIFO 任务队列,
//       用 INCR/DECR 计数器实现每个节点的在评配额管理。
// 键结构:
//   xmex:mq:{queue}        列表,任务负载(FIFO:LPUSH 入 / RPOP 出)
//   xmex:quota:{node}      字符串,节点配额上限(可同时领取的任务数)
//   xmex:inflight:{node}   计数器,节点当前在评任务数
#include "kv_admin.h"
#include "lsm_client.h"
#include "../include/xmex/runtime.h"
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace {
// ---- 值转义:共享存储不容忍值中的换行符(协议按行解析) ----
// 写入侧把换行与反斜杠转义;读取侧还原。对单行内容零影响。
std::string esc_val(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}
std::string unesc_val(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char n = in[i + 1];
            if (n == 'n') { out += '\n'; ++i; continue; }
            if (n == 'r') { out += '\r'; ++i; continue; }
            if (n == '\\') { out += '\\'; ++i; continue; }
        }
        out += in[i];
    }
    return out;
}

// 从 KvAdmin 持久化地址构造 LSM 客户端;未配置返回失效客户端
xmex_lsm::LsmClient make_client() {
    std::string addr = KvAdmin::instance().persistence_addr();
    std::string host = addr;
    int port = 6380;
    size_t c = addr.find(':');
    if (c != std::string::npos) {
        host = addr.substr(0, c);
        port = std::atoi(addr.substr(c + 1).c_str());
    }
    return xmex_lsm::LsmClient(host, port, 3000);
}

xmex::Value json_result(bool ok, const std::string& extra) {
    std::ostringstream j;
    j << "{\"success\":" << (ok ? "true" : "false");
    if (!extra.empty()) j << "," << extra;
    j << "}";
    std::string s = j.str();
    return str_new(s.c_str(), s.size());
}
}  // namespace

extern "C" {

// kv.enqueue(queue, payload) → 任务入队,返回 {"success":true,"qlen":N}
xmex::Value kv_enqueue(const char* queue, int64_t qLen,
                      const char* payload, int64_t pLen) {
    auto cli = make_client();
    if (!cli.ok()) return json_result(false, "\"error\":\"lsm unavailable\"");
    std::string key = "xmex:mq:" + std::string(queue, qLen);
    long long n = cli.lpush(key, esc_val(std::string(payload, pLen)));
    if (n < 0) return json_result(false, "\"error\":\"lpush failed\"");
    std::ostringstream ex;
    ex << "\"qlen\":" << n;
    return json_result(true, ex.str());
}

// kv.dequeue(node_id, queue) → 节点按配额领取任务
// 规则:节点在评数 < 配额上限时才允许领取;领取成功即在评数 +1。
// 返回约定(2026-10 修订,纯字符串便于语言层直接解析,无需 JSON):
//   领到任务 → 任务负载字符串本身
//   队列空   → "EMPTY"
//   配额已满 → "FULL"
//   存储不可用 → "ERROR"
// 配额未设置时默认上限 1(与现有消费者 prefetch=1 行为一致)。
xmex::Value kv_dequeue(const char* node_id, int64_t nLen,
                      const char* queue, int64_t qLen) {
    auto cli = make_client();
    if (!cli.ok()) return str_new("ERROR", 5);
    std::string node(node_id, nLen);
    std::string inflight_key = "xmex:inflight:" + node;
    // 读取配额上限(默认 1)
    long long quota = 1;
    std::string qs;
    if (cli.get("xmex:quota:" + node, qs) && !qs.empty()) {
        quota = std::atoll(qs.c_str());
        if (quota <= 0) quota = 1;
    }
    // 预占配额:先自增,超额则回退
    long long cur = cli.incr(inflight_key);
    if (cur < 0) return str_new("ERROR", 5);
    if (cur > quota) {
        cli.decr(inflight_key);
        return str_new("FULL", 4);
    }
    // 领取任务
    std::string task;
    std::string key = "xmex:mq:" + std::string(queue, qLen);
    if (!cli.rpop(key, task)) {
        cli.decr(inflight_key);  // 队列空,归还配额
        return str_new("EMPTY", 5);
    }
    std::string plain = unesc_val(task);
    return str_new(plain.c_str(), plain.size());
}

// kv.done(node_id) → 节点完成一个任务,在评数 -1
xmex::Value kv_done(const char* node_id, int64_t nLen) {
    auto cli = make_client();
    if (!cli.ok()) return json_result(false, "\"error\":\"lsm unavailable\"");
    long long cur = cli.decr("xmex:inflight:" + std::string(node_id, nLen));
    if (cur < 0) cur = 0;
    std::ostringstream ex;
    ex << "\"inflight\":" << cur;
    return json_result(true, ex.str());
}

// kv.qlen(queue) → 查询队列积压长度
xmex::Value kv_qlen(const char* queue, int64_t qLen) {
    auto cli = make_client();
    if (!cli.ok()) return json_result(false, "\"error\":\"lsm unavailable\"");
    long long n = cli.llen("xmex:mq:" + std::string(queue, qLen));
    if (n < 0) n = 0;
    std::ostringstream ex;
    ex << "\"qlen\":" << n;
    return json_result(true, ex.str());
}

// kv.set_quota(node_id, quota) → 设置节点配额上限(quota 为数字字符串)
xmex::Value kv_set_quota(const char* node_id, int64_t nLen,
                        const char* quota, int64_t qLen) {
    auto cli = make_client();
    if (!cli.ok()) return json_result(false, "\"error\":\"lsm unavailable\"");
    bool ok = cli.set("xmex:quota:" + std::string(node_id, nLen), std::string(quota, qLen));
    return json_result(ok, ok ? "" : "\"error\":\"set failed\"");
}

// ---- 通用 KV 读写(节点存取任务内容,2026-10 增补) ----
// kv.kset(key, value) → 写入任意键值
xmex::Value kv_kset(const char* key, int64_t kLen,
                   const char* value, int64_t vLen) {
    auto cli = make_client();
    if (!cli.ok()) return json_result(false, "\"error\":\"lsm unavailable\"");
    bool ok = cli.set(std::string(key, kLen), esc_val(std::string(value, vLen)));
    return json_result(ok, ok ? "" : "\"error\":\"set failed\"");
}

// kv.kget(key) → 读取键对应的值,不存在返回空串
xmex::Value kv_kget(const char* key, int64_t kLen) {
    auto cli = make_client();
    if (!cli.ok()) return str_new("", 0);
    std::string val;
    if (!cli.get(std::string(key, kLen), val)) val = "";
    std::string plain = unesc_val(val);
    return str_new(plain.c_str(), plain.size());
}

// ---- 本地文件写入(节点落盘代码供编译,2026-10 增补) ----
// fwrite(path, content) → 写入本地文件,返回 {"success":true}
xmex::Value file_write(const char* path, int64_t pLen,
                      const char* content, int64_t cLen) {
    std::string p(path, pLen);
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return json_result(false, "\"error\":\"open failed\"");
    size_t n = std::fwrite(content, 1, static_cast<size_t>(cLen), f);
    std::fclose(f);
    if (n != static_cast<size_t>(cLen)) return json_result(false, "\"error\":\"write failed\"");
    return json_result(true, "");
}

// fread(path) → 读取本地文件全部内容,不存在返回空串
xmex::Value file_read(const char* path, int64_t pLen) {
    std::string p(path, pLen);
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return str_new("", 0);
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string buf;
    buf.resize(sz > 0 ? static_cast<size_t>(sz) : 0);
    size_t n = sz > 0 ? std::fread(&buf[0], 1, static_cast<size_t>(sz), f) : 0;
    std::fclose(f);
    return str_new(buf.c_str(), n);
}

// str_int(s) → 字符串转整数(忽略首尾空白),供沙箱时限参数使用
int64_t str_to_int(const char* s, int64_t n) {
    std::string t(s, n);
    return std::atoll(t.c_str());
}

// int_to_str(v) → 整数转字符串,供拼接 task:{id}:in:{i} 等键使用
xmex::Value int_to_str(int64_t v) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return str_new(buf, n);
}

// kv.quota(node_id) → 查询节点配额与当前在评数
xmex::Value kv_quota(const char* node_id, int64_t nLen) {
    auto cli = make_client();
    if (!cli.ok()) return json_result(false, "\"error\":\"lsm unavailable\"");
    std::string node(node_id, nLen);
    std::string qs;
    long long quota = 1;
    if (cli.get("xmex:quota:" + node, qs) && !qs.empty()) {
        quota = std::atoll(qs.c_str());
        if (quota <= 0) quota = 1;
    }
    std::string inf;
    long long inflight = 0;
    if (cli.get("xmex:inflight:" + node, inf) && !inf.empty()) {
        inflight = std::atoll(inf.c_str());
    }
    std::ostringstream ex;
    ex << "\"quota\":" << quota << ",\"inflight\":" << inflight;
    return json_result(true, ex.str());
}

}  // extern "C"
