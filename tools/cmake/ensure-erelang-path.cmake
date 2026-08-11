# Ensure BIN_DIR is first on the user PATH so plain `erelang` hits the latest install.
# Also drop stale Physics build\bin\Debug|Release entries that shadow it.
if(NOT DEFINED BIN_DIR OR BIN_DIR STREQUAL "")
  message(FATAL_ERROR "ensure-erelang-path.cmake: BIN_DIR is required")
endif()

file(TO_CMAKE_PATH "${BIN_DIR}" _bin)
string(REPLACE "\\" "/" _bin "${_bin}")

if(WIN32)
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
      "$bin = [System.IO.Path]::GetFullPath('${_bin}'); $user = [Environment]::GetEnvironmentVariable('Path','User'); if (-not $user) { $user = '' }; $parts = @($user -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' }); $norm = { param($p) try { [System.IO.Path]::GetFullPath($p.TrimEnd('\\')) } catch { $p.TrimEnd('\\') } }; $filtered = New-Object System.Collections.Generic.List[string]; foreach ($p in $parts) { $n = & $norm $p; if ($n -ieq $bin) { continue }; if ($n -match '[\\/]Physics[\\/]build[\\/]bin[\\/](Debug|Release)$') { continue }; $filtered.Add($p) | Out-Null }; $filtered.Insert(0, $bin); [Environment]::SetEnvironmentVariable('Path', ($filtered -join ';'), 'User'); Write-Output \"PATH: $bin first (stale Physics Debug/Release entries removed)\""
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
