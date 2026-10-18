// ffi_test_dll.cpp —— 测试用的动态库,演示 FFI 的完整契约
//
// 编译(Linux):
//   g++ -std=c++20 -fPIC -shared ffi_test_dll.cpp -o ffi_test_dll.so
// 编译(Windows):
//   g++ -std=c++20 -shared ffi_test_dll.cpp -o ffi_test_dll.dll
//
// 导出两样东西:
//   1) xmex_ffi_describe —— 告诉宿主这个库有哪些函数、参数与返回类型
//   2) 一组按统一调用约定导出的函数
//
// 调用约定(宿主 ffi.h 里的定义):
//   int64_t fn(int64_t* args, int64_t nargs, int64_t* out)
//
// 参数编码:
//   int   → args[i] 就是数值
//   bool  → 0/1
//   float → args[i] 是 double 的位模式
//   str   → args[i] 是 char*,指向本次调用期间有效的缓冲

#include <cstdint>
#include <cstring>
#include <cstdio>

extern "C" {

// ---- 被调函数 ----

// add(int, int) -> int
int64_t dll_add(int64_t* args, int64_t nargs, int64_t* out) {
    if (nargs < 2) return 1;
    out[0] = args[0] + args[1];
    return 0;
}

// greet(str) -> str   返回一段新串,验证字符串参数与返回
int64_t dll_greet(int64_t* args, int64_t nargs, int64_t* out) {
    if (nargs < 1) return 1;
    const char* who = (const char*)(intptr_t)args[0];
    char buf[256];
    snprintf(buf, sizeof(buf), "hello, %s", who ? who : "(null)");
    // 关键:返回的字符串必须由**调用方**释放,否则跨模块分配器会打架。
    // 这里用固定缓冲:宿主在调用后立刻拷走,约定见 ffi.h。
    static char ret[256];
    snprintf(ret, sizeof(ret), "%s", buf);
    out[0] = (int64_t)(intptr_t)ret;
    return 0;
}

// scale(float, int) -> float
int64_t dll_scale(int64_t* args, int64_t nargs, int64_t* out) {
    if (nargs < 2) return 1;
    double d;
    memcpy(&d, &args[0], sizeof(d));
    out[0] = (int64_t)(intptr_t)0;
    double r = d * (double)args[1];
    int64_t bits = 0;
    memcpy(&bits, &r, sizeof(bits));
    out[0] = bits;
    return 0;
}

// flip(bool) -> bool
int64_t dll_flip(int64_t* args, int64_t nargs, int64_t* out) {
    if (nargs < 1) return 1;
    out[0] = args[0] ? 0 : 1;
    return 0;
}

// fail() -> 故意返回非 0,用来测错误路径
int64_t dll_fail(int64_t* args, int64_t nargs, int64_t* out) {
    (void)args; (void)nargs; (void)out;
    return 42;      // 非 0 表示失败
}

// ---- 描述符 ----
// 返回 JSON 文本的长度;内容在 buf 里。
int64_t xmex_ffi_describe(char* buf, int64_t cap) {
    const char* d =
        "{\"functions\":["
        "{\"name\":\"dll_add\",\"params\":[\"int\",\"int\"],\"ret\":\"int\"},"
        "{\"name\":\"dll_greet\",\"params\":[\"str\"],\"ret\":\"str\"},"
        "{\"name\":\"dll_scale\",\"params\":[\"float\",\"int\"],\"ret\":\"float\"},"
        "{\"name\":\"dll_flip\",\"params\":[\"bool\"],\"ret\":\"bool\"},"
        "{\"name\":\"dll_fail\",\"params\":[],\"ret\":\"int\"}"
        "]}";
    int64_t n = (int64_t)strlen(d);
    if (buf && cap > 0) {
        int64_t copy = n < cap - 1 ? n : cap - 1;
        memcpy(buf, d, (size_t)copy);
        buf[copy] = '\0';
    }
    return n;
}

}  // extern "C"
