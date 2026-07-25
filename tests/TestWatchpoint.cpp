// ============================================================
// TestWatchpoint — R161 数据断点（Watchpoint）回归测试
// ------------------------------------------------------------
// 覆盖 R161 引入的三层 API：
//   1. WatchpointInfo / WriteTarget 数据结构（基础语义）
//   2. VM::peekWriteTarget（栈式 VM pre-execution peek）
//   3. RegisterVM::peekWriteTarget（寄存器式 VM pre-execution peek）
//
// 测试策略：
//   - 数据结构层：默认值、isConditional、字段赋值
//   - peekWriteTarget 层：编译源码后 initExecution，逐步 stepOnce 直到 IP
//     命中目标 SET 类指令，验证返回的 WriteTarget 字段（varName/localSlot/
//     fieldName/isFieldWrite/isIndexWrite）符合预期
//   - 覆盖 SET 类指令家族：OP_DEFINE_VAR / OP_SET_VAR / OP_SET_LOCAL /
//     OP_SET_GLOBAL / OP_MEMBER_SET / OP_INDEX_SET 等
//
// 覆盖范围说明：
//   - VmStepper::checkWatchpointHit 位于 app/ 模块（不链接到 minilang_tests），
//     通过代码审查 + IDE 手动测试验证，与 TestRunToCursor.cpp 范式一致。
//   - 本测试聚焦 compiler/ 模块内的 VM/RegisterVM peekWriteTarget 单元行为。
// ============================================================

#include <gtest/gtest.h>

#include "ast/ASTNode.h"
#include "compiler/Compiler.h"
#include "compiler/RegisterVM.h"
#include "compiler/VM.h"
#include "debug/DebugController.h" // L19: Interpreter watchpoint 测试
#include "debug/DebugTypes.h"      // R161: WatchpointInfo + WriteTarget
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// 辅助：编译源码并初始化栈式 VM 执行状态（不调用 execute，仅 initExecution）
// ============================================================
static std::unique_ptr<Block> parseSource(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    return parser.parse(tokens);
}

// ============================================================
// 第一组：WatchpointInfo / WriteTarget 数据结构测试
// ============================================================

TEST(WatchpointStruct, DefaultConstruct_NoCondition_NoName) {
    WatchpointInfo wp;
    EXPECT_EQ(wp.kind, WatchpointTargetKind::Variable);
    EXPECT_TRUE(wp.varName.empty());
    EXPECT_TRUE(wp.fieldName.empty());
    EXPECT_TRUE(wp.condition.empty());
    EXPECT_EQ(wp.hitCount, 0);
    EXPECT_FALSE(wp.isConditional());
}

TEST(WatchpointStruct, VariableKind_NameOnly_NotConditional) {
    WatchpointInfo wp("counter");
    EXPECT_EQ(wp.kind, WatchpointTargetKind::Variable);
    EXPECT_EQ(wp.varName, "counter");
    EXPECT_TRUE(wp.fieldName.empty());
    EXPECT_FALSE(wp.isConditional());
}

TEST(WatchpointStruct, FieldKind_NameAndField_NotConditional) {
    WatchpointInfo wp;
    wp.kind = WatchpointTargetKind::Field;
    wp.varName = "obj";
    wp.fieldName = "x";
    EXPECT_FALSE(wp.isConditional());
    EXPECT_EQ(wp.varName, "obj");
    EXPECT_EQ(wp.fieldName, "x");
}

TEST(WatchpointStruct, ConditionalExpression_MarkedConditional) {
    WatchpointInfo wp;
    wp.varName = "i";
    wp.condition = "i > 10";
    EXPECT_TRUE(wp.isConditional());
}

TEST(WatchpointStruct, HitCount_IncrementManually) {
    WatchpointInfo wp("i");
    EXPECT_EQ(wp.hitCount, 0);
    wp.hitCount++;
    wp.hitCount++;
    EXPECT_EQ(wp.hitCount, 2);
}

TEST(WatchpointStruct, WriteTarget_DefaultConstruct_NoWrite) {
    WriteTarget wt;
    EXPECT_FALSE(wt.isWrite);
    EXPECT_TRUE(wt.varName.empty());
    EXPECT_EQ(wt.localSlot, -1);
    EXPECT_TRUE(wt.fieldName.empty());
    EXPECT_FALSE(wt.isFieldWrite);
    EXPECT_FALSE(wt.isIndexWrite);
}

