// ============================================================
// TestCoverageGaps3 — 覆盖率缺口定向补充测试（第三批）
// ------------------------------------------------------------
// 目标：补充 docs/testing.md「后续覆盖率提升方向」中在 P3-19（TestCoverageGaps2）
// 之后仍点名待覆盖的低覆盖路径：
//   方向 4（并发原语）：rwlock 读写锁排斥边界（写者饥饿的确定性单线程边界）
//   方向 5（IR 优化）：LICM 嵌套循环 / GVN 含副作用表达式不消除 / 级联内联
//
// 设计原则（与 TestCoverageGaps2.cpp 完全一致）：
//   - rwlock 用例四后端一致性验证（Interpreter / StackVM / StackVM-IR / RegisterVM）
//   - rwlock 只用「单线程单持有者」安全模式：读锁持有时试写锁、写锁持有时试读锁，
//     绝不在同一线程重复获取共享锁（std::shared_mutex 同线程重入是 UB）
//   - IR 优化测试沿用「构造 IR + 调用 pass + 验证语义不变 / 不崩溃」模式
// ============================================================

#include <gtest/gtest.h>

#include "compiler/core/Compiler.h"
#include "compiler/ir/IR.h"
#include "compiler/ir/IRSSA.h"
#include "compiler/backend-reg/RegisterVM.h"
#include "compiler/backend-stack/VM.h"
#include "interpreter/Interpreter.h"
#include "interpreter/RuntimeExceptions.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <memory>
#include <string>
#include <vector>

// ============================================================
// 辅助函数：四后端执行器（不带模块，与 TestCoverageGaps2 一致）
// ============================================================

namespace {

std::string runInterpreter(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Interpreter interp;
    std::string out;
    interp.setOutputCallback([&](const std::string& s) { out += s; });
    try {
        interp.execute(*ast);
    } catch (const RuntimeError& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    } catch (const std::exception& e) {
        return out + "<runtime:" + std::string(e.what()) + ">";
    }
    return out;
}

std::string runStackVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

std::string runStackVM_IR(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseIR(true);
    auto cr = c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    VM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(cr);
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

std::string runRegVM(const std::string& src) {
    Lexer lx;
    auto tk = lx.scan(src);
    Parser p;
    auto ast = p.parse(tk);
    if (!ast)
        return "<parse-fail>";
    Compiler c;
    c.setUseRegisterVM(true);
    c.compile(*ast);
    if (c.getDiagnostics().hasErrors())
        return "<compile:" + c.getLastError() + ">";
    RegisterVM vm;
    std::string out;
    vm.setOutputCallback([&](const std::string& s) { out += s; });
    vm.execute(c.getLastRegisterResult());
    if (vm.hasError())
        return out + "<runtime:" + vm.getLastError() + ">";
    return out;
}

/// 统计函数中某 IROp 的总数
size_t countOp(const IRFunction& fn, IROp op) {
    size_t n = 0;
    for (const auto& blk : fn.blocks)
        for (const auto& instr : blk.instructions)
            if (instr.op == op)
                ++n;
    return n;
}

/// 统计指定函数（main 或子函数）中 CALL 指令数
size_t countCallIn(const IRFunction* fn) {
    if (!fn)
        return 0;
    return countOp(*fn, IROp::CALL);
}

// ============================================================
// IR 构造辅助：嵌套 while 循环 + 内层循环体含循环不变量
// ------------------------------------------------------------
// 语义等价：for i in [0,2): for j in [0,2): { sink = 99; j++ } i++
// 单 IRBasicBlock 含多段 LABEL 切片（沿用 TestCoverageGaps2 / TestIRSSA 模式）：
//   entry:      i=0; JUMP L_outerCond
//   L_outerCond: vi=LOAD i; lim=2; cmp=LT vi,lim; JUMP_IF_FALSE cmp, L_exit
//   L_outerBody: j=0; JUMP L_innerCond
//   L_innerCond: vj=LOAD j; jlim=2; jcmp=LT vj,jlim; JUMP_IF_FALSE jcmp, L_outerIncr
//   L_innerBody: inv=LOAD_CONST 99（不变量）; sink=inv; j=j+1; JUMP L_innerCond
//   L_outerIncr: i=i+1; JUMP L_outerCond
//   L_exit:      RETURN_NULL
// 局部槽：i=0, j=1, sink=2
// ============================================================
std::unique_ptr<IRFunction> buildNestedLoopWithInvariantIR() {
    auto ir = std::make_unique<IRFunction>();
    ir->name = "nested_loop_with_invariant";
    ir->arity = 0;

    uint32_t lOuterCond = ir->allocLabel(); // 0
    uint32_t lOuterBody = ir->allocLabel(); // 1
    uint32_t lInnerCond = ir->allocLabel(); // 2
    uint32_t lInnerBody = ir->allocLabel(); // 3
    uint32_t lOuterIncr = ir->allocLabel(); // 4
    uint32_t lExit = ir->allocLabel();      // 5

    uint32_t cZero = ir->addConstant(Value(0));
    uint32_t cTwo = ir->addConstant(Value(2));
    uint32_t cOne = ir->addConstant(Value(1));
    uint32_t cInv = ir->addConstant(Value(99)); // 循环不变量

    IRBasicBlock block;
    block.labelIndex = lOuterCond;

    // entry: i = 0; JUMP L_outerCond
    IROperand vZeroI = ir->allocVReg();
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vZeroI, IROperand::constant(cZero)}, 1);
    block.instructions.emplace_back(IROp::STORE_LOCAL, IROperandList{IROperand::local(0), vZeroI}, 2);
    block.instructions.emplace_back(IROp::JUMP, IROperandList{IROperand::label(lOuterCond)}, 3);

