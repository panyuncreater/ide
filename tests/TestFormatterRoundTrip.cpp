// ============================================================
// D15 fix（审计报告 2026-08-03 D15）: Formatter 往返等价自动化测试
// ------------------------------------------------------------
// 审计发现 formatter 的"AST 等价比较器"只存在于默认不构建的
// test_harness/formatter_audit.cpp，tests/ 常规回归中仅有幂等
// （format→reparse→format 输出不变）断言而无 AST 等价断言。
// 本文件把 astEqual 比较器（与 test_harness 保持同步，含
// BUG-LPA-01/02 修复）提升进常规测试，并对 samples/mini 全部
// 样例做数据驱动验证：
//   1. parse(src) → format → reparse 成功；
//   2. astEqual(parse(src), parse(fmt)) —— 往返不改变 AST 结构；
//   3. format(parse(format(parse(src)))) == format(parse(src)) —— 幂等。
// 注释按行号锚定注入（F1 fix），往返可能移动注释位置，但注释
// 不影响 AST，等价断言不受影响。
// ============================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ast/ASTNode.h"
#include "formatter/Formatter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

namespace {

// ─── AST 结构等价性比较（与 test_harness/formatter_audit.cpp 同步）─────────
// 忽略行号等位置信息；显式 case 覆盖含自身标量字段的节点类型
// （BUG-LPA-01/02 fix），其余回退 children() 递归比较。
static bool astEqual(ASTNode* a, ASTNode* b) {
    if (!a && !b)
        return true;
    if (!a || !b)
        return false;
    if (a->nodeType != b->nodeType)
        return false;

    switch (a->nodeType) {
    case NodeType::NODE_BINARY_OP: {
        auto* ba = static_cast<BinaryOp*>(a);
        auto* bb = static_cast<BinaryOp*>(b);
        return ba->opType == bb->opType && astEqual(ba->left.get(), bb->left.get()) &&
               astEqual(ba->right.get(), bb->right.get());
    }
    case NodeType::NODE_UNARY_OP: {
        auto* ua = static_cast<UnaryOp*>(a);
        auto* ub = static_cast<UnaryOp*>(b);
        return ua->opType == ub->opType && astEqual(ua->operand.get(), ub->operand.get());
    }
    case NodeType::NODE_NUMBER_LITERAL: {
        auto* na = static_cast<NumberLiteral*>(a);
        auto* nb = static_cast<NumberLiteral*>(b);
        if (na->isInt() != nb->isInt())
            return false;
        return na->isInt() ? (na->intVal() == nb->intVal()) : (na->floatVal() == nb->floatVal());
    }
    case NodeType::NODE_BOOL_LITERAL:
        return static_cast<BoolLiteral*>(a)->value == static_cast<BoolLiteral*>(b)->value;
    case NodeType::NODE_STRING_LITERAL:
        return static_cast<StringLiteral*>(a)->value == static_cast<StringLiteral*>(b)->value;
    case NodeType::NODE_VAR_REF:
        return static_cast<VarRef*>(a)->name == static_cast<VarRef*>(b)->name;
    case NodeType::NODE_PRINT_STMT: {
        auto* pa = static_cast<PrintStmt*>(a);
        auto* pb = static_cast<PrintStmt*>(b);
        if (pa->values.size() != pb->values.size())
            return false;
        for (size_t i = 0; i < pa->values.size(); ++i) {
            if (!astEqual(pa->values[i].get(), pb->values[i].get()))
                return false;
        }
        return true;
    }
    case NodeType::NODE_VAR_DECL: {
        auto* va = static_cast<VarDecl*>(a);
        auto* vb = static_cast<VarDecl*>(b);
        if (va->name != vb->name)
            return false;
        if (va->typeAnnotation != vb->typeAnnotation)
            return false;
        return astEqual(va->initializer.get(), vb->initializer.get());
    }
    case NodeType::NODE_ASSIGNMENT: {
        auto* aa = static_cast<Assignment*>(a);
        auto* ab = static_cast<Assignment*>(b);
        if (aa->name != ab->name)
            return false;
        return astEqual(aa->value.get(), ab->value.get());
    }
    case NodeType::NODE_FUN_DECL: {
        auto* fa = static_cast<FunDecl*>(a);
        auto* fb = static_cast<FunDecl*>(b);
        if (fa->name != fb->name)
            return false;
        if (fa->params != fb->params)
            return false;
        if (fa->paramTypes != fb->paramTypes)
            return false;
        if (fa->returnType != fb->returnType)
            return false;
        if (fa->defaultValues.size() != fb->defaultValues.size())
            return false;
        for (size_t i = 0; i < fa->defaultValues.size(); ++i) {
            if (!astEqual(fa->defaultValues[i].get(), fb->defaultValues[i].get()))
                return false;
        }
        return astEqual(fa->body.get(), fb->body.get());
    }
    case NodeType::NODE_FUN_CALL: {
        auto* ca = static_cast<FunCall*>(a);
        auto* cb = static_cast<FunCall*>(b);
        if (ca->name != cb->name)
            return false;
        if ((ca->callee == nullptr) != (cb->callee == nullptr))
            return false;
        if (ca->callee && !astEqual(ca->callee.get(), cb->callee.get()))
            return false;
        if (ca->arguments.size() != cb->arguments.size())
            return false;
        for (size_t i = 0; i < ca->arguments.size(); ++i) {
            if (!astEqual(ca->arguments[i].get(), cb->arguments[i].get()))
                return false;
        }
        return true;
    }
    case NodeType::NODE_MEMBER_ACCESS: {
        auto* ma = static_cast<MemberAccess*>(a);
        auto* mb = static_cast<MemberAccess*>(b);
        if (ma->fieldName != mb->fieldName)
            return false;
        return astEqual(ma->object.get(), mb->object.get());
    }
    case NodeType::NODE_MEMBER_ASSIGN: {
        auto* ma = static_cast<MemberAssign*>(a);
        auto* mb = static_cast<MemberAssign*>(b);
        if (ma->fieldName != mb->fieldName)
            return false;
        if (!astEqual(ma->object.get(), mb->object.get()))
            return false;
        return astEqual(ma->value.get(), mb->value.get());
    }
    case NodeType::NODE_METHOD_CALL: {
        auto* ma = static_cast<MethodCall*>(a);
        auto* mb = static_cast<MethodCall*>(b);
        if (ma->methodName != mb->methodName)
            return false;
        if (!astEqual(ma->object.get(), mb->object.get()))
            return false;
        if (ma->arguments.size() != mb->arguments.size())
            return false;
        for (size_t i = 0; i < ma->arguments.size(); ++i) {
            if (!astEqual(ma->arguments[i].get(), mb->arguments[i].get()))
                return false;
        }
        return true;
    }
    case NodeType::NODE_CLASS_DECL: {
        auto* ca = static_cast<ClassDecl*>(a);
        auto* cb = static_cast<ClassDecl*>(b);
        if (ca->name != cb->name)
            return false;
        if (ca->superClassName != cb->superClassName)
            return false;
        if (ca->members.size() != cb->members.size())
            return false;
        for (size_t i = 0; i < ca->members.size(); ++i) {
            if (!astEqual(ca->members[i].get(), cb->members[i].get()))
                return false;
        }
        return true;
    }
    case NodeType::NODE_TRY_STMT: {
        auto* ta = static_cast<TryStmt*>(a);
        auto* tb = static_cast<TryStmt*>(b);
        if (ta->catchVarName != tb->catchVarName)
            return false;
        if (!astEqual(ta->tryBlock.get(), tb->tryBlock.get()))
            return false;
        if (!astEqual(ta->catchBlock.get(), tb->catchBlock.get()))
            return false;
        if ((ta->finallyBlock == nullptr) != (tb->finallyBlock == nullptr))
            return false;
        if (ta->finallyBlock && !astEqual(ta->finallyBlock.get(), tb->finallyBlock.get()))
            return false;
        return true;
    }
    case NodeType::NODE_IMPORT_STMT: {
        auto* ia = static_cast<ImportStmt*>(a);
        auto* ib = static_cast<ImportStmt*>(b);
        return ia->modulePath == ib->modulePath && ia->names == ib->names && ia->importAll == ib->importAll;
    }
    case NodeType::NODE_INTERPOLATED_STRING: {
        auto* ia = static_cast<InterpolatedString*>(a);
        auto* ib = static_cast<InterpolatedString*>(b);
        if (ia->literals != ib->literals)
            return false;
        if (ia->expressions.size() != ib->expressions.size())
            return false;
        for (size_t i = 0; i < ia->expressions.size(); ++i) {
            if (!astEqual(ia->expressions[i].get(), ib->expressions[i].get()))
                return false;
        }
        return true;
    }
    default:
        // 未覆盖的节点类型回退到子节点递归比较（其语义字段均在 children() 中）
        auto ca = a->children();
        auto cb = b->children();
        if (ca.size() != cb.size())
            return false;
        for (size_t i = 0; i < ca.size(); ++i) {
            if (!astEqual(ca[i], cb[i]))
                return false;
        }
        return true;
    }
}

// 解析源码；失败时返回 nullptr 并在 err 回传诊断摘要
static std::shared_ptr<Block> parseSource(const std::string& src, std::string& err) {
    Lexer lexer;
    auto tokens = lexer.scan(src);
    if (lexer.getDiagnostics().hasErrors()) {
        err = "LEX: " + lexer.getDiagnostics().summary();
        return nullptr;
    }
    Parser parser;
    auto ast = parser.parse(tokens);
    if (parser.hasErrors() || !ast) {
        err = "PARSE: " + parser.getDiagnostics().summary();
        return nullptr;
    }
    return ast;
}

// 格式化 AST（携带 lexer 注释供按行号注入）
static std::string formatSource(Block& ast, const Lexer& lexer) {
    Formatter fmt;
    fmt.setComments(lexer.comments());
    return fmt.format(ast);
}

// 完整往返：返回 {AST1, fmt1, AST2, fmt2}；失败时 err 非空
struct RoundTripResult {
    std::shared_ptr<Block> ast1;
    std::string fmt1;
    std::shared_ptr<Block> ast2;
    std::string fmt2;
    std::string err;
};

static RoundTripResult roundTrip(const std::string& src) {
    RoundTripResult r;
    std::string err;
    Lexer lexer;
    auto tokens = lexer.scan(src);
    if (lexer.getDiagnostics().hasErrors()) {
        r.err = "LEX: " + lexer.getDiagnostics().summary();
        return r;
    }
    Parser parser;
    r.ast1 = parser.parse(tokens);
    if (parser.hasErrors() || !r.ast1) {
        r.err = "PARSE1: " + parser.getDiagnostics().summary();
        return r;
    }
    r.fmt1 = formatSource(*r.ast1, lexer);
    r.ast2 = parseSource(r.fmt1, err);
    if (!r.ast2) {
        r.err = "REPARSE: " + err;
        return r;
    }
    // 二次格式化需携带第一次格式化文本的注释（重新词法）
    Lexer lexer2;
    auto tokens2 = lexer2.scan(r.fmt1);
    Parser parser2;
    auto astTmp = parser2.parse(tokens2);
    (void)astTmp;
    r.fmt2 = formatSource(*r.ast2, lexer2);
    return r;
}

#ifndef MINILANG_SAMPLES_DIR
#define MINILANG_SAMPLES_DIR ""
#endif

// 枚举 samples/mini 下全部 .mini 样例路径
static std::vector<std::filesystem::path> collectSampleFiles() {
    std::vector<std::filesystem::path> files;
    std::filesystem::path dir(MINILANG_SAMPLES_DIR);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec))
        return files;
    for (auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".mini")
            files.push_back(entry.path());
    }
    return files;
}

