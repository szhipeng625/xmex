# 为什么移除了自研 raft 模块

2026-10-18，`runtime/raft.cpp`、`runtime/raft.h`、`runtime/raft_api.cpp`
连同其语言层映射一起移除，共 904 行代码 + 15 个适配函数。

本文写清楚三件事：**它当时是什么、为什么删、什么时候需要把它拿回来**。
半年后如果有人问「我们的 raft 代码呢」，答案在这里。

---

## 一、它当时是什么

一个**跨平台可编译的 Raft 状态机演示**，不是可用的共识实现。

它的唯一价值是：`runtime/` 里绝大多数分布式模块都被 `if(NOT WIN32)` 挡住
（依赖 POSIX socket、fork、seccomp），而 raft 那三个文件不依赖 socket，
因此**在 Windows 上也能编译运行**——移植回 Windows 时它是唯一能立刻
演示的分布式模块（`examples/raft_demo.pys` 曾标注「移植回 Windows 后第一个
能真正跑起来的分布式模块」）。

---

## 二、为什么删：它没有共识，只有一个计数器

### 2.1 选举是模拟的

`raft.cpp` 的选举超时回调（原 130 行附近）：

```cpp
if (node->role_ == RaftRole::FOLLOWER) {
    node->role_ = RaftRole::CANDIDATE;
    node->term_++;
    node->voted_for_ = node->config_.node_id;
    // 模拟选举：如果没有其他节点，直接成为 leader
    if (node->config_.peers.empty()) {
        node->role_ = RaftRole::LEADER;
    }
}
```

没有 `RequestVote` RPC，没有投票计数，没有多数派判定。
**有 peers 时不做任何选举动作**，只是 `term++` 然后继续等下一个超时。

后果：多节点部署下，所有节点在 FOLLOWER ↔ CANDIDATE 之间来回摆动，
`term` 无限增长，**永远选不出 leader**。唯一能当 leader 的情况是
单节点（`peers` 为空）。

### 2.2 提交没有复制

`RaftNode::propose` 的全部"复制"逻辑：

```cpp
log_.push_back(entry);
commit_index_ = entry.index;     // 没有任何网络发送
auto handler_it = handlers_.find(cmd_type);
if (handler_it != handlers_.end()) {
    return handler_it->second(entry.cmd);   // 本地直接执行
}
```

`log_` 是 `std::vector<LogEntry>`，进程退出即消失。没有 `AppendEntries`，
没有 follower 确认，`commit_index_` 是自己给自己发的。

### 2.3 其余能力同样是空壳

| 方法 | 实际实现 |
|---|---|
| `mount_storage(name, path)` | `storage_mounts_[name] = path;` 然后打一行日志 |
| `plugin_load` | 检查 `.so` 存在后 `dlopen`，但加载的插件从不参与共识 |
| `heartbeat` 间隔配置 | 无任何发送方 |
| `snapshot_interval` | 无实现 |

**所有状态都在进程内存里。** 崩溃一次，term、log、storage_mounts 全部归零。

### 2.4 它是被"链接但从未使用"的

`grep -rn "raft_build\|raft_status\|raft_propose" runtime/ tools/`
（排除自身后）→ **零命中**。

它对外的唯一出口是 irgen 里 15 个语言层映射（`raft.build()` 等），
没有任何内部模块调用它。也就是说：**除用户手写 `raft.*` 代码外，
这个模块在整个项目里从未被执行过一次。**

---

## 三、替代品：adviskv 子模块

`third_party/adviskv`（20403 行 C++17）里有**真正的 Raft**：

```
third_party/adviskv/src/storage/raft/     13 个文件 3164 行
├── raft_election.cpp          选举（含 PreVote）
├── raft_replication.cpp       日志复制
├── raft_sender.cpp            RPC 发送
├── raft_log.cpp               日志管理
├── raft_membership.cpp        成员变更
├── raft_peer_progress.cpp     副本进度跟踪
├── raft_apply.cpp             状态机应用
├── core/raft_core.h           handle_request_vote / handle_append_entries
└── grpc_raft_rpc_transport.cpp  gRPC 传输
```

