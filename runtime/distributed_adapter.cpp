// 分布式模块 lite::Value 适配器
// 将 lite::Value* 参数转换为 const char* + int64_t 形式，
// 使 raft/cluster/kv 模块的函数可以被 PyLite 内置模块调用机制使用。
#include "pylite/runtime.h"
#include "pylite/value.h"
#include <cstring>

// 从 lite::Value 中提取字符串指针和长度
static const char* strPtr(const lite::Value* v) {
    if (v->tag != lite::kStr) return "";
    return str_data(v);
}
static int64_t strLen(const lite::Value* v) {
    if (v->tag != lite::kStr) return 0;
    return str_size(v);
}
// 底层 kv 接口前置声明
extern "C" lite::Value kv_persist(const char*, int64_t);
// ====== Raft 适配器 ======
extern "C" {

lite::Value raft_build_v(const lite::Value* config) {
    return raft_build(strPtr(config), strLen(config));
}

lite::Value raft_erase_v(const lite::Value* node_id) {
    return raft_erase(strPtr(node_id), strLen(node_id));
}

lite::Value raft_status_v(const lite::Value* node_id) {
    return raft_status(strPtr(node_id), strLen(node_id));
}

lite::Value raft_list_nodes_v() {
    return raft_list_nodes();
}

lite::Value raft_add_v(const lite::Value* node_id, const lite::Value* cmd_type, const lite::Value* payload) {
    return raft_add(strPtr(node_id), strLen(node_id),
                       strPtr(cmd_type), strLen(cmd_type),
                       strPtr(payload), strLen(payload));
}

lite::Value raft_register_handler_v(const lite::Value* node_id, const lite::Value* cmd_type) {
    int64_t r = raft_register_handler(strPtr(node_id), strLen(node_id),
                                          strPtr(cmd_type), strLen(cmd_type));
    return int_(r);
}

lite::Value raft_register_node_v(const lite::Value* node_id, const lite::Value* role, const lite::Value* addr) {
    return raft_register_node(strPtr(node_id), strLen(node_id),
                                 strPtr(role), strLen(role),
                                 strPtr(addr), strLen(addr));
}

lite::Value raft_cluster_nodes_v(const lite::Value* node_id) {
    return raft_cluster_nodes(strPtr(node_id), strLen(node_id));
}

lite::Value raft_mount_storage_v(const lite::Value* node_id, const lite::Value* name, const lite::Value* path) {
    return raft_mount_storage(strPtr(node_id), strLen(node_id),
                                 strPtr(name), strLen(name),
                                 strPtr(path), strLen(path));
}

lite::Value raft_plugin_load_v(const lite::Value* node_id, const lite::Value* name, const lite::Value* so_path) {
    return raft_plugin_load(strPtr(node_id), strLen(node_id),
                               strPtr(name), strLen(name),
                               strPtr(so_path), strLen(so_path));
}

lite::Value raft_plugin_unload_v(const lite::Value* node_id, const lite::Value* name) {
    return raft_plugin_unload(strPtr(node_id), strLen(node_id),
                                 strPtr(name), strLen(name));
}

lite::Value raft_plugin_list_v(const lite::Value* node_id) {
    return raft_plugin_list(strPtr(node_id), strLen(node_id));
}

// ====== 分布式便捷内建适配器(2026-10 增补) ======
lite::Value raft_leader_info_v(const lite::Value* node_id) {
    return raft_leader_info(strPtr(node_id), strLen(node_id));
}
lite::Value raft_send_v(const lite::Value* node_id, const lite::Value* msg) {
    return raft_send(strPtr(node_id), strLen(node_id),
                        strPtr(msg), strLen(msg));
}
lite::Value raft_recv_v(const lite::Value* node_id) {
    return raft_recv(strPtr(node_id), strLen(node_id));
}

}  // extern "C"

#ifndef _WIN32
// cluster / kv 的适配器依赖 cluster_api.cpp 与 kv_admin_api.cpp —— 那两个用了
// POSIX 网络与 fork,只在 Linux 构建里。raft 那一段是自足的,两个平台都编。

