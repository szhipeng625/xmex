#include "kv_admin.h"
#include "lsm_client.h"
#include <iostream>
#include <chrono>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static const char* state_str(KvNodeState s) {
    switch (s) {
        case KvNodeState::IDLE:     return "idle";
        case KvNodeState::RUNNING:  return "running";
        case KvNodeState::SLEEPING: return "sleeping";
        case KvNodeState::STOPPED:  return "stopped";
        case KvNodeState::ERASED:   return "erased";
        case KvNodeState::LOST:     return "lost";
    }
    return "unknown";
}

KvAdmin& KvAdmin::instance() {
    static KvAdmin mgr;
    return mgr;
}

// ---- LSM 持久化实现 ----

void KvAdmin::configure_persistence(const std::string& addr) {
    std::lock_guard<std::mutex> lk(mutex_);
    lsm_addr_ = addr;
}

void KvAdmin::persist_node(const KvNode& node) {
    if (lsm_addr_.empty()) return;
    size_t c = lsm_addr_.find(':');
    std::string host = c == std::string::npos ? lsm_addr_ : lsm_addr_.substr(0, c);
    int port = c == std::string::npos ? 6380 : std::atoi(lsm_addr_.substr(c + 1).c_str());
    xmex_lsm::LsmClient cli(host, port, 2000);
    if (!cli.ok()) return;
    std::ostringstream json;
    json << "{\"node_id\":\"" << node.node_id << "\",\"addr\":\"" << node.addr
         << "\",\"binary\":\"" << node.binary << "\",\"work_dir\":\"" << node.work_dir
         << "\",\"state\":\"" << state_str(node.state) << "\",\"pid\":" << node.pid
         << ",\"restart_count\":" << node.restart_count << "}";
    cli.set("xmex:kv:node:" + node.node_id, json.str());
}

void KvAdmin::persist_erase(const std::string& node_id) {
    if (lsm_addr_.empty()) return;
    size_t c = lsm_addr_.find(':');
    std::string host = c == std::string::npos ? lsm_addr_ : lsm_addr_.substr(0, c);
    int port = c == std::string::npos ? 6380 : std::atoi(lsm_addr_.substr(c + 1).c_str());
    xmex_lsm::LsmClient cli(host, port, 2000);
    if (!cli.ok()) return;
    cli.del("xmex:kv:node:" + node_id);
}

void KvAdmin::restore_from_lsm() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (lsm_addr_.empty() || lsm_restored_) return;
    lsm_restored_ = true;
    size_t c = lsm_addr_.find(':');
    std::string host = c == std::string::npos ? lsm_addr_ : lsm_addr_.substr(0, c);
    int port = c == std::string::npos ? 6380 : std::atoi(lsm_addr_.substr(c + 1).c_str());
    xmex_lsm::LsmClient cli(host, port, 3000);
    if (!cli.ok()) return;
    std::vector<std::string> keys;
    if (!cli.keys("xmex:kv:node:*", keys)) return;
    for (const auto& key : keys) {
        std::string val;
        if (!cli.get(key, val)) continue;
        auto pick = [&](const char* k) -> std::string {
            std::string pat = std::string("\"") + k + "\":\"";
            size_t p = val.find(pat);
            if (p == std::string::npos) return "";
            p += pat.size();
            size_t e = val.find('"', p);
            return e == std::string::npos ? "" : val.substr(p, e - p);
        };
        KvNode node;
        node.node_id = pick("node_id");
        node.addr = pick("addr");
        node.binary = pick("binary");
        node.work_dir = pick("work_dir");
        node.pid = -1;
        node.restart_count = 0;
        node.state = KvNodeState::STOPPED;
        if (!node.node_id.empty() && nodes_.find(node.node_id) == nodes_.end()) {
            nodes_[node.node_id] = node;
        }
    }
}

// ---- 内部方法 ----

bool KvAdmin::process_alive(int64_t pid) {
    if (pid <= 0) return false;
    int ret = waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG);
    if (ret == 0) return true;    // 仍在运行
    return false;                  // 已退出或被回收
}

