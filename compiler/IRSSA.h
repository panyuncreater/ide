/**
 * @file compiler/IRSSA.h
 * @brief P2-10 IR SSA 基础设施。
 *
 * 背景与目标（见 MiniLang优化清单_2026-07-23.md 第 10 项）：
 *   当前 IR 已是 SSA 风格（vreg 单赋值、单调递增不复用），但缺少：
 *     - 支配树 / 支配边界
 *     - PHI 节点（局部变量未进入 SSA）
 *     - 自然循环检测
 *     - 全局值编号（GVN，当前 CSE 仅基本块内）
 *     - 循环不变代码外提（LICM）
 *     - 函数内联
 *   本文件实现上述基础设施，使 IR 层具备生产级优化能力。
 *
 * 设计要点：
 *   1. **非侵入式 CFG 视图**：AstIRBuilder 产出的 IRBasicBlock 是"松散块"——单个块
 *      内可能含多个 LABEL 与终结指令（if/while/for 都在同一 currentBlock_ 中 emit）。
 *      IRCFG 将其展平并按 LABEL/终结指令切分为严格基本块（单入口、单终结），
 *      不修改 IRFunction 自身结构，因此既有 pass（CSE/常量折叠/循环展开）零影响。
 *   2. **PHI 生命周期**：PHI 仅在 ssaConstructPass 与 ssaDestructPass 之间存在。
 *      析构在 lowering 前完成，后端永远不看到 PHI（见 IR.h 的 PHI 注释）。
 *   3. **安全性**：GVN/LICM 与 CSE 同源约束——替换 dest vreg 引用后，栈式 VM 后端
 *      会产生栈残留 → OP_POP 栈下溢。因此这些 pass 仅对寄存器式后端启用，
 *      由 optimizeIR 的 enableGVN / enableLICM 旗标控制（默认 false）。
 *
 * 算法出处：
 *   - 支配树：Cooper, Harvey, Kennedy "A Simple, Fast Dominance Algorithm" (2001)
 *   - SSA 构造（PHI 插入）：Cytron 等 "Efficiently Computing Static Single Assignment
 *     Form and the Control Dependence Graph" (1991) —— 支配边界迭代插入
 *   - 自然循环：Aho 等 "Compilers: Principles, Techniques, and Tools" 回边定义
 *
 * @see IR.h optimizeIR gvnPass licmPass inlinePass
 */
#pragma once

#include "compiler/IR.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// 严格基本块（CFG 节点）
// ============================================================

/// CFG 中的一个严格基本块：单入口（一个 LABEL 或函数入口）、单终结指令。
/// 通过展平 IRFunction 的松散块并按 LABEL/终结指令切分得到。
struct CFGNode {
    uint32_t id = 0;         // 在 IRCFG::nodes 中的索引
    uint32_t labelIndex = 0; // 入口 LABEL 索引（入口块用 kEntryLabel）
    bool hasLabel = false;   // 是否以 LABEL 指令起始（入口块可能无 LABEL）

    // 源位置：所属的 IRBasicBlock 索引 + 指令范围 [startInstr, endInstr)
    // 切分不修改 IRFunction，变换 pass 通过此映射定位并修改原始指令。
    size_t sourceBlockIdx = 0;
    size_t startInstr = 0; // 含（指向 LABEL 或首条非终结指令）
    size_t endInstr = 0;   // 不含（指向终结指令的下一条）

    std::vector<uint32_t> preds; // 前驱节点 id 列表
    std::vector<uint32_t> succs; // 后继节点 id 列表

    static constexpr uint32_t kEntryLabel = 0xFFFFFFFF; // 入口块哨兵 label

    /// 是否为函数入口块（id == 0）
    bool isEntry() const { return id == 0; }
};

// ============================================================
// IRCFG：从 IRFunction 构建严格 CFG
// ============================================================

