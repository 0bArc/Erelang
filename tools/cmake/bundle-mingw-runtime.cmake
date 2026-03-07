cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED TARGET_DIR OR TARGET_DIR STREQUAL "")
  message(FATAL_ERROR "TARGET_DIR is required")
endif()

if(NOT IS_DIRECTORY "${TARGET_DIR}")
  message(STATUS "Target directory does not exist yet: ${TARGET_DIR}")
  return()
endif()

if(NOT DEFINED COMPILER_PATH OR COMPILER_PATH STREQUAL "")
  message(WARNING "COMPILER_PATH not provided; skipping MinGW runtime bundling")
  return()
endif()

get_filename_component(_compiler_dir "${COMPILER_PATH}" DIRECTORY)
if(NOT IS_DIRECTORY "${_compiler_dir}")
  message(WARNING "Compiler directory not found (${_compiler_dir}); skipping MinGW runtime bundling")
  return()
endif()

set(_runtime_dlls
  libstdc++-6.dll
  libgcc_s_seh-1.dll
  libwinpthread-1.dll
)

foreach(_dll IN LISTS _runtime_dlls)
  set(_src "${_compiler_dir}/${_dll}")
  if(EXISTS "${_src}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${TARGET_DIR}/${_dll}"
      RESULT_VARIABLE _copy_rc
      OUTPUT_QUIET
      ERROR_QUIET
    )
    if(NOT _copy_rc EQUAL 0)
      message(WARNING "Failed to copy ${_dll} from ${_src}")
    endif()
  endif()
endforeach()
