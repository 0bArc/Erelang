# Runs docker_smoke.elan with a fake docker CLI first on PATH and checks the recorded calls.
# Usage: .\run.ps1 -Exe <path to erelang.exe>
param([Parameter(Mandatory = $true)][string]$Exe)

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$plugin = Split-Path -Parent $here
$exePath = (Resolve-Path $Exe).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("docker-plugin-test-" + [guid]::NewGuid().ToString('N'))
$bin = Join-Path $work 'bin'
$pluginDir = Join-Path (Join-Path $bin 'plugins') 'docker'
New-Item -ItemType Directory -Path $pluginDir -Force | Out-Null
Get-ChildItem (Split-Path -Parent $exePath) -File | Copy-Item -Destination $bin
Get-ChildItem $plugin -File | Copy-Item -Destination $pluginDir
$log = Join-Path $work 'docker-calls.log'
$failed = 0

function Check([string]$name, [bool]$ok) {
    if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name"; $script:failed++ }
}

$oldPath = $env:PATH
try {
    $env:PATH = (Join-Path $here 'fake') + [IO.Path]::PathSeparator + $env:PATH
    $env:FAKE_DOCKER_LOG = $log
    $out = (& (Join-Path $bin (Split-Path -Leaf $exePath)) (Join-Path $here 'docker_smoke.elan') 2>&1 | Out-String)
    Write-Host $out
    $calls = @(Get-Content $log -ErrorAction SilentlyContinue)
    Check 'script finished' ($out -match 'docker-plugin-ok')
    Check 'ps used the json format' (@($calls | Where-Object { $_ -like 'ps --no-trunc --format {{json .}}*' }).Count -ge 1)
    Check 'run passed flags before the image' (@($calls | Where-Object { $_ -eq 'run -d -p 8080:80 --name web nginx' }).Count -eq 1)
    Check 'rm forced' (@($calls | Where-Object { $_ -eq 'rm -f c0ffee123' }).Count -eq 1)
    Check 'logs tail' (@($calls | Where-Object { $_ -like 'logs --tail 2 c0ffee123*' }).Count -eq 1)
    Check 'compose up and down' (@($calls | Where-Object { $_ -like 'compose up -d*' }).Count -eq 1 -and @($calls | Where-Object { $_ -like 'compose down*' }).Count -eq 1)
} finally {
    $env:PATH = $oldPath
    Remove-Item Env:\FAKE_DOCKER_LOG -ErrorAction SilentlyContinue
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed -gt 0) { exit 1 }
