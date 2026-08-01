#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""testing/driver.py — MiniLang 差分测试统一「编译并运行」驱动（阶段一）

职责（阶段一）：
  输入：一个 .mini 源文件 + 一个后端名
  行为：调用 mini_diff_runner 在进程内跑完整管线，捕获退出码/输出/错误
  输出：结构化 JSON {backend, compile_ok, run_ok, exit_code, status,
        stdout_hash, stderr, ...}
  超时：编译 + 运行双重超时（默认各 10s），超时归类为 HANG

设计：
  - 每个「后端 × 文件」各起一个 mini_diff_runner 子进程，崩溃互不影响。
  - runner 自带阶段看门狗（精确归因 HANG 于 compile/run）；driver 侧再套一层
    子进程整体超时作为兜底。
  - 无第三方依赖，兼容 Python 3.8+。

本文件同时可被阶段二 diff_test.py 作为库导入（run_backend / diff_file）。

约定：只新增测试/工具代码，不修改编译器实现逻辑。
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# ---- 后端集合 ----
# interp      : 树遍历解释器
# stackvm     : 栈式 VM（非 IR 路径）
# stackvm-ir  : 栈式 VM（IR 路径）
# regvm       : 寄存器式 VM（IR 路径，RegisterVM 唯一路径）
# jit         : x86-64 JIT（不支持场景优雅降级，degraded=true 时投票排除）
ALL_BACKENDS = ["interp", "stackvm", "stackvm-ir", "regvm", "jit"]

# strict 后端：项目核心不变量——语义必须严格一致。
# JIT 不在其中：JIT 为可选加速器，不支持场景优雅降级（不计入强制投票）。
STRICT_BACKENDS = ("interp", "stackvm", "stackvm-ir", "regvm")

REPO_ROOT = Path(__file__).resolve().parents[1]
_EXE = ".exe" if os.name == "nt" else ""


# ============================================================
# runner / Qt 运行时定位
# ============================================================
def find_runner(explicit=None, sanitized=False):
    """按优先级定位 mini_diff_runner 可执行文件。

    sanitized=True 时定位 sanitizer 构建树下的 runner（ASan/UBSan）：
    --san-runner > MINI_DIFF_RUNNER_SAN > 常见 sanitizer 构建目录。
    """
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise FileNotFoundError(f"--runner 指定的文件不存在: {explicit}")

    if sanitized:
        env = os.environ.get("MINI_DIFF_RUNNER_SAN")
        if env and Path(env).is_file():
            return Path(env)
        # sanitizer 构建目录（对齐 CMakePresets：windows-msvc-asan / linux-gcc-asan）
        for d in ["out/build/asan", "out/build/linux-asan", "build_asan"]:
            c = REPO_ROOT / d / "testing" / f"mini_diff_runner{_EXE}"
            if c.is_file():
                return c
        raise FileNotFoundError(
            "未找到 sanitizer 版 mini_diff_runner。请先构建 sanitizer 版：\n"
            "  cmake --preset windows-msvc-asan -DMINILANG_BUILD_TESTING_PIPELINE=ON  (Windows, 仅 ASan)\n"
            "  cmake --preset linux-gcc-asan   -DMINILANG_BUILD_TESTING_PIPELINE=ON  (Linux, ASan+UBSan)\n"
            "  cmake --build <dir> --target mini_diff_runner\n"
            "或用 --san-runner / MINI_DIFF_RUNNER_SAN 显式指定。"
        )

    env = os.environ.get("MINI_DIFF_RUNNER")
    if env and Path(env).is_file():
        return Path(env)

    candidates = []
    build_root = os.environ.get("MINI_BUILD_ROOT")
    if build_root:
        candidates.append(Path(build_root) / "testing" / f"mini_diff_runner{_EXE}")
    # 常见构建目录
    for d in ["build_debug", "build", "out/build/debug", "out/build/release",
              "out/build/asan", "out/build/linux-debug", "out/build/linux-release"]:
        candidates.append(REPO_ROOT / d / "testing" / f"mini_diff_runner{_EXE}")
    # 与本脚本同级的 testing 输出（少见）
    candidates.append(REPO_ROOT / "testing" / f"mini_diff_runner{_EXE}")

    for c in candidates:
        if c.is_file():
            return c
    raise FileNotFoundError(
        "未找到 mini_diff_runner，可用 --runner 显式指定，或设置 MINI_DIFF_RUNNER 环境变量。\n"
        "先构建：cmake -DMINILANG_BUILD_TESTING_PIPELINE=ON ... && cmake --build <dir> --target mini_diff_runner"
    )