static std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

// ---- 数据驱动：samples/mini 全样例 往返 AST 等价 + 二次格式化幂等 ----
TEST(FormatterRoundTrip, SamplesMiniAstEquivalenceAndIdempotence) {
    auto files = collectSampleFiles();
    ASSERT_GT(files.size(), 0u) << "未找到样例文件，MINILANG_SAMPLES_DIR=" << MINILANG_SAMPLES_DIR;
    for (const auto& path : files) {
        std::string src = readFile(path);
        auto r = roundTrip(src);
        ASSERT_TRUE(r.err.empty()) << path.filename().string() << ": " << r.err;
        EXPECT_TRUE(astEqual(r.ast1.get(), r.ast2.get()))
            << path.filename().string() << ": 格式化往返改变了 AST 结构\n--- fmt1 ---\n" << r.fmt1;
        EXPECT_EQ(r.fmt1, r.fmt2)
            << path.filename().string() << ": 二次格式化不幂等\n--- fmt1 ---\n"
            << r.fmt1 << "\n--- fmt2 ---\n" << r.fmt2;
    }
}

// ---- 内联边界用例：括号保留（右嵌套同优先级）----
TEST(FormatterRoundTrip, RightNestedSamePrecedenceParensPreserveAst) {
    // a - (b - c)：右嵌套同优先级括号是语义必要的，丢失则 AST 改变
    auto r = roundTrip("var x = a - (b - c);");
    ASSERT_TRUE(r.err.empty()) << r.err;
    EXPECT_TRUE(astEqual(r.ast1.get(), r.ast2.get())) << "fmt1:\n" << r.fmt1;
    EXPECT_EQ(r.fmt1, r.fmt2);
}

