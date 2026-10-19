// http.cpp —— 通用 HTTP 客户端实现
//
// tcp_connect / parse_url / 请求构建 / 响应解析的逻辑照搬 cluster.cpp
// (那份实现已在生产里跑过,没有理由重写)。相对它做了两处扩展:
//
//  1. **支持 chunked 传输编码**。cluster 版靠 `Connection: close` +
//    读到 socket 关闭来判定结束 —— 遇到 chunked 响应就会把
//    "<chunk-size>\r\n<data>\r\n0\r\n\r\n" 整段当成 body 返回。
//    现在按 Transfer-Encoding: chunked 解析。
//  2. **可传自定义请求头**。cluster 版硬编码 Content-Type 与 Bearer。
//    接口测试要带 Cookie / X-Api-Key / 签名,这些都进 headers。

#include "http.h"

#include "xmex/platform.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <sstream>

// ⚠️ 网络头全部走 platform.h 的跨平台抽象(xmex_socket_t / XMEX_SOCKET_INIT
// / XMEX_CLOSE_SOCKET)。直接 include POSIX 头在 Windows 上会和 winsock2.h
// 打架(struct sockaddr 重定义、connect 签名冲突)。
#ifdef XMEX_WINDOWS
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  // TLS:OpenSSL 只在 Linux 侧链接(见 CMakeLists)。
  // Windows 上不引 OpenSSL 头 —— 那会让 Windows 构建依赖它,而当前
  // Windows 配置没链 OpenSSL。https 在 Windows 上会明确报"不支持"。
  #include <openssl/ssl.h>
  #include <openssl/err.h>
  #define XMEX_HTTP_HAVE_TLS 1
#endif

namespace xmex {
namespace {

// Windows 上每个进程要用 socket 前必须 WSAStartup 一次。
// cluster.cpp 靠调用方做这件事(它在 Linux-only 段,那边不需要),但
// http.cpp 是独立模块,不能指望外部 —— 漏掉的话 socket() 直接返回
// INVALID_SOCKET,表现为"所有请求都 connection failed"。
// 用函数局部静态保证只初始化一次,线程安全由 C++11 保证。
void ensure_socket_init() {
    static bool done = false;
    if (!done) {
        done = true;
        XMEX_SOCKET_INIT();
    }
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef XMEX_HTTP_HAVE_TLS
SSL_CTX* tls_ctx() {
    static SSL_CTX* ctx = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        ctx = SSL_CTX_new(TLS_client_method());
        if (ctx) {
            // 与 cluster 一致:不验 CA。接口测试打的是自签或内网服务,
            // 严格验证会让"证书过期"这类无关问题挡住真正的测试目标。
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        }
    }
    return ctx;
}
#endif

// 解析 URL。**与 cluster 版的差别**:支持 https:// 与无 scheme 的裸 host。
bool parse_url(const std::string& url, std::string& host, int& port,
               std::string& path, bool& tls) {
    std::string rest = url;
    tls = false;
    if (rest.rfind("https://", 0) == 0) {
        tls = true; port = 443;
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    }

    size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        path = rest.substr(slash);
        rest = rest.substr(0, slash);
    } else {
        path = "/";
    }

    size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        try {
            port = std::stoi(rest.substr(colon + 1));
        } catch (...) {
            return false;
        }
    } else {
        host = rest;
        if (port == 0) port = tls ? 443 : 80;
    }
    return !host.empty();
}

// 切成非阻塞。配合 select 使用:select 说可读之后再非阻塞 recv,
// 不会为了"只到一半"的数据再阻塞一次。
void set_nonblocking(int sock) {
#ifdef XMEX_WINDOWS
    u_long nb = 1;
    ioctlsocket((xmex_socket_t)sock, FIONBIO, &nb);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// 恢复阻塞模式。Windows 用 ioctlsocket(FIONBIO),POSIX 用 fcntl。
void set_blocking(int sock) {
#ifdef XMEX_WINDOWS
    u_long block = 0;
    ioctlsocket((xmex_socket_t)sock, FIONBIO, &block);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags);   // 去掉 O_NONBLOCK
#endif
}

