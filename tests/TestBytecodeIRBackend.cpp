// ============================================================
// BytecodeIRBackend 专用单元测试
// ------------------------------------------------------------
// 直接验证 IR → BytecodeChunk (StackVM) lowering 的正确性。
// TestIR.cpp 仅覆盖基础 lowering（empty/LOAD_CONST/ADD/JUMP）；
// 本文件逐 IROp 验证 → OpCode 映射、栈深度追踪、跳转回填、
// 多函数 lowering 与 irToBytecodeOffset 映射正确性。
//
// 覆盖范围：
//   - 各 IROp → OpCode 完整映射验证
//   - 常量加载按 Value 类型分发（int/float/string/bool/null）
//   - 变量访问（local/global/upvalue）→ OP_GET/SET_LOCAL/GLOBAL/UPVALUE
//   - 控制流跳转回填（绝对偏移 / TRY_BEGIN 相对偏移）
//   - 调用与返回（CALL/RETURN/RETURN_NULL）
//   - 容器构造与索引（BUILD_ARRAY/DICT/INDEX_GET/SET）
//   - 成员访问（MEMBER_GET/SET）
//   - 异常处理（TRY_BEGIN/TRY_END/THROW）
//   - 多函数 lowering（lowerModule）
//   - irToBytecodeOffset 映射
// ============================================================

#include <gtest/gtest.h>

#include "compiler/Bytecode.h"
#include "compiler/IR.h"
#include "interpreter/Value.h"

#include <memory>
#include <string>
#include <vector>

namespace {

/// 构建一个含单基本块、单条指令的 IRFunction
struct LowerFixture {
    IRFunction ir;
    IRBasicBlock block;
    BytecodeIRBackend backend;

    LowerFixture(const std::string& name = "test") {
        ir.name = name;
        block.labelIndex = ir.allocLabel();
    }

    /// 添加一条 IR 指令到当前块
    void emit(IROp op, std::vector<IROperand> ops, int line = 1) {
        block.instructions.emplace_back(op, std::move(ops), line);
    }

    /// 完成构建，调用 lower
    bool lower() {
        ir.blocks.push_back(std::move(block));
        return backend.lower(ir);
    }

    /// 完成构建，调用 lowerModule（单 main 函数）
    bool lowerModule() {
        ir.blocks.push_back(std::move(block));
        IRModule module;
        module.mainFunction = std::make_unique<IRFunction>(std::move(ir));
        return backend.lowerModule(module);
    }

    /// 取生成的字节码（lower/lowerModule 后）
    std::unique_ptr<BytecodeChunk> takeChunk() { return backend.takeChunk(); }
};

/// 检查字节码序列中是否包含指定 OpCode
static bool containsOpCode(const std::vector<uint8_t>& code, OpCode target) {
    uint8_t targetByte = static_cast<uint8_t>(target);
    for (uint8_t b : code) {
        if (b == targetByte)
            return true;
    }
    return false;
}

} // namespace

// ============================================================
// 常量加载 → OP_INT/OP_FLOAT/OP_STRING/OP_NULL/OP_TRUE/OP_FALSE
// ============================================================

TEST(BytecodeIRBackendConst, LoadConstIntEmitsOP_INT) {
    LowerFixture f("const_int");
    uint32_t c = f.ir.addConstant(Value(42));
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::LOAD_CONST, {v, IROperand::constant(c)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_INT));
}

TEST(BytecodeIRBackendConst, LoadConstFloatEmitsOP_FLOAT) {
    LowerFixture f("const_float");
    uint32_t c = f.ir.addConstant(Value(3.14));
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::LOAD_CONST, {v, IROperand::constant(c)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_FLOAT));
}

TEST(BytecodeIRBackendConst, LoadConstStringEmitsOP_STRING) {
    LowerFixture f("const_str");
    uint32_t c = f.ir.addConstant(Value(std::string("hello")));
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::LOAD_CONST, {v, IROperand::constant(c)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_STRING));
}

TEST(BytecodeIRBackendConst, LoadNullEmitsOP_NULL) {
    LowerFixture f("const_null");
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::LOAD_NULL, {v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_NULL));
}

TEST(BytecodeIRBackendConst, LoadTrueEmitsOP_TRUE) {
    LowerFixture f("const_true");
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::LOAD_TRUE, {v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_TRUE));
}

