// ============================================================
// IRSSA.cpp - P2-10 IR SSA 基础设施实现
// ============================================================
// 本文件实现 IRSSA.h 中声明的全部基础设施：
//   1. IRCFG：从 IRFunction 构建严格基本块 CFG
//   2. DominatorTree：Cooper-Harvey-Kennedy 支配树
//   3. DominanceFrontier：支配边界（Cytron PHI 插入用）
//   4. NaturalLoopInfo：自然循环检测（回边 + 循环体）
//   5. ssaConstructPass / ssaDestructPass：SSA 构造与析构
//   6. gvnPass：全局值编号（跨基本块 CSE）
//   7. licmPass：循环不变代码外提
//   8. inlinePass：函数内联
//
// 设计约束：
//   - GVN/LICM 仅对寄存器式后端安全（与 CSE 同源约束），由调用方控制
//   - PHI 仅在 ssaConstruct 与 ssaDestruct 之间存在，后端 lowering 永不看到 PHI
//   - IRCFG 是只读分析视图，不修改 IRFunction 结构
// ============================================================

#include "compiler/IRSSA.h"

#include "common/Logger.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stack>

// ============================================================
// 辅助函数
// ============================================================
namespace {

/// 判断 IR 指令是否为终结指令（结束一个严格基本块）
bool isTerminatorOp(IROp op) {
    switch (op) {
    case IROp::JUMP:
    case IROp::JUMP_IF_FALSE:
    case IROp::RETURN:
    case IROp::RETURN_NULL:
    case IROp::THROW:
        return true;
    default:
        return false;
    }
}

/// 判断指令是否为纯计算（可参与 GVN/LICM，无副作用）
/// 与 IR.cpp 的 isPureCompute 对齐，但此处用于 SSA pass 内部
bool isPureComputeForSSA(IROp op) {
    switch (op) {
    case IROp::LOAD_CONST:
    case IROp::LOAD_NULL:
    case IROp::LOAD_TRUE:
    case IROp::LOAD_FALSE:
    case IROp::NOT:
    case IROp::EQ:
    case IROp::NEQ:
    case IROp::LT:
    case IROp::GT:
    case IROp::LTE:
    case IROp::GTE:
    case IROp::DUP:
        return true;
    default:
        return false;
    }
}

/// 判断指令是否有 dest vreg（首操作数为 VIRTUAL）
bool hasDestVReg(const IRInstruction& instr) {
    return !instr.operands.empty() && instr.operands[0].kind == IROperandKind::VIRTUAL;
}

/// 取 dest vreg（首操作数）
uint32_t getDestVReg(const IRInstruction& instr) {
    return instr.operands[0].index;
}

/// 将指令编码为值编号 key（用于 GVN/CSE）
std::string encodeValueKey(const IRInstruction& instr) {
    std::string key;
    key.push_back(static_cast<char>(instr.op));
    key.push_back('|');
    for (const auto& op : instr.operands) {
        // dest vreg 不编码进 key（同表达式不同 dest 应合并）
        if (&op == &instr.operands[0] && op.kind == IROperandKind::VIRTUAL)
            continue;
        key.push_back(static_cast<char>(op.kind));
        key.push_back(':');
        key.append(std::to_string(op.index));
        key.push_back(',');
    }
    return key;
}

} // namespace

// ============================================================
// IRCFG 实现
// ============================================================

IRCFG::IRCFG(const IRFunction& ir) {
    // 阶段 1：遍历所有 IRBasicBlock，按 LABEL/终结指令切分为严格基本块
    // 每个 IRBasicBlock 通常以 LABEL 起始（AstIRBuilder::newBlock 保证）
    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        const auto& blk = ir.blocks[bi];
        if (blk.instructions.empty()) {
            CFGNode node;
            node.id = static_cast<uint32_t>(nodes_.size());
            node.sourceBlockIdx = bi;
            node.startInstr = 0;
            node.endInstr = 0;
            node.labelIndex = blk.labelIndex;
            node.hasLabel = false;
            labelToNode_[blk.labelIndex] = node.id;
            nodes_.push_back(node);
            continue;
        }

        size_t i = 0;
        bool firstChunk = true;
        while (i < blk.instructions.size()) {
            CFGNode node;
            node.id = static_cast<uint32_t>(nodes_.size());
            node.sourceBlockIdx = bi;
            node.startInstr = i;

            // 若以 LABEL 起始，记录标签
            if (blk.instructions[i].op == IROp::LABEL) {
                node.hasLabel = true;
                node.labelIndex = blk.instructions[i].operands[0].index;
                labelToNode_[node.labelIndex] = node.id;
                ++i;
            } else {
                node.hasLabel = false;
                // 继承所在 IRBasicBlock 的 labelIndex 作为 fallback
                if (firstChunk) {
                    node.labelIndex = blk.labelIndex;
                    labelToNode_[blk.labelIndex] = node.id;
                } else {
                    // 无 LABEL 的非首块：用前一个块的 label 作为 fallback
                    node.labelIndex = blk.labelIndex;
                }
            }
            firstChunk = false;

            // 扫描到下一条 LABEL 或终结指令之后
            while (i < blk.instructions.size()) {
                IROp op = blk.instructions[i].op;
                if (op == IROp::LABEL) {
                    break; // 新严格块起始
                }
                ++i;
                if (isTerminatorOp(op)) {
                    break; // 终结指令结束本块（含）
                }
            }
            node.endInstr = i; // [startInstr, endInstr)
            nodes_.push_back(node);
        }
    }

    // 若函数为空（无 blocks），创建单入口空块
    if (nodes_.empty()) {
        CFGNode entry;
        entry.id = 0;
        entry.sourceBlockIdx = 0;
        entry.startInstr = 0;
        entry.endInstr = 0;
        entry.labelIndex = CFGNode::kEntryLabel;
        entry.hasLabel = false;
        nodes_.push_back(entry);
        return;
    }

    // 阶段 2：构建后继边
    for (size_t ni = 0; ni < nodes_.size(); ++ni) {
        auto& node = nodes_[ni];
        const auto& blk = ir.blocks[node.sourceBlockIdx];

        if (node.startInstr >= node.endInstr) {
            // 空块：fall-through 到下一个节点
            if (ni + 1 < nodes_.size()) {
                node.succs.push_back(static_cast<uint32_t>(ni + 1));
            }
            continue;
        }

        size_t lastIdx = node.endInstr - 1;
        const auto& lastInstr = blk.instructions[lastIdx];

        switch (lastInstr.op) {
        case IROp::JUMP: {
            uint32_t targetLabel = lastInstr.operands[0].index;
            auto it = labelToNode_.find(targetLabel);
            if (it != labelToNode_.end()) {
                node.succs.push_back(it->second);
            }
            break;
        }
        case IROp::JUMP_IF_FALSE: {
            // fall-through + 目标
            if (ni + 1 < nodes_.size()) {
                node.succs.push_back(static_cast<uint32_t>(ni + 1));
            }
            if (lastInstr.operands.size() >= 2) {
                uint32_t targetLabel = lastInstr.operands[1].index;
                auto it = labelToNode_.find(targetLabel);
                if (it != labelToNode_.end()) {
                    node.succs.push_back(it->second);
                }
            }
            break;
        }
        case IROp::RETURN:
        case IROp::RETURN_NULL:
        case IROp::THROW:
            // 无后继
            break;
        default:
            // 隐式 fall-through
            if (ni + 1 < nodes_.size()) {
                node.succs.push_back(static_cast<uint32_t>(ni + 1));
            }
            break;
        }

        // BUGFIX-P2 #55: TRY_BEGIN 隐式异常边——TRY_BEGIN 不是终结指令（不改变块分割），
        // 但它声明了 try 块的 catch handler。若块内任一指令抛异常，控制流跳转到 catch label。
        // 扫描本节点所有指令，为每个 TRY_BEGIN 添加到 catch handler 块的隐式边。
        for (size_t ii = node.startInstr; ii < node.endInstr; ++ii) {
            const auto& scanInstr = blk.instructions[ii];
            if (scanInstr.op == IROp::TRY_BEGIN && !scanInstr.operands.empty()) {
                uint32_t catchLabel = scanInstr.operands[0].index;
                auto cit = labelToNode_.find(catchLabel);
                if (cit != labelToNode_.end()) {
                    // 避免重复边
                    bool alreadyExists = false;
                    for (uint32_t s : node.succs) {
                        if (s == cit->second) { alreadyExists = true; break; }
                    }
                    if (!alreadyExists) {
                        node.succs.push_back(cit->second);
                    }
                }
            }
        }
    }

    // 阶段 3：从后继边构建前驱边
    for (auto& node : nodes_) {
        for (uint32_t s : node.succs) {
            if (s < nodes_.size()) {
                nodes_[s].preds.push_back(node.id);
            }
        }
    }
}