extern "C" {

// ====== Cluster 适配器 ======

lite::Value cluster_connect_v(const lite::Value* name, const lite::Value* addr, const lite::Value* secret) {
    return cluster_connect(strPtr(name), strLen(name),
                              strPtr(addr), strLen(addr),
                              strPtr(secret), strLen(secret));
}

lite::Value cluster_disconnect_v(const lite::Value* name) {
    return cluster_disconnect(strPtr(name), strLen(name));
}

lite::Value cluster_list_v() {
    return cluster_list();
}

lite::Value cluster_is_connected_v(const lite::Value* name) {
    int64_t r = cluster_is_connected(strPtr(name), strLen(name));
    return int_(r);
}

lite::Value cluster_status_v(const lite::Value* name) {
    return cluster_status(strPtr(name), strLen(name));
}

lite::Value cluster_nodes_v(const lite::Value* name) {
    return cluster_nodes(strPtr(name), strLen(name));
}

lite::Value cluster_health_v(const lite::Value* name) {
    int64_t r = cluster_health(strPtr(name), strLen(name));
    return int_(r);
}

lite::Value cluster_exec_v(const lite::Value* name, const lite::Value* cmd_type, const lite::Value* payload) {
    return cluster_exec(strPtr(name), strLen(name),
                           strPtr(cmd_type), strLen(cmd_type),
                           strPtr(payload), strLen(payload));
}

lite::Value cluster_query_v(const lite::Value* name, const lite::Value* path) {
    return cluster_query(strPtr(name), strLen(name),
                            strPtr(path), strLen(path));
}

lite::Value cluster_broadcast_v(const lite::Value* cmd_type, const lite::Value* payload) {
    return cluster_broadcast(strPtr(cmd_type), strLen(cmd_type),
                                strPtr(payload), strLen(payload));
}

lite::Value cluster_health_all_v() {
    return cluster_health_all();
}

// ====== KvAdmin 适配器 ======

lite::Value kv_build_v(const lite::Value* node_id, const lite::Value* binary, const lite::Value* args_json,
                      const lite::Value* work_dir, const lite::Value* addr) {
    return kv_build(strPtr(node_id), strLen(node_id),
                       strPtr(binary), strLen(binary),
                       strPtr(args_json), strLen(args_json),
                       strPtr(work_dir), strLen(work_dir),
                       strPtr(addr), strLen(addr));
}

lite::Value kv_add_v(const lite::Value* node_id, const lite::Value* binary, const lite::Value* args_json,
                    const lite::Value* work_dir, const lite::Value* addr) {
    return kv_add(strPtr(node_id), strLen(node_id),
                     strPtr(binary), strLen(binary),
                     strPtr(args_json), strLen(args_json),
                     strPtr(work_dir), strLen(work_dir),
                     strPtr(addr), strLen(addr));
}

lite::Value kv_erase_v(const lite::Value* node_id) {
    return kv_erase(strPtr(node_id), strLen(node_id));
}

lite::Value kv_skip_v(const lite::Value* node_id) {
    return kv_skip(strPtr(node_id), strLen(node_id));
}

lite::Value kv_sleep_v(const lite::Value* node_id) {
    return kv_sleep(strPtr(node_id), strLen(node_id));
}

lite::Value kv_wakeup_v(const lite::Value* node_id) {
    return kv_wakeup(strPtr(node_id), strLen(node_id));
}

lite::Value kv_stop_v(const lite::Value* node_id) {
    return kv_stop(strPtr(node_id), strLen(node_id));
}

lite::Value kv_restart_v(const lite::Value* node_id) {
    return kv_restart(strPtr(node_id), strLen(node_id));
}

lite::Value kv_status_v(const lite::Value* node_id) {
    return kv_status(strPtr(node_id), strLen(node_id));
}

lite::Value kv_list_v() {
    return kv_list();
}

lite::Value kv_persist_v(const lite::Value* addr) {
    return kv_persist(strPtr(addr), strLen(addr));
}
lite::Value kv_alive_v(const lite::Value* node_id) {
    int64_t r = kv_alive(strPtr(node_id), strLen(node_id));
    return int_(r);
}

lite::Value kv_bind_sandbox_v(const lite::Value* node_id, const lite::Value* handle) {
    int64_t r = kv_bind_sandbox(strPtr(node_id), strLen(node_id), handle->as.i);
    return int_(r);
}

lite::Value kv_get_sandbox_v(const lite::Value* node_id) {
    int64_t r = kv_get_sandbox(strPtr(node_id), strLen(node_id));
    return int_(r);
}

lite::Value kv_unbind_sandbox_v(const lite::Value* node_id) {
    int64_t r = kv_unbind_sandbox(strPtr(node_id), strLen(node_id));
    return int_(r);
}

// ====== add(box) 编排与容器内定位适配器(2026-10 增补) ======
// box 是实例对象,直接透传指针,不走 strPtr 字符串提取
lite::Value kv_add_box_v(const lite::Value* node_id, const lite::Value* box, const lite::Value* addr) {
    return kv_add_box(strPtr(node_id), strLen(node_id),
                         box,
                         strPtr(addr), strLen(addr));
}
lite::Value kv_locate_v(const lite::Value* box) {
    return kv_locate(box);
}
lite::Value kv_self_v() {
    return kv_self();
}
// ====== 节点迭代器适配器(2026-10 增补) ======
lite::Value kv_iter_v() {
    return kv_iter();
}
lite::Value kv_iter_next_v(const lite::Value* it) {
    return kv_iter_next(it);
}
lite::Value kv_iter_rewind_v(const lite::Value* it) {
    return kv_iter_rewind(it);
}
lite::Value kv_iter_destroy_v(const lite::Value* it) {
    return kv_iter_destroy(it);
}
lite::Value kv_iter_live_v() {
    return kv_iter_live();
}
// ====== 节点内置 MQ 队列适配器(2026-10 增补) ======
// 底层队列接口前置声明(kv_queue_api.cpp)
lite::Value kv_enqueue(const char*, int64_t, const char*, int64_t);
lite::Value kv_dequeue(const char*, int64_t, const char*, int64_t);
lite::Value kv_done(const char*, int64_t);
lite::Value kv_qlen(const char*, int64_t);
lite::Value kv_set_quota(const char*, int64_t, const char*, int64_t);
lite::Value kv_quota(const char*, int64_t);

lite::Value kv_enqueue_v(const lite::Value* queue, const lite::Value* payload) {
    return kv_enqueue(strPtr(queue), strLen(queue),
                         strPtr(payload), strLen(payload));
}
lite::Value kv_dequeue_v(const lite::Value* node_id, const lite::Value* queue) {
    return kv_dequeue(strPtr(node_id), strLen(node_id),
                         strPtr(queue), strLen(queue));
}
lite::Value kv_done_v(const lite::Value* node_id) {
    return kv_done(strPtr(node_id), strLen(node_id));
}
lite::Value kv_qlen_v(const lite::Value* queue) {
    return kv_qlen(strPtr(queue), strLen(queue));
}
lite::Value kv_set_quota_v(const lite::Value* node_id, const lite::Value* quota) {
    return kv_set_quota(strPtr(node_id), strLen(node_id),
                           strPtr(quota), strLen(quota));
}
lite::Value kv_quota_v(const lite::Value* node_id) {
    return kv_quota(strPtr(node_id), strLen(node_id));
}
// ====== 通用 KV 读写与本地文件写入适配器(2026-10 增补) ======
lite::Value kv_kset(const char*, int64_t, const char*, int64_t);
lite::Value kv_kget(const char*, int64_t);
lite::Value file_write(const char*, int64_t, const char*, int64_t);
lite::Value file_read(const char*, int64_t);
int64_t str_to_int(const char*, int64_t);
lite::Value int_to_str(int64_t);

lite::Value kv_kset_v(const lite::Value* key, const lite::Value* value) {
    return kv_kset(strPtr(key), strLen(key),
                      strPtr(value), strLen(value));
}
lite::Value kv_kget_v(const lite::Value* key) {
    return kv_kget(strPtr(key), strLen(key));
}
lite::Value file_write_v(const lite::Value* path, const lite::Value* content) {
    return file_write(strPtr(path), strLen(path),
                         strPtr(content), strLen(content));
}
lite::Value file_read_v(const lite::Value* path) {
    return file_read(strPtr(path), strLen(path));
}
lite::Value str_to_int_v(const lite::Value* s) {
    return int_(str_to_int(strPtr(s), strLen(s)));
}
lite::Value int_to_str_v(const lite::Value* v) {
    int64_t n = (v && v->tag == lite::kInt) ? v->as.i : 0;
    return int_to_str(n);
}

}  // extern "C"

#endif  // !_WIN32
