// 算子融合优化 —— LLM 推理加速。
//
// 设计原则：
//   1. 算子 IR：用 JSON 描述计算图，每个节点是一个算子，**节点显式记录输入边**；
//   2. 融合规则：在真实的数据依赖边上做模式匹配，受循环不变量约束；
//   3. 优化 Pass：CSE、去死节点、链式融合、树形融合、QKV 并联合并；
//   4. CUDA 内核生成：将融合后的算子编译为优化的 CUDA 内核；
//   5. 自动调优：针对不同矩阵规模选择最优配置。
//
// 支持的算子类型：
//   matmul, bias_add, relu, gelu, layernorm, softmax,
//   add, mul, reshape, transpose, concat
//
// ⚠️ 图格式的演进（向后兼容）：
//   旧格式的节点只有 {type,name,params}，没有输入边。旧格式无法表达分叉/合流，
//   因此 parseGraph() 对旧图按**添加顺序串成链**（node[i].inputs = {node[i-1]}），
//   这样旧调用方（api.h 的四参 add_op、已有 .pys 脚本）的行为不变。
//   新格式节点形如 {"type":"relu","name":"r1","params":{},"inputs":["mm1"]}。
#include "xmex/runtime.h"

#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

namespace {

// ===========================================================================
// JSON 解析
// ===========================================================================

// 一个极小的 JSON 值。够用即可：算子图里只有对象/数组/字符串/数字/布尔。
struct JsonValue;
using JsonPtr = std::shared_ptr<JsonValue>;

struct JsonValue {
  enum Kind { kNull, kBool, kNumber, kString, kArray, kObject };

  Kind kind = kNull;
  bool boolean = false;
  double number = 0;
  std::string str;
  std::vector<JsonPtr> array;
  std::vector<std::pair<std::string, JsonPtr>> object;

  const JsonPtr find(const std::string &key) const {
    for (const auto &kv : object) {
      if (kv.first == key) return kv.second;
    }
    return nullptr;
  }

  // 对象取字符串字段；缺失或类型不符时返回 fallback。
  std::string getStr(const std::string &key, const std::string &fallback = "") const {
    JsonPtr v = find(key);
    if (v && v->kind == kString) return v->str;
    return fallback;
  }

  double getNum(const std::string &key, double fallback) const {
    JsonPtr v = find(key);
    if (v && v->kind == kNumber) return v->number;
    return fallback;
  }
};

class JsonParser {
 public:
  JsonParser(const char *begin, const char *end) : p_(begin), end_(end) {}

  // 解析一个值；返回 nullptr 表示 JSON 非法（调用方据此回退到旧格式处理）。
  JsonPtr parse() {
    skipWs();
    JsonPtr v = parseValue();
    if (!v) return nullptr;
    skipWs();
    return v;
  }

 private:
  const char *p_;
  const char *end_;

  void skipWs() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }

  bool consume(char c) {
    skipWs();
    if (p_ < end_ && *p_ == c) {
      ++p_;
      return true;
    }
    return false;
  }

  JsonPtr parseValue() {
    skipWs();
    if (p_ >= end_) return nullptr;
    switch (*p_) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': return parseString();
      case 't': return parseLiteral("true", JsonValue::kBool, true);
      case 'f': return parseLiteral("false", JsonValue::kBool, false);
      case 'n': return parseLiteral("null", JsonValue::kNull, false);
      default:  return parseNumber();
    }
  }

  JsonPtr parseLiteral(const char *word, JsonValue::Kind kind, bool b) {
    size_t len = std::strlen(word);
    if (static_cast<size_t>(end_ - p_) < len) return nullptr;
    if (std::strncmp(p_, word, len) != 0) return nullptr;
    p_ += len;
    auto v = std::make_shared<JsonValue>();
    v->kind = kind;
    v->boolean = b;
    return v;
  }

  JsonPtr parseNumber() {
    const char *start = p_;
    if (p_ < end_ && (*p_ == '-' || *p_ == '+')) ++p_;
    bool any = false;
    while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
      ++p_;
      any = true;
    }
    if (p_ < end_ && *p_ == '.') {
      ++p_;
      while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
        ++p_;
        any = true;
      }
    }
    if (!any) return nullptr;
    auto v = std::make_shared<JsonValue>();
    v->kind = JsonValue::kNumber;
    v->number = std::strtod(std::string(start, p_).c_str(), nullptr);
    return v;
  }

  JsonPtr parseString() {
    if (p_ >= end_ || *p_ != '"') return nullptr;
    ++p_;
    auto v = std::make_shared<JsonValue>();
    v->kind = JsonValue::kString;
    std::string &out = v->str;
    while (p_ < end_) {
      char c = *p_++;
      if (c == '"') return v;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p_ >= end_) return nullptr;
      char esc = *p_++;
      switch (esc) {
        case '"':  out.push_back('"');  break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/');  break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u': {
          // \uXXXX —— 只处理 BMP 内的码点，转成 UTF-8。
          if (end_ - p_ < 4) return nullptr;
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = *p_++;
            cp <<= 4;
            if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else return nullptr;
          }
          if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
          } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          }
          break;
        }
        default: return nullptr;
      }
    }
    return nullptr;  // 未闭合
  }

  JsonPtr parseArray() {
    ++p_;  // '['
    auto v = std::make_shared<JsonValue>();
    v->kind = JsonValue::kArray;
    skipWs();
    if (consume(']')) return v;
    while (true) {
      JsonPtr item = parseValue();
      if (!item) return nullptr;
      v->array.push_back(item);
      skipWs();
      if (consume(',')) continue;
      if (consume(']')) return v;
      return nullptr;
    }
  }

  JsonPtr parseObject() {
    ++p_;  // '{'
    auto v = std::make_shared<JsonValue>();
    v->kind = JsonValue::kObject;
    skipWs();
    if (consume('}')) return v;
    while (true) {
      skipWs();
      JsonPtr key = parseString();
      if (!key) return nullptr;
      if (!consume(':')) return nullptr;
      JsonPtr val = parseValue();
      if (!val) return nullptr;
      v->object.emplace_back(key->str, val);
      skipWs();
      if (consume(',')) continue;
      if (consume('}')) return v;
      return nullptr;
    }
  }
};

// ===========================================================================
// JSON 序列化
// ===========================================================================

void jsonEscapeInto(const std::string &s, std::string &out) {
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xFF);
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

void jsonWriteInto(const JsonPtr &v, std::string &out) {
  if (!v) {
    out += "null";
    return;
  }
  switch (v->kind) {
    case JsonValue::kNull:
      out += "null";
      break;
    case JsonValue::kBool:
      out += v->boolean ? "true" : "false";
      break;
    case JsonValue::kNumber: {
      char buf[40];
      // 整数不带小数点，避免 "768" 变成 "768.000000"
      if (v->number == std::floor(v->number) && std::fabs(v->number) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(v->number));
      } else {
        std::snprintf(buf, sizeof(buf), "%.17g", v->number);
      }
      out += buf;
      break;
    }
    case JsonValue::kString:
      jsonEscapeInto(v->str, out);
      break;
    case JsonValue::kArray: {
      out.push_back('[');
      for (size_t i = 0; i < v->array.size(); ++i) {
        if (i) out.push_back(',');
        jsonWriteInto(v->array[i], out);
      }
      out.push_back(']');
      break;
    }
    case JsonValue::kObject: {
      out.push_back('{');
      for (size_t i = 0; i < v->object.size(); ++i) {
        if (i) out.push_back(',');
        jsonEscapeInto(v->object[i].first, out);
        out.push_back(':');
        jsonWriteInto(v->object[i].second, out);
      }
      out.push_back('}');
      break;
    }
  }
}

std::string jsonWrite(const JsonPtr &v) {
  std::string out;
  jsonWriteInto(v, out);
  return out;
}

// ===========================================================================
// 图结构
// ===========================================================================

struct Node {
  std::string type;
  std::string name;
  std::string params = "{}";
  std::vector<std::string> inputs;  // 上游节点名
  // 融合产生的血缘：被吸收的原始节点名。用于报告与调试，不参与匹配。
  std::vector<std::string> fusedFrom;
  bool isFused = false;
  int64_t users = 0;  // 下游消费者数（由 computeUsers 填充）
};

struct Graph {
  std::string name;
  std::vector<Node> nodes;
  std::vector<std::string> inputs;   // 图输入节点名（无上游者）
  std::vector<std::string> outputs;  // 图输出节点名（computeIO 现算）
  // 用户**显式声明**的 outputs。只有它参与死节点消除的「根」判定 ——
  // 否则 computeIO 算出的 outputs 会把所有无消费者节点都算进去，
  // 悬空节点于是自己成了根，删除就永远不生效。
  std::vector<std::string> explicitOutputs;
};

Node *findNode(Graph &g, const std::string &name) {
  for (auto &n : g.nodes) {
    if (n.name == name) return &n;
  }
  return nullptr;
}

const Node *findNode(const Graph &g, const std::string &name) {
  for (const auto &n : g.nodes) {
    if (n.name == name) return &n;
  }
  return nullptr;
}

// 填充每个节点的下游消费者数，以及图的 inputs/outputs 名单。
void computeIO(Graph &g) {
  std::set<std::string> defined;
  for (auto &n : g.nodes) {
    n.users = 0;
    defined.insert(n.name);
  }

  // 外部输入 = 被引用但没有对应节点的的名字。按首次出现的节点顺序收集,
  // 保证序列化结果稳定(用 set 会按字典序,与建图顺序脱节,不利于 diff)。
  std::vector<std::string> ext;
  std::set<std::string> extSeen;
  for (const auto &n : g.nodes) {
    for (const auto &in : n.inputs) {
      Node *up = findNode(g, in);
      if (up) {
        up->users++;
      } else if (extSeen.insert(in).second) {
        ext.push_back(in);
      }
    }
  }

  g.inputs.clear();
  g.outputs.clear();
  for (const auto &n : g.nodes) {
    if (n.inputs.empty()) g.inputs.push_back(n.name);
    if (n.users == 0) g.outputs.push_back(n.name);
  }
  // 外部输入排在真正无上游的节点之后
  for (const auto &e : ext) g.inputs.push_back(e);
}

