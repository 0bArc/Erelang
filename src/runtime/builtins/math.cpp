// Built-in math module for erelang
#include "erelang/runtime.hpp"
#include <cmath>
#include <limits>
#include <vector>


namespace erelang {

// Extend Runtime::eval_builtin_call via a weak-link style helper
static std::string math_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    auto argS = [&](size_t i){ return i < argv.size() ? argv[i] : std::string(); };
    auto to_d = [&](const std::string& s){ try { return std::stod(s); } catch (...) { return 0.0; } };
    auto to_i = [&](const std::string& s){ try { return (long long)std::stoll(s); } catch (...) { return 0LL; } };
    constexpr long long kMin = std::numeric_limits<long long>::min();
    constexpr long long kMax = std::numeric_limits<long long>::max();
    auto checked_add = [](long long a, long long b) -> long long {
        if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) throw std::runtime_error("math: integer overflow in add");
        return a + b;
    };
    auto checked_sub = [](long long a, long long b) -> long long {
        if ((b < 0 && a > kMax + b) || (b > 0 && a < kMin + b)) throw std::runtime_error("math: integer overflow in sub");
        return a - b;
    };
    auto checked_mul = [](long long a, long long b) -> long long {
        if (a == 0 || b == 0) return 0;
        if (a == -1 && b == kMin) throw std::runtime_error("math: integer overflow in mul");
        if (b == -1 && a == kMin) throw std::runtime_error("math: integer overflow in mul");
        long long r = a * b;
        if (r / b != a) throw std::runtime_error("math: integer overflow in mul");
        return r;
    };
    // integer arithmetic helpers
    if (name == "add") { return std::to_string(checked_add(to_i(argS(0)), to_i(argS(1)))); }
    if (name == "sub") { return std::to_string(checked_sub(to_i(argS(0)), to_i(argS(1)))); }
    if (name == "mul") { return std::to_string(checked_mul(to_i(argS(0)), to_i(argS(1)))); }
    if (name == "div") {
        long long a = to_i(argS(0)), b = to_i(argS(1));
        if (b == 0) throw std::runtime_error("math: division by zero");
        if (a == kMin && b == -1) throw std::runtime_error("math: integer overflow in div");
        return std::to_string(a / b);
    }
    if (name == "mod") {
        long long a = to_i(argS(0)), b = to_i(argS(1));
        if (b == 0) throw std::runtime_error("math: modulo by zero");
        if (a == kMin && b == -1) return std::to_string(0);
        return std::to_string(a % b);
    }
    if (name == "sin") { return std::to_string(std::sin(to_d(argS(0)))); }
    if (name == "cos") { return std::to_string(std::cos(to_d(argS(0)))); }
    if (name == "tan") { return std::to_string(std::tan(to_d(argS(0)))); }
    if (name == "sqrt") { return std::to_string((long long)std::round(std::sqrt(std::max(0.0, to_d(argS(0)))))); }
    if (name == "pow") { return std::to_string((long long)std::round(std::pow(to_d(argS(0)), to_d(argS(1))))); }
    if (name == "abs") {
        long long a = to_i(argS(0));
        if (a == kMin) return std::to_string(kMax); // -INT64_MIN would be UB
        return std::to_string(std::llabs(a));
    }
    if (name == "min") { long long a = to_i(argS(0)), b = to_i(argS(1)); return std::to_string(a < b ? a : b); }
    if (name == "max") { long long a = to_i(argS(0)), b = to_i(argS(1)); return std::to_string(a > b ? a : b); }
    if (name == "collatz_len") {
        long long x = to_i(argS(0));
        if (x < 1) return std::string("0");
        long long steps = 0;
        const long long kOverflowLimit = (std::numeric_limits<long long>::max() - 1) / 3;
        while (x > 1) {
            if ((x & 1LL) == 0) { x >>= 1; ++steps; }
            else {
                if (x > kOverflowLimit) return std::string("0"); // 3x+1 would overflow; bail
                x = 3 * x + 1; ++steps;
            }
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
        // Guard against script-controlled unbounded allocation.
        constexpr long long kMaxCollatzLimit = 10000000;
        if (limit > kMaxCollatzLimit) limit = kMaxCollatzLimit;
        std::vector<int> cache; cache.resize((size_t)limit + 1); cache[1] = 1;
        g_collatz_best_n = 1; g_collatz_best_steps = 1; g_collatz_total_steps = 0;
        const long long kOverflowLimit = (std::numeric_limits<long long>::max() - 1) / 3;
        for (long long n = 2; n <= limit; ++n) {
            long long x = n; int steps = 0; std::vector<long long> seq; seq.reserve(64);
            bool overflow = false;
            while (x > 1) {
                if (x <= limit && cache[(size_t)x] != 0) { steps += cache[(size_t)x]; break; }
                seq.push_back(x);
                if ((x & 1LL) == 0) { x >>= 1; ++steps; }
                else {
                    if (x > kOverflowLimit) { overflow = true; break; }
                    x = 3 * x + 1; ++steps;
                }
            }
            if (overflow) {
                g_collatz_last_limit = limit;
                return std::string("0");
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
