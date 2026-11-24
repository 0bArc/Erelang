param([string]$Exe = "erelang")
$dir = $PSScriptRoot
$failed = 0
foreach ($f in Get-ChildItem $dir -Filter *.elan | Sort-Object Name) {
    $out = & $Exe $f.FullName 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL $($f.Name)"; $out | Select-Object -Last 3; $failed++ }
    else { Write-Host "ok   $($f.Name)" }
}
exit $failed
