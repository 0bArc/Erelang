// Built-in math module for erelang
#include "erelang/runtime.hpp"
#include <cmath>
#include <vector>


namespace erelang {

// Extend Runtime::eval_builtin_call via a weak-link style helper
static std::string math_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i < argv.size() ? argv[i] : std::string(); };
    auto to_d = [&](const std::string& s){ try { return std::stod(s); } catch (...) { return 0.0; } };
    auto to_i = [&](const std::string& s){ try { return (long long)std::stoll(s); } catch (...) { return 0LL; } };
    // integer arithmetic helpers
    if (name == "add") { return std::to_string(to_i(argS(0)) + to_i(argS(1))); }
    if (name == "sub") { return std::to_string(to_i(argS(0)) - to_i(argS(1))); }
    if (name == "mul") { return std::to_string(to_i(argS(0)) * to_i(argS(1))); }
    if (name == "div") { long long b = to_i(argS(1)); return std::to_string(b==0?0:to_i(argS(0))/b); }
    if (name == "mod") { long long b = to_i(argS(1)); return std::to_string(b==0?0:to_i(argS(0))%b); }
    if (name == "sin") { return std::to_string(std::sin(to_d(argS(0)))); }
    if (name == "cos") { return std::to_string(std::cos(to_d(argS(0)))); }
    if (name == "tan") { return std::to_string(std::tan(to_d(argS(0)))); }
    if (name == "sqrt") { return std::to_string(std::sqrt(std::max(0.0, to_d(argS(0))))); }
    if (name == "pow") { return std::to_string(std::pow(to_d(argS(0)), to_d(argS(1)))); }
    if (name == "abs") { return std::to_string(std::llabs(to_i(argS(0)))); }
    if (name == "min") { long long a = to_i(argS(0)), b = to_i(argS(1)); return std::to_string(a < b ? a : b); }
    if (name == "max") { long long a = to_i(argS(0)), b = to_i(argS(1)); return std::to_string(a > b ? a : b); }
    if (name == "collatz_len") {
        long long x = to_i(argS(0));
        if (x < 1) return std::string("0");
        long long steps = 0;
        while (x > 1) {
            if ((x & 1LL) == 0) { x >>= 1; ++steps; }
            else { x = 3 * x + 1; ++steps; }
        }
        return std::to_string(steps);
    }
    // Fast full sweep with memoization; stores stats for subsequent queries.
    static long long g_collatz_last_limit = 0;
    static long long g_collatz_best_n = 0;
    static long long g_collatz_best_steps = 0;
    static long long g_collatz_total_steps = 0;
    if (name == "collatz_sweep") {
        long long limit = to_i(argS(0));
        if (limit < 2) limit = 2;
        std::vector<int> cache; cache.resize((size_t)limit + 1); cache[1] = 1;
        g_collatz_best_n = 1; g_collatz_best_steps = 1; g_collatz_total_steps = 0;
        for (long long n = 2; n <= limit; ++n) {
            long long x = n; int steps = 0; std::vector<long long> seq; seq.reserve(64);
            while (x > 1) {
                if (x <= limit && cache[(size_t)x] != 0) { steps += cache[(size_t)x]; break; }
                seq.push_back(x);
                if ((x & 1LL) == 0) { x >>= 1; ++steps; }
                else { x = 3 * x + 1; ++steps; }
            }
            int len = steps;
            // back-propagate
            for (size_t i = 0; i < seq.size(); ++i) {
                long long v = seq[i];
                if (v <= limit && cache[(size_t)v] == 0) {
                    cache[(size_t)v] = len;
                }
                --len;
            }
            g_collatz_total_steps += (steps > 0 ? steps : 0);
            if (steps > g_collatz_best_steps) { g_collatz_best_steps = steps; g_collatz_best_n = n; }
        }
        g_collatz_last_limit = limit;
        return std::to_string(g_collatz_best_n); // primary result: best n
    }
    if (name == "collatz_best_n") { return std::to_string(g_collatz_best_n); }
    if (name == "collatz_best_steps") { return std::to_string(g_collatz_best_steps); }
    if (name == "collatz_total_steps") { return std::to_string(g_collatz_total_steps); }
    if (name == "collatz_avg_steps") {
        if (g_collatz_last_limit > 0) return std::to_string(g_collatz_total_steps / g_collatz_last_limit);
        return std::string("0");
    }
    return {};
}

// Hook into Runtime via friend function access if necessary
std::string __erelang_builtin_math_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return math_dispatch(name, argv);
}

} // namespace erelang
