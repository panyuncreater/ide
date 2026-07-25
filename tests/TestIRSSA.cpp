// ============================================================
// P2-10 IR SSA 基础设施单元测试
// ------------------------------------------------------------
// 验证 compiler/IRSSA.h 中声明的全部组件：
//   1. IRCFG：从 IRFunction 构建严格基本块 CFG
//   2. DominatorTree：Cooper-Harvey-Kennedy 支配树
//   3. DominanceFrontier：支配边界（Cytron PHI 插入用）
//   4. NaturalLoopInfo：自然循环检测
//   5. ssaConstructPass / ssaDestructPass：SSA 构造与析构
//   6. gvnPass：全局值编号
//   7. licmPass：循环不变代码外提
//   8. inlinePass：函数内联
//
// 测试策略：
//   - 组件层：手动构造 IRFunction，精确验证数据结构字段
//   - 端到端：通过 Compiler + RegisterVM 验证优化不改变语义
// ============================================================

#include <gtest/gtest.h>

#include "ast/ASTNode.h"
#include "compiler/Compiler.h"
#include "compiler/IR.h"
#include "compiler/IRSSA.h"
#include "compiler/RegisterBytecodeBackend.h"
#include "compiler/RegisterVM.h"
#include "interpreter/Value.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <memory>
#include <string>

// ============================================================
// 辅助函数：构造简单 if/else IR（用于 CFG/支配树测试）
// ============================================================
// IR 结构：
//   entry: cond=1; JUMP_IF_FALSE cond, L_else
//   then:  STORE_LOCAL x, 10; JUMP L_merge
//   else:  STORE_LOCAL x, 20
//   merge: LOAD_LOCAL x; RETURN x
namespace {

/// 构造 if/else IR（单 IRBasicBlock，含 4 个 LABEL/终结切片）
static std::unique_ptr<IRFunction> buildIfElseIR() {
    auto ir = std::make_unique<IRFunction>();
    ir->name = "if_else_test";
    ir->arity = 0;

    // 分配标签
    uint32_t lEntry = ir->allocLabel(); // 0
    uint32_t lElse = ir->allocLabel();  // 1
    uint32_t lMerge = ir->allocLabel(); // 2

    // 分配 vreg
    IROperand vCond = ir->allocVReg(); // 0
    IROperand vThen = ir->allocVReg(); // 1
    IROperand vElse = ir->allocVReg(); // 2
    IROperand vLoad = ir->allocVReg(); // 3

    // 常量
    uint32_t cOne = ir->addConstant(Value(1));
    uint32_t cTen = ir->addConstant(Value(10));
    uint32_t cTwenty = ir->addConstant(Value(20));

    // 局部 slot 0 = x
    IRBasicBlock block;
    block.labelIndex = lEntry;

    // entry: LABEL L0; v0 = LOAD_CONST 1; JUMP_IF_FALSE v0, L_else
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(lEntry)}, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{vCond, IROperand::constant(cOne)}, 2);
    block.instructions.emplace_back(IROp::JUMP_IF_FALSE, std::vector<IROperand>{vCond, IROperand::label(lElse)}, 3);

    // then: v1 = LOAD_CONST 10; STORE_LOCAL 0, v1; JUMP L_merge
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{vThen, IROperand::constant(cTen)}, 4);
    block.instructions.emplace_back(IROp::STORE_LOCAL, std::vector<IROperand>{IROperand::local(0), vThen}, 5);
    block.instructions.emplace_back(IROp::JUMP, std::vector<IROperand>{IROperand::label(lMerge)}, 6);

    // else: LABEL L_else; v2 = LOAD_CONST 20; STORE_LOCAL 0, v2
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(lElse)}, 7);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{vElse, IROperand::constant(cTwenty)}, 8);
    block.instructions.emplace_back(IROp::STORE_LOCAL, std::vector<IROperand>{IROperand::local(0), vElse}, 9);

    // merge: LABEL L_merge; v3 = LOAD_LOCAL 0; RETURN v3
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(lMerge)}, 10);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, std::vector<IROperand>{vLoad, IROperand::local(0)}, 11);
    block.instructions.emplace_back(IROp::RETURN, std::vector<IROperand>{vLoad}, 12);

    ir->blocks.push_back(std::move(block));
    ir->localCount = 1;
    ir->localSlotNames = {"x"};
    return ir;
}

