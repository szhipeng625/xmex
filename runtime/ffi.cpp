// ffi.cpp —— 动态库 FFI 实现
//
// 职责:
//   1. dlopen/LoadLibrary 加载动态库,按路径索引
//   2. 读取描述符(xmex_ffi_describe),解析出函数名与签名
//   3. 按固定调用约定 int64_t fn(int64_t*, int64_t, int64_t*) 调用
//   4. 提供 introspection:列出库里有哪些函数、单个函数的签名
//
// 设计取舍:
//   - 不做通用 JSON 解析器。描述符格式是我们自己定的,专用解析更快也
//     更少出错。项目里现有的 JSON 处理(如 cluster.cpp 的 extractJsonString)
//     都是"按 key 抠字符串",不是解析器。
//   - 参数按描述符声明的类型编码。int/bool 直接传值;float 传 double 的
//     位模式;str 传本次调用期间有效的缓冲指针。
//   - 描述符缺失时退化为"裸调用"模式:调用方自己给函数名,参数一律
//     当 int64 传。这样连不想写描述符的简单库也能用。

#include "ffi.h"
#include "xmex/platform.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <algorithm>
#include "xmex/smartptr.h"

namespace {

// ---- 最小 JSON 值解析(只支持描述符需要的形态) ----

struct JsonValue;
using JsonPtr = xmex::SharedPtr<JsonValue>;

struct JsonValue {
    enum Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = kNull;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JsonPtr> arr;
    std::vector<std::pair<std::string, JsonPtr>> obj;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    JsonPtr parse() {
        skip_ws();
        JsonPtr v = parse_value();
        return v;
    }

private:
    const std::string& s_;
    size_t p_ = 0;

    void skip_ws() {
        while (p_ < s_.size() &&
               (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) {
            ++p_;
        }
    }

    bool eat(char c) {
        skip_ws();
        if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; }
        return false;
    }

    JsonPtr parse_value() {
        skip_ws();
        if (p_ >= s_.size()) return nullptr;
        char c = s_[p_];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') {
            auto v = xmex::makeShared<JsonValue>();
            v->kind = JsonValue::kString;
            v->str = parse_string();
            return v;
        }
        if (s_.compare(p_, 4, "true") == 0) {
            p_ += 4;
            auto v = xmex::makeShared<JsonValue>();
            v->kind = JsonValue::kBool; v->b = true;
            return v;
        }
        if (s_.compare(p_, 5, "false") == 0) {
            p_ += 5;
            auto v = xmex::makeShared<JsonValue>();
            v->kind = JsonValue::kBool; v->b = false;
            return v;
        }
        if (s_.compare(p_, 4, "null") == 0) {
            p_ += 4;
            return xmex::makeShared<JsonValue>();
        }
        return parse_number();
    }

    JsonPtr parse_number() {
        size_t start = p_;
        while (p_ < s_.size() && (isdigit((unsigned char)s_[p_]) || s_[p_] == '-' ||
                                  s_[p_] == '+' || s_[p_] == '.' || s_[p_] == 'e' ||
                                  s_[p_] == 'E')) {
            ++p_;
        }
        auto v = xmex::makeShared<JsonValue>();
        v->kind = JsonValue::kNumber;
        v->num = atof(s_.substr(start, p_ - start).c_str());
        return v;
    }

