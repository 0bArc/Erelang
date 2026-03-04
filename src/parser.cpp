#include "erelang/parser.hpp"
#include <stdexcept>
#include <cctype>

namespace erelang {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::string Parser::qualify_name(const std::string& name) const {
    if (name.empty()) return name;
    if (name.find("::") != std::string::npos) return name;
    if (namespaceStack_.empty()) return name;
    return namespaceStack_.back() + "::" + name;
}

static inline bool is_word_or_kw(const Token& t, std::string_view w) {
    return ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == w);
}

static inline std::string ident_text(const Token& t) {
    if (t.tag.empty()) return t.text;
    return t.text + "#" + t.tag;
}

// Parse duration lexeme like "2m30s", "5s", "250ms", "1.5h" into milliseconds.
static int64_t parse_duration_ms(const std::string& s) {
    auto isdigitc = [](char c){ return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    size_t i = 0; double totalMs = 0.0;
    while (i < s.size()) {
        // number part (int or float)
        size_t start = i; bool hadDigit = false;
        while (i < s.size() && isdigitc(s[i])) { ++i; hadDigit = true; }
        if (i < s.size() && s[i] == '.') { ++i; while (i < s.size() && isdigitc(s[i])) { ++i; hadDigit = true; } }
        if (!hadDigit) break;
        double val = 0.0; try { val = std::stod(s.substr(start, i - start)); } catch (...) { val = 0.0; }
        // unit part
        double mul = 0.0;
        auto rem = s.substr(i);
        if (rem.rfind("ms", 0) == 0) { mul = 1.0; i += 2; }
        else if (rem.rfind("us", 0) == 0) { mul = 0.001; i += 2; }
        else if (rem.rfind("ns", 0) == 0) { mul = 0.000001; i += 2; }
        else if (!rem.empty() && rem[0] == 's') { mul = 1000.0; ++i; }
        else if (!rem.empty() && rem[0] == 'm') { mul = 60000.0; ++i; }
        else if (!rem.empty() && rem[0] == 'h') { mul = 3600000.0; ++i; }
        else if (!rem.empty() && rem[0] == 'd') { mul = 86400000.0; ++i; }
        else if (!rem.empty() && rem[0] == 'w') { mul = 604800000.0; ++i; }
        else { break; }
        totalMs += val * mul;
    }
    if (totalMs < 0) totalMs = 0; // clamp
    return static_cast<int64_t>(totalMs + 0.5);
}

static int64_t parse_numeric_literal(const std::string& text) {
    if (text.empty()) {
        throw std::runtime_error("Empty numeric literal");
    }

    if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        int64_t value = 0;
        for (size_t i = 2; i < text.size(); ++i) {
            const char ch = text[i];
            if (ch != '0' && ch != '1') {
                throw std::runtime_error("Invalid binary literal: " + text);
            }
            value = (value << 1) + static_cast<int64_t>(ch - '0');
        }
        return value;
    }

    if (text.size() > 2 && text[0] == '0' && (text[1] == 'o' || text[1] == 'O')) {
        int64_t value = 0;
        for (size_t i = 2; i < text.size(); ++i) {
            const char ch = text[i];
            if (ch < '0' || ch > '7') {
                throw std::runtime_error("Invalid octal literal: " + text);
            }
            value = (value * 8) + static_cast<int64_t>(ch - '0');
        }
        return value;
    }

    if (text.find('.') != std::string::npos || text.find('e') != std::string::npos || text.find('E') != std::string::npos) {
        const double dv = std::stod(text);
        return static_cast<int64_t>(dv);
    }

    size_t idx = 0;
    return std::stoll(text, &idx, 0);
}

static bool is_float_literal_text(const std::string& text) {
    return text.find('.') != std::string::npos ||
           text.find('e') != std::string::npos ||
           text.find('E') != std::string::npos;
}

const Token& Parser::peek(size_t offset) const {
    size_t idx = pos_ + offset;
    if (idx >= tokens_.size()) return tokens_.back();
    return tokens_[idx];
}

const Token& Parser::consume() {
    const Token& t = peek();
    if (pos_ < tokens_.size()) ++pos_;
    return t;
}

bool Parser::match(TokenKind kind) {
    if (peek().kind == kind) { consume(); return true; }
    return false;
}

bool Parser::match_word(std::string_view w) {
    if ((peek().kind == TokenKind::Word || peek().kind == TokenKind::Keyword) && peek().text == w) { consume(); return true; }
    return false;
}

void Parser::expect(TokenKind kind, std::string_view what) {
    if (!match(kind)) {
        throw std::runtime_error(std::string("Expected ") + std::string(what));
    }
}

void Parser::skip_separators() {
    while (peek().kind == TokenKind::Newline || peek().kind == TokenKind::Semicolon) consume();
}

ExprPtr Parser::parse_expression() { return parse_coalesce(); }

// Coalesce binds looser than AND/OR? Typically it's between OR and assignment; here place below AND/OR for simplicity.
ExprPtr Parser::parse_coalesce() {
    auto left = parse_or();
    while (match(TokenKind::NullCoalesce)) {
        auto right = parse_or();
        left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::Coalesce, left, right } });
    }
    return left;
}

ExprPtr Parser::parse_or() {
    auto left = parse_and();
    while (match(TokenKind::PipePipe)) {
        auto right = parse_and();
        left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::Or, left, right } });
    }
    return left;
}

ExprPtr Parser::parse_and() {
    auto left = parse_equality();
    while (match(TokenKind::AmpAmp)) {
        auto right = parse_equality();
        left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::And, left, right } });
    }
    return left;
}

ExprPtr Parser::parse_equality() {
    auto left = parse_relational();
    while (true) {
        if (match(TokenKind::EqualEqual)) { auto right = parse_relational(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::EQ, left, right } }); }
        else if (match(TokenKind::BangEqual)) { auto right = parse_relational(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::NE, left, right } }); }
        else break;
    }
    return left;
}

ExprPtr Parser::parse_relational() {
    auto left = parse_additive();
    while (true) {
        if (match(TokenKind::Less)) { auto right = parse_additive(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::LT, left, right } }); }
        else if (match(TokenKind::LessEqual)) { auto right = parse_additive(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::LE, left, right } }); }
        else if (match(TokenKind::Greater)) { auto right = parse_additive(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::GT, left, right } }); }
        else if (match(TokenKind::GreaterEqual)) { auto right = parse_additive(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::GE, left, right } }); }
        else break;
    }
    return left;
}

ExprPtr Parser::parse_additive() {
    auto left = parse_multiplicative();
    while (true) {
        if (match(TokenKind::Plus)) { auto right = parse_multiplicative(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::Add, left, right } }); }
        else if (match(TokenKind::Minus)) { auto right = parse_multiplicative(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::Sub, left, right } }); }
        else break;
    }
    return left;
}

ExprPtr Parser::parse_multiplicative() {
    auto left = parse_unary();
    while (true) {
        if (match(TokenKind::Star)) { auto right = parse_unary(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::Mul, left, right } }); }
        else if (match(TokenKind::Slash)) { auto right = parse_unary(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::Div, left, right } }); }
        else if (match(TokenKind::Percent)) { auto right = parse_unary(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::Mod, left, right } }); }
        else if (match(TokenKind::Caret) || match(TokenKind::Pow)) { auto right = parse_unary(); left = std::make_shared<Expr>(Expr{ BinaryExpr{ BinOp::Pow, left, right } }); }
        else break;
    }
    return left;
}

