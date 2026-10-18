# xmex 分布式设计借鉴 adviskv

本文记录 xmex 当前分布式实现与 adviskv 的差距，以及借鉴方案。
写给改代码的人看，不是设计宣言——每条都标注了证据位置。

---

## 一、结论先行

xmex 已经有**进程管理**（spawn/kill/信号/持久化），但缺**状态收敛机制**。
两者不是同一层次的东西：xmex 是命令式的（调什么做什么），adviskv 是声明式的
（说清期望，系统自己调谐）。这是最本质的差距。

| 维度 | xmex 现状 | adviskv |
|---|---|---|
| 状态字段 | `KvNodeState` 单字段 6 态 | `ReplicaDesired` + `ReplicaPhase` + `observed_*` 三层 |
| 收敛机制 | 无 | `reconcile/` 两个调谐器 |
| 选择策略 | 无 | `selector/node_selector` |
| 副本组 | 无此概念 | `ReplicaGroup`（BOOTSTRAP / RAFT_RECONFIG） |
| 共识 | 自研 raft.cpp（模拟） | 真实 Raft 3164 行 |

---

## 二、xmex 现状（证据）

### 2.1 状态模型是单字段

`runtime/kv_admin.h:11`：

```cpp
enum class KvNodeState {
    IDLE      = 0,   // 已注册但未启动
    RUNNING   = 1,   // 进程运行中
    SLEEPING  = 2,   // SIGSTOP 暂停（故障注入）
    STOPPED   = 3,   // 已停止（可重新启动）
    ERASED    = 4,   // 已永久移除
    LOST      = 5    // 逻辑跳过（副本标记丢失）
};
```

`grep -c "desired|observed" runtime/kv_admin.{h,cpp}` → **0 0**

一个字段同时表达「我想要它跑」和「它现在跑没跑」。后果是：

- 无法表达「我想要 3 个副本，现在只有 2 个」——只有 LOST 一个笼统标记
- 状态只能被动改写。`skip_node()` 把状态设成 LOST 就结束了，**没有任何东西会去补副本**
- 崩溃恢复后不知道该回到什么状态

### 2.2 已有能力（这部分做得不错，保留）

- 进程操作是真的：spawn / waitpid / killpid（`kv_admin.cpp`）
- 故障注入是真的：`sleep_node` 发 SIGSTOP、`wakeup` 发 SIGCONT
- `skip_node` 标记 LOST 但保留进程，数据不丢——语义是对的
- LSM/Redis 持久化：节点变更自动同步，重启可恢复（`persist_node`）

### 2.3 自研 Raft 是模拟的

`runtime/raft.cpp:130`：

```cpp
// 模拟选举：如果没有其他节点，直接成为 leader
if (node->config_.peers.empty()) { node->role_ = RaftRole::LEADER; }
```

无 RequestVote、无 AppendEntries。多节点时 role 在 FOLLOWER/CANDIDATE 之间循环，
term 无限增长，选不出 leader。配置里的 `election_timeout_ms`(150ms)、
`heartbeat_interval_ms`(50ms) 无真实发送方。

**adviskv 的 Raft 是真的**：`third_party/adviskv/src/storage/raft/`，13 个文件 3164 行，
含 `handle_request_vote` / `handle_append_entries` / ReadIndex / PreVote / WAL / Snapshot。

---

## 三、借鉴方案

### 3.1 第一步：desired / observed 分离

把 `KvNodeState` 拆成三个字段。这是所有后续改动的地基。

```cpp
// 期望——由调用方声明
enum class NodeDesired {
    PRESENT = 1,   // 我要它在跑
    ABSENT  = 2,   // 我要它下线
};

// 实际——由系统观测填入
enum class NodeObserved {
    ABSENT    = 1,   // 进程不在
    ALIVE     = 2,   // 进程活着
    UNREACHABLE = 3, // 进程在但探针不通
};

struct KvNode {
    // ... 现有字段保留

    NodeDesired  desired{NodeDesired::PRESENT};
    NodeObserved observed{NodeObserved::ABSENT};
    std::string  last_error;
    int64_t      last_update_ms{0};

    // 兼容期保留，改造完成后删除
    KvNodeState state{KvNodeState::IDLE};
};
```

`desired` 由 `build/add` 设为 PRESENT，由 `erase/skip` 设为 ABSENT。
`observed` 由探测填入。**两者不一致就是需要调谐的信号。**

对照 adviskv（`src/topo/model/model.h:54`）：

