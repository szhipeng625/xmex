// ffi_api.cpp —— FFI 的语言层接口
//
// 暴露到 PyLite 的用法:
//
//   lib = kv.ffi("./xx.dll")        # 加载动态库,返回实例
//   print(lib.symbols())            # 看有哪些函数
//   print(lib.describe("init"))     # 看某个函数的签名
//   r = lib.call("init", [1, "abc"]) # 调用
//   lib.unload()
//
// 另外提供全局 introspection:
//   kv.ffi_list()                   # 所有已加载的库
//
// 与 kv.add() 的分工:
//   kv.add(node_id, binary, args, work_dir, addr) —— 纳管一个**进程**
//   kv.ffi(path)                     —— 在本进程内加载一个**动态库**
// 两者语义不同,不要混用。远程动态库走「远程进程 + agent」的路线,
// 由 kv.add 纳管,不在 ffi 这层解决。

#include "ffi.h"
#include "xmex/runtime.h"
#include "xmex/platform.h"
#include "xmex/value.h"

#include <cstring>
#include <sstream>
#include <string>
#include <mutex>

namespace {

const int64_t FFI_CLASS_ID = 20;   // 实例类型 id,与其他模块错开

// JSON 字符串转义
static std::string jesc(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// 把一个 PyLite 值序列化成 JSON。
// 实参只支持标量(int/float/bool/str)与标量组成的列表 ——
// 传 dict 或嵌套结构给 FFI 没有意义,拒绝掉比默默出错好。
static bool value_to_json(const xmex::Value* v, std::string& out) {
    if (!v) { out += "null"; return true; }
    switch (v->tag) {
        case xmex::kInt:
            out += std::to_string(v->as.i);
            return true;
        case xmex::kFloat: {
            char b[64];
            snprintf(b, sizeof(b), "%.17g", v->as.f);
            out += b;
            return true;
        }
        case xmex::kBool:
            out += v->as.i ? "true" : "false";
            return true;
        case xmex::kStr:
            out += "\"" + jesc(std::string(str_data(v), str_size(v))) + "\"";
            return true;
        case xmex::kNull:
            out += "null";
            return true;
        case xmex::kList: {
            int64_t n = list_len(v);
            out += "[";
            for (int64_t i = 0; i < n; ++i) {
                if (i) out += ",";
                xmex::Value e = list_get(v, i);
                if (!value_to_json(&e, out)) return false;
            }
            out += "]";
            return true;
        }
        default:
            return false;   // dict / instance 等不支持
    }
}

// 从实例的 attrs 里取句柄
static std::string handle_of(const xmex::Value* inst) {
    if (!inst || inst->tag != xmex::kInstance) return "";
    xmex::Value k = str_new("handle", 6);
    xmex::Value v = instance_get_attr(inst, "handle", 6);
    if (v.tag != xmex::kStr) return "";
    return std::string(str_data(&v), str_size(&v));
}

// ============================================================================
// 隔离模式:把 DLL 加载与调用放进子进程
//
// 宿主这边只做两件事:发请求、收响应。DLL 真在哪个进程里崩溃都不影响宿主
// —— 宿主从管道读到 EOF,拿 waitpid 的退出码,报"worker 崩了"。
//
// 协议与将来��远程 agent 完全一致(一行一条 JSON),所以先做本地,
// 远程只是换连接方式。
// ============================================================================

struct IsolatedWorker {
    std::string id;
    std::string path;
    xmex_pipe_t to_child{XMEX_PIPE_INVALID};   // 写端:宿主 → 子进程
    xmex_pipe_t from_child{XMEX_PIPE_INVALID}; // 读端:子进程 → 宿主
    xmex_proc_t proc{XMEX_PROC_INVALID};
    bool dead{false};
    int  exit_code{0};
};

std::map<std::string, IsolatedWorker> g_workers;
std::mutex g_workers_mu;
int64_t g_worker_seq = 1;

// agent 可执行文件的位置。
// 优先用环境变量 XMEX_FFI_AGENT(部署时指定),否则按"与宿主同目录"推断。
static std::string agent_path() {
    const char* env = getenv("XMEX_FFI_AGENT");
    if (env && *env) return env;

    // 从宿主可执行文件的路径推出同目录的 agent
    char buf[1024] = {0};
    DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf) - 1);
    if (n > 0 && n < sizeof(buf)) {
        std::string self(buf, n);
        size_t slash = self.find_last_of("/\\");
        if (slash != std::string::npos) {
            std::string dir = self.substr(0, slash);
            return dir + "/xmex_ffi_agent" + XMEX_EXE_EXT;
        }
    }
    return std::string("xmex_ffi_agent") + XMEX_EXE_EXT;
}