    // L_outerCond
    IROperand vI = ir->allocVReg();
    IROperand vLim = ir->allocVReg();
    IROperand vCmp = ir->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lOuterCond)}, 4);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, IROperandList{vI, IROperand::local(0)}, 5);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vLim, IROperand::constant(cTwo)}, 6);
    block.instructions.emplace_back(IROp::LT, IROperandList{vCmp, vI, vLim}, 7);
    block.instructions.emplace_back(IROp::JUMP_IF_FALSE, IROperandList{vCmp, IROperand::label(lExit)}, 8);

    // L_outerBody: j = 0; JUMP L_innerCond
    IROperand vZeroJ = ir->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lOuterBody)}, 9);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vZeroJ, IROperand::constant(cZero)}, 10);
    block.instructions.emplace_back(IROp::STORE_LOCAL, IROperandList{IROperand::local(1), vZeroJ}, 11);
    block.instructions.emplace_back(IROp::JUMP, IROperandList{IROperand::label(lInnerCond)}, 12);

    // L_innerCond
    IROperand vJ = ir->allocVReg();
    IROperand vJLim = ir->allocVReg();
    IROperand vJCmp = ir->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lInnerCond)}, 13);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, IROperandList{vJ, IROperand::local(1)}, 14);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vJLim, IROperand::constant(cTwo)}, 15);
    block.instructions.emplace_back(IROp::LT, IROperandList{vJCmp, vJ, vJLim}, 16);
    block.instructions.emplace_back(IROp::JUMP_IF_FALSE, IROperandList{vJCmp, IROperand::label(lOuterIncr)},
                                    17);

    // L_innerBody: sink = 99（不变量）; j = j + 1; JUMP L_innerCond
    IROperand vInv = ir->allocVReg();
    IROperand vJc = ir->allocVReg();
    IROperand vJOne = ir->allocVReg();
    IROperand vJn = ir->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lInnerBody)}, 18);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vInv, IROperand::constant(cInv)}, 19);
    block.instructions.emplace_back(IROp::STORE_LOCAL, IROperandList{IROperand::local(2), vInv}, 20);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, IROperandList{vJc, IROperand::local(1)}, 21);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vJOne, IROperand::constant(cOne)}, 22);
    block.instructions.emplace_back(IROp::ADD, IROperandList{vJn, vJc, vJOne}, 23);
    block.instructions.emplace_back(IROp::STORE_LOCAL, IROperandList{IROperand::local(1), vJn}, 24);
    block.instructions.emplace_back(IROp::JUMP, IROperandList{IROperand::label(lInnerCond)}, 25);

    // L_outerIncr: i = i + 1; JUMP L_outerCond
    IROperand vIc = ir->allocVReg();
    IROperand vIOne = ir->allocVReg();
    IROperand vIn = ir->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lOuterIncr)}, 26);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, IROperandList{vIc, IROperand::local(0)}, 27);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vIOne, IROperand::constant(cOne)}, 28);
    block.instructions.emplace_back(IROp::ADD, IROperandList{vIn, vIc, vIOne}, 29);
    block.instructions.emplace_back(IROp::STORE_LOCAL, IROperandList{IROperand::local(0), vIn}, 30);
    block.instructions.emplace_back(IROp::JUMP, IROperandList{IROperand::label(lOuterCond)}, 31);

    // L_exit
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lExit)}, 32);
    block.instructions.emplace_back(IROp::RETURN_NULL, IROperandList{}, 33);

    ir->blocks.push_back(std::move(block));
    ir->localCount = 3;
    ir->localSlotNames = {"i", "j", "sink"};
    return ir;
}