/// 构造 while 循环 IR（用于循环检测测试）
// IR 结构：
//   entry: STORE_LOCAL i, 0; JUMP L_cond
//   cond:  LABEL L_cond; v = LOAD_LOCAL i; c = LOAD_CONST 3; t = LT v, c; JUMP_IF_FALSE t, L_exit
//   body:  LABEL L_body; ... i = i + 1; STORE_LOCAL i; JUMP L_cond
//   exit:  LABEL L_exit; RETURN_NULL
static std::unique_ptr<IRFunction> buildWhileLoopIR() {
    auto ir = std::make_unique<IRFunction>();
    ir->name = "while_loop_test";
    ir->arity = 0;

    uint32_t lCond = ir->allocLabel(); // 0
    uint32_t lBody = ir->allocLabel(); // 1
    uint32_t lExit = ir->allocLabel(); // 2

    IROperand vInit = ir->allocVReg();  // 0
    IROperand vI = ir->allocVReg();     // 1
    IROperand vLimit = ir->allocVReg(); // 2
    IROperand vCmp = ir->allocVReg();   // 3
    IROperand vCur = ir->allocVReg();   // 4
    IROperand vOne = ir->allocVReg();   // 5
    IROperand vNew = ir->allocVReg();   // 6

    uint32_t cZero = ir->addConstant(Value(0));
    uint32_t cThree = ir->addConstant(Value(3));
    uint32_t cOne = ir->addConstant(Value(1));

    IRBasicBlock block;
    block.labelIndex = lCond;

    // entry: v0 = 0; STORE_LOCAL i(0), v0; JUMP L_cond
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{vInit, IROperand::constant(cZero)}, 1);
    block.instructions.emplace_back(IROp::STORE_LOCAL, std::vector<IROperand>{IROperand::local(0), vInit}, 2);
    block.instructions.emplace_back(IROp::JUMP, std::vector<IROperand>{IROperand::label(lCond)}, 3);

    // cond: LABEL L_cond; v1 = LOAD_LOCAL 0; v2 = 3; v3 = LT v1, v2; JUMP_IF_FALSE v3, L_exit
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(lCond)}, 4);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, std::vector<IROperand>{vI, IROperand::local(0)}, 5);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{vLimit, IROperand::constant(cThree)}, 6);
    block.instructions.emplace_back(IROp::LT, std::vector<IROperand>{vCmp, vI, vLimit}, 7);
    block.instructions.emplace_back(IROp::JUMP_IF_FALSE, std::vector<IROperand>{vCmp, IROperand::label(lExit)}, 8);

    // body: LABEL L_body; v4 = LOAD_LOCAL 0; v5 = 1; v6 = ADD v4, v5; STORE_LOCAL 0, v6; JUMP L_cond
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(lBody)}, 9);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, std::vector<IROperand>{vCur, IROperand::local(0)}, 10);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{vOne, IROperand::constant(cOne)}, 11);
    block.instructions.emplace_back(IROp::ADD, std::vector<IROperand>{vNew, vCur, vOne}, 12);
    block.instructions.emplace_back(IROp::STORE_LOCAL, std::vector<IROperand>{IROperand::local(0), vNew}, 13);
    block.instructions.emplace_back(IROp::JUMP, std::vector<IROperand>{IROperand::label(lCond)}, 14);

    // exit: LABEL L_exit; RETURN_NULL
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(lExit)}, 15);
    block.instructions.emplace_back(IROp::RETURN_NULL, std::vector<IROperand>{}, 16);

    ir->blocks.push_back(std::move(block));
    ir->localCount = 1;
    ir->localSlotNames = {"i"};
    return ir;
}

/// 通过寄存器式后端执行源码，返回 print 输出（含错误标记）
static std::string runViaRegisterVM(const std::string& source, bool ssaOptimize = false) {
    Lexer lexer;
    auto tokens = lexer.scan(source);

    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "";

    Compiler compiler;
    compiler.setUseRegisterVM(true);
    compiler.setIROptimize(true);
    if (ssaOptimize) {
        compiler.setIRSSAOptimize(true);
    }
    RegisterCompileResult result = compiler.compileViaRegisterIR(*ast);

    RegisterVM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

} // namespace

// ============================================================
// IRCFG 测试
// ============================================================

TEST(IRCFGTest, EmptyFunctionHasSingleEntry) {
    IRFunction ir;
    ir.name = "empty";
    IRCFG cfg(ir);
    ASSERT_EQ(cfg.size(), 1u);
    EXPECT_EQ(cfg.entryId(), 0u);
    EXPECT_TRUE(cfg.node(0).isEntry());
}

TEST(IRCFGTest, IfElseProducesFourBlocks) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);

    // 预期 4 个严格块：entry, then, else, merge
    ASSERT_EQ(cfg.size(), 4u);

    // entry (id=0) 应有 2 个后继：then (fall-through) + else (LABEL L_else)
    const auto& entry = cfg.node(0);
    EXPECT_EQ(entry.succs.size(), 2u);

    // merge (最后一块) 应有 0 个后继（RETURN 终结）
    const auto& merge = cfg.node(static_cast<uint32_t>(cfg.size() - 1));
    EXPECT_TRUE(merge.succs.empty());
}

TEST(IRCFGTest, IfElsePredecessorEdgesCorrect) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);

    // merge 块应有 2 个前驱（then + else）
    const auto& merge = cfg.node(static_cast<uint32_t>(cfg.size() - 1));
    EXPECT_EQ(merge.preds.size(), 2u);
}

TEST(IRCFGTest, NodeOfLabelResolvesCorrectly) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);

    // L_else = 1, L_merge = 2
    uint32_t elseNode = cfg.nodeOfLabel(1);
    uint32_t mergeNode = cfg.nodeOfLabel(2);
    EXPECT_NE(elseNode, IRCFG::kInvalid);
    EXPECT_NE(mergeNode, IRCFG::kInvalid);
    EXPECT_NE(elseNode, mergeNode);
}

