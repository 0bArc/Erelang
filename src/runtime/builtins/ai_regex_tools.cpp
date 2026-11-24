// builtin/ai: understand a regex. Parses a practical subset of ECMAScript regex into a small tree,
// then explains it in English, simplifies it, or generates sample strings from it.
//
// Supported: literals, escapes (\d \w \s \D \W \S and escaped punctuation), ., [classes], (groups),
// (?:non-capturing), alternation, * + ? {n} {n,} {n,m} (and lazy forms), ^ and $.
// Anything else (backreferences, lookaround, \b) makes the pattern "not understood": explain says so,
// optimize returns the input unchanged, examples returns nothing.
#include <algorithm>
#include <bitset>
#include <cctype>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include <vector>
#include "ai_internal.hpp"

namespace erelang::ai {

namespace {

using CharSet = std::bitset<256>;

struct Node;
using NodeList = std::vector<Node>;

struct Node {
    enum class Type { Lit, Any, Class, Group, Repeat, Start, End } type{Type::Lit};
    unsigned char ch{0};                 // Lit
    CharSet set;                         // Class members (before negation)
    bool negated{false};                 // Class
    std::vector<NodeList> alts;          // Group
    bool capture{false};                 // Group
    std::shared_ptr<Node> child;         // Repeat
    int min{0};                          // Repeat
    int max{-1};                         // Repeat, -1 = unbounded
    bool lazy{false};                    // Repeat
};

CharSet range_set(int lo, int hi) {
    CharSet s;
    for (int c = lo; c <= hi; ++c) s.set(static_cast<std::size_t>(c));
    return s;
}
const CharSet& digit_set() { static const CharSet s = range_set('0', '9'); return s; }
const CharSet& word_set() {
    static const CharSet s = range_set('0', '9') | range_set('a', 'z') | range_set('A', 'Z') | range_set('_', '_');
    return s;
}
const CharSet& space_set() {
    static const CharSet s = [] { CharSet x; for (char c : {' ', '\t', '\n', '\r', '\f', '\v'}) x.set(static_cast<unsigned char>(c)); return x; }();
    return s;
}

// ---- parser --------------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool parse(std::vector<NodeList>& alts) {
        alts = parse_alts();
        return ok_ && pos_ == s_.size();
    }

private:
    const std::string& s_;
    std::size_t pos_{0};
    bool ok_{true};

    bool more() const { return pos_ < s_.size(); }
    char peek() const { return s_[pos_]; }

    std::vector<NodeList> parse_alts() {
        std::vector<NodeList> alts;
        alts.push_back(parse_seq());
        while (ok_ && more() && peek() == '|') {
            ++pos_;
            alts.push_back(parse_seq());
        }
        return alts;
    }

    NodeList parse_seq() {
        NodeList seq;
        while (ok_ && more() && peek() != '|' && peek() != ')') {
            Node atom = parse_atom();
            if (!ok_) break;
            apply_quantifier(atom);
            seq.push_back(std::move(atom));
        }
        return seq;
    }

    bool parse_int(int& out) {
        if (!more() || !std::isdigit(static_cast<unsigned char>(peek()))) return false;
        long v = 0;
        while (more() && std::isdigit(static_cast<unsigned char>(peek()))) {
            v = v * 10 + (peek() - '0');
            if (v > 100000) { ok_ = false; return false; }
            ++pos_;
        }
        out = static_cast<int>(v);
        return true;
    }

    void apply_quantifier(Node& atom) {
        if (!more()) return;
        int lo = 0, hi = -1;
        const char c = peek();
        if (c == '*') { lo = 0; hi = -1; ++pos_; }
        else if (c == '+') { lo = 1; hi = -1; ++pos_; }
        else if (c == '?') { lo = 0; hi = 1; ++pos_; }
        else if (c == '{') {
            const std::size_t save = pos_;
            ++pos_;
            int a = 0;
            if (!parse_int(a)) { pos_ = save; return; }  // a literal brace
            lo = hi = a;
            if (more() && peek() == ',') {
                ++pos_;
                int b = 0;
                if (parse_int(b)) hi = b; else hi = -1;
            }
            if (!more() || peek() != '}') { ok_ = false; return; }
            ++pos_;
            if (hi != -1 && hi < lo) { ok_ = false; return; }
        } else return;
        if (atom.type == Node::Type::Start || atom.type == Node::Type::End || atom.type == Node::Type::Repeat) {
            ok_ = false;
            return;
        }
        Node rep;
        rep.type = Node::Type::Repeat;
        rep.child = std::make_shared<Node>(std::move(atom));
        rep.min = lo;
        rep.max = hi;
        if (more() && peek() == '?') { rep.lazy = true; ++pos_; }
        atom = std::move(rep);
    }

