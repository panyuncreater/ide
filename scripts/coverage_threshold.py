#!/usr/bin/env python3
# ============================================================
# MiniLang 覆盖率阈值检查工具
# ------------------------------------------------------------
# 用途：
#   解析 OpenCppCoverage / gcovr 输出的 Cobertura XML 覆盖率报告，
#   计算行覆盖率与分支覆盖率，若低于阈值则退出码 1。
#
# 用法：
#   python scripts/coverage_threshold.py \
#       --report out/build/debug/coverage.xml \
#       --line-threshold 60 \
#       --branch-threshold 0
#
#   # 仅检查行覆盖率（默认）
#   python scripts/coverage_threshold.py --report coverage.xml --line-threshold 70
#
# 设计原则：
#   - Cobertura XML 是 OpenCppCoverage 和 gcovr 共同支持的格式
#   - 行覆盖率 = 已执行行 / 可执行行（lines-covered / lines-valid）
#   - 分支覆盖率 = 已覆盖分支 / 总分支（branches-covered / branches-valid）
#   - threshold=0 表示不检查该维度
#   - 输出百分比与绝对数值便于 CI 日志定位
# ============================================================

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET


def parse_coverage(report_path: str) -> dict[str, float]:
    """解析 Cobertura XML，返回 line-rate 与 branch-rate（0.0~1.0）。"""
    tree = ET.parse(report_path)
    root = tree.getroot()
    # Cobertura 根 <coverage> 元素直接带 line-rate / branch-rate 属性
    line_rate = float(root.attrib.get("line-rate", "0.0"))
    branch_rate = float(root.attrib.get("branch-rate", "0.0"))

    # 同时收集 lines-covered / lines-valid 用于日志输出
    lines_valid = 0
    lines_covered = 0
    branches_valid = 0
    branches_covered = 0
    for cls in root.iter("class"):
        # <class> 也有 line-rate / branch-rate 属性，但我们要全局聚合
        for line in cls.iter("line"):
            lines_valid += 1
            if int(line.attrib.get("hits", "0")) > 0:
                lines_covered += 1
            # branch 信息（条件分支行）
            bcc = line.attrib.get("branch", "false")
            if bcc == "true":
                cc = line.attrib.get("condition-coverage", "")
                # Cobertura 格式: "100% (2/2)" 或 "50% (1/2)"
                # 提取括号内 m/n 格式
                if "(" in cc and ")" in cc:
                    inner = cc[cc.index("(") + 1: cc.index(")")]
                    parts = inner.split("/")
                    if len(parts) == 2:
                        try:
                            covered, total = int(parts[0]), int(parts[1])
                            branches_covered += covered
                            branches_valid += total
                        except ValueError:
                            pass

    return {
        "line_rate": line_rate,
        "branch_rate": branch_rate,
        "lines_valid": lines_valid,
        "lines_covered": lines_covered,
        "branches_valid": branches_valid,
        "branches_covered": branches_covered,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="MiniLang 覆盖率阈值检查工具（Cobertura XML 解析）"
    )
    parser.add_argument("--report", required=True, help="Cobertura XML 覆盖率报告路径")
    parser.add_argument(
        "--line-threshold", type=float, default=0.0,
        help="行覆盖率阈值（百分比 0-100，默认 0 表示不检查）",
    )
    parser.add_argument(
        "--branch-threshold", type=float, default=0.0,
        help="分支覆盖率阈值（百分比 0-100，默认 0 表示不检查）",
    )
    args = parser.parse_args()

    data = parse_coverage(args.report)
    line_pct = data["line_rate"] * 100.0
    branch_pct = data["branch_rate"] * 100.0

    print("=== MiniLang 覆盖率报告 ===")
    print(f"行覆盖率: {line_pct:.2f}% ({data['lines_covered']}/{data['lines_valid']} 行)")
    print(f"分支覆盖率: {branch_pct:.2f}% "
          f"({data['branches_covered']}/{data['branches_valid']} 分支)")

    failed: list[str] = []
    if args.line_threshold > 0 and line_pct < args.line_threshold:
        failed.append(
            f"行覆盖率 {line_pct:.2f}% 低于阈值 {args.line_threshold:.2f}%"
        )
    if args.branch_threshold > 0 and branch_pct < args.branch_threshold:
        failed.append(
            f"分支覆盖率 {branch_pct:.2f}% 低于阈值 {args.branch_threshold:.2f}%"
        )

    if failed:
        print("\n[FAIL] 覆盖率阈值检查未通过：")
        for msg in failed:
            print(f"  - {msg}")
        return 1

    print(f"\n[PASS] 覆盖率满足阈值要求（行 ≥ {args.line_threshold:.2f}%, "
          f"分支 ≥ {args.branch_threshold:.2f}%）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