def find_qt_bin(runner_path, explicit=None):
    """定位 Qt 运行期 DLL 目录（供 Windows PATH 注入）。"""
    if explicit:
        return explicit
    env = os.environ.get("MINI_QT_BIN")
    if env:
        return env
    # 从 runner 所在构建树的 CMakeCache.txt 解析 CMAKE_PREFIX_PATH
    build_dir = Path(runner_path).resolve().parent.parent  # <build>/testing/runner.exe → <build>
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        try:
            for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
                if "CMAKE_PREFIX_PATH" in line and "=" in line:
                    val = line.split("=", 1)[1].strip()
                    if val:
                        first = val.split(";")[0]
                        return str(Path(first) / "bin")
        except OSError:
            pass
    return None


def build_child_env(runner_path, qt_bin):
    """构造子进程环境：把 Qt bin / 构建根 / runner 目录并入库搜索路径。"""
    env = dict(os.environ)
    runner_dir = str(Path(runner_path).resolve().parent)
    build_root = str(Path(runner_path).resolve().parent.parent)
    extra = [d for d in [qt_bin, build_root, runner_dir] if d]
    if os.name == "nt":
        env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    else:
        env["LD_LIBRARY_PATH"] = os.pathsep.join(extra + [env.get("LD_LIBRARY_PATH", "")])
        env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    return env


# ============================================================
# Sanitizer（阶段三）：环境注入 + 报告解析
# ============================================================
SANITIZER_DIR = Path(__file__).resolve().parent / "sanitizer"

_TOOL_MAP = {
    "AddressSanitizer": "asan", "ThreadSanitizer": "tsan",
    "MemorySanitizer": "msan", "LeakSanitizer": "lsan",
}


def apply_sanitizer_env(env, suppressions_dir=None):
    """为子进程设置 ASAN/UBSAN/LSAN_OPTIONS（含抑制文件）。

    halt_on_error=0 使 runner 尽量跑完并打印 JSON；abort_on_error=0 避免 Windows 弹窗。
    detect_leaks / suppressions 仅 Linux/Clang(GCC) 下有效（MSVC ASan 不支持 LSan）。
    """
    sup = Path(suppressions_dir) if suppressions_dir else SANITIZER_DIR
    asan_supp, lsan_supp, ubsan_supp = sup / "asan.supp", sup / "lsan.supp", sup / "ubsan.supp"
    if os.name == "nt":
        # MSVC ASan：仅设置受支持的选项（无 LSan/抑制）
        env["ASAN_OPTIONS"] = "halt_on_error=0:abort_on_error=0:print_stats=0"
    else:
        asan_opts = "halt_on_error=0:abort_on_error=0:detect_leaks=1:print_stats=0"
        if asan_supp.is_file():
            asan_opts += f":suppressions={asan_supp}"
        env["ASAN_OPTIONS"] = asan_opts
        if lsan_supp.is_file():
            env["LSAN_OPTIONS"] = f"suppressions={lsan_supp}"
        ubsan_opts = "halt_on_error=0:print_stacktrace=1"
        if ubsan_supp.is_file():
            ubsan_opts += f":suppressions={ubsan_supp}"
        env["UBSAN_OPTIONS"] = ubsan_opts
    return env


def parse_sanitizer_report(stderr):
    """从 stderr 提取 sanitizer 命中（每种类型单独计为一类 bug）。

    返回 [{tool, type[, detail]}]；tool ∈ asan/lsan/ubsan/tsan/msan。无命中返回 []。
    """
    if not stderr:
        return []
    hits = []
    # ASan/TSan/MSan/LSan: [==pid==]ERROR: XxxSanitizer: <type>
    for m in re.finditer(r"ERROR: (AddressSanitizer|ThreadSanitizer|MemorySanitizer|LeakSanitizer):\s*([\w-]*)",
                         stderr):
        tool = _TOOL_MAP[m.group(1)]
        typ = "memory-leak" if tool == "lsan" else (m.group(2) or "error")
        hits.append({"tool": tool, "type": typ})
    if "LeakSanitizer: detected memory leaks" in stderr and not any(h["tool"] == "lsan" for h in hits):
        hits.append({"tool": "lsan", "type": "memory-leak"})
    # UBSan: <file>:<line>:<col>: runtime error: <desc>
    for m in re.finditer(r"runtime error:\s*(.+)", stderr):
        hits.append({"tool": "ubsan", "type": "undefined-behavior", "detail": m.group(1).strip()[:120]})
    # 去重
    seen, uniq = set(), []
    for h in hits:
        k = (h.get("tool"), h.get("type"), h.get("detail", ""))
        if k not in seen:
            seen.add(k)
            uniq.append(h)
    return uniq


