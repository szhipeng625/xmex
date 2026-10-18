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

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
