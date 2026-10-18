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

const char* desired_str(NodeDesired d) {
    switch (d) {
        case NodeDesired::PRESENT: return "present";
        case NodeDesired::ABSENT:  return "absent";
    }
    return "unknown";
}

const char* observed_str(NodeObserved o) {
    switch (o) {
        case NodeObserved::ABSENT:    return "absent";
        case NodeObserved::ALIVE:     return "alive";
        case NodeObserved::SUSPENDED: return "suspended";
        case NodeObserved::CRASHED:   return "crashed";
    }
    return "unknown";
}

KvAdmin& KvAdmin::instance() {
    static KvAdmin mgr;
    return mgr;
}

// 单例是静态存储期对象,析构时若调谐线程还活着会 std::terminate。
// 必须在静态对象析构前把线程停掉。
KvAdmin::~KvAdmin() {
    stop_reconcile();
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

// ---- 声明式：探测与调谐 ----

NodeObserved KvAdmin::probe_locked(KvNode& node) {
    if (node.pid <= 0) {
        node.observed = NodeObserved::ABSENT;
        return node.observed;
    }

    // 注入模式:用假进程表判断,不走 kill(pid,0) —— 假 pid 在真实系统里
    // 本来就不存在,用 kill 探测会把所有假进程都判成已死,测不出修复逻辑。
    if (spawn_hook_) {
        bool alive;
        {
            std::lock_guard<std::mutex> fk(fake_mu_);
            alive = fake_alive_.count(node.pid) > 0;
        }
        if (!alive) {
            node.pid = -1;
            node.observed = (node.state == KvNodeState::STOPPED ||
                             node.state == KvNodeState::IDLE)
                                ? NodeObserved::ABSENT
                                : NodeObserved::CRASHED;
            return node.observed;
        }
        // LOST(skip 过)必须继续报"不健康" —— 见下方非注入分支的说明。
        if (node.state == KvNodeState::LOST) {
            node.observed = NodeObserved::CRASHED;
            return node.observed;
        }
        node.observed = (node.state == KvNodeState::SLEEPING)
                            ? NodeObserved::SUSPENDED
                            : NodeObserved::ALIVE;
        return node.observed;
    }

    // ⚠️ process_alive 用的是 waitpid(WNOHANG)，它会**回收**已退出的子进程。
    // 这对「用户主动调用 is_alive」是对的，但对调谐循环不够：一旦回收，
    // 僵尸信息就没了，无法区分「刚退出」与「早就不在了」，也无法知道
    // 它是自己崩的还是被 kill 的。这里用 kill(pid, 0) 做纯存活探测 ——
    // 它不回收、且对僵尸进程仍返回成功（僵尸尚未被 wait 收割）。
    // 代价是崩溃的进程会留一个僵尸条目，直到这里主动收割。
    if (kill(static_cast<pid_t>(node.pid), 0) != 0 && errno == ESRCH) {
        // 进程确实不在了：顺手收割它，让 waitpid 的状态归位，
        // 否则 process_alive 之后会一直认为它活着。
        waitpid(static_cast<pid_t>(node.pid), nullptr, WNOHANG);
        node.pid = -1;
        node.observed = (node.state == KvNodeState::STOPPED ||
                         node.state == KvNodeState::IDLE)
                            ? NodeObserved::ABSENT
                            : NodeObserved::CRASHED;
        return node.observed;
    }
    // 还存在。SLEEPING 是我们自己发的 SIGSTOP，进程确实在，只是被暂停了 ——
    // 调谐时**不能**把它当掉线去拉起，否则会打断故障注入。
    //
    // LOST(skip 过)同理：进程还活着，但逻辑上已被跳过。观测必须继续报
    // "不健康"，否则组调谐看不到副本缺口,也就不会补替补 —— 而补替补
    // 正是 skip 的用途。desired 仍是 PRESENT,所以它既不会被单节点调谐
    // 原地重启(组内成员),也不会被 retire 掉。
    if (node.state == KvNodeState::LOST) {
        node.observed = NodeObserved::CRASHED;
        return node.observed;
    }
    node.observed = (node.state == KvNodeState::SLEEPING)
                        ? NodeObserved::SUSPENDED
                        : NodeObserved::ALIVE;
    return node.observed;
}

void KvAdmin::set_spawn_hook(SpawnHook hook) {
    std::lock_guard<std::mutex> lk(mutex_);
    spawn_hook_ = std::move(hook);
}

void KvAdmin::clear_spawn_hook() {
    std::lock_guard<std::mutex> lk(mutex_);
    spawn_hook_ = nullptr;
    std::lock_guard<std::mutex> fk(fake_mu_);
    fake_alive_.clear();
}

void KvAdmin::test_kill_fake(int64_t pid) {
    std::lock_guard<std::mutex> fk(fake_mu_);
    fake_alive_.erase(pid);
}

bool KvAdmin::reconcile_one_locked(KvNode& node, int64_t now) {
    // 已在期望状态 → 无事可做。这条判断是整个调谐幂等性的关键：
    // 连续跑两轮，第二轮必然什么都不做。
    if (node.observed == NodeObserved::ALIVE ||
        node.observed == NodeObserved::SUSPENDED) {
        // 观测正常，清掉失败计数，让退避窗口关闭。
        {
            std::lock_guard<std::mutex> rk(retry_mu_);
            retry_.erase(node.node_id);
        }
        return false;
    }

    // ⚠️ 组内成员的处置权归组调谐,这里不能擅自重启。
    // 否则组调谐永远看不到"少了一个健康副本"这个信号,也就永远不会
    // 补出新副本 —— 崩溃的成员被原地重启,副本数一直是满的,补副本逻辑
    // 形同虚设。组调谐会在本轮末尾统一处理组成员。
    if (!node.group_id.empty()) {
        return false;
    }

    // 退避检查：连续失败的节点不要每轮都重试，否则 spawn 失败的节点会
    // 疯狂 fork，把机器拖垮。
    {
        std::lock_guard<std::mutex> rk(retry_mu_);
        auto it = retry_.find(node.node_id);
        if (it != retry_.end() && now < it->second.next_attempt_ms) {
            return false;   // 还在退避窗口内
        }
    }

    std::string error;
    int64_t pid = spawn_process(node, error);
    if (pid < 0) {
        node.last_error = error;
        node.last_update_ms = now;
        // 指数退避：1s → 2s → 4s ... 上限 retry_backoff_max_ms_
        std::lock_guard<std::mutex> rk(retry_mu_);
        RetrySlot& slot = retry_[node.node_id];
        slot.consecutive_fail++;
        int64_t delay = 1000;
        for (int64_t i = 1; i < slot.consecutive_fail && delay < retry_backoff_max_ms_; ++i) {
            delay *= 2;
        }
        slot.next_attempt_ms = now + (delay > retry_backoff_max_ms_ ? retry_backoff_max_ms_ : delay);
        std::cerr << "[kv_reconcile] spawn " << node.node_id
                  << " failed (" << error << "), retry in "
                  << (slot.next_attempt_ms - now) << "ms (fail #"
                  << slot.consecutive_fail << ")" << std::endl;
        return false;
    }

    // 拉起成功
    node.pid = pid;
    node.state = KvNodeState::RUNNING;
    node.observed = NodeObserved::ALIVE;
    node.started_at_ms = now;
    node.last_active_ms = now;
    node.last_update_ms = now;
    node.last_error.clear();
    node.restart_count++;
    {
        std::lock_guard<std::mutex> rk(retry_mu_);
        retry_.erase(node.node_id);
    }
    persist_node(node);
    std::cout << "[kv_reconcile] repaired " << node.node_id
              << " pid=" << pid << " (restart #" << node.restart_count << ")" << std::endl;
    return true;
}

bool KvAdmin::retire_one_locked(const std::string& node_id, KvNode& node, int64_t now) {
    std::string error;
    // 注入模式:没有真进程,直接从假表里摘掉
    if (spawn_hook_) {
        if (node.pid > 0) {
            std::lock_guard<std::mutex> fk(fake_mu_);
            fake_alive_.erase(node.pid);
        }
    } else if (node.pid > 0 && process_alive(node.pid)) {
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
    node.observed = NodeObserved::ABSENT;
    node.last_update_ms = now;
    nodes_.erase(node_id);
    persist_erase(node_id);
    {
        std::lock_guard<std::mutex> rk(retry_mu_);
        retry_.erase(node_id);
    }
    std::cout << "[kv_reconcile] retired " << node_id << " (desired=absent)" << std::endl;
    return true;
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
    // 注入模式:交给测试的钩子,不真的 fork
    if (spawn_hook_) {
        int64_t pid = spawn_hook_(node, error);
        if (pid > 0) {
            std::lock_guard<std::mutex> fk(fake_mu_);
            fake_alive_.insert(pid);
        }
        return pid;
    }

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
        // 再次 build 意味着重新声明"我要它在跑"——这能救回一个被 erase
        // 前标过 ABSENT 或被 skip 的节点。
        KvNode& node = it->second;
        node.desired = NodeDesired::PRESENT;
        // 补齐可能缺失的注册信息（允许只给 node_id 来"唤醒"一个节点）
        if (!addr.empty())     node.addr = addr;
        if (!binary.empty())   node.binary = binary;
        if (!work_dir.empty()) node.work_dir = work_dir;
        if (!args.empty())     node.args = args;

        probe_locked(node);
        if (node.observed == NodeObserved::ALIVE ||
            node.observed == NodeObserved::SUSPENDED) {
            return true;  // 已在运行
        }
        // 已存在但未运行：复用注册信息重新启动。
        // 复用 reconcile_one_locked 而不是自己写一遍拉起逻辑，保证两条路径
        // （命令式 build 与声明式调谐）行为完全一致 —— 幂等性靠单一实现保证。
        {
            std::lock_guard<std::mutex> rk(retry_mu_);
            retry_.erase(node_id);
        }
        node.state = KvNodeState::STOPPED;   // 避免 probe 误判为"已在跑"
        if (!reconcile_one_locked(node, now_ms())) {
            return false;
        }
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
    node.desired = NodeDesired::PRESENT;      // 声明式:我要它在跑
    node.observed = NodeObserved::ABSENT;

    int64_t pid = spawn_process(node, error);
    if (pid < 0) {
        // 注册但保持 IDLE + observed=ABSENT。故意返回 false 让调用方知道
        // 没起来,但注册表要留 —— 调谐循环会按 desired=PRESENT 持续重试
        // (带指数退避),调用方也可以选择稍后 reconcile_once()。
        node.last_error = error;
        nodes_[node_id] = node;
        // 记一次失败:build 阶段的失败也要进退避计数,否则调谐第一轮就会
        // 立刻重试,紧接着 build 刚返回 false 又拉起来,像是抖动。
        {
            std::lock_guard<std::mutex> rk(retry_mu_);
            RetrySlot& slot = retry_[node_id];
            slot.consecutive_fail++;
            slot.next_attempt_ms = now_ms() + 1000;
        }
        persist_node(node);
        return false;
    }

    node.pid = pid;
    node.state = KvNodeState::RUNNING;
    node.observed = NodeObserved::ALIVE;
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

    // 先标 desired=ABSENT：即便这里把注册表删了，调谐循环也不会把它拉回来。
    // 顺序很重要 —— 必须在 erase 注册表之前落这个意图。
    node.desired = NodeDesired::ABSENT;
    node.observed = NodeObserved::ABSENT;
    node.last_update_ms = now_ms();

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
    {
        std::lock_guard<std::mutex> rk(retry_mu_);
        retry_.erase(node_id);
    }
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
    //
    // ⚠️ 这里**不**动 desired。skip 的语义是"这个节点我不要了，但进程先留着，
    // 数据别丢" —— 它是故障注入（模拟副本丢失），不是"我要下线"。
    // 如果在这里把 desired 设成 ABSENT，调谐循环会在下一轮把它连进程一起删掉，
    // 而 skip 的原意恰恰是保留进程。用 unskip() 恢复。
    //
    // 但 observed 必须改成 LOST 对应的"不健康":副本组的健康副本数是按
    // observed 统计的,若 observed 还是 ALIVE,组调谐看不到"少了一个副本",
    // 也就永远不会补替补 —— 而补替补正是 skip 的用途。
    it->second.state = KvNodeState::LOST;
    it->second.observed = NodeObserved::CRASHED;   // 视为不健康,触发补副本
    it->second.last_update_ms = now_ms();
    persist_node(it->second);
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

    // 注入模式:没有真进程可发信号,但状态标记仍要走通,否则测不了
    // 「暂停的节点不该被调谐误判为掉线」这条逻辑。
    if (spawn_hook_) {
        bool alive;
        {
            std::lock_guard<std::mutex> fk(fake_mu_);
            alive = node.pid > 0 && fake_alive_.count(node.pid) > 0;
        }
        if (!alive) {
            error = "node not running: " + node_id;
            return false;
        }
        node.state = KvNodeState::SLEEPING;
        node.observed = NodeObserved::SUSPENDED;
        std::cout << "[kv_admin] node " << node_id << " sleeping (fake)" << std::endl;
        return true;
    }

    if (node.pid <= 0 || !process_alive(node.pid)) {
        error = "node not running: " + node_id;
        return false;
    }

    if (!send_signal(node.pid, SIGSTOP, error)) {
        return false;
    }

    node.state = KvNodeState::SLEEPING;
    node.observed = NodeObserved::SUSPENDED;
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

    if (spawn_hook_) {
        node.state = KvNodeState::RUNNING;
        node.observed = NodeObserved::ALIVE;
        node.last_active_ms = now_ms();
        std::cout << "[kv_admin] node " << node_id << " woken up (fake)" << std::endl;
        return true;
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

// ---- 调谐循环 ----

int64_t KvAdmin::reconcile_once() {
    return reconcile_report_once().actions;
}

ReconcileReport KvAdmin::reconcile_report_once() {
    // 先取一份 desired 的快照再决定动作,而不是边遍历边改 —— erase 会让
    // 迭代器失效。这里收集要摘除的 id,统一在最后处理。
    int64_t now = now_ms();
    ReconcileReport rep;
    std::vector<std::string> to_retire;

    std::lock_guard<std::mutex> lk(mutex_);

    for (auto& [id, node] : nodes_) {
        // 探测:先把 observed 填成事实
        NodeObserved before = node.observed;
        probe_locked(node);
        if (node.observed != before) {
            node.last_update_ms = now;
        }

        if (node.desired == NodeDesired::ABSENT) {
            if (node.observed != NodeObserved::ABSENT) {
                to_retire.push_back(id);
            }
            continue;
        }

        // desired == PRESENT,看是否需要修复
        bool was_unhealthy = (before == NodeObserved::ABSENT ||
                              before == NodeObserved::CRASHED);
        if (reconcile_one_locked(node, now)) {
            rep.actions++;
            rep.repaired.push_back(id);
        } else if (was_unhealthy && node.last_error.empty() &&
                   node.observed != NodeObserved::ALIVE &&
                   node.observed != NodeObserved::SUSPENDED) {
            // 进了退避窗口,记为失败以便上层观测
            rep.failed.push_back(id);
        }
    }

    for (const auto& id : to_retire) {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) continue;
        if (retire_one_locked(id, it->second, now)) {
            rep.actions++;
            rep.retired.push_back(id);
        }
    }

    // ---- 副本组调谐:补足健康副本数 ----
    //
    // 健康副本 = 成员存在 且 observed ∈ {ALIVE, SUSPENDED}。
    // LOST(skip 过的)与 CRASHED 都不健康 —— 这正是"skip 后自动补副本"
    // 的触发点。
    //
    // 处置策略(这是组调谐与单节点调谐的分工点):
    //   组内成员崩溃 → 优先**原地重启**它(它仍属于本组,desired=PRESENT)。
    //   原地重启失败或不可用 → 另起替补顶上,并把替补加入成员列表。
    //   成员被 skip(LOST) → **不**原地重启(那是故障注入,重启会打破语义),
    //     直接补替补。
    for (auto& [gid, g] : groups_) {
        if (g.binary.empty()) continue;   // 没有可执行体,无法自动补

        // 第一步:处理崩溃但仍属本组的成员 —— 原地重启。
        for (const auto& mid : g.member_ids) {
            auto mit = nodes_.find(mid);
            if (mit == nodes_.end()) continue;
            KvNode& m = mit->second;
            if (m.group_id != gid) continue;          // 已被别的组接管
            if (m.observed != NodeObserved::CRASHED &&
                m.observed != NodeObserved::ABSENT) continue;
            if (m.state == KvNodeState::LOST) continue;  // skip 的不重启
            if (m.desired != NodeDesired::PRESENT) continue;

            // 原地重启:复用组内成员的注册信息
            std::string error;
            {
                std::lock_guard<std::mutex> rk(retry_mu_);
                auto rit = retry_.find(mid);
                if (rit != retry_.end() && now < rit->second.next_attempt_ms) {
                    continue;   // 退避中
                }
            }
            int64_t pid = spawn_process(m, error);
            if (pid > 0) {
                m.pid = pid;
                m.state = KvNodeState::RUNNING;
                m.observed = NodeObserved::ALIVE;
                m.started_at_ms = now;
                m.last_active_ms = now;
                m.last_update_ms = now;
                m.last_error.clear();
                m.restart_count++;
                {
                    std::lock_guard<std::mutex> rk(retry_mu_);
                    retry_.erase(mid);
                }
                persist_node(m);
                rep.actions++;
                rep.repaired.push_back(mid);
                std::cout << "[kv_group] " << gid << " restarted member "
                          << mid << " pid=" << pid << std::endl;
            }
        }

        // 第二步:统计健康副本,不足则补替补。
        int32_t healthy = 0;
        for (const auto& mid : g.member_ids) {
            auto mit = nodes_.find(mid);
            if (mit == nodes_.end()) continue;
            NodeObserved o = mit->second.observed;
            if (o == NodeObserved::ALIVE || o == NodeObserved::SUSPENDED) {
                healthy++;
            }
        }

        while (healthy < g.desired_replica_count) {
            int64_t seq = ++group_seq_[gid];
            std::string nid = gid + "-repl-" + std::to_string(seq);

            KvNode node;
            node.node_id = nid;
            node.binary = g.binary;
            node.args = g.args;
            node.work_dir = g.work_dir_prefix.empty()
                                ? ("/tmp/" + nid)
                                : (g.work_dir_prefix + "/" + nid);
            // 地址前缀形如 127.0.0.1:9200，替补按 seq 递增端口避免撞号
            if (!g.addr_prefix.empty()) {
                size_t colon = g.addr_prefix.rfind(':');
                if (colon != std::string::npos) {
                    int base = std::atoi(g.addr_prefix.c_str() + colon + 1);
                    node.addr = g.addr_prefix.substr(0, colon + 1) +
                                std::to_string(base + static_cast<int>(seq));
                } else {
                    node.addr = g.addr_prefix;
                }
            }
            node.desired = NodeDesired::PRESENT;
            node.observed = NodeObserved::ABSENT;
            node.state = KvNodeState::IDLE;
            node.pid = -1;
            node.restart_count = 0;
            node.group_id = gid;          // 替补也是本组成员

            std::string error;
            int64_t pid = spawn_process(node, error);
            nodes_[nid] = node;          // 先入表,失败也让调谐下一轮再试
            if (pid < 0) {
                node.last_error = error;
                nodes_[nid] = node;
                rep.failed.push_back(nid);
                std::cerr << "[kv_group] respawn " << gid << " replica failed: "
                          << error << std::endl;
                break;                   // 这一组本轮不再试,避免疯狂 fork
            }
            node.pid = pid;
            node.state = KvNodeState::RUNNING;
            node.observed = NodeObserved::ALIVE;
            node.started_at_ms = now;
            node.last_active_ms = now;
            nodes_[nid] = node;
            persist_node(node);

            g.member_ids.push_back(nid);
            g.last_update_ms = now;
            rep.actions++;
            rep.replaced.push_back(nid);
            healthy++;
            std::cout << "[kv_group] " << gid << " spawned replacement "
                      << nid << " pid=" << pid << " (" << healthy << "/"
                      << g.desired_replica_count << ")" << std::endl;
        }
    }

    return rep;
}

void KvAdmin::start_reconcile(int64_t interval_ms) {
    if (reconcile_active_.load()) return;      // 已在跑,幂等
    if (interval_ms <= 0) interval_ms = 1000;
    reconcile_interval_ms_ = interval_ms;
    reconcile_stop_.store(false);
    reconcile_active_.store(true);
    reconcile_thread_ = std::thread([this]() {
        while (!reconcile_stop_.load()) {
            reconcile_once();
            // 分片睡眠而不是一觉睡到底:stop_reconcile 能更快被响应
            int64_t slept = 0;
            while (slept < reconcile_interval_ms_ && !reconcile_stop_.load()) {
                int64_t step = 50;
                if (reconcile_interval_ms_ - slept < step) step = reconcile_interval_ms_ - slept;
                std::this_thread::sleep_for(std::chrono::milliseconds(step));
                slept += step;
            }
        }
    });
    std::cout << "[kv_reconcile] started, interval=" << interval_ms << "ms" << std::endl;
}

void KvAdmin::stop_reconcile() {
    if (!reconcile_active_.load()) {
        // 即使没在跑也要 join —— 线程可能刚被创建还没置位
        if (reconcile_thread_.joinable()) reconcile_thread_.join();
        return;
    }
    reconcile_stop_.store(true);
    if (reconcile_thread_.joinable()) reconcile_thread_.join();
    reconcile_active_.store(false);
    std::cout << "[kv_reconcile] stopped" << std::endl;
}

bool KvAdmin::reconcile_running() const {
    return reconcile_active_.load();
}

bool KvAdmin::unskip(const std::string& node_id, std::string& error) {
    std::string err;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = nodes_.find(node_id);
        if (it == nodes_.end()) {
            error = "node not found: " + node_id;
            return false;
        }
        KvNode& node = it->second;
        node.desired = NodeDesired::PRESENT;
        node.state = KvNodeState::STOPPED;   // 让 reconcile_one 不误判为"已在跑"
        node.last_error.clear();
        {
            std::lock_guard<std::mutex> rk(retry_mu_);
            retry_.erase(node_id);
        }
        persist_node(node);
    }
    // 立刻跑一轮,不用等下一个周期 —— 调用方期待的是"现在就恢复"
    reconcile_once();
    {
        // ⚠️ 不能写成 if (get_node(...)) —— get_node 内部会再加一次 mutex_,
        // 而这里已经持锁,std::mutex 不可重入会死锁。直接查表。
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = nodes_.find(node_id);
        if (it == nodes_.end()) {
            error = "node vanished after reconcile: " + node_id;
            return false;
        }
        const KvNode& node = it->second;
        if (node.observed == NodeObserved::ALIVE ||
            node.observed == NodeObserved::SUSPENDED) {
            std::cout << "[kv_admin] unskipped " << node_id << std::endl;
            return true;
        }
        error = node.last_error.empty() ? "respawn pending (in backoff)" : node.last_error;
        return false;
    }
}

int64_t KvAdmin::consecutive_failures(const std::string& node_id) {
    std::lock_guard<std::mutex> rk(retry_mu_);
    auto it = retry_.find(node_id);
    return it == retry_.end() ? 0 : it->second.consecutive_fail;
}

// ---- 副本组 ----

bool KvAdmin::group_create(const std::string& group_id,
                           int32_t desired_replica_count,
                           const std::string& binary,
                           const std::vector<std::string>& args,
                           const std::string& work_dir_prefix,
                           const std::string& addr_prefix,
                           std::string& error) {
    if (desired_replica_count <= 0) {
        error = "desired_replica_count must be positive";
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (groups_.find(group_id) != groups_.end()) {
        error = "group already exists: " + group_id;
        return false;
    }
    ReplicaGroup g;
    g.group_id = group_id;
    g.desired_replica_count = desired_replica_count;
    g.binary = binary;
    g.args = args;
    g.work_dir_prefix = work_dir_prefix;
    g.addr_prefix = addr_prefix;
    g.last_update_ms = now_ms();
    groups_[group_id] = g;
    group_seq_[group_id] = 0;
    std::cout << "[kv_group] created " << group_id
              << " replicas=" << desired_replica_count << std::endl;
    return true;
}

bool KvAdmin::group_add_member(const std::string& group_id,
                               const std::string& node_id,
                               std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto git = groups_.find(group_id);
    if (git == groups_.end()) {
        error = "group not found: " + group_id;
        return false;
    }
    if (nodes_.find(node_id) == nodes_.end()) {
        error = "node not found (build it first): " + node_id;
        return false;
    }
    ReplicaGroup& g = git->second;
    if (std::find(g.member_ids.begin(), g.member_ids.end(), node_id) != g.member_ids.end()) {
        return true;   // 幂等
    }
    g.member_ids.push_back(node_id);
    // 标记归属:组内成员的崩溃处置权归组调谐,不走单节点调谐的原地重启。
    // 不设这个标记的话,成员崩溃会被单节点调谐直接重启,组调谐永远看不到
    // "少了一个副本",也就永远不会补替补。
    nodes_[node_id].group_id = group_id;
    g.last_update_ms = now_ms();
    persist_node(nodes_[node_id]);
    std::cout << "[kv_group] " << group_id << " += " << node_id
              << " (" << g.member_ids.size() << "/" << g.desired_replica_count << ")" << std::endl;
    return true;
}

bool KvAdmin::group_set_replicas(const std::string& group_id,
                                 int32_t desired_replica_count,
                                 std::string& error) {
    if (desired_replica_count < 0) {
        error = "desired_replica_count must be >= 0";
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    auto git = groups_.find(group_id);
    if (git == groups_.end()) {
        error = "group not found: " + group_id;
        return false;
    }
    git->second.desired_replica_count = desired_replica_count;
    git->second.last_update_ms = now_ms();
    std::cout << "[kv_group] " << group_id << " desired_replicas -> "
              << desired_replica_count << std::endl;
    return true;
}

bool KvAdmin::group_destroy(const std::string& group_id, std::string& error) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (groups_.erase(group_id) == 0) {
        error = "group not found: " + group_id;
        return false;
    }
    group_seq_.erase(group_id);
    std::cout << "[kv_group] destroyed " << group_id
              << " (成员进程保持运行,只是不再维持)" << std::endl;
    return true;
}

bool KvAdmin::group_get(const std::string& group_id, ReplicaGroup& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return false;
    out = it->second;
    return true;
}

std::vector<ReplicaGroup> KvAdmin::group_list() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<ReplicaGroup> v;
    for (const auto& [_, g] : groups_) v.push_back(g);
    return v;
}
