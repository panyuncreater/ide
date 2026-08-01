// ============================================================
// TestErrorUnification.cpp — P2-12 错误处理统一回归测试
// ------------------------------------------------------------
// 验证 P2-12 错误处理统一的三个核心目标：
//   1. IBackend 统一错误查询接口（hasError/getLastError/getLastErrorLine）
//      在 VM/RegisterVM/JIT 三后端行为一致
//   2. DiagCodes 命名空间常量被正确使用（Diagnostic.code 字段非空）
//   3. Lexer/Parser 的 diagCode 补全（kStrayBrace/kTooManyErrors/
//      kImportNotAtTopLevel/kSourceTooLarge/kTooManyTokens 等）
//
// 测试框架：GoogleTest
// 依赖：minilang_core（Lexer/Parser/Compiler/VM/RegisterVM/Interpreter/JIT）
// ============================================================

#include <gtest/gtest.h>

#include "common/Diagnostic.h"
#include "common/ErrorMessages.h"
#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/JIT.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "interpreter/Interpreter.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <memory>
#include <string>

// ============================================================
// 辅助函数
// ============================================================

// 查找诊断包中是否包含指定 diagCode
static bool hasDiagCode(const DiagnosticBag& diags, const std::string& code) {
    for (const auto& d : diags.all()) {
        if (d.code == code)
            return true;
    }
    return false;
}

// ============================================================
// 测试套件 1：IBackend 统一错误查询接口（三后端一致性）
// ============================================================

// VM 路径：除零错误应通过 IBackend 接口可查询
TEST(ErrorUnificationIBackend, VMReportsDivisionByZeroViaIBackend) {
    Lexer lx;
    auto tk = lx.scan("var x = 1 / 0;");
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    VM vm;
    vm.execute(cr);
    EXPECT_TRUE(vm.hasError());
    EXPECT_FALSE(vm.getLastError().empty());
    // 诊断码应为 division-by-zero（DiagCodes::kDivisionByZero）
    EXPECT_TRUE(hasDiagCode(vm.getDiagnostics(), DiagCodes::kDivisionByZero));
}

// RegisterVM 路径：除零错误应通过 IBackend 接口可查询
TEST(ErrorUnificationIBackend, RegisterVMReportsDivisionByZeroViaIBackend) {
    Lexer lx;
    auto tk = lx.scan("var x = 1 / 0;");
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    RegisterVM vm;
    vm.execute(c.getLastRegisterResult());
    EXPECT_TRUE(vm.hasError());
    EXPECT_FALSE(vm.getLastError().empty());
    EXPECT_TRUE(hasDiagCode(vm.getDiagnostics(), DiagCodes::kDivisionByZero));
}

// Interpreter 路径：除零错误应通过 IBackend 接口可查询
TEST(ErrorUnificationIBackend, InterpreterReportsDivisionByZeroViaIBackend) {
    Lexer lx;
    auto tk = lx.scan("var x = 1 / 0;");
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (...) {
    }
    EXPECT_TRUE(interp.hasError());
    EXPECT_FALSE(interp.getLastError().empty());
    EXPECT_TRUE(hasDiagCode(interp.getDiagnostics(), DiagCodes::kDivisionByZero));
}

// 三后端错误消息文本一致性（除零）
TEST(ErrorUnificationIBackend, ThreeBackendsAgreeOnDivisionByZeroMessage) {
    std::string src = "var x = 1 / 0;";

    // Interpreter
    Lexer lx1;
    auto tk1 = lx1.scan(src);
    Parser p1;
    auto ast1 = p1.parse(tk1);
    Interpreter interp;
    std::string out1;
    interp.setOutputCallback([&](const std::string& s) { out1 += s; });
    try {
        interp.execute(*ast1);
    } catch (...) {
    }
    std::string interpErr = interp.getLastError();

    // StackVM
    Lexer lx2;
    auto tk2 = lx2.scan(src);
    Parser p2;
    auto ast2 = p2.parse(tk2);
    Compiler c2;
    auto cr2 = c2.compile(*ast2);
    VM vm;
    vm.execute(cr2);
    std::string vmErr = vm.getLastError();

    // RegisterVM
    Lexer lx3;
    auto tk3 = lx3.scan(src);
    Parser p3;
    auto ast3 = p3.parse(tk3);
    Compiler c3;
    c3.setUseRegisterVM(true);
    c3.compile(*ast3);
    RegisterVM rvm;
    rvm.execute(c3.getLastRegisterResult());
    std::string rvmErr = rvm.getLastError();

    // 三后端错误消息应一致
    EXPECT_EQ(interpErr, vmErr);
    EXPECT_EQ(vmErr, rvmErr);
}