bool IRCFG::isTerminator(IROp op) {
    return isTerminatorOp(op);
}

uint32_t IRCFG::nodeOfLabel(uint32_t labelIndex) const {
    auto it = labelToNode_.find(labelIndex);
    if (it == labelToNode_.end())
        return kInvalid;
    return it->second;
}

// ============================================================
// DominatorTree 实现（Cooper-Harvey-Kennedy 算法）
// ============================================================

DominatorTree::DominatorTree(const IRCFG& cfg) : cfg_(cfg) {
    size_t n = cfg.size();
    idom_.assign(n, kInvalid); // kInvalid 表示 undefined
    domChildren_.assign(n, {});
    postOrderNum_.assign(n, kInvalid);
    postorder_.clear();
    rpo_.clear();

    computeRPO();
    computeIDom();

    // 构建 domChildren_
    for (size_t i = 0; i < n; ++i) {
        uint32_t d = idom_[i];
        if (d != kInvalid && d != i) {
            domChildren_[d].push_back(static_cast<uint32_t>(i));
        }
    }
}

void DominatorTree::computeRPO() {
    // DFS 后序遍历（仅访问从 entry 可达的节点）
    std::vector<bool> visited(cfg_.size(), false);
    std::vector<std::pair<uint32_t, size_t>> stack; // (node, next succ idx)

    uint32_t entry = cfg_.entryId();
    visited[entry] = true;
    stack.push_back({entry, 0});

    while (!stack.empty()) {
        auto& [node, idx] = stack.back();
        const auto& succs = cfg_.node(node).succs;
        if (idx < succs.size()) {
            uint32_t s = succs[idx++];
            if (!visited[s]) {
                visited[s] = true;
                stack.push_back({s, 0});
            }
        } else {
            postorder_.push_back(node);
            stack.pop_back();
        }
    }

    // RPO = reverse postorder
    rpo_.assign(postorder_.rbegin(), postorder_.rend());
}

void DominatorTree::computeIDom() {
    if (postorder_.empty())
        return;

    // postorder 编号：用于 intersect 比较（已由 computeRPO 填充到 postOrderNum_）
    for (size_t i = 0; i < postorder_.size(); ++i) {
        postOrderNum_[postorder_[i]] = static_cast<uint32_t>(i);
    }

    uint32_t entry = cfg_.entryId();
    idom_[entry] = entry;

    bool changed = true;
    while (changed) {
        changed = false;
        // 按 RPO 遍历（跳过 entry）
        for (uint32_t b : rpo_) {
            if (b == entry)
                continue;

            uint32_t newIdom = kInvalid;
            for (uint32_t p : cfg_.node(b).preds) {
                if (idom_[p] == kInvalid)
                    continue;
                if (newIdom == kInvalid) {
                    newIdom = p;
                } else {
                    newIdom = intersect(p, newIdom);
                }
            }

            if (newIdom != kInvalid && idom_[b] != newIdom) {
                idom_[b] = newIdom;
                changed = true;
            }
        }
    }
}

uint32_t DominatorTree::intersect(uint32_t b1, uint32_t b2) const {
    // CHK 算法核心：沿 postorder 更深者上溯直到相遇
    while (b1 != b2) {
        while (postOrderNum_[b1] < postOrderNum_[b2]) {
            b1 = idom_[b1];
        }
        while (postOrderNum_[b2] < postOrderNum_[b1]) {
            b2 = idom_[b2];
        }
    }
    return b1;
}

bool DominatorTree::dominates(uint32_t a, uint32_t b) const {
    // a 支配 b 当且仅当从 b 沿 idom 链上溯能到达 a
    if (a >= idom_.size() || b >= idom_.size())
        return false;
    if (idom_[b] == kInvalid)
        return a == b;
    uint32_t cur = b;
    while (true) {
        if (cur == a)
            return true;
        if (cur == idom_[cur])
            return false; // 到达 entry 的自环
        cur = idom_[cur];
        if (cur == kInvalid)
            return false;
    }
}

uint32_t DominatorTree::nearestCommonDominator(uint32_t a, uint32_t b) const {
    if (a >= idom_.size() || b >= idom_.size())
        return cfg_.entryId();
    if (idom_[a] == kInvalid || idom_[b] == kInvalid)
        return cfg_.entryId();

    // 简化实现：用路径集合法
    std::unordered_set<uint32_t> pathA;
    uint32_t cur = a;
    while (true) {
        pathA.insert(cur);
        if (cur == idom_[cur])
            break;
        cur = idom_[cur];
        if (cur == kInvalid)
            break;
    }
    cur = b;
    while (pathA.find(cur) == pathA.end()) {
        if (cur == idom_[cur])
            return cfg_.entryId();
        cur = idom_[cur];
        if (cur == kInvalid)
            return cfg_.entryId();
    }
    return cur;
}

// ============================================================
// DominanceFrontier 实现
// ============================================================

