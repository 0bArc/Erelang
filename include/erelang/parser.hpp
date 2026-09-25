#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <variant>
#include <memory>
#include <cstdint>
#include <atomic>
#include "erelang/lexer.hpp"
#include "erelang/value.hpp"

namespace erelang {

enum class BinOp { Add, Sub, Mul, Div, Mod, Pow, EQ, NE, LT, LE, GT, GE, And, Or, Coalesce, BitAnd, BitXor, BitOr, Shl, Shr, StrictEQ, StrictNE };
enum class UnOp { Neg, Not, Deref, AddressOf, BitNot };
enum class Visibility { Public, Private };

struct ExprString { std::string v; };
struct ExprNull {};
struct ExprNumber {
    int64_t v{0};
    bool isFloatLiteral{false};
    std::string raw;
};
struct ExprBool { bool v{false}; };
struct ExprIdent { std::string name; };

struct Expr; using ExprPtr = std::shared_ptr<Expr>;
struct BinaryExpr { BinOp op; ExprPtr left; ExprPtr right; };
struct RangeExpr { ExprPtr start; ExprPtr end; bool exclusive{false}; }; // a .. b or a ..< b
struct TernaryExpr { ExprPtr cond; ExprPtr thenExpr; ExprPtr elseExpr; };
struct UnaryExpr { UnOp op; ExprPtr expr; };
struct NewExpr { std::string typeName; std::vector<ExprPtr> args; };
struct MemberExpr { std::string objectName; std::string field; };
struct IndexExpr { ExprPtr object; ExprPtr index; };
struct FunctionCallExpr {
    std::string name;
    std::vector<ExprPtr> args;
    std::vector<std::string> typeArgs;
};
struct PostfixExpr { ExprPtr operand; bool isInc{true}; };      // x++ / x--
struct PrefixExpr  { ExprPtr operand; bool isInc{true}; };      // ++x / --x
struct CompoundAssignExpr { BinOp op; ExprPtr left; ExprPtr right; }; // x += 1, x -= 2, ...

struct ListLiteralExpr {
    std::vector<ExprPtr> elements;
};

struct DictLiteralExpr {
    std::vector<ExprPtr> entries; // alternating key-string, value pairs
};

struct TupleLiteralExpr {
    std::vector<ExprPtr> elements;
};

struct TryExpr {
    ExprPtr inner;
};

struct AwaitExpr {
    ExprPtr inner;
};

struct Block; // forward for recursive AST

struct PrintStmt { ExprPtr value; };
struct SleepStmt { int64_t ms{0}; };
struct ActionCallStmt { std::string name; std::vector<ExprPtr> args; std::vector<std::string> typeArgs; };
struct ParallelStmt; // forward
struct WaitAllStmt {};
struct PauseStmt {};
struct InputStmt { std::string name; };
struct FireStmt { std::string name; };

struct Pattern;
using PatternPtr = std::shared_ptr<Pattern>;

struct LetStmt {
    bool isConst{false};
    std::string name;
    ExprPtr value;
    std::string declaredType;
    PatternPtr pattern;
};
struct ReturnStmt { std::optional<ExprPtr> value; };
struct SetStmt { bool isMember{false}; std::string varOrField; std::string objectName; ExprPtr value; };
struct MethodCallStmt { std::string objectName; std::string method; std::vector<ExprPtr> args; };
struct IfStmt { ExprPtr cond; std::shared_ptr<Block> thenBlk; std::shared_ptr<Block> elseBlk; };
struct SwitchCase { std::string value; std::shared_ptr<Block> body; };
struct SwitchStmt { ExprPtr selector; std::vector<SwitchCase> cases; std::shared_ptr<Block> defaultBlk; };

struct PatWildcard {};
struct PatBinding { std::string name; };
struct PatCtor {
    std::string name;
    std::vector<PatternPtr> args;
};
struct PatField {
    std::string field;
    PatternPtr pattern;
};
struct PatStruct {
    std::vector<PatField> fields;
};
struct PatArray {
    std::vector<PatternPtr> elements;
};
struct PatTuple {
    std::vector<PatternPtr> elements;
};
struct PatOr {
    std::vector<PatternPtr> alts;
};
struct Pattern {
    std::variant<PatWildcard, PatBinding, PatCtor, PatStruct, PatArray, PatTuple, PatOr> node;
};

struct MatchCase {
    PatternPtr pattern;
    std::shared_ptr<Block> body;
    ExprPtr guard; // optional: case Pat if cond:
};
struct MatchStmt { ExprPtr selector; std::vector<MatchCase> cases; };
struct WhileStmt { ExprPtr cond; std::shared_ptr<Block> body; };
struct BreakStmt {};
struct ContinueStmt {};
struct DoWhileStmt { std::shared_ptr<Block> body; ExprPtr cond; };
struct RepeatStmt { ExprPtr count; std::shared_ptr<Block> body; };
struct ForStmt {
    std::shared_ptr<Block> init; // optional single-statement block
    std::optional<ExprPtr> cond;
    std::shared_ptr<Block> step; // optional single-statement block
    std::shared_ptr<Block> body;
};
struct ForInStmt { std::string var; std::string varType; std::optional<std::string> valueVar; bool usedColon{false}; ExprPtr iterable; std::shared_ptr<Block> body; };
struct TryCatchStmt { std::shared_ptr<Block> tryBlk; std::string catchVar; std::shared_ptr<Block> catchBlk; };
struct UnsafeStmt { std::shared_ptr<Block> body; };
struct PointerSetStmt { ExprPtr pointer; ExprPtr value; };
struct IndexSetStmt { ExprPtr object; ExprPtr index; ExprPtr value; };
struct ExprStmt { ExprPtr expr; };
struct ImportStmt {};

using Statement = std::variant<PrintStmt, SleepStmt, ActionCallStmt, std::shared_ptr<ParallelStmt>, WaitAllStmt, PauseStmt, InputStmt, FireStmt, LetStmt, ReturnStmt, SetStmt, MethodCallStmt, IfStmt, SwitchStmt, MatchStmt, WhileStmt, DoWhileStmt, RepeatStmt, ForStmt, ForInStmt, TryCatchStmt, UnsafeStmt, PointerSetStmt, IndexSetStmt, ExprStmt, BreakStmt, ContinueStmt, ImportStmt>;

struct Block {
    std::vector<Statement> stmts;
    std::vector<int> lines; // 1-based, parallel to stmts; empty = unknown
};

struct ParallelStmt { Block body; };

struct Param { std::string name; std::string type; };
struct Attribute { std::string name; std::optional<std::string> value; };

struct TypeRef {
    std::string name;
    std::vector<TypeRef> args;
    std::string canonical;
};

struct TypeParam {
    std::string name;
    std::vector<TypeRef> constraints;
};

struct EnumVariant {
    std::string name;
    std::vector<TypeRef> payloads;
};

[[nodiscard]] inline std::string type_ref_canonical(const TypeRef& t) {
    if (!t.canonical.empty()) return t.canonical;
    if (t.args.empty()) return t.name;
    std::string out = t.name + "<";
    for (size_t i = 0; i < t.args.size(); ++i) {
        if (i) out += ", ";
        out += type_ref_canonical(t.args[i]);
    }
    out += ">";
    return out;
}

[[nodiscard]] inline TypeRef make_type_ref(std::string name, std::vector<TypeRef> args = {}) {
    TypeRef t;
    t.name = std::move(name);
    t.args = std::move(args);
    t.canonical = type_ref_canonical(t);
    return t;
}

[[nodiscard]] TypeRef parse_type_ref_string(std::string_view text);
[[nodiscard]] std::string substitute_type_string(const std::string& typeText,
                                                 const std::unordered_map<std::string, std::string>& subst);

// Lambda expression: lambda(x: int, y: int) -> expr  OR  lambda(x: int) { block }
struct LambdaExpr {
    std::vector<Param> params;
    std::string returnType;   // explicit annotation, or inferred
    ExprPtr body;             // arrow form: single expression body
    Block blockBody;          // block form: statement body (mutually exclusive with body)
    bool isArrow{false};      // true = arrow, false = block
    std::vector<std::string> capturedVars; // free variable names from enclosing scope
};

// Now all expression structs are complete — define Expr
struct Expr {
    std::variant<ExprString, ExprNull, ExprNumber, ExprBool, ExprIdent, BinaryExpr, RangeExpr, TernaryExpr, UnaryExpr, NewExpr, MemberExpr, IndexExpr, FunctionCallExpr, LambdaExpr, ListLiteralExpr, DictLiteralExpr, TupleLiteralExpr, TryExpr, AwaitExpr, PostfixExpr, PrefixExpr, CompoundAssignExpr> node;
};

struct Action {
    std::string name;
    std::vector<TypeParam> typeParams;
    std::vector<Param> params;
    Block body;
    std::string returnType; // e.g., void, int, etc.
    Visibility visibility{Visibility::Public};
    bool exported{false};
    std::vector<Attribute> attributes;
    std::string sourcePath; // file where declared
    bool isAsync{false};
};

// Function type for type checking: Func<ParamType..., ReturnType>
struct FuncType {
    std::vector<std::string> paramTypes;
    std::string returnType;
};

// Captured variable in a closure (shared cell = mutable capture of outer local)
struct ClosedVar {
    std::string name;
    std::shared_ptr<Value> cell;
    std::string type;
};

// Runtime closure data (reference-counted, stored in g_closures)
struct ClosureData {
    Action body;
    std::vector<ClosedVar> captured;
    std::atomic<int> refCount{1};
    std::string sourcePath;
    int sourceLine{0};