bool KvAdmin::send_signal(int64_t pid, int sig, std::string& error) {
    if (pid <= 0) {
        error = "invalid pid";
        return false;
    }
    if (kill(static_cast<pid_t>(pid), sig) != 0) {
        error = std::string("kill(") + std::to_string(pid) + ", "
                + std::to_string(sig) + ") failed: " + strerror(errno);
        return false;
    }
    return true;
}

int64_t KvAdmin::spawn_process(const KvNode& node, std::string& error) {
    pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + strerror(errno);
        return -1;
    }

    if (pid == 0) {
        // 子进程独立成新会话和新进程组:脱离拉起方(一次性执行程序/终端会话)
        // 的生命周期,避免拉起方退出时整个进程组被终止导致托管节点被杀。
        setsid();
        // 注入节点身份:容器内代码经 XMEX_NODE_ID 反查所属节点
        setenv("XMEX_NODE_ID", node.node_id.c_str(), 1);
        setenv("XMEX_NODE_ADDR", node.addr.c_str(), 1);
        // 子进程：切换工作目录
        if (!node.work_dir.empty() && chdir(node.work_dir.c_str()) != 0) {
            _exit(126);
        }

        // 标准输入重定向到 /dev/null:防止服务因子进程无输入源而异常退出
        // (如 orchestrator 从 stdin 读任务,无 stdin 时会立即退出)
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            close(nullfd);
        }
        // 重定向 stdout/stderr 到日志文件（node_id.log）
        std::string log_path = (node.work_dir.empty() ? "/tmp" : node.work_dir)
                               + "/" + node.node_id + ".log";
        int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        // 构建 argv
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(node.binary.c_str()));
        for (const auto& a : node.args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        execv(node.binary.c_str(), argv.data());
        _exit(127);  // exec 失败
    }

    // 父进程：返回子进程 pid
    return static_cast<int64_t>(pid);
}

// ---- 节点生命周期 ----

bool KvAdmin::build(const std::string& node_id,
                    const std::string& binary,
                    const std::vector<std::string>& args,
                    const std::string& work_dir,
                    const std::string& addr,
                    std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);

    // 幂等：已存在且运行中则直接返回
    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        if (it->second.state == KvNodeState::RUNNING && process_alive(it->second.pid)) {
            return true;  // 已在运行
        }
        // 已存在但未运行：复用注册信息重新启动
        KvNode& node = it->second;
        int64_t pid = spawn_process(node, error);
        if (pid < 0) return false;
        node.pid = pid;
        node.state = KvNodeState::RUNNING;
        node.started_at_ms = now_ms();
        node.last_active_ms = now_ms();
        node.restart_count++;
        std::cout << "[kv_admin] restarted node " << node_id << " pid=" << pid << std::endl;
        return true;
    }

    // 新建节点
    KvNode node;
    node.node_id = node_id;
    node.addr = addr;
    node.binary = binary;
    node.args = args;
    node.work_dir = work_dir;
    node.state = KvNodeState::IDLE;
    node.pid = -1;
    node.restart_count = 0;

    int64_t pid = spawn_process(node, error);
    if (pid < 0) {
        nodes_[node_id] = node;  // 注册但保持 IDLE
        return false;
    }

    node.pid = pid;
    node.state = KvNodeState::RUNNING;
    node.started_at_ms = now_ms();
    node.last_active_ms = now_ms();
    nodes_[node_id] = node;
    persist_node(node);  // 同步到 LSM,进程重启后可恢复

    std::cout << "[kv_admin] built node " << node_id << " pid=" << pid
              << " addr=" << addr << std::endl;
    return true;
}

bool KvAdmin::add(const std::string& node_id,
                  const std::string& binary,
                  const std::vector<std::string>& args,
                  const std::string& work_dir,
                  const std::string& addr,
                  std::string& error) {
    // add 与 build 语义相同（Topo 通过副本调整机制纳入 Raft 组）
    return build(node_id, binary, args, work_dir, addr, error);
}