DominanceFrontier::DominanceFrontier(const IRCFG& cfg, const DominatorTree& domTree) {
    df_.assign(cfg.size(), {});

    for (size_t b = 0; b < cfg.size(); ++b) {
        const auto& node = cfg.node(static_cast<uint32_t>(b));
        if (node.preds.size() >= 2) {
            uint32_t idomB = domTree.idom(static_cast<uint32_t>(b));
            for (uint32_t p : node.preds) {
                uint32_t runner = p;
                // runner 上溯到 idom[b]，沿途加入 b 到 DF
                while (runner != DominatorTree::kInvalid && runner != idomB) {
                    df_[runner].push_back(static_cast<uint32_t>(b));
                    uint32_t next = domTree.idom(runner);
                    if (next == runner || next == DominatorTree::kInvalid)
                        break; // 到达 entry 自环或不可达
                    runner = next;
                }
            }
        }
    }

    // 去重
    for (auto& list : df_) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
}

// ============================================================
// NaturalLoopInfo 实现
// ============================================================

NaturalLoopInfo::NaturalLoopInfo(const IRCFG& cfg, const DominatorTree& domTree) : cfg_(cfg) {
    detectBackEdges(domTree);

    // 对每条回边收集自然循环体
    for (const auto& edge : backEdges_) {
        Loop loop;
        loop.header = edge.second;
        loop.backEdges.push_back(edge.first);
        collectLoopBody(edge.second, edge.first);
        // collectLoopBody 填充 loopBody_，转移到 loop
        // P2-10 fix: 原代码误用 loop.body.end() 作为结束迭代器（loop.body 此时为空，
        // 导致赋值空范围，loop.body 恒仅含 header）。修正为 loopBody_.end()。
        loop.body.assign(loopBody_.begin(), loopBody_.end());
        std::sort(loop.body.begin(), loop.body.end());

        size_t loopIdx = loops_.size();
        loops_.push_back(std::move(loop));

        // 更新 loopsOf_ 和 inLoop_
        for (uint32_t n : loops_.back().body) {
            loopsOf_[n].push_back(loopIdx);
            inLoop_.insert(n);
        }
    }
}

void NaturalLoopInfo::detectBackEdges(const DominatorTree& domTree) {
    backEdges_.clear();
    for (size_t n = 0; n < cfg_.size(); ++n) {
        const auto& node = cfg_.node(static_cast<uint32_t>(n));
        for (uint32_t s : node.succs) {
            // 边 n→s 是回边当且仅当 s 支配 n
            if (domTree.dominates(s, static_cast<uint32_t>(n))) {
                backEdges_.push_back({static_cast<uint32_t>(n), s});
            }
        }
    }
}

void NaturalLoopInfo::collectLoopBody(uint32_t header, uint32_t tail) {
    loopBody_.clear();
    loopBody_.insert(header);
    if (tail == header)
        return;

    loopBody_.insert(tail);
    std::stack<uint32_t> stack;
    stack.push(tail);
    while (!stack.empty()) {
        uint32_t n = stack.top();
        stack.pop();
        for (uint32_t p : cfg_.node(n).preds) {
            if (p != header && loopBody_.find(p) == loopBody_.end()) {
                loopBody_.insert(p);
                stack.push(p);
            }
        }
    }
}

// ============================================================
// SSA 构造与析构
// ============================================================

