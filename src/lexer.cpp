#include "erelang/lexer.hpp"
#include <cctype>
#include <stdexcept>
#include <sstream>
#include <unordered_set>

namespace erelang {

static bool is_word_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c=='_' || c=='$' ;
}
static bool is_word_part(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c=='_' || c=='$' ;
}

static bool canEXPR() {
    
}


Lexer::Lexer(std::string source) : src_(std::move(source)) {}
Lexer::Lexer(std::string source, LexerOptions options) : src_(std::move(source)), opts_(std::move(options)) {}

std::vector<Token> Lexer::lex() {
    std::vector<Token> out;
    int line = 1;
    int col = 1;
    bool atLineStart = true;
    size_t i = 0;

    auto push = [&](TokenKind k, std::string s = {}) {
        int len = static_cast<int>(s.size());
        out.push_back(Token{ k, std::move(s), line, col, len });
    };

    auto push_span = [&](TokenKind k, size_t start, size_t end) {
        if (end < start) end = start;
        int len = static_cast<int>(end - start);
        out.push_back(Token{ k, std::string(src_.data() + start, len), line, col, len });
    };

    // Normalize CRLF to LF and handle line continuation (\\\n)
    // Note: we do this on the fly to avoid copying the whole string

    while (i < src_.size()) {
        char c = src_[i];
        if (c == '\r') { ++i; continue; }
        if (c == '\\' && i+1 < src_.size() && src_[i+1] == '\n') { i += 2; ++line; col = 1; continue; }
        if (c == '\n') { push(TokenKind::Newline); ++i; ++line; col = 1; atLineStart = true; continue; }
        if (c == ' ' || c == '\t') { ++i; ++col; continue; }
    // Preprocessor-style include: #include "path" or <path>
        if (c == '#' && atLineStart) {
            // capture directive name
            size_t j = i + 1;
            int ccol = col;
            while (j < src_.size() && (src_[j] == ' ' || src_[j] == '\t')) { ++j; ++ccol; }
            size_t k = j;
            while (k < src_.size() && std::isalpha(static_cast<unsigned char>(src_[k]))) { ++k; ++ccol; }
            std::string dir = std::string(src_.data() + j, k - j);
            // Only support include for now
            if (dir == "include") {
                // skip spaces
                while (k < src_.size() && (src_[k] == ' ' || src_[k] == '\t')) { ++k; ++ccol; }
                std::string path;
                if (k < src_.size() && (src_[k] == '"' || src_[k] == '<')) {
                    char open = src_[k];
                    char close = (open == '"') ? '"' : '>';
                    ++k; ++ccol;
                    while (k < src_.size() && src_[k] != close) { path.push_back(src_[k]); ++k; ++ccol; }
                    if (k < src_.size() && src_[k] == close) { ++k; ++ccol; }
                } else {
                    // bare path until whitespace
                    size_t m = k;
                    while (m < src_.size() && src_[m] != '\n' && src_[m] != '\r' && src_[m] != ' ' && src_[m] != '\t') { ++m; }
                    path = std::string(src_.data() + k, m - k);
                    k = m;
                }
                // Always emit as import. Resolution will search local, cpp/, then std/ and handle missing gracefully.
                push(TokenKind::Word, "import");
                push(TokenKind::String, path);

                // Optional alias support: #include "x" as name
                size_t tail = k;
                while (tail < src_.size() && (src_[tail] == ' ' || src_[tail] == '\t')) { ++tail; }
                if (tail + 2 <= src_.size() && src_[tail] == 'a' && src_[tail + 1] == 's') {
                    size_t probe = tail + 2;
                    const bool boundary = (probe >= src_.size()) || src_[probe] == ' ' || src_[probe] == '\t' || src_[probe] == '\n' || src_[probe] == '\r';
                    if (boundary) {
                        while (probe < src_.size() && (src_[probe] == ' ' || src_[probe] == '\t')) { ++probe; }
                        if (probe < src_.size() && is_word_start(src_[probe])) {
                            const size_t aliasStart = probe;
                            while (probe < src_.size() && is_word_part(src_[probe])) { ++probe; }
                            std::string alias(src_.data() + aliasStart, probe - aliasStart);
                            if (!alias.empty()) {
                                push(TokenKind::Word, "as");
                                push(TokenKind::Word, alias);
                            }
                        }
                    }
                }

                // consume until end of line
                while (k < src_.size() && src_[k] != '\n') { ++k; }
                i = k; // do not consume newline here; it will be handled next loop
                continue;
            }
            // Unknown preprocessor line: skip to end-of-line
            size_t m = i;
            while (m < src_.size() && src_[m] != '\n') { ++m; }
            i = m;
            continue;
        }
        atLineStart = false;
        // Block comments /* ... */ with nesting; also /** ... */ as DocComment when enabled
        if (c == '/' && i+1 < src_.size() && src_[i+1] == '*') {
            bool isDoc = (i+2 < src_.size() && src_[i+2] == '*');
            size_t start = i;
            i += 2; col += 2; int depth = 1;
            while (i+1 < src_.size() && depth > 0) {
                if (src_[i] == '/' && src_[i+1] == '*') { ++depth; i += 2; col += 2; continue; }
                if (src_[i] == '*' && src_[i+1] == '/') { --depth; i += 2; col += 2; continue; }
                if (src_[i] == '\n') { ++line; col = 1; ++i; continue; }
                ++i; ++col;
            }
            if (depth != 0) {
                if (opts_.emitDocComments && isDoc) {
                    push(TokenKind::Bad, "/**");
                }
                std::ostringstream oss; oss << "Unterminated block comment at " << line << ":" << col;
                push(TokenKind::Bad, oss.str());
                continue;
            }
            if (opts_.emitComments) {
                if (opts_.emitDocComments && isDoc) {
                    push(TokenKind::DocComment, std::string(src_.data()+start, i-start));
                }
            }
            continue;
        }
        if (c == '{') { push(TokenKind::LBrace, "{"); ++i; ++col; continue; }
        if (c == '}') { push(TokenKind::RBrace, "}"); ++i; ++col; continue; }
    if (c == '(') { push(TokenKind::LParen, "("); ++i; ++col; continue; }
    if (c == ')') { push(TokenKind::RParen, ")"); ++i; ++col; continue; }
    if (c == '[') { push(TokenKind::LBracket, "["); ++i; ++col; continue; }
    if (c == ']') { push(TokenKind::RBracket, "]"); ++i; ++col; continue; }
        if (c == ',') { push(TokenKind::Comma, ","); ++i; ++col; continue; }
        if (c == ';') { push(TokenKind::Semicolon, ";"); ++i; ++col; continue; }
        if (c == ':') {
            if (i+1 < src_.size() && src_[i+1] == ':') { push(TokenKind::Scope, "::"); i += 2; col += 2; continue; }
            push(TokenKind::Colon, ":"); ++i; ++col; continue;
        }
        if (c == '@') { push(TokenKind::At, "@"); ++i; ++col; continue; }
        if (c == '?') {
            if (i+1 < src_.size() && src_[i+1] == '?') { push(TokenKind::NullCoalesce, "??"); i+=2; col+=2; continue; }
            push(TokenKind::Question, "?"); ++i; ++col; continue;
        }
        if (c == '.') { push(TokenKind::Dot, "."); ++i; ++col; continue; }
        if (c == '+') {
            if (i+1 < src_.size() && src_[i+1] == '+') { push(TokenKind::PlusPlus, "++"); i+=2; col+=2; continue; }
            if (i+1 < src_.size() && src_[i+1] == '=') { push(TokenKind::PlusAssign, "+="); i+=2; col+=2; continue; }
            push(TokenKind::Plus, "+"); ++i; ++col; continue;
        }
        if (c == '-') {
            if (i+1 < src_.size() && src_[i+1] == '-') { push(TokenKind::MinusMinus, "--"); i+=2; col+=2; continue; }
            if (i+1 < src_.size() && src_[i+1] == '=') { push(TokenKind::MinusAssign, "-="); i+=2; col+=2; continue; }
            if (i+1 < src_.size() && src_[i+1] == '>') { push(TokenKind::Arrow, "->"); i+=2; col+=2; continue; }
            push(TokenKind::Minus, "-"); ++i; ++col; continue;
        }
        if (c == '*') {
            if (i+1 < src_.size() && src_[i+1] == '*') {
                if (i+2 < src_.size() && src_[i+2] == '=') { push(TokenKind::PowAssign, "**="); i+=3; col+=3; }
                else { push(TokenKind::Pow, "**"); i+=2; col+=2; }
                continue;
            }
            if (i+1 < src_.size() && src_[i+1]=='=') { push(TokenKind::StarAssign, "*="); i+=2; col+=2; }
            else { push(TokenKind::Star, "*"); ++i; ++col; }
            continue;
        }
        // slash: either comment '//' or division '/'
        if (c == '/') {
            if (i+1 < src_.size() && src_[i+1] == '/') {
                // line comment '//' or doc comment '///'
                bool isDoc = (i+2 < src_.size() && src_[i+2] == '/');
                size_t start = i;
                while (i < src_.size() && src_[i] != '\n') { ++i; }
                if (opts_.emitDocComments && isDoc) {
                    push(TokenKind::DocComment, std::string(src_.data()+start, i-start));
                }
                continue;
            }
            if (i+1 < src_.size() && src_[i+1] == '=') { push(TokenKind::SlashAssign, "/="); i+=2; col+=2; continue; }
            push(TokenKind::Slash, "/"); ++i; ++col; continue;
        }
        if (c == '%') { if (i+1 < src_.size() && src_[i+1]=='=') { push(TokenKind::PercentAssign, "%="); i+=2; col+=2; } else { push(TokenKind::Percent, "%"); ++i; ++col; } continue; }
        if (c == '!') {
            if (i+1<src_.size() && src_[i+1]=='=') { push(TokenKind::BangEqual, "!="); i+=2; col+=2; }
            else { push(TokenKind::Bang, "!"); ++i; ++col; }
            continue;
        }
        if (c == '=') {
            if (i+2 < src_.size() && src_[i+1]=='=' && src_[i+2]=='=') { push(TokenKind::StrictEqual, "==="); i+=3; col+=3; continue; }
            if (i+1<src_.size() && src_[i+1]=='=') { push(TokenKind::EqualEqual, "=="); i+=2; col+=2; }
            else if (i+1<src_.size() && src_[i+1]=='>') { push(TokenKind::FatArrow, "=>"); i+=2; col+=2; }
            else { push(TokenKind::Assign, "="); ++i; ++col; }
            continue;
        }
        if (c == '<') {
            if (i+2<src_.size() && src_[i+1]=='<' && src_[i+2]=='=') { push(TokenKind::ShlAssign, "<<="); i+=3; col+=3; continue; }
            if (i+1<src_.size() && src_[i+1]=='<') { push(TokenKind::Shl, "<<"); i+=2; col+=2; continue; }
            if (i+1<src_.size() && src_[i+1]=='=') { push(TokenKind::LessEqual, "<="); i+=2; col+=2; }
            else { push(TokenKind::Less, "<"); ++i; ++col; }
            continue;
        }
        if (c == '>') {
            if (i+3<src_.size() && src_[i+1]=='>' && src_[i+2]=='>' && src_[i+3]=='=') { push(TokenKind::Bad, ">>>="); i+=4; col+=4; continue; }
            if (i+2<src_.size() && src_[i+1]=='>' && src_[i+2]=='=') { push(TokenKind::ShrAssign, ">>="); i+=3; col+=3; continue; }
            if (i+2<src_.size() && src_[i+1]=='>' && src_[i+2]=='>') { push(TokenKind::Ushr, ">>>"); i+=3; col+=3; continue; }
            if (i+1<src_.size() && src_[i+1]=='>') { push(TokenKind::Shr, ">>"); i+=2; col+=2; continue; }
            if (i+1<src_.size() && src_[i+1]=='=') { push(TokenKind::GreaterEqual, ">="); i+=2; col+=2; }
            else { push(TokenKind::Greater, ">"); ++i; ++col; }
            continue;
        }
        if (c == '!') {
            if (i+2 < src_.size() && src_[i+1]=='=' && src_[i+2]=='=') { push(TokenKind::StrictNotEqual, "!=="); i+=3; col+=3; }
            else if (i+1<src_.size() && src_[i+1]=='=') { push(TokenKind::BangEqual, "!="); i+=2; col+=2; }
            else { push(TokenKind::Bang, "!"); ++i; ++col; }
            continue;
        }
        if (c == '|') {
            if (i+1<src_.size() && src_[i+1]=='|') { push(TokenKind::PipePipe, "||"); i+=2; col+=2; continue; }
            if (i+1<src_.size() && src_[i+1]=='=') { push(TokenKind::PipeAssign, "|="); i+=2; col+=2; continue; }
            push(TokenKind::Pipe, "|"); ++i; ++col; continue;
        }
        if (c == '^') { push(TokenKind::Caret, "^"); ++i; ++col; continue; }
        if (c == '~') { push(TokenKind::Tilde, "~"); ++i; ++col; continue; }
        // Raw string r"..." (no escapes); multi-line allowed
        if (c == 'r' && i+1 < src_.size() && src_[i+1] == '"') {
            i += 2; col += 2; std::string s;
            while (i < src_.size() && src_[i] != '"') {
                if (src_[i] == '\n') { ++line; col = 1; ++i; continue; }
                s.push_back(src_[i]); ++i; ++col;
            }
            if (i >= src_.size()) { std::ostringstream oss; oss << "Unterminated raw string at " << line << ":" << col; push(TokenKind::Bad, oss.str()); continue; }
            ++i; ++col;
            push(TokenKind::RawString, s);
            continue;
        }
        // Normal strings and character literals with escapes; allow multi-line with embedded \n
        if (c == '"' || c=='\'') {
            char quote = c; ++i; ++col; std::string s;
            while (i < src_.size() && src_[i] != quote) {
                if (src_[i]=='\\' && i+1<src_.size()) {
                    char esc = src_[i+1];
                    switch (esc) {
                        case 'n': s.push_back('\n'); break;
                        case 't': s.push_back('\t'); break;
                        case 'r': s.push_back('\r'); break;
                        case '\\': s.push_back('\\'); break;
                        case '"': s.push_back('"'); break;
                        case '\'': s.push_back('\''); break;
                        case 'x': { // hex escape \xFF
                            size_t k = i+2; int val = 0; int digits = 0;
                            while (k < src_.size() && digits < 2 && std::isxdigit(static_cast<unsigned char>(src_[k]))) {
                                char hc = src_[k]; val *= 16;
                                if (hc>='0'&&hc<='9') val += hc - '0';
                                else if (hc>='a'&&hc<='f') val += 10 + (hc - 'a');
                                else if (hc>='A'&&hc<='F') val += 10 + (hc - 'A');
                                ++k; ++digits;
                            }
                            if (digits>0) { s.push_back(static_cast<char>(val)); i = k; col += (2+digits); continue; }
                            else { s.push_back('x'); i+=2; col+=2; continue; }
                        }
                        case 'u': { // unicode \uXXXX (BMP)
                            size_t k = i+2; int val = 0; int digits = 0;
                            while (k < src_.size() && digits < 4 && std::isxdigit(static_cast<unsigned char>(src_[k]))) {
                                char hc = src_[k]; val *= 16;
                                if (hc>='0'&&hc<='9') val += hc - '0';
                                else if (hc>='a'&&hc<='f') val += 10 + (hc - 'a');
                                else if (hc>='A'&&hc<='F') val += 10 + (hc - 'A');
                                ++k; ++digits;
                            }
                            if (digits==4) { s.push_back(static_cast<char>(val & 0xFF)); i = k; col += 6; continue; }
                            else { s.push_back('u'); i+=2; col+=2; continue; }
                        }
                        default: s.push_back(esc); break;
                    }
                    i+=2; col+=2; continue;
                }
                if (src_[i] == '\n') { ++line; col = 1; ++i; continue; }
                s.push_back(src_[i]); ++i; ++col;
            }
            if (i>=src_.size()) {
                std::ostringstream oss; oss << "Unterminated string literal at " << line << ":" << col;
                push(TokenKind::Bad, oss.str());
                continue;
            }
            ++i; ++col; // consume closing quote
            if (quote == '\'') push(TokenKind::Char, s.size() ? s.substr(0,1) : std::string()); else push(TokenKind::String, s);
            continue;
        }
        // Numbers, including .5 and 123. forms; may be followed by duration or unit suffixes
        if (std::isdigit(static_cast<unsigned char>(c)) || (c=='.' && i+1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[i+1])))) {
            size_t j = i;
            // Hex 0x..., Bin 0b..., Oct 0o... or 0..., Decimal with optional fraction and exponent
            if (c == '0' && j+1 < src_.size() && (src_[j+1]=='x' || src_[j+1]=='X')) {
                j += 2; while (j < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[j]))) ++j;
            } else if (c == '0' && j+1 < src_.size() && (src_[j+1]=='b' || src_[j+1]=='B')) {
                j += 2; while (j < src_.size() && (src_[j]=='0' || src_[j]=='1')) ++j;
            } else if (c == '0' && j+1 < src_.size() && (src_[j+1]=='o' || src_[j+1]=='O')) {
                j += 2; while (j < src_.size() && (src_[j]>='0' && src_[j]<='7')) ++j;
            } else {
                bool hasDot = false;
                if (src_[j] != '.') while (j < src_.size() && std::isdigit(static_cast<unsigned char>(src_[j]))) ++j;
                if (j < src_.size() && src_[j]=='.') { hasDot = true; ++j; while (j < src_.size() && std::isdigit(static_cast<unsigned char>(src_[j]))) ++j; }
                if (j < src_.size() && (src_[j]=='e' || src_[j]=='E')) {
                    ++j; if (j < src_.size() && (src_[j]=='+' || src_[j]=='-')) ++j;
                    while (j < src_.size() && std::isdigit(static_cast<unsigned char>(src_[j]))) ++j;
                }
                (void)hasDot;
            }

            // Duration detection (e.g., 2m30s, 5s, 3h). Prefer durations over unit numbers when enabled.
            auto isDurUnit = [&](size_t p, size_t& advanced) -> bool {
                // Recognize longest of: "ms","us","ns","s","m","h","d","w"
                if (p >= src_.size()) return false;
                advanced = 0;
                auto match = [&](const char* t) -> bool {
                    size_t n = 0; while (t[n] && p+n < src_.size() && src_[p+n] == t[n]) ++n; return t[n]==0; };
                if (p+2 <= src_.size() && match("ms")) { advanced = 2; return true; }
                if (p+2 <= src_.size() && match("us")) { advanced = 2; return true; }
                if (p+2 <= src_.size() && match("ns")) { advanced = 2; return true; }
                if (match("s")) { advanced = 1; return true; }
                if (match("m")) { advanced = 1; return true; }
                if (match("h")) { advanced = 1; return true; }
                if (match("d")) { advanced = 1; return true; }
                if (match("w")) { advanced = 1; return true; }
                return false;
            };

            auto isDigit = [&](char ch)->bool{ return std::isdigit(static_cast<unsigned char>(ch)); };

            if (opts_.enableDurations) {
                size_t k = j; bool any = false; size_t last = j;
                while (k < src_.size()) {
                    // parse number component (int or float)
                    size_t nstart = k; bool hadDigit = false;
                    while (k < src_.size() && isDigit(src_[k])) { ++k; hadDigit = true; }
                    if (k < src_.size() && src_[k] == '.') { ++k; while (k < src_.size() && isDigit(src_[k])) { ++k; hadDigit = true; } }
                    if (!hadDigit) break;
                    // parse unit
                    size_t adv = 0; if (!isDurUnit(k, adv)) { k = nstart; break; }
                    k += adv; any = true; last = k;
                    // next component must start immediately (no separator). If next char is digit, loop continues; else stop.
                    if (!(k < src_.size() && isDigit(src_[k]))) break;
                }
                if (any) {
                    std::string lexeme(src_.data() + i, last - i);
                    push(TokenKind::Duration, lexeme);
                    // attach unit marker
                    out.back().unit = "duration";
                    col += static_cast<int>(last - i);
                    i = last;
                    continue;
                }
            }

            // Unit number detection (e.g., 10kg, 9.81m/s^2, 5USD)
            if (opts_.enableUnits) {
                auto isAlpha = [&](char ch)->bool{ return std::isalpha(static_cast<unsigned char>(ch)) != 0; };
                auto isUnitSym = [&](char ch)->bool{ return isAlpha(ch); };
                size_t k = j; size_t startUnit = k; bool ok = false;
                // First symbol must start with a letter (avoid treating things like 123_ as unit)
                if (k < src_.size() && isUnitSym(src_[k])) {
                    ok = true;
                    auto parseSymbol = [&](size_t& p)->bool {
                        if (!(p < src_.size() && isUnitSym(src_[p]))) return false;
                        while (p < src_.size() && (std::isalnum(static_cast<unsigned char>(src_[p])))) ++p;
                        // optional exponent ^-?digits
                        if (p < src_.size() && src_[p] == '^') {
                            size_t q = p + 1; if (q < src_.size() && (src_[q]=='+' || src_[q]=='-')) ++q;
                            size_t d = q; while (q < src_.size() && std::isdigit(static_cast<unsigned char>(src_[q]))) ++q;
                            if (q == d) return false; // require at least one digit
                            p = q;
                        }
                        return true;
                    };
                    // parse first symbol
                    size_t p = k; if (!parseSymbol(p)) ok = false;
                    // allow chain of */ symbol^exp
                    while (ok && p < src_.size() && (src_[p] == '*' || src_[p] == '/')) {
                        ++p; if (!parseSymbol(p)) { ok = false; break; }
                    }
                    if (ok) {
                        k = p;
                    }
                }
                if (ok) {
                    std::string full(src_.data() + i, k - i);
                    push(TokenKind::UnitNumber, full);
                    out.back().unit = std::string(src_.data() + startUnit, k - startUnit);
                    col += static_cast<int>(k - i);
                    i = k;
                    continue;
                }
            }

            push(TokenKind::Number, std::string(src_.data() + i, j - i));
            col += static_cast<int>(j - i);
            i = j;
            continue;
        }
        if (is_word_start(c)) {
            size_t j = i;
            while (j < src_.size() && (is_word_part(src_[j]) || (opts_.enablePolyIdentifiers && src_[j] == '#'))) { ++j; }
            std::string word(src_.data() + i, j - i);
            std::string tag;
            if (opts_.enablePolyIdentifiers) {
                size_t hashPos = word.find('#');
                if (hashPos != std::string::npos) {
                    tag = word.substr(hashPos + 1);
                    word = word.substr(0, hashPos);
                }
            }
            // Keyword/boolean/null handling
            auto kw = opts_.keywords;
            if (kw.empty()) {
                kw = { "if","else","while","for","switch","case","default","return","let","const","action","entity","hook","global","run","in","public","private","export","true","false","null","nil","nullptr" };
            }
            std::string low = word; for (auto& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (kw.count(low)) push(TokenKind::Keyword, word); else push(TokenKind::Word, word);
            if (!tag.empty()) out.back().tag = std::move(tag);
            col += static_cast<int>(j - i);
            i = j;
            continue;
        }
        std::ostringstream oss; oss << "Unexpected character '" << c << "' at " << line << ":" << col;
        push(TokenKind::Bad, oss.str());
        ++i; ++col;
        continue;
    }
    // Emit End token with current position (line/col) for better diagnostics
    out.push_back(Token{ TokenKind::End, {}, line, col, 0 });
    return out;
}

} // namespace erelang