    // Parses the character after a backslash. Returns true when it produced a set (\d etc).
    bool parse_escape(CharSet& set, unsigned char& lit, bool& negated) {
        negated = false;
        if (!more()) { ok_ = false; return false; }
        const char c = s_[pos_++];
        switch (c) {
            case 'd': set = digit_set(); return true;
            case 'D': set = digit_set(); negated = true; return true;
            case 'w': set = word_set(); return true;
            case 'W': set = word_set(); negated = true; return true;
            case 's': set = space_set(); return true;
            case 'S': set = space_set(); negated = true; return true;
            case 'n': lit = '\n'; return false;
            case 't': lit = '\t'; return false;
            case 'r': lit = '\r'; return false;
            case 'f': lit = '\f'; return false;
            case 'v': lit = '\v'; return false;
            default:
                if (std::isalnum(static_cast<unsigned char>(c))) { ok_ = false; return false; }  // \b \1 \x ...
                lit = static_cast<unsigned char>(c);
                return false;
        }
    }

    Node parse_class() {
        Node n;
        n.type = Node::Type::Class;
        if (more() && peek() == '^') { n.negated = true; ++pos_; }
        bool first = true;
        while (more() && (peek() != ']' || first)) {
            first = false;
            unsigned char lo = 0;
            CharSet esc;
            bool escNeg = false;
            if (peek() == '[') { ok_ = false; return n; }  // POSIX classes
            if (peek() == '\\') {
                ++pos_;
                if (parse_escape(esc, lo, escNeg)) {
                    n.set |= escNeg ? ~esc : esc;
                    continue;
                }
                if (!ok_) return n;
            } else {
                lo = static_cast<unsigned char>(s_[pos_++]);
            }
            unsigned char hi = lo;
            if (more() && peek() == '-' && pos_ + 1 < s_.size() && s_[pos_ + 1] != ']') {
                ++pos_;
                if (peek() == '\\') {
                    ++pos_;
                    CharSet e2;
                    bool n2 = false;
                    if (parse_escape(e2, hi, n2)) { ok_ = false; return n; }
                } else {
                    hi = static_cast<unsigned char>(s_[pos_++]);
                }
                if (hi < lo) { ok_ = false; return n; }
            }
            for (int c = lo; c <= hi; ++c) n.set.set(static_cast<std::size_t>(c));
        }
        if (!more()) { ok_ = false; return n; }
        ++pos_;  // ]
        return n;
    }

    Node parse_atom() {
        Node n;
        const char c = s_[pos_++];
        switch (c) {
            case '.': n.type = Node::Type::Any; return n;
            case '^': n.type = Node::Type::Start; return n;
            case '$': n.type = Node::Type::End; return n;
            case '[': return parse_class();
            case '(': {
                n.type = Node::Type::Group;
                n.capture = true;
                if (more() && peek() == '?') {
                    if (pos_ + 1 < s_.size() && s_[pos_ + 1] == ':') { pos_ += 2; n.capture = false; }
                    else { ok_ = false; return n; }  // lookaround, named groups
                }
                n.alts = parse_alts();
                if (!ok_ || !more() || peek() != ')') { ok_ = false; return n; }
                ++pos_;
                return n;
            }
            case '\\': {
                CharSet set;
                unsigned char lit = 0;
                bool neg = false;
                if (parse_escape(set, lit, neg)) {
                    n.type = Node::Type::Class;
                    n.set = set;
                    n.negated = neg;
                } else {
                    n.type = Node::Type::Lit;
                    n.ch = lit;
                }
                return n;
            }
            case '*': case '+': case '?': ok_ = false; return n;
            default:
                n.type = Node::Type::Lit;
                n.ch = static_cast<unsigned char>(c);
                return n;
        }
    }
};

bool parse_regex(const std::string& rx, std::vector<NodeList>& alts) {
    try {
        Parser p(rx);
        return p.parse(alts);
    } catch (...) { return false; }
}

// ---- rendering -----------------------------------------------------------------------------