bool ssaConstructPass(IRFunction& ir) {
    // 仅在含控制流的函数上构造（≥2 个严格块）
    bool hasControlFlow = false;
    for (const auto& blk : ir.blocks) {
        for (const auto& instr : blk.instructions) {
            if (instr.op == IROp::JUMP_IF_FALSE || instr.op == IROp::JUMP) {
                hasControlFlow = true;
                break;
            }
        }
        if (hasControlFlow)
            break;
    }
    if (!hasControlFlow)
        return false;

    IRCFG cfg(ir);
    if (cfg.size() < 2)
        return false;

    DominatorTree domTree(cfg);
    DominanceFrontier df(cfg, domTree);

    // 收集每个 local slot 的定义块（含 STORE_LOCAL 的 CFG 节点）
    std::map<uint32_t, std::set<uint32_t>> slotDefBlocks; // slot → block ids
    for (size_t ni = 0; ni < cfg.size(); ++ni) {
        const auto& node = cfg.node(static_cast<uint32_t>(ni));
        const auto& blk = ir.blocks[node.sourceBlockIdx];
        for (size_t i = node.startInstr; i < node.endInstr; ++i) {
            if (blk.instructions[i].op == IROp::STORE_LOCAL) {
                if (!blk.instructions[i].operands.empty()) {
                    uint32_t slot = blk.instructions[i].operands[0].index;
                    slotDefBlocks[slot].insert(static_cast<uint32_t>(ni));
                }
            }
        }
    }

    if (slotDefBlocks.empty())
        return false;

    // 阶段 1：迭代支配边界 PHI 放置（Cytron 算法）
    // phiPlacement[blockId] = {slots needing PHI in this block}
    std::map<uint32_t, std::set<uint32_t>> phiPlacement;
    bool placedAny = false;

    for (auto& [slot, defBlocks] : slotDefBlocks) {
        std::set<uint32_t> worklist = defBlocks;
        std::set<uint32_t> hasPhi;
        while (!worklist.empty()) {
            uint32_t b = *worklist.begin();
            worklist.erase(worklist.begin());
            for (uint32_t d : df.df(b)) {
                if (hasPhi.find(d) == hasPhi.end()) {
                    hasPhi.insert(d);
                    phiPlacement[d].insert(slot);
                    placedAny = true;
                    // 若 d 不在 defBlocks，加入 worklist
                    if (defBlocks.find(d) == defBlocks.end()) {
                        worklist.insert(d);
                    }
                }
            }
        }
    }

    if (!placedAny)
        return false;

    // 阶段 2：插入 PHI 指令到 IRFunction
    // PHI 格式: [dest_vreg, slot(LOCAL_SLOT), pred1_nodeid(IMM_UINT), vreg1(VIRTUAL), ...]
    // 初始 pred/vreg 对为空，重命名阶段填充
    // P2-10 fix3: pred 标识符从 labelIndex（LABEL）改为 CFG node ID（IMM_UINT），
    // 避免 fall-through 块 labelIndex 不稳定导致的 PHI 前驱歧义
    // phiInstrMap[(blockId, slot)] = (blockIdx, localInstrIdx, destVReg)
    struct PhiEntry {
        uint32_t blockId;
        uint32_t slot;
        size_t blockIdx;
        size_t localIdx; // 插入位置
        uint32_t destVReg;
    };
    std::vector<PhiEntry> phiEntries;

    // P2-10 fix4: PHI 插入顺序——按 (sourceBlockIdx, startInstr) 降序插入。
    // 原实现按 blockId 升序（= startInstr 升序）遍历 phiPlacement，先在低位置插入
    // PHI 后在高位置插入，但低位置 insert 会使高位置的 startInstr 偏移，导致后续
    // PHI 插入到错误位置（如 L5 的 PHI 被插入到前驱块 node 2 中部而非 L5 块首）。
    // 降序插入：高位置先插入不影响低位置的 startInstr，确保每个 PHI 都插入到
    // 正确的块首（LABEL 之后）。同块内多个 PHI 仍按 slot 升序 + offset 递增插入。
    std::vector<std::pair<uint32_t, std::set<uint32_t>>> sortedPhiPlacement(phiPlacement.begin(), phiPlacement.end());
    std::sort(sortedPhiPlacement.begin(), sortedPhiPlacement.end(),
              [&](const auto& a, const auto& b) {
                  const auto& nodeA = cfg.node(a.first);
                  const auto& nodeB = cfg.node(b.first);
                  if (nodeA.sourceBlockIdx != nodeB.sourceBlockIdx)
                      return nodeA.sourceBlockIdx > nodeB.sourceBlockIdx; // 降序
                  return nodeA.startInstr > nodeB.startInstr;             // 降序
              });

    for (auto& [blockId, slots] : sortedPhiPlacement) {
        const auto& node = cfg.node(blockId);
        size_t bi = node.sourceBlockIdx;
        auto& blk = ir.blocks[bi];
        // 插入位置：LABEL 之后（若有），否则块起始
        size_t insertPos = node.startInstr;
        if (node.hasLabel) {
            insertPos = node.startInstr + 1; // 跳过 LABEL
        }

        size_t offset = 0;
        for (uint32_t slot : slots) {
            uint32_t destVReg = ir.nextVReg++;
            std::vector<IROperand> ops;
            ops.push_back(IROperand::vreg(destVReg));
            ops.push_back(IROperand::local(slot));
            // pred/vreg 对在重命名阶段填充

            IRInstruction phiInstr(IROp::PHI, std::move(ops), 0);
            blk.instructions.insert(blk.instructions.begin() + insertPos + offset, std::move(phiInstr));

            phiEntries.push_back({blockId, slot, bi, insertPos + offset, destVReg});
            ++offset;
        }
    }

    // P2-10 fix: PHI 插入改变了 IR 指令序列的索引，原 CFG 的 startInstr/endInstr
    // 已失效。重命名阶段遍历指令依赖准确的索引，必须重建 CFG。
    // 不变量：PHI 插入不改变控制流（LABEL/JUMP/RETURN 位置关系不变），重建后的
    // CFG 拓扑与原 CFG 一致，仅指令索引偏移。phiPlacement/phiDestMap 基于原
    // blockId，重建后 blockId 不变（节点数和顺序不变）。
    cfg = IRCFG(ir);
    // 重建支配树（DominatorTree 持 cfg_ 引用，cfg 被重新赋值后需重建以更新引用）
    // P2-10 fix: 原 std::swap(domTree, newDomTree) 因 DominatorTree 含引用成员
    // 不可交换而编译失败；改为直接用 newDomTree 替代后续 domTree 引用。
    DominatorTree newDomTree(cfg);

    // 阶段 3：重命名（DFS 支配树）
    // 构建 (blockId, slot) → destVReg 映射
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> phiDestMap;
    for (const auto& pe : phiEntries) {
        phiDestMap[{pe.blockId, pe.slot}] = pe.destVReg;
    }

    // slotStacks[slot] = 当前 SSA 值栈
    std::unordered_map<uint32_t, std::vector<uint32_t>> slotStacks;
    // vregRepl[vreg] = 替代 vreg（LOAD_LOCAL dest → 当前 slot 值）
    std::unordered_map<uint32_t, uint32_t> vregRepl;

    // 函数参数初始化：slot 0..arity-1 分配初始 vreg
    for (int i = 0; i < ir.arity; ++i) {
        uint32_t v = ir.nextVReg++;
        slotStacks[static_cast<uint32_t>(i)].push_back(v);
    }

    // 递归重命名
    std::function<void(uint32_t)> rename = [&](uint32_t blockId) {
        const auto& node = cfg.node(blockId);
        auto& blk = ir.blocks[node.sourceBlockIdx];

        // 记录本块压栈的 (slot) 列表，退出时弹栈
        std::vector<uint32_t> pushedSlots;

        // 处理本块的 PHI（在指令范围起始）
        auto it = phiPlacement.find(blockId);
        if (it != phiPlacement.end()) {
            for (uint32_t slot : it->second) {
                uint32_t destVReg = phiDestMap[{blockId, slot}];
                slotStacks[slot].push_back(destVReg);
                pushedSlots.push_back(slot);
            }
        }

        // 处理常规指令
        for (size_t i = node.startInstr; i < node.endInstr; ++i) {
            auto& instr = blk.instructions[i];

            // 跳过 PHI 指令
            if (instr.op == IROp::PHI)
                continue;

            // 先应用 vregRepl 到 VIRTUAL 操作数
            for (auto& op : instr.operands) {
                if (op.kind == IROperandKind::VIRTUAL) {
                    auto rit = vregRepl.find(op.index);
                    if (rit != vregRepl.end()) {
                        op.index = rit->second;
                    }
                }
            }

            if (instr.op == IROp::STORE_LOCAL && instr.operands.size() >= 2) {
                uint32_t slot = instr.operands[0].index;
                // Bug #86 fix: 上方循环已对所有 VIRTUAL 操作数（含 operands[1]）
                // 统一应用 vregRepl，此处不再重复查找，避免双重替换语义错误。
                uint32_t srcVReg = instr.operands[1].index;
                slotStacks[slot].push_back(srcVReg);
                pushedSlots.push_back(slot);
            } else if (instr.op == IROp::LOAD_LOCAL && instr.operands.size() >= 2) {
                uint32_t destVReg = instr.operands[0].index;
                uint32_t slot = instr.operands[1].index;
                auto& stack = slotStacks[slot];
                if (!stack.empty()) {
                    vregRepl[destVReg] = stack.back();
                }
            }
        }

        // 填充后继块的 PHI 操作数
        // P2-10 fix3: 使用 CFG node ID（IMM_UINT）作为前驱标识符，而非 labelIndex。
        // 原方案用 labelIndex（LABEL）标识前驱，但 fall-through 块无 LABEL 时继承
        // 所在 IRBasicBlock 的 labelIndex，导致同一 IRBasicBlock 内多个 CFG 节点共享
        // labelIndex，PHI 前驱标识歧义。ssaDestructPass 据此 node ID 通过 CFG 精确
        // 定位前驱块的终结指令位置（仅在 [startInstr, endInstr) 范围搜索，不跨越块边界）。
        for (uint32_t succId : node.succs) {
            auto succIt = phiPlacement.find(succId);
            if (succIt == phiPlacement.end())
                continue;
            uint32_t predNodeId = node.id;

            const auto& succNode = cfg.node(succId);
            auto& succBlk = ir.blocks[succNode.sourceBlockIdx];
            for (uint32_t slot : succIt->second) {
                // 找到 succId 块中此 slot 的 PHI 指令
                uint32_t phiDest = phiDestMap[{succId, slot}];
                // 在 succBlk 中查找 PHI
                for (auto& instr : succBlk.instructions) {
                    if (instr.op == IROp::PHI && !instr.operands.empty() && instr.operands[0].index == phiDest) {
                        // 追加 (predNodeId, vreg) 对——predNodeId 以 IMM_UINT 存储
                        uint32_t vreg = 0;
                        auto& stack = slotStacks[slot];
                        if (!stack.empty()) {
                            vreg = stack.back();
                        }
                        instr.operands.push_back(IROperand::imm(predNodeId));
                        instr.operands.push_back(IROperand::vreg(vreg));
                        break;
                    }
                }
            }
        }

        // 递归到支配树子节点（P2-10 fix: 用 newDomTree 而非已失效的 domTree）
        for (uint32_t child : newDomTree.children(blockId)) {
            rename(child);
        }

        // 弹栈
        for (auto popIt = pushedSlots.rbegin(); popIt != pushedSlots.rend(); ++popIt) {
            slotStacks[*popIt].pop_back();
        }
    };

    rename(cfg.entryId());

    LOG_DEBUG("ssaConstructPass: 插入 " + std::to_string(phiEntries.size()) + " 个 PHI 节点", "IR-SSA");
    return true;
}

