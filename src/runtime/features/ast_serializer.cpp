#include "erelang/features/ast_serializer.hpp"
#include <cstring>
#include <stdexcept>

namespace erelang::features {

namespace {

// ─── Binary format helpers ───────────────────────────────────────────────────

constexpr uint8_t TAG_NULL      = 0;
constexpr uint8_t TAG_EXPR      = 1;
constexpr uint8_t TAG_BLOCK     = 2;
constexpr uint8_t TAG_STMT      = 3;
constexpr uint8_t TAG_ACTION    = 4;
constexpr uint8_t TAG_ENTITY    = 5;
constexpr uint8_t TAG_HOOK      = 6;
constexpr uint8_t TAG_GLOBAL    = 7;
constexpr uint8_t TAG_IMPORT    = 8;
constexpr uint8_t TAG_STRUCT    = 9;
constexpr uint8_t TAG_ENUM      = 10;
constexpr uint8_t TAG_TYPEALIAS = 11;

// Expr variant tags
constexpr uint8_t EX_STRING   = 0;
constexpr uint8_t EX_NUMBER   = 1;
constexpr uint8_t EX_BOOL     = 2;
constexpr uint8_t EX_IDENT    = 3;
constexpr uint8_t EX_BINARY   = 4;
constexpr uint8_t EX_UNARY    = 5;
constexpr uint8_t EX_CALL     = 6;
constexpr uint8_t EX_MEMBER   = 7;
constexpr uint8_t EX_INDEX    = 8;
constexpr uint8_t EX_TERNARY  = 9;
constexpr uint8_t EX_NEW      = 10;
constexpr uint8_t EX_LISTLIT  = 11;
constexpr uint8_t EX_DICTLIT  = 12;
constexpr uint8_t EX_LAMBDA   = 13;
constexpr uint8_t EX_NULL     = 14;
constexpr uint8_t EX_POSTFIX  = 15;
constexpr uint8_t EX_PREFIX   = 16;
constexpr uint8_t EX_COMPOUND = 17;

// Stmt variant tags
constexpr uint8_t ST_PRINT    = 0;
constexpr uint8_t ST_ACTION   = 1;
constexpr uint8_t ST_LET      = 2;
constexpr uint8_t ST_RETURN   = 3;
constexpr uint8_t ST_SET      = 4;
constexpr uint8_t ST_METHOD   = 5;
constexpr uint8_t ST_IF       = 6;
constexpr uint8_t ST_SWITCH   = 7;
constexpr uint8_t ST_WHILE    = 8;
constexpr uint8_t ST_FOR      = 9;
constexpr uint8_t ST_FORIN    = 10;
constexpr uint8_t ST_BREAK    = 11;
constexpr uint8_t ST_CONTINUE = 12;
constexpr uint8_t ST_REPEAT   = 13;
constexpr uint8_t ST_DOWHILE  = 14;
constexpr uint8_t ST_TRY      = 15;
constexpr uint8_t ST_UNSAFE   = 16;
constexpr uint8_t ST_PTRSET   = 17;
constexpr uint8_t ST_PARALLEL = 18;
constexpr uint8_t ST_SLEEP    = 19;
constexpr uint8_t ST_INPUT    = 20;
constexpr uint8_t ST_FIRE     = 21;
constexpr uint8_t ST_WAITALL  = 22;
constexpr uint8_t ST_PAUSE    = 23;
constexpr uint8_t ST_IMPORT   = 24;
constexpr uint8_t ST_EXPR     = 25;

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}

    void u8(uint8_t v)  { out_.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) out_.push_back(static_cast<uint8_t>(v >> (i * 8))); }
    void i64(int64_t v)  { for (int i = 0; i < 8; ++i) out_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF)); }
    void str(const std::string& s) { u32(static_cast<uint32_t>(s.size())); out_.insert(out_.end(), s.begin(), s.end()); }

    void optStr(const std::optional<std::string>& s) {
        if (s) { u8(1); str(*s); } else { u8(0); }
    }
    void optI64(const std::optional<int64_t>& v) {
        if (v) { u8(1); i64(*v); } else { u8(0); }
    }