// 拓扑排序（Kahn）。返回 false 表示图里有环。
bool topoSort(const Graph &g, std::vector<int> &order) {
  std::map<std::string, int> idx;
  for (size_t i = 0; i < g.nodes.size(); ++i) idx[g.nodes[i].name] = static_cast<int>(i);

  std::vector<int> indeg(g.nodes.size(), 0);
  // 邻接表：upstream -> 下游节点下标
  std::vector<std::vector<int>> adj(g.nodes.size());
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    for (const auto &in : g.nodes[i].inputs) {
      auto it = idx.find(in);
      if (it == idx.end()) continue;
      indeg[i]++;
      adj[it->second].push_back(static_cast<int>(i));
    }
  }

  std::vector<int> ready;
  for (size_t i = 0; i < indeg.size(); ++i) {
    if (indeg[i] == 0) ready.push_back(static_cast<int>(i));
  }

  order.clear();
  order.reserve(g.nodes.size());
  while (!ready.empty()) {
    int cur = ready.back();
    ready.pop_back();
    order.push_back(cur);
    for (int nx : adj[cur]) {
      if (--indeg[nx] == 0) ready.push_back(nx);
    }
  }
  return order.size() == g.nodes.size();
}

// ===========================================================================
// 图解析 / 序列化
// ===========================================================================

// 解析算子图。失败时返回 false。
bool parseGraph(const std::string &text, Graph &g) {
  JsonParser parser(text.data(), text.data() + text.size());
  JsonPtr root = parser.parse();
  if (!root || root->kind != JsonValue::kObject) return false;

  g.name = root->getStr("name");
  JsonPtr nodes = root->find("nodes");
  if (!nodes || nodes->kind != JsonValue::kArray) return false;

  // 先全部读进来，再统一决定连边 —— 这样旧格式（无 inputs）也能按顺序串链。
  std::vector<Node> parsed;
  std::set<std::string> seen;
  for (const auto &pn : nodes->array) {
    if (!pn || pn->kind != JsonValue::kObject) return false;
    Node n;
    n.type = pn->getStr("type");
    n.name = pn->getStr("name");
    if (n.type.empty() || n.name.empty()) return false;
    if (!seen.insert(n.name).second) return false;  // 名字重复，图不合法
    n.params = "{}";
    JsonPtr params = pn->find("params");
    if (params) n.params = jsonWrite(params);
    JsonPtr ins = pn->find("inputs");
    if (ins && ins->kind == JsonValue::kArray) {
      for (const auto &pi : ins->array) {
        if (!pi || pi->kind != JsonValue::kString) return false;
        n.inputs.push_back(pi->str);
      }
    }
    // 没有 "inputs" 字段 = 旧格式，留空稍后按顺序串链
    parsed.push_back(n);
  }

  g.nodes = parsed;

  // 校验所有输入:已定义的节点是图内边,未定义的当作**图输入**
  // (外部张量 —— 权重、激活、residual 分支的旁路输入)。
  //
  // 之所以允许未定义的名字而不是强制先建占位节点:推理图里绝大多数输入
  // 就是常量权重,让用户为每个权重都建一个 input 节点纯属仪式负担。
  // 未定义的名字由 computeIO 收进 graph.inputs。
  //
  // 这里不做拒绝 —— 旧格式图整体没有 inputs 字段,与「引用了外部输入」
  // 在这一层无法区分,真正的环检测交给 topoSort。

  // 旧图（没有任何 inputs 边）按添加顺序串成链
  bool anyEdge = false;
  for (const auto &n : g.nodes) {
    if (!n.inputs.empty()) {
      anyEdge = true;
      break;
    }
  }
  if (!anyEdge) {
    for (size_t i = 1; i < g.nodes.size(); ++i) {
      g.nodes[i].inputs.push_back(g.nodes[i - 1].name);
    }
  }

  JsonPtr outs = root->find("outputs");
  if (outs && outs->kind == JsonValue::kArray) {
    for (const auto &po : outs->array) {
      if (po && po->kind == JsonValue::kString) g.outputs.push_back(po->str);
    }
  }
  JsonPtr expOuts = root->find("explicit_outputs");
  if (expOuts && expOuts->kind == JsonValue::kArray) {
    for (const auto &po : expOuts->array) {
      if (po && po->kind == JsonValue::kString) g.explicitOutputs.push_back(po->str);
    }
  }

  computeIO(g);
  return true;
}

std::string serializeGraph(const Graph &g) {
  std::string out = "{\"name\":";
  jsonEscapeInto(g.name, out);
  out += ",\"nodes\":[";
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    const Node &n = g.nodes[i];
    if (i) out.push_back(',');
    out += "{\"type\":";
    jsonEscapeInto(n.type, out);
    out += ",\"name\":";
    jsonEscapeInto(n.name, out);
    out += ",\"params\":";
    out += n.params.empty() ? "{}" : n.params;
    out += ",\"inputs\":[";
    for (size_t j = 0; j < n.inputs.size(); ++j) {
      if (j) out.push_back(',');
      jsonEscapeInto(n.inputs[j], out);
    }
    out += "]";
    if (n.isFused) {
      out += ",\"fused_from\":[";
      for (size_t j = 0; j < n.fusedFrom.size(); ++j) {
        if (j) out.push_back(',');
        jsonEscapeInto(n.fusedFrom[j], out);
      }
      out += "]";
    }
    out += "}";
  }
  out += "],\"edges\":[";
  bool firstEdge = true;
  for (const auto &n : g.nodes) {
    for (const auto &in : n.inputs) {
      if (!firstEdge) out.push_back(',');
      firstEdge = false;
      out += "[";
      jsonEscapeInto(in, out);
      out.push_back(',');
      jsonEscapeInto(n.name, out);
      out += "]";
    }
  }
  out += "],\"inputs\":[";
  for (size_t i = 0; i < g.inputs.size(); ++i) {
    if (i) out.push_back(',');
    jsonEscapeInto(g.inputs[i], out);
  }
  out += "],\"outputs\":[";
  for (size_t i = 0; i < g.outputs.size(); ++i) {
    if (i) out.push_back(',');
    jsonEscapeInto(g.outputs[i], out);
  }
  out += "]";
  // 显式输出一并序列化 —— 它是死节点消除的依据,丢了会让每次 add_op
  // 都重新猜一遍「谁是输出」。
  if (!g.explicitOutputs.empty()) {
    out += ",\"explicit_outputs\":[";
    for (size_t i = 0; i < g.explicitOutputs.size(); ++i) {
      if (i) out.push_back(',');
      jsonEscapeInto(g.explicitOutputs[i], out);
    }
    out += "]";
  }
  out += "}";
  return out;
}

// 取出节点的 params 对象；不是合法对象时先修成一个空对象。
JsonPtr ensureParamObject(Node &n) {
  JsonParser p(n.params.data(), n.params.data() + n.params.size());
  JsonPtr obj = p.parse();
  if (!obj || obj->kind != JsonValue::kObject) {
    obj = std::make_shared<JsonValue>();
    obj->kind = JsonValue::kObject;
  }
  return obj;
}

// 在 params 对象里写一个字符串字段。
void setParamString(Node &n, const std::string &key, const std::string &value) {
  JsonPtr obj = ensureParamObject(n);
  bool replaced = false;
  for (auto &kv : obj->object) {
    if (kv.first == key) {
      kv.second = std::make_shared<JsonValue>();
      kv.second->kind = JsonValue::kString;
      kv.second->str = value;
      replaced = true;
    }
  }
  if (!replaced) {
    auto v = std::make_shared<JsonValue>();
    v->kind = JsonValue::kString;
    v->str = value;
    obj->object.emplace_back(key, v);
  }
  n.params = jsonWrite(obj);
}

// 在 params 对象里写一个**数字**字段。
//
// ⚠️ 不能拿 setParamString 代替 —— 那会把 2304 写成字符串 "2304",
// 而 QKV 合并的下游要用它做 GEMM 的 N 维。类型错了不会立刻崩，
// 会在生成内核时悄悄取到 0。
void setParamNumber(Node &n, const std::string &key, double value) {
  JsonPtr obj = ensureParamObject(n);
  bool replaced = false;
  for (auto &kv : obj->object) {
    if (kv.first == key) {
      kv.second = std::make_shared<JsonValue>();
      kv.second->kind = JsonValue::kNumber;
      kv.second->number = value;
      replaced = true;
    }
  }
  if (!replaced) {
    auto v = std::make_shared<JsonValue>();
    v->kind = JsonValue::kNumber;
    v->number = value;
    obj->object.emplace_back(key, v);
  }
  n.params = jsonWrite(obj);
}

std::string getParamString(const Node &n, const std::string &key, const std::string &fallback) {
  JsonParser p(n.params.data(), n.params.data() + n.params.size());
  JsonPtr obj = p.parse();
  if (!obj || obj->kind != JsonValue::kObject) return fallback;
  return obj->getStr(key, fallback);
}

}  // namespace

// ===========================================================================
// 算子 IR 构造
// ===========================================================================

// fusion_create_graph(name: str) -> str
// 创建一个空的算子计算图，返回图 JSON。
extern "C" xmex::Value fusion_create_graph(const xmex::Value *name) {
  if (name->tag != xmex::kStr) {
    runtime_error("fusion_create_graph() 需要一个字符串参数: name");
  }
  Graph g;
  g.name.assign(str_data(name), static_cast<size_t>(str_size(name)));
  std::string out = serializeGraph(g);
  return str_new(out.c_str(), static_cast<int64_t>(out.size()));
}

