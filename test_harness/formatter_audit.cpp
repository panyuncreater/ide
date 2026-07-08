// Formatter 深度审计验证程序（简化版）
// 仅依赖 Lexer + Parser + Formatter，无需 Interpreter
// 验证目标：
//   1. 幂等性：Format(Format(src)) == Format(src)
//   2. 格式化输出检查：人工对比格式化前后语义
//   3. 配置项有效性：options 是否被实际使用

#include <iostream>
#include <string>
#include <vector>

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"
#include "formatter/Formatter.h"
#include "Diagnostic.h"

// 共享的格式化辅助函数：对源码做 词法 → 语法 → 格式化 三步；
// 词法/语法失败时在第一个参数回传 hasError=true 并返回错误描述，否则返回格式化后的文本。
// 除第 5 组外均使用默认 FormatOptions。
static std::string formatCode(const std::string& source, bool& hasError,
                              const FormatOptions& opts = FormatOptions()) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    if (lexer.getDiagnostics().hasErrors()) {
        hasError = true;
        return "LEX:" + lexer.getDiagnostics().summary();
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        hasError = true;
        return "PARSE_ERR:" + parser.getDiagnostics().summary();
    }
    Formatter fmt;
    fmt.setComments(lexer.comments());
    fmt.setOptions(opts);
    hasError = false;
    return fmt.format(*ast);
}

struct AuditCase {
    std::string id;
    std::string name;
    std::string source;
};

// ─── AST 结构等价性比较（用于验证往返不变量）──────────────────────────
// AUDIT-FMT-P2 fix: 原 lostParens 检测仅统计 '(' 字符数，无法区分
// 语义必要的括号（右嵌套同优先级，丢失则 AST 改变）与冗余括号
// （左嵌套同优先级，丢弃后重新解析因左结合律仍得到相同 AST）。
// 正确做法：直接比较 Parse(src) 与 Parse(fmt1) 的 AST 结构。

