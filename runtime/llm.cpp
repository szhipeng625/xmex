// LLM 神经网络接口 —— 通过 curl 调用 OpenAI 兼容 API。
//
// 设计原则：
//   1. 零外部依赖：仅通过 popen 调用系统自带的 curl；
//   2. 支持 OpenAI 兼容的 chat completions API；
//   3. 提供神经网络结构描述和修改能力（通过 JSON 中间表示）；
//   4. 所有操作返回 xmex::Value，与 Xmex 类型系统无缝对接。
#include "xmex/runtime.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

// 执行 shell 命令并捕获 stdout，返回动态分配的 C 字符串。
// 调用方负责用 free() 释放。
char *captureCommand(const char *cmd) {
  FILE *fp = popen(cmd, "r");
  if (!fp) return nullptr;

  size_t cap = 4096;
  char *buf = static_cast<char *>(malloc(cap));
  if (!buf) { pclose(fp); return nullptr; }

  size_t len = 0;
  while (!feof(fp) && !ferror(fp)) {
    if (len + 256 >= cap) {
      cap *= 2;
      char *nb = static_cast<char *>(realloc(buf, cap));
      if (!nb) { free(buf); pclose(fp); return nullptr; }
      buf = nb;
    }
    size_t n = fread(buf + len, 1, cap - len - 1, fp);
    len += n;
  }
  buf[len] = '\0';
  pclose(fp);
  return buf;
}

// JSON 字符串转义：将 " \ \n 等字符转义为 JSON 安全形式。
// 返回动态分配的字符串，调用方负责 free。
char *jsonEscape(const char *s, int64_t len) {
  size_t cap = static_cast<size_t>(len) * 2 + 4;
  char *out = static_cast<char *>(malloc(cap));
  if (!out) return nullptr;

  size_t pos = 0;
  for (int64_t i = 0; i < len; ++i) {
    if (pos + 4 >= cap) {
      cap *= 2;
      char *nb = static_cast<char *>(realloc(out, cap));
      if (!nb) { free(out); return nullptr; }
      out = nb;
    }
    switch (s[i]) {
      case '"':  out[pos++] = '\\'; out[pos++] = '"'; break;
      case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
      case '\n': out[pos++] = '\\'; out[pos++] = 'n'; break;
      case '\r': out[pos++] = '\\'; out[pos++] = 'r'; break;
      case '\t': out[pos++] = '\\'; out[pos++] = 't'; break;
      default:   out[pos++] = s[i]; break;
    }
  }
  out[pos] = '\0';
  return out;
}

// 从 JSON 响应中提取 content 字段的值。
// 简单解析：查找 "content":" 并提取到下一个未转义的 "。
char *extractContent(const char *json) {
  const char *key = "\"content\":\"";
  const char *start = strstr(json, key);
  if (!start) {
    // 尝试 "content": " (带空格)
    key = "\"content\": \"";
    start = strstr(json, key);
  }
  if (!start) return nullptr;

  start += strlen(key);

  size_t cap = 256;
  char *out = static_cast<char *>(malloc(cap));
  if (!out) return nullptr;

  size_t pos = 0;
  while (*start && *start != '"') {
    if (pos + 2 >= cap) {
      cap *= 2;
      char *nb = static_cast<char *>(realloc(out, cap));
      if (!nb) { free(out); return nullptr; }
      out = nb;
    }
    if (*start == '\\' && *(start + 1)) {
      start++;
      switch (*start) {
        case 'n': out[pos++] = '\n'; break;
        case 'r': out[pos++] = '\r'; break;
        case 't': out[pos++] = '\t'; break;
        case '"': out[pos++] = '"'; break;
        case '\\': out[pos++] = '\\'; break;
        default: out[pos++] = *start; break;
      }
    } else {
      out[pos++] = *start;
    }
    start++;
  }
  out[pos] = '\0';
  return out;
}

}  // namespace

