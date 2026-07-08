// ast_test.cpp — AST 双向解析一致性校验（专项5）
// 流程: 源码 → AST1 → Formatter → 新源码 → AST2 → 对比 AST1 == AST2

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "formatter/Formatter.h"
#include "ast/ASTNode.h"
#include "interpreter/Value.h"
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

    // 按节点类型分派：先比 nodeType，再比节点自身字段，最后递归比较子节点。
    // 位置信息（line/column）被刻意忽略，仅比较语义结构，因而重格式化后重解析得到的 AST 可判定为等价。
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
        // A1 fix: NumberLiteral 存储标量，用 getValue() 构造 Value 比较
        Value xv = x->getValue();
        Value yv = y->getValue();
        if (!xv.equals(yv)) {
            diff = path + ": number value mismatch (" + xv.toString() + " vs " + yv.toString() + ")";
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
        // 无字段节点：仅 nodeType 相同即视为等价
        return true;

    case NodeType::NODE_VAR_DECL: {
        auto* x = static_cast<VarDecl*>(a);
        auto* y = static_cast<VarDecl*>(b);
        // 比较变量名、可选类型注解与初始化表达式
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
        // 比较函数名、可选 callee（方法式调用对象）以及实参列表
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
        // 比较键值对数量，并逐一比较每个键与值
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
        // 比较类名、父类名与成员列表（字段与方法）
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
        // 比较方法名、参数个数、调用对象与方法实参
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

// 三阶段流水线：源码 → 解析出 AST1 → 格式化 → 重解析出 AST2，最后比较两次 AST 是否语义等价。
// 任一步骤失败（词法/语法/格式化错误）都会记录到 detail 并返回，不会崩溃。
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
    if (parser1.hasErrors()) {
        r.detail = "parse1 error: " + parser1.getDiagnostics().all()[0].message;
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
    if (parser2.hasErrors()) {
        r.detail = "parse2 error: " + parser2.getDiagnostics().all()[0].message;
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

    // ─── 字面量组：验证各类字面量经「解析→格式化→重解析」后值不变 ───
    // 验证: 整数常量在两次 AST 之间数值一致，且格式化不改变字面量文本语义
    T("LIT1", "integer literal",       "var x = 42;");
    // 验证: 浮点字面量精度在双向解析后保持一致
    T("LIT2", "float literal",         "var x = 3.14;");
    // 验证: 字符串字面量内容（含空格）在重解析后逐字符相等
    T("LIT3", "string literal",        "var s = \"hello world\";");
    // 验证: 布尔与空值在 AST 中等价（无字段节点，仅类型相同即判定一致）
    T("LIT4", "bool and null",         "var a = true; var b = null;");

    // ─── 二元运算组：验证运算符、结合性与优先级在重解析后结构不变 ───
    // 验证: 加法节点左右子树在 AST2 中对应相等
    T("BIN1", "addition",              "var x = 1 + 2;");
    // 验证: 减法节点结构被原样保留
    T("BIN2", "subtraction",           "var x = 10 - 3;");
    // 验证: 乘/除/取模三类算术运算在同一语句内都正确还原
    T("BIN3", "mul-div-mod",           "var x = 4 * 5; var y = 20 / 4; var z = 17 % 5;");
    // 验证: 乘法优先级高于加法，生成的 AST 子树层级与源码一致
    T("BIN4", "precedence mul+add",    "var x = 1 + 2 * 3;");
    // 验证: 括号分组在 AST 中体现为独立的嵌套二元节点
    T("BIN5", "parenthesized group",   "var x = (1 + 2) * 3;");
    // 验证: 比较运算与逻辑运算（and/or）混合时结构正确还原
    T("BIN6", "comparison and logic",  "var b = 1 == 2; var c = true and false or true;");

    // ─── 一元运算组 ───
    // 验证: 一元负号节点在两次 AST 中等价
    T("UN1", "unary negate",           "var x = -5;");
    // 验证: 一元 not 逻辑非节点正确还原
    T("UN2", "unary not",              "var b = not true;");
    // 验证: 嵌套一元运算（双重负号）的子树嵌套结构保持一致
    T("UN3", "nested unary",           "var x = -(-3);");

    // ─── 变量与赋值组 ───
    // 验证: 无初始化的变量声明（仅声明节点）可被等价还原
    T("VR1", "var decl no init",       "var x;");
    // 验证: 带初始化表达式的变量声明，初始化子树完整保留
    T("VR2", "var decl with init",     "var x = 10;");
    // 验证: 赋值语句的变量名与右值表达式在重解析后一致
    T("VR3", "assignment",             "var x = 1; x = 2;");
    // 验证: 在表达式中引用已声明变量（VarRef）被正确解析与还原
    T("VR4", "var ref in expr",        "var x = 1; var y = x + 2;");

    // ─── 分支语句组：验证 if/else 各分支子树结构一致 ───
    // 验证: 标准 if-else 的 then/else 分支在 AST2 中等价
    T("IF1", "if-else",                "if (false) { print(1); } else { print(2); }");
    // 验证: else-if 链式结构（多个 else 内嵌 if）被完整还原
    T("IF2", "else-if chain",          "var x = 2; if (x == 1) { print(1); } else if (x == 2) { print(2); } else { print(3); }");
    // 验证: if 内嵌套 if-else 的子树嵌套层级保持一致
    T("IF3", "nested if",              "if (true) { if (false) { print(1); } else { print(2); } }");
    // 验证: 悬空 else（无花括号）的语法糖结构在重解析后不变
    T("IF4", "dangling else",          "if (true) if (false) print(1); else print(2);");
    // 验证: 复合布尔条件（含比较与 and）的 AST 结构正确还原
    T("IF5", "complex condition",      "if (1 + 2 > 2 and 3 < 5) { print(1); } else { print(0); }");

    // ─── 循环语句组 ───
    // 验证: while 循环的条件与循环体子树在 AST2 中等价
    T("LP1", "while basic",            "var i = 0; while (i < 3) { i = i + 1; }");
    // 验证: for 的初始化/条件/更新三段在重解析后完整保留
    T("LP2", "for with init/cond/upd", "for (var i = 0; i < 5; i = i + 1) { print(i); }");
    // 验证: 嵌套 for 循环的多层子树结构正确还原
    T("LP3", "nested for",             "for (var i = 0; i < 2; i = i + 1) { for (var j = 0; j < 2; j = j + 1) { print(i + j); } }");
    // 验证: 循环体内含 return 的函数定义语法可被双向解析保持
    T("LP4", "while with return",      "fun f() { var i = 0; while (true) { if (i > 5) { return i; } i = i + 1; } }");

    // ─── 函数定义组：验证参数、递归、类型注解等价 ───
    // 验证: 普通函数定义与调用语句组合在重解析后结构一致
    T("FN1", "basic fun",              "fun add(a, b) { return a + b; } print(add(1, 2));");
    // 验证: 递归函数（函数内调用自身）的 AST 可正确还原
    T("FN2", "recursive fun",          "fun fact(n) { if (n <= 1) { return 1; } return n * fact(n - 1); } print(fact(5));");
    // 验证: 带类型注解的参数（int a, int b）在 AST2 中保持
    T("FN3", "typed params",           "fun add(int a, int b) { return a + b; }");
    // 验证: 函数调用作为表达式参与运算时结构被正确解析
    T("FN4", "fun as expression",      "fun f() { return 42; } var x = f() + 1;");
    // 验证: 互递归（两个函数互相调用）的多函数定义结构完整还原
    T("FN5", "mutual recursion",       "fun isEven(n) { if (n == 0) { return true; } return isOdd(n - 1); } fun isOdd(n) { if (n == 0) { return false; } return isEven(n - 1); }");

    // ─── 数组组 ───
    // 验证: 数组字面量的元素列表在重解析后一一对应
    T("AR1", "array literal",          "var a = [1, 2, 3];");
    // 验证: 数组下标访问（IndexAccess）的 object 与 index 子树一致
    T("AR2", "array index access",     "var a = [10, 20, 30]; var x = a[1];");
    // 验证: 数组下标赋值（IndexAssign）在 AST2 中完整保留
    T("AR3", "array index assign",     "var a = [1, 2, 3]; a[0] = 99;");
    // 验证: 嵌套数组（数组元素仍为数组）的多层结构正确还原
    T("AR4", "nested array",           "var a = [[1, 2], [3, 4]];");

    // ─── 字典组：验证键值对与成员式访问的等价性 ───
    // 验证: 字典字面量的 key/value 对在双向解析后保持数量与内容一致
    T("DC1", "dict literal",           "var d = {\"a\": 1, \"b\": 2};");
    // 验证: 字典成员式访问（d.x）等价于下标访问，AST 结构被正确还原
    T("DC2", "dict member access",     "var d = {\"x\": 10}; var v = d.x;");
    // 验证: 字典成员赋值（d.x = ...）的 AST 结构等价
    T("DC3", "dict member assign",     "var d = {\"x\": 1}; d.x = 42;");

    // ─── 类与继承组 ───
    // 验证: 带 init 构造函数的类声明在重解析后字段定义保持
    T("CL1", "basic class with init",  "class Foo { init() { this.x = 1; } }");
    // 验证: 类继承 + super.init() 调用（MethodCall/成员调用）结构完整还原
    T("CL2", "class inheritance+super","class A { init() { this.a = 1; } } class B : A { init() { super.init(); this.b = 2; } }");
    // 验证: 类字段与方法的混合定义在 AST2 中正确还原
    T("CL3", "class field + method",   "class Calc { var x = 0; add(a) { return this.x + a; } }");
    // 验证: 类实例化（Foo()）与成员方法调用组合的结构一致性
    T("CL4", "class member call",      "class Foo { init() { this.x = 1; } getX() { return this.x; } } var f = Foo(); print(f.getX());");

    // ─── 混合结构组：验证多种构造复合时的整体等价性 ───
    // 验证: 复杂算术表达式（含括号/乘除取模混合）的子树层级正确
    T("MX1", "complex arith expr",     "var x = (1 + 2) * (3 - 4) / 5 % 6;");
    // 验证: 嵌套代码块（Block 内再嵌 Block）的语句列表结构一致
    T("MX2", "block scoping",          "{ var x = 1; { var y = 2; print(x + y); } }");
    // 验证: print 多实参（PrintStmt 的 values 列表）数量与顺序保持
    T("MX3", "print multi values",     "print(1, 2, 3);");
    // 验证: 无返回值的裸 return 语句（value 为空）被正确还原
    T("MX4", "return stmt bare",       "fun f() { return; }");
    // 验证: 数组方法调用（a.push(2)）与成员方法（a.len()）的组合结构
    T("MX5", "array method push",      "var a = [1]; a.push(2); print(a.len());");
    // 验证: 字符串方法调用（s.len()）在 AST2 中等价
    T("MX6", "string methods",         "var s = \"hello\"; print(s.len());");
    // 验证: 深层嵌套代码块（四层 Block）的语句结构完整还原
    T("MX7", "deeply nested blocks",   "{ { { { var x = 1; print(x); } } } }");
    // 验证: 连续比较链（and 连接的多个比较）的 AST 结构一致
    T("MX8", "comparison chain",       "var b = 1 < 2 and 2 < 3 and 3 < 4;");

    // ─── 运算符结合性与优先级专项：验证格式化不改变语义结构 ───
    // 验证: 减法左结合（(10-3)-2）在重解析后子树结合方向不变
    T("PR1", "left-assoc subtraction", "var x = 10 - 3 - 2;");
    // 验证: 除法左结合（(24/4)/2）的结合方向一致
    T("PR2", "left-assoc division",    "var x = 24 / 4 / 2;");
    // 验证: 乘法优先级高于加法，生成的 AST 层级与源码一致
    T("PR3", "mul over add",           "var x = 2 + 3 * 4;");
    // 验证: 括号强制改变优先级时，结构以括号分组为准还原
    T("PR4", "paren override",         "var x = (2 + 3) * 4;");
    // 验证: 多层嵌套括号的子树嵌套层级正确保持
    T("PR5", "nested parens",          "var x = ((1 + 2) * (3 + 4));");
    // 验证: 一元负号优先级高于二元加（-(1)+2）的结构正确还原
    T("PR6", "unary over binary",      "var x = -1 + 2;");
    // 验证: 一元与括号混合的复杂表达式子树层级一致
    T("PR7", "complex nested",         "var x = -(1 + 2) * (3 - -(4));");
    // 验证: and 优先级高于 or（true or (false and true)）结构保持
    T("PR8", "and-or precedence",      "var b = true or false and true;");
    // 验证: 比较运算与算术混合时的优先级结构正确还原
    T("PR9", "cmp with arith",         "var b = 1 + 2 < 4 and 5 > 2 + 1;");
    // 验证: 深度嵌套括号组合（含多层子表达式）的 AST 完全一致
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

    // 逐个运行测试用例：按失败发生的阶段（PARSE1 / FORMAT / PARSE2 / AST 不一致）归类，便于定位问题
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
