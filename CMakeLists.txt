cmake_minimum_required(VERSION 3.20)
project(erelang LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(_OB_OUTPUT_BIN "${CMAKE_BINARY_DIR}/bin")
set(_OB_OUTPUT_DLL "${_OB_OUTPUT_BIN}/dll")
set(_OB_OUTPUT_LIB "${CMAKE_BINARY_DIR}/lib")

function(ob_set_output target kind)
  set(bin "${_OB_OUTPUT_BIN}/$<CONFIG>")
  set(dll "${_OB_OUTPUT_DLL}/$<CONFIG>")
  set(lib "${_OB_OUTPUT_LIB}/$<CONFIG>")
  if (kind STREQUAL "EXECUTABLE")
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${bin}")
  elseif (kind STREQUAL "SHARED")
    set_target_properties(${target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${dll}"
      LIBRARY_OUTPUT_DIRECTORY "${dll}"
      ARCHIVE_OUTPUT_DIRECTORY "${lib}"
    )
  elseif (kind STREQUAL "STATIC")
    set_target_properties(${target} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${lib}")
  endif()
endfunction()

# Control whether erelang embeds the full compiler payload (static lib + headers). Off by default to keep erelang small.
option(ERELANG_EMBED_PAYLOAD "Embed erelang static lib and headers into erelang for out-of-tree portability" OFF)
option(SPLIT_RUNTIME_DLLS "Build runtime as many small component DLLs instead of one monolithic erelang.dll" OFF)
option(BUILD_SHARED_RUNTIME "Build erelang as a shared DLL (erelang.dll). OFF = no runtime DLLs produced" OFF)
option(BUILD_DEBUGGER_EXE "Build debug.exe helper via --make-debug after erelang links" OFF)

# Default to Release if not specified
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Release")
  # Favor size + link-time optimization
  if (MSVC)
    add_compile_options(/O1 /GL)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")
    set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
  else()
      add_compile_options(-Os -ffunction-sections -fdata-sections)
      add_link_options(-Wl,--gc-sections)
      # Avoid enabling IPO/LTO globally for MinGW/GCC to prevent ICE with <filesystem>/DLL link
      include(CheckIPOSupported)
      check_ipo_supported(RESULT lto_ok OUTPUT lto_out)
      if (lto_ok AND MSVC)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
      endif()
    endif()
endif()

add_library(erelang STATIC
    src/lexer.cpp
    src/parser.cpp
    src/runtime.cpp
  src/erodsl/spec.cpp
  src/builtins/math.cpp
  src/builtins/network.cpp
  src/builtins/system.cpp
  src/builtins/crypto.cpp
  src/builtins/data.cpp
  src/builtins/regex.cpp
  src/builtins/perm.cpp
  src/builtins/binary.cpp
  src/builtins/threads.cpp
  src/builtins/monitor.cpp
  src/ast.cpp
  src/typechecker.cpp
  src/error.cpp
  src/optimizer.cpp
  src/gc.cpp
  src/ffi.cpp
  src/stdlib.cpp
  src/modules.cpp
  src/symboltable.cpp
  src/plugins.cpp
  src/creation_kit.cpp
  src/language_profile.cpp
)

target_include_directories(erelang PUBLIC include)
ob_set_output(erelang STATIC)
if (WIN32)
  add_custom_command(TARGET erelang POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:erelang>"
            "$<TARGET_FILE_DIR:erelang>/libobskript.a"
    COMMENT "Creating legacy static library alias libobskript.a"
  )
endif()

# Build a shared runtime (erelang.dll) only if requested. Default OFF for single-file erelang builds.
if (BUILD_SHARED_RUNTIME AND SPLIT_RUNTIME_DLLS)
  ## ---- Split shared runtime into many DLLs (legacy mode) ----
  # Component DLLs (export all symbols to allow linking from host DLL)
  add_library(ob_frontend SHARED
    src/lexer.cpp
    src/parser.cpp
    src/ast.cpp
    src/language_profile.cpp
  )
  set_target_properties(ob_frontend PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_frontend PUBLIC include)
  set_property(TARGET ob_frontend PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_frontend SHARED)

  add_library(ob_semantic SHARED
    src/typechecker.cpp
    src/symboltable.cpp
    src/error.cpp
  )
  set_target_properties(ob_semantic PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_semantic PUBLIC include)
  set_property(TARGET ob_semantic PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_semantic SHARED)

  add_library(ob_optimizer SHARED src/optimizer.cpp)
  set_target_properties(ob_optimizer PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_optimizer PUBLIC include)
  set_property(TARGET ob_optimizer PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_optimizer SHARED)

  add_library(ob_gc SHARED src/gc.cpp)
  set_target_properties(ob_gc PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_gc PUBLIC include)
  set_property(TARGET ob_gc PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_gc SHARED)

  add_library(ob_ffi SHARED src/ffi.cpp)
  set_target_properties(ob_ffi PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_ffi PUBLIC include)
  set_property(TARGET ob_ffi PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_ffi SHARED)

  add_library(ob_builtins_math SHARED src/builtins/math.cpp)
  set_target_properties(ob_builtins_math PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_builtins_math PUBLIC include)
  set_property(TARGET ob_builtins_math PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_builtins_math SHARED)

  add_library(ob_builtins_network SHARED src/builtins/network.cpp)
  set_target_properties(ob_builtins_network PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_builtins_network PUBLIC include)
  set_property(TARGET ob_builtins_network PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_builtins_network SHARED)

  add_library(ob_builtins_system SHARED src/builtins/system.cpp)
  set_target_properties(ob_builtins_system PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_builtins_system PUBLIC include)
  set_property(TARGET ob_builtins_system PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_builtins_system SHARED)

  add_library(ob_stdlib SHARED src/stdlib.cpp)
  set_target_properties(ob_stdlib PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_stdlib PUBLIC include)
  target_link_libraries(ob_stdlib PUBLIC ob_builtins_math ob_builtins_network ob_builtins_system)
  set_property(TARGET ob_stdlib PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_stdlib SHARED)

  add_library(ob_modules SHARED src/modules.cpp)
  set_target_properties(ob_modules PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_include_directories(ob_modules PUBLIC include)
  set_property(TARGET ob_modules PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(ob_modules SHARED)

  add_library(erelang_shared SHARED
    src/runtime.cpp
    src/cabi_exports.cpp
    src/language_profile.cpp
    src/lexer.cpp
    src/parser.cpp
    src/ast.cpp
    src/typechecker.cpp
    src/symboltable.cpp
    src/error.cpp
    src/optimizer.cpp
    src/gc.cpp
    src/ffi.cpp
    src/builtins/math.cpp
    src/builtins/network.cpp
    src/stdlib.cpp
    src/modules.cpp
  )
  set_target_properties(erelang_shared PROPERTIES OUTPUT_NAME "erelang")
  set_target_properties(erelang_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
  target_compile_definitions(erelang_shared PRIVATE BUILDING_ERELANG_DLL OB_C_ABI_IMPL)
  target_include_directories(erelang_shared PUBLIC include)
  set_property(TARGET erelang_shared PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(erelang_shared SHARED)
  target_link_libraries(erelang_shared PUBLIC
    ob_frontend
    ob_semantic
    ob_optimizer
    ob_gc
    ob_ffi
    ob_stdlib
    ob_modules
    ob_builtins_math
    ob_builtins_network
  )
  if (WIN32)
    # Link common Windows libs where broadly needed
    foreach(tgt IN ITEMS ob_frontend ob_semantic ob_optimizer ob_gc ob_ffi ob_stdlib ob_modules ob_builtins_math ob_builtins_network ob_builtins_system erelang_shared)
      target_link_libraries(${tgt} PUBLIC advapi32 user32 gdi32 comctl32 rpcrt4 winhttp)
      target_compile_definitions(${tgt} PUBLIC UNICODE _UNICODE)
    endforeach()
  endif()

  if (NOT MSVC)
    foreach(tgt IN ITEMS ob_frontend ob_semantic ob_optimizer ob_gc ob_ffi ob_stdlib ob_modules ob_builtins_math ob_builtins_network ob_builtins_system erelang_shared)
      target_compile_options(${tgt} PRIVATE -fno-lto)
      target_link_options(${tgt} PRIVATE -fno-lto)
    endforeach()
  endif()
elseif (BUILD_SHARED_RUNTIME)
  ## ---- Monolithic runtime: single erelang.dll ----
  add_library(erelang_shared SHARED
    src/lexer.cpp
    src/parser.cpp
    src/ast.cpp
    src/language_profile.cpp
    src/erodsl/spec.cpp
    src/erodsl/spec.cpp
    src/typechecker.cpp
    src/symboltable.cpp
    src/error.cpp
    src/optimizer.cpp
    src/gc.cpp
    src/ffi.cpp
    src/builtins/math.cpp
    src/builtins/network.cpp
    src/stdlib.cpp
    src/modules.cpp
    src/runtime.cpp
    src/cabi_exports.cpp
  )
  set_target_properties(erelang_shared PROPERTIES OUTPUT_NAME "erelang")
  target_compile_definitions(erelang_shared PRIVATE BUILDING_ERELANG_DLL OB_C_ABI_IMPL)
  target_include_directories(erelang_shared PUBLIC include)
  set_property(TARGET erelang_shared PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
  ob_set_output(erelang_shared SHARED)
  if (WIN32)
    target_link_libraries(erelang_shared PUBLIC advapi32 user32 gdi32 comctl32 rpcrt4 winhttp)
    target_compile_definitions(erelang_shared PUBLIC UNICODE _UNICODE)
  endif()
  if (NOT MSVC)
    target_compile_options(erelang_shared PRIVATE -fno-lto)
    target_link_options(erelang_shared PRIVATE -fno-lto)
  endif()
endif()

# Ensure erelang builds after the shared DLL so its post-build step can run (added after erelang is defined)

add_executable(obc src/main.cpp)

target_link_libraries(obc PRIVATE erelang)
ob_set_output(obc EXECUTABLE)

# simple runner helper that prints a usage hint
add_executable(erelang_runner src/obs_main.cpp)
set_target_properties(erelang_runner PROPERTIES OUTPUT_NAME "erelang")
target_include_directories(erelang_runner PRIVATE include)
ob_set_output(erelang_runner EXECUTABLE)
if (WIN32)
  add_custom_command(TARGET erelang_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:erelang_runner>"
            "$<TARGET_FILE_DIR:erelang_runner>/obs.exe"
    COMMENT "Creating legacy runner alias obs.exe"
  )
endif()
if (WIN32)
  target_compile_definitions(erelang_runner PRIVATE UNICODE _UNICODE)
  target_link_libraries(erelang_runner PRIVATE erelang advapi32 user32 gdi32 comctl32 rpcrt4 winhttp shell32)
else()
  target_link_libraries(erelang_runner PRIVATE erelang)
endif()
target_compile_definitions(erelang_runner PRIVATE ERELANG_STATIC_RUNNER=1)
set_property(TARGET erelang_runner PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
if (NOT MSVC)
  target_link_options(erelang_runner PRIVATE -s)
  target_compile_options(erelang_runner PRIVATE -fno-lto)
  target_link_options(erelang_runner PRIVATE -fno-lto)
endif()

if (BUILD_SHARED_RUNTIME)
  add_dependencies(erelang_runner erelang_shared)
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
  add_custom_command(TARGET erelang_runner POST_BUILD
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:erelang_runner>
    COMMENT "Stripping symbols from erelang for smaller size")
endif()

if (BUILD_DEBUGGER_EXE)
  add_custom_command(TARGET erelang_runner POST_BUILD
    COMMAND $<TARGET_FILE:erelang_runner> --make-debug --output $<TARGET_FILE_DIR:erelang_runner>/debug.exe
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Building debug.exe via erelang"
  )
endif()


if (WIN32 AND BUILD_SHARED_RUNTIME AND NOT SPLIT_RUNTIME_DLLS)
  add_custom_command(TARGET erelang_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove -f
            "$<TARGET_FILE_DIR:erelang_runner>/libob_frontend.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/libob_semantic.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/libob_optimizer.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/libob_gc.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/libob_ffi.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/libob_builtins_math.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/libob_builtins_network.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/libob_stdlib.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/libob_modules.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_frontend.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_semantic.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_optimizer.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_gc.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_ffi.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_builtins_math.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_builtins_network.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_stdlib.dll"
            "$<TARGET_FILE_DIR:erelang_runner>/ob_modules.dll"
    COMMENT "Cleaning up old split DLLs (if any)"
  )
endif()

if (EXISTS "${CMAKE_SOURCE_DIR}/erevos-language")
  add_custom_command(TARGET erelang_runner POST_BUILD
    COMMAND pwsh -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/build-and-update-extension.ps1
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packaging and installing VS Code extension (best-effort)"
  )
endif()

if (MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Windows-specific UI/linking settings
if (WIN32)

  target_link_libraries(erelang PUBLIC advapi32 user32 gdi32 comctl32 rpcrt4 winhttp)
  target_compile_definitions(erelang PUBLIC UNICODE _UNICODE)
  target_link_libraries(erelang_runner PUBLIC advapi32 user32 gdi32 comctl32 rpcrt4 winhttp)
  target_compile_definitions(erelang_runner PUBLIC UNICODE _UNICODE)

  # Compute path to the built static library (used when embedding payload)
  add_custom_target(_ensure_lib ALL DEPENDS erelang)
  if (MSVC)
    set(_OB_LIB_PATH $<TARGET_FILE_DIR:erelang>/erelang.lib)
    set(_OB_LIB_EXT ".lib")
  else()
    # MinGW/Clang typically produce liberelang.a
    set(_OB_LIB_PATH $<TARGET_FILE_DIR:erelang>/liberelang.a)
    set(_OB_LIB_EXT ".a")
  endif()

  # Always include erelang resources (manifest). Actual payload RCDATA controlled via defines.
  # Removed reference to win/erelang.rc as the file is missing.

  if (ERELANG_EMBED_PAYLOAD)
    target_compile_definitions(erelang_runner PRIVATE ERELANG_EMBED_LIB_PATH="\"${_OB_LIB_PATH}\"")
    target_compile_definitions(erelang_runner PRIVATE ERELANG_EMBED_LIB_EXT="\"${_OB_LIB_EXT}\"")
    target_compile_definitions(erelang_runner PRIVATE 
      ERELANG_EMBED_INC_LEXER_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/lexer.hpp\""
      ERELANG_EMBED_INC_PARSER_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/parser.hpp\""
      ERELANG_EMBED_INC_RUNTIME_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/runtime.hpp\""
      ERELANG_EMBED_INC_TYPECHECKER_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/typechecker.hpp\""
      ERELANG_EMBED_INC_OPTIMIZER_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/optimizer.hpp\""
      ERELANG_EMBED_INC_SYMBOLTABLE_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/symboltable.hpp\""
      ERELANG_EMBED_INC_MODULES_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/modules.hpp\""
      ERELANG_EMBED_INC_STDLIB_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/stdlib.hpp\""
      ERELANG_EMBED_INC_FFI_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/ffi.hpp\""
      ERELANG_EMBED_INC_ERROR_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/error.hpp\""
      ERELANG_EMBED_INC_AST_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/ast.hpp\""
      ERELANG_EMBED_INC_GC_PATH="\"${CMAKE_SOURCE_DIR}/include/erelang/gc.hpp\""
    )
    # Embed cpp/std modules if paths are provided
    if (EXISTS "${CMAKE_SOURCE_DIR}/cpp/std/math.0bs")
      target_compile_definitions(erelang_runner PRIVATE ERELANG_EMBED_CPP_STD_MATH_PATH="\"${CMAKE_SOURCE_DIR}/cpp/std/math.0bs\"")
    endif()
    add_dependencies(erelang_runner _ensure_lib)
  endif()
endif()

add_custom_target(update_extension
  COMMAND pwsh -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/build-and-update-extension.ps1
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Build compiler and package/install VS Code extension"
)

add_dependencies(update_extension obc)

# Simple convenience launcher 'build' that proxies to erelang (runner) so user can run build.exe <script>
add_executable(build WIN32 src/build_launcher.cpp)
if (WIN32)
  target_link_libraries(build PRIVATE user32 gdi32 comctl32 shell32 dwmapi msimg32)
endif()
if (WIN32)
  set_target_properties(build PROPERTIES OUTPUT_NAME "build")
endif()
ob_set_output(build EXECUTABLE)

