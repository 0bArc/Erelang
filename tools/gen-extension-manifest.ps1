# Writes erevos-language/resources/builtins.json from `erelang --dump-builtins --json`.
# The VS Code extension bundles that file, so new builtins reach completion without editing TypeScript.
# Usage: .\tools\gen-extension-manifest.ps1 [-Exe <path to erelang.exe>]
param([string]$Exe)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    $name = if ($env:OS -eq 'Windows_NT') { 'erelang.exe' } else { 'erelang' }
    $candidates = @(
        (Join-Path $root "build/release/bin/Release/$name"),
        (Join-Path $root "build/debug/bin/Debug/$name")
    )
    $Exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Exe) {
        $cmd = Get-Command erelang -ErrorAction SilentlyContinue
        if ($cmd) { $Exe = $cmd.Source }
    }
}
if (-not $Exe -or -not (Test-Path $Exe)) { throw 'erelang.exe not found; build it first or pass -Exe' }

$json = & $Exe --dump-builtins --json
if ($LASTEXITCODE -ne 0) { throw "erelang --dump-builtins failed with exit code $LASTEXITCODE" }
$text = ($json -join "`n").Trim()
$null = $text | ConvertFrom-Json   # fail loudly on a broken manifest
$outDir = Join-Path $root 'erevos-language\resources'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out = Join-Path $outDir 'builtins.json'
[IO.File]::WriteAllText($out, $text + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Host "wrote $out from $Exe"
