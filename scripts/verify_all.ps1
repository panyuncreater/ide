# ============================================================
# scripts/verify_all.ps1 — 全量构建 + 测试验证入口
# ------------------------------------------------------------
# AGENTS.md 强制验证规则的标准执行脚本：
#   1. 构建全部目标（W4 + /WX 零警告基线，任何 error/LNK 即失败）
#   2. ctest 全量测试（-j 8 并行）
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts/verify_all.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/verify_all.ps1 -Filter "PkgLockfileTest"
# 依赖：VS 2022 DevShell（路径按本机 D:\vs 安装位置）
# ============================================================
param(
    [string]$Filter = ""  # 可选 ctest -R 过滤器；空 = 全量
)

Import-Module 'D:\vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Enter-VsDevShell -VsInstallPath 'D:\vs' -Arch amd64 -SkipAutomaticLocation | Out-Null
Set-Location "$PSScriptRoot\.."

cmake --build out/build/debug 2>&1 | Select-String -Pattern ': error|LNK2005|LNK2019|LNK1' | Select-Object -First 15
Write-Output ("BUILD ALL: " + $LASTEXITCODE)
if ($LASTEXITCODE -ne 0) { exit 1 }

if ($Filter -ne "") {
    ctest --test-dir out/build/debug -R $Filter --output-on-failure 2>&1 | Select-Object -Last 8
} else {
    ctest --test-dir out/build/debug -j 8 2>&1 | Select-Object -Last 4
}
Write-Output ("CTEST: " + $LASTEXITCODE)
if ($LASTEXITCODE -ne 0) { exit 1 }
