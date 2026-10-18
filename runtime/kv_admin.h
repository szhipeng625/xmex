#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

// ============================================================================
// 副本组 —— 声明「这一组要几个副本」的期望
//
// 这是"skip 之后自动补副本"的载体。单独一个 KvNode 只能表达
// "这个节点我要它在跑",无法表达"这一组要 3 个"—— 补副本需要知道
// 期望副本数、当前有几个、以及从哪台机器补。
// 参照 adviskv ReplicaGroup(third_party/adviskv/src/topo/model/model.h)。
// ============================================================================
struct ReplicaGroup {
    std::string group_id;
    int32_t     desired_replica_count{1};           // 期望副本数
    std::vector<std::string> member_ids;            // 当前成员节点 id
    std::string binary;                             // 新副本用这个可执行体
    std::vector<std::string> args;                  // 新副本用这些参数
    std::string work_dir_prefix;                    // 新副本工作目录前缀
    std::string addr_prefix;                        // 新副本地址前缀,补副本时递增端口
    int64_t     last_update_ms{0};
};

// ============================================================================
// 调谐一轮的结果摘要
// ============================================================================
struct ReconcileReport {
    int64_t actions{0};                 // 本轮实际执行的动作总数
    std::vector<std::string> repaired;  // 被拉起的节点
    std::vector<std::string> retired;   // 被摘除的节点
    std::vector<std::string> failed;    // 拉起失败(在退避中)的节点
    std::vector<std::string> replaced;  // 因副本数不足被补充的新节点 id
};

// ============================================================================
// 节点状态
// ============================================================================
// ============================================================================
// 节点期望状态 —— 声明「我要它在跑」还是「我要它下线」
//
// 引入 desired/observed 分离的动机:原来的 KvNodeState 一个字段同时表达
// 「我想要它跑」和「它现在跑没跑」。后果是 skip() 标记 LOST 之后没有任何
// 东西会去补副本 —— 状态只能被动改写,无法自动收敛。
// 参照 adviskv 的 ReplicaState(third_party/adviskv/src/topo/model/model.h:54)。
// ============================================================================
enum class NodeDesired {
    PRESENT = 1,   // 我要它在跑
    ABSENT  = 2,   // 我要它下线
};

// ============================================================================
// 节点观测状态 —— 由系统探测填入,表示「实际怎么样」
// ============================================================================
enum class NodeObserved {
    ABSENT      = 1,   // 进程不存在
    ALIVE       = 2,   // 进程活着
    SUSPENDED   = 3,   // SIGSTOP 暂停中(故障注入,算"活着")
    CRASHED    = 4,   // 曾启动但已消失(需要拉起)
};

const char* desired_str(NodeDesired d);
const char* observed_str(NodeObserved o);

enum class KvNodeState {
    IDLE      = 0,   // 已注册但未启动
    RUNNING   = 1,   // 进程运行中
    SLEEPING  = 2,   // SIGSTOP 暂停（故障注入）
    STOPPED   = 3,   // 已停止（可重新启动）
    ERASED    = 4,   // 已永久移除
    LOST      = 5    // 逻辑跳过（副本标记丢失）
};

struct KvNode {
    std::string node_id;            // 节点唯一标识
    std::string addr;               // 监听地址（host:port）
    std::string binary;             // 可执行文件路径
    std::vector<std::string> args;  // 启动参数
    std::string work_dir;           // 工作目录

    // ---- 声明式字段（2026-10 增补）----
    NodeDesired  desired{NodeDesired::PRESENT};   // 期望,由 build/erase/skip 设定
    NodeObserved observed{NodeObserved::ABSENT};  // 观测,由探测与调谐循环填入
    std::string  last_error;                     // 最近一次失败原因
    int64_t      last_update_ms{0};              // 最近一次观测更新时间
    // 归属的副本组(空 = 不属于任何组,由单节点调谐直接管)。
    //
    // ⚠️ 这个字段决定谁来修复崩溃的节点。组内成员崩溃时,**不能**由单节点
    // 调谐直接重启 —— 那会让副本组永远等不到"少了一个"这个信号,也就
    // 永远不会补出新副本。组内成员的处置权归组调谐:它可以选择重启原节点
    // (desired 仍 PRESENT)或另起替补(补足副本数)。这是单节点调谐与组
    // 调谐不打架的关键。
    std::string  group_id;

