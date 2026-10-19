// http_test.cpp —— HTTP 客户端的独立验证
//
// 测法:在测试进程里起一个**最小的 HTTP 服务端**(裸 socket,几十行),
// 然后用 http_get/http_post 打它。这样测的是真链路 —— 真 TCP、真 HTTP
// 报文、真 chunked 解码,不是 mock。
//
// 刻意不依赖 runtime:http.cpp 只依赖 platform.h 的 socket 抽象,
// 这样测试能链在任何环境上。
//
// 覆盖:URL 解析、GET/POST、自定义头、chunked 解码、状态码、超时、
// 连接失败、404/500 等错误路径。

#include "http.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

#include "xmex/platform.h"
#ifdef XMEX_WINDOWS
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
#endif

static int g_pass = 0, g_fail = 0;
static void check(bool cond, const char* what) {
    if (cond) { g_pass++; std::printf("  [PASS] %s\n", what); }
    else { g_fail++; std::printf("  [FAIL] %s\n", what); }
}

// ---- 迷你 HTTP 服务端 ----
namespace {

std::atomic<bool> g_stop{false};
int g_port = 0;

void serve_once(int sock) {
    char buf[8192] = {0};
    int n = (int)recv(sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { XMEX_CLOSE_SOCKET((xmex_socket_t)sock); return; }
    std::string req(buf, (size_t)n);

    // 路由
    std::string resp;
    if (req.rfind("GET /hello", 0) == 0) {
        std::string body = "world";
        resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    } else if (req.rfind("GET /chunked", 0) == 0) {
        // chunked 响应 —— 这是重点:cluster 版会把整段 chunk 协议文本
        // 当 body 返回,调用方一解析就炸
        resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
               "Connection: close\r\n\r\n"
               "5\r\nhello\r\n"
               "6\r\n world\r\n"
               "0\r\n\r\n";
    } else if (req.rfind("POST /echo", 0) == 0) {
        // 取请求体回显
        size_t he = req.find("\r\n\r\n");
        std::string body = (he == std::string::npos) ? "" : req.substr(he + 4);
        resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    } else if (req.rfind("GET /slow", 0) == 0) {
        // 故意慢响应,测超时
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    } else if (req.rfind("GET /boom", 0) == 0) {
        resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 3\r\n"
               "Connection: close\r\n\r\nerr";
    } else if (req.rfind("GET /missing", 0) == 0) {
        resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
        resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    }
    send(sock, resp.c_str(), (int)resp.size(), 0);
    XMEX_CLOSE_SOCKET((xmex_socket_t)sock);
}

void server_loop(int listen_sock) {
    while (!g_stop) {
        struct timeval tv{0, 100000};   // 100ms 轮询退出标志
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET((xmex_socket_t)listen_sock, &rfds);
        if (select((int)listen_sock + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
        int c = (int)accept(listen_sock, nullptr, nullptr);
        if (c < 0) continue;
        // 每个连接起个线程,/slow 的睡眠不阻塞其它请求
        std::thread(serve_once, c).detach();
    }
    XMEX_CLOSE_SOCKET((xmex_socket_t)listen_sock);
}

int start_server() {
    int ls = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    int on = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;                 // 让内核分配端口
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) != 0) return -1;
    if (listen(ls, 16) != 0) return -1;

    struct sockaddr_in bound{};
#ifdef XMEX_WINDOWS
    int blen = sizeof(bound);
#else
    socklen_t blen = sizeof(bound);
#endif
    getsockname(ls, (struct sockaddr*)&bound, &blen);
    g_port = ntohs(bound.sin_port);

    g_stop = false;
    std::thread(server_loop, ls).detach();
    // listen() 在返回前就已生效,但 accept 循环要线程真正跑起来才收得到连接。
    // 不等的话第一个请求可能正好落在"端口已绑定但循环未启动"的窗口里,
    // 表现为莫名其妙的 connection failed —— 这种竞态极难查。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return g_port;
}

void stop_server() {
    g_stop = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

std::string url(const char* path) {
    return "http://127.0.0.1:" + std::to_string(g_port) + path;
}

}  // namespace

int main() {
    XMEX_SOCKET_INIT();
    std::printf("=== xmex HTTP 客户端验证 ===\n");

    int port = start_server();
    if (port <= 0) {
        std::printf("[FAIL] 无法启动测试服务端\n");
        return 1;
    }
    std::printf("测试服务端:127.0.0.1:%d\n\n", port);

    // ---- 1. 基本 GET ----
    std::printf("=== 1. 基本 GET ===\n");
    {
        xmex::HttpResponse r = xmex::http_get(url("/hello"), 3000);
        check(r.ok, "GET /hello 成功");
        check(r.status == 200, "状态码 200");
        check(r.body == "world", "响应体是 \"world\"");
        check(r.content_length == 5, "解析出 Content-Length=5");
        check(r.error.empty(), "无错误");
        std::printf("  (耗时 %lldms)\n", (long long)r.elapsed_ms);
    }

    // ---- 2. chunked 解码 ----
    std::printf("\n=== 2. chunked 传输编码 ===\n");
    {
        xmex::HttpResponse r = xmex::http_get(url("/chunked"), 3000);
        check(r.ok, "GET /chunked 成功");
        // 正确的解码结果是 "hello world";不解码的话拿到的是
        // "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n" 整段协议文本
        check(r.body == "hello world", "chunked 被正确解码为 \"hello world\"");
        check(r.body.find("5\\r\\n") == std::string::npos,
              "body 里不含 chunk 协议残留");
    }

    // ---- 3. POST ----
    std::printf("\n=== 3. POST ===\n");
    {
        xmex::HttpResponse r = xmex::http_post(url("/echo"), "{\"k\":1}", 3000);
        check(r.ok, "POST /echo 成功");
        check(r.body == "{\"k\":1}", "请求体被原样回显");
    }

    // ---- 4. 错误状态码 ----
    std::printf("\n=== 4. 错误状态码 ===\n");
    {
        xmex::HttpResponse r = xmex::http_get(url("/missing"), 3000);
        check(!r.ok, "404 不算 ok");
        check(r.status == 404, "状态码 404");
        check(r.error.find("404") != std::string::npos, "错误信息含状态码");

        r = xmex::http_get(url("/boom"), 3000);
        check(!r.ok, "500 不算 ok");
        check(r.status == 500, "状态码 500");
        check(r.body == "err", "500 的响应体照样能取到");
    }

    // ---- 5. 连接失败 ----
    std::printf("\n=== 5. 连接失败 ===\n");
    {
        // 端口 1 基本不会有人监听
        xmex::HttpResponse r = xmex::http_get("http://127.0.0.1:1/x", 1000);
        check(!r.ok, "连不上时 ok=false");
        check(r.error.find("connection failed") != std::string::npos,
              "错误信息说明了连接失败");
    }

    // ---- 6. 超时 ----
    std::printf("\n=== 6. 超时 ===\n");
    {
        int64_t t0 = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count();
        xmex::HttpResponse r = xmex::http_get(url("/slow"), 800);   // 服务端要 3s
        int64_t spent = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count() - t0;
        check(!r.ok, "超时后 ok=false");
        check(spent < 2500, "超时提前返回(没等满服务端的 3s)");
        std::printf("  (设定 800ms,实际 %lldms)\n", (long long)spent);
    }

    // ---- 7. URL 解析 ----
    std::printf("\n=== 7. URL 解析 ===\n");
    {
        xmex::HttpResponse r = xmex::http_get("http://", 500);
        check(!r.ok && r.error.find("invalid url") != std::string::npos,
              "空 host 报 invalid url");
    }

    // ---- 8. JSON 输出 ----
    std::printf("\n=== 8. JSON 输出 ===\n");
    {
        xmex::HttpResponse r = xmex::http_get(url("/hello"), 3000);
        std::string j = xmex::http_response_to_json(r);
        check(j.find("\"status\":200") != std::string::npos, "JSON 含 status");
        check(j.find("\"body\":\"world\"") != std::string::npos, "JSON 含转义后的 body");
        check(j.find("\"ok\":true") != std::string::npos, "JSON 含 ok");
        // body 里若含引号/换行,必须被转义,否则产出坏 JSON
        xmex::HttpResponse q = xmex::http_post(url("/echo"), "line1\nline2\"quoted\"", 3000);
        std::string jq = xmex::http_response_to_json(q);
        check(jq.find("\\n") != std::string::npos, "换行被转义成 \\n");
        check(jq.find("\\\"") != std::string::npos, "引号被转义成 \\\"");
    }

    stop_server();
    XMEX_SOCKET_CLEANUP();

    std::printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}