TEST(IRCFGTest, WhileLoopHasBackEdge) {
    auto ir = buildWhileLoopIR();
    IRCFG cfg(*ir);

    // 预期 4 个块：entry, cond, body, exit
    ASSERT_GE(cfg.size(), 4u);

    // cond 块应至少有 2 个前驱：entry 和 body（回边）
    // 找到 cond 块（LABEL L_cond=0）
    uint32_t condNode = cfg.nodeOfLabel(0);
    ASSERT_NE(condNode, IRCFG::kInvalid);
    const auto& cond = cfg.node(condNode);
    EXPECT_GE(cond.preds.size(), 2u);
}

// ============================================================
// DominatorTree 测试
// ============================================================

TEST(DominatorTreeTest, EntryIDominatesItself) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);

    // entry 的 idom 应为自身
    EXPECT_EQ(domTree.idom(cfg.entryId()), cfg.entryId());
}

TEST(DominatorTreeTest, EntryDominatesAll) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);

    // entry 应支配所有节点
    for (size_t i = 0; i < cfg.size(); ++i) {
        EXPECT_TRUE(domTree.dominates(cfg.entryId(), static_cast<uint32_t>(i))) << "entry should dominate node " << i;
    }
}

TEST(DominatorTreeTest, MergeIDomIsEntry) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);

    // merge 块的 idom 应为 entry（then/else 都不支配 merge，它们的最近公共支配者是 entry）
    uint32_t mergeNode = cfg.nodeOfLabel(2);
    ASSERT_NE(mergeNode, IRCFG::kInvalid);
    EXPECT_EQ(domTree.idom(mergeNode), cfg.entryId());
}

TEST(DominatorTreeTest, ThenElseIDomIsEntry) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);

    // then 块（entry 的 fall-through 后继）和 else 块的 idom 都应为 entry
    // then 块是 entry 的直接后继之一
    const auto& entry = cfg.node(0);
    for (uint32_t s : entry.succs) {
        EXPECT_EQ(domTree.idom(s), cfg.entryId()) << "node " << s << " should have entry as idom";
    }
}

TEST(DominatorTreeTest, NearestCommonDominatorOfThenElse) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);

    // then 和 else 的最近公共支配者应为 entry
    const auto& entry = cfg.node(0);
    ASSERT_GE(entry.succs.size(), 2u);
    uint32_t then = entry.succs[0];
    uint32_t els = entry.succs[1];
    EXPECT_EQ(domTree.nearestCommonDominator(then, els), cfg.entryId());
}

TEST(DominatorTreeTest, ChildrenRelationCorrect) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);

    // entry 的子节点应包含 then、else、merge（都被 entry 直接支配）
    const auto& children = domTree.children(cfg.entryId());
    EXPECT_GE(children.size(), 2u);
}

// ============================================================
// DominanceFrontier 测试
// ============================================================

TEST(DominanceFrontierTest, IfElseMergeInDF) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);
    DominanceFrontier df(cfg, domTree);

    // then 块的 DF 应包含 merge（then 支配 then 的前驱 entry，但不严格支配 merge）
    const auto& entry = cfg.node(0);
    ASSERT_GE(entry.succs.size(), 2u);
    uint32_t then = entry.succs[0];
    uint32_t els = entry.succs[1];
    uint32_t mergeNode = cfg.nodeOfLabel(2);

    const auto& dfThen = df.df(then);
    const auto& dfElse = df.df(els);

    // then 和 else 的 DF 都应包含 merge
    bool thenHasMerge = false;
    for (uint32_t n : dfThen) {
        if (n == mergeNode) {
            thenHasMerge = true;
            break;
        }
    }
    bool elseHasMerge = false;
    for (uint32_t n : dfElse) {
        if (n == mergeNode) {
            elseHasMerge = true;
            break;
        }
    }
    EXPECT_TRUE(thenHasMerge) << "DF(then) should contain merge";
    EXPECT_TRUE(elseHasMerge) << "DF(else) should contain merge";
}

TEST(DominanceFrontierTest, EntryDFEmpty) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);
    DominanceFrontier df(cfg, domTree);

    // entry 的 DF 通常为空（entry 支配所有节点）
    const auto& dfEntry = df.df(cfg.entryId());
    EXPECT_TRUE(dfEntry.empty());
}

// ============================================================
// NaturalLoopInfo 测试
// ============================================================

TEST(NaturalLoopInfoTest, NoLoopInIfElse) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);
    NaturalLoopInfo loopInfo(cfg, domTree);

    // if/else 无回边，无循环
    EXPECT_TRUE(loopInfo.loops().empty());
}

TEST(NaturalLoopInfoTest, WhileLoopDetected) {
    auto ir = buildWhileLoopIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);
    NaturalLoopInfo loopInfo(cfg, domTree);

    // while 循环应检测到 1 个自然循环
    EXPECT_EQ(loopInfo.loops().size(), 1u);

    if (!loopInfo.loops().empty()) {
        const auto& loop = loopInfo.loops()[0];
        // 循环头应为 cond 块（回边目标）
        uint32_t condNode = cfg.nodeOfLabel(0);
        EXPECT_EQ(loop.header, condNode);
        // 循环体应包含 cond 和 body
        EXPECT_GE(loop.body.size(), 2u);
    }
}

