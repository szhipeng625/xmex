// Xmex 运行时库的 C ABI。
//
// IRGen 会对这里每个函数生成 call;用户手写的 C++ 也可以直接调用它们。
// 所有函数都从 libxmex_runtime.dll 导出(靠 MinGW 的 -shared 默认自动导出,
// 刻意不加 __declspec(dllexport) —— 那会触发 MinGW "遇到第一个 dllexport
// 就停止自动导出其余符号"的行为,反而让 JIT 找不到别的符号)。
//
// ⚠️ 跨边界约定 —— 改动前务必先读 docs/llvm-notes.md 第 2 节
//
// 下面这些函数在 C++ 源码里是按值的 `xmex::Value f(xmex::Value, xmex::Value)`,
// 但 IRGen 在 IR 里必须把它们声明成:
//
//     void @f(ptr sret(%xmex::Value), ptr, ptr)
//
// 即参数是"指向 xmex::Value 的指针"、返回值写进 sret 指针。
// 两者在机器层面一致,因为 Win64 ABI 规定 16 字节聚合体按引用传递、
// 返回值走隐藏 sret 寄存器(RCX = sret,RDX/R8/... = 各参数)。
//
// 若在 IR 里按值声明(`%xmex::Value @f(%xmex::Value, %xmex::Value)`),LLVM 会选另一套
// 降低方式,与 GCC 不匹配 —— 链接通过、lookup 成功,但一执行就段错误。
//
// 其它约定:
//   * 谓词一律返回 int32_t 而非 bool(i1 的 ABI 标注 GCC 与 Clang 不一致)
//   * 数组参数(如 print_ 的 xmex::Value*)是普通指针,不受上面限制
#pragma once

#include "xmex/value.h"

namespace xmex {

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

}  // namespace xmex

