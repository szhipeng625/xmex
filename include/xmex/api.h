// Xmex C++ API —— 统一的 C++ 调用接口
//
// 本文件为所有 Xmex 运行时函数提供命名空间化的 C++ 封装，
// 外部 C++ 项目只需包含此头文件并链接 libxmex_runtime.so 即可使用。
//
// 使用方式：
//   #include "xmex/api.h"
//   // 编译: g++ -std=c++17 -I<include_dir> -L<lib_dir> -lxmex_runtime main.cpp
//
// 命名空间结构：
//   xmex::docker::run()       - Docker 容器管理
//   xmex::llm::chat()         - LLM 推理调用
//   xmex::llm::finetune::*    - LLM 微调
//   xmex::cuda::info()        - CUDA 编程
//   xmex::fusion::create_graph() - 算子融合优化
//
// 所有函数返回 std::string（自动管理内存），失败时抛出 std::runtime_error。
#pragma once

#include "xmex/runtime.h"
#include "xmex/value.h"

#include <string>
#include <stdexcept>
#include <cstring>

namespace xmex {

// ===========================================================================
// 辅助函数
// ===========================================================================

// 将 xmex::Value 字符串转换为 std::string
inline std::string to_string(const xmex::Value &v) {
  if (v.tag != xmex::kStr) throw std::runtime_error("xmex::Value is not a string");
  return std::string(str_data(&v), static_cast<size_t>(str_size(&v)));
}

// 从 std::string 创建 xmex::Value 字符串
inline xmex::Value from_string(const std::string &s) {
  return str_new(s.data(), static_cast<int64_t>(s.size()));
}

// 从 int64_t 创建 xmex::Value 整数
inline xmex::Value from_int(int64_t v) { return int_(v); }

// 检查 xmex::Value 是否为 None
inline bool is_none(const xmex::Value &v) { return v.tag == xmex::kNull; }

// ===========================================================================
// Docker 容器管理
// ===========================================================================
namespace docker {

// 启动容器，返回容器 ID
inline std::string run(const std::string &image, const std::string &command) {
  xmex::Value img = from_string(image);
  xmex::Value cmd = from_string(command);
  xmex::Value result = docker_run(&img, &cmd);
  return to_string(result);
}

// 列出运行中的容器
inline std::string ps() {
  xmex::Value result = docker_ps();
  return to_string(result);
}

// 停止容器
inline void stop(const std::string &containerId) {
  xmex::Value cid = from_string(containerId);
  docker_stop(&cid);
}

// 获取容器日志
inline std::string logs(const std::string &containerId) {
  xmex::Value cid = from_string(containerId);
  xmex::Value result = docker_logs(&cid);
  return to_string(result);
}

// 拉取镜像
inline std::string pull(const std::string &image) {
  xmex::Value img = from_string(image);
  xmex::Value result = docker_pull(&img);
  return to_string(result);
}

// 生成 Dockerfile
inline std::string generate_dockerfile(
    const std::string &baseImage,
    const std::string &projectName,
    const std::string &setupCommands,
    const std::string &entrypoint,
    int64_t exposePort = 0) {
  xmex::Value img = from_string(baseImage);
  xmex::Value name = from_string(projectName);
  xmex::Value cmds = from_string(setupCommands);
  xmex::Value entry = from_string(entrypoint);
  xmex::Value port = from_int(exposePort);
  xmex::Value result = docker_generate_dockerfile(&img, &name, &cmds, &entry, &port);
  return to_string(result);
}

// 构建镜像
inline std::string build(const std::string &dockerfile,
                          const std::string &imageName,
                          const std::string &tag,
                          const std::string &contextDir) {
  xmex::Value df = from_string(dockerfile);
  xmex::Value name = from_string(imageName);
  xmex::Value t = from_string(tag);
  xmex::Value ctx = from_string(contextDir);
  xmex::Value result = docker_build(&df, &name, &t, &ctx);
  return to_string(result);
}

// 推送镜像
inline std::string push(const std::string &imageName,
                         const std::string &tag,
                         const std::string &registry = "") {
  xmex::Value name = from_string(imageName);
  xmex::Value t = from_string(tag);
  xmex::Value reg = from_string(registry);
  xmex::Value result = docker_push(&name, &t, &reg);
  return to_string(result);
}

// 一键封装项目为 Docker 镜像
inline std::string project_package(
    const std::string &projectDir,
    const std::string &imageName,
    const std::string &tag,
    const std::string &baseImage,
    const std::string &setupCommands,
    const std::string &entrypoint,
    int64_t exposePort = 0) {
  xmex::Value dir = from_string(projectDir);
  xmex::Value name = from_string(imageName);
  xmex::Value t = from_string(tag);
  xmex::Value img = from_string(baseImage);
  xmex::Value cmds = from_string(setupCommands);
  xmex::Value entry = from_string(entrypoint);
  xmex::Value port = from_int(exposePort);
  xmex::Value result = docker_project_package(&dir, &name, &t, &img, &cmds, &entry, &port);
  return to_string(result);
}

}  // namespace docker

// ===========================================================================
// LLM 推理与微调
// ===========================================================================
namespace llm {

// 调用 LLM 推理 API
inline std::string chat(const std::string &prompt,
                         const std::string &systemPrompt,
                         const std::string &model,
                         const std::string &apiKey,
                         const std::string &endpoint) {
  xmex::Value p = from_string(prompt);
  xmex::Value sp = from_string(systemPrompt);
  xmex::Value m = from_string(model);
  xmex::Value key = from_string(apiKey);
  xmex::Value ep = from_string(endpoint);
  xmex::Value result = llm_chat(&p, &sp, &m, &key, &ep);
  return to_string(result);
}

// 创建神经网络结构描述
inline std::string create_network(const std::string &name,
                                   const std::string &layersJson) {
  xmex::Value n = from_string(name);
  xmex::Value l = from_string(layersJson);
  xmex::Value result = llm_create_network(&n, &l);
  return to_string(result);
}

// 向网络添加层
inline std::string add_layer(const std::string &networkJson,
                              const std::string &layerJson) {
  xmex::Value net = from_string(networkJson);
  xmex::Value lay = from_string(layerJson);
  xmex::Value result = llm_add_layer(&net, &lay);
  return to_string(result);
}

// 网络结构摘要
inline std::string network_summary(const std::string &networkJson) {
  xmex::Value net = from_string(networkJson);
  xmex::Value result = llm_network_summary(&net);
  return to_string(result);
}

// ===========================================================================
// LLM 微调子命名空间
// ===========================================================================
namespace finetune {

// 创建训练数据集（JSONL 格式）
inline std::string create_dataset(const std::string &examplesJson,
                                   const std::string &systemPrompt = "") {
  xmex::Value ex = from_string(examplesJson);
  xmex::Value sp = from_string(systemPrompt);
  xmex::Value result = llm_create_dataset(&ex, &sp);
  return to_string(result);
}

// 上传训练文件
inline std::string upload_file(const std::string &data,
                                const std::string &filename,
                                const std::string &apiKey,
                                const std::string &endpoint) {
  xmex::Value d = from_string(data);
  xmex::Value fn = from_string(filename);
  xmex::Value key = from_string(apiKey);
  xmex::Value ep = from_string(endpoint);
  xmex::Value result = llm_upload_file(&d, &fn, &key, &ep);
  return to_string(result);
}

// 创建微调任务
inline std::string create_job(const std::string &fileId,
                               const std::string &model,
                               const std::string &suffix,
                               const std::string &apiKey,
                               const std::string &endpoint) {
  xmex::Value fid = from_string(fileId);
  xmex::Value m = from_string(model);
  xmex::Value suf = from_string(suffix);
  xmex::Value key = from_string(apiKey);
  xmex::Value ep = from_string(endpoint);
  xmex::Value result = llm_create_finetune(&fid, &m, &suf, &key, &ep);
  return to_string(result);
}

// 查询微调状态
inline std::string job_status(const std::string &jobId,
                               const std::string &apiKey,
                               const std::string &endpoint) {
  xmex::Value jid = from_string(jobId);
  xmex::Value key = from_string(apiKey);
  xmex::Value ep = from_string(endpoint);
  xmex::Value result = llm_finetune_status(&jid, &key, &ep);
  return to_string(result);
}

// 列出所有微调任务
inline std::string list_jobs(const std::string &apiKey,
                              const std::string &endpoint) {
  xmex::Value key = from_string(apiKey);
  xmex::Value ep = from_string(endpoint);
  xmex::Value result = llm_list_finetunes(&key, &ep);
  return to_string(result);
}

}  // namespace finetune
}  // namespace llm

// ===========================================================================
// CUDA 编程
// ===========================================================================
namespace cuda {

// 获取 GPU 信息
inline std::string info() {
  xmex::Value result = cuda_info();
  return to_string(result);
}

// 编译 CUDA 内核为 PTX
inline std::string compile_ptx(const std::string &kernelCode,
                                const std::string &kernelName) {
  xmex::Value code = from_string(kernelCode);
  xmex::Value name = from_string(kernelName);
  xmex::Value result = cuda_compile_ptx(&code, &name);
  return to_string(result);
}

// 启动 CUDA 内核
inline std::string launch_kernel(const std::string &ptxCode,
                                  const std::string &kernelName,
                                  int64_t gridX = 1, int64_t gridY = 1, int64_t gridZ = 1,
                                  int64_t blockX = 256, int64_t blockY = 1, int64_t blockZ = 1,
                                  const std::string &args = "[]") {
  xmex::Value ptx = from_string(ptxCode);
  xmex::Value name = from_string(kernelName);
  xmex::Value gx = from_int(gridX);
  xmex::Value gy = from_int(gridY);
  xmex::Value gz = from_int(gridZ);
  xmex::Value bx = from_int(blockX);
  xmex::Value by = from_int(blockY);
  xmex::Value bz = from_int(blockZ);
  xmex::Value a = from_string(args);
  xmex::Value result = cuda_launch_kernel(&ptx, &name, &gx, &gy, &gz, &bx, &by, &bz, &a);
  return to_string(result);
}

// GPU 内存分配
inline std::string alloc(int64_t size) {
  xmex::Value sz = from_int(size);
  xmex::Value result = cuda_alloc(&sz);
  return to_string(result);
}

// GPU 内存释放
inline void free(const std::string &handle) {
  xmex::Value h = from_string(handle);
  cuda_free(&h);
}

// 数据复制到 GPU
inline void memcpy_to_device(const std::string &handle, const std::string &data) {
  xmex::Value h = from_string(handle);
  xmex::Value d = from_string(data);
  cuda_memcpy_to_device(&h, &d);
}

// 从 GPU 复制数据
inline std::string memcpy_from_device(const std::string &handle, int64_t size) {
  xmex::Value h = from_string(handle);
  xmex::Value sz = from_int(size);
  xmex::Value result = cuda_memcpy_from_device(&h, &sz);
  return to_string(result);
}

}  // namespace cuda

// ===========================================================================
// 算子融合优化
// ===========================================================================
namespace fusion {

// 创建计算图
inline std::string create_graph(const std::string &name) {
  xmex::Value n = from_string(name);
  xmex::Value result = fusion_create_graph(&n);
  return to_string(result);
}

// 添加算子
inline std::string add_op(const std::string &graph,
                           const std::string &opType,
                           const std::string &opName,
                           const std::string &params = "{}") {
  xmex::Value g = from_string(graph);
  xmex::Value type = from_string(opType);
  xmex::Value name = from_string(opName);
  xmex::Value p = from_string(params);
  xmex::Value result = fusion_add_op(&g, &type, &name, &p);
  return to_string(result);
}

// 应用融合规则
inline std::string apply_rules(const std::string &graph) {
  xmex::Value g = from_string(graph);
  xmex::Value result = fusion_apply_rules(&g);
  return to_string(result);
}

// 生成 CUDA 内核
inline std::string generate_kernel(const std::string &fusedOp,
                                    const std::string &config = "{}") {
  xmex::Value op = from_string(fusedOp);
  xmex::Value cfg = from_string(config);
  xmex::Value result = fusion_generate_kernel(&op, &cfg);
  return to_string(result);
}

// 自动调优
inline std::string autotune(const std::string &opType,
                             int64_t M, int64_t N, int64_t K) {
  xmex::Value op = from_string(opType);
  xmex::Value m = from_int(M);
  xmex::Value n = from_int(N);
  xmex::Value k = from_int(K);
  xmex::Value result = fusion_autotune(&op, &m, &n, &k);
  return to_string(result);
}

// 编译并运行内核
inline std::string compile_and_run(const std::string &kernelCode,
                                    const std::string &kernelName,
                                    int64_t M = 1024, int64_t N = 4096, int64_t K = 768) {
  xmex::Value code = from_string(kernelCode);
  xmex::Value name = from_string(kernelName);
  xmex::Value m = from_int(M);
  xmex::Value n = from_int(N);
  xmex::Value k = from_int(K);
  xmex::Value result = fusion_compile_and_run(&code, &name, &m, &n, &k);
  return to_string(result);
}

// 性能基准测试
inline std::string benchmark(const std::string &kernelCode,
                              const std::string &kernelName,
                              int64_t M = 1024, int64_t N = 4096, int64_t K = 768,
                              int64_t iterations = 100) {
  xmex::Value code = from_string(kernelCode);
  xmex::Value name = from_string(kernelName);
  xmex::Value m = from_int(M);
  xmex::Value n = from_int(N);
  xmex::Value k = from_int(K);
  xmex::Value iters = from_int(iterations);
  xmex::Value result = fusion_benchmark(&code, &name, &m, &n, &k, &iters);
  return to_string(result);
}

}  // namespace fusion

}  // namespace xmex
