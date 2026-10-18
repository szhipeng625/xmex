#include "sysmon.h"
#include "../include/xmex/runtime.h"
#include <cstring>
#include <sstream>

extern "C" {

// ============================================================================
// CPU 监控 API
// ============================================================================

// 返回 CPU 统计信息的 JSON 字符串
xmex::Value sysmon_cpu_stats() {
    auto stats = read_cpu_stats();
    std::ostringstream json;
    json << "{";
    json << "\"num_cores\":" << stats.num_cores << ",";
    json << "\"usage_percent\":" << stats.usage_percent << ",";
    json << "\"total\":{";
    json << "\"user\":" << stats.total.user << ",";
    json << "\"system\":" << stats.total.system << ",";
    json << "\"idle\":" << stats.total.idle << ",";
    json << "\"iowait\":" << stats.total.iowait;
    json << "},\"cores\":[";
    for (size_t i = 0; i < stats.cores.size(); ++i) {
        if (i > 0) json << ",";
        json << "{\"id\":" << i
             << ",\"user\":" << stats.cores[i].user
             << ",\"system\":" << stats.cores[i].system
             << ",\"idle\":" << stats.cores[i].idle
             << ",\"iowait\":" << stats.cores[i].iowait << "}";
    }
    json << "]}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// 获取指定进程的 CPU 使用时间（毫秒）
int64_t sysmon_process_cpu_ms(int64_t pid) {
    return static_cast<int64_t>(read_process_cpu_percent(static_cast<pid_t>(pid)));
}

// ============================================================================
// 内存监控 API
// ============================================================================

// 返回系统内存信息的 JSON 字符串
xmex::Value sysmon_mem_info() {
    auto info = read_mem_info();
    std::ostringstream json;
    json << "{";
    json << "\"total_kb\":" << info.total_kb << ",";
    json << "\"free_kb\":" << info.free_kb << ",";
    json << "\"available_kb\":" << info.available_kb << ",";
    json << "\"buffers_kb\":" << info.buffers_kb << ",";
    json << "\"cached_kb\":" << info.cached_kb << ",";
    json << "\"swap_total_kb\":" << info.swap_total_kb << ",";
    json << "\"swap_free_kb\":" << info.swap_free_kb << ",";
    json << "\"usage_percent\":" << info.usage_percent;
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// 获取指定进程的内存使用（RSS，KB）
int64_t sysmon_process_mem_kb(int64_t pid) {
    auto info = read_process_mem(static_cast<pid_t>(pid));
    return static_cast<int64_t>(info.vm_rss_kb);
}

// 获取指定进程的详细内存信息 JSON
xmex::Value sysmon_process_mem_info(int64_t pid) {
    auto info = read_process_mem(static_cast<pid_t>(pid));
    std::ostringstream json;
    json << "{";
    json << "\"vm_peak_kb\":" << info.vm_peak_kb << ",";
    json << "\"vm_size_kb\":" << info.vm_size_kb << ",";
    json << "\"vm_rss_kb\":" << info.vm_rss_kb << ",";
    json << "\"vm_data_kb\":" << info.vm_data_kb << ",";
    json << "\"vm_stack_kb\":" << info.vm_stack_kb;
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// ============================================================================
// 进程监控 API
// ============================================================================

// 获取所有进程 PID 列表（JSON 数组）
xmex::Value sysmon_list_processes() {
    auto pids = list_processes();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i > 0) json << ",";
        json << pids[i];
    }
    json << "]";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// 获取指定进程的详细信息 JSON
xmex::Value sysmon_process_info(int64_t pid) {
    auto info = read_process_info(static_cast<pid_t>(pid));
    std::ostringstream json;
    json << "{";
    json << "\"pid\":" << info.pid << ",";
    json << "\"ppid\":" << info.ppid << ",";
    json << "\"name\":\"" << info.name << "\",";
    json << "\"state\":\"" << info.state << "\",";
    json << "\"cpu_time_ns\":" << info.cpu_time_ns << ",";
    json << "\"mem_rss_kb\":" << info.mem_rss_kb << ",";
    json << "\"num_threads\":" << info.num_threads << ",";
    json << "\"children\":[";
    for (size_t i = 0; i < info.children.size(); ++i) {
        if (i > 0) json << ",";
        json << info.children[i];
    }
    json << "]}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// 获取进程树的总资源用量 JSON
xmex::Value sysmon_tree_usage(int64_t root_pid) {
    auto usage = read_tree_usage(static_cast<pid_t>(root_pid));
    std::ostringstream json;
    json << "{";
    json << "\"total_cpu_ns\":" << usage.total_cpu_ns << ",";
    json << "\"total_mem_kb\":" << usage.total_mem_kb << ",";
    json << "\"total_pids\":" << usage.total_pids;
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// ============================================================================
// 网络监控 API
// ============================================================================

// 返回网络接口统计 JSON
xmex::Value sysmon_net_stats() {
    auto devs = read_net_stats();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < devs.size(); ++i) {
        if (i > 0) json << ",";
        json << "{";
        json << "\"name\":\"" << devs[i].name << "\",";
        json << "\"rx_bytes\":" << devs[i].rx_bytes << ",";
        json << "\"tx_bytes\":" << devs[i].tx_bytes << ",";
        json << "\"rx_packets\":" << devs[i].rx_packets << ",";
        json << "\"tx_packets\":" << devs[i].tx_packets;
        json << "}";
    }
    json << "]";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// ============================================================================
// 文件系统监控 API
// ============================================================================

// 返回文件系统配额 JSON
xmex::Value sysmon_fs_quota(const char *path, int64_t pathLen) {
    std::string p(path, pathLen);
    auto quota = read_fs_quota(p);
    std::ostringstream json;
    json << "{";
    json << "\"mount_point\":\"" << quota.mount_point << "\",";
    json << "\"total_bytes\":" << quota.total_bytes << ",";
    json << "\"used_bytes\":" << quota.used_bytes << ",";
    json << "\"available_bytes\":" << quota.available_bytes << ",";
    json << "\"inodes_total\":" << quota.inodes_total << ",";
    json << "\"inodes_used\":" << quota.inodes_used << ",";
    json << "\"usage_percent\":" << quota.usage_percent;
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// 返回目录总大小（字节）
int64_t sysmon_dir_size(const char *path, int64_t pathLen) {
    std::string p(path, pathLen);
    return static_cast<int64_t>(dir_total_size(p));
}

// 返回目录文件数量
int64_t sysmon_dir_file_count(const char *path, int64_t pathLen) {
    std::string p(path, pathLen);
    return dir_file_count(p);
}

// ============================================================================
// 系统信息 API
// ============================================================================

// 返回系统信息 JSON
xmex::Value sysmon_sys_info() {
    auto info = read_sys_info();
    std::ostringstream json;
    json << "{";
    json << "\"hostname\":\"" << info.hostname << "\",";
    json << "\"kernel_version\":\"" << info.kernel_version << "\",";
    json << "\"os_release\":\"" << info.os_release << "\",";
    json << "\"uptime_seconds\":" << info.uptime_seconds << ",";
    json << "\"num_cpus\":" << info.num_cpus << ",";
    json << "\"total_mem_kb\":" << info.total_mem_kb;
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

// 返回系统负载
xmex::Value sysmon_load_avg() {
    std::ostringstream json;
    json << "{";
    json << "\"load_1min\":" << read_load_avg_1min() << ",";
    json << "\"load_5min\":" << read_load_avg_5min() << ",";
    json << "\"load_15min\":" << read_load_avg_15min();
    json << "}";
    std::string s = json.str();
    return str_new(s.c_str(), s.size());
}

}  // extern "C"
