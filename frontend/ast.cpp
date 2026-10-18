#include "ast.h"

#include <ostream>

namespace xmex {

namespace {

void pad(std::ostream &os, int indent) {
  for (int i = 0; i < indent; ++i) os << "  ";
}

// 把字符串里的换行等转义掉,免得 dump 输出被撑乱
std::string quoted(const std::string &s) {
  std::string r = "\"";
  for (char c : s) {
    switch (c) {
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '"':  r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      default:   r += c;
    }
  }
  return r + "\"";
}

void dumpBlock(std::ostream &os, int indent, const std::vector<StmtPtr> &body) {
  if (body.empty()) {
    pad(os, indent);
    os << "(空)\n";
    return;
  }
  for (const auto &s : body) s->dump(os, indent);
}

}  // namespace

const char *typeNameStr(TypeName t) {
  switch (t) {
    case TypeName::Any:   return "Any";
    case TypeName::Int:   return "int";
    case TypeName::Float: return "float";
    case TypeName::Bool:  return "bool";
    case TypeName::Str:   return "str";
  }
  return "<?>";
}

// ---------------------------------------------------------------------------
// 表达式
// ---------------------------------------------------------------------------

void IntLit::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Int " << value << "\n";
}

void FloatLit::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Float " << value << "\n";
}

void StringLit::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "String " << quoted(value) << "\n";
}

void BoolLit::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Bool " << (value ? "True" : "False") << "\n";
}

void NoneLit::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "None\n";
}

void Name::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Name " << id << "\n";
}

void BinOp::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "BinOp '" << op << "'\n";
  lhs->dump(os, indent + 1);
  rhs->dump(os, indent + 1);
}

void UnaryOp::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "UnaryOp '" << op << "'\n";
  operand->dump(os, indent + 1);
}

void Call::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Call\n";
  pad(os, indent + 1);
  os << "callee:\n";
  callee->dump(os, indent + 2);
  if (!args.empty()) {
    pad(os, indent + 1);
    os << "args:\n";
    for (const auto &a : args) a->dump(os, indent + 2);
  }
  if (!kwargs.empty()) {
    pad(os, indent + 1);
    os << "kwargs:\n";
    for (const auto &kw : kwargs) {
      pad(os, indent + 2);
      os << kw.first << " =\n";
      kw.second->dump(os, indent + 3);
    }
  }
}

void RangeExpr::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Range\n";
  auto part = [&](const char *label, const ExprPtr &e) {
    if (!e) return;
    pad(os, indent + 1);
    os << label << ":\n";
    e->dump(os, indent + 2);
  };
  part("start", start);
  part("stop", stop);
  part("step", step);
}

void InputExpr::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Input(";
  for (size_t i = 0; i < kinds.size(); ++i) {
    if (i) os << ", ";
    os << typeNameStr(kinds[i]);
  }
  os << ")\n";
}

void Subscript::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Subscript\n";
  pad(os, indent + 1);
  os << "obj:\n";
  obj->dump(os, indent + 2);
  pad(os, indent + 1);
  os << "index:\n";
  index->dump(os, indent + 2);
}

void SliceExpr::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Slice\n";
  pad(os, indent + 1);
  os << "obj:\n";
  obj->dump(os, indent + 2);
  auto part = [&](const char *label, const ExprPtr &e) {
    pad(os, indent + 1);
    os << label << ": ";
    if (e) {
      os << "\n";
      e->dump(os, indent + 2);
    } else {
      os << "(省略)\n";
    }
  };
  part("lo", lo);
  part("hi", hi);
  part("step", step);
}

void ListLit::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "List[" << items.size() << "]\n";
  for (const auto &i : items) i->dump(os, indent + 1);
}

void DictLit::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Dict[" << items.size() << "]\n";
  for (const auto &kv : items) {
    pad(os, indent + 1);
    os << "key:\n";
    kv.first->dump(os, indent + 2);
    pad(os, indent + 1);
    os << "value:\n";
    kv.second->dump(os, indent + 2);
  }
}