对照缺失项：

| 能力 | 自研 raft | adviskv |
|---|---|---|
| RequestVote / AppendEntries | 无 | 有 |
| 多数派判定 | 无 | 有 |
| PreVote（防旧 Leader 干扰） | 无 | 有 |
| 日志追赶 | 无 | 有 |
| WAL + Snapshot | 无 | 有（`persist/` 1159 行） |
| ReadIndex | 无 | 有 |
| 跨进程通信 | 无（不碰 socket） | gRPC |

**这不是"更好的实现 vs 更差的实现"，而是"实现"和"没实现"。**
两套并存只会让人误以为 xmex 有共识能力。

---

## 四、现在的分布式架构

2026-10-18 确定走**进程纳管**路线：

```
xmex KvAdmin（进程管理 + 故障注入）
    │  spawn / kill / SIGSTOP / SIGCONT / 健康检查 / Redis 持久化
    ▼
adviskv topo / catalog / storage（三个可执行体，自带真 Raft）
```

对应 adviskv 的边界划分：

- `KvAdmin` 负责**进程生死**：拉起、停止、暂停、恢复
- adviskv 负责**数据一致**：Raft 复制、WAL、选主
- xmex 负责**观测**：`sleep_node` 制造故障，`distributed_adapter` 收集埋点

这样分工后，`KvNodeState` 里的 `LOST` / `skip_node` 的副本语义才真正说得通——
它们本来就是为了管理一个带副本的分布式存储而设计的。

设计细节见 [distributed-reconcile-design.md](distributed-reconcile-design.md)。

---

## 五、什么时候需要把它拿回来

以下任一情况出现，可以从 git 历史恢复（`0b6af19` 的父提交）：

### 5.1 需要跨平台共识

adviskv 是 Linux + gRPC 项目，**在 Windows 上不参与构建**。
如果 xmex 必须在 Windows 上提供真正的共识能力，自研实现（哪怕不完整）
可以作为起点。恢复方式：

```bash
git show 0b6af19^:runtime/raft.cpp > runtime/raft.cpp
git show 0b6af19^:runtime/raft.h   > runtime/raft.h
git show 0b6af19^:runtime/raft_api.cpp > runtime/raft_api.cpp
# 再把 CMakeLists 的 runtime/raft.cpp、runtime/raft_api.cpp 加回
# XMEX_RUNTIME_SOURCES，以及 distributed_adapter.cpp 与 irgen 的映射段
```

### 5.2 需要教学演示

自研实现的**优点**是可以在单文件里展示 Raft 的状态机流转，
没有 gRPC 依赖，`git clone` 就能跑。adviskv 做不到这一点——
它需要 vcpkg 拉 protobuf/gRPC/absl，首次构建以十分钟计。

如果文档、课程、REPL 演示需要"能随手跑的 Raft"，恢复它有价值。

### 5.3 想真正把它做成共识实现

如果打算在自研代码上补齐 RequestVote / AppendEntries，
**建议先认真评估 adviskv**：它已经有 3164 行实现 + PreVote + WAL + Snapshot，
从零补齐的活量远大于移植成本。

---

## 六、恢复前的检查清单

若决定恢复，注意这几点（都是移除时同步清理的）：

- [ ] `CMakeLists.txt` 的 `XMEX_RUNTIME_SOURCES` 加回 `runtime/raft.cpp`、`runtime/raft_api.cpp`
- [ ] `distributed_adapter.cpp` 加回 `// ====== Raft 适配器 ======` 段
      （它原本在 `#ifndef _WIN32` 之前，是跨平台的；注意现在的文件结构已变，
      extern "C" 块的起始位置和移除前不同，直接照抄旧版可能编译不过）
- [ ] `irgen.cpp` 的 `builtinModulePrefix()` 加回 `if (mod == "raft")` 分支
- [ ] `docs/distributed-benchmark.md` 里那段"已移除"的注可以删掉

**注意** `raft.cpp` 不依赖 socket，所以三个文件都是跨平台编译的——
这与 `cluster` / `kv_admin` / `sandbox` 不同（那几个被 `if(NOT WIN32)` 排除）。