TEST(BytecodeIRBackendConst, LoadFalseEmitsOP_FALSE) {
    LowerFixture f("const_false");
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::LOAD_FALSE, {v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_FALSE));
}

// ============================================================
// 算术运算 → OP_ADD/SUBTRACT/MULTIPLY/DIVIDE/MODULO/NEGATE
// ============================================================

TEST(BytecodeIRBackendArith, AddEmitsOP_ADD) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::ADD, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_ADD));
}

TEST(BytecodeIRBackendArith, SubEmitsOP_SUBTRACT) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::SUB, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_SUBTRACT));
}

TEST(BytecodeIRBackendArith, MulEmitsOP_MULTIPLY) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::MUL, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_MULTIPLY));
}

TEST(BytecodeIRBackendArith, DivEmitsOP_DIVIDE) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::DIV, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_DIVIDE));
}

TEST(BytecodeIRBackendArith, ModEmitsOP_MODULO) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::MOD, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_MODULO));
}

TEST(BytecodeIRBackendArith, NegateEmitsOP_NEGATE) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    f.emit(IROp::NEGATE, {v1, v0});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_NEGATE));
}

// ============================================================
// 比较 → OP_EQUAL/OP_NOT_EQUAL/OP_LESS/OP_GREATER/OP_LESS_EQUAL/OP_GREATER_EQUAL
// ============================================================

TEST(BytecodeIRBackendCompare, EqEmitsOP_EQUAL) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::EQ, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_EQUAL));
}

TEST(BytecodeIRBackendCompare, NeqEmitsOP_NOT_EQUAL) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::NEQ, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_NOT_EQUAL));
}

TEST(BytecodeIRBackendCompare, LtEmitsOP_LESS) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::LT, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_LESS));
}

TEST(BytecodeIRBackendCompare, GtEmitsOP_GREATER) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::GT, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_GREATER));
}

TEST(BytecodeIRBackendCompare, LteEmitsOP_LESS_EQUAL) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::LTE, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_LESS_EQUAL));
}

TEST(BytecodeIRBackendCompare, GteEmitsOP_GREATER_EQUAL) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::GTE, {v2, v0, v1});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_GREATER_EQUAL));
}

TEST(BytecodeIRBackendCompare, NotEmitsOP_NOT) {
    LowerFixture f;
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    f.emit(IROp::NOT, {v1, v0});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_NOT));
}

// ============================================================
// 变量访问 → OP_GET/SET_LOCAL/GLOBAL/DEFINE_GLOBAL
// ============================================================

TEST(BytecodeIRBackendVar, LoadLocalEmitsOP_GET_LOCAL) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::LOAD_LOCAL, {v, IROperand::local(0)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_GET_LOCAL));
}

TEST(BytecodeIRBackendVar, StoreLocalEmitsOP_SET_LOCAL) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::STORE_LOCAL, {IROperand::local(0), v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_SET_LOCAL));
}

TEST(BytecodeIRBackendVar, LoadGlobalEmitsOP_GET_GLOBAL) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    uint32_t g = f.ir.addGlobal("x");
    // 槽位版路径：LOAD_GLOBAL 的源操作数为 IMM_UINT（全局槽位号）时
    // lowering 为 OP_GET_GLOBAL slot(2B)；GLOBAL_NAME kind 则走名称版 OP_GET_VAR。
    f.emit(IROp::LOAD_GLOBAL, {v, IROperand::imm(g)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_GET_GLOBAL));
}

/// 名称版路径：GLOBAL_NAME kind 操作数 lowering 为 OP_GET_VAR nameIdx(2B)
TEST(BytecodeIRBackendVar, LoadGlobalByNameEmitsOP_GET_VAR) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    uint32_t g = f.ir.addGlobal("x");
    f.emit(IROp::LOAD_GLOBAL, {v, IROperand::global(g)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_GET_VAR));
}

TEST(BytecodeIRBackendVar, StoreGlobalEmitsOP_SET_GLOBAL) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    uint32_t g = f.ir.addGlobal("x");
    // 槽位版：目标操作数 IMM_UINT → OP_SET_GLOBAL；GLOBAL_NAME → OP_SET_VAR
    f.emit(IROp::STORE_GLOBAL, {IROperand::imm(g), v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_SET_GLOBAL));
}