ExprPtr Parser::parse_unary() {
    if (match(TokenKind::Minus)) {
        auto e = parse_unary();
        return std::make_shared<Expr>(Expr{ UnaryExpr{ UnOp::Neg, e } });
    }
    if (match(TokenKind::Bang)) {
        auto e = parse_unary();
        return std::make_shared<Expr>(Expr{ UnaryExpr{ UnOp::Not, e } });
    }
    if (match(TokenKind::Star)) {
        auto e = parse_unary();
        return std::make_shared<Expr>(Expr{ UnaryExpr{ UnOp::Deref, e } });
    }
    if (match(TokenKind::Amp)) {
        auto e = parse_unary();
        return std::make_shared<Expr>(Expr{ UnaryExpr{ UnOp::AddressOf, e } });
    }
    return parse_primary();
}

ExprPtr Parser::parse_primary() {
    auto apply_colon_chain = [&](ExprPtr target) -> ExprPtr {
        while (match(TokenKind::Colon)) {
            const Token& methodTok = consume();
            if (!(methodTok.kind == TokenKind::Word || methodTok.kind == TokenKind::Keyword)) {
                throw std::runtime_error("Expected method name after ':'");
            }
            FunctionCallExpr call;
            call.name = std::string("string.") + ident_text(methodTok);
            call.args.push_back(target);
            expect(TokenKind::LParen, "(");
            if (!match(TokenKind::RParen)) {
                do { call.args.push_back(parse_expression()); } while (match(TokenKind::Comma));
                expect(TokenKind::RParen, ")");
            }
            target = std::make_shared<Expr>(Expr{ call });
        }
        return target;
    };

    const Token& t = peek();
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "await") {
        consume();
        return parse_primary();
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "static_cast") {
        consume();
        expect(TokenKind::Less, "<");
        std::string castType = parse_type_annotation();
        if (peek().kind == TokenKind::Greater) {
            consume();
        }
        expect(TokenKind::LParen, "(");
        auto inner = parse_expression();
        expect(TokenKind::RParen, ")");
        FunctionCallExpr call;
        std::string lower = castType;
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower == "int") call.name = "toInt";
        else if (lower == "double" || lower == "float") call.name = "tofloat";
        else if (lower == "string" || lower == "str" || lower == "char") call.name = "toString";
        else if (lower == "bool") call.name = "tobool";
        else call.name = "toString";
        call.args.push_back(inner);
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "dynamic_cast") {
        consume();
        expect(TokenKind::Less, "<");
        std::string castType = parse_type_annotation();
        if (peek().kind == TokenKind::Greater) {
            consume();
        }
        expect(TokenKind::LParen, "(");
        auto inner = parse_expression();
        expect(TokenKind::RParen, ")");
        FunctionCallExpr call;
        call.name = "dynamic_cast";
        call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ castType } }));
        call.args.push_back(inner);
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "reinterpret_cast") {
        consume();
        expect(TokenKind::Less, "<");
        std::string castType = parse_type_annotation();
        if (peek().kind == TokenKind::Greater) {
            consume();
        }
        expect(TokenKind::LParen, "(");
        auto inner = parse_expression();
        expect(TokenKind::RParen, ")");
        FunctionCallExpr call;
        call.name = "reinterpret_cast";
        call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ castType } }));
        call.args.push_back(inner);
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "sizeof") {
        consume();
        FunctionCallExpr call;
        call.name = "__builtin_sizeof";
        if (match(TokenKind::Less)) {
            const std::string typeName = parse_type_annotation();
            if (peek().kind == TokenKind::Greater) consume();
            call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ typeName } }));
            if (match(TokenKind::LParen)) expect(TokenKind::RParen, ")");
        } else if (match(TokenKind::LParen)) {
            call.args.push_back(parse_expression());
            expect(TokenKind::RParen, ")");
        } else {
            throw std::runtime_error("Expected '<type>' or '(expr)' after sizeof");
        }
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "alignof") {
        consume();
        FunctionCallExpr call;
        call.name = "__builtin_alignof";
        if (match(TokenKind::Less)) {
            const std::string typeName = parse_type_annotation();
            if (peek().kind == TokenKind::Greater) consume();
            call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ typeName } }));
            if (match(TokenKind::LParen)) expect(TokenKind::RParen, ")");
        } else if (match(TokenKind::LParen)) {
            call.args.push_back(parse_expression());
            expect(TokenKind::RParen, ")");
        } else {
            throw std::runtime_error("Expected '<type>' or '(expr)' after alignof");
        }
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "typeof") {
        consume();
        FunctionCallExpr call;
        call.name = "__builtin_typeof";
        expect(TokenKind::LParen, "(");
        call.args.push_back(parse_expression());
        expect(TokenKind::RParen, ")");
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "decltype") {
        consume();
        FunctionCallExpr call;
        call.name = "__builtin_decltype";
        expect(TokenKind::LParen, "(");
        call.args.push_back(parse_expression());
        expect(TokenKind::RParen, ")");
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "offsetof") {
        consume();
        FunctionCallExpr call;
        call.name = "__builtin_offsetof";
        expect(TokenKind::LParen, "(");
        const std::string typeName = parse_type_annotation();
        expect(TokenKind::Comma, ",");
        const Token& fieldTok = consume();
        if (!(fieldTok.kind == TokenKind::Word || fieldTok.kind == TokenKind::Keyword)) {
            throw std::runtime_error("Expected field name in offsetof(type, field)");
        }
        call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ typeName } }));
        call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ ident_text(fieldTok) } }));
        expect(TokenKind::RParen, ")");
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "is_base_of") {
        consume();
        FunctionCallExpr call;
        call.name = "__builtin_is_base_of";
        expect(TokenKind::Less, "<");
        const std::string baseType = parse_type_annotation();
        expect(TokenKind::Comma, ",");
        const std::string derivedType = parse_type_annotation();
        if (peek().kind == TokenKind::Greater) consume();
        if (match(TokenKind::LParen)) {
            if (!match(TokenKind::RParen)) {
                do { (void)parse_expression(); } while (match(TokenKind::Comma));
                expect(TokenKind::RParen, ")");
            }
        }
        call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ baseType } }));
        call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ derivedType } }));
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    // new Type(expr, ...)
    if ((t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) && t.text == "new") {
        consume();
        const Token& typeTok = consume();
            if (!(typeTok.kind == TokenKind::Word || typeTok.kind == TokenKind::Keyword)) throw std::runtime_error("Expected type name after 'new'");
        NewExpr ne; ne.typeName = ident_text(typeTok);
        while (match(TokenKind::Scope)) {
            const Token& seg = consume();
            if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected type segment after '::'");
            ne.typeName += "::" + ident_text(seg);
        }
        if (match(TokenKind::LParen)) {
            if (!match(TokenKind::RParen)) {
                do { ne.args.push_back(parse_expression()); } while (match(TokenKind::Comma));
                expect(TokenKind::RParen, ")");
            }
        }
        return std::make_shared<Expr>(Expr{ ne });
    }
    if (t.kind == TokenKind::String || t.kind == TokenKind::Char || t.kind == TokenKind::RawString) {
        consume();
        return apply_colon_chain(std::make_shared<Expr>(Expr{ ExprString{ t.text } }));
    }
    if (t.kind == TokenKind::Number) {
        consume();
        return apply_colon_chain(std::make_shared<Expr>(Expr{ ExprNumber{ parse_numeric_literal(t.text), is_float_literal_text(t.text), t.text } }));
    }
    if (t.kind == TokenKind::Duration) {
        consume();
        const auto ms = parse_duration_ms(t.text);
        return apply_colon_chain(std::make_shared<Expr>(Expr{ ExprNumber{ ms, false, std::to_string(ms) } }));
    }
    if (t.kind == TokenKind::UnitNumber) {
        consume();
        return apply_colon_chain(std::make_shared<Expr>(Expr{ ExprString{ t.text } }));
    }
    if (t.kind == TokenKind::Word || t.kind == TokenKind::Keyword) {
    if (t.text == "true" || t.text == "false") { consume(); return apply_colon_chain(std::make_shared<Expr>(Expr{ ExprBool{ t.text == "true" } })); }
    if (t.text == "null" || t.text == "nil" || t.text == "nullptr") { consume(); return apply_colon_chain(std::make_shared<Expr>(Expr{ ExprNull{} })); }
        consume();
        std::string baseName = ident_text(t);
        while (match(TokenKind::Scope)) {
            const Token& seg = consume();
            if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected identifier after '::'");
            baseName += "::" + ident_text(seg);
        }
        bool hadIndex = false;
        while (match(TokenKind::LBracket)) {
            hadIndex = true;
            const Token& keyTok = consume();
            std::string key;
            if (keyTok.kind == TokenKind::String || keyTok.kind == TokenKind::Char) key = keyTok.text;
            else if (keyTok.kind == TokenKind::Word || keyTok.kind == TokenKind::Keyword || keyTok.kind == TokenKind::Number) key = keyTok.text;
            else throw std::runtime_error("Expected literal key inside '[]'");
            expect(TokenKind::RBracket, "]");
            if (key.empty()) throw std::runtime_error("Empty key inside '[]'");
            baseName += ":" + key;
        }
        // function call: ident(...)
        if (match(TokenKind::LParen)) {
            if (hadIndex) throw std::runtime_error("Indexed identifier cannot be called as a function");
            FunctionCallExpr call; call.name = baseName;
            if (!match(TokenKind::RParen)) {
                do { call.args.push_back(parse_expression()); } while (match(TokenKind::Comma));
                expect(TokenKind::RParen, ")");
            }
            return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
        }
        // lookahead for dotted member access / dotted function calls as expressions
        std::vector<std::string> members;
        while (match(TokenKind::Dot)) {
            const Token& mem = consume();
            if (!(mem.kind == TokenKind::Word || mem.kind == TokenKind::Keyword)) throw std::runtime_error("Expected member name after '.'");
            members.push_back(ident_text(mem));
        }
        if (!members.empty()) {
            std::string dottedName = baseName;
            for (const auto& member : members) {
                dottedName += "." + member;
            }
            if (match(TokenKind::LParen)) {
                FunctionCallExpr call; call.name = dottedName;
                if (!match(TokenKind::RParen)) {
                    do { call.args.push_back(parse_expression()); } while (match(TokenKind::Comma));
                    expect(TokenKind::RParen, ")");
                }
                return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
            }
            if (members.size() == 1) {
                return apply_colon_chain(std::make_shared<Expr>(Expr{ MemberExpr{ baseName, members.front() } }));
            }
            return apply_colon_chain(std::make_shared<Expr>(Expr{ ExprIdent{ dottedName } }));
        }
        return apply_colon_chain(std::make_shared<Expr>(Expr{ ExprIdent{ baseName } }));
    }
    if (match(TokenKind::LBracket)) {
        FunctionCallExpr call;
        call.name = "list_new";
        skip_separators();
        if (!match(TokenKind::RBracket)) {
            do {
                skip_separators();
                call.args.push_back(parse_expression());
                skip_separators();
            } while (match(TokenKind::Comma));
            expect(TokenKind::RBracket, "]");
        }
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if (match(TokenKind::LBrace)) {
        FunctionCallExpr call;
        call.name = "dict_new";
        skip_separators();
        if (!match(TokenKind::RBrace)) {
            while (true) {
                skip_separators();
                const Token& keyTok = consume();
                std::string key;
                if (keyTok.kind == TokenKind::String || keyTok.kind == TokenKind::Char || keyTok.kind == TokenKind::Word || keyTok.kind == TokenKind::Keyword || keyTok.kind == TokenKind::Number) {
                    key = keyTok.text;
                } else {
                    throw std::runtime_error("Expected dictionary key");
                }
                call.args.push_back(std::make_shared<Expr>(Expr{ ExprString{ key } }));
                expect(TokenKind::Colon, ":");
                call.args.push_back(parse_expression());
                if (match(TokenKind::Comma)) {
                    continue;
                }
                if (peek().kind == TokenKind::Newline || peek().kind == TokenKind::Semicolon) {
                    skip_separators();
                    if (peek().kind != TokenKind::RBrace) {
                        continue;
                    }
                }
                break;
            }
            expect(TokenKind::RBrace, "}");
        }
        return apply_colon_chain(std::make_shared<Expr>(Expr{ call }));
    }
    if (match(TokenKind::LParen)) { auto e = parse_expression(); expect(TokenKind::RParen, ")"); return apply_colon_chain(e); }
    throw std::runtime_error(std::string("Expected expression near token: '") + std::string(peek().text) + "'");
}