std::string esc_char(unsigned char c, bool inClass) {
    std::string out;
    if (c == '\n') return "\\n";
    if (c == '\t') return "\\t";
    if (c == '\r') return "\\r";
    if (inClass) {
        if (c == '\\' || c == ']' || c == '^' || c == '-' || c == '[') out += '\\';
    } else {
        static const std::string special = "\\^$.|?*+()[]{}";
        if (special.find(static_cast<char>(c)) != std::string::npos) out += '\\';
    }
    out += static_cast<char>(c);
    return out;
}

CharSet effective_set(const Node& n) { return n.negated ? ~n.set : n.set; }

std::string render_class(const Node& n, bool shortForms) {
    const CharSet eff = effective_set(n);
    if (shortForms) {
        if (eff == digit_set()) return "\\d";
        if (eff == word_set()) return "\\w";
        if (eff == space_set()) return "\\s";
        if (eff == ~digit_set()) return "\\D";
        if (eff == ~word_set()) return "\\W";
        if (eff == ~space_set()) return "\\S";
    }
    if (!n.negated && n.set.count() == 1) {
        for (int c = 0; c < 256; ++c) if (n.set.test(static_cast<std::size_t>(c))) return esc_char(static_cast<unsigned char>(c), false);
    }
    std::string out = "[";
    if (n.negated) out += '^';
    int c = 0;
    while (c < 256) {
        if (!n.set.test(static_cast<std::size_t>(c))) { ++c; continue; }
        int e = c;
        while (e + 1 < 256 && n.set.test(static_cast<std::size_t>(e + 1))) ++e;
        const auto one = [&](int v) { return esc_char(static_cast<unsigned char>(v), true); };
        if (e - c >= 2) out += one(c) + "-" + one(e);
        else for (int v = c; v <= e; ++v) out += one(v);
        c = e + 1;
    }
    return out + "]";
}

std::string quant_string(int lo, int hi, bool lazy) {
    std::string q;
    if (lo == 0 && hi == -1) q = "*";
    else if (lo == 1 && hi == -1) q = "+";
    else if (lo == 0 && hi == 1) q = "?";
    else if (hi == -1) q = "{" + std::to_string(lo) + ",}";
    else if (lo == hi) q = "{" + std::to_string(lo) + "}";
    else q = "{" + std::to_string(lo) + "," + std::to_string(hi) + "}";
    if (lazy) q += '?';
    return q;
}

std::string render_list(const NodeList& seq, bool shortForms);

std::string render_node(const Node& n, bool shortForms) {
    switch (n.type) {
        case Node::Type::Lit: return esc_char(n.ch, false);
        case Node::Type::Any: return ".";
        case Node::Type::Class: return render_class(n, shortForms);
        case Node::Type::Start: return "^";
        case Node::Type::End: return "$";
        case Node::Type::Group: {
            std::string out = n.capture ? "(" : "(?:";
            for (std::size_t i = 0; i < n.alts.size(); ++i) {
                if (i) out += '|';
                out += render_list(n.alts[i], shortForms);
            }
            return out + ")";
        }
        case Node::Type::Repeat: {
            // Short fixed runs of one literal read better spelled out: aaa, not a{3}.
            if (n.child->type == Node::Type::Lit && n.min == n.max && n.min >= 1 && n.min <= 4 && !n.lazy) {
                std::string out;
                for (int i = 0; i < n.min; ++i) out += esc_char(n.child->ch, false);
                return out;
            }
            return render_node(*n.child, shortForms) + quant_string(n.min, n.max, n.lazy);
        }
    }
    return {};
}

std::string render_list(const NodeList& seq, bool shortForms) {
    std::string out;
    for (const auto& n : seq) out += render_node(n, shortForms);
    return out;
}

// ---- compile cache -------------------------------------------------------------------------

const std::regex* compile_cached(const std::string& rx) {
    static std::mutex m;
    static std::vector<std::pair<std::string, std::shared_ptr<std::regex>>> cache;
    std::lock_guard<std::mutex> lock(m);
    for (const auto& [k, v] : cache) if (k == rx) return v.get();
    std::shared_ptr<std::regex> re;
    try { re = std::make_shared<std::regex>(rx); } catch (...) { re = nullptr; }
    if (cache.size() >= 128) cache.erase(cache.begin());
    cache.emplace_back(rx, re);
    return re.get();
}

// ---- explain -------------------------------------------------------------------------------

std::string plural(const std::string& noun, int n) {
    if (n == 1) return noun;
    if (noun == "any character") return "any characters";
    if (noun.back() == 's') return noun;
    return noun + "s";
}