TEST(BytecodeIRBackendVar, DefineGlobalEmitsOP_DEFINE_GLOBAL) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    uint32_t g = f.ir.addGlobal("x");
    // 槽位版：目标操作数 IMM_UINT → OP_DEFINE_GLOBAL；GLOBAL_NAME → OP_DEFINE_VAR
    f.emit(IROp::DEFINE_GLOBAL, {IROperand::imm(g), v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_DEFINE_GLOBAL));
}

// ============================================================
// 控制流 → OP_JUMP/OP_JUMP_IF_FALSE + 标签回填
// ============================================================

TEST(BytecodeIRBackendControl, JumpIfFalseEmitsOP_JUMP_IF_FALSE) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    uint32_t target = f.ir.allocLabel();
    f.emit(IROp::JUMP_IF_FALSE, {v, IROperand::label(target)});
    // 目标 LABEL 必须存在，否则 patchJumps 回填阶段找不到标签失败
    f.emit(IROp::LABEL, {IROperand::label(target)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_JUMP_IF_FALSE));
}

TEST(BytecodeIRBackendControl, JumpEmitsOP_JUMP) {
    LowerFixture f;
    uint32_t target = f.ir.allocLabel();
    f.emit(IROp::JUMP, {IROperand::label(target)});
    // 目标 LABEL 必须存在，否则 patchJumps 回填阶段找不到标签失败
    f.emit(IROp::LABEL, {IROperand::label(target)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_JUMP));
}

TEST(BytecodeIRBackendControl, LabelDoesNotEmitBytecode) {
    // LABEL 是基本块标记，不应产生字节码
    LowerFixture f;
    uint32_t label = f.ir.allocLabel();
    f.emit(IROp::LABEL, {IROperand::label(label)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(chunk->code.empty());
}

TEST(BytecodeIRBackendControl, ForwardJumpResolvesToTargetOffset) {
    // JUMP forward → 目标偏移为 LABEL 之后的位置
    LowerFixture f;
    uint32_t start = f.ir.allocLabel();
    uint32_t end = f.ir.allocLabel();
    f.emit(IROp::LABEL, {IROperand::label(start)});
    f.emit(IROp::JUMP, {IROperand::label(end)});
    f.emit(IROp::LABEL, {IROperand::label(end)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    // JUMP 在 offset 0，后接 2 字节目标偏移
    ASSERT_GE(chunk->code.size(), 3u);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_JUMP));
    uint16_t target = chunk->code[1] | (chunk->code[2] << 8);
    EXPECT_EQ(target, 3u); // 跳过 JUMP 自身（3 字节）到达 LABEL end 位置
}

TEST(BytecodeIRBackendControl, BackwardJumpResolvesToTargetOffset) {
    // JUMP backward → 回到前序 LABEL 位置
    LowerFixture f;
    uint32_t loopStart = f.ir.allocLabel();
    f.emit(IROp::LABEL, {IROperand::label(loopStart)});
    f.emit(IROp::JUMP, {IROperand::label(loopStart)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    ASSERT_GE(chunk->code.size(), 3u);
    EXPECT_EQ(chunk->code[0], static_cast<uint8_t>(OpCode::OP_JUMP));
    uint16_t target = chunk->code[1] | (chunk->code[2] << 8);
    EXPECT_EQ(target, 0u); // 回到起始 LABEL 位置
}

// ============================================================
// 调用与返回 → OP_CALL/OP_RETURN/OP_RETURN_NULL
// ============================================================

TEST(BytecodeIRBackendCall, ReturnEmitsOP_RETURN) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::RETURN, {v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_RETURN));
}

TEST(BytecodeIRBackendCall, ReturnNullEmitsOP_NULL_then_OP_RETURN) {
    // RETURN_NULL 在栈式 VM 后端 lowering 为 OP_NULL + OP_RETURN 两字节
    // （栈式 VM 无独立 OP_RETURN_NULL 指令，先 push null 作为返回值再 return）
    LowerFixture f;
    f.emit(IROp::RETURN_NULL, {});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_NULL));
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_RETURN));
}

TEST(BytecodeIRBackendCall, CallEmitsOP_CALL) {
    LowerFixture f;
    IROperand dst = f.ir.allocVReg();
    uint32_t nameIdx = f.ir.addGlobal("f");
    f.emit(IROp::CALL, {dst, IROperand::global(nameIdx), IROperand::imm(0)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_CALL));
}