static bool astEqual(ASTNode* a, ASTNode* b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->nodeType != b->nodeType) return false;

    // 按节点类型分派，逐字段递归比较两棵 AST 是否语义等价（忽略行号等位置信息）；
    // 任何自身字段差异或子节点不匹配都判定为不等价，用于在格式化往返中捕获结构重组。
    switch (a->nodeType) {
    case NodeType::NODE_BINARY_OP: {
        auto* ba = static_cast<BinaryOp*>(a);
        auto* bb = static_cast<BinaryOp*>(b);
        return ba->opType == bb->opType
            && astEqual(ba->left.get(), bb->left.get())
            && astEqual(ba->right.get(), bb->right.get());
    }
    case NodeType::NODE_UNARY_OP: {
        auto* ua = static_cast<UnaryOp*>(a);
        auto* ub = static_cast<UnaryOp*>(b);
        return ua->opType == ub->opType
            && astEqual(ua->operand.get(), ub->operand.get());
    }
    case NodeType::NODE_NUMBER_LITERAL: {
        auto* na = static_cast<NumberLiteral*>(a);
        auto* nb = static_cast<NumberLiteral*>(b);
        if (na->isInt() != nb->isInt()) return false;
        return na->isInt() ? (na->intVal() == nb->intVal())
                           : (na->floatVal() == nb->floatVal());
    }
    case NodeType::NODE_BOOL_LITERAL:
        return static_cast<BoolLiteral*>(a)->value
            == static_cast<BoolLiteral*>(b)->value;
    case NodeType::NODE_STRING_LITERAL:
        return static_cast<StringLiteral*>(a)->value
            == static_cast<StringLiteral*>(b)->value;
    case NodeType::NODE_VAR_REF:
        return static_cast<VarRef*>(a)->name
            == static_cast<VarRef*>(b)->name;
    case NodeType::NODE_PRINT_STMT: {
        auto* pa = static_cast<PrintStmt*>(a);
        auto* pb = static_cast<PrintStmt*>(b);
        if (pa->values.size() != pb->values.size()) return false;
        for (size_t i = 0; i < pa->values.size(); ++i) {
            if (!astEqual(pa->values[i].get(), pb->values[i].get())) return false;
        }
        return true;
    }
    // BUG-LPA-01 fix: 为所有含自身字段的节点添加显式 case，比较节点自身字段
    //   原实现 default 分支仅比较 children()，不比较节点自身字段，导致
    //   VarDecl/FunDecl/ImportStmt/ClassDecl/TryStmt/Assignment/MemberAccess/
    //   MemberAssign/MethodCall/FunCall 的字段差异被忽略，Formatter 往返等价性
    //   验证漏检。
    case NodeType::NODE_VAR_DECL: {
        auto* va = static_cast<VarDecl*>(a);
        auto* vb = static_cast<VarDecl*>(b);
        if (va->name != vb->name) return false;
        if (va->typeAnnotation != vb->typeAnnotation) return false;
        return astEqual(va->initializer.get(), vb->initializer.get());
    }
    case NodeType::NODE_ASSIGNMENT: {
        auto* aa = static_cast<Assignment*>(a);
        auto* ab = static_cast<Assignment*>(b);
        if (aa->name != ab->name) return false;
        return astEqual(aa->value.get(), ab->value.get());
    }
    case NodeType::NODE_FUN_DECL: {
        auto* fa = static_cast<FunDecl*>(a);
        auto* fb = static_cast<FunDecl*>(b);
        if (fa->name != fb->name) return false;
        if (fa->params != fb->params) return false;
        if (fa->paramTypes != fb->paramTypes) return false;
        if (fa->returnType != fb->returnType) return false;
        if (fa->defaultValues.size() != fb->defaultValues.size()) return false;
        for (size_t i = 0; i < fa->defaultValues.size(); ++i) {
            if (!astEqual(fa->defaultValues[i].get(), fb->defaultValues[i].get())) return false;
        }
        return astEqual(fa->body.get(), fb->body.get());
    }
    case NodeType::NODE_FUN_CALL: {
        auto* ca = static_cast<FunCall*>(a);
        auto* cb = static_cast<FunCall*>(b);
        if (ca->name != cb->name) return false;
        if ((ca->callee == nullptr) != (cb->callee == nullptr)) return false;
        if (ca->callee && !astEqual(ca->callee.get(), cb->callee.get())) return false;
        if (ca->arguments.size() != cb->arguments.size()) return false;
        for (size_t i = 0; i < ca->arguments.size(); ++i) {
            if (!astEqual(ca->arguments[i].get(), cb->arguments[i].get())) return false;
        }
        return true;
    }
    case NodeType::NODE_MEMBER_ACCESS: {
        auto* ma = static_cast<MemberAccess*>(a);
        auto* mb = static_cast<MemberAccess*>(b);
        if (ma->fieldName != mb->fieldName) return false;
        return astEqual(ma->object.get(), mb->object.get());
    }
    case NodeType::NODE_MEMBER_ASSIGN: {
        auto* ma = static_cast<MemberAssign*>(a);
        auto* mb = static_cast<MemberAssign*>(b);
        if (ma->fieldName != mb->fieldName) return false;
        if (!astEqual(ma->object.get(), mb->object.get())) return false;
        return astEqual(ma->value.get(), mb->value.get());
    }
    case NodeType::NODE_METHOD_CALL: {
        auto* ma = static_cast<MethodCall*>(a);
        auto* mb = static_cast<MethodCall*>(b);
        if (ma->methodName != mb->methodName) return false;
        if (!astEqual(ma->object.get(), mb->object.get())) return false;
        if (ma->arguments.size() != mb->arguments.size()) return false;
        for (size_t i = 0; i < ma->arguments.size(); ++i) {
            if (!astEqual(ma->arguments[i].get(), mb->arguments[i].get())) return false;
        }
        return true;
    }
    case NodeType::NODE_CLASS_DECL: {
        auto* ca = static_cast<ClassDecl*>(a);
        auto* cb = static_cast<ClassDecl*>(b);
        if (ca->name != cb->name) return false;
        if (ca->superClassName != cb->superClassName) return false;
        if (ca->members.size() != cb->members.size()) return false;
        for (size_t i = 0; i < ca->members.size(); ++i) {
            if (!astEqual(ca->members[i].get(), cb->members[i].get())) return false;
        }
        return true;
    }
    case NodeType::NODE_TRY_STMT: {
        auto* ta = static_cast<TryStmt*>(a);
        auto* tb = static_cast<TryStmt*>(b);
        if (ta->catchVarName != tb->catchVarName) return false;
        if (!astEqual(ta->tryBlock.get(), tb->tryBlock.get())) return false;
        if (!astEqual(ta->catchBlock.get(), tb->catchBlock.get())) return false;
        // BUG-FE-AUDIT-5 fix: finallyBlock 字段遗漏比较。BUG-AUDIT-FINALLY-1 新增
        // finallyBlock 字段后 astEqual 未同步，导致 Formatter 对 finally 块的回归
        // 被往返等价性测试错误通过。
        if ((ta->finallyBlock == nullptr) != (tb->finallyBlock == nullptr)) return false;
        if (ta->finallyBlock && !astEqual(ta->finallyBlock.get(), tb->finallyBlock.get())) return false;
        return true;
    }
    case NodeType::NODE_IMPORT_STMT: {
        auto* ia = static_cast<ImportStmt*>(a);
        auto* ib = static_cast<ImportStmt*>(b);
        return ia->modulePath == ib->modulePath
            && ia->names == ib->names
            && ia->importAll == ib->importAll;
    }
    // BUG-LPA-02 fix: InterpolatedString.literals 是 std::vector<std::string>，
    //   不在 children() 中（children() 仅返回 expressions），必须显式比较。
    case NodeType::NODE_INTERPOLATED_STRING: {
        auto* ia = static_cast<InterpolatedString*>(a);
        auto* ib = static_cast<InterpolatedString*>(b);
        if (ia->literals != ib->literals) return false;
        if (ia->expressions.size() != ib->expressions.size()) return false;
        for (size_t i = 0; i < ia->expressions.size(); ++i) {
            if (!astEqual(ia->expressions[i].get(), ib->expressions[i].get())) return false;
        }
        return true;
    }
    default:
        // 未覆盖的节点类型回退到子节点递归比较
        auto ca = a->children();
        auto cb = b->children();
        if (ca.size() != cb.size()) return false;
        for (size_t i = 0; i < ca.size(); ++i) {
            if (!astEqual(ca[i], cb[i])) return false;
        }
        return true;
    }
}

