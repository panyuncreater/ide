#!/usr/bin/env python3
"""MiniLang 知识卫生检查。

硬检查：预算、L0 历史泄漏、失效路径、重复事实、BACKLOG 格式、测试计数出处。
仅使用标准库；退出码 0 = 通过，1 = 有错误。
"""

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

LIVE = [
    "AGENTS.md",
    "README.md",
    "docs/architecture.md",
    "docs/development.md",
    "docs/testing.md",
    "docs/faq.md",
    "docs/getting-started.md",
    "docs/specs/backend-consistency.md",
    "docs/specs/memory-model.md",
    "docs/specs/testing-conventions.md",
    "docs/specs/defect-taxonomy.md",
    "docs/specs/rejected-decisions.md",
    "docs/BACKLOG.md",
    "docs/knowledge/INDEX.md",
    "docs/knowledge/ROUND-END.md",
]

SKILLS = sorted(str(p.relative_to(ROOT)).replace("\\", "/") for p in ROOT.glob(".qoder/skills/*/SKILL.md"))

REQUIRED = [
    "AGENTS.md",
    "docs/BACKLOG.md",
    "docs/specs/defect-taxonomy.md",
    "docs/specs/rejected-decisions.md",
    "docs/knowledge/INDEX.md",
    "docs/knowledge/ROUND-END.md",
    "docs/generated/test-inventory.md",
]

# (bytes, lines); None = 不检查
BUDGETS = {
    "AGENTS.md": (8192, 120),
    "docs/BACKLOG.md": (None, 200),
    "docs/specs/backend-consistency.md": (None, 160),
    "docs/specs/memory-model.md": (None, 160),
    "docs/specs/defect-taxonomy.md": (None, 160),
    "docs/specs/rejected-decisions.md": (None, 160),
    "docs/knowledge/INDEX.md": (None, 160),
    "docs/knowledge/ROUND-END.md": (None, 160),
}
SKILL_LINE_BUDGET = 150

AGENTS_BANNED = [
    (re.compile(r"\b20\d{2}-\d{2}-\d{2}\b"), "L0 禁止日期"),
    (re.compile(r"\b[0-9a-f]{7,40}\b"), "L0 禁止提交号/哈希"),
    (re.compile(r"(?:\b4\d{3}\s*(?:/|个|用例)|(?:tests?|ctest|测试)[^\n]{0,30}\b4\d{3}\b)"), "L0 禁止测试计数"),
    (re.compile(r"变更记录|剩余未修|维持登记|P[0-9] 清偿|提交 [0-9a-f]"), "L0 禁止轮次叙事"),
]

STALE_PATHS = [
    "compiler/VM.cpp",
    "compiler/RegisterVM.cpp",
    "compiler/BytecodeIRBackend.cpp",
    "compiler/JIT*.cpp",
    "tests/TestAuditBatch1-5.cpp",
    ".trae-cn",
]

COUNT_RE = re.compile(r"(?:\b4\d{3}\s*(?:/|个|用例)|(?:tests?|ctest|测试)[^\n]{0,30}\b4\d{3}\b)")
COUNT_ALLOW = ("CHANGELOG.md", "docs/audit/", "docs/code-audit-report-", "docs/generated/")

PATH_PREFIXES = (
    "docs/", "tests/", "test_harness/", "compiler/", "interpreter/", "common/",
    "gui/", "cli/", "app/", "parser/", "lexer/", "ast/", "debug/", "formatter/",
    "capi/", "samples/", "cmake/", ".qoder/", "scripts/", "third_party/",
)
PATH_SUFFIXES = (
    ".cpp", ".h", ".hpp", ".md", ".py", ".ps1", ".bat", ".sh", ".json", ".txt", ".mmd",
)
BACKTICK_RE = re.compile(r"`([^`\n]+)`")
STATUSES = {"open", "blocked", "verifying", "candidate", "done", "dropped"}

errors: list[str] = []
warnings: list[str] = []


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


def add_error(kind: str, msg: str) -> None:
    errors.append(f"ERROR [{kind}] {msg}")


def add_warning(kind: str, msg: str) -> None:
    warnings.append(f"WARN  [{kind}] {msg}")


def check_required() -> None:
    for rel in REQUIRED:
        if not (ROOT / rel).exists():
            add_error("required", f"缺少权威文件：{rel}")