// ============================================================
// 容器构造与索引
// ============================================================

TEST(BytecodeIRBackendContainer, BuildArrayEmitsOP_BUILD_ARRAY) {
    LowerFixture f;
    IROperand dst = f.ir.allocVReg();
    f.emit(IROp::BUILD_ARRAY, {dst, IROperand::imm(0)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_BUILD_ARRAY));
}

TEST(BytecodeIRBackendContainer, BuildDictEmitsOP_BUILD_DICT) {
    LowerFixture f;
    IROperand dst = f.ir.allocVReg();
    f.emit(IROp::BUILD_DICT, {dst, IROperand::imm(0)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_BUILD_DICT));
}

TEST(BytecodeIRBackendContainer, IndexGetEmitsOP_INDEX_GET) {
    LowerFixture f;
    IROperand dst = f.ir.allocVReg();
    IROperand obj = f.ir.allocVReg();
    IROperand idx = f.ir.allocVReg();
    f.emit(IROp::INDEX_GET, {dst, obj, idx});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_INDEX_GET));
}

TEST(BytecodeIRBackendContainer, IndexSetEmitsOP_INDEX_SET) {
    LowerFixture f;
    IROperand obj = f.ir.allocVReg();
    IROperand idx = f.ir.allocVReg();
    IROperand val = f.ir.allocVReg();
    f.emit(IROp::INDEX_SET, {obj, idx, val});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_INDEX_SET));
}

// ============================================================
// 成员访问 → OP_MEMBER_GET/OP_MEMBER_SET
// ============================================================

TEST(BytecodeIRBackendMember, MemberGetEmitsOP_MEMBER_GET) {
    LowerFixture f;
    IROperand dst = f.ir.allocVReg();
    IROperand obj = f.ir.allocVReg();
    uint32_t fieldName = f.ir.addGlobal("x");
    f.emit(IROp::MEMBER_GET, {dst, obj, IROperand::field(fieldName)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_MEMBER_GET));
}

TEST(BytecodeIRBackendMember, MemberSetEmitsOP_MEMBER_SET) {
    LowerFixture f;
    IROperand obj = f.ir.allocVReg();
    uint32_t fieldName = f.ir.addGlobal("x");
    IROperand val = f.ir.allocVReg();
    f.emit(IROp::MEMBER_SET, {obj, IROperand::field(fieldName), val});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_MEMBER_SET));
}

// ============================================================
// 异常处理 → OP_TRY_BEGIN/OP_TRY_END/OP_THROW
// ============================================================

TEST(BytecodeIRBackendException, TryBeginEmitsOP_TRY_BEGIN) {
    LowerFixture f;
    uint32_t catchLabel = f.ir.allocLabel();
    f.emit(IROp::TRY_BEGIN, {IROperand::label(catchLabel)});
    // catch 目标 LABEL 必须存在且位于 TRY_BEGIN 之后（相对偏移回填要求）
    f.emit(IROp::LABEL, {IROperand::label(catchLabel)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_TRY_BEGIN));
}

TEST(BytecodeIRBackendException, TryEndEmitsOP_TRY_END) {
    LowerFixture f;
    f.emit(IROp::TRY_END, {});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_TRY_END));
}

TEST(BytecodeIRBackendException, ThrowEmitsOP_THROW) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::THROW, {v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_THROW));
}

// ============================================================
// print/POP/DUP
// ============================================================

TEST(BytecodeIRBackendMisc, PrintEmitsOP_PRINT) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::PRINT, {v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_PRINT));
}

TEST(BytecodeIRBackendMisc, DupEmitsOP_DUP) {
    LowerFixture f;
    IROperand dst = f.ir.allocVReg();
    IROperand src = f.ir.allocVReg();
    f.emit(IROp::DUP, {dst, src});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_DUP));
}

TEST(BytecodeIRBackendMisc, PopEmitsOP_POP) {
    LowerFixture f;
    IROperand v = f.ir.allocVReg();
    f.emit(IROp::POP, {v});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_POP));
}

// ============================================================
// 多函数 lowering (lowerModule)
// ============================================================