std::string describe_set(const CharSet& eff, bool& single) {
    single = true;
    if (eff == digit_set()) return "digit";
    if (eff == range_set('a', 'z') || eff == range_set('A', 'Z') || eff == (range_set('a', 'z') | range_set('A', 'Z'))) {
        if (eff == range_set('a', 'z')) return "lowercase letter";
        if (eff == range_set('A', 'Z')) return "uppercase letter";
        return "letter";
    }
    if (eff == (range_set('a', 'z') | range_set('A', 'Z') | digit_set())) return "letter or digit";
    if (eff == word_set()) return "letter, digit or underscore";
    if (eff == space_set()) return "whitespace character";
    if (eff == (digit_set() | range_set('a', 'f') | range_set('A', 'F'))) return "hex digit";
    single = false;
    return {};
}

std::string describe_atom(const Node& n, int count);

std::string describe_list(const NodeList& seq) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i < seq.size()) {
        if (seq[i].type == Node::Type::Lit) {
            std::string text;
            while (i < seq.size() && seq[i].type == Node::Type::Lit) text += static_cast<char>(seq[i++].ch);
            parts.push_back("the text \"" + text + "\"");
            continue;
        }
        parts.push_back(describe_atom(seq[i], 1));
        ++i;
    }
    std::string out;
    for (std::size_t k = 0; k < parts.size(); ++k) {
        if (k) out += k + 1 == parts.size() ? ", then " : ", then ";
        out += parts[k];
    }
    return out.empty() ? "nothing" : out;
}

// Singular noun for a character class: "digit", "letter", or "character from [abc]".
std::string class_noun(const Node& n) {
    bool single = false;
    const std::string name = describe_set(effective_set(n), single);
    if (single) return name;
    Node positive = n;
    positive.negated = false;
    return n.negated ? "character other than " + render_class(positive, false)
                     : "character from " + render_class(n, false);
}

std::string pluralize_noun(const std::string& noun) {
    // "character from [abc]" -> "characters from [abc]"
    const std::size_t space = noun.find(' ');
    if (noun.rfind("character ", 0) == 0) return "characters" + noun.substr(space);
    return plural(noun, 2);
}

std::string describe_class(const Node& n, int count) {
    const std::string noun = class_noun(n);
    if (count == 1) return "a " + noun;
    return pluralize_noun(noun);
}

std::string describe_atom(const Node& n, int count) {
    switch (n.type) {
        case Node::Type::Lit: return "the character \"" + std::string(1, static_cast<char>(n.ch)) + "\"";
        case Node::Type::Any: return count == 1 ? "any character" : "any characters";
        case Node::Type::Class: return describe_class(n, count);
        case Node::Type::Start: return "the start of the text";
        case Node::Type::End: return "the end of the text";
        case Node::Type::Group: {
            std::string out = n.capture ? "a captured group of " : "a group of ";
            for (std::size_t i = 0; i < n.alts.size(); ++i) {
                if (i) out += " or ";
                out += describe_list(n.alts[i]);
            }
            return out;
        }
        case Node::Type::Repeat: {
            const Node& c = *n.child;
            const int lo = n.min, hi = n.max;
            const bool simple = c.type == Node::Type::Class || c.type == Node::Type::Any;
            if (!simple) {
                std::string inner = describe_atom(c, 1);
                if (lo == 0 && hi == 1) return "optionally " + inner;
                if (lo == 0 && hi == -1) return "zero or more times, " + inner;
                if (lo == 1 && hi == -1) return "one or more times, " + inner;
                if (hi == -1) return "at least " + std::to_string(lo) + " times, " + inner;
                if (lo == hi) return std::to_string(lo) + " times, " + inner;
                return std::to_string(lo) + " to " + std::to_string(hi) + " times, " + inner;
            }
            const std::string noun = c.type == Node::Type::Any ? "characters" : pluralize_noun(class_noun(c));
            if (lo == 0 && hi == 1) return "an optional " + (c.type == Node::Type::Any ? std::string("character") : class_noun(c));
            if (lo == 0 && hi == -1) return "zero or more " + noun;
            if (lo == 1 && hi == -1) return "one or more " + noun;
            if (hi == -1) return "at least " + std::to_string(lo) + " " + noun;
            if (lo == hi) return "exactly " + std::to_string(lo) + " " + noun;
            return std::to_string(lo) + " to " + std::to_string(hi) + " " + noun;
        }
    }
    return {};
}

// ---- optimize ------------------------------------------------------------------------------