// llm_chat(prompt: str, system_prompt: str, model: str, api_key: str, endpoint: str) -> str
// 调用 OpenAI 兼容的 chat completions API。
//
// 参数：
//   prompt       - 用户消息
//   system_prompt - 系统提示词（可为空字符串）
//   model        - 模型名称，如 "gpt-4o"、"deepseek-chat"
//   api_key      - API 密钥
//   endpoint     - API 端点，如 "https://api.openai.com/v1/chat/completions"
//
// 返回：模型的文本回复
extern "C" xmex::Value llm_chat(const xmex::Value *prompt,
                               const xmex::Value *systemPrompt,
                               const xmex::Value *model,
                               const xmex::Value *apiKey,
                               const xmex::Value *endpoint) {
  if (prompt->tag != xmex::kStr || model->tag != xmex::kStr ||
      apiKey->tag != xmex::kStr || endpoint->tag != xmex::kStr) {
    runtime_error("llm_chat() 需要字符串参数: prompt, model, api_key, endpoint");
  }

  const char *p = str_data(prompt);
  const char *sp = (systemPrompt->tag == xmex::kStr) ? str_data(systemPrompt) : "";
  const char *m = str_data(model);
  const char *key = str_data(apiKey);
  const char *ep = str_data(endpoint);

  int64_t pLen = str_size(prompt);
  int64_t spLen = (systemPrompt->tag == xmex::kStr) ? str_size(systemPrompt) : 0;
  int64_t mLen = str_size(model);
  int64_t keyLen = str_size(apiKey);
  int64_t epLen = str_size(endpoint);

  // JSON 转义用户输入
  char *escPrompt = jsonEscape(p, pLen);
  char *escSystem = spLen > 0 ? jsonEscape(sp, spLen) : nullptr;

  if (!escPrompt) runtime_error("内存不足");

  // 构造 JSON 请求体
  size_t jsonCap = static_cast<size_t>(pLen + spLen + mLen + keyLen + epLen) + 1024;
  char *jsonBody = static_cast<char *>(malloc(jsonCap));
  if (!jsonBody) { free(escPrompt); free(escSystem); runtime_error("内存不足"); }

  int n = snprintf(jsonBody, jsonCap,
    "{"
    "\"model\":\"%.*s\","
    "\"messages\":[",
    static_cast<int>(mLen), m);

  if (escSystem && spLen > 0) {
    n += snprintf(jsonBody + n, jsonCap - static_cast<size_t>(n),
      "{\"role\":\"system\",\"content\":\"%s\"},", escSystem);
  }

  n += snprintf(jsonBody + n, jsonCap - static_cast<size_t>(n),
    "{\"role\":\"user\",\"content\":\"%s\"}"
    "],"
    "\"temperature\":0.7,"
    "\"max_tokens\":2048"
    "}", escPrompt);

  free(escPrompt);
  free(escSystem);

  // 构造 curl 命令
  // 使用 --data-raw 避免 shell 对 JSON 中特殊字符的二次解释
  size_t cmdCap = jsonCap + keyLen + epLen + 512;
  char *cmd = static_cast<char *>(malloc(cmdCap));
  if (!cmd) { free(jsonBody); runtime_error("内存不足"); }

  snprintf(cmd, cmdCap,
    "curl -s -X POST '%.*s' "
    "-H 'Content-Type: application/json' "
    "-H 'Authorization: Bearer %.*s' "
    "--data-raw '%s' 2>&1",
    static_cast<int>(epLen), ep,
    static_cast<int>(keyLen), key,
    jsonBody);

  free(jsonBody);

  char *response = captureCommand(cmd);
  free(cmd);

  if (!response || strlen(response) == 0) {
    free(response);
    runtime_error("LLM API 调用失败: 无响应");
  }

  // 检查错误响应
  if (strstr(response, "\"error\"")) {
    xmex::Value err = str_new(response, static_cast<int64_t>(strlen(response)));
    free(response);
    char errBuf[1024];
    snprintf(errBuf, sizeof(errBuf), "LLM API 返回错误: %s", str_data(&err));
    runtime_error(errBuf);
  }

  // 提取 content 字段
  char *content = extractContent(response);
  if (!content) {
    // 如果解析失败，返回原始响应（截断到合理长度）
    int64_t respLen = static_cast<int64_t>(strlen(response));
    if (respLen > 2000) respLen = 2000;
    xmex::Value result = str_new(response, respLen);
    free(response);
    return result;
  }

  xmex::Value result = str_new(content, static_cast<int64_t>(strlen(content)));
  free(response);
  free(content);
  return result;
}