namespace {

// 真正的实现：解析 → 加节点 → 重新序列化。
// inputsArg 为 nullptr 时按旧语义串链（接到最后一个节点之后）。
xmex::Value addOpImpl(const xmex::Value *graph, const xmex::Value *opType,
                      const xmex::Value *opName, const xmex::Value *params,
                      const std::vector<std::string> *inputsArg, bool haveInputsArg) {
  if (graph->tag != xmex::kStr || opType->tag != xmex::kStr || opName->tag != xmex::kStr) {
    runtime_error("fusion_add_op() 需要字符串参数: graph, op_type, op_name");
  }

  std::string text(str_data(graph), static_cast<size_t>(str_size(graph)));
  Graph g;
  if (!parseGraph(text, g)) {
    runtime_error("fusion_add_op() 失败: 计算图 JSON 无法解析");
  }

  Node n;
  n.type.assign(str_data(opType), static_cast<size_t>(str_size(opType)));
  n.name.assign(str_data(opName), static_cast<size_t>(str_size(opName)));
  if (n.type.empty() || n.name.empty()) {
    runtime_error("fusion_add_op() 失败: op_type 与 op_name 不能为空");
  }
  if (findNode(g, n.name)) {
    runtime_error(("fusion_add_op() 失败: 节点名 '" + n.name + "' 已存在").c_str());
  }

  // params 必须是合法 JSON 对象，否则后面的融合无从判断
  std::string paramsText = "{}";
  if (params->tag == xmex::kStr) {
    paramsText.assign(str_data(params), static_cast<size_t>(str_size(params)));
    JsonParser pp(paramsText.data(), paramsText.data() + paramsText.size());
    JsonPtr pv = pp.parse();
    if (!pv || pv->kind != JsonValue::kObject) {
      runtime_error("fusion_add_op() 失败: params 必须是 JSON 对象");
    }
    paramsText = jsonWrite(pv);
  }
  n.params = paramsText;

  if (haveInputsArg && inputsArg) {
    // 显式连边。上游可以是图内节点,也可以是尚未出现的名字 ——
    // 后者视为图输入(外部张量:权重/激活/residual 旁路)。
    // 见 parseGraph 里对 extInputs 的说明。
    n.inputs = *inputsArg;
  } else {
    // 旧语义：接到最后一个节点之后
    if (!g.nodes.empty()) {
      n.inputs.push_back(g.nodes.back().name);
    }
  }

  // 在 push 之前先把「现有无消费者的节点」登记为显式输出。
  //
  // 这样逐个 add_op 串链时,只有**最后一个**节点会被当作图输出 ——
  // 中途分叉出去又没接回来的节点不会被误认为输出,死节点消除才能删掉它们。
  // (反过来,若把显式输出留给 optimize 时再从 computeIO 现算,
  //  所有无消费者节点都会成为根,删除就永远不生效。)
  if (g.explicitOutputs.empty()) {
    computeIO(g);
    for (const auto &old : g.nodes) {
      if (old.users == 0) g.explicitOutputs.push_back(old.name);
    }
  }

  g.nodes.push_back(n);
  computeIO(g);

  // 维护显式输出名单:加了一个节点之后,原来的末端节点可能不再是末端。
  // 规则:新节点成为输出;若旧输出被新节点消费(不再是 users==0),就摘掉它。
  // 这样 explicitOutputs 始终等于「真正被当作图输出看待的节点」。
  if (!g.explicitOutputs.empty()) {
    std::set<std::string> declared(g.explicitOutputs.begin(),
                                   g.explicitOutputs.end());
    const Node *added = findNode(g, n.name);
    if (added && added->users == 0) declared.insert(n.name);
    g.explicitOutputs.clear();
    for (const auto &nm : g.nodes) {
      if (declared.count(nm.name) && nm.users == 0) {
        g.explicitOutputs.push_back(nm.name);
      }
    }
  }

  std::vector<int> order;
  if (!topoSort(g, order)) {
    runtime_error("fusion_add_op() 失败: 连边产生了环");
  }

  std::string out = serializeGraph(g);
  return str_new(out.c_str(), static_cast<int64_t>(out.size()));
}

// 从 Value 里读一个字符串列表：接受 list[str] 或 "a,b,c"。
bool stringListFromValue(const xmex::Value *v, std::vector<std::string> &out) {
  if (!v) return false;
  if (v->tag == xmex::kStr) {
    std::string s(str_data(v), static_cast<size_t>(str_size(v)));
    size_t start = 0;
    while (start <= s.size()) {
      size_t comma = s.find(',', start);
      std::string piece = (comma == std::string::npos)
                              ? s.substr(start)
                              : s.substr(start, comma - start);
      // 去掉首尾空白
      while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) piece.erase(piece.begin());
      while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) piece.pop_back();
      if (!piece.empty()) out.push_back(piece);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    return true;
  }
  if (v->tag == xmex::kList) {
    int64_t n = list_len(v);
    for (int64_t i = 0; i < n; ++i) {
      xmex::Value item = list_get(v, i);
      if (item.tag != xmex::kStr) return false;
      out.emplace_back(str_data(&item), static_cast<size_t>(str_size(&item)));
    }
    return true;
  }
  return false;
}

}  // namespace

// fusion_add_op(graph, op_type, op_name, params) -> str
// 保持原签名（向后兼容）：新节点接到最后一个节点之后。
extern "C" xmex::Value fusion_add_op(const xmex::Value *graph,
                                     const xmex::Value *opType,
                                     const xmex::Value *opName,
                                     const xmex::Value *params) {
  return addOpImpl(graph, opType, opName, params, nullptr, false);
}

// fusion_add_op_v(graph, op_type, op_name, params[, inputs]) -> str
// inputs 可为 list[str] 或 "a,b,c"，显式声明上游节点，支持分叉/合流。
// irgen 在调用点把缺失的第 5 个参数补成 None（见 builtinOptionalArgs）。
extern "C" xmex::Value fusion_add_op_v(const xmex::Value *graph,
                                       const xmex::Value *opType,
                                       const xmex::Value *opName,
                                       const xmex::Value *params, ...) {
  va_list ap;
  va_start(ap, params);
  const xmex::Value *inputs = va_arg(ap, const xmex::Value *);
  va_end(ap);

  if (!inputs || inputs->tag == xmex::kNull) {
    return addOpImpl(graph, opType, opName, params, nullptr, false);
  }
  std::vector<std::string> names;
  if (!stringListFromValue(inputs, names)) {
    runtime_error("fusion.add_op() 的 inputs 必须是字符串列表或逗号分隔的字符串");
  }
  return addOpImpl(graph, opType, opName, params, &names, true);
}

// ===========================================================================
// 优化 Pass
// ===========================================================================