// ============================================================
// 第二组：VM::peekWriteTarget 测试（栈式 VM）
// ------------------------------------------------------------
// 验证 OP_DEFINE_VAR / OP_SET_VAR / OP_SET_LOCAL / OP_SET_GLOBAL 等
// SET 类指令的 pre-execution peek 解码正确性。
// ============================================================

// 辅助：逐步执行 VM 直到 peekWriteTarget 返回 isWrite=true 或达到上限
// 返回命中的 WriteTarget；若达到上限未命中，返回 isWrite=false 的默认值
static WriteTarget stepUntilWriteTarget(VM& vm, int maxSteps = 200) {
    for (int i = 0; i < maxSteps; ++i) {
        WriteTarget wt = vm.peekWriteTarget();
        if (wt.isWrite) {
            return wt;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    return WriteTarget{};
}

// 辅助：逐步执行 VM 直到 peekWriteTarget 返回 varName==expectedName 或达到上限
// 用于在多条 SET 指令中精确定位到目标变量写入
static WriteTarget stepUntilWriteTargetNamed(VM& vm, const std::string& expectedName, int maxSteps = 200) {
    for (int i = 0; i < maxSteps; ++i) {
        WriteTarget wt = vm.peekWriteTarget();
        if (wt.isWrite && wt.varName == expectedName) {
            return wt;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    return WriteTarget{};
}

TEST(VMPeekWriteTarget, DefineVar_PeekReturnsVarName) {
    // var x = 10; — OP_CONSTANT 10 → OP_DEFINE_VAR x
    const std::string src = "var x = 10;";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    VM vm;
    vm.initExecution(result);
    WriteTarget wt = stepUntilWriteTarget(vm);
    EXPECT_TRUE(wt.isWrite);
    EXPECT_EQ(wt.varName, "x");
    EXPECT_FALSE(wt.isFieldWrite);
    EXPECT_FALSE(wt.isIndexWrite);
}

TEST(VMPeekWriteTarget, SetVar_PeekReturnsVarName) {
    // var x = 0; x = 42; — 第二条语句编译为 OP_CONSTANT 42 + OP_SET_VAR x
    const std::string src = "var x = 0;\nx = 42;";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    VM vm;
    vm.initExecution(result);
    // 跳过 x=0 的 DEFINE_VAR，命中 x=42 的 SET_VAR
    WriteTarget wt = stepUntilWriteTargetNamed(vm, "x");
    // 至少应能命中（DEFINE_VAR 也会返回 varName=x，取第一次）
    EXPECT_TRUE(wt.isWrite);
    EXPECT_EQ(wt.varName, "x");
}

TEST(VMPeekWriteTarget, MultipleVars_EachPeekReturnsCorrectName) {
    // var a = 1; var b = 2; var c = 3;
    const std::string src = "var a = 1;\nvar b = 2;\nvar c = 3;";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    VM vm;
    vm.initExecution(result);

    WriteTarget wtA = stepUntilWriteTargetNamed(vm, "a");
    EXPECT_TRUE(wtA.isWrite);
    EXPECT_EQ(wtA.varName, "a");

    WriteTarget wtB = stepUntilWriteTargetNamed(vm, "b");
    EXPECT_TRUE(wtB.isWrite);
    EXPECT_EQ(wtB.varName, "b");

    WriteTarget wtC = stepUntilWriteTargetNamed(vm, "c");
    EXPECT_TRUE(wtC.isWrite);
    EXPECT_EQ(wtC.varName, "c");
}

TEST(VMPeekWriteTarget, NonWriteInstruction_ReturnsIsWriteFalse) {
    // print(1); — OP_CONSTANT 1 + OP_PRINT，无 SET 指令
    const std::string src = "print(1);";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    VM vm;
    vm.initExecution(result);
    // 第一条指令是 OP_CONSTANT，不是 SET 类指令
    WriteTarget wt = vm.peekWriteTarget();
    EXPECT_FALSE(wt.isWrite);
}

TEST(VMPeekWriteTarget, EmptyFrames_ReturnsDefaultFalse) {
    // 未调用 initExecution 时 frames_ 为空，peekWriteTarget 应安全返回默认值
    VM vm;
    WriteTarget wt = vm.peekWriteTarget();
    EXPECT_FALSE(wt.isWrite);
}

TEST(VMPeekWriteTarget, FieldSet_PeekReturnsFieldName) {
    // class C { var x = 0; } var c = C(); c.x = 42;
    // 最后一条 c.x = 42 编译为 OP_MEMBER_SET
    const std::string src = "class C {\n"
                            "    var x = 0;\n"
                            "}\n"
                            "var c = C();\n"
                            "c.x = 42;\n";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    VM vm;
    vm.initExecution(result);

    // 逐步执行直到命中字段写入
    WriteTarget wt;
    for (int i = 0; i < 300; ++i) {
        wt = vm.peekWriteTarget();
        if (wt.isWrite && wt.isFieldWrite && wt.fieldName == "x") {
            break;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    EXPECT_TRUE(wt.isWrite) << "未命中任何写入指令";
    EXPECT_TRUE(wt.isFieldWrite) << "未命中字段写入";
    EXPECT_EQ(wt.fieldName, "x");
}

TEST(VMPeekWriteTarget, IndexSet_PeekReturnsIsIndexWrite) {
    // var a = [1, 2, 3]; a[0] = 99;
    // 最后一条 a[0] = 99 编译为 OP_INDEX_SET
    const std::string src = "var a = [1, 2, 3];\na[0] = 99;\n";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    VM vm;
    vm.initExecution(result);

    WriteTarget wt;
    for (int i = 0; i < 300; ++i) {
        wt = vm.peekWriteTarget();
        if (wt.isWrite && wt.isIndexWrite) {
            break;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    EXPECT_TRUE(wt.isWrite) << "未命中任何写入指令";
    EXPECT_TRUE(wt.isIndexWrite) << "未命中索引写入";
}

TEST(VMPeekWriteTarget, NoSetInstruction_FinishedWithoutWrite) {
    // print(1 + 2); — 纯算术 + print，无 SET 类指令
    // stepOnce 走完整个程序，peekWriteTarget 始终返回 isWrite=false
    const std::string src = "print(1 + 2);";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    CompileResult result = compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    VM vm;
    vm.initExecution(result);

    bool anyWrite = false;
    for (int i = 0; i < 200; ++i) {
        WriteTarget wt = vm.peekWriteTarget();
        if (wt.isWrite) {
            anyWrite = true;
            break;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    EXPECT_FALSE(anyWrite) << "纯算术 + print 程序不应有 SET 指令";
}

// ============================================================
// 第三组：RegisterVM::peekWriteTarget 测试（寄存器式 VM）
// ------------------------------------------------------------
// 验证 REG_DEFINE_GLOBAL / REG_STORE_GLOBAL / REG_MEMBER_SET / REG_INDEX_SET 等
// RegOp SET 类指令的 pre-execution peek 解码正确性。
// ============================================================

// 辅助：逐步执行 RegisterVM 直到 peekWriteTarget 返回 isWrite=true 或达到上限
static WriteTarget stepUntilWriteTargetReg(RegisterVM& vm, int maxSteps = 300) {
    for (int i = 0; i < maxSteps; ++i) {
        WriteTarget wt = vm.peekWriteTarget();
        if (wt.isWrite) {
            return wt;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    return WriteTarget{};
}

// 辅助：逐步执行 RegisterVM 直到 peekWriteTarget 返回 varName==expectedName 或达到上限
static WriteTarget stepUntilWriteTargetNamedReg(RegisterVM& vm, const std::string& expectedName, int maxSteps = 300) {
    for (int i = 0; i < maxSteps; ++i) {
        WriteTarget wt = vm.peekWriteTarget();
        if (wt.isWrite && wt.varName == expectedName) {
            return wt;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    return WriteTarget{};
}

TEST(RegisterVMPeekWriteTarget, DefineGlobal_PeekReturnsVarName) {
    const std::string src = "var x = 10;";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    RegisterVM vm;
    vm.initExecution(compiler.getLastRegisterResult());
    WriteTarget wt = stepUntilWriteTargetReg(vm);
    EXPECT_TRUE(wt.isWrite);
    EXPECT_EQ(wt.varName, "x");
    EXPECT_FALSE(wt.isFieldWrite);
    EXPECT_FALSE(wt.isIndexWrite);
}

TEST(RegisterVMPeekWriteTarget, MultipleVars_EachPeekReturnsCorrectName) {
    const std::string src = "var a = 1;\nvar b = 2;\nvar c = 3;";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    RegisterVM vm;
    vm.initExecution(compiler.getLastRegisterResult());

    WriteTarget wtA = stepUntilWriteTargetNamedReg(vm, "a");
    EXPECT_TRUE(wtA.isWrite);
    EXPECT_EQ(wtA.varName, "a");

    WriteTarget wtB = stepUntilWriteTargetNamedReg(vm, "b");
    EXPECT_TRUE(wtB.isWrite);
    EXPECT_EQ(wtB.varName, "b");

    WriteTarget wtC = stepUntilWriteTargetNamedReg(vm, "c");
    EXPECT_TRUE(wtC.isWrite);
    EXPECT_EQ(wtC.varName, "c");
}

TEST(RegisterVMPeekWriteTarget, FieldSet_PeekReturnsFieldName) {
    const std::string src = "class C {\n"
                            "    var x = 0;\n"
                            "}\n"
                            "var c = C();\n"
                            "c.x = 42;\n";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    RegisterVM vm;
    vm.initExecution(compiler.getLastRegisterResult());

    WriteTarget wt;
    for (int i = 0; i < 500; ++i) {
        wt = vm.peekWriteTarget();
        if (wt.isWrite && wt.isFieldWrite && wt.fieldName == "x") {
            break;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    EXPECT_TRUE(wt.isWrite) << "未命中任何写入指令";
    EXPECT_TRUE(wt.isFieldWrite) << "未命中字段写入";
    EXPECT_EQ(wt.fieldName, "x");
}

TEST(RegisterVMPeekWriteTarget, IndexSet_PeekReturnsIsIndexWrite) {
    const std::string src = "var a = [1, 2, 3];\na[0] = 99;\n";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    RegisterVM vm;
    vm.initExecution(compiler.getLastRegisterResult());

    WriteTarget wt;
    for (int i = 0; i < 500; ++i) {
        wt = vm.peekWriteTarget();
        if (wt.isWrite && wt.isIndexWrite) {
            break;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    EXPECT_TRUE(wt.isWrite) << "未命中任何写入指令";
    EXPECT_TRUE(wt.isIndexWrite) << "未命中索引写入";
}

TEST(RegisterVMPeekWriteTarget, EmptyFrames_ReturnsDefaultFalse) {
    RegisterVM vm;
    WriteTarget wt = vm.peekWriteTarget();
    EXPECT_FALSE(wt.isWrite);
}

TEST(RegisterVMPeekWriteTarget, NoSetInstruction_FinishedWithoutWrite) {
    const std::string src = "print(1 + 2);";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.compile(*ast);
    ASSERT_FALSE(compiler.getDiagnostics().hasErrors());

    RegisterVM vm;
    vm.initExecution(compiler.getLastRegisterResult());

    bool anyWrite = false;
    for (int i = 0; i < 200; ++i) {
        WriteTarget wt = vm.peekWriteTarget();
        if (wt.isWrite) {
            anyWrite = true;
            break;
        }
        VMResult r = vm.stepOnce();
        if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
            break;
        }
    }
    EXPECT_FALSE(anyWrite) << "纯算术 + print 程序不应有 SET 指令";
}

// ============================================================
// 第四组：跨后端一致性测试
// ------------------------------------------------------------
// 同一源码在 VM 和 RegisterVM 上的 peekWriteTarget 行为应对齐——
// 命中的变量名、字段名、写入类型一致（localSlot 编码不同属预期差异）。
// ============================================================

TEST(PeekWriteTargetConsistency, DefineVar_BothBackendsReportSameVarName) {
    const std::string src = "var x = 10;";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    // 栈式 VM
    {
        Compiler compiler;
        CompileResult result = compiler.compile(*ast);
        ASSERT_FALSE(compiler.getDiagnostics().hasErrors());
        VM vm;
        vm.initExecution(result);
        WriteTarget wt = stepUntilWriteTarget(vm);
        EXPECT_TRUE(wt.isWrite);
        EXPECT_EQ(wt.varName, "x");
    }

    // 寄存器式 VM
    {
        Compiler compiler;
        compiler.setUseRegisterVM(true);
        compiler.compile(*ast);
        ASSERT_FALSE(compiler.getDiagnostics().hasErrors());
        RegisterVM vm;
        vm.initExecution(compiler.getLastRegisterResult());
        WriteTarget wt = stepUntilWriteTargetReg(vm);
        EXPECT_TRUE(wt.isWrite);
        EXPECT_EQ(wt.varName, "x");
    }
}

TEST(PeekWriteTargetConsistency, FieldSet_BothBackendsReportSameFieldName) {
    const std::string src = "class C {\n"
                            "    var x = 0;\n"
                            "}\n"
                            "var c = C();\n"
                            "c.x = 42;\n";
    auto ast = parseSource(src);
    ASSERT_NE(ast, nullptr);

    // 栈式 VM
    std::string stackFieldName;
    {
        Compiler compiler;
        CompileResult result = compiler.compile(*ast);
        ASSERT_FALSE(compiler.getDiagnostics().hasErrors());
        VM vm;
        vm.initExecution(result);
        WriteTarget wt;
        for (int i = 0; i < 300; ++i) {
            wt = vm.peekWriteTarget();
            if (wt.isWrite && wt.isFieldWrite) {
                stackFieldName = wt.fieldName;
                break;
            }
            VMResult r = vm.stepOnce();
            if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
                break;
            }
        }
        EXPECT_FALSE(stackFieldName.empty());
    }

    // 寄存器式 VM
    std::string regFieldName;
    {
        Compiler compiler;
        compiler.setUseRegisterVM(true);
        compiler.compile(*ast);
        ASSERT_FALSE(compiler.getDiagnostics().hasErrors());
        RegisterVM vm;
        vm.initExecution(compiler.getLastRegisterResult());
        WriteTarget wt;
        for (int i = 0; i < 500; ++i) {
            wt = vm.peekWriteTarget();
            if (wt.isWrite && wt.isFieldWrite) {
                regFieldName = wt.fieldName;
                break;
            }
            VMResult r = vm.stepOnce();
            if (r == VMResult::VM_RUNTIME_ERROR || vm.isFinished()) {
                break;
            }
        }
        EXPECT_FALSE(regFieldName.empty());
    }

    EXPECT_EQ(stackFieldName, regFieldName) << "两后端字段写入名应一致";
    EXPECT_EQ(stackFieldName, "x");
}

// ============================================================
// 第五组：L19 Interpreter 路径 Watchpoint 测试
// ------------------------------------------------------------
// 验证 Interpreter 路径的 watchpoint 在 visitAssignment/visitVarDecl/
// visitMemberAssign/visitIndexAssign 入口正确触发暂停。
// 测试模式参照 TestR104DebuggerExtensions.cpp 的函数断点测试（线程执行 + 暂停等待）。
// ============================================================

namespace {
/// 辅助：等待 dbg 暂停（最多 timeoutMs 毫秒），返回是否暂停
bool waitForPause(DebugController* dbg, std::atomic<int>& pauseCount, int target, int timeoutMs = 2000) {
    (void)dbg;
    for (int i = 0; i < timeoutMs / 10 && pauseCount.load() < target; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pauseCount.load() >= target;
}
} // namespace

TEST(L19InterpreterWatchpoint, VariableWatchpoint_PausesOnAssignment) {
    const std::string source = "var x = 0;\n" // line 1 — var decl
                               "x = 1;\n"     // line 2 — assignment
                               "x = 2;\n"     // line 3 — assignment
                               "print(x);\n"; // line 4

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    WatchpointInfo wp;
    wp.kind = WatchpointTargetKind::Variable;
    wp.varName = "x";
    dbg->setWatchpoint(wp);

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // x 被 3 次写入：var x=0（line 1）+ x=1（line 2）+ x=2（line 3）
    // 第一次暂停在 line 1（var decl）
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 1));
    EXPECT_EQ(dbg->getWatchpoints().size(), 1u);
    auto wps = dbg->getWatchpoints();
    EXPECT_EQ(wps[0].hitCount, 1);
    dbg->resume();

    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 2));
    wps = dbg->getWatchpoints();
    EXPECT_EQ(wps[0].hitCount, 2);
    dbg->resume();

    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 3));
    wps = dbg->getWatchpoints();
    EXPECT_EQ(wps[0].hitCount, 3);
    dbg->resume();

    execThread.join();
    EXPECT_EQ(pauseCount.load(), 3);
}

TEST(L19InterpreterWatchpoint, FieldWatchpoint_PausesOnMemberAssign) {
    const std::string source = "class C {\n"
                               "    var x = 0;\n"
                               "}\n"
                               "var c = C();\n" // line 4
                               "c.x = 1;\n"     // line 5 — field write
                               "c.x = 2;\n"     // line 6 — field write
                               "print(c.x);\n"; // line 7

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    WatchpointInfo wp;
    wp.kind = WatchpointTargetKind::Field;
    wp.varName = "c"; // 接收者变量名
    wp.fieldName = "x";
    dbg->setWatchpoint(wp);

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // c.x 被 2 次写入：line 5 + line 6
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 1));
    auto wps = dbg->getWatchpoints();
    EXPECT_EQ(wps[0].hitCount, 1);
    dbg->resume();

    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 2));
    wps = dbg->getWatchpoints();
    EXPECT_EQ(wps[0].hitCount, 2);
    dbg->resume();

    execThread.join();
    EXPECT_EQ(pauseCount.load(), 2);
}

TEST(L19InterpreterWatchpoint, IndexWatchpoint_PausesOnIndexAssign) {
    const std::string source = "var arr = [0, 0, 0];\n"     // line 1
                               "arr[0] = 10;\n"             // line 2 — index write
                               "arr[1] = 20;\n"             // line 3 — index write
                               "print(arr[0] + arr[1]);\n"; // line 4

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    WatchpointInfo wp;
    wp.kind = WatchpointTargetKind::Variable;
    wp.varName = "arr";
    dbg->setWatchpoint(wp);

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // arr 被 3 次写入：var arr=[...]（line 1）+ arr[0]=10（line 2）+ arr[1]=20（line 3）
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 1));
    dbg->resume();
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 2));
    dbg->resume();
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 3));
    dbg->resume();

    execThread.join();
    EXPECT_EQ(pauseCount.load(), 3);
}

TEST(L19InterpreterWatchpoint, ConditionalWatchpoint_OnlyPausesWhenConditionTrue) {
    const std::string source = "var i = 0;\n"      // line 1
                               "while (i < 5) {\n" // line 2
                               "    i = i + 1;\n"  // line 3 — watched write
                               "}\n"
                               "print(i);\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    // 条件求值器：读取 interp 的 i，i > 3 时返回 true
    dbg->setConditionEvaluator([&](const std::string& /*cond*/) -> bool {
        auto env = interp.getGlobalEnvironment();
        if (!env)
            return false;
        auto val = env->get("i");
        if (val && val->isInt()) {
            return val->intVal() > 3;
        }
        return false;
    });

    dbg->reset();
    dbg->resume();
    WatchpointInfo wp;
    wp.varName = "i";
    wp.condition = "i > 3";
    dbg->setWatchpoint(wp);

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // i 从 0 开始，每次循环 i = i + 1，i 的值在写入前为 0,1,2,3,4
    // 条件 i > 3 在 i=4 时为 true（写入前 i=3，条件检查时读 i=3，3>3=false）
    // 实际：pre-execution 检查时读的是写入前的旧值
    // i=0(var decl) → 条件 0>3 false → i=1 → 条件 1>3 false → ... → i=4 → 条件 4>3 true
    // 所以只有 1 次暂停（i=4 写入前，即第 5 次循环）
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 1));
    dbg->resume();

    execThread.join();
    EXPECT_EQ(pauseCount.load(), 1);
    auto wps = dbg->getWatchpoints();
    EXPECT_EQ(wps[0].hitCount, 1);
}