// llm_create_network(name: str, layers: str) -> str
// 创建一个神经网络结构描述（JSON 格式）。
//
// layers 是一个 JSON 数组字符串，每个元素描述一层：
//   [{"type":"linear","in":784,"out":256},
//    {"type":"relu"},
//    {"type":"linear","in":256,"out":10}]
//
// 返回完整的网络描述 JSON 字符串。
extern "C" xmex::Value llm_create_network(const xmex::Value *name,
                                          const xmex::Value *layers) {
  if (name->tag != xmex::kStr || layers->tag != xmex::kStr) {
    runtime_error("llm_create_network() 需要两个字符串参数: name, layers");
  }

  const char *n = str_data(name);
  const char *l = str_data(layers);
  int64_t nLen = str_size(name);
  int64_t lLen = str_size(layers);

  size_t cap = static_cast<size_t>(nLen + lLen) + 128;
  char *json = static_cast<char *>(malloc(cap));
  if (!json) runtime_error("内存不足");

  snprintf(json, cap,
    "{\"name\":\"%.*s\",\"layers\":%.*s}",
    static_cast<int>(nLen), n,
    static_cast<int>(lLen), l);

  xmex::Value result = str_new(json, static_cast<int64_t>(strlen(json)));
  free(json);
  return result;
}

// llm_add_layer(network: str, layer: str) -> str
// 向已有网络描述中添加一层。
//
// 返回修改后的网络描述 JSON 字符串。
extern "C" xmex::Value llm_add_layer(const xmex::Value *network,
                                     const xmex::Value *layer) {
  if (network->tag != xmex::kStr || layer->tag != xmex::kStr) {
    runtime_error("llm_add_layer() 需要两个字符串参数: network, layer");
  }

  const char *net = str_data(network);
  const char *lay = str_data(layer);
  int64_t netLen = str_size(network);
  int64_t layLen = str_size(layer);

  // 在最后一个 ] 之前插入新层
  // 简单实现：找到 "layers":[ 后面的内容，在最后的 ] 前插入
  const char *layersStart = strstr(net, "\"layers\":[");
  if (!layersStart) {
    runtime_error("llm_add_layer() 失败: 无效的网络描述格式");
  }

  // 找到 layers 数组的结束 ]
  const char *arrStart = strchr(layersStart, '[');
  if (!arrStart) {
    runtime_error("llm_add_layer() 失败: 无效的网络描述格式");
  }

  // 从后往前找最后一个 ]
  const char *end = net + netLen - 1;
  while (end > arrStart && *end != ']') end--;
  if (end <= arrStart) {
    runtime_error("llm_add_layer() 失败: 无效的网络描述格式");
  }

  // 构造新 JSON：前缀 + 逗号 + 新层 + 后缀
  size_t prefixLen = static_cast<size_t>(end - net);
  size_t suffixLen = static_cast<size_t>(netLen - (end - net));

  // 检查数组是否为空
  bool isEmpty = (end == arrStart + 1);

  size_t cap = prefixLen + static_cast<size_t>(layLen) + suffixLen + 4;
  char *newJson = static_cast<char *>(malloc(cap));
  if (!newJson) runtime_error("内存不足");

  if (isEmpty) {
    snprintf(newJson, cap, "%.*s%.*s%.*s",
             static_cast<int>(prefixLen), net,
             static_cast<int>(layLen), lay,
             static_cast<int>(suffixLen), end);
  } else {
    snprintf(newJson, cap, "%.*s,%.*s%.*s",
             static_cast<int>(prefixLen), net,
             static_cast<int>(layLen), lay,
             static_cast<int>(suffixLen), end);
  }

  xmex::Value result = str_new(newJson, static_cast<int64_t>(strlen(newJson)));
  free(newJson);
  return result;
}