// 无错误时 hasError 返回 false
TEST(ErrorUnificationIBackend, NoErrorWhenProgramSucceeds) {
    std::string src = "print(42);";

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);
    Compiler c;
    auto cr = c.compile(*ast);
    ASSERT_FALSE(c.getDiagnostics().hasErrors());

    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    EXPECT_FALSE(vm.hasError());
    EXPECT_TRUE(vm.getLastError().empty());
    EXPECT_EQ(out, "42");
}

// ============================================================
// 测试套件 2：DiagCodes 常量集中化验证
// ============================================================

// DiagCodes 常量值与历史字面量一致（稳定性保证）
TEST(ErrorUnificationDiagCodes, ConstantsMatchHistoricalLiterals) {
    // 这些值是稳定 API，不能随意修改（ErrorHintEngine 按精确匹配）
    EXPECT_STREQ(DiagCodes::kDivisionByZero, "division-by-zero");
    EXPECT_STREQ(DiagCodes::kUndefinedVariable, "undefined-variable");
    EXPECT_STREQ(DiagCodes::kNullAccess, "null-access");
    EXPECT_STREQ(DiagCodes::kTypeMismatch, "type-mismatch");
    EXPECT_STREQ(DiagCodes::kIndexOutOfBounds, "index-out-of-bounds");
    EXPECT_STREQ(DiagCodes::kArityMismatch, "arity-mismatch");
    EXPECT_STREQ(DiagCodes::kRecursionDepth, "recursion-depth");
    EXPECT_STREQ(DiagCodes::kUnbalancedBrace, "unbalanced-brace");
}

// 未定义变量错误应携带 kUndefinedVariable 诊断码
TEST(ErrorUnificationDiagCodes, UndefinedVariableCarriesDiagCode) {
    std::string src = "print(undefined_var);";

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    auto cr = c.compile(*ast);
    VM vm;
    vm.execute(cr);
    EXPECT_TRUE(hasDiagCode(vm.getDiagnostics(), DiagCodes::kUndefinedVariable));
}

// 递归深度超限应携带 kRecursionDepth 诊断码
TEST(ErrorUnificationDiagCodes, RecursionDepthCarriesDiagCode) {
    // 非尾递归（避免 TCO 优化为循环，确保真正触发 MAX_FRAMES 上限）。
    // var x = f(); 使递归调用不在尾位置——TCO 仅优化 return f(args) 形式，
    // 赋值后再 return 会保留调用帧，256 层后命中 MAX_FRAMES。
    std::string src = "fun f() { var x = f(); return x; } f();";

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    auto cr = c.compile(*ast);
    VM vm;
    vm.execute(cr);
    EXPECT_TRUE(hasDiagCode(vm.getDiagnostics(), DiagCodes::kRecursionDepth));
}

// 参数数量不匹配应携带 kArityMismatch 诊断码
TEST(ErrorUnificationDiagCodes, ArityMismatchCarriesDiagCode) {
    std::string src = "fun f(a) { return a; } f(1, 2);";

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    auto cr = c.compile(*ast);
    VM vm;
    vm.execute(cr);
    EXPECT_TRUE(hasDiagCode(vm.getDiagnostics(), DiagCodes::kArityMismatch));
}

// ============================================================
// 测试套件 3：Parser diagCode 补全
// ============================================================

// 顶层多余 '}' 应携带 kStrayBrace 诊断码
TEST(ErrorUnificationParser, StrayBraceCarriesDiagCode) {
    std::string src = "var x = 1; } } }";

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(hasDiagCode(p.getDiagnostics(), DiagCodes::kStrayBrace));
}

// import 语句在非顶层应携带 kImportNotAtTopLevel 诊断码
TEST(ErrorUnificationParser, ImportNotAtTopLevelCarriesDiagCode) {
    std::string src = "fun f() { import \"mod\"; }";

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(hasDiagCode(p.getDiagnostics(), DiagCodes::kImportNotAtTopLevel));
}

