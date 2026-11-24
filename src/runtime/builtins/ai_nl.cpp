// builtin/ai: turn a plain English description into a regex with a deterministic phrase grammar.
//
//   "Match a corporate email address ending in .com"
//   "3 digits then a dash then 4 letters"
//   "starts with ORD- followed by 4 digits"
//   "an optional minus sign then one or more digits"
//
// Anything the grammar does not recognise makes generate() return an empty string, so a caller can
// tell "understood" from "guessed". The result matches the WHOLE text.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include "ai_internal.hpp"

namespace erelang::ai {

namespace {

struct Token {
    std::string text;
    bool quoted{false};
};

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == ',' || c == ';') { out.push_back({",", false}); ++i; continue; }
        if (c == '"' || c == '\'' || c == '`') {
            const std::size_t close = text.find(c, i + 1);
            if (close != std::string::npos) {
                out.push_back({text.substr(i + 1, close - i - 1), true});
                i = close + 1;
                continue;
            }
        }
        std::size_t j = i;
        while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) && text[j] != ',' && text[j] != ';') ++j;
        std::string word = text.substr(i, j - i);
        // Sentence punctuation after a word, but keep leading-dot literals like ".com".
        while (word.size() > 1 && (word.back() == '.' || word.back() == '!' || word.back() == '?' || word.back() == ':')) word.pop_back();
        out.push_back({word, false});
        i = j;
    }
    return out;
}

const std::vector<std::string>& fillers() {
    static const std::vector<std::string> f = {
        "match", "matches", "matching", "find", "a", "an", "the", "valid", "string", "strings", "text", "that", "is",
        "are", "which", "regex", "pattern", "for", "of", "please", "only", "just", "simple", "corporate", "company",
        "business", "work", "address", "any", "number", "in", "with", "by", "followed", "then", "and", "ending",
        "starting", "beginning", "containing", "contains", "contain", "include", "including", "ends", "starts",
        "begins", "begin", "start", "end", "or", "optional", "optionally", "maybe", "exactly", "least", "most", "up",
        "to", "between", "more", "fewer", "less", "no", "than", "many", "several", "some", "zero", "one", "case",
        "insensitive", "sensitive", "ignoring"};
    return f;
}

int number_word(const std::string& w) {
    static const char* names[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
                                  "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
                                  "seventeen", "eighteen", "nineteen", "twenty"};
    for (int i = 0; i <= 20; ++i) if (w == names[i]) return i;
    if (!w.empty() && std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c); }) && w.size() < 6) {
        return std::stoi(w);
    }
    return -1;
}

struct Phrase {
    std::vector<std::string> words;
    enum class Type { Class, Kind, Literal } type;
    std::string value;   // regex for Class, kind name for Kind, text for Literal
    bool pluralForm;
};

