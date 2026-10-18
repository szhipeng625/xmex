// PyLite 运行时库的 C ABI。
//
// IRGen 会对这里每个函数生成 call;用户手写的 C++ 也可以直接调用它们。
// 所有函数都从 libpylite_runtime.dll 导出(靠 MinGW 的 -shared 默认自动导出,
// 刻意不加 __declspec(dllexport) —— 那会触发 MinGW "遇到第一个 dllexport
// 就停止自动导出其余符号"的行为,反而让 JIT 找不到别的符号)。
//
// ⚠️ 跨边界约定 —— 改动前务必先读 docs/llvm-notes.md 第 2 节
//
// 下面这些函数在 C++ 源码里是按值的 `lite::Value f(lite::Value, lite::Value)`,
// 但 IRGen 在 IR 里必须把它们声明成:
//
//     void @f(ptr sret(%lite::Value), ptr, ptr)
//
// 即参数是"指向 lite::Value 的指针"、返回值写进 sret 指针。
// 两者在机器层面一致,因为 Win64 ABI 规定 16 字节聚合体按引用传递、
// 返回值走隐藏 sret 寄存器(RCX = sret,RDX/R8/... = 各参数)。
//
// 若在 IR 里按值声明(`%lite::Value @f(%lite::Value, %lite::Value)`),LLVM 会选另一套
// 降低方式,与 GCC 不匹配 —— 链接通过、lookup 成功,但一执行就段错误。
//
// 其它约定:
//   * 谓词一律返回 int32_t 而非 bool(i1 的 ABI 标注 GCC 与 Clang 不一致)
//   * 数组参数(如 print_ 的 lite::Value*)是普通指针,不受上面限制
#pragma once

#include "pylite/value.h"

namespace lite {

enum GcScan : int32_t {
  kScanNone = 0,
  kScanWords = 1,
  kScanValues = 2,
};

struct GcStats {
  int64_t alloc_bytes_total;
  int64_t freed_bytes_total;
  int64_t live_bytes;
  int64_t live_blocks;
  int64_t collections;
  int32_t dry_run;
  int32_t disabled;
};

typedef Value (*InstanceMethodFn)(const Value *inst, Value *args, int64_t nargs);

}  // namespace lite

