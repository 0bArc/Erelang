$ErrorActionPreference = "Continue"
$exe = Join-Path $PSScriptRoot "..\..\build\bin\Debug\erelang.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Missing erelang at $exe - build with cmake --build build --target erelang erelang_runner -j 8"
    exit 1
}

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$script:pass = 0
$script:fail = 0

function Run-ExpectOk([string]$rel) {
    $path = Join-Path $root $rel
    & $exe $path | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "OK   $rel"
        $script:pass++
    } else {
        Write-Host "FAIL $rel (exit $LASTEXITCODE, expected 0)"
        $script:fail++
    }
}

function Run-ExpectFail([string]$rel) {
    $path = Join-Path $root $rel
    & $exe $path 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "OK   $rel (failed as expected)"
        $script:pass++
    } else {
        Write-Host "FAIL $rel (exit 0, expected typecheck failure)"
        $script:fail++
    }
}

Write-Host "=== smoke ==="
Run-ExpectOk "examples\generics.elan"

Write-Host "=== positive ==="
Get-ChildItem (Join-Path $PSScriptRoot "positive\*.elan") | Sort-Object Name | ForEach-Object {
    $rel = "examples\generics\positive\$($_.Name)"
    Run-ExpectOk $rel
}

Write-Host "=== negative ==="
Get-ChildItem (Join-Path $PSScriptRoot "negative\*.elan") | Sort-Object Name | ForEach-Object {
    $rel = "examples\generics\negative\$($_.Name)"
    Run-ExpectFail $rel
}

Write-Host ""
Write-Host "Passed $($script:pass)  Failed $($script:fail)"
if ($script:fail -gt 0) { exit 1 }
exit 0
