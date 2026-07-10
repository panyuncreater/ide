"""将课程设计报告 Markdown 转为 Word docx。

依赖：pypandoc-binary（自带 pandoc 二进制，无需系统安装 pandoc）。
用法：py -3 scripts\md_to_docx.py
"""
import os
import sys
import pypandoc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "课程设计报告.md")
DST = os.path.join(ROOT, "课程设计报告.docx")


def main():
    if not os.path.isfile(SRC):
        print(f"ERROR: 源文件不存在: {SRC}", file=sys.stderr)
        return 1
    print(f"Pandoc version: {pypandoc.get_pandoc_version()}")
    print(f"Converting: {SRC}")
    print(f"      -> {DST}")
    # --resource-path 让 pandoc 在项目根目录查找图片（docs/screenshots/*.png 等）
    # --standalone 生成完整文档
    pypandoc.convert_file(
        SRC,
        "docx",
        format="markdown",
        outputfile=DST,
        extra_args=[
            "--standalone",
            f"--resource-path={ROOT}",
            "--wrap=none",
        ],
    )
    size_kb = os.path.getsize(DST) / 1024
    print(f"DONE: {DST} ({size_kb:.1f} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
