#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <variant>
#include <memory>
#include <cstdint>
#include "erelang/lexer.hpp"

namespace erelang {

enum class BinOp { Add, Sub, Mul, Div, Mod, Pow, EQ, NE, LT, LE, GT, GE, And, Or, Coalesce };
enum class UnOp { Neg, Not };
enum class Visibility { Public, Private };

struct ExprString { std::string v; };
struct ExprNumber { int64_t v{0}; };
struct ExprBool { bool v{false}; };
struct ExprIdent { std::string name; };

struct Expr; using ExprPtr = std::shared_ptr<Expr>;
struct BinaryExpr { BinOp op; ExprPtr left; ExprPtr right; };
struct UnaryExpr { UnOp op; ExprPtr expr; };
struct NewExpr { std::string typeName; std::vector<ExprPtr> args; };
struct MemberExpr { std::string objectName; std::string field; };
struct FunctionCallExpr { std::string name; std::vector<ExprPtr> args; };

struct Expr {
    std::variant<ExprString, ExprNumber, ExprBool, ExprIdent, BinaryExpr, UnaryExpr, NewExpr, MemberExpr, FunctionCallExpr> node;
};

struct Block; // forward for recursive AST

struct PrintStmt { ExprPtr value; };
struct SleepStmt { int64_t ms{0}; };
struct ActionCallStmt { std::string name; std::vector<ExprPtr> args; };
struct ParallelStmt; // forward
struct WaitAllStmt {};
struct PauseStmt {};
struct InputStmt { std::string name; };
struct FireStmt { std::string name; };
struct LetStmt { bool isConst{false}; std::string name; ExprPtr value; std::string declaredType; };
struct ReturnStmt { std::optional<ExprPtr> value; };
struct SetStmt { bool isMember{false}; std::string varOrField; std::string objectName; ExprPtr value; };
struct MethodCallStmt { std::string objectName; std::string method; std::vector<ExprPtr> args; };
struct IfStmt { ExprPtr cond; std::shared_ptr<Block> thenBlk; std::shared_ptr<Block> elseBlk; };
struct SwitchCase { std::string value; std::shared_ptr<Block> body; };
struct SwitchStmt { ExprPtr selector; std::vector<SwitchCase> cases; std::shared_ptr<Block> defaultBlk; };
struct WhileStmt { ExprPtr cond; std::shared_ptr<Block> body; };
struct ForStmt {
    std::shared_ptr<Block> init; // optional single-statement block
    std::optional<ExprPtr> cond;
    std::shared_ptr<Block> step; // optional single-statement block
    std::shared_ptr<Block> body;
};
struct ForInStmt { std::string var; std::optional<std::string> valueVar; bool usedColon{false}; ExprPtr iterable; std::shared_ptr<Block> body; };
struct TryCatchStmt { std::shared_ptr<Block> tryBlk; std::string catchVar; std::shared_ptr<Block> catchBlk; };

using Statement = std::variant<PrintStmt, SleepStmt, ActionCallStmt, std::shared_ptr<ParallelStmt>, WaitAllStmt, PauseStmt, InputStmt, FireStmt, LetStmt, ReturnStmt, SetStmt, MethodCallStmt, IfStmt, SwitchStmt, WhileStmt, ForStmt, ForInStmt, TryCatchStmt>;

struct Block { std::vector<Statement> stmts; };

struct ParallelStmt { Block body; };

struct Param { std::string name; std::string type; };
struct Attribute { std::string name; std::optional<std::string> value; };

struct Action {
    std::string name;
    std::vector<Param> params;
    Block body;
    std::string returnType; // e.g., void, int, etc.
    Visibility visibility{Visibility::Public};
    bool exported{false};
    std::vector<Attribute> attributes;
    std::string sourcePath; // file where declared
    bool isAsync{false};
};

struct Hook { std::string name; Block body; std::string sourcePath; std::vector<Attribute> attributes; };

struct Field { std::string name; std::string type; Visibility visibility{Visibility::Public}; std::vector<Attribute> attributes; };
struct Entity {
    std::string name;
    std::vector<Field> fields;
    std::vector<Action> methods; // actions within entity
    Visibility visibility{Visibility::Public};
    bool exported{false};
    std::vector<Attribute> attributes;
    std::string sourcePath;
};

struct GlobalDecl { std::string name; ExprPtr value; std::string sourcePath; };

struct ImportDecl {
    std::string path;
    std::optional<std::string> alias;
    bool pluginGlob{false};
};

struct StructFieldDecl { std::string name; std::string type; };
struct StructDecl { std::string name; std::vector<StructFieldDecl> fields; };
struct EnumDecl { std::string name; std::vector<std::string> members; };
struct TypeAliasDecl { std::string name; std::string targetType; };

struct Program {
    std::vector<Action> actions;
    std::vector<Hook> hooks;
    std::vector<Entity> entities;
    std::vector<ImportDecl> imports; // module paths
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<TypeAliasDecl> typeAliases;
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
    Program parse();

private:
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
    Block parse_block();
    Statement parse_statement();
    WhileStmt parse_while();
    ForStmt parse_for();
    ForInStmt parse_for_in_after_lparen();
    TryCatchStmt parse_try_catch();
    IfStmt parse_if();
    SwitchStmt parse_switch();
    StructDecl parse_struct();
    EnumDecl parse_enum();
    TypeAliasDecl parse_type_alias();
    std::vector<Attribute> parse_attributes();
    std::string parse_type_annotation();
    ExprPtr parse_expression();
    ExprPtr parse_coalesce();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_equality();
    ExprPtr parse_relational();
    ExprPtr parse_additive();
    ExprPtr parse_multiplicative();
    ExprPtr parse_unary();
    ExprPtr parse_primary();

private:
    std::vector<Token> tokens_;
    size_t pos_{0};
    bool strict_{false};
};

} // namespace erelang
