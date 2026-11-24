// Pulls every builtin module into a static link. Only the interpreter, the C ABI DLL and obc call
// link_all_modules(); a program built with `erelang --compile` references just the anchors of the
// modules it includes, so this file (and the modules) stay out of it.
#include "erelang/module_registry.hpp"

extern "C" {
void erelang_module_anchor_net();
void erelang_module_anchor_ws();
void erelang_module_anchor_regex();
void erelang_module_anchor_ai();
void erelang_module_anchor_crypto();
void erelang_module_anchor_perf();
void erelang_module_anchor_binary();
void erelang_module_anchor_threads();
void erelang_module_anchor_monitor();
void erelang_module_anchor_math();
void erelang_module_anchor_system();
void erelang_module_anchor_data();
void erelang_module_anchor_perm();
void erelang_module_anchor_json();
}

namespace erelang::modules {

void link_all_modules() {
    erelang_module_anchor_net();
    erelang_module_anchor_ws();
    erelang_module_anchor_regex();
    erelang_module_anchor_ai();
    erelang_module_anchor_crypto();
    erelang_module_anchor_perf();
    erelang_module_anchor_binary();
    erelang_module_anchor_threads();
    erelang_module_anchor_monitor();
    erelang_module_anchor_math();
    erelang_module_anchor_system();
    erelang_module_anchor_data();
    erelang_module_anchor_perm();
    erelang_module_anchor_json();
}

}  // namespace erelang::modules