private:
    std::vector<uint8_t>& out_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ok() const { return pos_ <= size_; }
    uint8_t u8()  { return pos_ < size_ ? data_[pos_++] : 0; }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4 && pos_ < size_; ++i) v |= static_cast<uint32_t>(data_[pos_++]) << (i * 8);
        return v;
    }
    int64_t i64() {
        int64_t v = 0;
        for (int i = 0; i < 8 && pos_ < size_; ++i) v |= static_cast<int64_t>(data_[pos_++]) << (i * 8);
        return v;
    }
    std::string str() {
        uint32_t len = u32();
        if (pos_ + len > size_) { pos_ = size_ + 1; return {}; }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return s;
    }
    bool optBool() { return u8() != 0; }
    std::optional<std::string> optStr() {
        if (u8()) return str();
        return std::nullopt;
    }
    std::optional<int64_t> optI64() {
        if (u8()) return i64();
        return std::nullopt;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

// ─── Forward declarations ────────────────────────────────────────────────────

void writeExpr(Writer& w, const Expr* e);
ExprPtr readExpr(Reader& r);
void writeBlock(Writer& w, const Block* b);
std::shared_ptr<Block> readBlock(Reader& r);
void writeStmt(Writer& w, const Statement& s);
Statement readStmt(Reader& r);

// ─── Expr ────────────────────────────────────────────────────────────────────

void writeExpr(Writer& w, const Expr* e) {
    if (!e) { w.u8(EX_NULL); return; }
    std::visit([&](auto&& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, ExprString>)   { w.u8(EX_STRING); w.str(v.v); }
        else if constexpr (std::is_same_v<T, ExprNull>) { w.u8(EX_NULL); }
        else if constexpr (std::is_same_v<T, ExprNumber>) { w.u8(EX_NUMBER); w.i64(v.v); w.u8(v.isFloatLiteral ? 1 : 0); w.str(v.raw); }
        else if constexpr (std::is_same_v<T, ExprBool>)   { w.u8(EX_BOOL); w.u8(v.v ? 1 : 0); }
        else if constexpr (std::is_same_v<T, ExprIdent>)  { w.u8(EX_IDENT); w.str(v.name); }
        else if constexpr (std::is_same_v<T, BinaryExpr>) { w.u8(EX_BINARY); w.u8(static_cast<uint8_t>(v.op)); writeExpr(w, v.left.get()); writeExpr(w, v.right.get()); }
        else if constexpr (std::is_same_v<T, UnaryExpr>)  { w.u8(EX_UNARY); w.u8(static_cast<uint8_t>(v.op)); writeExpr(w, v.expr.get()); }
        else if constexpr (std::is_same_v<T, FunctionCallExpr>) {
            w.u8(EX_CALL); w.str(v.name); w.u32(static_cast<uint32_t>(v.args.size()));
            for (auto& a : v.args) writeExpr(w, a.get());
        }
        else if constexpr (std::is_same_v<T, MemberExpr>) { w.u8(EX_MEMBER); w.str(v.objectName); w.str(v.field); }
        else if constexpr (std::is_same_v<T, IndexExpr>)  { w.u8(EX_INDEX); writeExpr(w, v.object.get()); writeExpr(w, v.index.get()); }
        else if constexpr (std::is_same_v<T, TernaryExpr>) { w.u8(EX_TERNARY); writeExpr(w, v.cond.get()); writeExpr(w, v.thenExpr.get()); writeExpr(w, v.elseExpr.get()); }
        else if constexpr (std::is_same_v<T, NewExpr>) {
            w.u8(EX_NEW); w.str(v.typeName); w.u32(static_cast<uint32_t>(v.args.size()));
            for (auto& a : v.args) writeExpr(w, a.get());
        }
        else if constexpr (std::is_same_v<T, ListLiteralExpr>) {
            w.u8(EX_LISTLIT); w.u32(static_cast<uint32_t>(v.elements.size()));
            for (auto& a : v.elements) writeExpr(w, a.get());
        }
        else if constexpr (std::is_same_v<T, DictLiteralExpr>) {
            w.u8(EX_DICTLIT); w.u32(static_cast<uint32_t>(v.entries.size()));
            for (auto& a : v.entries) writeExpr(w, a.get());
        }
        else if constexpr (std::is_same_v<T, LambdaExpr>) {
            w.u8(EX_LAMBDA);
            w.u32(static_cast<uint32_t>(v.params.size()));
            for (auto& p : v.params) { w.str(p.name); w.str(p.type); }
            w.str(v.returnType);
            w.u8(v.isArrow ? 1 : 0);
            if (v.isArrow && v.body) writeExpr(w, v.body.get());
            else {
                w.u32(static_cast<uint32_t>(v.blockBody.stmts.size()));
                for (auto& s : v.blockBody.stmts) writeStmt(w, s);
            }
            w.u32(static_cast<uint32_t>(v.capturedVars.size()));
            for (auto& cv : v.capturedVars) w.str(cv);
        }
        else if constexpr (std::is_same_v<T, PostfixExpr>) { w.u8(EX_POSTFIX); w.u8(v.isInc ? 1 : 0); writeExpr(w, v.operand.get()); }
        else if constexpr (std::is_same_v<T, PrefixExpr>)  { w.u8(EX_PREFIX); w.u8(v.isInc ? 1 : 0); writeExpr(w, v.operand.get()); }
        else if constexpr (std::is_same_v<T, CompoundAssignExpr>) { w.u8(EX_COMPOUND); w.u8(static_cast<uint8_t>(v.op)); writeExpr(w, v.left.get()); writeExpr(w, v.right.get()); }
    }, e->node);
}