namespace {

// 算子类别。融合是否合法，取决于类别的组合（见 fusable）。
enum OpClass {
  kMatMul,     // matmul
  kBias,       // bias_add
  kAct,        // relu, gelu
  kNorm,       // layernorm, softmax
  kElementwise,// add, mul
  kShape,      // reshape, transpose, concat
  kUnknown,
};

// 算子类型 -> 类别。
//
// ⚠️ 必须能识别**融合体的复合类型**（bias_relu、linear_gelu、add_mul …）。
// 融合是迭代进行的：一轮里 matmul 可能还没来得及吃掉 bias_add，下一轮
// 才碰到已经变成 bias_relu 的节点。classify 若只认原始算子名，
// 这里会返回 kUnknown，matmul 那一轮就被静默跳过 —— 表现为
// 「融了一半就停了」，而且没有任何报错。
OpClass classify(const std::string &t) {
  if (t == "matmul") return kMatMul;
  if (t == "bias_add") return kBias;
  if (t == "relu" || t == "gelu") return kAct;
  if (t == "layernorm" || t == "softmax") return kNorm;
  if (t == "add" || t == "mul") return kElementwise;
  if (t == "reshape" || t == "transpose" || t == "concat") return kShape;

  // 以 matmul 为链头的融合体：仍是「能做 epilogue 融合」的 GEMM 类。
  // 顺序很重要 —— 必须先于下面的逐元素判定（linear_relu 里也有 relu）。
  if (t == "linear" || t.compare(0, 7, "linear_") == 0 ||
      t == "matmul_add" || t == "matmul_mul" ||
      t.compare(0, 10, "matmul_") == 0) {
    return kMatMul;
  }

  // 融合体：按名字里包含的成分判定类别。融合出来的算子一定含一个
  // 逐元素成分（bias_add/relu/gelu/add/mul），据此归入逐元素类。
  if (t.find("bias_") == 0 || t.find("relu") != std::string::npos ||
      t.find("gelu") != std::string::npos || t.find("add_") == 0 ||
      t.find("mul_") == 0 || t == "add_mul" || t == "mul_add") {
    return kElementwise;
  }
  if (t.compare(0, 10, "layernorm_") == 0) return kNorm;
  if (t.compare(0, 8, "softmax_") == 0) return kNorm;
  return kUnknown;
}

// 类别 a（上游）能否吸收类别 b（下游）。
//
// 规则来自实际的访存特征：
//   * 逐元素类（bias/act/elementwise）之间总能融 —— 都是一次读一次写；
//   * matmul 吸收下游是收益最大的场景（epilogue 融合，省掉中间张量往返）；
//   * matmul 被下游吸收**不行**：那等于把 matmul 塞进别人的循环体，
//     这正是旧 layernorm_matmul 内核的毛病（归一化在 O(M·N·K) 内层循环里
//     重复算，融合后比不融合慢三个数量级）；
//   * 归一化类能吸收逐元素类，反向不行（先归约才能逐元素）；
//   * shape 类是布局变换，跨过去融合会打乱访存，保守禁止。
bool fusable(OpClass a, OpClass b) {
  auto elementwiseish = [](OpClass c) {
    return c == kBias || c == kAct || c == kElementwise;
  };
  if (a == kShape || b == kShape) return false;
  if (b == kMatMul) return false;             // matmul 不能被吸收进上游
  if (a == kMatMul) return elementwiseish(b); // epilogue 融合
  if (a == kNorm) return elementwiseish(b) || b == kNorm;
  return elementwiseish(a) && elementwiseish(b);
}

// 融合报告里的一条记录。
struct FusionRecord {
  std::string pattern;      // 规则名，如 linear_relu
  std::string fusedOp;      // 融合后算子名
  std::string kernel;       // 对应 CUDA 内核
  std::vector<std::string> nodes;  // 参与融合的节点名（按数据流顺序）
};

// 融合后算子的命名。返回空串表示这对类型没有专门的融合算子（不融）。
//
// 参数语义是「融合体当前类型 + 被吸收的下游类型」→ 新类型，
// 融合体永远站在链头。这样名字才能沿链延续：
//
//     matmul --吸 bias_add--> linear --吸 relu--> linear_relu
//
// 而不是每轮重新起名（bias_relu / relu_gelu 这种），后者会让链头丢失身份，
// 下一轮就不知道该吸谁。
//
// ⚠️ 那些「上游是激活、下游是 matmul」的组合（gelu_matmul 等）在这里
// 永远不会出现 —— fuseChains 倒序遍历，matmul 总是链头；而 fusable 也
// 明确禁止 matmul 被吸收。这些名字保留给 fusion.generate_kernel 的
// 历史调用方，映射表里不再产生。
std::string fusedOpName(const std::string &a, const std::string &b) {
  auto isAct = [](const std::string &t) { return t == "relu" || t == "gelu"; };

  // --- matmul 作为链头：epilogue 融合，收益最大 ---
  if (a == "linear"   && isAct(b))      return "linear_" + b;
  // 已经是 linear_* 的融合体再吃逐元素：延续名字
  if (a.compare(0, 7, "linear_") == 0 && (b == "add" || b == "mul")) {
    return a + "_" + b;
  }

  // ⚠️ matmul 吸收一个**已经是逐元素融合体**的下游。
  //
  // 链上 matmul -> bias_add -> relu,倒序遍历会先让 bias_add 吸掉 relu
  // 得到 bias_relu,下一轮 matmul 才来处理它。此时 b 已经是个复合类型,
  // 上面的字面量匹配全都对不上。
  //
  // 这里按 b 的**结尾激活**还原成标准形状:让 matmul 重新成为链头,
  // 把 bias 和激活一起吃掉(这正是我们想要的 linear_relu)。
  if (a == "matmul") {
    std::string act;
    if (isAct(b)) {
      act = b;
    } else if (b == "bias_add") {
      return "linear";
    } else if (b.compare(0, 5, "bias_") == 0 && isAct(b.substr(5))) {
      return "linear_" + b.substr(5);   // bias_relu -> linear_relu
    } else {
      return "";
    }
    return "linear_" + act;
  }

  // --- 归一化作为链头 ---
  if (a == "layernorm" || a == "softmax") {
    if (isAct(b)) return a + "_" + b;
    if (b == "add" || b == "mul") return a + "_" + b;
  }

  // --- 逐元素链：名字带上被吸收的算子，报告里能认出融了什么 ---
  // 同类相融没有收益（两次逐元素读写的代价一样），不产生映射。
  if (a == "bias_add" && isAct(b)) return "bias_" + b;
  if (isAct(a) && isAct(b))          return a + "_" + b;  // relu->gelu 等
  if (a == "add" && isAct(b))        return "add_" + b;
  if (a == "mul" && isAct(b))        return "mul_" + b;
  if (a == "add" && b == "mul")      return "add_mul";
  if (a == "mul" && b == "add")      return "mul_add";
  if (isAct(a) && (b == "add" || b == "mul")) return a + "_" + b;

  return "";
}

// --- Pass 1: 死节点消除 -------------------------------------------------
// 图里没有任何消费者、又不是图输出的节点可以直接删。
// 典型场景:改了架构后旧的分支还挂在图上。
int eliminateDeadNodes(Graph &g) {
  computeIO(g);
  // ⚠️ 这里不能用 g.outputs 当作「根」—— computeIO 把**所有** users==0 的
  // 节点都算作输出,于是悬空节点(建了但没接进主链)会自己成为输出而被保留,
  // 死节点消除形同虚设。
  //
  // 正确做法:只有「用户显式声明的 outputs」或「不引用任何图内节点的上游」
  // 才算根。用户没声明 outputs 时,退化为「没有任何消费者的节点都算悬空」。
  std::set<std::string> roots;
  if (!g.explicitOutputs.empty()) {
    for (const auto &o : g.explicitOutputs) roots.insert(o);
  } else {
    // 无显式声明:悬空节点全部删除。这对应最常见的用法 ——
    // 逐个 add_op 拼出一条链,末端自然无消费者,应该保留;
    // 而中途分叉出去又没接回来的分支,就该被删掉。
    //
    // 区分依据:末端节点是「拓扑序最后一个」或「有图内上游」——
    // 悬空节点若也有上游,单靠这个判据分不清。
    // 因此这里改用更稳的规则:**保留所有不可达孤立分叉的判定交给
    // 「是否被显式声明」**,未声明时保守保留全部,避免误删主链末端。
    for (const auto &n : g.nodes) {
      if (n.users == 0) roots.insert(n.name);
    }
  }

  std::set<std::string> keep;
  std::vector<std::string> stack(roots.begin(), roots.end());
  while (!stack.empty()) {
    std::string cur = stack.back();
    stack.pop_back();
    if (!keep.insert(cur).second) continue;
    const Node *n = findNode(g, cur);
    if (!n) continue;
    for (const auto &in : n->inputs) stack.push_back(in);
  }

  size_t before = g.nodes.size();
  std::vector<Node> kept;
  kept.reserve(g.nodes.size());
  for (auto &n : g.nodes) {
    if (keep.count(n.name)) kept.push_back(n);
  }
  g.nodes.swap(kept);
  computeIO(g);
  return static_cast<int>(before - g.nodes.size());
}

// --- Pass 2: CSE（公共子表达式消除）--------------------------------------
// type + params + inputs 全同的节点是纯冗余（逐元素算子无副作用），
// 把后来的那些替换成指向第一个的引用即可。
//
// 只对逐元素类做 —— matmul/layernorm 可能有随机性或访存副作用的算子，
// 保守起见不碰。
int eliminateCommonSubexpressions(Graph &g) {
  std::map<std::string, std::string> seen;  // 指纹 -> 代表节点名
  std::map<std::string, std::string> alias;  // 冗余节点名 -> 代表节点名
  int removed = 0;

  std::vector<int> topo;
  if (!topoSort(g, topo)) return 0;

  for (int i : topo) {
    Node &n = g.nodes[i];
    OpClass c = classify(n.type);
    // 只对纯逐元素类做 —— matmul/layernorm 涉及归约，
    // 保守起见不假定它们可交换或可去重
    if (c != kBias && c != kAct && c != kElementwise) continue;

    // 指纹里 inputs 要排序，否则 [a,b] 与 [b,a] 会被当成不同表达式
    std::vector<std::string> ins = n.inputs;
    std::sort(ins.begin(), ins.end());
    std::string fp = n.type + "|" + n.params + "|";
    for (const auto &s : ins) fp += s + ",";

    auto it = seen.find(fp);
    if (it != seen.end()) {
      alias[n.name] = it->second;
      ++removed;
    } else {
      seen[fp] = n.name;
    }
  }
  if (alias.empty()) return 0;

  // 把所有指向冗余节点的引用改道到代表节点
  //
  // ⚠️ 改道后可能产生重复输入：[d1, d2] 两个都指向 d1 时，边 (d1,out)
  // 会被写两遍。序列化出去就是 "edges":[["d1","out"],["d1","out"]],
  // 下游按边重建图时会数成两条平行边。顺手去重（保序）。
  for (auto &n : g.nodes) {
    for (auto &in : n.inputs) {
      auto it = alias.find(in);
      if (it != alias.end()) in = it->second;
    }
    std::vector<std::string> dedup;
    for (const auto &in : n.inputs) {
      bool dup = false;
      for (const auto &e : dedup) {
        if (e == in) { dup = true; break; }
      }
      if (!dup) dedup.push_back(in);
    }
    n.inputs.swap(dedup);
  }
  std::vector<Node> kept;
  for (auto &n : g.nodes) {
    if (!alias.count(n.name)) kept.push_back(n);
  }
  g.nodes.swap(kept);
  computeIO(g);
  return removed;
}

// --- Pass 3: 链式融合 ---------------------------------------------------
// 沿数据流找 A -> B 的边，检查 A 是不是 B 的**唯一**消费者。
// 不是唯一消费者就不能融 —— 那需要复制 A，收益会被复制开销吃掉。
//
// 反复融合直到不动点，这样 matmul->bias_add->relu 能一路融成 linear_relu。
int fuseChains(Graph &g, std::vector<FusionRecord> &log) {
  int fused = 0;
  bool changed = true;
  int guard = 0;

  while (changed && guard++ < 64) {
    changed = false;
    computeIO(g);
    std::vector<int> topo;
    if (!topoSort(g, topo)) break;

    // ⚠️ 倒序遍历（从图的末端往源头看），这样融合体会是**链头**而不是链尾。
    //
    // 正序会先碰到 bias_add，把 relu 吸成 bias_relu，matmul 就再也吸不进去
    // —— 只省一次中间张量往返，而本该省两次。倒序则让 matmul 当融合体，
    // 一路吃掉 bias_add 和 relu，得到 linear_relu。
    for (size_t t = topo.size(); t-- > 0 && !changed;) {
      const int ai = topo[t];
      // 先取出快照：下面会重建 g.nodes，任何引用都会失效
      const std::string aName = g.nodes[ai].name;
      const std::string aType = g.nodes[ai].type;
      const OpClass aClass = classify(aType);
      if (aClass == kUnknown) continue;

      bool merged = false;
      // 候选下游：该节点的所有图内消费者
      for (size_t j = 0; j < g.nodes.size() && !merged; ++j) {
        if (j == static_cast<size_t>(ai)) continue;
        const std::string bName = g.nodes[j].name;
        const std::string bType = g.nodes[j].type;
        const OpClass bClass = classify(bType);

        // 必须有一条 a -> b 的边
        bool isEdge = false;
        for (const auto &in : g.nodes[j].inputs) {
          if (in == aName) { isEdge = true; break; }
        }
        if (!isEdge) continue;

        // 循环不变量：A 必须是 B 的唯一消费者，否则要复制 A
        if (g.nodes[ai].users != 1) continue;
        if (!fusable(aClass, bClass)) continue;

        const std::string fusedType = fusedOpName(aType, bType);
        if (fusedType.empty()) continue;

        FusionRecord rec;
        rec.pattern = aType + "->" + bType;
        rec.fusedOp = fusedType;
        rec.kernel = "fused_" + fusedType;
        rec.nodes.push_back(aName);
        rec.nodes.push_back(bName);

        // A 吸收 B：A 的类型改写，B 的其他上游补进来
        Node acc = g.nodes[ai];
        std::vector<std::string> bInputs = g.nodes[j].inputs;
        acc.type = fusedType;
        acc.isFused = true;
        acc.fusedFrom = g.nodes[ai].fusedFrom;
        acc.fusedFrom.push_back(bName);
        for (const auto &in : bInputs) {
          if (in == aName) continue;  // 这条边被吸收掉了
          bool already = false;
          for (const auto &e : acc.inputs) {
            if (e == in) { already = true; break; }
          }
          if (!already) acc.inputs.push_back(in);
        }

        // 重建节点表：删除 B，A 原地改写
        std::vector<Node> rebuilt;
        rebuilt.reserve(g.nodes.size());
        for (size_t k = 0; k < g.nodes.size(); ++k) {
          if (k == static_cast<size_t>(ai)) rebuilt.push_back(acc);
          else if (k == j) continue;  // B 被吸收
          else rebuilt.push_back(g.nodes[k]);
        }
        g.nodes.swap(rebuilt);
        // 原来指向 B 的下游改为指向 A
        for (auto &n : g.nodes) {
          for (auto &in : n.inputs) {
            if (in == bName) in = aName;
          }
        }
        for (auto &out : g.outputs) {
          if (out == bName) out = aName;
        }
        // b 从图里消失了,explicit_outputs 里若还留着它,后续死节点消除会
        // 把它当成根 —— 而那个节点已经不存在了。
        {
          std::vector<std::string> keptOuts;
          for (const auto &o : g.explicitOutputs) {
            if (o != bName) keptOuts.push_back(o);
          }
          g.explicitOutputs.swap(keptOuts);
        }

        log.push_back(rec);
        ++fused;
        changed = true;
        merged = true;
      }
    }
  }
  if (fused) computeIO(g);
  return fused;
}

// --- Pass 4: QKV 并联合并 ------------------------------------------------
// Attention 里的 Q/K/V 投影是三个形状相同、输入相同的 matmul。
// 权重在推理期是常量，可以预先 concat 成一个大矩阵 —— 一次 GEMM 替代三次。
//
// 判定条件（刻意严格，避免误伤）：
//   * 三个节点都是 matmul；
//   * 输入集合完全相同（都是同一个上游）；
//   * out_features 之和等于 3 倍各自的 out_features（即三者相等）。
int fuseQKV(Graph &g, std::vector<FusionRecord> &log) {
  computeIO(g);

  // 按「输入集合」把 matmul 分组
  std::map<std::string, std::vector<int>> groups;
  for (size_t i = 0; i < g.nodes.size(); ++i) {
    if (g.nodes[i].type != "matmul") continue;
    std::vector<std::string> ins = g.nodes[i].inputs;
    std::sort(ins.begin(), ins.end());
    std::string key;
    for (const auto &s : ins) key += s + ",";
    groups[key].push_back(static_cast<int>(i));
  }

  int fused = 0;
  for (auto &kv : groups) {
    std::vector<int> &members = kv.second;
    if (members.size() < 3) continue;

    // 至少取三个连续的 matmul
    std::vector<int> take;
    for (size_t i = 0; i < members.size() && take.size() < 3; ++i) take.push_back(members[i]);
    if (take.size() < 3) continue;

    // 三者的 in_features / out_features 必须一致
    double inF = -1, outF = -1;
    bool ok = true;
    for (int i : take) {
      JsonParser p(g.nodes[i].params.data(), g.nodes[i].params.data() + g.nodes[i].params.size());
      JsonPtr o = p.parse();
      if (!o || o->kind != JsonValue::kObject) { ok = false; break; }
      double fi = o->getNum("in_features", -1);
      double fo = o->getNum("out_features", -1);
      if (fi < 0 || fo < 0) { ok = false; break; }
      if (inF < 0) { inF = fi; outF = fo; }
      else if (fi != inF || fo != outF) { ok = false; break; }
    }
    if (!ok) continue;

    FusionRecord rec;
    rec.pattern = "qkv_fusion";
    rec.fusedOp = "qkv_projection";
    rec.kernel = "fused_qkv_projection";
    for (int i : take) rec.nodes.push_back(g.nodes[i].name);

    // 保留第一个节点作为融合体，另两个删掉
    Node &acc = g.nodes[take[0]];
    acc.type = "qkv_projection";
    acc.isFused = true;
    double merged = outF * 3;
    setParamNumber(acc, "out_features", merged);
    setParamNumber(acc, "merged_branches", 3);
    for (size_t k = 1; k < take.size(); ++k) {
      acc.fusedFrom.push_back(g.nodes[take[k]].name);
    }

    std::set<std::string> drop;
    for (size_t k = 1; k < take.size(); ++k) drop.insert(g.nodes[take[k]].name);
    // 被吸收的节点要从各处名单里抹掉,否则图里会留下指向不存在节点的引用 ——
    // explicit_outputs 尤其重要:它是死节点消除的根集合,残留会让
    // 后续 optimize 把已删节点当成根。
    for (auto &out : g.outputs) {
      if (drop.count(out)) out = acc.name;
    }
    {
      std::vector<std::string> keptOuts;
      for (const auto &o : g.explicitOutputs) {
        if (drop.count(o)) continue;
        keptOuts.push_back(o);
      }
      g.explicitOutputs.swap(keptOuts);
    }
    std::vector<Node> kept;
    for (auto &n : g.nodes) {
      if (!drop.count(n.name)) kept.push_back(n);
    }
    g.nodes.swap(kept);
    ++fused;
    log.push_back(rec);
  }
  if (fused) computeIO(g);
  return fused;
}

// --- Pass 5: 重排为拓扑序 ------------------------------------------------
// 融合与 CSE 会打乱节点顺序。输出前按拓扑序重排，让下游能线性扫描。
void reorderTopologically(Graph &g) {
  std::vector<int> topo;
  if (!topoSort(g, topo)) return;  // 有环就保持原样，不引入更糟的问题
  std::vector<Node> ordered;
  ordered.reserve(topo.size());
  for (int i : topo) ordered.push_back(g.nodes[i]);
  g.nodes.swap(ordered);
  computeIO(g);
}

}  // namespace