Block Parser::parse_block() {
    Block b;
    expect(TokenKind::LBrace, "{");
    skip_separators();
    while (peek().kind != TokenKind::RBrace && peek().kind != TokenKind::End) {
        b.stmts.push_back(parse_statement());
        skip_separators();
    }
    expect(TokenKind::RBrace, "}");
    return b;
}

Action Parser::parse_action() {
    // optional visibility/export and attributes precede 'action'
    auto attrs = parse_attributes();
    bool isExport = false;
    bool isAsync = false;
    Visibility vis = Visibility::Public;
    bool hadVis = false;
    while (true) {
        if (match_word("export")) { isExport = true; continue; }
        if (match_word("async")) { isAsync = true; continue; }
        if (match_word("public")) { vis = Visibility::Public; hadVis = true; continue; }
        if (match_word("private")) { vis = Visibility::Private; hadVis = true; continue; }
        break;
    }
    if (!match_word("action")) throw std::runtime_error("Expected 'action'");
    const Token& nameTok = consume();
    if (!(nameTok.kind == TokenKind::Word || nameTok.kind == TokenKind::Keyword)) throw std::runtime_error("Action name");
    Action a;
    const std::string rawName = ident_text(nameTok);
    a.name = parsingEntityMethod_ ? rawName : qualify_name(rawName);
    a.visibility = vis;
    a.exported = isExport;
    a.attributes = std::move(attrs);
    if (match(TokenKind::LParen)) {
        if (!match(TokenKind::RParen)) {
            do {
                const Token& pn = consume();
                if (!(pn.kind == TokenKind::Word || pn.kind == TokenKind::Keyword)) throw std::runtime_error("Param name");
                std::string ptype;
                if (match(TokenKind::Colon)) {
                    ptype = parse_type_annotation();
                }
                a.params.push_back(Param{ident_text(pn), ptype});
            } while (match(TokenKind::Comma));
            expect(TokenKind::RParen, ")");
        }
    }
    // optional return type: : type
    if (match(TokenKind::Colon)) {
        a.returnType = parse_type_annotation();
    }
    a.isAsync = isAsync;
    a.body = parse_block();
    if (!hadVis) throw std::runtime_error("Missing visibility (public/private) for action: " + a.name);
    return a;
}

Hook Parser::parse_hook() {
    auto attrs = parse_attributes();
    if (!match_word("hook")) throw std::runtime_error("Expected 'hook'");
    const Token& nameTok = consume();
    if (!(nameTok.kind == TokenKind::Word || nameTok.kind == TokenKind::Keyword)) throw std::runtime_error("Hook name");
    Hook h; h.name = qualify_name(ident_text(nameTok)); h.attributes = std::move(attrs); h.body = parse_block();
    return h;
}

GlobalDecl Parser::parse_global() {
    // optional attributes/modifiers before global
    auto attrs = parse_attributes();
    (void)attrs;

    while (true) {
        bool consumedModifier = false;
        if (match_word("export")) consumedModifier = true;
        else if (match_word("public")) consumedModifier = true;
        else if (match_word("private")) consumedModifier = true;

        if (!consumedModifier) break;
        skip_separators();
    }

    skip_separators();
    if (!match_word("global")) throw std::runtime_error("Expected 'global'");
    const Token& nameTok = consume();
    if (!(nameTok.kind == TokenKind::Word || nameTok.kind == TokenKind::Keyword)) throw std::runtime_error("Global name");
    expect(TokenKind::Assign, "=");
    auto val = parse_expression();
    return GlobalDecl{ qualify_name(ident_text(nameTok)), val, std::string{} };
}