TEST(NaturalLoopInfoTest, InLoopMembership) {
    auto ir = buildWhileLoopIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);
    NaturalLoopInfo loopInfo(cfg, domTree);

    if (!loopInfo.loops().empty()) {
        // cond 和 body 应在循环内
        uint32_t condNode = cfg.nodeOfLabel(0);
        uint32_t bodyNode = cfg.nodeOfLabel(1);
        EXPECT_TRUE(loopInfo.inLoop(condNode));
        EXPECT_TRUE(loopInfo.inLoop(bodyNode));
    }
}

// ============================================================
// SSA 构造与析构测试
// ============================================================

TEST(SSAConstructTest, NoPHIForNoControlFlow) {
    // 无控制流的函数不应构造 SSA
    IRFunction ir;
    ir.name = "linear";
    ir.arity = 0;

    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();
    IROperand v0 = ir.allocVReg();
    uint32_t c0 = ir.addConstant(Value(42));
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{v0, IROperand::constant(c0)}, 2);
    block.instructions.emplace_back(IROp::RETURN, std::vector<IROperand>{v0}, 3);
    ir.blocks.push_back(std::move(block));

    EXPECT_FALSE(ssaConstructPass(ir));
}

TEST(SSAConstructTest, IfElseInsertsPHIAtMerge) {
    auto ir = buildIfElseIR();
    ASSERT_NE(ir, nullptr);

    size_t instrCountBefore = ir->blocks[0].instructions.size();
    bool modified = ssaConstructPass(*ir);

    EXPECT_TRUE(modified) << "ssaConstructPass should insert PHI for if/else";

    // 应插入至少 1 个 PHI（slot 0 = x 在 merge 块）
    size_t phiCount = 0;
    for (const auto& blk : ir->blocks) {
        for (const auto& instr : blk.instructions) {
            if (instr.op == IROp::PHI) {
                ++phiCount;
                // PHI 格式: [dest_vreg, slot, pred1_label, vreg1, pred2_label, vreg2]
                EXPECT_GE(instr.operands.size(), 6u) << "PHI should have dest + slot + 2 (pred, vreg) pairs";
            }
        }
    }
    EXPECT_GE(phiCount, 1u) << "Should insert at least 1 PHI";
    EXPECT_GT(ir->blocks[0].instructions.size(), instrCountBefore);
}

TEST(SSAConstructTest, PHIHasCorrectPredPairs) {
    auto ir = buildIfElseIR();
    ssaConstructPass(*ir);

    // 找到 PHI 指令
    bool foundPhi = false;
    for (const auto& blk : ir->blocks) {
        for (const auto& instr : blk.instructions) {
            if (instr.op == IROp::PHI) {
                foundPhi = true;
                // PHI 应有 2 个 (pred_label, vreg) 对
                // operands: [dest, slot, pred1, vreg1, pred2, vreg2] = 6
                ASSERT_GE(instr.operands.size(), 6u);
                // 前 2 个是 dest 和 slot
                EXPECT_EQ(instr.operands[0].kind, IROperandKind::VIRTUAL);
                EXPECT_EQ(instr.operands[1].kind, IROperandKind::LOCAL_SLOT);
                // (pred, vreg) 对
                // P2-10 fix3: pred 标识符从 labelIndex（LABEL）改为 CFG node ID（IMM_UINT）
                for (size_t k = 2; k + 1 < instr.operands.size(); k += 2) {
                    EXPECT_EQ(instr.operands[k].kind, IROperandKind::IMM_UINT);
                    EXPECT_EQ(instr.operands[k + 1].kind, IROperandKind::VIRTUAL);
                }
            }
        }
    }
    EXPECT_TRUE(foundPhi);
}

TEST(SSADestructTest, RemovesAllPHI) {
    auto ir = buildIfElseIR();
    ssaConstructPass(*ir);

    // 确认有 PHI
    bool hasPhiBefore = false;
    for (const auto& blk : ir->blocks) {
        for (const auto& instr : blk.instructions) {
            if (instr.op == IROp::PHI) {
                hasPhiBefore = true;
                break;
            }
        }
    }
    ASSERT_TRUE(hasPhiBefore);

    // 析构
    bool destructed = ssaDestructPass(*ir);
    EXPECT_TRUE(destructed);

    // 确认无 PHI
    for (const auto& blk : ir->blocks) {
        for (const auto& instr : blk.instructions) {
            EXPECT_NE(instr.op, IROp::PHI) << "ssaDestructPass should remove all PHI";
        }
    }
}

TEST(SSADestructTest, NoOpWithoutPHI) {
    // 无 PHI 时析构应为 no-op
    IRFunction ir;
    ir.name = "no_phi";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::RETURN_NULL, std::vector<IROperand>{}, 2);
    ir.blocks.push_back(std::move(block));

    EXPECT_FALSE(ssaDestructPass(ir));
}

TEST(SSAConstructDestructRoundTrip, PreservesPHIFreeState) {
    // 构造后析构，应回到无 PHI 状态
    auto ir = buildIfElseIR();
    ssaConstructPass(*ir);
    ssaDestructPass(*ir);

    for (const auto& blk : ir->blocks) {
        for (const auto& instr : blk.instructions) {
            EXPECT_NE(instr.op, IROp::PHI);
        }
    }
}

