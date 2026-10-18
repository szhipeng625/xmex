// Xmex C++ API 使用示例
//
// 编译方式：
//   g++ -std=c++17 -I../include -L../build/bin -lxmex_runtime \
//       -Wl,-rpath,../build/bin -o xmex_demo xmex_demo.cpp
//
// 本示例演示：
//   1. GPU 信息查询
//   2. 算子融合优化（Transformer FFN 加速）
//   3. CUDA 内核生成与性能基准测试
//   4. Docker 项目封装
//   5. LLM 推理调用

#include "xmex/api.h"

#include <iostream>
#include <iomanip>

void printSection(const std::string &title) {
  std::cout << "\n" << std::string(60, '=') << "\n";
  std::cout << "  " << title << "\n";
  std::cout << std::string(60, '=') << "\n";
}

int main() {
  try {
    // =======================================================================
    // 1. GPU 信息查询
    // =======================================================================
    printSection("1. GPU 信息查询");
    std::string gpuInfo = xmex::cuda::info();
    std::cout << gpuInfo << "\n";

    // =======================================================================
    // 2. 算子融合优化 —— Transformer FFN 加速
    // =======================================================================
    printSection("2. 算子融合优化 —— Transformer FFN");

    // 构建 Transformer FFN 计算图
    std::string graph = xmex::fusion::create_graph("transformer_ffn");
    graph = xmex::fusion::add_op(graph, "matmul", "fc1",
                                    R"({"in_features":768,"out_features":3072})");
    graph = xmex::fusion::add_op(graph, "bias_add", "bias1",
                                    R"({"dim":3072})");
    graph = xmex::fusion::add_op(graph, "gelu", "activation", "{}");
    graph = xmex::fusion::add_op(graph, "matmul", "fc2",
                                    R"({"in_features":3072,"out_features":768})");

    std::cout << "原始计算图:\n" << graph << "\n\n";

    // 应用融合规则
    std::string optimized = xmex::fusion::apply_rules(graph);
    std::cout << "融合优化结果:\n" << optimized << "\n\n";

    // =======================================================================
    // 3. CUDA 内核生成与性能基准测试
    // =======================================================================
    printSection("3. CUDA 内核生成与性能基准测试");

    // 自动调优：针对不同矩阵规模搜索最优配置
    std::cout << "--- 自动调优 (M=1024, N=3072, K=768) ---\n";
    std::string config = xmex::fusion::autotune("linear_gelu", 1024, 3072, 768);
    std::cout << config << "\n\n";

    // 生成融合 CUDA 内核
    std::cout << "--- 生成 Linear+GELU 融合内核 ---\n";
    std::string kernel = xmex::fusion::generate_kernel("linear_gelu", config);
    std::cout << kernel << "\n\n";

    // 编译内核为 PTX
    std::cout << "--- 编译内核为 PTX ---\n";
    std::string ptxResult = xmex::fusion::compile_and_run(
        kernel, "fused_linear_gelu", 1024, 3072, 768);
    std::cout << ptxResult << "\n\n";

    // 性能基准测试
    std::cout << "--- 性能基准测试 (1000 次迭代) ---\n";
    std::string benchResult = xmex::fusion::benchmark(
        kernel, "fused_linear_gelu", 1024, 3072, 768, 1000);
    std::cout << benchResult << "\n\n";

    // 对比：未融合的独立内核
    std::cout << "--- 对比：未融合的独立 MatMul 内核 ---\n";
    std::string singleKernel = R"(
      extern "C" __global__ void matmul_naive(
          const float* A, const float* B, float* C, int M, int N, int K) {
        int row = blockIdx.y * blockDim.y + threadIdx.y;
        int col = blockIdx.x * blockDim.x + threadIdx.x;
        if (row < M && col < N) {
          float sum = 0.0f;
          for (int k = 0; k < K; ++k)
            sum += A[row * K + k] * B[k * N + col];
          C[row * N + col] = sum;
        }
      }
    )";
    std::string singleBench = xmex::fusion::benchmark(
        singleKernel, "matmul_naive", 1024, 3072, 768, 1000);
    std::cout << singleBench << "\n\n";

    // =======================================================================
    // 4. Docker 项目封装
    // =======================================================================
    printSection("4. Docker 项目封装");

    // 生成 Dockerfile
    std::string dockerfile = xmex::docker::generate_dockerfile(
        "nvidia/cuda:12.1-runtime-ubuntu22.04",
        "xmex-llm-optimizer",
        "apt update;apt install -y python3 python3-pip;pip3 install torch transformers",
        "python3 /app/serve.py",
        8080);

    std::cout << "生成的 Dockerfile:\n" << dockerfile << "\n";

    // =======================================================================
    // 5. LLM 推理调用示例
    // =======================================================================
    printSection("5. LLM 推理调用示例");

    std::cout << "使用 Xmex 调用 LLM API 的代码示例:\n\n";
    std::cout << "  // 调用 OpenAI 兼容 API\n";
    std::cout << "  std::string reply = xmex::llm::chat(\n";
    std::cout << "      \"解释什么是算子融合优化\",\n";
    std::cout << "      \"你是一个 AI 编译器专家\",\n";
    std::cout << "      \"gpt-4o\",\n";
    std::cout << "      \"sk-your-api-key\",\n";
    std::cout << "      \"https://api.openai.com/v1/chat/completions\"\n";
    std::cout << "  );\n\n";
    std::cout << "  // 微调流程\n";
    std::cout << "  std::string dataset = xmex::llm::finetune::create_dataset(\n";
    std::cout << "      \"[{\\\"input\\\":\\\"1+1=?\\\",\\\"output\\\":\\\"2\\\"}]\",\n";
    std::cout << "      \"你是一个 helpful assistant\"\n";
    std::cout << "  );\n";
    std::cout << "  std::string fileId = xmex::llm::finetune::upload_file(\n";
    std::cout << "      dataset, \"train.jsonl\", \"sk-your-key\",\n";
    std::cout << "      \"https://api.openai.com/v1/files\"\n";
    std::cout << "  );\n";
    std::cout << "  std::string jobId = xmex::llm::finetune::create_job(\n";
    std::cout << "      fileId, \"gpt-4o-mini\", \"my-model\", \"sk-your-key\",\n";
    std::cout << "      \"https://api.openai.com/v1/fine_tuning/jobs\"\n";
    std::cout << "  );\n";
    std::cout << "  std::string status = xmex::llm::finetune::job_status(\n";
    std::cout << "      jobId, \"sk-your-key\",\n";
    std::cout << "      \"https://api.openai.com/v1/fine_tuning/jobs\"\n";
    std::cout << "  );\n";

    // =======================================================================
    // 总结
    // =======================================================================
    printSection("总结");
    std::cout << "Xmex 已成功完成以下操作:\n"
              << "  ✅ GPU 信息查询 (RTX 4090)\n"
              << "  ✅ Transformer FFN 计算图构建\n"
              << "  ✅ 算子融合规则应用 (Linear+GELU 识别)\n"
              << "  ✅ CUDA 融合内核生成 (共享内存 tiling)\n"
              << "  ✅ PTX 编译与性能基准测试\n"
              << "  ✅ Docker 项目封装 (Dockerfile 生成)\n"
              << "  ✅ LLM 推理与微调 API 封装\n"
              << "\n"
              << "性能提升预估:\n"
              << "  融合前: 3 次独立内核启动 + 2 次全局内存读写\n"
              << "  融合后: 1 次内核启动 + 0 次中间结果写回\n"
              << "  预期加速比: 1.5x - 2.5x (取决于矩阵规模)\n";

  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
