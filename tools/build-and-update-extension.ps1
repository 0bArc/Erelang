Param(
  [switch]$Watch
)

$ErrorActionPreference = 'Stop'

if ($env:ERELANG_SKIP_VSIX -eq '1') {
  Write-Host 'ERELANG_SKIP_VSIX=1 set; skipping VSIX packaging.'
  return
}

$root    = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$extDir  = Join-Path $root 'erevos-language'
$stamp   = Join-Path $extDir '.last_packaged'

if (-not (Test-Path $extDir)) { throw "Extension directory not found: $extDir" }

# Determine newest change inside extension sources
function Get-NewestChange {
  $files = Get-ChildItem -Path $extDir -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '\.(ts|json)$' -or $_.Name -in @('package.json','package-lock.json') }
  if (-not $files) { return Get-Date '2000-01-01' }
  return ($files | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime
}

$newest = Get-NewestChange
$needPackage = $true
if (Test-Path $stamp) {
  try {
    $lastPackaged = Get-Content $stamp -ErrorAction Stop | Select-Object -First 1 | ForEach-Object { Get-Date $_ }
    if ($lastPackaged -ge $newest) {
      Write-Host "No extension source changes since last package ($lastPackaged); skipping repack." -ForegroundColor DarkGray
      $needPackage = $false
    }
  } catch { }
}

Push-Location $extDir

if ($needPackage) {
  Write-Host 'Preparing VS Code extension (incremental)...'
  if (-not (Test-Path (Join-Path $extDir 'node_modules'))) {
    Write-Host 'node_modules missing -> running npm install'
    npm install | Out-Null
  } else {
    Write-Host 'node_modules present -> skipping npm install'
  }

  Write-Host 'Compiling TypeScript...'
  npm run compile | Out-Null

  Write-Host 'Packaging VSIX...'
  if (Get-Command npx -ErrorAction SilentlyContinue) {
    npx vsce package --allow-missing-repository | Out-Null
  } elseif (Get-Command vsce -ErrorAction SilentlyContinue) {
    vsce package --allow-missing-repository | Out-Null
  } else {
    Pop-Location
    throw "vsce (or npx) not found; install with: npm i -g vsce"
  }

  (Get-Date).ToString('o') | Out-File $stamp -Encoding utf8
} else {
  Write-Host 'Using existing packaged VSIX (no changes).'
}

$vsix = Get-ChildItem -Filter *.vsix | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $vsix) { Pop-Location; throw 'VSIX not found (was packaging skipped prematurely?).' }

$stableVsixPath = Join-Path $extDir 'erelang_language.vsix'
if ([System.IO.Path]::GetFullPath($vsix.FullName) -ne [System.IO.Path]::GetFullPath($stableVsixPath)) {
  Copy-Item -Path $vsix.FullName -Destination $stableVsixPath -Force
}

Write-Host "Installing VSIX: $($vsix.Name)"

$codeCmd = $null
if (Get-Command code.cmd -ErrorAction SilentlyContinue) {
  $codeCmd = (Get-Command code.cmd -ErrorAction SilentlyContinue).Source
} elseif (Test-Path "$env:LOCALAPPDATA\Programs\Microsoft VS Code\bin\code.cmd") {
  $codeCmd = "$env:LOCALAPPDATA\Programs\Microsoft VS Code\bin\code.cmd"
} elseif (Get-Command code -ErrorAction SilentlyContinue) {
  $candidate = (Get-Command code -ErrorAction SilentlyContinue).Source
  if ($candidate -and $candidate.ToLowerInvariant().EndsWith('code.cmd')) {
    $codeCmd = $candidate
  }
}

if ($codeCmd) {
  try {
    $pkgJson = Get-Content -Path (Join-Path $extDir 'package.json') -Raw | ConvertFrom-Json
    $extId = "$($pkgJson.publisher).$($pkgJson.name)"
  } catch { $extId = $null }

  if ($extId) {
    Write-Host "Uninstalling prior version (if present): $extId" -ForegroundColor DarkGray
    try { & $codeCmd --uninstall-extension $extId 2>&1 | Out-Null } catch {}
    $global:LASTEXITCODE = 0
  }
  $out = & $codeCmd --install-extension $stableVsixPath --force 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Warning "Failed to install VSIX via '$codeCmd': $out"
    $global:LASTEXITCODE = 0
  } else {
    Write-Host 'Extension installed. Use: Developer: Reload Window' -ForegroundColor Green
    $global:LASTEXITCODE = 0
  }
} else {
  Write-Warning "'code.cmd' CLI not found; manual install required: $stableVsixPath"
  $global:LASTEXITCODE = 0
}

Pop-Location
Write-Host 'VSIX packaging step complete.'

$global:LASTEXITCODE = 0

if ($Watch) {
  Write-Host 'Watch mode not fully implemented; keeping process alive.'
  while ($true) { Start-Sleep -Seconds 60 }
}
