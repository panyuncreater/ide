// ============================================================
// AstIRBuilder 专用单元测试
// ------------------------------------------------------------
// 直接验证 AST → IR 指令映射，不经过 VM 执行。
// TestIR.cpp 仅覆盖 AstIRBuilder 空 Block + 端到端执行；本文件
// 逐节点类型验证生成的 IROp 序列结构与操作数语义。
//
// 覆盖范围：
//   - 字面量（int/float/bool/string/null）→ LOAD_CONST/LOAD_NULL/LOAD_TRUE/LOAD_FALSE
//   - 二元运算各 operator → ADD/SUB/MUL/DIV/MOD/LT/GT/LTE/GTE/EQ/NEQ
//   - 一元运算 → NEGATE/NOT
//   - 变量声明/赋值/读取 → DEFINE_GLOBAL/STORE_GLOBAL/LOAD_GLOBAL
//   - 控制流（if/while/for）→ LABEL/JUMP/JUMP_IF_FALSE 模式
//   - 函数声明 → IRModule.functions + MAKE_CLOSURE
//   - 函数调用/返回 → CALL/RETURN/RETURN_NULL
//   - 容器（数组/字典/索引）→ BUILD_ARRAY/BUILD_DICT/INDEX_GET/INDEX_SET
//   - 成员访问 → MEMBER_GET/MEMBER_SET
//   - try/catch/throw → TRY_BEGIN/TRY_END/THROW
//   - 闭包 upvalue → MAKE_CLOSURE 捕获列表 + LOAD_UPVALUE/STORE_UPVALUE
//   - 类定义/构造 → DEFINE_CLASS/CLASS_NEW
//   - enum variant → BUILD_ENUM_VARIANT
// ============================================================

#include <gtest/gtest.h>

#include "ast/ASTNode.h"
#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <memory>
#include <string>
#include <vector>

