# Put build\debug\bin\Debug first on the user PATH and drop other PATH entries inside this repo.
# The debug build does this itself after linking erelang; run this only to repair PATH by hand.
$repo = (Resolve-Path (Split-Path $PSScriptRoot -Parent)).Path.TrimEnd('\')
$binDir = Join-Path $repo "build\debug\bin\Debug"

if (-not (Test-Path (Join-Path $binDir "erelang.exe"))) {
  throw "Missing $binDir\erelang.exe. Build first: cmake --preset debug; cmake --build --preset debug"
}

$user = [Environment]::GetEnvironmentVariable("Path", "User")
$parts = @($user -split ";" | ForEach-Object { $_.Trim() } | Where-Object {
  $_ -and
  -not ($_.TrimEnd('\') + '\').StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase) -and
  ($_ -notmatch '[\\/]AppData[\\/]Local[\\/]Erelang[\\/]bin$')
})
$parts = @($binDir) + $parts
[Environment]::SetEnvironmentVariable("Path", ($parts -join ";"), "User")
Write-Host "PATH fixed. First erelang entry: $binDir"
Write-Host "Refresh this shell with:"
Write-Host '  $env:Path = [Environment]::GetEnvironmentVariable("Path","User") + ";" + [Environment]::GetEnvironmentVariable("Path","Machine")'