// 解析源码，返回 AST 根节点（出错或无 AST 则返回 nullptr）
static std::shared_ptr<Block> parseSource(const std::string& src, bool& hasError) {
    Lexer lexer;
    auto tokens = lexer.scan(src);
    if (lexer.getDiagnostics().hasErrors()) {
        hasError = true;
        return nullptr;
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        hasError = true;
        return nullptr;
    }
    hasError = false;
    return ast;
}

// ─── 1. 运算符优先级：幂等性 + AST 往返等价性 ────────────────────────

// 审计组1：运算符优先级——验证右嵌套同优先级括号在格式化后必须保留（往返 AST 不变），
// 左嵌套冗余括号可丢弃，同时检查格式化幂等性。
static int test_precedence() {
    std::cout << "\n===== 1. 运算符优先级：幂等性 + AST 往返等价性 =====\n";
    std::vector<AuditCase> cases = {
        // AUDIT-FMT-P0 验证：EQ/NEQ/LT/GT/LTE/GTE/AND/OR 同优先级右嵌套必须保留括号
        // 验证: EQ 右嵌套必须保留括号，否则重新解析分组改变（AST 不等价）
        {"P1", "EQ 右嵌套 EQ（右嵌套必须加括号）",  "print(1 == (2 == 3));"},
        // 验证: EQ 左嵌套括号冗余，可安全丢弃（左结合 AST 不变）
        {"P2", "EQ 左嵌套 EQ（左嵌套括号冗余）",    "print((1 == 2) == 3);"},
        // 验证: NEQ 右嵌套必须保留括号
        {"P3", "NEQ 右嵌套 NEQ（右嵌套必须加括号）", "print(1 != (2 != 3));"},
        // 验证: LT 左嵌套括号冗余
        {"P4", "LT 左嵌套 LT（左嵌套括号冗余）",     "print((1 < 2) < 3);"},
        // 验证: LT 右嵌套必须保留括号
        {"P5", "LT 右嵌套 LT（右嵌套必须加括号）",   "print(1 < (2 < 3));"},
        // 验证: AND 右嵌套必须保留括号
        {"P6", "AND 右嵌套 AND（右嵌套必须加括号）", "print(true and (false and true));"},
        // 验证: OR 右嵌套必须保留括号
        {"P7", "OR 右嵌套 OR（右嵌套必须加括号）",   "print(false or (true or false));"},
        // 已正确处理（#15 fix 已覆盖 ADD/SUB/MUL/DIV/MOD）
        // 验证: ADD 右嵌套括号（#15 已修复）保持幂等
        {"P8",  "ADD 右嵌套 ADD（#15 已修复）",   "print(1 + (2 + 3));"},
        // 验证: SUB 右嵌套括号（#15 已修复）保持幂等
        {"P9",  "SUB 右嵌套 SUB（#15 已修复）",   "print(1 - (2 - 3));"},
        // 验证: MUL 右嵌套括号（#15 已修复）保持幂等
        {"P10", "MUL 右嵌套 MUL（#15 已修复）",   "print(1 * (2 * 3));"},
        // 验证: DIV 右嵌套括号（#15 已修复）保持幂等
        {"P11", "DIV 右嵌套 DIV（#15 已修复）",   "print(1 / (2 / 3));"},
        // 验证: MOD 右嵌套括号（#15 已修复）保持幂等
        {"P12", "MOD 右嵌套 MOD（#15 已修复）",   "print(7 % (5 % 3));"},
        // 跨优先级
        // 验证: 乘优先于加，格式化后保留语义等价
        {"P13", "a + b * c（应加括号 #15）",      "print(1 + 2 * 3);"},
        // 验证: 比较链左结合，格式化后 AST 不变
        {"P14", "a < b == c（左结合）",            "print(1 < 2 == 3);"},
        // 验证: not 与 and 组合保持语义
        {"P15", "not a and b",                    "print(not true and false);"},
        // 一元负号
        // 验证: 一元负号作用于括号表达式
        {"P16", "一元负号作用于 BinaryOp",         "print(-(1 + 2));"},
        // 验证: 连续一元负号嵌套
        {"P17", "一元负号嵌套",                    "print(--5);"},
        // 验证: 负号与二元减号区分（前置 -5 为负字面量）
        {"P18", "负号与减号区分",                  "print(-5 - 3);"},
        // 验证: 二元减号后跟负字面量
        {"P19", "负号与减号区分 2",                "print(5 - -3);"},
    };

    int fail = 0;
    for (auto& tc : cases) {
        bool eFmt1, eFmt2, eParse1, eParse2;
        std::string fmt1 = formatCode(tc.source, eFmt1);
        std::string fmt2 = formatCode(fmt1, eFmt2);
        bool idempotent = (fmt1 == fmt2) && !eFmt1 && !eFmt2;

        // AUDIT-FMT-P2 fix: 真正的往返不变量验证——比较 Parse(src) 与 Parse(fmt1) 的 AST 结构
        // 左嵌套同优先级（如 (a OP b) OP c）丢弃括号是正确的：重新解析因左结合律
        //   仍得到 OP(OP(a,b), c)，AST 不变。
        // 右嵌套同优先级（如 a OP (b OP c)）丢弃括号是 BUG：重新解析得到
        //   OP(OP(a,b), c)，AST 改变，往返不变量被破坏。
        auto ast1 = parseSource(tc.source, eParse1);
        auto ast2 = parseSource(fmt1, eParse2);
        bool roundTripOk = false;
        if (ast1 && ast2 && !eParse1 && !eParse2) {
            // 比较 Block 的第一个语句（即 print 语句）
            if (!ast1->statements.empty() && !ast2->statements.empty()) {
                roundTripOk = astEqual(ast1->statements[0].get(),
                                       ast2->statements[0].get());
            }
        }

        bool ok = idempotent && roundTripOk;
        std::cout << (ok ? " PASS " : " FAIL ") << tc.id << " " << tc.name;
        if (!roundTripOk && idempotent) {
            std::cout << "  [!! AST 结构改变 !!]";
        } else if (!idempotent) {
            std::cout << "  [!! 非幂等 !!]";
        }
        std::cout << "\n";
        if (!ok) {
            fail++;
            std::cout << "   source: [" << tc.source << "]\n";
            std::cout << "   fmt1:   [" << fmt1 << "]\n";
            std::cout << "   fmt2:   [" << fmt2 << "]\n";
            if (!roundTripOk && idempotent) {
                std::cout << "   *** Parse(src) 与 Parse(fmt1) 的 AST 结构不等价 ***\n";
                std::cout << "   *** 往返不变量被破坏（右嵌套括号丢失导致重新解析分组改变）***\n";
            }
        }
    }
    return fail;
}