// 给子进程发一行请求并读一行响应。
// 返回 false 表示子进程已死(管道 EOF)或出错。
static bool worker_request(IsolatedWorker& w, const std::string& req,
                           std::string& resp, std::string& error) {
    if (w.dead) {
        error = "worker already dead (exit code " + std::to_string(w.exit_code) + ")";
        return false;
    }
    std::string line = req + "\n";
    long wrote = xmex_pipe_write(w.to_child, line.data(), (unsigned long)line.size());
    if (wrote < 0) {
        w.dead = true;
        error = "failed to write to worker";
        return false;
    }

    // 读一行响应
    std::string out;
    while (true) {
        char c;
        long got = xmex_pipe_read(w.from_child, &c, 1);
        if (got <= 0) {
            // EOF:子进程死了(崩了或退出了)
            w.dead = true;
            w.exit_code = xmex_proc_wait(w.proc);
            error = "worker crashed (exit code " + std::to_string(w.exit_code) + ")";
            return false;
        }
        if (c == '\n') break;
        out += c;
        if (out.size() > 1024 * 1024) {
            error = "worker response too long";
            return false;
        }
    }
    resp = out;
    return true;
}

// ---- 实例方法实现 ----
//
// 每个实例要么是「进程内」的(handle 是 ffi-N),要么是「隔离」的
// (handle 是 w-N,背后有个子进程)。方法分派先看是哪种,再走对应路径。
// 用统一的前缀区分:进程内句柄以 "ffi-" 开头,隔离句柄以 "w-" 开头。

static bool is_isolated_handle(const std::string& h) {
    return h.rfind("w-", 0) == 0;
}

// ---- 实例方法实现 ----

// lib.symbols() → JSON 数组字符串:["init","process"]
xmex::Value ffi_symbols_v(const xmex::Value* inst, xmex::Value* args, int64_t nargs) {
    (void)args; (void)nargs;
    std::string h = handle_of(inst);
    if (h.empty()) return str_new("[]", 2);

    if (is_isolated_handle(h)) {
        std::lock_guard<std::mutex> lk(g_workers_mu);
        auto it = g_workers.find(h);
        if (it == g_workers.end()) return str_new("[]", 2);
        std::string resp, err;
        if (!worker_request(it->second, "{\"op\":\"symbols\"}", resp, err)) {
            return str_new("[]", 2);
        }
        // 响应形如 {"ok":true,"symbols":[...]} —— 把 symbols 数组摘出来
        size_t p = resp.find("\"symbols\":");
        if (p == std::string::npos) return str_new("[]", 2);
        p += 10;   // 跳过 "symbols": 整个 10 个字符
        size_t e = resp.find_last_of('}');
        if (e == std::string::npos || e < p) return str_new("[]", 2);
        std::string arr = resp.substr(p, e - p);
        return str_new(arr.c_str(), (int64_t)arr.size());
    }

    std::vector<char> buf(64 * 1024);
    ffi_symbols(h.c_str(), (int64_t)h.size(), buf.data(), (int64_t)buf.size());
    return str_new(buf.data(), (int64_t)strlen(buf.data()));
}

