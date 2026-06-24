// ast_test.cpp — AST 双向解析一致性校验（专项5）
// 流程: 源码 → AST1 → Formatter → 新源码 → AST2 → 对比 AST1 == AST2

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "formatter/Formatter.h"
#include "ast/ASTNode.h"
#include <iostream>
#include <string>
#include <vector>
#include <memory>

// ─── AST 结构比较（忽略 line/column）────────────────────────────

static bool astEqual(ASTNode* a, ASTNode* b, std::string& diff, const std::string& path = "root");

/// 透明解包单语句 Block：Block([X]) ≡ X
/// Formatter 总是添加花括号，重解析后 body 变为 Block([stmt])，
/// 而原始 AST 可能是裸 stmt。语义上两者等价。
static ASTNode* unwrapSingleBlock(ASTNode* node) {
    if (!node || node->nodeType != NodeType::NODE_BLOCK) return node;
    auto* block = static_cast<Block*>(node);
    if (block->statements.size() == 1) return block->statements[0].get();
    return node;
}

static bool childEqual(ASTNode* a, ASTNode* b, std::string& diff,
                       const std::string& path, const char* childName) {
    a = unwrapSingleBlock(a);
    b = unwrapSingleBlock(b);
    if (!a && !b) return true;
    if (!a || !b) {
        diff = path + "." + childName + ": one null, other not";
        return false;
    }
    return astEqual(a, b, diff, path + "." + childName);
}

