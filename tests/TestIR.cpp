// ============================================================
// IR 中间表示层单元测试
// ------------------------------------------------------------
// ARCH-06 fix: 验证轻量 IR 框架的核心功能：
//   1. IRFunction 数据结构（vreg/label/constant/global 分配）
//   2. BytecodeIRBackend lowering（IR → BytecodeChunk）
//   3. IRToString 调试输出
//   4. IRBuilder 接口（默认实现返回空 IRFunction）
// ============================================================

#include <gtest/gtest.h>

#include "compiler/IR.h"
#include "compiler/Bytecode.h"
#include "compiler/Compiler.h"
#include "compiler/VM.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "ast/ASTNode.h"

#include <memory>
#include <string>

// ============================================================
// IRFunction 数据结构测试
// ============================================================

TEST(IRFunctionTest, AllocVRegReturnsUniqueIndices) {
    IRFunction ir;
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();
    IROperand v2 = ir.allocVReg();
    EXPECT_EQ(v0.kind, IROperandKind::VIRTUAL);
    EXPECT_EQ(v0.index, 0u);
    EXPECT_EQ(v1.index, 1u);
    EXPECT_EQ(v2.index, 2u);
    EXPECT_EQ(ir.nextVReg, 3u);
}

TEST(IRFunctionTest, AllocLabelReturnsUniqueIndices) {
    IRFunction ir;
    uint32_t l0 = ir.allocLabel();
    uint32_t l1 = ir.allocLabel();
    EXPECT_NE(l0, l1);
    EXPECT_EQ(ir.nextLabel, 2u);
}

TEST(IRFunctionTest, AddConstantDeduplicates) {
    IRFunction ir;
    uint32_t idx1 = ir.addConstant(Value(42));
    uint32_t idx2 = ir.addConstant(Value(42));  // 相同值应复用
    uint32_t idx3 = ir.addConstant(Value(100));
    EXPECT_EQ(idx1, idx2);  // 去重
    EXPECT_NE(idx1, idx3);  // 不同值不合并
    EXPECT_EQ(ir.constants.size(), 2u);
}

TEST(IRFunctionTest, AddGlobalDeduplicates) {
    IRFunction ir;
    uint32_t idx1 = ir.addGlobal("x");
    uint32_t idx2 = ir.addGlobal("y");
    uint32_t idx3 = ir.addGlobal("x");  // 相同名应复用
    EXPECT_NE(idx1, idx2);
    EXPECT_EQ(idx1, idx3);  // 去重
    EXPECT_EQ(ir.globalNames.size(), 2u);
}

// ============================================================
// BytecodeIRBackend lowering 测试
// ============================================================

TEST(BytecodeIRBackendTest, LowerEmptyIRReturnsEmptyChunk) {
    IRFunction ir;
    ir.name = "empty";
    BytecodeIRBackend backend;
    ASSERT_TRUE(backend.lower(ir));
    auto chunk = backend.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->name, "empty");
    EXPECT_TRUE(chunk->code.empty());
}

TEST(BytecodeIRBackendTest, LowerLoadConstIntEmitsOP_INT) {
    IRFunction ir;
    ir.name = "load_const";

    // 构建 IR: LOAD_CONST v0, c0
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    uint32_t constIdx = ir.addConstant(Value(42));
    IROperand dest = ir.allocVReg();
    block.instructions.emplace_back(IROp::LOAD_CONST,
        std::vector<IROperand>{ dest, IROperand::constant(constIdx) }, 1);

    ir.blocks.push_back(std::move(block));

    BytecodeIRBackend backend;
    ASSERT_TRUE(backend.lower(ir));
    auto chunk = backend.takeChunk();
    ASSERT_NE(chunk, nullptr);
    ASSERT_GE(chunk->code.size(), 3u);
    // 规格：LOAD_CONST 按 Value 类型分发，Value(42) 为 VAL_INT → OP_INT
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_INT));
    // 后两字节是常量索引（小端）
    uint16_t emittedIdx = chunk->code[1] | (chunk->code[2] << 8);
    EXPECT_EQ(emittedIdx, constIdx);
    ASSERT_GE(chunk->constants.size(), 1u);
    EXPECT_EQ(chunk->constants[0].intVal(), 42);
}