// ============================================================
// IR 构造辅助：双循环 + "gap 块仅含可外提纯计算"（触发 LICM 分区塌缩）
// ------------------------------------------------------------
// 语义等价：i=0; while i<3 { s1=42; } while i<2 { s2=7; }
// 关键构造：每个循环体内"JUMP_IF_FALSE 与下一 LABEL 之间"的 gap 节点仅含
// 一条可外提的 LOAD_CONST——全量外提后该 gap 节点从 CFG 分区中消失
// （节点数 9→8→7 递减），触发 licmPass 结构分析复用的塌缩重建回退路径；
// 第二个循环的外提发生在第一次塌缩重建之后，锁定"重建后的支配树/循环
// 信息仍可用"不变量。局部槽：i=0, s1=1, s2=2
// ============================================================
std::unique_ptr<IRFunction> buildTwoLoopsCollapsingGapIR() {
    auto ir = std::make_unique<IRFunction>();
    ir->name = "two_loops_collapsing_gap";
    ir->arity = 0;

    uint32_t lCond1 = ir->allocLabel(); // 0
    uint32_t lBody1 = ir->allocLabel(); // 1
    uint32_t lExit1 = ir->allocLabel(); // 2
    uint32_t lCond2 = ir->allocLabel(); // 3
    uint32_t lBody2 = ir->allocLabel(); // 4
    uint32_t lExit2 = ir->allocLabel(); // 5

    uint32_t cZero = ir->addConstant(Value(0));
    uint32_t cThree = ir->addConstant(Value(3));
    uint32_t cTwo = ir->addConstant(Value(2));
    uint32_t cInv1 = ir->addConstant(Value(42)); // 循环 1 不变量（gap 块）
    uint32_t cInv2 = ir->addConstant(Value(7));  // 循环 2 不变量（gap 块）

    IRBasicBlock block;
    block.labelIndex = lCond1;

    // 入口：i = 0; JUMP L_cond1（循环 1 preheader）
    IROperand vZero = ir->allocVReg();
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vZero, IROperand::constant(cZero)}, 1);
    block.instructions.emplace_back(IROp::STORE_LOCAL, IROperandList{IROperand::local(0), vZero}, 2);
    block.instructions.emplace_back(IROp::JUMP, IROperandList{IROperand::label(lCond1)}, 3);

    // L_cond1（循环 1 header）
    IROperand vI1 = ir->allocVReg();
    IROperand vLim1 = ir->allocVReg();
    IROperand vCmp1 = ir->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lCond1)}, 4);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, IROperandList{vI1, IROperand::local(0)}, 5);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vLim1, IROperand::constant(cThree)}, 6);
    block.instructions.emplace_back(IROp::LT, IROperandList{vCmp1, vI1, vLim1}, 7);
    block.instructions.emplace_back(IROp::JUMP_IF_FALSE, IROperandList{vCmp1, IROperand::label(lExit1)}, 8);

    // gap 块（JUMP_IF_FALSE 与 LABEL 之间，无 LABEL 起始）：仅含可外提纯计算
    IROperand vA = ir->allocVReg();
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vA, IROperand::constant(cInv1)}, 9);

    // L_body1：消费 vA 后回跳（STORE_LOCAL 不可外提，留在循环体内）
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lBody1)}, 10);
    block.instructions.emplace_back(IROp::STORE_LOCAL, IROperandList{IROperand::local(1), vA}, 11);
    block.instructions.emplace_back(IROp::JUMP, IROperandList{IROperand::label(lCond1)}, 12);

    // L_exit1 → 串接进入循环 2（循环 2 preheader）
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lExit1)}, 13);
    block.instructions.emplace_back(IROp::JUMP, IROperandList{IROperand::label(lCond2)}, 14);

    // L_cond2（循环 2 header）
    IROperand vI2 = ir->allocVReg();
    IROperand vLim2 = ir->allocVReg();
    IROperand vCmp2 = ir->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lCond2)}, 15);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, IROperandList{vI2, IROperand::local(0)}, 16);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vLim2, IROperand::constant(cTwo)}, 17);
    block.instructions.emplace_back(IROp::LT, IROperandList{vCmp2, vI2, vLim2}, 18);
    block.instructions.emplace_back(IROp::JUMP_IF_FALSE, IROperandList{vCmp2, IROperand::label(lExit2)}, 19);

    // gap 块 2：仅含可外提纯计算
    IROperand vB = ir->allocVReg();
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vB, IROperand::constant(cInv2)}, 20);

    // L_body2
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lBody2)}, 21);
    block.instructions.emplace_back(IROp::STORE_LOCAL, IROperandList{IROperand::local(2), vB}, 22);
    block.instructions.emplace_back(IROp::JUMP, IROperandList{IROperand::label(lCond2)}, 23);

    // L_exit2
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(lExit2)}, 24);
    block.instructions.emplace_back(IROp::RETURN_NULL, IROperandList{}, 25);

    ir->blocks.push_back(std::move(block));
    ir->localCount = 3;
    ir->localSlotNames = {"i", "s1", "s2"};
    return ir;
}

