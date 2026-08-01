#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scripts/sync_i18n_zh.py — 把缺失的 mlTr 字符串增量同步进 zh_CN.ts（源语言文件）。

用法：
    python scripts/sync_i18n_zh.py --strings <missing.txt> [--ts app/translations/minilang_zh_CN.ts]

--strings 每行一条 Python repr 字符串（如 '反向调试时间轴'，含换行的串以
\\n 转义单行存放，来自 check_i18n.py 缺失清单的 repr 输出），已存在的条目
自动跳过。同步后运行：
    python scripts/generate_en_us_translation.py   # 重新生成 en_US.ts
    python scripts/translation_check.py            # 验证 zh/en 一致
    python scripts/check_i18n.py                   # 验证无硬性错误、未同步数回落

实现说明：纯文本插入（保留文件头 DOCTYPE/注释/引号格式），不经过
xml.etree 序列化——ET.write 会改写头声明并重排缩进，产生超大 diff。
lupdate 不识别自定义 mlTr() 调用，zh_CN.ts 由本脚本 + 手工维护，
en_US.ts 由 generate_en_us_translation.py 从 zh_CN.ts 派生。
"""
import argparse
import ast
import pathlib
import sys

TS_DEFAULT = "app/translations/minilang_zh_CN.ts"


def escape_xml(text: str) -> str:
    return (text.replace("&", "&amp;")
                .replace("<", "&lt;")
                .replace(">", "&gt;"))


def main() -> int:
    parser = argparse.ArgumentParser(description="增量同步缺失字符串到 zh_CN.ts")
    parser.add_argument("--strings", required=True, help="缺失字符串列表文件（每行一条 Python repr）")
    parser.add_argument("--ts", default=TS_DEFAULT, help="zh_CN.ts 路径")
    args = parser.parse_args()

    ts_path = pathlib.Path(args.ts)
    text = ts_path.read_text(encoding="utf-8")
    context_close = "</context>"
    idx = text.find(context_close)
    if idx < 0:
        print("ERROR: zh_CN.ts 缺少 </context>，结构异常", file=sys.stderr)
        return 1

    with open(args.strings, "r", encoding="utf-8") as f:
        wanted = [ast.literal_eval(ln) for ln in f if ln.strip()]

    # 现有 source 集合（与 check_i18n.py 同口径：findtext("source") or ""）
    import xml.etree.ElementTree as ET
    existing = {m.findtext("source") or "" for m in ET.parse(ts_path).iter("message")}

    added = []
    skipped = 0
    for s in wanted:
        if s in existing:
            skipped += 1
            continue
        src_esc = escape_xml(s)
        block = (
            "    <message>\n"
            f"        <source>{src_esc}</source>\n"
            f"        <translation type=\"finished\">{src_esc}</translation>\n"
            "    </message>\n"
        )
        text = text[:idx] + block + text[idx:]
        idx += len(block)  # </context> 位置随插入后移
        added.append(s)

    if added:
        ts_path.write_text(text, encoding="utf-8", newline="\n")
    print(f"zh_CN.ts 同步完成：新增 {len(added)} 条，已存在跳过 {skipped} 条，总计 {len(existing) + len(added)} 条")
    if added:
        print("请随后运行：")
        print("  python scripts/generate_en_us_translation.py")
        print("  python scripts/translation_check.py")
        print("  python scripts/check_i18n.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
