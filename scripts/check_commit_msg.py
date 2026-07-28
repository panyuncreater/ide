#!/usr/bin/env python3
"""
Conventional Commits 验证脚本。

验证 commit message 是否符合 Conventional Commits 格式：
  <type>[(scope)]: <description>

支持的 type:
  feat, fix, refactor, perf, docs, test, chore, ci, build, style, revert

用法:
  # 验证单条消息
  python scripts/check_commit_msg.py --message "feat: add new feature"

  # 验证文件中的消息（git commit-msg hook）
  python scripts/check_commit_msg.py --file .git/COMMIT_EDITMSG

  # 验证 PR 的所有 commit（CI 用）
  python scripts/check_commit_msg.py --range origin/main..HEAD

退出码:
  0  所有消息合规
  1  存在不合规消息
"""
import argparse
import re
import subprocess
import sys

# Conventional Commits 正则
# <type>[(scope)][!]: <description>
PATTERN = re.compile(
    r"^(feat|fix|refactor|perf|docs|test|chore|ci|build|style|revert)"
    r"(\([a-zA-Z0-9_/.-]+\))?"  # optional scope
    r"!?"                        # optional breaking change marker
    r": .{1,100}"               # colon + space + description (max 100 chars)
)

# 允许的特殊前缀（Merge commits、Revert commits）
SPECIAL_PREFIXES = [
    "Merge ",
    "Revert \"",
    "Revert: ",
]


def validate_message(msg: str) -> tuple[bool, str]:
    """验证单条 commit message。返回 (valid, reason)。"""
    # 取第一行
    first_line = msg.strip().split("\n")[0].strip()
    if not first_line:
        return False, "commit message 为空"

    # 允许特殊前缀
    for prefix in SPECIAL_PREFIXES:
        if first_line.startswith(prefix):
            return True, "特殊前缀（Merge/Revert）"

    if PATTERN.match(first_line):
        return True, "合规"
    else:
        return False, (
            f"不符合 Conventional Commits 格式。\n"
            f"  实际: {first_line}\n"
            f"  期望: <type>[(scope)]: <description>\n"
            f"  type: feat|fix|refactor|perf|docs|test|chore|ci|build|style|revert"
        )


def main():
    parser = argparse.ArgumentParser(description="Conventional Commits 验证")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--message", "-m", help="直接验证消息字符串")
    group.add_argument("--file", "-f", help="从文件读取消息（commit-msg hook）")
    group.add_argument("--range", "-r", help="Git commit range（如 origin/main..HEAD）")
    args = parser.parse_args()

    messages: list[tuple[str, str]] = []  # (commit_id_or_source, message)

    if args.message:
        messages.append(("cli", args.message))
    elif args.file:
        try:
            with open(args.file, "r", encoding="utf-8") as f:
                content = f.read()
            messages.append((args.file, content))
        except FileNotFoundError:
            print(f"错误: 文件不存在: {args.file}", file=sys.stderr)
            sys.exit(1)
    elif args.range:
        try:
            result = subprocess.run(
                ["git", "log", "--format=%H %s", args.range],
                capture_output=True, text=True, check=True
            )
            for line in result.stdout.strip().split("\n"):
                if line.strip():
                    parts = line.split(" ", 1)
                    sha = parts[0][:8]
                    msg = parts[1] if len(parts) > 1 else ""
                    messages.append((sha, msg))
        except subprocess.CalledProcessError as e:
            print(f"错误: git log 失败: {e.stderr}", file=sys.stderr)
            sys.exit(1)

    if not messages:
        print("无 commit 需要验证")
        sys.exit(0)

    failures: list[tuple[str, str]] = []
    for source, msg in messages:
        valid, reason = validate_message(msg)
        if not valid:
            failures.append((source, reason))

    if failures:
        print(f"[FAIL] {len(failures)}/{len(messages)} 条 commit message 不合规:\n")
        for source, reason in failures:
            print(f"  [{source}] {reason}\n")
        sys.exit(1)
    else:
        print(f"[OK] {len(messages)} 条 commit message 全部合规")
        sys.exit(0)


if __name__ == "__main__":
    main()