bool ssaDestructPass(IRFunction& ir) {
    // 收集所有 PHI 指令，拆解为前驱块尾部的 STORE_LOCAL + 替换为 LOAD_LOCAL
    bool hasPhi = false;
    for (const auto& blk : ir.blocks) {
        for (const auto& instr : blk.instructions) {
            if (instr.op == IROp::PHI) {
                hasPhi = true;
                break;
            }
        }
        if (hasPhi)
            break;
    }
    if (!hasPhi)
        return false;

    // P2-10 fix3: 构建 CFG，使用 node ID 精确定位前驱块的终结指令位置。
    // 原 labelPos 方案在 fall-through 块中失效：fall-through 块无终结指令，
    // 从 LABEL 位置向前搜索终结指令会跨越 CFG 节点边界，找到下一个 CFG 节点
    // （如循环条件块）的 JUMP_IF_FALSE，导致 STORE_LOCAL 插入到错误位置
    // （如 PHI 所在块内部而非前驱块尾部），破坏循环变量更新语义。
    //
    // 新方案：ssaConstructPass 以 IMM_UINT 存储 CFG node ID 作为前驱标识符，
    // ssaDestructPass 通过 cfg.node(predNodeId) 取得前驱节点的 sourceBlockIdx
    // 与 [startInstr, endInstr) 指令范围，仅在此范围内搜索终结指令，
    // 确保 STORE_LOCAL 插入在前驱块的正确位置（终结指令之前或 fall-through 块末尾）。
    IRCFG cfg(ir);

    // BUGFIX-P1 (#25): 两阶段处理，避免使用过期 CFG 索引。
    // 原实现在处理每个 PHI 块时边收集边向前驱块插入 STORE_LOCAL，改变了指令
    // 数量，但函数入口构建的 CFG 的 startInstr/endInstr 未更新；后续 PHI 若引用
    // 已被修改的前驱块，会使用过期索引定位终结指令，导致 STORE_LOCAL 插入到
    // 错误位置。修复：
    //   阶段 1：在任何修改发生前，遍历所有 PHI，使用新鲜 CFG 收集待插入的
    //           STORE_LOCAL 信息（目标块索引、终结指令位置、slot、源 vreg）。
    //   阶段 2：按 (blockIdx, position) 降序插入，靠后的插入不会使靠前位置失效。
    bool modified = false;

    // 阶段 1：收集所有待插入的 STORE_LOCAL（此时 CFG 仍与当前 IR 一致）
    struct PendingStore {
        size_t blockIdx;
        size_t position;
        std::vector<std::pair<uint32_t, uint32_t>> stores; // (slot, vreg)
    };
    std::map<std::pair<size_t, size_t>, std::vector<std::pair<uint32_t, uint32_t>>> predStores;

    for (const auto& blk : ir.blocks) {
        for (const auto& instr : blk.instructions) {
            if (instr.op != IROp::PHI)
                continue;
            if (instr.operands.size() < 2)
                continue;
            uint32_t slot = instr.operands[1].index;

            // 遍历 (pred_node_id, vreg) 对——pred_node_id 以 IMM_UINT 存储
            for (size_t k = 2; k + 1 < instr.operands.size(); k += 2) {
                uint32_t predNodeId = instr.operands[k].index;
                uint32_t vreg = instr.operands[k + 1].index;

                if (predNodeId >= cfg.size())
                    continue;

                const auto& predNode = cfg.node(predNodeId);
                size_t predBlockIdx = predNode.sourceBlockIdx;
                const auto& predBlk = ir.blocks[predBlockIdx];

                // 仅在 predNode 的指令范围 [startInstr, endInstr) 内搜索终结指令。
                // fall-through 块（无终结指令）的 insertBefore = endInstr（节点末尾）。
                size_t insertBefore = predNode.endInstr;
                for (size_t i = predNode.startInstr; i < predNode.endInstr; ++i) {
                    if (isTerminatorOp(predBlk.instructions[i].op)) {
                        insertBefore = i;
                        break;
                    }
                }
                predStores[{predBlockIdx, insertBefore}].push_back({slot, vreg});
            }
        }
    }

    // 阶段 2：按 (blockIdx, position) 降序插入 STORE_LOCAL。
    // 降序保证靠后/靠高位置的插入不会使靠前/靠低位置的索引失效。
    {
        std::vector<PendingStore> sortedStores;
        sortedStores.reserve(predStores.size());
        for (auto& [key, stores] : predStores) {
            sortedStores.push_back(PendingStore{key.first, key.second, std::move(stores)});
        }
        std::sort(sortedStores.begin(), sortedStores.end(), [](const PendingStore& a, const PendingStore& b) {
            if (a.blockIdx != b.blockIdx)
                return a.blockIdx > b.blockIdx; // blockIdx 降序（不同块互不影响）
            return a.position > b.position;     // position 降序
        });
        for (auto& ps : sortedStores) {
            auto& targetBlock = ir.blocks[ps.blockIdx];
            size_t offset = 0;
            for (auto& [slot, vreg] : ps.stores) {
                IRInstruction storeInstr(IROp::STORE_LOCAL, {IROperand::local(slot), IROperand::vreg(vreg)}, 0);
                targetBlock.instructions.insert(targetBlock.instructions.begin() + ps.position + offset,
                                                std::move(storeInstr));
                ++offset;
            }
        }
    }

    // 阶段 3：将所有 PHI 替换为 LOAD_LOCAL（不改变指令数量，不影响已插入位置）
    for (auto& blk : ir.blocks) {
        std::vector<IRInstruction> newInstrs;
        newInstrs.reserve(blk.instructions.size());
        for (const auto& instr : blk.instructions) {
            if (instr.op == IROp::PHI) {
                if (instr.operands.size() >= 2) {
                    uint32_t destVReg = instr.operands[0].index;
                    uint32_t slot = instr.operands[1].index;
                    newInstrs.emplace_back(IROp::LOAD_LOCAL,
                                           std::vector<IROperand>{IROperand::vreg(destVReg), IROperand::local(slot)},
                                           instr.line);
                    modified = true;
                }
            } else {
                newInstrs.push_back(instr);
            }
        }
        blk.instructions = std::move(newInstrs);
    }

    LOG_DEBUG("ssaDestructPass: PHI 消除完成", "IR-SSA");
    return modified;
}