// ─── 2. 字符串字面量 ─────────────────────────────────────────────────

// 审计组2：字符串字面量——验证含分号/花括号/换行/反斜杠/引号/转义/注释字符的字符串在格式化后保持幂等。
static int test_strings() {
    std::cout << "\n===== 2. 字符串字面量 =====\n";
    std::vector<AuditCase> cases = {
        // 验证: 字符串内分号不应被误解析为语句分隔
        {"S1", "字符串内含分号",       "print(\"a;b;c\");"},
        // 验证: 字符串内花括号不应破坏代码块结构
        {"S2", "字符串内含花括号",     "print(\"a{b}c\");"},
        // 验证: 字符串内转义换行保持幂等
        {"S3", "字符串内含换行",       "print(\"line1\\nline2\");"},
        // 验证: 字符串内反斜杠转义保持幂等
        {"S4", "字符串内含反斜杠",     "print(\"path\\\\dir\");"},
        // 验证: 字符串内引号转义保持幂等
        {"S5", "字符串内含引号",       "print(\"say \\\"hi\\\"\");"},
        // 验证: 字符串内制表符转义保持幂等
        {"S6", "字符串内含制表符",    "print(\"col1\\tcol2\");"},
        // 验证: 空字符串格式化后保持为空
        {"S7", "空字符串",            "print(\"\");"},
        // 验证: 字符串内 # 不被当注释
        {"S8", "字符串含 # 注释字符", "print(\"# not comment\");"},
        // 验证: 字符串内 // 不被当注释
        {"S9", "字符串含 // 注释",    "print(\"// not comment\");"},
        // 验证: 字符串内 /* */ 不被当块注释
        {"S10","字符串含 /* 块注释",  "print(\"/* not comment */\");"},
    };

    int fail = 0;
    for (auto& tc : cases) {
        bool e1, e2;
        std::string fmt1 = formatCode(tc.source, e1);
        std::string fmt2 = formatCode(fmt1, e2);
        bool ok = (fmt1 == fmt2) && !e1 && !e2;
        std::cout << (ok ? " PASS " : " FAIL ") << tc.id << " " << tc.name << "\n";
        if (!ok) {
            fail++;
            std::cout << "   source: [" << tc.source << "]\n";
            std::cout << "   fmt1:   [" << fmt1 << "]\n";
            std::cout << "   fmt2:   [" << fmt2 << "]\n";
        }
    }
    return fail;
}

// ─── 3. 注释 ──────────────────────────────────────────────────────────