bool single_char_node(const Node& n, CharSet& set) {
    if (n.type == Node::Type::Lit) { set.reset(); set.set(n.ch); return true; }
    if (n.type == Node::Type::Class) { set = effective_set(n); return true; }
    return false;
}

void simplify_list(NodeList& seq);

void simplify_node(Node& n) {
    if (n.type == Node::Type::Group) {
        for (auto& alt : n.alts) simplify_list(alt);
        // Drop duplicate alternatives.
        std::vector<NodeList> unique;
        std::vector<std::string> seen;
        for (auto& alt : n.alts) {
            const std::string key = render_list(alt, true);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            unique.push_back(std::move(alt));
        }
        n.alts = std::move(unique);
        // (?:a|b|[0-9]) of single characters becomes one class.
        if (!n.capture && n.alts.size() > 1) {
            CharSet merged;
            bool all = true;
            for (const auto& alt : n.alts) {
                CharSet s;
                if (alt.size() != 1 || !single_char_node(alt[0], s)) { all = false; break; }
                merged |= s;
            }
            if (all) {
                Node cls;
                cls.type = Node::Type::Class;
                cls.set = merged;
                n = std::move(cls);
            }
        }
    } else if (n.type == Node::Type::Repeat) {
        simplify_node(*n.child);
        // (?:x){n} with a single plain atom inside needs no group.
        Node& c = *n.child;
        if (c.type == Node::Type::Group && !c.capture && c.alts.size() == 1 && c.alts[0].size() == 1 &&
            c.alts[0][0].type != Node::Type::Repeat && c.alts[0][0].type != Node::Type::Start &&
            c.alts[0][0].type != Node::Type::End) {
            Node inner = std::move(c.alts[0][0]);
            *n.child = std::move(inner);
        }
    }
}

std::string atom_key(const Node& n) { return render_node(n, true); }

void simplify_list(NodeList& seq) {
    for (auto& n : seq) simplify_node(n);
    // Splice non-capturing single alternative groups that carry no quantifier.
    NodeList spliced;
    for (auto& n : seq) {
        if (n.type == Node::Type::Group && !n.capture && n.alts.size() == 1) {
            for (auto& inner : n.alts[0]) spliced.push_back(std::move(inner));
        } else {
            spliced.push_back(std::move(n));
        }
    }
    // Merge neighbours with the same atom: [0-9][0-9]{2} -> [0-9]{3}.
    NodeList merged;
    for (auto& n : spliced) {
        const bool rep = n.type == Node::Type::Repeat;
        const bool mergeable = (rep && !n.lazy) || (n.type != Node::Type::Repeat && n.type != Node::Type::Start &&
                                                    n.type != Node::Type::End && n.type != Node::Type::Group);
        if (!mergeable || merged.empty()) { merged.push_back(std::move(n)); continue; }
        Node& prev = merged.back();
        const bool prevRep = prev.type == Node::Type::Repeat;
        const bool prevMergeable = (prevRep && !prev.lazy) || (prev.type != Node::Type::Repeat && prev.type != Node::Type::Start &&
                                                              prev.type != Node::Type::End && prev.type != Node::Type::Group);
        const Node& a = prevRep ? *prev.child : prev;
        const Node& b = rep ? *n.child : n;
        if (!prevMergeable || atom_key(a) != atom_key(b)) { merged.push_back(std::move(n)); continue; }
        const int lo = (prevRep ? prev.min : 1) + (rep ? n.min : 1);
        const int hi = ((prevRep ? prev.max : 1) == -1 || (rep ? n.max : 1) == -1) ? -1
                       : (prevRep ? prev.max : 1) + (rep ? n.max : 1);
        Node out;
        out.type = Node::Type::Repeat;
        out.child = std::make_shared<Node>(a);
        out.min = lo;
        out.max = hi;
        prev = std::move(out);
    }
    // Normalize bounds: x{1} -> x.
    for (auto& n : merged) {
        if (n.type == Node::Type::Repeat && n.min == 1 && n.max == 1) {
            Node inner = *n.child;
            n = std::move(inner);
        }
    }
    seq = std::move(merged);
}

// ---- examples ------------------------------------------------------------------------------

struct Rng {
    unsigned long long state;
    unsigned next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<unsigned>(state >> 33);
    }
    unsigned below(unsigned n) { return n ? next() % n : 0; }
};

