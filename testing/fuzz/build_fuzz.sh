#!/usr/bin/env bash
# ============================================================
# build_fuzz.sh — 用 libFuzzer + ASan/UBSan 构建前端 fuzz target
# ------------------------------------------------------------
# 编译最小前端链接闭包（6 个未修改的编译器源文件）+ fuzz_parser.cpp，
# 全部带覆盖率插桩（-fsanitize=fuzzer）与 Sanitizer，产出可执行 fuzz_parser。
#
# 依赖：Clang（自带 libFuzzer）。目标环境为 WSL/Linux/macOS 或
#       支持 -fsanitize=fuzzer 的 clang-cl。MSVC 无 libFuzzer，
#       Windows 上请改用 CTest 的 standalone 回放（见 README §MSVC 回放）。
#
# 可配置环境变量：
#   CXX            编译器（默认 clang++）
#   SANITIZERS     sanitizer 列表（默认 fuzzer,address,undefined）
#   BUILD_TYPE     opt 级别（默认 -O1；设 debug 用 -O0）
#
# 纯新增工具脚本，不修改编译器实现（仅编译既有源文件）。
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CXX="${CXX:-clang++}"
SANITIZERS="${SANITIZERS:-fuzzer,address,undefined}"
BUILD_TYPE="${BUILD_TYPE:-release}"
OUT_BIN="${SCRIPT_DIR}/fuzz_parser"

# ---- 前置检查：Clang 是否可用 ----
if ! command -v "${CXX}" >/dev/null 2>&1; then
    echo "[build] 错误：找不到编译器 '${CXX}'。libFuzzer 需要 Clang。" >&2
    echo "        Ubuntu/WSL: sudo apt-get install clang" >&2
    echo "        或设置 CXX=/path/to/clang++ 后重试。" >&2
    exit 1
fi
if ! "${CXX}" --version 2>/dev/null | grep -qi clang; then
    echo "[build] 警告：'${CXX}' 似乎不是 Clang；-fsanitize=fuzzer 可能不被支持。" >&2
fi

# ---- 前端最小链接闭包（6 个源文件，见 README §链接闭包）+ fuzz target ----
# 这些是既有的编译器源文件，此处仅编译、不修改。
SOURCES=(
    "${REPO_ROOT}/lexer/Lexer.cpp"
    "${REPO_ROOT}/parser/Parser.cpp"
    "${REPO_ROOT}/ast/ASTNode.cpp"
    "${REPO_ROOT}/ast/MacroExpander.cpp"
    "${REPO_ROOT}/interpreter/Value.cpp"
    "${REPO_ROOT}/interpreter/GcManager.cpp"
    "${SCRIPT_DIR}/../fuzz_parser.cpp"
)

OPT_FLAGS="-O1"
if [[ "${BUILD_TYPE}" == "debug" ]]; then
    OPT_FLAGS="-O0"
fi

echo "[build] 编译器: ${CXX}"
echo "[build] sanitizers: ${SANITIZERS}"
echo "[build] 源文件: ${#SOURCES[@]} 个（6 前端闭包 + 1 fuzz target）"
echo "[build] 输出: ${OUT_BIN}"

# 单次编译+链接：全部源文件带插桩，保证覆盖率反馈作用于前端全路径。
# -fno-sanitize-recover=all：UBSan 命中即硬失败（libFuzzer 记为 crash）。
# -fno-omit-frame-pointer：保留清晰栈回溯。
# include 目录：-I<root> 解析 "lexer/Lexer.h" 等根相对路径；
#              -I<root>/common 解析 Lexer.h/Parser.h 里的裸 "Diagnostic.h"。
# -include common/pch.h：镜像主构建的 PCH 强制包含（纯标准库头），
#              保证这 6 个源文件看到与主构建一致的标准库可见性。
set -x
"${CXX}" \
    -std=c++20 \
    ${OPT_FLAGS} -g \
    -fno-omit-frame-pointer \
    -fsanitize="${SANITIZERS}" \
    -fsanitize-address-use-after-scope \
    -fno-sanitize-recover=all \
    -I"${REPO_ROOT}" \
    -I"${REPO_ROOT}/common" \
    -include "${REPO_ROOT}/common/pch.h" \
    "${SOURCES[@]}" \
    -o "${OUT_BIN}"
set +x

echo "[build] 完成：${OUT_BIN}"