// ============================================================
// IR 构造辅助：单块函数含两次相同的 CALL（含副作用）
// ------------------------------------------------------------
//   LABEL L0
//   v0 = CALL f()          // 副作用调用
//   v1 = CALL f()          // 与上等价（但含副作用，GVN 不应消除）
//   v2 = ADD v0, v1
//   RETURN v2
// GVN 只对 isPureComputeForSSA 白名单内的纯计算做值编号，CALL/ADD 不在白名单，
// 故两次 CALL 必须保留。此测试锁定「含副作用表达式不被 GVN 消除」不变量。
// ============================================================
std::unique_ptr<IRFunction> buildTwoIdenticalCallsIR() {
    auto ir = std::make_unique<IRFunction>();
    ir->name = "two_identical_calls";
    ir->arity = 0;
    uint32_t fIdx = ir->addGlobal("f");

    IRBasicBlock block;
    block.labelIndex = ir->allocLabel();

    IROperand v0 = ir->allocVReg();
    IROperand v1 = ir->allocVReg();
    IROperand v2 = ir->allocVReg();

    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::CALL, IROperandList{v0, IROperand::funcName(fIdx), IROperand::imm(0)},
                                    2);
    block.instructions.emplace_back(IROp::CALL, IROperandList{v1, IROperand::funcName(fIdx), IROperand::imm(0)},
                                    3);
    block.instructions.emplace_back(IROp::ADD, IROperandList{v2, v0, v1}, 4);
    block.instructions.emplace_back(IROp::RETURN, IROperandList{v2}, 5);

    ir->blocks.push_back(std::move(block));
    ir->localCount = 0;
    return ir;
}

// ============================================================
// IR 构造辅助：级联内联链 main → a → b
// ------------------------------------------------------------
// buildLeaf: fn(x) { return x + 1; }（纯参数表达式，可内联）
// buildRelay: fn(x) { return target(x); }（含 1 个 CALL，无 STORE_LOCAL，可内联）
// buildMainCalling: main { return callee(10); }
// ============================================================
std::unique_ptr<IRFunction> buildLeaf(const std::string& name) {
    auto fn = std::make_unique<IRFunction>();
    fn->name = name;
    fn->arity = 1;
    IRBasicBlock block;
    block.labelIndex = fn->allocLabel();
    IROperand vParam = fn->allocVReg();
    IROperand vOne = fn->allocVReg();
    IROperand vAcc = fn->allocVReg();
    uint32_t cOne = fn->addConstant(Value(1));
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, IROperandList{vParam, IROperand::local(0)}, 2);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{vOne, IROperand::constant(cOne)}, 3);
    block.instructions.emplace_back(IROp::ADD, IROperandList{vAcc, vParam, vOne}, 4);
    block.instructions.emplace_back(IROp::RETURN, IROperandList{vAcc}, 5);
    fn->blocks.push_back(std::move(block));
    fn->localCount = 1;
    fn->localSlotNames = {"x"};
    return fn;
}

