// ffi_worker.cpp —— FFI 隔离模式的子进程代理
//
// 职责:加载一个动态库,然后循环处理来自宿主的请求。
//
// 协议:一行一条 JSON。
//   请求 {"op":"call","fn":"dll_add","args":[3,4]}
//        {"op":"symbols"}
//        {"op":"describe","fn":"dll_add"}
//   响应 {"ok":true,"value":7}
//        {"ok":true,"symbols":["dll_add",...]}
//        {"ok":false,"error":"..."}
//
// 为什么要有这个进程:
//   DLL 段错误/死循环时,只有子进程死,宿主从管道读到 EOF,拿退出码即可。
//   宿主不会跟着崩 —— 这正是隔离的意义。
//
// 关键设计:**DLL 在这个进程里是进程内加载的**。所以参数与返回值仍要走
// 「序列化 → 反序列化」这一层(跨进程不能传指针),但 DLL 本身的调用
// 走的是 ffi.cpp 的进程内路径 —— 一份代码,两种模型。
//
// 未来做远程时,这个 agent 的协议不用变,只是把 stdin/stdout 换成 socket。

#include "ffi.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <iostream>

namespace {

// 从一行 JSON 里取某个字符串字段的值。
// 只处理我们自己产出的格式,不做通用 JSON 解析。
std::string extract_str(const std::string& line, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = line.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = line.find('"', p);
    if (e == std::string::npos) return "";
    return line.substr(p, e - p);
}

// 取某个字段之后到下一个 ',' 或 '}' 之间的原始文本(用于 args 这种数组)。
std::string extract_raw(const std::string& line, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = line.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    // 跳过空白
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
    size_t start = p;
    int depth = 0;
    bool in_str = false;
    while (p < line.size()) {
        char c = line[p];
        if (in_str) {
            if (c == '\\') { ++p; continue; }
            if (c == '"') in_str = false;
        } else {
            if (c == '"') in_str = true;
            else if (c == '[' || c == '{') ++depth;
            else if (c == ']' || c == '}') {
                if (depth == 0) break;
                --depth;
            }
            else if (c == ',' && depth == 0) break;
        }
        ++p;
    }
    return line.substr(start, p - start);
}

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

// agent 主循环。stdin 读请求,stdout 写响应。
// 返回 0 正常退出(读到 EOF),非 0 表示出错。
extern "C" int ffi_worker_main(const char* lib_path) {
    if (!lib_path || !*lib_path) {
        std::fprintf(stderr, "ffi_worker: no library path\n");
        return 2;
    }

    char handle[64] = {0};
    char err[512] = {0};
    ffi_load(lib_path, (int64_t)strlen(lib_path),
             handle, (int64_t)sizeof(handle), err, (int64_t)sizeof(err));
    if (!handle[0]) {
        // 加载失败:把错误作为一条响应发回去,宿主据此报错
        std::string resp = "{\"ok\":false,\"error\":\"";
        resp += json_escape(err);
        resp += "\"}";
        resp += "\n";
        std::fwrite(resp.data(), 1, resp.size(), stdout);
        std::fflush(stdout);
        return 3;
    }

    // 告诉宿主加载成功了(第一行响应)
    {
        std::string resp = "{\"ok\":true,\"loaded\":\"";
        resp += json_escape(handle);
        resp += "\"}\n";
        std::fwrite(resp.data(), 1, resp.size(), stdout);
        std::fflush(stdout);
    }

    std::vector<char> buf(256 * 1024);
    std::string line;
    while (true) {
        // 逐字节读,按 \n 切分。管道是流式的,不能一次读一大块再切 ——
        // 那样会阻塞等更多数据。
        int c = std::fgetc(stdin);
        if (c == EOF) break;               // 宿主关闭了管道
        if (c != '\n') {
            line += (char)c;
            if (line.size() > buf.size()) {
                // 单行超长:丢弃并报错,别无限涨
                std::string resp = "{\"ok\":false,\"error\":\"request too long\"}\n";
                std::fwrite(resp.data(), 1, resp.size(), stdout);
                std::fflush(stdout);
                line.clear();
            }
            continue;
        }

        if (line.empty()) { line.clear(); continue; }

        std::string op = extract_str(line, "op");
        std::string resp;

        if (op == "symbols") {
            std::vector<char> sb(64 * 1024);
            ffi_symbols(handle, (int64_t)strlen(handle), sb.data(), (int64_t)sb.size());
            resp = "{\"ok\":true,\"symbols\":";
            resp += sb.data();
            resp += "}";
        } else if (op == "describe") {
            std::string fn = extract_str(line, "fn");
            std::vector<char> sb(64 * 1024);
            ffi_describe(handle, (int64_t)strlen(handle),
                         fn.c_str(), (int64_t)fn.size(),
                         sb.data(), (int64_t)sb.size());
            resp = "{\"ok\":true,\"desc\":";
            resp += sb.data();
            resp += "}";
        } else if (op == "call") {
            std::string fn = extract_str(line, "fn");
            std::string args = extract_raw(line, "args");
            if (args.empty()) args = "[]";
            std::vector<char> sb(256 * 1024);
            ffi_call(handle, (int64_t)strlen(handle),
                     fn.c_str(), (int64_t)fn.size(),
                     args.c_str(), (int64_t)args.size(),
                     sb.data(), (int64_t)sb.size());
            resp = sb.data();
        } else if (op == "quit") {
            resp = "{\"ok\":true}\n";
            std::fwrite(resp.data(), 1, resp.size(), stdout);
            std::fflush(stdout);
            break;
        } else {
            resp = "{\"ok\":false,\"error\":\"unknown op\"}";
        }

        resp += "\n";
        std::fwrite(resp.data(), 1, resp.size(), stdout);
        std::fflush(stdout);   // 必须刷:宿主在等这一行
        line.clear();
    }

    return 0;
}
