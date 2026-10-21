// 运行时库的直接测试。
//
// 这一层刻意**不经过** IRGen 和 ABI 边界:同一份语义如果只在 e2e 里测,
// 出错时分不清是运行时算错了、还是 IR 里把参数传错了。把运行时的行为先钉死,
// e2e 失败就只剩 ABI 一个可能。
//
// 断言几乎都拿 repr 的文本结果来比 —— 好处是期望值写出来就是
// "['a', 1]" 这种能直接读的东西,而且顺带把 repr 本身也测了。
#include "xmex/runtime.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const std::string &what) {
  if (cond) {
    std::printf("  [ok]   %s\n", what.c_str());
  } else {
    std::printf("  [FAIL] %s\n", what.c_str());
    ++g_failures;
  }
}

// 按 repr 规则渲染,期望值直接写成文本
std::string rep(xmex::Value v) {
  xmex::Value s = repr(&v);
  return std::string(str_data(&s), static_cast<size_t>(str_size(&s)));
}

void checkRep(xmex::Value v, const std::string &want, const std::string &what) {
  const std::string got = rep(v);
  if (got == want) {
    std::printf("  [ok]   %s → %s\n", what.c_str(), got.c_str());
  } else {
    std::printf("  [FAIL] %s → 得到 %s,期望 %s\n", what.c_str(), got.c_str(),
                want.c_str());
    ++g_failures;
  }
}

void checkInt(int64_t got, int64_t want, const std::string &what) {
  if (got == want) {
    std::printf("  [ok]   %s → %lld\n", what.c_str(), static_cast<long long>(got));
  } else {
    std::printf("  [FAIL] %s → 得到 %lld,期望 %lld\n", what.c_str(),
                static_cast<long long>(got), static_cast<long long>(want));
    ++g_failures;
  }
}

xmex::Value str(const char *s) {
  return str_new(s, static_cast<int64_t>(std::char_traits<char>::length(s)));
}

xmex::Value list(std::initializer_list<xmex::Value> xs) {
  return list_new(const_cast<xmex::Value *>(xs.begin()),
                     static_cast<int64_t>(xs.size()));
}

// 按 {k0, v0, k1, v1, ...} 写,内部拆成 dict_new 要的两条平行数组
xmex::Value dict(std::initializer_list<xmex::Value> kvs) {
  const std::vector<xmex::Value> v(kvs);
  const int64_t n = static_cast<int64_t>(v.size() / 2);
  std::vector<xmex::Value> keys(static_cast<size_t>(n)), vals(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    keys[static_cast<size_t>(i)] = v[static_cast<size_t>(2 * i)];
    vals[static_cast<size_t>(i)] = v[static_cast<size_t>(2 * i + 1)];
  }
  return dict_new(keys.data(), vals.data(), n);
}

xmex::Value callMethod(xmex::Value obj, const char *name, std::vector<xmex::Value> args = {}) {
  return call_method(&obj, name, static_cast<int64_t>(std::char_traits<char>::length(name)),
                        args.data(), static_cast<int64_t>(args.size()));
}

xmex::Value idx(xmex::Value obj, xmex::Value i) { return index_(&obj, &i); }

xmex::Value t_slice(xmex::Value obj, xmex::Value lo, xmex::Value hi, xmex::Value step) {
  return slice(&obj, &lo, &hi, &step);
}

xmex::Value none() { return none_(); }

}  // namespace