TEST(BytecodeIRBackendTest, LowerArithEmitsCorrectOpCode) {
    IRFunction ir;
    ir.name = "arith";

    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    // v0 = 1, v1 = 2, v2 = v0 + v1
    uint32_t c0 = ir.addConstant(Value(1));
    uint32_t c1 = ir.addConstant(Value(2));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();
    IROperand v2 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST,
        std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST,
        std::vector<IROperand>{ v1, IROperand::constant(c1) }, 2);
    block.instructions.emplace_back(IROp::ADD,
        std::vector<IROperand>{ v2, v0, v1 }, 3);

    ir.blocks.push_back(std::move(block));

    BytecodeIRBackend backend;
    ASSERT_TRUE(backend.lower(ir));
    auto chunk = backend.takeChunk();
    ASSERT_NE(chunk, nullptr);
    // 期望: OP_CONSTANT c0, OP_CONSTANT c1, OP_ADD
    // 偏移: 0-2, 3-5, 6
    ASSERT_GE(chunk->code.size(), 7u);
    EXPECT_EQ(chunk->code[6], static_cast<uint8_t>(OpCode::OP_ADD));
}

TEST(BytecodeIRBackendTest, LowerJumpResolvesLabelTarget) {
    IRFunction ir;
    ir.name = "jump_test";

    IRBasicBlock block;
    uint32_t labelStart = ir.allocLabel();
    uint32_t labelEnd = ir.allocLabel();
    block.labelIndex = labelStart;

    // LABEL start
    block.instructions.emplace_back(IROp::LABEL,
        std::vector<IROperand>{ IROperand::label(labelStart) }, 1);
    // JUMP end
    block.instructions.emplace_back(IROp::JUMP,
        std::vector<IROperand>{ IROperand::label(labelEnd) }, 2);
    // LABEL end
    block.instructions.emplace_back(IROp::LABEL,
        std::vector<IROperand>{ IROperand::label(labelEnd) }, 3);

    ir.blocks.push_back(std::move(block));

    BytecodeIRBackend backend;
    ASSERT_TRUE(backend.lower(ir));
    auto chunk = backend.takeChunk();
    ASSERT_NE(chunk, nullptr);
    // JUMP 指令: opcode + 2 bytes target
    // 第一条 LABEL 不产生字节码，所以 JUMP 在 offset 0
    ASSERT_GE(chunk->code.size(), 3u);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_JUMP));
    uint16_t target = chunk->code[1] | (chunk->code[2] << 8);
    // 目标应是 LABEL end 的位置（即 JUMP 指令之后，offset 3）
    EXPECT_EQ(target, 3u);
}

// ============================================================
// IRBuilder 接口测试
// ============================================================

TEST(IRBuilderTest, AstBuilderProducesIRFromEmptyBlock) {
    // IRBuilder 为抽象接口（build(Block&) 纯虚），无法直接实例化。
    // 验证具体子类 AstIRBuilder 对空 Block 构建 IR：构造函数已创建入口基本块。
    AstIRBuilder builder;
    Block emptyBlock(std::vector<std::shared_ptr<ASTNode>>{});
    auto ir = builder.build(emptyBlock);
    ASSERT_NE(ir, nullptr);
    // 入口基本块已创建（含 LABEL），故 blocks 非空
    EXPECT_FALSE(ir->blocks.empty());
}

// ============================================================
// IRToString 测试
// ============================================================

TEST(IRToStringTest, FormatsEmptyIR) {
    IRFunction ir;
    ir.name = "test_func";
    std::string str = IRToString(ir);
    EXPECT_NE(str.find("IRFunction"), std::string::npos);
    EXPECT_NE(str.find("test_func"), std::string::npos);
    EXPECT_NE(str.find("blocks: 0"), std::string::npos);
}

