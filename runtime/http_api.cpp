// http_api.cpp —— HTTP 客户端的语言层接口
//
// 暴露到 PyLite:
//   http.get(url, timeout_ms)          → 响应 JSON 字符串
//   http.post(url, body, timeout_ms)   → 响应 JSON 字符串
//   http.request(method, url, body, timeout_ms, token, headers)
//   http.head(url, timeout_ms)         → 只取状态码与响应头
//
// 全部返回 JSON 字符串,理由与 kv.* 一致:PyLite 侧解析 JSON 字符串比解析
// 嵌套结构省事,而且和项目现有的返回值风格统一(没有 JSON 解析器可依赖)。
//
// 故意**不做**的事:不提供连接池、不提供异步、不做重定向跟随、不做
// multipart/form-data。接口测试脚本要的是"看得见每一步",这些"聪明"
// 的行为反而会让失败难以归因 —— 比如自动重试把你刚注入的故障冲掉了。

#include "http.h"
#include "xmex/runtime.h"
#include "xmex/value.h"

#include <cstdarg>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string str_of(const xmex::Value* v) {
    if (!v || v->tag != xmex::kStr) return "";
    return std::string(str_data(v), str_size(v));
}

int64_t int_or(const xmex::Value* v, int64_t dflt) {
    if (!v || v->tag != xmex::kInt) return dflt;
    return v->as.i;
}

// headers 参数:接受 {"k":"v", ...} 或 [["k","v"], ...]。
// 后者允许同一个头出现多次(比如多个 Cookie),dict 做不到。
//
// 跨进程/跨语言边界上 dict 的 key 顺序不保证,所以两种形态都支持。
std::vector<std::pair<std::string, std::string>> parse_headers(const xmex::Value* v) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!v) return out;

    if (v->tag == xmex::kDict) {
        int64_t n = dict_len(v);
        for (int64_t i = 0; i < n; ++i) {
            xmex::Value k = dict_key_at(v, i);
            xmex::Value val = dict_val_at(v, i);
            if (k.tag != xmex::kStr || val.tag != xmex::kStr) continue;
            out.emplace_back(std::string(str_data(&k), str_size(&k)),
                             std::string(str_data(&val), str_size(&val)));
        }
    } else if (v->tag == xmex::kList) {
        int64_t n = list_len(v);
        for (int64_t i = 0; i < n; ++i) {
            xmex::Value pair = list_get(v, i);
            if (pair.tag != xmex::kList) continue;
            xmex::Value k = list_get(&pair, 0);
            xmex::Value val = list_get(&pair, 1);
            if (k.tag != xmex::kStr || val.tag != xmex::kStr) continue;
            out.emplace_back(std::string(str_data(&k), str_size(&k)),
                             std::string(str_data(&val), str_size(&val)));
        }
    }
    return out;
}

}  // namespace

extern "C" {

// http.get(url[, timeout_ms]) -> 响应 JSON
xmex::Value http_get_v(const xmex::Value* url, ...) {
    va_list ap;
    va_start(ap, url);
    const xmex::Value* timeout_ms = va_arg(ap, const xmex::Value*);
    va_end(ap);

    xmex::HttpRequest req;
    req.method = "GET";
    req.url = str_of(url);
    req.timeout_ms = int_or(timeout_ms, 5000);
    if (req.url.empty()) {
        std::string e = "{\"ok\":false,\"error\":\"http.get needs a url\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }
    std::string j = xmex::http_response_to_json(xmex::http_send(req));
    return str_new(j.c_str(), (int64_t)j.size());
}

// http.post(url, body[, timeout_ms]) -> 响应 JSON
xmex::Value http_post_v(const xmex::Value* url, const xmex::Value* body, ...) {
    va_list ap;
    va_start(ap, body);
    const xmex::Value* timeout_ms = va_arg(ap, const xmex::Value*);
    va_end(ap);

    xmex::HttpRequest req;
    req.method = "POST";
    req.url = str_of(url);
    req.body = str_of(body);
    req.timeout_ms = int_or(timeout_ms, 5000);
    if (req.url.empty()) {
        std::string e = "{\"ok\":false,\"error\":\"http.post needs a url\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }
    std::string j = xmex::http_response_to_json(xmex::http_send(req));
    return str_new(j.c_str(), (int64_t)j.size());
}

// http.request(method, url[, body[, timeout_ms[, token[, headers]]]]) -> 响应 JSON
//
// 这是最通用的入口;get/post/head 都是它的薄封装。测试要发 PUT/DELETE
// 或带自定义头时用它。
//
// ⚠️ 变参签名是**必需的** —— 见 include/xmex/runtime.h 的说明:IRGen 按调用点
// 的实参个数生成 IR,固定参个数会让少传参数的调用点链接失败。
//
// 可变参数在 x86_64 上要按 8 字节对齐取(指针就是 8 字节,天然满足)。
// 传进来的可能是 nullptr(irgen 对"没写的参数"不补值,那个槽是 0),
// 所以每个都要判空。
xmex::Value http_request_v(const xmex::Value* method, const xmex::Value* url, ...) {
    xmex::HttpRequest req;
    req.method = str_of(method);
    req.url = str_of(url);

    va_list ap;
    va_start(ap, url);
    const xmex::Value* body = va_arg(ap, const xmex::Value*);
    const xmex::Value* timeout_ms = va_arg(ap, const xmex::Value*);
    const xmex::Value* token = va_arg(ap, const xmex::Value*);
    const xmex::Value* headers = va_arg(ap, const xmex::Value*);
    va_end(ap);

    req.body = str_of(body);
    req.timeout_ms = int_or(timeout_ms, 5000);
    req.bearer_token = str_of(token);
    req.headers = parse_headers(headers);

    if (req.method.empty()) req.method = "GET";
    if (req.url.empty()) {
        std::string e = "{\"ok\":false,\"error\":\"http.request needs a url\"}";
        return str_new(e.c_str(), (int64_t)e.size());
    }
    std::string j = xmex::http_response_to_json(xmex::http_send(req));
    return str_new(j.c_str(), (int64_t)j.size());
}

// http.head(url[, timeout_ms]) -> 响应 JSON
// HEAD 的响应没有 body,但状态码与响应头齐全,拿来做"接口活着吗"的探针很合适。
xmex::Value http_head_v(const xmex::Value* url, ...) {
    va_list ap;
    va_start(ap, url);
    const xmex::Value* timeout_ms = va_arg(ap, const xmex::Value*);
    va_end(ap);

    xmex::HttpRequest req;
    req.method = "HEAD";
    req.url = str_of(url);
    req.timeout_ms = int_or(timeout_ms, 5000);
    std::string j = xmex::http_response_to_json(xmex::http_send(req));
    return str_new(j.c_str(), (int64_t)j.size());
}

}  // extern "C"