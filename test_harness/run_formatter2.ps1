$exe = "C:\Users\v\Desktop\ide\ide\x64\Debug\test_harness.exe"
try {
    $output = & $exe 2>&1
    Write-Host "Output:"
    $output | ForEach-Object { Write-Host $_ }
    Write-Host "ExitCode: $LASTEXITCODE"
} catch {
    Write-Host "Exception: $_"
    Write-Host "ExitCode: $LASTEXITCODE"
}