    std::string parse_string() {
        std::string out;
        if (p_ >= s_.size() || s_[p_] != '"') return out;
        ++p_;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\' && p_ < s_.size()) {
                char e = s_[p_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        // \uXXXX → UTF-8
                        if (p_ + 4 <= s_.size()) {
                            unsigned cp = (unsigned)strtoul(s_.substr(p_, 4).c_str(), nullptr, 16);
                            p_ += 4;
                            if (cp < 0x80) {
                                out += (char)cp;
                            } else if (cp < 0x800) {
                                out += (char)(0xC0 | (cp >> 6));
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xE0 | (cp >> 12));
                                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        if (p_ < s_.size()) ++p_;   // 收尾引号
        return out;
    }

    JsonPtr parse_array() {
        auto v = xmex::makeShared<JsonValue>();
        v->kind = JsonValue::kArray;
        eat('[');
        skip_ws();
        if (eat(']')) return v;
        while (p_ < s_.size()) {
            JsonPtr item = parse_value();
            if (item) v->arr.push_back(item);
            skip_ws();
            if (eat(',')) continue;
            eat(']');
            break;
        }
        return v;
    }

    JsonPtr parse_object() {
        auto v = xmex::makeShared<JsonValue>();
        v->kind = JsonValue::kObject;
        eat('{');
        skip_ws();
        if (eat('}')) return v;
        while (p_ < s_.size()) {
            skip_ws();
            if (p_ >= s_.size() || s_[p_] != '"') break;
            std::string key = parse_string();
            if (!eat(':')) break;
            JsonPtr val = parse_value();
            v->obj.emplace_back(key, val);
            skip_ws();
            if (eat(',')) continue;
            eat('}');
            break;
        }
        return v;
    }
};

const JsonPtr obj_get(const JsonPtr& o, const char* key) {
    if (!o || o->kind != JsonValue::kObject) return nullptr;
    for (const auto& [k, v] : o->obj) {
        if (k == key) return v;
    }
    return nullptr;
}

// ---- JSON 输出 ----

std::string json_escape(const std::string& s) {
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
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// ---- 已加载的库 ----

struct FfiFunction {
    std::string name;
    std::vector<std::string> params;   // "int"/"bool"/"float"/"str"/"ptr"
    std::string ret;                   // "int"/"bool"/"float"/"str"/"void"
    void* addr = nullptr;
};

struct FfiLibrary {
    std::string path;
    void* handle = nullptr;
    std::vector<FfiFunction> functions;
    bool has_descriptor = false;
};

std::mutex g_mu;
std::map<std::string, FfiLibrary> g_libs;
int64_t g_next_id = 1;

// 调用约定
using XmexFfiFn = int64_t (*)(int64_t*, int64_t, int64_t*);
// 描述符导出
using XmexFfiDescribeFn = int64_t (*)(char*, int64_t);

std::string make_id() {
    return "ffi-" + std::to_string(g_next_id++);
}

// 把描述符 JSON 解析进 functions
void parse_descriptor(FfiLibrary& lib, const std::string& json) {
    JsonParser parser(json);
    JsonPtr root = parser.parse();
    JsonPtr fns = obj_get(root, "functions");
    if (!fns || fns->kind != JsonValue::kArray) return;

    for (const auto& fv : fns->arr) {
        if (!fv || fv->kind != JsonValue::kObject) continue;
        JsonPtr nv = obj_get(fv, "name");
        if (!nv || nv->kind != JsonValue::kString) continue;
        FfiFunction fn;
        fn.name = nv->str;

        JsonPtr pv = obj_get(fv, "params");
        if (pv && pv->kind == JsonValue::kArray) {
            for (const auto& p : pv->arr) {
                if (p && p->kind == JsonValue::kString) fn.params.push_back(p->str);
            }
        }
        JsonPtr rv = obj_get(fv, "ret");
        if (rv && rv->kind == JsonValue::kString) fn.ret = rv->str;
        else fn.ret = "int";

        // 取符号地址。描述符里没给地址就用同名符号 —— 约定被调函数
        // 的导出名与描述符里的 name 一致。
        fn.addr = XMEX_DLSYM(lib.handle, fn.name.c_str());
        lib.functions.push_back(fn);
    }
    lib.has_descriptor = true;
}

// 找出库里某个符号(用于无描述符模式)
void* lookup_symbol(void* handle, const std::string& name) {
    return XMEX_DLSYM(handle, name.c_str());
}

}  // namespace

extern "C" {

void ffi_load(const char* path, int64_t pathLen,
              char* out, int64_t outCap, char* err, int64_t errCap) {
    if (out && outCap > 0) out[0] = '\0';
    std::string p(path, pathLen);
    if (p.empty()) {
        snprintf(err, (size_t)errCap, "empty path");
        return;
    }

    // 已经在加载过?直接返回它的 id
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& [id, lib] : g_libs) {
            if (lib.path == p) {
                snprintf(out, (size_t)outCap, "%s", id.c_str());
                snprintf(err, (size_t)errCap, "ok");
                return;
            }
        }
    }

    void* h = XMEX_DLOPEN(p.c_str());
    if (!h) {
        const char* e = XMEX_DLERROR();
        snprintf(err, (size_t)errCap, "dlopen failed: %s", e && *e ? e : "unknown");
        return;
    }

    FfiLibrary lib;
    lib.path = p;
    lib.handle = h;

    // 读描述符
    auto desc = (XmexFfiDescribeFn)XMEX_DLSYM(h, "xmex_ffi_describe");
    if (desc) {
        // 描述符可能返回长度,也可能把内容写进 buf。用两段式:
        // 先给 64KB 缓冲,不够再按返回的长度重试。
        std::vector<char> buf(64 * 1024);
        int64_t n = desc(buf.data(), (int64_t)buf.size());
        if (n > 0) {
            if ((size_t)n >= buf.size()) {
                buf.resize((size_t)n + 1);
                n = desc(buf.data(), (int64_t)buf.size());
            }
            if (n > 0 && (size_t)n < buf.size()) {
                parse_descriptor(lib, std::string(buf.data(), (size_t)n));
            }
        }
    }

    std::string id = make_id();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_libs[id] = std::move(lib);
    }
    snprintf(out, (size_t)outCap, "%s", id.c_str());
    snprintf(err, (size_t)errCap, "ok");
}

