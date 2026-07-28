# ============================================================
# MiniLang IDE 崩溃 minidump 符号化解析脚本（拓展计划·基建）
# ------------------------------------------------------------
# 背景：
#   CrashHandler (R128) 在 Windows 崩溃时通过 MiniDumpWriteDump 写出
#   .dmp 文件（CrashReport.dumpFilePath），但 Windows 侧 stackTrace 留空，
#   需借助调试器 + 对应版本 PDB 才能解析出源码级栈回溯。
#   本脚本自动定位 cdb.exe（Windows SDK Debugging Tools）并执行
#   `!analyze -v` + 异常上下文栈回溯，输出可读的崩溃分析报告。
#
# 用法：
#   # 本地开发构建（PDB 就在构建目录旁，脚本自动推断）
#   .\scripts\analyze_minidump.ps1 -DumpFile crash_20260728.dmp
#
#   # 用户上报的 Release 崩溃（先从 GitHub Release 下载对应版本
#   # MiniLangIDE-<ver>-windows-x64-symbols.zip 并解压）
#   .\scripts\analyze_minidump.ps1 -DumpFile crash.dmp -SymbolDir C:\syms\v1.2.0
#
#   # 输出到文件
#   .\scripts\analyze_minidump.ps1 -DumpFile crash.dmp -OutFile report.txt
#
# 依赖：
#   cdb.exe — 随 Windows SDK "Debugging Tools for Windows" 安装。
#   若未安装：winget install Microsoft.WindowsSDK 或从
#   https://learn.microsoft.com/windows-hardware/downloads/windows-sdk 勾选
#   "Debugging Tools for Windows" 组件。
# ============================================================
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DumpFile,

    # PDB 符号目录（Release 崩溃：解压 symbols.zip 的目录）。
    # 省略时自动追加本仓库常见构建输出目录。
    [string]$SymbolDir = '',

    # 分析报告输出文件（省略则打印到控制台）
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $DumpFile)) {
    Write-Error "dump 文件不存在: $DumpFile"
}

# ---- 定位 cdb.exe（x64 优先）----
$cdbCandidates = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe",
    "${env:ProgramFiles(x86)}\Windows Kits\11\Debuggers\x64\cdb.exe",
    "$env:ProgramFiles\Windows Kits\10\Debuggers\x64\cdb.exe"
)
$cdb = $cdbCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cdb) {
    $found = Get-Command cdb.exe -ErrorAction SilentlyContinue
    if ($found) { $cdb = $found.Source }
}
if (-not $cdb) {
    Write-Error @"
未找到 cdb.exe（Debugging Tools for Windows）。
安装方式：Windows SDK 安装器中勾选 "Debugging Tools for Windows"。
下载：https://learn.microsoft.com/windows-hardware/downloads/windows-sdk
"@
}

# ---- 组装符号路径 ----
# 顺序：用户指定目录 → 本仓库构建输出目录 → 微软公共符号服务器（系统 DLL 帧）
$repoRoot = Split-Path -Parent $PSScriptRoot
$symParts = @()
if ($SymbolDir) {
    if (-not (Test-Path $SymbolDir)) { Write-Error "符号目录不存在: $SymbolDir" }
    $symParts += $SymbolDir
}
foreach ($d in @('out\build\release', 'out\build\release\cli', 'out\build\debug', 'out\build\debug\cli')) {
    $p = Join-Path $repoRoot $d
    if (Test-Path $p) { $symParts += $p }
}
$symCache = Join-Path $env:TEMP 'minilang-symcache'
$symParts += "srv*$symCache*https://msdl.microsoft.com/download/symbols"
$symPath = $symParts -join ';'

Write-Host "cdb:        $cdb"
Write-Host "dump:       $DumpFile"
Write-Host "symbol 路径: $symPath"
Write-Host ('-' * 60)

# ---- 执行分析 ----
# !analyze -v  : 自动崩溃归因（异常码/故障模块/候选源行）
# .ecxr        : 切换到异常发生时的上下文
# kv           : 异常上下文的完整栈回溯（带参数）
# lm vm minilang* : 列出 MiniLang 模块的版本/PDB 匹配情况（验证符号是否命中）
$cmds = '!analyze -v; .ecxr; kv; lm vm minilang*; q'
$rawOutput = & $cdb -z $DumpFile -y $symPath -c $cmds 2>&1 | Out-String

# 裁剪 cdb 启动横幅之前的噪声（保留从异常分析开始的部分）
$analysisStart = $rawOutput.IndexOf('ExceptionAddress')
if ($analysisStart -lt 0) { $analysisStart = $rawOutput.IndexOf('!analyze') }
$report = if ($analysisStart -gt 0) { $rawOutput.Substring([Math]::Max(0, $analysisStart - 200)) } else { $rawOutput }

$header = @"
============================================================
MiniLang IDE 崩溃分析报告
dump:    $DumpFile
生成时间: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
============================================================
"@

if ($OutFile) {
    ($header + $report) | Out-File -FilePath $OutFile -Encoding utf8
    Write-Host "分析报告已写入: $OutFile"
} else {
    Write-Host $header
    Write-Host $report
}

# 符号未命中提示（PDB 版本不匹配是最常见失败原因）
if ($rawOutput -match 'PDB not found|Symbols not loaded|no symbols') {
    Write-Warning '检测到符号未完全加载：请确认 -SymbolDir 指向与崩溃版本完全一致的 symbols.zip 解压目录。'
}
