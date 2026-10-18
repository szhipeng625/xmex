// plugin module: external C++ project plugin access
// flow: project writes pylite_plugin.pys -> plugin.scan -> plugin.register
//       (compile + bind as distributed node) -> plugin.call
#include "pylite/runtime.h"
#include "pylite/value.h"
#include "kv_admin.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
struct PluginInfo {
    std::string name;
    std::string dir;
    std::string binary;
    std::string node_id;
    std::string addr;
    std::string status{"discovered"};
    std::string error;
};
static std::map<std::string, PluginInfo> g_plugins;
static std::mutex g_mu;

static int run_cmd(const std::string& cmd, std::string* out) {
    std::string full = cmd + " 2>&1";
    FILE* fp = popen(full.c_str(), "r");
    if (!fp) return -1;
    char buf[512];
    if (out) { out->clear(); while (fgets(buf, sizeof(buf), fp)) out->append(buf); }
    else { while (fgets(buf, sizeof(buf), fp)) {} }
    return pclose(fp);
}
}  // namespace

extern "C" {

static lite::Value jstr(const std::string& s) { return str_new(s.c_str(), s.size()); }

// plugin.scan(dir) —— 在指定目录下发现 pylite_plugin.pys 注册文件
lite::Value plugin_scan(const lite::Value* dir) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string d = (dir && dir->tag == lite::kStr) ? std::string(str_data(dir), str_size(dir)) : ".";
    std::string pys = d + "/pylite_plugin.pys";
    std::ifstream ifs(pys);
    bool found = ifs.good();
    std::ostringstream oss;
    oss << "{\"dir\":\"" << d << "\",\"found\":" << (found ? "true" : "false");
    if (found) {
        PluginInfo& p = g_plugins[d];
        p.dir = d;
        p.status = "discovered";
        oss << ",\"registered\":true";
    }
    oss << "}";
    return jstr(oss.str());
}

// 在文本中提取 key: "value" 形式的值
static std::string pick(const std::string& text, const std::string& key) {
    std::string pat = key + " = \"";
    size_t p = text.find(pat);
    if (p == std::string::npos) {
        pat = key + ": \"";
        p = text.find(pat);
    }
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = text.find('"', p);
    if (e == std::string::npos) return "";
    return text.substr(p, e - p);
}

// plugin.register(dir, addr) —— 读取注册文件,编译项目,绑定为分布式节点
lite::Value plugin_register(const lite::Value* dir, const lite::Value* addr) {
    std::string d = (dir && dir->tag == lite::kStr) ? std::string(str_data(dir), str_size(dir)) : ".";
    std::string a = (addr && addr->tag == lite::kStr) ? std::string(str_data(addr), str_size(addr)) : "127.0.0.1:19100";
    std::string pys = d + "/pylite_plugin.pys";
    std::string text((std::istreambuf_iterator<char>(*(new std::ifstream(pys)))),
                     std::istreambuf_iterator<char>());
    std::string build = pick(text, "build");
    std::string binary = pick(text, "binary");
    std::string name = pick(text, "name");
    std::string error;
    PluginInfo* p = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        p = &g_plugins[d];
        p->dir = d;
        p->addr = a;
        p->node_id = name.empty() ? ("plugin-" + d) : ("plugin-" + name);
    }
    // 1. 执行项目自带构建命令(如 make 或 g++ ...)
    if (!build.empty()) {
        std::string out;
        int rc = run_cmd("cd " + d + " && " + build, &out);
        if (rc != 0) {
            std::lock_guard<std::mutex> lk(g_mu);
            p->status = "error";
            p->error = "build failed: " + out.substr(0, out.size() < 300 ? out.size() : 300);
            return jstr("{\"ok\":false,\"stage\":\"build\",\"error\":" + p->error + "}");
        }
    }
    // 2. 绑定编译产物为分布式节点(交给现有 kv 节点管理器托管)
    if (!binary.empty()) {
        std::string err;
        bool ok = KvAdmin::instance().build(p->node_id, d + "/" + binary, {}, d, a, err);
        std::lock_guard<std::mutex> lk(g_mu);
        if (ok) {
            p->binary = binary;
            p->status = "bound";
        } else {
            p->status = "error";
            p->error = err;
            return jstr("{\"ok\":false,\"stage\":\"bind\",\"error\":\"" + err + "\"}");
        }
    } else {
        std::lock_guard<std::mutex> lk(g_mu);
        p->status = "registered";
    }
    std::ostringstream oss;
    oss << "{\"ok\":true,\"node_id\":\"" << p->node_id << "\",\"status\":\"" << p->status << "\"}";
    return jstr(oss.str());
}

// plugin.call(dir, fn_name, args_json) —— 调用已绑定插件项目的接口
// 通过节点对应的项目二进制,以命令行方式执行: <binary> <fn_name> <args_json>
// 项目侧约定: argv[1] 为函数名, argv[2] 为 JSON 参数串, 结果输出到 stdout
lite::Value plugin_call(const lite::Value* dir, const lite::Value* fn, const lite::Value* args) {
    std::string d = (dir && dir->tag == lite::kStr) ? std::string(str_data(dir), str_size(dir)) : ".";
    std::string f = (fn && fn->tag == lite::kStr) ? std::string(str_data(fn), str_size(fn)) : "";
    std::string a = (args && args->tag == lite::kStr) ? std::string(str_data(args), str_size(args)) : "{}";
    std::string binary;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_plugins.find(d);
        if (it == g_plugins.end() || it->second.binary.empty()) {
            return jstr("{\"ok\":false,\"error\":\"plugin not registered or no binary\"}");
        }
        binary = it->second.binary;
    }
    std::string out;
    std::string cmd = "cd " + d + " && ./" + binary + " '" + f + "' '" + a + "'";
    int rc = run_cmd(cmd, &out);
    std::ostringstream oss;
    oss << "{\"ok\":" << (rc == 0 ? "true" : "false") << ",\"exit_code\":" << rc << ",\"output\":\"";
    for (char c : out) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else if (c == '\n') oss << "\\n";
        else if (c == '\r') {}
        else oss << c;
    }
    oss << "\"}";
    return jstr(oss.str());
}

// plugin.list() —— 列出全部已发现/注册的插件
lite::Value plugin_list() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const auto& kv : g_plugins) {
        if (!first) oss << ",";
        first = false;
        const PluginInfo& p = kv.second;
        oss << "{\"dir\":\"" << p.dir << "\",\"node_id\":\"" << p.node_id
            << "\",\"status\":\"" << p.status << "\",\"binary\":\"" << p.binary << "\"}";
    }
    oss << "]";
    return jstr(oss.str());
}

}  // extern "C"

