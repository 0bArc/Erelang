# Erelang conformance suite runner
# Usage (repo root): .\tests\run.ps1

param(
    [string]$Exe = "build\bin\Debug\erelang.exe"
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
Set-Location $root

if (-not (Test-Path $Exe)) {
    Write-Error "Missing runner: $Exe - build target erelang_runner first."
    exit 2
}

$pass = 0
$fail = 0
$skip = 0
$failures = New-Object System.Collections.Generic.List[string]

$files = Get-ChildItem -Path (Join-Path $root "tests") -Recurse -Filter *.elan |
    Where-Object {
        $_.Name -ne "cycle_b.elan" -and
        $_.Name -ne "package.elan" -and
        $_.FullName -notmatch '[\\/]packages[\\/]registry[\\/]' -and
        $_.FullName -notmatch '[\\/]\.erelang[\\/]'
    } |
    Sort-Object FullName

$registry = Join-Path $root "tests\packages\registry"
$appDir = Join-Path $root "tests\packages\app"
if (Test-Path (Join-Path $appDir "package.elan")) {
    & $Exe --lock (Join-Path $appDir "package.elan") --registry $registry 2>&1 | Out-Null
    & $Exe --fetch (Join-Path $appDir "package.elan") --registry $registry 2>&1 | Out-Null
}

foreach ($f in $files) {
    $rel = $f.FullName.Substring($root.Length).TrimStart('\', '/')
    $isNeg = $f.Name -like "*_neg.elan"
    $expect = $null
    $first = Get-Content -Path $f.FullName -TotalCount 1 -ErrorAction SilentlyContinue
    if ($first -match '^//\s*expect:\s*(.+)\s*$') {
        $expect = $Matches[1].Trim()
    }

    $out = ""
    $code = -1
    try {
        $out = & $Exe $f.FullName 2>&1 | Out-String
        if ($null -eq $out) { $out = "" }
        $code = $LASTEXITCODE
    } catch {
        $out = $_.Exception.Message
        $code = -1
    }

    $ok = $false
    if ($isNeg) {
        $ok = ($code -ne 0)
        if ($ok -and $expect -and $out) {
            $ok = $out.Contains($expect)
        }
    } else {
        $ok = ($code -eq 0)
    }

    if ($ok) {
        $pass++
        Write-Host "PASS $rel"
    } else {
        $fail++
        [void]$failures.Add($rel)
        Write-Host "FAIL $rel (exit=$code)"
        if ($expect) { Write-Host "  expect: $expect" }
        $lines = $out -split "`r?`n" | Select-Object -First 6
        foreach ($line in $lines) {
            if ($line) { Write-Host $line }
        }
    }
}

Write-Host ""
Write-Host "pass=$pass fail=$fail skip=$skip total=$($pass + $fail + $skip)"
if ($fail -gt 0) {
    Write-Host "failures:"
    foreach ($item in $failures) {
        Write-Host "  $item"
    }
    exit 1
}
exit 0