char pick(const CharSet& set, Rng& rng) {
    std::vector<unsigned char> good, any;
    for (int c = 0; c < 256; ++c) {
        if (!set.test(static_cast<std::size_t>(c))) continue;
        any.push_back(static_cast<unsigned char>(c));
        if (std::isalnum(c) || c == '_') good.push_back(static_cast<unsigned char>(c));
    }
    std::vector<unsigned char> printable;
    for (unsigned char c : any) if (c >= 33 && c < 127) printable.push_back(c);
    const auto& pool = !good.empty() ? good : (!printable.empty() ? printable : any);
    return pool.empty() ? '?' : static_cast<char>(pool[rng.below(static_cast<unsigned>(pool.size()))]);
}

void generate_list(const NodeList& seq, std::string& out, Rng& rng);

void generate_node(const Node& n, std::string& out, Rng& rng) {
    switch (n.type) {
        case Node::Type::Lit: out += static_cast<char>(n.ch); break;
        case Node::Type::Any: { CharSet s = range_set('a', 'z') | range_set('0', '9'); out += pick(s, rng); break; }
        case Node::Type::Class: out += pick(effective_set(n), rng); break;
        case Node::Type::Start: case Node::Type::End: break;
        case Node::Type::Group: generate_list(n.alts[rng.below(static_cast<unsigned>(n.alts.size()))], out, rng); break;
        case Node::Type::Repeat: {
            const int hi = n.max == -1 ? n.min + 3 : std::min(n.max, n.min + 3);
            const int count = n.min + static_cast<int>(rng.below(static_cast<unsigned>(hi - n.min + 1)));
            for (int i = 0; i < count; ++i) generate_node(*n.child, out, rng);
            break;
        }
    }
}

void generate_list(const NodeList& seq, std::string& out, Rng& rng) {
    for (const auto& n : seq) generate_node(n, out, rng);
}

}  // namespace

// ---- public --------------------------------------------------------------------------------

std::string escape_literal(std::string_view text) {
    std::string out;
    for (const char c : text) out += esc_char(static_cast<unsigned char>(c), false);
    return out;
}

bool regex_valid(const std::string& rx) { return compile_cached(rx) != nullptr; }

bool full_match(const std::string& rx, const std::string& text) {
    const std::regex* re = compile_cached(rx);
    if (!re) return false;
    try { return std::regex_match(text, *re); } catch (...) { return false; }
}

std::string explain(const std::string& rx) {
    if (rx.empty()) return "An empty pattern: it matches only empty text.";
    std::vector<NodeList> alts;
    if (!parse_regex(rx, alts)) {
        return "This pattern uses syntax the explainer does not cover (lookaround, backreferences or word boundaries): " + rx;
    }
    // Merge neighbours first so [0-9][0-9] reads as "exactly 2 digits".
    for (auto& alt : alts) simplify_list(alt);
    std::string out = "Matches ";
    for (std::size_t i = 0; i < alts.size(); ++i) {
        if (i) out += "; or ";
        out += describe_list(alts[i]);
    }
    return out + ".";
}

std::vector<std::string> examples(const std::string& rx, int count) {
    std::vector<std::string> out;
    std::vector<NodeList> alts;
    if (count <= 0 || !parse_regex(rx, alts)) return out;
    Rng rng{0x9E3779B97F4A7C15ULL};
    for (int attempt = 0; attempt < count * 8 && static_cast<int>(out.size()) < count; ++attempt) {
        std::string s;
        generate_list(alts[rng.below(static_cast<unsigned>(alts.size()))], s, rng);
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
    }
    return out;
}

std::string optimize(const std::string& rx) {
    std::vector<NodeList> alts;
    if (!parse_regex(rx, alts)) return rx;
    // Top level group-free alternation: simplify each branch.
    Node root;
    root.type = Node::Type::Group;
    root.capture = false;
    root.alts = alts;
    simplify_node(root);
    std::string out;
    if (root.type == Node::Type::Group) {
        for (std::size_t i = 0; i < root.alts.size(); ++i) {
            if (i) out += '|';
            out += render_list(root.alts[i], true);
        }
    } else {
        out = render_node(root, true);
    }
    if (out.size() > rx.size() || !regex_valid(out)) return rx;
    // Safety net: the result must accept everything the original generates, and the reverse.
    for (const auto& s : examples(rx, 40)) if (!full_match(out, s)) return rx;
    for (const auto& s : examples(out, 40)) if (!full_match(rx, s)) return rx;
    return out;
}

}  // namespace erelang::ai