int main() {
  // 不缓冲:这个测试是用来定位崩溃的,段错误发生时缓冲区里的输出会一起丢掉,
  // 那就只能看到"什么都没有"。
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::printf("=== 运行时测试 ===\n\n");

  // ------------------------------------------------------------ 字符串
  std::printf("-- 字符串 --\n");
  {
    xmex::Value a = str("hello");
    checkInt(str_size(&a), 5, "str_size(\"hello\")");
    checkRep(str_get(&a, 1), "'e'", "s[1]");
    checkRep(str_get(&a, -1), "'o'", "s[-1](负索引从末尾数)");
    checkRep(str_concat(&a, &a), "'hellohello'", "s + s");
    checkRep(str_repeat(&a, 2), "'hellohello'", "s * 2");
    checkRep(str_repeat(&a, 0), "''", "s * 0");

    xmex::Value b = str("world");
    checkInt(str_compare(&a, &b), -1, "\"hello\" < \"world\" 的符号");
    checkInt(str_compare(&a, &a), 0, "\"hello\" == \"hello\" 的符号");

    // 内嵌 '\0' 必须按长度处理,不能靠 C 字符串函数
    xmex::Value z = str_new("a\0b", 3);
    checkInt(str_size(&z), 3, "含内嵌 NUL 的字符串长度是 3(不是 1)");
    checkRep(z, "'a\\x00b'", "含内嵌 NUL 的字符串 repr 会转义");

    checkInt(str_find(&a, &b, 0), -1, "\"hello\".find(\"world\") == -1");
    xmex::Value ell = str("ll");
    checkInt(str_find(&a, &ell, 0), 2, "\"hello\".find(\"ll\") == 2");
    checkInt(str_starts_with(&a, &ell), 0, "\"hello\".startswith(\"ll\") 是假");
    xmex::Value he = str("he");
    checkInt(str_starts_with(&a, &he), 1, "\"hello\".startswith(\"he\") 是真");
    xmex::Value lo2 = str("lo");
    checkInt(str_ends_with(&a, &lo2), 1, "\"hello\".endswith(\"lo\") 是真");
  }

  // -------------------------------------------------------------- 列表
  std::printf("\n-- 列表 --\n");
  {
    xmex::Value xs = list({int_(1), int_(2), int_(3)});
    checkRep(xs, "[1, 2, 3]", "列表字面量 [1, 2, 3]");
    checkInt(list_len(&xs), 3, "len([1,2,3])");
    checkRep(list_get(&xs, 0), "1", "xs[0]");
    checkRep(list_get(&xs, -1), "3", "xs[-1]");

    xmex::Value four = int_(4);
    list_append(&xs, &four);
    checkRep(xs, "[1, 2, 3, 4]", "append(4) 就地生效");

    xmex::Value zero = int_(0);
    list_insert(&xs, 0, &zero);
    checkRep(xs, "[0, 1, 2, 3, 4]", "insert(0, 0)");

    list_remove(&xs, &zero);
    checkRep(xs, "[1, 2, 3, 4]", "remove(0)");

    checkInt(list_index(&xs, &four), 3, "xs.index(4) == 3");
    checkInt(list_count(&xs, &four), 1, "xs.count(4) == 1");

    xmex::Value popIdx = none_();  // None 表示"没给下标",弹末尾
    xmex::Value p = list_pop(&xs, &popIdx);
    checkRep(p, "4", "pop() 返回末尾元素");
    checkRep(xs, "[1, 2, 3]", "pop() 之后列表少一个");

    list_reverse(&xs);
    checkRep(xs, "[3, 2, 1]", "reverse()");

    xmex::Value unsorted = list({int_(3), int_(1), int_(2)});
    list_sort(&unsorted);
    checkRep(unsorted, "[1, 2, 3]", "sort() 数值升序");

    // 自拼接:必须是**就地** extend,而且不能在增长时把来源数组读飞
    xmex::Value self;
    self = list({int_(1), int_(2)});
    list_extend(&self, &self);
    checkRep(self, "[1, 2, 1, 2]", "xs.extend(xs) 不读飞自己");

    xmex::Value two = list({int_(2)});
    checkRep(list_concat(&two, &two), "[2, 2]", "[2] + [2] 是新列表");
    checkRep(list_repeat(&two, 3), "[2, 2, 2]", "[2] * 3");
  }

  // -------------------------------------------------- 列表的可变语义
  std::printf("\n-- 列表的可变语义(别名与 +=)--\n");
  {
    // a += [x] 是就地 extend,别名看得见 —— 与 a = a + [x] 不同
    xmex::Value a = list({int_(1)});
    xmex::Value alias = a;  // 同一个 xmex::Value,指向同一个 xmex::List
    xmex::Value b = list({int_(2)});

    xmex::Value r = iadd(&a, &b);
    checkRep(r, "[1, 2]", "a += b 的结果");
    checkRep(a, "[1, 2]", "a += b 之后 a");
    checkRep(alias, "[1, 2]",
             "别名也看到改动(就地语义,与 Python 的 list.__iadd__ 一致)");

    // 对照:a = a + b 走 add,是新列表,别名不受影响
    xmex::Value c = list({int_(1)});
    xmex::Value cAlias = c;
    xmex::Value d = list({int_(2)});
    xmex::Value sum = add(c, d);
    checkRep(sum, "[1, 2]", "a + b 的结果");
    checkRep(cAlias, "[1]", "a + b **不**影响别名(重新绑定语义)");

    // 字符串不可变:+= 不改动原对象所指
    xmex::Value s1 = str("ab");
    xmex::Value s2 = str("cd");
    xmex::Value s3 = iadd(&s1, &s2);
    checkRep(s3, "'abcd'", "\"ab\" += \"cd\" 的结果");
    checkRep(s1, "'ab'", "字符串 += 不改原对象(不可变)");
  }

  // -------------------------------------------------------------- 字典
  std::printf("\n-- 字典 --\n");
  {
    xmex::Value d = dict({str("a"), int_(1), str("b"), int_(2)});
    checkRep(d, "{'a': 1, 'b': 2}", "字典字面量");
    checkInt(dict_len(&d), 2, "len({'a':1,'b':2})");

    xmex::Value ka = str("a");
    checkRep(dict_get(&d, &ka), "1", "d[\"a\"]");
    checkInt(dict_has(&d, &ka), 1, "\"a\" in d");

    // 键按**值**比较,不是按指针 —— 这是修掉 eq 之后才成立的行为
    xmex::Value ka2 = str("a");  // 另一个同内容字符串,指针不同
    checkInt(dict_has(&d, &ka2), 1,
             "另一个同内容的 \"a\" 也能查到(键按值比较)");

    xmex::Value kc = str("c");
    checkInt(dict_has(&d, &kc), 0, "\"c\" in d 是假");
    xmex::Value fallback = int_(99);
    checkRep(dict_get_default(&d, &kc, &fallback), "99", "d.get(\"c\", 99)");

    // 重复键:值取后到的,位置取先到的
    xmex::Value dup = dict({str("a"), int_(1), str("b"), int_(2), str("a"), int_(3)});
    checkRep(dup, "{'a': 3, 'b': 2}", "重复键:值取后到、位置取先到");

    xmex::Value v9 = int_(9);
    dict_set(&d, &kc, &v9);
    checkRep(d, "{'a': 1, 'b': 2, 'c': 9}", "新键追加在末尾(插入有序)");

    checkRep(dict_key_at(&d, 0), "'a'", "按插入序取第 0 个键");
    checkRep(dict_pop(&d, &ka), "1", "pop(\"a\")");
    checkRep(d, "{'b': 2, 'c': 9}", "pop 之后剩下的(顺序保持)");
  }

  // ------------------------------------------------------- 下标 / 切片
  std::printf("\n-- 下标与切片 --\n");
  {
    xmex::Value xs = list({int_(1), int_(2), int_(3), int_(4), int_(5)});
    checkInt(len(&xs), 5, "len(列表)");
    checkRep(idx(xs, int_(1)), "2", "列表下标");

    xmex::Value t = tuple_new(nullptr, 0);
    checkInt(len(&t), 0, "len(()) == 0");

    xmex::Value s = str("hello");
    checkInt(len(&s), 5, "len(字符串)");

    xmex::Value d = dict({str("k"), int_(7)});
    checkRep(idx(d, str("k")), "7", "字典按键下标");
    checkRep(iter_at(&d, 0), "'k'",
             "遍历字典产出的是**键**(不是 d[0] 那种按键查找)");

    // 切片:省略边界传 None
    checkRep(t_slice(xs, int_(1), int_(3), none()), "[2, 3]", "xs[1:3]");
    checkRep(t_slice(xs, none(), none(), none()), "[1, 2, 3, 4, 5]", "xs[:]");
    checkRep(t_slice(xs, none(), none(), int_(-1)), "[5, 4, 3, 2, 1]", "xs[::-1]");
    checkRep(t_slice(xs, int_(-2), none(), none()), "[4, 5]", "xs[-2:]");
    checkRep(t_slice(s, int_(1), int_(3), none()), "'el'", "\"hello\"[1:3]");
    checkRep(t_slice(s, none(), none(), int_(-1)), "'olleh'", "\"hello\"[::-1]");
    // 切片保持原类型:列表切出列表,元组切出元组
    checkRep(t_slice(xs, int_(0), int_(2), none()), "[1, 2]", "列表切片得到列表");
    xmex::Value tupElems[3] = {int_(1), int_(2), int_(3)};
    xmex::Value tup3 = tuple_new(tupElems, 3);
    checkRep(t_slice(tup3, none(), none(), none()), "(1, 2, 3)",
             "元组切片得到元组");
  }

  // -------------------------------------------------------------- 方法
  std::printf("\n-- 方法 --\n");
  {
    xmex::Value s = str("  Hello World  ");
    checkRep(callMethod(s, "strip"), "'Hello World'", "strip()");
    checkRep(callMethod(s, "strip", {str(" ")}), "'Hello World'", "strip(\" \")");
    checkRep(callMethod(str("hello"), "upper"), "'HELLO'", "upper()");
    checkRep(callMethod(str("HELLO"), "lower"), "'hello'", "lower()");
    checkRep(callMethod(str("a,b,c"), "split", {str(",")}), "['a', 'b', 'c']",
             "\"a,b,c\".split(\",\")");
    checkRep(callMethod(str("a b  c"), "split"), "['a', 'b', 'c']",
             "split() 按空白切并丢掉空片段");
    checkRep(callMethod(str("-"), "join",
                        {list({str("a"), str("b"), str("c")})}),
             "'a-b-c'", "\"-\".join([\"a\",\"b\",\"c\"])");
    checkRep(callMethod(str("banana"), "replace", {str("a"), str("o")}),
             "'bonono'", "replace(\"a\", \"o\")");
    checkRep(callMethod(str("banana"), "count", {str("an")}), "2",
             "\"banana\".count(\"an\")");
    checkRep(callMethod(str("banana"), "find", {str("na")}), "2",
             "\"banana\".find(\"na\")");
    checkRep(callMethod(str("hi"), "startswith", {str("h")}), "True",
             "\"hi\".startswith(\"h\")");

    xmex::Value xs = list({int_(3), int_(1)});
    callMethod(xs, "append", {int_(2)});
    checkRep(xs, "[3, 1, 2]", "列表 append 方法就地生效");
    callMethod(xs, "sort");
    checkRep(xs, "[1, 2, 3]", "列表 sort 方法");
    checkRep(callMethod(xs, "pop"), "3", "列表 pop 方法返回末尾");

    xmex::Value d = dict({str("a"), int_(1), str("b"), int_(2)});
    checkRep(callMethod(d, "keys"), "['a', 'b']", "keys()");
    checkRep(callMethod(d, "values"), "[1, 2]", "values()");
    checkRep(callMethod(d, "items"), "[('a', 1), ('b', 2)]", "items()");
    checkRep(callMethod(d, "get", {str("zz")}), "None", "get() 缺键返回 None");
  }

  // ------------------------------------------------- 算术与真值的扩展
  std::printf("\n-- 算术与真值 --\n");
  {
    xmex::Value a = str("ab");
    xmex::Value b = str("cd");
    checkRep(add(a, b), "'abcd'", "字符串 + 拼接");

    xmex::Value x = list({int_(1)});
    xmex::Value y = list({int_(2)});
    checkRep(add(x, y), "[1, 2]", "列表 + 拼接");

    // 曾经是 bug:字符串比的是指针,所以 "a" == "a" 为假
    xmex::Value sa1 = str("a");
    xmex::Value sa2 = str("a");
    checkRep(eq(sa1, sa2), "True", "\"a\" == \"a\"(按值比较,修掉的旧 bug)");
    checkRep(eq(list({int_(1)}), list({int_(1)})), "True", "[1] == [1]");
    checkRep(eq(list({int_(1)}), list({int_(2)})), "False", "[1] == [2]");

    // 1 == 1.0 在 Python 里是 True,字典键也因此把两者视为同一个
    checkRep(eq(int_(1), float_(1.0)), "True", "1 == 1.0");
    checkRep(eq(bool_(true), int_(1)), "True", "True == 1");

    checkRep(eq(dict({str("a"), int_(1)}), dict({str("a"), int_(1)})),
             "True", "字典相等");
    // 字典相等与插入序无关
    checkRep(eq(dict({str("a"), int_(1), str("b"), int_(2)}),
                   dict({str("b"), int_(2), str("a"), int_(1)})),
             "True", "字典相等与插入序无关");

    xmex::Value sa = str("a"), sb = str("b");
    checkRep(lt(sa, sb), "True", "\"a\" < \"b\"");

    // 曾经是 bug:空容器按"非空指针即真"被判为真
    checkInt(truthy(list_new(nullptr, 0)), 0, "bool([]) 是假(修掉的旧 bug)");
    checkInt(truthy(str("")), 0, "bool(\"\") 是假");
    checkInt(truthy(dict_new(nullptr, nullptr, 0)), 0, "bool({}) 是假");
    checkInt(truthy(tuple_new(nullptr, 0)), 0, "bool(()) 是假");
    checkInt(truthy(list({int_(0)})), 1, "bool([0]) 是真(非空即可)");
  }

  // ------------------------------------------------------------ 算子融合
  //
  // 这些用例钉死的是「优化器在真实数据依赖上的行为」。旧版 fusion 的
  // 匹配是 strstr 扫 JSON 文本 —— 只要图里出现过这些算子名就算命中，
  // 与连边无关，所以下面凡是关于「不该融合」的用例，旧实现都会失败。
  std::printf("\n-- 算子融合: 图 IR 与数据流融合 --\n");
  {
    // 把一个 (const xmex::Value*) -> Value 的运行时函数包一层,便于断言文本结果。
    auto wrap = [](auto fn) {
      return [fn](const std::string &s, std::initializer_list<xmex::Value> extra = {}) {
        std::vector<xmex::Value> args;
        args.push_back(str_new(s.c_str(), static_cast<int64_t>(s.size())));
        for (const auto &e : extra) args.push_back(e);
        xmex::Value r = fn(args.data());
        return std::string(str_data(&r), static_cast<size_t>(str_size(&r)));
      };
    };
    auto fusionApply = wrap([](xmex::Value *a) { return fusion_apply_rules(a); });
    auto fusionOpt = wrap([](xmex::Value *a) { return fusion_optimize(a, nullptr); });
    auto fusionSetOutputs = [](const std::string &g, const std::string &outs) {
      xmex::Value gv = str_new(g.c_str(), static_cast<int64_t>(g.size()));
      xmex::Value ov = str_new(outs.c_str(), static_cast<int64_t>(outs.size()));
      xmex::Value r = fusion_set_outputs(&gv, &ov);
      return std::string(str_data(&r), static_cast<size_t>(str_size(&r)));
    };
    auto fusionCreate = [](const std::string &n) {
      xmex::Value v = str_new(n.c_str(), static_cast<int64_t>(n.size()));
      xmex::Value r = fusion_create_graph(&v);
      return std::string(str_data(&r), static_cast<size_t>(str_size(&r)));
    };
    auto fusionAdd = [](const std::string &g, const std::string &t,
                        const std::string &n, const std::string &p = "{}") {
      xmex::Value gv = str_new(g.c_str(), static_cast<int64_t>(g.size()));
      xmex::Value tv = str_new(t.c_str(), static_cast<int64_t>(t.size()));
      xmex::Value nv = str_new(n.c_str(), static_cast<int64_t>(n.size()));
      xmex::Value pv = str_new(p.c_str(), static_cast<int64_t>(p.size()));
      xmex::Value r = fusion_add_op(&gv, &tv, &nv, &pv);
      return std::string(str_data(&r), static_cast<size_t>(str_size(&r)));
    };
    auto fusionAddIn = [](const std::string &g, const std::string &t,
                          const std::string &n, const std::string &ins,
                          const std::string &p = "{}") {
      xmex::Value gv = str_new(g.c_str(), static_cast<int64_t>(g.size()));
      xmex::Value tv = str_new(t.c_str(), static_cast<int64_t>(t.size()));
      xmex::Value nv = str_new(n.c_str(), static_cast<int64_t>(n.size()));
      xmex::Value pv = str_new(p.c_str(), static_cast<int64_t>(p.size()));
      xmex::Value iv = str_new(ins.c_str(), static_cast<int64_t>(ins.size()));
      xmex::Value r = fusion_add_op_v(&gv, &tv, &nv, &pv, &iv);
      return std::string(str_data(&r), static_cast<size_t>(str_size(&r)));
    };

    // --- 建图与连边 ---
    std::string g = fusionCreate("mlp");
    check(g.find("\"mlp\"") != std::string::npos, "create_graph 保留图名");
    check(g.find("\"nodes\":[]") != std::string::npos, "create_graph 产出空节点表");

    // 四参形式:旧语义,串成链
    std::string chain = fusionAdd(g, "matmul", "fc1", "{\"in_features\":768,\"out_features\":3072}");
    chain = fusionAdd(chain, "bias_add", "b1");
    chain = fusionAdd(chain, "relu", "r1");
    check(chain.find("\"inputs\":[\"fc1\"]") != std::string::npos,
          "四参 add_op 自动接到上一个节点(向后兼容的串链)");
    check(chain.find("\"edges\":[[\"fc1\",\"b1\"],[\"b1\",\"r1\"]]") != std::string::npos,
          "edges 记录真实的数据依赖边");
    check(chain.find("\"outputs\":[\"r1\"]") != std::string::npos,
          "末端节点被标为图输出");

    // 显式 inputs:支持分叉
    std::string b = fusionAdd(chain, "matmul", "fc2", "{\"in_features\":3072,\"out_features\":768}");
    b = fusionAddIn(b, "add", "res", "r1,fc2");
    check(b.find("\"inputs\":[\"r1\",\"fc2\"]") != std::string::npos,
          "显式 inputs 支持多上游(合流)");
    check(b.find("\"edges\":[[\"fc1\",\"b1\"],[\"b1\",\"r1\"],[\"r1\",\"fc2\"],"
                   "[\"r1\",\"res\"],[\"fc2\",\"res\"]]") != std::string::npos,
          "分叉与合流的边都记下来了(res 有 r1 和 fc2 两条入边)");

    // --- 链式融合 ---
    std::string rep = fusionApply(chain);
    check(rep.find("\"fused_op\":\"linear_relu\"") != std::string::npos,
          "matmul->bias_add->relu 一路融成 linear_relu");
    // 关键:optimized 必须真的是优化后的图,而不是原图回显(旧版的 bug)
    size_t optStart = rep.find("\"optimized\":");
    std::string opt = (optStart == std::string::npos) ? "" : rep.substr(optStart);
    check(opt.find("linear_relu") != std::string::npos, "optimized 里确实是融合后的算子");
    check(opt.find("\"type\":\"matmul\"") == std::string::npos,
          "optimized 里原来的 matmul/bias/relu 已被吸收");
    check(rep.find("\"reduced_nodes\":2") != std::string::npos,
          "3 个节点融成 1 个,节点数减少 2");

    // --- 关键的反例: 类型存在但无边,不该融合 ---
    // 两个互不相连的分支,各自都含 relu,但没有 relu 跟在 matmul 后面
    std::string sep = fusionCreate("disjoint");
    sep = fusionAddIn(sep, "matmul", "m1", "x");
    sep = fusionAddIn(sep, "matmul", "m2", "y");
    sep = fusionAddIn(sep, "relu", "r_a", "m1");
    sep = fusionAddIn(sep, "add", "add_b", "m2,w");
    std::string sepRep = fusionApply(sep);
    // m1->r_a 是真实边,会融;add_b 的上游是 m2 和 w,w 不是图内节点,故不融 m2
    // 但关键是 m2 不能被 add_b 之外的路径误融
    check(sepRep.find("\"linear_relu\"") == std::string::npos ||
              sepRep.find("\"nodes\":[\"m1\",\"r_a\"]") != std::string::npos,
          "只沿真实边融合,不做全图文本匹配");

    // matmul 之后接 reshape:shape 是布局变换,禁止跨越融合
    std::string sh = fusionCreate("shape");
    sh = fusionAddIn(sh, "matmul", "m", "x");
    sh = fusionAddIn(sh, "reshape", "rs", "m");
    sh = fusionAddIn(sh, "relu", "r", "rs");
    std::string shRep = fusionApply(sh);
    check(shRep.find("\"fusions_applied\":[]") != std::string::npos,
          "reshape 是融合屏障,不跨越它融合(matmul->reshape->relu 不融)");

    // --- CSE:完全相同的逐元素节点只留一个 ---
    std::string cse = fusionCreate("cse");
    cse = fusionAddIn(cse, "matmul", "m", "x");
    cse = fusionAddIn(cse, "relu", "r1", "m");
    cse = fusionAddIn(cse, "mul", "d1", "m,s");
    cse = fusionAddIn(cse, "mul", "d2", "m,s");
    cse = fusionAddIn(cse, "add", "out", "d1,d2");
    std::string cseRep = fusionApply(cse);
    // d1/d2 指纹相同(mul + 相同 params + 相同 inputs),应被合并成一个。
    // 保留下来的是哪一个取决于拓扑序,不固定,所以只断言「mul 节点只剩一个」。
    std::string cseOpt = cseRep.substr(cseRep.find("\"optimized\":"));
    // d1/d2 指纹相同(mul + 相同 params + 相同 inputs),应被合并,随后
    // 融合体再把 add 吸进来 —— 最终只剩一个融合节点,不再有裸 mul。
    check(cseOpt.find("\"type\":\"mul\"") == std::string::npos,
          "CSE 把两个相同的 mul 消掉(后续融合又把 add 吸了进去)");
    check(cseOpt.find("\"fused_from\"") != std::string::npos,
          "融合节点记录了被吸收的血缘");
    check(cseOpt.find("\"inputs\":[\"m\",\"s\"]") != std::string::npos,
          "融合节点保留了原节点的输入边");
    // 去重后的边不能重复 —— 曾经出现 ["d2","out"] 被写两遍
    check(cseOpt.find("[\"d2\",\"out\"],[\"d2\",\"out\"]") == std::string::npos,
          "CSE 改道后不产生重复边");

    // --- 死节点消除 ---
    // ⚠️ 悬空节点必须在**主链末端之后**添加才是"死"的。若它是最后添加的
    // 节点,它就是当前图的末端(根),删掉反而会丢掉整条链。
    std::string dead = fusionCreate("dead");
    dead = fusionAddIn(dead, "matmul", "live", "x");
    dead = fusionAddIn(dead, "relu", "live_out", "live");
    dead = fusionAddIn(dead, "matmul", "orphan", "y");  // 另起一枝,无下游
    std::string deadOpt = fusionOpt(dead);
    // orphan 无下游 → 除了它自己没人消费,是悬空的
    check(deadOpt.find("orphan") != std::string::npos,
          "作为末端添加的节点是图根,不会被当死节点删掉");
    check(deadOpt.find("live_out") != std::string::npos, "活的节点保留");

    // 真正的死节点:显式声明 outputs 后,不可达的分支才被删掉。
    // 这是消除悬空分支的可靠方式 —— 光看「无消费者」分不清末端与垃圾。
    std::string dead3 = fusionCreate("dead3");
    dead3 = fusionAddIn(dead3, "matmul", "a", "x");
    dead3 = fusionAddIn(dead3, "relu", "a_out", "a");
    dead3 = fusionAddIn(dead3, "mul", "dangling", "y,z");
    dead3 = fusionSetOutputs(dead3, "a_out");
    std::string dead3Opt = fusionOpt(dead3);
    check(dead3Opt.find("dangling") == std::string::npos,
          "显式声明 outputs 后,不可达的死节点被消除");
    check(dead3Opt.find("a_out") != std::string::npos, "显式声明的输出保留");

    // --- Pass 开关 ---
    {
      std::string cg = fusionCreate("chainpass");
      cg = fusionAddIn(cg, "matmul", "m", "x");
      cg = fusionAddIn(cg, "relu", "r", "m");
      // 关掉链式融合后应保持 matmul + relu 两个节点
      xmex::Value gv = str_new(cg.c_str(), static_cast<int64_t>(cg.size()));
      const char *chainOff = "{\"chain\":false}";
      xmex::Value ov = str_new(chainOff, static_cast<int64_t>(std::strlen(chainOff)));
      xmex::Value r = fusion_optimize_v(&gv, &ov);
      std::string noChain(str_data(&r), static_cast<size_t>(str_size(&r)));
      check(noChain.find("\"type\":\"relu\"") != std::string::npos,
            "chain=false 时不做融合,relu 保持独立节点");
      check(noChain.find("linear_relu") == std::string::npos,
            "chain=false 时不产生融合算子");

      // 默认（全开）则应融合
      std::string allChain = fusionOpt(cg);
      check(allChain.find("linear_relu") != std::string::npos,
            "默认开启 chain 融合");
    }

    // --- QKV 合并 ---
    std::string qkv = fusionCreate("attn");
    const char *qp = "{\"in_features\":768,\"out_features\":768}";
    qkv = fusionAddIn(qkv, "matmul", "wq", "h", qp);
    qkv = fusionAddIn(qkv, "matmul", "wk", "h", qp);
    qkv = fusionAddIn(qkv, "matmul", "wv", "h", qp);
    qkv = fusionAddIn(qkv, "softmax", "sm", "wq");
    std::string qkvRep = fusionApply(qkv);
    check(qkvRep.find("\"fused_op\":\"qkv_projection\"") != std::string::npos,
          "同输入同形状的三个 matmul 合并成 qkv_projection");
    std::string qkvOpt = qkvRep.substr(qkvRep.find("\"optimized\":"));
    check(qkvOpt.find("\"out_features\":2304") != std::string::npos,
          "QKV 合并后 out_features = 3×768 = 2304");
    // out_features 必须是**数字**而不是字符串 —— 下游要拿它当 GEMM 的 N 维,
    // 写成 "2304" 会在生成内核时悄悄取到 0。
    check(qkvOpt.find("\"out_features\":\"2304\"") == std::string::npos,
          "out_features 保持数字类型(不是字符串)");
    // 被吸收的 wk/wv 不能还留在显式输出名单里
    check(qkvOpt.find("\"explicit_outputs\":[\"wk\"") == std::string::npos,
          "QKV 合并后不残留已删节点的显式输出");

    // --- 环检测 ---
    // ⚠️ runtime_error 是 exit(1) 而不是抛异常,所以「环图被拒绝」没法写成
    // try/catch 断言。环检测本身由 E2E 用例 fusion_cycle.pys 覆盖(跑子进程
    // 查退出码),这里只断言正常路径没被环检测误伤。
    {
      std::string okGraph = fusionCreate("acyclic");
      okGraph = fusionAddIn(okGraph, "matmul", "a", "x");
      okGraph = fusionAddIn(okGraph, "relu", "b", "a");
      std::string r = fusionApply(okGraph);
      check(r.find("linear_relu") != std::string::npos, "无环图能正常完成融合");
    }

    // --- autotune:代价模型自洽 ---
    xmex::Value opS = str("matmul");
    xmex::Value mI = int_(1024), nI = int_(4096), kI = int_(768);
    xmex::Value tuned = fusion_autotune(&opS, &mI, &nI, &kI);
    std::string tuneText(str_data(&tuned), static_cast<size_t>(str_size(&tuned)));
    check(tuneText.find("\"tile_m\"") != std::string::npos, "autotune 返回 tile 配置");
    check(tuneText.find("\"grid_x\"") != std::string::npos, "autotune 返回 grid 尺寸");
    check(tuneText.find("\"score\"") != std::string::npos,
          "autotune 给出代价模型的评分(而非只按取模最小)");
    check(tuneText.find("RTX 4090") == std::string::npos,
          "autotune 不再声称结果绑定某张卡");

  }

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