ExprPtr readExpr(Reader& r) {
    uint8_t tag = r.u8();
    switch (tag) {
        case EX_NULL:    return std::make_shared<Expr>(Expr{ExprNull{}});
        case EX_STRING:  return std::make_shared<Expr>(Expr{ExprString{r.str()}});
        case EX_NUMBER:  { auto v = r.i64(); auto fl = r.u8() != 0; auto raw = r.str(); return std::make_shared<Expr>(Expr{ExprNumber{v, fl, raw}}); }
        case EX_BOOL:    return std::make_shared<Expr>(Expr{ExprBool{r.u8() != 0}});
        case EX_IDENT:   return std::make_shared<Expr>(Expr{ExprIdent{r.str()}});
        case EX_BINARY:  { auto op = static_cast<BinOp>(r.u8()); auto l = readExpr(r); auto r2 = readExpr(r); return std::make_shared<Expr>(Expr{BinaryExpr{op, l, r2}}); }
        case EX_UNARY:   { auto op = static_cast<UnOp>(r.u8()); auto e = readExpr(r); return std::make_shared<Expr>(Expr{UnaryExpr{op, e}}); }
        case EX_CALL:    {
            auto name = r.str(); auto n = r.u32(); std::vector<ExprPtr> args; args.reserve(n);
            for (uint32_t i = 0; i < n; ++i) args.push_back(readExpr(r));
            return std::make_shared<Expr>(Expr{FunctionCallExpr{name, std::move(args)}});
        }
        case EX_MEMBER:  { auto on = r.str(); auto f = r.str(); return std::make_shared<Expr>(Expr{MemberExpr{on, f}}); }
        case EX_INDEX:   { auto obj = readExpr(r); auto idx = readExpr(r); return std::make_shared<Expr>(Expr{IndexExpr{obj, idx}}); }
        case EX_TERNARY: { auto c = readExpr(r); auto t = readExpr(r); auto e = readExpr(r); return std::make_shared<Expr>(Expr{TernaryExpr{c, t, e}}); }
        case EX_NEW: {
            auto tn = r.str(); auto n = r.u32(); std::vector<ExprPtr> args; args.reserve(n);
            for (uint32_t i = 0; i < n; ++i) args.push_back(readExpr(r));
            return std::make_shared<Expr>(Expr{NewExpr{tn, std::move(args)}});
        }
        case EX_LISTLIT: {
            auto n = r.u32(); std::vector<ExprPtr> elems; elems.reserve(n);
            for (uint32_t i = 0; i < n; ++i) elems.push_back(readExpr(r));
            return std::make_shared<Expr>(Expr{ListLiteralExpr{std::move(elems)}});
        }
        case EX_DICTLIT: {
            auto n = r.u32(); std::vector<ExprPtr> entries; entries.reserve(n);
            for (uint32_t i = 0; i < n; ++i) entries.push_back(readExpr(r));
            return std::make_shared<Expr>(Expr{DictLiteralExpr{std::move(entries)}});
        }
        case EX_LAMBDA: {
            LambdaExpr lambda;
            auto nParams = r.u32();
            for (uint32_t i = 0; i < nParams; ++i) {
                Param p; p.name = r.str(); p.type = r.str();
                lambda.params.push_back(p);
            }
            lambda.returnType = r.str();
            lambda.isArrow = r.u8() != 0;
            if (lambda.isArrow) {
                lambda.body = readExpr(r);
            } else {
                auto nStmts = r.u32();
                for (uint32_t i = 0; i < nStmts; ++i) lambda.blockBody.stmts.push_back(readStmt(r));
            }
            auto nCaps = r.u32();
            for (uint32_t i = 0; i < nCaps; ++i) lambda.capturedVars.push_back(r.str());
            return std::make_shared<Expr>(Expr{lambda});
        }
        case EX_POSTFIX:  { auto inc = r.u8() != 0; auto op = readExpr(r); return std::make_shared<Expr>(Expr{PostfixExpr{op, inc}}); }
        case EX_PREFIX:   { auto inc = r.u8() != 0; auto op = readExpr(r); return std::make_shared<Expr>(Expr{PrefixExpr{op, inc}}); }
        case EX_COMPOUND: { auto op = static_cast<BinOp>(r.u8()); auto l = readExpr(r); auto rr = readExpr(r); return std::make_shared<Expr>(Expr{CompoundAssignExpr{op, l, rr}}); }
        default: return std::make_shared<Expr>(Expr{ExprNull{}});
    }
}

// ─── Block ───────────────────────────────────────────────────────────────────