static bool astEqual(ASTNode* a, ASTNode* b, std::string& diff, const std::string& path) {
    if (!a && !b) return true;
    if (!a || !b) {
        diff = path + ": one null, other not";
        return false;
    }
    if (a->nodeType != b->nodeType) {
        diff = path + ": nodeType mismatch (" + a->nodeName() + " vs " + b->nodeName() + ")";
        return false;
    }

    switch (a->nodeType) {
    case NodeType::NODE_BINARY_OP: {
        auto* x = static_cast<BinaryOp*>(a);
        auto* y = static_cast<BinaryOp*>(b);
        if (x->opType != y->opType) { diff = path + ": BinOpType mismatch"; return false; }
        return childEqual(x->left.get(), y->left.get(), diff, path, "left")
            && childEqual(x->right.get(), y->right.get(), diff, path, "right");
    }
    case NodeType::NODE_UNARY_OP: {
        auto* x = static_cast<UnaryOp*>(a);
        auto* y = static_cast<UnaryOp*>(b);
        if (x->opType != y->opType) { diff = path + ": UnaryOpType mismatch"; return false; }
        return childEqual(x->operand.get(), y->operand.get(), diff, path, "operand");
    }
    case NodeType::NODE_NUMBER_LITERAL: {
        auto* x = static_cast<NumberLiteral*>(a);
        auto* y = static_cast<NumberLiteral*>(b);
        if (!x->value.equals(y->value)) {
            diff = path + ": number value mismatch (" + x->value.toString() + " vs " + y->value.toString() + ")";
            return false;
        }
        return true;
    }
    case NodeType::NODE_STRING_LITERAL: {
        auto* x = static_cast<StringLiteral*>(a);
        auto* y = static_cast<StringLiteral*>(b);
        if (x->value != y->value) { diff = path + ": string value mismatch"; return false; }
        return true;
    }
    case NodeType::NODE_BOOL_LITERAL: {
        auto* x = static_cast<BoolLiteral*>(a);
        auto* y = static_cast<BoolLiteral*>(b);
        if (x->value != y->value) { diff = path + ": bool value mismatch"; return false; }
        return true;
    }
    case NodeType::NODE_NULL_LITERAL:
    case NodeType::NODE_SUPER_EXPR:
        return true;

    case NodeType::NODE_VAR_DECL: {
        auto* x = static_cast<VarDecl*>(a);
        auto* y = static_cast<VarDecl*>(b);
        if (x->name != y->name) { diff = path + ": var name mismatch"; return false; }
        if (x->typeAnnotation != y->typeAnnotation) { diff = path + ": typeAnnotation mismatch"; return false; }
        return childEqual(x->initializer.get(), y->initializer.get(), diff, path, "init");
    }
    case NodeType::NODE_ASSIGNMENT: {
        auto* x = static_cast<Assignment*>(a);
        auto* y = static_cast<Assignment*>(b);
        if (x->name != y->name) { diff = path + ": assign name mismatch"; return false; }
        return childEqual(x->value.get(), y->value.get(), diff, path, "value");
    }
    case NodeType::NODE_VAR_REF: {
        auto* x = static_cast<VarRef*>(a);
        auto* y = static_cast<VarRef*>(b);
        if (x->name != y->name) { diff = path + ": varref name mismatch"; return false; }
        return true;
    }

    case NodeType::NODE_IF_STMT: {
        auto* x = static_cast<IfStmt*>(a);
        auto* y = static_cast<IfStmt*>(b);
        return childEqual(x->condition.get(), y->condition.get(), diff, path, "cond")
            && childEqual(x->thenBranch.get(), y->thenBranch.get(), diff, path, "then")
            && childEqual(x->elseBranch.get(), y->elseBranch.get(), diff, path, "else");
    }
    case NodeType::NODE_WHILE_STMT: {
        auto* x = static_cast<WhileStmt*>(a);
        auto* y = static_cast<WhileStmt*>(b);
        return childEqual(x->condition.get(), y->condition.get(), diff, path, "cond")
            && childEqual(x->body.get(), y->body.get(), diff, path, "body");
    }
    case NodeType::NODE_FOR_STMT: {
        auto* x = static_cast<ForStmt*>(a);
        auto* y = static_cast<ForStmt*>(b);
        return childEqual(x->initializer.get(), y->initializer.get(), diff, path, "init")
            && childEqual(x->condition.get(), y->condition.get(), diff, path, "cond")
            && childEqual(x->update.get(), y->update.get(), diff, path, "update")
            && childEqual(x->body.get(), y->body.get(), diff, path, "body");
    }

    case NodeType::NODE_FUN_DECL: {
        auto* x = static_cast<FunDecl*>(a);
        auto* y = static_cast<FunDecl*>(b);
        if (x->name != y->name) { diff = path + ": fun name mismatch"; return false; }
        if (x->params != y->params) { diff = path + ": fun params mismatch"; return false; }
        if (x->paramTypes != y->paramTypes) { diff = path + ": fun paramTypes mismatch"; return false; }
        if (x->returnType != y->returnType) { diff = path + ": fun returnType mismatch"; return false; }
        return childEqual(x->body.get(), y->body.get(), diff, path, "body");
    }
    case NodeType::NODE_FUN_CALL: {
        auto* x = static_cast<FunCall*>(a);
        auto* y = static_cast<FunCall*>(b);
        if (x->name != y->name) { diff = path + ": funCall name mismatch"; return false; }
        if ((x->callee != nullptr) != (y->callee != nullptr)) { diff = path + ": funCall callee mismatch"; return false; }
        if (x->callee && !childEqual(x->callee.get(), y->callee.get(), diff, path, "callee")) return false;
        if (x->arguments.size() != y->arguments.size()) { diff = path + ": funCall arg count mismatch"; return false; }
        for (size_t i = 0; i < x->arguments.size(); i++) {
            if (!childEqual(x->arguments[i].get(), y->arguments[i].get(), diff, path,
                           ("arg" + std::to_string(i)).c_str())) return false;
        }
        return true;
    }

    case NodeType::NODE_RETURN_STMT: {
        auto* x = static_cast<ReturnStmt*>(a);
        auto* y = static_cast<ReturnStmt*>(b);
        return childEqual(x->value.get(), y->value.get(), diff, path, "value");
    }
    case NodeType::NODE_PRINT_STMT: {
        auto* x = static_cast<PrintStmt*>(a);
        auto* y = static_cast<PrintStmt*>(b);
        if (x->values.size() != y->values.size()) { diff = path + ": print values count mismatch"; return false; }
        for (size_t i = 0; i < x->values.size(); i++) {
            if (!childEqual(x->values[i].get(), y->values[i].get(), diff, path,
                           ("val" + std::to_string(i)).c_str())) return false;
        }
        return true;
    }
    case NodeType::NODE_BLOCK: {
        auto* x = static_cast<Block*>(a);
        auto* y = static_cast<Block*>(b);
        if (x->statements.size() != y->statements.size()) {
            diff = path + ": block stmt count mismatch (" + std::to_string(x->statements.size()) + " vs " + std::to_string(y->statements.size()) + ")";
            return false;
        }
        for (size_t i = 0; i < x->statements.size(); i++) {
            if (!childEqual(x->statements[i].get(), y->statements[i].get(), diff, path,
                           ("stmt" + std::to_string(i)).c_str())) return false;
        }
        return true;
    }

    case NodeType::NODE_ARRAY_LITERAL: {
        auto* x = static_cast<ArrayLiteral*>(a);
        auto* y = static_cast<ArrayLiteral*>(b);
        if (x->elements.size() != y->elements.size()) { diff = path + ": array element count mismatch"; return false; }
        for (size_t i = 0; i < x->elements.size(); i++) {
            if (!childEqual(x->elements[i].get(), y->elements[i].get(), diff, path,
                           ("elem" + std::to_string(i)).c_str())) return false;
        }
        return true;
    }
    case NodeType::NODE_DICT_LITERAL: {
        auto* x = static_cast<DictLiteral*>(a);
        auto* y = static_cast<DictLiteral*>(b);
        if (x->pairs.size() != y->pairs.size()) { diff = path + ": dict pair count mismatch"; return false; }
        for (size_t i = 0; i < x->pairs.size(); i++) {
            if (!childEqual(x->pairs[i].first.get(), y->pairs[i].first.get(), diff, path,
                           ("key" + std::to_string(i)).c_str())) return false;
            if (!childEqual(x->pairs[i].second.get(), y->pairs[i].second.get(), diff, path,
                           ("val" + std::to_string(i)).c_str())) return false;
        }
        return true;
    }
    case NodeType::NODE_INDEX_ACCESS: {
        auto* x = static_cast<IndexAccess*>(a);
        auto* y = static_cast<IndexAccess*>(b);
        return childEqual(x->object.get(), y->object.get(), diff, path, "object")
            && childEqual(x->index.get(), y->index.get(), diff, path, "index");
    }
    case NodeType::NODE_INDEX_ASSIGN: {
        auto* x = static_cast<IndexAssign*>(a);
        auto* y = static_cast<IndexAssign*>(b);
        return childEqual(x->object.get(), y->object.get(), diff, path, "object")
            && childEqual(x->index.get(), y->index.get(), diff, path, "index")
            && childEqual(x->value.get(), y->value.get(), diff, path, "value");
    }

    case NodeType::NODE_CLASS_DECL: {
        auto* x = static_cast<ClassDecl*>(a);
        auto* y = static_cast<ClassDecl*>(b);
        if (x->name != y->name) { diff = path + ": class name mismatch"; return false; }
        if (x->superClassName != y->superClassName) { diff = path + ": superClassName mismatch"; return false; }
        if (x->members.size() != y->members.size()) { diff = path + ": class member count mismatch"; return false; }
        for (size_t i = 0; i < x->members.size(); i++) {
            if (!childEqual(x->members[i].get(), y->members[i].get(), diff, path,
                           ("member" + std::to_string(i)).c_str())) return false;
        }
        return true;
    }
    case NodeType::NODE_MEMBER_ACCESS: {
        auto* x = static_cast<MemberAccess*>(a);
        auto* y = static_cast<MemberAccess*>(b);
        if (x->fieldName != y->fieldName) { diff = path + ": memberAccess fieldName mismatch"; return false; }
        return childEqual(x->object.get(), y->object.get(), diff, path, "object");
    }
    case NodeType::NODE_MEMBER_ASSIGN: {
        auto* x = static_cast<MemberAssign*>(a);
        auto* y = static_cast<MemberAssign*>(b);
        if (x->fieldName != y->fieldName) { diff = path + ": memberAssign fieldName mismatch"; return false; }
        return childEqual(x->object.get(), y->object.get(), diff, path, "object")
            && childEqual(x->value.get(), y->value.get(), diff, path, "value");
    }
    case NodeType::NODE_METHOD_CALL: {
        auto* x = static_cast<MethodCall*>(a);
        auto* y = static_cast<MethodCall*>(b);
        if (x->methodName != y->methodName) { diff = path + ": methodName mismatch"; return false; }
        if (x->arguments.size() != y->arguments.size()) { diff = path + ": methodCall arg count mismatch"; return false; }
        if (!childEqual(x->object.get(), y->object.get(), diff, path, "object")) return false;
        for (size_t i = 0; i < x->arguments.size(); i++) {
            if (!childEqual(x->arguments[i].get(), y->arguments[i].get(), diff, path,
                           ("arg" + std::to_string(i)).c_str())) return false;
        }
        return true;
    }

    default:
        diff = path + ": unknown nodeType " + std::to_string(static_cast<int>(a->nodeType));
        return false;
    }
}

