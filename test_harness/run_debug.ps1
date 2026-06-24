$exe = "C:\Users\v\Desktop\ide\ide\x64\Debug\test_harness.exe"
$outFile = "C:\Users\v\Desktop\ide\ide\test_harness\debug_result.txt"
$errFile = "C:\Users\v\Desktop\ide\ide\test_harness\debug_result.err"
$process = Start-Process -FilePath $exe -Wait -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
Write-Host "ExitCode: $($process.ExitCode)"
if (Test-Path $outFile) { Get-Content $outFile }
if (Test-Path $errFile) {
    $errContent = Get-Content $errFile -Raw
    if ($errContent) { Write-Host "STDERR:"; Write-Host $errContent }
}
