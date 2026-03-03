#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_set>

namespace erelang {

enum class TokenKind {
    End,
    Newline,
    Word,        // identifier/keyword
    Number,      // digits
    String,      // "..." or '...'
    Char,        // 'a', '\n'
    RawString,   // r"..."
    LBrace,      // {
    RBrace,      // }
    LParen,      // (
    RParen,      // )
    LBracket,    // [
    RBracket,    // ]
    Comma,       // ,
    Semicolon,   // ;
    Colon,       // :
    Plus, Minus, Star, Slash, Percent,
    EqualEqual, BangEqual, Less, LessEqual, Greater, GreaterEqual,
    AmpAmp, PipePipe, Assign, Bang,
    At,
    Dot,
    Duration, // e.g., 2m30s or 5s
    UnitNumber, // e.g., 10kg, 5USD, 9.81m/s^2 (single literal token)
    PlusPlus,       // ++
    MinusMinus,     // --
    PlusAssign,     // +=
    MinusAssign,    // -=
    StarAssign,     // *=
    SlashAssign,    // /=
    PercentAssign,  // %=
    AmpAssign,      // &=
    PipeAssign,     // |=
    Amp,            // &
    Pipe,           // |
    Caret,          // ^
    Tilde,          // ~
    Arrow,          // ->
    Scope,          // ::
    Question,       // ?
    NullCoalesce,   // ??
    FatArrow,       // =>
    StrictEqual,    // ===
    StrictNotEqual, // !==
    Shl,            // <<
    Shr,            // >>
    ShlAssign,      // <<=
    ShrAssign,      // >>=
    Ushr,           // >>>
    Pow,            // **
    PowAssign,      // **=
    DocComment,     // ///... or /** ... */
    Keyword,        // reserved keywords (configurable)
    Bad,            // bad/unrecognized token (for error recovery)
};

struct Token {
    TokenKind kind{};
    std::string text{};
    int line{1};
    int column{1};
    int length{0};
    // For Duration/UnitNumber kinds, carry a normalized unit string
    std::string unit; // optional; empty otherwise
    // For polymorphic identifiers like name#tag, carry tag separately
    std::string tag; // optional; empty otherwise
};

struct LexerOptions {
    bool emitComments{false};
    bool emitDocComments{false};
    bool enableDurations{false};
    bool enableUnits{false};
    bool enablePolyIdentifiers{true}; // allow '#' inside identifiers
    std::unordered_set<std::string> keywords; // leave empty to use defaults
};

class Lexer {
public:
    explicit Lexer(std::string source);
    Lexer(std::string source, LexerOptions options);
    std::vector<Token> lex();

private:
    std::string src_;
    LexerOptions opts_{};
};

} // namespace erelang