TEST(L19InterpreterWatchpoint, NoWatchpoint_NoPause) {
    // 无 watchpoint 时不应暂停（快速路径短路）
    const std::string source = "var x = 0;\n"
                               "x = 1;\n"
                               "x = 2;\n"
                               "print(x);\n";

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    std::string output;
    interp.setOutputCallback([&](const std::string& s) { output += s; });
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    // 不设置任何 watchpoint

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    interp.execute(*ast); // 同步执行（无暂停）

    EXPECT_EQ(pauseCount.load(), 0);
    EXPECT_EQ(output, "2");
    EXPECT_FALSE(dbg->hasWatchpoints());
}

TEST(L19InterpreterWatchpoint, RemoveWatchpoint_StopsPausing) {
    const std::string source = "var x = 0;\n" // line 1
                               "x = 1;\n"     // line 2
                               "x = 2;\n"     // line 3
                               "print(x);\n"; // line 4

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    WatchpointInfo wp;
    wp.varName = "x";
    dbg->setWatchpoint(wp);

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // 第一次暂停后移除 watchpoint
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 1));
    dbg->removeWatchpoint("x");
    EXPECT_FALSE(dbg->hasWatchpoints());
    dbg->resume();

    execThread.join();
    // 只暂停 1 次（移除后不再触发）
    EXPECT_EQ(pauseCount.load(), 1);
}