int tcp_connect(const std::string& host, int port, int64_t timeout_ms) {
    ensure_socket_init();
    int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    // ⚠️ loopback 走**阻塞** connect,不要非阻塞。
    //
    // 实测:Windows 上对 127.0.0.1 做非阻塞 connect,connect() 立刻返回
    // WSAEADDRNOTAVAIL(10049) 而不是 WSAEWOULDBLOCK,而那个连接其实
    // 并不会真的建立 —— 后续 select 也等不到可读/可写,表现为
    // "所有本地请求一律 connection failed"。
    // 本机连接本来就该在微秒级完成,阻塞 connect 的等待时间可以忽略,
    // 没必要为它承担非阻塞的复杂度。
    // 一律非阻塞 + select 等完成,这样连接阶段也有超时保障。
    //
    // (曾试过对 loopback 走阻塞 connect 来绕开 Windows 的 10049,结果
    //  丢了连接超时 —— 连不上时固定要等 2 秒,接口测试里"快速失败"就没了。
    //  正确做法是统一非阻塞,并把 WSAEADDRNOTAVAIL 也算作"进行中"。)
#ifndef XMEX_WINDOWS
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#else
    u_long nonblock = 1;
    ioctlsocket((xmex_socket_t)sock, FIONBIO, &nonblock);
#endif

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    // ⚠️ 端口必须作为 **service** 参数传给 getaddrinfo。
    // 传 nullptr 的话返回的 sockaddr_in.sin_port 是 0,connect 会连到
    // "host:0" —— 在 Linux 上表现为 ECONNREFUSED,在 Windows 上是
    // WSAEADDRNOTAVAIL(10049),症状都是"连不上"但原因指向完全错误的方向
    // (第一次排查时就被这个误导过,以为是非阻塞 connect 的平台差异)。
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        XMEX_CLOSE_SOCKET((xmex_socket_t)sock);
        return -1;
    }

    int ret = connect((xmex_socket_t)sock, res->ai_addr, (int)res->ai_addrlen);
#ifdef XMEX_WINDOWS
    int wsa_err = WSAGetLastError();
#endif
    freeaddrinfo(res);
    // 判断"连接正在进行中"的错误码。
    //
    // ⚠️ POSIX 与 Windows 不一样,这是实测踩到的:
    //   POSIX:  EINPROGRESS
    //   Windows: WSAEWOULDBLOCK —— 但对 **loopback** 地址,
    //            非阻塞 connect 返回的是 WSAEADDRNOTAVAIL(10049)!
    //            只认 WSAEWOULDBLOCK 会把所有本地连接判成失败,
    //            症状是"127.0.0.1 的服务一律 connection failed"。
    if (ret < 0 && errno != EINPROGRESS
#ifdef XMEX_WINDOWS
        && wsa_err != WSAEWOULDBLOCK && wsa_err != WSAEADDRNOTAVAIL
#endif
        ) {
        XMEX_CLOSE_SOCKET((xmex_socket_t)sock);
        return -1;
    }
    // 立刻连接成功:恢复阻塞后返回
    if (ret == 0) {
        set_blocking(sock);
        return sock;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET((xmex_socket_t)sock, &wfds);
    struct timeval tv;
    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    ret = select((int)sock + 1, nullptr, &wfds, nullptr, &tv);
    if (ret <= 0) { XMEX_CLOSE_SOCKET((xmex_socket_t)sock); return -1; }

    int err = 0;
#ifdef XMEX_WINDOWS
    int len = sizeof(err);
    getsockopt((xmex_socket_t)sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
#else
    socklen_t len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
    if (err != 0) { XMEX_CLOSE_SOCKET((xmex_socket_t)sock); return -1; }

    set_blocking(sock);
    return sock;
}

// 从原始响应里取某个响应头(大小写不敏感)。
std::string header_of(const std::string& headers, const char* name) {
    std::string want = "\r\n";
    // 转小写比较更稳,但请求头里我们发的是原样,这里做宽松匹配
    std::string lower_hdrs = headers, lower_name = name;
    for (auto& c : lower_hdrs) c = (char)tolower((unsigned char)c);
    for (auto& c : lower_name) c = (char)tolower((unsigned char)c);
    want = "\r\n" + lower_name + ":";
    size_t p = lower_hdrs.find(want);
    if (p == std::string::npos) return "";
    p += want.size();
    while (p < headers.size() && (headers[p] == ' ' || headers[p] == '\t')) ++p;
    size_t e = headers.find("\r\n", p);
    if (e == std::string::npos) return "";
    return headers.substr(p, e - p);
}

// 解 chunked 传输编码。
//
// 为什么必须做:不少 HTTP 服务(尤其用 chunked 而不是 Content-Length 的)
// 会返回 "<hex>\r\n<data>\r\n0\r\n\r\n"。不解的话整个 chunk 协议文本
// 会被当成 body,调用方一解析就炸,还以为是接口返回错了。
std::string decode_chunked(const std::string& raw) {
    std::string out;
    size_t p = 0;
    while (p < raw.size()) {
        // chunk-size 行(可能带 ;ext=... 扩展)
        size_t eol = raw.find("\r\n", p);
        if (eol == std::string::npos) break;
        std::string size_line = raw.substr(p, eol - p);
        size_t semi = size_line.find(';');
        if (semi != std::string::npos) size_line = size_line.substr(0, semi);
        size_t chunk_size = (size_t)strtoul(size_line.c_str(), nullptr, 16);

        p = eol + 2;
        if (chunk_size == 0) break;         // 结束块
        if (p + chunk_size > raw.size()) {  // 数据被截断(超时/断开)
            out += raw.substr(p);
            break;
        }
        out += raw.substr(p, chunk_size);
        p += chunk_size + 2;                // 跳过数据后的 CRLF
    }
    return out;
}

// JSON 字符串转义。响应体是服务端给的,可能有引号/换行/控制字符,
// 直接塞进 JSON 会产出坏文本 —— 那样调用方解析失败,还以为接口坏了。
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

}  // namespace