int64_t ffi_unload(const char* handle, int64_t handleLen) {
    std::string id(handle, handleLen);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_libs.find(id);
    if (it == g_libs.end()) return 0;
    if (it->second.handle) XMEX_DLCLOSE(it->second.handle);
    g_libs.erase(it);
    return 1;
}

const char* ffi_symbols(const char* handle, int64_t handleLen, char* buf, int64_t bufCap) {
    std::string id(handle, handleLen);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_libs.find(id);
    if (it == g_libs.end()) {
        snprintf(buf, (size_t)bufCap, "[]");
        return buf;
    }
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& fn : it->second.functions) {
        if (!first) os << ",";
        first = false;
        os << "\"" << json_escape(fn.name) << "\"";
    }
    os << "]";
    std::string s = os.str();
    snprintf(buf, (size_t)bufCap, "%s", s.c_str());
    return buf;
}

const char* ffi_describe(const char* handle, int64_t handleLen,
                         const char* fn, int64_t fnLen, char* buf, int64_t bufCap) {
    std::string id(handle, handleLen);
    std::string name(fn, fnLen);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_libs.find(id);
    if (it == g_libs.end()) {
        snprintf(buf, (size_t)bufCap, "null");
        return buf;
    }
    for (const auto& f : it->second.functions) {
        if (f.name != name) continue;
        std::ostringstream os;
        os << "{\"name\":\"" << json_escape(f.name) << "\",\"params\":[";
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (i) os << ",";
            os << "\"" << json_escape(f.params[i]) << "\"";
        }
        os << "],\"ret\":\"" << json_escape(f.ret) << "\"}";
        std::string s = os.str();
        snprintf(buf, (size_t)bufCap, "%s", s.c_str());
        return buf;
    }
    snprintf(buf, (size_t)bufCap, "null");
    return buf;
}

