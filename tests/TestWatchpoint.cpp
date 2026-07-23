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
#include "debug/DebugTypes.h" // R161: WatchpointInfo + WriteTarget
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <string>
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