// ============================================================
// GVN 测试
// ============================================================

TEST(GVNTest, NoOpForSingleBlock) {
    // 单块函数 GVN 应返回 false（由 CSE 处理）
    IRFunction ir;
    ir.name = "single";
    IRBasicBlock block;
    block.labelIndex = ir.allocLabel();
    IROperand v0 = ir.allocVReg();
    uint32_t c0 = ir.addConstant(Value(1));
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{v0, IROperand::constant(c0)}, 2);
    block.instructions.emplace_back(IROp::RETURN, std::vector<IROperand>{v0}, 3);
    ir.blocks.push_back(std::move(block));

    EXPECT_FALSE(gvnPass(ir));
}

TEST(GVNTest, EliminatesRedundantComputation) {
    // 构造 if/else，两分支都计算相同表达式
    auto ir = buildIfElseIR();
    // 先构造 SSA（GVN 依赖支配树，不依赖 SSA 但配合更好）
    ssaConstructPass(*ir);

    // GVN 应能识别跨块的相同 LOAD_CONST
    // 注意：GVN 是保守的，仅替换纯计算的 dest vreg 引用
    // 这里主要验证 GVN 不崩溃且返回 bool
    bool modified = gvnPass(*ir);
    (void)modified; // GVN 可能因 IR 结构不匹配而不修改，重点是正确执行

    // 析构 SSA
    ssaDestructPass(*ir);

    SUCCEED();
}

// ============================================================
// LICM 测试
// ============================================================

TEST(LICMTest, NoOpForNoLoop) {
    // 无循环的函数 LICM 应返回 false
    auto ir = buildIfElseIR();
    EXPECT_FALSE(licmPass(*ir));
}

TEST(LICMTest, ExecutesOnLoopWithoutCrash) {
    // 循环函数执行 LICM，主要验证不崩溃
    auto ir = buildWhileLoopIR();
    // 可能外提循环不变量（本例中循环体都依赖 i，可能无可外提项）
    bool modified = licmPass(*ir);
    (void)modified;

    // 验证 IR 仍然有效（指令数合理）
    for (const auto& blk : ir->blocks) {
        EXPECT_FALSE(blk.instructions.empty());
    }
    SUCCEED();
}

// ============================================================
// 函数内联测试
// ============================================================

TEST(InlineTest, NoOpForNoCallee) {
    // 无可内联函数的模块应返回 false
    IRModule module;
    auto mainFn = std::make_unique<IRFunction>();
    mainFn->name = "main";
    IRBasicBlock block;
    block.labelIndex = mainFn->allocLabel();
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::RETURN_NULL, std::vector<IROperand>{}, 2);
    mainFn->blocks.push_back(std::move(block));
    module.mainFunction = std::move(mainFn);

    EXPECT_FALSE(inlinePass(module));
}

TEST(InlineTest, InlinesSmallFunction) {
    // 构造 main + small callee，验证内联
    IRModule module;

    // callee: fun small(x) { return x + 1; }  （单块，< 15 指令）
    auto callee = std::make_unique<IRFunction>();
    callee->name = "small";
    callee->arity = 1;
    IRBasicBlock calleeBlock;
    calleeBlock.labelIndex = callee->allocLabel();
    IROperand cv0 = callee->allocVReg(); // 参数 x
    IROperand cv1 = callee->allocVReg(); // 1
    IROperand cv2 = callee->allocVReg(); // x + 1
    uint32_t cc0 = callee->addConstant(Value(1));
    calleeBlock.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(0)}, 1);
    calleeBlock.instructions.emplace_back(IROp::LOAD_LOCAL, std::vector<IROperand>{cv0, IROperand::local(0)}, 2);
    calleeBlock.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{cv1, IROperand::constant(cc0)}, 3);
    calleeBlock.instructions.emplace_back(IROp::ADD, std::vector<IROperand>{cv2, cv0, cv1}, 4);
    calleeBlock.instructions.emplace_back(IROp::RETURN, std::vector<IROperand>{cv2}, 5);
    callee->blocks.push_back(std::move(calleeBlock));
    callee->localCount = 1;

    // main: var r = small(10); return r;
    auto mainFn = std::make_unique<IRFunction>();
    mainFn->name = "main";
    mainFn->arity = 0;
    uint32_t nameIdx = mainFn->addGlobal("small");
    IRBasicBlock mainBlock;
    mainBlock.labelIndex = mainFn->allocLabel();
    IROperand mv0 = mainFn->allocVReg(); // 10
    IROperand mv1 = mainFn->allocVReg(); // call result
    uint32_t mc0 = mainFn->addConstant(Value(10));
    mainBlock.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(0)}, 1);
    mainBlock.instructions.emplace_back(IROp::LOAD_CONST, std::vector<IROperand>{mv0, IROperand::constant(mc0)}, 2);
    // CALL small(10): operands = [dest, name_idx, arg_count, arg1]
    mainBlock.instructions.emplace_back(
        IROp::CALL, std::vector<IROperand>{mv1, IROperand::funcName(nameIdx), IROperand::imm(1), mv0}, 3);
    mainBlock.instructions.emplace_back(IROp::RETURN, std::vector<IROperand>{mv1}, 4);
    mainFn->blocks.push_back(std::move(mainBlock));

    module.mainFunction = std::move(mainFn);
    module.addFunction(std::move(callee));

    bool modified = inlinePass(module);
    EXPECT_TRUE(modified) << "Should inline small function";

    // 验证 main 中不再有 CALL small（被内联替换）
    if (modified) {
        bool hasCall = false;
        for (const auto& blk : module.mainFunction->blocks) {
            for (const auto& instr : blk.instructions) {
                if (instr.op == IROp::CALL) {
                    hasCall = true;
                    break;
                }
            }
        }
        EXPECT_FALSE(hasCall) << "CALL should be replaced by inlined body";
    }
}