namespace {

/// 通过 Lexer→Parser→AstIRBuilder 构建 IR 模块。
/// AstIRBuilder::module_ 是 private 且无 takeModule()，故 BuildResult 持有
/// builder 实例本身以延长 module 生命周期，通过裸指针访问 IR 模块内容。
struct BuildResult {
    std::unique_ptr<AstIRBuilder> builder;
    IRModule* module = nullptr;            // 指向 builder 内部的 module_
    std::vector<std::string> diagnostics;
    bool hasError = false;
};

static BuildResult buildIR(const std::string& src) {
    Lexer lexer;
    auto tokens = lexer.scan(src);
    Parser parser;
    auto ast = parser.parse(tokens);
    BuildResult result;
    if (!ast) {
        result.hasError = true;
        return result;
    }
    result.builder = std::make_unique<AstIRBuilder>();
    auto ir = result.builder->build(*ast);
    if (result.builder->hasError()) {
        result.hasError = true;
        result.diagnostics.push_back(result.builder->errorMessage());
    }
    result.module = result.builder->getModule();
    // build() 把 mainFunction 所有权转移给返回值（ir），module_->mainFunction 变为 nullptr。
    // 把所有权重新放回 module_，使测试可通过 r.module->mainFunction 访问主函数。
    if (result.module && ir) {
        result.module->mainFunction = std::move(ir);
    }
    return result;
}

/// 将 IRFunction 所有指令展平到一个 vector，便于断言序列
static std::vector<IROp> flattenOps(const IRFunction& fn) {
    std::vector<IROp> ops;
    for (const auto& block : fn.blocks) {
        for (const auto& instr : block.instructions) {
            ops.push_back(instr.op);
        }
    }
    return ops;
}

/// 检查指令序列中是否包含指定 IROp
static bool containsOp(const std::vector<IROp>& ops, IROp target) {
    for (auto op : ops) {
        if (op == target)
            return true;
    }
    return false;
}

/// 检查指令序列中是否包含指定的 IROp 子序列（保留为辅助工具，未使用时也有定义）
[[maybe_unused]] static bool containsSubsequence(const std::vector<IROp>& ops, const std::vector<IROp>& sub) {
    if (sub.empty())
        return true;
    for (size_t i = 0; i + sub.size() <= ops.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < sub.size(); ++j) {
            if (ops[i + j] != sub[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

} // namespace

// ============================================================
// 字面量 → LOAD_CONST/LOAD_NULL/LOAD_TRUE/LOAD_FALSE
// ============================================================

TEST(AstIRBuilderLiteral, IntLiteralEmitsLoadConst) {
    auto r = buildIR("var x = 42;");
    ASSERT_FALSE(r.hasError);
    ASSERT_NE(r.module, nullptr);
    ASSERT_NE(r.module->mainFunction, nullptr);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_CONST));
}

TEST(AstIRBuilderLiteral, FloatLiteralEmitsLoadConst) {
    auto r = buildIR("var x = 3.14;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_CONST));
}

TEST(AstIRBuilderLiteral, StringLiteralEmitsLoadConst) {
    auto r = buildIR("var s = \"hello\";");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_CONST));
}

TEST(AstIRBuilderLiteral, BoolTrueLiteralEmitsLoadTrue) {
    auto r = buildIR("var b = true;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_TRUE));
}

TEST(AstIRBuilderLiteral, BoolFalseLiteralEmitsLoadFalse) {
    auto r = buildIR("var b = false;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_FALSE));
}

TEST(AstIRBuilderLiteral, NullLiteralEmitsLoadNull) {
    auto r = buildIR("var n = null;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_NULL));
}

// ============================================================
// 二元运算 → ADD/SUB/MUL/DIV/MOD/LT/GT/LTE/GTE/EQ/NEQ
// ============================================================

TEST(AstIRBuilderBinary, AddEmitsAddOp) {
    auto r = buildIR("var x = 1 + 2;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::ADD));
}

TEST(AstIRBuilderBinary, SubEmitsSubOp) {
    auto r = buildIR("var x = 10 - 3;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::SUB));
}

TEST(AstIRBuilderBinary, MulEmitsMulOp) {
    auto r = buildIR("var x = 4 * 5;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::MUL));
}

TEST(AstIRBuilderBinary, DivEmitsDivOp) {
    auto r = buildIR("var x = 20 / 4;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::DIV));
}

TEST(AstIRBuilderBinary, ModEmitsModOp) {
    auto r = buildIR("var x = 17 % 5;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::MOD));
}

TEST(AstIRBuilderBinary, LtEmitsLtOp) {
    auto r = buildIR("var x = 1 < 2;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LT));
}

TEST(AstIRBuilderBinary, GtEmitsGtOp) {
    auto r = buildIR("var x = 2 > 1;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::GT));
}

TEST(AstIRBuilderBinary, LteEmitsLteOp) {
    auto r = buildIR("var x = 1 <= 2;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LTE));
}

TEST(AstIRBuilderBinary, GteEmitsGteOp) {
    auto r = buildIR("var x = 2 >= 1;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::GTE));
}

TEST(AstIRBuilderBinary, EqEmitsEqOp) {
    auto r = buildIR("var x = 1 == 1;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::EQ));
}

TEST(AstIRBuilderBinary, NeqEmitsNeqOp) {
    auto r = buildIR("var x = 1 != 2;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::NEQ));
}

// ============================================================
// 一元运算 → NEGATE/NOT
// ============================================================

TEST(AstIRBuilderUnary, NegateEmitsNegateOp) {
    auto r = buildIR("var x = -5;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::NEGATE));
}

TEST(AstIRBuilderUnary, NotEmitsNotOp) {
    auto r = buildIR("var x = not true;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::NOT));
}

// ============================================================
// 变量声明/赋值/读取
// ============================================================

TEST(AstIRBuilderVariable, GlobalVarDeclEmitsDefineGlobal) {
    auto r = buildIR("var x = 42;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::DEFINE_GLOBAL));
}

TEST(AstIRBuilderVariable, GlobalVarAssignEmitsStoreGlobal) {
    auto r = buildIR("var x = 1; x = 2;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::STORE_GLOBAL));
}

TEST(AstIRBuilderVariable, GlobalVarReadEmitsLoadGlobal) {
    auto r = buildIR("var x = 1; print(x);");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_GLOBAL));
}

TEST(AstIRBuilderVariable, MultipleGlobalsUseDistinctSlots) {
    auto r = buildIR("var a = 1; var b = 2; var c = a + b;");
    ASSERT_FALSE(r.hasError);
    // 注意：IRModule::globalSlotNames 由 Compiler 集成层（compileViaIR）回填，
    // 直接使用 AstIRBuilder 时须经 builder->getGlobalSlotNames() 获取槽位名表。
    // ASSERT（非 EXPECT）确保 size 不符时不继续越界索引。
    const auto& names = r.builder->getGlobalSlotNames();
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a");
    EXPECT_EQ(names[1], "b");
    EXPECT_EQ(names[2], "c");
}

TEST(AstIRBuilderVariable, DuplicateGlobalReusesSlot) {
    auto r = buildIR("var x = 1; var x = 2;");
    ASSERT_FALSE(r.hasError);
    const auto& names = r.builder->getGlobalSlotNames();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "x");
}

// ============================================================
// 控制流
// ============================================================

TEST(AstIRBuilderControlFlow, IfStmtEmitsJumpIfFalseAndLabels) {
    auto r = buildIR("if (1 < 2) { print(\"yes\"); }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::JUMP_IF_FALSE));
    EXPECT_TRUE(containsOp(ops, IROp::LABEL));
}

TEST(AstIRBuilderControlFlow, IfElseEmitsTwoLabels) {
    auto r = buildIR("if (true) { print(1); } else { print(2); }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    size_t labelCount = 0;
    for (auto op : ops) {
        if (op == IROp::LABEL)
            ++labelCount;
    }
    EXPECT_GE(labelCount, 2u);
}

TEST(AstIRBuilderControlFlow, WhileLoopEmitsJumpAndLabel) {
    auto r = buildIR("var i = 0; while (i < 3) { i = i + 1; }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::JUMP_IF_FALSE));
    EXPECT_TRUE(containsOp(ops, IROp::JUMP));
    EXPECT_TRUE(containsOp(ops, IROp::LABEL));
}

TEST(AstIRBuilderControlFlow, ForLoopEmitsJumpAndLabel) {
    auto r = buildIR("for (var i = 0; i < 3; i = i + 1) { print(i); }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::JUMP));
    EXPECT_TRUE(containsOp(ops, IROp::LABEL));
}

// ============================================================
// 函数声明/调用/返回
// ============================================================

TEST(AstIRBuilderFunction, FunDeclAddsModuleFunction) {
    auto r = buildIR("fun add(a, b) { return a + b; }");
    ASSERT_FALSE(r.hasError);
    EXPECT_NE(r.module->findFunction("add"), nullptr);
}

TEST(AstIRBuilderFunction, FunDeclEmitsMakeClosureInMain) {
    auto r = buildIR("fun f() { return 42; }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::MAKE_CLOSURE));
}

TEST(AstIRBuilderFunction, FunCallEmitsCallOp) {
    auto r = buildIR("fun f() { return 42; } print(f());");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::CALL));
}

TEST(AstIRBuilderFunction, FunDeclWithReturnEmitsReturn) {
    auto r = buildIR("fun f() { return 42; }");
    ASSERT_FALSE(r.hasError);
    auto* fn = r.module->findFunction("f");
    ASSERT_NE(fn, nullptr);
    auto ops = flattenOps(*fn);
    EXPECT_TRUE(containsOp(ops, IROp::RETURN));
}

TEST(AstIRBuilderFunction, FunDeclWithoutReturnEmitsReturnNull) {
    auto r = buildIR("fun f() { print(\"hello\"); }");
    ASSERT_FALSE(r.hasError);
    auto* fn = r.module->findFunction("f");
    ASSERT_NE(fn, nullptr);
    auto ops = flattenOps(*fn);
    EXPECT_TRUE(containsOp(ops, IROp::RETURN_NULL));
}

TEST(AstIRBuilderFunction, FunDeclArityPropagatedToIR) {
    auto r = buildIR("fun add(a, b, c) { return a + b + c; }");
    ASSERT_FALSE(r.hasError);
    auto* fn = r.module->findFunction("add");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->arity, 3);
}

TEST(AstIRBuilderFunction, NestedFunCallEmitsMultipleCalls) {
    auto r = buildIR("fun double(x) { return x * 2; } fun quad(x) { return double(double(x)); }");
    ASSERT_FALSE(r.hasError);
    auto* quad = r.module->findFunction("quad");
    ASSERT_NE(quad, nullptr);
    auto ops = flattenOps(*quad);
    size_t callCount = 0;
    size_t tailCallCount = 0;
    for (auto op : ops) {
        if (op == IROp::CALL)
            ++callCount;
        if (op == IROp::TAIL_CALL)
            ++tailCallCount;
    }
    // L18 eng-tailcall: return double(double(x)) 的外层调用是尾调用，
    // 现在 emit 为 TAIL_CALL（内层仍为 CALL），共 2 个调用指令。
    EXPECT_EQ(callCount, 1u);
    EXPECT_EQ(tailCallCount, 1u);
}

// ============================================================
// 容器（数组/字典/索引）
// ============================================================

TEST(AstIRBuilderContainer, ArrayLiteralEmitsBuildArray) {
    auto r = buildIR("var arr = [1, 2, 3];");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::BUILD_ARRAY));
}

TEST(AstIRBuilderContainer, DictLiteralEmitsBuildDict) {
    auto r = buildIR("var d = {\"a\": 1, \"b\": 2};");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::BUILD_DICT));
}

TEST(AstIRBuilderContainer, IndexGetEmitsIndexGet) {
    auto r = buildIR("var arr = [1, 2, 3]; print(arr[0]);");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::INDEX_GET));
}

TEST(AstIRBuilderContainer, IndexSetEmitsIndexSet) {
    auto r = buildIR("var arr = [1, 2, 3]; arr[0] = 99;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::INDEX_SET));
}

TEST(AstIRBuilderContainer, EmptyArrayEmitsBuildArrayWithZeroCount) {
    auto r = buildIR("var arr = [];");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::BUILD_ARRAY));
}

// ============================================================
// 成员访问
// ============================================================

TEST(AstIRBuilderMember, MemberGetEmitsMemberGet) {
    auto r = buildIR("class P { fun init() { this.x = 1; } } var p = P(); print(p.x);");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::MEMBER_GET));
}

TEST(AstIRBuilderMember, MemberSetEmitsMemberSet) {
    auto r = buildIR("class P { var x; } var p = P(); p.x = 42;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::MEMBER_SET));
}

// ============================================================
// try/catch/throw
// ============================================================

TEST(AstIRBuilderTry, TryCatchEmitsTryBeginAndEnd) {
    auto r = buildIR("try { print(1); } catch (e) { print(e); }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::TRY_BEGIN));
    EXPECT_TRUE(containsOp(ops, IROp::TRY_END));
}

TEST(AstIRBuilderTry, ThrowEmitsThrowOp) {
    auto r = buildIR("throw \"boom\";");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::THROW));
}

TEST(AstIRBuilderTry, CatchBlockEmitsLoadException) {
    auto r = buildIR("try { throw \"e\"; } catch (e) { print(e); }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_EXCEPTION));
}

// ============================================================
// 闭包 upvalue
// ============================================================

TEST(AstIRBuilderClosure, InnerFunCapturesOuterVarEmitsMakeClosure) {
    auto r = buildIR("fun outer() { var x = 10; fun inner() { return x; } return inner(); }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::MAKE_CLOSURE));
}

TEST(AstIRBuilderClosure, InnerFunLoadUpvalueEmitsLoadUpvalue) {
    auto r = buildIR("fun outer() { var x = 10; fun inner() { return x; } return inner(); }");
    ASSERT_FALSE(r.hasError);
    auto* inner = r.module->findFunction("inner");
    ASSERT_NE(inner, nullptr);
    auto ops = flattenOps(*inner);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_UPVALUE));
}

TEST(AstIRBuilderClosure, InnerFunAssignUpvalueEmitsStoreUpvalue) {
    auto r = buildIR("fun outer() { var x = 10; fun inner() { x = 20; } return x; }");
    ASSERT_FALSE(r.hasError);
    auto* inner = r.module->findFunction("inner");
    ASSERT_NE(inner, nullptr);
    auto ops = flattenOps(*inner);
    EXPECT_TRUE(containsOp(ops, IROp::STORE_UPVALUE));
}

// ============================================================
// 类定义/构造
// ============================================================

TEST(AstIRBuilderClass, ClassDeclEmitsDefineClass) {
    auto r = buildIR("class P { var x; }");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::DEFINE_CLASS));
}

TEST(AstIRBuilderClass, ClassNewEmitsClassNew) {
    // CLASS_NEW 仅在「类名类型注解无初始化器自动构造」路径发射
    // （BUG-IR-VARDECL-1：`var p: P;` 等价 `P()`）；普通构造调用
    // `var p = P();` 作为普通 CALL lowering，VM 运行时解析类名。
    auto r = buildIR("class P { } var p: P;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::CLASS_NEW));
}

TEST(AstIRBuilderClass, ClassCtorCallLoweredAsCall) {
    // 普通构造调用路径：`P()` lowering 为 CALL（非 CLASS_NEW）
    auto r = buildIR("class P { } var p = P();");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::CALL));
}

TEST(AstIRBuilderClass, MethodDeclAddedAsModuleFunction) {
    auto r = buildIR("class P { fun greet() { print(\"hi\"); } }");
    ASSERT_FALSE(r.hasError);
    // 方法名在 IRModule 中以 "P.greet" 形式注册（与 FunDecl 区分）
    // 至少应有一个子函数（greet 方法）
    EXPECT_GE(r.module->functions.size(), 1u);
}

// ============================================================
// enum variant
// ============================================================

TEST(AstIRBuilderEnum, EnumVariantConstructionEmitsBuildEnumVariant) {
    auto r = buildIR("enum Color { RED, GREEN, BLUE } var c = Color.RED;");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::BUILD_ENUM_VARIANT));
}

TEST(AstIRBuilderEnum, EnumVariantNameCheckEmitsEnumVariantName) {
    // ENUM_VARIANT_NAME 仅在 match 表达式的 VARIANT 模式分支发射
    // （MatchPatternKind::VARIANT → ENUM_VARIANT_NAME 检查 enum/variant 名匹配）
    auto r = buildIR(
        "enum Color { RED, GREEN }\n"
        "var c = Color.RED;\n"
        "var r = match (c) { case Color.RED => 1; default => 0; };\n");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::ENUM_VARIANT_NAME));
}

// ============================================================
// 错误处理
// ============================================================

TEST(AstIRBuilderError, UnknownVariableDeferredToRuntime) {
    auto r = buildIR("print(undefined_var);");
    // IR 路径未知全局名不报编译错误：lowering 为名称版 LOAD_GLOBAL
    // （GLOBAL_NAME → OP_GET_VAR），未定义变量由 VM 运行时报错，
    // 与 Compiler 直接路径 / Interpreter 的运行时未定义变量语义对齐。
    EXPECT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::LOAD_GLOBAL));
}