Statement Parser::parse_statement() {
    // C++-style pointer assignment: *p = expr
    if (peek().kind == TokenKind::Star) {
        const size_t savePos = pos_;
        consume();
        if (peek().kind == TokenKind::Word || peek().kind == TokenKind::Keyword) {
            const Token& ptrTok = consume();
            if (match(TokenKind::Assign)) {
                auto ptrExpr = std::make_shared<Expr>(Expr{ ExprIdent{ ident_text(ptrTok) } });
                auto valueExpr = parse_expression();
                return PointerSetStmt{ ptrExpr, valueExpr };
            }
        }
        pos_ = savePos;
    }

    // if
    if (is_word_or_kw(peek(), "if")) {
        return parse_if();
    }
    if (is_word_or_kw(peek(), "unsafe")) {
        consume();
        auto body = std::make_shared<Block>(parse_block());
        return UnsafeStmt{ body };
    }
    if (is_word_or_kw(peek(), "try")) {
        return parse_try_catch();
    }
    if (is_word_or_kw(peek(), "match")) {
        consume();
        auto sel = parse_expression();
        expect(TokenKind::LBrace, "{");
        SwitchStmt sw; sw.selector = sel;
        skip_separators();
        while (peek().kind != TokenKind::RBrace && peek().kind != TokenKind::End) {
            if (match_word("case")) {
                const Token& v = consume();
                if (!(v.kind == TokenKind::String || v.kind == TokenKind::Word || v.kind == TokenKind::Keyword || v.kind == TokenKind::Number)) {
                    throw std::runtime_error("case value");
                }
                Block b;
                if (match(TokenKind::Colon)) {
                    skip_separators();
                    b.stmts.push_back(parse_statement());
                } else {
                    b = parse_block();
                }
                sw.cases.push_back(SwitchCase{v.text, std::make_shared<Block>(std::move(b))});
            } else if (match_word("default")) {
                if (match(TokenKind::Colon)) {
                    Block b;
                    skip_separators();
                    b.stmts.push_back(parse_statement());
                    sw.defaultBlk = std::make_shared<Block>(std::move(b));
                } else {
                    sw.defaultBlk = std::make_shared<Block>(parse_block());
                }
            } else {
                skip_separators();
                if (!(peek().kind == TokenKind::RBrace || peek().kind == TokenKind::End)) throw std::runtime_error("Expected 'case' or 'default'");
            }
            skip_separators();
        }
        expect(TokenKind::RBrace, "}");
        return sw;
    }
    // while
    if (is_word_or_kw(peek(), "while")) {
        return parse_while();
    }
    // repeat N { ... }
    if (is_word_or_kw(peek(), "repeat")) {
        consume();
        auto count = parse_expression();
        auto body = std::make_shared<Block>(parse_block());
        return RepeatStmt{ count, body };
    }
    // for / for-each
    if ((peek().kind == TokenKind::Word || peek().kind == TokenKind::Keyword) && peek().text == "for") {
        // Lookahead for for-each forms (colon-only):
        // for (item : iterable), for (k, v : iterable)
        bool isForIn = false;
        if (peek(1).kind == TokenKind::LParen && (peek(2).kind == TokenKind::Word || peek(2).kind == TokenKind::Keyword)) {
            if (peek(3).kind == TokenKind::Colon) {
                isForIn = true;
            } else if (peek(3).kind == TokenKind::Comma && (peek(4).kind == TokenKind::Word || peek(4).kind == TokenKind::Keyword)) {
                if (peek(5).kind == TokenKind::Colon) {
                    isForIn = true;
                }
            }
        }
        if (isForIn) {
            consume(); // 'for'
            expect(TokenKind::LParen, "(");
            return parse_for_in_after_lparen();
        }
        return parse_for();
    }
    // print expr
    if (match_word("print")) {
        return PrintStmt{ parse_expression() };
    }
    // sleep 250 [ms]
    if (match_word("sleep")) {
        const Token& t = consume();
        if (t.kind == TokenKind::Duration) {
            return SleepStmt{ parse_duration_ms(t.text) };
        }
        if (t.kind == TokenKind::Number) {
            int64_t num = std::stoll(t.text);
            if (peek().kind == TokenKind::Word && peek().text == "ms") consume();
            return SleepStmt{ num };
        }
        throw std::runtime_error("Expected duration literal or number after 'sleep'");
    }
    // typed declaration with optional constexpr/static modifiers and generic type names
    if (!is_word_or_kw(peek(), "let") && !is_word_or_kw(peek(), "const")) {
        const size_t savePos = pos_;
        bool isConstDecl = false;
        if (match_word("constexpr")) isConstDecl = true;
        (void)match_word("static");
        try {
            std::string declaredType = parse_type_annotation();
            const Token& nameTok = consume();
            if (nameTok.kind != TokenKind::Word) throw std::runtime_error("Variable name");
            if (!match(TokenKind::Assign) && peek().kind != TokenKind::LBrace) {
                throw std::runtime_error("=");
            }
            auto val = parse_expression();
            return LetStmt{ isConstDecl, ident_text(nameTok), val, declaredType };
        } catch (...) {
            pos_ = savePos;
        }
    }
    // let/const variable declaration (treat keyword or word)
    if (is_word_or_kw(peek(), "let") || is_word_or_kw(peek(), "const")) {
        bool isConst = false;
        if (match_word("let")) isConst = false; else if (match_word("const")) isConst = true;
        const Token& nameTok = consume();
        if (nameTok.kind != TokenKind::Word) throw std::runtime_error("Variable name");
        if (!match(TokenKind::Assign) && peek().kind != TokenKind::LBrace) {
            throw std::runtime_error("=");
        }
        auto val = parse_expression();
        return LetStmt{ isConst, ident_text(nameTok), val, std::string{} };
    }
    // return [expr] (expression optional)
    if (match_word("return")) {
        // If the next token ends the statement or block, treat as no-value return
        TokenKind k = peek().kind;
        if (k == TokenKind::RBrace || k == TokenKind::Semicolon || k == TokenKind::Newline || k == TokenKind::End) {
            return ReturnStmt{ std::nullopt };
        }
        auto val = parse_expression();
        return ReturnStmt{ val };
    }
    // parallel { ... }
    if (match_word("parallel")) {
        auto p = std::make_shared<ParallelStmt>();
        p->body = parse_block();
        return p;
    }
    // wait all
    if (match_word("wait")) {
        if (!match_word("all")) throw std::runtime_error("Expected 'all'");
        return WaitAllStmt{};
    }
    // pause (wait for Enter)
    if (match_word("pause")) {
        return PauseStmt{};
    }
    // input <name>
    if (match_word("input")) {
        const Token& nameTok = consume();
        if (nameTok.kind != TokenKind::Word) throw std::runtime_error("Expected variable name after 'input'");
        return InputStmt{ ident_text(nameTok) };
    }
    // switch
    if (is_word_or_kw(peek(), "switch")) {
        return parse_switch();
    }
    // fire eventName
    if (match_word("fire")) {
        const Token& e = consume();
        if (e.kind != TokenKind::Word) throw std::runtime_error("Expected event name");
        return FireStmt{ ident_text(e) };
    }
    // run main (top-level only, but allow as stmt capturing target)
    if (match_word("run")) {
        const Token& nameTok = consume();
        if (!(nameTok.kind == TokenKind::Word || nameTok.kind == TokenKind::Keyword)) throw std::runtime_error("Expected action name after run");
        std::string callName = ident_text(nameTok);
        while (match(TokenKind::Scope)) {
            const Token& seg = consume();
            if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected scoped segment after '::'");
            callName += "::" + ident_text(seg);
        }
        return ActionCallStmt{ callName, {} };
    }
    // member assignment or dotted call like obj.field = expr or a.b.c(...)
    if (peek().kind == TokenKind::Word || peek().kind == TokenKind::Keyword) {
        const Token& objTok = peek();
        if (peek(1).kind == TokenKind::Dot) {
            consume(); // object name
            expect(TokenKind::Dot, ".");
            const Token& mem = consume();
            if (!(mem.kind == TokenKind::Word || mem.kind == TokenKind::Keyword)) throw std::runtime_error("Expected member name after '.'");
            // assignment
            if (match(TokenKind::Assign)) {
                auto val = parse_expression();
                return SetStmt{ true, ident_text(mem), ident_text(objTok), val };
            }
            // method call (single-dot) or dotted function-style call (multi-dot)
            std::string dottedName = ident_text(objTok) + "." + ident_text(mem);
            while (match(TokenKind::Dot)) {
                const Token& seg = consume();
                if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected member name after '.'");
                dottedName += "." + ident_text(seg);
            }
            if (match(TokenKind::LParen)) {
                ActionCallStmt call;
                call.name = dottedName;
                if (!match(TokenKind::RParen)) {
                    do { call.args.push_back(parse_expression()); } while (match(TokenKind::Comma));
                    expect(TokenKind::RParen, ")");
                }
                return call;
            }
            throw std::runtime_error("Expected '=' or '(' after member name");
        }
        // plain assignment: name = expr
        if (peek(1).kind == TokenKind::Assign) {
            const Token& n = consume(); // name
            consume(); // '='
            auto v = parse_expression();
            return SetStmt{ false, ident_text(n), std::string{}, v };
        }
    }
    if (match_word("await")) {
        const Token& fn = consume();
        if (!(fn.kind == TokenKind::Word || fn.kind == TokenKind::Keyword)) throw std::runtime_error("Expected callable name after 'await'");
        ActionCallStmt call; call.name = ident_text(fn);
        while (match(TokenKind::Scope)) {
            const Token& seg = consume();
            if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected scoped segment after '::'");
            call.name += "::" + ident_text(seg);
        }
        if (match(TokenKind::LParen)) {
            if (!match(TokenKind::RParen)) {
                do { call.args.push_back(parse_expression()); } while (match(TokenKind::Comma));
                expect(TokenKind::RParen, ")");
            }
        }
        return call;
    }

    // action/builtin call like greet("World")
    const Token& id = peek();
    if (id.kind == TokenKind::Word || id.kind == TokenKind::Keyword) {
    consume();
    ActionCallStmt call; call.name = ident_text(id);
        while (match(TokenKind::Scope)) {
            const Token& seg = consume();
            if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected scoped segment after '::'");
            call.name += "::" + ident_text(seg);
        }
        if (match(TokenKind::LParen)) {
            if (!match(TokenKind::RParen)) {
                do { call.args.push_back(parse_expression()); } while (match(TokenKind::Comma));
                expect(TokenKind::RParen, ")");
            }
        }
        return call;
    }

    throw std::runtime_error("Unknown statement");
}