// llm_remove_layer(network: str, index: int) -> str
// 从网络描述中移除指定索引的层（从 0 开始）。
//
// 返回修改后的网络描述 JSON 字符串。
extern "C" xmex::Value llm_remove_layer(const xmex::Value *network,
                                        const xmex::Value *index) {
  if (network->tag != xmex::kStr || index->tag != xmex::kInt) {
    runtime_error("llm_remove_layer() 需要 (str, int) 参数");
  }

  // 这个功能比较复杂，需要完整的 JSON 解析。
  // 当前版本返回错误提示，建议用户用 llm_create_network 重建。
  runtime_error(
    "llm_remove_layer() 暂未实现。"
    "建议使用 llm_create_network() 重新构建网络描述，"
    "或使用 llm_add_layer() 逐层构建。");
}

// llm_network_summary(network: str) -> str
// 生成网络结构的可读摘要。
extern "C" xmex::Value llm_network_summary(const xmex::Value *network) {
  if (network->tag != xmex::kStr) {
    runtime_error("llm_network_summary() 需要一个字符串参数: network");
  }

  // 直接返回网络描述本身作为摘要
  // 未来可以扩展为解析 JSON 并生成格式化的表格
  const char *net = str_data(network);
  int64_t netLen = str_size(network);

  size_t cap = static_cast<size_t>(netLen) + 256;
  char *summary = static_cast<char *>(malloc(cap));
  if (!summary) runtime_error("内存不足");

  snprintf(summary, cap,
    "[Xmex 神经网络描述]\n%.*s\n\n"
    "提示: 使用 llm_add_layer() 添加层，使用 llm_chat() 调用推理 API。",
    static_cast<int>(netLen), net);

  xmex::Value result = str_new(summary, static_cast<int64_t>(strlen(summary)));
  free(summary);
  return result;
}

// ===========================================================================
// LLM 微调（Fine-tuning）
// ===========================================================================

// llm_create_dataset(examples: str, system_prompt: str) -> str
// 从 Xmex 列表生成 JSONL 格式的训练数据。
//
// examples 是一个 JSON 数组字符串，每个元素是 {"input":"...", "output":"..."}：
//   [{"input":"1+1=?","output":"2"},
//    {"input":"hello in French?","output":"bonjour"}]
//
// system_prompt 是可选的系统提示词（可为空字符串）。
//
// 返回 JSONL 格式的训练数据字符串（每行一个 JSON 对象）。
extern "C" xmex::Value llm_create_dataset(const xmex::Value *examples,
                                          const xmex::Value *systemPrompt) {
  if (examples->tag != xmex::kStr) {
    runtime_error("llm_create_dataset() 需要字符串参数: examples (JSON 数组)");
  }

  const char *ex = str_data(examples);
  int64_t exLen = str_size(examples);
  const char *sp = (systemPrompt->tag == xmex::kStr) ? str_data(systemPrompt) : "";
  int64_t spLen = (systemPrompt->tag == xmex::kStr) ? str_size(systemPrompt) : 0;

  // 简单解析 JSON 数组：按 {"input": 分割每个示例
  // 构造 JSONL 格式：每行 {"messages":[{"role":"system","content":"..."},
  //                                     {"role":"user","content":"..."},
  //                                     {"role":"assistant","content":"..."}]}

  size_t cap = static_cast<size_t>(exLen + spLen) * 3 + 4096;
  char *jsonl = static_cast<char *>(malloc(cap));
  if (!jsonl) runtime_error("内存不足");

  size_t pos = 0;
  const char *p = ex;
  const char *end = ex + exLen;

  while (p < end) {
    // 跳过空白和逗号
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
    if (p >= end || *p == ']') break;
    if (*p != '{') { p++; continue; }

    // 找到一个 JSON 对象，提取 input 和 output
    const char *objStart = p;
    int braceDepth = 0;
    while (p < end) {
      if (*p == '{') braceDepth++;
      else if (*p == '}') { braceDepth--; if (braceDepth == 0) { p++; break; } }
      p++;
    }

    // 提取 "input":"..." 和 "output":"..."
    const char *inputStart = nullptr, *inputEnd = nullptr;
    const char *outputStart = nullptr, *outputEnd = nullptr;

    const char *scan = objStart;
    while (scan < p) {
      const char *key = strstr(scan, "\"input\"");
      if (key && key < p) {
        const char *val = strchr(key + 7, '"');
        if (val) {
          val = strchr(val + 1, '"');
          if (val) { inputStart = val + 1; val = strchr(inputStart, '"'); if (val) inputEnd = val; }
        }
      }
      key = strstr(scan, "\"output\"");
      if (key && key < p) {
        const char *val = strchr(key + 8, '"');
        if (val) {
          val = strchr(val + 1, '"');
          if (val) { outputStart = val + 1; val = strchr(outputStart, '"'); if (val) outputEnd = val; }
        }
      }
      if (inputStart && outputStart) break;
      scan++;
    }

    if (!inputStart || !outputStart) continue;

    // 构造 JSONL 行
    int64_t inLen = inputEnd - inputStart;
    int64_t outLen = outputEnd - outputStart;

    char *escInput = jsonEscape(inputStart, inLen);
    char *escOutput = jsonEscape(outputStart, outLen);

    if (!escInput || !escOutput) {
      free(escInput); free(escOutput);
      continue;
    }

    int n = 0;
    if (spLen > 0) {
      char *escSystem = jsonEscape(sp, spLen);
      n = snprintf(jsonl + pos, cap - pos,
        "{\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"},"
        "{\"role\":\"assistant\",\"content\":\"%s\"}"
        "]}\n",
        escSystem, escInput, escOutput);
      free(escSystem);
    } else {
      n = snprintf(jsonl + pos, cap - pos,
        "{\"messages\":["
        "{\"role\":\"user\",\"content\":\"%s\"},"
        "{\"role\":\"assistant\",\"content\":\"%s\"}"
        "]}\n",
        escInput, escOutput);
    }

    free(escInput);
    free(escOutput);

    if (n > 0) pos += static_cast<size_t>(n);
  }

  jsonl[pos] = '\0';
  xmex::Value result = str_new(jsonl, static_cast<int64_t>(pos));
  free(jsonl);
  return result;
}