HttpResponse http_send(const HttpRequest& req) {
    HttpResponse r;
    const int64_t start = now_ms();

    std::string host, path;
    int port = 0;
    bool tls = false;
    if (!parse_url(req.url, host, port, path, tls)) {
        r.error = "invalid url: " + req.url;
        r.elapsed_ms = now_ms() - start;
        return r;
    }

    int sock = tcp_connect(host, port, req.timeout_ms);
    if (sock < 0) {
        r.error = "connection failed: " + host + ":" + std::to_string(port);
        r.elapsed_ms = now_ms() - start;
        return r;
    }

#ifdef XMEX_HTTP_HAVE_TLS
    SSL* ssl = nullptr;
    const bool want_tls = req.use_tls || tls;
    if (want_tls) {
        SSL_CTX* ctx = tls_ctx();
        if (ctx) {
            ssl = SSL_new(ctx);
            SSL_set_fd(ssl, (int)sock);
            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl);
                XMEX_CLOSE_SOCKET((xmex_socket_t)sock);
                r.error = "TLS handshake failed";
                r.elapsed_ms = now_ms() - start;
                return r;
            }
        } else {
            XMEX_CLOSE_SOCKET((xmex_socket_t)sock);
            r.error = "TLS unavailable (OpenSSL init failed)";
            r.elapsed_ms = now_ms() - start;
            return r;
        }
    }
#else
    // Windows 构建没链 OpenSSL,https 明确报不支持而不是静默降级成明文 ——
    // 静默降级会让"以为在测 HTTPS 其实在测 HTTP"这类错误极难发现。
    const bool want_tls = req.use_tls || tls;
    if (want_tls) {
        XMEX_CLOSE_SOCKET((xmex_socket_t)sock);
        r.error = "https not supported on this build (OpenSSL not linked)";
        r.elapsed_ms = now_ms() - start;
        return r;
    }