void writeBlock(Writer& w, const Block* b) {
    if (!b) { w.u32(0); return; }
    w.u32(static_cast<uint32_t>(b->stmts.size()));
    for (auto& s : b->stmts) writeStmt(w, s);
}

std::shared_ptr<Block> readBlock(Reader& r) {
    auto n = r.u32();
    auto blk = std::make_shared<Block>();
    blk->stmts.reserve(n);
    for (uint32_t i = 0; i < n; ++i) blk->stmts.push_back(readStmt(r));
    return blk;
}

// ─── Statement ───────────────────────────────────────────────────────────────

void writeStmt(Writer& w, const Statement& s) {
    std::visit([&](auto&& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, PrintStmt>) {
            w.u8(ST_PRINT); writeExpr(w, v.value.get());
        }
        else if constexpr (std::is_same_v<T, ActionCallStmt>) {
            w.u8(ST_ACTION); w.str(v.name); w.u32(static_cast<uint32_t>(v.args.size()));
            for (auto& a : v.args) writeExpr(w, a.get());
        }
        else if constexpr (std::is_same_v<T, LetStmt>) {
            w.u8(ST_LET); w.u8(v.isConst ? 1 : 0); w.str(v.name); w.str(v.declaredType); writeExpr(w, v.value.get());
        }
        else if constexpr (std::is_same_v<T, ReturnStmt>) {
            w.u8(ST_RETURN);
            if (v.value) { w.u8(1); writeExpr(w, v.value->get()); } else { w.u8(0); }
        }
        else if constexpr (std::is_same_v<T, SetStmt>) {
            w.u8(ST_SET); w.u8(v.isMember ? 1 : 0); w.str(v.varOrField); w.str(v.objectName); writeExpr(w, v.value.get());
        }
        else if constexpr (std::is_same_v<T, MethodCallStmt>) {
            w.u8(ST_METHOD); w.str(v.objectName); w.str(v.method); w.u32(static_cast<uint32_t>(v.args.size()));
            for (auto& a : v.args) writeExpr(w, a.get());
        }
        else if constexpr (std::is_same_v<T, IfStmt>) {
            w.u8(ST_IF); writeExpr(w, v.cond.get());
            w.u8(v.thenBlk ? 1 : 0); if (v.thenBlk) writeBlock(w, v.thenBlk.get());
            w.u8(v.elseBlk ? 1 : 0); if (v.elseBlk) writeBlock(w, v.elseBlk.get());
        }
        else if constexpr (std::is_same_v<T, SwitchStmt>) {
            w.u8(ST_SWITCH); writeExpr(w, v.selector.get());
            w.u32(static_cast<uint32_t>(v.cases.size()));
            for (auto& c : v.cases) { w.str(c.value); w.u8(c.body ? 1 : 0); if (c.body) writeBlock(w, c.body.get()); }
            w.u8(v.defaultBlk ? 1 : 0); if (v.defaultBlk) writeBlock(w, v.defaultBlk.get());
        }
        else if constexpr (std::is_same_v<T, WhileStmt>) {
            w.u8(ST_WHILE); writeExpr(w, v.cond.get());
            w.u8(v.body ? 1 : 0); if (v.body) writeBlock(w, v.body.get());
        }
        else if constexpr (std::is_same_v<T, ForStmt>) {
            w.u8(ST_FOR);
            w.u8(v.init ? 1 : 0); if (v.init) writeBlock(w, v.init.get());
            w.u8(v.cond && *v.cond ? 1 : 0); if (v.cond && *v.cond) writeExpr(w, v.cond->get());
            w.u8(v.step ? 1 : 0); if (v.step) writeBlock(w, v.step.get());
            w.u8(v.body ? 1 : 0); if (v.body) writeBlock(w, v.body.get());
        }
        else if constexpr (std::is_same_v<T, ForInStmt>) {
            w.u8(ST_FORIN); w.str(v.var); w.str(v.varType); w.u8(v.valueVar ? 1 : 0); if (v.valueVar) w.str(*v.valueVar);
            w.u8(v.usedColon ? 1 : 0); writeExpr(w, v.iterable.get());
            w.u8(v.body ? 1 : 0); if (v.body) writeBlock(w, v.body.get());
        }
        else if constexpr (std::is_same_v<T, BreakStmt>)    { w.u8(ST_BREAK); }
        else if constexpr (std::is_same_v<T, ContinueStmt>)  { w.u8(ST_CONTINUE); }
        else if constexpr (std::is_same_v<T, RepeatStmt>) {
            w.u8(ST_REPEAT); writeExpr(w, v.count.get());
            w.u8(v.body ? 1 : 0); if (v.body) writeBlock(w, v.body.get());
        }
        else if constexpr (std::is_same_v<T, DoWhileStmt>) {
            w.u8(ST_DOWHILE); writeExpr(w, v.cond.get());
            w.u8(v.body ? 1 : 0); if (v.body) writeBlock(w, v.body.get());
        }
        else if constexpr (std::is_same_v<T, TryCatchStmt>) {
            w.u8(ST_TRY); w.str(v.catchVar);
            w.u8(v.tryBlk ? 1 : 0); if (v.tryBlk) writeBlock(w, v.tryBlk.get());
            w.u8(v.catchBlk ? 1 : 0); if (v.catchBlk) writeBlock(w, v.catchBlk.get());
        }
        else if constexpr (std::is_same_v<T, UnsafeStmt>) {
            w.u8(ST_UNSAFE);
            w.u8(v.body ? 1 : 0); if (v.body) writeBlock(w, v.body.get());
        }
        else if constexpr (std::is_same_v<T, PointerSetStmt>) {
            w.u8(ST_PTRSET); writeExpr(w, v.pointer.get()); writeExpr(w, v.value.get());
        }
        else if constexpr (std::is_same_v<T, std::shared_ptr<ParallelStmt>>) {
            w.u8(ST_PARALLEL);
            if (v) writeBlock(w, &v->body); else w.u32(0);
        }
        else if constexpr (std::is_same_v<T, SleepStmt>) {
            w.u8(ST_SLEEP); w.i64(v.ms);
        }
        else if constexpr (std::is_same_v<T, InputStmt>) {
            w.u8(ST_INPUT); w.str(v.name);
        }
        else if constexpr (std::is_same_v<T, FireStmt>) {
            w.u8(ST_FIRE); w.str(v.name);
        }
        else if constexpr (std::is_same_v<T, WaitAllStmt>)   { w.u8(ST_WAITALL); }
        else if constexpr (std::is_same_v<T, PauseStmt>)      { w.u8(ST_PAUSE); }
        else if constexpr (std::is_same_v<T, ImportStmt>)      { w.u8(ST_IMPORT); }
        else if constexpr (std::is_same_v<T, ExprStmt>)        { w.u8(ST_EXPR); writeExpr(w, v.expr.get()); }
    }, s);
}

