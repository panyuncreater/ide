#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""testing/static-analysis/run_clang_tidy.py — clang-tidy 全量分析驱动（阶段六）

聚焦 bugprone-* / clang-analyzer-* / cppcoreguidelines-* 三组（配置见同目录 .clang-tidy）。

关键点：本工程 Windows 侧用 MSVC 构建，compile_commands.json 里是 cl.exe 命令，
含 clang 前端无法消化的 MSVC 专有开关（/Yu /Fp<pch> /Zf /WX …）。本驱动会先
**净化** 编译数据库（剔除这些开关、保留 /FI 强制包含使头文件可解析），再逐 TU
调用 clang-tidy，并行执行 + 单 TU 超时，最后解析诊断、去重、按严重度产出 Top 20 报告。

前置：需在已初始化 MSVC 环境（VS DevShell，INCLUDE 已设）中运行，clang-cl 前端
才能定位 MSVC/SDK 系统头。clang-tidy 可用 `py -3 -m pip install clang-tidy` 获取。

只新增工具代码，不修改编译器实现，也不改动根 .clang-tidy。无第三方依赖，Python 3.8+。
"""

import argparse
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sa_common  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
DEFAULT_CONFIG = HERE / ".clang-tidy"
DEFAULT_REPORT = HERE / "reports" / "clang-tidy-report.md"
DEFAULT_RAW = HERE / "reports" / "clang-tidy-findings.jsonl"

# 排除：三方库 / 生成代码 / 构建产物 / 其它构建树。
EXCLUDE_SUBSTR = ("third_party", "_deps", "autogen", "mocs_compilation",
                  "moc_", "qrc_", "ui_", "/out/", "\\out\\",
                  "cmake_pch", "CompilerId")

# 需从 cl.exe 命令中剔除的 MSVC 专有 / 无关开关（前缀匹配）。保留 /FI（强制包含）。
_DROP_PREFIX = ("/Yu", "/Yc", "/Fp", "/Fo", "/Fd", "/FS", "/Zf", "/MP",
                "/JMC", "/Gm", "/RTC", "/Zi", "/ZI", "/analyze")
_DROP_EXACT = ("/W4", "/WX", "/WX-", "/Wall", "-Werror")


def _split_command(entry):
    """取得 entry 的参数列表（优先 arguments；否则 shlex 拆 command）。"""
    if entry.get("arguments"):
        return list(entry["arguments"])
    import shlex
    return shlex.split(entry.get("command", ""), posix=False)


def _is_bad(tok):
    t = tok.strip('"')
    if t in _DROP_EXACT:
        return True
    return any(t.startswith(p) for p in _DROP_PREFIX)


def sanitize_db(cc_path, cache_dir, exclude=EXCLUDE_SUBSTR, file_filter=None, limit=None):
    """读取 compile_commands.json，净化命令并写入 cache_dir/compile_commands.json。

    返回参与分析的文件绝对路径列表。
    """
    data = json.loads(Path(cc_path).read_text(encoding="utf-8"))
    out = []
    files = []
    seen = set()
    for e in data:
        f = e.get("file", "")
        fn = f.replace("\\", "/")
        if any(s.replace("\\", "/") in fn for s in exclude):
            continue
        if not (fn.endswith(".cpp") or fn.endswith(".cc") or fn.endswith(".cxx")):
            continue
        if file_filter and file_filter not in fn:
            continue
        rf = str(Path(f).resolve())
        if rf in seen:
            continue
        seen.add(rf)
        args = [a for a in _split_command(e) if not _is_bad(a)]
        out.append({"directory": e.get("directory", str(REPO_ROOT)),
                    "file": f, "arguments": args})
        files.append(rf)
        if limit and len(files) >= limit:
            break
    cache_dir = Path(cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)
    (cache_dir / "compile_commands.json").write_text(
        json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8", newline="\n")
    return files


def run_one(clang_tidy, cache_dir, config_file, file, timeout):
    """对单个 TU 跑 clang-tidy，返回 (file, stdout, state)。state: ok/timeout/error。"""
    cmd = [clang_tidy, "-p", str(cache_dir), "--quiet",
           f"--config-file={config_file}", file]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout,
                              cwd=str(cache_dir))
    except subprocess.TimeoutExpired:
        return file, "", "timeout"
    except OSError as e:
        return file, f"<os-error: {e}>", "error"
    out = (proc.stdout or b"").decode("utf-8", "replace")
    err = (proc.stderr or b"").decode("utf-8", "replace")
    return file, out + "\n" + err, "ok"


def tool_version(clang_tidy):
    try:
        p = subprocess.run([clang_tidy, "--version"], capture_output=True, timeout=30)
        for line in (p.stdout or b"").decode("utf-8", "replace").splitlines():
            if "version" in line.lower():
                return line.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "unknown"


def main(argv=None):
    ap = argparse.ArgumentParser(prog="run_clang_tidy.py",
                                 description="clang-tidy 全量分析驱动（阶段六）")
    ap.add_argument("--build-dir", default=str(REPO_ROOT / "build_debug"),
                    help="含 compile_commands.json 的构建目录（默认 build_debug）")
    ap.add_argument("--config-file", default=str(DEFAULT_CONFIG),
                    help="clang-tidy 配置（默认本目录 .clang-tidy）")
    ap.add_argument("--clang-tidy", default=None, help="clang-tidy 路径（默认自动探测）")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4)),
                    help="并行 TU 数（默认 CPU 核数）")
    ap.add_argument("--timeout", type=float, default=180.0, help="单 TU 超时秒（默认 180）")
    ap.add_argument("--file-filter", default=None, help="仅分析路径含该子串的 TU（冒烟/局部）")
    ap.add_argument("--limit", type=int, default=None, help="最多分析 N 个 TU（冒烟）")
    ap.add_argument("--out", default=str(DEFAULT_REPORT), help="Markdown 报告输出路径")
    ap.add_argument("--raw", default=str(DEFAULT_RAW), help="原始 findings JSONL 输出路径")
    ap.add_argument("--top", type=int, default=20, help="Top N 清单条数（默认 20）")
    ap.add_argument("--from-raw", default=None,
                    help="跳过分析，直接由已有 JSONL 重渲染报告（改分级/解释后免重跑）")
    args = ap.parse_args(argv)

    # ---- 复用已有 findings 重渲染（不重新分析）----
    if args.from_raw:
        findings = []
        with open(args.from_raw, encoding="utf-8") as fh:
            for ln in fh:
                d = json.loads(ln)
                findings.append(sa_common.Finding(
                    d["file"], d["line"], d["col"], d["sev"], d["msg"], d["check"]))
        meta = {
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "tool_path": "(复用 " + args.from_raw + "，未重新分析)",
            "version": "-", "config": args.config_file,
            "scope": f"bugprone-* / clang-analyzer-* / cppcoreguidelines-*（重渲染）",
            "tu_total": "-", "tu_ok": "-", "tu_timeout": "-", "tu_error": "-",
            "elapsed": 0.0,
        }
        report = sa_common.write_report(args.out, "clang-tidy", meta, findings, 0,
                                        top_n=args.top)
        print(f"[clang-tidy] 已由 {args.from_raw} 重渲染报告：{report}", file=sys.stderr)
        return 0

    clang_tidy = args.clang_tidy or sa_common.locate_tool(
        ["clang-tidy", "clang-tidy-18", "clang-tidy-17", "clang-tidy-16"],
        env_var="MINI_CLANG_TIDY", extra_dirs=sa_common.default_clang_tidy_dirs())
    if not clang_tidy:
        print("错误：未找到 clang-tidy。安装：`py -3 -m pip install clang-tidy`，"
              "或设 MINI_CLANG_TIDY 环境变量指向可执行文件。", file=sys.stderr)
        return 3

    cc_path = Path(args.build_dir) / "compile_commands.json"
    if not cc_path.is_file():
        print(f"错误：找不到 {cc_path}。请先用 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 配置构建。",
              file=sys.stderr)
        return 3

    if os.name == "nt" and not os.environ.get("INCLUDE"):
        print("警告：未检测到 INCLUDE 环境变量。clang-tidy 以 cl 驱动模式解析 MSVC 头需要"
              "在 VS DevShell 中运行，否则大量 TU 会因缺系统头而解析失败。", file=sys.stderr)

    cache_dir = HERE / ".cache-clang-tidy"
    files = sanitize_db(cc_path, cache_dir, file_filter=args.file_filter, limit=args.limit)
    print(f"[clang-tidy] {clang_tidy}", file=sys.stderr)
    print(f"[clang-tidy] 参与分析 {len(files)} 个 TU，jobs={args.jobs}，"
          f"单 TU 超时 {args.timeout:.0f}s", file=sys.stderr)

    start = time.time()
    all_out = []
    n_ok = n_timeout = n_error = 0
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_one, clang_tidy, cache_dir, args.config_file, f, args.timeout): f
                for f in files}
        for fut in as_completed(futs):
            file, out, state = fut.result()
            done += 1
            if state == "timeout":
                n_timeout += 1
            elif state == "error":
                n_error += 1
            else:
                n_ok += 1
            all_out.append(out)
            if done % 10 == 0 or done == len(files):
                print(f"  ...{done}/{len(files)} TU（ok={n_ok} timeout={n_timeout} "
                      f"err={n_error}）", file=sys.stderr)
    elapsed = time.time() - start

    findings, parse_errors = sa_common.parse_diagnostics(
        "\n".join(all_out), repo_root=REPO_ROOT)

    # 原始 findings 落盘（JSONL）
    raw_path = Path(args.raw)
    raw_path.parent.mkdir(parents=True, exist_ok=True)
    with raw_path.open("w", encoding="utf-8", newline="\n") as fh:
        for fd in findings:
            fh.write(json.dumps(fd.as_dict(), ensure_ascii=False) + "\n")

    meta = {
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "tool_path": clang_tidy,
        "version": tool_version(clang_tidy),
        "config": args.config_file,
        "scope": f"bugprone-* / clang-analyzer-* / cppcoreguidelines-*；"
                 f"排除 third_party/_deps/autogen；build={args.build_dir}"
                 + (f"；filter={args.file_filter}" if args.file_filter else ""),
        "tu_total": len(files), "tu_ok": n_ok, "tu_timeout": n_timeout,
        "tu_error": n_error, "elapsed": elapsed,
    }
    if parse_errors and n_ok == 0:
        meta["note"] = ("本轮全部 TU 均出现 clang 前端解析级错误：通常是未在 VS DevShell "
                        "中运行（缺 INCLUDE 系统头）或 MSVC 版本过新。请在 DevShell 内重跑。")

    report = sa_common.write_report(args.out, "clang-tidy", meta, findings,
                                    parse_errors, top_n=args.top)
    print(f"\n[clang-tidy] 完成：{len(findings)} 条独立告警，{parse_errors} 条解析级错误，"
          f"耗时 {elapsed:.1f}s", file=sys.stderr)
    print(f"[clang-tidy] 报告：{report}", file=sys.stderr)
    print(f"[clang-tidy] 原始：{raw_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