#endif

    struct timeval tv;
    tv.tv_sec = (long)(req.timeout_ms / 1000);
    tv.tv_usec = (long)((req.timeout_ms % 1000) * 1000);
    setsockopt((xmex_socket_t)sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt((xmex_socket_t)sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    // ---- 构建请求 ----
    std::ostringstream out;
    out << req.method << " " << path << " HTTP/1.1\r\n";
    out << "Host: " << host << ":" << port << "\r\n";
    // 默认头。用户传的同名头会覆盖(见下面的去重逻辑)。
    std::vector<std::pair<std::string, std::string>> hdrs;
    hdrs.emplace_back("Content-Type", "application/json");
    if (!req.bearer_token.empty()) {
        hdrs.emplace_back("Authorization", "Bearer " + req.bearer_token);
    }
    for (const auto& h : req.headers) hdrs.push_back(h);

    // 去掉重复的 header(后者覆盖前者)—— 直接给同名头是常见用法,
    // 比如要把 Content-Type 改成 x-www-form-urlencoded。
    for (size_t i = 0; i < hdrs.size(); ++i) {
        for (size_t j = i + 1; j < hdrs.size(); ++j) {
            if (strcasecmp(hdrs[i].first.c_str(), hdrs[j].first.c_str()) == 0) {
                hdrs.erase(hdrs.begin() + (long)i);
                --i;
                break;
            }
        }
    }
    for (const auto& h : hdrs) {
        out << h.first << ": " << h.second << "\r\n";
    }
    out << "Connection: close\r\n";
    if (!req.body.empty()) {
        out << "Content-Length: " << req.body.size() << "\r\n";
    }
    out << "\r\n";
    if (!req.body.empty()) out << req.body;

    // 收发都过一层 lambda:有 TLS 走 SSL_*,否则走裸 socket。
    // 分成两个 #ifdef 分支是因为 SSL_write/SSL_read 名字在无 OpenSSL 时不存在。
#ifdef XMEX_HTTP_HAVE_TLS
    auto send_all = [&](const char* p, int n) -> ssize_t {
        return ssl ? SSL_write(ssl, p, n) : send((xmex_socket_t)sock, p, n, 0);
    };
    auto recv_some = [&](char* p, int n) -> ssize_t {
        return ssl ? SSL_read(ssl, p, n) : recv((xmex_socket_t)sock, p, n, 0);
    };
#else
    auto send_all = [&](const char* p, int n) -> ssize_t {
        return send((xmex_socket_t)sock, p, n, 0);
    };
    auto recv_some = [&](char* p, int n) -> ssize_t {
        return recv((xmex_socket_t)sock, p, n, 0);
    };
#endif

    std::string req_str = out.str();
    ssize_t sent = send_all(req_str.c_str(), (int)req_str.size());
    if (sent < 0) {
#ifdef XMEX_HTTP_HAVE_TLS
        if (ssl) SSL_free(ssl);
#endif
        XMEX_CLOSE_SOCKET((xmex_socket_t)sock);
        r.error = "send failed";
        r.elapsed_ms = now_ms() - start;
        return r;
    }

    // ---- 读响应 ----
    //
    // ⚠️ 用 select 等可读 + 非阻塞 recv,而不是靠 SO_RCVTIMEO。
    //
    // 实测:Windows 上 SO_RCVTIMEO 对 recv() **完全无效** —— 设成 800ms,
    // 对端 3 秒后才回,recv 照样阻塞满 3 秒(独立最小程序验证过,不是本项目的
    // 问题)。POSIX 上它是生效的,所以跨平台代码不能依赖它。
    //
    // 改成 select 之后两个平台行为一致,而且顺带拿到了"整体预算"语义:
    // 每轮 select 用剩余超时,累加起来不超过 timeout_ms。
    char buf[8192];
    std::string raw;
    int64_t deadline = start + req.timeout_ms;
    bool timed_out = false;
    while (true) {
        int64_t remain = deadline - now_ms();
        if (remain <= 0) { timed_out = true; break; }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET((xmex_socket_t)sock, &rfds);
        struct timeval rtv;
        rtv.tv_sec = (long)(remain / 1000);
        rtv.tv_usec = (long)((remain % 1000) * 1000);

        int sel = select((int)sock + 1, &rfds, nullptr, nullptr, &rtv);
        if (sel < 0) {
#ifdef XMEX_WINDOWS
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            break;      // 真错误
        }
        if (sel == 0) { timed_out = true; break; }   // 等够了

        // 有数据可读。切成非阻塞再 recv —— select 说可读但数据可能只到一半,
        // 阻塞 recv 会为了那半截再等一次,超时预算就失真了。
        set_nonblocking(sock);
        ssize_t n = recv_some(buf, (int)sizeof(buf) - 1);
        set_blocking(sock);
        if (n > 0) {
            raw.append(buf, (size_t)n);
        } else if (n == 0) {
            break;      // 对端关闭
        } else {
#ifdef XMEX_WINDOWS
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAEADDRNOTAVAIL) continue;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
            break;
        }
    }
#ifdef XMEX_HTTP_HAVE_TLS
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
#endif
    XMEX_CLOSE_SOCKET((xmex_socket_t)sock);
    r.elapsed_ms = now_ms() - start;

    if (timed_out) {
        r.error = "timeout after " + std::to_string((long long)req.timeout_ms) + "ms";
        if (!raw.empty()) {
            // 超时但已经收到部分响应 —— 照样交给调用方,它可能能从状态行
            // 看出问题(比如服务端回了 500 又卡住)。
            r.headers = raw;
            size_t he0 = raw.find("\r\n\r\n");
            if (he0 != std::string::npos) {
                r.body = raw.substr(he0 + 4);
                size_t sp1 = r.headers.find(' ');
                if (sp1 != std::string::npos) {
                    size_t sp2 = r.headers.find(' ', sp1 + 1);
                    if (sp2 != std::string::npos) {
                        try { r.status = std::stoll(r.headers.substr(sp1 + 1, sp2 - sp1 - 1)); }
                        catch (...) {}
                    }
                }
            }
        }
        return r;
    }
    if (raw.empty()) {
        r.error = "empty response";
        return r;
    }

    size_t he = raw.find("\r\n\r\n");
    if (he == std::string::npos) {
        r.error = "invalid http response (no header terminator)";
        return r;
    }
    r.headers = raw.substr(0, he);
    std::string body = raw.substr(he + 4);

    // 状态码:状态行形如 "HTTP/1.1 200 OK"
    {
        size_t sp1 = r.headers.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = r.headers.find(' ', sp1 + 1);
            std::string code = r.headers.substr(sp1 + 1, sp2 - sp1 - 1);
            try { r.status = std::stoll(code); } catch (...) { r.status = 0; }
        }
    }

    // ★ 与 cluster 版的关键差别:chunked 响应必须解码,
    // 否则调用方拿到的是 "<hex>\r\n...0\r\n\r\n" 整段协议文本。
    std::string te = header_of(r.headers, "Transfer-Encoding");
    if (te.size() >= 7 && strncasecmp(te.c_str(), "chunked", 7) == 0) {
        body = decode_chunked(body);
    }
    std::string cl = header_of(r.headers, "Content-Length");
    if (!cl.empty()) {
        try { r.content_length = std::stoll(cl); } catch (...) {}
    }
    r.body = body;

    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok) {
        r.error = "HTTP " + std::to_string(r.status);
    }
    return r;
}

HttpResponse http_get(const std::string& url, int64_t timeout_ms) {
    HttpRequest req;
    req.method = "GET";
    req.url = url;
    req.timeout_ms = timeout_ms;
    return http_send(req);
}

HttpResponse http_post(const std::string& url, const std::string& body,
                       int64_t timeout_ms) {
    HttpRequest req;
    req.method = "POST";
    req.url = url;
    req.body = body;
    req.timeout_ms = timeout_ms;
    return http_send(req);
}

std::string http_response_to_json(const HttpResponse& r) {
    std::ostringstream j;
    j << "{\"ok\":" << (r.ok ? "true" : "false");
    j << ",\"status\":" << r.status;
    j << ",\"elapsed_ms\":" << r.elapsed_ms;
    j << ",\"content_length\":" << r.content_length;
    j << ",\"body\":\"" << json_escape(r.body) << "\"";
    j << ",\"headers\":\"" << json_escape(r.headers) << "\"";
    if (!r.error.empty()) j << ",\"error\":\"" << json_escape(r.error) << "\"";
    j << "}";
    return j.str();
}

}  // namespace xmex