Program Parser::parse_program() {
    Program prog;
    skip_separators();
    // Optional @erelang header (case-insensitive). Only consume when it matches.
    if (peek().kind == TokenKind::At && peek(1).kind == TokenKind::Word) {
        std::string name = std::string(peek(1).text);
        for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (name == "erelang") {
            consume(); // '@'
            consume(); // 'erelang'
            skip_separators();
        }
    }
    // file-level directives like @strict, @debug, @entry(name) and unknowns retained
    while (peek().kind == TokenKind::At) {
        consume();
        const Token& d = consume();
        if (!(d.kind == TokenKind::Word || d.kind == TokenKind::Keyword)) throw std::runtime_error("Directive name");
        Attribute attr; attr.name = d.text;
        std::string dname = std::string(d.text);
        for (auto& ch : dname) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (match(TokenKind::LParen)) {
            const Token& v = consume();
            if (v.kind != TokenKind::String && v.kind != TokenKind::Word && v.kind != TokenKind::Number) throw std::runtime_error("Directive value");
            attr.value = v.text;
            expect(TokenKind::RParen, ")");
        }
        if (dname == "strict") { prog.strict = true; strict_ = true; }
        else if (dname == "debug") prog.debug = true;
        else if (dname == "entry" && attr.value) prog.runTarget = *attr.value;
        prog.directives.push_back(std::move(attr));
        skip_separators();
    }
    while (peek().kind != TokenKind::End) {
        if (peek().kind == TokenKind::RBrace) {
            if (!namespaceStack_.empty()) {
                consume();
                namespaceStack_.pop_back();
                skip_separators();
                continue;
            }
            throw std::runtime_error("Unexpected '}' at top level");
        }
        // classify next top-level declaration while allowing attributes/modifiers
        auto classify_next = [&](void) -> std::string {
            size_t i = 0;
            // skip separators
            while (peek(i).kind == TokenKind::Newline || peek(i).kind == TokenKind::Semicolon) ++i;
            // attributes
            while (peek(i).kind == TokenKind::At || peek(i).kind == TokenKind::LBracket) {
                if (peek(i).kind == TokenKind::At) {
                    ++i; // '@'
                    if (!(peek(i).kind == TokenKind::Word || peek(i).kind == TokenKind::Keyword)) return std::string();
                    ++i; // name
                    if (peek(i).kind == TokenKind::LParen) {
                        ++i; // '('
                        while (peek(i).kind != TokenKind::RParen && peek(i).kind != TokenKind::End) ++i;
                        if (peek(i).kind == TokenKind::RParen) ++i;
                    }
                } else {
                    ++i; // '['
                    if (!(peek(i).kind == TokenKind::Word || peek(i).kind == TokenKind::Keyword)) return std::string();
                    ++i; // name
                    if (peek(i).kind == TokenKind::LParen) {
                        ++i;
                        while (peek(i).kind != TokenKind::RParen && peek(i).kind != TokenKind::End) ++i;
                        if (peek(i).kind == TokenKind::RParen) ++i;
                    }
                    if (peek(i).kind == TokenKind::RBracket) ++i;
                }
                while (peek(i).kind == TokenKind::Newline || peek(i).kind == TokenKind::Semicolon) ++i;
            }
            // modifiers
            while ((peek(i).kind == TokenKind::Word || peek(i).kind == TokenKind::Keyword) && (peek(i).text == "export" || peek(i).text == "public" || peek(i).text == "private" || peek(i).text == "async")) {
                ++i;
                while (peek(i).kind == TokenKind::Newline || peek(i).kind == TokenKind::Semicolon) ++i;
            }
            if (peek(i).kind == TokenKind::Word || peek(i).kind == TokenKind::Keyword) {
                std::string w = std::string(peek(i).text);
                if (w == "action") return std::string("action");
                if (w == "entity") return std::string("entity");
                if (w == "hook") return std::string("hook");
                if (w == "global") return std::string("global");
                if (w == "import") return std::string("import");
                if (w == "struct") return std::string("struct");
                if (w == "enum") return std::string("enum");
                if (w == "type") return std::string("type");
                if (w == "namespace") return std::string("namespace");
                if (w == "run") return std::string("run");
            }
            return std::string();
        };

        std::string kind = classify_next();
        if (kind == "action") {
            prog.actions.push_back(parse_action());
        } else if (kind == "entity") {
            prog.entities.push_back(parse_entity());
        } else if (kind == "hook") {
            prog.hooks.push_back(parse_hook());
        } else if (kind == "global") {
            prog.globals.push_back(parse_global());
        } else if (kind == "import") {
            consume();
            ImportDecl decl;
            auto normalize_path = [](std::string path) {
                for (auto& ch : path) if (ch == '\\') ch = '/';
                return path;
            };
            if (match(TokenKind::Less)) {
                std::string path;
                while (peek().kind != TokenKind::Greater && peek().kind != TokenKind::End) {
                    const Token& tk = consume();
                    if (tk.kind == TokenKind::Newline || tk.kind == TokenKind::Semicolon) break;
                    path += tk.text;
                }
                if (!match(TokenKind::Greater)) throw std::runtime_error("Unterminated import <...> path");
                if (path.empty()) throw std::runtime_error("Empty import <...> path");
                decl.path = normalize_path(path);
            } else {
                const Token& first = consume();
                if (first.kind != TokenKind::Word && first.kind != TokenKind::String && first.kind != TokenKind::Keyword && first.kind != TokenKind::Slash)
                    throw std::runtime_error("Module path");
                std::string path = first.text;
                if (first.kind != TokenKind::String) {
                    while (true) {
                        const Token& next = peek();
                        if (next.kind == TokenKind::End || next.kind == TokenKind::Newline || next.kind == TokenKind::Semicolon) break;
                        if (next.kind == TokenKind::Word && next.text == "as") break;
                        if (next.kind == TokenKind::Comma) break;
                        path += consume().text;
                    }
                }
                if (path.empty()) throw std::runtime_error("Empty import path");
                decl.path = normalize_path(path);
            }
            if (match_word("as")) {
                const Token& aliasTok = consume();
                if (!(aliasTok.kind == TokenKind::Word || aliasTok.kind == TokenKind::Keyword)) throw std::runtime_error("Expected alias after 'as'");
                decl.alias = ident_text(aliasTok);
            }
            if (decl.path == "/plugins/*/project.elp") {
                decl.pluginGlob = true;
                if (!decl.alias) decl.alias = std::string{"plugin"};
                prog.pluginAliases.push_back(*decl.alias);
            }
            prog.imports.push_back(std::move(decl));
        } else if (kind == "struct") {
            prog.structs.push_back(parse_struct());
        } else if (kind == "enum") {
            prog.enums.push_back(parse_enum());
        } else if (kind == "type") {
            prog.typeAliases.push_back(parse_type_alias());
        } else if (kind == "namespace") {
            consume();
            const Token& firstNs = consume();
            if (!(firstNs.kind == TokenKind::Word || firstNs.kind == TokenKind::Keyword)) throw std::runtime_error("Expected namespace name");
            std::string nsName = ident_text(firstNs);
            while (match(TokenKind::Scope)) {
                const Token& seg = consume();
                if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected namespace segment after '::'");
                nsName += "::" + ident_text(seg);
            }
            expect(TokenKind::LBrace, "{");
            namespaceStack_.push_back(qualify_name(nsName));
        } else if (kind == "run") {
            consume();
            const Token& t = consume();
            if (!(t.kind == TokenKind::Word || t.kind == TokenKind::Keyword)) throw std::runtime_error("Expected action name after run");
            std::string runName = ident_text(t);
            while (match(TokenKind::Scope)) {
                const Token& seg = consume();
                if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected scoped segment after '::'");
                runName += "::" + ident_text(seg);
            }
            prog.runTarget = qualify_name(runName);
        } else {
            // allow stray separators and ignore unknown top-level '@' blocks handled above
            skip_separators();
            if (peek().kind == TokenKind::End) break;
            if (peek().kind == TokenKind::At) continue;
            throw std::runtime_error("Unexpected top-level token");
        }
        skip_separators();
    }
    if (!namespaceStack_.empty()) {
        throw std::runtime_error("Unterminated namespace block");
    }
    return prog;
}

