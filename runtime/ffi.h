// ffi.h —— 动态库 FFI:加载本地 .dll/.so,按描述符暴露函数给 Xmex
//
// 设计目标(对应 kv.add 的两种形态):
//   本地动态库 → 进程内 dlopen,直接调函数
//   远程动态库 → 降级为「远程可执行体」,由 kv_admin 纳管(见 kv_ffi_remote)
//
// 之所以要求动态库导出**描述符**,是因为 Xmex 侧的 `lib.init()` 是
// 运行时动态方法调用 —— 参数个数与类型编译期不知道,只能由动态库自己
// 声明。否则就只能支持固定签名的裸调用,类型全丢。
//
// 描述符导出约定(动态库侧实现其中之一):
//   1) int64_t xmex_ffi_describe(char* buf, int64_t cap)
//      返回 JSON 文本长度,内容形如:
//      {"functions":[{"name":"init","params":["int","string"],"ret":"bool"}]}
//   2) 若不实现 1),则退化为「无描述符模式」:只暴露 int64 单参/无参的
//      裸调用能力,函数名由调用方指定。此时 kv.add 返回的对象只有
//      call/symbols 两个方法,没有按名直接调用的能力。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>

// FFI 调用的统一调用约定。
//
// 所有被调函数按这个签名导出:
//   int64_t fn(int64_t* args, int64_t nargs, int64_t* out)
//     args  —— 入参数组(按描述符的 params 顺序)
//     nargs —— 入参个数
//     out   —— 返回值槽,至少 1 个 int64
//     返回值:0 表示成功,非 0 表示错误码
//
// 为什么固定成一个原型:dlsym 拿到的是裸函数指针,没有类型信息,
// 只能按一个已知原型调用。要求对方按约定导出,就把"任意 C 签名"的
// 不确定性挡在门外 —— 代价是对方要写一层薄封装。
//
// 参数类型编码(与描述符 params 对应):
//   int   → args[i] 就是数值
//   bool  → args[i] 为 0/1
//   float → args[i] 存的是 double 的位模式(用 memcpy 传)
//   str   → args[i] 是 char*,指向本次调用期间有效的缓冲;由运行时管理,
//            函数返回后失效。动态库若要保存必须自己拷贝。
//   ptr   → args[i] 就是裸指针(用于出参缓冲)
extern "C" {

// 加载动态库并把句柄写进 out(至少 32 字节)。
// 失败时 out 为空串,错误原因写进 err。
//
// ⚠️ 刻意**不**返回 const char* —— 返回局部 std::string 的 c_str() 会
// 造成悬空指针(调用方拿到的内容在函数返回瞬间就失效了)。这个坑踩过一次:
// kv.ffi() 拿到的 handle 是空串,后面所有调用都报 "no handle"。
// 所以句柄一律通过输出参数回传。
void ffi_load(const char* path, int64_t pathLen,
              char* out, int64_t outCap, char* err, int64_t errCap);

// 卸载。返回 1 成功,0 失败(句柄不存在)。
int64_t ffi_unload(const char* handle, int64_t handleLen);

// 列出该库导出的、可被调用的函数名。返回 JSON 数组文本。
// 形如: ["init","process","shutdown"]
const char* ffi_symbols(const char* handle, int64_t handleLen, char* buf, int64_t bufCap);

// 取单个函数的描述(JSON 对象)。没有描述符时返回 "null"。
const char* ffi_describe(const char* handle, int64_t handleLen,
                         const char* fn, int64_t fnLen, char* buf, int64_t bufCap);

// 按名字调用。args_json 是 JSON 数组,返回值是 JSON。
// 形如: {"ok":true,"value":42} 或 {"ok":false,"error":"..."}
const char* ffi_call(const char* handle, int64_t handleLen,
                     const char* fn, int64_t fnLen,
                     const char* args_json, int64_t argsLen,
                     char* buf, int64_t bufCap);

// 列出所有已加载的库(句柄 + 路径 + 函数数)。返回 JSON 数组。
const char* ffi_list(char* buf, int64_t bufCap);

}  // extern "C"
