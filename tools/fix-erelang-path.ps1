$bin = Join-Path $env:LOCALAPPDATA "Erelang\bin"
$good = "D:\Development\Game\Physics\build\bin\Release\erelang.exe"
if (Test-Path $good) {
  New-Item -ItemType Directory -Force $bin | Out-Null
  Copy-Item -Force $good (Join-Path $bin "erelang.exe")
  $debugDir = "D:\Development\Game\Physics\build\bin\Debug"
  if (Test-Path $debugDir) {
    Copy-Item -Force $good (Join-Path $debugDir "erelang.exe")
  }
  Copy-Item -Force $good "D:\Development\Game\Physics\build\bin\erelang.exe"
}

$user = [Environment]::GetEnvironmentVariable("Path", "User")
$parts = @($user -split ";" | ForEach-Object { $_.Trim() } | Where-Object {
  $_ -and ($_ -notmatch '[\\/]Physics[\\/]build[\\/]bin')
})
$parts = @($bin) + @($parts | Where-Object { $_ -ne $bin })
[Environment]::SetEnvironmentVariable("Path", ($parts -join ";"), "User")
Write-Host "PATH fixed. First erelang entry: $bin"
Write-Host "Refresh this shell with:"
Write-Host '  $env:Path = [Environment]::GetEnvironmentVariable("Path","User") + ";" + [Environment]::GetEnvironmentVariable("Path","Machine")'
