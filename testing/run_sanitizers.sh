#!/usr/bin/env bash
# ============================================================
# testing/run_sanitizers.sh — 阶段三 Sanitizer 检测矩阵一键脚本
# ------------------------------------------------------------
# 目标环境：Linux + GCC/Clang（ASan + UBSan 全支持）。Windows/MSVC 仅 ASan，
# 见 testing/README.md 阶段三（用 windows-msvc-asan 预设手动构建）。
#
# 行为：
#   1. 用 linux-gcc-asan 预设（MINILANG_SANITIZE=both）+ 流水线开关配置构建
#   2. 构建 sanitizer 版 mini_diff_runner
#   3. 以 --sanitized 跑 N 个随机种子，ASan/UBSan 报告自动归类落盘 + 报告
#
# 用法: testing/run_sanitizers.sh [N=200] [SEED=1] [STATEMENTS=14]
# ============================================================
set -euo pipefail

N="${1:-200}"
SEED="${2:-1}"
STATEMENTS="${3:-14}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESET="linux-gcc-asan"
BUILD="$ROOT/out/build/linux-asan"

echo "[run_sanitizers] 配置 sanitizer 构建（预设 $PRESET，MINILANG_SANITIZE=both）..."
cmake --preset "$PRESET" -DMINILANG_BUILD_TESTING_PIPELINE=ON

echo "[run_sanitizers] 构建 mini_diff_runner（sanitizer 版）..."
cmake --build "$BUILD" --target mini_diff_runner

RUNNER="$BUILD/testing/mini_diff_runner"
if [[ ! -x "$RUNNER" ]]; then
    echo "错误：未找到 sanitizer runner: $RUNNER" >&2
    exit 1
fi
export MINI_DIFF_RUNNER_SAN="$RUNNER"

echo "[run_sanitizers] 以 --sanitized 跑 $N 个种子（起始 seed=$SEED, statements=$STATEMENTS）..."
python3 "$ROOT/testing/diff_test.py" --sanitized --n "$N" --seed "$SEED" \
    --statements "$STATEMENTS" --no-fail \
    --san-runner "$RUNNER" --out-dir "$ROOT/testing/artifacts"

echo "[run_sanitizers] 完成。报告：$ROOT/testing/artifacts/report.md"
echo "[run_sanitizers] Sanitizer 命中用例：$ROOT/testing/artifacts/sanitizer/"