// ============================================================
// GVN（全局值编号）实现
// ============================================================

bool gvnPass(IRFunction& ir) {
    IRCFG cfg(ir);
    if (cfg.size() < 2)
        return false; // 单块用 CSE 即可

    DominatorTree domTree(cfg);

    bool modified = false;

    // 支配树 DFS，维护值编号表（继承自父块）
    // valueTable: (op, operands) → dest vreg
    // replacements: 被替换的 dest vreg → 替代 vreg（沿支配树向下传播）
    std::function<void(uint32_t, std::unordered_map<std::string, uint32_t>&,
                       const std::unordered_map<uint32_t, uint32_t>&)>
        dfs = [&](uint32_t blockId, std::unordered_map<std::string, uint32_t>& parentTable,
                  const std::unordered_map<uint32_t, uint32_t>& parentReplacements) {
            const auto& node = cfg.node(blockId);
            auto& blk = ir.blocks[node.sourceBlockIdx]; // 非 const：GVN 需修改操作数 vreg 引用

            // 拷贝父表（scoped）
            std::unordered_map<std::string, uint32_t> table = parentTable;
            // 继承父块替换映射并添加本块新替换
            std::unordered_map<uint32_t, uint32_t> replacements = parentReplacements;

            for (size_t i = node.startInstr; i < node.endInstr; ++i) {
                auto& instr = blk.instructions[i];

                // PHI 指令：完全跳过，不应用替换到操作数，不加入值表。
                // PHI 的 (pred_node_id, vreg) 对有特定语义：vreg 引用前驱块末尾的具体
                // SSA 值。即使两个 vreg 计算相同值（如两个 LOAD_CONST 0），也不能在
                // PHI 中替换——ssaDestructPass 据此 vreg 在前驱块尾部插入 STORE_LOCAL，
                // 替换会破坏前驱块 SSA 值与 PHI 操作数的引用关系，导致循环变量初始化
                // 错误（如 NestedLoopsPassIsolationWithLocals 测试中 i/j 被 GVN 合并到
                // v0 后，destruct 插入的 STORE_LOCAL 引用错误 vreg）。
                if (instr.op == IROp::PHI)
                    continue;

                // 应用替换到 VIRTUAL 操作数
                for (auto& op : instr.operands) {
                    if (op.kind == IROperandKind::VIRTUAL) {
                        auto rit = replacements.find(op.index);
                        if (rit != replacements.end()) {
                            op.index = rit->second;
                            modified = true;
                        }
                    }
                }

                // 跳过非纯计算
                if (!isPureComputeForSSA(instr.op))
                    continue;
                if (!hasDestVReg(instr))
                    continue;

                uint32_t destVReg = getDestVReg(instr);
                std::string key = encodeValueKey(instr);
                auto it = table.find(key);
                if (it != table.end()) {
                    // 相同表达式已存在：替换 dest 引用
                    replacements[destVReg] = it->second;
                } else {
                    table[key] = destVReg;
                }
            }

            // 递归到支配树子节点（传播替换映射）
            for (uint32_t child : domTree.children(blockId)) {
                dfs(child, table, replacements);
            }
        };

    std::unordered_map<std::string, uint32_t> emptyTable;
    std::unordered_map<uint32_t, uint32_t> emptyReplacements;
    dfs(cfg.entryId(), emptyTable, emptyReplacements);

    if (modified) {
        LOG_DEBUG("gvnPass: 全局值编号完成", "IR-SSA");
    }
    return modified;
}

// ============================================================
// LICM（循环不变代码外提）实现
// ============================================================

bool licmPass(IRFunction& ir) {
    // P2-10 fix: 嵌套循环索引失效保护。原实现在入口处一次性构建 CFG/loopInfo，
    // 处理一个循环并修改 IR（删除+插入）后，后续循环的 CFG 节点范围
    // (startInstr/endInstr) 已偏移，继续处理会删错指令。
    // 正确做法：每次修改后重建 CFG 重跑，直到无更多可外提指令。
    bool modified = false;

    while (true) {
        IRCFG cfg(ir);
        if (cfg.size() < 2)
            break;

        DominatorTree domTree(cfg);
        NaturalLoopInfo loopInfo(cfg, domTree);

        if (loopInfo.loops().empty())
            break;

        bool changedThisIter = false;

        for (const auto& loop : loopInfo.loops()) {
            uint32_t header = loop.header;

            // 寻找 preheader：header 的前驱中不在循环体内的节点
            uint32_t preheader = IRCFG::kInvalid;
            int preheaderCount = 0;
            for (uint32_t p : cfg.node(header).preds) {
                bool inLoop = false;
                for (uint32_t b : loop.body) {
                    if (b == p) {
                        inLoop = true;
                        break;
                    }
                }
                if (!inLoop) {
                    preheader = p;
                    ++preheaderCount;
                }
            }

            if (preheaderCount != 1 || preheader == IRCFG::kInvalid)
                continue;

            // 收集循环体内定义的所有 vreg
            std::unordered_set<uint32_t> loopDefinedVRegs;
            for (uint32_t b : loop.body) {
                const auto& node = cfg.node(b);
                const auto& blk = ir.blocks[node.sourceBlockIdx];
                for (size_t i = node.startInstr; i < node.endInstr; ++i) {
                    const auto& instr = blk.instructions[i];
                    if (hasDestVReg(instr)) {
                        loopDefinedVRegs.insert(getDestVReg(instr));
                    }
                }
            }

            // 寻找可外提的指令（纯计算 + 所有 vreg 操作数在循环外定义 + 不涉及 local slot）
            struct HoistCandidate {
                uint32_t blockId;
                size_t blockIdx;
                size_t localIdx;
                IRInstruction instr;
            };
            std::vector<HoistCandidate> candidates;

            for (uint32_t b : loop.body) {
                if (b == header)
                    continue;
                const auto& node = cfg.node(b);
                const auto& blk = ir.blocks[node.sourceBlockIdx];
                for (size_t i = node.startInstr; i < node.endInstr; ++i) {
                    const auto& instr = blk.instructions[i];
                    if (!isPureComputeForSSA(instr.op))
                        continue;
                    if (!hasDestVReg(instr))
                        continue;

                    bool invariant = true;
                    for (size_t k = 1; k < instr.operands.size(); ++k) {
                        if (instr.operands[k].kind == IROperandKind::VIRTUAL) {
                            if (loopDefinedVRegs.count(instr.operands[k].index)) {
                                invariant = false;
                                break;
                            }
                        } else if (instr.operands[k].kind == IROperandKind::LOCAL_SLOT) {
                            invariant = false;
                            break;
                        }
                    }
                    if (invariant) {
                        candidates.push_back({b, node.sourceBlockIdx, i, instr});
                    }
                }
            }

            if (candidates.empty())
                continue;

            // 步骤 1：从原块删除外提指令（按 blockIdx/localIdx 降序）
            std::sort(candidates.begin(), candidates.end(), [](const HoistCandidate& a, const HoistCandidate& b) {
                if (a.blockIdx != b.blockIdx)
                    return a.blockIdx > b.blockIdx;
                return a.localIdx > b.localIdx;
            });
            for (const auto& cand : candidates) {
                ir.blocks[cand.blockIdx].instructions.erase(ir.blocks[cand.blockIdx].instructions.begin() +
                                                            cand.localIdx);
            }

            // 步骤 2：在 preheader 的终结指令前插入外提指令
            const auto& phNode = cfg.node(preheader);
            auto& phBlk = ir.blocks[phNode.sourceBlockIdx];
            size_t insertBefore = phNode.endInstr;
            if (insertBefore > phBlk.instructions.size())
                insertBefore = phBlk.instructions.size();
            for (size_t i = phNode.startInstr; i < insertBefore; ++i) {
                if (isTerminatorOp(phBlk.instructions[i].op)) {
                    insertBefore = i;
                    break;
                }
            }
            size_t offset = 0;
            for (const auto& cand : candidates) {
                phBlk.instructions.insert(phBlk.instructions.begin() + insertBefore + offset, cand.instr);
                ++offset;
            }

            modified = true;
            changedThisIter = true;
            break; // 重建 CFG 后重跑
        }

        if (!changedThisIter)
            break;
    }

    if (modified) {
        LOG_DEBUG("licmPass: 循环不变代码外提完成", "IR-SSA");
    }
    return modified;
}