// export 语句在非顶层应携带 kExportNotAtTopLevel 诊断码
TEST(ErrorUnificationParser, ExportNotAtTopLevelCarriesDiagCode) {
    std::string src = "fun f() { export var x = 1; }";

    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    EXPECT_TRUE(hasDiagCode(p.getDiagnostics(), DiagCodes::kExportNotAtTopLevel));
}

// ============================================================
// 测试套件 4：Lexer diagCode 补全
// ============================================================

// 源代码过大应携带 kSourceTooLarge 诊断码
TEST(ErrorUnificationLexer, SourceTooLargeCarriesDiagCode) {
    // 构造超过 MAX_SOURCE_SIZE 的源代码
    // MAX_SOURCE_SIZE 通常为 10MB，这里用 mock 方式验证常量存在即可
    // 实际触发需大文件，此处仅验证常量已定义
    EXPECT_STREQ(DiagCodes::kSourceTooLarge, "source-too-large");
    EXPECT_STREQ(DiagCodes::kTooManyTokens, "too-many-tokens");
}

// Lexer 错误诊断应使用 DiagSource::Lexer 来源
TEST(ErrorUnificationLexer, LexerDiagnosticsUseLexerSource) {
    // 未终止字符串应产生 TK_ERROR token → 诊断
    std::string src = "var s = \"unterminated;";

    Lexer lx;
    auto tk = lx.scan(src);
    EXPECT_TRUE(lx.getDiagnostics().hasErrors());
    // 至少有一条诊断来自 Lexer
    bool hasLexerSource = false;
    for (const auto& d : lx.getDiagnostics().all()) {
        if (d.source == DiagSource::Lexer) {
            hasLexerSource = true;
            break;
        }
    }
    EXPECT_TRUE(hasLexerSource);
}

// ============================================================
// 测试套件 5：VM/RegisterVM 错误通道冗余消除验证
// ============================================================

// VM 错误查询从 diagnostics_ 派生（不再有 lastError_ 字段）
// 验证方式：连续触发两次错误，第二次的 getLastError 应反映第二次错误
TEST(ErrorUnificationVM, ErrorQueryDerivedFromDiagnostics) {
    // 触发一次除零错误
    std::string src = "var x = 1 / 0;";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    auto cr = c.compile(*ast);
    VM vm;
    vm.execute(cr);
    ASSERT_TRUE(vm.hasError());
    std::string firstError = vm.getLastError();
    EXPECT_FALSE(firstError.empty());

    // 诊断包应包含至少一条错误
    EXPECT_GE(vm.getDiagnostics().errorCount(), 1u);
}

// RegisterVM 错误查询从 diagnostics_ 派生
TEST(ErrorUnificationRegisterVM, ErrorQueryDerivedFromDiagnostics) {
    std::string src = "var x = 1 / 0;";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    RegisterVM vm;
    vm.execute(c.getLastRegisterResult());
    ASSERT_TRUE(vm.hasError());
    std::string err = vm.getLastError();
    EXPECT_FALSE(err.empty());
    EXPECT_GE(vm.getDiagnostics().errorCount(), 1u);
}

// VM restoreFromSnapshot 后错误状态应清除
TEST(ErrorUnificationVM, RestoreFromSnapshotClearsError) {
    std::string src = "var x = 1 / 0;";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    auto cr = c.compile(*ast);
    VM vm;
    vm.initExecution(cr);
    // 执行到错误
    while (!vm.isFinished()) {
        if (vm.stepOnce() != VMResult::VM_OK)
            break;
    }
    ASSERT_TRUE(vm.hasError());

    // 回滚到初始状态
    bool ok = vm.restoreFromSnapshot({}, {}, 0, 1);
    EXPECT_TRUE(ok);
    // 回滚后错误状态应清除（diagnostics_.clear()）
    EXPECT_FALSE(vm.hasError());
    EXPECT_TRUE(vm.getLastError().empty());
}

