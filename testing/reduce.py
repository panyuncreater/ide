#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""testing/reduce.py — MiniLang 触发用例自动缩减（阶段五，思路对齐 C-Reduce）

职责：
  输入：一个「触发 bug」的 .mini 源程序 + 一个「趣味性判定（interestingness）」配置
        （判定哪个后端、哪种失败模式：崩溃 / 输出不一致 / 超时 / JIT 偏差…）。
  行为：通过一系列源码变换（去注释/空行、块删除、行级 delta-debugging、
        表达式/字面量简化、标识符重命名）迭代缩减；**每一步都重新调用驱动脚本
        （driver.diff_file / driver.run_backend）验证 bug 仍触发**，不触发即回退。
  输出：最小复现用例（目标缩减到 30 行以内或无法再缩）。
        成功后可自动拷贝进 testing/regression/ 并登记到 manifest.json（供 CTest 回归）。

「趣味性判定」= C-Reduce 的 interestingness test：本文件用「差分驱动重跑」实现，
判据完全复用阶段一 driver 的分类，保证「缩减前后触发的是同一类 bug」，避免缩偏
（可选 --suspects / --match-error 进一步锁定同一根因）。

verify 子命令：给 CTest 用。exit 0 当且仅当「bug 已消失（判据不再成立）」，
即缩减用例登记为回归时：status=open → WILL_FAIL 包裹（bug 仍在→判据成立→exit 1
→CTest 记 PASS）；一旦后端修复→判据不再成立→exit 0→WILL_FAIL 变 FAIL→自动提醒转正。