// ============================================================
// 函数内联实现
// ============================================================

bool inlinePass(IRModule& module) {
    bool modified = false;
    constexpr size_t kInlineThreshold = 15;  // 指令数阈值
    constexpr int kMaxTotalInlineOps = 1000; // 总指令预算（防代码膨胀）

    int totalInlinedOps = 0;

    // 收集所有可内联的函数
    auto isInlineable = [&](const IRFunction& fn) -> bool {
        if (fn.isGenerator)
            return false;
        if (fn.blocks.size() != 1)
            return false; // 仅内联单块函数
        const auto& instrs = fn.blocks[0].instructions;
        size_t count = 0;
        for (const auto& instr : instrs) {
            // 跳过 LABEL
            if (instr.op == IROp::LABEL)
                continue;
            // 不内联含控制流的函数（LABEL 除外）
            if (instr.op == IROp::JUMP || instr.op == IROp::JUMP_IF_FALSE)
                return false;
            // 不内联含 try/throw 的函数
            if (instr.op == IROp::TRY_BEGIN || instr.op == IROp::TRY_END || instr.op == IROp::THROW)
                return false;
            // 不内联含 yield 的函数（生成器）
            if (instr.op == IROp::YIELD)
                return false;
            ++count;
        }
        // BUGFIX-P2 #58: 若 callee 含 STORE_LOCAL 写入参数槽位（slot < arity），
        // 说明参数被重新赋值。内联后参数映射（LOAD_LOCAL → 实参 vreg）无法正确
        // 处理后续对同一参数的读取（会读到 caller 的槽位而非更新后的值），拒绝内联。
        // STORE_LOCAL operands: [slot, src_vreg]，slot 在 operands[0]。
        for (const auto& instr2 : instrs) {
            if (instr2.op == IROp::STORE_LOCAL &&
                !instr2.operands.empty() &&
                instr2.operands[0].kind == IROperandKind::LOCAL_SLOT &&
                instr2.operands[0].index < static_cast<uint32_t>(fn.arity)) {
                return false; // 参数被重新赋值，不可内联
            }
        }
        return count <= kInlineThreshold;
    };

    // 遍历所有函数（main + 子函数），寻找可内联的 CALL 指令
    std::vector<IRFunction*> allFns;
    if (module.mainFunction)
        allFns.push_back(module.mainFunction.get());
    for (auto& fn : module.functions) {
        if (fn)
            allFns.push_back(fn.get());
    }

    for (auto* caller : allFns) {
        if (!caller || caller->blocks.empty())
            continue;

        auto& instrs = caller->blocks[0].instructions;
        std::vector<IRInstruction> newInstrs;
        newInstrs.reserve(instrs.size());

        for (size_t i = 0; i < instrs.size(); ++i) {
            const auto& instr = instrs[i];

            // 查找 CALL 指令: dest = call name(args)
            // operands: [dest, name_idx, arg_count, arg1, arg2, ...]
            if (instr.op == IROp::CALL && instr.operands.size() >= 3) {
                uint32_t nameIdx = instr.operands[1].index;
                if (nameIdx < caller->globalNames.size()) {
                    const std::string& calleeName = caller->globalNames[nameIdx];
                    IRFunction* callee = module.findFunction(calleeName);

                    // 防递归内联
                    if (callee && callee != caller && isInlineable(*callee) && totalInlinedOps < kMaxTotalInlineOps) {
                        // 内联：复制 callee 指令，重映射 vreg
                        uint32_t vregBase = caller->nextVReg;
                        uint32_t calleeVRegCount = callee->nextVReg;
                        caller->nextVReg += calleeVRegCount;

                        auto remapVReg = [&](uint32_t v) -> uint32_t { return vregBase + v; };

                        // P2-10 fix: 常量池/名称池重映射——callee 的 CONSTANT/FUNC_NAME/
                        // GLOBAL_NAME/FIELD_NAME 操作数索引指向 callee 自己的 constants/
                        // globalNames 数组，内联到 caller 后必须重映射到 caller 的对应数组，
                        // 否则 LOAD_CONST 读到错误常量、CALL 调用错误函数名。
                        std::unordered_map<uint32_t, uint32_t> constRemap;
                        std::unordered_map<uint32_t, uint32_t> nameRemap;
                        auto remapConst = [&](uint32_t idx) -> uint32_t {
                            auto it = constRemap.find(idx);
                            if (it != constRemap.end())
                                return it->second;
                            if (idx < callee->constants.size()) {
                                uint32_t newIdx = caller->addConstant(callee->constants[idx]);
                                constRemap[idx] = newIdx;
                                return newIdx;
                            }
                            return idx;
                        };
                        auto remapName = [&](uint32_t idx) -> uint32_t {
                            auto it = nameRemap.find(idx);
                            if (it != nameRemap.end())
                                return it->second;
                            if (idx < callee->globalNames.size()) {
                                uint32_t newIdx = caller->addGlobal(callee->globalNames[idx]);
                                nameRemap[idx] = newIdx;
                                return newIdx;
                            }
                            return idx;
                        };

                        // P2-10 fix: 参数映射——callee 的 LOAD_LOCAL slot_p (p < arity) 读取的是
                        // callee 自己的参数槽位，但内联到 caller 后这些槽位属于 caller 的局部变量，
                        // 语义错误（会读到无关变量）。正确做法：将 callee 参数的 LOAD_LOCAL 替换为
                        // 对 CALL 指令中对应实参 vreg 的引用（DUP 语义）。
                        // CALL operands: [dest, name_idx, arg_count, arg1, arg2, ...]
                        // arg_{p+1} 对应 callee 的 slot_p（参数 p）。
                        std::unordered_map<uint32_t, uint32_t> paramSlotToArgVReg;
                        if (instr.operands.size() >= 3 + static_cast<size_t>(callee->arity)) {
                            for (int p = 0; p < callee->arity; ++p) {
                                uint32_t argVReg = instr.operands[3 + p].index;
                                paramSlotToArgVReg[static_cast<uint32_t>(p)] = argVReg;
                            }
                        }
                        // paramDestRemap: callee 内 LOAD_LOCAL slot_p 的 dest vreg（已重映射）
                        // → 对应实参 vreg。后续引用此 dest 的指令直接用实参 vreg。
                        // 局限性：若 callee 重新赋值参数（STORE_LOCAL slot_p），后续 LOAD_LOCAL
                        // 仍会读取 caller 槽位（错误）。当前仅支持参数只读的常见场景。
                        std::unordered_map<uint32_t, uint32_t> paramDestRemap;
                        auto resolveVReg = [&](uint32_t v) -> uint32_t {
                            auto rit = paramDestRemap.find(v);
                            return rit != paramDestRemap.end() ? rit->second : v;
                        };

                        size_t calleeOps = 0;
                        for (const auto& cInstr : callee->blocks[0].instructions) {
                            if (cInstr.op == IROp::LABEL)
                                continue;
                            if (cInstr.op == IROp::RETURN_NULL)
                                continue; // 隐式返回，跳过
                            if (cInstr.op == IROp::RETURN) {
                                // return src → 将 src 赋值给 CALL 的 dest
                                if (!cInstr.operands.empty() && cInstr.operands[0].kind == IROperandKind::VIRTUAL) {
                                    uint32_t retVal = resolveVReg(remapVReg(cInstr.operands[0].index));
                                    uint32_t destVReg = instr.operands[0].index;
                                    newInstrs.emplace_back(
                                        IROp::DUP,
                                        std::vector<IROperand>{IROperand::vreg(destVReg), IROperand::vreg(retVal)},
                                        cInstr.line);
                                }
                                continue;
                            }

                            // 参数 LOAD_LOCAL：跳过，记录 dest → 实参 vreg 映射
                            if (cInstr.op == IROp::LOAD_LOCAL && cInstr.operands.size() >= 2 &&
                                cInstr.operands[1].kind == IROperandKind::LOCAL_SLOT) {
                                uint32_t slot = cInstr.operands[1].index;
                                if (slot < static_cast<uint32_t>(callee->arity)) {
                                    auto pit = paramSlotToArgVReg.find(slot);
                                    if (pit != paramSlotToArgVReg.end()) {
                                        uint32_t remappedDest = remapVReg(cInstr.operands[0].index);
                                        paramDestRemap[remappedDest] = pit->second;
                                        continue; // 跳过 LOAD_LOCAL，后续引用改用实参 vreg
                                    }
                                }
                            }

                            // 复制指令，重映射 vreg + 常量池/名称池 + 参数 dest 替换
                            std::vector<IROperand> newOps;
                            newOps.reserve(cInstr.operands.size());
                            for (const auto& op : cInstr.operands) {
                                if (op.kind == IROperandKind::VIRTUAL) {
                                    newOps.push_back(IROperand::vreg(resolveVReg(remapVReg(op.index))));
                                } else if (op.kind == IROperandKind::CONSTANT) {
                                    newOps.push_back(IROperand::constant(remapConst(op.index)));
                                } else if (op.kind == IROperandKind::FUNC_NAME ||
                                           op.kind == IROperandKind::GLOBAL_NAME ||
                                           op.kind == IROperandKind::FIELD_NAME) {
                                    newOps.push_back(IROperand{op.kind, remapName(op.index)});
                                } else {
                                    newOps.push_back(op);
                                }
                            }
                            newInstrs.emplace_back(cInstr.op, std::move(newOps), cInstr.line);
                            ++calleeOps;
                        }
                        totalInlinedOps += static_cast<int>(calleeOps);
                        modified = true;
                        continue; // 跳过原 CALL 指令
                    }
                }
            }

            newInstrs.push_back(instr);
        }

        if (modified) {
            instrs = std::move(newInstrs);
        }
    }

    if (modified) {
        LOG_DEBUG("inlinePass: 函数内联完成", "IR-SSA");
    }
    return modified;
}