// ---- 内联边界用例：左嵌套同优先级（括号可丢弃，AST 等价仍须成立）----
TEST(FormatterRoundTrip, LeftNestedSamePrecedenceAstEquivalent) {
    // (a - b) - c：左嵌套冗余括号被丢弃后按左结合律重新解析得到相同 AST
    auto r = roundTrip("var x = (a - b) - c;");
    ASSERT_TRUE(r.err.empty()) << r.err;
    EXPECT_TRUE(astEqual(r.ast1.get(), r.ast2.get())) << "fmt1:\n" << r.fmt1;
    EXPECT_EQ(r.fmt1, r.fmt2);
}

// ---- 内联边界用例：混合优先级 + 成员/索引/方法调用链 ----
TEST(FormatterRoundTrip, MixedPrecedenceAndMemberChain) {
    std::string src =
        "var r = obj.field + arr[0] * (a - b) / obj.method(1, 2) - (c + d);\n"
        "obj.field = arr[i + 1];\n"
        "if (a < b and c > d) { print(\"x {a} y\"); } else { print(b); }\n"
        "while (i < 10) { i = i + 1; }\n"
        "for (var k = 0; k < 3; k = k + 1) { sum = sum + k; }\n";
    auto r = roundTrip(src);
    ASSERT_TRUE(r.err.empty()) << r.err;
    EXPECT_TRUE(astEqual(r.ast1.get(), r.ast2.get())) << "fmt1:\n" << r.fmt1;
    EXPECT_EQ(r.fmt1, r.fmt2);
}

// ---- 内联边界用例：try/catch/finally + 闭包 + 函数/类结构 ----
TEST(FormatterRoundTrip, StructuralStatements) {
    std::string src =
        "class A extends B {\n"
        "    fun m(x) { return x + this.v; }\n"
        "}\n"
        "fun outer(a, b) {\n"
        "    fun inner(c) { return c * 2; }\n"
        "    var f = fun() { return a + b; };\n"
        "    try { return inner(a); } catch (err) { print(err); } finally { print(\"f\"); }\n"
        "}\n"
        "var d = {\"k\": [1, 2, 3], \"j\": (4, 5)};\n";
    auto r = roundTrip(src);
    ASSERT_TRUE(r.err.empty()) << r.err;
    EXPECT_TRUE(astEqual(r.ast1.get(), r.ast2.get())) << "fmt1:\n" << r.fmt1;
    EXPECT_EQ(r.fmt1, r.fmt2);
}