Statement readStmt(Reader& r) {
    uint8_t tag = r.u8();
    switch (tag) {
        case ST_PRINT: { auto v = readExpr(r); return PrintStmt{v}; }
        case ST_ACTION: {
            auto name = r.str(); auto n = r.u32();
            std::vector<ExprPtr> args; args.reserve(n);
            for (uint32_t i = 0; i < n; ++i) args.push_back(readExpr(r));
            return ActionCallStmt{name, std::move(args)};
        }
        case ST_LET: { auto isC = r.u8() != 0; auto name = r.str(); auto dt = r.str(); auto v = readExpr(r); return LetStmt{isC, name, v, dt}; }
        case ST_RETURN: { if (r.u8()) { auto v = readExpr(r); return ReturnStmt{v}; } return ReturnStmt{}; }
        case ST_SET: { auto isM = r.u8() != 0; auto vf = r.str(); auto on = r.str(); auto val = readExpr(r); return SetStmt{isM, vf, on, val}; }
        case ST_METHOD: {
            auto on = r.str(); auto m = r.str(); auto n = r.u32();
            std::vector<ExprPtr> args; args.reserve(n);
            for (uint32_t i = 0; i < n; ++i) args.push_back(readExpr(r));
            return MethodCallStmt{on, m, std::move(args)};
        }
        case ST_IF: {
            auto c = readExpr(r);
            auto tb = r.u8() ? readBlock(r) : nullptr;
            auto eb = r.u8() ? readBlock(r) : nullptr;
            return IfStmt{c, tb, eb};
        }
        case ST_SWITCH: {
            auto sel = readExpr(r); auto n = r.u32();
            std::vector<SwitchCase> cases; cases.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                auto val = r.str();
                auto body = r.u8() ? readBlock(r) : nullptr;
                cases.push_back({val, body});
            }
            auto def = r.u8() ? readBlock(r) : nullptr;
            return SwitchStmt{sel, std::move(cases), def};
        }
        case ST_WHILE: { auto c = readExpr(r); auto b = r.u8() ? readBlock(r) : nullptr; return WhileStmt{c, b}; }
        case ST_FOR: {
            auto init = r.u8() ? readBlock(r) : nullptr;
            ExprPtr cond; if (r.u8()) cond = readExpr(r);
            auto step = r.u8() ? readBlock(r) : nullptr;
            auto body = r.u8() ? readBlock(r) : nullptr;
            return ForStmt{init, cond ? std::optional<ExprPtr>(cond) : std::nullopt, step, body};
        }
        case ST_FORIN: {
            auto vv = r.str(); auto vt = r.str();
            std::optional<std::string> valVar; if (r.u8()) valVar = r.str();
            auto uc = r.u8() != 0; auto iter = readExpr(r);
            auto body = r.u8() ? readBlock(r) : nullptr;
            return ForInStmt{vv, vt, valVar, uc, iter, body};
        }
        case ST_BREAK:    return BreakStmt{};
        case ST_CONTINUE: return ContinueStmt{};
        case ST_REPEAT: { auto c = readExpr(r); auto b = r.u8() ? readBlock(r) : nullptr; return RepeatStmt{c, b}; }
        case ST_DOWHILE: { auto c = readExpr(r); auto b = r.u8() ? readBlock(r) : nullptr; return DoWhileStmt{b, c}; }
        case ST_TRY: {
            auto cv = r.str();
            auto tb = r.u8() ? readBlock(r) : nullptr;
            auto cb = r.u8() ? readBlock(r) : nullptr;
            return TryCatchStmt{tb, cv, cb};
        }
        case ST_UNSAFE: { auto b = r.u8() ? readBlock(r) : nullptr; return UnsafeStmt{b}; }
        case ST_PTRSET: { auto p = readExpr(r); auto v = readExpr(r); return PointerSetStmt{p, v}; }
        case ST_PARALLEL: { return std::make_shared<ParallelStmt>(ParallelStmt{*readBlock(r)}); }
        case ST_SLEEP:   { return SleepStmt{r.i64()}; }
        case ST_INPUT:   { return InputStmt{r.str()}; }
        case ST_FIRE:    { return FireStmt{r.str()}; }
        case ST_WAITALL: return WaitAllStmt{};
        case ST_PAUSE:   return PauseStmt{};
        case ST_IMPORT:  return ImportStmt{};
        case ST_EXPR:    { auto e = readExpr(r); return ExprStmt{e}; }
        default: return PrintStmt{std::make_shared<Expr>(Expr{ExprString{"<deserialize error>"}})};
    }
}