const std::vector<Phrase>& phrases() {
    static const std::vector<Phrase> table = [] {
        std::vector<Phrase> t;
        auto split = [](const std::string& s) {
            std::vector<std::string> w;
            std::size_t i = 0;
            while (i < s.size()) {
                const std::size_t j = s.find(' ', i);
                w.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
                if (j == std::string::npos) break;
                i = j + 1;
            }
            return w;
        };
        auto cls = [&](const std::string& singular, const std::string& pluralWord, const std::string& rx) {
            t.push_back({split(singular), Phrase::Type::Class, rx, false});
            t.push_back({split(pluralWord), Phrase::Type::Class, rx, true});
        };
        auto kind = [&](const std::string& words, const std::string& name) {
            t.push_back({split(words), Phrase::Type::Kind, name, false});
        };
        auto lit = [&](const std::string& words, const std::string& text) {
            t.push_back({split(words), Phrase::Type::Literal, text, false});
        };
        cls("lowercase letter", "lowercase letters", "[a-z]");
        cls("lowercase character", "lowercase characters", "[a-z]");
        cls("uppercase letter", "uppercase letters", "[A-Z]");
        cls("uppercase character", "uppercase characters", "[A-Z]");
        cls("hex digit", "hex digits", "[0-9a-fA-F]");
        cls("hexadecimal digit", "hexadecimal digits", "[0-9a-fA-F]");
        cls("alphanumeric character", "alphanumeric characters", "[A-Za-z0-9]");
        cls("alphanumeric", "alphanumerics", "[A-Za-z0-9]");
        cls("word character", "word characters", "[A-Za-z0-9_]");
        cls("digit", "digits", "[0-9]");
        cls("numeral", "numerals", "[0-9]");
        cls("letter", "letters", "[A-Za-z]");
        cls("alphabetic character", "alphabetic characters", "[A-Za-z]");
        cls("whitespace", "whitespace", "\\s");
        cls("space", "spaces", " ");
        cls("tab", "tabs", "\\t");
        cls("character", "characters", ".");
        cls("char", "chars", ".");
        cls("symbol", "symbols", "[^A-Za-z0-9\\s]");
        cls("special character", "special characters", "[^A-Za-z0-9\\s]");
        cls("punctuation mark", "punctuation marks", "[^A-Za-z0-9\\s]");
        cls("underscore", "underscores", "_");
        cls("dash", "dashes", "-");
        cls("hyphen", "hyphens", "-");
        cls("dot", "dots", "\\.");
        cls("period", "periods", "\\.");
        cls("comma", "commas", ",");
        cls("colon", "colons", ":");
        cls("semicolon", "semicolons", ";");
        cls("slash", "slashes", "/");
        cls("backslash", "backslashes", "\\\\");
        cls("plus sign", "plus signs", "\\+");
        cls("minus sign", "minus signs", "-");
        cls("percent sign", "percent signs", "%");
        cls("hash", "hashes", "#");
        cls("at sign", "at signs", "@");
        cls("at symbol", "at symbols", "@");
        kind("email address", "email");
        kind("email", "email");
        kind("e-mail", "email");
        kind("url", "url");
        kind("web address", "url");
        kind("link", "url");
        kind("ipv4 address", "ipv4");
        kind("ip address", "ipv4");
        kind("ipv4", "ipv4");
        kind("ipv6 address", "ipv6");
        kind("mac address", "mac");
        kind("uuid", "uuid");
        kind("guid", "uuid");
        kind("date and time", "datetime");
        kind("datetime", "datetime");
        kind("timestamp", "datetime");
        kind("date", "date");
        kind("time", "time");
        kind("hex color", "hexcolor");
        kind("hex colour", "hexcolor");
        kind("color code", "hexcolor");
        kind("hex number", "hex");
        kind("semantic version", "semver");
        kind("semver", "semver");
        kind("version number", "semver");
        kind("phone number", "phone");
        kind("phone", "phone");
        kind("zip code", "zip");
        kind("postal code", "zip");
        kind("zip", "zip");
        kind("credit card number", "card");
        kind("card number", "card");
        kind("slug", "slug");
        kind("boolean", "bool");
        kind("integer", "int");
        kind("whole number", "int");
        kind("decimal number", "float");
        kind("decimal", "float");
        kind("float", "float");
        kind("number", "int");
        kind("word", "word");
        lit("at", "@");
        std::stable_sort(t.begin(), t.end(), [](const Phrase& a, const Phrase& b) { return a.words.size() > b.words.size(); });
        return t;
    }();
    return table;
}

std::string wrap(const std::string& rx) {
    // Single atoms and already grouped fragments need no wrapper.
    if (rx.size() == 1) return rx;
    if (rx.size() == 2 && rx[0] == '\\') return rx;
    if (!rx.empty() && rx.front() == '[' && rx.find(']') == rx.size() - 1) return rx;
    return "(?:" + rx + ")";
}

