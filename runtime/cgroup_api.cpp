#include "cgroup.h"
#include "../include/pylite/runtime.h"
#include <cstring>

extern "C" {

lite::Value cgroup_create_judge(const char *task_id, int64_t taskIdLen,
                               uint32_t cpu_limit_ms, uint64_t mem_limit_bytes,
                               int64_t max_pids) {
    lite::Value result;
    memset(&result, 0, sizeof(result));

    try {
        std::string tid(task_id, taskIdLen);
        std::string cg_path = CgroupManager::create_judge(tid, cpu_limit_ms, mem_limit_bytes, max_pids);

        // 返回 cgroup 路径字符串
        result = str_new(cg_path.c_str(), cg_path.size());
    } catch (const std::exception& e) {
        // 出错时返回空字符串
        result = str_new("", 0);
    }

    return result;
}

void cgroup_attach(const char *cg_path, int64_t pathLen, int64_t pid) {
    try {
        std::string path(cg_path, pathLen);
        CgroupManager::attach_pid(path, static_cast<pid_t>(pid));
    } catch (const std::exception&) {
        // 静默失败
    }
}

int64_t cgroup_read_cpu_ns(const char *cg_path, int64_t pathLen) {
    try {
        std::string path(cg_path, pathLen);
        auto usage = CgroupManager::read_usage(path);
        return static_cast<int64_t>(usage.cpu_ns);
    } catch (const std::exception&) {
        return -1;
    }
}

int64_t cgroup_read_mem_bytes(const char *cg_path, int64_t pathLen) {
    try {
        std::string path(cg_path, pathLen);
        auto usage = CgroupManager::read_usage(path);
        return static_cast<int64_t>(usage.mem_bytes);
    } catch (const std::exception&) {
        return -1;
    }
}

void cgroup_destroy(const char *cg_path, int64_t pathLen) {
    try {
        std::string path(cg_path, pathLen);
        CgroupManager::destroy(path);
    } catch (const std::exception&) {
        // 静默失败
    }
}

}  // extern "C"
