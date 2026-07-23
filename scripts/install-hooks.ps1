# ============================================================
# MiniLang git hooks 安装脚本（Windows PowerShell）
# ------------------------------------------------------------
# 用途：将 scripts/pre-commit-hook.ps1 链接到 .git/hooks/pre-commit
#
# 用法（在仓库根执行）：
#   powershell -ExecutionPolicy Bypass -File scripts\install-hooks.ps1
# ============================================================

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = (Resolve-Path "$ScriptDir\..").Path
$HooksDir = Join-Path $ProjectRoot ".git\hooks"

if (-not (Test-Path $HooksDir)) {
    Write-Host "[ERROR] .git\hooks 不存在：$HooksDir"
    Write-Host "        请在仓库根目录运行本脚本"
    exit 1
}

# Git for Windows 直接执行 .sh 文件即可（通过 bash），无需 PowerShell 包装
# 但为了让 hook 在纯 Windows 环境也能跑，我们生成一个 wrapper
$HookScript = Join-Path $HooksDir "pre-commit"
$Ps1Source = Join-Path $ScriptDir "pre-commit-hook.ps1"

if (-not (Test-Path $Ps1Source)) {
    Write-Host "[ERROR] 未找到 $Ps1Source"
    exit 1
}

# Git for Windows 默认调用 sh 执行 hook，所以最简单可靠的方式是写入 sh 脚本
# 调用 PowerShell。也可直接 cp pre-commit-hook.sh，但 Windows 原生 git
# 调用 sh 即可执行 .sh（git for windows 自带 bash）
$ShSource = Join-Path $ScriptDir "pre-commit-hook.sh"
if (Test-Path $ShSource) {
    Copy-Item $ShSource $HookScript -Force
    Write-Host "[OK] pre-commit hook 已安装到 $HookScript"
    Write-Host "     实际脚本：$ShSource"
} else {
    Write-Host "[ERROR] 未找到 $ShSource"
    exit 1
}

Write-Host ""
Write-Host "测试：修改一个 .cpp 文件不格式化，git commit 应被阻止"
Write-Host "卸载：Remove-Item $HookScript"
Write-Host "跳过：git commit --no-verify"