std::string quantify(const std::string& atom, int lo, int hi) {
    if (lo == 1 && hi == 1) return atom;
    std::string q;
    if (lo == 0 && hi == -1) q = "*";
    else if (lo == 1 && hi == -1) q = "+";
    else if (lo == 0 && hi == 1) q = "?";
    else if (hi == -1) q = "{" + std::to_string(lo) + ",}";
    else if (lo == hi) q = "{" + std::to_string(lo) + "}";
    else q = "{" + std::to_string(lo) + "," + std::to_string(hi) + "}";
    return wrap(atom) + q;
}

std::string literal_text(const std::string& text, bool ci) {
    std::string out;
    for (const char c : text) {
        if (ci && std::isalpha(static_cast<unsigned char>(c))) {
            out += '[';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            out += ']';
        } else {
            out += escape_literal(std::string(1, c));
        }
    }
    return out;
}

bool is_filler(const std::string& w) {
    const auto& f = fillers();
    return std::find(f.begin(), f.end(), w) != f.end();
}

// Bucket the next elements land in.
enum class Bucket { Main, Head, Contains, Tail };

struct Builder {
    std::vector<std::vector<std::string>> alternatives;  // finished alternatives (joined by "or")
    std::vector<std::string> head, main, contains, tail;
    bool ci{false};
    bool failed{false};
    bool nextOptional{false};
    Bucket bucket{Bucket::Main};
    std::string mainKind;  // set when main is exactly one known kind (enables the "open" form)

    std::vector<std::string>& current() {
        switch (bucket) {
            case Bucket::Head: return head;
            case Bucket::Contains: return contains;
            case Bucket::Tail: return tail;
            default: return main;
        }
    }
    void push(std::string rx, const std::string& kindName = {}) {
        if (nextOptional) { rx = wrap(rx) + "?"; nextOptional = false; }
        auto& dest = current();
        if (bucket == Bucket::Main && dest.empty() && !kindName.empty()) mainKind = kindName; else if (bucket == Bucket::Main) mainKind.clear();
        dest.push_back(std::move(rx));
    }
};

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& p : v) s += p;
    return s;
}

std::string assemble(const Builder& b) {
    const std::string H = join(b.head), M = join(b.main), C = join(b.contains), T = join(b.tail);
    if (H.empty() && M.empty() && C.empty() && T.empty()) return {};
    std::string out = H + M;
    if (!C.empty()) out += (out.empty() ? ".*" : ".*") + C + ".*";
    if (!T.empty()) {
        const Kind* k = b.mainKind.empty() ? nullptr : find_kind(b.mainKind);
        if (k && k->open && H.empty() && C.empty()) {
            out = std::string(k->open) + T;
        } else if (!C.empty()) {
            out += T;  // the contains clause already left a trailing .*, which can absorb the tail
            out = H + M + ".*" + C + ".*" + T;
        } else {
            out += ".*" + T;
        }
    } else if (!H.empty() && M.empty() && C.empty()) {
        out += ".*";
    }
    return out;
}

}  // namespace