// ─── 基础设施 ────────────────────────────────────────────────────

struct TestResult {
    bool parse1Ok = false;
    bool formatOk = false;
    bool parse2Ok = false;
    bool astMatch = false;
    std::string detail;
};

static TestResult runTest(const std::string& source) {
    TestResult r;

    // Pass 1: source -> tokens1 -> AST1
    Lexer lexer1;
    auto tokens1 = lexer1.scan(source);
    if (lexer1.getDiagnostics().hasErrors()) {
        r.detail = "lex1 error: " + lexer1.getDiagnostics().all()[0].message;
        return r;
    }
    Parser parser1;
    auto ast1 = parser1.parse(tokens1);
    if (!parser1.getErrors().empty()) {
        r.detail = "parse1 error: " + std::string(parser1.getErrors()[0].what());
        return r;
    }
    r.parse1Ok = true;

    // Collect comment tokens for Formatter (Lexer 已将注释分离到 comments())
    const auto& commentTokens = lexer1.comments();

    // Format: AST1 -> formatted source
    Formatter fmt;
    fmt.setComments(commentTokens);
    std::string formatted;
    try {
        formatted = fmt.format(*ast1);
    } catch (const std::exception& e) {
        r.detail = "format error: " + std::string(e.what());
        return r;
    }
    r.formatOk = true;

    // Pass 2: formatted source -> tokens2 -> AST2
    Lexer lexer2;
    auto tokens2 = lexer2.scan(formatted);
    if (lexer2.getDiagnostics().hasErrors()) {
        r.detail = "lex2 error: " + lexer2.getDiagnostics().all()[0].message;
        return r;
    }
    Parser parser2;
    auto ast2 = parser2.parse(tokens2);
    if (!parser2.getErrors().empty()) {
        r.detail = "parse2 error: " + std::string(parser2.getErrors()[0].what());
        return r;
    }
    r.parse2Ok = true;

    // Compare AST1 vs AST2
    std::string diff;
    if (astEqual(ast1.get(), ast2.get(), diff)) {
        r.astMatch = true;
    } else {
        r.detail = "AST diff: " + diff;
    }

    return r;
}