无第三方依赖，兼容 Python 3.8+。约定：只新增测试/工具代码，不修改编译器实现逻辑。
"""

import argparse
import json
import os
import re
import sys
import tempfile
import time
from pathlib import Path

# 使 driver 可被导入（与本文件同目录）
sys.path.insert(0, str(Path(__file__).resolve().parent))
import driver  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[1]
REGRESSION_DIR = Path(__file__).resolve().parent / "regression"
MANIFEST = REGRESSION_DIR / "manifest.json"

# 差分分类（driver.diff_file 的 category 取值） + "mismatch"（任意非 agree）。
DIFF_MODES = ("disagreement", "jit_deviation", "crash", "hang", "all_error", "mismatch")
# 单后端 status（driver.run_backend 的 status 取值）。
SINGLE_STATUSES = ("crash", "hang", "runtime_error", "compile_error",
                   "parse_error", "jit_runtime_error", "jit_unsupported", "ok")

# 词法关键字（取自 lexer/Lexer.cpp）+ 常见内建名：重命名/简化时跳过，避免破坏语义。
# 说明：即便漏列某个内建，被误改的候选也会被趣味性判定否决并回退，故此表仅为「减少
#       无效尝试」的启发式，不影响正确性。
RESERVED = {
    # keywords
    "and", "array", "as", "async", "await", "bool", "break", "case", "catch",
    "class", "const", "continue", "default", "dict", "else", "enum", "export",
    "extends", "false", "finally", "float", "for", "from", "fun", "func",
    "function", "if", "import", "int", "macro", "match", "not", "null", "or",
    "print", "return", "string", "super", "throw", "trait", "true", "try",
    "var", "while", "with", "yield",
    # 常见内建函数 / 方法名
    "len", "sum", "push", "pop", "keys", "values", "contains", "remove",
    "insert", "append", "size", "length", "abs", "min", "max", "sqrt", "pow",
    "floor", "ceil", "round", "str", "num", "type", "range", "clone", "copy",
    "sort", "reverse", "join", "split", "map", "filter", "reduce", "print",
}

_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_NUM_RE = re.compile(r"\b\d+\b")


# ============================================================
# 源码分段扫描（区分 代码 / 字符串字面量 / 注释）
# ============================================================
def _scan_segments(text):
    """把源码切成若干段： (kind, s)，kind ∈ {'code','str','cmt'}。

    - 'str' : 双引号字符串字面量（含内部 {插值}，整体视为不可拆分的原子）。
    - 'cmt' : // 行注释（到行尾，不含换行）或 /* ... */ 块注释。
    - 'code': 其余。
    用于让各变换只作用于该作用的段（如重命名只改 code、清注释只删 cmt）。
    """
    segs = []
    i, n = 0, len(text)
    buf = []

    def flush_code():
        if buf:
            segs.append(("code", "".join(buf)))
            buf.clear()

    while i < n:
        c = text[i]
        # 字符串
        if c == '"':
            flush_code()
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            segs.append(("str", text[i:j]))
            i = j
            continue
        # 行注释
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            flush_code()
            j = i
            while j < n and text[j] != "\n":
                j += 1
            segs.append(("cmt", text[i:j]))
            i = j
            continue
        # 块注释
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            flush_code()
            j = text.find("*/", i + 2)
            j = (j + 2) if j != -1 else n
            segs.append(("cmt", text[i:j]))
            i = j
            continue
        buf.append(c)
        i += 1
    flush_code()
    return segs


def _map_code(text, fn):
    """只对 code 段应用 fn(str)->str，字符串/注释段原样保留。"""
    out = []
    for kind, s in _scan_segments(text):
        out.append(fn(s) if kind == "code" else s)
    return "".join(out)


# ============================================================
# 趣味性判定（interestingness）
# ============================================================
class Oracle:
    """判断某段源码是否仍触发目标 bug（趣味性判定）。

    spec 关键字段：
      kind        : 'diff'（多后端差分）| 'single'（单后端 status 断言）
      mode        : diff → category（或 'mismatch'）；single → 期望 status
      backend     : single 模式的后端名
      backends    : diff 模式的后端子集（默认 strict 四后端，跳过 jit 提速）
      suspects    : diff 模式要求的少数派后端子集（可选，锁定同一根因）
      match_error : 关键错误文本子串（可选，锁定同一根因）
      runner/qt_bin/compile_timeout/run_timeout : 透传 driver
    """

    def __init__(self, spec):
        self.kind = spec.get("kind") or ("single" if spec.get("backend") else "diff")
        self.mode = spec["mode"]
        self.backend = spec.get("backend")
        self.backends = spec.get("backends") or list(driver.STRICT_BACKENDS)
        self.suspects = set(spec.get("suspects") or [])
        self.match_error = spec.get("match_error") or ""
        self.runner = spec.get("runner")
        self.qt_bin = spec.get("qt_bin")
        self.compile_timeout = float(spec.get("compile_timeout", 10.0))
        self.run_timeout = float(spec.get("run_timeout", 10.0))
        self.calls = 0
        self.last = None
        # 单次复用的临时文件（driver 每次读文件，覆盖写即可）
        fd, self._tmp = tempfile.mkstemp(suffix=".mini", prefix="mlreduce_")
        os.close(fd)

    def to_spec(self):
        """导出可序列化的判据（供 manifest 登记 / verify 复用）。"""
        s = {"kind": self.kind, "mode": self.mode}
        if self.backend:
            s["backend"] = self.backend
        if self.kind == "diff":
            s["backends"] = self.backends
        if self.suspects:
            s["suspects"] = sorted(self.suspects)
        if self.match_error:
            s["match_error"] = self.match_error
        return s

    def _write(self, text):
        Path(self._tmp).write_text(text, encoding="utf-8", newline="\n")

    def interesting_text(self, text):
        self._write(text)
        return self.interesting_path(self._tmp)

    def interesting_path(self, path):
        self.calls += 1
        if self.kind == "single":
            r = driver.run_backend(
                path, self.backend, runner=self.runner,
                compile_timeout=self.compile_timeout, run_timeout=self.run_timeout,
                qt_bin=self.qt_bin)
            self.last = {"status": r.get("status"), "error": r.get("error", "")}
            if r.get("status") != self.mode:
                return False
            if self.match_error and self.match_error not in (
                    (r.get("error") or "") + (r.get("diff") or "")):
                return False
            return True

        res = driver.diff_file(
            path, backends=self.backends, runner=self.runner,
            compile_timeout=self.compile_timeout, run_timeout=self.run_timeout,
            qt_bin=self.qt_bin)
        cat = res.get("category")
        suspects = set(res.get("suspects", []))
        self.last = {"category": cat, "suspects": sorted(suspects)}
        if self.mode == "mismatch":
            if cat == "agree":
                return False
        elif cat != self.mode:
            return False
        if self.suspects and not self.suspects.issubset(suspects):
            return False
        if self.match_error:
            blob = " ".join((r.get("error", "") or "") + (r.get("diff", "") or "")
                            for r in res.get("results", []))
            if self.match_error not in blob:
                return False
        return True

    def cleanup(self):
        try:
            os.unlink(self._tmp)
        except OSError:
            pass


# ============================================================
# 规模度量与工具
# ============================================================
def _nonblank_lines(text):
    return [ln for ln in text.split("\n") if ln.strip()]


def _line_count(text):
    return len(_nonblank_lines(text))


def _better(candidate, current):
    """candidate 是否比 current「更小」：先比非空行数，再比字节数。"""
    ca, cu = _nonblank_lines(candidate), _nonblank_lines(current)
    if len(ca) != len(cu):
        return len(ca) < len(cu)
    return len("\n".join(ca)) < len("\n".join(cu))


# ============================================================
# 变换 Pass（均返回「更小的新文本」或 None 表示无进展）
# ============================================================
def strip_comments_and_blanks(text, oracle):
    """去掉注释与空行（一次性候选）。"""
    kept = []
    for kind, s in _scan_segments(text):
        if kind != "cmt":
            kept.append(s)
    stripped = "".join(kept)
    stripped = "\n".join(ln for ln in stripped.split("\n") if ln.strip())
    stripped = stripped + "\n"
    if stripped != text and _line_count(stripped) <= _line_count(text) \
            and oracle.interesting_text(stripped):
        return stripped
    return None


def ddmin_lines(text, oracle):
    """行级 delta-debugging：多粒度（大→小）贪心删行，能删则删。"""
    lines = text.split("\n")
    changed = False
    size = max(1, len(lines) // 2)
    while size >= 1:
        i = 0
        while i < len(lines):
            cand = lines[:i] + lines[i + size:]
            cand_text = "\n".join(cand)
            if cand_text.strip() and oracle.interesting_text(cand_text):
                lines = cand
                changed = True
                # 不前移 i：让后续块顶上来继续尝试
            else:
                i += size
        if size == 1:
            break
        size //= 2
    return "\n".join(lines) if changed else None


def _brace_pairs(text):
    """string/comment 感知地找出所有配对 {..}，返回 [(open_idx, close_idx)]。"""
    # 先算出「非代码」掩码，花括号只在 code 段生效。
    mask = bytearray(len(text))  # 1 表示该位置在 str/cmt 内，忽略
    pos = 0
    for kind, s in _scan_segments(text):
        if kind != "code":
            for k in range(pos, pos + len(s)):
                mask[k] = 1
        pos += len(s)
    stack, pairs = [], []
    for idx, ch in enumerate(text):
        if mask[idx]:
            continue
        if ch == "{":
            stack.append(idx)
        elif ch == "}" and stack:
            o = stack.pop()
            pairs.append((o, idx))
    return pairs


def remove_brace_blocks(text, oracle):
    """把非空 {..} 体清空为 {}（优先大跨度），可整体消除函数/循环/分支体。"""
    current = text
    changed = False
    while True:
        pairs = _brace_pairs(current)
        # 大跨度优先
        pairs.sort(key=lambda p: p[1] - p[0], reverse=True)
        progressed = False
        for o, c in pairs:
            if c - o <= 1:
                continue  # 已是 {}
            cand = current[:o + 1] + current[c:]
            if oracle.interesting_text(cand):
                current = cand
                changed = True
                progressed = True
                break
        if not progressed:
            break
    return current if changed else None


def simplify_tokens(text, oracle):
    """字面量简化：大数字 → 0/1；非空字符串 → ""。逐个 distinct token 试。"""
    current = text
    changed = False

    # 数字（仅 code 段中出现的多位数字）
    nums = set()
    for kind, s in _scan_segments(current):
        if kind == "code":
            nums.update(m.group(0) for m in _NUM_RE.finditer(s) if len(m.group(0)) > 1)
    for num in sorted(nums, key=lambda x: (-len(x), x)):
        for repl in ("0", "1"):
            if num == repl:
                continue
            cand = _map_code(current, lambda code: re.sub(
                r"\b" + re.escape(num) + r"\b", repl, code))
            if cand != current and oracle.interesting_text(cand):
                current = cand
                changed = True
                break

    # 字符串字面量 → ""
    strs = set()
    for kind, s in _scan_segments(current):
        if kind == "str" and len(s) > 2:
            strs.add(s)
    for lit in sorted(strs, key=lambda x: -len(x)):
        cand = current.replace(lit, '""')
        if cand != current and oracle.interesting_text(cand):
            current = cand
            changed = True

    return current if changed else None


def rename_identifiers(text, oracle):
    """把用户标识符统一重命名为短名（a,b,c,…），一次性候选。缩短字节、提升可读性。"""
    names = []
    seen = set()
    for kind, s in _scan_segments(text):
        if kind != "code":
            continue
        for m in _IDENT_RE.finditer(s):
            w = m.group(0)
            if w in RESERVED or w in seen:
                continue
            seen.add(w)
            names.append(w)
    if not names:
        return None

    def short_name(k):
        # a..z, 然后 a1,b1,...
        base = chr(ord("a") + (k % 26))
        suffix = k // 26
        return base if suffix == 0 else f"{base}{suffix}"

    mapping = {}
    used = set(RESERVED)
    k = 0
    for w in names:
        cand = short_name(k)
        while cand in used:
            k += 1
            cand = short_name(k)
        mapping[w] = cand
        used.add(cand)
        k += 1

    # 仅当确有缩短空间才尝试
    if all(len(mapping[w]) >= len(w) for w in names):
        return None

    # \b 词边界：确保按「完整标识符」替换，不会命中保留字/更长标识符的子串。
    pat = re.compile(r"\b(?:" +
                     "|".join(re.escape(w) for w in sorted(names, key=len, reverse=True)) +
                     r")\b")

    def rename_code(code):
        return pat.sub(lambda m: mapping.get(m.group(0), m.group(0)), code)

    cand = _map_code(text, rename_code)
    if cand != text and oracle.interesting_text(cand):
        return cand
    return None


PASSES = [
    ("strip", strip_comments_and_blanks),
    ("blocks", remove_brace_blocks),
    ("lines", ddmin_lines),
    ("tokens", simplify_tokens),
    ("rename", rename_identifiers),
]


# ============================================================
# 缩减主循环（多 Pass 迭代至不动点）
# ============================================================
def reduce_source(text, oracle, target_lines=30, max_rounds=40, max_calls=100000,
                  verbose=True):
    if not text.endswith("\n"):
        text += "\n"
    if not oracle.interesting_text(text):
        raise RuntimeError(
            "原始程序未触发目标 bug（趣味性判定不成立）；请检查 --mode/--backend/"
            "--suspects/--match-error 是否与实际失败一致。当前判定：%r" % (oracle.last,))
    current = text
    if verbose:
        print(f"[reduce] 起始 {_line_count(current)} 行 / {len(current)} 字节；"
              f"判据 kind={oracle.kind} mode={oracle.mode} "
              f"backend={oracle.backend or '-'} suspects={sorted(oracle.suspects) or '-'} "
              f"match_error={oracle.match_error or '-'}", file=sys.stderr)

    rnd = 0
    while rnd < max_rounds and oracle.calls < max_calls:
        rnd += 1
        progressed = False
        for name, fn in PASSES:
            if oracle.calls >= max_calls:
                break
            res = fn(current, oracle)
            if res is not None and _better(res, current):
                if verbose:
                    print(f"  [round {rnd}] pass={name:7s} "
                          f"{_line_count(current)}→{_line_count(res)} 行 "
                          f"({len(current)}→{len(res)} 字节, calls={oracle.calls})",
                          file=sys.stderr)
                current = res
                progressed = True
        if not progressed:
            break

    # 收尾：去空行、保证末尾换行
    current = "\n".join(ln for ln in current.split("\n") if ln.strip()) + "\n"
    if verbose:
        n = _line_count(current)
        goal = "✅ 达成 ≤%d 行目标" % target_lines if n <= target_lines else \
               "⚠ 未达 ≤%d 行（已至不动点，无法再缩）" % target_lines
        print(f"[reduce] 完成：{n} 行 / {len(current)} 字节，共 {oracle.calls} 次判定。{goal}",
              file=sys.stderr)
    return current


# ============================================================
# 回归登记：拷贝进 testing/regression/ + 更新 manifest.json
# ============================================================
def _load_manifest():
    if MANIFEST.is_file():
        try:
            return json.loads(MANIFEST.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    return {"version": 1, "cases": []}


def _save_manifest(data):
    REGRESSION_DIR.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8", newline="\n")


def register_case(name, minimal_text, oracle, status="open", origin=None, notes=""):
    """把最小用例写入 testing/regression/<name>.mini 并 upsert manifest 条目。"""
    REGRESSION_DIR.mkdir(parents=True, exist_ok=True)
    safe = re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_") or "case"
    mini_path = REGRESSION_DIR / f"{safe}.mini"
    mini_path.write_text(minimal_text, encoding="utf-8", newline="\n")

    entry = {
        "name": safe,
        "file": f"{safe}.mini",
        "status": status,          # open → WILL_FAIL；fixed → 必须通过
        "lines": _line_count(minimal_text),
        "created": time.strftime("%Y-%m-%d"),
    }
    entry.update(oracle.to_spec())
    if origin:
        entry["origin"] = str(origin)
    if notes:
        entry["notes"] = notes

    data = _load_manifest()
    cases = data.setdefault("cases", [])
    for i, c in enumerate(cases):
        if c.get("name") == safe:
            cases[i] = entry
            break
    else:
        cases.append(entry)
    cases.sort(key=lambda c: c["name"])
    _save_manifest(data)
    return mini_path


# ============================================================
# CLI: reduce
# ============================================================
def _oracle_spec_from_args(args):
    spec = {}
    if args.config:
        spec.update(json.loads(Path(args.config).read_text(encoding="utf-8")))
    # CLI 覆盖 config
    if args.mode:
        spec["mode"] = args.mode
    if args.oracle:
        spec["kind"] = args.oracle
    if args.backend:
        spec["backend"] = args.backend
    if args.backends:
        spec["backends"] = args.backends.split(",")
    if args.suspects:
        spec["suspects"] = args.suspects.split(",")
    if args.match_error is not None:
        spec["match_error"] = args.match_error
    spec["runner"] = args.runner
    spec["qt_bin"] = args.qt_bin
    spec["compile_timeout"] = args.compile_timeout
    spec["run_timeout"] = args.run_timeout
    if "mode" not in spec:
        raise SystemExit("错误：需通过 --mode 或 --config 指定失败模式（mode）。")
    return spec


def cmd_reduce(args):
    src = Path(args.file).read_text(encoding="utf-8", errors="replace")
    oracle = Oracle(_oracle_spec_from_args(args))
    try:
        minimal = reduce_source(src, oracle, target_lines=args.target_lines,
                                max_calls=args.max_calls, verbose=not args.quiet)
    finally:
        pass

    if args.out:
        Path(args.out).write_text(minimal, encoding="utf-8", newline="\n")
        print(f"[reduce] 最小用例已写入 {args.out}", file=sys.stderr)
    if not args.no_print and not args.out:
        sys.stdout.write(minimal)

    if args.register:
        name = args.name or Path(args.file).stem
        p = register_case(name, minimal, oracle, status=args.status,
                          origin=args.file, notes=args.notes or "")
        print(f"[reduce] 已登记回归用例：{p}（status={args.status}），"
              f"并写入 {MANIFEST}", file=sys.stderr)
    oracle.cleanup()
    return 0


# ============================================================
# CLI: verify（CTest 用）
# ============================================================
def cmd_verify(args):
    """读取 manifest[index] 的判据，验证目标 bug 是否已消失。

    退出码：0 = bug 已消失（判据不再成立，correct）；1 = bug 仍在（判据成立）。
    配合 CMake：status=open → WILL_FAIL 包裹（bug 在→exit1→CTest PASS；
    修复后→exit0→WILL_FAIL 记 FAIL 提醒转正）。status=fixed → 直接要求 exit0。
    """
    data = _load_manifest()
    cases = data.get("cases", [])
    if args.index < 0 or args.index >= len(cases):
        print(f"错误：index {args.index} 超出 manifest 用例范围（0..{len(cases) - 1}）",
              file=sys.stderr)
        return 2
    case = cases[args.index]

    file_path = args.file or str(REGRESSION_DIR / case["file"])
    spec = dict(case)
    spec["runner"] = args.runner
    spec["qt_bin"] = args.qt_bin
    spec["compile_timeout"] = args.compile_timeout
    spec["run_timeout"] = args.run_timeout
    oracle = Oracle(spec)
    present = oracle.interesting_path(file_path)
    oracle.cleanup()

    verdict = {
        "case": case.get("name"), "file": file_path, "status": case.get("status"),
        "bug_present": present, "observed": oracle.last,
    }
    print(json.dumps(verdict, ensure_ascii=False, indent=2))
    # bug 仍在 → 1；已消失 → 0
    return 1 if present else 0


# ============================================================
# 参数解析
# ============================================================
def build_parser():
    p = argparse.ArgumentParser(
        prog="reduce.py",
        description="MiniLang 触发用例自动缩减（阶段五，C-Reduce 思路）")
    sub = p.add_subparsers(dest="command", required=True)

    def add_oracle_opts(sp):
        sp.add_argument("--runner", default=None, help="mini_diff_runner 路径（默认自动探测）")
        sp.add_argument("--qt-bin", dest="qt_bin", default=None, help="Qt 运行期 DLL 目录")
        sp.add_argument("--compile-timeout", type=float, default=10.0)
        sp.add_argument("--run-timeout", type=float, default=10.0)

    # reduce
    r = sub.add_parser("reduce", help="缩减一个触发 bug 的源程序")
    r.add_argument("--file", required=True, help="触发 bug 的 .mini 源文件")
    r.add_argument("--config", default=None, help="趣味性判定 JSON 配置（CLI 参数可覆盖）")
    r.add_argument("--mode", default=None,
                   help="失败模式：diff 类 %s；single 类 status %s" %
                        (list(DIFF_MODES), list(SINGLE_STATUSES)))
    r.add_argument("--oracle", choices=("diff", "single"), default=None,
                   help="判定方式：diff 多后端差分 / single 单后端 status（默认：给 --backend 则 single）")
    r.add_argument("--backend", default=None, help="single 模式后端名")
    r.add_argument("--backends", default=None, help="diff 模式后端子集（逗号分隔，默认 strict 四后端）")
    r.add_argument("--suspects", default=None, help="diff 模式要求的少数派后端（逗号分隔，锁定同一根因）")
    r.add_argument("--match-error", dest="match_error", default=None,
                   help="关键错误文本子串（锁定同一根因，建议用 ASCII 片段）")
    r.add_argument("--target-lines", type=int, default=30, help="目标行数上限（默认 30）")
    r.add_argument("--max-calls", type=int, default=100000, help="趣味性判定次数上限（兜底）")
    r.add_argument("--out", default=None, help="最小用例输出文件（默认打印到 stdout）")
    r.add_argument("--no-print", action="store_true", help="不向 stdout 打印最小用例")
    r.add_argument("--quiet", action="store_true", help="静默（不打印缩减进度）")
    r.add_argument("--register", action="store_true",
                   help="成功后拷贝进 testing/regression/ 并登记 CTest 回归 manifest")
    r.add_argument("--name", default=None, help="回归用例名（默认取输入文件名）")
    r.add_argument("--status", choices=("open", "fixed"), default="open",
                   help="回归状态：open→WILL_FAIL（bug 仍在）；fixed→必须通过")
    r.add_argument("--notes", default=None, help="回归用例备注")
    add_oracle_opts(r)
    r.set_defaults(func=cmd_reduce)

    # verify
    v = sub.add_parser("verify", help="CTest 用：验证 manifest 某用例的 bug 是否已消失")
    v.add_argument("--manifest", default=str(MANIFEST), help="manifest.json 路径")
    v.add_argument("--index", type=int, required=True, help="用例在 manifest.cases 中的下标")
    v.add_argument("--file", default=None, help="覆盖用例源文件路径（默认取 regression/<file>）")
    add_oracle_opts(v)
    v.set_defaults(func=cmd_verify)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    # verify 支持自定义 manifest 路径
    if getattr(args, "manifest", None):
        global MANIFEST
        MANIFEST = Path(args.manifest)
    try:
        return args.func(args)
    except FileNotFoundError as e:
        print(json.dumps({"error": str(e)}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
