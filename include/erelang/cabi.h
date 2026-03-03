#pragma once

#ifdef _WIN32
  #ifdef BUILDING_ERELANG_DLL
    #define OB_API __declspec(dllexport)
  #else
    #define OB_API __declspec(dllimport)
  #endif
#else
  #define OB_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Flags for ob_run_file
// bit 0 (0x1): debug mode

// Runs a script file through the full pipeline and returns its exit code.
// On error, returns non-zero and may set *out_error (allocated by DLL; free with ob_free_string).
OB_API int ob_run_file(const char* main_file,
                       int argc, const char* argv[],
                       int flags,
                       char** out_error);

// Collect all files reachable from main_file (imports resolved), in dependency order.
// Calls on_file(path, contents, user) for each file. Returns 0 on success, non-zero on error and sets *out_error.
OB_API int ob_collect_files(const char* main_file,
                            void (*on_file)(const char* path, const char* contents, void* user),
                            void* user,
                            char** out_error);

// Free a C string allocated by the DLL (e.g., returned via out_error).
OB_API void ob_free_string(char* s);

#ifdef __cplusplus
}
#endif