```cpp
struct ReplicaState {
    ReplicaDesired desired{ReplicaDesired::PRESENT};      // 期望
    ReplicaPhase   phase{ReplicaPhase::PENDING};          // 阶段
    RaftMemberType observed_member_type{...};             // 观测
    Endpoint       observed_endpoint;                     // 观测
    StorageReplicaStatus observed_storage_status{...};    // 观测
    bool      observed_no_exist{false};                   // 观测
    std::string last_error_msg;                           // 错误
    int64_t update_ts{0};                                 // 时间
};
```

注意 adviskv 的 `observed_*` 有 5 个，xmex 第一版不需要这么多，先要
`observed` + `last_error` + `last_update_ms` 三个。

### 3.2 第二步：调谐循环

新增 `runtime/kv_reconcile.cpp`，一个后台线程，周期跑（比如 1 秒），做三件事：

```
reconcile(node):
    if node.desired == PRESENT and node.observed != ALIVE:
        spawn(node)                 # 拉起来
        node.last_update_ms = now

    if node.desired == ABSENT and node.observed != ABSENT:
        kill(node)                  # 收掉
        erase_from_registry(node)

    # 不做任何事就是收敛了
```

**关键：调谐循环是幂等的。** 跑一次和跑一百次结果相同，这是它比命令式优越的地方。

对照 adviskv 的两个调谐器（`src/topo/reconcile/`）：

- `replica_group_plan_reconciler` —— 把「期望 3 副本」展开成具体计划（选哪些节点）
- `replica_group_membership_reconciler` —— 驱动实际副本向计划靠拢

xmex 第一版只需要一个，逻辑简单得多。

### 3.3 第三步：副本组（可选，需求驱动）

如果需要「声明要 N 个副本」，引入最小的 `ReplicaGroup`：

```cpp
struct ReplicaGroup {
    std::string group_id;
    int32_t     desired_replica_count{1};
    std::vector<std::string> member_ids;    // 当前实际成员
    std::string primary_id;                 // 选出的主
};
```

`desired_replica_count` 与 `member_ids.size()` 不一致时，调谐循环补充或收缩。
这一步依赖 3.1 和 3.2 完成。

---

## 四、不该借鉴的部分

| adviskv 的东西 | 为什么不要照搬 |
|---|---|
| gRPC 通信 | xmex 已有一套手写 TCP + OpenSSL（`cluster.cpp`），够了。引入 gRPC 会带 protobuf/absl 一堆依赖 |
| 自己实现 Raft | adviskv 已有真 Raft，xmex 的 `raft.cpp` 建议**退役**而不是完善。两套共识并存是负担 |
| `EngineType`/`dc` 多引擎多机房 | xmex 当前是单机房场景，YAGNI |
| 完整 WAL + Snapshot | adviskv 的 `persist/` 1159 行。xmex 节点注册表只需 Redis，不需要自建 WAL |

**尤其最后一条关于 Raft**：`runtime/raft.cpp` 唯一的价值是跨平台编译（不在
`if(NOT WIN32)` 里），可以当教学演示留着，但不要在它上面继续加功能。

---

## 五、落地顺序与完成情况

| 步骤 | 内容 | 涉及文件 | 状态 |
|---|---|---|---|
| 1 | desired/observed 字段拆分，`state` 保留兼容 | `kv_admin.h/.cpp` | ✅ 完成 |
| 2 | 探测函数填 observed | `kv_admin.cpp` | ✅ 完成（`probe_locked`） |
| 3 | 调谐循环 + 后台线程 | `kv_admin.cpp` | ✅ 完成（未单独建文件，逻辑放在 kv_admin 内） |
| 4 | CMakeLists 加源文件，irgen 加语言层入口 | `CMakeLists.txt`、`irgen.cpp` | ✅ 完成 |
| 5 | 副本组 | `kv_admin.h/.cpp` | ✅ 完成 |
| 6 | Redis 持久化改存 desired | `kv_admin.cpp` | ⬜ **未做，见下** |

### 步骤 6 是唯一剩下的坑

`persist_node` 现在存的是 `state`，不是 `desired`。加了 `desired` 后如果不同步存，
**进程重启后节点会丢掉意图**——这正是原设计缺陷的持久化版本。

具体表现：节点在运行中被标记 `desired=ABSENT`（准备下线），
节点管理器重启后从 Redis 恢复出来的是 `state=stopped`，
`desired` 回到默认的 `PRESENT`，调谐循环会把它重新拉起来。