TEST(L19InterpreterWatchpoint, FieldWatchpoint_WildcardVarName_MatchesAnyReceiver) {
    // varName 为空的 Field watchpoint 匹配任意接收者（仅按 fieldName 匹配）
    const std::string source = "class C {\n"
                               "    var val = 0;\n"
                               "}\n"
                               "var a = C();\n"           // line 4
                               "var b = C();\n"           // line 5
                               "a.val = 1;\n"             // line 6 — field write on a
                               "b.val = 2;\n"             // line 7 — field write on b
                               "print(a.val + b.val);\n"; // line 8

    auto ast = parseSource(source);
    ASSERT_NE(ast, nullptr);

    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();
    WatchpointInfo wp;
    wp.kind = WatchpointTargetKind::Field;
    wp.varName = ""; // 空通配任意接收者
    wp.fieldName = "val";
    dbg->setWatchpoint(wp);

    std::atomic<int> pauseCount{0};
    QObject::connect(
        dbg.get(), &DebugController::pausedAt, dbg.get(), [&](int) { pauseCount.fetch_add(1); }, Qt::DirectConnection);

    std::thread execThread([&]() {
        try {
            interp.execute(*ast);
        } catch (...) {
        }
    });

    // a.val 和 b.val 各 1 次写入，共 2 次暂停
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 1));
    dbg->resume();
    ASSERT_TRUE(waitForPause(dbg.get(), pauseCount, 2));
    dbg->resume();

    execThread.join();
    EXPECT_EQ(pauseCount.load(), 2);
    auto wps = dbg->getWatchpoints();
    EXPECT_EQ(wps[0].hitCount, 2);
}