TEST(IRToStringTest, IncludesInstructionInfo) {
    IRFunction ir;
    ir.name = "with_instr";

    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();
    uint32_t c0 = ir.addConstant(Value(42));
    IROperand v0 = ir.allocVReg();
    block.instructions.emplace_back(IROp::LOAD_CONST,
        std::vector<IROperand>{ v0, IROperand::constant(c0) }, 10);

    ir.blocks.push_back(std::move(block));

    std::string str = IRToString(ir);
    EXPECT_NE(str.find("LOAD_CONST"), std::string::npos);
    EXPECT_NE(str.find("c0"), std::string::npos);
    EXPECT_NE(str.find("v0"), std::string::npos);
    EXPECT_NE(str.find("line 10"), std::string::npos);
}

// ============================================================
// 方向一：IR 端到端测试（AST → IR → Bytecode → VM 执行）
// ============================================================
// 验证 IR 路径生成的字节码能被 VM 正确执行，结果与直接编译路径一致。
// 使用 Compiler::setUseIR(true) 启用 IR 路径。

namespace {

/// 辅助：通过 IR 路径执行源码，返回 print 输出
// AUDIT-HELPER fix: 原实现忽略 vm.hasError()，VM 出错时返回部分输出，
// 与 runViaIROptimized 对比时"部分输出 == 部分输出"可能误判通过，
// 掩盖优化 pass 引入的语义错误。现改为检查 hasError 并编码错误。
static std::string runViaIR(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "";

    Compiler compiler;
    compiler.setUseIR(true);
    CompileResult result = compiler.compile(*ast);

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

/// 辅助：通过 IR + 优化 pass 路径执行源码，返回 print 输出
// AUDIT-HELPER fix: 同 runViaIR，检查 hasError 防止掩盖优化 pass 引入的错误。
static std::string runViaIROptimized(const std::string& source) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast) return "";

    Compiler compiler;
    compiler.setUseIR(true);
    compiler.setIROptimize(true);
    CompileResult result = compiler.compile(*ast);

    VM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

} // anonymous namespace

// ---- 基本算术 ----

TEST(IRE2E, BasicAddition) {
    EXPECT_EQ(runViaIR("print(1 + 2);"), "3");
}

TEST(IRE2E, BasicArithmetic) {
    EXPECT_EQ(runViaIR("print(10 - 3);"), "7");
    EXPECT_EQ(runViaIR("print(4 * 5);"), "20");
    EXPECT_EQ(runViaIR("print(20 / 4);"), "5");
    EXPECT_EQ(runViaIR("print(17 % 5);"), "2");
}

TEST(IRE2E, OperatorPrecedence) {
    EXPECT_EQ(runViaIR("print(2 + 3 * 4);"), "14");
    EXPECT_EQ(runViaIR("print((2 + 3) * 4);"), "20");
}

TEST(IRE2E, FloatArithmetic) {
    EXPECT_EQ(runViaIR("print(1.5 + 2.5);"), "4");
}

TEST(IRE2E, UnaryNegate) {
    EXPECT_EQ(runViaIR("var x = 5; print(-x);"), "-5");
}

// ---- 变量与赋值 ----

TEST(IRE2E, VariableDeclaration) {
    EXPECT_EQ(runViaIR("var x = 5; print(x);"), "5");
}

TEST(IRE2E, VariableAssignment) {
    EXPECT_EQ(runViaIR("var x = 5; print(x); x = 10; print(x);"), "510");
}

TEST(IRE2E, MultipleVariables) {
    EXPECT_EQ(runViaIR("var a = 1; var b = 2; var c = a + b; print(c);"), "3");
}

// ---- 控制流 ----

TEST(IRE2E, IfTrue) {
    EXPECT_EQ(runViaIR("if (1 < 2) { print(\"yes\"); }"), "yes");
}

TEST(IRE2E, IfFalse) {
    EXPECT_EQ(runViaIR("if (1 > 2) { print(\"yes\"); } else { print(\"no\"); }"), "no");
}