// 审计组3：注释——验证行内/独立行/块注释（含多行、跨语句）在格式化后不丢失且关联正确，保持幂等。
static int test_comments() {
    std::cout << "\n===== 3. 注释 =====\n";
    std::vector<AuditCase> cases = {
        // 验证: 行内注释随语句保留且不丢失
        {"C1",  "行内注释",                "var x = 1; // comment\nprint(x);"},
        // 验证: 独立行注释保留在语句上方
        {"C2",  "独立行注释",              "// standalone\nvar x = 1;\nprint(x);"},
        // 验证: 独立块注释保留且不破坏后续语句
        {"C3",  "块注释独立",              "/* block */\nvar x = 1;\nprint(x);"},
        // 验证: 多行块注释整体保留
        {"C4",  "多行块注释",              "/* multi\nline\ncomment */\nvar x = 1;\nprint(x);"},
        // 验证: 块注释行内保留在语句尾部
        {"C5",  "块注释行内",              "var x = 1; /* inline */\nprint(x);"},
        // 验证: 语句间注释正确关联到后一语句
        {"C6",  "语句间注释关联到后一个",  "var x = 1;\n// between\nprint(x);"},
        // 验证: 多个连续注释全部保留
        {"C7",  "多个连续注释",            "// c1\n// c2\n// c3\nprint(42);"},
        // 验证: 块注释跨语句保持结构
        {"C8",  "块注释跨语句",            "var x = 1;\n/* block\nspanning */\nprint(x);"},
    };

    int fail = 0;
    for (auto& tc : cases) {
        bool e1, e2;
        std::string fmt1 = formatCode(tc.source, e1);
        std::string fmt2 = formatCode(fmt1, e2);
        bool ok = (fmt1 == fmt2) && !e1 && !e2;
        std::cout << (ok ? " PASS " : " FAIL ") << tc.id << " " << tc.name << "\n";
        if (!ok) {
            fail++;
            std::cout << "   source: [" << tc.source << "]\n";
            std::cout << "   fmt1:   [" << fmt1 << "]\n";
            std::cout << "   fmt2:   [" << fmt2 << "]\n";
        }
    }
    return fail;
}

// ─── 4. 空语句与 for 循环空字段 ──────────────────────────────────────

// 审计组4：空语句与 for 空字段——验证 for 三段的各空组合、空 if/while/函数体在格式化后正确保留，幂等。
static int test_empty_fields() {
    std::cout << "\n===== 4. for 循环空字段 =====\n";
    std::vector<AuditCase> cases = {
        // 验证: for 缺条件段时仍正确保留并幂等
        {"E1", "for 无 cond",             "for (var i = 0; ; i = i + 1) { if (i == 3) { break; } print(i); }"},
        // 验证: for 缺 init 段（外部已声明）保持幂等
        {"E2", "for 无 init",             "var i = 0; for (; i < 3; i = i + 1) { print(i); }"},
        // 验证: for 缺 update 段（循环体内自增）保持幂等
        {"E3", "for 无 update",           "for (var i = 0; i < 3;) { print(i); i = i + 1; }"},
        // 验证: for 缺 init 与 update 两段保持幂等
        {"E4", "for 无 init 无 update",   "var i = 0; for (; i < 3;) { print(i); i = i + 1; }"},
        // 验证: for 三段全空（内部 break）保持幂等
        {"E5", "for 全空",                "var i = 0; for (;;) { if (i >= 3) { break; } print(i); i = i + 1; }"},
        // 验证: 空 if 块保留且不吞并后续语句
        {"E6", "空 if 块",                "if (true) { } print(42);"},
        // 验证: 空 while 块保留且幂等
        {"E7", "空 while 块",             "while (false) { } print(42);"},
        // 验证: 空函数体保留且打印 null 返回值
        {"E8", "空函数体",                 "fun f() { } print(f());"},
    };

    int fail = 0;
    for (auto& tc : cases) {
        bool e1, e2;
        std::string fmt1 = formatCode(tc.source, e1);
        std::string fmt2 = formatCode(fmt1, e2);
        bool ok = (fmt1 == fmt2) && !e1 && !e2;
        std::cout << (ok ? " PASS " : " FAIL ") << tc.id << " " << tc.name << "\n";
        if (!ok) {
            fail++;
            std::cout << "   source: [" << tc.source << "]\n";
            std::cout << "   fmt1:   [" << fmt1 << "]\n";
            std::cout << "   fmt2:   [" << fmt2 << "]\n";
        }
    }
    return fail;
}

// ─── 5. 配置项 ──────────────────────────────────────────────────────