extern "C" {

// --- 错误 ---------------------------------------------------------------
// [[noreturn]] 不只是给编译器看的优化提示:没有它,GCC 会认为
// badOperands 这类同样标了 [[noreturn]] 的辅助函数"会返回",从而报警告。
[[noreturn]] void runtime_error(const char *msg);
const char *tag_name(int32_t tag);

// --- 构造 ---------------------------------------------------------------
// 刻意没有 int_/float_/bool_ 的运行时版本 —— 它们作为 inline 函数
// 定义在 value.h 里。IRGen 用 insertvalue 指令内联构造 xmex::Value,不发起调用;
// 若在这里再声明一份 extern "C" 版本,会与 value.h 的 C++ 链接版本构成
// 同名不同语言链接的非法重声明。

// --- 算术 ---------------------------------------------------------------
xmex::Value add(xmex::Value a, xmex::Value b);       // 数值相加 / 字符串拼接 / 列表拼接
xmex::Value sub(xmex::Value a, xmex::Value b);
xmex::Value mul(xmex::Value a, xmex::Value b);       // 数值相乘 / 字符串与列表重复
xmex::Value div_(xmex::Value a, xmex::Value b);       // 真除法,恒为 float
xmex::Value floordiv(xmex::Value a, xmex::Value b);  // 向下取整除法
xmex::Value mod_(xmex::Value a, xmex::Value b);       // 取模,符号跟随除数
xmex::Value pow_(xmex::Value a, xmex::Value b);
xmex::Value neg(xmex::Value a);                  // 一元负号

// a += b。与 add 的区别**只在列表上**:Python 的 `a += [x]` 是就地 extend,
// 别名看得见改动;而 `a = a + [x]` 是重新绑定,别名不受影响。
// 第一个参数因此是可写的 —— 列表就地改,其余类型(数值、字符串不可变)
// 一律返回新值,由调用点写回自己的槽位。
xmex::Value iadd(xmex::Value *a, const xmex::Value *b);

// --- 比较(返回 bool 值的 xmex::Value)----------------------------------------
xmex::Value eq(xmex::Value a, xmex::Value b);
xmex::Value ne(xmex::Value a, xmex::Value b);
xmex::Value lt(xmex::Value a, xmex::Value b);
xmex::Value le(xmex::Value a, xmex::Value b);
xmex::Value gt(xmex::Value a, xmex::Value b);
xmex::Value ge(xmex::Value a, xmex::Value b);

// 三路比较,返回 -1 / 0 / 1。数值之间比大小、两个字符串按字典序比;
// 类型不可比时报运行时错误(供 < <= > >= 与 list.sort 共用)。
int32_t compare_(const xmex::Value *a, const xmex::Value *b);

// --- 逻辑 ---------------------------------------------------------------
// and / or 的短路由 IRGen 用 PHI 实现(且必须返回操作数本身而非布尔值),
// 所以运行时只提供 not。
xmex::Value not_(xmex::Value a);

// --- 真值判定 -----------------------------------------------------------
int32_t truthy(xmex::Value v);

// --- 转换 ---------------------------------------------------------------
// 取出整数负载;非整数时报运行时错误(用于 range 的边界)
int64_t to_int(xmex::Value v);

// --- 输出 ---------------------------------------------------------------
// 打印 n 个值,空格分隔,末尾换行。
void print_(xmex::Value *vals, int64_t n);

// 按 repr 规则渲染成一个新字符串:容器里的字符串带引号并转义,
// 与 print 的顶层 str 规则不同 —— print("hi") 打 hi,print(["hi"]) 打 ['hi']。
// 目前供字典的 KeyError 类错误信息使用。
xmex::Value repr(const xmex::Value *v);

// --- 输入 ---------------------------------------------------------------
// input(int, int) 在 IRGen 里被拆成若干个具体读取调用,结果打包成元组
// 再交给多目标赋值解包。全部返回 xmex::Value,在 IR 里声明成 void(ptr sret)。
xmex::Value read_int();
xmex::Value read_float();
xmex::Value read_bool();
xmex::Value read_str();

// --- 内存与垃圾回收 ------------------------------------------------------
// 保守式标记-清除(见 runtime/gc.cpp 的说明)。只有 str/list/dict/tuple 是堆
// 对象,int/float/bool/None 是内联标量、根本不分配。
//
// 根 = 保守扫描本线程的栈与寄存器 + 显式根栈。**根必须放在栈上** —— 全局变量、
// 静态变量、thread_local 里放 xmex::Value 不会被认为是根,对象会被回收掉。
//
// 下面这些是运行时内部用的;用户手写 C++ 只需要 alloc。

// 分配 n 字节并登记为一个 GC 块。n 会向上取整到 16 的倍数。
void *gc_alloc(size_t n, xmex::GcScan kind);
// SCAN_VALUES 的专用入口:从 firstCellOffset 起是若干 16 字节的 xmex::Value 单元格。
// ⚠️ 元组传 8(xmex::Tuple 是 {int64 len; xmex::Value items[]}),传 0 会把 len 当 tag、
// 整条链错位,那是会丢根的。
void *gc_alloc_values(size_t n, size_t firstCellOffset);

void gc_collect();          // 手动触发一次回收
void gc_stats(xmex::GcStats *out);
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
// 计数位记在**块头**里,对象自身数据布局与 xmex::Value 布局都不动,不触碰 ABI 契约。
// 生命周期:
//   * 强引用归零                 -> 对象内容失效(仍可被弱引用观察)
//   * 强引用归零且已无弱引用     -> 真正归还内存
//   * 弱引用归零                 -> 对象早已失效时,此刻归还内存
// ⚠️ 强引用计数为 0 表示"尚无强引用登记",此时减引用不做任何事,交回标记-清除;
//    因此即使生成代码漏插计数调用,也只是退回原行为,绝不会误释放活对象。
// ⚠️ 循环引用回收不了,由标记-清除按"从根可达"兜底,整体不会泄漏。
// payload 是堆对象指针(与 xmex::Value.as.ptr 一致)。
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
xmex::Value tuple_new(xmex::Value *elems, int64_t n);
int64_t tuple_len(const xmex::Value *t);
xmex::Value tuple_get(const xmex::Value *t, int64_t i);   // 支持负索引
void unpack(const xmex::Value *t, xmex::Value *out, int64_t n);

// --- 字符串 -------------------------------------------------------------
xmex::Value str_new(const char *bytes, int64_t len);
int64_t str_size(const xmex::Value *s);
const char *str_data(const xmex::Value *s);
xmex::Value str_concat(const xmex::Value *a, const xmex::Value *b);
xmex::Value str_get(const xmex::Value *s, int64_t i);          // 取单个字符,支持负索引
xmex::Value str_repeat(const xmex::Value *s, int64_t times);
// 按字节序比较,返回 -1 / 0 / 1
int32_t str_compare(const xmex::Value *a, const xmex::Value *b);
int32_t str_find(const xmex::Value *s, const xmex::Value *needle, int64_t from);
int32_t str_starts_with(const xmex::Value *s, const xmex::Value *prefix);
int32_t str_ends_with(const xmex::Value *s, const xmex::Value *suffix);

// --- 列表 ---------------------------------------------------------------
// xmex::List 是可变对象:append/insert/pop/... 就地改,别名的调用方看得见改动。
// 增长靠"新分配 + 拷贝",因为 bump 分配器没有 realloc(旧数组就地泄漏,
// 与项目其它部分一致,见 runtime/arena.cpp)。
xmex::Value list_new(xmex::Value *elems, int64_t n);
int64_t list_len(const xmex::Value *v);
xmex::Value list_get(const xmex::Value *v, int64_t i);          // 支持负索引
void list_set(const xmex::Value *v, int64_t i, const xmex::Value *val);
xmex::Value list_concat(const xmex::Value *a, const xmex::Value *b);  // 新列表
xmex::Value list_repeat(const xmex::Value *a, int64_t times);     // 新列表

void list_append(const xmex::Value *v, const xmex::Value *item);
void list_extend(const xmex::Value *v, const xmex::Value *other);
void list_insert(const xmex::Value *v, int64_t i, const xmex::Value *item);
void list_remove(const xmex::Value *v, const xmex::Value *item);
// idxOrNone 传的是一个 **None 值**(none_()),不是空指针 —— 长度为 0 的
// 下标在语言里表达不出来,所以借 None 当"没给下标"的哨兵。传 nullptr 会崩。
xmex::Value list_pop(const xmex::Value *v, const xmex::Value *idxOrNone);
int64_t list_index(const xmex::Value *v, const xmex::Value *item);     // 找不到报错
int64_t list_count(const xmex::Value *v, const xmex::Value *item);
void list_reverse(const xmex::Value *v);
void list_clear(const xmex::Value *v);
void list_sort(const xmex::Value *v);

// --- 字典 ---------------------------------------------------------------
// 插入有序 + 线性扫描。不用哈希表:维护 Python 3.7 的可观测插入序,
// 且省掉为每种键类型写 hash 的面上。代价是查找 O(n),见 docs/language.md。
xmex::Value dict_new(xmex::Value *keys, xmex::Value *vals, int64_t n);
int64_t dict_len(const xmex::Value *d);
xmex::Value dict_get(const xmex::Value *d, const xmex::Value *key);   // 键不存在则报错
xmex::Value dict_get_default(const xmex::Value *d, const xmex::Value *key,
                            const xmex::Value *def);
int32_t dict_has(const xmex::Value *d, const xmex::Value *key);
void dict_set(const xmex::Value *d, const xmex::Value *key, const xmex::Value *val);
xmex::Value dict_key_at(const xmex::Value *d, int64_t i);   // 按插入序取第 i 个键
xmex::Value dict_val_at(const xmex::Value *d, int64_t i);
// pop 必须给键。这里刻意不用 None 当"没给键"的哨兵 —— None 本身是合法的键,
// 两者混淆会让 `d.pop(None)` 变得无法表达。
xmex::Value dict_pop(const xmex::Value *d, const xmex::Value *key);
void dict_clear(const xmex::Value *d);

// --- 下标 / 切片 / 长度 --------------------------------------------------
// "不支持下标"这类错误的唯一产生点。按 tag 分派到上面各类型的访问器。
int64_t len(const xmex::Value *v);       // str / list / dict / tuple
xmex::Value index_(const xmex::Value *obj, const xmex::Value *idx);
// obj[idx] = val。列表按位置改,字典按键插入或更新(新增键追加在末尾)。
void setindex(const xmex::Value *obj, const xmex::Value *idx, const xmex::Value *val);
// lo / hi / step 省略时传 None(即 xmex::kNull),各自按默认值处理
xmex::Value slice(const xmex::Value *obj, const xmex::Value *lo, const xmex::Value *hi,
                 const xmex::Value *step);

// for 循环用的"第 i 个元素"。**不能**拿 index_ 代替:遍历字典要的是第 i 个
// 键,而下标 d[k] 是按键查找,两者语义不同。
xmex::Value iter_at(const xmex::Value *obj, int64_t i);

// --- 方法调用 -----------------------------------------------------------
// obj.print(...) 形式统一走这一个入口,按 tag + 方法名分派。
// 方法表在运行时里,所以拼错方法名是**运行时**报错 —— 本项目的类型注解
// 本来就不做检查(见 docs/language.md),这是动态语言的必然。
xmex::Value call_method(const xmex::Value *obj, const char *name, int64_t nameLen,
                       xmex::Value *args, int64_t nargs);

// --- Docker 容器调用 -----------------------------------------------------
// 通过 popen 调用 docker CLI，不引入额外依赖。
// 部署到有 Docker daemon 的环境中即可直接使用。
xmex::Value docker_run(const xmex::Value *image, const xmex::Value *cmd);
xmex::Value docker_ps();
void docker_stop(const xmex::Value *containerId);
xmex::Value docker_logs(const xmex::Value *containerId);
xmex::Value docker_pull(const xmex::Value *image);

// Docker 项目封装
xmex::Value docker_generate_dockerfile(const xmex::Value *baseImage,
                                       const xmex::Value *projectName,
                                       const xmex::Value *setupCommands,
                                       const xmex::Value *entrypoint,
                                       const xmex::Value *exposePort);
xmex::Value docker_build(const xmex::Value *dockerfile, const xmex::Value *imageName,
                         const xmex::Value *tag, const xmex::Value *contextDir);
xmex::Value docker_push(const xmex::Value *imageName, const xmex::Value *tag,
                        const xmex::Value *registry);
xmex::Value docker_project_package(const xmex::Value *projectDir,
                                   const xmex::Value *imageName,
                                   const xmex::Value *tag,
                                   const xmex::Value *baseImage,
                                   const xmex::Value *setupCommands,
                                   const xmex::Value *entrypoint,
                                   const xmex::Value *exposePort);

// --- LLM 神经网络接口 -----------------------------------------------------
// 通过 curl 调用 OpenAI 兼容 API，支持网络结构描述与修改。
xmex::Value llm_chat(const xmex::Value *prompt, const xmex::Value *systemPrompt,
                    const xmex::Value *model, const xmex::Value *apiKey,
                    const xmex::Value *endpoint);
xmex::Value llm_create_network(const xmex::Value *name, const xmex::Value *layers);
xmex::Value llm_add_layer(const xmex::Value *network, const xmex::Value *layer);
xmex::Value llm_remove_layer(const xmex::Value *network, const xmex::Value *index);
xmex::Value llm_network_summary(const xmex::Value *network);

// LLM 微调（Fine-tuning）
xmex::Value llm_create_dataset(const xmex::Value *examples, const xmex::Value *systemPrompt);
xmex::Value llm_upload_file(const xmex::Value *data, const xmex::Value *filename,
                           const xmex::Value *apiKey, const xmex::Value *endpoint);
xmex::Value llm_create_finetune(const xmex::Value *fileId, const xmex::Value *model,
                                const xmex::Value *suffix, const xmex::Value *apiKey,
                                const xmex::Value *endpoint);
xmex::Value llm_finetune_status(const xmex::Value *jobId, const xmex::Value *apiKey,
                                const xmex::Value *endpoint);
xmex::Value llm_list_finetunes(const xmex::Value *apiKey, const xmex::Value *endpoint);

// --- CUDA 编程支持 --------------------------------------------------------
// 通过 CUDA Driver API 和 LLVM NVPTX 后端实现 GPU 编程。
// 需要 NVIDIA GPU 和 CUDA 驱动。
xmex::Value cuda_info();
xmex::Value cuda_compile_ptx(const xmex::Value *kernelCode, const xmex::Value *kernelName);
xmex::Value cuda_launch_kernel(const xmex::Value *ptxCode, const xmex::Value *kernelName,
                               const xmex::Value *gridX, const xmex::Value *gridY,
                               const xmex::Value *gridZ, const xmex::Value *blockX,
                               const xmex::Value *blockY, const xmex::Value *blockZ,
                               const xmex::Value *args);
xmex::Value cuda_alloc(const xmex::Value *size);
void cuda_free(const xmex::Value *handle);
void cuda_memcpy_to_device(const xmex::Value *handle, const xmex::Value *data);
xmex::Value cuda_memcpy_from_device(const xmex::Value *handle, const xmex::Value *size);

// --- 算子融合优化 ---------------------------------------------------------
// LLM 推理加速：算子 IR 描述、融合规则引擎、CUDA 内核生成、自动调优。
xmex::Value fusion_create_graph(const xmex::Value *name);
xmex::Value fusion_add_op(const xmex::Value *graph, const xmex::Value *opType,
                          const xmex::Value *opName, const xmex::Value *params);
xmex::Value fusion_apply_rules(const xmex::Value *graph);
xmex::Value fusion_generate_kernel(const xmex::Value *fusedOp, const xmex::Value *config);
xmex::Value fusion_autotune(const xmex::Value *opType, const xmex::Value *M,
                            const xmex::Value *N, const xmex::Value *K);
xmex::Value fusion_compile_and_run(const xmex::Value *kernelCode,
                                   const xmex::Value *kernelName,
                                   const xmex::Value *M, const xmex::Value *N,
                                   const xmex::Value *K);
xmex::Value fusion_benchmark(const xmex::Value *kernelCode, const xmex::Value *kernelName,
                             const xmex::Value *M, const xmex::Value *N,
                             const xmex::Value *K, const xmex::Value *iterations);

// --- LLM 推理运行时 -------------------------------------------------------
// 模型加载、Token 生成、KV Cache 管理、FlashAttention、多 GPU、性能分析。
xmex::Value inference_load_model(const xmex::Value *modelPath, const xmex::Value *configPath);
xmex::Value inference_generate(const xmex::Value *prompt, const xmex::Value *maxTokens,
                               const xmex::Value *temperature, const xmex::Value *topP,
                               const xmex::Value *topK);
xmex::Value inference_kv_cache_stats();
xmex::Value inference_flash_attention_kernel();
xmex::Value inference_multi_gpu_info();
xmex::Value inference_profile();
void inference_unload();

// --- Tokenizer 分词器 -----------------------------------------------------
// BPE 编码/解码，支持 HuggingFace tokenizer.json 格式。
xmex::Value tokenizer_load(const xmex::Value *path);
xmex::Value tokenizer_encode(const xmex::Value *text);
xmex::Value tokenizer_decode(const xmex::Value *tokenIds);
xmex::Value tokenizer_info();

// --- REST API 服务 --------------------------------------------------------
// OpenAI 兼容的 HTTP 推理接口，零外部依赖。
xmex::Value server_start(const xmex::Value *port);
void server_stop();
xmex::Value server_status();

// --- 模型量化工具与多架构支持 ---------------------------------------------
// FP16→AWQ 4-bit 转换、主流架构配置模板、量化精度验证。
xmex::Value quantize_fp16_to_awq(const xmex::Value *weightsJson, const xmex::Value *bits,
                                 const xmex::Value *groupSize);
xmex::Value quantize_arch_template(const xmex::Value *archName);
xmex::Value quantize_verify_precision(const xmex::Value *fp16Values,
                                      const xmex::Value *awqValues);
xmex::Value quantize_model_info(const xmex::Value *modelPath);

// --- Xmex 原生推理服务引擎 ---------------------------------------------
// vLLM 风格的 LLM 推理引擎：PagedAttention KV Cache + 连续批处理。
xmex::Value engine_init(const xmex::Value *configJson);
xmex::Value engine_add_request(const xmex::Value *prompt, const xmex::Value *maxTokens,
                               const xmex::Value *temperature, const xmex::Value *topP,
                               const xmex::Value *topK);
xmex::Value engine_step();
xmex::Value engine_status();
void engine_reset();
xmex::Value engine_serve(const xmex::Value *port);

// --- 线程 / 进程 / 资源管理 -----------------------------------------------
// 线程任务用注册表管理:这门语言目前还没有一等函数,线程入口无法在语言里
// 直接写,由宿主 C++ 按名字注册、语言层按名字启动。
// 所有跨线程共享状态由一把互斥锁保护,句柄登记在带锁的表里、结束可回收。
// 线程任务函数的真实签名是 void (*)(void *),arg 原样透传。
int64_t thread_register_task(const char *name, void *fn, void *arg);
xmex::Value thread_start(const char *taskName);   // 返回线程号
int64_t thread_join(int64_t id);              // 等待结束并回收条目
int64_t thread_alive(int64_t id);             // 1 = 仍在运行
int64_t thread_count();                       // 登记表中的线程数
// 进程:同步执行(返回标准输出)、后台启动(返回进程号)、等待(返回退出码)、
// 判活、终止。
xmex::Value process_run(const xmex::Value *cmd);
int64_t process_spawn(const xmex::Value *cmd);
int64_t process_wait(int64_t id);
int64_t process_alive(int64_t id);
int64_t process_kill(int64_t id);
// 资源管理:快照(线程与进程计数)与回收已结束的线程条目。
xmex::Value resource_stats();
int64_t resource_reap();

// --- 类实例 ---------------------------------------------------------------
// 实例 = 类标识 + 属性表(用字典承载)。属性名是字符串,实例有没有某个属性
// 只有运行时才知道 —— 与"方法名写错是运行时错误"是同一套取舍。
// 方法不进属性表:成员方法由编译期直接解析成函数调用,运行时只负责属性。
xmex::Value instance_new(int64_t classId, const xmex::Value *attrs);
int64_t instance_class_id(const xmex::Value *inst);
void instance_set_attr(const xmex::Value *inst, const char *name, int64_t nameLen,
                          const xmex::Value *val);
xmex::Value instance_get_attr(const xmex::Value *inst, const char *name, int64_t nameLen);
int32_t instance_has_attr(const xmex::Value *inst, const char *name, int64_t nameLen);
int64_t instance_attr_count(const xmex::Value *inst);

// --- 互斥锁与条件变量 -----------------------------------------------------
// 句柄是一个整数,实际对象登记在运行时内部,靠句柄访问 —— 语言层不必认识原生指针。
// 条件变量的等待会原子地放开传入的那把锁,被唤醒后重新拿回它;
// 这正是"检查条件"与"等待"之间不丢唤醒的关键。
// ⚠️ 等条件变量前必须先拿到配套的那把锁:检查条件与进入等待之间不能有缝,
// 否则通知可能在两者之间发出,等待者就永远等下去了。
xmex::Value sync_mutex_new();                              // 创建锁,返回句柄
xmex::Value sync_mutex_lock(const xmex::Value *h);             // 加锁(阻塞直到拿到)
xmex::Value sync_mutex_unlock(const xmex::Value *h);           // 解锁
xmex::Value sync_mutex_free(const xmex::Value *h);             // 销毁锁
xmex::Value sync_cond_new();                               // 创建条件变量,返回句柄
xmex::Value sync_cond_wait(const xmex::Value *ch, const xmex::Value *mh);  // 等待通知
xmex::Value sync_cond_signal(const xmex::Value *ch);           // 唤醒一个等待者
xmex::Value sync_cond_broadcast(const xmex::Value *ch);        // 唤醒全部等待者
xmex::Value sync_cond_free(const xmex::Value *ch);             // 销毁条件变量
xmex::Value sync_stats();                                  // 同步原语数量快照

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
                            const xmex::Value *const *paramValues);  // 函数进入:附带形参名与实参值