std::unique_ptr<IRFunction> buildRelay(const std::string& name, const std::string& target) {
    auto fn = std::make_unique<IRFunction>();
    fn->name = name;
    fn->arity = 1;
    uint32_t tIdx = fn->addGlobal(target);
    IRBasicBlock block;
    block.labelIndex = fn->allocLabel();
    IROperand vParam = fn->allocVReg();
    IROperand vRes = fn->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, IROperandList{vParam, IROperand::local(0)}, 2);
    block.instructions.emplace_back(
        IROp::CALL, IROperandList{vRes, IROperand::funcName(tIdx), IROperand::imm(1), vParam}, 3);
    block.instructions.emplace_back(IROp::RETURN, IROperandList{vRes}, 4);
    fn->blocks.push_back(std::move(block));
    fn->localCount = 1;
    fn->localSlotNames = {"x"};
    return fn;
}

std::unique_ptr<IRFunction> buildMainCalling(const std::string& calleeName) {
    auto mainFn = std::make_unique<IRFunction>();
    mainFn->name = "main";
    mainFn->arity = 0;
    uint32_t nameIdx = mainFn->addGlobal(calleeName);
    IRBasicBlock block;
    block.labelIndex = mainFn->allocLabel();
    IROperand mv0 = mainFn->allocVReg();
    IROperand mv1 = mainFn->allocVReg();
    uint32_t mc0 = mainFn->addConstant(Value(10));
    block.instructions.emplace_back(IROp::LABEL, IROperandList{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, IROperandList{mv0, IROperand::constant(mc0)}, 2);
    block.instructions.emplace_back(
        IROp::CALL, IROperandList{mv1, IROperand::funcName(nameIdx), IROperand::imm(1), mv0}, 3);
    block.instructions.emplace_back(IROp::RETURN, IROperandList{mv1}, 4);
    mainFn->blocks.push_back(std::move(block));
    return mainFn;
}

} // namespace

// ============================================================
// 第一组：rwlock 读写锁排斥边界（方向 4：并发原语）
// ------------------------------------------------------------
// 「写者饥饿」的确定性单线程边界：读锁持有期间写锁不可获取（tryWriteLock 返回 false），
// 写锁持有期间读锁不可获取（tryReadLock 返回 false）。四后端经 handleSyncObjectMethod
// 单点分发（RwLockInner 用 std::shared_mutex），语义天然一致。
// ============================================================

