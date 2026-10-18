// ffi_test.cpp —— 动态库 FFI 的独立验证
//
// 不依赖 PyLite 运行时,只链 ffi.cpp。链一个真实的动态库(测试同时编译
// 出来),验证:加载 → 读描述符 → introspection → 按类型调用 → 错误路径。
//
// 用法:
//   g++ -std=c++20 -I include -I runtime tests/ffi_test.cpp \
//       runtime/ffi.cpp tests/ffi_test_dll.cpp -o t && ./t

#include "ffi.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        g_pass++;
        std::printf("  [PASS] %s\n", what);
    } else {
        g_fail++;
        std::printf("  [FAIL] %s\n", what);
    }
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// 数某个字符出现几次 —— 用来验证 JSON 的括号配平
static int count_char(const std::string& s, char c) {
    int n = 0;
    for (char x : s) if (x == c) ++n;
    return n;
}

int main(int argc, char** argv) {
    // 动态库路径:优先用命令行给的,否则按平台猜同名文件
    std::string lib_path;
    if (argc > 1) {
        lib_path = argv[1];
    } else {
#ifdef _WIN32
        lib_path = "ffi_test_dll.dll";
#else
        lib_path = "./ffi_test_dll.so";
#endif
    }

    std::printf("=== xmex FFI 验证 ===\n");
    std::printf("动态库: %s\n\n", lib_path.c_str());

    // ---- 1. 加载 ----
    std::printf("=== 1. 加载动态库 ===\n");
    char err[512] = {0};
    char handle_buf[64] = {0};
    ffi_load(lib_path.c_str(), (int64_t)lib_path.size(),
             handle_buf, (int64_t)sizeof(handle_buf), err, (int64_t)sizeof(err));
    if (!handle_buf[0]) {
        std::printf("  [FAIL] 加载失败: %s\n", err);
        std::printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail + 1);
        return 1;
    }
    std::string handle(handle_buf);
    check(true, "加载成功");

    // ---- 2. introspection:有哪些函数 ----
    std::printf("\n=== 2. introspection ===\n");
    std::vector<char> buf(64 * 1024);
    ffi_symbols(handle.c_str(), (int64_t)handle.size(), buf.data(), (int64_t)buf.size());
    std::string syms(buf.data());
    std::printf("  symbols: %s\n", syms.c_str());
    check(syms.find("dll_add") != std::string::npos, "列出了 dll_add");
    check(syms.find("dll_greet") != std::string::npos, "列出了 dll_greet");
    check(syms.find("dll_scale") != std::string::npos, "列出了 dll_scale");
    check(syms.find("dll_fail") != std::string::npos, "列出了 dll_fail");

    // ---- 3. 描述符:签名 ----
    std::printf("\n=== 3. 函数签名 ===\n");
    ffi_describe(handle.c_str(), (int64_t)handle.size(),
                 "dll_add", 7, buf.data(), (int64_t)buf.size());
    std::string desc(buf.data());
    std::printf("  describe(dll_add): %s\n", desc.c_str());
    check(contains(desc, "\"params\":[\"int\",\"int\"]"), "dll_add 签名正确");
    check(contains(desc, "\"ret\":\"int\""), "dll_add 返回类型正确");
    // 严格检查:JSON 必须是完整对象,不能缺右括号
    // (子串匹配看不出来这个问题 —— 曾漏过结尾的 '}')
    check(desc.size() >= 2 && desc.front() == '{' && desc.back() == '}',
          "describe 返回的是完整 JSON 对象");
    check(count_char(desc, '{') == count_char(desc, '}'), "describe 的花括号配平");
    check(count_char(desc, '[') == count_char(desc, ']'), "describe 的方括号配平");

    ffi_describe(handle.c_str(), (int64_t)handle.size(),
                 "dll_greet", 9, buf.data(), (int64_t)buf.size());
    std::string gdesc(buf.data());
    check(contains(gdesc, "\"params\":[\"str\"]"), "dll_greet 参数是 str");
    check(contains(gdesc, "\"ret\":\"str\""), "dll_greet 返回 str");

    // 不存在的函数
    ffi_describe(handle.c_str(), (int64_t)handle.size(),
                 "nope", 4, buf.data(), (int64_t)buf.size());
    check(contains(std::string(buf.data()), "null"), "未知函数返回 null");

    // ---- 4. 调用:各类型 ----
    std::printf("\n=== 4. 按类型调用 ===\n");

    // int
    ffi_call(handle.c_str(), (int64_t)handle.size(), "dll_add", 7,
             "[3,4]", 5, buf.data(), (int64_t)buf.size());
    std::string r(buf.data());
    std::printf("  dll_add(3,4) -> %s\n", r.c_str());
    check(contains(r, "\"ok\":true"), "dll_add 成功");
    check(contains(r, "\"value\":7"), "dll_add 返回 7");

    // str 入参 + str 返回
    ffi_call(handle.c_str(), (int64_t)handle.size(), "dll_greet", 9,
             "[\"world\"]", 9, buf.data(), (int64_t)buf.size());
    r = buf.data();
    std::printf("  dll_greet(\"world\") -> %s\n", r.c_str());
    check(contains(r, "\"ok\":true"), "dll_greet 成功");
    check(contains(r, "hello, world"), "dll_greet 返回正确字符串");

    // float
    ffi_call(handle.c_str(), (int64_t)handle.size(), "dll_scale", 9,
             "[2.5,4]", 9, buf.data(), (int64_t)buf.size());
    r = buf.data();
    std::printf("  dll_scale(2.5,4) -> %s\n", r.c_str());
    check(contains(r, "\"ok\":true"), "dll_scale 成功");
    check(contains(r, "10"), "dll_scale 返回 10");

    // bool
    ffi_call(handle.c_str(), (int64_t)handle.size(), "dll_flip", 8,
             "[true]", 6, buf.data(), (int64_t)buf.size());
    r = buf.data();
    std::printf("  dll_flip(true) -> %s\n", r.c_str());
    check(contains(r, "\"ok\":true"), "dll_flip 成功");
    check(contains(r, "\"value\":false"), "dll_flip(true) 返回 false");

    // ---- 5. 错误路径 ----
    std::printf("\n=== 5. 错误路径 ===\n");
    ffi_call(handle.c_str(), (int64_t)handle.size(), "dll_fail", 8,
             "[]", 2, buf.data(), (int64_t)buf.size());
    r = buf.data();
    std::printf("  dll_fail() -> %s\n", r.c_str());
    check(contains(r, "\"ok\":false"), "被调函数返回非 0 → 报失败");
    check(contains(r, "code=42"), "错误里带上了返回码");

    ffi_call(handle.c_str(), (int64_t)handle.size(), "no_such_fn", 9,
             "[]", 2, buf.data(), (int64_t)buf.size());
    r = buf.data();
    check(contains(r, "\"ok\":false"), "调用不存在的函数报错");
    check(contains(r, "symbol not found"), "错误信息说明了原因");

    // ---- 6. 全局列表 ----
    std::printf("\n=== 6. 全局 introspection ===\n");
    ffi_list(buf.data(), (int64_t)buf.size());
    r = buf.data();
    std::printf("  ffi_list -> %s\n", r.c_str());
    check(contains(r, handle), "列表里能看到这个库");
    check(contains(r, "\"functions\":5"), "列出了 5 个函数");
    check(contains(r, "\"has_descriptor\":true"), "标记了有描述符");

    // ---- 7. 卸载 ----
    std::printf("\n=== 7. 卸载 ===\n");
    check(ffi_unload(handle.c_str(), (int64_t)handle.size()) == 1, "卸载成功");
    check(ffi_unload(handle.c_str(), (int64_t)handle.size()) == 0, "重复卸载返回 0");
    ffi_symbols(handle.c_str(), (int64_t)handle.size(), buf.data(), (int64_t)buf.size());
    check(contains(std::string(buf.data()), "[]"), "卸载后查不到函数");

    std::printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