TEST(BytecodeIRBackendModule, LowerModuleProducesMainChunk) {
    IRModule module;
    auto mainFn = std::make_unique<IRFunction>();
    mainFn->name = "main";
    IRBasicBlock block;
    block.labelIndex = mainFn->allocLabel();
    // [[maybe_unused]]：仅需 allocVReg 的分配副作用，返回值不参与断言
    [[maybe_unused]] IROperand v = mainFn->allocVReg();
    block.instructions.emplace_back(IROp::RETURN_NULL, std::vector<IROperand>{}, 1);
    mainFn->blocks.push_back(std::move(block));
    module.mainFunction = std::move(mainFn);

    BytecodeIRBackend backend;
    ASSERT_TRUE(backend.lowerModule(module));
    auto chunk = backend.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->name, "main");
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_NULL));
    EXPECT_TRUE(containsOpCode(chunk->code, OpCode::OP_RETURN));
}

TEST(BytecodeIRBackendModule, LowerModuleWithSubFunctionProducesFunctionChunk) {
    IRModule module;
    // main 调用 f 并返回
    auto mainFn = std::make_unique<IRFunction>();
    mainFn->name = "main";
    IRBasicBlock mainBlock;
    mainBlock.labelIndex = mainFn->allocLabel();
    mainBlock.instructions.emplace_back(IROp::RETURN_NULL, std::vector<IROperand>{}, 1);
    mainFn->blocks.push_back(std::move(mainBlock));
    module.mainFunction = std::move(mainFn);

    // 子函数 f
    auto subFn = std::make_unique<IRFunction>();
    subFn->name = "f";
    IRBasicBlock subBlock;
    subBlock.labelIndex = subFn->allocLabel();
    IROperand rv = subFn->allocVReg();
    subBlock.instructions.emplace_back(IROp::RETURN, std::vector<IROperand>{rv}, 1);
    subFn->blocks.push_back(std::move(subBlock));
    module.addFunction(std::move(subFn));

    BytecodeIRBackend backend;
    ASSERT_TRUE(backend.lowerModule(module));
    auto mainChunk = backend.takeChunk();
    ASSERT_NE(mainChunk, nullptr);
    auto functionChunks = backend.takeFunctionChunks();
    EXPECT_EQ(functionChunks.count("f"), 1u);
    EXPECT_TRUE(containsOpCode(functionChunks.at("f").code, OpCode::OP_RETURN));
}

// ============================================================
// irToBytecodeOffset 映射正确性
// ============================================================

TEST(BytecodeIRBackendOffsetMap, EachIRInstructionMapsToBytecodeOffset) {
    // 3 条 IR 指令：LOAD_CONST/LOAD_CONST/ADD
    // 对应字节码：OP_INT(3B) + OP_INT(3B) + OP_ADD(1B)
    LowerFixture f;
    uint32_t c0 = f.ir.addConstant(Value(1));
    uint32_t c1 = f.ir.addConstant(Value(2));
    IROperand v0 = f.ir.allocVReg();
    IROperand v1 = f.ir.allocVReg();
    IROperand v2 = f.ir.allocVReg();
    f.emit(IROp::LOAD_CONST, {v0, IROperand::constant(c0)}, 1);
    f.emit(IROp::LOAD_CONST, {v1, IROperand::constant(c1)}, 2);
    f.emit(IROp::ADD, {v2, v0, v1}, 3);
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    const auto& map = f.backend.irToBytecodeOffset();
    // 至少 3 个映射条目（每条 IR 指令对应一个字节码偏移）
    EXPECT_GE(map.size(), 3u);
    // 偏移应递增
    for (size_t i = 1; i < map.size(); ++i) {
        EXPECT_GT(map[i].second, map[i - 1].second);
    }
}

// ============================================================
// 边界：空函数 + 错误处理
// ============================================================

TEST(BytecodeIRBackendEdge, EmptyIRFunctionProducesEmptyChunk) {
    LowerFixture f("empty");
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->name, "empty");
    EXPECT_TRUE(chunk->code.empty());
}

TEST(BytecodeIRBackendEdge, EmptyBlockProducesEmptyChunk) {
    LowerFixture f;
    // 仅 LABEL，无指令
    uint32_t label = f.ir.allocLabel();
    f.emit(IROp::LABEL, {IROperand::label(label)});
    ASSERT_TRUE(f.lower());
    auto chunk = f.takeChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(chunk->code.empty());
}
