#!/usr/bin/env python3
# ============================================================
# perf_to_benchmark_action.py — 转换 perf 输出为 benchmark-action 格式
# ------------------------------------------------------------
# 用途：
#   把 minilang_perf_test 的 JSON 输出转换为 benchmark-action
#   （github.com/benchmark-action/github-action-benchmark）兼容的格式，
#   以便在 gh-pages 上自动生成性能趋势图。
#
# benchmark-action 期望的输入格式（CustomBenchmark）：
#   JSON 数组，每个元素字段：
#     - name:  基准名（如 "Interpreter::fibonacci"）
#     - unit:  单位（如 "ms"）
#     - value: 测量值（数值）
#     - extra: 可选，附加字段（如 category）
#
# 输入示例（perf-baseline.json schema）：
#   {
#     "benchmark_results": [
#       {"backend": "Interpreter", "benchmark": "fibonacci",
#        "category": "recursion", "elapsed_ms": 123.45, "output": "46368"},
#       ...
#     ]
#   }
#
# 输出示例（JSON 数组）：
#   [
#     {"name":"Interpreter::fibonacci","unit":"ms","value":123.45,"extra":{...}},
#     ...
#   ]
#
# 注：benchmark-action 要求 JSON 数组（JSONL 会报 "must be JSON file
# containing an array of entries"），2026-08-01 CI 实测确认。
# 用法：
#   python scripts/perf_to_benchmark_action.py \
#       --input perf-results.json \
#       --output perf-benchmark-action.json
#
#   # 直接从 stdin 读取（管道）
#   ./minilang_perf_test | python scripts/perf_to_benchmark_action.py --output out.json
# ============================================================

from __future__ import annotations

import argparse
import json
import re
import sys
from typing import Any


def extract_perf_json(stdout: str) -> dict[str, Any]:
    """
    从 minilang_perf_test 的 stdout 中提取 perf JSON 块。
    stdout 混合了 gtest 输出与 JSON 块（由 emitJsonResults 输出）。
    策略：找第一个 '{' 行，用括号配平找配对的 '}'。
    """
    lines = stdout.splitlines()
    start_idx = -1
    for i, line in enumerate(lines):
        if line.strip().startswith("{"):
            start_idx = i
            break
    if start_idx < 0:
        raise RuntimeError("stdout 中未找到 JSON 起始 '{'")

    depth = 0
    in_string = False
    collected: list[str] = []
    for i in range(start_idx, len(lines)):
        line = lines[i]
        for ch in line:
            if ch == '"' and (not collected or collected[-1][-1] != "\\"):
                in_string = not in_string
            if not in_string:
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
            collected.append(ch)
        collected.append("\n")
        if depth == 0:
            break
    if depth != 0:
        raise RuntimeError("JSON 括号未配平")
    return json.loads("".join(collected))


def convert_to_benchmark_action(payload: dict[str, Any]) -> list[dict[str, Any]]:
    """
    把 perf JSON 转换为 benchmark-action 兼容的 JSON 对象列表。
    每个基准 × 后端组合一个对象，name 格式为 "<Backend>::<benchmark>"。
    """
    output: list[dict[str, Any]] = []
    for raw in payload.get("benchmark_results", []):
        backend = str(raw.get("backend", ""))
        benchmark = str(raw.get("benchmark", ""))
        category = str(raw.get("category", "uncategorized"))
        elapsed_ms = float(raw.get("elapsed_ms", -1.0))
        result_output = str(raw.get("output", ""))

        if elapsed_ms < 0:
            # 跳过无效结果（如 parse-error）
            continue

        entry = {
            "name": f"{backend}::{benchmark}",
            "unit": "ms",
            "value": elapsed_ms,
            "extra": {
                "category": category,
                "backend": backend,
                "benchmark": benchmark,
                "output": result_output,
            },
        }
        output.append(entry)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(
        description="把 minilang_perf_test JSON 输出转换为 benchmark-action 兼容格式"
    )
    parser.add_argument(
        "--input", "-i",
        help="输入 JSON 文件路径（默认从 stdin 读取）",
    )
    parser.add_argument(
        "--output", "-o",
        required=True,
        help="输出 JSON 文件路径（JSON 数组，benchmark-action 兼容）",
    )
    args = parser.parse_args()

    # 读取输入
    if args.input:
        with open(args.input, "r", encoding="utf-8") as f:
            content = f.read()
    else:
        content = sys.stdin.read()

    # 提取并转换
    payload = extract_perf_json(content)
    entries = convert_to_benchmark_action(payload)

    # 写入输出（JSON 数组，benchmark-action 期望的格式）
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(entries, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print(f"[OK] 已转换 {len(entries)} 项基准结果 → {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