# ============================================================
# 单后端执行
# ============================================================
def run_backend(file, backend, runner=None, compile_timeout=10.0, run_timeout=10.0,
                qt_bin=None, child_env=None, sanitized=False, san_runner=None,
                suppressions_dir=None):
    """运行「单文件 × 单后端」，返回结构化结果 dict。

    结果 dict 关键字段：
      backend, status, compile_ok, run_ok, degraded, exit_code,
      output_len, output_hash, diff_hash, diff, output(stdout), error(stderr)
      runner_returncode, raw_stderr
    status 取值：ok / parse_error / compile_error / runtime_error /
                 jit_unsupported / jit_runtime_error / crash / hang / io_error
    """
    runner_path = find_runner(san_runner if sanitized else runner, sanitized=sanitized)
    if qt_bin is None:
        qt_bin = find_qt_bin(runner_path)
    if child_env is None:
        child_env = build_child_env(runner_path, qt_bin)
        if sanitized:
            child_env = apply_sanitizer_env(child_env, suppressions_dir)

    cmd = [
        str(runner_path),
        "--file", str(file),
        "--backend", backend,
        "--compile-timeout-ms", str(int(compile_timeout * 1000)),
        "--run-timeout-ms", str(int(run_timeout * 1000)),
    ]
    overall_timeout = compile_timeout + run_timeout + 5.0  # 兜底：略高于 runner 内部看门狗

    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=overall_timeout, env=child_env)
    except subprocess.TimeoutExpired:
        # runner 内部看门狗未能先触发的极端情况（兜底）
        return _synth(backend, "hang", exit_code=124,
                      error=f"driver overall timeout ({overall_timeout:.0f}s)", phase="overall")

    stdout = proc.stdout.decode("utf-8", errors="replace") if proc.stdout else ""
    stderr = proc.stderr.decode("utf-8", errors="replace") if proc.stderr else ""
    san = parse_sanitizer_report(stderr) if sanitized else []

    # runner 输出一行 JSON（取最后一行非空）
    result = None
    for line in reversed(stdout.strip().splitlines()):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                result = json.loads(line)
                break
            except json.JSONDecodeError:
                continue

    if result is None:
        # 无 JSON → 极可能是硬崩溃（段错误/abort）或 DLL 加载失败（sanitized 模式下常为 ASan abort）
        status = "crash" if proc.returncode not in (0,) else "unknown"
        res = _synth(backend, status, exit_code=proc.returncode,
                     error=(stderr.strip() or "runner produced no JSON output"),
                     raw_stderr=stderr, raw_stdout=stdout, runner_returncode=proc.returncode)
        res["sanitizer"] = san
        return res

    result["runner_returncode"] = proc.returncode
    result["raw_stderr"] = stderr
    result["sanitizer"] = san
    # 归一：确保关键字段存在
    result.setdefault("backend", backend)
    result.setdefault("output", "")
    result.setdefault("diff", "")
    result.setdefault("compile_ok", False)
    result.setdefault("run_ok", False)
    result.setdefault("degraded", False)
    return result


def _synth(backend, status, exit_code=0, error="", phase="done",
           raw_stderr="", raw_stdout="", runner_returncode=None):
    """合成一个结果 dict（用于 runner 无 JSON / 超时等异常路径）。"""
    return {
        "backend": backend, "status": status, "phase": phase,
        "compile_ok": False, "run_ok": False, "degraded": False,
        "exit_code": exit_code, "output_len": 0, "output_hash": "", "diff_hash": "",
        "output": "", "diff": f"<{status}:{error}>", "error": error,
        "raw_stderr": raw_stderr, "raw_stdout": raw_stdout,
        "runner_returncode": runner_returncode,
    }