// llm_upload_file(data: str, filename: str, api_key: str, endpoint: str) -> str
// 上传训练文件到 OpenAI 兼容 API。
//
// endpoint 是 files API 端点，如 "https://api.openai.com/v1/files"
//
// 返回文件 ID 字符串。
extern "C" xmex::Value llm_upload_file(const xmex::Value *data,
                                       const xmex::Value *filename,
                                       const xmex::Value *apiKey,
                                       const xmex::Value *endpoint) {
  if (data->tag != xmex::kStr || filename->tag != xmex::kStr ||
      apiKey->tag != xmex::kStr || endpoint->tag != xmex::kStr) {
    runtime_error("llm_upload_file() 需要四个字符串参数");
  }

  const char *d = str_data(data);
  const char *fn = str_data(filename);
  const char *key = str_data(apiKey);
  const char *ep = str_data(endpoint);

  int64_t dLen = str_size(data);
  int64_t fnLen = str_size(filename);
  int64_t keyLen = str_size(apiKey);
  int64_t epLen = str_size(endpoint);

  // 将数据写入临时文件
  char tmpPath[256];
  snprintf(tmpPath, sizeof(tmpPath), "/tmp/xmex_upload_%.*s",
           static_cast<int>(fnLen), fn);

  FILE *fp = fopen(tmpPath, "w");
  if (!fp) runtime_error("llm_upload_file() 失败: 无法创建临时文件");
  fwrite(d, 1, static_cast<size_t>(dLen), fp);
  fclose(fp);

  // 构造 curl 命令上传文件
  size_t cmdCap = static_cast<size_t>(epLen + keyLen + fnLen) + 512;
  char *cmd = static_cast<char *>(malloc(cmdCap));
  if (!cmd) runtime_error("内存不足");

  snprintf(cmd, cmdCap,
    "curl -s -X POST '%.*s' "
    "-H 'Authorization: Bearer %.*s' "
    "-F 'purpose=fine-tune' "
    "-F 'file=@%s' 2>&1",
    static_cast<int>(epLen), ep,
    static_cast<int>(keyLen), key,
    tmpPath);

  char *response = captureCommand(cmd);
  free(cmd);
  remove(tmpPath);

  if (!response || strlen(response) == 0) {
    free(response);
    runtime_error("文件上传失败: 无响应");
  }

  // 提取文件 ID
  const char *idKey = "\"id\":\"";
  const char *idStart = strstr(response, idKey);
  char *fileId = nullptr;

  if (idStart) {
    idStart += strlen(idKey);
    const char *idEnd = strchr(idStart, '"');
    if (idEnd) {
      int64_t idLen = idEnd - idStart;
      fileId = static_cast<char *>(malloc(static_cast<size_t>(idLen) + 1));
      if (fileId) {
        memcpy(fileId, idStart, static_cast<size_t>(idLen));
        fileId[idLen] = '\0';
      }
    }
  }

  if (!fileId) {
    // 返回原始响应作为错误信息
    xmex::Value result = str_new(response, static_cast<int64_t>(strlen(response)));
    free(response);
    return result;
  }

  xmex::Value result = str_new(fileId, static_cast<int64_t>(strlen(fileId)));
  free(response);
  free(fileId);
  return result;
}

