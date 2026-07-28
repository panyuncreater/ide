#!/usr/bin/env python3
# ============================================================
# scripts/check_i18n.py — i18n 翻译完整性检查（拓展二期·平台）
# ------------------------------------------------------------
# 两层检查（不依赖 Qt lupdate，纯 Python，可在任何 CI runner 跑）：
#   1. .ts 完整性：minilang_en_US.ts / minilang_zh_CN.ts 中不得存在
#      type="unfinished" 或空 <translation>（en_US 视为待翻译回归）。
#   2. 源码同步性：从 app/ gui/ 源码提取全部 mlTr("...") / 
#      mlTrCtx("ctx","...") 字符串字面量，检查每条都在 en_US.ts 的
#      <source> 集合中——捕获「新增 UI 字符串但忘记跑 lupdate 同步」。
#
# 退出码：0 = 通过；1 = 发现问题（清单打印到 stdout）。
# 用法：python scripts/check_i18n.py [--repo-root DIR] [--max-missing N]
#
# ratchet 基线：存量欠账（历史上未跑 lupdate 同步的字符串）通过
# --max-missing 阀値固定，新增未同步字符串会超过阈値即红；
# 每次补同步后应下调阈値（只减不增）。
# ============================================================
import argparse
import io
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

# Windows 控制台默认 GBK 无法输出 emoji/全部 Unicode，强制 UTF-8（替换不可编码字符）
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

TS_FILES = ["app/translations/minilang_en_US.ts", "app/translations/minilang_zh_CN.ts"]
SOURCE_DIRS = ["app", "gui"]

# mlTr("...") / mlTrCtx("ctx", "...")：支持相邻字符串字面量拼接（C++ 预处理拼接）
STRING_LIT = r'"(?:[^"\\]|\\.)*"'
ML_TR_RE = re.compile(r'\bmlTr\(\s*(' + STRING_LIT + r'(?:\s*' + STRING_LIT + r')*)\s*\)')
ML_TRCTX_RE = re.compile(
    r'\bmlTrCtx\(\s*' + STRING_LIT + r'\s*,\s*(' + STRING_LIT + r'(?:\s*' + STRING_LIT + r')*)\s*\)')


def unescape_cpp(concat: str) -> str:
    """把「相邻 C++ 字符串字面量拼接」还原为运行时字符串（处理常见转义）。"""
    parts = re.findall(STRING_LIT, concat)
    out = []
    for p in parts:
        body = p[1:-1]
        body = (body.replace(r"\\", "\x00")
                    .replace(r"\n", "\n")
                    .replace(r"\t", "\t")
                    .replace(r"\"", '"')
                    .replace("\x00", "\\"))
        out.append(body)
    return "".join(out)


def collect_sources(root: pathlib.Path):
    """提取源码中全部 mlTr/mlTrCtx 字符串 → {字符串: 首个出现位置}。"""
    found = {}
    for d in SOURCE_DIRS:
        for f in sorted((root / d).rglob("*.cpp")) + sorted((root / d).rglob("*.h")):
            try:
                text = f.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for regex in (ML_TR_RE, ML_TRCTX_RE):
                for m in regex.finditer(text):
                    s = unescape_cpp(m.group(1))
                    if s and s not in found:
                        line = text.count("\n", 0, m.start()) + 1
                        found[s] = f"{f.relative_to(root)}:{line}"
    return found


def check_ts(root: pathlib.Path, ts_rel: str, errors: list):
    """检查单个 .ts：unfinished / 空翻译，返回 <source> 集合。"""
    path = root / ts_rel
    if not path.exists():
        errors.append(f"[缺失] {ts_rel} 不存在")
        return set()
    # 安全防护：.ts 是仓内自有文件（可信），但仍拒绝 DTD，
    # 避免实体扩展/外部实体类攻击面（lupdate 从不生成 DOCTYPE 内容实体）
    head = path.read_text(encoding="utf-8", errors="ignore")[:2048]
    if "<!ENTITY" in head:
        errors.append(f"[拒绝] {ts_rel} 含自定义实体声明，拒绝解析")
        return set()
    tree = ET.parse(path)
    sources = set()
    for message in tree.iter("message"):
        src_el = message.find("source")
        tr_el = message.find("translation")
        src = (src_el.text or "") if src_el is not None else ""
        sources.add(src)
        if tr_el is None:
            errors.append(f"[无 translation] {ts_rel}: {src[:60]!r}")
            continue
        if tr_el.get("type") == "unfinished":
            errors.append(f"[unfinished] {ts_rel}: {src[:60]!r}")
        elif tr_el.get("type") != "vanished" and not (tr_el.text or "").strip():
            # vanished（源码已删的历史条目）允许为空；正常条目必须有翻译
            errors.append(f"[空翻译] {ts_rel}: {src[:60]!r}")
    return sources


def main() -> int:
    parser = argparse.ArgumentParser(description="MiniLang i18n 翻译完整性检查")
    parser.add_argument("--repo-root", default=".", help="仓库根目录")
    # ratchet 基线：存量未同步欠账（2026-07-28 盘点为 806 条，历史上
    # 部分面板字符串未跑 lupdate）。新增未同步字符串使缺口超过阈値即红；
    # 补同步后应下调此默认値（只减不增）。
    parser.add_argument("--max-missing", type=int, default=806,
                        help="允许的未同步字符串上限（ratchet 基线，只减不增）")
    args = parser.parse_args()
    root = pathlib.Path(args.repo_root).resolve()

    # 硬性错误：unfinished/空翻译/实体声明 —— 任何一条都红（已有翻译不得回退）
    hard_errors = []
    en_sources = set()
    for ts in TS_FILES:
        sources = check_ts(root, ts, hard_errors)
        if "en_US" in ts:
            en_sources = sources

    # 同步性缺口：源码 mlTr 字符串未进 en_US.ts（ratchet 阈値控制）
    code_strings = collect_sources(root)
    missing = {s: loc for s, loc in code_strings.items() if s not in en_sources}

    print(f"i18n 检查：源码 mlTr/mlTrCtx 字符串 {len(code_strings)} 条，"
          f"en_US.ts source {len(en_sources)} 条，"
          f"硬性问题 {len(hard_errors)} 条，未同步 {len(missing)}/{args.max_missing} 条")

    failed = False
    if hard_errors:
        failed = True
        for e in hard_errors[:40]:
            print("  " + e)
        if len(hard_errors) > 40:
            print(f"  ...（其余 {len(hard_errors) - 40} 条省略）")
    if len(missing) > args.max_missing:
        failed = True
        print(f"未同步字符串 {len(missing)} 条超过 ratchet 阈値 {args.max_missing}（新增 UI 字符串未跑 lupdate）：")
        for s, loc in sorted(missing.items(), key=lambda kv: kv[1])[:40]:
            print(f"  [未同步] {loc}: {s[:60]!r}")
    elif len(missing) < args.max_missing:
        print(f"提示：未同步数已降至 {len(missing)}，可下调 --max-missing 默认値锁住成果")

    if failed:
        print("失败：请运行 lupdate 同步 .ts 并补全翻译")
        return 1
    print("通过：翻译条目完整，未同步数在 ratchet 基线内")
    return 0


if __name__ == "__main__":
    sys.exit(main())
