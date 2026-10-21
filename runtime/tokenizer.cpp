// Tokenizer 分词器 —— BPE 编码/解码
//
// 支持 HuggingFace tokenizer.json 格式的精简解析。
// 实现核心功能：文本→token ID（编码）、token ID→文本（解码）。
//
// 设计原则：
//   1. 零外部依赖：纯 C++ 实现，不依赖 Python/HuggingFace 库
//   2. 内存高效：vocab 使用哈希表存储，支持 150K+ 词表
//   3. 与推理引擎集成：直接输出 token ID 供模型使用
#include "xmex/runtime.h"

#include "xmex/align.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace {

// 全局 tokenizer 状态
struct TokenizerState {
  bool loaded = false;
  std::unordered_map<std::string, int> vocab;       // token → id
  std::vector<std::string> idToToken;                // id → token
  std::unordered_map<int, std::string> addedTokens;  // 特殊 token
  int bosTokenId = -1;
  int eosTokenId = -1;
  int vocabSize = 0;
};

TokenizerState g_tokenizer;

// 简单的 JSON 字符串值提取（支持转义）
std::string jsonGetStringVal(const char *json, const char *key) {
  std::string search = "\"" + std::string(key) + "\":\"";
  const char *start = strstr(json, search.c_str());
  if (!start) {
    // 尝试数字值
    search = "\"" + std::string(key) + "\":";
    start = strstr(json, search.c_str());
    if (!start) return "";
    start += search.size();
    // 跳过空白
    while (*start == ' ' || *start == '\t') start++;
    if (*start == '"') {
      start++;
      const char *end = strchr(start, '"');
      if (!end) return "";
      return std::string(start, static_cast<size_t>(end - start));
    }
    // 数字值
    const char *end = start;
    while (*end && (*end >= '0' && *end <= '9' || *end == '-')) end++;
    return std::string(start, static_cast<size_t>(end - start));
  }
  start += search.size();
  const char *end = start;
  while (*end && *end != '"') {
    if (*end == '\\' && *(end + 1)) end++;
    end++;
  }
  return std::string(start, static_cast<size_t>(end - start));
}

// 简单的 JSON 整数值提取
int jsonGetIntVal(const char *json, const char *key) {
  std::string val = jsonGetStringVal(json, key);
  if (val.empty()) return -1;
  return std::stoi(val);
}

}  // namespace

// ===========================================================================
// Tokenizer 加载
// ===========================================================================

