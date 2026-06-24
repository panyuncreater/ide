$exe = "C:\Users\v\Desktop\ide\ide\x64\Debug\test_harness.exe"
$outFile = "C:\Users\v\Desktop\ide\ide\test_harness\test_result.txt"

try {
    $process = Start-Process -FilePath $exe -Wait -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError "$outFile.err"
    Write-Host "ExitCode: $($process.ExitCode)"
} catch {
    Write-Host "Error: $($_.Exception.Message)"
}

if (Test-Path $outFile) {
    Get-Content $outFile
}
if (Test-Path "$outFile.err") {
    Write-Host "--- STDERR ---"
    Get-Content "$outFile.err"
}