Program Parser::parse() { return parse_program(); }

WhileStmt Parser::parse_while() {
    if (!match_word("while")) throw std::runtime_error("Expected 'while'");
    expect(TokenKind::LParen, "(");
    auto cond = parse_expression();
    expect(TokenKind::RParen, ")");
    auto body = std::make_shared<Block>(parse_block());
    return WhileStmt{ cond, body };
}

ForStmt Parser::parse_for() {
    if (!match_word("for")) throw std::runtime_error("Expected 'for'");
    expect(TokenKind::LParen, "(");
    // init (optional)
    std::shared_ptr<Block> initBlk = std::make_shared<Block>();
    if (!match(TokenKind::Semicolon)) {
        // allow let/const or assignment simple form: name = expr
        if (peek().kind == TokenKind::Word && (peek().text == "let" || peek().text == "const")) {
            bool isConst = match_word("let") ? false : (match_word("const"), true);
            if (!isConst && peek().text == "const") { consume(); isConst = true; }
            const Token& nameTok = consume(); if (nameTok.kind != TokenKind::Word) throw std::runtime_error("Variable name");
            expect(TokenKind::Assign, "="); auto val = parse_expression();
            initBlk->stmts.push_back(LetStmt{ isConst, ident_text(nameTok), val, std::string{} });
        } else if (peek().kind == TokenKind::Word && peek(1).kind == TokenKind::Assign) {
            const Token& n = consume(); consume(); auto v = parse_expression();
            initBlk->stmts.push_back(SetStmt{ false, ident_text(n), std::string{}, v });
        } else {
            const size_t savePos = pos_;
            bool typedAccepted = false;
            bool isConstDecl = false;
            if (match_word("constexpr")) isConstDecl = true;
            (void)match_word("static");
            try {
                std::string declaredType = parse_type_annotation();
                if (peek().kind == TokenKind::Word && peek(1).kind == TokenKind::Assign) {
                    const Token& nameTok = consume();
                    consume();
                    auto val = parse_expression();
                    initBlk->stmts.push_back(LetStmt{ isConstDecl, ident_text(nameTok), val, declaredType });
                    typedAccepted = true;
                }
            } catch (...) {
                typedAccepted = false;
            }
            if (!typedAccepted) {
                pos_ = savePos;
            }
        }
        expect(TokenKind::Semicolon, ";");
    }
    // cond (optional)
    std::optional<ExprPtr> cond;
    if (!match(TokenKind::Semicolon)) { cond = parse_expression(); expect(TokenKind::Semicolon, ";"); }
    // step (optional)
    std::shared_ptr<Block> stepBlk = std::make_shared<Block>();
    if (!match(TokenKind::RParen)) {
        if (peek().kind == TokenKind::Word && peek(1).kind == TokenKind::Assign) {
            const Token& n = consume(); consume(); auto v = parse_expression();
            stepBlk->stmts.push_back(SetStmt{ false, ident_text(n), std::string{}, v });
        } else if (peek().kind == TokenKind::Word && (peek().text == "let" || peek().text == "const")) {
            bool isConst = match_word("let") ? false : (match_word("const"), true);
            if (!isConst && peek().text == "const") { consume(); isConst = true; }
            const Token& nameTok = consume(); if (nameTok.kind != TokenKind::Word) throw std::runtime_error("Variable name");
            expect(TokenKind::Assign, "="); auto val = parse_expression();
            stepBlk->stmts.push_back(LetStmt{ isConst, ident_text(nameTok), val, std::string{} });
        } else {
            const size_t savePos = pos_;
            bool typedAccepted = false;
            bool isConstDecl = false;
            if (match_word("constexpr")) isConstDecl = true;
            (void)match_word("static");
            try {
                std::string declaredType = parse_type_annotation();
                if (peek().kind == TokenKind::Word && peek(1).kind == TokenKind::Assign) {
                    const Token& nameTok = consume();
                    consume();
                    auto val = parse_expression();
                    stepBlk->stmts.push_back(LetStmt{ isConstDecl, ident_text(nameTok), val, declaredType });
                    typedAccepted = true;
                }
            } catch (...) {
                typedAccepted = false;
            }
            if (!typedAccepted) {
                pos_ = savePos;
            }
        }
        expect(TokenKind::RParen, ")");
    }
    auto body = std::make_shared<Block>(parse_block());
    return ForStmt{ initBlk, cond, stepBlk, body };
}

