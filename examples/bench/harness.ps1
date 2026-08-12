# Erelang vs Node vs Python microbenchmark harness
# Usage: powershell -NoProfile -File examples/bench/harness.ps1

$ErrorActionPreference = 'Stop'
$BenchDir = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $BenchDir '../..')).Path
$TreeDir = Join-Path $BenchDir '_tree'
$Runs = 5
$Discard = 1

$erelang = $null
foreach ($c in @(
    (Join-Path $RepoRoot 'build\bin\Debug\erelang.exe'),
    (Join-Path $RepoRoot 'build\bin\Release\erelang.exe'),
    (Join-Path $RepoRoot 'build\bin\erelang.exe')
)) {
    if (Test-Path $c) { $erelang = $c; break }
}
if (-not $erelang) {
    $cmd = Get-Command erelang -ErrorAction SilentlyContinue
    if ($cmd) { $erelang = $cmd.Source }
}
if (-not $erelang) { throw 'erelang.exe not found (build erelang_runner first)' }

$node = (Get-Command node -ErrorAction Stop).Source

$python = $null
foreach ($c in @(
    'C:\Python313\python.exe',
    'C:\Python312\python.exe',
    'C:\Python311\python.exe'
)) {
    if (Test-Path $c) { $python = $c; break }
}
if (-not $python) {
    foreach ($name in @('python', 'python3', 'py')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source -notmatch 'WindowsApps') { $python = $cmd.Source; break }
    }
}
if (-not $python) { throw 'python not found (install CPython, not the Store stub)' }

function Get-Median([double[]]$vals) {
    $s = $vals | Sort-Object
    $n = $s.Count
    if ($n -eq 0) { return 0 }
    if ($n % 2 -eq 1) { return [double]$s[[int]($n / 2)] }
    return ([double]$s[$n / 2 - 1] + [double]$s[$n / 2]) / 2.0
}

function New-BenchTree([string]$root) {
    if (Test-Path $root) { Remove-Item -Recurse -Force $root }
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    for ($d = 0; $d -lt 10; $d++) {
        $dir = Join-Path $root ("dir{0:D2}" -f $d)
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        for ($f = 0; $f -lt 9; $f++) {
            Set-Content -Path (Join-Path $dir ("f{0:D2}.txt" -f $f)) -Value "x" -NoNewline
        }
    }
    for ($f = 0; $f -lt 10; $f++) {
        Set-Content -Path (Join-Path $root ("root{0:D2}.txt" -f $f)) -Value "x" -NoNewline
    }
}

function Invoke-Timed([scriptblock]$block) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $out = & $block 2>&1 | Out-String
    $sw.Stop()
    $inner = $null
    if ($out -match 'INNER_MS=(-?\d+)') { $inner = [double]$Matches[1] }
    return [pscustomobject]@{
        WallMs  = [double]$sw.Elapsed.TotalMilliseconds
        InnerMs = $inner
        Output  = $out
    }
}

function Measure-Runtime([string]$label, [scriptblock]$block) {
    $walls = New-Object System.Collections.Generic.List[double]
    $inners = New-Object System.Collections.Generic.List[double]
    for ($i = 0; $i -lt ($Discard + $Runs); $i++) {
        $r = Invoke-Timed $block
        if ($i -lt $Discard) { continue }
        $walls.Add($r.WallMs) | Out-Null
        if ($null -ne $r.InnerMs) { $inners.Add($r.InnerMs) | Out-Null }
        if ($r.Output -match '(?i)(error|Traceback|TC0)') {
            Write-Host ("WARN {0}:{1}{2}" -f $label, [Environment]::NewLine, $r.Output) -ForegroundColor Yellow
        }
    }
    $wallMed = Get-Median ($walls.ToArray())
    $wallMin = ($walls | Measure-Object -Minimum).Minimum
    $innerMed = if ($inners.Count -gt 0) { Get-Median ($inners.ToArray()) } else { $null }
    return [pscustomobject]@{
        WallMed  = [math]::Round($wallMed, 2)
        WallMin  = [math]::Round($wallMin, 2)
        InnerMed = if ($null -ne $innerMed) { [math]::Round($innerMed, 2) } else { $null }
    }
}

