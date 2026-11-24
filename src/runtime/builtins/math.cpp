// builtin/math: numeric functions.
//
// Integer helpers (add, sub, mul, div, mod) are exact and report overflow and division by zero as
// errors. Everything that is naturally real-valued (sqrt, pow, trig, logs) returns a float. min,
// max, abs and clamp keep ints as ints and only switch to float when an argument is a float.
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "erelang/runtime.hpp"

namespace erelang {

namespace {

constexpr long long kMin = std::numeric_limits<long long>::min();
constexpr long long kMax = std::numeric_limits<long long>::max();

// A builtin argument as a number. `isInt` is true for integer text ("42", "-7").
struct Num {
    double d = 0.0;
    long long i = 0;
    bool isInt = true;
};

Num parse(const std::string& s) {
    Num n;
    const char* b = s.data();
    const char* e = s.data() + s.size();
    long long iv = 0;
    auto [p, ec] = std::from_chars(b, e, iv);
    if (ec == std::errc() && p == e && !s.empty()) {
        n.i = iv;
        n.d = static_cast<double>(iv);
        return n;
    }
    n.isInt = false;
    try { n.d = std::stod(s); } catch (...) { n.d = 0.0; }
    n.i = static_cast<long long>(n.d);
    return n;
}

std::string fmt_int(long long v) { return std::to_string(v); }

// Shortest text that reads back as the same double; always shows it is a float ("2.0", not "2").
std::string fmt_float(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char buf[64];
    auto [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
    std::string out(buf, ec == std::errc() ? p : buf);
    if (out.find_first_of(".eE") == std::string::npos) out += ".0";
    return out;
}

std::string fmt(const Num& n) { return n.isInt ? fmt_int(n.i) : fmt_float(n.d); }

long long checked_add(long long a, long long b) {
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) throw std::runtime_error("math.add: integer overflow");
    return a + b;
}
long long checked_sub(long long a, long long b) {
    if ((b < 0 && a > kMax + b) || (b > 0 && a < kMin + b)) throw std::runtime_error("math.sub: integer overflow");
    return a - b;
}
long long checked_mul(long long a, long long b) {
    long long r = 0;
    if (__builtin_mul_overflow(a, b, &r)) throw std::runtime_error("math.mul: integer overflow");
    return r;
}

long long gcd_ll(long long a, long long b) {
    if (a == kMin || b == kMin) throw std::runtime_error("math.gcd: value out of range");
    a = a < 0 ? -a : a;
    b = b < 0 ? -b : b;
    while (b != 0) {
        const long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

bool is_prime(long long n) {
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    if (n % 3 == 0) return n == 3;
    for (long long f = 5; f <= n / f; f += 6) {
        if (n % f == 0 || n % (f + 2) == 0) return false;
    }
    return true;
}

long long to_int_rounded(double v, const char* fn) {
    if (!std::isfinite(v) || v < -9.2233720368547758e18 || v >= 9.2233720368547758e18) {
        throw std::runtime_error(std::string("math.") + fn + ": result does not fit in an int");
    }
    return static_cast<long long>(v);
}

// Collatz statistics of the last collatz_sweep.
struct CollatzStats {
    long long lastLimit = 0;
    long long bestN = 0;
    long long bestSteps = 0;
    long long totalSteps = 0;
};
CollatzStats g_collatz;

long long collatz_len(long long x) {
    if (x < 1) return 0;
    long long steps = 0;
    const long long kLimit = (kMax - 1) / 3;
    while (x > 1) {
        if ((x & 1LL) == 0) {
            x >>= 1;
        } else {
            if (x > kLimit) return 0;  // 3x+1 would overflow
            x = 3 * x + 1;
        }
        ++steps;
    }
    return steps;
}

long long collatz_sweep(long long limit) {
    if (limit < 2) limit = 2;
    constexpr long long kMaxLimit = 10000000;  // bound script controlled allocation
    if (limit > kMaxLimit) limit = kMaxLimit;
    std::vector<int> cache(static_cast<std::size_t>(limit) + 1, 0);
    cache[1] = 1;
    g_collatz = CollatzStats{limit, 1, 1, 0};
    const long long kOverflow = (kMax - 1) / 3;
    std::vector<long long> seq;
    seq.reserve(128);
    for (long long n = 2; n <= limit; ++n) {
        long long x = n;
        int steps = 0;
        seq.clear();
        while (x > 1) {
            if (x <= limit && cache[static_cast<std::size_t>(x)] != 0) {
                steps += cache[static_cast<std::size_t>(x)];
                break;
            }
            seq.push_back(x);
            if ((x & 1LL) == 0) {
                x >>= 1;
            } else {
                if (x > kOverflow) return 0;
                x = 3 * x + 1;
            }
            ++steps;
        }
        int len = steps;
        for (const long long v : seq) {
            if (v <= limit && cache[static_cast<std::size_t>(v)] == 0) cache[static_cast<std::size_t>(v)] = len;
            --len;
        }
        g_collatz.totalSteps += steps;
        if (steps > g_collatz.bestSteps) {
            g_collatz.bestSteps = steps;
            g_collatz.bestN = n;
        }
    }
    return g_collatz.bestN;
}

std::string math_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    const auto num = [&](std::size_t i) { return parse(i < argv.size() ? argv[i] : std::string()); };
    const auto real = [&](double v) { return fmt_float(v); };
    const auto unary = [&](double (*f)(double)) { return real(f(num(0).d)); };

    // Exact integer arithmetic.
    if (name == "add") return fmt_int(checked_add(num(0).i, num(1).i));
    if (name == "sub") return fmt_int(checked_sub(num(0).i, num(1).i));
    if (name == "mul") return fmt_int(checked_mul(num(0).i, num(1).i));
    if (name == "div" || name == "mod") {
        const long long a = num(0).i, b = num(1).i;
        if (b == 0) throw std::runtime_error(name == "div" ? "math.div: division by zero" : "math.mod: modulo by zero");
        if (a == kMin && b == -1) {
            if (name == "div") throw std::runtime_error("math.div: integer overflow");
            return "0";
        }
        return fmt_int(name == "div" ? a / b : a % b);
    }
    if (name == "gcd") return fmt_int(gcd_ll(num(0).i, num(1).i));
    if (name == "lcm") {
        const long long a = num(0).i, b = num(1).i;
        if (a == 0 || b == 0) return "0";
        const long long g = gcd_ll(a, b);
        return fmt_int(std::llabs(checked_mul(a / g, b)));
    }
    if (name == "is_prime") return is_prime(num(0).i) ? "true" : "false";
    if (name == "factorial") {
        const long long n = num(0).i;
        if (n < 0) throw std::runtime_error("math.factorial: negative argument");
        if (n > 20) throw std::runtime_error("math.factorial: n above 20 does not fit in an int");
        long long r = 1;
        for (long long k = 2; k <= n; ++k) r *= k;
        return fmt_int(r);
    }

    // Keep the argument kind.
    if (name == "abs") {
        const Num a = num(0);
        if (!a.isInt) return real(std::fabs(a.d));
        if (a.i == kMin) throw std::runtime_error("math.abs: integer overflow");
        return fmt_int(a.i < 0 ? -a.i : a.i);
    }
    if (name == "min" || name == "max") {
        const Num a = num(0), b = num(1);
        const bool pickA = name == "min" ? a.d <= b.d : a.d >= b.d;
        if (a.isInt && b.isInt) return fmt_int(name == "min" ? std::min(a.i, b.i) : std::max(a.i, b.i));
        return real(pickA ? a.d : b.d);
    }
    if (name == "clamp") {
        const Num v = num(0), lo = num(1), hi = num(2);
        if (lo.d > hi.d) throw std::runtime_error("math.clamp: lo is greater than hi");
        if (v.isInt && lo.isInt && hi.isInt) return fmt_int(v.i < lo.i ? lo.i : (v.i > hi.i ? hi.i : v.i));
        return real(v.d < lo.d ? lo.d : (v.d > hi.d ? hi.d : v.d));
    }
    if (name == "sign") {
        const double d = num(0).d;
        return fmt_int(d > 0 ? 1 : (d < 0 ? -1 : 0));
    }

    // Real valued.
    if (name == "sin") return unary(std::sin);
    if (name == "cos") return unary(std::cos);
    if (name == "tan") return unary(std::tan);
    if (name == "asin") return unary(std::asin);
    if (name == "acos") return unary(std::acos);
    if (name == "atan") return unary(std::atan);
    if (name == "atan2") return real(std::atan2(num(0).d, num(1).d));
    if (name == "sqrt") {
        const double v = num(0).d;
        if (v < 0) throw std::runtime_error("math.sqrt: negative argument");
        return real(std::sqrt(v));
    }
    if (name == "cbrt") return unary(std::cbrt);
    if (name == "pow") return real(std::pow(num(0).d, num(1).d));
    if (name == "exp") return unary(std::exp);
    if (name == "log" || name == "log2" || name == "log10") {
        const double v = num(0).d;
        if (v <= 0) throw std::runtime_error("math." + name + ": argument must be positive");
        return real(name == "log" ? std::log(v) : name == "log2" ? std::log2(v) : std::log10(v));
    }
    if (name == "hypot") return real(std::hypot(num(0).d, num(1).d));
    if (name == "lerp") {
        const double a = num(0).d, b = num(1).d, t = num(2).d;
        return real(a + (b - a) * t);
    }
    if (name == "pi") return real(3.14159265358979323846);
    if (name == "e") return real(2.71828182845904523536);
    if (name == "radians") return real(num(0).d * 3.14159265358979323846 / 180.0);
    if (name == "degrees") return real(num(0).d * 180.0 / 3.14159265358979323846);

    // Rounding to int.
    if (name == "floor") return fmt_int(to_int_rounded(std::floor(num(0).d), "floor"));
    if (name == "ceil") return fmt_int(to_int_rounded(std::ceil(num(0).d), "ceil"));
    if (name == "round") return fmt_int(to_int_rounded(std::round(num(0).d), "round"));
    if (name == "trunc") return fmt_int(to_int_rounded(std::trunc(num(0).d), "trunc"));

    // Collatz.
    if (name == "collatz_len") return fmt_int(collatz_len(num(0).i));
    if (name == "collatz_sweep") return fmt_int(collatz_sweep(num(0).i));
    if (name == "collatz_best_n") return fmt_int(g_collatz.bestN);
    if (name == "collatz_best_steps") return fmt_int(g_collatz.bestSteps);
    if (name == "collatz_total_steps") return fmt_int(g_collatz.totalSteps);
    if (name == "collatz_avg_steps") {
        return fmt_int(g_collatz.lastLimit > 0 ? g_collatz.totalSteps / g_collatz.lastLimit : 0);
    }
    return {};
}

}  // namespace

std::string __erelang_builtin_math_dispatch(const std::string& name, const std::vector<std::string>& argv) {
    return math_dispatch(name, argv);
}

}  // namespace erelang

// Module registration: see include/erelang/module_registry.hpp.
#include "erelang/module_registry.hpp"
namespace {
const bool kModuleRegistered = erelang::modules::register_dispatch("math",
    [](erelang::Runtime*, const std::string& name, const std::vector<std::string>& argv) {
        return erelang::__erelang_builtin_math_dispatch(name, argv);
    });
}  // namespace
ERELANG_MODULE_ANCHOR(math)
