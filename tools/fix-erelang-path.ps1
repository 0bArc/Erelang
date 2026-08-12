# Point user PATH at build\bin\Debug (always refreshed by erelang_runner post-build).
# Prefer an existing Debug binary over Release so a manual fix cannot clobber a newer Debug build.
$repo = Split-Path $PSScriptRoot -Parent
$debugDir = Join-Path $repo "build\bin\Debug"
$debugExe = Join-Path $debugDir "erelang.exe"
$releaseExe = Join-Path $repo "build\bin\Release\erelang.exe"
$flatExe = Join-Path $repo "build\bin\erelang.exe"
$localBin = Join-Path $env:LOCALAPPDATA "Erelang\bin"

$src = $null
if (Test-Path $debugExe) { $src = $debugExe }
elseif (Test-Path $flatExe) { $src = $flatExe }
elseif (Test-Path $releaseExe) { $src = $releaseExe }

if ($src) {
  New-Item -ItemType Directory -Force $debugDir | Out-Null
  if ($src -ne $debugExe) {
    Copy-Item -Force $src $debugExe
  }
  New-Item -ItemType Directory -Force $localBin | Out-Null
  Copy-Item -Force $src (Join-Path $localBin "erelang.exe")
  Copy-Item -Force $src $flatExe
}

$user = [Environment]::GetEnvironmentVariable("Path", "User")
$parts = @($user -split ";" | ForEach-Object { $_.Trim() } | Where-Object {
  $_ -and
  ($_ -notmatch '[\\/]build[\\/]bin[\\/]Release$') -and
  ($_ -notmatch '[\\/]AppData[\\/]Local[\\/]Erelang[\\/]bin$') -and
  ($_ -ne $debugDir)
})
$parts = @($debugDir) + $parts
[Environment]::SetEnvironmentVariable("Path", ($parts -join ";"), "User")
Write-Host "PATH fixed. First erelang entry: $debugDir"
Write-Host "Refresh this shell with:"
Write-Host '  $env:Path = [Environment]::GetEnvironmentVariable("Path","User") + ";" + [Environment]::GetEnvironmentVariable("Path","Machine")'
