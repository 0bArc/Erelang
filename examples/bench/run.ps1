# Times every examples/bench/*.elan (and examples/types/bench.elan) with an erelang executable.
# Best of N runs, minus the startup time of an empty script, printed as a table.
# Usage: ./examples/bench/run.ps1 [-Exe <erelang executable>] [-Runs 3]   (default: the release preset build)
param(
    [string]$Exe,
    [int]$Runs = 3
)

$ErrorActionPreference = 'Stop'
if (-not $Exe) {
    $name = if ($env:OS -eq 'Windows_NT') { 'erelang.exe' } else { 'erelang' }
    $Exe = Join-Path $PSScriptRoot "../../build/release/bin/Release/$name"
}
if (-not (Test-Path $Exe)) { throw "erelang not found: $Exe" }

function Best([string]$script) {
    $best = [double]::MaxValue
    $out = ''
    for ($i = 0; $i -lt $Runs; $i++) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $out = (& $Exe $script 2>&1 | Out-String).Trim()
        $sw.Stop()
        if ($LASTEXITCODE -ne 0) { throw "$script failed: $out" }
        $best = [Math]::Min($best, $sw.Elapsed.TotalMilliseconds)
    }
    return @{ ms = $best; out = $out }
}

$empty = Join-Path ([IO.Path]::GetTempPath()) 'erelang-bench-empty.elan'
# No BOM: Windows PowerShell's -Encoding UTF8 writes one, and the lexer rejects it.
[IO.File]::WriteAllText($empty, "@erelang`npublic action main() {`n}`nrun main;`n", (New-Object Text.UTF8Encoding($false)))
$startup = (Best $empty).ms

$rows = foreach ($file in Get-ChildItem $PSScriptRoot -Filter *.elan | Sort-Object Name) {
    $r = Best $file.FullName
    [pscustomobject]@{
        Benchmark = $file.BaseName
        'Total ms' = [int]$r.ms
        'Work ms' = [int][Math]::Max(0, $r.ms - $startup)
        Result = ($r.out -split "`n" | Select-Object -Last 1)
    }
}
Write-Host ("startup (empty script): {0:N0} ms, best of {1}" -f $startup, $Runs)
$rows | Format-Table -AutoSize
