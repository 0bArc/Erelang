param(
  [Parameter(Mandatory = $true)]
  [string]$TargetDir
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $TargetDir)) {
  exit 0
}

$compiler = $null
try {
  $compiler = (Get-Command g++.exe -ErrorAction Stop).Source
} catch {
  try {
    $compiler = (Get-Command gcc.exe -ErrorAction Stop).Source
  } catch {
    exit 0
  }
}

if (-not $compiler) {
  exit 0
}

$binDir = Split-Path -Parent $compiler
$dlls = @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')

foreach ($dll in $dlls) {
  $src = Join-Path $binDir $dll
  if (Test-Path -LiteralPath $src) {
    Copy-Item -LiteralPath $src -Destination (Join-Path $TargetDir $dll) -Force
  }
}