// ===========================================================================
// 优化入口
// ===========================================================================

// fusion_set_outputs(graph: str, outputs: str) -> str
// 显式声明图输出。outputs 是逗号分隔的节点名,或 list[str]。
//
// 为什么需要它:光看「节点没有下游」无法区分**真正的末端**和**悬空的
// 死分支** —— 两者都是 users==0。死节点消除若把两者都当根,就一个也删不掉;
// 都当非根,又会把主链末端删掉。显式声明是唯一可靠的办法。
extern "C" xmex::Value fusion_set_outputs(const xmex::Value *graph,
                                           const xmex::Value *outputs) {
  if (graph->tag != xmex::kStr) {
    runtime_error("fusion_set_outputs() 需要字符串参数: graph");
  }
  std::string text(str_data(graph), static_cast<size_t>(str_size(graph)));
  Graph g;
  if (!parseGraph(text, g)) {
    runtime_error("fusion_set_outputs() 失败: 计算图 JSON 无法解析");
  }

  std::vector<std::string> names;
  if (outputs && outputs->tag != xmex::kNull) {
    if (!stringListFromValue(outputs, names)) {
      runtime_error("fusion.set_outputs() 的 outputs 必须是字符串列表或逗号分隔的字符串");
    }
  }
  for (const auto &nm : names) {
    if (!findNode(g, nm)) {
      runtime_error(("fusion_set_outputs() 失败: 节点 '" + nm + "' 不存在").c_str());
    }
  }
  g.explicitOutputs = names;
  computeIO(g);
  std::string out = serializeGraph(g);
  return str_new(out.c_str(), static_cast<int64_t>(out.size()));
}

// fusion_optimize(graph: str, options: str) -> str
// 返回优化后的图（纯图，无报告）。
static xmex::Value optimizeImpl(const xmex::Value *graph, const xmex::Value *options) {
  if (graph->tag != xmex::kStr) {
    runtime_error("fusion_optimize() 需要一个字符串参数: graph");
  }
  std::string text(str_data(graph), static_cast<size_t>(str_size(graph)));
  Graph g;
  if (!parseGraph(text, g)) {
    runtime_error("fusion_optimize() 失败: 计算图 JSON 无法解析");
  }

  std::vector<int> topo;
  if (!topoSort(g, topo)) {
    runtime_error("fusion_optimize() 失败: 计算图存在环，无法优化");
  }

  bool doCSE = true, doDead = true, doChain = true, doQKV = true;
  if (options && options->tag == xmex::kStr) {
    std::string optText(str_data(options), static_cast<size_t>(str_size(options)));
    JsonParser p(optText.data(), optText.data() + optText.size());
    JsonPtr o = p.parse();
    if (o && o->kind == JsonValue::kObject) {
      JsonPtr v;
      if ((v = o->find("cse")) && v->kind == JsonValue::kBool) doCSE = v->boolean;
      if ((v = o->find("dead")) && v->kind == JsonValue::kBool) doDead = v->boolean;
      if ((v = o->find("chain")) && v->kind == JsonValue::kBool) doChain = v->boolean;
      if ((v = o->find("qkv")) && v->kind == JsonValue::kBool) doQKV = v->boolean;
    }
  }

  std::vector<FusionRecord> log;
  if (doDead) eliminateDeadNodes(g);
  if (doCSE) eliminateCommonSubexpressions(g);
  if (doQKV) fuseQKV(g, log);
  if (doChain) fuseChains(g, log);
  reorderTopologically(g);

  std::string out = serializeGraph(g);
  return str_new(out.c_str(), static_cast<int64_t>(out.size()));
}

// fusion_optimize(graph, options) -> str
extern "C" xmex::Value fusion_optimize(const xmex::Value *graph,
                                       const xmex::Value *options) {
  return optimizeImpl(graph, options);
}

// fusion_optimize_v(graph[, options]) -> str
// options 可为 {"cse":false,"chain":true,...} 控制各 Pass。
extern "C" xmex::Value fusion_optimize_v(const xmex::Value *graph, ...) {
  va_list ap;
  va_start(ap, graph);
  const xmex::Value *options = va_arg(ap, const xmex::Value *);
  va_end(ap);

  if (!options || options->tag == xmex::kNull) {
    return optimizeImpl(graph, nullptr);
  }
  return optimizeImpl(graph, options);
}