/// 严格 CFG 构建器。
///
/// 切分规则：
///   - 展平 IRFunction 所有 IRBasicBlock 的指令为单一序列（保持顺序）。
///   - 一个严格块在以下位置起始：函数第一条指令，或任意 LABEL 指令。
///   - 一个严格块在以下位置终结：第一条终结指令（JUMP/JUMP_IF_FALSE/RETURN/
///     RETURN_NULL/THROW），或下一条 LABEL 之前（隐式 fall-through）。
///   - 后继边：JUMP→目标 LABEL 块；JUMP_IF_FALSE→fall-through 块 + 目标 LABEL 块；
///     RETURN/RETURN_NULL/THROW→无后继；fall-through→相邻下一块。
///
/// 不可变性：构建后 CFG 不随 IRFunction 变更自动更新；变换 pass 修改 IR 后应重建。
class IRCFG {
public:
    /// 从 IRFunction 构建 CFG。空函数返回单入口空块。
    explicit IRCFG(const IRFunction& ir);

    /// 节点数
    size_t size() const { return nodes_.size(); }
    /// 入口节点 id（恒为 0）
    uint32_t entryId() const { return 0; }
    /// 取节点（只读）
    const CFGNode& node(uint32_t id) const { return nodes_[id]; }
    /// 取所有节点（只读）
    const std::vector<CFGNode>& nodes() const { return nodes_; }
    /// 按 LABEL 索引查找节点 id（无则返回 kInvalid）
    uint32_t nodeOfLabel(uint32_t labelIndex) const;

    static constexpr uint32_t kInvalid = 0xFFFFFFFF;

private:
    std::vector<CFGNode> nodes_;
    std::unordered_map<uint32_t, uint32_t> labelToNode_; // labelIndex → node id

    /// 判断指令是否为终结指令（块结束）
    static bool isTerminator(IROp op);
};

// ============================================================
// DominatorTree：Cooper-Harvey-Kennedy 支配树
// ============================================================

/// 支配树。基于 Cooper-Harvey-Kennedy 算法计算 immediate dominator。
///
/// 不变量：
///   - idom_[entry] = entry（自支配）
///   - idom_[n] 是 n 的严格支配者中离 n 最近的
///   - dominates(a, b) 当且仅当 a 在从 entry 到 b 的路径上必经 a
class DominatorTree {
public:
    /// 从 CFG 构建支配树
    explicit DominatorTree(const IRCFG& cfg);

    /// 取 immediate dominator（entry 返回自身，未计算/不可达返回 kInvalid）
    uint32_t idom(uint32_t node) const { return idom_[node]; }
    /// a 是否支配 b
    bool dominates(uint32_t a, uint32_t b) const;
    /// a 与 b 的最近公共支配者
    uint32_t nearestCommonDominator(uint32_t a, uint32_t b) const;
    /// 取 a 在支配树中的直接子节点（被 a 直接支配的节点）
    const std::vector<uint32_t>& children(uint32_t a) const { return domChildren_[a]; }

    static constexpr uint32_t kInvalid = 0xFFFFFFFF;

private:
    const IRCFG& cfg_;
    std::vector<uint32_t> idom_;                     // node → immediate dominator
    std::vector<std::vector<uint32_t>> domChildren_; // node → 直接支配的子节点列表
    std::vector<uint32_t> rpo_;                      // 反向后序
    std::vector<uint32_t> postorder_;                // 后序（用于 intersect 比较）
    std::vector<uint32_t> postOrderNum_;             // node → postorder 编号

    /// 反向后序遍历（构建 rpo_ 与 postorder_）
    void computeRPO();
    /// Cooper-Harvey-Kennedy 迭代求解 idom
    void computeIDom();
    /// intersect：沿 postorder 更深者上溯直到相遇（CHK 算法核心）
    uint32_t intersect(uint32_t b1, uint32_t b2) const;
};

// ============================================================
// DominanceFrontier：支配边界
// ============================================================

/// 支配边界。DF[n] = { m | n 支配 m 的某前驱，但 n 不严格支配 m }。
/// 用于 SSA 构造的 PHI 插入（Cytron 算法）。
class DominanceFrontier {
public:
    DominanceFrontier(const IRCFG& cfg, const DominatorTree& domTree);

    /// 取节点 n 的支配边界集合（节点 id 列表）
    const std::vector<uint32_t>& df(uint32_t n) const { return df_[n]; }

private:
    std::vector<std::vector<uint32_t>> df_;
};