// ============================================================
// 调试输出
// ============================================================

std::string cfgToString(const IRCFG& cfg) {
    std::ostringstream oss;
    oss << "CFG (" << cfg.size() << " nodes):\n";
    for (size_t i = 0; i < cfg.size(); ++i) {
        const auto& node = cfg.node(static_cast<uint32_t>(i));
        oss << "  BB" << node.id;
        if (node.hasLabel) {
            oss << " (label=" << node.labelIndex << ")";
        } else {
            oss << " (no label)";
        }
        oss << " [block=" << node.sourceBlockIdx << " instrs=" << node.startInstr << ".." << node.endInstr << "]";
        if (!node.preds.empty()) {
            oss << " preds={";
            for (size_t j = 0; j < node.preds.size(); ++j) {
                if (j > 0)
                    oss << ",";
                oss << node.preds[j];
            }
            oss << "}";
        }
        if (!node.succs.empty()) {
            oss << " succs={";
            for (size_t j = 0; j < node.succs.size(); ++j) {
                if (j > 0)
                    oss << ",";
                oss << node.succs[j];
            }
            oss << "}";
        }
        oss << "\n";
    }
    return oss.str();
}

std::string domTreeToString(const DominatorTree& domTree) {
    std::ostringstream oss;
    oss << "DominatorTree:\n";

    // BFS 从 entry 开始输出
    std::queue<uint32_t> queue;
    uint32_t entry = 0; // entryId
    queue.push(entry);
    std::unordered_set<uint32_t> visited;
    visited.insert(entry);

    while (!queue.empty()) {
        uint32_t n = queue.front();
        queue.pop();
        uint32_t id = domTree.idom(n);
        oss << "  BB" << n << " idom=BB" << (id == n ? static_cast<uint32_t>(-1) : id) << "\n";
        for (uint32_t c : domTree.children(n)) {
            if (visited.find(c) == visited.end()) {
                visited.insert(c);
                queue.push(c);
            }
        }
    }
    return oss.str();
}