ForInStmt Parser::parse_for_in_after_lparen() {
    // Assumes '(' has been consumed
    const Token& varTok = consume();
    if (!(varTok.kind == TokenKind::Word || varTok.kind == TokenKind::Keyword)) throw std::runtime_error("Expected loop variable name");
    std::optional<std::string> valueVar;
    if (match(TokenKind::Comma)) {
        const Token& valueTok = consume();
        if (!(valueTok.kind == TokenKind::Word || valueTok.kind == TokenKind::Keyword)) throw std::runtime_error("Expected value variable name");
        valueVar = ident_text(valueTok);
    }
    bool usedColon = false;
    if (match(TokenKind::Colon)) usedColon = true;
    else throw std::runtime_error("Expected ':' in for-each loop");
    auto it = parse_expression();
    expect(TokenKind::RParen, ")");
    auto body = std::make_shared<Block>(parse_block());
    return ForInStmt{ ident_text(varTok), valueVar, usedColon, it, body };
}

Entity Parser::parse_entity() {
    // optional attributes/modifiers
    auto attrs = parse_attributes();
    bool isExport = false; Visibility vis = Visibility::Public;
        if (match_word("export")) isExport = true;
        bool hadVis = false;
        if (match_word("public")) { vis = Visibility::Public; hadVis = true; }
        else if (match_word("private")) { vis = Visibility::Private; hadVis = true; }
    if (!match_word("entity")) throw std::runtime_error("Expected 'entity'");
    const Token& nameTok = consume();
    if (!(nameTok.kind == TokenKind::Word || nameTok.kind == TokenKind::Keyword)) throw std::runtime_error("Entity name");
    Entity e; e.name = qualify_name(ident_text(nameTok)); e.visibility = vis; e.exported = isExport; e.attributes = std::move(attrs);
    if (match(TokenKind::Colon)) {
        e.baseType = parse_type_annotation();
    }
    expect(TokenKind::LBrace, "{");
    skip_separators();
    while (peek().kind != TokenKind::RBrace && peek().kind != TokenKind::End) {
        // Decide if next member is a method or a field
        bool parseMethod = false;
        if (peek().kind == TokenKind::Word || peek().kind == TokenKind::Keyword) {
            if (peek().text == "action") parseMethod = true;
            else if (peek().text == "public" || peek().text == "private" || peek().text == "export") {
                // Look ahead after modifiers; if we find 'action', it's a method
                size_t i = 0;
                // consume any sequence of modifiers with optional separators between
                while ((peek(i).kind == TokenKind::Word || peek(i).kind == TokenKind::Keyword) &&
                       (peek(i).text == "public" || peek(i).text == "private" || peek(i).text == "export")) {
                    ++i; while (peek(i).kind == TokenKind::Newline || peek(i).kind == TokenKind::Semicolon) ++i;
                }
                if ((peek(i).kind == TokenKind::Word || peek(i).kind == TokenKind::Keyword) && peek(i).text == "action") parseMethod = true;
            }
        }
        if (peek().kind == TokenKind::At || peek().kind == TokenKind::LBracket) {
            // Attributes can apply to either methods or fields; decide after peeking the next non-attr token
            // We'll parse attributes inside the branch handlers to avoid consuming them twice.
            // Fall through to unified handling below.
        }

        if (parseMethod || peek().kind == TokenKind::At) {
            // It might still be a field with attributes but no 'action'. To disambiguate,
            // peek past attributes and modifiers here.
            size_t i = 0;
            // Skip attributes
            if (peek().kind == TokenKind::At || peek().kind == TokenKind::LBracket) {
                // simulate parse_attributes look-ahead
                while (peek(i).kind == TokenKind::At || peek(i).kind == TokenKind::LBracket) {
                    if (peek(i).kind == TokenKind::At) {
                        ++i;
                        ++i;
                        if (peek(i).kind == TokenKind::LParen) { ++i; while (peek(i).kind != TokenKind::RParen && peek(i).kind != TokenKind::End) ++i; if (peek(i).kind == TokenKind::RParen) ++i; }
                    } else {
                        ++i;
                        ++i;
                        if (peek(i).kind == TokenKind::LParen) { ++i; while (peek(i).kind != TokenKind::RParen && peek(i).kind != TokenKind::End) ++i; if (peek(i).kind == TokenKind::RParen) ++i; }
                        if (peek(i).kind == TokenKind::RBracket) ++i;
                    }
                    while (peek(i).kind == TokenKind::Newline || peek(i).kind == TokenKind::Semicolon) ++i;
                }
            }
            // Skip modifiers
            while ((peek(i).kind == TokenKind::Word || peek(i).kind == TokenKind::Keyword) &&
                   (peek(i).text == "public" || peek(i).text == "private" || peek(i).text == "export")) {
                ++i; while (peek(i).kind == TokenKind::Newline || peek(i).kind == TokenKind::Semicolon) ++i;
            }
            if ((peek(i).kind == TokenKind::Word || peek(i).kind == TokenKind::Keyword) && peek(i).text == "action") {
                parsingEntityMethod_ = true;
                e.methods.push_back(parse_action());
                parsingEntityMethod_ = false;
                skip_separators();
                continue;
            }
            // Otherwise treat as field
        }

        // Field declaration path: [@attrs] [public|private] [field] name [: type]
        auto fattrs = parse_attributes();
        Visibility fvis = Visibility::Public; bool fHadVis = false;
        if (match_word("public")) { fvis = Visibility::Public; fHadVis = true; }
        else if (match_word("private")) { fvis = Visibility::Private; fHadVis = true; }
        match_word("field");
        const Token& firstTok = consume();
        if (!(firstTok.kind == TokenKind::Word || firstTok.kind == TokenKind::Keyword)) throw std::runtime_error("Field declaration");
        Field field;
        field.visibility = fvis;
        field.attributes = std::move(fattrs);
        if (match(TokenKind::Colon)) {
            field.name = ident_text(firstTok);
            field.type = parse_type_annotation();
        } else if (peek().kind == TokenKind::Word || peek().kind == TokenKind::Keyword) {
            // type-first form: int age
            const Token& secondTok = consume();
            field.type = ident_text(firstTok);
            field.name = ident_text(secondTok);
        } else {
            field.name = ident_text(firstTok);
        }
        if (match(TokenKind::Assign)) {
            field.defaultValue = parse_expression();
        }
        e.fields.push_back(std::move(field));
        match(TokenKind::Semicolon);
        skip_separators();
    }
    expect(TokenKind::RBrace, "}");
    return e;
}

IfStmt Parser::parse_if() {
    if (!match_word("if")) throw std::runtime_error("Expected 'if'");
    ExprPtr cond;
    if (match(TokenKind::LParen)) {
        cond = parse_expression();
        expect(TokenKind::RParen, ")");
    } else {
        cond = parse_expression();
    }
    auto thenBlk = std::make_shared<Block>(parse_block());
    std::shared_ptr<Block> elseBlk;
    skip_separators();
    if (match_word("else")) {
        skip_separators();
        if (is_word_or_kw(peek(), "if")) {
            auto nested = parse_if();
            auto blk = std::make_shared<Block>();
            blk->stmts.push_back(nested);
            elseBlk = blk;
        } else {
            elseBlk = std::make_shared<Block>(parse_block());
        }
    }
    return IfStmt{cond, thenBlk, elseBlk};
}

