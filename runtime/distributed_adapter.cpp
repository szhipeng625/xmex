// 分布式模块 xmex::Value 适配器
// 将 xmex::Value* 参数转换为 const char* + int64_t 形式，
// 使 cluster/kv 模块的函数可以被 Xmex 内置模块调用机制使用。
#include "xmex/runtime.h"
#include "xmex/value.h"
#include <cstring>

// 从 xmex::Value 中提取字符串指针和长度
static const char* strPtr(const xmex::Value* v) {
    if (v->tag != xmex::kStr) return "";
    return str_data(v);
}
static int64_t strLen(const xmex::Value* v) {
    if (v->tag != xmex::kStr) return 0;
    return str_size(v);
}
// 底层 kv 接口前置声明
extern "C" xmex::Value kv_persist(const char*, int64_t);

#ifndef _WIN32
// cluster / kv 的适配器依赖 cluster_api.cpp 与 kv_admin_api.cpp —— 那两个用了
// POSIX 网络与 fork,只在 Linux 构建里。

extern "C" {

// ====== Cluster 适配器 ======

xmex::Value cluster_connect_v(const xmex::Value* name, const xmex::Value* addr, const xmex::Value* secret) {
    return cluster_connect(strPtr(name), strLen(name),
                              strPtr(addr), strLen(addr),
                              strPtr(secret), strLen(secret));
}

xmex::Value cluster_disconnect_v(const xmex::Value* name) {
    return cluster_disconnect(strPtr(name), strLen(name));
}

xmex::Value cluster_list_v() {
    return cluster_list();
}

xmex::Value cluster_is_connected_v(const xmex::Value* name) {
    int64_t r = cluster_is_connected(strPtr(name), strLen(name));
    return int_(r);
}

xmex::Value cluster_status_v(const xmex::Value* name) {
    return cluster_status(strPtr(name), strLen(name));
}

xmex::Value cluster_nodes_v(const xmex::Value* name) {
    return cluster_nodes(strPtr(name), strLen(name));
}

xmex::Value cluster_health_v(const xmex::Value* name) {
    int64_t r = cluster_health(strPtr(name), strLen(name));
    return int_(r);
}

xmex::Value cluster_exec_v(const xmex::Value* name, const xmex::Value* cmd_type, const xmex::Value* payload) {
    return cluster_exec(strPtr(name), strLen(name),
                           strPtr(cmd_type), strLen(cmd_type),
                           strPtr(payload), strLen(payload));
}

xmex::Value cluster_query_v(const xmex::Value* name, const xmex::Value* path) {
    return cluster_query(strPtr(name), strLen(name),
                            strPtr(path), strLen(path));
}

xmex::Value cluster_broadcast_v(const xmex::Value* cmd_type, const xmex::Value* payload) {
    return cluster_broadcast(strPtr(cmd_type), strLen(cmd_type),
                                strPtr(payload), strLen(payload));
}

xmex::Value cluster_health_all_v() {
    return cluster_health_all();
}

// ====== KvAdmin 适配器 ======

xmex::Value kv_build_v(const xmex::Value* node_id, const xmex::Value* binary, const xmex::Value* args_json,
                      const xmex::Value* work_dir, const xmex::Value* addr) {
    return kv_build(strPtr(node_id), strLen(node_id),
                       strPtr(binary), strLen(binary),
                       strPtr(args_json), strLen(args_json),
                       strPtr(work_dir), strLen(work_dir),
                       strPtr(addr), strLen(addr));
}

xmex::Value kv_add_v(const xmex::Value* node_id, const xmex::Value* binary, const xmex::Value* args_json,
                    const xmex::Value* work_dir, const xmex::Value* addr) {
    return kv_add(strPtr(node_id), strLen(node_id),
                     strPtr(binary), strLen(binary),
                     strPtr(args_json), strLen(args_json),
                     strPtr(work_dir), strLen(work_dir),
                     strPtr(addr), strLen(addr));
}

xmex::Value kv_erase_v(const xmex::Value* node_id) {
    return kv_erase(strPtr(node_id), strLen(node_id));
}

xmex::Value kv_skip_v(const xmex::Value* node_id) {
    return kv_skip(strPtr(node_id), strLen(node_id));
}

xmex::Value kv_sleep_v(const xmex::Value* node_id) {
    return kv_sleep(strPtr(node_id), strLen(node_id));
}

xmex::Value kv_wakeup_v(const xmex::Value* node_id) {
    return kv_wakeup(strPtr(node_id), strLen(node_id));
}

xmex::Value kv_stop_v(const xmex::Value* node_id) {
    return kv_stop(strPtr(node_id), strLen(node_id));
}

xmex::Value kv_restart_v(const xmex::Value* node_id) {
    return kv_restart(strPtr(node_id), strLen(node_id));
}

xmex::Value kv_status_v(const xmex::Value* node_id) {
    return kv_status(strPtr(node_id), strLen(node_id));
}

xmex::Value kv_list_v() {
    return kv_list();
}

xmex::Value kv_persist_v(const xmex::Value* addr) {
    return kv_persist(strPtr(addr), strLen(addr));
}
xmex::Value kv_alive_v(const xmex::Value* node_id) {
    int64_t r = kv_alive(strPtr(node_id), strLen(node_id));
    return int_(r);
}

xmex::Value kv_bind_sandbox_v(const xmex::Value* node_id, const xmex::Value* handle) {
    int64_t r = kv_bind_sandbox(strPtr(node_id), strLen(node_id), handle->as.i);
    return int_(r);
}

xmex::Value kv_get_sandbox_v(const xmex::Value* node_id) {
    int64_t r = kv_get_sandbox(strPtr(node_id), strLen(node_id));
    return int_(r);
}

xmex::Value kv_unbind_sandbox_v(const xmex::Value* node_id) {
    int64_t r = kv_unbind_sandbox(strPtr(node_id), strLen(node_id));
    return int_(r);
}

// ====== add(box) 编排与容器内定位适配器(2026-10 增补) ======
// box 是实例对象,直接透传指针,不走 strPtr 字符串提取
xmex::Value kv_add_box_v(const xmex::Value* node_id, const xmex::Value* box, const xmex::Value* addr) {
    return kv_add_box(strPtr(node_id), strLen(node_id),
                         box,
                         strPtr(addr), strLen(addr));
}
xmex::Value kv_locate_v(const xmex::Value* box) {
    return kv_locate(box);
}
xmex::Value kv_self_v() {
    return kv_self();
}
// ====== 节点迭代器适配器(2026-10 增补) ======
xmex::Value kv_iter_v() {
    return kv_iter();
}
xmex::Value kv_iter_next_v(const xmex::Value* it) {
    return kv_iter_next(it);
}
xmex::Value kv_iter_rewind_v(const xmex::Value* it) {
    return kv_iter_rewind(it);
}
xmex::Value kv_iter_destroy_v(const xmex::Value* it) {
    return kv_iter_destroy(it);
}
xmex::Value kv_iter_live_v() {
    return kv_iter_live();
}
// ====== 节点内置 MQ 队列适配器(2026-10 增补) ======
// 底层队列接口前置声明(kv_queue_api.cpp)
xmex::Value kv_enqueue(const char*, int64_t, const char*, int64_t);
xmex::Value kv_dequeue(const char*, int64_t, const char*, int64_t);
xmex::Value kv_done(const char*, int64_t);
xmex::Value kv_qlen(const char*, int64_t);
xmex::Value kv_set_quota(const char*, int64_t, const char*, int64_t);
xmex::Value kv_quota(const char*, int64_t);

xmex::Value kv_enqueue_v(const xmex::Value* queue, const xmex::Value* payload) {
    return kv_enqueue(strPtr(queue), strLen(queue),
                         strPtr(payload), strLen(payload));
}
xmex::Value kv_dequeue_v(const xmex::Value* node_id, const xmex::Value* queue) {
    return kv_dequeue(strPtr(node_id), strLen(node_id),
                         strPtr(queue), strLen(queue));
}
xmex::Value kv_done_v(const xmex::Value* node_id) {
    return kv_done(strPtr(node_id), strLen(node_id));
}
xmex::Value kv_qlen_v(const xmex::Value* queue) {
    return kv_qlen(strPtr(queue), strLen(queue));
}
xmex::Value kv_set_quota_v(const xmex::Value* node_id, const xmex::Value* quota) {
    return kv_set_quota(strPtr(node_id), strLen(node_id),
                           strPtr(quota), strLen(quota));
}
xmex::Value kv_quota_v(const xmex::Value* node_id) {
    return kv_quota(strPtr(node_id), strLen(node_id));
}
// ====== 通用 KV 读写与本地文件写入适配器(2026-10 增补) ======
xmex::Value kv_kset(const char*, int64_t, const char*, int64_t);
xmex::Value kv_kget(const char*, int64_t);
xmex::Value file_write(const char*, int64_t, const char*, int64_t);
xmex::Value file_read(const char*, int64_t);
int64_t str_to_int(const char*, int64_t);
xmex::Value int_to_str(int64_t);

xmex::Value kv_kset_v(const xmex::Value* key, const xmex::Value* value) {
    return kv_kset(strPtr(key), strLen(key),
                      strPtr(value), strLen(value));
}
xmex::Value kv_kget_v(const xmex::Value* key) {
    return kv_kget(strPtr(key), strLen(key));
}
xmex::Value file_write_v(const xmex::Value* path, const xmex::Value* content) {
    return file_write(strPtr(path), strLen(path),
                         strPtr(content), strLen(content));
}
xmex::Value file_read_v(const xmex::Value* path) {
    return file_read(strPtr(path), strLen(path));
}
xmex::Value str_to_int_v(const xmex::Value* s) {
    return int_(str_to_int(strPtr(s), strLen(s)));
}
xmex::Value int_to_str_v(const xmex::Value* v) {
    int64_t n = (v && v->tag == xmex::kInt) ? v->as.i : 0;
    return int_to_str(n);
}

}  // extern "C"

#endif  // !_WIN32