bool KvAdmin::erase(const std::string& node_id, std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        error = "node not found: " + node_id;
        return false;
    }

    KvNode& node = it->second;

    // 停止进程（先 SIGTERM，等待后 SIGKILL）
    if (node.pid > 0 && process_alive(node.pid)) {
        send_signal(node.pid, SIGTERM, error);
        // 等待最多 2 秒
        for (int i = 0; i < 20; ++i) {
            if (!process_alive(node.pid)) break;
            usleep(100 * 1000);
        }
        if (process_alive(node.pid)) {
            send_signal(node.pid, SIGKILL, error);
        }
        waitpid(static_cast<pid_t>(node.pid), nullptr, 0);
    }

    nodes_.erase(it);
    persist_erase(node_id);  // 从 LSM 同步删除
    std::cout << "[kv_admin] erased node " << node_id << std::endl;
    return true;
}

bool KvAdmin::skip(const std::string& node_id, std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        error = "node not found: " + node_id;
        return false;
    }

    // 标记 LOST：进程保留运行，但逻辑上跳过（等待副本替换）
    it->second.state = KvNodeState::LOST;
    std::cout << "[kv_admin] skipped node " << node_id << " (marked LOST)" << std::endl;
    return true;
}

bool KvAdmin::sleep_node(const std::string& node_id, std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        error = "node not found: " + node_id;
        return false;
    }

    KvNode& node = it->second;
    if (node.pid <= 0 || !process_alive(node.pid)) {
        error = "node not running: " + node_id;
        return false;
    }

    if (!send_signal(node.pid, SIGSTOP, error)) {
        return false;
    }

    node.state = KvNodeState::SLEEPING;
    std::cout << "[kv_admin] node " << node_id << " sleeping (SIGSTOP)" << std::endl;
    return true;
}

bool KvAdmin::wakeup(const std::string& node_id, std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        error = "node not found: " + node_id;
        return false;
    }

    KvNode& node = it->second;
    if (node.state != KvNodeState::SLEEPING) {
        error = "node not sleeping: " + node_id;
        return false;
    }

    if (!send_signal(node.pid, SIGCONT, error)) {
        return false;
    }

    node.state = KvNodeState::RUNNING;
    node.last_active_ms = now_ms();
    std::cout << "[kv_admin] node " << node_id << " woken up (SIGCONT)" << std::endl;
    return true;
}

bool KvAdmin::stop(const std::string& node_id, std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        error = "node not found: " + node_id;
        return false;
    }

    KvNode& node = it->second;
    if (node.pid > 0 && process_alive(node.pid)) {
        send_signal(node.pid, SIGTERM, error);
        for (int i = 0; i < 20; ++i) {
            if (!process_alive(node.pid)) break;
            usleep(100 * 1000);
        }
        if (process_alive(node.pid)) {
            send_signal(node.pid, SIGKILL, error);
        }
        waitpid(static_cast<pid_t>(node.pid), nullptr, 0);
    }

    node.pid = -1;
    node.state = KvNodeState::STOPPED;
    std::cout << "[kv_admin] stopped node " << node_id << std::endl;
    return true;
}

bool KvAdmin::restart(const std::string& node_id, std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        error = "node not found: " + node_id;
        return false;
    }

    KvNode& node = it->second;
    if (node.state == KvNodeState::RUNNING && process_alive(node.pid)) {
        return true;  // 已在运行
    }

    int64_t pid = spawn_process(node, error);
    if (pid < 0) return false;

    node.pid = pid;
    node.state = KvNodeState::RUNNING;
    node.started_at_ms = now_ms();
    node.last_active_ms = now_ms();
    node.restart_count++;
    std::cout << "[kv_admin] restarted node " << node_id << " pid=" << pid << std::endl;
    return true;
}

// ---- 状态查询 ----

bool KvAdmin::get_node(const std::string& node_id, KvNode& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) return false;
    out = it->second;
    return true;
}

std::vector<KvNode> KvAdmin::list_nodes() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<KvNode> list;
    for (const auto& [_, node] : nodes_) {
        list.push_back(node);
    }
    return list;
}

bool KvAdmin::is_alive(const std::string& node_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) return false;
    return process_alive(it->second.pid);
}
