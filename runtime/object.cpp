// 类实例。
//
// 实例 = 类标识 + 属性表。属性表直接复用现成的字典能力承载(字符串键),
// 这样属性查找、插入、更新、遍历都沿用同一套语义,不必另写一份。
//
// 方法不进属性表:成员方法是编译期生成的函数,由代码生成层直接解析成调用,
// 运行时只负责"这个实例有哪些属性"。
//
// 与项目其它运行时模块一致:结构体定义只在本翻译单元内可见,外部经
// instance_* 访问。
#include "xmex/runtime.h"

#include <cstdio>
#include <cstring>

// 与 value.h 里的前置声明对应,必须是全局作用域。
namespace xmex {
struct Instance {
  int64_t classId;   // 类标识(编译期为每个类分配,用于区分不同类的实例)
  Value attrs;     // 属性表:一个字典
};
}  // namespace xmex

namespace {
xmex::Instance *asInst(const xmex::Value *v) {
  return static_cast<xmex::Instance *>(v->as.ptr);
}
void needInst(const xmex::Value *v, const char *what) {
  if (v->tag != xmex::kInstance) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "%s 需要一个类实例,实际得到 '%s'", what,
                  tag_name(v->tag));
    runtime_error(buf);
  }
}
// 属性名转成 xmex::Value 字符串,供字典键使用。
xmex::Value nameKey(const char *name, int64_t nameLen) {
  return str_new(name, nameLen);
}
}  // namespace

// 创建一个实例。attrs 必须是一个字典(由调用方先用 dict_new 建好)。
extern "C" xmex::Value instance_new(int64_t classId, const xmex::Value *attrs) {
  if (attrs == nullptr || attrs->tag != xmex::kDict) {
    runtime_error("实例的属性表必须是一个字典");
  }
  // SCAN_WORDS:xmex::Instance 的第二个字是属性字典指针,必须照到 ——
  // 靠它才能沿指针找到属性表那个块(不需要为实例单独写 tracer)。
  auto *p = static_cast<xmex::Instance *>(
      gc_alloc(sizeof(xmex::Instance), xmex::kScanWords));
  p->classId = classId;
  p->attrs = *attrs;
  return ptr(xmex::kInstance, p);
}

// 读取类标识。
extern "C" int64_t instance_class_id(const xmex::Value *inst) {
  needInst(inst, "读取类标识");
  return asInst(inst)->classId;
}

// 设置属性:存在则更新,不存在则插入。
extern "C" void instance_set_attr(const xmex::Value *inst, const char *name,
                                      int64_t nameLen, const xmex::Value *val) {
  needInst(inst, "设置属性");
  xmex::Value k = nameKey(name, nameLen);
  dict_set(&asInst(inst)->attrs, &k, val);
}

// 读取属性。不存在时报错 —— 与"方法名写错是运行时错误"保持一致的风格:
// 类型注解不检查,实例上有没有这个属性只有运行时才知道。
extern "C" xmex::Value instance_get_attr(const xmex::Value *inst, const char *name,
                                       int64_t nameLen) {
  needInst(inst, "读取属性");
  xmex::Value k = nameKey(name, nameLen);
  if (!dict_has(&asInst(inst)->attrs, &k)) {
    static thread_local char buf[192];
    std::snprintf(buf, sizeof(buf), "实例没有属性 '%.*s'",
                  static_cast<int>(nameLen), name);
    runtime_error(buf);
  }
  return dict_get(&asInst(inst)->attrs, &k);
}

// 属性是否存在(不报错)。
extern "C" int32_t instance_has_attr(const xmex::Value *inst, const char *name,
                                        int64_t nameLen) {
  needInst(inst, "检查属性");
  xmex::Value k = nameKey(name, nameLen);
  return dict_has(&asInst(inst)->attrs, &k);
}

// 属性个数(供调试与遍历使用)。
extern "C" int64_t instance_attr_count(const xmex::Value *inst) {
  needInst(inst, "属性计数");
  return dict_len(&asInst(inst)->attrs);
}
