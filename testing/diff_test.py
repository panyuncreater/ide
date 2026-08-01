#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""testing/diff_test.py — 四后端差分测试核心（阶段二）

对「生成程序」或「给定文件」做四后端差分投票，命中案例落盘 + 汇总 Markdown 报告。

oracle（无外部参考编译器）：
  MiniLang 为自研语言，GCC/Clang 无法编译，故不做「五方投票（含参考编译器）」。
  改用项目核心不变量作 oracle —— 四条 strict 执行后端
  （interp / stackvm / stackvm-ir / regvm）语义必须严格一致；
  JIT 为可选加速器，不支持场景优雅降级（不崩溃不错值），仅在跑完却错值/崩溃时判负。
  分类由 driver.diff_file 给出：agree / disagreement / jit_deviation / crash / hang / all_error。

模式：
  单文件： --file X.mini
  批量：   --n 1000 [--seed BASE]   连续跑 N 个种子，遇错继续，末尾输出分类统计

命中落盘：testing/artifacts/<category>/seed_<seed>.{mini,json}
汇总报告：testing/artifacts/report.md

无第三方依赖，兼容 Python 3.8+。
"""

import argparse
import json
import re
import shutil
import sys
import time
from pathlib import Path

# 使 driver / generator 可被导入（与本文件同目录）
sys.path.insert(0, str(Path(__file__).resolve().parent))
import driver          # noqa: E402
import generator       # noqa: E402

ARTIFACTS = Path(__file__).resolve().parent / "artifacts"

# 内部分类 → 面向报告的中文类目
CATEGORY_ZH = {
    "agree": "通过",
    "disagreement": "输出不一致",
    "jit_deviation": "JIT 偏差",
    "crash": "崩溃",
    "hang": "超时",
    "all_error": "全部错误",
}
FINDING_CATEGORIES = ("disagreement", "jit_deviation", "crash", "hang", "all_error")


def _short(s, n=60):
    s = s.replace("\n", "\\n")
    return s if len(s) <= n else s[:n] + "…"


def _divergence_phase(result):
    """判断 disagreement 是编译期还是运行期分歧（用于报告细分）。"""
    statuses = {r["backend"]: r.get("status") for r in result.get("results", [])}
    if any(s in ("compile_error", "parse_error") for s in statuses.values()):
        return "compile"
    return "runtime"


def _suspect_error(result):
    """取疑似后端的错误文本（无则取任一非空错误）。"""
    suspects = set(result.get("suspects", []))
    for r in result.get("results", []):
        if r["backend"] in suspects and r.get("error"):
            return r["error"]
    for r in result.get("results", []):
        if r.get("error"):
            return r["error"]
    return ""


def _normalize_error(msg):
    """错误文本归一化（数字/引号内容去具体值），作为签名的一部分。"""
    m = re.sub(r"\d+", "#", msg or "")
    m = re.sub(r"['\"][^'\"]*['\"]", "'…'", m)
    return m.strip()[:80]


def finding_signature(result, category, phase):
    """将一个命中归约为签名，用于去重（同根因的大量命中合并为一条）。"""
    suspects = tuple(sorted(result.get("suspects", [])))
    return (category, phase, suspects, _normalize_error(_suspect_error(result)))


def run_source(seed, source, args, work_dir):
    """把一段源码写入临时文件并做四后端差分。"""
    work_dir.mkdir(parents=True, exist_ok=True)
    src_path = work_dir / f"seed_{seed}.mini"
    src_path.write_text(source, encoding="utf-8", newline="\n")
    result = driver.diff_file(
        src_path, backends=args.backends, runner=args.runner,
        compile_timeout=args.compile_timeout, run_timeout=args.run_timeout,
        qt_bin=args.qt_bin, sanitized=args.sanitized, san_runner=args.san_runner)
    result["seed"] = seed
    result["source"] = source
    return result


def record_finding(result, category, out_root):
    """命中案例落盘：源码 + 完整差分结果（含各后端输出与复现命令）。"""
    cat_dir = out_root / category
    cat_dir.mkdir(parents=True, exist_ok=True)
    seed = result.get("seed", "file")
    stem = f"seed_{seed}"
    (cat_dir / f"{stem}.mini").write_text(result.get("source", ""), encoding="utf-8", newline="\n")
    # 复现命令（便于人工重跑单后端）
    repro = {
        "file": result.get("file"),
        "category": category,
        "suspects": result.get("suspects", []),
        "notes": result.get("notes", []),
        "per_backend": [
            {"backend": r["backend"], "status": r.get("status"),
             "diff_hash": r.get("diff_hash"), "error": r.get("error", ""),
             "output_len": r.get("output_len", 0),
             "sanitizer": r.get("sanitizer", []),
             "stderr": (r.get("raw_stderr", "") or "")[:2000]}
            for r in result.get("results", [])
        ],
        "repro_cmd": f"python testing/diff_test.py --file {cat_dir.name}/{stem}.mini",
    }
    (cat_dir / f"{stem}.json").write_text(
        json.dumps(repro, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n")
    return cat_dir / f"{stem}.mini"


def make_config(args):
    return generator.Config(
        num_globals=args.globals,
        num_statements=args.statements,
        max_depth=args.max_depth,
        max_loop_iters=args.max_loop_iters,
        enable_loops=not args.no_loops,
        enable_functions=not args.no_functions,
        enable_arrays=not args.no_arrays,
        enable_dicts=not args.no_dicts,
        enable_strings=not args.no_strings,
        enable_closures=not args.no_closures,
        enable_classes=not args.no_classes,
    )


def write_report(stats, findings, signatures, meta, out_root, san_signatures=None):
    """生成 Markdown 汇总报告。"""
    lines = []
    lines.append("# MiniLang 差分测试报告（阶段二）\n")
    lines.append(f"- 生成时间：{meta['time']}")
    lines.append(f"- 模式：{meta['mode']}")
    lines.append(f"- 种子范围：{meta['seed_range']}")
    lines.append(f"- 参与后端：{', '.join(meta['backends'])}")
    lines.append(f"- 生成配置：{meta['config']}")
    lines.append(f"- 总耗时：{meta['elapsed']:.1f}s\n")

    lines.append("## Oracle 说明\n")
    lines.append("MiniLang 为自研语言，GCC/Clang 无法作参考编译器，故不做含外部参考的五方投票；")
    lines.append("改用**四后端交叉差分**（interp/stackvm/stackvm-ir/regvm 必须严格一致）作 oracle，")
    lines.append("JIT 为可选加速器：不支持场景优雅降级（排除投票），仅跑完却错值/崩溃时判负。\n")

    total = sum(stats.values())
    lines.append("## 分类统计\n")
    lines.append("| 类目 | 内部类别 | 数量 | 占比 |")
    lines.append("|------|----------|-----:|-----:|")
    for cat in ("agree", "disagreement", "jit_deviation", "crash", "hang", "all_error"):
        c = stats.get(cat, 0)
        pct = (100.0 * c / total) if total else 0.0
        lines.append(f"| {CATEGORY_ZH[cat]} | {cat} | {c} | {pct:.1f}% |")
    lines.append(f"\n合计 {total} 个用例，命中（非通过）{total - stats.get('agree', 0)} 个。\n")

    if findings:
        # 去重：同签名（类别+分歧期+疑似后端+归一化错误）合并为一条独立问题。
        lines.append("## 去重后的独立问题（按签名归并）\n")
        lines.append("| # | 类别 | 分歧期 | 疑似后端 | 归一化错误 | 命中数 | 代表种子 |")
        lines.append("|--:|------|--------|----------|----------|-----:|--------|")
        ranked = sorted(signatures.values(), key=lambda s: s["count"], reverse=True)
        for i, sig in enumerate(ranked, 1):
            cat, phase, suspects, err = sig["key"]
            lines.append(f"| {i} | {cat} | {phase} | {', '.join(suspects) or '-'} | "
                         f"{_short(err, 44) or '-'} | {sig['count']} | {sig['first_seed']} |")
        lines.append(f"\n→ {len(findings)} 个命中归并为 **{len(signatures)} 个独立问题**。\n")

        lines.append("## 命中用例（前 15 条）\n")
        lines.append("| 种子 | 类别 | 分歧期 | 疑似后端 | 摘要 |")
        lines.append("|------|------|--------|----------|------|")
        for f in findings[:15]:
            phase = f.get("phase", "-")
            suspects = ", ".join(f.get("suspects", [])) or "-"
            note = _short("; ".join(f.get("notes", [])), 70) or "-"
            lines.append(f"| {f['seed']} | {f['category']} | {phase} | {suspects} | {note} |")
        if len(findings) > 15:
            lines.append(f"\n> 其余 {len(findings) - 15} 条见各类别目录。")
        lines.append("\n> 每个命中用例的源码与各后端输出见 "
                     "`testing/artifacts/<类别>/seed_<seed>.{mini,json}`。")
    else:
        lines.append("## 命中用例\n\n无命中：全部用例四后端一致。✅")

    # 阶段三：Sanitizer 命中（ASan/UBSan，每种类型单独一类）
    if san_signatures is not None:
        lines.append("\n## Sanitizer 命中（按 tool:type 归并）\n")
        if san_signatures:
            lines.append("| # | 工具 | 类型 | 后端 | 详情 | 命中数 | 代表种子 |")
            lines.append("|--:|------|------|------|------|-----:|--------|")
            ranked = sorted(san_signatures.values(), key=lambda s: s["count"], reverse=True)
            for i, s in enumerate(ranked, 1):
                lines.append(f"| {i} | {s['tool']} | {s['type']} | {s.get('backend') or '-'} | "
                             f"{_short(s.get('detail', ''), 40) or '-'} | {s['count']} | {s['first_seed']} |")
            lines.append("\n> 命中用例与完整 sanitizer 输出见 `testing/artifacts/sanitizer/seed_<seed>.{mini,json}`（含 raw_stderr）。")
        else:
            lines.append("无 sanitizer 命中（当前种子集未触发编译器/VM 的内存安全/UB 问题）。✅")

    report_path = out_root / "report.md"
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return report_path


def cmd_batch(args):
    cfg = make_config(args)
    out_root = Path(args.out_dir) if args.out_dir else ARTIFACTS
    out_root.mkdir(parents=True, exist_ok=True)
    work_dir = out_root / ".work"
    if work_dir.exists():
        shutil.rmtree(work_dir, ignore_errors=True)

    stats = {c: 0 for c in CATEGORY_ZH}
    findings = []
    signatures = {}
    san_signatures = {}   # 阶段三：sanitizer 命中去重（tool,type,detail）
    san_findings = []
    start = time.time()
    base = args.seed
    print(f"[diff_test] 批量：{args.n} 个种子，起始 seed={base}", file=sys.stderr)

    for i in range(args.n):
        seed = base + i
        source = generator.generate(seed, cfg)
        result = run_source(seed, source, args, work_dir)
        cat = result["category"]
        stats[cat] = stats.get(cat, 0) + 1
        if cat in FINDING_CATEGORIES:
            phase = _divergence_phase(result) if cat == "disagreement" else "-"
            record_finding(result, cat, out_root)
            findings.append({"seed": seed, "category": cat, "phase": phase,
                             "suspects": result.get("suspects", []),
                             "notes": result.get("notes", [])})
            sig = finding_signature(result, cat, phase)
            if sig in signatures:
                signatures[sig]["count"] += 1
            else:
                signatures[sig] = {"key": sig, "count": 1, "first_seed": seed}
            print(f"  [命中] seed={seed} category={cat} phase={phase} "
                  f"suspects={result.get('suspects', [])}", file=sys.stderr)
        # 阶段三：sanitizer 命中与上述分类正交（输出可能一致但内存不安全），单独统计。
        if args.sanitized:
            hits = result.get("sanitizer_hits", [])
            if hits:
                record_finding(result, "sanitizer", out_root)
                for h in hits:
                    key = (h.get("tool"), h.get("type"), h.get("detail", ""))
                    if key in san_signatures:
                        san_signatures[key]["count"] += 1
                    else:
                        san_signatures[key] = {"tool": h.get("tool"), "type": h.get("type"),
                                               "detail": h.get("detail", ""), "count": 1,
                                               "first_seed": seed, "backend": h.get("backend")}
                san_findings.append({"seed": seed, "hits": hits})
                print(f"  [SANITIZER] seed={seed} "
                      f"hits={[(h.get('tool'), h.get('type')) for h in hits]}", file=sys.stderr)
        if (i + 1) % 50 == 0:
            print(f"  ...{i + 1}/{args.n} 已跑，命中 {len(findings)}", file=sys.stderr)

    elapsed = time.time() - start
    shutil.rmtree(work_dir, ignore_errors=True)

    meta = {
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "mode": "batch",
        "seed_range": f"{base}..{base + args.n - 1}",
        "backends": args.backends or driver.ALL_BACKENDS,
        "config": (f"statements={cfg.num_statements}, globals={cfg.num_globals}, "
                   f"depth={cfg.max_depth}, loops={cfg.enable_loops}, "
                   f"funcs={cfg.enable_functions}, arrays={cfg.enable_arrays}"),
        "elapsed": elapsed,
    }
    report = write_report(stats, findings, signatures, meta, out_root,
                          san_signatures if args.sanitized else None)

    print("\n===== 分类统计 =====", file=sys.stderr)
    for cat in ("agree", "disagreement", "jit_deviation", "crash", "hang", "all_error"):
        print(f"  {CATEGORY_ZH[cat]:8s} ({cat}): {stats.get(cat, 0)}", file=sys.stderr)
    if args.sanitized:
        print(f"  Sanitizer 命中用例: {len(san_findings)}（去重后 {len(san_signatures)} 个独立问题）",
              file=sys.stderr)
    print(f"报告：{report}", file=sys.stderr)
    return 1 if ((findings or san_findings) and not args.no_fail) else 0


def cmd_file(args):
    result = driver.diff_file(
        args.file, backends=args.backends, runner=args.runner,
        compile_timeout=args.compile_timeout, run_timeout=args.run_timeout,
        qt_bin=args.qt_bin, sanitized=args.sanitized, san_runner=args.san_runner)
    result["seed"] = Path(args.file).stem
    cat = result["category"]
    san_hits = result.get("sanitizer_hits", [])
    print(json.dumps({
        "file": result["file"], "category": cat,
        "voting_backends": result.get("voting_backends"),
        "suspects": result.get("suspects"), "jit_status": result.get("jit_status"),
        "notes": result.get("notes"), "sanitizer_hits": san_hits,
    }, ensure_ascii=False, indent=2))
    record = args.record and (cat in FINDING_CATEGORIES or san_hits)
    if record:
        out_root = Path(args.out_dir) if args.out_dir else ARTIFACTS
        result["source"] = Path(args.file).read_text(encoding="utf-8", errors="replace")
        p = record_finding(result, "sanitizer" if san_hits else cat, out_root)
        print(f"[已落盘] {p}", file=sys.stderr)
    if san_hits and not args.no_fail:
        return 1
    return 1 if cat in FINDING_CATEGORIES and not args.no_fail else 0


def build_parser():
    p = argparse.ArgumentParser(prog="diff_test.py",
                                description="MiniLang 四后端差分测试核心（阶段二）")
    p.add_argument("--runner", default=None, help="mini_diff_runner 路径（默认自动探测）")
    p.add_argument("--qt-bin", dest="qt_bin", default=None, help="Qt 运行期 DLL 目录")
    p.add_argument("--backends", default=None, type=lambda s: s.split(","),
                   help="逗号分隔的后端子集（默认全部 5 配置）")
    p.add_argument("--compile-timeout", type=float, default=10.0)
    p.add_argument("--run-timeout", type=float, default=10.0)
    p.add_argument("--no-fail", action="store_true", help="即使有命中也返回 0")
    p.add_argument("--sanitized", action="store_true",
                   help="阶段三：用 sanitizer(ASan/UBSan) 构建的 runner 跑全部种子，归类 sanitizer 报告")
    p.add_argument("--san-runner", dest="san_runner", default=None,
                   help="sanitizer 版 mini_diff_runner 路径（默认自动探测 asan 构建目录）")
    p.add_argument("--out-dir", dest="out_dir", default=None,
                   help="产物/报告输出目录（默认 testing/artifacts；CI 可指向构建目录避免污染源树）")

    # 生成配置（批量模式）
    p.add_argument("--n", type=int, default=None, help="批量模式：连续跑 N 个种子")
    p.add_argument("--seed", type=int, default=1, help="批量起始种子（默认 1）")
    p.add_argument("--statements", type=int, default=14)
    p.add_argument("--globals", type=int, default=6)
    p.add_argument("--max-depth", type=int, default=2)
    p.add_argument("--max-loop-iters", type=int, default=10)
    p.add_argument("--no-loops", action="store_true")
    p.add_argument("--no-functions", action="store_true")
    p.add_argument("--no-arrays", action="store_true")
    p.add_argument("--no-dicts", action="store_true")
    p.add_argument("--no-strings", action="store_true")
    p.add_argument("--no-closures", action="store_true")
    p.add_argument("--no-classes", action="store_true")

    # 单文件模式
    p.add_argument("--file", default=None, help="单文件模式：对给定 .mini 做差分")
    p.add_argument("--record", action="store_true", help="单文件命中时也落盘")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.file:
        return cmd_file(args)
    if args.n:
        return cmd_batch(args)
    print("错误：需指定 --file <路径> 或 --n <数量>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
