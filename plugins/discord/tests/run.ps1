# Runs bot_smoke.elan against the local mock Discord server and checks the recorded traffic.
# Usage: .\run.ps1 -Exe <path to erelang.exe>
param([Parameter(Mandatory = $true)][string]$Exe)

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$plugin = Split-Path -Parent $here
$exePath = (Resolve-Path $Exe).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("discord-plugin-test-" + [guid]::NewGuid().ToString('N'))
$bin = Join-Path $work 'bin'
$pluginDir = Join-Path (Join-Path $bin 'plugins') 'discord'
New-Item -ItemType Directory -Path $pluginDir -Force | Out-Null
Get-ChildItem (Split-Path -Parent $exePath) -File | Copy-Item -Destination $bin
Get-ChildItem $plugin -File | Copy-Item -Destination $pluginDir
$log = Join-Path $work 'mock.jsonl'
$mockArgs = @('-I', (Join-Path $here 'mock_discord.py'), '--rest-port', '18110', '--ws-port', '18111', '--log', $log)
if ($env:OS -eq 'Windows_NT') {
    $mock = Start-Process python -ArgumentList $mockArgs -PassThru -WindowStyle Hidden
} else {
    $mock = Start-Process python3 -ArgumentList $mockArgs -PassThru
}
$failed = 0

function Check([string]$name, [bool]$ok) {
    if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name"; $script:failed++ }
}

try {
    Start-Sleep -Milliseconds 800
    $out = (& (Join-Path $bin (Split-Path -Leaf $exePath)) (Join-Path $here 'bot_smoke.elan') 2>&1 | Out-String)
    Write-Host $out
    $entries = @(Get-Content $log | ForEach-Object { $_ | ConvertFrom-Json })
    $frames = @($entries | Where-Object { $_.event -eq 'frame' })
    $rest = @($entries | Where-Object { $_.kind -eq 'rest' })

    Check 'registered command with bot auth' (@($rest | Where-Object { $_.path -like '*/commands' -and $_.auth -eq 'Bot TESTTOKEN' }).Count -ge 1)
    Check 'identify sent with intents 513' (@($frames | Where-Object { $_.raw -like '*"op":2*' -and $_.raw -like '*"intents":513*' }).Count -eq 1)
    Check 'heartbeat sent' (@($frames | Where-Object { $_.raw -like '*"op":1,*' }).Count -ge 1)
    Check 'resume sent after reconnect' (@($frames | Where-Object { $_.raw -like '*"op":6*' -and $_.raw -like '*sess1*' }).Count -eq 1)
    Check 'rate limited send retried' (@($rest | Where-Object { $_.path -like '*/channels/c1/messages' }).Count -eq 2)
    Check 'interaction callback posted' (@($rest | Where-Object { $_.path -like '*/interactions/i1/tok1/callback' }).Count -eq 1)
    Check 'user message delivered, bot message ignored' (($out -match 'message alice: !ping') -and ($out -notmatch 'bot talking'))
    Check 'option with quotes decoded' ($out -match 'text=hi "there"')
    Check 'clean stop' ($out -match 'run rc=0 messages=2 commands=1')
} finally {
    if (-not $mock.HasExited) { Stop-Process -Id $mock.Id -Force }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed -gt 0) { exit 1 }