# ============================================================
# 多后端差分（阶段一：plumbing 级自洽检查；完整投票见阶段二 diff_test.py）
# ============================================================
def diff_file(file, backends=None, runner=None, compile_timeout=10.0, run_timeout=10.0,
              qt_bin=None, sanitized=False, san_runner=None, suppressions_dir=None):
    """对单文件跑指定后端集合并做一致性分类。

    返回 dict：
      file, results(每后端), voting_backends(参与投票的), category, agree(bool),
      groups(diff串 → [后端]), suspects(少数派后端), notes, sanitizer_hits
    category：agree / disagreement / crash / hang / all_error / degraded_only
    sanitized=True 时额外聚合 sanitizer_hits（每后端的 ASan/UBSan 报告）。
    """
    if backends is None:
        backends = ALL_BACKENDS
    runner_path = find_runner(san_runner if sanitized else runner, sanitized=sanitized)
    if qt_bin is None:
        qt_bin = find_qt_bin(runner_path)
    child_env = build_child_env(runner_path, qt_bin)
    if sanitized:
        child_env = apply_sanitizer_env(child_env, suppressions_dir)

    results = []
    for b in backends:
        results.append(run_backend(file, b, runner=str(runner_path),
                                   compile_timeout=compile_timeout, run_timeout=run_timeout,
                                   qt_bin=qt_bin, child_env=child_env, sanitized=sanitized))

    # 后端分层：strict 后端必须永远一致；JIT 是可选加分项（不支持时优雅降级）。
    # 依据确认的设计：JIT「不崩溃不错值」——JIT 报错/降级只记录不判负，
    # 仅当 JIT 跑完(run_ok) 却与 strict 共识不一致（错值）或崩溃时才算真实 JIT bug。
    strict = [r for r in results if r["backend"] in STRICT_BACKENDS
              and r.get("status") != "io_error"]
    jit = next((r for r in results if r["backend"] == "jit"), None)

    strict_crashed = [r["backend"] for r in strict if r.get("status") == "crash"]
    strict_hung = [r["backend"] for r in strict if r.get("status") == "hang"]

    # strict 后端按规范输出串分组
    groups = {}
    for r in strict:
        groups.setdefault(r.get("diff", ""), []).append(r["backend"])
    consensus = next(iter(groups)) if len(groups) == 1 else None

    notes = []
    suspects = []
    voting_backends = [b for grp in groups.values() for b in grp]

    if not strict:
        category = "all_error"
    elif strict_crashed:
        category = "crash"
        notes.append(f"strict 崩溃后端: {', '.join(strict_crashed)}")
    elif strict_hung:
        category = "hang"
        notes.append(f"strict 超时后端: {', '.join(strict_hung)}")
    elif len(groups) > 1:
        category = "disagreement"
        ranked = sorted(groups.items(), key=lambda kv: len(kv[1]), reverse=True)
        majority = ranked[0][1]
        for _diff, bes in ranked[1:]:
            suspects.extend(bes)
        notes.append(f"strict 多数派: {', '.join(majority)}；少数派(疑似 bug): {', '.join(suspects)}")
    else:
        category = "agree"  # strict 全部一致（JIT 评估见下）

    # ---- JIT 评估（仅在 strict 达成共识后判定 JIT 偏差）----
    jit_status = None  # agree / deviation / degraded / crash / absent
    if jit is not None:
        js = jit.get("status")
        if js == "crash":
            jit_status = "crash"
            notes.append("JIT 崩溃（违反『不崩溃』）")
            if category == "agree":
                category = "jit_deviation"
        elif jit.get("run_ok") and consensus is not None:
            if jit.get("diff") == consensus:
                jit_status = "agree"
                voting_backends.append("jit")
            else:
                jit_status = "deviation"
                suspects.append("jit")
                notes.append("JIT 跑完但输出与 strict 共识不一致（违反『不错值』）")
                if category == "agree":
                    category = "jit_deviation"
        else:
            # jit_unsupported / jit_runtime_error / parse|compile_error 等 → 优雅降级
            jit_status = "degraded"
            notes.append(f"JIT 优雅降级（{js}），已排除出投票")

    # 阶段三：聚合各后端的 sanitizer 命中（编译器/VM 自身的内存安全/UB 问题）。
    sanitizer_hits = []
    if sanitized:
        for r in results:
            for h in r.get("sanitizer", []) or []:
                sanitizer_hits.append({"backend": r["backend"], **h})

    return {
        "file": str(file),
        "results": results,
        "voting_backends": voting_backends,
        "category": category,
        "agree": category == "agree",
        "groups": groups,
        "suspects": suspects,
        "jit_status": jit_status,
        "crashed": strict_crashed + (["jit"] if jit_status == "crash" else []),
        "hung": strict_hung,
        "notes": notes,
        "sanitizer_hits": sanitizer_hits,
    }