extern "C" {

// --- 错误 ---------------------------------------------------------------
// [[noreturn]] 不只是给编译器看的优化提示:没有它,GCC 会认为
// badOperands 这类同样标了 [[noreturn]] 的辅助函数"会返回",从而报警告。
[[noreturn]] void runtime_error(const char *msg);
const char *tag_name(int32_t tag);

// --- 构造 ---------------------------------------------------------------
// 刻意没有 int_/float_/bool_ 的运行时版本 —— 它们作为 inline 函数
// 定义在 value.h 里。IRGen 用 insertvalue 指令内联构造 lite::Value,不发起调用;
// 若在这里再声明一份 extern "C" 版本,会与 value.h 的 C++ 链接版本构成
// 同名不同语言链接的非法重声明。

// --- 算术 ---------------------------------------------------------------
lite::Value add(lite::Value a, lite::Value b);       // 数值相加 / 字符串拼接 / 列表拼接
lite::Value sub(lite::Value a, lite::Value b);
lite::Value mul(lite::Value a, lite::Value b);       // 数值相乘 / 字符串与列表重复
lite::Value div_(lite::Value a, lite::Value b);       // 真除法,恒为 float
lite::Value floordiv(lite::Value a, lite::Value b);  // 向下取整除法
lite::Value mod_(lite::Value a, lite::Value b);       // 取模,符号跟随除数
lite::Value pow_(lite::Value a, lite::Value b);
lite::Value neg(lite::Value a);                  // 一元负号

// a += b。与 add 的区别**只在列表上**:Python 的 `a += [x]` 是就地 extend,
// 别名看得见改动;而 `a = a + [x]` 是重新绑定,别名不受影响。
// 第一个参数因此是可写的 —— 列表就地改,其余类型(数值、字符串不可变)
// 一律返回新值,由调用点写回自己的槽位。
lite::Value iadd(lite::Value *a, const lite::Value *b);

// --- 比较(返回 bool 值的 lite::Value)----------------------------------------
lite::Value eq(lite::Value a, lite::Value b);
lite::Value ne(lite::Value a, lite::Value b);
lite::Value lt(lite::Value a, lite::Value b);
lite::Value le(lite::Value a, lite::Value b);
lite::Value gt(lite::Value a, lite::Value b);
lite::Value ge(lite::Value a, lite::Value b);

// 三路比较,返回 -1 / 0 / 1。数值之间比大小、两个字符串按字典序比;
// 类型不可比时报运行时错误(供 < <= > >= 与 list.sort 共用)。
int32_t compare_(const lite::Value *a, const lite::Value *b);

// --- 逻辑 ---------------------------------------------------------------
// and / or 的短路由 IRGen 用 PHI 实现(且必须返回操作数本身而非布尔值),
// 所以运行时只提供 not。
lite::Value not_(lite::Value a);

// --- 真值判定 -----------------------------------------------------------
int32_t truthy(lite::Value v);

// --- 转换 ---------------------------------------------------------------
// 取出整数负载;非整数时报运行时错误(用于 range 的边界)
int64_t to_int(lite::Value v);

// --- 输出 ---------------------------------------------------------------
// 打印 n 个值,空格分隔,末尾换行。
void print_(lite::Value *vals, int64_t n);

// 按 repr 规则渲染成一个新字符串:容器里的字符串带引号并转义,
// 与 print 的顶层 str 规则不同 —— print("hi") 打 hi,print(["hi"]) 打 ['hi']。
// 目前供字典的 KeyError 类错误信息使用。
lite::Value repr(const lite::Value *v);

// --- 输入 ---------------------------------------------------------------
// input(int, int) 在 IRGen 里被拆成若干个具体读取调用,结果打包成元组
// 再交给多目标赋值解包。全部返回 lite::Value,在 IR 里声明成 void(ptr sret)。
lite::Value read_int();
lite::Value read_float();
lite::Value read_bool();
lite::Value read_str();

// --- 内存与垃圾回收 ------------------------------------------------------
// 保守式标记-清除(见 runtime/gc.cpp 的说明)。只有 str/list/dict/tuple 是堆
// 对象,int/float/bool/None 是内联标量、根本不分配。
//
// 根 = 保守扫描本线程的栈与寄存器 + 显式根栈。**根必须放在栈上** —— 全局变量、
// 静态变量、thread_local 里放 lite::Value 不会被认为是根,对象会被回收掉。
//
// 下面这些是运行时内部用的;用户手写 C++ 只需要 alloc。

// 分配 n 字节并登记为一个 GC 块。n 会向上取整到 16 的倍数。
void *gc_alloc(size_t n, lite::GcScan kind);
// SCAN_VALUES 的专用入口:从 firstCellOffset 起是若干 16 字节的 lite::Value 单元格。
// ⚠️ 元组传 8(lite::Tuple 是 {int64 len; lite::Value items[]}),传 0 会把 len 当 tag、
// 整条链错位,那是会丢根的。
void *gc_alloc_values(size_t n, size_t firstCellOffset);

void gc_collect();          // 手动触发一次回收
void gc_stats(lite::GcStats *out);
void gc_init();             // 可选:提前读环境变量并注册 atexit 统计

// 显式根栈。用于运行时内部的 C++ 局部变量 —— 见 gc.cpp 里"最可能出错的一处"。
// slot 必须指向一个局部指针变量;取地址这个动作会迫使该值留在内存里,
// 于是不再依赖编译器恰好在 -O2 下把它 spill 到栈上。
void gc_root_push(void **slot);
void gc_root_pop(size_t n);

// 历史符号,保留以维持 ABI(用户手写 C++ 会直接调 alloc)。
// alloc 按最保守的方式扫描 —— 宁可多保留,不可漏根。
void *alloc(size_t n);
void arena_reset();         // 语义已改为"强制回收一次"
// --- 引用计数(共享指针 / 弱指针语义) -----------------------------------
// 计数位记在**块头**里,对象自身数据布局与 lite::Value 布局都不动,不触碰 ABI 契约。
// 生命周期:
//   * 强引用归零                 -> 对象内容失效(仍可被弱引用观察)
//   * 强引用归零且已无弱引用     -> 真正归还内存
//   * 弱引用归零                 -> 对象早已失效时,此刻归还内存
// ⚠️ 强引用计数为 0 表示"尚无强引用登记",此时减引用不做任何事,交回标记-清除;
//    因此即使生成代码漏插计数调用,也只是退回原行为,绝不会误释放活对象。
// ⚠️ 循环引用回收不了,由标记-清除按"从根可达"兜底,整体不会泄漏。
// payload 是堆对象指针(与 lite::Value.as.ptr 一致)。
void incref(void *payload);        // 加强引用
void decref(void *payload);        // 减弱引用(归零即失效/释放)
void weakref(void *payload);       // 登记一个弱引用(不影响对象生命周期)
void weak_release(void *payload);  // 释放一个弱引用
// 弱引用升级为强引用:对象已失效返回 0,否则加引用并返回 1 —— 绝不返回悬空指针。
int32_t weak_upgrade(void *payload);
int32_t weak_alive(void *payload); // 弱引用是否仍指向活对象
int64_t refcount(void *payload);   // 读取强引用计数

// --- 元组 ---------------------------------------------------------------
// 全部按指针收发,避免聚合体按值跨 ABI 边界
lite::Value tuple_new(lite::Value *elems, int64_t n);
int64_t tuple_len(const lite::Value *t);
lite::Value tuple_get(const lite::Value *t, int64_t i);   // 支持负索引
void unpack(const lite::Value *t, lite::Value *out, int64_t n);

// --- 字符串 -------------------------------------------------------------
lite::Value str_new(const char *bytes, int64_t len);
int64_t str_size(const lite::Value *s);
const char *str_data(const lite::Value *s);
lite::Value str_concat(const lite::Value *a, const lite::Value *b);
lite::Value str_get(const lite::Value *s, int64_t i);          // 取单个字符,支持负索引
lite::Value str_repeat(const lite::Value *s, int64_t times);
// 按字节序比较,返回 -1 / 0 / 1
int32_t str_compare(const lite::Value *a, const lite::Value *b);
int32_t str_find(const lite::Value *s, const lite::Value *needle, int64_t from);
int32_t str_starts_with(const lite::Value *s, const lite::Value *prefix);
int32_t str_ends_with(const lite::Value *s, const lite::Value *suffix);

// --- 列表 ---------------------------------------------------------------
// lite::List 是可变对象:append/insert/pop/... 就地改,别名的调用方看得见改动。
// 增长靠"新分配 + 拷贝",因为 bump 分配器没有 realloc(旧数组就地泄漏,
// 与项目其它部分一致,见 runtime/arena.cpp)。
lite::Value list_new(lite::Value *elems, int64_t n);
int64_t list_len(const lite::Value *v);
lite::Value list_get(const lite::Value *v, int64_t i);          // 支持负索引
void list_set(const lite::Value *v, int64_t i, const lite::Value *val);
lite::Value list_concat(const lite::Value *a, const lite::Value *b);  // 新列表
lite::Value list_repeat(const lite::Value *a, int64_t times);     // 新列表

void list_append(const lite::Value *v, const lite::Value *item);
void list_extend(const lite::Value *v, const lite::Value *other);
void list_insert(const lite::Value *v, int64_t i, const lite::Value *item);
void list_remove(const lite::Value *v, const lite::Value *item);
// idxOrNone 传的是一个 **None 值**(none_()),不是空指针 —— 长度为 0 的
// 下标在语言里表达不出来,所以借 None 当"没给下标"的哨兵。传 nullptr 会崩。
lite::Value list_pop(const lite::Value *v, const lite::Value *idxOrNone);
int64_t list_index(const lite::Value *v, const lite::Value *item);     // 找不到报错
int64_t list_count(const lite::Value *v, const lite::Value *item);
void list_reverse(const lite::Value *v);
void list_clear(const lite::Value *v);
void list_sort(const lite::Value *v);

// --- 字典 ---------------------------------------------------------------
// 插入有序 + 线性扫描。不用哈希表:维护 Python 3.7 的可观测插入序,
// 且省掉为每种键类型写 hash 的面上。代价是查找 O(n),见 docs/language.md。
lite::Value dict_new(lite::Value *keys, lite::Value *vals, int64_t n);
int64_t dict_len(const lite::Value *d);
lite::Value dict_get(const lite::Value *d, const lite::Value *key);   // 键不存在则报错
lite::Value dict_get_default(const lite::Value *d, const lite::Value *key,
                            const lite::Value *def);
int32_t dict_has(const lite::Value *d, const lite::Value *key);
void dict_set(const lite::Value *d, const lite::Value *key, const lite::Value *val);
lite::Value dict_key_at(const lite::Value *d, int64_t i);   // 按插入序取第 i 个键
lite::Value dict_val_at(const lite::Value *d, int64_t i);
// pop 必须给键。这里刻意不用 None 当"没给键"的哨兵 —— None 本身是合法的键,
// 两者混淆会让 `d.pop(None)` 变得无法表达。
lite::Value dict_pop(const lite::Value *d, const lite::Value *key);
void dict_clear(const lite::Value *d);

// --- 下标 / 切片 / 长度 --------------------------------------------------
// "不支持下标"这类错误的唯一产生点。按 tag 分派到上面各类型的访问器。
int64_t len(const lite::Value *v);       // str / list / dict / tuple
lite::Value index_(const lite::Value *obj, const lite::Value *idx);
// obj[idx] = val。列表按位置改,字典按键插入或更新(新增键追加在末尾)。
void setindex(const lite::Value *obj, const lite::Value *idx, const lite::Value *val);
// lo / hi / step 省略时传 None(即 lite::kNull),各自按默认值处理
lite::Value slice(const lite::Value *obj, const lite::Value *lo, const lite::Value *hi,
                 const lite::Value *step);

// for 循环用的"第 i 个元素"。**不能**拿 index_ 代替:遍历字典要的是第 i 个
// 键,而下标 d[k] 是按键查找,两者语义不同。
lite::Value iter_at(const lite::Value *obj, int64_t i);

// --- 方法调用 -----------------------------------------------------------
// obj.print(...) 形式统一走这一个入口,按 tag + 方法名分派。
// 方法表在运行时里,所以拼错方法名是**运行时**报错 —— 本项目的类型注解
// 本来就不做检查(见 docs/language.md),这是动态语言的必然。
lite::Value call_method(const lite::Value *obj, const char *name, int64_t nameLen,
                       lite::Value *args, int64_t nargs);

// --- Docker 容器调用 -----------------------------------------------------
// 通过 popen 调用 docker CLI，不引入额外依赖。
// 部署到有 Docker daemon 的环境中即可直接使用。
lite::Value docker_run(const lite::Value *image, const lite::Value *cmd);
lite::Value docker_ps();
void docker_stop(const lite::Value *containerId);
lite::Value docker_logs(const lite::Value *containerId);
lite::Value docker_pull(const lite::Value *image);

// Docker 项目封装
lite::Value docker_generate_dockerfile(const lite::Value *baseImage,
                                       const lite::Value *projectName,
                                       const lite::Value *setupCommands,
                                       const lite::Value *entrypoint,
                                       const lite::Value *exposePort);
lite::Value docker_build(const lite::Value *dockerfile, const lite::Value *imageName,
                         const lite::Value *tag, const lite::Value *contextDir);
lite::Value docker_push(const lite::Value *imageName, const lite::Value *tag,
                        const lite::Value *registry);
lite::Value docker_project_package(const lite::Value *projectDir,
                                   const lite::Value *imageName,
                                   const lite::Value *tag,
                                   const lite::Value *baseImage,
                                   const lite::Value *setupCommands,
                                   const lite::Value *entrypoint,
                                   const lite::Value *exposePort);

// --- LLM 神经网络接口 -----------------------------------------------------
// 通过 curl 调用 OpenAI 兼容 API，支持网络结构描述与修改。
lite::Value llm_chat(const lite::Value *prompt, const lite::Value *systemPrompt,
                    const lite::Value *model, const lite::Value *apiKey,
                    const lite::Value *endpoint);
lite::Value llm_create_network(const lite::Value *name, const lite::Value *layers);
lite::Value llm_add_layer(const lite::Value *network, const lite::Value *layer);
lite::Value llm_remove_layer(const lite::Value *network, const lite::Value *index);
lite::Value llm_network_summary(const lite::Value *network);

// LLM 微调（Fine-tuning）
lite::Value llm_create_dataset(const lite::Value *examples, const lite::Value *systemPrompt);
lite::Value llm_upload_file(const lite::Value *data, const lite::Value *filename,
                           const lite::Value *apiKey, const lite::Value *endpoint);
lite::Value llm_create_finetune(const lite::Value *fileId, const lite::Value *model,
                                const lite::Value *suffix, const lite::Value *apiKey,
                                const lite::Value *endpoint);
lite::Value llm_finetune_status(const lite::Value *jobId, const lite::Value *apiKey,
                                const lite::Value *endpoint);
lite::Value llm_list_finetunes(const lite::Value *apiKey, const lite::Value *endpoint);

// --- CUDA 编程支持 --------------------------------------------------------
// 通过 CUDA Driver API 和 LLVM NVPTX 后端实现 GPU 编程。
// 需要 NVIDIA GPU 和 CUDA 驱动。
lite::Value cuda_info();
lite::Value cuda_compile_ptx(const lite::Value *kernelCode, const lite::Value *kernelName);
lite::Value cuda_launch_kernel(const lite::Value *ptxCode, const lite::Value *kernelName,
                               const lite::Value *gridX, const lite::Value *gridY,
                               const lite::Value *gridZ, const lite::Value *blockX,
                               const lite::Value *blockY, const lite::Value *blockZ,
                               const lite::Value *args);
lite::Value cuda_alloc(const lite::Value *size);
void cuda_free(const lite::Value *handle);
void cuda_memcpy_to_device(const lite::Value *handle, const lite::Value *data);
lite::Value cuda_memcpy_from_device(const lite::Value *handle, const lite::Value *size);

// --- 算子融合优化 ---------------------------------------------------------
// LLM 推理加速：算子 IR 描述、融合规则引擎、CUDA 内核生成、自动调优。
lite::Value fusion_create_graph(const lite::Value *name);
lite::Value fusion_add_op(const lite::Value *graph, const lite::Value *opType,
                          const lite::Value *opName, const lite::Value *params);
lite::Value fusion_apply_rules(const lite::Value *graph);
lite::Value fusion_generate_kernel(const lite::Value *fusedOp, const lite::Value *config);
lite::Value fusion_autotune(const lite::Value *opType, const lite::Value *M,
                            const lite::Value *N, const lite::Value *K);
lite::Value fusion_compile_and_run(const lite::Value *kernelCode,
                                   const lite::Value *kernelName,
                                   const lite::Value *M, const lite::Value *N,
                                   const lite::Value *K);
lite::Value fusion_benchmark(const lite::Value *kernelCode, const lite::Value *kernelName,
                             const lite::Value *M, const lite::Value *N,
                             const lite::Value *K, const lite::Value *iterations);

// --- LLM 推理运行时 -------------------------------------------------------
// 模型加载、Token 生成、KV Cache 管理、FlashAttention、多 GPU、性能分析。
lite::Value inference_load_model(const lite::Value *modelPath, const lite::Value *configPath);
lite::Value inference_generate(const lite::Value *prompt, const lite::Value *maxTokens,
                               const lite::Value *temperature, const lite::Value *topP,
                               const lite::Value *topK);
lite::Value inference_kv_cache_stats();
lite::Value inference_flash_attention_kernel();
lite::Value inference_multi_gpu_info();
lite::Value inference_profile();
void inference_unload();

// --- Tokenizer 分词器 -----------------------------------------------------
// BPE 编码/解码，支持 HuggingFace tokenizer.json 格式。
lite::Value tokenizer_load(const lite::Value *path);
lite::Value tokenizer_encode(const lite::Value *text);
lite::Value tokenizer_decode(const lite::Value *tokenIds);
lite::Value tokenizer_info();

// --- REST API 服务 --------------------------------------------------------
// OpenAI 兼容的 HTTP 推理接口，零外部依赖。
lite::Value server_start(const lite::Value *port);
void server_stop();
lite::Value server_status();

// --- 模型量化工具与多架构支持 ---------------------------------------------
// FP16→AWQ 4-bit 转换、主流架构配置模板、量化精度验证。
lite::Value quantize_fp16_to_awq(const lite::Value *weightsJson, const lite::Value *bits,
                                 const lite::Value *groupSize);
lite::Value quantize_arch_template(const lite::Value *archName);
lite::Value quantize_verify_precision(const lite::Value *fp16Values,
                                      const lite::Value *awqValues);
lite::Value quantize_model_info(const lite::Value *modelPath);

// --- PyLite 原生推理服务引擎 ---------------------------------------------
// vLLM 风格的 LLM 推理引擎：PagedAttention KV Cache + 连续批处理。
lite::Value engine_init(const lite::Value *configJson);
lite::Value engine_add_request(const lite::Value *prompt, const lite::Value *maxTokens,
                               const lite::Value *temperature, const lite::Value *topP,
                               const lite::Value *topK);
lite::Value engine_step();
lite::Value engine_status();
void engine_reset();
lite::Value engine_serve(const lite::Value *port);

// --- 线程 / 进程 / 资源管理 -----------------------------------------------
// 线程任务用注册表管理:这门语言目前还没有一等函数,线程入口无法在语言里
// 直接写,由宿主 C++ 按名字注册、语言层按名字启动。
// 所有跨线程共享状态由一把互斥锁保护,句柄登记在带锁的表里、结束可回收。
// 线程任务函数的真实签名是 void (*)(void *),arg 原样透传。
int64_t thread_register_task(const char *name, void *fn, void *arg);
lite::Value thread_start(const char *taskName);   // 返回线程号
int64_t thread_join(int64_t id);              // 等待结束并回收条目
int64_t thread_alive(int64_t id);             // 1 = 仍在运行
int64_t thread_count();                       // 登记表中的线程数
// 进程:同步执行(返回标准输出)、后台启动(返回进程号)、等待(返回退出码)、
// 判活、终止。
lite::Value process_run(const lite::Value *cmd);
int64_t process_spawn(const lite::Value *cmd);
int64_t process_wait(int64_t id);
int64_t process_alive(int64_t id);
int64_t process_kill(int64_t id);
// 资源管理:快照(线程与进程计数)与回收已结束的线程条目。
lite::Value resource_stats();
int64_t resource_reap();

// --- 类实例 ---------------------------------------------------------------
// 实例 = 类标识 + 属性表(用字典承载)。属性名是字符串,实例有没有某个属性
// 只有运行时才知道 —— 与"方法名写错是运行时错误"是同一套取舍。
// 方法不进属性表:成员方法由编译期直接解析成函数调用,运行时只负责属性。
lite::Value instance_new(int64_t classId, const lite::Value *attrs);
int64_t instance_class_id(const lite::Value *inst);
void instance_set_attr(const lite::Value *inst, const char *name, int64_t nameLen,
                          const lite::Value *val);
lite::Value instance_get_attr(const lite::Value *inst, const char *name, int64_t nameLen);
int32_t instance_has_attr(const lite::Value *inst, const char *name, int64_t nameLen);
int64_t instance_attr_count(const lite::Value *inst);

// --- 互斥锁与条件变量 -----------------------------------------------------
// 句柄是一个整数,实际对象登记在运行时内部,靠句柄访问 —— 语言层不必认识原生指针。
// 条件变量的等待会原子地放开传入的那把锁,被唤醒后重新拿回它;
// 这正是"检查条件"与"等待"之间不丢唤醒的关键。
// ⚠️ 等条件变量前必须先拿到配套的那把锁:检查条件与进入等待之间不能有缝,
// 否则通知可能在两者之间发出,等待者就永远等下去了。
lite::Value sync_mutex_new();                              // 创建锁,返回句柄
lite::Value sync_mutex_lock(const lite::Value *h);             // 加锁(阻塞直到拿到)
lite::Value sync_mutex_unlock(const lite::Value *h);           // 解锁
lite::Value sync_mutex_free(const lite::Value *h);             // 销毁锁
lite::Value sync_cond_new();                               // 创建条件变量,返回句柄
lite::Value sync_cond_wait(const lite::Value *ch, const lite::Value *mh);  // 等待通知
lite::Value sync_cond_signal(const lite::Value *ch);           // 唤醒一个等待者
lite::Value sync_cond_broadcast(const lite::Value *ch);        // 唤醒全部等待者
lite::Value sync_cond_free(const lite::Value *ch);             // 销毁条件变量
lite::Value sync_stats();                                  // 同步原语数量快照

// --- 运行时通信 -----------------------------------------------------------
// 把程序运行情况(函数进入/退出、变量赋值、错误)实时推送到前端。
// 启动后会在指定端口上跑一个极简 HTTP 服务,前端通过服务器推送事件连接。
// port 为 0 表示用默认端口 18900。
void trace_start(int port);                            // 启动通信服务
void trace_stop();                                     // 停止通信服务
int64_t trace_enter(const char *name, int64_t parentId); // 函数进入,返回帧标识
void trace_exit(int64_t id, const char *name, int ok, int64_t elapsed); // 函数退出
void trace_vars(const char *varsJson);                 // 变量赋值(JSON 键值对)
void trace_error(const char *msg);                     // 错误事件
int32_t trace_enabled();                               // 是否已启用
// --- 供编译期插桩使用的便捷接口 ---
int64_t trace_begin(const char *name);                 // 函数进入:自动计时与层级
int64_t trace_begin_args(const char *name, int count,
                            const char *const *paramNames,
                            const lite::Value *const *paramValues);  // 函数进入:附带形参名与实参值
void trace_end(int64_t id, const char *name, int ok);  // 函数退出:自动算耗时
void trace_var(const char *name, const char *value);   // 变量上报(字符串值)
void trace_var_value(const char *name, const lite::Value *v); // 变量上报(直接收值)
void trace_set_dump(const char *path);                 // 设置事件落盘文件路径
// cuda / docker 操作事件:category 取 cuda 或 docker,op 为操作名,
// detail 为对象描述,ok 为是否成功,elapsed 为耗时(毫秒)。
void trace_op(const char *category, const char *op, const char *detail,
                 int ok, int64_t elapsed);

// --- 判题沙箱 -------------------------------------------------------------
// 在受控环境中执行用户提交的代码：seccomp 系统调用白名单 + 降权 + 资源限制。
// 这些接口供 .pys 脚本在编译时通过 IRGen 生成调用，也可在 C++ 侧直接使用。

// 创建沙箱实例，返回 lite::kInstance 对象
lite::Value sandbox_create();
// 设置 CPU 时间限制（毫秒）
void sandbox_set_cpu_limit(const lite::Value *inst, uint32_t cpu_limit_ms);
// 设置内存限制（字节）
void sandbox_set_mem_limit(const lite::Value *inst, uint64_t mem_limit_bytes);
// 设置最大进程数（防进程炸弹）
void sandbox_set_max_pids(const lite::Value *inst, int64_t max_pids);
// 设置工作目录
void sandbox_set_work_dir(const lite::Value *inst, const char *path, int64_t pathLen);
// 设置用户可执行文件路径
void sandbox_set_binary(const lite::Value *inst, const char *path, int64_t pathLen);
// 添加命令行参数
void sandbox_add_arg(const lite::Value *inst, const char *arg, int64_t argLen);
// 设置标准输入/输出/错误重定向文件
void sandbox_set_stdin(const lite::Value *inst, const char *path, int64_t pathLen);
void sandbox_set_stdout(const lite::Value *inst, const char *path, int64_t pathLen);
void sandbox_set_stderr(const lite::Value *inst, const char *path, int64_t pathLen);
// 执行沙箱（fork + sandbox_main + exec），返回子进程 pid
int64_t sandbox_exec(const lite::Value *inst);
// 销毁沙箱实例
void sandbox_destroy(const lite::Value *inst);

// --- cgroup 资源管理 ------------------------------------------------------
// 基于 Linux cgroup v2 的 CPU/内存/PID 限制与用量监控。

// 创建判题 cgroup（高 CPU 权重 200），返回 cgroup 路径字符串
lite::Value cgroup_create_judge(const char *task_id, int64_t taskIdLen,
                               uint32_t cpu_limit_ms, uint64_t mem_limit_bytes,
                               int64_t max_pids);
// 将进程加入 cgroup
void cgroup_attach(const char *cg_path, int64_t pathLen, int64_t pid);
// 读取 CPU 使用时间（纳秒）
int64_t cgroup_read_cpu_ns(const char *cg_path, int64_t pathLen);
// 读取内存使用量（字节）
int64_t cgroup_read_mem_bytes(const char *cg_path, int64_t pathLen);
// 销毁 cgroup
void cgroup_destroy(const char *cg_path, int64_t pathLen);

// --- 系统监控 (sysmon) ----------------------------------------------------
// 基于 /proc 文件系统的 Linux 系统监控：CPU、内存、进程、网络、文件系统。
// 所有返回 JSON 字符串的函数，调用方负责通过 str_* 解析。

// CPU 统计（所有核心 + 每个核心 + 使用率）
lite::Value sysmon_cpu_stats();
// 指定进程的 CPU 使用时间（毫秒）
int64_t sysmon_process_cpu_ms(int64_t pid);
// 系统内存信息（总量/可用/缓存/Swap + 使用率）
lite::Value sysmon_mem_info();
// 指定进程的物理内存（RSS，KB）
int64_t sysmon_process_mem_kb(int64_t pid);
// 指定进程的详细内存信息（峰值/虚拟/RSS/数据段/栈）
lite::Value sysmon_process_mem_info(int64_t pid);
// 所有进程 PID 列表
lite::Value sysmon_list_processes();
// 指定进程的详细信息（PID/PPID/名称/状态/CPU/内存/线程/子进程）
lite::Value sysmon_process_info(int64_t pid);
// 进程树的总资源用量（CPU/内存/PID 数）
lite::Value sysmon_tree_usage(int64_t root_pid);
// 网络接口统计（收发字节/包数）
lite::Value sysmon_net_stats();
// 文件系统配额（总量/已用/可用/inode + 使用率）
lite::Value sysmon_fs_quota(const char *path, int64_t pathLen);
// 目录总大小（字节，递归）
int64_t sysmon_dir_size(const char *path, int64_t pathLen);
// 目录文件数量（递归）
int64_t sysmon_dir_file_count(const char *path, int64_t pathLen);
// 系统信息（主机名/内核/发行版/运行时间/CPU 数/总内存）
lite::Value sysmon_sys_info();
// 系统负载（1/5/15 分钟）
lite::Value sysmon_load_avg();

// --- Raft 分布式框架 ------------------------------------------------------
// 基于 Raft 共识算法的分布式集群管理：节点生命周期、状态机命令、
// 节点注册、动态插件加载。所有接口返回 JSON 字符串。

// 构建并启动 Raft 节点（config_json 含 node_id/bind_addr/peers 等）
lite::Value raft_build(const char *config_json, int64_t jsonLen);
// 删除指定节点
lite::Value raft_erase(const char *node_id, int64_t idLen);
// 获取节点状态（角色/任期/leader/提交索引等）
lite::Value raft_status(const char *node_id, int64_t idLen);
// 列出所有本地节点 ID
lite::Value raft_list_nodes();
// 提交命令到集群（仅 leader 可提交，follower 自动转发）
lite::Value raft_add(const char *node_id, int64_t idLen,
                    const char *cmd_type, int64_t typeLen,
                    const char *payload, int64_t payloadLen);
// 注册命令处理器
int64_t raft_register_handler(const char *node_id, int64_t idLen,
                                  const char *cmd_type, int64_t typeLen);
// 注册节点角色
lite::Value raft_register_node(const char *node_id, int64_t idLen,
                               const char *role, int64_t roleLen,
                               const char *addr, int64_t addrLen);
// 获取集群节点列表
lite::Value raft_cluster_nodes(const char *node_id, int64_t idLen);
// 注册共享存储
lite::Value raft_mount_storage(const char *node_id, int64_t idLen,
                               const char *name, int64_t nameLen,
                               const char *path, int64_t pathLen);
// 加载动态插件（集群同步）
lite::Value raft_plugin_load(const char *node_id, int64_t idLen,
                             const char *name, int64_t nameLen,
                             const char *so_path, int64_t pathLen);
// 卸载插件
lite::Value raft_plugin_unload(const char *node_id, int64_t idLen,
                               const char *name, int64_t nameLen);
// 列出已加载插件
lite::Value raft_plugin_list(const char *node_id, int64_t idLen);

// --- 集群管理台 (cluster) -------------------------------------------------
// 轻量级多集群管理控制台：通过指定远程地址连接多个独立集群，
// 统一查看状态、执行命令、批量操作。管理台本身不运行 Raft 共识。

// 连接到远程集群（name: 别名, addr: host:port, secret: 密钥）
lite::Value cluster_connect(const char *name, int64_t nameLen,
                            const char *addr, int64_t addrLen,
                            const char *secret, int64_t secretLen);
// 断开集群连接
lite::Value cluster_disconnect(const char *name, int64_t nameLen);
// 列出所有已连接集群
lite::Value cluster_list();
// 检查集群是否已连接（返回 1/0）
int64_t cluster_is_connected(const char *name, int64_t nameLen);
// 获取集群完整状态（leader/term/节点列表/健康/延迟）
lite::Value cluster_status(const char *name, int64_t nameLen);
// 获取集群节点列表
lite::Value cluster_nodes(const char *name, int64_t nameLen);
// 健康检查（ping 集群 /status 端点）
int64_t cluster_health(const char *name, int64_t nameLen);
// 向集群发送命令（HTTP POST /exec）
lite::Value cluster_exec(const char *name, int64_t nameLen,
                         const char *cmd_type, int64_t typeLen,
                         const char *payload, int64_t payloadLen);
// 向集群发送查询（HTTP GET）
lite::Value cluster_query(const char *name, int64_t nameLen,
                          const char *path, int64_t pathLen);
// 向所有已连接集群广播命令
lite::Value cluster_broadcast(const char *cmd_type, int64_t typeLen,
                              const char *payload, int64_t payloadLen);
// 所有集群健康检查
lite::Value cluster_health_all();

// --- 节点管理 (kv_admin) --------------------------------------------------
// 结合 AdvisKV 的节点生命周期管理：建立节点、进程控制（sleep/wakeup）、
// 逻辑跳过（skip）、永久移除（erase）。节点间通过 Raft 互相通信。

// 建立节点并启动进程（node_id + 可执行文件 + 参数 + 工作目录 + 地址）
lite::Value kv_build(const char *node_id, int64_t idLen,
                    const char *binary, int64_t binLen,
                    const char *args_json, int64_t argsLen,
                    const char *work_dir, int64_t dirLen,
                    const char *addr, int64_t addrLen);
// 向集群追加节点（语义同 build）
lite::Value kv_add(const char *node_id, int64_t idLen,
                  const char *binary, int64_t binLen,
                  const char *args_json, int64_t argsLen,
                  const char *work_dir, int64_t dirLen,
                  const char *addr, int64_t addrLen);
// 永久移除节点（停止进程并删除注册）
lite::Value kv_erase(const char *node_id, int64_t idLen);
// 逻辑跳过节点（标记 LOST，等待副本替换）
lite::Value kv_skip(const char *node_id, int64_t idLen);
// 暂停节点（SIGSTOP，故障注入）
lite::Value kv_sleep(const char *node_id, int64_t idLen);
// 唤醒节点（SIGCONT）
lite::Value kv_wakeup(const char *node_id, int64_t idLen);
// 停止节点（保留注册，可重启）
lite::Value kv_stop(const char *node_id, int64_t idLen);
// 重启节点
lite::Value kv_restart(const char *node_id, int64_t idLen);
// 获取节点详情 JSON
lite::Value kv_status(const char *node_id, int64_t idLen);
// 列出所有节点 JSON
lite::Value kv_list();
// 节点进程是否存活（1/0）
int64_t kv_alive(const char *node_id, int64_t idLen);

// 沙箱绑定与查找：将沙箱句柄关联到分布式节点
int64_t kv_bind_sandbox(const char *node_id, int64_t idLen, int64_t handle);
int64_t kv_get_sandbox(const char *node_id, int64_t idLen);
int64_t kv_unbind_sandbox(const char *node_id, int64_t idLen);


// ---- 分布式便捷内建(2026-10 增补) ----
// sleep_ms(ms):当前线程休眠指定毫秒数
void sleep_ms(int64_t ms);
// time_ms() / time_s():当前 Unix 时间戳(毫秒 / 秒)
lite::Value time_ms();
lite::Value time_s();
// raft 选主查询:返回 {node_id,role,leader_id,is_leader,term}
lite::Value raft_leader_info(const char *node_id, int64_t idLen);
// raft 消息收件箱:send 入队 / recv 出队(空返回空串)
lite::Value raft_send(const char *node_id, int64_t idLen,
                     const char *msg, int64_t msgLen);
lite::Value raft_recv(const char *node_id, int64_t idLen);

// ---- add(box) 统一编排与容器内定位(2026-10 增补) ----
// kv.add_box(node_id, box, addr):一步完成注册节点+绑定盒子,addr 空则本地启动
lite::Value kv_add_box(const char *node_id, int64_t idLen,
                      const lite::Value *box,
                      const char *addr, int64_t addrLen);
// kv.locate(box):由盒子反查所属 kv 节点的 kv->box 绑定关系
lite::Value kv_locate(const lite::Value *box);
// kv.self():容器内定位,读 PYLITE_NODE_ID 环境变量反查所属节点
lite::Value kv_self();

// 按 handle 取盒子登记的二进制路径(供 kv.add_box 使用)
lite::Value sandbox_binary_path(int64_t handle);

// ---- 节点迭代器适配器(2026-10 增补) ----
// kv.iter():创建节点迭代器实例(快照当前节点列表)
lite::Value kv_iter();
// kv.next(it):取下一个节点信息,迭代结束返回 None
lite::Value kv_iter_next(const lite::Value *it);
// kv.rewind(it):重置游标到开头,返回元素个数
lite::Value kv_iter_rewind(const lite::Value *it);
// kv.iter_destroy(it):销毁迭代器释放快照
lite::Value kv_iter_destroy(const lite::Value *it);

// server.config(secret, node_id):配置集群通信密钥与本节点ID
lite::Value server_config(const lite::Value *secret, const lite::Value *node_id);
// kv.iter_live():实时迭代器,next 时实时查询节点列表
lite::Value kv_iter_live();

// 实例方法类型化注册(迭代协议分派入口)
void instance_register_method(const char *type_name, int64_t typeLen,
                                 const char *method, int64_t mLen,
                                 lite::InstanceMethodFn fn);
// server.tls(cert_path, key_path):开启 TLS 加密监听
lite::Value server_tls(const lite::Value *cert, const lite::Value *key);
}  // extern "C"