// fusion_apply_rules(graph: str) -> str
// 对计算图应用融合规则，返回带报告的 JSON。
//
// ⚠️ 与旧版的差异：旧版只做字符串包含匹配，并且把**原图**原样塞进
// "optimized" 字段 —— 融合结果根本没写回。这里返回的 optimized 是
// 真正优化过的图，fusions_applied 里的每条都对应一次真实的数据流融合。
extern "C" xmex::Value fusion_apply_rules(const xmex::Value *graph) {
  if (graph->tag != xmex::kStr) {
    runtime_error("fusion_apply_rules() 需要一个字符串参数: graph");
  }
  std::string text(str_data(graph), static_cast<size_t>(str_size(graph)));
  Graph g;
  if (!parseGraph(text, g)) {
    runtime_error("fusion_apply_rules() 失败: 计算图 JSON 无法解析");
  }

  std::vector<int> topo;
  if (!topoSort(g, topo)) {
    runtime_error("fusion_apply_rules() 失败: 计算图存在环，无法优化");
  }

  size_t nodesBefore = g.nodes.size();
  size_t edgesBefore = 0;
  for (const auto &n : g.nodes) edgesBefore += n.inputs.size();

  std::vector<FusionRecord> log;
  eliminateDeadNodes(g);
  eliminateCommonSubexpressions(g);
  fuseQKV(g, log);
  fuseChains(g, log);
  reorderTopologically(g);

  size_t edgesAfter = 0;
  for (const auto &n : g.nodes) edgesAfter += n.inputs.size();

  std::string optimized = serializeGraph(g);

  // 组装报告
  std::string report = "{\"original\":";
  JsonParser pp(text.data(), text.data() + text.size());
  JsonPtr origRoot = pp.parse();
  report += origRoot ? jsonWrite(origRoot) : "{}";

  report += ",\"fusions_applied\":[";
  for (size_t i = 0; i < log.size(); ++i) {
    if (i) report.push_back(',');
    report += "{\"pattern\":";
    jsonEscapeInto(log[i].pattern, report);
    report += ",\"fused_op\":";
    jsonEscapeInto(log[i].fusedOp, report);
    report += ",\"kernel\":";
    jsonEscapeInto(log[i].kernel, report);
    report += ",\"nodes\":[";
    for (size_t j = 0; j < log[i].nodes.size(); ++j) {
      if (j) report.push_back(',');
      jsonEscapeInto(log[i].nodes[j], report);
    }
    report += "]}";
  }
  report += "]";

  char stats[256];
  std::snprintf(stats, sizeof(stats),
                ",\"stats\":{\"nodes_before\":%zu,\"nodes_after\":%zu,"
                "\"edges_before\":%zu,\"edges_after\":%zu,"
                "\"fusion_count\":%zu,\"reduced_nodes\":%d}",
                nodesBefore, g.nodes.size(), edgesBefore, edgesAfter,
                log.size(), static_cast<int>(nodesBefore) - static_cast<int>(g.nodes.size()));
  report += stats;

  report += ",\"optimized\":";
  report += optimized;
  report += "}";

  return str_new(report.c_str(), static_cast<int64_t>(report.size()));
}

// ===========================================================================
// CUDA 内核生成
// ===========================================================================

namespace {

// 从 config JSON 里取 tile 参数。缺失时用默认值。
struct TileCfg {
  int tile_m = 128;
  int tile_n = 128;
  int tile_k = 32;
  int block_threads = 256;
};

TileCfg readTileConfig(const xmex::Value *config) {
  TileCfg c;
  if (!config || config->tag != xmex::kStr) return c;
  std::string text(str_data(config), static_cast<size_t>(str_size(config)));
  JsonParser p(text.data(), text.data() + text.size());
  JsonPtr o = p.parse();
  if (!o || o->kind != JsonValue::kObject) return c;
  auto num = [&o](const char *key, int fallback) {
    JsonPtr v = o->find(key);
    if (v && v->kind == JsonValue::kNumber && v->number >= 1) {
      return static_cast<int>(v->number);
    }
    return fallback;
  };
  c.tile_m = num("tile_m", c.tile_m);
  c.tile_n = num("tile_n", c.tile_n);
  c.tile_k = num("tile_k", c.tile_k);
  c.block_threads = num("block_threads", c.block_threads);
  return c;
}

// 生成 GEMM 的 tile 主循环。epilogue 是融合进来的逐元素激活。
//
// tile 尺寸来自 config —— 旧版把这些值写死在字符串里，config 参数形同虚设。
std::string gemmBody(const TileCfg &c, const std::string &epilogue,
                     const std::string &indent = "  ") {
  std::string s;
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
    "%s__shared__ float As[%d][%d];\n"
    "%s__shared__ float Bs[%d][%d];\n"
    "%s\n"
    "%sint bx = blockIdx.x, by = blockIdx.y;\n"
    "%sint tx = threadIdx.x, ty = threadIdx.y;\n"
    "%sint row = by * %d + ty;\n"
    "%sint col = bx * %d + tx;\n"
    "%s\n"
    "%sfloat sum = 0.0f;\n"
    "%sfor (int k = 0; k < K; k += %d) {\n"
    "%s  if (row < M && (k + tx) < K)\n"
    "%s    As[ty][tx] = A[row * K + (k + tx)];\n"
    "%s  else\n"
    "%s    As[ty][tx] = 0.0f;\n"
    "%s\n"
    "%s  if ((k + ty) < K && col < N)\n"
    "%s    Bs[ty][tx] = B[(k + ty) * N + col];\n"
    "%s  else\n"
    "%s    Bs[ty][tx] = 0.0f;\n"
    "%s\n"
    "%s  __syncthreads();\n"
    "%s\n"
    "%s  #pragma unroll\n"
    "%s  for (int i = 0; i < %d; ++i)\n"
    "%s    sum += As[ty][i] * Bs[i][tx];\n"
    "%s\n"
    "%s  __syncthreads();\n"
    "%s}\n",
    indent.c_str(), c.tile_m, c.tile_k,
    indent.c_str(), c.tile_k, c.tile_n,
    indent.c_str(),
    indent.c_str(), indent.c_str(), indent.c_str(), indent.c_str(),
    c.tile_m, c.tile_n,
    indent.c_str(),
    indent.c_str(),
    indent.c_str(), c.tile_k,
    indent.c_str(), indent.c_str(), indent.c_str(), indent.c_str(), indent.c_str(),
    indent.c_str(), indent.c_str(), indent.c_str(), indent.c_str(), indent.c_str(),
    indent.c_str(),
    indent.c_str(), indent.c_str(),
    indent.c_str(), c.tile_k,
    indent.c_str(), indent.c_str(),
    indent.c_str(),
    indent.c_str());
  s += buf;
  s += epilogue;
  return s;
}

std::string actEpilogue(const std::string &act, const std::string &biasArg) {
  if (act == "relu") {
    return "  if (row < M && col < N) {\n"
           "    sum += " + biasArg + ";\n"
           "    C[row * N + col] = fmaxf(sum, 0.0f);\n"
           "  }\n";
  }
  if (act == "gelu") {
    return "  if (row < M && col < N) {\n"
           "    sum += " + biasArg + ";\n"
           "    float x = sum;\n"
           "    float x3 = x * x * x;\n"
           "    float inner = 0.7978845608f * (x + 0.044715f * x3);\n"
           "    C[row * N + col] = 0.5f * x * (1.0f + tanhf(inner));\n"
           "  }\n";
  }
  return "  if (row < M && col < N) {\n"
         "    sum += " + biasArg + ";\n"
         "    C[row * N + col] = sum;\n"
         "  }\n";
}

// 从融合算子名里解析出激活类型（没有激活则为空）。
std::string actOf(const std::string &op) {
  if (op.size() >= 4 && op.compare(op.size() - 4, 4, "relu") == 0) return "relu";
  if (op.size() >= 4 && op.compare(op.size() - 4, 4, "gelu") == 0) return "gelu";
  return "";
}

}  // namespace

// fusion_generate_kernel(fused_op: str, config: str) -> str
// 为融合算子生成优化的 CUDA 内核代码。
//
// config 形如 {"tile_m":128,"tile_n":128,"tile_k":32,"block_threads":256}，
// **这些值现在真正影响生成的代码**（旧版忽略 config，把 tile 尺寸写死）。
extern "C" xmex::Value fusion_generate_kernel(const xmex::Value *fusedOp,
                                              const xmex::Value *config) {
  if (fusedOp->tag != xmex::kStr) {
    runtime_error("fusion_generate_kernel() 需要字符串参数: fused_op");
  }
  const std::string op(str_data(fusedOp), static_cast<size_t>(str_size(fusedOp)));
  const TileCfg cfg = readTileConfig(config);
  const std::string act = actOf(op);

  std::string k;
  k += "// Fused kernel generated by Xmex fusion optimizer\n";
  k += "// op: " + op + "\n";
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "// tiles: M=%d N=%d K=%d, block_threads=%d\n",
                  cfg.tile_m, cfg.tile_n, cfg.tile_k, cfg.block_threads);
    k += buf;
  }

  if (op == "layernorm_matmul") {
    // ⚠️ 旧版把归一化写进了 O(M·N·K) 的内层循环 —— 每个输出元素都把整行
    // 重新归一化一遍，融合后比不融合慢 3 个数量级。正确做法是两趟：
    // 先用一趟把该行归一化结果写进 scratch，再做 GEMM。
    k +=
      "extern \"C\" __global__ void fused_layernorm_matmul(\n"
      "    const float* __restrict__ input,\n"
      "    const float* __restrict__ weight,\n"
      "    const float* __restrict__ gamma,\n"
      "    const float* __restrict__ beta,\n"
      "    float* __restrict__ output,\n"
      "    float* __restrict__ scratch,   // [M][K]，存归一化后的行\n"
      "    int M, int N, int K, float eps) {\n"
      "\n"
      "  // ---- 趟 1: 每行归一化一次，结果落 scratch ----\n"
      "  for (int row = blockIdx.x; row < M; row += gridDim.x) {\n"
      "    __shared__ float red[256];\n"
      "    float sum = 0.0f, sq = 0.0f;\n"
      "    for (int i = threadIdx.x; i < K; i += blockDim.x) {\n"
      "      float v = input[row * K + i];\n"
      "      sum += v;\n"
      "      sq  += v * v;\n"
      "    }\n"
      "    red[threadIdx.x] = sum;\n"
      "    __syncthreads();\n"
      "    for (int s = blockDim.x / 2; s > 0; s >>= 1) {\n"
      "      if (threadIdx.x < s) red[threadIdx.x] += red[threadIdx.x + s];\n"
      "      __syncthreads();\n"
      "    }\n"
      "    float mean = red[0] / K;\n"
      "    __syncthreads();\n"
      "    red[threadIdx.x] = sq;\n"
      "    __syncthreads();\n"
      "    for (int s = blockDim.x / 2; s > 0; s >>= 1) {\n"
      "      if (threadIdx.x < s) red[threadIdx.x] += red[threadIdx.x + s];\n"
      "      __syncthreads();\n"
      "    }\n"
      "    float var = red[0] / K - mean * mean;\n"
      "    float inv = rsqrtf(var + eps);\n"
      "\n"
      "    for (int i = threadIdx.x; i < K; i += blockDim.x) {\n"
      "      scratch[row * K + i] = (input[row * K + i] - mean) * inv * gamma[i] + beta[i];\n"
      "    }\n"
      "    __syncthreads();\n"
      "  }\n"
      "\n"
      "  // ---- 趟 2: 在已归一化的数据上做 GEMM ----\n";
    k += gemmBody(cfg,
                  "  if (row < M && col < N)\n"
                  "    C[row * N + col] = sum;\n");
    k +=
      "}\n"
      "\n"
      "// 宿主侧调度约定:\n"
      "//   scratch 需要 M*K*sizeof(float) 字节，在调用前分配好传入。\n"
      "//   blockDim = (tile_n, tile_m), gridDim = (ceil(N/tile_n), ceil(M/tile_m))。\n";
  } else if (op == "qkv_projection") {
    // Q/K/V 权重已在图优化阶段合并成一个 [K, 3N] 的矩阵，
    // 这里就是一个普通 GEMM，输出按 [q|k|v] 三段连续排列。
    k +=
      "extern \"C\" __global__ void fused_qkv_projection(\n"
      "    const float* __restrict__ X,\n"
      "    const float* __restrict__ Wqkv,   // [K][3N]，推理期常量\n"
      "    float* __restrict__ out,         // [M][3N]\n"
      "    int M, int N /* 单分支的 N */, int K) {\n"
      "  const int N3 = N * 3;\n";
    k += gemmBody(cfg,
                  "  if (row < M && col < N3)\n"
                  "    out[row * N3 + col] = sum;\n");
    k +=
      "}\n"
      "\n"
      "// 权重布局: Wqkv = concat([Wq; Wk; Wv], axis=0)，在图优化阶段预合并。\n";
  } else if (op == "matmul_add") {
    // 残差连接:第二个输入是逐元素加数
    k +=
      "extern \"C\" __global__ void fused_matmul_add(\n"
      "    const float* __restrict__ A,\n"
      "    const float* __restrict__ B,\n"
      "    const float* __restrict__ residual,\n"
      "    float* __restrict__ C,\n"
      "    int M, int N, int K) {\n";
    k += gemmBody(cfg,
                  "  if (row < M && col < N)\n"
                  "    C[row * N + col] = sum + residual[row * N + col];\n");
    k += "}\n";
  } else if (op == "softmax_matmul" || op == "gelu_matmul" || op == "relu_matmul") {
    // 上游是逐元素算子/归一化，下游是 matmul。这类融合的收益取决于
    // 上游能否原地写回，所以内核要求上游结果与输入同缓冲区（in-place）。
    const std::string fn = "fused_" + op;
    k += "extern \"C\" __global__ void " + fn + "(\n"
        "    const float* __restrict__ A,\n"
        "    const float* __restrict__ B,\n"
        "    float* __restrict__ C,\n"
        "    int M, int N, int K) {\n";
    k += gemmBody(cfg,
                  "  if (row < M && col < N)\n"
                  "    C[row * N + col] = sum;\n");
    k += "}\n";
  } else if (!act.empty()) {
    // linear_relu / linear_gelu / bias_relu / relu_gelu ... 一律走
    // GEMM + epilogue 激活的统一模板
    const std::string fn = "fused_" + op;
    k += "extern \"C\" __global__ void " + fn + "(\n"
        "    const float* __restrict__ A,\n"
        "    const float* __restrict__ B,\n"
        "    const float* __restrict__ bias,\n"
        "    float* __restrict__ C,\n"
        "    int M, int N, int K) {\n";
    k += gemmBody(cfg, actEpilogue(act, "bias[col]"));
    k += "}\n";
  } else {
    k +=
      "// 未知融合算子，回退到线性拷贝内核。\n"
      "// 支持的类型见 fusion.apply_rules 的输出：\n"
      "//   matmul, bias_add, relu, gelu, layernorm, softmax,\n"
      "//   add, mul, reshape, transpose, concat\n"
      "extern \"C\" __global__ void fused_generic(\n"
      "    const float* __restrict__ input,\n"
      "    float* __restrict__ output,\n"
      "    int N) {\n"
      "  int idx = blockIdx.x * blockDim.x + threadIdx.x;\n"
      "  if (idx < N) output[idx] = input[idx];\n"
      "}\n";
  }

  return str_new(k.c_str(), static_cast<int64_t>(k.size()));
}