// ─── Top-level structures ───────────────────────────────────────────────────

void writeParam(Writer& w, const Param& p) { w.str(p.name); w.str(p.type); }
Param readParam(Reader& r) { return {r.str(), r.str()}; }

void writeAttr(Writer& w, const Attribute& a) { w.str(a.name); w.optStr(a.value); }
Attribute readAttr(Reader& r) { Attribute a; a.name = r.str(); a.value = r.optStr(); return a; }

void writeAction(Writer& w, const Action& a) {
    w.str(a.name); w.u32(static_cast<uint32_t>(a.params.size()));
    for (auto& p : a.params) writeParam(w, p);
    writeBlock(w, &a.body);
    w.str(a.returnType); w.u8(static_cast<uint8_t>(a.visibility)); w.u8(a.exported ? 1 : 0);
    w.u32(static_cast<uint32_t>(a.attributes.size()));
    for (auto& at : a.attributes) writeAttr(w, at);
    w.str(a.sourcePath); w.u8(a.isAsync ? 1 : 0);
}
Action readAction(Reader& r) {
    Action a; a.name = r.str();
    auto np = r.u32(); a.params.reserve(np);
    for (uint32_t i = 0; i < np; ++i) a.params.push_back(readParam(r));
    a.body = *readBlock(r);
    a.returnType = r.str(); a.visibility = static_cast<Visibility>(r.u8()); a.exported = r.u8() != 0;
    auto na = r.u32(); a.attributes.reserve(na);
    for (uint32_t i = 0; i < na; ++i) a.attributes.push_back(readAttr(r));
    a.sourcePath = r.str(); a.isAsync = r.u8() != 0;
    return a;
}

void writeEntity(Writer& w, const Entity& e) {
    w.str(e.name); w.str(e.baseType);
    w.u32(static_cast<uint32_t>(e.fields.size()));
    for (auto& f : e.fields) {
        w.str(f.name); w.str(f.type);
        w.u8(f.defaultValue ? 1 : 0); if (f.defaultValue) writeExpr(w, f.defaultValue.get());
        w.u8(static_cast<uint8_t>(f.visibility));
        w.u32(static_cast<uint32_t>(f.attributes.size()));
        for (auto& at : f.attributes) writeAttr(w, at);
    }
    w.u32(static_cast<uint32_t>(e.methods.size()));
    for (auto& m : e.methods) writeAction(w, m);
    w.u8(static_cast<uint8_t>(e.visibility)); w.u8(e.exported ? 1 : 0);
    w.u32(static_cast<uint32_t>(e.attributes.size()));
    for (auto& at : e.attributes) writeAttr(w, at);
    w.str(e.sourcePath);
}
Entity readEntity(Reader& r) {
    Entity e; e.name = r.str(); e.baseType = r.str();
    auto nf = r.u32(); e.fields.reserve(nf);
    for (uint32_t i = 0; i < nf; ++i) {
        Field f; f.name = r.str(); f.type = r.str();
        if (r.u8()) f.defaultValue = readExpr(r);
        f.visibility = static_cast<Visibility>(r.u8());
        auto na = r.u32(); f.attributes.reserve(na);
        for (uint32_t j = 0; j < na; ++j) f.attributes.push_back(readAttr(r));
        e.fields.push_back(std::move(f));
    }
    auto nm = r.u32(); e.methods.reserve(nm);
    for (uint32_t i = 0; i < nm; ++i) e.methods.push_back(readAction(r));
    e.visibility = static_cast<Visibility>(r.u8()); e.exported = r.u8() != 0;
    auto na = r.u32(); e.attributes.reserve(na);
    for (uint32_t i = 0; i < na; ++i) e.attributes.push_back(readAttr(r));
    e.sourcePath = r.str();
    return e;
}

