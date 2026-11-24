# Ensure BIN_DIR is first on the user PATH so plain `erelang` hits the latest build.
# Other PATH entries inside SOURCE_DIR (older build folders) are dropped so only one erelang wins.
# Only rewrites User PATH when BIN_DIR is not already first (avoids churn every build).
if(NOT DEFINED BIN_DIR OR BIN_DIR STREQUAL "")
  message(FATAL_ERROR "ensure-erelang-path.cmake: BIN_DIR is required")
endif()
if(NOT DEFINED SOURCE_DIR)
  set(SOURCE_DIR "")
endif()

file(TO_CMAKE_PATH "${BIN_DIR}" _bin)
file(TO_CMAKE_PATH "${SOURCE_DIR}" _src)

if(WIN32)
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
      "$bin = [System.IO.Path]::GetFullPath('${_bin}').TrimEnd('\\'); $src = '${_src}'; if ($src) { $src = [System.IO.Path]::GetFullPath($src).TrimEnd('\\') + '\\' }; $user = [Environment]::GetEnvironmentVariable('Path','User'); if (-not $user) { $user = '' }; $parts = @($user -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' }); $norm = { param($p) try { [System.IO.Path]::GetFullPath($p).TrimEnd('\\') } catch { $p.TrimEnd('\\') } }; if ($parts.Count -gt 0) { $first = & $norm $parts[0]; if ($first -ieq $bin) { Write-Output \"PATH: $bin already first\"; exit 0 } }; $filtered = New-Object System.Collections.Generic.List[string]; foreach ($p in $parts) { $n = & $norm $p; if ($n -ieq $bin) { continue }; if ($src -and ($n + '\\').StartsWith($src, [StringComparison]::OrdinalIgnoreCase)) { continue }; if ($n -match '[\\/]AppData[\\/]Local[\\/]Erelang[\\/]bin$') { continue }; $filtered.Add($p) | Out-Null }; $filtered.Insert(0, $bin); [Environment]::SetEnvironmentVariable('Path', ($filtered -join ';'), 'User'); Write-Output \"PATH: $bin first\""
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
  )
  if(NOT _rc EQUAL 0)
    message(WARNING "Could not update user PATH: ${_err}")
  else()
    message(STATUS "${_out}")
  endif()
endif()
