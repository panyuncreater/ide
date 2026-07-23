#!/usr/bin/env python3
# ============================================================
# MiniLang CI 性能基准基线管理工具
# ------------------------------------------------------------
# 用途：
#   1. 运行 minilang_perf_test，捕获 JSON 输出
#   2. 与基线 perf-baseline.json 对比，回归超阈值则失败
#   3. 支持自动更新基线（首次运行 / 硬件升级后使用）
#
# 用法：
#   # 检查模式（CI 使用）：回归超阈值则退出码 1
#   python scripts/perf_baseline.py check \
#       --baseline perf-baseline.json \
#       --binary ./out/build/linux-release/tests/minilang_perf_test
#
#   # 更新模式（手动）：用当前测量值覆盖基线
#   python scripts/perf_baseline.py update \
#       --baseline perf-baseline.json \
#       --binary ./out/build/linux-release/tests/minilang_perf_test
#
#   # 直接对比已有结果（无二进制）
#   python scripts/perf_baseline.py check \
#       --baseline perf-baseline.json --result results.json
#
# 设计原则：
#   - 仅对 elapsed_ms 进行回归判定（output 仅作辅助输出）
#   - 阈值默认 2.0x（当前耗时 > 基线 × 阈值 = 失败）
#   - 基线 elapsed_ms = null 表示尚未建立，跳过判定
#   - 失败时不立即退出，全部对比完后汇总输出便于定位
# ============================================================

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from typing import Any


# 单个基准结果
@dataclass
class BenchItem:
    backend: str
    benchmark: str
    elapsed_ms: float
    output: str
    category: str = "uncategorized"  # R117 v2 新增，v1 兼容为 uncategorized


# ------------------------------------------------------------
# JSON 提取：minilang_perf_test 的 stdout 中混合了 gtest 输出与本工具
# 关心的 JSON 块（由 emitJsonResults 输出）。提取策略：
#   1. 找到第一个以 "{" 开头的行
#   2. 用括号配平算法找到对应的 "}"
#   3. 解析这段 JSON
# ------------------------------------------------------------
def extract_json_from_output(stdout: str) -> dict[str, Any]:
    lines = stdout.splitlines()
    start_idx = -1
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("{"):
            start_idx = i
            break
    if start_idx < 0:
        raise RuntimeError("stdout 中未找到 JSON 起始 '{'")

    # 括号配平（忽略字符串内的括号 —— JSON 输出已转义，简化处理）
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
    text = "".join(collected)
    return json.loads(text)


def parse_bench_items(payload: dict[str, Any]) -> list[BenchItem]:
    items: list[BenchItem] = []
    for raw in payload.get("benchmark_results", []):
        items.append(
            BenchItem(
                backend=str(raw.get("backend", "")),
                benchmark=str(raw.get("benchmark", "")),
                elapsed_ms=float(raw.get("elapsed_ms", -1.0)),
                output=str(raw.get("output", "")),
                # R117 v2 兼容：category 字段缺失时回退为 "uncategorized"
                category=str(raw.get("category", "uncategorized")),
            )
        )
    return items


# ------------------------------------------------------------
# 运行 minilang_perf_test 并捕获 JSON 输出
# ------------------------------------------------------------
def run_perf_binary(binary: str) -> list[BenchItem]:
    # --gtest_filter 限制为 PerfBenchmark.* —— 但 ZZZ_PrintResults 也属于此 suite
    # --gtest_brief=1 减少失败输出噪音（perf 测试不会失败，仅保险）
    cmd = [binary, "--gtest_brief=1", "--gtest_filter=PerfBenchmark.*"]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        # gtest 在某些环境下会因环境变量缺失返回非零，但只要 stdout 有 JSON 即视为成功
        sys.stderr.write(f"[WARN] perf binary 返回码 {proc.returncode}\n")
    payload = extract_json_from_output(proc.stdout)
    return parse_bench_items(payload)


# ------------------------------------------------------------
# 对比当前结果与基线
# ------------------------------------------------------------
@dataclass
class ComparisonRow:
    name: str
    baseline_ms: float | None
    current_ms: float
    ratio: float | None  # current / baseline
    regressed: bool
    note: str
    category: str = "uncategorized"  # R117 v2 新增


def compare(
    items: list[BenchItem], baseline: dict[str, Any], tolerance: float
) -> list[ComparisonRow]:
    baseline_map: dict[str, dict[str, Any]] = {}
    for entry in baseline.get("benchmarks", []):
        key = f"{entry['backend']}::{entry['benchmark']}"
        baseline_map[key] = entry

    rows: list[ComparisonRow] = []
    for it in items:
        key = f"{it.backend}::{it.benchmark}"
        b = baseline_map.get(key)
        baseline_ms = b.get("elapsed_ms") if b else None
        # R117 v2：从基线条目读取 category（若存在），否则用当前结果的 category
        category = str(b.get("category", it.category)) if b else it.category
        if baseline_ms is None or baseline_ms <= 0:
            rows.append(
                ComparisonRow(
                    name=key,
                    baseline_ms=baseline_ms,
                    current_ms=it.elapsed_ms,
                    ratio=None,
                    regressed=False,
                    note="基线未建立，跳过判定",
                    category=category,
                )
            )
        else:
            ratio = it.elapsed_ms / baseline_ms
            regressed = ratio > tolerance
            note = (
                f"回归 {ratio:.2f}x > 阈值 {tolerance:.2f}x"
                if regressed
                else f"正常 ({ratio:.2f}x)"
            )
            rows.append(
                ComparisonRow(
                    name=key,
                    baseline_ms=baseline_ms,
                    current_ms=it.elapsed_ms,
                    ratio=ratio,
                    regressed=regressed,
                    note=note,
                    category=category,
                )
            )
    return rows