void writeHook(Writer& w, const Hook& h) {
    w.str(h.name); writeBlock(w, &h.body); w.str(h.sourcePath);
    w.u32(static_cast<uint32_t>(h.attributes.size()));
    for (auto& at : h.attributes) writeAttr(w, at);
}
Hook readHook(Reader& r) {
    Hook h; h.name = r.str(); h.body = *readBlock(r); h.sourcePath = r.str();
    auto na = r.u32(); h.attributes.reserve(na);
    for (uint32_t i = 0; i < na; ++i) h.attributes.push_back(readAttr(r));
    return h;
}

void writeGlobal(Writer& w, const GlobalDecl& g) {
    w.str(g.name); w.str(g.typeName);
    w.u8(g.value ? 1 : 0); if (g.value) writeExpr(w, g.value.get());
    w.str(g.sourcePath); w.u8(static_cast<uint8_t>(g.visibility)); w.u8(g.exported ? 1 : 0);
}
GlobalDecl readGlobal(Reader& r) {
    GlobalDecl g; g.name = r.str(); g.typeName = r.str();
    if (r.u8()) g.value = readExpr(r);
    g.sourcePath = r.str(); g.visibility = static_cast<Visibility>(r.u8()); g.exported = r.u8() != 0;
    return g;
}

void writeImport(Writer& w, const ImportDecl& imp) {
    w.str(imp.path); w.u8(imp.alias ? 1 : 0); if (imp.alias) w.str(*imp.alias);
    w.u8(imp.pluginGlob ? 1 : 0);
    w.u32(static_cast<uint32_t>(imp.namedImports.size()));
    for (auto& n : imp.namedImports) w.str(n);
}
ImportDecl readImport(Reader& r) {
    ImportDecl imp; imp.path = r.str(); if (r.u8()) imp.alias = r.str();
    imp.pluginGlob = r.u8() != 0;
    auto nn = r.u32(); imp.namedImports.reserve(nn);
    for (uint32_t i = 0; i < nn; ++i) imp.namedImports.push_back(r.str());
    return imp;
}

void writeStruct(Writer& w, const StructDecl& s) {
    w.str(s.name); w.u32(static_cast<uint32_t>(s.fields.size()));
    for (auto& f : s.fields) { w.str(f.name); w.str(f.type); }
    w.u32(static_cast<uint32_t>(s.methods.size()));
    for (auto& m : s.methods) writeAction(w, m);
}
StructDecl readStruct(Reader& r) {
    StructDecl s; s.name = r.str();
    auto nf = r.u32(); s.fields.reserve(nf);
    for (uint32_t i = 0; i < nf; ++i) s.fields.push_back({r.str(), r.str()});
    auto nm = r.u32(); s.methods.reserve(nm);
    for (uint32_t i = 0; i < nm; ++i) s.methods.push_back(readAction(r));
    return s;
}

void writeEnum(Writer& w, const EnumDecl& e) {
    w.str(e.name); w.u32(static_cast<uint32_t>(e.members.size()));
    for (auto& m : e.members) w.str(m);
}
EnumDecl readEnum(Reader& r) {
    EnumDecl e; e.name = r.str();
    auto n = r.u32(); e.members.reserve(n);
    for (uint32_t i = 0; i < n; ++i) e.members.push_back(r.str());
    return e;
}

void writeTypeAlias(Writer& w, const TypeAliasDecl& t) { w.str(t.name); w.str(t.targetType); }
TypeAliasDecl readTypeAlias(Reader& r) { return {r.str(), r.str()}; }

void writeExtern(Writer& w, const ExternDecl& e) {
    w.str(e.name); w.u32(static_cast<uint32_t>(e.params.size()));
    for (auto& p : e.params) writeParam(w, p);
    w.str(e.returnType);
}
ExternDecl readExtern(Reader& r) {
    ExternDecl e; e.name = r.str();
    auto np = r.u32(); e.params.reserve(np);
    for (uint32_t i = 0; i < np; ++i) e.params.push_back(readParam(r));
    e.returnType = r.str();
    return e;
}

} // namespace