    // ---- 兼容字段:老的 KvNodeState,待调用方迁移后删除 ----
    KvNodeState state{KvNodeState::IDLE};

    int64_t pid;                    // 进程 PID（-1 表示未运行）
    int64_t started_at_ms;          // 启动时间
    int64_t last_active_ms;         // 最后活跃时间
    int64_t restart_count;          // 重启次数

    // ---- 容器节点(2026-10 增补)----
    //
    // 非空时这个节点是**容器**,不是本机进程。区别很实在:
    //   进程节点: fork/exec 拉起,SIGSTOP 注入暂停,kill(pid) 注入崩溃
    //   容器节点: docker run 拉起,`docker pause` 注入暂停,`docker kill` 注入崩溃
    //
    // 为什么容器要单独一套:容器里的进程宿主机 kill 不到(SIGKILL 也只杀容器
    // 的 init),暂停与崩溃得走 Docker 的 API。硬套进程那套会给出误导性的结果 ——
    // 比如"暂停"后容器照常服务,看起来像注入失败。
    std::string container_id;         // docker run 返回的容器 ID;空 = 进程节点
    std::string container_image;      // 镜像名(重启时要重新 docker run)
    std::string container_cmd;        // 容器内命令
    std::string container_cpus;       // 资源限制,如 "0.5";空 = 不限
    std::string container_memory;     // 资源限制,如 "256m";空 = 不限
    // 容器节点没有 pid 可探测存活,用 docker inspect 查状态,记在这里
    std::string container_status;     // running / exited / paused / ...
};

// ============================================================================
// 节点管理器
// ============================================================================
class KvAdmin {
public:
    static KvAdmin& instance();

    // ---- 节点生命周期 ----

    // 建立节点：spawn 进程并注册（幂等：已存在则返回已有节点）
    bool build(const std::string& node_id,
               const std::string& binary,
               const std::vector<std::string>& args,
               const std::string& work_dir,
               const std::string& addr,
               std::string& error);

    // 向集群追加节点（语义同 build，别名）
    bool add(const std::string& node_id,
             const std::string& binary,
             const std::vector<std::string>& args,
             const std::string& work_dir,
             const std::string& addr,
             std::string& error);

    // 永久移除节点：停止进程并从注册表删除
    bool erase(const std::string& node_id, std::string& error);

    // 逻辑跳过：标记 LOST 但保留进程（数据保留，等待副本替换）
    bool skip(const std::string& node_id, std::string& error);

    // 暂停节点：SIGSTOP（故障注入）
    bool sleep_node(const std::string& node_id, std::string& error);

    // 唤醒节点：SIGCONT
    bool wakeup(const std::string& node_id, std::string& error);

    // 停止节点但保留注册（可重新启动）
    bool stop(const std::string& node_id, std::string& error);

    // 重新启动已停止的节点
    bool restart(const std::string& node_id, std::string& error);

    // ---- 状态查询 ----

    // 获取节点信息（不存在返回 false）
    bool get_node(const std::string& node_id, KvNode& out);

    // 列出所有节点
    std::vector<KvNode> list_nodes();

    // 检查节点进程是否存活
    bool is_alive(const std::string& node_id);

    // ---- LSM 持久化 ----
    // 配置 LSM 后端地址(如 hermes 的 6380)。配置后节点变更自动写入,
    // 进程内首次使用时自动从 LSM 恢复注册表。addr 为空则关闭持久化。
    void configure_persistence(const std::string& addr);
    // 从 LSM 恢复注册表(进程启动或首次操作时调用,幂等)
    void restore_from_lsm();
    // 读取当前持久化地址(节点内置 MQ 用于定位队列存储,2026-10 增补)
    std::string persistence_addr() const { return lsm_addr_; }