def check_budgets() -> None:
    for rel, (byte_limit, line_limit) in BUDGETS.items():
        p = ROOT / rel
        if not p.exists():
            continue
        raw = p.read_bytes()
        lines = raw.decode("utf-8", errors="replace").count("\n") + 1
        if byte_limit is not None and len(raw) > byte_limit:
            add_error("budget", f"{rel}: {len(raw)} 字节 > {byte_limit}")
        if line_limit is not None and lines > line_limit:
            add_error("budget", f"{rel}: {lines} 行 > {line_limit}")
    for rel in SKILLS:
        p = ROOT / rel
        lines = p.read_text(encoding="utf-8", errors="replace").count("\n") + 1
        if lines > SKILL_LINE_BUDGET:
            add_error("budget", f"{rel}: {lines} 行 > {SKILL_LINE_BUDGET}")


def check_agents() -> None:
    rel = "AGENTS.md"
    if not (ROOT / rel).exists():
        return
    text = read(rel)
    for pattern, label in AGENTS_BANNED:
        m = pattern.search(text)
        if m:
            add_error("history", f"{rel}: {label}：`{m.group(0)}`")
    if "## 8. 变更记录" in text or "## 7. 记忆归档" in text:
        add_error("history", f"{rel}: 仍含已退役章节（变更记录/记忆归档）")


def check_count_sources() -> None:
    for rel in LIVE:
        if rel.startswith(COUNT_ALLOW):
            continue
        text = read(rel)
        for m in COUNT_RE.finditer(text):
            add_error("count", f"{rel}: 测试计数 `{m.group(0)}` 只能出现在生成文件/档案")


def check_stale_paths() -> None:
    targets = LIVE + SKILLS
    for rel in targets:
        if not (ROOT / rel).exists():
            continue
        text = read(rel)
        for stale in STALE_PATHS:
            if stale in text:
                add_error("stale", f"{rel}: 失效引用 `{stale}`")


def is_checkable_path(token: str) -> bool:
    token = token.strip()
    if not token or any(ch in token for ch in " <>|()$*"):
        return False
    if token.startswith(("http://", "https://", "file:")) or token.startswith("../"):
        return False
    if not token.startswith(PATH_PREFIXES):
        return False
    if not token.endswith(PATH_SUFFIXES):
        return False
    if ".h/.cpp" in token or ".cpp/.h" in token:
        return False
    return True


def check_paths() -> None:
    targets = ["AGENTS.md"] + [r for r in LIVE if r.startswith("docs/specs/")] + SKILLS
    for rel in targets:
        if not (ROOT / rel).exists():
            continue
        text = read(rel)
        for token in BACKTICK_RE.findall(text):
            if not is_checkable_path(token):
                continue
            if not (ROOT / token).exists():
                add_error("path", f"{rel}: 路径不存在 `{token}`")


def check_duplicates() -> None:
    seen: dict[str, set[str]] = defaultdict(set)
    for rel in LIVE:
        if rel == "docs/BACKLOG.md":
            continue
        text = read(rel)
        for line in text.splitlines():
            norm = re.sub(r"\s+", " ", line.strip())
            if len(norm) < 120 or set(norm) <= {"-", "|", ":", " "}:
                continue
            seen[norm].add(rel)
    for norm, files in seen.items():
        if len(files) > 1:
            add_error("duplicate", f"{len(files)} 个文件含相同长句：{sorted(files)} — {norm[:80]}...")


def check_backlog() -> None:
    rel = "docs/BACKLOG.md"
    if not (ROOT / rel).exists():
        return
    ids: set[str] = set()
    for lineno, line in enumerate(read(rel).splitlines(), 1):
        if not re.match(r"^\| (B|OPT)-\d{3} ", line):
            continue
        parts = [p.strip() for p in line.strip().strip("|").split("|")]
        if len(parts) != 6:
            add_error("backlog", f"{rel}:{lineno}: 期望 6 列，实际 {len(parts)}")
            continue
        item_id, severity, status, _topic, evidence, next_step = parts
        if item_id in ids:
            add_error("backlog", f"{rel}:{lineno}: ID 重复 {item_id}")
        ids.add(item_id)
        if severity not in {"P0", "P1", "P2", "P3"}:
            add_error("backlog", f"{rel}:{lineno}: 非法严重性 {severity}")
        if status not in STATUSES:
            add_error("backlog", f"{rel}:{lineno}: 非法状态 {status}")
        if not evidence or not next_step:
            add_error("backlog", f"{rel}:{lineno}: 证据/下一步不得为空")


def check_legacy_sections() -> None:
    rel = "docs/development.md"
    if (ROOT / rel).exists() and "## 最近变更摘要" in read(rel):
        add_error("history", f"{rel}: 仍含重复的“最近变更摘要”章节")


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    check_required()
    check_budgets()
    check_agents()
    check_count_sources()
    check_stale_paths()
    check_paths()
    check_duplicates()
    check_backlog()
    check_legacy_sections()

    for line in errors + warnings:
        print(line)
    print(f"\n结果：{len(errors)} errors, {len(warnings)} warnings")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
