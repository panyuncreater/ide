#!/usr/bin/env python3
"""
从 cmake/minilang_core.cmake 的 target 依赖关系自动生成 Mermaid 格式架构图。

输出: docs/architecture_auto.md（不覆盖手写 architecture.md）

用法:
  python scripts/generate_arch_diagram.py
"""
import re
import sys
from pathlib import Path


def parse_cmake_targets(cmake_path: Path) -> dict[str, list[str]]:
    """解析 cmake 文件中的 target 依赖关系。"""
    content = cmake_path.read_text(encoding="utf-8")

    # 提取 add_library 定义的 target 名称
    targets: dict[str, list[str]] = {}

    # 匹配 target_link_libraries(target_name PUBLIC/PRIVATE dep1 dep2 ...)
    link_pattern = re.compile(
        r"target_link_libraries\(\s*(\w+)\s+(?:PUBLIC|PRIVATE)\s+(.*?)\)",
        re.DOTALL
    )

    for match in link_pattern.finditer(content):
        target = match.group(1)
        deps_str = match.group(2)
        # 提取 minilang_* 依赖（排除 Qt6::Core 等外部依赖）
        deps = [
            d.strip() for d in re.split(r"\s+", deps_str.strip())
            if d.strip().startswith("minilang_") and d.strip() != target
        ]
        if target.startswith("minilang_"):
            if target not in targets:
                targets[target] = []
            targets[target].extend(deps)

    # 去重
    for t in targets:
        targets[t] = list(dict.fromkeys(targets[t]))

    return targets


def generate_mermaid(targets: dict[str, list[str]]) -> str:
    """生成 Mermaid 图。"""
    lines = ["graph TB"]

    # 节点别名（短名称）
    aliases = {}
    for t in sorted(targets.keys()):
        short = t.replace("minilang_", "")
        aliases[t] = short

    # 添加边
    for target, deps in sorted(targets.items()):
        src = aliases.get(target, target)
        for dep in deps:
            dst = aliases.get(dep, dep)
            if dst != src:
                lines.append(f"    {src}[{target}] --> {dst}[{dep}]")

    # 添加外部消费者
    lines.append("")
    lines.append("    ide[minilang_ide] --> core[minilang_core]")
    lines.append("    tests[minilang_tests] --> core[minilang_core]")
    lines.append("    cli_tools[CLI tools] --> core[minilang_core]")

    return "\n".join(lines)


def main():
    project_root = Path(__file__).resolve().parent.parent
    cmake_path = project_root / "cmake" / "minilang_core.cmake"

    if not cmake_path.exists():
        print(f"错误: 找不到 {cmake_path}", file=sys.stderr)
        sys.exit(1)

    targets = parse_cmake_targets(cmake_path)
    mermaid = generate_mermaid(targets)

    output_path = project_root / "docs" / "architecture_auto.md"
    output_content = f"""# MiniLang 模块依赖图（自动生成）

> 由 `scripts/generate_arch_diagram.py` 从 `cmake/minilang_core.cmake` 自动生成。
> 不要手动编辑此文件。手写架构文档请参见 [architecture.md](architecture.md)。

```mermaid
{mermaid}
```

## 模块说明

| 模块 | 职责 |
|------|------|
| minilang_core_base | 强连通核心（common + ast + interpreter + debug） |
| minilang_frontend | 前端工具集（lexer + parser + formatter + lint + doc） |
| minilang_backend | 编译器/三后端（compiler/*.cpp，含 JIT） |
| minilang_guibridge | app 桥接层（MagicCommands + ExecutionTraceRecorder） |
| minilang_core | STATIC 聚合（合并上述 4 个 OBJECT 库） |
| minilang_ide | GUI 应用主目标 |
| minilang_tests | GoogleTest 单元测试 |
| CLI tools | 命令行工具（fmt/lint/lsp/dap/fuzz/pkg/compile/coverage/doc） |
"""

    output_path.write_text(output_content, encoding="utf-8")
    print(f"[OK] 架构图已生成: {output_path}")


if __name__ == "__main__":
    main()