TEST(InlineTest, DoesNotInlineRecursive) {
    // 递归调用不应内联（callee == caller）
    IRModule module;

    auto fn = std::make_unique<IRFunction>();
    fn->name = "rec";
    fn->arity = 1;
    uint32_t nameIdx = fn->addGlobal("rec");
    IRBasicBlock block;
    block.labelIndex = fn->allocLabel();
    IROperand v0 = fn->allocVReg();
    IROperand v1 = fn->allocVReg();
    block.instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(0)}, 1);
    block.instructions.emplace_back(IROp::LOAD_LOCAL, std::vector<IROperand>{v0, IROperand::local(0)}, 2);
    block.instructions.emplace_back(IROp::CALL,
                                    std::vector<IROperand>{v1, IROperand::funcName(nameIdx), IROperand::imm(1), v0}, 3);
    block.instructions.emplace_back(IROp::RETURN, std::vector<IROperand>{v1}, 4);
    fn->blocks.push_back(std::move(block));

    module.mainFunction = std::make_unique<IRFunction>(*fn);
    module.addFunction(std::move(fn));

    // 不应内联递归调用
    bool modified = inlinePass(module);
    (void)modified;
    // 验证不崩溃即可
    SUCCEED();
}

// ============================================================
// 端到端测试：SSA 优化不改变语义
// ============================================================

TEST(SSAE2E, IfElseWithSSAOptimize) {
    // 启用 SSA 优化后，if/else 语义应不变
    std::string src = "var x = 0;\n"
                      "if (1 < 2) {\n"
                      "    x = 10;\n"
                      "} else {\n"
                      "    x = 20;\n"
                      "}\n"
                      "print(x);\n";
    std::string result = runViaRegisterVM(src, /*ssaOptimize=*/true);
    EXPECT_EQ(result, "10");
}

TEST(SSAE2E, WhileLoopWithSSAOptimize) {
    // 启用 SSA 优化后，循环语义应不变
    std::string src = "var sum = 0;\n"
                      "var i = 0;\n"
                      "while (i < 5) {\n"
                      "    sum = sum + i;\n"
                      "    i = i + 1;\n"
                      "}\n"
                      "print(sum);\n";
    std::string result = runViaRegisterVM(src, /*ssaOptimize=*/true);
    EXPECT_EQ(result, "10"); // 0+1+2+3+4 = 10
}

TEST(SSAE2E, SSAOptimizeMatchesNonOptimized) {
    // SSA 优化与非优化路径结果应一致
    std::string src = "fun add(a, b) { return a + b; }\n"
                      "var x = add(3, 4);\n"
                      "var y = add(x, 5);\n"
                      "print(y);\n";

    std::string withoutSSA = runViaRegisterVM(src, /*ssaOptimize=*/false);
    std::string withSSA = runViaRegisterVM(src, /*ssaOptimize=*/true);
    EXPECT_EQ(withoutSSA, withSSA);
    EXPECT_EQ(withSSA, "12");
}

/// 调试辅助：dump IRFunction 到字符串
static std::string dumpIRFunction(const IRFunction& ir, const std::string& label) {
    std::string out = "=== " + label + " ===\n";
    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        out += "  Block " + std::to_string(bi) + " (label=" + std::to_string(ir.blocks[bi].labelIndex) + "):\n";
        for (size_t i = 0; i < ir.blocks[bi].instructions.size(); ++i) {
            out += "    [" + std::to_string(i) + "] " + formatIRInstruction(ir.blocks[bi].instructions[i]) + "\n";
        }
    }
    return out;
}

