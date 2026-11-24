// builtin/ai: infer patterns and regexes from sample text.
#include <algorithm>
#include <string>
#include <vector>
#include "ai_internal.hpp"

namespace erelang::ai {

namespace {

enum class Cls { Digit, Letter, Other };

Cls classify(unsigned char c) {
    if (c >= '0' && c <= '9') return Cls::Digit;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return Cls::Letter;
    return Cls::Other;
}

// A literal inside an expect pattern: the wildcard characters need a backslash.
void append_expect_literal(std::string& out, char c) {
    if (c == 'x' || c == 'X' || c == '?' || c == '*' || c == '\\') out += '\\';
    out += c;
}

// One run of the same class, or one "other" character.
struct Run {
    Cls cls;
    std::string text;
};

std::vector<Run> runs_of(const std::string& s) {
    std::vector<Run> runs;
    for (const char ch : s) {
        const Cls cls = classify(static_cast<unsigned char>(ch));
        if (cls != Cls::Other && !runs.empty() && runs.back().cls == cls) runs.back().text += ch;
        else runs.push_back({cls, std::string(1, ch)});
    }
    return runs;
}

std::string quantifier(std::size_t lo, std::size_t hi) {
    if (lo == hi) return lo == 1 ? "" : "{" + std::to_string(lo) + "}";
    if (lo == 0 && hi == 1) return "?";
    return "{" + std::to_string(lo) + "," + std::to_string(hi) + "}";
}

// Column agreement for the expect pattern merge.
bool column_agrees(const std::vector<std::string>& samples, std::size_t pos, bool fromRight) {
    const auto at = [&](const std::string& s) {
        return static_cast<unsigned char>(fromRight ? s[s.size() - 1 - pos] : s[pos]);
    };
    const unsigned char first = at(samples[0]);
    const Cls cls = classify(first);
    for (const auto& s : samples) {
        const unsigned char c = at(s);
        if (classify(c) != cls) return false;
        if (cls == Cls::Other && c != first) return false;
    }
    return true;
}

std::string column_symbol(const std::vector<std::string>& samples, std::size_t pos, bool fromRight) {
    const unsigned char first = static_cast<unsigned char>(
        fromRight ? samples[0][samples[0].size() - 1 - pos] : samples[0][pos]);
    std::string out;
    switch (classify(first)) {
        case Cls::Digit: out = "x"; break;
        case Cls::Letter: out = "X"; break;
        default: append_expect_literal(out, static_cast<char>(first)); break;
    }
    return out;
}

}  // namespace

std::string pattern_of(const std::string& sample) {
    std::string out;
    for (const char c : sample) {
        switch (classify(static_cast<unsigned char>(c))) {
            case Cls::Digit: out += 'x'; break;
            case Cls::Letter: out += 'X'; break;
            default: append_expect_literal(out, c); break;
        }
    }
    return out;
}

std::string compact_regex(const std::string& sample) {
    std::string out;
    for (const Run& run : runs_of(sample)) {
        if (run.cls == Cls::Other) out += escape_literal(run.text);
        else out += std::string(run.cls == Cls::Digit ? "[0-9]" : "[A-Za-z]") + quantifier(run.text.size(), run.text.size());
    }
    return out;
}

std::string pattern_of_all(const std::vector<std::string>& samples) {
    if (samples.empty()) return {};
    std::size_t minLen = samples[0].size();
    bool sameLen = true;
    for (const auto& s : samples) {
        minLen = std::min(minLen, s.size());
        if (s.size() != samples[0].size()) sameLen = false;
    }
    std::string out;
    if (sameLen) {
        for (std::size_t i = 0; i < minLen; ++i) {
            out += column_agrees(samples, i, false) ? column_symbol(samples, i, false) : "?";
        }
        return out;
    }
    // Different lengths: shared head, then *, then shared tail.
    std::size_t head = 0;
    while (head < minLen && column_agrees(samples, head, false)) ++head;
    std::size_t tail = 0;
    while (tail < minLen - head && column_agrees(samples, tail, true)) ++tail;
    for (std::size_t i = 0; i < head; ++i) out += column_symbol(samples, i, false);
    out += '*';
    std::vector<std::string> pieces;
    for (std::size_t i = 0; i < tail; ++i) pieces.push_back(column_symbol(samples, i, true));
    for (auto it = pieces.rbegin(); it != pieces.rend(); ++it) out += *it;
    return out;
}

std::string infer_regex(const std::vector<std::string>& samples) {
    if (samples.empty()) return {};
    if (samples.size() == 1) return compact_regex(samples[0]);

    // Same shape (same run classes, same separators): one regex with counts that cover every sample.
    std::vector<std::vector<Run>> all;
    for (const auto& s : samples) all.push_back(runs_of(s));
    bool sameShape = true;
    for (const auto& r : all) {
        if (r.size() != all[0].size()) { sameShape = false; break; }
        for (std::size_t i = 0; i < r.size(); ++i) {
            if (r[i].cls != all[0][i].cls) sameShape = false;
            if (r[i].cls == Cls::Other && r[i].text != all[0][i].text) sameShape = false;
        }
        if (!sameShape) break;
    }
    if (sameShape) {
        std::string out;
        for (std::size_t i = 0; i < all[0].size(); ++i) {
            const Cls cls = all[0][i].cls;
            if (cls == Cls::Other) { out += escape_literal(all[0][i].text); continue; }
            std::size_t lo = all[0][i].text.size(), hi = lo;
            bool constant = true;
            for (const auto& r : all) {
                lo = std::min(lo, r[i].text.size());
                hi = std::max(hi, r[i].text.size());
                if (r[i].text != all[0][i].text) constant = false;
            }
            // A letter run that never changes is a keyword (ORD, QA): keep it literal.
            if (cls == Cls::Letter && constant) out += all[0][i].text;
            else out += std::string(cls == Cls::Digit ? "[0-9]" : "[A-Za-z]") + quantifier(lo, hi);
        }
        return out;
    }

    // Different shapes: shared head and tail around .*
    const std::string expect = pattern_of_all(samples);
    std::string out;
    for (std::size_t i = 0; i < expect.size();) {
        const char c = expect[i];
        if (c == '\\' && i + 1 < expect.size()) { out += escape_literal(std::string(1, expect[i + 1])); i += 2; continue; }
        if (c == 'x' || c == 'X') {
            std::size_t j = i;
            while (j < expect.size() && expect[j] == c) ++j;
            out += std::string(c == 'x' ? "[0-9]" : "[A-Za-z]") + quantifier(j - i, j - i);
            i = j;
            continue;
        }
        if (c == '?') out += '.';
        else if (c == '*') out += ".*";
        else out += escape_literal(std::string(1, c));
        ++i;
    }
    return out;
}

std::string auto_regex(const std::vector<std::string>& samples) {
    if (samples.empty()) return {};
    const std::string first = kind_of(samples[0]);
    bool same = true;
    for (const auto& s : samples) if (kind_of(s) != first) { same = false; break; }
    // word and slug are shapes more than formats, so those fall through to inference.
    if (same && first != "text" && first != "empty" && first != "word" && first != "slug" && first != "int") {
        if (const Kind* k = find_kind(first)) return k->regex;
    }
    return infer_regex(samples);
}

}  // namespace erelang::ai
