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

// ---- 实例方法实现 ----

// lib.symbols() → JSON 数组字符串:["init","process"]
xmex::Value ffi_symbols_v(const xmex::Value* inst, xmex::Value* args, int64_t nargs) {
    (void)args; (void)nargs;
    std::string h = handle_of(inst);
    if (h.empty()) return str_new("[]", 2);
    std::vector<char> buf(64 * 1024);
    ffi_symbols(h.c_str(), (int64_t)h.size(), buf.data(), (int64_t)buf.size());
    return str_new(buf.data(), (int64_t)strlen(buf.data()));
}

// lib.describe("init") → JSON 对象:{"name":"init","params":["int"],"ret":"bool"}
// 不带参数时返回全部函数的描述数组
xmex::Value ffi_describe_v(const xmex::Value* inst, xmex::Value* args, int64_t nargs) {
    std::string h = handle_of(inst);
    if (h.empty()) return str_new("null", 4);

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
    ffi_unload(h.c_str(), (int64_t)h.size());
    return none_();
}

// lib.path() → 动态库路径
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

// kv.ffi_list() → 所有已加载的动态库
xmex::Value kv_ffi_list() {
    std::vector<char> buf(64 * 1024);
    ffi_list(buf.data(), (int64_t)buf.size());
    return str_new(buf.data(), (int64_t)strlen(buf.data()));
}

}  // extern "C"