def print_rows(rows: list[ComparisonRow]) -> None:
    # R117 v2：新增 Category 列
    print(f"{'Backend::Benchmark':<40} {'Category':<12} {'Baseline(ms)':>14} "
          f"{'Current(ms)':>14} {'Ratio':>8} {'Status':>10}")
    print("-" * 102)
    for r in rows:
        base_str = f"{r.baseline_ms:.2f}" if r.baseline_ms else "N/A"
        ratio_str = f"{r.ratio:.2f}x" if r.ratio else "N/A"
        status = "REGRESSED" if r.regressed else "OK"
        print(f"{r.name:<40} {r.category:<12} {base_str:>14} {r.current_ms:>14.2f} "
              f"{ratio_str:>8} {status:>10}")
        print(f"    {r.note}")
    print("-" * 102)


# ------------------------------------------------------------
# 子命令：check / update
# ------------------------------------------------------------
def cmd_check(args: argparse.Namespace) -> int:
    with open(args.baseline, "r", encoding="utf-8") as f:
        baseline = json.load(f)
    tolerance = float(baseline.get("tolerance", args.tolerance))

    if args.result:
        with open(args.result, "r", encoding="utf-8") as f:
            payload = json.load(f)
        items = parse_bench_items(payload)
    else:
        items = run_perf_binary(args.binary)

    rows = compare(items, baseline, tolerance)
    print_rows(rows)

    regressed = [r for r in rows if r.regressed]
    if regressed:
        print(f"\n[FAIL] {len(regressed)} 项基准回归超阈值")
        for r in regressed:
            print(f"  - {r.name}: {r.ratio:.2f}x (基线 {r.baseline_ms:.2f}ms, "
                  f"当前 {r.current_ms:.2f}ms)")
        print("\n如属合理性能变化，请用 update 模式刷新基线：")
        print(f"  python scripts/perf_baseline.py update --baseline {args.baseline} "
              f"--binary {args.binary}")
        return 1
    print(f"\n[PASS] 全部 {len(rows)} 项基准未触发回归阈值")
    return 0


def cmd_update(args: argparse.Namespace) -> int:
    items = run_perf_binary(args.binary)
    # 保留旧基线的 tolerance 字段，若无则用默认值
    try:
        with open(args.baseline, "r", encoding="utf-8") as f:
            old = json.load(f)
        tolerance = float(old.get("tolerance", args.tolerance))
    except FileNotFoundError:
        tolerance = args.tolerance

    # R117 v2：输出含 category 字段；version 升级为 2
    payload = {
        "version": 2,
        "tolerance": tolerance,
        "description": "MiniLang CI 性能基线（由 scripts/perf_baseline.py update 生成）",
        "benchmarks": [
            {
                "backend": it.backend,
                "benchmark": it.benchmark,
                "category": it.category,
                "elapsed_ms": round(it.elapsed_ms, 3),
                "output": it.output,
            }
            for it in items
        ],
    }
    with open(args.baseline, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"[OK] 基线已写入 {args.baseline}（{len(items)} 项，v2 schema）")
    return 0


# ------------------------------------------------------------
# 入口
# ------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser(
        description="MiniLang CI 性能基准基线管理工具"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_check = sub.add_parser("check", help="运行 perf 测试并对比基线")
    p_check.add_argument("--baseline", required=True, help="基线 JSON 文件路径")
    p_check.add_argument("--binary", help="minilang_perf_test 可执行文件路径")
    p_check.add_argument("--result", help="已有的 perf 测试结果 JSON（替代 --binary）")
    p_check.add_argument(
        "--tolerance", type=float, default=2.0,
        help="回归容忍倍数（默认 2.0x，可被基线文件中的 tolerance 字段覆盖）",
    )
    p_check.set_defaults(func=cmd_check)

    p_update = sub.add_parser("update", help="运行 perf 测试并刷新基线")
    p_update.add_argument("--baseline", required=True, help="基线 JSON 文件路径")
    p_update.add_argument("--binary", required=True, help="minilang_perf_test 可执行文件路径")
    p_update.add_argument("--tolerance", type=float, default=2.0, help="回归容忍倍数")
    p_update.set_defaults(func=cmd_update)

    args = parser.parse_args()
    if args.command == "check" and not args.binary and not args.result:
        parser.error("check 子命令需要 --binary 或 --result")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