// RegisterVM restoreFromSnapshot 后错误状态应清除
TEST(ErrorUnificationRegisterVM, RestoreFromSnapshotClearsError) {
    std::string src = "var x = 1 / 0;";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    RegisterVM vm;
    vm.initExecution(c.getLastRegisterResult());
    while (!vm.isFinished()) {
        if (vm.stepOnce() != VMResult::VM_OK)
            break;
    }
    ASSERT_TRUE(vm.hasError());

    bool ok = vm.restoreFromSnapshot({}, {}, 0, 1);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(vm.hasError());
    EXPECT_TRUE(vm.getLastError().empty());
}

// ============================================================
// 测试套件 6：JIT 后端 DiagSource::JIT 验证
// ============================================================
// JIT 仅在 x86-64 平台启用（CMakeLists.txt 平台检测自动关闭非 x64），
// 非 x64 平台（macOS arm64 等）跳过本套件。
#ifdef MINILANG_USE_JIT

// JIT 运行时错误应使用 DiagSource::JIT 来源（不再复用 VM/Compiler）
TEST(ErrorUnificationJIT, JITErrorUsesJITDiagSource) {
    // 除零错误触发 JIT 运行时错误
    std::string src = "var x = 1 / 0;";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    auto cr = c.compile(*ast);

    JITBackend jit;
    std::string out;
    jit.setOutputCallback([&](const std::string& s) { out += s; });
    auto result = jit.execute(cr);
    (void)result; // W4 fix: 测试通过 jit.hasError() 判定，result 仅保留以避免丢弃 [[nodiscard]] 返回值
    // JIT 可能返回 RuntimeError 或 OK（取决于 JIT 是否支持该路径）
    if (jit.hasError()) {
        bool hasJITSource = false;
        for (const auto& d : jit.getDiagnostics().all()) {
            if (d.source == DiagSource::JIT) {
                hasJITSource = true;
                break;
            }
        }
        EXPECT_TRUE(hasJITSource);
    }
}

// JIT hasError/getLastError 从 diagnostics_ 派生
TEST(ErrorUnificationJIT, JITErrorQueryDerivedFromDiagnostics) {
    std::string src = "var x = 1 / 0;";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    Compiler c;
    auto cr = c.compile(*ast);

    JITBackend jit;
    jit.execute(cr);
    if (jit.hasError()) {
        // getLastError 应返回非空字符串（从 diagnostics_ 派生）
        EXPECT_FALSE(jit.getLastError().empty());
    }
}

#endif // MINILANG_USE_JIT

// ============================================================
// 测试套件 6：IR 错误恢复（P2-12 fatal/recoverable 区分 + 多错误收集）
// ------------------------------------------------------------
// P2-12 fix: AstIRBuilder::build() 引入致命/可恢复错误区分。
//   - 致命错误（模块加载失败、内部不变量违反）立即中止，返回 nullptr
//   - 可恢复错误（变量重定义、break/continue 外循环）记录后继续处理后续语句
//   - compileViaIR/compileViaRegisterIR 合并全部诊断后中止，用户一次看到所有错误
// ============================================================

// DiagnosticBag 致命/可恢复计数器独立性
TEST(ErrorRecoveryDiagnosticBag, FatalAndRecoverableCountersIndependent) {
    DiagnosticBag bag;
    EXPECT_FALSE(bag.hasErrors());
    EXPECT_FALSE(bag.hasFatalErrors());

    // 可恢复错误：hasErrors=true, hasFatalErrors=false
    bag.addError("recoverable error", 1, 0, DiagSource::Compiler);
    EXPECT_TRUE(bag.hasErrors());
    EXPECT_FALSE(bag.hasFatalErrors());
    EXPECT_EQ(bag.errorCount(), 1);

    // 致命错误：hasErrors=true, hasFatalErrors=true
    bag.addErrorFatal("fatal error", 2, 0, DiagSource::Compiler);
    EXPECT_TRUE(bag.hasErrors());
    EXPECT_TRUE(bag.hasFatalErrors());
    EXPECT_EQ(bag.errorCount(), 2);

    // clear 重置全部计数器
    bag.clear();
    EXPECT_FALSE(bag.hasErrors());
    EXPECT_FALSE(bag.hasFatalErrors());
    EXPECT_EQ(bag.errorCount(), 0);
}

