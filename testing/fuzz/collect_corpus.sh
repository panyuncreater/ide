#!/usr/bin/env bash
# ============================================================
# collect_corpus.sh — 收集仓库示例源程序为 libFuzzer 初始种子语料
# ------------------------------------------------------------
# 把 <repo>/samples/ 下全部 *.mini 示例源程序复制进 testing/fuzz/corpus/，
# 文件名按相对路径扁平化（/ 与 \ 替换为 __）避免同名冲突。
# 幂等：重复运行只覆盖同名种子，不产生重复。
#
# 用法：
#   ./collect_corpus.sh              # 收集到默认 corpus/
#   ./collect_corpus.sh <目标目录>   # 收集到指定目录
#
# 纯新增工具脚本，不修改编译器实现。
# ============================================================
set -euo pipefail

# 脚本所在目录（testing/fuzz） → 仓库根（上两级）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SAMPLES_DIR="${REPO_ROOT}/samples"
CORPUS_DIR="${1:-${SCRIPT_DIR}/corpus}"

mkdir -p "${CORPUS_DIR}"

if [[ ! -d "${SAMPLES_DIR}" ]]; then
    echo "[collect] 错误：找不到示例目录 ${SAMPLES_DIR}" >&2
    exit 1
fi

count=0
# 收集全部 *.mini（示例源程序）。-print0 兼容含空格/特殊字符的路径。
while IFS= read -r -d '' f; do
    rel="${f#"${SAMPLES_DIR}/"}"           # 相对 samples/ 的路径
    flat="samples__${rel//\//__}"          # 扁平化：/ → __
    flat="${flat//\\/__}"                  # 兼容反斜杠
    cp -f "${f}" "${CORPUS_DIR}/${flat}"
    count=$((count + 1))
done < <(find "${SAMPLES_DIR}" -type f -name '*.mini' -print0)

echo "[collect] 已收集 ${count} 个示例源程序到 ${CORPUS_DIR}"
