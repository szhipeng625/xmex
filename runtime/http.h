// http.h —— 通用 HTTP 客户端
//
// 为什么要有这个:接口测试要能打**任意** URL,而 cluster.cpp 里已有一份
// 写得不错的 HTTP 实现,只是它是 static 私有函数、绑死了 cluster 协议
// (要 secret、要先注册节点)。这里把那部分抽成独立模块。
//
// ⚠️ 不是重写。tcp_connect / parse_url / 请求构建 / 响应解析的逻辑照搬
// cluster.cpp,只做了两处扩展(见下方"与 cluster 版的差别")。
#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>

namespace xmex {

// 一次 HTTP 请求的结果。
struct HttpResponse {
    bool        ok = false;       // 传输层成功 且 状态码 2xx
    int64_t     status = 0;       // HTTP 状态码;传输失败时为 0
    std::string body;             // 响应体(已解 chunked)
    std::string headers;          // 原始响应头(含状态行)
    std::string error;            // 失败原因;成功时为空
    int64_t     elapsed_ms = 0;   // 端到端耗时
    int64_t     content_length = -1;
};

// 一次请求的参数。
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::string body;
    int64_t     timeout_ms = 5000;
    // 自定义请求头。加同名项会覆盖默认生成的。
    std::vector<std::pair<std::string, std::string>> headers;
    // 默认加 "Authorization: Bearer <token>"。空 = 不加。
    std::string bearer_token;
    bool        use_tls = false;      // https://
    bool        insecure_skip_verify = true;  // 不验 CA(与 cluster 一致)
};

// 发一次请求。**同步阻塞** —— 接口测试是脚本驱动的,不需要异步。
HttpResponse http_send(const HttpRequest& req);

// 便利包装。
HttpResponse http_get(const std::string& url, int64_t timeout_ms = 5000);
HttpResponse http_post(const std::string& url, const std::string& body,
                       int64_t timeout_ms = 5000);

// 把 HttpResponse 转成 JSON 文本(给语言层用)。
std::string http_response_to_json(const HttpResponse& r);

}  // namespace xmex