void TupleLit::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Tuple[" << items.size() << "]\n";
  for (const auto &i : items) i->dump(os, indent + 1);
}

void Attribute::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Attribute ." << name << "\n";
  obj->dump(os, indent + 1);
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

void ExprStmt::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "ExprStmt\n";
  expr->dump(os, indent + 1);
}

void Assign::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Assign (" << targets.size() << " 个目标)\n";
  pad(os, indent + 1);
  os << "targets:\n";
  for (const auto &t : targets) t->dump(os, indent + 2);
  pad(os, indent + 1);
  os << "value:\n";
  value->dump(os, indent + 2);
}

void AugAssign::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "AugAssign '" << op << "='\n";
  pad(os, indent + 1);
  os << "target:\n";
  target->dump(os, indent + 2);
  pad(os, indent + 1);
  os << "value:\n";
  value->dump(os, indent + 2);
}

void If::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "If\n";
  pad(os, indent + 1);
  os << "cond:\n";
  cond->dump(os, indent + 2);
  pad(os, indent + 1);
  os << "body:\n";
  dumpBlock(os, indent + 2, body);

  for (const auto &e : elifs) {
    pad(os, indent + 1);
    os << "elif cond:\n";
    e.first->dump(os, indent + 2);
    pad(os, indent + 1);
    os << "body:\n";
    dumpBlock(os, indent + 2, e.second);
  }
  if (!orelse.empty()) {
    pad(os, indent + 1);
    os << "else body:\n";
    dumpBlock(os, indent + 2, orelse);
  }
}

void While::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "While\n";
  pad(os, indent + 1);
  os << "cond:\n";
  cond->dump(os, indent + 2);
  pad(os, indent + 1);
  os << "body:\n";
  dumpBlock(os, indent + 2, body);
}

void For::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "For " << var << "\n";
  pad(os, indent + 1);
  os << "iterable:\n";
  iterable->dump(os, indent + 2);
  pad(os, indent + 1);
  os << "body:\n";
  dumpBlock(os, indent + 2, body);
}

void Return::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Return\n";
  if (value) value->dump(os, indent + 1);
}

void Break::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Break\n";
}

void Continue::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Continue\n";
}

// 形参列表的共用打印:函数定义与匿名函数都用它。
// 默认值与可变参数都显式标出来,便于核对解析结果。
static void dumpParams(std::ostream &os, const std::vector<Param> &params) {
  for (size_t i = 0; i < params.size(); ++i) {
    if (i) os << ", ";
    if (params[i].isVararg) os << "*";
    os << params[i].name;
    if (params[i].type != TypeName::Any) os << ": " << typeNameStr(params[i].type);
    if (params[i].defaultValue) os << "=<默认值>";
  }
}

void FuncDef::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "FuncDef " << name << "(";
  dumpParams(os, params);
  os << ")";
  if (retType != TypeName::Any) os << " -> " << typeNameStr(retType);
  os << "\n";
  pad(os, indent + 1);
  os << "body:\n";
  dumpBlock(os, indent + 2, body);
}

void Lambda::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Lambda(";
  dumpParams(os, params);
  os << ")\n";
  pad(os, indent + 1);
  os << "body:\n";
  dumpBlock(os, indent + 2, body);
}

void ClassDef::dump(std::ostream &os, int indent) const {
  pad(os, indent);
  os << "Class " << name;
  if (!baseName.empty()) os << " : " << baseName;
  os << "\n";
  for (const auto &m : methods) {
    pad(os, indent + 1);
    os << "method:\n";
    m->dump(os, indent + 2);
  }
}

void Module::dump(std::ostream &os) const {
  os << "Module\n";
  for (const auto &s : body) s->dump(os, 1);
}

}  // namespace xmex