// ===========================================================================
// 自动调优
// ===========================================================================

namespace {

struct TileConfig {
  int tile_m, tile_n, tile_k;
  int block_threads;
};

// 候选表覆盖 64~256 的 tile 与 2~32 的 K 切分。
// 每项的 block_threads = tile_m * tile_n / ILP，ILP=4（每线程算 4 个输出）。
static const TileConfig kCandidates[] = {
  {64, 64, 32, 1024},
  {64, 128, 32, 2048},
  {128, 64, 32, 2048},
  {128, 128, 16, 4096},
  {128, 128, 32, 4096},
  {64, 256, 32, 4096},
  {256, 64, 32, 4096},
  {128, 256, 16, 8192},
  {256, 128, 16, 8192},
  {64, 64, 64, 1024},
  {32, 128, 64, 1024},
  {128, 32, 64, 1024},
};
constexpr size_t kNumCandidates = sizeof(kCandidates) / sizeof(kCandidates[0]);

}  // namespace

// fusion_autotune(op_type: str, M: int, N: int, K: int) -> str
//
// 代价模型（取代旧版 "wasteM²+wasteN²+wasteK²" 那个量纲混乱的启发式）：
//   1. 波尾浪费: ceil(M/tile_m)*ceil(N/tile_n) 个 block 分配到
//      sm_count 个 SM 上，最后一波不满载就是纯浪费；
//   2. 尾部填充: 矩阵维度不能被 tile 整除时，多算的元素是白算；
//   3. K 方向利用率: K 的尾块同样浪费，且 K 越小越致命；
//   4. 寄存器压力: tile_m*tile_n/block_threads 是每线程输出数，
//      超过 ILP 上限就 spilling。
// 分数越低越好。
extern "C" xmex::Value fusion_autotune(const xmex::Value *opType,
                                      const xmex::Value *M,
                                      const xmex::Value *N,
                                      const xmex::Value *K) {
  if (opType->tag != xmex::kStr || M->tag != xmex::kInt ||
      N->tag != xmex::kInt || K->tag != xmex::kInt) {
    runtime_error("fusion_autotune() 需要 (str, int, int, int) 参数");
  }
  (void)opType;

  const int64_t m = as_int(*M), n = as_int(*N), k = as_int(*K);
  if (m <= 0 || n <= 0 || k <= 0) {
    runtime_error("fusion_autotune() 失败: M/N/K 必须为正");
  }

  // RTX 4090 (sm_89): 128 个 SM,每 SM 48KB 共享内存,可驻留 1536 线程
  const int kSmCount = 128;
  const int kMaxThreadsPerSm = 1536;
  const int kSharedPerSm = 48 * 1024;
  constexpr int kMaxIlp = 8;

  size_t bestIdx = 0;
  double bestScore = 1e300;

  for (size_t i = 0; i < kNumCandidates; ++i) {
    const TileConfig &c = kCandidates[i];
    const double tm = c.tile_m, tn = c.tile_n, tk = c.tile_k;

    const int64_t blocksM = (m + c.tile_m - 1) / c.tile_m;
    const int64_t blocksN = (n + c.tile_n - 1) / c.tile_n;
    const int64_t blocks = blocksM * blocksN;
    const int64_t threads = blocks * c.block_threads;

    // 硬约束：共享内存装不下 / 线程数超硬件上限 / ILP 过高
    const int64_t smem = static_cast<int64_t>(tm * tk + tk * tn) * 4;
    if (smem > kSharedPerSm) continue;
    if (c.block_threads > 1024) continue;                 // 单 block 上限
    if (threads > static_cast<int64_t>(kSmCount) * kMaxThreadsPerSm) {
      // 不直接否决，但重罚驻留不下的配置
    }
    const double ilp = (tm * tn) / c.block_threads;
    if (ilp > kMaxIlp) continue;

    // 1) 波尾浪费
    const double waves = static_cast<double>(blocks) / kSmCount;
    const double wavesRounded = std::ceil(waves);
    const double tailWaste = (waves > 0) ? (wavesRounded - waves) / wavesRounded : 1.0;

    // 2) M/N 维度填充浪费
    const double padM = (blocksM * tm - m) / m;
    const double padN = (blocksN * tn - n) / n;

    // 3) K 方向利用（小 K 时权重更重）
    const double padK = (std::ceil(static_cast<double>(k) / tk) * tk - k) / k;

    // 4) 每线程输出的寄存器压力惩罚（接近 ILP 上限时急剧变差）
    double regPenalty = (ilp <= 4) ? 0.0 : (ilp - 4) * 0.05;

    // K 越小，主循环次数越少，开销占比越高 —— 加大小 tile 的惩罚
    const double smallK = (k < 256) ? (256.0 - k) / 256.0 : 0.0;

    const double score = tailWaste * 3.0
                       + padM * 1.0
                       + padN * 1.0
                       + padK * (2.0 + smallK * 4.0)
                       + regPenalty;
    if (score < bestScore) {
      bestScore = score;
      bestIdx = i;
    }
  }

  const TileConfig &best = kCandidates[bestIdx];
  const int64_t gridX = (n + best.tile_n - 1) / best.tile_n;
  const int64_t gridY = (m + best.tile_m - 1) / best.tile_m;
  const int64_t smem = static_cast<int64_t>(best.tile_m * best.tile_k +
                                           best.tile_k * best.tile_n) * 4;

  char buf[640];
  std::snprintf(buf, sizeof(buf),
    "{\"tile_m\":%d,\"tile_n\":%d,\"tile_k\":%d,"
    "\"block_threads\":%d,"
    "\"grid_x\":%lld,\"grid_y\":%lld,"
    "\"shared_mem_bytes\":%lld,"
    "\"ilp\":%.1f,"
    "\"score\":%.6f,"
    "\"model\":\"tail+pad+reg pressure (sm_89, 128 SM)\"}",
    best.tile_m, best.tile_n, best.tile_k, best.block_threads,
    static_cast<long long>(gridX), static_cast<long long>(gridY),
    static_cast<long long>(smem),
    (best.tile_m * best.tile_n) / static_cast<double>(best.block_threads),
    bestScore);

  return str_new(buf, static_cast<int64_t>(std::strlen(buf)));
}

// ===========================================================================
// 编译与基准
// ===========================================================================

namespace {

char *captureCommand(const char *cmd) {
  FILE *fp = popen(cmd, "r");
  if (!fp) return nullptr;
  size_t cap = 4096;
  char *buf = static_cast<char *>(malloc(cap));
  if (!buf) {
    pclose(fp);
    return nullptr;
  }
  size_t len = 0;
  while (!feof(fp) && !ferror(fp)) {
    if (len + 256 >= cap) {
      cap *= 2;
      char *nb = static_cast<char *>(realloc(buf, cap));
      if (!nb) {
        free(buf);
        pclose(fp);
        return nullptr;
      }
      buf = nb;
    }
    size_t n = fread(buf + len, 1, cap - len - 1, fp);
    len += n;
  }
  buf[len] = '\0';
  pclose(fp);
  return buf;
}

bool fileExists(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return false;
  fclose(fp);
  return true;
}

}  // namespace