// ─── Public API ──────────────────────────────────────────────────────────────

std::vector<uint8_t> serialize_program(const Program& prog) {
    std::vector<uint8_t> out;
    Writer w(out);

    // Magic + version
    out.insert(out.end(), {'E','R','A','S'}); // "ERAS" = ERelang AST Serialized
    w.u32(1); // format version

    w.u8(prog.strict ? 1 : 0);
    w.u8(prog.debug ? 1 : 0);
    w.optStr(prog.runTarget);

    w.u32(static_cast<uint32_t>(prog.actions.size()));
    for (auto& a : prog.actions) writeAction(w, a);

    w.u32(static_cast<uint32_t>(prog.hooks.size()));
    for (auto& h : prog.hooks) writeHook(w, h);

    w.u32(static_cast<uint32_t>(prog.entities.size()));
    for (auto& e : prog.entities) writeEntity(w, e);

    w.u32(static_cast<uint32_t>(prog.imports.size()));
    for (auto& imp : prog.imports) writeImport(w, imp);

    w.u32(static_cast<uint32_t>(prog.structs.size()));
    for (auto& s : prog.structs) writeStruct(w, s);

    w.u32(static_cast<uint32_t>(prog.enums.size()));
    for (auto& e : prog.enums) writeEnum(w, e);

    w.u32(static_cast<uint32_t>(prog.typeAliases.size()));
    for (auto& t : prog.typeAliases) writeTypeAlias(w, t);

    w.u32(static_cast<uint32_t>(prog.externs.size()));
    for (auto& xt : prog.externs) writeExtern(w, xt);

    w.u32(static_cast<uint32_t>(prog.globals.size()));
    for (auto& g : prog.globals) writeGlobal(w, g);

    w.u32(static_cast<uint32_t>(prog.pluginAliases.size()));
    for (auto& pa : prog.pluginAliases) w.str(pa);

    // Also serialize directives
    w.u32(static_cast<uint32_t>(prog.directives.size()));
    for (auto& d : prog.directives) writeAttr(w, d);

    return out;
}

std::optional<Program> deserialize_program(const uint8_t* data, size_t size) {
    try {
        if (size < 8) return std::nullopt;
        Reader r(data, size);

        // Magic
        if (r.u8() != 'E' || r.u8() != 'R' || r.u8() != 'A' || r.u8() != 'S') return std::nullopt;
        if (r.u32() != 1) return std::nullopt; // version

        Program prog;
        prog.strict = r.u8() != 0;
        prog.debug  = r.u8() != 0;
        prog.runTarget = r.optStr();

        auto na = r.u32(); prog.actions.reserve(na);
        for (uint32_t i = 0; i < na; ++i) prog.actions.push_back(readAction(r));

        auto nh = r.u32(); prog.hooks.reserve(nh);
        for (uint32_t i = 0; i < nh; ++i) prog.hooks.push_back(readHook(r));

        auto ne = r.u32(); prog.entities.reserve(ne);
        for (uint32_t i = 0; i < ne; ++i) prog.entities.push_back(readEntity(r));

        auto ni = r.u32(); prog.imports.reserve(ni);
        for (uint32_t i = 0; i < ni; ++i) prog.imports.push_back(readImport(r));

        auto ns = r.u32(); prog.structs.reserve(ns);
        for (uint32_t i = 0; i < ns; ++i) prog.structs.push_back(readStruct(r));

        auto nen = r.u32(); prog.enums.reserve(nen);
        for (uint32_t i = 0; i < nen; ++i) prog.enums.push_back(readEnum(r));

        auto nt = r.u32(); prog.typeAliases.reserve(nt);
        for (uint32_t i = 0; i < nt; ++i) prog.typeAliases.push_back(readTypeAlias(r));

        auto nxt = r.u32(); prog.externs.reserve(nxt);
        for (uint32_t i = 0; i < nxt; ++i) prog.externs.push_back(readExtern(r));

        auto ng = r.u32(); prog.globals.reserve(ng);
        for (uint32_t i = 0; i < ng; ++i) prog.globals.push_back(readGlobal(r));

        auto npa = r.u32(); prog.pluginAliases.reserve(npa);
        for (uint32_t i = 0; i < npa; ++i) prog.pluginAliases.push_back(r.str());

        auto nd = r.u32(); prog.directives.reserve(nd);
        for (uint32_t i = 0; i < nd; ++i) prog.directives.push_back(readAttr(r));

        if (!r.ok()) return std::nullopt;
        return prog;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace erelang::features