    // ---- 调谐循环(2026-10 增补) ----
    //
    // 声明式的核心:周期性地把 observed 拉回 desired。
    // 每一轮都是幂等的 —— 跑一次和跑一百次结果相同。
    //
    //   desired=PRESENT 且 observed!=ALIVE  → 拉起进程
    //   desired=ABSENT  且 observed!=ABSENT → 停进程并从注册表摘除
    //   两者一致                            → 什么都不做(已收敛)
    //
    // 不启动循环时,一切行为与从前完全一致(命令式);这是刻意保留的——
    // 调谐是个可选增强,不该成为使用门槛。
    void start_reconcile(int64_t interval_ms = 1000);
    void stop_reconcile();
    bool reconcile_running() const;
    // 手动跑一轮,用于测试与"立刻生效"场景。返回本轮实际执行的动作数。
    int64_t reconcile_once();
    // 标记某节点期望为 PRESENT 并立即拉起(skip 的逆操作)
    bool unskip(const std::string& node_id, std::string& error);
    // 拉起失败后的重试退避上限(毫秒);超过则不再自动重试,需人工介入
    void set_retry_backoff_max(int64_t max_ms) { retry_backoff_max_ms_ = max_ms; }
    // 某节点连续失败次数;0 表示健康
    int64_t consecutive_failures(const std::string& node_id);

    // ---- 可注入的 spawn 钩子(供测试用) ----
    //
    // 生产路径用 fork+execv 起真实进程。但那要求 POSIX,在别的平台上
    // 没法验证调谐逻辑本身。注入一个假的 spawn,就能在任何地方测
    // 「观测 → 判断 → 动作」这套逻辑,不必真的 fork。
    //
    // 钩子返回 pid(>0 视为成功),或 -1 表示失败(错误写进 error)。
    // 存活判定也随之切换:注入模式下用「pid 是否在册」判断,
    // 不再走 kill(pid,0) —— 否则假 pid 会被当成已死,测不出修复逻辑。
    using SpawnHook = std::function<int64_t(const KvNode&, std::string&)>;
    void set_spawn_hook(SpawnHook hook);
    void clear_spawn_hook();
    // 测试辅助:让注入的假进程"崩掉"(下一轮观测会认为它不在了)
    void test_kill_fake(int64_t pid);
    // 测试辅助:当前是否处于注入模式
    bool spawn_hook_installed() const { return static_cast<bool>(spawn_hook_); }

    // ---- 副本组(2026-10 增补) ----
    // 声明一个副本组:即使一个成员挂了,调谐循环也会自动补足到期望副本数。
    // 这是"skip 之后自动补副本"的入口。
    //
    // 用法(PyLite 侧示意):
    //   kv.group("user-shard-1", replicas=3, binary="...", addr_prefix="127.0.0.1:9200")
    //   kv.group_add("user-shard-1", "node-a")   # 加一个现有节点作为成员
    bool group_create(const std::string& group_id,
                      int32_t desired_replica_count,
                      const std::string& binary,
                      const std::vector<std::string>& args,
                      const std::string& work_dir_prefix,
                      const std::string& addr_prefix,
                      std::string& error);

    // ---- 容器节点(2026-10 增补)----
    //
    // 容器节点与进程节点的分野在**故障注入的手段**上:
    //   暂停:  进程 SIGSTOP        / 容器 `docker pause`
    //   崩溃:  kill(pid, SIGKILL) / 容器 `docker kill`
    //   存活:  waitpid(WNOHANG)   / `docker inspect --format {{.State.Running}}`
    //   限额:  rlimit(未实现)      / `--cpus` `--memory`
    //
    // 容器里的进程宿主机 kill 不到(即使拿到 pid),SIGKILL 也只终止容器的
    // init 而不一定让服务停下,所以必须走 Docker API —— 硬套进程那套会给出
    // 误导性的结果(比如"暂停"后容器照常服务,看着像注入失败)。
    //
    // 全部需要 Docker CLI 在 PATH 上,且当前用户有权限访问 docker daemon。
    bool container_add(const std::string& node_id,
                       const std::string& image,
                       const std::string& command,
                       const std::string& cpus,
                       const std::string& memory,
                       std::string& error);