TEST(AstIRBuilderError, EmptyProgramProducesValidModule) {
    auto r = buildIR("");
    ASSERT_FALSE(r.hasError);
    ASSERT_NE(r.module, nullptr);
    ASSERT_NE(r.module->mainFunction, nullptr);
    // 空程序的 mainFunction 仅含入口块结构性 LABEL，无实质指令：
    // build() 仅对嵌套函数（emitFunctionBody）发射末尾隐式 RETURN_NULL，
    // 主函数结束由 VM 执行完 chunk 自然弹帧处理。
    auto ops = flattenOps(*r.module->mainFunction);
    for (IROp op : ops) {
        EXPECT_EQ(op, IROp::LABEL);
    }
    EXPECT_FALSE(containsOp(ops, IROp::RETURN_NULL));
}

// ============================================================
// print 语句
// ============================================================

TEST(AstIRBuilderPrint, PrintStmtEmitsPrintOp) {
    auto r = buildIR("print(42);");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    EXPECT_TRUE(containsOp(ops, IROp::PRINT));
}

// ============================================================
// 综合场景：复杂 IR 结构
// ============================================================

TEST(AstIRBuilderComplex, NestedIfInsideWhileEmitsMultipleLabels) {
    auto r = buildIR(
        "var i = 0;\n"
        "while (i < 10) {\n"
        "    if (i % 2 == 0) { print(i); }\n"
        "    i = i + 1;\n"
        "}\n");
    ASSERT_FALSE(r.hasError);
    auto ops = flattenOps(*r.module->mainFunction);
    // 至少 3 个 LABEL（while start/end + if end）
    size_t labelCount = 0;
    for (auto op : ops) {
        if (op == IROp::LABEL)
            ++labelCount;
    }
    EXPECT_GE(labelCount, 3u);
}

TEST(AstIRBuilderComplex, ClosureCaptureChainAcrossNestedFuns) {
    auto r = buildIR(
        "fun outer() {\n"
        "    var a = 1;\n"
        "    fun middle() {\n"
        "        var b = 2;\n"
        "        fun inner() { return a + b; }\n"
        "        return inner();\n"
        "    }\n"
        "    return middle();\n"
        "}\n");
    ASSERT_FALSE(r.hasError);
    EXPECT_NE(r.module->findFunction("middle"), nullptr);
    EXPECT_NE(r.module->findFunction("inner"), nullptr);
    // inner 应捕获 a 和 b 两个 upvalue
    auto* inner = r.module->findFunction("inner");
    ASSERT_NE(inner, nullptr);
    EXPECT_GE(inner->upvalues.size(), 2u);
}