// ─── 测试用例 ────────────────────────────────────────────────────

struct TestCase {
    std::string id;
    std::string name;
    std::string source;
};

static std::vector<TestCase> buildTests() {
    std::vector<TestCase> tests;
    auto T = [&](const std::string& id, const std::string& name, const std::string& src) {
        tests.push_back({id, name, src});
    };

    // --- Literals (4) ---
    T("LIT1", "integer literal",       "var x = 42;");
    T("LIT2", "float literal",         "var x = 3.14;");
    T("LIT3", "string literal",        "var s = \"hello world\";");
    T("LIT4", "bool and null",         "var a = true; var b = null;");

    // --- Binary ops (6) ---
    T("BIN1", "addition",              "var x = 1 + 2;");
    T("BIN2", "subtraction",           "var x = 10 - 3;");
    T("BIN3", "mul-div-mod",           "var x = 4 * 5; var y = 20 / 4; var z = 17 % 5;");
    T("BIN4", "precedence mul+add",    "var x = 1 + 2 * 3;");
    T("BIN5", "parenthesized group",   "var x = (1 + 2) * 3;");
    T("BIN6", "comparison and logic",  "var b = 1 == 2; var c = true and false or true;");

    // --- Unary ops (3) ---
    T("UN1", "unary negate",           "var x = -5;");
    T("UN2", "unary not",              "var b = not true;");
    T("UN3", "nested unary",           "var x = -(-3);");

    // --- Variables & assignment (4) ---
    T("VR1", "var decl no init",       "var x;");
    T("VR2", "var decl with init",     "var x = 10;");
    T("VR3", "assignment",             "var x = 1; x = 2;");
    T("VR4", "var ref in expr",        "var x = 1; var y = x + 2;");

    // --- Branching (5) ---
    T("IF1", "if-else",                "if (false) { print(1); } else { print(2); }");
    T("IF2", "else-if chain",          "var x = 2; if (x == 1) { print(1); } else if (x == 2) { print(2); } else { print(3); }");
    T("IF3", "nested if",              "if (true) { if (false) { print(1); } else { print(2); } }");
    T("IF4", "dangling else",          "if (true) if (false) print(1); else print(2);");
    T("IF5", "complex condition",      "if (1 + 2 > 2 and 3 < 5) { print(1); } else { print(0); }");

    // --- Loops (4) ---
    T("LP1", "while basic",            "var i = 0; while (i < 3) { i = i + 1; }");
    T("LP2", "for with init/cond/upd", "for (var i = 0; i < 5; i = i + 1) { print(i); }");
    T("LP3", "nested for",             "for (var i = 0; i < 2; i = i + 1) { for (var j = 0; j < 2; j = j + 1) { print(i + j); } }");
    T("LP4", "while with return",      "fun f() { var i = 0; while (true) { if (i > 5) { return i; } i = i + 1; } }");

    // --- Functions (5) ---
    T("FN1", "basic fun",              "fun add(a, b) { return a + b; } print(add(1, 2));");
    T("FN2", "recursive fun",          "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); } print(fact(5));");
    T("FN3", "typed params",           "fun add(int a, int b) { return a + b; }");
    T("FN4", "fun as expression",      "fun f() { return 42; } var x = f() + 1;");
    T("FN5", "mutual recursion",       "fun isEven(n) { if (n == 0) { return true; } return isOdd(n - 1); } fun isOdd(n) { if (n == 0) { return false; } return isEven(n - 1); }");

    // --- Arrays (4) ---
    T("AR1", "array literal",          "var a = [1, 2, 3];");
    T("AR2", "array index access",     "var a = [10, 20, 30]; var x = a[1];");
    T("AR3", "array index assign",     "var a = [1, 2, 3]; a[0] = 99;");
    T("AR4", "nested array",           "var a = [[1, 2], [3, 4]];");

    // --- Dicts (3) ---
    T("DC1", "dict literal",           "var d = {\"a\": 1, \"b\": 2};");
    T("DC2", "dict member access",     "var d = {\"x\": 10}; var v = d.x;");
    T("DC3", "dict member assign",     "var d = {\"x\": 1}; d.x = 42;");

    // --- Classes (5) ---
    T("CL1", "basic class with init",  "class Foo { init() { this.x = 1; } }");
    T("CL2", "class inheritance+super","class A { init() { this.a = 1; } } class B : A { init() { super.init(); this.b = 2; } }");
    T("CL3", "class field + method",   "class Calc { var x = 0; add(a) { return this.x + a; } }");
    T("CL4", "class member call",      "class Foo { init() { this.x = 1; } getX() { return this.x; } } var f = Foo(); print(f.getX());");

    // --- Mixed structures (8) ---
    T("MX1", "complex arith expr",     "var x = (1 + 2) * (3 - 4) / 5 % 6;");
    T("MX2", "block scoping",          "{ var x = 1; { var y = 2; print(x + y); } }");
    T("MX3", "print multi values",     "print(1, 2, 3);");
    T("MX4", "return stmt bare",       "fun f() { return; }");
    T("MX5", "array method push",      "var a = [1]; a.push(2); print(a.len());");
    T("MX6", "string methods",         "var s = \"hello\"; print(s.len());");
    T("MX7", "deeply nested blocks",   "{ { { { var x = 1; print(x); } } } }");
    T("MX8", "comparison chain",       "var b = 1 < 2 and 2 < 3 and 3 < 4;");

    // --- Operator associativity & precedence (13) ---
    T("PR1", "left-assoc subtraction", "var x = 10 - 3 - 2;");
    T("PR2", "left-assoc division",    "var x = 24 / 4 / 2;");
    T("PR3", "mul over add",           "var x = 2 + 3 * 4;");
    T("PR4", "paren override",         "var x = (2 + 3) * 4;");
    T("PR5", "nested parens",          "var x = ((1 + 2) * (3 + 4));");
    T("PR6", "unary over binary",      "var x = -1 + 2;");
    T("PR7", "complex nested",         "var x = -(1 + 2) * (3 - -(4));");
    T("PR8", "and-or precedence",      "var b = true or false and true;");
    T("PR9", "cmp with arith",         "var b = 1 + 2 < 4 and 5 > 2 + 1;");
    T("PR10", "deep paren nesting",    "var x = ((1 + (2 * 3)) - (4 / (5 + 1)));");

    return tests;
}

