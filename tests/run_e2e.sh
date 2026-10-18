#!/usr/bin/env bash
# 手动跑 e2e 用例的开发脚本:编译 -> 链接 -> 运行 -> 显示输出。
# ctest 里有等价的自动化版本(见 CMakeLists.txt),这个脚本用于快速迭代。
set -u

ROOT=/root/autodl-tmp/llvm-project/llvm
OUT=/tmp/xmex-e2e
mkdir -p "$OUT"

pass=0
fail=0

for pys in "$ROOT"/tests/e2e/*.pys; do
  name=$(basename "$pys" .pys)
  obj="$OUT/$name.o"
  drv="$OUT/${name}_main.cpp"
  exe="$OUT/$name.exe"

  if ! "$ROOT/build/bin/xmexc" "$pys" -o "$obj" > "$OUT/$name.log" 2>&1; then
    echo "=== $name: 编译失败 ==="
    cat "$OUT/$name.log"
    fail=$((fail + 1))
    continue
  fi

  # 每个用例配一个最小的 C++ 驱动,只调用编译产物
  printf 'extern "C" void xmex_%s_main();\nint main(){ xmex_%s_main(); return 0; }\n' \
    "$name" "$name" > "$drv"

  if ! clang++-22 -std=c++20 "$drv" "$obj" -o "$exe" \
        -L"$ROOT/build/bin" -lxmex_runtime -Wl,-rpath,"$ROOT/build/bin" 2> "$OUT/$name.link.log"; then
    echo "=== $name: 链接失败 ==="
    cat "$OUT/$name.link.log"
    fail=$((fail + 1))
    continue
  fi

  echo "--- $name ---"
  if [ -f "$ROOT/tests/e2e/$name.in" ]; then
    PATH="$ROOT/build/bin:$PATH" "$exe" < "$ROOT/tests/e2e/$name.in"
  else
    PATH="$ROOT/build/bin:$PATH" "$exe"
  fi
  echo "    (退出码 $?)"
  pass=$((pass + 1))
done

echo
echo "编译链接成功 $pass 个,失败 $fail 个"