void trace_end(int64_t id, const char *name, int ok);  // 函数退出:自动算耗时
void trace_var(const char *name, const char *value);   // 变量上报(字符串值)
void trace_var_value(const char *name, const xmex::Value *v); // 变量上报(直接收值)
void trace_set_dump(const char *path);                 // 设置事件落盘文件路径
// cuda / docker 操作事件:category 取 cuda 或 docker,op 为操作名,
// detail 为对象描述,ok 为是否成功,elapsed 为耗时(毫秒)。
void trace_op(const char *category, const char *op, const char *detail,
                 int ok, int64_t elapsed);

// --- 判题沙箱 -------------------------------------------------------------
// 在受控环境中执行用户提交的代码：seccomp 系统调用白名单 + 降权 + 资源限制。
// 这些接口供 .pys 脚本在编译时通过 IRGen 生成调用，也可在 C++ 侧直接使用。

// 创建沙箱实例，返回 xmex::kInstance 对象
xmex::Value sandbox_create();
// 设置 CPU 时间限制（毫秒）
void sandbox_set_cpu_limit(const xmex::Value *inst, uint32_t cpu_limit_ms);
// 设置内存限制（字节）
void sandbox_set_mem_limit(const xmex::Value *inst, uint64_t mem_limit_bytes);
// 设置最大进程数（防进程炸弹）
void sandbox_set_max_pids(const xmex::Value *inst, int64_t max_pids);
// 设置工作目录
void sandbox_set_work_dir(const xmex::Value *inst, const char *path, int64_t pathLen);
// 设置用户可执行文件路径
void sandbox_set_binary(const xmex::Value *inst, const char *path, int64_t pathLen);
// 添加命令行参数
void sandbox_add_arg(const xmex::Value *inst, const char *arg, int64_t argLen);
// 设置标准输入/输出/错误重定向文件
void sandbox_set_stdin(const xmex::Value *inst, const char *path, int64_t pathLen);
void sandbox_set_stdout(const xmex::Value *inst, const char *path, int64_t pathLen);
void sandbox_set_stderr(const xmex::Value *inst, const char *path, int64_t pathLen);
// 执行沙箱（fork + sandbox_main + exec），返回子进程 pid
int64_t sandbox_exec(const xmex::Value *inst);
// 销毁沙箱实例
void sandbox_destroy(const xmex::Value *inst);

