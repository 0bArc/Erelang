cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

if(NOT DEFINED BEST_EFFORT)
  set(BEST_EFFORT OFF)
endif()

set(_OB_FAILED OFF)

set(_ext_dir "${SOURCE_DIR}/erevos-language")
if(NOT IS_DIRECTORY "${_ext_dir}")
  if(BEST_EFFORT)
    message(STATUS "Extension directory not found: ${_ext_dir}; skipping")
    return()
  endif()
  message(FATAL_ERROR "Extension directory not found: ${_ext_dir}")
endif()

function(_run_or_handle)
  set(options)
  set(oneValueArgs WORKING_DIRECTORY LABEL)
  set(multiValueArgs COMMAND)
  cmake_parse_arguments(RUN "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  execute_process(
    COMMAND ${RUN_COMMAND}
    WORKING_DIRECTORY "${RUN_WORKING_DIRECTORY}"
    RESULT_VARIABLE _rc
  )

  if(NOT _rc EQUAL 0)
    if(BEST_EFFORT)
      message(WARNING "${RUN_LABEL} failed (exit ${_rc}); skipping")
      set(_OB_FAILED ON PARENT_SCOPE)
      return()
    endif()
    message(FATAL_ERROR "${RUN_LABEL} failed (exit ${_rc})")
  endif()
endfunction()

find_program(_npm NAMES npm npm.cmd)
if(NOT _npm)
  if(BEST_EFFORT)
    message(WARNING "npm not found; skipping extension packaging")
    return()
  endif()
  message(FATAL_ERROR "npm not found; cannot build extension")
endif()

if(NOT EXISTS "${_ext_dir}/node_modules")
  _run_or_handle(
    LABEL "npm install"
    WORKING_DIRECTORY "${_ext_dir}"
    COMMAND "${_npm}" install
  )
  if(_OB_FAILED)
    return()
  endif()
endif()

_run_or_handle(
  LABEL "npm run compile"
  WORKING_DIRECTORY "${_ext_dir}"
  COMMAND "${_npm}" run compile
)
if(_OB_FAILED)
  return()
endif()

find_program(_npx NAMES npx npx.cmd)
find_program(_vsce NAMES vsce vsce.cmd)

if(_npx)
  _run_or_handle(
    LABEL "npx vsce package"
    WORKING_DIRECTORY "${_ext_dir}"
    COMMAND "${_npx}" vsce package --allow-missing-repository
  )
  if(_OB_FAILED)
    return()
  endif()
elseif(_vsce)
  _run_or_handle(
    LABEL "vsce package"
    WORKING_DIRECTORY "${_ext_dir}"
    COMMAND "${_vsce}" package --allow-missing-repository
  )
  if(_OB_FAILED)
    return()
  endif()
else()
  if(BEST_EFFORT)
    message(WARNING "npx/vsce not found; skipping VSIX packaging")
    return()
  endif()
  message(FATAL_ERROR "npx or vsce not found; cannot package extension")
endif()

file(GLOB _vsix_files "${_ext_dir}/*.vsix")
if(NOT _vsix_files)
  if(BEST_EFFORT)
    message(WARNING "No VSIX produced; skipping install")
    return()
  endif()
  message(FATAL_ERROR "No VSIX produced")
endif()

list(SORT _vsix_files)
list(REVERSE _vsix_files)
list(GET _vsix_files 0 _latest_vsix)
set(_stable_vsix "${_ext_dir}/erelang_language.vsix")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_latest_vsix}" "${_stable_vsix}"
  RESULT_VARIABLE _copy_rc
)
if(NOT _copy_rc EQUAL 0)
  if(BEST_EFFORT)
    message(WARNING "Failed to copy stable VSIX alias")
  else()
    message(FATAL_ERROR "Failed to copy stable VSIX alias")
  endif()
endif()

find_program(_code NAMES code.cmd code)
if(_code)
  execute_process(
    COMMAND "${_code}" --install-extension "${_stable_vsix}" --force
    RESULT_VARIABLE _code_rc
  )
  if(NOT _code_rc EQUAL 0)
    if(BEST_EFFORT)
      message(WARNING "VSIX install via code CLI failed")
      return()
    endif()
    message(FATAL_ERROR "VSIX install via code CLI failed")
  endif()
else()
  message(STATUS "code CLI not found; VSIX built at ${_stable_vsix}")
endif()