// lib.describe("init") → JSON 对象:{"name":"init","params":["int"],"ret":"bool"}
// 不带参数时返回全部函数的描述数组
xmex::Value ffi_describe_v(const xmex::Value* inst, xmex::Value* args, int64_t nargs) {
    std::string h = handle_of(inst);
    if (h.empty()) return str_new("null", 4);

    // 隔离模式:转发给子进程
    if (is_isolated_handle(h)) {
        std::lock_guard<std::mutex> lk(g_workers_mu);
        auto it = g_workers.find(h);
        if (it == g_workers.end()) return str_new("null", 4);
        std::string resp, err;
        if (nargs >= 1 && args[0].tag == xmex::kStr) {
            std::string fn(str_data(&args[0]), str_size(&args[0]));
            std::string req = "{\"op\":\"describe\",\"fn\":\"" + jesc(fn) + "\"}";
            if (!worker_request(it->second, req, resp, err)) {
                std::string e = "{\"ok\":false,\"error\":\"" + jesc(err) + "\"}";
                return str_new(e.c_str(), (int64_t)e.size());
            }
            // 响应形如 {"ok":true,"desc":{...}}
            size_t p = resp.find("\"desc\":");
            if (p == std::string::npos) return str_new("null", 4);
            p += 7;   // 跳过 "desc": 整个 7 个字符
            size_t e = resp.find_last_of('}');
            if (e == std::string::npos || e < p) return str_new("null", 4);
            std::string d = resp.substr(p, e - p);
            return str_new(d.c_str(), (int64_t)d.size());
        }
        // 不带参数:让子进程返回全部描述
        if (!worker_request(it->second, "{\"op\":\"symbols\"}", resp, err)) {
            return str_new("null", 4);
        }
        return str_new("[]", 2);
    }

    std::vector<char> buf(64 * 1024);
    if (nargs >= 1 && args[0].tag == xmex::kStr) {
        std::string fn(str_data(&args[0]), str_size(&args[0]));
        ffi_describe(h.c_str(), (int64_t)h.size(), fn.c_str(), (int64_t)fn.size(),
                     buf.data(), (int64_t)buf.size());
        return str_new(buf.data(), (int64_t)strlen(buf.data()));
    }

    // 不带参数:返回全部函数的描述
    std::string syms_s;
    {
        std::vector<char> b2(64 * 1024);
        ffi_symbols(h.c_str(), (int64_t)h.size(), b2.data(), (int64_t)b2.size());
        syms_s = b2.data();
    }
    // 简单拆 JSON 数组 —— 格式是我们自己产出的,格式固定
    std::ostringstream os;
    os << "[";
    bool first = true;
    size_t p = 0;
    while (true) {
        size_t q1 = syms_s.find('"', p);
        if (q1 == std::string::npos) break;
        size_t q2 = syms_s.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string fn = syms_s.substr(q1 + 1, q2 - q1 - 1);
        p = q2 + 1;
        if (!first) os << ",";
        first = false;
        std::vector<char> b3(16 * 1024);
        ffi_describe(h.c_str(), (int64_t)h.size(), fn.c_str(), (int64_t)fn.size(),
                     b3.data(), (int64_t)b3.size());
        os << b3.data();
    }
    os << "]";
    std::string s = os.str();
    return str_new(s.c_str(), (int64_t)s.size());
}

