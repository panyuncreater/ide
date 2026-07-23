#!/usr/bin/env bash
# ============================================================
# MiniLang git pre-commit hook（bash 版，Linux / macOS / Git Bash）
# ------------------------------------------------------------
# 安装：
#   cp scripts/pre-commit-hook.sh .git/hooks/pre-commit
#   chmod +x .git/hooks/pre-commit
#
# 功能：
#   1. 仅检查本次暂存（staged）的 .cpp / .h 文件
#   2. 用项目固定版本 clang-format 22.1.5 检查格式
#   3. 不修改文件，发现违规时退出码 1 阻止提交
#
# 跳过：
#   git commit --no-verify
# ============================================================

set -euo pipefail

# --- 定位项目根（hooks 软链 / 模板安装可能在任意目录）---
HOOK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -d "$HOOK_DIR/../../scripts" ]; then
    # 通过 .git/hooks/pre-commit 软链调用
    PROJECT_ROOT="$(cd "$HOOK_DIR/../.." && pwd)"
else
    # 直接从 scripts/pre-commit-hook.sh 复制到 .git/hooks/pre-commit
    PROJECT_ROOT="$(cd "$HOOK_DIR" && pwd)"
fi
cd "$PROJECT_ROOT"

# --- 收集暂存的 .cpp / .h 文件 ---
STAGED_FILES=$(git diff --cached --name-only --diff-filter=ACM -- 'lexer/*.cpp' 'lexer/*.h' \
    'parser/*.cpp' 'parser/*.h' 'ast/*.cpp' 'ast/*.h' \
    'interpreter/*.cpp' 'interpreter/*.h' \
    'compiler/*.cpp' 'compiler/*.h' \
    'debug/*.cpp' 'debug/*.h' \
    'formatter/*.cpp' 'formatter/*.h' \
    'common/*.cpp' 'common/*.h' \
    'gui/*.cpp' 'gui/*.h' \
    'app/*.cpp' 'app/*.h' \
    'cli/*.cpp' 'cli/*.h' \
    'tests/*.cpp' 'tests/*.h' \
    'test_harness/*.cpp' 'test_harness/*.h' \
    | grep -E '\.(cpp|h)$' || true)

if [ -z "$STAGED_FILES" ]; then
    echo "[pre-commit] 无 .cpp/.h 文件需要检查，跳过"
    exit 0
fi

# --- 定位 clang-format ---
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" &>/dev/null; then
    echo "[pre-commit][WARN] 未找到 clang-format，跳过格式检查"
    echo "  安装方式：pip install clang-format==22.1.5"
    exit 0
fi

# --- 检查版本（推荐 22.1.5）---
VERSION=$("$CLANG_FORMAT" --version 2>/dev/null | head -1 || echo "unknown")
echo "[pre-commit] 使用 $CLANG_FORMAT ($VERSION)"

# --- 逐文件检查 ---
FAILED=0
for f in $STAGED_FILES; do
    if [ ! -f "$f" ]; then
        continue
    fi
    DIFF=$("$CLANG_FORMAT" --dry-run --Werror "$f" 2>&1 || true)
    if [ -n "$DIFF" ]; then
        echo "[pre-commit][FAIL] $f 需要格式化："
        echo "$DIFF"
        FAILED=1
    fi
done

if [ "$FAILED" -ne 0 ]; then
    echo ""
    echo "[pre-commit] 提交被阻止：上述文件未通过 clang-format 检查"
    echo "  修复方式：clang-format -i <file...>"
    echo "  跳过本次：git commit --no-verify"
    exit 1
fi

echo "[pre-commit] 全部通过"
exit 0