// 读锁持有时写锁被排斥（写者被读者阻塞——写者饥饿边界）
TEST(RwLockGaps3, WriteLockExcludedWhileReadHeld) {
    std::string src = "var rw = rwlock();"
                      "rw.readLock();"
                      "if (rw.tryWriteLock()) {"
                      "  print(\"w-acquired\");"
                      "  rw.writeUnlock();"
                      "} else {"
                      "  print(\"w-blocked\");"
                      "}"
                      "rw.readUnlock();"
                      "print(\"done\");";
    std::string expected = "w-blockeddone";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// 写锁持有时读锁被排斥（读者被写者阻塞）
TEST(RwLockGaps3, ReadLockExcludedWhileWriteHeld) {
    std::string src = "var rw = rwlock();"
                      "rw.writeLock();"
                      "if (rw.tryReadLock()) {"
                      "  print(\"r-acquired\");"
                      "  rw.readUnlock();"
                      "} else {"
                      "  print(\"r-blocked\");"
                      "}"
                      "rw.writeUnlock();"
                      "print(\"done\");";
    std::string expected = "r-blockeddone";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// 读锁释放后写锁可获取（排斥解除，写者不再饥饿）
TEST(RwLockGaps3, WriteAcquirableAfterReadReleased) {
    std::string src = "var rw = rwlock();"
                      "rw.readLock();"
                      "rw.readUnlock();"
                      "if (rw.tryWriteLock()) {"
                      "  print(\"w-acquired\");"
                      "  rw.writeUnlock();"
                      "} else {"
                      "  print(\"w-blocked\");"
                      "}"
                      "print(\"done\");";
    std::string expected = "w-acquireddone";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 第一组补：mutex 自死锁检测（方向 4：并发原语）
// ------------------------------------------------------------
// 既有 ConcurrencyGaps2.MutexDoubleLockGuardedByTryLock 仅验证 tryLock 防护。
// 本组验证新增的自死锁检测：同一线程重复 lock() 不再永久挂起，
// 而是 fail-fast 报 runtime 错误（std::mutex 非递归）。四后端经
// handleSyncObjectMethod 单点分发，语义天然一致。
// ============================================================

// 同一线程重复 lock → 检测到死锁，报 runtime 错误（含“死锁”关键字）
TEST(MutexDeadlockGaps3, SelfReLockDetectedAsRuntimeError) {
    std::string src = "var m = mutex();"
                      "m.lock();"
                      "m.lock();";
    auto isDeadlockErr = [](const std::string& out) {
        return out.find("<runtime:") != std::string::npos && out.find("\xe6\xad\xbb\xe9\x94\x81") != std::string::npos;
    };
    EXPECT_TRUE(isDeadlockErr(runInterpreter(src))) << runInterpreter(src);
    EXPECT_TRUE(isDeadlockErr(runStackVM(src))) << runStackVM(src);
    EXPECT_TRUE(isDeadlockErr(runStackVM_IR(src))) << runStackVM_IR(src);
    EXPECT_TRUE(isDeadlockErr(runRegVM(src))) << runRegVM(src);
}

// 正常 lock → unlock → lock 不误报死锁（owner 在 unlock 后正确清空）
TEST(MutexDeadlockGaps3, LockUnlockRelockNoFalsePositive) {
    std::string src = "var m = mutex();"
                      "m.lock();"
                      "m.unlock();"
                      "m.lock();"
                      "m.unlock();"
                      "print(\"ok\");";
    std::string expected = "ok";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// 自持有时 tryLock 返回 false（不抛、不触发 std::mutex 同线程 try_lock UB）
TEST(MutexDeadlockGaps3, TryLockWhileSelfHeldReturnsFalse) {
    std::string src = "var m = mutex();"
                      "m.lock();"
                      "if (m.tryLock()) { print(\"reacquired\"); m.unlock(); } else { print(\"blocked\"); }"
                      "m.unlock();"
                      "print(\"done\");";
    std::string expected = "blockeddone";
    EXPECT_EQ(runInterpreter(src), expected);
    EXPECT_EQ(runStackVM(src), expected);
    EXPECT_EQ(runStackVM_IR(src), expected);
    EXPECT_EQ(runRegVM(src), expected);
}

// ============================================================
// 第二组：LICM 嵌套循环（方向 5：IR 优化）
// ------------------------------------------------------------
// 现有 LICMGaps2.HoistsLoopInvariantLoadConst 仅覆盖单层 while 循环。
// 本测试构造两层嵌套循环（内层含循环不变量 LOAD_CONST 99），验证 LICM
// 在嵌套自然循环上的分析路径被覆盖，不崩溃，且控制流未被破坏。
// ============================================================
TEST(LICMNestedGaps3, NestedLoopNoCrashKeepsControlFlow) {
    auto ir = buildNestedLoopWithInvariantIR();

    size_t jumpBefore = countOp(*ir, IROp::JUMP);
    size_t jifBefore = countOp(*ir, IROp::JUMP_IF_FALSE);

    // LICM 对嵌套循环做自然循环检测 + 不变量分析（每轮重建 CFG 取新鲜指令区间，
    // 支配树/循环信息结构复用，块分区塌缩时重建——见 LICMCollapseGaps3 组）
    bool modified = licmPass(*ir);
    (void)modified; // LICM 保守，可能不修改；重点是嵌套循环路径被覆盖且不崩溃

    // 控制流完整性：跳转指令数不减少（LICM 只外提计算，不删除控制流边）
    EXPECT_GE(countOp(*ir, IROp::JUMP), jumpBefore) << "LICM 不应删除 JUMP 边";
    EXPECT_EQ(countOp(*ir, IROp::JUMP_IF_FALSE), jifBefore) << "LICM 不应删除条件跳转";
    // exit 块的 RETURN_NULL 保留
    EXPECT_EQ(countOp(*ir, IROp::RETURN_NULL), 1u) << "RETURN_NULL 应保留";
    // 所有基本块非空
    for (const auto& blk : ir->blocks) {
        EXPECT_FALSE(blk.instructions.empty()) << "LICM 后基本块不应为空";
    }
    SUCCEED();
}

// ============================================================
// 第五组：LICM 结构分析复用的分区塌缩回退路径（第四轮优化）
// ------------------------------------------------------------
// licmPass 现将 DominatorTree/NaturalLoopInfo 建一次跨不动点批次复用，仅当
// 重建 IRCFG 后节点数变化（块分区塌缩）时才重建。本测试手工构造触发塌缩的
// IR：循环体内 gap 节点（JUMP_IF_FALSE 与下一 LABEL 之间，无 LABEL 起始）仅含
// 一条可外提 LOAD_CONST，全量外提后 gap 节点消失 → 节点数 9→8→7 → 触发两次
// 塌缩重建；第二个循环的外提发生在第一次塌缩重建之后。断言锁定：
//   (1) 两个循环的不变量都被外提（modified 且外提指令位于对应循环 header
//       之前——第二次外提证明重建后的支配树/循环信息仍可用）；
//   (2) 外提是移动而非删除（LOAD_CONST/STORE_LOCAL/LABEL 计数不变）；
//   (3) 控制流完整（JUMP/JUMP_IF_FALSE 计数不变）。
// ============================================================
TEST(LICMCollapseGaps3, CollapsedGapRebuildStillHoistsSecondLoop) {
    auto ir = buildTwoLoopsCollapsingGapIR();

    size_t jumpBefore = countOp(*ir, IROp::JUMP);
    size_t jifBefore = countOp(*ir, IROp::JUMP_IF_FALSE);
    size_t loadConstBefore = countOp(*ir, IROp::LOAD_CONST);
    size_t storeLocalBefore = countOp(*ir, IROp::STORE_LOCAL);
    size_t labelBefore = countOp(*ir, IROp::LABEL);
    ASSERT_EQ(jumpBefore, 4u);
    ASSERT_EQ(jifBefore, 2u);
    ASSERT_EQ(loadConstBefore, 5u);
    ASSERT_EQ(storeLocalBefore, 3u);
    ASSERT_EQ(labelBefore, 6u);

    bool modified = licmPass(*ir);
    EXPECT_TRUE(modified) << "两个循环的不变量均应被外提";

    // 外提是移动而非删除：指令总数按类别守恒
    EXPECT_EQ(countOp(*ir, IROp::LOAD_CONST), loadConstBefore) << "LOAD_CONST 应移动而非删除";
    EXPECT_EQ(countOp(*ir, IROp::STORE_LOCAL), storeLocalBefore) << "STORE_LOCAL 应保留";
    EXPECT_EQ(countOp(*ir, IROp::LABEL), labelBefore) << "LABEL 应保留";
    // 控制流完整
    EXPECT_EQ(countOp(*ir, IROp::JUMP), jumpBefore) << "JUMP 边应保留";
    EXPECT_EQ(countOp(*ir, IROp::JUMP_IF_FALSE), jifBefore) << "条件跳转应保留";
    EXPECT_EQ(countOp(*ir, IROp::RETURN_NULL), 1u) << "RETURN_NULL 应保留";

    // 位置断言：外提常量位于对应循环 header LABEL 之前（ flattened 指令序）。
    // 第二个循环的外提发生在第一次塌缩重建之后——此断言失败即意味着重建后的
    // 支配树/循环信息不可用（节点 id 偏移导致外提错位或漏做）。
    auto flattenIndexOf = [&](IROp op, uint32_t constIdx) -> ptrdiff_t {
        ptrdiff_t idx = 0;
        for (const auto& blk : ir->blocks) {
            for (const auto& instr : blk.instructions) {
                bool match = instr.op == op;
                if (match && op == IROp::LOAD_CONST)
                    match = instr.operands.size() >= 2 && instr.operands[1].kind == IROperandKind::CONSTANT &&
                            instr.operands[1].index == constIdx;
                if (match)
                    return idx;
                ++idx;
            }
        }
        return -1;
    };
    auto labelIndexOf = [&](uint32_t labelIdx) -> ptrdiff_t {
        ptrdiff_t idx = 0;
        for (const auto& blk : ir->blocks) {
            for (const auto& instr : blk.instructions) {
                if (instr.op == IROp::LABEL && instr.operands.size() == 1 &&
                    instr.operands[0].kind == IROperandKind::LABEL && instr.operands[0].index == labelIdx)
                    return idx;
                ++idx;
            }
        }
        return -1;
    };
    ptrdiff_t inv1Pos = flattenIndexOf(IROp::LOAD_CONST, 3); // 常量池 3 = 42
    ptrdiff_t inv2Pos = flattenIndexOf(IROp::LOAD_CONST, 4); // 常量池 4 = 7
    ptrdiff_t cond1Pos = labelIndexOf(0);
    ptrdiff_t cond2Pos = labelIndexOf(3);
    ASSERT_GE(inv1Pos, 0);
    ASSERT_GE(inv2Pos, 0);
    ASSERT_GE(cond1Pos, 0);
    ASSERT_GE(cond2Pos, 0);
    EXPECT_LT(inv1Pos, cond1Pos) << "循环 1 不变量应外提到 L_cond1 之前（preheader）";
    EXPECT_LT(inv2Pos, cond2Pos) << "循环 2 不变量应外提到 L_cond2 之前（塌缩重建后仍正确）";
}

// ============================================================
// 第三组：GVN 含副作用表达式不消除（方向 5：IR 优化）
// ------------------------------------------------------------
// 现有 GVNGaps2 用两分支等价 ADD 表达式验证 GVN 不增加指令。本测试针对
// 「含副作用表达式」这一具体缺口：两次语法相同的 CALL f() 必须都保留——
// GVN 的 isPureComputeForSSA 白名单不含 CALL，故不参与值编号消除。
// ============================================================
TEST(GVNSideEffectGaps3, DoesNotEliminateRedundantCalls) {
    auto ir = buildTwoIdenticalCallsIR();

    size_t callBefore = countOp(*ir, IROp::CALL);
    ASSERT_EQ(callBefore, 2u) << "构造应含 2 次 CALL";

    bool modified = gvnPass(*ir);
    (void)modified;

    // 含副作用的 CALL 不被 GVN 合并——两次调用必须都保留
    EXPECT_EQ(countOp(*ir, IROp::CALL), 2u) << "GVN 不应消除含副作用的 CALL 表达式";
    // RETURN 保留
    EXPECT_EQ(countOp(*ir, IROp::RETURN), 1u);
    SUCCEED();
}

// ============================================================
// 第四组：级联内联（方向 5：IR 优化）
// ------------------------------------------------------------
// 2026-10-04 优化审计修复：inlinePass 原实现仅扫 caller blocks[0] 且单趟遍历
// （main 在前，子函数在后），main 内联的是 a 的「原始」副本（仍含 CALL b），
// main → a → b 的塌缩需调用方多趟迭代。现 inlinePass 内部迭代到不动点
// （最多 3 轮）并扫描 caller 全部块——单次调用即完成级联塌缩。
// 本组两个测试更新为锁定新行为：
//   (1) 单次 inlinePass 调用后 main 与 a 的 CALL 全部消除（级联一次到位）；
//   (2) 再次调用应收敛（返回 false，无更多内联）。
// ============================================================

// 单次调用：main 内联 a → 轮 2 内联残留的 CALL b → main 与 a 的 CALL 清零
TEST(InlineCascadeGaps3, SinglePassLeavesResidualCall) {
    IRModule module;
    module.mainFunction = buildMainCalling("a"); // main { return a(10); }
    module.addFunction(buildRelay("a", "b"));    // a(x) { return b(x); }
    module.addFunction(buildLeaf("b"));          // b(x) { return x + 1; }

    bool modified = inlinePass(module);
    EXPECT_TRUE(modified) << "调用应发生内联";
    // 不动点迭代下，轮 1 内联 a（原始副本含 CALL b），轮 2 继续内联该 CALL——
    // 单次调用即完全塌缩（旧行为：单趟残留 1 个 CALL，需调用方自行迭代）
    EXPECT_EQ(countCallIn(module.mainFunction.get()), 0u) << "单次调用后 main 应无 CALL（级联塌缩）";
    // a 内已把 b 内联，a 的 CALL 清零
    EXPECT_EQ(countCallIn(module.findFunction("a")), 0u) << "a 内的 b 应已内联";
}

// 收敛性：第二次调用应无更多内联（不动点已达成）
TEST(InlineCascadeGaps3, CascadeReachesFixpointZeroCalls) {
    IRModule module;
    module.mainFunction = buildMainCalling("a");
    module.addFunction(buildRelay("a", "b"));
    module.addFunction(buildLeaf("b"));

    // 首次调用：级联塌缩 main→a→b（旧行为需外部 while 循环 ≥2 趟，现内部迭代）
    ASSERT_TRUE(inlinePass(module)) << "首趟应发生内联";
    EXPECT_EQ(countCallIn(module.mainFunction.get()), 0u) << "不动点后 main 应无 CALL";
    EXPECT_EQ(countCallIn(module.findFunction("a")), 0u) << "不动点后 a 应无 CALL";
    // 第二次调用：已到不动点，无修改
    EXPECT_FALSE(inlinePass(module)) << "第二趟应收敛（无更多内联）";
}