TEST(IRE2E, WhileLoop) {
    std::string src = "var i = 0; while (i < 3) { print(i); i = i + 1; }";
    EXPECT_EQ(runViaIR(src), "012");
}

TEST(IRE2E, ForLoop) {
    std::string src = "for (var i = 0; i < 3; i = i + 1) { print(i); }";
    EXPECT_EQ(runViaIR(src), "012");
}

// ---- 逻辑与比较 ----

TEST(IRE2E, LogicalOps) {
    EXPECT_EQ(runViaIR("print(true and false);"), "false");
    EXPECT_EQ(runViaIR("print(true or false);"), "true");
    EXPECT_EQ(runViaIR("print(not true);"), "false");
}

TEST(IRE2E, ComparisonOps) {
    EXPECT_EQ(runViaIR("print(1 < 2);"), "true");
    EXPECT_EQ(runViaIR("print(2 > 1);"), "true");
    EXPECT_EQ(runViaIR("print(1 == 1);"), "true");
    EXPECT_EQ(runViaIR("print(1 != 2);"), "true");
}

// ---- 字符串 ----

TEST(IRE2E, StringConcat) {
    EXPECT_EQ(runViaIR("print(\"hello\" + \" world\");"), "hello world");
}

TEST(IRE2E, StringVariable) {
    EXPECT_EQ(runViaIR("var s = \"abc\"; print(s);"), "abc");
}

// ---- 函数 ----

TEST(IRE2E, SimpleFunctionCall) {
    std::string src = "fun add(a, b) { return a + b; } print(add(3, 4));";
    EXPECT_EQ(runViaIR(src), "7");
}

TEST(IRE2E, FunctionReturn) {
    std::string src = "fun f() { return 42; } print(f());";
    EXPECT_EQ(runViaIR(src), "42");
}

TEST(IRE2E, NestedFunctionCall) {
    std::string src = "fun double(x) { return x * 2; } fun quad(x) { return double(double(x)); } print(quad(5));";
    EXPECT_EQ(runViaIR(src), "20");
}

// ---- 闭包 upvalue 容器变异（#7 fix: WRITEBACK_*_UPVALUE）----

TEST(IRE2E, ClosureUpvalueArrayIndexAssign) {
    // 内层闭包修改外层函数局部数组：arr[i] = v
    // 修复前：COW ensureUnique 产生新数组副本，但未写回 upvalue，修改丢失
    std::string src =
        "fun outer() {\n"
        "    var arr = [1, 2, 3];\n"
        "    fun mutate() {\n"
        "        arr[0] = 99;\n"
        "    }\n"
        "    mutate();\n"
        "    return arr[0];\n"
        "}\n"
        "print(outer());\n";
    EXPECT_EQ(runViaIR(src), "99");
}

TEST(IRE2E, ClosureUpvalueDictMemberAssign) {
    // 内层闭包修改外层函数局部字典字段：d.f = v
    std::string src =
        "fun outer() {\n"
        "    var d = {\"x\": 1};\n"
        "    fun setX() {\n"
        "        d.x = 42;\n"
        "    }\n"
        "    setX();\n"
        "    return d.x;\n"
        "}\n"
        "print(outer());\n";
    EXPECT_EQ(runViaIR(src), "42");
}

TEST(IRE2E, ClosureUpvalueArrayIndexAssignOptimized) {
    // 同上，但启用 IR 优化 pass，确保 WRITEBACK_*_UPVALUE 不被错误删除
    std::string src =
        "fun outer() {\n"
        "    var arr = [1, 2, 3];\n"
        "    fun mutate() {\n"
        "        arr[1] = 77;\n"
        "    }\n"
        "    mutate();\n"
        "    return arr[1];\n"
        "}\n"
    "print(outer());\n";
    EXPECT_EQ(runViaIROptimized(src), "77");
}

// ---- 优化 pass 与非优化路径结果一致性 ----

