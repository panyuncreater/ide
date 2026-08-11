# ============================================================
# scripts/compress_memory.ps1 — 记忆压缩与归档脚本
# ------------------------------------------------------------
# AGENTS.md「记忆压缩与归档机制」的执行脚本：
#   1. 归档旧日期文件夹（> -KeepDays，默认 14 天）到 archive/topics/<YYYYMMDD>/
#   2. 归档当前活跃日期内超大 session_memory 文件到 archive/session_memory/
#   3. 检测 project_memory.md 是否超阈值，提示 AI 需执行内容压缩
#
# 分工：脚本只做文件搬移与大小检测；project_memory.md 的语义分类（永久区/
#       活跃区/历史区）由 AI 按 AGENTS.md 规则执行，脚本不改写 markdown 内容。
#
# 用法：
#   pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
#        -File scripts/compress_memory.ps1                 # 默认 DryRun，只报告
#   pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
#        -File scripts/compress_memory.ps1 -Apply          # 执行实际搬移
#   pwsh -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
#        -File scripts/compress_memory.ps1 -Apply -KeepDays 30
# ============================================================
param(
    [string]$ProjectDir = "",            # 空则自动推断（仅一个项目目录时）
    [switch]$Apply,                      # 指定才执行搬移；默认 DryRun
    [int]$KeepDays = 14,                  # 旧日期文件夹归档阈值
    [int]$SessionLineThreshold = 500,    # 单 session_memory 文件行数阈值
    [int]$MemoryLineThreshold = 200,     # project_memory.md 行数阈值
    [int]$MemoryByteThreshold = 8192     # project_memory.md 字节阈值
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'

$execute = $Apply.IsPresent

# --- 推断 ProjectDir ---
if ($ProjectDir -eq "") {
    $memoryRoot = Join-Path $env:USERPROFILE ".trae-cn\memory\projects"
    if (-not (Test-Path -LiteralPath $memoryRoot)) {
        @{ ok = $false; error = "memory projects root not found: $memoryRoot" } | ConvertTo-Json -Depth 10
        exit 1
    }
    $candidates = Get-ChildItem -LiteralPath $memoryRoot -Directory
    if ($candidates.Count -eq 1) {
        $ProjectDir = $candidates[0].FullName
    } else {
        @{ ok = $false; error = "multiple project dirs under $memoryRoot; specify -ProjectDir explicitly" } | ConvertTo-Json -Depth 10
        exit 1
    }
}

if (-not (Test-Path -LiteralPath $ProjectDir)) {
    @{ ok = $false; error = "project dir not found: $ProjectDir" } | ConvertTo-Json -Depth 10
    exit 1
}

$archiveDir       = Join-Path $ProjectDir "archive"
$archiveTopics    = Join-Path $archiveDir "topics"
$archiveSession   = Join-Path $archiveDir "session_memory"
$archiveProject   = Join-Path $archiveDir "project_memory"

$report = [ordered]@{
    ok              = $true
    mode            = if ($execute) { "apply" } else { "dryrun" }
    project_dir     = $ProjectDir
    keep_days       = $KeepDays
    thresholds      = [ordered]@{ session_lines = $SessionLineThreshold; memory_lines = $MemoryLineThreshold; memory_bytes = $MemoryByteThreshold }
    archived_topics = @()
    archived_sessions = @()
    project_memory  = $null
}

$today   = Get-Date
$cutoff  = $today.AddDays(-$KeepDays)

# --- 1. 归档旧日期文件夹（整体搬移，含 topics.md 与 session_memory_*.jsonl）---
$dateFolders = Get-ChildItem -LiteralPath $ProjectDir -Directory | Where-Object { $_.Name -match '^\d{8}$' }
foreach ($df in $dateFolders) {
    try {
        $folderDate = [datetime]::ParseExact($df.Name, "yyyyMMdd", $null)
    } catch { continue }
    if ($folderDate -lt $cutoff) {
        $target = Join-Path $archiveTopics $df.Name
        $entry = [ordered]@{ source = $df.FullName; target = $target; date = $df.Name }
        if ($execute) {
            New-Item -ItemType Directory -Force -Path $archiveTopics | Out-Null
            Move-Item -LiteralPath $df.FullName -Destination $target
        }
        $report.archived_topics += $entry
    }
}

# --- 2. 归档当前活跃日期文件夹内超大 session_memory 文件 ---
$activeDateFolders = Get-ChildItem -LiteralPath $ProjectDir -Directory | Where-Object { $_.Name -match '^\d{8}$' }
foreach ($df in $activeDateFolders) {
    $sessions = Get-ChildItem -LiteralPath $df.FullName -Filter "session_memory_*.jsonl" -File
    foreach ($s in $sessions) {
        $lineCount = (Get-Content -LiteralPath $s.FullName | Measure-Object).Count
        if ($lineCount -gt $SessionLineThreshold) {
            $target = Join-Path $archiveSession ($df.Name + "_" + $s.Name)
            $entry = [ordered]@{ source = $s.FullName; target = $target; lines = $lineCount }
            if ($execute) {
                New-Item -ItemType Directory -Force -Path $archiveSession | Out-Null
                Move-Item -LiteralPath $s.FullName -Destination $target
            }
            $report.archived_sessions += $entry
        }
    }
}

# --- 3. 检测 project_memory.md 大小（脚本不改写内容，仅提示 AI 压缩）---
$pmPath = Join-Path $ProjectDir "project_memory.md"
if (Test-Path -LiteralPath $pmPath) {
    $lineCount = (Get-Content -LiteralPath $pmPath | Measure-Object).Count
    $byteCount = (Get-Item -LiteralPath $pmPath).Length
    $needsCompression = ($lineCount -gt $MemoryLineThreshold) -or ($byteCount -gt $MemoryByteThreshold)
    $report.project_memory = [ordered]@{
        path               = $pmPath
        exists             = $true
        lines              = $lineCount
        bytes              = $byteCount
        needs_compression  = $needsCompression
        hint               = if ($needsCompression) { "AI 应按 AGENTS.md 规则执行内容压缩（永久区/活跃区/历史区分类）" } else { "ok" }
    }
} else {
    $report.project_memory = [ordered]@{
        path   = $pmPath
        exists = $false
        hint   = "尚未创建 project_memory.md；按 AGENTS.md 标准结构初始化"
    }
}

$report | ConvertTo-Json -Depth 10