// tokenizer_load(path: str) -> str
// 加载 tokenizer.json 文件。
extern "C" xmex::Value tokenizer_load(const xmex::Value *path) {
  if (path->tag != xmex::kStr) {
    runtime_error("tokenizer_load() 需要一个字符串参数: path");
  }

  const char *p = str_data(path);

  // 读取文件。
  //
  // 用 xmex::readFilePrefix 取代手写的 fopen/fseek/fread:
  //   1. 缓冲首地址按 64 字节对齐 —— 后面在这个缓冲里做 strstr 扫描
  //      找 "added_tokens" / "vocab" 时,不会因为跨缓存行而慢;
  //   2. 内部给 stdio 挂了 64KB 缓冲,读系统调用次数大幅减少;
  //   3. 顺带修掉了原来的一个 bug:早先用的是 fopen(p, "r") —— 文本模式。
  //      Windows 上文本模式会把 \n 翻译成 \r\n,JSON 里的字符串字面量
  //      会被改写,导致某些 tokenizer 的 content 解析出错。
  //      这里用二进制模式,跨平台行为一致。
  //
  // 只读取前 2MB 用于解析 vocab(完整文件 11MB,大部分是 vocab 数据)
  constexpr size_t kMaxRead = 2 * 1024 * 1024;
  xmex::AlignedBuffer json = xmex::readFilePrefix(p, kMaxRead);
  if (!json) runtime_error("无法打开 tokenizer 文件");
  if (json.empty()) runtime_error("tokenizer 文件为空");

  // 解析 added_tokens
  const char *addedStart = strstr(json.data(), "\"added_tokens\":[");
  if (addedStart) {
    const char *p2 = addedStart + 16;
    while (*p2) {
      if (*p2 == ']') break;
      if (*p2 == '{') {
        int tid = -1;
        std::string content;
        bool special = false;

        // 解析 id
        const char *idKey = strstr(p2, "\"id\":");
        if (idKey && idKey < strchr(p2, '}')) {
          idKey += 5;
          tid = static_cast<int>(strtol(idKey, nullptr, 10));
        }

        // 解析 content
        const char *contentKey = strstr(p2, "\"content\":\"");
        if (contentKey && contentKey < strchr(p2, '}')) {
          contentKey += 11;
          const char *contentEnd = strchr(contentKey, '"');
          if (contentEnd) {
            content = std::string(contentKey, static_cast<size_t>(contentEnd - contentKey));
          }
        }

        // 解析 special
        if (strstr(p2, "\"special\":true")) special = true;

        if (tid >= 0 && !content.empty()) {
          g_tokenizer.addedTokens[tid] = content;
          if (special) {
            if (content == "<|endoftext|>" || content == "<|im_end|>") {
              g_tokenizer.eosTokenId = tid;
            }
            if (content == "<|im_start|>" || content == "<s>") {
              g_tokenizer.bosTokenId = tid;
            }
          }
        }

        // 跳到下一个 }
        p2 = strchr(p2, '}');
        if (!p2) break;
      }
      p2++;
    }
  }

  // 解析 vocab（model.vocab 部分）
  const char *vocabStart = strstr(json.data(), "\"vocab\":{");
  if (vocabStart) {
    vocabStart += 8;  // 跳过 "vocab":
    const char *p2 = vocabStart;
    int depth = 0;

    while (*p2) {
      if (*p2 == '{') {
        depth++;
        p2++;
        continue;
      }
      if (*p2 == '}') {
        depth--;
        if (depth == 0) break;
        p2++;
        continue;
      }

      if (*p2 == '"') {
        p2++;
        const char *tokenStart = p2;
        while (*p2 && *p2 != '"') {
          if (*p2 == '\\' && *(p2 + 1)) p2++;
          p2++;
        }
        std::string token(tokenStart, static_cast<size_t>(p2 - tokenStart));
        p2++;  // 跳过结束引号

        // 跳过 :
        while (*p2 && (*p2 == ' ' || *p2 == ':' || *p2 == '\t')) p2++;

        // 读取数字 ID
        char *endptr = nullptr;
        int tid = static_cast<int>(strtol(p2, &endptr, 10));
        if (endptr > p2) {
          g_tokenizer.vocab[token] = tid;
          if (tid >= static_cast<int>(g_tokenizer.idToToken.size())) {
            g_tokenizer.idToToken.resize(static_cast<size_t>(tid) + 1);
          }
          g_tokenizer.idToToken[static_cast<size_t>(tid)] = token;
          p2 = endptr;
        }

        // 跳过逗号
        while (*p2 && (*p2 == ' ' || *p2 == ',' || *p2 == '\t' || *p2 == '\n')) p2++;
        continue;
      }
      p2++;
    }
  }

  g_tokenizer.vocabSize = static_cast<int>(g_tokenizer.idToToken.size());
  g_tokenizer.loaded = true;


  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"status\":\"loaded\",\"vocab_size\":%d,\"added_tokens\":%zu,"
    "\"bos_token_id\":%d,\"eos_token_id\":%d}",
    g_tokenizer.vocabSize, g_tokenizer.addedTokens.size(),
    g_tokenizer.bosTokenId, g_tokenizer.eosTokenId);

  return str_new(buf, static_cast<int64_t>(strlen(buf)));
}

// ===========================================================================
// 编码（文本 → Token IDs）
// ===========================================================================