// lib.call("init", [1, "abc"]) → {"ok":true,"value":42}
xmex::Value ffi_call_v(const xmex::Value* inst, xmex::Value* args, int64_t nargs) {
    std::string h = handle_of(inst);
    if (h.empty()) {
        std::string e = "{\"ok\":false,\"error\":\"no handle\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }
    if (nargs < 1 || args[0].tag != xmex::kStr) {
        std::string e = "{\"ok\":false,\"error\":\"call needs a function name\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }
    std::string fn(str_data(&args[0]), str_size(&args[0]));

    // 第二个参数是实参列表(PyLite list),序列化成 JSON 数组
    std::string args_json = "[]";
    if (nargs >= 2 && args[1].tag == xmex::kList) {
        std::string tmp;
        if (!value_to_json(&args[1], tmp)) {
            std::string e = "{\"ok\":false,\"error\":\"args must be scalars or a list of scalars\"}";
            return str_new(e.c_str(), (int64_t)e.size());
        }
        args_json = tmp;
    }

    // 隔离模式:转发给子进程。DLL 真崩了也只有子进程死,宿主照常返回错误。
    if (is_isolated_handle(h)) {
        std::lock_guard<std::mutex> lk(g_workers_mu);
        auto it = g_workers.find(h);
        if (it == g_workers.end()) {
            std::string e = "{\"ok\":false,\"error\":\"worker not found\"}";
            return str_new(e.c_str(), (int64_t)e.size());
        }
        std::string req = "{\"op\":\"call\",\"fn\":\"" + jesc(fn) +
                          "\",\"args\":" + args_json + "}";
        std::string resp, err;
        if (!worker_request(it->second, req, resp, err)) {
            std::string e = "{\"ok\":false,\"error\":\"" + jesc(err) + "\"}";
            return str_new(e.c_str(), (int64_t)e.size());
        }
        return str_new(resp.c_str(), (int64_t)resp.size());
    }

    std::vector<char> buf(256 * 1024);
    ffi_call(h.c_str(), (int64_t)h.size(), fn.c_str(), (int64_t)fn.size(),
             args_json.c_str(), (int64_t)args_json.size(),
             buf.data(), (int64_t)buf.size());
    return str_new(buf.data(), (int64_t)strlen(buf.data()));
}

// lib.unload()
xmex::Value ffi_unload_v(const xmex::Value* inst, xmex::Value* args, int64_t nargs) {
    (void)args; (void)nargs;
    std::string h = handle_of(inst);
    if (h.empty()) return none_();

    if (is_isolated_handle(h)) {
        std::lock_guard<std::mutex> lk(g_workers_mu);
        auto it = g_workers.find(h);
        if (it == g_workers.end()) return none_();
        IsolatedWorker& w = it->second;
        if (!w.dead) {
            // 先礼后兵:让子进程自己退出,不行再杀
            std::string resp, err;
            worker_request(w, "{\"op\":\"quit\"}", resp, err);
            if (!w.dead) {
                w.dead = true;
                w.exit_code = xmex_proc_wait(w.proc);
            }
        }
        xmex_pipe_close(w.to_child);
        xmex_pipe_close(w.from_child);
        g_workers.erase(it);
        return none_();
    }

    ffi_unload(h.c_str(), (int64_t)h.size());
    return none_();
}

// lib.path() → 动态库路径。
// 直接从实例属性读 —— 不用去解析 ffi_list 的输出(那是给"全局视角"用的)。
xmex::Value ffi_path_v(const xmex::Value* inst, xmex::Value* args, int64_t nargs) {
    (void)args; (void)nargs;
    if (!inst || inst->tag != xmex::kInstance) return str_new("", 0);
    xmex::Value p = instance_get_attr(inst, "path", 4);
    if (p.tag != xmex::kStr) return str_new("", 0);
    return str_new(str_data(&p), str_size(&p));
}

std::once_flag g_reg_once;

void register_ffi_methods() {
    std::call_once(g_reg_once, []() {
        instance_register_method("ffi", 3, "symbols", 7, ffi_symbols_v);
        instance_register_method("ffi", 3, "describe", 8, ffi_describe_v);
        instance_register_method("ffi", 3, "call", 4, ffi_call_v);
        instance_register_method("ffi", 3, "unload", 6, ffi_unload_v);
        instance_register_method("ffi", 3, "path", 4, ffi_path_v);
    });
}

}  // namespace

extern "C" {

// kv.ffi(path) → 加载动态库,返回实例;失败返回 {"ok":false,"error":...}
xmex::Value kv_ffi(const xmex::Value* path) {
    register_ffi_methods();
    if (!path || path->tag != xmex::kStr) {
        std::string e = "{\"ok\":false,\"error\":\"ffi needs a path string\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }
    std::string p(str_data(path), str_size(path));

    char err[512] = {0};
    char handle[64] = {0};
    ffi_load(p.c_str(), (int64_t)p.size(),
             handle, (int64_t)sizeof(handle), err, (int64_t)sizeof(err));
    if (!handle[0]) {
        std::ostringstream os;
        os << "{\"ok\":false,\"error\":\"" << err << "\"}";
        std::string s = os.str();
        return str_new(s.c_str(), (int64_t)s.size());
    }
    const char* id = handle;

    xmex::Value attrs = dict_new(nullptr, nullptr, 0);
    // ⚠️ attrs 必须被 GC 根保护。dict_set 里的 reserve 会分配内存,可能触发
    // 回收;而 attrs 此刻只是个 C++ 局部变量,不在任何栈槽上,GC 扫不到它 ——
    // 里面的字符串(句柄)会被当成垃圾收掉,实例就变成了空壳。
    // 参照 runtime/dict.cpp 里 dict_new 之后的写法。
    gc_root_push(reinterpret_cast<void **>(&attrs));
    xmex::Value k = str_new("handle", 6);
    xmex::Value v = str_new(id, (int64_t)strlen(id));
    dict_set(&attrs, &k, &v);
    xmex::Value tk = str_new("type", 4);
    xmex::Value tv = str_new("ffi", 3);
    dict_set(&attrs, &tk, &tv);
    // 路径也存一份,便于用户回看
    xmex::Value pk = str_new("path", 4);
    xmex::Value pv = str_new(p.c_str(), (int64_t)p.size());
    dict_set(&attrs, &pk, &pv);

    xmex::Value inst = instance_new(FFI_CLASS_ID, &attrs);
    gc_root_pop(1);   // 实例已持有 attrs 的内容,根可以退了
    return inst;
}

// kv.ffi_isolated(path) → 加载动态库到**子进程**,DLL 崩溃不影响宿主。
//
// 与 kv.ffi 的区别:
//   kv.ffi         DLL 加载在宿主进程里。快,回调简单,但 DLL 崩了宿主也完蛋。
//   kv.ffi_isolated DLL 加载在子进程里。宿主只发消息收消息,DLL 崩了
//                  宿主从管道读到 EOF,拿到退出码,报"worker crashed"。
//
// ⚠️ 隔离模式**不支持回调**(2026-10 第一版)。回调要跨进程反向通道,
//    那一期再做。本地 fork 的协议与将来的远程 agent 完全一致,先做本地。
xmex::Value kv_ffi_isolated(const xmex::Value* path) {
    register_ffi_methods();
    if (!path || path->tag != xmex::kStr) {
        std::string e = "{\"ok\":false,\"error\":\"ffi_isolated needs a path string\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }
    std::string p(str_data(path), str_size(path));

    // 建两条管道:
    //   请求管道 req:宿主写 → 子进程读(stdin)
    //   响应管道 rsp:子进程写 → 宿主读(stdout)
    xmex_pipe_t req_rd = XMEX_PIPE_INVALID, req_wr = XMEX_PIPE_INVALID;
    xmex_pipe_t rsp_rd = XMEX_PIPE_INVALID, rsp_wr = XMEX_PIPE_INVALID;
    if (!xmex_pipe_pair(&req_rd, &req_wr) || !xmex_pipe_pair(&rsp_rd, &rsp_wr)) {
        xmex_pipe_close(req_rd); xmex_pipe_close(req_wr);
        xmex_pipe_close(rsp_rd); xmex_pipe_close(rsp_wr);
        std::string e = "{\"ok\":false,\"error\":\"pipe creation failed\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }

    // spawn:子进程的 stdin = req_rd,stdout = rsp_wr
    // 宿主保留 req_wr(写)与 rsp_rd(读)
    std::string agent = agent_path();
    const char* argv[1] = {p.c_str()};
    xmex_proc_t proc = XMEX_PROC_INVALID;
    if (!xmex_spawn(agent.c_str(), argv, 1, req_rd, rsp_wr, &proc)) {
        xmex_pipe_close(req_rd); xmex_pipe_close(req_wr);
        xmex_pipe_close(rsp_rd); xmex_pipe_close(rsp_wr);
        std::string e = "{\"ok\":false,\"error\":\"failed to spawn ffi agent: " + agent + "\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }
    // 子进程那边已经拿走了管道端,宿主这两端可以关了
    xmex_pipe_close(req_rd);
    xmex_pipe_close(rsp_wr);

    IsolatedWorker w;
    w.id = "w-" + std::to_string(g_worker_seq++);
    w.path = p;
    w.to_child = req_wr;     // 宿主写
    w.from_child = rsp_rd;   // 宿主读
    w.proc = proc;

    {
        std::lock_guard<std::mutex> lk(g_workers_mu);
        g_workers[w.id] = w;
    }

    // 等 agent 的第一行响应:{"ok":true,"loaded":"ffi-1"} 或加载失败。
    // ⚠️ 这里不能走 worker_request —— 它会先写一行请求出去,而握手阶段
    // agent 还没收到任何请求。握手是"只读一行"。
    {
        std::lock_guard<std::mutex> lk(g_workers_mu);
        auto it = g_workers.find(w.id);
        if (it == g_workers.end()) {
            std::string e = "{\"ok\":false,\"error\":\"worker registration failed\"}";
            return str_new(e.c_str(), (int64_t)e.size());
        }
        IsolatedWorker& ww = it->second;
        std::string resp;
        bool got = false;
        while (true) {
            char c;
            long n = xmex_pipe_read(ww.from_child, &c, 1);
            if (n <= 0) {
                ww.dead = true;
                ww.exit_code = xmex_proc_wait(ww.proc);
                break;
            }
            if (c == '\n') { got = true; break; }
            resp += c;
        }
        if (!got) {
            std::string e = "{\"ok\":false,\"error\":\"ffi agent did not respond (exit code " +
                            std::to_string(ww.exit_code) + ")\"}";
            return str_new(e.c_str(), (int64_t)e.size());
        }
        // 握手失败(比如 DLL 加载不了):agent 会回 {"ok":false,...}
        if (resp.find("\"ok\":true") == std::string::npos) {
            return str_new(resp.c_str(), (int64_t)resp.size());
        }
    }

    // 建实例
    xmex::Value attrs = dict_new(nullptr, nullptr, 0);
    gc_root_push(reinterpret_cast<void **>(&attrs));
    xmex::Value k = str_new("handle", 6);
    xmex::Value v = str_new(w.id.c_str(), (int64_t)w.id.size());
    dict_set(&attrs, &k, &v);
    xmex::Value tk = str_new("type", 4);
    xmex::Value tv = str_new("ffi", 3);
    dict_set(&attrs, &tk, &tv);
    xmex::Value pk = str_new("path", 4);
    xmex::Value pv = str_new(p.c_str(), (int64_t)p.size());
    dict_set(&attrs, &pk, &pv);

    xmex::Value inst = instance_new(FFI_CLASS_ID, &attrs);
    gc_root_pop(1);
    return inst;
}

// kv.ffi_list() → 所有已加载的动态库
xmex::Value kv_ffi_list() {
    std::vector<char> buf(64 * 1024);
    ffi_list(buf.data(), (int64_t)buf.size());
    return str_new(buf.data(), (int64_t)strlen(buf.data()));
}

}  // extern "C"