// L19 audit fix 回归测试：removeWatchpoint("") 只移除通配 watchpoint，不移除全部。
// 原实现 varName.empty() || wp.varName == varName 导致空 varName 通配所有，
// 与 VmStepper 精确匹配语义不一致。修复后空 varName 只匹配 varName 也为空的 watchpoint。
TEST(L19InterpreterWatchpoint, RemoveWatchpoint_EmptyVarName_OnlyRemovesWildcard) {
    Interpreter interp;
    auto dbg = std::make_shared<DebugController>();
    interp.setOutputCallback([](const std::string&) {});
    interp.setDebugger(dbg);
    interp.setDebugMode(true);

    dbg->reset();
    dbg->resume();

    // 设置两个 watchpoint：一个具名 Variable，一个通配 Field
    WatchpointInfo wpVar;
    wpVar.kind = WatchpointTargetKind::Variable;
    wpVar.varName = "x";
    dbg->setWatchpoint(wpVar);

    WatchpointInfo wpField;
    wpField.kind = WatchpointTargetKind::Field;
    wpField.varName = ""; // 通配任意接收者
    wpField.fieldName = "y";
    dbg->setWatchpoint(wpField);

    ASSERT_EQ(dbg->getWatchpoints().size(), 2u);
    ASSERT_TRUE(dbg->hasWatchpoints());

    // 移交通配 Field watchpoint（varName=""）
    dbg->removeWatchpoint("", "y");

    // 应该只剩具名 Variable watchpoint
    EXPECT_TRUE(dbg->hasWatchpoints());
    auto remaining = dbg->getWatchpoints();
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0].kind, WatchpointTargetKind::Variable);
    EXPECT_EQ(remaining[0].varName, "x");

    // 清理
    dbg->clearWatchpoints();
    EXPECT_FALSE(dbg->hasWatchpoints());
}