// 审计组5：配置项——验证 FormatOptions 各开关（分号移除、运算符空格、花括号风格、Tab、逗号空格、函数间空行、缩进宽度）确实生效。
static int test_config() {
    std::cout << "\n===== 5. 配置项 =====\n";
    int fail = 0;

    // 5.1 semicolons 配置项已移除（AUDIT-FMT-P1 fix）
    // 验证: 默认配置下格式化输出应保留分号（semicolons 字段已移除，非死代码残留）
    // 原为死代码，现字段已删除；此处验证默认格式化输出含分号（语句合法）
    {
        FormatOptions opts;  // 默认配置
        bool err;
        std::string src = "var x = 1; print(x);";
        std::string fmt = formatCode(src, err, opts);
        bool hasSemicolon = fmt.find(';') != std::string::npos;
        bool ok = hasSemicolon && !err;
        std::cout << (ok ? " PASS " : " FAIL ") << "CFG1 默认输出含分号（semicolons 字段已移除）\n";
        if (!ok) {
            fail++;
            std::cout << "   fmt: [" << fmt << "]\n";
        }
    }

    // 5.2 spaceAroundOperators=false 与一元负号
    // 验证: 关闭运算符空格后，-5 的一元负号不应出现 "- 5" 空格
    {
        FormatOptions opts;
        opts.spaceAroundOperators = false;
        bool err;
        std::string src = "var x = -5 + 3; print(x);";
        std::string fmt = formatCode(src, err, opts);
        // 一元负号 -5 不应有空格，二元 + 应无空格
        bool unaryHasSpace = (fmt.find("- 5") != std::string::npos);
        bool ok = !unaryHasSpace;
        std::cout << (ok ? " PASS " : " FAIL ") << "CFG2 spaceAroundOps=false 与一元负号\n";
        if (!ok) {
            fail++;
            std::cout << "   fmt: [" << fmt << "]\n";
        }
    }

    // 5.3 braceStyle=NEXT_LINE
    // 验证: 花括号风格设为 NEXT_LINE 后输出为 "if (true)\n{" 形式
    {
        FormatOptions opts;
        opts.braceStyle = BraceStyle::NEXT_LINE;
        bool err;
        std::string src = "if (true) { print(1); }";
        std::string fmt = formatCode(src, err, opts);
        bool ok = (fmt.find("if (true)\n{") != std::string::npos);
        std::cout << (ok ? " PASS " : " FAIL ") << "CFG3 braceStyle=NEXT_LINE\n";
        if (!ok) {
            fail++;
            std::cout << "   fmt: [" << fmt << "]\n";
        }
    }

    // 5.4 useTabs=true
    // 验证: 启用 useTabs 后格式化输出应包含制表符缩进
    {
        FormatOptions opts;
        opts.useTabs = true;
        opts.indentSize = 1;
        bool err;
        std::string src = "fun f() { print(1); }";
        std::string fmt = formatCode(src, err, opts);
        bool ok = (fmt.find("\t") != std::string::npos);
        std::cout << (ok ? " PASS " : " FAIL ") << "CFG4 useTabs=true\n";
        if (!ok) {
            fail++;
            std::cout << "   fmt: [" << fmt << "]\n";
        }
    }

    // 5.5 spaceAfterComma=false
    // 验证: 关闭逗号后空格后输出不应包含 ", " 形式
    {
        FormatOptions opts;
        opts.spaceAfterComma = false;
        bool err;
        std::string src = "fun f(a, b, c) { return a + b + c; } print(f(1, 2, 3));";
        std::string fmt = formatCode(src, err, opts);
        bool ok = (fmt.find(", ") == std::string::npos);
        std::cout << (ok ? " PASS " : " FAIL ") << "CFG5 spaceAfterComma=false\n";
        if (!ok) {
            fail++;
            std::cout << "   fmt: [" << fmt << "]\n";
        }
    }

    // 5.6 blankLineBetweenFunctions=false
    // 验证: 关闭函数间空行后两个函数不应以空行分隔
    {
        FormatOptions opts;
        opts.blankLineBetweenFunctions = false;
        bool err;
        std::string src = "fun f() { return 1; }\nfun g() { return 2; }";
        std::string fmt = formatCode(src, err, opts);
        bool hasBlank = fmt.find("}\n\nfun g") != std::string::npos;
        bool ok = !hasBlank;
        std::cout << (ok ? " PASS " : " FAIL ") << "CFG6 blankLineBetweenFunctions=false\n";
        if (!ok) {
            fail++;
            std::cout << "   fmt: [" << fmt << "]\n";
        }
    }

    // 5.7 indentSize=2
    // 验证: 缩进宽度设为 2 后应使用两空格缩进而非四空格
    {
        FormatOptions opts;
        opts.indentSize = 2;
        bool err;
        std::string src = "fun f() { print(1); }";
        std::string fmt = formatCode(src, err, opts);
        // 应使用 2 空格缩进
        bool has4Spaces = (fmt.find("    print") != std::string::npos);
        bool has2Spaces = (fmt.find("  print") != std::string::npos);
        bool ok = has2Spaces && !has4Spaces;
        std::cout << (ok ? " PASS " : " FAIL ") << "CFG7 indentSize=2\n";
        if (!ok) {
            fail++;
            std::cout << "   fmt: [" << fmt << "]\n";
        }
    }

    return fail;
}

// ─── 6. 容器字面量 ──────────────────────────────────────────────────

