#!/usr/bin/env bash
#
# Xmex 的 Windows(UCRT64)构建脚本。
#
#   bash build-windows.sh [构建目录]      # 默认 build-win
#
# 存在的理由是这个项目不能在任意 shell 里构建 —— 见下面第 1 步。
# 从 MSYS2 的 UCRT64 shell(D:\msys64\ucrt64.exe)里跑也可以,那时 PATH
# 本来就是对的,本脚本的修正只是空转。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

# ---------------------------------------------------------------------------
# 1) PATH 去毒
# ---------------------------------------------------------------------------
# 本机 PATH 里 /mingw64/bin（实际是 D:\Git\mingw64,MSVCRT 版）排在
# /d/msys64/ucrt64/bin 前面。UCRT64 的 cc1plus.exe 会按 PATH 解析依赖 DLL,
# 命中的是 MSVCRT 版的 libgcc_s_seh-1.dll / libwinpthread-1.dll ——
# ABI 不兼容,cc1plus 静默崩溃:任何编译都 exit 1 且没有任何输出。
#
# 不只毒编译:构建期 cmake 要运行 xmexc.exe,测试期要加载 libLLVM-22.dll,
# 全走同一份 PATH,所以整条流水线都必须在修好的 PATH 里。
export PATH=$(echo "$PATH" | tr ':' '\n' | grep -v -E '^/(d/)?mingw64/bin$' | paste -sd:)
export PATH="/d/msys64/ucrt64/bin:$PATH"

# ---------------------------------------------------------------------------
# 2) 自检:编译器必须真的能编出程序
# ---------------------------------------------------------------------------
# "g++ --version 能打印" 不代表能编译 —— 崩溃发生在 cc1plus 启动时,
# 而 --version 不经过 cc1plus。必须实编一次。
PATHCHECK="${TMPDIR:-/tmp}/xmex_pathcheck.exe"
printf 'int main(){return 0;}\n' | g++ -x c++ - -o "$PATHCHECK"
"$PATHCHECK"
echo "[ok] PATH 修复生效: $(which g++) / $(g++ --version | head -1)"

# ---------------------------------------------------------------------------
# 3) configure
# ---------------------------------------------------------------------------
# LLVM_DIR 显式传入:CMakeLists 里的 HINTS 指向 Linux 路径,在 Windows 上
# 不匹配即跳过,不会自动找到 UCRT64 的 LLVM。
BUILD_DIR="${1:-build-win}"
cmake -G Ninja -S "$REPO_ROOT" -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLLVM_DIR="$(llvm-config --cmakedir)"

# ---------------------------------------------------------------------------
# 4) 先建关键目标
# ---------------------------------------------------------------------------
# 把 ABI / 链接问题在最小集合上暴露出来,失败点比全量构建清楚得多:
#   undefined reference to `__imp_socket`  → ws2_32 没链上
#   undefined reference to `py_raft_*`     → 源文件边界划错了
#   链接通过但 jit_spike 段错误            → abi.cpp 的 sret 边界不对
echo "--- 关键目标 ---"
cmake --build "$BUILD_DIR" --target \
      xmexc jit_spike abi_test runtime_test gc_test frontend_test

# ---------------------------------------------------------------------------
# 5) 全量构建 + 测试
# ---------------------------------------------------------------------------
echo "--- 全量构建 ---"
cmake --build "$BUILD_DIR"

# 出目标文件先冒烟:必须是 COFF,出现 ELF 说明 kTargetTriple 没改对。
echo "--- COFF 冒烟 ---"
./"$BUILD_DIR"/bin/xmexc.exe tests/e2e/arith_ops.pys -o "$BUILD_DIR/arith_ops.o"
file "$BUILD_DIR/arith_ops.o"

echo "--- 测试 ---"
ctest --test-dir "$BUILD_DIR" --output-on-failure
