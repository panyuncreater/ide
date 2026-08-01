#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""testing/static-analysis/run_clazy.py — clazy（Qt 专用静态检查）驱动（阶段六）

clazy 是构建在 clang 之上的 Qt 语义检查器（clazy-standalone 消费 compile_commands.json，
与 clang-tidy 用法/诊断格式一致）。本驱动：
  - 定位 clazy-standalone / clazy；
  - 复用 run_clang_tidy 的编译数据库净化（去 MSVC 专有开关；Linux 下为幂等无害）；
  - 以 CLAZY_CHECKS 指定的等级并行分析各 TU；
  - 复用 sa_common 解析/分级/生成 Top 20 报告；clazy 的 `-Wclazy-xxx` 标签归一为 `clazy-xxx`。

clazy 在 Windows/MSVC 下无成熟可用发行版（需匹配 clang 版本自行构建），**推荐在
Linux/WSL 运行**（见同目录 README.md 的安装方式）。若本机未安装，本驱动会输出一份
**如实的前置条件报告**（不伪造 findings），并给出安装与运行命令。

只新增工具代码，不修改编译器实现。无第三方依赖，Python 3.8+。
"""

import argparse
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sa_common          # noqa: E402
import run_clang_tidy     # noqa: E402  复用 sanitize_db / EXCLUDE_SUBSTR

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
DEFAULT_REPORT = HERE / "reports" / "clazy-report.md"
DEFAULT_RAW = HERE / "reports" / "clazy-findings.jsonl"

# CLAZY_CHECKS 推荐等级：level1 = 官方推荐（较少误报，覆盖常见 Qt 反模式）。
# level0 最保守；level2 更激进（更多误报）；level3 极噪声，仅偶尔排查用。
DEFAULT_CHECKS = "level1"


def normalize_clazy_check(check):
    """clazy 诊断标签形如 -Wclazy-foo → 归一为 clazy-foo。"""
    if check.startswith("-Wclazy-"):
        return "clazy-" + check[len("-Wclazy-"):]
    if check.startswith("clazy-"):
        return check
    return check


def run_one(clazy, cache_dir, checks, file, timeout):
    cmd = [clazy, "-p", str(cache_dir), f"-checks={checks}", file]
    env = dict(os.environ)
    env["CLAZY_CHECKS"] = checks
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout,
                              cwd=str(cache_dir), env=env)
    except subprocess.TimeoutExpired:
        return file, "", "timeout"
    except OSError as e:
        return file, f"<os-error: {e}>", "error"
    out = (proc.stdout or b"").decode("utf-8", "replace")
    err = (proc.stderr or b"").decode("utf-8", "replace")
    return file, out + "\n" + err, "ok"


def _prereq_note():
    return (
        "clazy 未在本机找到，因此**未产生真实 findings**（阶段六不伪造结果）。\n"
        "> clazy 依赖 Clang 工具链，推荐在 Linux/WSL 安装后重跑：\n"
        "> - Ubuntu/Debian：`sudo apt-get install -y clazy`（提供 clazy-standalone）\n"
        "> - 然后：在 Linux 侧用 clang 生成 compile_commands.json 的构建目录上运行\n"
        ">   `CLAZY_CHECKS=level1 python testing/static-analysis/run_clazy.py --build-dir <linux-build>`\n"
        "> - 或设 `MINI_CLAZY` 指向 clazy-standalone 可执行文件。\n"
        "> 详见 `testing/static-analysis/README.md`（安装方式 + CLAZY_CHECKS 推荐级别）。")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="run_clazy.py",
                                 description="clazy（Qt 专用）静态分析驱动（阶段六）")
    ap.add_argument("--build-dir", default=str(REPO_ROOT / "build_debug"),
                    help="含 compile_commands.json 的构建目录（默认 build_debug）")
    ap.add_argument("--clazy", default=None, help="clazy-standalone 路径（默认自动探测）")
    ap.add_argument("--checks", default=os.environ.get("CLAZY_CHECKS", DEFAULT_CHECKS),
                    help="CLAZY_CHECKS 等级或清单（默认 level1）")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4)))
    ap.add_argument("--timeout", type=float, default=150.0, help="单 TU 超时秒")
    ap.add_argument("--file-filter", default=None, help="仅分析路径含该子串的 TU")
    ap.add_argument("--limit", type=int, default=None, help="最多分析 N 个 TU")
    ap.add_argument("--out", default=str(DEFAULT_REPORT))
    ap.add_argument("--raw", default=str(DEFAULT_RAW))
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args(argv)

    clazy = args.clazy or sa_common.locate_tool(
        ["clazy-standalone", "clazy"], env_var="MINI_CLAZY",
        extra_dirs=sa_common.default_clang_tidy_dirs())

    # 未找到 clazy：输出如实前置条件报告（不伪造）。
    if not clazy:
        meta = {
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "tool_path": "(未找到 clazy-standalone / clazy)",
            "version": "-", "config": f"CLAZY_CHECKS={args.checks}",
            "scope": "Qt 专用检查（clazy）——本机未安装，见下前置条件",
            "tu_total": 0, "tu_ok": 0, "tu_timeout": 0, "tu_error": 0,
            "elapsed": 0.0, "note": _prereq_note(),
        }
        report = sa_common.write_report(args.out, "clazy", meta, [], 0, top_n=args.top)
        print("clazy 未安装：已输出前置条件报告（不伪造 findings）。", file=sys.stderr)
        print(f"[clazy] 报告：{report}", file=sys.stderr)
        print("安装指引见 testing/static-analysis/README.md（推荐 Linux/WSL）。", file=sys.stderr)
        return 3

    cc_path = Path(args.build_dir) / "compile_commands.json"
    if not cc_path.is_file():
        print(f"错误：找不到 {cc_path}。", file=sys.stderr)
        return 3

    cache_dir = HERE / ".cache-clazy"
    files = run_clang_tidy.sanitize_db(cc_path, cache_dir,
                                       file_filter=args.file_filter, limit=args.limit)
    print(f"[clazy] {clazy}  CLAZY_CHECKS={args.checks}", file=sys.stderr)
    print(f"[clazy] 参与分析 {len(files)} 个 TU，jobs={args.jobs}", file=sys.stderr)

    start = time.time()
    all_out = []
    n_ok = n_timeout = n_error = 0
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_one, clazy, cache_dir, args.checks, f, args.timeout): f
                for f in files}
        for fut in as_completed(futs):
            _f, out, state = fut.result()
            done += 1
            n_ok += (state == "ok")
            n_timeout += (state == "timeout")
            n_error += (state == "error")
            all_out.append(out)
            if done % 10 == 0 or done == len(files):
                print(f"  ...{done}/{len(files)} TU（ok={n_ok} timeout={n_timeout} "
                      f"err={n_error}）", file=sys.stderr)
    elapsed = time.time() - start

    findings, parse_errors = sa_common.parse_diagnostics(
        "\n".join(all_out), repo_root=REPO_ROOT)
    for fd in findings:
        fd.check = normalize_clazy_check(fd.check)

    raw_path = Path(args.raw)
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    import json
    with raw_path.open("w", encoding="utf-8", newline="\n") as fh:
        for fd in findings:
            fh.write(json.dumps(fd.as_dict(), ensure_ascii=False) + "\n")

    meta = {
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "tool_path": clazy, "version": "clazy",
        "config": f"CLAZY_CHECKS={args.checks}",
        "scope": f"Qt 专用检查（clazy）；build={args.build_dir}"
                 + (f"；filter={args.file_filter}" if args.file_filter else ""),
        "tu_total": len(files), "tu_ok": n_ok, "tu_timeout": n_timeout,
        "tu_error": n_error, "elapsed": elapsed,
    }
    report = sa_common.write_report(args.out, "clazy", meta, findings,
                                    parse_errors, top_n=args.top)
    print(f"\n[clazy] 完成：{len(findings)} 条独立告警，耗时 {elapsed:.1f}s", file=sys.stderr)
    print(f"[clazy] 报告：{report}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