    void addRef() { ++refCount; }
    void release() { if (--refCount == 0) delete this; }
};

struct Hook { std::string name; Block body; std::string sourcePath; std::vector<Attribute> attributes; };

struct Field {
    std::string name;
    std::string type;
    ExprPtr defaultValue;
    Visibility visibility{Visibility::Public};
    std::vector<Attribute> attributes;
};
struct Entity {
    std::string name;
    std::vector<TypeParam> typeParams;
    std::string baseType;
    std::vector<Field> fields;
    std::vector<Action> methods; // actions within entity
    Visibility visibility{Visibility::Public};
    bool exported{false};
    std::vector<Attribute> attributes;
    std::string sourcePath;
};

struct GlobalDecl {
    std::string name;
    std::string typeName; // optional type annotation (string, int, bool, ...)
    ExprPtr value;
    std::string sourcePath;
    Visibility visibility{Visibility::Public};
    bool exported{false};
};

struct ImportDecl {
    std::string path;
    std::optional<std::string> alias;
    bool pluginGlob{false};
    std::vector<std::string> namedImports; // import {a,b,c} from "path" → expands to individual imports
};

struct ExternDecl {
    std::string name;
    std::vector<Param> params;
    std::string returnType;
};

struct StructFieldDecl { std::string name; std::string type; };
struct StructDecl {
    std::string name;
    std::vector<TypeParam> typeParams;
    std::vector<StructFieldDecl> fields;
    std::vector<Action> methods;
};
struct EnumDecl {
    std::string name;
    std::vector<TypeParam> typeParams;
    std::vector<EnumVariant> variants;
    std::vector<Action> methods;
};
struct TypeAliasDecl {
    std::string name;
    std::vector<TypeParam> typeParams;
    std::string targetType;
};

struct TraitMethodSig {
    std::string name;
    std::vector<Param> params;
    std::string returnType;
};
struct TraitDecl {
    std::string name;
    std::vector<TypeParam> typeParams;
    std::vector<TraitMethodSig> methods;
    std::vector<std::string> associatedTypes;
};

struct Program {
    std::vector<Action> actions;
    std::vector<Hook> hooks;
    std::vector<Entity> entities;
    std::vector<ImportDecl> imports; // module paths
    std::vector<ExternDecl> externs;
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<TypeAliasDecl> typeAliases;
    std::vector<TraitDecl> traits;
    std::vector<std::string> pluginAliases; // aliases referencing /plugins/* glob
    std::vector<GlobalDecl> globals;
    std::vector<Attribute> directives; // file-level @directives
    bool strict{false};
    bool debug{false};
    std::optional<std::string> runTarget; // e.g., main
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    Parser(std::vector<Token> tokens, std::string sourceName);
    Program parse();

private:
    std::string qualify_name(const std::string& name) const;
    const Token& peek(size_t offset = 0) const;
    const Token& consume();
    bool match(TokenKind kind);
    bool match_word(std::string_view);
    void expect(TokenKind kind, std::string_view what);
    void skip_separators(); // newlines/semicolons