std::string generate(const std::string& description) {
    std::vector<Token> toks = tokenize(description);
    Builder b;
    // Case flag first so every literal sees it.
    for (std::size_t i = 0; i < toks.size(); ++i) {
        const std::string w = lower(toks[i].text);
        if (!toks[i].quoted && (w == "case-insensitive" || w == "case-insensitively" ||
            (w == "case" && i + 1 < toks.size() && lower(toks[i + 1].text) == "insensitive") ||
            (w == "ignoring" && i + 1 < toks.size() && lower(toks[i + 1].text) == "case"))) {
            b.ci = true;
        }
    }

    for (std::size_t i = 0; i < toks.size() && !b.failed;) {
        const Token& tok = toks[i];
        const std::string w = tok.quoted ? tok.text : lower(tok.text);
        if (tok.quoted) { b.push(literal_text(tok.text, b.ci)); ++i; continue; }
        if (w == ",") { ++i; continue; }
        // "the word \"error\"": the noun only introduces a quoted literal.
        if ((w == "word" || w == "text" || w == "phrase" || w == "string" || w == "substring") &&
            i + 1 < toks.size() && toks[i + 1].quoted) { ++i; continue; }

        // Anchors and modes.
        if ((w == "starts" || w == "start" || w == "starting" || w == "begins" || w == "begin" || w == "beginning") &&
            i + 1 < toks.size() && lower(toks[i + 1].text) == "with") {
            b.bucket = Bucket::Head; i += 2; continue;
        }
        if ((w == "ends" || w == "end" || w == "ending") && i + 1 < toks.size() &&
            (lower(toks[i + 1].text) == "with" || lower(toks[i + 1].text) == "in" || lower(toks[i + 1].text) == "on")) {
            b.bucket = Bucket::Tail; i += 2; continue;
        }
        if (w == "contains" || w == "contain" || w == "containing" || w == "including" || w == "include") {
            b.bucket = Bucket::Contains; ++i; continue;
        }
        if (w == "optional" || w == "optionally" || w == "maybe") { b.nextOptional = true; ++i; continue; }
        if (w == "or") {
            if (b.main.empty() && b.head.empty() && b.contains.empty() && b.tail.empty()) { b.failed = true; break; }
            b.alternatives.push_back({assemble(b)});
            b.head.clear(); b.main.clear(); b.contains.clear(); b.tail.clear();
            b.mainKind.clear();
            b.bucket = Bucket::Main;
            ++i; continue;
        }
        if (w == "case" || w == "insensitive" || w == "case-insensitive" || w == "case-insensitively" ||
            w == "ignoring" || w == "sensitive") {
            // flag handled above; "case insensitive" and "ignoring case" are two words
            ++i; continue;
        }

        // Counts: "3", "three", "exactly 3", "at least 2", "2 to 4", "between 2 and 4", "one or more".
        int lo = 1, hi = 1;
        bool counted = false;
        std::size_t j = i;
        auto word_at = [&](std::size_t k) { return k < toks.size() && !toks[k].quoted ? lower(toks[k].text) : std::string(); };
        if (w == "exactly" && number_word(word_at(j + 1)) >= 0) { lo = hi = number_word(word_at(j + 1)); j += 2; counted = true; }
        else if (w == "at" && word_at(j + 1) == "least" && number_word(word_at(j + 2)) >= 0) { lo = number_word(word_at(j + 2)); hi = -1; j += 3; counted = true; }
        else if ((w == "at" && word_at(j + 1) == "most" && number_word(word_at(j + 2)) >= 0) ) { lo = 0; hi = number_word(word_at(j + 2)); j += 3; counted = true; }
        else if (w == "up" && word_at(j + 1) == "to" && number_word(word_at(j + 2)) >= 0) { lo = 0; hi = number_word(word_at(j + 2)); j += 3; counted = true; }
        else if (w == "no" && word_at(j + 1) == "more" && word_at(j + 2) == "than" && number_word(word_at(j + 3)) >= 0) { lo = 0; hi = number_word(word_at(j + 3)); j += 4; counted = true; }
        else if (w == "between" && number_word(word_at(j + 1)) >= 0 && word_at(j + 2) == "and" && number_word(word_at(j + 3)) >= 0) { lo = number_word(word_at(j + 1)); hi = number_word(word_at(j + 3)); j += 4; counted = true; }
        else if (w == "one" && word_at(j + 1) == "or" && word_at(j + 2) == "more") { lo = 1; hi = -1; j += 3; counted = true; }
        else if (w == "zero" && word_at(j + 1) == "or" && word_at(j + 2) == "more") { lo = 0; hi = -1; j += 3; counted = true; }
        else if (w == "any" && word_at(j + 1) == "number" && word_at(j + 2) == "of") { lo = 0; hi = -1; j += 3; counted = true; }
        else if ((w == "many" || w == "several" || w == "some") ) { lo = 1; hi = -1; j += 1; counted = true; }
        else if (number_word(w) >= 0 && !(w == "one" && j + 1 < toks.size() && number_word(word_at(j + 1)) >= 0)) {
            lo = hi = number_word(w);
            ++j;
            counted = true;
            if (word_at(j) == "to" && number_word(word_at(j + 1)) >= 0) { hi = number_word(word_at(j + 1)); j += 2; }
            else if (word_at(j) == "or" && word_at(j + 1) == "more") { hi = -1; j += 2; }
            else if (word_at(j) == "or" && (word_at(j + 1) == "fewer" || word_at(j + 1) == "less")) { hi = lo; lo = 0; j += 2; }
            else if (word_at(j) == "-" && number_word(word_at(j + 1)) >= 0) { hi = number_word(word_at(j + 1)); j += 2; }
        }
        // Skip articles and "of" between a count and its noun.
        std::size_t k = j;
        while (k < toks.size() && !toks[k].quoted && (lower(toks[k].text) == "of" || lower(toks[k].text) == "a" || lower(toks[k].text) == "an" || lower(toks[k].text) == "the")) ++k;

        // Noun phrase (class, kind, literal word) at k.
        const Phrase* hit = nullptr;
        for (const auto& p : phrases()) {
            if (k + p.words.size() > toks.size()) continue;
            bool same = true;
            for (std::size_t m = 0; m < p.words.size(); ++m) {
                if (toks[k + m].quoted || lower(toks[k + m].text) != p.words[m]) { same = false; break; }
            }
            if (same) { hit = &p; break; }
        }
        if (hit && !counted && hit->type == Phrase::Type::Kind && hit->value == "word" &&
            k + 1 < toks.size() && toks[k + 1].quoted) {
            i = k + 1;  // "the word \"error\"": the quoted text is what matters
            continue;
        }
        if (hit) {
            if (hit->type == Phrase::Type::Kind) {
                const Kind* kd = find_kind(hit->value);
                if (!kd) { b.failed = true; break; }
                std::string rx = std::string(kd->regex);
                b.push(counted && !(lo == 1 && hi == 1) ? quantify(rx, lo, hi) : wrap(rx), counted ? std::string() : hit->value);
            } else if (hit->type == Phrase::Type::Class) {
                int clo = lo, chi = hi;
                if (!counted) { if (hit->pluralForm) { clo = 1; chi = -1; } }
                b.push(quantify(hit->value, clo, chi));
            } else {
                b.push(literal_text(hit->value, b.ci));
            }
            i = k + hit->words.size();
            continue;
        }
        if (counted) {
            // A plain number with nothing to count is literal text ("starts with 42").
            const bool plainNumber = j == i + 1 && std::all_of(tok.text.begin(), tok.text.end(), [](unsigned char c) { return std::isdigit(c); });
            if (!plainNumber) { b.failed = true; break; }
            b.push(literal_text(tok.text, b.ci));
            ++i;
            continue;
        }

        // Literal-looking token: ".com", "ORD-", "@", "v1"
        const bool wordLike = !tok.text.empty() && std::all_of(tok.text.begin(), tok.text.end(), [](unsigned char c) { return std::isalpha(c); });
        if (!tok.text.empty() && (!wordLike || (!is_filler(w) && b.bucket != Bucket::Main && tok.text.size() <= 24))) {
            if (!wordLike) {
                b.push(literal_text(tok.text, b.ci));
                ++i;
                continue;
            }
        }
        if (is_filler(w)) { ++i; continue; }
        // A bare word after starts with / ends with / contains is literal text ("starts with Hello").
        if (wordLike && b.bucket != Bucket::Main) { b.push(literal_text(tok.text, b.ci)); ++i; continue; }
        b.failed = true;
    }
    if (b.failed) return {};

    std::string result = assemble(b);
    if (!b.alternatives.empty()) {
        std::vector<std::string> alts;
        for (const auto& a : b.alternatives) alts.push_back(a[0]);
        if (!result.empty()) alts.push_back(result);
        result.clear();
        for (std::size_t n = 0; n < alts.size(); ++n) { if (n) result += '|'; result += alts[n]; }
        result = "(?:" + result + ")";
    }
    if (result.empty() || !regex_valid(result)) return {};
    return optimize(result);
}

}  // namespace erelang::ai