    bool container_pause(const std::string& node_id, std::string& error);
    bool container_unpause(const std::string& node_id, std::string& error);
    // 崩溃注入:docker kill,容器进程被 SIGKILL。调谐会把它重新 run 起来。
    bool container_crash(const std::string& node_id, std::string& error);
    // 容器日志
    bool container_logs(const std::string& node_id, std::string& out, std::string& error);
    // 把一个已纳管的节点纳入某组(节点本身不重启)
    bool group_add_member(const std::string& group_id,
                          const std::string& node_id,
                          std::string& error);
    // 调整期望副本数(扩容/缩容都会在下一轮调谐里生效)
    bool group_set_replicas(const std::string& group_id,
                            int32_t desired_replica_count,
                            std::string& error);
    // 解散副本组(不停止成员进程,只是不再维持它们)
    bool group_destroy(const std::string& group_id, std::string& error);
    bool group_get(const std::string& group_id, ReplicaGroup& out);
    std::vector<ReplicaGroup> group_list();
    // 手动跑一轮并拿到详细报告(测试与观测用)
    ReconcileReport reconcile_report_once();

private:
    KvAdmin() = default;
    ~KvAdmin();
    KvAdmin(const KvAdmin&) = delete;
    KvAdmin& operator=(const KvAdmin&) = delete;

    // 内部：spawn 进程
    int64_t spawn_process(const KvNode& node, std::string& error);
    // 内部：发送信号
    bool send_signal(int64_t pid, int sig, std::string& error);
    // 内部：检查进程存活（waitpid WNOHANG）
    bool process_alive(int64_t pid);
    // 内部：探测单个节点,填 observed。调用方必须已持有 mutex_。
    NodeObserved probe_locked(KvNode& node);
    // 内部：拉起 desired=PRESENT 但观测不健康的节点。调用方须已持有 mutex_。
    bool reconcile_one_locked(KvNode& node, int64_t now);
    // 内部：停掉并摘除 desired=ABSENT 的节点。调用方须已持有 mutex_。
    bool retire_one_locked(const std::string& node_id, KvNode& node, int64_t now);

    std::map<std::string, KvNode> nodes_;
    std::mutex mutex_;

    // ---- 调谐循环内部状态(2026-10 增补) ----
    std::thread reconcile_thread_;
    std::atomic<bool> reconcile_stop_{false};
    std::atomic<bool> reconcile_active_{false};
    int64_t reconcile_interval_ms_{1000};
    int64_t retry_backoff_max_ms_{60000};   // 连续失败退避上限,默认 60s
    // 连续拉起失败次数与下次允许重试的时刻。由 retry_mu_ 保护,不用主锁 ——
    // 主锁在 spawn 期间必须保持干净,且这些不属于节点注册表本身。
    struct RetrySlot {
        int64_t consecutive_fail{0};
        int64_t next_attempt_ms{0};
    };
    std::map<std::string, RetrySlot> retry_;
    std::mutex retry_mu_;

    // ---- 可注入的 spawn 钩子(测试用) ----
    SpawnHook spawn_hook_;
    // 注入模式下"活着"的假 pid 集合。由 test_kill_fake 移除。
    std::set<int64_t> fake_alive_;
    std::mutex fake_mu_;

    // ---- 副本组(2026-10 增补) ----
    std::map<std::string, ReplicaGroup> groups_;
    // 组调谐的补副本序列号,用于生成不冲突的节点 id/端口
    std::map<std::string, int64_t> group_seq_;

    // ---- LSM 持久化内部状态 ----
    std::string lsm_addr_;            // LSM 后端地址(host:port),空表示未启用
    bool lsm_restored_{false};        // 是否已从 LSM 恢复过(幂等控制)
    // 将单个节点状态同步写入 LSM(需在已持有锁或变更点调用)
    void persist_node(const KvNode& node);
    // 从 LSM 删除节点记录
    void persist_erase(const std::string& node_id);
};