TEST(IRE2E, OptimizedMatchesNonOptimized) {
    // 确保优化 pass 不改变程序语义
    std::vector<std::string> sources = {
        "print(1 + 2);",
        "print(10 - 3);",
        "print(4 * 5);",
        "print(2 + 3 * 4);",
        "var x = 5; print(-x);",
        "print(1 < 2);",
        "print(true and false);",
        "var a = 1; var b = 2; print(a + b);",
        "if (1 < 2) { print(\"yes\"); }",
        "var i = 0; while (i < 3) { print(i); i = i + 1; }",
        "fun add(a, b) { return a + b; } print(add(3, 4));",
        "fun double(x) { return x * 2; } print(double(21));",
    };
    for (const auto& src : sources) {
        std::string normal = runViaIR(src);
        std::string optimized = runViaIROptimized(src);
        EXPECT_EQ(normal, optimized)
            << "优化 pass 改变了语义: source=\"" << src << "\""
            << " normal=\"" << normal << "\" optimized=\"" << optimized << "\"";
    }
}

// ============================================================
// 方向二：IR 优化 Pass 单元测试
// ============================================================

// ---- 常量折叠 ----

TEST(IROptimizeTest, ConstantFoldingAddInt) {
    IRFunction ir;
    ir.name = "fold_add";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    // v0 = 1, v1 = 2, v2 = v0 + v1 → 折叠为 v2 = 3
    uint32_t c0 = ir.addConstant(Value(1));
    uint32_t c1 = ir.addConstant(Value(2));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();
    IROperand v2 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v1, IROperand::constant(c1) }, 2);
    block.instructions.emplace_back(IROp::ADD, std::vector<IROperand>{ v2, v0, v1 }, 3);

    ir.blocks.push_back(std::move(block));

    bool modified = constantFoldingPass(ir);
    EXPECT_TRUE(modified);

    // 第三条指令应变为 LOAD_CONST
    const auto& folded = ir.blocks[0].instructions[2];
    EXPECT_EQ(folded.op, IROp::LOAD_CONST);
    ASSERT_EQ(folded.operands.size(), 2u);
    EXPECT_EQ(folded.operands[0].kind, IROperandKind::VIRTUAL);
    uint32_t constIdx = folded.operands[1].index;
    ASSERT_LT(constIdx, ir.constants.size());
    EXPECT_EQ(ir.constants[constIdx].intVal(), 3);
}

TEST(IROptimizeTest, ConstantFoldingSubFloat) {
    IRFunction ir;
    ir.name = "fold_sub";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    uint32_t c0 = ir.addConstant(Value(3.5));
    uint32_t c1 = ir.addConstant(Value(1.0));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();
    IROperand v2 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v1, IROperand::constant(c1) }, 2);
    block.instructions.emplace_back(IROp::SUB, std::vector<IROperand>{ v2, v0, v1 }, 3);

    ir.blocks.push_back(std::move(block));

    bool modified = constantFoldingPass(ir);
    EXPECT_TRUE(modified);

    const auto& folded = ir.blocks[0].instructions[2];
    EXPECT_EQ(folded.op, IROp::LOAD_CONST);
    uint32_t constIdx = folded.operands[1].index;
    ASSERT_LT(constIdx, ir.constants.size());
    EXPECT_DOUBLE_EQ(ir.constants[constIdx].floatVal(), 2.5);
}

TEST(IROptimizeTest, ConstantFoldingComparison) {
    IRFunction ir;
    ir.name = "fold_cmp";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    // 1 < 2 → true
    uint32_t c0 = ir.addConstant(Value(1));
    uint32_t c1 = ir.addConstant(Value(2));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();
    IROperand v2 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v1, IROperand::constant(c1) }, 2);
    block.instructions.emplace_back(IROp::LT, std::vector<IROperand>{ v2, v0, v1 }, 3);

    ir.blocks.push_back(std::move(block));

    bool modified = constantFoldingPass(ir);
    EXPECT_TRUE(modified);

    const auto& folded = ir.blocks[0].instructions[2];
    EXPECT_EQ(folded.op, IROp::LOAD_CONST);
    uint32_t constIdx = folded.operands[1].index;
    ASSERT_LT(constIdx, ir.constants.size());
    EXPECT_EQ(ir.constants[constIdx].boolVal(), true);
}