# ============================================================
# CLI
# ============================================================
def _cmd_run(args):
    res = run_backend(args.file, args.backend, runner=args.runner,
                      compile_timeout=args.compile_timeout, run_timeout=args.run_timeout,
                      qt_bin=args.qt_bin, sanitized=args.sanitized, san_runner=args.san_runner)
    print(json.dumps(res, ensure_ascii=False, indent=2 if args.pretty else None))
    if args.assert_status:
        return 0 if res.get("status") == args.assert_status else 3
    # sanitized 模式：命中 sanitizer 即视为发现（退出 1）
    if args.sanitized and res.get("sanitizer"):
        return 1
    # 默认：ok / 优雅降级视为成功；其余非零
    return 0 if res.get("status") in ("ok", "jit_unsupported") else 1


def _cmd_diff(args):
    backends = args.backends.split(",") if args.backends else ALL_BACKENDS
    res = diff_file(args.file, backends=backends, runner=args.runner,
                    compile_timeout=args.compile_timeout, run_timeout=args.run_timeout,
                    qt_bin=args.qt_bin, sanitized=args.sanitized, san_runner=args.san_runner)
    if args.quiet:
        # 精简：仅打印分类与分组摘要
        summary = {"file": res["file"], "category": res["category"],
                   "agree": res["agree"], "voting_backends": res["voting_backends"],
                   "groups": {k[:80]: v for k, v in res["groups"].items()},
                   "suspects": res["suspects"], "notes": res["notes"],
                   "sanitizer_hits": res.get("sanitizer_hits", [])}
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    else:
        print(json.dumps(res, ensure_ascii=False, indent=2))

    if args.sanitized and res.get("sanitizer_hits"):
        return 1  # sanitizer 命中优先判为发现
    if args.require_agreement:
        # CTest 用：仅 strict 后端完全一致且无 JIT 错值/崩溃时判为通过
        return 0 if res["category"] == "agree" else 1
    return 0


def build_parser():
    p = argparse.ArgumentParser(
        prog="driver.py",
        description="MiniLang 差分测试统一驱动（阶段一）")
    sub = p.add_subparsers(dest="command", required=True)

    def add_common(sp):
        sp.add_argument("--file", required=True, help="MiniLang 源文件 (.mini)")
        sp.add_argument("--runner", default=None, help="mini_diff_runner 路径（默认自动探测）")
        sp.add_argument("--qt-bin", dest="qt_bin", default=None, help="Qt 运行期 DLL 目录")
        sp.add_argument("--compile-timeout", type=float, default=10.0, help="编译超时秒（默认 10）")
        sp.add_argument("--run-timeout", type=float, default=10.0, help="运行超时秒（默认 10）")
        sp.add_argument("--sanitized", action="store_true",
                        help="使用 sanitizer(ASan/UBSan) 构建的 runner，并解析分类 sanitizer 报告")
        sp.add_argument("--san-runner", dest="san_runner", default=None,
                        help="sanitizer 版 mini_diff_runner 路径（默认自动探测 asan 构建目录）")

    sp_run = sub.add_parser("run", help="运行单文件×单后端，输出结构化 JSON")
    add_common(sp_run)
    sp_run.add_argument("--backend", required=True, choices=ALL_BACKENDS)
    sp_run.add_argument("--assert-status", default=None,
                        help="断言 status 等于该值，否则退出码 3（回归用）")
    sp_run.add_argument("--pretty", action="store_true", help="缩进美化输出")
    sp_run.set_defaults(func=_cmd_run)

    sp_diff = sub.add_parser("diff", help="单文件多后端一致性检查")
    add_common(sp_diff)
    sp_diff.add_argument("--backends", default=None,
                         help="逗号分隔的后端子集（默认全部）")
    sp_diff.add_argument("--require-agreement", action="store_true",
                         help="仅当所有非降级后端一致时退出 0（CTest 用）")
    sp_diff.add_argument("--quiet", action="store_true", help="仅打印分类摘要")
    sp_diff.set_defaults(func=_cmd_diff)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except FileNotFoundError as e:
        print(json.dumps({"error": str(e)}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
