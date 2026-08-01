#!/usr/bin/env bash
# ============================================================
# run_fuzz.sh — 运行前端 libFuzzer（默认 5 分钟）+ crash 自动归档
# ------------------------------------------------------------
# 流程：
#   1. 若 fuzz_parser 未构建 → 调用 build_fuzz.sh 构建；
#   2. 若 corpus/ 为空 → 调用 collect_corpus.sh 收集示例种子；
#   3. 用 corpus/ 播种运行语料 corpus_work/（保持 corpus/ 种子纯净）；
#   4. 运行 libFuzzer（默认 max_total_time=300s，即 5 分钟）；
#   5. 结束后把 crash-/leak-/timeout-/oom- 用例连同日志归档到
#      crashes/archive_<时间戳>/，并打印复现命令。
#
# 用法：
#   ./run_fuzz.sh                 # 默认跑 5 分钟
#   ./run_fuzz.sh -t 60           # 跑 60 秒
#   ./run_fuzz.sh --rebuild       # 强制重新构建后再跑
#   ./run_fuzz.sh -- -jobs=4      # -- 之后的参数原样透传给 libFuzzer
#
# 退出码：透传 libFuzzer 退出码（发现 crash 时非 0，便于 CI 判定）。
# 纯新增工具脚本，不修改编译器实现。
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/fuzz_parser"
CORPUS_SEED="${SCRIPT_DIR}/corpus"
CORPUS_WORK="${SCRIPT_DIR}/corpus_work"
CRASH_DIR="${SCRIPT_DIR}/crashes"
DICT="${SCRIPT_DIR}/fuzz_parser.dict"

MAX_TIME=300           # 默认 5 分钟
REBUILD=0
declare -a PASSTHRU=() # -- 之后原样透传给 libFuzzer 的参数

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--time)   MAX_TIME="$2"; shift 2 ;;
        --rebuild)   REBUILD=1; shift ;;
        --)          shift; PASSTHRU=("$@"); break ;;
        -h|--help)
            sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "[run] 未知参数：$1（-- 之后的参数会透传 libFuzzer）" >&2; exit 2 ;;
    esac
done

# ---- 1. 构建 ----
if [[ "${REBUILD}" -eq 1 || ! -x "${BIN}" ]]; then
    echo "[run] 构建 fuzz target..."
    bash "${SCRIPT_DIR}/build_fuzz.sh"
fi

# ---- 2. 种子语料 ----
mkdir -p "${CORPUS_SEED}"
if ! find "${CORPUS_SEED}" -type f -print -quit | grep -q .; then
    echo "[run] corpus/ 为空，收集示例种子..."
    bash "${SCRIPT_DIR}/collect_corpus.sh" "${CORPUS_SEED}"
fi

# ---- 3. 播种运行语料（不污染 corpus/ 种子）----
mkdir -p "${CORPUS_WORK}" "${CRASH_DIR}"
cp -n "${CORPUS_SEED}"/* "${CORPUS_WORK}/" 2>/dev/null || true

# ---- 4. 运行 libFuzzer ----
LOG="${SCRIPT_DIR}/last_run.log"
echo "[run] 开始 fuzzing：max_total_time=${MAX_TIME}s，语料=${CORPUS_WORK}"
echo "[run] 日志：${LOG}"

# artifact_prefix 必须以 / 结尾且目录存在，crash 用例落到 crashes/。
# set +e：libFuzzer 命中 crash 会以非 0 退出，需先归档再退出。
set +e
"${BIN}" \
    "${CORPUS_WORK}" \
    -artifact_prefix="${CRASH_DIR}/" \
    -dict="${DICT}" \
    -max_total_time="${MAX_TIME}" \
    -max_len=65536 \
    -timeout=10 \
    -rss_limit_mb=2048 \
    -print_final_stats=1 \
    "${PASSTHRU[@]}" 2>&1 | tee "${LOG}"
FUZZ_RC="${PIPESTATUS[0]}"
set -e

# ---- 5. 归档 crash 用例 ----
shopt -s nullglob
ARTIFACTS=("${CRASH_DIR}"/crash-* "${CRASH_DIR}"/leak-* "${CRASH_DIR}"/timeout-* "${CRASH_DIR}"/oom-*)
shopt -u nullglob

if [[ ${#ARTIFACTS[@]} -gt 0 ]]; then
    TS="$(date +%Y%m%d_%H%M%S)"
    ARCHIVE="${CRASH_DIR}/archive_${TS}"
    mkdir -p "${ARCHIVE}"
    for a in "${ARTIFACTS[@]}"; do
        mv "${a}" "${ARCHIVE}/"
    done
    cp "${LOG}" "${ARCHIVE}/fuzz.log" 2>/dev/null || true
    echo ""
    echo "[run] ⚠ 发现 ${#ARTIFACTS[@]} 个 crash/leak/timeout 用例，已归档到："
    echo "      ${ARCHIVE}"
    echo "[run] 复现单个用例（示例）："
    echo "      ${BIN} ${ARCHIVE}/$(basename "${ARTIFACTS[0]}")"
else
    echo ""
    echo "[run] ✓ 未发现 crash（前端在 ${MAX_TIME}s 内对所有变异输入保持不崩溃）。"
fi

echo "[run] libFuzzer 退出码：${FUZZ_RC}"
exit "${FUZZ_RC}"