修法：`persist_node` 的 JSON 里加 `"desired":"present|absent"`，
`restore_from_lsm` 读回来填 `node.desired`。

---

## 六、验证方式

`tests/reconcile_test.cpp`（11 个用例，56 项断言）覆盖了下列要点。
它用 `set_spawn_hook` 注入假 spawn，不 fork 真实进程，
**因此在 Windows 上也能跑**——这是刻意的：调谐逻辑不必等到有 Linux CI 才能验证。

```bash
# Linux(CMake 已接好)
cmake --build build && cd build && ctest -R reconcile_test --output-on-failure
```

| # | 用例 | 验证什么 |
|---|---|---|
| 1 | desired/observed 分离 | build 后 desired=PRESENT、observed=ALIVE |
| 2 | 调谐幂等性 | 连续两轮，第二轮 actions==0、pid 不变 |
| 3 | 崩溃自动恢复 | 假进程"崩"后被拉起，pid 变、restart_count 增 |
| 4 | **erase 后不被复活** | 节点从注册表消失后调谐不会拉回来 |
| 5 | 暂停节点不被误修复 | SIGSTOP 的节点 observed=SUSPENDED，不被当掉线 |
| 6 | skip 语义 | 进程保留、desired 仍 PRESENT、LOST 标记可清除 |
| 7 | 失败退避 | spawn 失败后进退避窗口，不反复重试 |
| 8 | **副本组** | 崩溃成员原地重启；skip 成员触发补替补 |
| 9 | 声明缩容 | 期望副本数可改 |
| 10 | 调谐线程生命周期 | 启停幂等 |
| 11 | 副本组调谐幂等 | 成员健康时不重复补 |

### 写测试时发现的三个真实设计缺陷

这三个 bug 都是测试先发现的，不是读代码看出来的：

**1. 单节点调谐和组调谐会打架。**
最初实现里，组内成员崩溃会被单节点调谐直接原地重启。这导致组调谐永远看不到
"少了一个健康副本"这个信号，也就永远不会补替补——副本数一直是满的，
补副本逻辑形同虚设。
修法：给 `KvNode` 加 `group_id` 字段，组内成员的处置权归组调谐，
单节点调谐遇到 `group_id` 非空的节点直接跳过。

**2. skip 后 observed 被 probe 改回 ALIVE，副本缺口看不见。**
`skip` 把 `state` 设成 LOST，但进程还活着，下一轮 `probe_locked` 会把
observed 重新判成 ALIVE。而健康副本数是按 observed 统计的，于是组调谐
以为副本还是满的，不补替补。
修法：probe 时若 `state==LOST`，observed 继续报 CRASHED（不健康）。

**3. build 阶段失败没进退避计数。**
`build` 失败时返回 false 但没记失败次数，调谐第一轮就立刻重试，
紧接着 build 刚返回 false 又被拉起来，看起来像抖动。
修法：build 失败也写 `retry_` 计数与下次重试时刻。

---

## 七、语言层接口

调谐与副本组通过 `kv.*` 内置模块暴露（`irgen.cpp` 的 `builtinModulePrefix` 映射）：

| Xmex 写法 | 作用 |
|---|---|
| `kv.reconcile_start(1000)` | 启动后台调谐循环（毫秒间隔） |
| `kv.reconcile_stop()` | 停止 |
| `kv.reconcile_status()` | `{"running":bool,"nodes":N,"groups":G}` |
| `kv.reconcile_once()` | 手动跑一轮，返回详细报告 |
| `kv.unskip(node_id)` | 恢复被 skip 的节点并立刻拉起 |
| `kv.failures(node_id)` | 连续拉起失败次数 |
| `kv.group_create(id, replicas, binary, work_dir, addr)` | 声明副本组 |
| `kv.group_add(id, node_id)` | 把已纳管节点纳入组 |
| `kv.group_replicas(id, n)` | 调整期望副本数 |
| `kv.group_status(id)` | 组状态；空 id 返回全部组 |
| `kv.group_destroy(id)` | 解散组（成员进程不受影响） |

`kv.status(node_id)` 的返回里现在多了 `desired` / `observed` / `last_error` /
`last_update_ms` 四个字段，与旧的 `state` 并存。

**不启动调谐循环时，一切行为与从前完全一致**（命令式）。
这是刻意保留的：调谐是可选增强，不该成为使用门槛。