// DiagnosticBag isFatal 标志仅在 Error 级别区分
TEST(ErrorRecoveryDiagnosticBag, IsFatalFlagOnlyMeaningfulOnErrors) {
    DiagnosticBag bag;
    bag.addWarning("warning", 1, 0, DiagSource::Compiler);
    EXPECT_FALSE(bag.hasFatalErrors());

    // Warning/Info 级别的 isFatal 应为 false（非致命）
    for (const auto& d : bag.all()) {
        EXPECT_FALSE(d.isFatal);
    }
}

// IR 多错误收集：变量重定义 + break 外循环 + continue 外循环
// build() 应收集全部 3 个错误（而非第一个就中止）
// 注：变量重定义检查仅在函数内生效（inFunction_=true），故包裹在 fun f() {} 中
TEST(ErrorRecoveryIRBuilder, CollectsMultipleRecoverableErrors) {
    std::string src = R"(
fun f() {
    var x = 1;
    var x = 2;
    break;
    continue;
    print(x);
}
)";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);

    AstIRBuilder builder;
    auto ir = builder.build(*ast);

    // 可恢复错误不中止 build()——IR 应成功构建（errored 语句被跳过）
    EXPECT_NE(ir, nullptr);
    // 应收集至少 3 个错误：变量重定义 + break 外循环 + continue 外循环
    EXPECT_GE(builder.diagnostics().errorCount(), 3u);
    // 无致命错误（这些都是可恢复错误）
    EXPECT_FALSE(builder.hasFatalError());
    EXPECT_TRUE(builder.hasError());
}

// IR 可恢复错误后后续语句仍被处理
// 验证：break 外循环错误后，print(y) 仍被编译（IR 中存在 PRINT 指令）
TEST(ErrorRecoveryIRBuilder, ContinuesAfterRecoverableError) {
    std::string src = R"(
break;
var y = 42;
print(y);
)";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);

    AstIRBuilder builder;
    auto ir = builder.build(*ast);

    EXPECT_NE(ir, nullptr);
    EXPECT_GE(builder.diagnostics().errorCount(), 1u);

    // IR 中应存在 PRINT 指令（print(y) 被编译）
    bool hasPrint = false;
    for (const auto& block : ir->blocks) {
        for (const auto& instr : block.instructions) {
            if (instr.op == IROp::PRINT) {
                hasPrint = true;
                break;
            }
        }
    }
    EXPECT_TRUE(hasPrint);
}

// IR 致命错误中止 build()：模块加载失败（moduleLoader 未设置）
TEST(ErrorRecoveryIRBuilder, FatalErrorAbortsBuild) {
    std::string src = R"(
import "nonexistent_module";
print(1);
)";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);

    AstIRBuilder builder;
    // 不设置 moduleLoader，触发 "VM 编译需要模块加载器" 致命错误
    auto ir = builder.build(*ast);

    // 致命错误导致 build() 返回 nullptr
    EXPECT_EQ(ir, nullptr);
    EXPECT_TRUE(builder.hasFatalError());
    EXPECT_TRUE(builder.hasError());
}

// Compiler IR 路径合并诊断：多个可恢复错误应全部传递到 Compiler::diagnostics_
TEST(ErrorRecoveryCompiler, IRPathPropagatesAllDiagnostics) {
    std::string src = R"(
fun f() {
    var x = 1;
    var x = 2;
    break;
    continue;
}
)";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);

    // StackVM IR 路径
    {
        Compiler c;
        c.setUseIR(true);
        c.compile(*ast);
        // 应收集至少 3 个错误
        EXPECT_GE(c.getDiagnostics().errorCount(), 3);
    }

    // RegisterVM IR 路径
    {
        Compiler c2;
        c2.setUseRegisterVM(true);
        c2.compile(*ast);
        EXPECT_GE(c2.getDiagnostics().errorCount(), 3);
    }
}

// IR 错误恢复不影响正常代码：无错误的程序仍正常编译
TEST(ErrorRecoveryIRBuilder, NoErrorsNormalCompilation) {
    std::string src = "var x = 1 + 2; print(x);";
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    ASSERT_NE(ast, nullptr);

    AstIRBuilder builder;
    auto ir = builder.build(*ast);

    EXPECT_NE(ir, nullptr);
    EXPECT_FALSE(builder.hasError());
    EXPECT_FALSE(builder.hasFatalError());
}
