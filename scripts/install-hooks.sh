#!/usr/bin/env bash
# ============================================================
# MiniLang git hooks 安装脚本
# ------------------------------------------------------------
# 用途：将 scripts/pre-commit-hook.{sh,ps1} 链接到 .git/hooks/pre-commit
#
# 用法（在仓库根执行）：
#   ./scripts/install-hooks.sh          # Linux/macOS/Git Bash
#   ./scripts/install-hooks.ps1         # Windows PowerShell
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOOKS_DIR="$PROJECT_ROOT/.git/hooks"

if [ ! -d "$HOOKS_DIR" ]; then
    echo "[ERROR] .git/hooks 不存在：$HOOKS_DIR"
    echo "        请在仓库根目录运行本脚本"
    exit 1
fi

TARGET="$SCRIPT_DIR/pre-commit-hook.sh"
if [ ! -f "$TARGET" ]; then
    echo "[ERROR] 未找到 $TARGET"
    exit 1
fi

# 创建符号链接（避免复制后修改不同步）
ln -sf "$TARGET" "$HOOKS_DIR/pre-commit"
chmod +x "$TARGET" "$HOOKS_DIR/pre-commit"

echo "[OK] pre-commit hook 已安装到 $HOOKS_DIR/pre-commit"
echo "     实际脚本：$TARGET"
echo ""
echo "测试：修改一个 .cpp 文件不格式化，git commit 应被阻止"
echo "卸载：rm $HOOKS_DIR/pre-commit"
echo "跳过：git commit --no-verify"