SwitchStmt Parser::parse_switch() {
    if (!match_word("switch")) throw std::runtime_error("Expected 'switch'");
    auto sel = parse_expression();
    expect(TokenKind::LBrace, "{");
    SwitchStmt sw; sw.selector = sel;
    skip_separators();
    while (peek().kind != TokenKind::RBrace && peek().kind != TokenKind::End) {
        if (match_word("case")) {
            const Token& v = consume();
            if (!(v.kind == TokenKind::String || v.kind == TokenKind::Word || v.kind == TokenKind::Number)) throw std::runtime_error("case value");
            Block b = parse_block();
            sw.cases.push_back(SwitchCase{v.text, std::make_shared<Block>(std::move(b))});
        } else if (match_word("default")) {
            sw.defaultBlk = std::make_shared<Block>(parse_block());
        } else {
            skip_separators();
            if (!(peek().kind == TokenKind::RBrace || peek().kind == TokenKind::End)) throw std::runtime_error("Expected 'case' or 'default'");
        }
        skip_separators();
    }
    expect(TokenKind::RBrace, "}");
    return sw;
}

TryCatchStmt Parser::parse_try_catch() {
    if (!match_word("try")) throw std::runtime_error("Expected 'try'");
    auto tryBlk = std::make_shared<Block>(parse_block());
    skip_separators();
    if (!match_word("catch")) throw std::runtime_error("Expected 'catch' after try block");
    std::string catchVar = "error";
    if (match(TokenKind::LParen)) {
        const Token& cv = consume();
        if (!(cv.kind == TokenKind::Word || cv.kind == TokenKind::Keyword)) throw std::runtime_error("Expected catch variable name");
        catchVar = ident_text(cv);
        expect(TokenKind::RParen, ")");
    }
    auto catchBlk = std::make_shared<Block>(parse_block());
    return TryCatchStmt{ tryBlk, catchVar, catchBlk };
}

StructDecl Parser::parse_struct() {
    auto attrs = parse_attributes();
    (void)attrs;
    while (true) {
        if (match_word("export") || match_word("public") || match_word("private")) {
            skip_separators();
            continue;
        }
        break;
    }
    if (!match_word("struct")) throw std::runtime_error("Expected 'struct'");
    const Token& nameTok = consume();
    if (!(nameTok.kind == TokenKind::Word || nameTok.kind == TokenKind::Keyword)) throw std::runtime_error("Struct name");
    StructDecl decl;
    decl.name = qualify_name(ident_text(nameTok));
    expect(TokenKind::LBrace, "{");
    skip_separators();
    while (peek().kind != TokenKind::RBrace && peek().kind != TokenKind::End) {
        const Token& fieldName = consume();
        if (!(fieldName.kind == TokenKind::Word || fieldName.kind == TokenKind::Keyword)) throw std::runtime_error("Struct field name");
        expect(TokenKind::Colon, ":");
        std::string fieldType = parse_type_annotation();
        decl.fields.push_back(StructFieldDecl{ ident_text(fieldName), fieldType });
        match(TokenKind::Semicolon);
        skip_separators();
    }
    expect(TokenKind::RBrace, "}");
    return decl;
}

EnumDecl Parser::parse_enum() {
    auto attrs = parse_attributes();
    (void)attrs;
    while (true) {
        if (match_word("export") || match_word("public") || match_word("private")) {
            skip_separators();
            continue;
        }
        break;
    }
    if (!match_word("enum")) throw std::runtime_error("Expected 'enum'");
    const Token& nameTok = consume();
    if (!(nameTok.kind == TokenKind::Word || nameTok.kind == TokenKind::Keyword)) throw std::runtime_error("Enum name");
    EnumDecl decl;
    decl.name = qualify_name(ident_text(nameTok));
    expect(TokenKind::LBrace, "{");
    skip_separators();
    while (peek().kind != TokenKind::RBrace && peek().kind != TokenKind::End) {
        const Token& member = consume();
        if (!(member.kind == TokenKind::Word || member.kind == TokenKind::Keyword)) throw std::runtime_error("Enum member");
        decl.members.push_back(ident_text(member));
        (void)match(TokenKind::Comma);
        (void)match(TokenKind::Semicolon);
        skip_separators();
    }
    expect(TokenKind::RBrace, "}");
    return decl;
}

TypeAliasDecl Parser::parse_type_alias() {
    auto attrs = parse_attributes();
    (void)attrs;
    while (true) {
        if (match_word("export") || match_word("public") || match_word("private")) {
            skip_separators();
            continue;
        }
        break;
    }
    if (!match_word("type")) throw std::runtime_error("Expected 'type'");
    const Token& nameTok = consume();
    if (!(nameTok.kind == TokenKind::Word || nameTok.kind == TokenKind::Keyword)) throw std::runtime_error("Type alias name");
    expect(TokenKind::Assign, "=");
    std::string targetType = parse_type_annotation();
    (void)match(TokenKind::Semicolon);
    return TypeAliasDecl{ qualify_name(ident_text(nameTok)), targetType };
}

std::string Parser::parse_type_annotation() {
    const Token& first = consume();
    if (!(first.kind == TokenKind::Word || first.kind == TokenKind::Keyword)) throw std::runtime_error("Expected type name");
    std::string out = ident_text(first);
    while (match(TokenKind::Scope)) {
        const Token& seg = consume();
        if (!(seg.kind == TokenKind::Word || seg.kind == TokenKind::Keyword)) throw std::runtime_error("Expected type segment after '::'");
        out += "::" + ident_text(seg);
    }
    if (match(TokenKind::Less)) {
        out += "<";
        int depth = 1;
        while (depth > 0) {
            const Token& tk = consume();
            if (tk.kind == TokenKind::End) throw std::runtime_error("Unterminated generic type annotation");
            if (tk.kind == TokenKind::Less) {
                out += "<";
                ++depth;
                continue;
            }
            if (tk.kind == TokenKind::Greater) {
                --depth;
                out += ">";
                continue;
            }
            if (tk.kind == TokenKind::Comma) {
                out += ", ";
                continue;
            }
            if (tk.kind == TokenKind::Dot || tk.kind == TokenKind::Colon || tk.kind == TokenKind::Question ||
                tk.kind == TokenKind::Scope ||
                tk.kind == TokenKind::Word || tk.kind == TokenKind::Keyword || tk.kind == TokenKind::Number) {
                out += tk.text;
                continue;
            }
            throw std::runtime_error("Invalid token in type annotation");
        }
    }
    while (true) {
        if (match(TokenKind::Star)) {
            out += "*";
            continue;
        }
        if (match(TokenKind::Amp)) {
            out += "&";
            continue;
        }
        break;
    }
    return out;
}

std::vector<Attribute> Parser::parse_attributes() {
    std::vector<Attribute> attrs;
    while (peek().kind == TokenKind::At || peek().kind == TokenKind::LBracket) {
        if (peek().kind == TokenKind::At) {
            consume();
            const Token& an = consume();
            if (!(an.kind == TokenKind::Word || an.kind == TokenKind::Keyword)) throw std::runtime_error("Attribute name");
            Attribute a; a.name = an.text;
            if (match(TokenKind::LParen)) {
                const Token& v = consume();
                if (v.kind == TokenKind::String || v.kind == TokenKind::Word || v.kind == TokenKind::Keyword || v.kind == TokenKind::Number) a.value = v.text; else throw std::runtime_error("Attribute value");
                expect(TokenKind::RParen, ")");
            }
            attrs.push_back(std::move(a));
            skip_separators();
            continue;
        }
        expect(TokenKind::LBracket, "[");
        const Token& an = consume();
        if (!(an.kind == TokenKind::Word || an.kind == TokenKind::Keyword)) throw std::runtime_error("Attribute name");
        Attribute a; a.name = an.text;
        if (match(TokenKind::LParen)) {
            const Token& v = consume();
            if (v.kind == TokenKind::String || v.kind == TokenKind::Word || v.kind == TokenKind::Keyword || v.kind == TokenKind::Number) a.value = v.text; else throw std::runtime_error("Attribute value");
            expect(TokenKind::RParen, ")");
        }
        expect(TokenKind::RBracket, "]");
        attrs.push_back(std::move(a));
        skip_separators();
    }
    return attrs;
}

} // namespace erelang