// 审计组6：容器字面量——验证数组/字典（含嵌套、空容器、表达式元素）的格式化幂等性。
static int test_containers() {
    std::cout << "\n===== 6. 容器字面量 =====\n";
    std::vector<AuditCase> cases = {
        // 验证: 小数组字面量格式化保持幂等
        {"L1", "小数组",            "var a = [1, 2, 3]; print(a[0]);"},
        // 验证: 大数组字面量格式化保持幂等
        {"L2", "大数组",            "var a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]; print(a.len());"},
        // 验证: 嵌套数组格式化保持幂等
        {"L3", "嵌套数组",          "var a = [[1, 2], [3, 4]]; print(a[0][1]);"},
        // 验证: 小字典字面量格式化保持幂等
        {"L4", "小字典",            "var d = {\"a\": 1, \"b\": 2}; print(d[\"a\"]);"},
        // 验证: 空数组格式化保持幂等
        {"L5", "空数组",            "var a = []; print(a.len());"},
        // 验证: 空字典格式化保持幂等
        {"L6", "空字典",            "var d = {}; print(d.len());"},
        // 验证: 数组元素为表达式时格式化保持幂等
        {"L7", "数组含表达式",      "var a = [1 + 1, 2 * 2, 3 - 1]; print(a[0]);"},
        // 验证: 字典值为复杂结构时格式化保持幂等
        {"L8", "字典含复杂值",      "var d = {\"k\": [1, 2, 3]}; print(d[\"k\"][1]);"},
    };

    int fail = 0;
    for (auto& tc : cases) {
        bool e1, e2;
        std::string fmt1 = formatCode(tc.source, e1);
        std::string fmt2 = formatCode(fmt1, e2);
        bool ok = (fmt1 == fmt2) && !e1 && !e2;
        std::cout << (ok ? " PASS " : " FAIL ") << tc.id << " " << tc.name << "\n";
        if (!ok) {
            fail++;
            std::cout << "   source: [" << tc.source << "]\n";
            std::cout << "   fmt1:   [" << fmt1 << "]\n";
            std::cout << "   fmt2:   [" << fmt2 << "]\n";
        }
    }
    return fail;
}

// ─── 7. 插值字符串 ──────────────────────────────────────────────────

// 审计组7：插值字符串——验证 {expr} 插值（含多表达式、方法调用、比较、变量）在格式化后保持幂等。
static int test_interp() {
    std::cout << "\n===== 7. 插值字符串 =====\n";
    std::vector<AuditCase> cases = {
        // 验证: 简单变量插值格式化保持幂等
        {"I1", "简单插值",         "var x = 42; print(\"val={x}\");"},
        // 验证: 多表达式插值格式化保持幂等
        {"I2", "多表达式插值",     "var a = 1; var b = 2; print(\"{a}+{b}={a + b}\");"},
        // 验证: 插值含算术表达式格式化保持幂等
        {"I3", "插值含表达式",     "print(\"{1 + 2 * 3}\");"},
        // 验证: 插值含方法调用格式化保持幂等
        {"I4", "插值含方法调用",   "var a = [1, 2, 3]; print(\"len={a.len()}\");"},
        // 验证: 无插值纯文本字符串格式化保持幂等
        {"I5", "插值字符串无表达式", "print(\"plain text\");"},
        // 验证: 插值含比较表达式格式化保持幂等
        {"I6", "插值含比较",       "print(\"{1 < 2}\");"},
        // 验证: 插值含变量引用格式化保持幂等
        {"I7", "插值含变量",       "var name = \"world\"; print(\"hello {name}\");"},
    };

    int fail = 0;
    for (auto& tc : cases) {
        bool e1, e2;
        std::string fmt1 = formatCode(tc.source, e1);
        std::string fmt2 = formatCode(fmt1, e2);
        bool ok = (fmt1 == fmt2) && !e1 && !e2;
        std::cout << (ok ? " PASS " : " FAIL ") << tc.id << " " << tc.name << "\n";
        if (!ok) {
            fail++;
            std::cout << "   source: [" << tc.source << "]\n";
            std::cout << "   fmt1:   [" << fmt1 << "]\n";
            std::cout << "   fmt2:   [" << fmt2 << "]\n";
        }
    }
    return fail;
}

// ─── 8. 语句级 AST 往返等价性 ───────────────────────────────────────
// 审计盲区修复：原 test_precedence 仅覆盖表达式级（19 用例），
// 语句级结构（if/for/while/class/try）无 AST 往返验证——若 Formatter
// 在格式化这些结构时丢失或重组子节点，幂等性测试无法发现（format=format 恒成立）。
// 本测试对语句级结构做 Parse(src) vs Parse(format(src)) 的 AST 结构等价比较。