const char* ffi_call(const char* handle, int64_t handleLen,
                     const char* fn, int64_t fnLen,
                     const char* args_json, int64_t argsLen,
                     char* buf, int64_t bufCap) {
    std::string id(handle, handleLen);
    std::string name(fn, fnLen);
    std::string args_str(args_json, argsLen);

    FfiFunction target;
    bool have_desc = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_libs.find(id);
        if (it == g_libs.end()) {
            snprintf(buf, (size_t)bufCap, "{\"ok\":false,\"error\":\"library not loaded\"}");
            return buf;
        }
        for (const auto& f : it->second.functions) {
            if (f.name == name) { target = f; have_desc = true; break; }
        }
        if (!have_desc) {
            // 无描述符模式:直接按同名符号找
            target.name = name;
            target.ret = "int";
            target.addr = lookup_symbol(it->second.handle, name);
            if (!target.addr) {
                snprintf(buf, (size_t)bufCap, "{\"ok\":false,\"error\":\"symbol not found: %s\"}",
                         name.c_str());
                return buf;
            }
        }
    }
    if (!target.addr) {
        snprintf(buf, (size_t)bufCap, "{\"ok\":false,\"error\":\"symbol not found: %s\"}",
                 name.c_str());
        return buf;
    }

    // 解析实参
    JsonParser parser(args_str);
    JsonPtr root = parser.parse();
    std::vector<JsonPtr> argv;
    if (root && root->kind == JsonValue::kArray) argv = root->arr;

    // 字符串缓冲:本次调用期间有效,函数返回后失效
    std::string scratch;
    std::vector<int64_t> slots(argv.size(), 0);
    // 描述符声明的参数类型优先;无描述符时一律按 int64 处理
    for (size_t i = 0; i < argv.size(); ++i) {
        std::string type = (have_desc && i < target.params.size()) ? target.params[i] : "int";
        const JsonPtr& a = argv[i];
        if (!a) { slots[i] = 0; continue; }
        if (type == "float") {
            double d = (a->kind == JsonValue::kNumber) ? a->num : 0;
            int64_t bits = 0;
            memcpy(&bits, &d, sizeof(bits));
            slots[i] = bits;
        } else if (type == "str") {
            std::string sv = (a->kind == JsonValue::kString) ? a->str : "";
            // 在 scratch 里追加,4 字节对齐,存指针
            while (scratch.size() % 8 != 0) scratch += '\0';
            size_t off = scratch.size();
            scratch += sv;
            scratch += '\0';
            slots[i] = (int64_t)(scratch.data() + off);
        } else if (type == "bool") {
            slots[i] = (a->kind == JsonValue::kBool && a->b) ? 1 : 0;
        } else {
            if (a->kind == JsonValue::kNumber) {
                slots[i] = (int64_t)a->num;
            } else if (a->kind == JsonValue::kBool) {
                slots[i] = a->b ? 1 : 0;
            } else {
                slots[i] = 0;
            }
        }
    }

    int64_t out[4] = {0, 0, 0, 0};
    auto fnp = (XmexFfiFn)target.addr;
    int64_t rc = fnp(slots.data(), (int64_t)slots.size(), out);

    std::ostringstream os;
    if (rc != 0) {
        os << "{\"ok\":false,\"error\":\"call failed, code=" << rc << "\"}";
    } else {
        os << "{\"ok\":true";
        std::string rt = target.ret;
        if (rt == "float") {
            double d;
            memcpy(&d, &out[0], sizeof(d));
            os << ",\"value\":" << d;
        } else if (rt == "bool") {
            os << ",\"value\":" << (out[0] ? "true" : "false");
        } else if (rt == "str") {
            const char* sp = (const char*)(intptr_t)out[0];
            os << ",\"value\":\"" << json_escape(sp ? sp : "") << "\"";
        } else {
            os << ",\"value\":" << out[0];
        }
        os << "}";
    }
    std::string s = os.str();
    snprintf(buf, (size_t)bufCap, "%s", s.c_str());
    return buf;
}

const char* ffi_list(char* buf, int64_t bufCap) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& [id, lib] : g_libs) {
        if (!first) os << ",";
        first = false;
        os << "{\"id\":\"" << json_escape(id) << "\"";
        os << ",\"path\":\"" << json_escape(lib.path) << "\"";
        os << ",\"functions\":" << lib.functions.size();
        os << ",\"has_descriptor\":" << (lib.has_descriptor ? "true" : "false");
        os << "}";
    }
    os << "]";
    std::string s = os.str();
    snprintf(buf, (size_t)bufCap, "%s", s.c_str());
    return buf;
}

}  // extern "C"
