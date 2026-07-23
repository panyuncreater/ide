# ============================================================
# MiniLang git pre-commit hook（PowerShell 版，Windows 原生）
# ------------------------------------------------------------
# 安装：
#   Copy-Item scripts\pre-commit-hook.ps1 .git\hooks\pre-commit
#   # 或在 .git\hooks\pre-commit 中写入：
#   #   & powershell -ExecutionPolicy Bypass -File "<repo>\scripts\pre-commit-hook.ps1"
#
# 功能：
#   1. 仅检查本次暂存（staged）的 .cpp / .h 文件
#   2. 用项目固定版本 clang-format 22.1.5 检查格式
#   3. 不修改文件，发现违规时退出码 1 阻止提交
#
# 跳过：
#   git commit --no-verify
# ============================================================

$ErrorActionPreference = "Stop"

# --- 定位项目根 ---
$HookDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (Test-Path "$HookDir\..\..\.git") {
    $ProjectRoot = (Resolve-Path "$HookDir\..\..").Path
} else {
    $ProjectRoot = $HookDir
}
Set-Location $ProjectRoot

# --- 收集暂存的 .cpp / .h 文件 ---
$StagedFiles = git diff --cached --name-only --diff-filter=ACM |
    Where-Object { $_ -match '\.(cpp|h)$' } |
    Where-Object {
        $_ -match '^(lexer|parser|ast|interpreter|compiler|debug|formatter|common|gui|app|cli|tests|test_harness)/'
    }

if (-not $StagedFiles) {
    Write-Host "[pre-commit] 无 .cpp/.h 文件需要检查，跳过"
    exit 0
}

# --- 定位 clang-format ---
$ClangFormat = $env:CLANG_FORMAT
if (-not $ClangFormat) {
    $ClangFormat = (Get-Command clang-format -ErrorAction SilentlyContinue).Source
}
if (-not $ClangFormat) {
    # 常见 pip 安装路径
    $pipPath = "$env:LOCALAPPDATA\Programs\Python\Python313\Scripts\clang-format.exe"
    if (Test-Path $pipPath) {
        $ClangFormat = $pipPath
    }
}
if (-not $ClangFormat) {
    Write-Host "[pre-commit][WARN] 未找到 clang-format，跳过格式检查"
    Write-Host "  安装方式：pip install clang-format==22.1.5"
    exit 0
}

# --- 检查版本（推荐 22.1.5）---
$Version = & $ClangFormat --version 2>$null | Select-Object -First 1
Write-Host "[pre-commit] 使用 $ClangFormat ($Version)"

# --- 逐文件检查 ---
$Failed = $false
foreach ($f in $StagedFiles) {
    if (-not (Test-Path $f)) {
        continue
    }
    $output = & $ClangFormat --dry-run --Werror $f 2>&1
    if ($LASTEXITCODE -ne 0 -or $output) {
        Write-Host "[pre-commit][FAIL] $f 需要格式化："
        Write-Host $output
        $Failed = $true
    }
}

if ($Failed) {
    Write-Host ""
    Write-Host "[pre-commit] 提交被阻止：上述文件未通过 clang-format 检查"
    Write-Host "  修复方式：clang-format -i <file...>"
    Write-Host "  跳过本次：git commit --no-verify"
    exit 1
}

Write-Host "[pre-commit] 全部通过"
exit 0
