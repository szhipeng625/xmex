// value_tags.h —— 运行时值的 tag 常量
//
// 从 value.h 里拆出来的原因:
//   irgen 只需要 tag 常量(生成比较指令时用),不需要 Value 结构体本身。
//   但如果 irgen 引入 value.h,`xmex::Value`(PyLite 的值类型)就会进入
//   `namespace xmex` 的作用域 —— 而 irgen 内部大量使用裸 `Value` 指
//   `llvm::Value`(LLVM 的 SSA 值)。两者同名,近邻查找规则让裸 `Value`
//   解析到 `xmex::Value`,于是报:
//       no declaration matches 'xmex::Value* xmex::Abi::newSlot(...)'
//
//   拆开之后,irgen 只引本文件,`xmex::Value` 不会出现在它的作用域里,
//   裸 `Value` 稳定解析到 `llvm::Value`。
//
// ⚠️ 加新 tag 时,value.h 与本文件必须同步。value.h 里 include 本文件,
//    不重复定义。
#pragma once

#include <cstdint>

namespace xmex {

enum Tag : int32_t {
  kNull     = 0,
  kBool     = 1,
  kInt      = 2,
  kFloat    = 3,
  kStr      = 4,
  kList     = 5,
  kDict     = 6,
  kTuple    = 7,
  kFunc     = 8,
  kInstance = 9,
};

}  // namespace xmex