// ─── Main ────────────────────────────────────────────────────────

int main() {
    auto tests = buildTests();
    std::cout << "MiniLang AST Bidirectional Parse Consistency Test\n";
    std::cout << "==================================================\n";
    std::cout << "Test count: " << tests.size() << "\n\n";

    int pass = 0, fail = 0, skip = 0;
    struct FailInfo {
        std::string id, name, detail;
    };
    std::vector<FailInfo> failures;

    for (auto& tc : tests) {
        auto r = runTest(tc.source);

        if (r.astMatch) {
            pass++;
            std::cout << "PASS " << tc.id << " " << tc.name << "\n";
        } else {
            fail++;
            std::string phase;
            if (!r.parse1Ok) phase = "PARSE1_FAIL";
            else if (!r.formatOk) phase = "FORMAT_FAIL";
            else if (!r.parse2Ok) phase = "PARSE2_FAIL";
            else phase = "AST_MISMATCH";

            std::string d = phase;
            if (!r.detail.empty()) d += ": " + r.detail;
            std::cout << "FAIL " << tc.id << " " << tc.name << " -- " << d << "\n";
            failures.push_back({tc.id, tc.name, d});
        }
    }

    // Summary
    int total = pass + fail + skip;
    std::cout << "\n==================================================\n";
    std::cout << "RESULTS: " << pass << " passed, " << fail << " failed, "
              << skip << " skipped, " << total << " total\n";
    std::cout << "==================================================\n";

    if (!failures.empty()) {
        std::cout << "\nFAILURES:\n";
        for (auto& f : failures) {
            std::cout << "  [" << f.id << "] " << f.name << ": " << f.detail << "\n";
        }
    }

    return fail > 0 ? 1 : 0;
}