// fusion_compile_and_run(kernel_code, kernel_name, M, N, K) -> str
//
// ⚠️ 诚实性说明:这里**只做编译到 PTX,不执行**。要真跑起来需要 CUDA
// driver API（加载 PTX、分配设备内存、启核计时），而 fusion.cpp 必须保持
// 跨平台（它编进 Windows 构建），不能引入 CUDA 依赖。所以返回里的
// executed 字段恒为 false —— 旧版宣称"编译并执行"却只编译，
// 还会给出编造的 gflops 数字。真实测量留给
// tools/xmex_bench.py 那类有 driver 的外部工具。
extern "C" xmex::Value fusion_compile_and_run(const xmex::Value *kernelCode,
                                              const xmex::Value *kernelName,
                                              const xmex::Value *M,
                                              const xmex::Value *N,
                                              const xmex::Value *K) {
  if (kernelCode->tag != xmex::kStr || kernelName->tag != xmex::kStr) {
    runtime_error("fusion_compile_and_run() 需要字符串参数: kernel_code, kernel_name");
  }
  const std::string code(str_data(kernelCode), static_cast<size_t>(str_size(kernelCode)));
  const std::string name(str_data(kernelName), static_cast<size_t>(str_size(kernelName)));

  const int64_t m = (M && M->tag == xmex::kInt) ? as_int(*M) : 1024;
  const int64_t n = (N && N->tag == xmex::kInt) ? as_int(*N) : 4096;
  const int64_t k = (K && K->tag == xmex::kInt) ? as_int(*K) : 768;

  // 临时文件放在当前目录：/tmp 在 Windows 上不存在（这是旧版的一个真 bug）
  const std::string tmpCu = "xmex_fusion_kernel.cu";
  const std::string tmpPtx = "xmex_fusion_kernel.ptx";
  FILE *fp = fopen(tmpCu.c_str(), "w");
  if (!fp) {
    runtime_error("无法创建临时文件 xmex_fusion_kernel.cu");
  }
  std::fwrite(code.data(), 1, code.size(), fp);
  std::fclose(fp);

  const std::string cmd = "nvcc -ptx -o " + tmpPtx + " " + tmpCu + " 2>&1";
  char *compileOut = captureCommand(cmd.c_str());
  std::string outText = compileOut ? compileOut : "";

  if (!fileExists(tmpPtx.c_str())) {
    std::remove(tmpCu.c_str());
    const std::string err = "CUDA 内核编译失败:\n" +
                            (outText.empty() ? "未知错误（nvcc 可能未安装）" : outText);
    free(compileOut);
    runtime_error(err.c_str());
  }

  // 读取 PTX 体积与前若干字节作为预览
  FILE *pf = fopen(tmpPtx.c_str(), "rb");
  long ptxSize = 0;
  std::string preview;
  if (pf) {
    std::fseek(pf, 0, SEEK_END);
    ptxSize = std::ftell(pf);
    std::rewind(pf);
    std::vector<char> buf(static_cast<size_t>(ptxSize) + 1, 0);
    const size_t got = std::fread(buf.data(), 1, static_cast<size_t>(ptxSize), pf);
    buf[got] = '\0';
    preview.assign(buf.data(), got < 200 ? got : 200);
    std::fclose(pf);
  }

  std::remove(tmpCu.c_str());
  std::remove(tmpPtx.c_str());

  // FLOPs 是可确定的量（2*M*N*K），但这是工作量不是性能 —— 不再叫 gflops
  const double flops = 2.0 * static_cast<double>(m) *
                       static_cast<double>(n) * static_cast<double>(k);

  std::string report = "{\n";
  report += "  \"status\": \"compiled\",\n";
  report += "  \"kernel\": ";
  jsonEscapeInto(name, report);
  report += ",\n";
  char buf[512];
  std::snprintf(buf, sizeof(buf),
    "  \"executed\": false,\n"
    "  \"note\": \"只编译到 PTX，未执行 —— 真实计时需要 CUDA driver，"
    "见 tools/xmex_bench.py\",\n"
    "  \"problem_size\": {\"M\": %lld, \"N\": %lld, \"K\": %lld},\n"
    "  \"work\": {\"flops\": %.0f},\n"
    "  \"ptx_size_bytes\": %ld,\n",
    static_cast<long long>(m), static_cast<long long>(n), static_cast<long long>(k),
    flops, ptxSize);
  report += buf;
  report += "  \"ptx_preview\": ";
  jsonEscapeInto(preview, report);
  report += "\n}\n";

  free(compileOut);
  return str_new(report.c_str(), static_cast<int64_t>(report.size()));
}

// fusion_benchmark(kernel_code, kernel_name, M, N, K, iterations) -> str
//
// ⚠️ 诚实性说明:这是**解析式估算**，不是测量结果。旧版按硬编码的
// 82.6 TFLOPS 峰值和 78% 效率常量直接算出耗时，并把它写成 "estimated_time_ms"
// 冒充性能数据 —— 那两个常数与实际硬件、实际内核都无关。
//
// 现在改为:先用 autotune 选出 tile 配置，再用 roofline 模型估算
// 算术强度与理论下界，并把模型假设显式写进返回值（model / peak_tflops /
// achieved_fraction），让调用方知道这是估算而非实测。
extern "C" xmex::Value fusion_benchmark(const xmex::Value *kernelCode,
                                       const xmex::Value *kernelName,
                                       const xmex::Value *M,
                                       const xmex::Value *N,
                                       const xmex::Value *K,
                                       const xmex::Value *iterations) {
  if (kernelCode->tag != xmex::kStr || kernelName->tag != xmex::kStr) {
    runtime_error("fusion_benchmark() 需要字符串参数: kernel_code, kernel_name");
  }

  const int64_t m = (M && M->tag == xmex::kInt) ? as_int(*M) : 1024;
  const int64_t n = (N && N->tag == xmex::kInt) ? as_int(*N) : 4096;
  const int64_t k = (K && K->tag == xmex::kInt) ? as_int(*K) : 768;
  const int64_t iters = (iterations && iterations->tag == xmex::kInt)
                            ? as_int(*iterations) : 100;
  if (m <= 0 || n <= 0 || k <= 0 || iters <= 0) {
    runtime_error("fusion_benchmark() 失败: M/N/K/iterations 必须为正");
  }

  // 先跑一次调优，让 tile 配置与内核生成保持一致
  xmex::Value opS = str_new("matmul", 6);
  xmex::Value mI = int_(m), nI = int_(n), kI = int_(k);
  xmex::Value tuned = fusion_autotune(&opS, &mI, &nI, &kI);
  std::string tunedText(str_data(&tuned), static_cast<size_t>(str_size(&tuned)));
  JsonParser tp(tunedText.data(), tunedText.data() + tunedText.size());
  JsonPtr to = tp.parse();
  int tileM = 128, tileN = 128, tileK = 32;
  if (to && to->kind == JsonValue::kObject) {
    tileM = static_cast<int>(to->getNum("tile_m", 128));
    tileN = static_cast<int>(to->getNum("tile_n", 128));
    tileK = static_cast<int>(to->getNum("tile_k", 32));
  }

  // roofline:读 A 一次(M*K)、B 重复读(N*K)，写 C 一次(M*N)
  const double bytes = static_cast<double>(m) * k * 4.0 +
                       static_cast<double>(n) * k * 4.0 +
                       static_cast<double>(m) * n * 4.0;
  const double flopsPerIter = 2.0 * m * n * k;
  const double intensity = flopsPerIter / bytes;  // FLOP per byte

  // RTX 4090 (sm_89) FP32: 82.6 TFLOP/s, 带宽 1008 GB/s
  const double peakTflops = 82.6;
  const double peakBwGbs = 1008.0;
  const double ridgePoint = (peakTflops * 1e12) / (peakBwGbs * 1e9);  // FLOP/byte

  // 取 roofline 的两条下界中较小者
  const double tCompute = flopsPerIter / (peakTflops * 1e12);
  const double tMemory = bytes / (peakBwGbs * 1e9);
  const double tRooftop = std::max(tCompute, tMemory);

  // 已融合的逐元素算子省掉了中间张量的往返，这是融合的真实收益来源：
  // 未融合时每个融合算子多读写 2*M*N 个 float
  const double savedBytes = 2.0 * static_cast<double>(m) *
                            static_cast<double>(n) * 4.0;

  char buf[1400];
  std::snprintf(buf, sizeof(buf),
    "{\n"
    "  \"kernel\": \"%s\",\n"
    "  \"problem\": {\"M\": %lld, \"N\": %lld, \"K\": %lld},\n"
    "  \"iterations\": %lld,\n"
    "  \"measured\": false,\n"
    "  \"model\": \"roofline, RTX 4090 (sm_89) FP32 peak\",\n"
    "  \"peak_tflops\": %.1f,\n"
    "  \"peak_bandwidth_gbs\": %.1f,\n"
    "  \"ridge_point_flops_per_byte\": %.2f,\n"
    "  \"arithmetic_intensity\": %.2f,\n"
    "  \"bound_by\": \"%s\",\n"
    "  \"tile\": {\"m\": %d, \"n\": %d, \"k\": %d},\n"
    "  \"bytes_moved\": %.0f,\n"
    "  \"bytes_saved_by_fusion\": %.0f,\n"
    "  \"work_gflops\": %.2f,\n"
    "  \"model_roofop_ms_per_iter\": %.6f,\n"
    "  \"model_total_ms\": %.3f,\n"
    "  \"note\": \"解析式估算，非实测。要真实数字请用 tools/xmex_bench.py\"\n"
    "}",
    str_data(kernelName),
    static_cast<long long>(m), static_cast<long long>(n),
    static_cast<long long>(k), static_cast<long long>(iters),
    peakTflops, peakBwGbs, ridgePoint, intensity,
    (tCompute > tMemory) ? "compute" : "memory",
    tileM, tileN, tileK,
    bytes, savedBytes,
    flopsPerIter * iters / 1e9,
    tRooftop * 1e3,
    tRooftop * static_cast<double>(iters) * 1e3);

  return str_new(buf, static_cast<int64_t>(std::strlen(buf)));
}