TEST(IROptimizeTest, ConstantFoldingNegate) {
    IRFunction ir;
    ir.name = "fold_neg";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    // -5 → -5
    uint32_t c0 = ir.addConstant(Value(5));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::NEGATE, std::vector<IROperand>{ v1, v0 }, 2);

    ir.blocks.push_back(std::move(block));

    bool modified = constantFoldingPass(ir);
    EXPECT_TRUE(modified);

    const auto& folded = ir.blocks[0].instructions[1];
    EXPECT_EQ(folded.op, IROp::LOAD_CONST);
    uint32_t constIdx = folded.operands[1].index;
    ASSERT_LT(constIdx, ir.constants.size());
    EXPECT_EQ(ir.constants[constIdx].intVal(), -5);
}

TEST(IROptimizeTest, ConstantFoldingDivByZeroNotFolded) {
    IRFunction ir;
    ir.name = "fold_div_zero";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    // 10 / 0 → 不应折叠（除零留待运行时报错）
    uint32_t c0 = ir.addConstant(Value(10));
    uint32_t c1 = ir.addConstant(Value(0));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();
    IROperand v2 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v1, IROperand::constant(c1) }, 2);
    block.instructions.emplace_back(IROp::DIV, std::vector<IROperand>{ v2, v0, v1 }, 3);

    ir.blocks.push_back(std::move(block));

    bool modified = constantFoldingPass(ir);
    EXPECT_FALSE(modified);  // 除零不应折叠
}

// ---- 死代码消除 ----

TEST(IROptimizeTest, DeadCodeEliminationRemovesUnused) {
    IRFunction ir;
    ir.name = "dce";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    // v0 = 42 (从不被使用) → 应被删除
    // v1 = 100 (被 print 使用) → 应保留
    uint32_t c0 = ir.addConstant(Value(42));
    uint32_t c1 = ir.addConstant(Value(100));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v1, IROperand::constant(c1) }, 2);
    block.instructions.emplace_back(IROp::PRINT, std::vector<IROperand>{ v1 }, 3);

    ir.blocks.push_back(std::move(block));

    size_t before = ir.blocks[0].instructions.size();
    EXPECT_EQ(before, 3u);

    bool modified = deadCodeEliminationPass(ir);
    EXPECT_TRUE(modified);

    size_t after = ir.blocks[0].instructions.size();
    EXPECT_EQ(after, 2u);  // v0 的 LOAD_CONST 被删除
    // 剩余：LOAD_CONST v1, PRINT v1
    EXPECT_EQ(ir.blocks[0].instructions[0].op, IROp::LOAD_CONST);
    EXPECT_EQ(ir.blocks[0].instructions[1].op, IROp::PRINT);
}

TEST(IROptimizeTest, DeadCodeEliminationKeepsUsed) {
    IRFunction ir;
    ir.name = "dce_keep";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    // v0 = 1, v1 = 2, v2 = v0 + v1, print(v2) → 全部保留
    uint32_t c0 = ir.addConstant(Value(1));
    uint32_t c1 = ir.addConstant(Value(2));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();
    IROperand v2 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v1, IROperand::constant(c1) }, 2);
    block.instructions.emplace_back(IROp::ADD, std::vector<IROperand>{ v2, v0, v1 }, 3);
    block.instructions.emplace_back(IROp::PRINT, std::vector<IROperand>{ v2 }, 4);

    ir.blocks.push_back(std::move(block));

    bool modified = deadCodeEliminationPass(ir);
    EXPECT_FALSE(modified);  // 无死代码
    EXPECT_EQ(ir.blocks[0].instructions.size(), 4u);
}

// ---- 复制传播 ----
// PERF-15: copyPropagationPass 现已完整实现（寄存器式后端安全启用）
// 测试验证：LOAD_CONST 定义 vreg 后，后续引用 v0 被替换为直接常量引用

