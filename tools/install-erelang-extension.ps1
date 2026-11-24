# Build erelang_language.vsix (stable name, no version bump) and install into VS Code / Cursor.
# Preferred: run via Erelang (dogfoods the toolchain). This script only finds erelang + sets repo root.

$ErrorActionPreference = 'Stop'

$root   = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script = Join-Path $PSScriptRoot 'install-erelang-extension.elan'

$erelang = $null
if (Get-Command erelang -ErrorAction SilentlyContinue) {
  $erelang = (Get-Command erelang).Source
} elseif (Test-Path (Join-Path $root 'build\debug\bin\Debug\erelang.exe')) {
  $erelang = Join-Path $root 'build\debug\bin\Debug\erelang.exe'
} elseif (Test-Path (Join-Path $root 'build\release\bin\Release\erelang.exe')) {
  $erelang = Join-Path $root 'build\release\bin\Release\erelang.exe'
} else {
  throw "erelang not found on PATH and not built under build/debug or build/release"
}

$env:ERELANG_REPO_ROOT = $root
Push-Location $root
try {
  & $erelang $script
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
  Pop-Location
}