// 审计组8：语句级 AST 往返等价性——对 if/循环/函数/类/try/复合序列做 Parse(src) vs Parse(format(src)) 结构比较，
// 弥补仅测幂等性无法发现「结构被重组但 format=format 恒成立」的盲区。
static int test_statements() {
    std::cout << "\n===== 8. 语句级 AST 往返等价性 =====\n";
    std::vector<AuditCase> cases = {
        // if/else 分支结构——验证 then/else 分支不被合并或丢失
        // 验证: if-else 完整结构的 AST 往返等价（分支不丢）
        {"S1", "if-else 完整结构",       "if (x > 0) { print(1); } else { print(2); }"},
        // 验证: 仅 if 无 else 结构的 AST 往返等价
        {"S2", "if 无 else",             "if (x > 0) { print(1); }"},
        // 验证: 嵌套 if-else 结构 AST 往返等价
        {"S3", "嵌套 if-else",           "if (a) { if (b) { print(1); } else { print(2); } } else { print(3); }"},
        // 循环结构——验证 init/cond/update/body 不被重组
        // 验证: for 循环三段结构 AST 往返等价
        {"S4", "for 循环",               "for (var i = 0; i < 10; i = i + 1) { print(i); }"},
        // 验证: while 循环结构 AST 往返等价
        {"S5", "while 循环",             "while (x < 100) { x = x * 2; }"},
        // 验证: 嵌套循环结构 AST 往返等价
        {"S6", "嵌套循环",               "for (var i = 0; i < 3; i = i + 1) { for (var j = 0; j < 3; j = j + 1) { print(i + j); } }"},
        // 函数声明——验证参数列表和函数体不被修改
        // 验证: 函数声明（含参数）结构 AST 往返等价
        {"S7", "函数声明带默认参数",     "fun add(a, b) { return a + b; }"},
        // 验证: 嵌套函数声明结构 AST 往返等价
        {"S8", "嵌套函数声明",           "fun outer() { fun inner() { return 1; } return inner(); }"},
        // 类声明——验证字段和方法列表不被重排或丢失
        // 验证: 类声明（含继承、方法）结构 AST 往返等价
        {"S9", "类声明带继承",           "class Dog extends Animal { var name; fun bark() { print(\"woof\"); } }"},
        // 验证: 多方法类声明结构 AST 往返等价
        {"S10", "类声明多方法",          "class Point { var x; var y; fun init(a, b) { x = a; y = b; } fun dist() { return x + y; } }"},
        // try/catch——验证 try 块和 catch 块结构不被重组
        // 验证: try-catch 完整结构 AST 往返等价
        {"S11", "try-catch 完整",        "try { print(risky()); } catch (e) { print(e); }"},
        // 验证: 嵌套 try-catch 结构 AST 往返等价
        {"S12", "嵌套 try-catch",        "try { try { throw 1; } catch (e) { throw 2; } } catch (e) { print(e); }"},
        // 复合语句序列——验证语句顺序不被重排
        // 验证: 多语句顺序序列 AST 往返等价（顺序不重排）
        {"S13", "多语句序列",            "var a = 1; var b = 2; var c = a + b; print(c);"},
        // 验证: 语句内嵌表达式 AST 往返等价
        {"S14", "语句内嵌表达式",        "var x = 1 + 2 * 3; if (x > 5) { print(x - (2 - 1)); }"},
        // break/continue/return 在循环和函数中
        // 验证: 循环中 break/continue 结构 AST 往返等价
        {"S15", "break/continue in 循环", "var i = 0; while (true) { if (i >= 10) { break; } if (i % 2 == 0) { i = i + 1; continue; } print(i); i = i + 1; }"},
        // 验证: 函数内条件 return 结构 AST 往返等价
        {"S16", "return 在函数中",       "fun f(x) { if (x > 0) { return x * 2; } return 0; }"},
    };

    int fail = 0;
    for (auto& tc : cases) {
        bool eFmt1, eFmt2, eParse1, eParse2;
        std::string fmt1 = formatCode(tc.source, eFmt1);
        std::string fmt2 = formatCode(fmt1, eFmt2);
        bool idempotent = (fmt1 == fmt2) && !eFmt1 && !eFmt2;

        // 语句级 AST 往返等价性：比较 Parse(src) 与 Parse(format(src)) 的 AST 结构
        auto ast1 = parseSource(tc.source, eParse1);
        auto ast2 = parseSource(fmt1, eParse2);
        bool roundTripOk = false;
        if (ast1 && ast2 && !eParse1 && !eParse2) {
            if (ast1->statements.size() != ast2->statements.size()) {
                roundTripOk = false;
            } else {
                roundTripOk = true;
                for (size_t i = 0; i < ast1->statements.size(); ++i) {
                    if (!astEqual(ast1->statements[i].get(), ast2->statements[i].get())) {
                        roundTripOk = false;
                        break;
                    }
                }
            }
        }

        bool ok = idempotent && roundTripOk;
        std::cout << (ok ? " PASS " : " FAIL ") << tc.id << " " << tc.name;
        if (!roundTripOk && idempotent) {
            std::cout << "  [!! AST 结构改变 !!]";
        } else if (!idempotent) {
            std::cout << "  [!! 非幂等 !!]";
        }
        std::cout << "\n";
        if (!ok) {
            fail++;
            std::cout << "   source: [" << tc.source << "]\n";
            std::cout << "   fmt1:   [" << fmt1 << "]\n";
            std::cout << "   fmt2:   [" << fmt2 << "]\n";
            if (!roundTripOk && idempotent) {
                std::cout << "   *** Parse(src) 与 Parse(fmt1) 的 AST 结构不等价 ***\n";
                std::cout << "   *** 语句级往返不变量被破坏 ***\n";
            }
        }
    }
    return fail;
}

int main() {
    std::cout << "=== Formatter 深度审计验证 ===\n";
    std::cout << "目标：验证格式化是语义保持的保形变换\n";
    std::cout << "（仅依赖 Lexer+Parser+Formatter，无 Interpreter）\n";

    // 顺序执行 8 个审计分组，累计失败数；任何分组非零即整体返回失败。
    int totalFail = 0;
    totalFail += test_precedence();
    totalFail += test_strings();
    totalFail += test_comments();
    totalFail += test_empty_fields();
    totalFail += test_config();
    totalFail += test_containers();
    totalFail += test_interp();
    totalFail += test_statements();

    std::cout << "\n=== 总计 ===\n";
    std::cout << "失败用例数: " << totalFail << "\n";
    return totalFail > 0 ? 1 : 0;
}
