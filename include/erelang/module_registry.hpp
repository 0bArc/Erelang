// Runtime hookup of builtin modules.
//
// Each module source file (network.cpp, regex.cpp, ...) registers its dispatch functions here from a
// static initializer and defines an anchor function. The core never names a module symbol, so when
// a program is compiled with `erelang --compile` only the modules it includes are linked: the
// generated main references their anchors, and the linker drops every other module object.
// The interpreter calls link_all_modules() so it has everything.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace erelang {

class Runtime;

namespace modules {

// Builtin call: return "" when the name is not handled by this module.
using DispatchFn = std::string (*)(Runtime* rt, const std::string& name, const std::vector<std::string>& argv);
// Method on a handle value such as "ws:3": return false when this handler does not own the id.
using HandleFn = bool (*)(Runtime* rt, int id, const std::string& method, const std::vector<std::string>& args,
                          std::string& out);

// Register from a static initializer. `moduleId` matches registry::Module::id (net, regex, ...).
bool register_dispatch(const char* moduleId, DispatchFn fn);
// Several handlers may share a prefix; they are tried in priority order (lower first).
bool register_handle(const char* prefix, HandleFn fn, int priority = 10);

// Dispatch `name` to the modules a program imported (in import order). Empty when none handled it.
std::string dispatch(Runtime* rt, const std::vector<std::string>& moduleIds, const std::string& name,
                     const std::vector<std::string>& argv);
// True when some module registered handle methods for this prefix.
bool has_handle_prefix(std::string_view prefix);
// Call a handle method. False when no handler owns the handle.
bool dispatch_handle(Runtime* rt, std::string_view prefix, int id, const std::string& method,
                     const std::vector<std::string>& args, std::string& out);

// Source module that implements a registry module (process is implemented by system), or nullptr
// when the module lives in the core and needs nothing linked.
const char* implementation_of(std::string_view moduleId);

// Force every module into the binary (interpreter, DLL).
void link_all_modules();

}  // namespace modules
}  // namespace erelang

// Defines the anchor `erelang_module_anchor_<id>` that pulls a module object into a static link.
#define ERELANG_MODULE_ANCHOR(id) extern "C" void erelang_module_anchor_##id() {}