    Program parse_program();
    Action parse_action();
    Hook parse_hook();
    Entity parse_entity();
    GlobalDecl parse_global();
    ExternDecl parse_extern_decl();
    Block parse_block(bool allowImplicit = false);
    Statement parse_statement();
    WhileStmt parse_while();
    BreakStmt parse_break();
    ContinueStmt parse_continue();
    DoWhileStmt parse_do_while();
    ForStmt parse_for();
    ForInStmt parse_for_in_after_lparen();
    TryCatchStmt parse_try_catch();
    IfStmt parse_if();
    SwitchStmt parse_switch();
    MatchStmt parse_match();
    PatternPtr parse_pattern(bool topLevel);
    PatternPtr parse_pattern_atom(bool topLevel);
    StructDecl parse_struct();
    EnumDecl parse_enum();
    TypeAliasDecl parse_type_alias();
    TraitDecl parse_trait();
    std::vector<Attribute> parse_attributes();
    std::string parse_type_annotation();
    TypeRef parse_type_ref();
    std::vector<TypeParam> parse_type_param_list();
    std::vector<std::string> try_parse_call_type_args();
    std::string parse_type_annotation_angles();
    ImportDecl parse_import_decl();
    ExprPtr parse_expression();
    ExprPtr parse_ternary();
    ExprPtr parse_coalesce();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_bitor();
    ExprPtr parse_bitxor();
    ExprPtr parse_bitand();
    ExprPtr parse_equality();
    ExprPtr parse_relational();
    ExprPtr parse_range();
    ExprPtr parse_shift();
    ExprPtr parse_additive();
    ExprPtr parse_multiplicative();
    ExprPtr parse_unary();
    ExprPtr parse_primary();

private:
    std::vector<Token> tokens_;
    size_t pos_{0};
    bool strict_{false};
    std::vector<std::string> namespaceStack_;
    bool parsingEntityMethod_{false};
    std::string sourceName_;
    std::vector<ImportDecl>* programImports_{nullptr};
    std::vector<std::string> pendingErrors_; // errors from block recovery, drained by parse_program
    int inTernaryThen_{0}; // >0 while parsing a ternary then-branch: suppresses ':' method sugar
    int parseDepth_{0};    // recursion guard against deeply nested expressions/statements
    bool pendingGreater_{false}; // leftover '>' when '>>' closed an inner generic
};

} // namespace erelang