// tokenizer_encode(text: str) -> str
// 将文本编码为 token ID 列表（JSON 数组格式）。
//
// 使用简单的 BPE 分词：从长到短匹配 vocab 中的 token。
extern "C" xmex::Value tokenizer_encode(const xmex::Value *text) {
  if (!g_tokenizer.loaded) {
    runtime_error("tokenizer 未加载，请先调用 tokenizer_load()");
  }

  if (text->tag != xmex::kStr) {
    runtime_error("tokenizer_encode() 需要一个字符串参数: text");
  }

  const char *t = str_data(text);
  int64_t tLen = str_size(text);
  std::string input(t, static_cast<size_t>(tLen));

  std::vector<int> tokens;

  // 添加 BOS token
  if (g_tokenizer.bosTokenId >= 0) {
    tokens.push_back(g_tokenizer.bosTokenId);
  }

  // 简单的贪婪分词：从当前位置开始，找最长的匹配 token
  size_t pos = 0;
  while (pos < input.size()) {
    // 跳过空白
    if (input[pos] == ' ') {
      // 查找空格对应的 token
      auto it = g_tokenizer.vocab.find(" ");
      if (it != g_tokenizer.vocab.end()) {
        tokens.push_back(it->second);
      }
      pos++;
      continue;
    }

    // 尝试从长到短匹配
    bool found = false;
    size_t maxLen = std::min(input.size() - pos, static_cast<size_t>(32));
    for (size_t len = maxLen; len > 0; --len) {
      std::string sub = input.substr(pos, len);
      auto it = g_tokenizer.vocab.find(sub);
      if (it != g_tokenizer.vocab.end()) {
        tokens.push_back(it->second);
        pos += len;
        found = true;
        break;
      }
    }

    if (!found) {
      // 未匹配的字符：尝试逐字节编码
      std::string byteStr = std::string("<0x") +
                            std::to_string(static_cast<int>(static_cast<unsigned char>(input[pos]))) +
                            ">";
      auto it = g_tokenizer.vocab.find(byteStr);
      if (it != g_tokenizer.vocab.end()) {
        tokens.push_back(it->second);
      }
      pos++;
    }
  }

  // 构造 JSON 数组输出
  std::string result = "[";
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) result += ",";
    result += std::to_string(tokens[i]);
  }
  result += "]";

  return str_new(result.c_str(), static_cast<int64_t>(result.size()));
}

// ===========================================================================
// 解码（Token IDs → 文本）
// ===========================================================================

// tokenizer_decode(token_ids: str) -> str
// 将 token ID 列表（JSON 数组）解码为文本。
extern "C" xmex::Value tokenizer_decode(const xmex::Value *tokenIds) {
  if (!g_tokenizer.loaded) {
    runtime_error("tokenizer 未加载，请先调用 tokenizer_load()");
  }

  if (tokenIds->tag != xmex::kStr) {
    runtime_error("tokenizer_decode() 需要一个字符串参数: token_ids (JSON 数组)");
  }

  const char *ids = str_data(tokenIds);
  std::string result;

  // 简单解析 JSON 数组
  const char *p = ids;
  while (*p) {
    if (*p == '[' || *p == ' ' || *p == '\n' || *p == ',') { p++; continue; }
    if (*p == ']') break;

    char *endptr = nullptr;
    int tid = static_cast<int>(strtol(p, &endptr, 10));
    if (endptr > p) {
      if (tid >= 0 && tid < static_cast<int>(g_tokenizer.idToToken.size())) {
        std::string token = g_tokenizer.idToToken[static_cast<size_t>(tid)];
        // 跳过特殊 token
        if (g_tokenizer.addedTokens.find(tid) == g_tokenizer.addedTokens.end() ||
            tid == g_tokenizer.bosTokenId || tid == g_tokenizer.eosTokenId) {
          // BOS/EOS 不输出
        } else {
          result += token;
        }
      }
      p = endptr;
    } else {
      p++;
    }
  }

  return str_new(result.c_str(), static_cast<int64_t>(result.size()));
}

// ===========================================================================
// Tokenizer 信息
// ===========================================================================

// tokenizer_info() -> str
// 返回 tokenizer 的详细信息。
extern "C" xmex::Value tokenizer_info() {
  if (!g_tokenizer.loaded) {
    runtime_error("tokenizer 未加载");
  }

  char buf[1024];
  snprintf(buf, sizeof(buf),
    "{\n"
    "  \"vocab_size\": %d,\n"
    "  \"added_tokens\": %zu,\n"
    "  \"bos_token_id\": %d,\n"
    "  \"eos_token_id\": %d,\n"
    "  \"special_tokens\": [",
    g_tokenizer.vocabSize, g_tokenizer.addedTokens.size(),
    g_tokenizer.bosTokenId, g_tokenizer.eosTokenId);

  // 列出特殊 token
  int pos = static_cast<int>(strlen(buf));
  bool first = true;
  for (const auto &kv : g_tokenizer.addedTokens) {
    if (!first) buf[pos++] = ',';
    first = false;
    pos += snprintf(buf + pos, sizeof(buf) - static_cast<size_t>(pos),
                    "{\"id\":%d,\"token\":\"%s\"}", kv.first, kv.second.c_str());
    if (pos > 900) break;
  }
  snprintf(buf + pos, sizeof(buf) - static_cast<size_t>(pos), "]\n}");

  return str_new(buf, static_cast<int64_t>(strlen(buf)));
}