// --- cgroup 资源管理 ------------------------------------------------------
// 基于 Linux cgroup v2 的 CPU/内存/PID 限制与用量监控。

// 创建判题 cgroup（高 CPU 权重 200），返回 cgroup 路径字符串
xmex::Value cgroup_create_judge(const char *task_id, int64_t taskIdLen,
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
xmex::Value sysmon_cpu_stats();
// 指定进程的 CPU 使用时间（毫秒）
int64_t sysmon_process_cpu_ms(int64_t pid);
// 系统内存信息（总量/可用/缓存/Swap + 使用率）
xmex::Value sysmon_mem_info();
// 指定进程的物理内存（RSS，KB）
int64_t sysmon_process_mem_kb(int64_t pid);
// 指定进程的详细内存信息（峰值/虚拟/RSS/数据段/栈）
xmex::Value sysmon_process_mem_info(int64_t pid);
// 所有进程 PID 列表
xmex::Value sysmon_list_processes();
// 指定进程的详细信息（PID/PPID/名称/状态/CPU/内存/线程/子进程）
xmex::Value sysmon_process_info(int64_t pid);
// 进程树的总资源用量（CPU/内存/PID 数）
xmex::Value sysmon_tree_usage(int64_t root_pid);
// 网络接口统计（收发字节/包数）
xmex::Value sysmon_net_stats();
// 文件系统配额（总量/已用/可用/inode + 使用率）
xmex::Value sysmon_fs_quota(const char *path, int64_t pathLen);
// 目录总大小（字节，递归）
int64_t sysmon_dir_size(const char *path, int64_t pathLen);
// 目录文件数量（递归）
int64_t sysmon_dir_file_count(const char *path, int64_t pathLen);
// 系统信息（主机名/内核/发行版/运行时间/CPU 数/总内存）
xmex::Value sysmon_sys_info();
// 系统负载（1/5/15 分钟）
xmex::Value sysmon_load_avg();

// --- 集群管理台 (cluster) -------------------------------------------------
// 轻量级多集群管理控制台：通过指定远程地址连接多个独立集群，
// 统一查看状态、执行命令、批量操作。管理台本身不运行 Raft 共识。

// 连接到远程集群（name: 别名, addr: host:port, secret: 密钥）
xmex::Value cluster_connect(const char *name, int64_t nameLen,
                            const char *addr, int64_t addrLen,
                            const char *secret, int64_t secretLen);
// 断开集群连接
xmex::Value cluster_disconnect(const char *name, int64_t nameLen);
// 列出所有已连接集群
xmex::Value cluster_list();
// 检查集群是否已连接（返回 1/0）
int64_t cluster_is_connected(const char *name, int64_t nameLen);
// 获取集群完整状态（leader/term/节点列表/健康/延迟）
xmex::Value cluster_status(const char *name, int64_t nameLen);
// 获取集群节点列表
xmex::Value cluster_nodes(const char *name, int64_t nameLen);
// 健康检查（ping 集群 /status 端点）
int64_t cluster_health(const char *name, int64_t nameLen);
// 向集群发送命令（HTTP POST /exec）
xmex::Value cluster_exec(const char *name, int64_t nameLen,
                         const char *cmd_type, int64_t typeLen,
                         const char *payload, int64_t payloadLen);
// 向集群发送查询（HTTP GET）
xmex::Value cluster_query(const char *name, int64_t nameLen,
                          const char *path, int64_t pathLen);
// 向所有已连接集群广播命令
xmex::Value cluster_broadcast(const char *cmd_type, int64_t typeLen,
                              const char *payload, int64_t payloadLen);
// 所有集群健康检查
xmex::Value cluster_health_all();

// --- 节点管理 (kv_admin) --------------------------------------------------
// 结合 AdvisKV 的节点生命周期管理：建立节点、进程控制（sleep/wakeup）、
// 逻辑跳过（skip）、永久移除（erase）。节点间通过 Raft 互相通信。

// 建立节点并启动进程（node_id + 可执行文件 + 参数 + 工作目录 + 地址）
xmex::Value kv_build(const char *node_id, int64_t idLen,
                    const char *binary, int64_t binLen,
                    const char *args_json, int64_t argsLen,
                    const char *work_dir, int64_t dirLen,
                    const char *addr, int64_t addrLen);
// 向集群追加节点（语义同 build）
xmex::Value kv_add(const char *node_id, int64_t idLen,
                  const char *binary, int64_t binLen,
                  const char *args_json, int64_t argsLen,
                  const char *work_dir, int64_t dirLen,
                  const char *addr, int64_t addrLen);
// 永久移除节点（停止进程并删除注册）
xmex::Value kv_erase(const char *node_id, int64_t idLen);
// 逻辑跳过节点（标记 LOST，等待副本替换）
xmex::Value kv_skip(const char *node_id, int64_t idLen);
// 暂停节点（SIGSTOP，故障注入）
xmex::Value kv_sleep(const char *node_id, int64_t idLen);
// 唤醒节点（SIGCONT）
xmex::Value kv_wakeup(const char *node_id, int64_t idLen);
// 停止节点（保留注册，可重启）
xmex::Value kv_stop(const char *node_id, int64_t idLen);
// 重启节点
xmex::Value kv_restart(const char *node_id, int64_t idLen);
// 获取节点详情 JSON
xmex::Value kv_status(const char *node_id, int64_t idLen);
// 列出所有节点 JSON
xmex::Value kv_list();
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
xmex::Value time_ms();
xmex::Value time_s();

// ---- add(box) 统一编排与容器内定位(2026-10 增补) ----
// kv.add_box(node_id, box, addr):一步完成注册节点+绑定盒子,addr 空则本地启动
xmex::Value kv_add_box(const char *node_id, int64_t idLen,
                      const xmex::Value *box,
                      const char *addr, int64_t addrLen);
// kv.locate(box):由盒子反查所属 kv 节点的 kv->box 绑定关系
xmex::Value kv_locate(const xmex::Value *box);
// kv.self():容器内定位,读 XMEX_NODE_ID 环境变量反查所属节点
xmex::Value kv_self();

// 按 handle 取盒子登记的二进制路径(供 kv.add_box 使用)
xmex::Value sandbox_binary_path(int64_t handle);

// ---- 节点迭代器适配器(2026-10 增补) ----
// kv.iter():创建节点迭代器实例(快照当前节点列表)
xmex::Value kv_iter();
// kv.next(it):取下一个节点信息,迭代结束返回 None
xmex::Value kv_iter_next(const xmex::Value *it);
// kv.rewind(it):重置游标到开头,返回元素个数
xmex::Value kv_iter_rewind(const xmex::Value *it);
// kv.iter_destroy(it):销毁迭代器释放快照
xmex::Value kv_iter_destroy(const xmex::Value *it);

// ---- 声明式调谐(2026-10 增补) ----
// 后台把 observed 拉回 desired:进程崩了自动拉起,desired=absent 的摘除。
// 不启动循环时行为与从前完全一致(命令式),调谐是可选增强。
// 详见 docs/distributed-reconcile-design.md
xmex::Value kv_reconcile_start(const xmex::Value *interval_ms);

// ---- 通用 HTTP 客户端(2026-10 增补)----
// 能打**任意** URL,供接口测试用。cluster.query 只能对已注册的 cluster
// 节点调用,且要 secret —— 测第三方接口时用不上。
//
//   http.get(url[, timeout_ms]) -> 响应 JSON
//   http.post(url, body[, timeout_ms]) -> 响应 JSON
//   http.head(url[, timeout_ms]) -> 响应 JSON(无 body,状态码齐全)
//   http.request(method, url[, body, timeout_ms, token, headers])
//
// 响应 JSON 形如:
//   {"ok":true,"status":200,"elapsed_ms":12,"content_length":18,
//    "body":"...","headers":"HTTP/1.1 200 OK\r\n..."}
//
// 刻意不做:连接池、异步、自动重定向跟随、自动重试、multipart。
// 接口测试要"看得见每一步" —— 自动重试会把刚注入的故障冲掉,重定向跟随
// 会让"接口返回了 302"这个事实被藏起来。
xmex::Value http_get_v(const xmex::Value *url, ...);
xmex::Value http_post_v(const xmex::Value *url, const xmex::Value *body, ...);
xmex::Value http_head_v(const xmex::Value *url, ...);
// ⚠️ 可变参数是**必需的**,不是偷懒。
// IRGen 按**调用点的实际实参个数**生成 IR(见 irgen.cpp 的 genCall:argTypes
// 逐个 push),所以 `http.request(m,u)` 与 `http.request(m,u,b,t,k,h)` 会
// 生成两个**不同签名**的调用。固定 6 参的 C 函数会让少传参数的调用点
// 在 LLVM 校验时报 "Incorrect number of arguments passed to called function"。
//
// 变参让任意个数的调用都落到同一个 C 符号上,缺失的参数就是 va_arg 取到的
// 0(空指针),由实现侧按默认值补齐。
xmex::Value http_request_v(const xmex::Value *method, const xmex::Value *url, ...);

// ---- 容器节点(2026-10 增补)----
// 与 kv.add 平行但作用于 Docker 容器。刻意不并入 kv.add 的参数:
// 一个纳管本机进程(fork/exec + SIGSTOP/SIGKILL),一个跑容器
// (docker run + docker pause/kill)—— 原语全不一样,硬合会让两边都变脏。
//
//   kv.container_add(node_id, image, command, cpus, memory)
//   kv.container_pause(node_id)      冻结容器,模拟服务不可用
//   kv.container_unpause(node_id)    解冻
//   kv.container_crash(node_id)      SIGKILL 掉,调谐会重新拉起
//   kv.container_logs(node_id)       容器日志
//
// cpus/memory 传空串表示不限,例:"0.5" / "256m"。
// 需要 PATH 上有 docker CLI 且当前用户能访问 daemon(**Linux only**)。
xmex::Value kv_container_add(const xmex::Value *node_id, const xmex::Value *image,
                             const xmex::Value *command, const xmex::Value *cpus,
                             const xmex::Value *memory);
xmex::Value kv_container_pause(const xmex::Value *node_id);
xmex::Value kv_container_unpause(const xmex::Value *node_id);
xmex::Value kv_container_crash(const xmex::Value *node_id);
xmex::Value kv_container_logs(const xmex::Value *node_id);
xmex::Value kv_reconcile_stop();
xmex::Value kv_reconcile_status();
xmex::Value kv_reconcile_once();          // 手动一轮,返回详细报告
xmex::Value kv_unskip(const xmex::Value *node_id);
xmex::Value kv_failures(const xmex::Value *node_id);

// ---- 动态库 FFI(2026-10 增补) ----
// kv.ffi(path) 在本进程内加载 .dll/.so,返回带 methods 的实例:
//   lib.symbols()          列出库里可调用的函数
//   lib.describe("init")   看某个函数的签名(参数类型与返回类型)
//   lib.call("init", [..])  调用
//   lib.unload()            卸载
// kv.ffi_list() 列出所有已加载的动态库。
//
// 与 kv.add() 的分工:kv.add 纳管一个**进程**,kv.ffi 在**本进程内**
// 加载一个**动态库**。远程动态库要走「远程进程 + agent」,由 kv.add 纳管。
//
// 动态库需导出描述符 xmex_ffi_describe(char*, int64) 返回 JSON,
// 以及按统一调用约定导出的函数 int64_t fn(int64_t*, int64_t, int64_t*)。
// 详见 runtime/ffi.h 与 docs/ffi-design.md。
xmex::Value kv_ffi(const xmex::Value *path);
xmex::Value kv_ffi_list();

// kv.ffi_isolated(path) —— 把动态库加载到**子进程**。
// DLL 崩溃/死循环时只有子进程死,宿主从管道读到 EOF,拿退出码报
// "worker crashed",宿主本身不受影响。
//
// 与 kv.ffi 的取舍:
//   kv.ffi         进程内加载。快,支持回调(回调是给 DLL 一个函数指针),
//                  但 DLL 崩了宿主一起完蛋。
//   kv.ffi_isolated 子进程加载。有隔离,协议与将来的远程 agent 一致,
//                  但**不支持回调**,且参数要跨进程序列化。
xmex::Value kv_ffi_isolated(const xmex::Value *path);

// ---- 副本组(2026-10 增补) ----
// 声明「这一组要几个副本」;成员不健康时调谐循环自动补足到期望副本数。
// 参照 adviskv ReplicaGroup 设计。
xmex::Value kv_group_create(const xmex::Value *group_id,
                            const xmex::Value *replicas,
                            const xmex::Value *binary,
                            const xmex::Value *work_dir_prefix,
                            const xmex::Value *addr_prefix);
xmex::Value kv_group_add(const xmex::Value *group_id, const xmex::Value *node_id);
xmex::Value kv_group_replicas(const xmex::Value *group_id, const xmex::Value *n);
xmex::Value kv_group_status(const xmex::Value *group_id);  // 空 id 返回全部组
xmex::Value kv_group_destroy(const xmex::Value *group_id);

// server.config(secret, node_id):配置集群通信密钥与本节点ID
xmex::Value server_config(const xmex::Value *secret, const xmex::Value *node_id);
// kv.iter_live():实时迭代器,next 时实时查询节点列表
xmex::Value kv_iter_live();

// 实例方法类型化注册(迭代协议分派入口)
void instance_register_method(const char *type_name, int64_t typeLen,
                                 const char *method, int64_t mLen,
                                 xmex::InstanceMethodFn fn);
// server.tls(cert_path, key_path):开启 TLS 加密监听
xmex::Value server_tls(const xmex::Value *cert, const xmex::Value *key);
}  // extern "C"