// llm_create_finetune(file_id: str, model: str, suffix: str,
//                      api_key: str, endpoint: str) -> str
// 创建微调任务。
//
// endpoint 是 fine-tunes API 端点，如 "https://api.openai.com/v1/fine_tuning/jobs"
//
// 返回微调任务 ID 字符串。
extern "C" xmex::Value llm_create_finetune(const xmex::Value *fileId,
                                           const xmex::Value *model,
                                           const xmex::Value *suffix,
                                           const xmex::Value *apiKey,
                                           const xmex::Value *endpoint) {
  if (fileId->tag != xmex::kStr || model->tag != xmex::kStr ||
      apiKey->tag != xmex::kStr || endpoint->tag != xmex::kStr) {
    runtime_error("llm_create_finetune() 需要字符串参数: file_id, model, api_key, endpoint");
  }

  const char *fid = str_data(fileId);
  const char *m = str_data(model);
  const char *suf = (suffix->tag == xmex::kStr) ? str_data(suffix) : "xmex-model";
  const char *key = str_data(apiKey);
  const char *ep = str_data(endpoint);

  int64_t fidLen = str_size(fileId);
  int64_t mLen = str_size(model);
  int64_t sufLen = (suffix->tag == xmex::kStr) ? str_size(suffix) : 11;
  int64_t keyLen = str_size(apiKey);
  int64_t epLen = str_size(endpoint);

  // 构造 JSON 请求体
  size_t jsonCap = static_cast<size_t>(fidLen + mLen + sufLen) + 256;
  char *jsonBody = static_cast<char *>(malloc(jsonCap));
  if (!jsonBody) runtime_error("内存不足");

  snprintf(jsonBody, jsonCap,
    "{\"training_file\":\"%.*s\","
    "\"model\":\"%.*s\","
    "\"suffix\":\"%.*s\"}",
    static_cast<int>(fidLen), fid,
    static_cast<int>(mLen), m,
    static_cast<int>(sufLen), suf);

  // 构造 curl 命令
  size_t cmdCap = jsonCap + static_cast<size_t>(epLen + keyLen) + 256;
  char *cmd = static_cast<char *>(malloc(cmdCap));
  if (!cmd) { free(jsonBody); runtime_error("内存不足"); }

  snprintf(cmd, cmdCap,
    "curl -s -X POST '%.*s' "
    "-H 'Content-Type: application/json' "
    "-H 'Authorization: Bearer %.*s' "
    "--data-raw '%s' 2>&1",
    static_cast<int>(epLen), ep,
    static_cast<int>(keyLen), key,
    jsonBody);

  free(jsonBody);

  char *response = captureCommand(cmd);
  free(cmd);

  if (!response || strlen(response) == 0) {
    free(response);
    runtime_error("微调任务创建失败: 无响应");
  }

  // 提取任务 ID
  const char *idKey = "\"id\":\"";
  const char *idStart = strstr(response, idKey);
  char *jobId = nullptr;

  if (idStart) {
    idStart += strlen(idKey);
    const char *idEnd = strchr(idStart, '"');
    if (idEnd) {
      int64_t idLen = idEnd - idStart;
      jobId = static_cast<char *>(malloc(static_cast<size_t>(idLen) + 1));
      if (jobId) {
        memcpy(jobId, idStart, static_cast<size_t>(idLen));
        jobId[idLen] = '\0';
      }
    }
  }

  if (!jobId) {
    xmex::Value result = str_new(response, static_cast<int64_t>(strlen(response)));
    free(response);
    return result;
  }

  xmex::Value result = str_new(jobId, static_cast<int64_t>(strlen(jobId)));
  free(response);
  free(jobId);
  return result;
}

