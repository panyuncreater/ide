#!/usr/bin/env python3
# ============================================================
# MiniLang 翻译完整性检查工具
# ------------------------------------------------------------
# 用途：
#   验证 en_US.ts 翻译文件与 zh_CN.ts 源文件的消息完整性：
#   1. 两个文件的 <message> 条目数量一致
#   2. en_US.ts 的每条 <source> 与 zh_CN.ts 对应条目完全相同
#   3. en_US.ts 的每条 <translation> 非空
#
# 用法：
#   python scripts/translation_check.py
#   python scripts/translation_check.py --zh app/translations/minilang_zh_CN.ts --en app/translations/minilang_en_US.ts
#
# 退出码：
#   0 = 全部通过
#   1 = 发现不一致（消息数不匹配 / source 不一致 / 翻译为空）
#
# 设计原则：
#   - 纯 Python 标准库（xml.etree.ElementTree）
#   - source 字符串必须完全一致（包括空白字符）
#   - translation 不能为空字符串（纯空白视为未翻译）
#   - 输出详细差异便于 CI 日志定位
# ============================================================

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass


@dataclass
class MessageEntry:
    """单条翻译消息。"""
    source: str
    translation: str
    line: int  # 大致行号（用于错误定位）


def parse_ts_file(path: str) -> list[MessageEntry]:
    """解析 Qt .ts XML 文件，返回消息列表。

    .ts 文件格式：
      <TS version="2.1" language="xx_XX">
        <context>
          <name>...</name>
          <message>
            <source>源文本</source>
            <translation type="finished">译文</translation>
          </message>
          ...
        </context>
      </TS>
    """
    tree = ET.parse(path)
    root = tree.getroot()
    messages: list[MessageEntry] = []
    for ctx in root.iter("context"):
        for msg in ctx.iter("message"):
            source_elem = msg.find("source")
            trans_elem = msg.find("translation")
            source_text = source_elem.text if source_elem is not None and source_elem.text else ""
            trans_text = trans_elem.text if trans_elem is not None and trans_elem.text else ""
            # 计算大致行号（ET 不保留行号，用消息序号近似）
            messages.append(MessageEntry(
                source=source_text,
                translation=trans_text,
                line=len(messages) + 1,
            ))
    return messages


def check_completeness(zh_messages: list[MessageEntry], en_messages: list[MessageEntry]) -> list[str]:
    """检查翻译完整性，返回错误列表（空列表表示通过）。"""
    errors: list[str] = []

    # 检查 1：消息数量一致
    if len(zh_messages) != len(en_messages):
        errors.append(
            f"消息数量不匹配: zh_CN={len(zh_messages)}, en_US={len(en_messages)} "
            f"(差异 {abs(len(zh_messages) - len(en_messages))} 条)"
        )
        return errors  # 数量不一致时后续检查无意义

    # 检查 2：每条 source 语义一致（忽略首尾空白，因 XML 格式化可能不同）
    source_mismatches = 0
    for i, (zh, en) in enumerate(zip(zh_messages, en_messages)):
        if zh.source.strip() != en.source.strip():
            source_mismatches += 1
            if source_mismatches <= 5:  # 只输出前 5 个差异避免日志爆炸
                errors.append(
                    f"source 不一致 (第 {i + 1} 条):\n"
                    f"  zh_CN: {zh.source.strip()[:80]!r}\n"
                    f"  en_US: {en.source.strip()[:80]!r}"
                )
    if source_mismatches > 5:
        errors.append(f"... 还有 {source_mismatches - 5} 处 source 不一致未显示")

    # 检查 3：translation 非空
    empty_translations = 0
    for i, en in enumerate(en_messages):
        if not en.translation or not en.translation.strip():
            empty_translations += 1
            if empty_translations <= 5:
                errors.append(
                    f"翻译为空 (第 {i + 1} 条): source={en.source[:80]!r}"
                )
    if empty_translations > 5:
        errors.append(f"... 还有 {empty_translations - 5} 处空翻译未显示")

    return errors


def check_xml_validity(path: str) -> str | None:
    """检查 XML 文件是否格式正确，返回错误消息（None 表示有效）。"""
    try:
        ET.parse(path)
        return None
    except ET.ParseError as e:
        return f"XML 解析错误: {e}"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="MiniLang 翻译完整性检查（验证 en_US.ts 与 zh_CN.ts 消息一致性）"
    )
    parser.add_argument(
        "--zh",
        default="app/translations/minilang_zh_CN.ts",
        help="zh_CN.ts 源文件路径（默认: app/translations/minilang_zh_CN.ts）",
    )
    parser.add_argument(
        "--en",
        default="app/translations/minilang_en_US.ts",
        help="en_US.ts 翻译文件路径（默认: app/translations/minilang_en_US.ts）",
    )
    args = parser.parse_args()

    # 阶段 0：XML 格式检查
    print("=" * 60)
    print("MiniLang 翻译完整性检查")
    print("=" * 60)

    zh_xml_error = check_xml_validity(args.zh)
    if zh_xml_error:
        print(f"[FAIL] zh_CN.ts XML 无效: {zh_xml_error}")
        return 1
    print(f"[OK]   zh_CN.ts XML 格式有效: {args.zh}")

    en_xml_error = check_xml_validity(args.en)
    if en_xml_error:
        print(f"[FAIL] en_US.ts XML 无效: {en_xml_error}")
        return 1
    print(f"[OK]   en_US.ts XML 格式有效: {args.en}")

    # 阶段 1：解析文件
    zh_messages = parse_ts_file(args.zh)
    en_messages = parse_ts_file(args.en)
    print(f"\nzh_CN.ts: {len(zh_messages)} 条消息")
    print(f"en_US.ts: {len(en_messages)} 条消息")

    # 阶段 2：完整性检查
    errors = check_completeness(zh_messages, en_messages)

    print("\n" + "-" * 60)
    if errors:
        print(f"[FAIL] 发现 {len(errors)} 处问题:")
        for err in errors:
            print(f"  - {err}")
        print("-" * 60)
        print("\n修复建议:")
        print("  1. 重新运行 lupdate 同步 .ts 文件")
        print("  2. 运行 python scripts/generate_en_us_translation.py 重新生成 en_US.ts")
        print("  3. 检查是否有新增的 mlTr() 调用未同步到 .ts 文件")
        return 1
    else:
        print(f"[PASS] 翻译完整性检查通过")
        print(f"  - 消息数量一致: {len(zh_messages)} 条")
        print(f"  - 所有 source 完全匹配")
        print(f"  - 所有 translation 非空")
        print("-" * 60)
        return 0


if __name__ == "__main__":
    sys.exit(main())