// ============================================================
// NaturalLoopInfo：自然循环检测
// ============================================================

/// 自然循环信息。
///
/// 回边定义：边 n→h 是回边当且仅当 h 支配 n。
/// 自然循环 of 回边 n→h = {h} ∪ { 所有不经过 h 能到达 n 的节点 }。
class NaturalLoopInfo {
public:
    struct Loop {
        uint32_t header;                 // 循环头（支配者）
        std::vector<uint32_t> body;      // 循环体节点 id（含 header）
        std::vector<uint32_t> backEdges; // 回边源节点列表（尾→头）
    };

    NaturalLoopInfo(const IRCFG& cfg, const DominatorTree& domTree);

    /// 所有自然循环
    const std::vector<Loop>& loops() const { return loops_; }
    /// 节点 n 是否在任一循环体内
    bool inLoop(uint32_t n) const { return inLoop_.count(n) > 0; }
    /// 节点 n 所属的循环索引列表（可能属于多个嵌套循环）
    const std::vector<size_t>& loopsOf(uint32_t n) const {
        // BUGFIX-P2 #54: 非循环节点不在 loopsOf_ 中，.at() 会抛 std::out_of_range。
        // 改为返回静态空 vector 的 fallback，调用方可安全查询任意节点。
        static const std::vector<size_t> empty;
        auto it = loopsOf_.find(n);
        return it != loopsOf_.end() ? it->second : empty;
    }

private:
    const IRCFG& cfg_;
    std::vector<Loop> loops_;
    std::unordered_map<uint32_t, std::vector<size_t>> loopsOf_; // node → loop indices
    std::unordered_set<uint32_t> inLoop_;
    std::vector<std::pair<uint32_t, uint32_t>> backEdges_; // (tail, header) 回边列表
    std::unordered_set<uint32_t> loopBody_;                // collectLoopBody 的临时结果

    void detectBackEdges(const DominatorTree& domTree);
    /// 收集回边 n→h 的自然循环体（反向 BFS/DFS 从 n 出发，不越过 h）
    void collectLoopBody(uint32_t header, uint32_t tail);
};

// ============================================================
// SSA 构造与析构（局部变量 → SSA，含 PHI 插入）
// ============================================================

/// SSA 构造 pass：将函数内局部变量（LOAD_LOCAL/STORE_LOCAL）提升为 SSA 形态。
///
/// 步骤（Cytron 算法）：
///   1. 收集每个 local slot 的所有定义点（STORE_LOCAL）。
///   2. 对每个 slot，在其定义的支配边界迭代插入 PHI（直到不动点）。
///   3. 重命名：遍历支配树，将 LOAD_LOCAL/STORE_LOCAL 替换为 vreg 引用，
///      PHI 在合并点选择对应前驱的 vreg。
///
/// 约束：必须在 ssaDestructPass 之前调用；PHI 不可到达后端 lowering。
/// 返回：是否插入了 PHI（true 表示进入 SSA 形态）。
bool ssaConstructPass(IRFunction& ir);

/// SSA 析构 pass：将 PHI 节点消除，恢复为 LOAD_LOCAL/STORE_LOCAL 形态。
///
/// 策略：对每个 PHI(dest, (pred1, v1), (pred2, v2), ...)，在 pred1 尾部
/// 插入 STORE_LOCAL slot=v1，在 pred2 尾部插入 STORE_LOCAL slot=v2，
/// 将 PHI 替换为 LOAD_LOCAL slot→dest。
///
/// 返回：是否消除了 PHI（true 表示已退出 SSA 形态）。
bool ssaDestructPass(IRFunction& ir);

// ============================================================
// 优化 Pass（已在 IR.h 前向声明）
// ============================================================
// bool gvnPass(IRFunction& ir);     // 见 IR.h
// bool licmPass(IRFunction& ir);    // 见 IR.h
// bool inlinePass(IRModule& module); // 见 IR.h

// ============================================================
// 调试输出
// ============================================================

/// 将 CFG 格式化为可读字符串（含节点、前驱、后继）
std::string cfgToString(const IRCFG& cfg);
/// 将支配树格式化为可读字符串
std::string domTreeToString(const DominatorTree& domTree);