// llm_finetune_status(job_id: str, api_key: str, endpoint: str) -> str
// 查询微调任务状态。
//
// endpoint 格式: "https://api.openai.com/v1/fine_tuning/jobs/{job_id}"
// 如果 endpoint 不含 {job_id}，会自动拼接。
extern "C" xmex::Value llm_finetune_status(const xmex::Value *jobId,
                                           const xmex::Value *apiKey,
                                           const xmex::Value *endpoint) {
  if (jobId->tag != xmex::kStr || apiKey->tag != xmex::kStr || endpoint->tag != xmex::kStr) {
    runtime_error("llm_finetune_status() 需要三个字符串参数");
  }

  const char *jid = str_data(jobId);
  const char *key = str_data(apiKey);
  const char *ep = str_data(endpoint);

  int64_t jidLen = str_size(jobId);
  int64_t keyLen = str_size(apiKey);
  int64_t epLen = str_size(endpoint);

  // 构造 curl 命令
  size_t cmdCap = static_cast<size_t>(epLen + jidLen + keyLen) + 256;
  char *cmd = static_cast<char *>(malloc(cmdCap));
  if (!cmd) runtime_error("内存不足");

  // 如果 endpoint 不含 job_id，自动拼接
  if (strstr(ep, "{job_id}") || strstr(ep, "%s")) {
    snprintf(cmd, cmdCap,
      "curl -s -X GET '%.*s' "
      "-H 'Authorization: Bearer %.*s' 2>&1",
      static_cast<int>(epLen), ep,
      static_cast<int>(keyLen), key);
  } else {
    snprintf(cmd, cmdCap,
      "curl -s -X GET '%.*s/%.*s' "
      "-H 'Authorization: Bearer %.*s' 2>&1",
      static_cast<int>(epLen), ep,
      static_cast<int>(jidLen), jid,
      static_cast<int>(keyLen), key);
  }

  char *response = captureCommand(cmd);
  free(cmd);

  if (!response || strlen(response) == 0) {
    free(response);
    runtime_error("微调状态查询失败: 无响应");
  }

  xmex::Value result = str_new(response, static_cast<int64_t>(strlen(response)));
  free(response);
  return result;
}

// llm_list_finetunes(api_key: str, endpoint: str) -> str
// 列出所有微调任务。
extern "C" xmex::Value llm_list_finetunes(const xmex::Value *apiKey,
                                          const xmex::Value *endpoint) {
  if (apiKey->tag != xmex::kStr || endpoint->tag != xmex::kStr) {
    runtime_error("llm_list_finetunes() 需要两个字符串参数");
  }

  const char *key = str_data(apiKey);
  const char *ep = str_data(endpoint);
  int64_t keyLen = str_size(apiKey);
  int64_t epLen = str_size(endpoint);

  size_t cmdCap = static_cast<size_t>(epLen + keyLen) + 256;
  char *cmd = static_cast<char *>(malloc(cmdCap));
  if (!cmd) runtime_error("内存不足");

  snprintf(cmd, cmdCap,
    "curl -s -X GET '%.*s' "
    "-H 'Authorization: Bearer %.*s' 2>&1",
    static_cast<int>(epLen), ep,
    static_cast<int>(keyLen), key);

  char *response = captureCommand(cmd);
  free(cmd);

  if (!response || strlen(response) == 0) {
    free(response);
    runtime_error("微调列表查询失败: 无响应");
  }

  xmex::Value result = str_new(response, static_cast<int64_t>(strlen(response)));
  free(response);
  return result;
}