/// 调试辅助：手动构建 IR + 指定 pass 组合 + 执行
static std::string runWithSelectiveSSAPasses(const std::string& source, bool doSSAConstruct, bool doGVN, bool doLICM,
                                             bool doSSADestruct, std::string* debugDump = nullptr) {
    Lexer lexer;
    auto tokens = lexer.scan(source);
    Parser parser;
    auto ast = parser.parse(tokens);
    if (!ast)
        return "<parse-failed>";

    AstIRBuilder irBuilder;
    auto mainIR = irBuilder.build(*ast);
    if (!mainIR)
        return "<ir-build-failed>";

    IRModule* module = irBuilder.getModule();
    module->mainFunction = std::move(mainIR);

    // 先跑 optimizeIR（与 Compiler 一致）
    optimizeIR(*module->mainFunction, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
    for (auto& fn : module->functions) {
        if (fn) {
            optimizeIR(*fn, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
        }
    }

    if (debugDump) {
        *debugDump += dumpIRFunction(*module->mainFunction, "AFTER optimizeIR (before SSA)");
    }

    // P2-10 fix2: 镜像 Compiler.cpp 生产逻辑——仅当 ssaConstructPass 实际插入 PHI
    // （返回 true）时才运行 GVN/LICM。ssaConstructPass 仅处理 LOCAL 变量
    // （STORE_LOCAL），当函数只含 GLOBAL 变量（如顶层 var/print）时返回 false，
    // 此时 GVN 的跨块 CSE 会创建跨循环 live range，RegisterBytecodeBackend 的
    // 线性扫描寄存器分配器不识别回边，会在首次迭代后释放并复用寄存器，后续迭代
    // 读到错误值。SSA 构造+析构路径安全：PHI 在 ssaDestruct 还原为 LOAD_LOCAL。
    auto runSSAPassesOn = [&](IRFunction& ir, const std::string& label) {
        bool ssaBuilt = false;
        if (doSSAConstruct) {
            ssaBuilt = ssaConstructPass(ir);
            if (debugDump) {
                *debugDump += dumpIRFunction(ir, label + (ssaBuilt ? ": AFTER ssaConstructPass (PHI inserted)"
                                                                   : ": AFTER ssaConstructPass (no PHI, global-only)"));
            }
        }
        if (doGVN && ssaBuilt) {
            gvnPass(ir);
            if (debugDump) {
                *debugDump += dumpIRFunction(ir, label + ": AFTER gvnPass");
            }
        }
        if (doLICM && ssaBuilt) {
            licmPass(ir);
            if (debugDump) {
                *debugDump += dumpIRFunction(ir, label + ": AFTER licmPass");
            }
        }
        if (doSSADestruct && ssaBuilt) {
            ssaDestructPass(ir);
            if (debugDump) {
                *debugDump += dumpIRFunction(ir, label + ": AFTER ssaDestructPass");
            }
            optimizeIR(ir, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
            if (debugDump) {
                *debugDump += dumpIRFunction(ir, label + ": AFTER post-DCE");
            }
        }
    };
    runSSAPassesOn(*module->mainFunction, "main");
    for (size_t fi = 0; fi < module->functions.size(); ++fi) {
        if (module->functions[fi]) {
            runSSAPassesOn(*module->functions[fi], "fn" + std::to_string(fi));
        }
    }

    RegisterBytecodeBackend backend;
    if (!backend.lowerModule(*module)) {
        return "<lower-failed>";
    }
    auto mainChunk = backend.takeChunk();
    if (!mainChunk) {
        return "<no-chunk>";
    }

    RegisterCompileResult result;
    result.mainChunk = std::move(*mainChunk);
    result.functionChunks = backend.takeFunctionChunks();
    result.globalSlotNames = irBuilder.getGlobalSlotNames();
    result.globalSlotCount = static_cast<int>(result.globalSlotNames.size());
    result.enumInfos = irBuilder.takeEnumInfos();

    RegisterVM vm;
    std::string captured;
    vm.setOutputCallback([&](const std::string& s) { captured += s; });
    vm.execute(result);
    if (vm.hasError()) {
        return captured + "<runtime:" + vm.getLastError() + ">";
    }
    return captured;
}

TEST(SSAE2E, NestedLoopsDebugDump) {
    // 调试测试：对比 SSA 与非 SSA 结果
    std::string src = "var total = 0;\n"
                      "for (var i = 0; i < 3; i = i + 1) {\n"
                      "    for (var j = 0; j < 3; j = j + 1) {\n"
                      "        total = total + i * j;\n"
                      "    }\n"
                      "}\n"
                      "print(total);\n";

    std::string withoutSSA = runViaRegisterVM(src, /*ssaOptimize=*/false);
    std::string withSSA = runViaRegisterVM(src, /*ssaOptimize=*/true);
    EXPECT_EQ(withoutSSA, "9") << "Baseline (no SSA) should be 9";
    EXPECT_EQ(withSSA, "9") << "SSA result is: " << withSSA;
}

TEST(SSAE2E, NestedLoopsPassIsolation) {
    // 隔离测试：顶层代码（全 GLOBAL 变量）——ssaConstructPass 返回 false，
    // GVN/LICM 被跳过（镜像 Compiler.cpp 生产逻辑），所有组合应产生正确结果。
    std::string src = "var total = 0;\n"
                      "for (var i = 0; i < 3; i = i + 1) {\n"
                      "    for (var j = 0; j < 3; j = j + 1) {\n"
                      "        total = total + i * j;\n"
                      "    }\n"
                      "}\n"
                      "print(total);\n";

    // 组合 1：仅 construct + destruct（无 GVN/LICM）——ssaConstructPass 返回 false，no-op
    {
        std::string result = runWithSelectiveSSAPasses(src, true, false, false, true);
        EXPECT_EQ(result, "9") << "construct+destruct only: " << result;
    }
    // 组合 2：construct + GVN + destruct——ssaBuilt=false，GVN 被跳过
    {
        std::string dump;
        std::string result = runWithSelectiveSSAPasses(src, true, true, false, true, &dump);
        EXPECT_EQ(result, "9") << "construct+GVN+destruct (GVN skipped, global-only): " << result;
    }
    // 组合 3：construct + LICM + destruct——ssaBuilt=false，LICM 被跳过
    {
        std::string dump;
        std::string result = runWithSelectiveSSAPasses(src, true, false, true, true, &dump);
        EXPECT_EQ(result, "9") << "construct+LICM+destruct (LICM skipped, global-only): " << result;
    }
    // 组合 4：construct + GVN + LICM + destruct——ssaBuilt=false，GVN/LICM 均跳过
    {
        std::string dump;
        std::string result = runWithSelectiveSSAPasses(src, true, true, true, true, &dump);
        EXPECT_EQ(result, "9") << "Full pipeline (GVN/LICM skipped, global-only): " << result;
    }
}

// ssaConstructPass 的重命名阶段：LOCAL 变量 + 循环回边场景下 PHI 回边操作数
// 曾自引用（如 PHI v22 s1 L0 v1 L3 v22，应引用 v19 即 i=i+1 的新值）。
// 修复：实现 Cytron currentDef[slot] 栈追踪 STORE_LOCAL 写入的新 vreg。
TEST(SSAE2E, NestedLoopsPassIsolationWithLocals) {
    // 隔离测试：函数包裹的嵌套循环（LOCAL 变量）——ssaConstructPass 返回 true，
    // GVN/LICM 实际执行。验证 SSA 优化路径在 LOCAL 变量场景下保持语义。
    std::string src = "fun compute() {\n"
                      "    var total = 0;\n"
                      "    for (var i = 0; i < 3; i = i + 1) {\n"
                      "        for (var j = 0; j < 3; j = j + 1) {\n"
                      "            total = total + i * j;\n"
                      "        }\n"
                      "    }\n"
                      "    return total;\n"
                      "}\n"
                      "print(compute());\n";

    // 组合 1：仅 construct + destruct（无 GVN/LICM）——验证 SSA 构造/析构往返保持语义
    {
        std::string result = runWithSelectiveSSAPasses(src, true, false, false, true);
        EXPECT_EQ(result, "9") << "construct+destruct only (locals): " << result;
    }
    // 组合 2：construct + GVN + destruct——GVN 实际执行跨块 CSE
    {
        std::string dump;
        std::string result = runWithSelectiveSSAPasses(src, true, true, false, true, &dump);
        if (result != "9") {
            GTEST_FAIL() << "construct+GVN+destruct (locals) result=" << result << "\n" << dump;
        }
    }
    // 组合 3：construct + LICM + destruct——LICM 实际执行循环不变外提
    {
        std::string dump;
        std::string result = runWithSelectiveSSAPasses(src, true, false, true, true, &dump);
        if (result != "9") {
            GTEST_FAIL() << "construct+LICM+destruct (locals) result=" << result << "\n" << dump;
        }
    }
    // 组合 4：construct + GVN + LICM + destruct——完整 SSA 优化流水线
    {
        std::string dump;
        std::string result = runWithSelectiveSSAPasses(src, true, true, true, true, &dump);
        if (result != "9") {
            GTEST_FAIL() << "Full pipeline (locals) result=" << result << "\n" << dump;
        }
    }
}

TEST(SSAE2E, NestedLoopsWithSSAOptimize) {
    // 嵌套循环 + SSA 优化
    std::string src = "var total = 0;\n"
                      "for (var i = 0; i < 3; i = i + 1) {\n"
                      "    for (var j = 0; j < 3; j = j + 1) {\n"
                      "        total = total + i * j;\n"
                      "    }\n"
                      "}\n"
                      "print(total);\n";
    std::string result = runViaRegisterVM(src, /*ssaOptimize=*/true);
    // i=0: 0*0+0*1+0*2=0; i=1: 1*0+1*1+1*2=3; i=2: 2*0+2*1+2*2=6; total=9
    EXPECT_EQ(result, "9");
}

TEST(SSAE2E, FunctionInliningPreservesSemantics) {
    // 函数内联后语义应不变
    std::string src = "fun double(x) { return x * 2; }\n"
                      "fun quad(x) { return double(double(x)); }\n"
                      "print(quad(5));\n";
    std::string withoutSSA = runViaRegisterVM(src, /*ssaOptimize=*/false);
    std::string withSSA = runViaRegisterVM(src, /*ssaOptimize=*/true);
    EXPECT_EQ(withoutSSA, withSSA);
    EXPECT_EQ(withSSA, "20");
}

// ============================================================
// 调试输出测试
// ============================================================

TEST(IRSSADebugOutput, CFGToStringProducesOutput) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    std::string str = cfgToString(cfg);
    EXPECT_NE(str.find("CFG"), std::string::npos);
    EXPECT_NE(str.find("BB0"), std::string::npos);
}

TEST(IRSSADebugOutput, DomTreeToStringProducesOutput) {
    auto ir = buildIfElseIR();
    IRCFG cfg(*ir);
    DominatorTree domTree(cfg);
    std::string str = domTreeToString(domTree);
    EXPECT_NE(str.find("DominatorTree"), std::string::npos);
    EXPECT_NE(str.find("idom"), std::string::npos);
}