function Fmt([object]$v) {
    if ($null -eq $v) { return '-' }
    return "$v"
}

function Ratio([object]$a, [object]$b) {
    if ($null -eq $a -or $null -eq $b -or [double]$b -le 0) { return '-' }
    return [math]::Round(([double]$a / [double]$b), 2)
}

Write-Host "erelang: $erelang"
Write-Host "node:    $node"
Write-Host "python:  $python"
Write-Host "runs:    $Discard discard + $Runs measured"
Write-Host "tree:    $TreeDir"
Write-Host ""

New-BenchTree $TreeDir

$benches = @(
    @{
        Name = 'startup'
        Elan = { & $erelang (Join-Path $BenchDir 'startup.elan') | Out-String }
        Node = { & $node (Join-Path $BenchDir 'startup.js') | Out-String }
        Py   = { & $python (Join-Path $BenchDir 'startup.py') | Out-String }
    },
    @{
        Name = 'int_loop'
        Elan = { & $erelang (Join-Path $BenchDir 'int_loop.elan') | Out-String }
        Node = { & $node (Join-Path $BenchDir 'int_loop.js') | Out-String }
        Py   = { & $python (Join-Path $BenchDir 'int_loop.py') | Out-String }
    },
    @{
        Name = 'string_concat'
        Elan = { & $erelang (Join-Path $BenchDir 'string_concat.elan') | Out-String }
        Node = { & $node (Join-Path $BenchDir 'string_concat.js') | Out-String }
        Py   = { & $python (Join-Path $BenchDir 'string_concat.py') | Out-String }
    },
    @{
        Name = 'fs_list'
        Elan = { & $erelang (Join-Path $BenchDir 'fs_list.elan') $TreeDir | Out-String }
        Node = { & $node (Join-Path $BenchDir 'fs_list.js') $TreeDir | Out-String }
        Py   = { & $python (Join-Path $BenchDir 'fs_list.py') $TreeDir | Out-String }
    }
)

$results = @()
foreach ($b in $benches) {
    Write-Host ("measuring {0}..." -f $b.Name)
    $e = Measure-Runtime ("erelang/" + $b.Name) $b.Elan
    $n = Measure-Runtime ("node/" + $b.Name) $b.Node
    $p = Measure-Runtime ("python/" + $b.Name) $b.Py
    $results += [pscustomobject]@{
        Bench = $b.Name
        EWall = $e.WallMed; NWall = $n.WallMed; PWall = $p.WallMed
        EInn  = $e.InnerMed; NInn = $n.InnerMed; PInn = $p.InnerMed
        EMin  = $e.WallMin; NMin = $n.WallMin; PMin = $p.WallMin
    }
}

Write-Host ""
Write-Host '| bench | erelang wall | node wall | python wall | e/n wall | e/p wall | erelang inner | node inner | python inner | e/n inner | e/p inner |'
Write-Host '|-------|-------------:|----------:|------------:|---------:|---------:|--------------:|-----------:|-------------:|----------:|----------:|'
foreach ($r in $results) {
    Write-Host ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} |" -f `
        $r.Bench,
        $r.EWall, $r.NWall, $r.PWall,
        (Ratio $r.EWall $r.NWall), (Ratio $r.EWall $r.PWall),
        (Fmt $r.EInn), (Fmt $r.NInn), (Fmt $r.PInn),
        (Ratio $r.EInn $r.NInn), (Ratio $r.EInn $r.PInn))
}

Write-Host ""
Write-Host 'wall = full process ms (median); inner = INNER_MS from script (median). ratio > 1 => erelang slower.'
Write-Host 'min wall (erelang / node / python):'
foreach ($r in $results) {
    Write-Host ("  {0}: {1} / {2} / {3}" -f $r.Bench, $r.EMin, $r.NMin, $r.PMin)
}

if (Test-Path $TreeDir) {
    Remove-Item -Recurse -Force $TreeDir
    Write-Host ""
    Write-Host "cleaned $TreeDir"
}
