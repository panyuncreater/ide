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
#include "interpreter/Value.h"

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

TEST(BytecodeIRBackendTest, LowerLoadConstEmitsOP_CONSTANT) {
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
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_CONSTANT));
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

TEST(IRBuilderTest, DefaultBuildReturnsEmptyIR) {
    IRBuilder builder;
    auto ir = builder.build();
    ASSERT_NE(ir, nullptr);
    EXPECT_TRUE(ir->blocks.empty());
    EXPECT_EQ(ir->nextVReg, 0u);
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