TEST(IROptimizeTest, CopyPropagationReplacesVregWithConstant) {
    IRFunction ir;
    ir.name = "copy_prop";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    uint32_t c0 = ir.addConstant(Value(42));
    IROperand v0 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::PRINT, std::vector<IROperand>{ v0 }, 2);

    ir.blocks.push_back(std::move(block));

    // 复制传播应修改 IR：PRINT 的操作数从 v0 替换为 constant c0
    bool modified = copyPropagationPass(ir);
    EXPECT_TRUE(modified);
    // 指令数不变（copyPropagationPass 只替换操作数，不删除指令）
    EXPECT_EQ(ir.blocks[0].instructions.size(), 2u);
    // PRINT 的操作数应已变为 constant
    EXPECT_EQ(ir.blocks[0].instructions[1].operands[0].kind, IROperandKind::CONSTANT);
    EXPECT_EQ(ir.blocks[0].instructions[1].operands[0].index, c0);
}

// 验证 optimizeIR 在栈式模式（默认）下不启用复制传播
TEST(IROptimizeTest, OptimizeIRStackModeSkipsCopyPropagation) {
    IRFunction ir;
    ir.name = "stack_mode";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    uint32_t c0 = ir.addConstant(Value(42));
    IROperand v0 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c0) }, 1);
    block.instructions.emplace_back(IROp::PRINT, std::vector<IROperand>{ v0 }, 2);

    ir.blocks.push_back(std::move(block));

    // 栈式模式（enableCopyPropagation=false）不应触发复制传播
    // BUG-IR-DCE-2 fix: optimizeIR 新增 enableDCE 参数，栈式后端 DCE 不安全故传 false
    optimizeIR(ir, /*enableCopyPropagation=*/false, /*enableDCE=*/false);
    // PRINT 的操作数应仍为 vreg（未被替换为 constant）
    EXPECT_EQ(ir.blocks[0].instructions[1].operands[0].kind, IROperandKind::VIRTUAL);
}

// ---- 端到端优化验证 ----

TEST(IROptimizeTest, OptimizeReducesInstructionCount) {
    // 构造一个可优化的 IR：print(1 + 2 + 3)
    // 优化前：LOAD_CONST 1, LOAD_CONST 2, ADD, LOAD_CONST 3, ADD, PRINT
    // 优化后（常量折叠）：LOAD_CONST 6, PRINT
    IRFunction ir;
    ir.name = "optimize_e2e";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();

    uint32_t c1 = ir.addConstant(Value(1));
    uint32_t c2 = ir.addConstant(Value(2));
    uint32_t c3 = ir.addConstant(Value(3));
    IROperand v0 = ir.allocVReg();
    IROperand v1 = ir.allocVReg();
    IROperand v2 = ir.allocVReg();
    IROperand v3 = ir.allocVReg();
    IROperand v4 = ir.allocVReg();

    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v0, IROperand::constant(c1) }, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v1, IROperand::constant(c2) }, 2);
    block.instructions.emplace_back(IROp::ADD, std::vector<IROperand>{ v2, v0, v1 }, 3);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{ v3, IROperand::constant(c3) }, 4);
    block.instructions.emplace_back(IROp::ADD, std::vector<IROperand>{ v4, v2, v3 }, 5);
    block.instructions.emplace_back(IROp::PRINT, std::vector<IROperand>{ v4 }, 6);

    ir.blocks.push_back(std::move(block));

    size_t beforeCount = ir.blocks[0].instructions.size();
    EXPECT_EQ(beforeCount, 6u);

    bool modified = optimizeIR(ir, /*enableCopyPropagation=*/true, /*enableDCE=*/true);
    EXPECT_TRUE(modified);

    size_t afterCount = ir.blocks[0].instructions.size();
    // 优化后应减少指令数（常量折叠 + 复制传播 + 死代码消除后应剩 LOAD_CONST 6 + PRINT）
    EXPECT_LT(afterCount, beforeCount);
    EXPECT_LE(afterCount, 3u);  // 至少 LOAD_CONST + PRINT，可能还有残留
}
