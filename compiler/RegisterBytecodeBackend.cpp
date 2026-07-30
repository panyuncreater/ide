// ============================================================
// RegisterBytecodeBackend.cpp — PERF-14: IR → 寄存器式字节码 lowering
// ------------------------------------------------------------
// 将 IRFunction lowering 为 RegBytecodeChunk。
// 寄存器分配策略（简单线性映射）：
//   - 局部变量 slot N → 寄存器 N（slot 0..localCount-1 → reg 0..localCount-1）
//   - vreg N → 寄存器 localCount + N（如果 < 32）
//   - 超过 32 的寄存器需求：当前报错（未来实现溢出）
// ============================================================

#include "compiler/RegisterBytecodeBackend.h"
#include "common/Logger.h"
#include "common/RuntimeLimits.h" // P3-14: NO_INDEX/NO_SLOT 哨兵常量
#include "compiler/IRSSA.h"       // P2-10 fix: IRCFG/DominatorTree/NaturalLoopInfo 用于循环感知寄存器分配

RegisterBytecodeBackend::RegisterBytecodeBackend() = default;

void RegisterBytecodeBackend::resetState() {
    chunk_.reset();
    functionChunks_.clear();
    irToBytecodeOffset_.clear();
    vregToReg_.clear();
    vregLastUse_.clear();
    lastUseToVregs_.clear();
    freeRegs_.clear();
    labelToOffset_.clear();
    pendingJumps_.clear();
    hasError_ = false;
}

void RegisterBytecodeBackend::emitShort(uint16_t v, int line) {
    chunk_->writeShort(v, line);
}

uint16_t RegisterBytecodeBackend::addStringConstant(const std::string& s, const IRFunction& /*ir*/) {
    return chunk_->addConstant(Value(s));
}

uint8_t RegisterBytecodeBackend::vregToReg(uint32_t vreg) {
    auto it = vregToReg_.find(vreg);
    if (it != vregToReg_.end())
        return it->second;
    // P0-REGALLOC fix: 优先复用已释放的空闲寄存器，避免 vreg 单调递增导致溢出 32 上限。
    uint8_t reg;
    if (!freeRegs_.empty()) {
        reg = freeRegs_.back();
        freeRegs_.pop_back();
    } else {
        int newReg = chunk_->registerCount;
        if (newReg >= 32) {
            Logger::Error("RegisterBytecodeBackend: 寄存器分配溢出（vreg=" + std::to_string(vreg) + "，已用 " +
                              std::to_string(newReg) + "/32 寄存器，需减少局部变量/简化表达式或实现寄存器溢出）",
                          "RegIR");
            hasError_ = true;
            return 0; // 占位值，lower 会失败
        }
        chunk_->registerCount = newReg + 1;
        reg = static_cast<uint8_t>(newReg);
    }
    vregToReg_[vreg] = reg;
    return reg;
}

// P0-REGALLOC fix: 预扫描所有指令，记录每个 vreg 的最后使用 instrIndex。
// IR 的 vreg 是 SSA-like 的（allocVReg 单调递增不复用），因此生命周期从首次定义到
// 最后使用。在最后使用点释放寄存器是安全的——后续不会有指令再读取该 vreg。
//
// 关键例外：MEMBER_SET/INDEX_SET 的 obj vreg（操作数[0]）在后续 WRITEBACK_* 指令中
// 被"隐式使用"——RegisterVM 的 MEMBER_SET/INDEX_SET 将 objReg 记录到
// lastMutatedReceiverReg_，后续 WRITEBACK_* 通过 reg(lastMutatedReceiverReg_) 读取
// 变异后的容器。如果 obj vreg 在 MEMBER_SET 后被释放并复用，WRITEBACK 会读到错误值。
// 因此 obj vreg 的最后使用点必须延长到下一个 WRITEBACK_* 指令之后。
void RegisterBytecodeBackend::collectVRegLastUse(const IRFunction& ir) {
    vregLastUse_.clear();
    lastUseToVregs_.clear();
    size_t globalIdx = 0;
    // 待延长的 obj vreg（来自 MEMBER_SET/INDEX_SET，等待下一个 WRITEBACK 确定最后使用点）
    std::vector<uint32_t> pendingMutatedObjs;
    // METHOD_CALL/SUPER_CALL 的 obj vreg，默认最后使用点为 METHOD_CALL 本身，
    // 但如果后续有 LOAD_MUTATED（变异方法写回），则延长到 LOAD_MUTATED。
    // RegisterVM 的 METHOD_CALL 将 objReg 记录到 lastMutatedReceiverReg_，
    // 后续 LOAD_MUTATED 通过 reg(lastMutatedReceiverReg_) 读取变异后的容器。
    // 如果 obj vreg 在 METHOD_CALL 后被释放并复用，LOAD_MUTATED 会读到错误值。
    // 但非变异方法调用（如 len/substr）没有 LOAD_MUTATED，无需延长。
    // AUDIT-R4 BUG-02 fix: 与 pendingMutatedObjs 对齐为跨基本块保留——原实现
    // 在每个块开头 clear()，若 METHOD_CALL 与其 LOAD_MUTATED 被块边界分隔
    // （如 try handler 边、未来 IR 变换插入 LABEL），接收者寄存器会提前
    // 释放被复用，LOAD_MUTATED 读脏值损坏变异写回。跨块保留仅使无
    // LOAD_MUTATED 的旧条目被后续 LOAD_MUTATED 保守延长（寄存器多持有
    // 一段时间，安全方向），不会产生错误释放。
    std::vector<uint32_t> pendingMethodCallObjs;
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            bool isMutatingSet = (instr.op == IROp::MEMBER_SET || instr.op == IROp::INDEX_SET);
            bool isWriteback =
                (instr.op == IROp::WRITEBACK_MEMBER_VAR || instr.op == IROp::WRITEBACK_MEMBER_LOCAL ||
                 instr.op == IROp::WRITEBACK_INDEX_VAR || instr.op == IROp::WRITEBACK_INDEX_LOCAL ||
                 instr.op == IROp::WRITEBACK_MEMBER_UPVALUE || instr.op == IROp::WRITEBACK_INDEX_UPVALUE);
            bool isMethodOrSuperCall = (instr.op == IROp::METHOD_CALL || instr.op == IROp::SUPER_CALL);
            bool isLoadMutated = (instr.op == IROp::LOAD_MUTATED);
            for (size_t opi = 0; opi < instr.operands.size(); ++opi) {
                const auto& op = instr.operands[opi];
                if (op.kind != IROperandKind::VIRTUAL)
                    continue;
                // MEMBER_SET/INDEX_SET 的 obj vreg（操作数[0]）跳过——
                // 其最后使用点延长到下一个 WRITEBACK_* 指令
                if (isMutatingSet && opi == 0) {
                    pendingMutatedObjs.push_back(op.index);
                    continue;
                }
                // METHOD_CALL/SUPER_CALL 的 obj vreg（操作数[1]）：默认最后使用为当前指令，
                // 但加入 pending 列表，如果后续有 LOAD_MUTATED 则覆盖延长。
                if (isMethodOrSuperCall && opi == 1) {
                    vregLastUse_[op.index] = globalIdx;
                    pendingMethodCallObjs.push_back(op.index);
                    continue;
                }
                vregLastUse_[op.index] = globalIdx;
            }
            // 遇到 WRITEBACK_* 时，延长 pending obj vreg 的最后使用点到此处
            if (isWriteback && !pendingMutatedObjs.empty()) {
                for (uint32_t v : pendingMutatedObjs) {
                    vregLastUse_[v] = globalIdx;
                }
                pendingMutatedObjs.clear();
            }
            // 遇到 LOAD_MUTATED 时，延长 METHOD_CALL/SUPER_CALL 的 obj vreg 生命周期到此处。
            // 这会覆盖之前设的默认值（METHOD_CALL 指令索引），使寄存器在 LOAD_MUTATED 后才释放。
            if (isLoadMutated && !pendingMethodCallObjs.empty()) {
                for (uint32_t v : pendingMethodCallObjs) {
                    vregLastUse_[v] = globalIdx;
                }
                pendingMethodCallObjs.clear();
            }
            ++globalIdx;
        }
    }
    // 未匹配到 WRITEBACK 的 obj vreg 不释放（保守安全，避免寄存器被复用覆盖）
    // P2-10 fix: 线性扫描不感知循环回边，循环外定义、循环内使用的 vreg 会被提前释放。
    // 在构建反向映射前，先延长循环不变 vreg 的最后使用点到循环末尾。
    extendLastUseForLoops(ir);
    // L4 fix: 线性扫描不感知 finally 块控制流，return 值 vreg 会被 finally 块覆盖。
    extendLastUseForFinallyJumps(ir);

    // 构建反向映射：instrIndex → 在此处最后使用的 vreg 列表
    for (const auto& kv : vregLastUse_) {
        lastUseToVregs_[kv.second].push_back(kv.first);
    }
}

// P2-10 fix: 循环感知最后使用扩展
// ------------------------------------------------------------
// 问题：collectVRegLastUse 的线性扫描假设 vreg 的"最后使用"是指令序列中最后一次引用。
//       但在循环中，vreg 会被多次使用（每轮迭代一次）。如果 vreg 定义在循环外、
//       使用在循环内（如循环上界常量 v3 = LOAD_CONST 3，用于 LT v8 v22 v3），
//       线性扫描会在循环内的最后一次引用后释放寄存器。该寄存器可能被循环内的
//       另一个 vreg 复用（如 MUL v12 v24 v22 复用 v3 的寄存器），导致下一轮迭代
//       LT 指令读到 v12 的值而非 v3 的值（3），循环条件错误。
//
// 修复：对每个自然循环，收集循环体内"定义"和"使用"的 vreg 集合。
//       对使用在循环内但定义在循环外的 vreg（循环不变量），将其最后使用点
//       延长到循环体的最后一条指令索引，确保寄存器在整个循环期间不被释放复用。
//
// 判定"定义"：指令首操作数为 VIRTUAL 且指令属于"写"语义（LOAD_*/算术/比较/CALL 等）。
//            STORE_LOCAL/MEMBER_SET/INDEX_SET 等指令的首操作数虽可能为 VIRTUAL（obj），
//            但属于"读"语义，不算定义。
void RegisterBytecodeBackend::extendLastUseForLoops(const IRFunction& ir) {
    IRCFG cfg(ir);
    if (cfg.size() < 2)
        return;
    DominatorTree domTree(cfg);
    NaturalLoopInfo loopInfo(cfg, domTree);
    if (loopInfo.loops().empty())
        return;

    // 计算每个 IRBasicBlock 在扁平指令序列中的起始偏移
    // collectVRegLastUse 的 globalIdx = sum(block[0..i-1].instructions.size()) + localIdx
    std::vector<size_t> blockFlatOffset;
    blockFlatOffset.reserve(ir.blocks.size());
    size_t acc = 0;
    for (const auto& blk : ir.blocks) {
        blockFlatOffset.push_back(acc);
        acc += blk.instructions.size();
    }

    // 辅助：判断指令是否"定义"了首操作数 vreg（写入 dest）
    // 与 IR.cpp 的 isPureCompute / lowering 代码中的 dest 约定一致：
    // 有 dest 的指令首操作数为 VIRTUAL；STORE_*/MEMBER_SET/INDEX_SET/WRITEBACK_*
    // 等无 dest，首操作数为 slot/obj/name_idx（虽可能 VIRTUAL 但非 dest）。
    static const auto isStoreLikeOp = [](IROp op) {
        switch (op) {
        case IROp::STORE_LOCAL:
        case IROp::STORE_GLOBAL:
        case IROp::STORE_UPVALUE:
        case IROp::MEMBER_SET:
        case IROp::MEMBER_SET_LOCAL: // L7 fix
        case IROp::INDEX_SET:
        case IROp::WRITEBACK_MEMBER_VAR:
        case IROp::WRITEBACK_MEMBER_LOCAL:
        case IROp::WRITEBACK_INDEX_VAR:
        case IROp::WRITEBACK_INDEX_LOCAL:
        case IROp::WRITEBACK_MEMBER_UPVALUE:
        case IROp::WRITEBACK_INDEX_UPVALUE:
        case IROp::DEFINE_GLOBAL:
        case IROp::DELETE_VAR:
        case IROp::CLOSE_UPVALUE:
        case IROp::LABEL:
        case IROp::JUMP:
        case IROp::JUMP_IF_FALSE:
        case IROp::RETURN:
        case IROp::RETURN_NULL:
        case IROp::THROW:
        case IROp::POP:
        case IROp::PRINT:
        case IROp::YIELD:
        case IROp::AWAIT:
        case IROp::INIT_FIELD:
            return true;
        default:
            return false;
        }
    };

    for (const auto& loop : loopInfo.loops()) {
        // 计算循环体的最后一条指令的扁平索引（循环末尾）
        size_t loopEndIdx = 0;
        for (uint32_t nodeId : loop.body) {
            const auto& node = cfg.node(nodeId);
            size_t nodeEnd = blockFlatOffset[node.sourceBlockIdx] + node.endInstr;
            if (nodeEnd > loopEndIdx)
                loopEndIdx = nodeEnd;
        }
        if (loopEndIdx == 0)
            continue;

        // 收集循环体内定义和使用的 vreg
        std::unordered_set<uint32_t> vregsDefinedInLoop;
        std::unordered_set<uint32_t> vregsUsedInLoop;
        for (uint32_t nodeId : loop.body) {
            const auto& node = cfg.node(nodeId);
            const auto& blk = ir.blocks[node.sourceBlockIdx];
            for (size_t i = node.startInstr; i < node.endInstr; ++i) {
                const auto& instr = blk.instructions[i];
                bool hasDest = !instr.operands.empty() && instr.operands[0].kind == IROperandKind::VIRTUAL &&
                               !isStoreLikeOp(instr.op);
                for (size_t opi = 0; opi < instr.operands.size(); ++opi) {
                    const auto& op = instr.operands[opi];
                    if (op.kind != IROperandKind::VIRTUAL)
                        continue;
                    if (opi == 0 && hasDest) {
                        vregsDefinedInLoop.insert(op.index);
                    } else {
                        vregsUsedInLoop.insert(op.index);
                    }
                }
            }
        }

        // 对使用在循环内但定义在循环外的 vreg，延长最后使用点到循环末尾
        for (uint32_t vreg : vregsUsedInLoop) {
            if (vregsDefinedInLoop.count(vreg) > 0)
                continue; // 循环内定义，每轮迭代重新写入，安全释放
            auto it = vregLastUse_.find(vreg);
            if (it != vregLastUse_.end() && it->second < loopEndIdx) {
                it->second = loopEndIdx;
            }
        }
    }
}

// L4 fix: finally 块感知最后使用扩展
// ------------------------------------------------------------
// 问题：return 在 try-finally 内时，IR 生成如下序列：
//   <define val>
//   PUSH_JUMP_TARGET returnLandingLabel   ← 记录返回着陆点
//   JUMP finallyEntryLabel                 ← 跳到 finally 入口
//   LABEL returnLandingLabel              ← 着陆点（控制流从 FINALLY_END 跳回此处）
//   RETURN val                             ← 使用 return 值 vreg
//   ...
//   LABEL finallyEntryLabel                ← finally 块开始
//   <finally block body>
//   FINALLY_END                             ← finally 块结束（跳回 returnLandingLabel）
//
// 线性顺序中 RETURN(val) 在 finally 块之前，故 vregLastUse_[val] = RETURN 索引，
// finally 块的 vreg 可能复用 val 的寄存器。但控制流上 finally 块在 RETURN 之前执行，
// 导致 RETURN 读到被 finally 块覆盖的错误值。
//
// 修复：扫描 PUSH_JUMP_TARGET 指令，定位对应的 finally 块（finallyEntryLabel→FINALLY_END），
// 将着陆点后使用但定义在 PUSH_JUMP_TARGET 之前的 vreg 的最后使用点延长到 FINALLY_END。
void RegisterBytecodeBackend::extendLastUseForFinallyJumps(const IRFunction& ir) {
    // 扁平化所有指令，构建 label → 全局索引 映射
    struct FlatInstr {
        const IRInstruction* instr;
        size_t globalIdx;
    };
    std::vector<FlatInstr> flat;
    std::unordered_map<uint32_t, size_t> labelToIdx; // label index → global instruction index

    size_t globalIdx = 0;
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            flat.push_back({&instr, globalIdx});
            if (instr.op == IROp::LABEL && !instr.operands.empty() && instr.operands[0].kind == IROperandKind::LABEL) {
                labelToIdx[instr.operands[0].index] = globalIdx;
            }
            ++globalIdx;
        }
    }
    if (flat.empty())
        return;

    // 扫描 PUSH_JUMP_TARGET，对每个 finally 跳转扩展 vreg 生命周期
    for (size_t i = 0; i < flat.size(); ++i) {
        if (flat[i].instr->op != IROp::PUSH_JUMP_TARGET)
            continue;
        if (flat[i].instr->operands.empty() || flat[i].instr->operands[0].kind != IROperandKind::LABEL)
            continue;
        uint32_t landingPadLabel = flat[i].instr->operands[0].index;

        // 下一条应为 JUMP finallyEntryLabel
        if (i + 1 >= flat.size() || flat[i + 1].instr->op != IROp::JUMP)
            continue;
        if (flat[i + 1].instr->operands.empty() || flat[i + 1].instr->operands[0].kind != IROperandKind::LABEL)
            continue;
        uint32_t finallyEntryLabel = flat[i + 1].instr->operands[0].index;

        // 查找着陆点 LABEL 的全局索引
        auto landingIt = labelToIdx.find(landingPadLabel);
        if (landingIt == labelToIdx.end())
            continue;
        size_t landingIdx = landingIt->second;

        // 查找 finally 入口 LABEL 的全局索引
        auto finallyEntryIt = labelToIdx.find(finallyEntryLabel);
        if (finallyEntryIt == labelToIdx.end())
            continue;
        size_t finallyEntryIdx = finallyEntryIt->second;

        // 从 finally 入口扫描到 FINALLY_END，确定 finally 块末尾索引
        size_t finallyEndIdx = finallyEntryIdx;
        for (size_t j = finallyEntryIdx; j < flat.size(); ++j) {
            if (flat[j].instr->op == IROp::FINALLY_END) {
                finallyEndIdx = j;
                break;
            }
        }
        // 若未找到 FINALLY_END，用 finally 块区域的最大索引兜底
        if (finallyEndIdx == finallyEntryIdx)
            finallyEndIdx = flat.size() - 1;

        // 收集着陆点之后、finally 入口之前使用的所有 vreg
        // （这些 vreg 定义在 PUSH_JUMP_TARGET 之前，需跨越 finally 块存活）
        // landingIdx 指向 LABEL 指令，从下一条开始扫描到 finallyEntryIdx
        for (size_t j = landingIdx + 1; j < finallyEntryIdx && j < flat.size(); ++j) {
            for (const auto& op : flat[j].instr->operands) {
                if (op.kind != IROperandKind::VIRTUAL)
                    continue;
                auto it = vregLastUse_.find(op.index);
                if (it != vregLastUse_.end() && it->second < finallyEndIdx) {
                    it->second = finallyEndIdx;
                }
            }
        }
    }
}

// P0-REGALLOC fix: 释放最后使用点 == instrIndex 的 vreg 的寄存器到 freeRegs_。
// 在每条指令 lower 完成后调用，确保该指令已读取完所有操作数。
void RegisterBytecodeBackend::releaseDeadVRegs(size_t instrIndex) {
    auto it = lastUseToVregs_.find(instrIndex);
    if (it == lastUseToVregs_.end())
        return;
    for (uint32_t vreg : it->second) {
        auto regIt = vregToReg_.find(vreg);
        if (regIt != vregToReg_.end()) {
            freeRegs_.push_back(regIt->second);
            vregToReg_.erase(regIt);
        }
    }
    lastUseToVregs_.erase(it);
}

bool RegisterBytecodeBackend::lower(const IRFunction& ir) {
    chunk_ = std::make_unique<RegBytecodeChunk>();
    chunk_->name = ir.name;
    chunk_->arity = ir.arity;
    chunk_->requiredArity = ir.requiredArity;
    // P2-1 fix: localCount 超 32 会导致 chunk_->registerCount > 32，
    // RegCallFrame::registers 是 std::array<Value, 32>，写入 registers[32+] 为 OOB。
    // 与 vregToReg 的 reg >= 32 检查互补，硬失败阻止生成损坏字节码。
    if (ir.localCount > 32) {
        Logger::Error("RegisterBytecodeBackend: localCount 超出 32 寄存器上限 (" + std::to_string(ir.localCount) +
                          "，函数 " + ir.name + ")，需减少局部变量或实现寄存器溢出",
                      "RegIR");
        hasError_ = true;
        return false;
    }
    chunk_->localCount = ir.localCount;
    chunk_->registerCount = ir.localCount; // 初始为 localCount，vreg 分配后增长
    chunk_->defaultConstIndices = ir.defaultConstIndices;
    chunk_->upvalues = ir.upvalues;
    // BUG-IDE-12 fix: 复制 slot→name 映射，供 RegisterVM 条件断点求值反查变量名
    chunk_->localRegNames = ir.localSlotNames;
    // R164 协程/生成器：复制生成器标记和 yieldCount（RegisterVM 在 REG_CALL 时检测）
    chunk_->isGenerator = ir.isGenerator;
    chunk_->yieldCount = ir.yieldCount;

    // 复制常量池（保持索引一致）
    for (const auto& c : ir.constants) {
        chunk_->constants.push_back(c);
    }

    // P0-REGALLOC fix: 预扫描所有指令，构建 vreg → 最后使用 instrIndex 映射，
    // 供 releaseDeadVRegs 在 lowering 过程中及时释放死寄存器复用。
    collectVRegLastUse(ir);

    // 遍历所有基本块的所有指令，逐条 lowering
    size_t instrIndex = 0;
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            irToBytecodeOffset_.push_back({instrIndex, chunk_->code.size()});
            if (!lowerInstruction(instr, ir) || hasError_) {
                return false; // lowering 失败或 vreg 溢出：不生成损坏的字节码
            }
            // 填充行号表
            // BUG-IBACKEND-2: 同步填充 columns（IRInstruction 暂无 column 字段，默认 0）
            while (static_cast<int>(chunk_->lines.size()) < static_cast<int>(chunk_->code.size())) {
                chunk_->lines.push_back(instr.line);
                chunk_->columns.push_back(0);
            }
            // P0-REGALLOC fix: 释放最后使用点 == instrIndex 的 vreg 的寄存器
            releaseDeadVRegs(instrIndex);
            ++instrIndex;
        }
    }
    if (!patchJumps())
        return false;

    // L1 fix: 将 IR 指令范围的 slotNameRanges 翻译为字节码 IP 范围（对齐 BytecodeIRBackend）
    if (!ir.slotNameRanges.empty() && !irToBytecodeOffset_.empty()) {
        auto translateInstr = [this](size_t instrIdx) -> size_t {
            size_t lo = 0, hi = irToBytecodeOffset_.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (irToBytecodeOffset_[mid].first <= instrIdx)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo == 0)
                return irToBytecodeOffset_[0].second;
            return irToBytecodeOffset_[lo - 1].second;
        };
        for (const auto& range : ir.slotNameRanges) {
            RegBytecodeChunk::SlotNameRange out;
            out.slot = static_cast<uint8_t>(range.slot);
            out.name = range.name;
            out.startIp = translateInstr(range.startInstr);
            if (range.endInstr == 0) {
                out.endIp = 0;
            } else {
                out.endIp = translateInstr(range.endInstr);
            }
            chunk_->slotNameRanges.push_back(std::move(out));
        }
    }
    return true;
}

// ============================================================
// lowerInstruction — 按 IROp 类别分发的 lowering 主入口
// ------------------------------------------------------------
// 本函数原为 979 行的巨型 switch，按 IROp 类别拆分为 11 个 lowerXxxOps 私有方法。
// 主函数保留 switch 外壳做类别分发，每个 case 调用对应的 lowerXxxOps。
// 三后端语义等价 / 寄存器分配 / patch 位置约定等不变量在拆分后保持一致。
//
// 统一的写码约定（理解各 lowerXxxOps 内部 case 的关键）：
//   · writeOp(RegOp)        写 1 字节操作码。
//   · writeReg(reg)         写 1 字节"寄存器号"(0..31)，内部 assert(reg<32)——只用于真正的寄存器。
//   · writeShort(uint16)    写 2 字节小端常量池索引 / 跳转目标 / 名称索引。
//   · writeByte(uint8)      写 1 字节"数量或描述符"(argCount/uvCount/fieldCount/isLocal/idx 等)，
//                           无 assert。正因如此，凡"数量操作数"必须用 writeByte 而非 writeReg，
//                           否则数量 >= 32 时 Debug 构建的 assert 会崩溃（见各处 AUDIT-BUG-C1）。
// 寄存器式与栈式的本质差异：所有中间结果存在固定寄存器而非操作数栈，故 POP/DUP 退化为
// REG_MOVE 或 no-op；嵌套左值变异时 obj 寄存器记录在 lastMutatedReceiverReg_，由 WRITEBACK_*
// 读取——这正是 collectVRegLastUse 需把 obj vreg 寿命延到 WRITEBACK 之后的原因。
// AUDIT-P2 fix: vregToReg 溢出后设置 hasError_ 并返回占位 reg=0，但此前各 case 不检查
// hasError_ 仍继续写入损坏字节码。在 lowerInstruction 入口快速失败，避免溢出后后续
// 指令继续写入。当前指令的损坏字节码由 lower() 行 164 的 hasError_ 检查兜底（return false），
// chunk_ 不会被 VM 加载使用。
// ============================================================
bool RegisterBytecodeBackend::lowerInstruction(const IRInstruction& instr, const IRFunction& ir) {
    if (hasError_)
        return false;

    // AUDIT-R4 BUG-05 fix: 操作数 kind 防御——CONSTANT kind 仅在以下白名单指令
    // 的固定位置合法（LOAD_CONST/TYPE_CHECK/TYPE_TEST/BUILD_ENUM_VARIANT/
    // ENUM_VARIANT_NAME 的常量池索引位）。其余指令的操作数若出现 CONSTANT
    // kind（典型来源：copyPropagationPass 将 VIRTUAL 替换为常量索引），
    // 下方各 lower* 会把常量索引误作 vreg 编号传入 vregToReg 读错寄存器
    // （静默数据损坏）。此处快速失败转为干净的编译错误，在未来重新
    // 启用复制传播而 lowering 未先支持常量物化时立即暴露而非静默读错。
    switch (instr.op) {
    case IROp::LOAD_CONST:
    case IROp::TYPE_CHECK:
    case IROp::TYPE_TEST:
    case IROp::BUILD_ENUM_VARIANT:
    case IROp::ENUM_VARIANT_NAME:
        break; // 白名单：自身按固定位置解释 CONSTANT 操作数
    default:
        for (const auto& op : instr.operands) {
            if (op.kind == IROperandKind::CONSTANT) {
                Logger::Error("RegisterBytecodeBackend: 指令 " + std::to_string(static_cast<int>(instr.op)) +
                                  " 含非法 CONSTANT 操作数（常量索引 " + std::to_string(op.index) +
                                  "）——lowering 尚不支持常量物化，请先禁用复制传播",
                              "RegIR");
                hasError_ = true;
                return false;
            }
        }
        break;
    }

    switch (instr.op) {
    // ---- 常量加载 ----
    case IROp::LOAD_CONST:
    case IROp::LOAD_NULL:
    case IROp::LOAD_TRUE:
    case IROp::LOAD_FALSE:
        return lowerConstOps(instr, ir);

    // ---- 局部变量 / 全局变量 ----
    case IROp::LOAD_LOCAL:
    case IROp::STORE_LOCAL:
    case IROp::LOAD_GLOBAL:
    case IROp::STORE_GLOBAL:
    case IROp::DEFINE_GLOBAL:
    case IROp::DELETE_VAR:
        return lowerVarOps(instr, ir);

    // ---- upvalue / 闭包 ----
    case IROp::LOAD_UPVALUE:
    case IROp::STORE_UPVALUE:
    case IROp::CLOSE_UPVALUE:
    case IROp::MAKE_CLOSURE:
        return lowerUpvalueOps(instr, ir);

    // ---- 算术 ----
    case IROp::ADD:
    case IROp::SUB:
    case IROp::MUL:
    case IROp::DIV:
    case IROp::MOD:
    case IROp::NEGATE:
        return lowerArithOps(instr, ir);

    // ---- 比较 ----
    case IROp::EQ:
    case IROp::NEQ:
    case IROp::LT:
    case IROp::GT:
    case IROp::LTE:
    case IROp::GTE:
        return lowerCompareOps(instr, ir);

    // ---- 逻辑 ----
    case IROp::NOT:
        return lowerLogicOps(instr, ir);

    // ---- 控制流 ----
    case IROp::LABEL:
    case IROp::JUMP:
    case IROp::JUMP_IF_FALSE:
    case IROp::RETURN:
    case IROp::RETURN_NULL:
    // R164 协程/生成器：YIELD 可中断执行（抛 VMYieldSignal），归入控制流组
    case IROp::YIELD:
    // 七特性 MVP 阶段 4：AWAIT 同步 drain（可能抛异常/错误），归入控制流组
    case IROp::AWAIT:
        return lowerControlOps(instr, ir);

    // ---- 调用 ----
    case IROp::CALL:
    case IROp::CALL_EXPR:
    case IROp::TAIL_CALL: // L18 eng-tailcall
    case IROp::METHOD_CALL:
    case IROp::SUPER_CALL:
        return lowerCallOps(instr, ir);

    // ---- 容器 / 成员访问 ----
    case IROp::BUILD_ARRAY:
    case IROp::BUILD_DICT:
    case IROp::BUILD_TUPLE:
    case IROp::INDEX_GET:
    case IROp::INDEX_SET:
    case IROp::MEMBER_GET:
    case IROp::SUPER_MEMBER_GET:
    case IROp::MEMBER_SET:
    case IROp::MEMBER_SET_LOCAL: // L7 fix
    // R99 枚举与 ADT：enum variant 构造/检查/取字段（与容器指令同属 lowerContainerOps）
    case IROp::BUILD_ENUM_VARIANT:
    case IROp::ENUM_VARIANT_NAME:
    case IROp::ENUM_VARIANT_FIELD:
    // R133 模式匹配扩展：容器长度（与容器指令同属 lowerContainerOps）
    case IROp::LEN:
        return lowerContainerOps(instr, ir);

    // ---- 类 ----
    case IROp::DEFINE_CLASS:
    case IROp::CLASS_NEW:
    case IROp::INIT_FIELD:
        return lowerClassOps(instr, ir);

    // ---- 异常 / 写回 / 其他 ----
    case IROp::TRY_BEGIN:
    case IROp::TRY_END:
    case IROp::THROW:
    case IROp::LOAD_EXCEPTION:
    case IROp::PUSH_JUMP_TARGET:
    case IROp::FINALLY_END:
    case IROp::WRITEBACK_MEMBER_VAR:
    case IROp::WRITEBACK_MEMBER_LOCAL:
    case IROp::WRITEBACK_INDEX_VAR:
    case IROp::WRITEBACK_INDEX_LOCAL:
    case IROp::WRITEBACK_MEMBER_UPVALUE:
    case IROp::WRITEBACK_INDEX_UPVALUE:
    case IROp::PRINT:
    case IROp::POP:
    case IROp::DUP:
    case IROp::LOAD_MUTATED:
    case IROp::TYPE_CHECK:
    case IROp::TYPE_TEST: // R133: 软类型测试，与 TYPE_CHECK 同属 misc 类
        return lowerMiscOps(instr, ir);

    default:
        Logger::Error("RegisterBytecodeBackend: 未支持的 IR 指令 " + std::to_string(static_cast<int>(instr.op)),
                      "RegIR");
        return false;
    }
}

// ============================================================
// globalName — 全局变量名查找辅助
// ------------------------------------------------------------
// idx 越界返回空字符串。原 lowerInstruction 内的 lambda，提取为静态方法
// 供各 lowerXxxOps 共用。
// ============================================================
std::string RegisterBytecodeBackend::globalName(const IRFunction& ir, uint32_t idx) {
    return idx < ir.globalNames.size() ? ir.globalNames[idx] : std::string{};
}

// ============================================================
// lowerConstOps — 常量加载类（LOAD_CONST / LOAD_NULL / LOAD_TRUE / LOAD_FALSE）
// ============================================================
bool RegisterBytecodeBackend::lowerConstOps(const IRInstruction& instr, const IRFunction& /*ir*/) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::LOAD_CONST: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t constIdx = static_cast<uint16_t>(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_LOAD_CONST, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(constIdx, line);
        break;
    }
    case IROp::LOAD_NULL: {
        if (instr.operands.empty())
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_NULL, line);
        chunk_->writeReg(dst, line);
        break;
    }
    case IROp::LOAD_TRUE: {
        if (instr.operands.empty())
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_TRUE, line);
        chunk_->writeReg(dst, line);
        break;
    }
    case IROp::LOAD_FALSE: {
        if (instr.operands.empty())
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_FALSE, line);
        chunk_->writeReg(dst, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerVarOps — 变量类（LOAD/STORE_LOCAL + LOAD/STORE/DEFINE_GLOBAL + DELETE_VAR）
// ============================================================
bool RegisterBytecodeBackend::lowerVarOps(const IRInstruction& instr, const IRFunction& ir) {
    int line = instr.line;
    switch (instr.op) {
    // ---- 局部变量（在寄存器式中，local slot 即寄存器）----
    case IROp::LOAD_LOCAL: {
        // IR: LOAD_LOCAL dest_vreg, slot
        // → REG_MOVE dest_vreg_reg, slot_reg
        if (instr.operands.size() < 2)
            return false;
        // Bug-14 同型修复：local slot 超 32 寄存器硬上限静默截断会读写错误寄存器
        if (instr.operands[1].index >= 32) {
            Logger::Error("RegBytecodeBackend: LOAD_LOCAL local slot 超出 32 寄存器上限 (slot=" +
                              std::to_string(instr.operands[1].index) + ")",
                          "RegBackend");
            return false;
        }
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = static_cast<uint8_t>(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_MOVE, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }
    case IROp::STORE_LOCAL: {
        // IR: STORE_LOCAL slot, src_vreg
        // → REG_MOVE slot_reg, src_vreg_reg
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].index >= 32) {
            Logger::Error("RegBytecodeBackend: STORE_LOCAL local slot 超出 32 寄存器上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "RegBackend");
            return false;
        }
        uint8_t dst = static_cast<uint8_t>(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_MOVE, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }

    // ---- 全局变量 ----
    case IROp::LOAD_GLOBAL: {
        // IR: LOAD_GLOBAL dest_vreg, global_idx
        //   IMM_UINT → REG_LOAD_GLOBAL dest, slot(2B)
        //   GLOBAL_NAME → REG_LOAD_GLOBAL dest, nameIdx(2B)（统一用 slot 形式，name 也走常量池）
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        if (instr.operands[1].kind == IROperandKind::IMM_UINT) {
            chunk_->writeOp(RegOp::REG_LOAD_GLOBAL, line);
            chunk_->writeReg(dst, line);
            chunk_->writeShort(static_cast<uint16_t>(instr.operands[1].index), line);
        } else {
            // 名称版：将名称作为常量，用高 bit 标记（slot < 32768, nameIdx >= 32768）
            // 简化：名称版也走常量池查找 slot
            uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[1].index), ir);
            chunk_->writeOp(RegOp::REG_LOAD_GLOBAL, line);
            chunk_->writeReg(dst, line);
            chunk_->writeShort(nameIdx | 0x8000, line); // 高 bit 置 1 表示名称索引
        }
        break;
    }
    case IROp::STORE_GLOBAL: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t src = vregToReg(instr.operands[1].index);
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->writeOp(RegOp::REG_STORE_GLOBAL, line);
            chunk_->writeReg(src, line);
            chunk_->writeShort(static_cast<uint16_t>(instr.operands[0].index), line);
        } else {
            uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[0].index), ir);
            chunk_->writeOp(RegOp::REG_STORE_GLOBAL, line);
            chunk_->writeReg(src, line);
            chunk_->writeShort(nameIdx | 0x8000, line);
        }
        break;
    }
    case IROp::DEFINE_GLOBAL: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t src = vregToReg(instr.operands[1].index);
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->writeOp(RegOp::REG_DEFINE_GLOBAL, line);
            chunk_->writeReg(src, line);
            chunk_->writeShort(static_cast<uint16_t>(instr.operands[0].index), line);
        } else {
            uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[0].index), ir);
            chunk_->writeOp(RegOp::REG_DEFINE_GLOBAL, line);
            chunk_->writeReg(src, line);
            chunk_->writeShort(nameIdx | 0x8000, line);
        }
        break;
    }
    case IROp::DELETE_VAR: {
        // BUG-IR-TRY-1 fix: 删除全局变量（catch 块退出后清理 catch 变量）
        // operands: [global_idx]，kind 可为 IMM_UINT (GLOBAL_SLOT) 或 GLOBAL_NAME
        // → REG_DELETE_GLOBAL nameConstIdx(2B)
        // 注意：REG_DELETE_GLOBAL 按 name 常量索引删除，无高 bit 标记区分
        if (instr.operands.size() < 1)
            return false;
        uint16_t nameIdx;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            // IMM_UINT (slot) 路径：RegisterBytecodeBackend 无 globalSlotNames 表，
            // 理论上不会触达（visitTryStmt 用 GLOBAL_NAME kind），此处保守返回 false。
            Logger::Error("RegisterBytecodeBackend: DELETE_VAR 不支持 IMM_UINT kind（无 slot→name 表）", "RegIR");
            return false;
        } else {
            nameIdx = addStringConstant(globalName(ir, instr.operands[0].index), ir);
        }
        chunk_->writeOp(RegOp::REG_DELETE_GLOBAL, line);
        chunk_->writeShort(nameIdx, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerUpvalueOps — 闭包 upvalue 类（LOAD/STORE_UPVALUE + CLOSE_UPVALUE + MAKE_CLOSURE）
// ============================================================
bool RegisterBytecodeBackend::lowerUpvalueOps(const IRInstruction& instr, const IRFunction& ir) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::LOAD_UPVALUE: {
        if (instr.operands.size() < 2)
            return false;
        // Bug-14 同型修复：upvalue 索引超 255 静默截断会读写错误槽
        if (instr.operands[1].index >= 256) {
            Logger::Error("RegBytecodeBackend: LOAD_UPVALUE upvalue 索引超出 255 上限 (uvIdx=" +
                              std::to_string(instr.operands[1].index) + ")",
                          "RegBackend");
            return false;
        }
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_UPVALUE, line);
        chunk_->writeReg(dst, line);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index));
        while (static_cast<int>(chunk_->lines.size()) < static_cast<int>(chunk_->code.size())) {
            chunk_->lines.push_back(line);
            chunk_->columns.push_back(0); // BUG-IBACKEND-2
        }
        break;
    }
    case IROp::STORE_UPVALUE: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error("RegBytecodeBackend: STORE_UPVALUE upvalue 索引超出 255 上限 (uvIdx=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "RegBackend");
            return false;
        }
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_STORE_UPVALUE, line);
        chunk_->writeReg(src, line);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index));
        while (static_cast<int>(chunk_->lines.size()) < static_cast<int>(chunk_->code.size())) {
            chunk_->lines.push_back(line);
            chunk_->columns.push_back(0); // BUG-IBACKEND-2
        }
        break;
    }
    case IROp::CLOSE_UPVALUE: {
        // B1 fix: operand 现为 slot_base（块作用域基址），运行时关闭所有
        // 指向 slot >= frameIdx*MAX_REGISTERS+slot_base 的 open upvalues。
        if (instr.operands.empty())
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error("RegBytecodeBackend: CLOSE_UPVALUE slot_base 超出 255 上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "RegBackend");
            return false;
        }
        chunk_->writeOp(RegOp::REG_CLOSE_UPVALUE, line);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index));
        while (static_cast<int>(chunk_->lines.size()) < static_cast<int>(chunk_->code.size())) {
            chunk_->lines.push_back(line);
            chunk_->columns.push_back(0); // BUG-IBACKEND-2
        }
        break;
    }
    case IROp::MAKE_CLOSURE: {
        // IR: MAKE_CLOSURE dest, name_idx, uv_count, [isLocal, idx]×uv_count
        if (instr.operands.size() < 3)
            return false;
        // P2-3 fix: uvCount/isLocal/idx 经 writeByte 编码为 1 字节，
        // & 0xFF 静默截断会生成错误 upvalue 描述符（闭包捕获错误变量）。
        if (instr.operands[2].index >= 256) {
            Logger::Error("RegisterBytecodeBackend: MAKE_CLOSURE uvCount 超出 255 上限 (" +
                              std::to_string(instr.operands[2].index) + ")",
                          "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[1].index), ir);
        uint8_t uvCount = static_cast<uint8_t>(instr.operands[2].index);
        chunk_->writeOp(RegOp::REG_MAKE_CLOSURE, line);
        chunk_->writeReg(dst, line); // dst 是真正的寄存器号，用 writeReg
        chunk_->writeShort(nameIdx, line);
        // AUDIT-REGVB fix: uvCount/isLocal/idx 不是寄存器号，是 upvalue 描述符。
        // 原用 writeReg（含 assert(reg < 32)），uvCount >= 32 或 idx >= 32 时 Debug 构建崩溃。
        // 改用 writeByte（无 assert），合法范围 0-255 已由上方 >= 256 检查保证。
        chunk_->writeByte(uvCount, line);
        for (uint8_t i = 0; i < uvCount; ++i) {
            size_t base = 3 + i * 2;
            if (base + 1 >= instr.operands.size())
                return false;
            if (instr.operands[base].index >= 256) {
                Logger::Error("RegisterBytecodeBackend: MAKE_CLOSURE isLocal 超出 255 上限 (" +
                                  std::to_string(instr.operands[base].index) + ")",
                              "RegIR");
                hasError_ = true;
                return false;
            }
            if (instr.operands[base + 1].index >= 256) {
                Logger::Error("RegisterBytecodeBackend: MAKE_CLOSURE upvalue idx 超出 255 上限 (" +
                                  std::to_string(instr.operands[base + 1].index) + ")",
                              "RegIR");
                hasError_ = true;
                return false;
            }
            chunk_->writeByte(static_cast<uint8_t>(instr.operands[base].index), line);     // isLocal
            chunk_->writeByte(static_cast<uint8_t>(instr.operands[base + 1].index), line); // idx
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerArithOps — 算术类（ADD / SUB / MUL / DIV / MOD / NEGATE）
// ============================================================
bool RegisterBytecodeBackend::lowerArithOps(const IRInstruction& instr, const IRFunction& /*ir*/) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::ADD:
    case IROp::SUB:
    case IROp::MUL:
    case IROp::DIV:
    case IROp::MOD: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t s1 = vregToReg(instr.operands[1].index);
        uint8_t s2 = vregToReg(instr.operands[2].index);
        RegOp op = RegOp::REG_ADD;
        switch (instr.op) {
        case IROp::ADD:
            op = RegOp::REG_ADD;
            break;
        case IROp::SUB:
            op = RegOp::REG_SUB;
            break;
        case IROp::MUL:
            op = RegOp::REG_MUL;
            break;
        case IROp::DIV:
            op = RegOp::REG_DIV;
            break;
        case IROp::MOD:
            op = RegOp::REG_MOD;
            break;
        default:
            break;
        }
        chunk_->writeOp(op, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(s1, line);
        chunk_->writeReg(s2, line);
        break;
    }
    case IROp::NEGATE: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_NEGATE, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerCompareOps — 比较类（EQ / NEQ / LT / GT / LTE / GTE）
// ============================================================
bool RegisterBytecodeBackend::lowerCompareOps(const IRInstruction& instr, const IRFunction& /*ir*/) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::EQ:
    case IROp::NEQ:
    case IROp::LT:
    case IROp::GT:
    case IROp::LTE:
    case IROp::GTE: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t s1 = vregToReg(instr.operands[1].index);
        uint8_t s2 = vregToReg(instr.operands[2].index);
        RegOp op = RegOp::REG_EQ;
        switch (instr.op) {
        case IROp::EQ:
            op = RegOp::REG_EQ;
            break;
        case IROp::NEQ:
            op = RegOp::REG_NEQ;
            break;
        case IROp::LT:
            op = RegOp::REG_LT;
            break;
        case IROp::GT:
            op = RegOp::REG_GT;
            break;
        case IROp::LTE:
            op = RegOp::REG_LTE;
            break;
        case IROp::GTE:
            op = RegOp::REG_GTE;
            break;
        default:
            break;
        }
        chunk_->writeOp(op, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(s1, line);
        chunk_->writeReg(s2, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerLogicOps — 逻辑类（NOT）
// ------------------------------------------------------------
// 注：AND/OR 已在 AST 层展开为分支（短路语义），IR 层只有 NOT。
// ============================================================
bool RegisterBytecodeBackend::lowerLogicOps(const IRInstruction& instr, const IRFunction& /*ir*/) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::NOT: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_NOT, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerControlOps — 控制流类（LABEL / JUMP / JUMP_IF_FALSE / RETURN / RETURN_NULL）
// ============================================================
bool RegisterBytecodeBackend::lowerControlOps(const IRInstruction& instr, const IRFunction& /*ir*/) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::LABEL: {
        if (instr.operands.empty())
            return false;
        labelToOffset_[instr.operands[0].index] = chunk_->code.size();
        break; // LABEL 不产生字节码
    }
    case IROp::JUMP: {
        if (instr.operands.empty())
            return false;
        chunk_->writeOp(RegOp::REG_JUMP, line);
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index});
        chunk_->writeShort(0, line); // 占位符
        break;
    }
    case IROp::JUMP_IF_FALSE: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t src = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_JUMP_IF_FALSE, line);
        chunk_->writeReg(src, line);
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[1].index});
        chunk_->writeShort(0, line); // 占位符
        break;
    }
    case IROp::RETURN: {
        if (instr.operands.empty())
            return false;
        uint8_t src = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_RETURN, line);
        chunk_->writeReg(src, line);
        break;
    }
    case IROp::RETURN_NULL: {
        chunk_->writeOp(RegOp::REG_RETURN_NULL, line);
        break;
    }
    // R164 协程/生成器：YIELD dest, src → REG_YIELD dst, src（3 字节）
    // 语义：RegisterVM 执行 REG_YIELD 时比较运行时计数器与目标 yieldId，
    //   - 命中目标：抛 VMYieldSignal 返回 src 寄存器值
    //   - 未命中：dst = src，继续执行函数体
    case IROp::YIELD: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_YIELD, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }
    // 七特性 MVP 阶段 4：AWAIT dest, src → REG_AWAIT dst, src（3 字节）
    // 语义：RegisterVM 执行 REG_AWAIT 时：src 非协程 → dst = src；
    //   src 为协程 → 驱动到 done，dst = 最后一次 next() 的值。
    case IROp::AWAIT: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_AWAIT, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerCallOps — 调用类（CALL / CALL_EXPR / METHOD_CALL / SUPER_CALL）
// ============================================================
bool RegisterBytecodeBackend::lowerCallOps(const IRInstruction& instr, const IRFunction& ir) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::CALL: {
        // IR: CALL dest, fun_name_idx, arg_count, arg1, ...
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[1].index), ir);
        // 参数数量上限检查：CALL 无隐式 this，上限 255
        if (instr.operands[2].index > 255) {
            Logger::Error("RegisterBytecodeBackend: CALL 参数数量超过 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t argCount = static_cast<uint8_t>(instr.operands[2].index & 0xFF);
        chunk_->writeOp(RegOp::REG_CALL, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(nameIdx, line);
        // AUDIT-BUG-C1 fix: argCount 是数量操作数（合法 0-255），不是寄存器号。
        // 原用 writeReg（含 assert(reg < 32)），argCount >= 32 时 Debug 构建崩溃。
        // 改用 writeByte（无 assert），与 MAKE_CLOSURE:504 一致。
        chunk_->writeByte(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (3 + i >= instr.operands.size())
                return false;
            uint8_t argReg = vregToReg(instr.operands[3 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }
    case IROp::TAIL_CALL: {
        // L18 eng-tailcall: 布局同 CALL，lower 为 REG_TAIL_CALL（后随 IROp::RETURN
        // lower 为 REG_RETURN dst，供降级路径使用）。
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[1].index), ir);
        if (instr.operands[2].index > 255) {
            Logger::Error("RegisterBytecodeBackend: TAIL_CALL 参数数量超过 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t argCount = static_cast<uint8_t>(instr.operands[2].index & 0xFF);
        chunk_->writeOp(RegOp::REG_TAIL_CALL, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(nameIdx, line);
        chunk_->writeByte(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (3 + i >= instr.operands.size())
                return false;
            uint8_t argReg = vregToReg(instr.operands[3 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }
    case IROp::CALL_EXPR: {
        // IR: CALL_EXPR dest, closure_vreg, arg_count, arg1, ...
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t callee = vregToReg(instr.operands[1].index);
        if (instr.operands[2].index > 255) {
            Logger::Error("RegisterBytecodeBackend: CALL_EXPR 参数数量超过 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t argCount = static_cast<uint8_t>(instr.operands[2].index & 0xFF);
        chunk_->writeOp(RegOp::REG_CALL_EXPR, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(callee, line);
        // AUDIT-BUG-C1 fix: argCount 是数量操作数，改用 writeByte（详见 MAKE_CLOSURE:504）。
        chunk_->writeByte(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (3 + i >= instr.operands.size())
                return false;
            uint8_t argReg = vregToReg(instr.operands[3 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }
    case IROp::METHOD_CALL: {
        // IR: METHOD_CALL dest, obj, method_idx, arg_count, args...
        if (instr.operands.size() < 4)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t obj = vregToReg(instr.operands[1].index);
        uint16_t methodIdx = addStringConstant(globalName(ir, instr.operands[2].index), ir);
        // 参数数量上限检查：METHOD_CALL 运行时 +1（this），上限 254
        if (instr.operands[3].index > 254) {
            Logger::Error("RegisterBytecodeBackend: METHOD_CALL 参数数量超过 254 上限（含 this 共 255）", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t argCount = static_cast<uint8_t>(instr.operands[3].index & 0xFF);
        chunk_->writeOp(RegOp::REG_METHOD_CALL, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(obj, line);
        chunk_->writeShort(methodIdx, line);
        // AUDIT-BUG-C1 fix: argCount 是数量操作数，改用 writeByte（详见 MAKE_CLOSURE:504）。
        chunk_->writeByte(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (4 + i >= instr.operands.size())
                return false;
            uint8_t argReg = vregToReg(instr.operands[4 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }
    case IROp::SUPER_CALL: {
        // P1-3 fix: IR: SUPER_CALL dest, this_vreg, method_idx, class_idx, arg_count, args...
        // → REG_SUPER_CALL: op + dst(1B) + nameIdx(2B) + argCount(1B) + recvReg(1B) + classIdx(2B) + args...
        if (instr.operands.size() < 5)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t recvReg = vregToReg(instr.operands[1].index);
        uint16_t methodIdx = addStringConstant(globalName(ir, instr.operands[2].index), ir);
        uint16_t classIdx = addStringConstant(globalName(ir, instr.operands[3].index), ir);
        if (instr.operands[4].index > 254) {
            Logger::Error("RegisterBytecodeBackend: SUPER_CALL 参数数量超过 254 上限（含 this 共 255）", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t argCount = static_cast<uint8_t>(instr.operands[4].index & 0xFF);
        chunk_->writeOp(RegOp::REG_SUPER_CALL, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(methodIdx, line);
        // AUDIT-BUG-C1 fix: argCount 是数量操作数，改用 writeByte（详见 MAKE_CLOSURE:504）。
        chunk_->writeByte(argCount, line);
        chunk_->writeReg(recvReg, line);
        chunk_->writeShort(classIdx, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (5 + i >= instr.operands.size())
                return false;
            uint8_t argReg = vregToReg(instr.operands[5 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerContainerOps — 容器 / 成员访问类（thin dispatcher）
// R133-A 重构：原 203 行单 switch 拆为 thin dispatcher + 4 个 helper，按操作类型分组。
// 拆分模式与 R118 StackVM executeContainerOps 同构（BUILD/INDEX/ENUM_QUERY/MEMBER 四类），
// 这是 R118/R120 模式在 IR→RegBytecode lowering 层的跨层迁移。
// helper 签名与原函数一致（const IRInstruction&, const IRFunction&），共享状态全成员
// （chunk_ / hasError_ / vregToReg / addStringConstant / globalName），传递成本为零。
// ============================================================
bool RegisterBytecodeBackend::lowerContainerOps(const IRInstruction& instr, const IRFunction& ir) {
    switch (instr.op) {
    case IROp::BUILD_ARRAY:
    case IROp::BUILD_DICT:
    case IROp::BUILD_TUPLE:
    case IROp::BUILD_ENUM_VARIANT:
        return lowerContainerBuildOps(instr, ir);
    case IROp::INDEX_GET:
    case IROp::INDEX_SET:
    // R133: LEN 与 INDEX_GET 同属"容器元素/属性访问"语义族
    case IROp::LEN:
        return lowerContainerIndexOps(instr, ir);
    case IROp::ENUM_VARIANT_NAME:
    case IROp::ENUM_VARIANT_FIELD:
        return lowerContainerEnumQueryOps(instr, ir);
    case IROp::MEMBER_GET:
    case IROp::SUPER_MEMBER_GET:
    case IROp::MEMBER_SET:
    case IROp::MEMBER_SET_LOCAL: // L7 fix
        return lowerContainerMemberOps(instr, ir);
    default:
        return false;
    }
}

// ============================================================
// lowerContainerBuildOps — 容器构造指令 lowering
// BUILD_ARRAY / BUILD_DICT / BUILD_TUPLE / BUILD_ENUM_VARIANT
// 与 R118/R120 StackVM/RegisterVM executeContainerBuildOps/executeArrayBuildOps 同构
// ============================================================
bool RegisterBytecodeBackend::lowerContainerBuildOps(const IRInstruction& instr, const IRFunction& /*ir*/) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::BUILD_ARRAY: {
        // IR: BUILD_ARRAY dest, count, arg1, arg2, ...
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        if (instr.operands[1].index > 255) {
            Logger::Error("RegisterBytecodeBackend: BUILD_ARRAY 元素数量超过 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t count = static_cast<uint8_t>(instr.operands[1].index & 0xFF);
        chunk_->writeOp(RegOp::REG_BUILD_ARRAY, line);
        chunk_->writeReg(dst, line);
        // AUDIT-BUG-C1 fix: count 是数量操作数，改用 writeByte（详见 MAKE_CLOSURE:504）。
        chunk_->writeByte(count, line);
        for (uint8_t i = 0; i < count; ++i) {
            if (2 + i >= instr.operands.size())
                return false;
            uint8_t elemReg = vregToReg(instr.operands[2 + i].index);
            chunk_->writeReg(elemReg, line);
        }
        break;
    }
    case IROp::BUILD_DICT: {
        // IR: BUILD_DICT dest, pair_count, k1, v1, k2, v2, ...
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        if (instr.operands[1].index > 255) {
            Logger::Error("RegisterBytecodeBackend: BUILD_DICT 键值对数量超过 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t pairCount = static_cast<uint8_t>(instr.operands[1].index & 0xFF);
        chunk_->writeOp(RegOp::REG_BUILD_DICT, line);
        chunk_->writeReg(dst, line);
        // AUDIT-BUG-C1 fix: pairCount 是数量操作数，改用 writeByte（详见 MAKE_CLOSURE:504）。
        chunk_->writeByte(pairCount, line);
        for (uint8_t i = 0; i < pairCount; ++i) {
            size_t kBase = 2 + i * 2;
            if (kBase + 1 >= instr.operands.size())
                return false;
            chunk_->writeReg(vregToReg(instr.operands[kBase].index), line);     // key
            chunk_->writeReg(vregToReg(instr.operands[kBase + 1].index), line); // value
        }
        break;
    }
    // R98 元组与解构：BUILD_TUPLE → REG_BUILD_TUPLE lowering
    case IROp::BUILD_TUPLE: {
        // IR: BUILD_TUPLE dest, count, arg1, arg2, ...
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        if (instr.operands[1].index > 255) {
            Logger::Error("RegisterBytecodeBackend: BUILD_TUPLE 元素数量超过 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t count = static_cast<uint8_t>(instr.operands[1].index & 0xFF);
        chunk_->writeOp(RegOp::REG_BUILD_TUPLE, line);
        chunk_->writeReg(dst, line);
        // AUDIT-BUG-C1 fix: count 是数量操作数，改用 writeByte。
        chunk_->writeByte(count, line);
        for (uint8_t i = 0; i < count; ++i) {
            if (2 + i >= instr.operands.size())
                return false;
            uint8_t elemReg = vregToReg(instr.operands[2 + i].index);
            chunk_->writeReg(elemReg, line);
        }
        break;
    }
    // R99 枚举与 ADT：BUILD_ENUM_VARIANT → REG_BUILD_ENUM_VARIANT lowering
    // IR: [dest_vreg, enumNameConstIdx, variantNameConstIdx, argCount, arg1, arg2, ...]
    // RegBytecode: op + dst + enumNameIdx(2B) + variantNameIdx(2B) + argCount(1B) + argRegs...
    case IROp::BUILD_ENUM_VARIANT: {
        if (instr.operands.size() < 4)
            return false;
        if (instr.operands[3].index > 255) {
            Logger::Error("RegisterBytecodeBackend: BUILD_ENUM_VARIANT argCount 超出 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t enumNameIdx = static_cast<uint16_t>(instr.operands[1].index);
        uint16_t variantNameIdx = static_cast<uint16_t>(instr.operands[2].index);
        uint8_t argCount = static_cast<uint8_t>(instr.operands[3].index & 0xFF);
        chunk_->writeOp(RegOp::REG_BUILD_ENUM_VARIANT, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(enumNameIdx, line);
        chunk_->writeShort(variantNameIdx, line);
        chunk_->writeByte(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (4 + i >= instr.operands.size())
                return false;
            uint8_t argReg = vregToReg(instr.operands[4 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerContainerIndexOps — 索引访问指令 lowering
// INDEX_GET / INDEX_SET
// ============================================================
bool RegisterBytecodeBackend::lowerContainerIndexOps(const IRInstruction& instr, const IRFunction& ir) {
    (void)ir; // 暂未使用 IRFunction，保留参数签名与原函数一致
    int line = instr.line;
    switch (instr.op) {
    case IROp::INDEX_GET: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t obj = vregToReg(instr.operands[1].index);
        uint8_t idx = vregToReg(instr.operands[2].index);
        chunk_->writeOp(RegOp::REG_INDEX_GET, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(obj, line);
        chunk_->writeReg(idx, line);
        break;
    }
    case IROp::INDEX_SET: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t obj = vregToReg(instr.operands[0].index);
        uint8_t idx = vregToReg(instr.operands[1].index);
        uint8_t val = vregToReg(instr.operands[2].index);
        chunk_->writeOp(RegOp::REG_INDEX_SET, line);
        chunk_->writeReg(obj, line);
        chunk_->writeReg(idx, line);
        chunk_->writeReg(val, line);
        break;
    }
    // R133 模式匹配扩展：LEN → REG_LEN lowering
    // IR: [dest_vreg, src_vreg]  RegBytecode: op + dst + src
    case IROp::LEN: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_LEN, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerContainerEnumQueryOps — 枚举 variant 查询指令 lowering
// ENUM_VARIANT_NAME / ENUM_VARIANT_FIELD（与 R118/R120 StackVM/RegisterVM executeEnumOps 同构）
// ============================================================
bool RegisterBytecodeBackend::lowerContainerEnumQueryOps(const IRInstruction& instr, const IRFunction& ir) {
    (void)ir; // 暂未使用 IRFunction，保留参数签名与原函数一致
    int line = instr.line;
    switch (instr.op) {
    // R99 枚举与 ADT：ENUM_VARIANT_NAME → REG_ENUM_VARIANT_NAME lowering
    // IR: [dest_bool, scrut_vreg, enumNameConstIdx, variantNameConstIdx]
    // RegBytecode: op + dst + scrut + enumNameIdx(2B) + variantNameIdx(2B)
    case IROp::ENUM_VARIANT_NAME: {
        if (instr.operands.size() < 4)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t scrut = vregToReg(instr.operands[1].index);
        uint16_t enumNameIdx = static_cast<uint16_t>(instr.operands[2].index);
        uint16_t variantNameIdx = static_cast<uint16_t>(instr.operands[3].index);
        chunk_->writeOp(RegOp::REG_ENUM_VARIANT_NAME, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(scrut, line);
        chunk_->writeShort(enumNameIdx, line);
        chunk_->writeShort(variantNameIdx, line);
        break;
    }
    // R99 枚举与 ADT：ENUM_VARIANT_FIELD → REG_ENUM_VARIANT_FIELD lowering
    // IR: [dest, scrut_vreg, idx_vreg]
    // RegBytecode: op + dst + scrut + idx
    case IROp::ENUM_VARIANT_FIELD: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t scrut = vregToReg(instr.operands[1].index);
        uint8_t idx = vregToReg(instr.operands[2].index);
        chunk_->writeOp(RegOp::REG_ENUM_VARIANT_FIELD, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(scrut, line);
        chunk_->writeReg(idx, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerContainerMemberOps — 成员访问指令 lowering
// MEMBER_GET / SUPER_MEMBER_GET / MEMBER_SET
// ============================================================
bool RegisterBytecodeBackend::lowerContainerMemberOps(const IRInstruction& instr, const IRFunction& ir) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::MEMBER_GET: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t obj = vregToReg(instr.operands[1].index);
        uint16_t fieldIdx = addStringConstant(globalName(ir, instr.operands[2].index), ir);
        chunk_->writeOp(RegOp::REG_MEMBER_GET, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(obj, line);
        chunk_->writeShort(fieldIdx, line);
        break;
    }
    case IROp::SUPER_MEMBER_GET: {
        // P1-2 fix: super.field → REG_SUPER_MEMBER_GET（运行时与 MEMBER_GET 同语义，
        // 字段已由父类 init 设置到实例上，但保留独立 op 以便未来语义扩展）
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t obj = vregToReg(instr.operands[1].index);
        uint16_t fieldIdx = addStringConstant(globalName(ir, instr.operands[2].index), ir);
        chunk_->writeOp(RegOp::REG_SUPER_MEMBER_GET, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(obj, line);
        chunk_->writeShort(fieldIdx, line);
        break;
    }
    case IROp::MEMBER_SET: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t obj = vregToReg(instr.operands[0].index);
        uint16_t fieldIdx = addStringConstant(globalName(ir, instr.operands[1].index), ir);
        uint8_t val = vregToReg(instr.operands[2].index);
        chunk_->writeOp(RegOp::REG_MEMBER_SET, line);
        chunk_->writeReg(obj, line);
        chunk_->writeShort(fieldIdx, line);
        chunk_->writeReg(val, line);
        break;
    }
    case IROp::MEMBER_SET_LOCAL: {
        // L7 fix: 方法体内 this.field = val → REG_MEMBER_SET reg(slot), field, val
        // 直接用 slot 作为 objReg（RegisterVM slot N → reg N），修改 reg(slot) in place，
        // 避免 vreg 副本导致 COW detach 后原 reg 不变。
        if (instr.operands.size() < 3)
            return false;
        if (instr.operands[0].index >= 32) {
            Logger::Error("RegisterBytecodeBackend: MEMBER_SET_LOCAL slot 超出 32 寄存器上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "RegBackend");
            return false;
        }
        uint8_t objReg = static_cast<uint8_t>(instr.operands[0].index); // slot = reg
        uint16_t fieldIdx = addStringConstant(globalName(ir, instr.operands[1].index), ir);
        uint8_t val = vregToReg(instr.operands[2].index);
        chunk_->writeOp(RegOp::REG_MEMBER_SET, line);
        chunk_->writeReg(objReg, line);
        chunk_->writeShort(fieldIdx, line);
        chunk_->writeReg(val, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerClassOps — 类相关（DEFINE_CLASS / CLASS_NEW / INIT_FIELD）
// ============================================================
bool RegisterBytecodeBackend::lowerClassOps(const IRInstruction& instr, const IRFunction& ir) {
    int line = instr.line;
    switch (instr.op) {
    case IROp::DEFINE_CLASS: {
        // C-9 fix: 携带完整类元数据（父类、字段顺序、方法名→函数名映射）
        // BUG-INH-1 fix: 新增字段默认值常量索引
        // BUG-INH-IR-1 fix: 新增字段表达式临时局部变量槽位（=寄存器号）
        // 操作数布局（见 IR.cpp visitClassDecl）：
        //   [0] className (FUNC_NAME)
        //   [1] parentName (IMM_UINT, UINT32_MAX=无父类)
        //   [2] fieldCount (IMM_UINT)
        //   [3+i*3] field name (FIELD_NAME)
        //   [3+i*3+1] fieldDefaultConstIdx (IMM_UINT, UINT32_MAX=null/无默认值/有表达式)
        //   [3+i*3+2] fieldExprLocalSlot (IMM_UINT, UINT32_MAX=使用常量或null, 否则使用临时 local slot)
        //   [3+3F] methodCount (IMM_UINT)
        //   [3+3F+1 .. ] (methodName FIELD_NAME, funName FUNC_NAME) × M
        if (instr.operands.size() < 3)
            return false;
        uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[0].index), ir);

        uint32_t parentRaw = instr.operands[1].index;
        uint16_t parentIdx =
            (parentRaw == UINT32_MAX) ? RuntimeLimits::NO_INDEX : addStringConstant(globalName(ir, parentRaw), ir);

        uint32_t fieldCount = instr.operands[2].index;
        if (instr.operands.size() < 3 + fieldCount * 3 + 1)
            return false;
        uint32_t methodCount = instr.operands[3 + fieldCount * 3].index;
        if (instr.operands.size() < 3 + fieldCount * 3 + 1 + methodCount * 2)
            return false;
        // R7 fix: fieldCount/methodCount 经 writeByte 编码为 uint8_t，超 255 时静默截断
        // 会导致后续读取循环用截断值迭代，字段名/方法名索引完全错位，写出损坏字节码。
        if (fieldCount > 255) {
            Logger::Error("RegisterBytecodeBackend: DEFINE_CLASS fieldCount 超过 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }
        if (methodCount > 255) {
            Logger::Error("RegisterBytecodeBackend: DEFINE_CLASS methodCount 超过 255 上限", "RegIR");
            hasError_ = true;
            return false;
        }

        chunk_->writeOp(RegOp::REG_DEFINE_CLASS, line);
        chunk_->writeShort(nameIdx, line);
        chunk_->writeShort(parentIdx, line);
        chunk_->writeByte(static_cast<uint8_t>(fieldCount), line);
        for (uint32_t i = 0; i < fieldCount; ++i) {
            size_t nameOpIdx = 3 + i * 3;
            size_t defaultOpIdx = 3 + i * 3 + 1;
            size_t exprSlotOpIdx = 3 + i * 3 + 2;
            uint16_t fIdx = addStringConstant(globalName(ir, instr.operands[nameOpIdx].index), ir);
            chunk_->writeShort(fIdx, line);
            // BUG-INH-1 fix: 编码字段默认值常量索引（UINT32_MAX → NO_INDEX 表示 null）
            // BUG-INH-IR-1 fix: 若有非字面量表达式，default const 写 NO_INDEX，
            //   额外编码 1 字节 exprReg（= local slot 号，RegisterVM 中 local slot = register）
            uint32_t defaultConstIdx = instr.operands[defaultOpIdx].index;
            uint32_t exprSlot = instr.operands[exprSlotOpIdx].index;
            if (exprSlot != UINT32_MAX) {
                // 非字面量表达式：default const 写 NO_INDEX，exprReg 写 slot 号
                if (exprSlot >= 32) {
                    Logger::Error("RegisterBytecodeBackend: DEFINE_CLASS 字段临时局部变量槽位超出 32 寄存器上限",
                                  "RegIR");
                    hasError_ = true;
                    return false;
                }
                chunk_->writeShort(RuntimeLimits::NO_INDEX, line);
                chunk_->writeByte(static_cast<uint8_t>(exprSlot), line);
            } else if (defaultConstIdx == UINT32_MAX) {
                chunk_->writeShort(RuntimeLimits::NO_INDEX, line);
                chunk_->writeByte(RuntimeLimits::NO_SLOT, line); // NO_SLOT = 无表达式寄存器
            } else {
                if (defaultConstIdx >= ir.constants.size()) {
                    Logger::Error("RegisterBytecodeBackend: DEFINE_CLASS 字段默认值常量索引越界", "RegIR");
                    hasError_ = true;
                    return false;
                }
                const Value& defaultVal = ir.constants[defaultConstIdx];
                uint16_t constIdx = chunk_->addConstant(defaultVal);
                chunk_->writeShort(constIdx, line);
                chunk_->writeByte(RuntimeLimits::NO_SLOT, line); // NO_SLOT = 无表达式寄存器
            }
        }
        chunk_->writeByte(static_cast<uint8_t>(methodCount), line);
        for (uint32_t i = 0; i < methodCount; ++i) {
            size_t base = 3 + fieldCount * 3 + 1 + i * 2;
            uint16_t mIdx = addStringConstant(globalName(ir, instr.operands[base].index), ir);
            uint16_t fIdx = addStringConstant(globalName(ir, instr.operands[base + 1].index), ir);
            chunk_->writeShort(mIdx, line);
            chunk_->writeShort(fIdx, line);
        }
        break;
    }
    case IROp::CLASS_NEW: {
        // IR: CLASS_NEW dest, name_idx, arg_count, args...
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[1].index), ir);
        if (instr.operands[2].index > 254) {
            Logger::Error("RegisterBytecodeBackend: CLASS_NEW 参数数量超过 254 上限（含 instance 共 255）", "RegIR");
            hasError_ = true;
            return false;
        }
        uint8_t argCount = static_cast<uint8_t>(instr.operands[2].index & 0xFF);
        chunk_->writeOp(RegOp::REG_CLASS_NEW, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(nameIdx, line);
        // AUDIT-BUG-C1 fix: argCount 是数量操作数，改用 writeByte（详见 MAKE_CLOSURE:504）。
        chunk_->writeByte(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (3 + i >= instr.operands.size())
                return false;
            chunk_->writeReg(vregToReg(instr.operands[3 + i].index), line);
        }
        break;
    }
    case IROp::INIT_FIELD: {
        if (instr.operands.empty())
            return false;
        uint16_t fieldIdx = addStringConstant(globalName(ir, instr.operands[0].index), ir);
        chunk_->writeOp(RegOp::REG_INIT_FIELD, line);
        chunk_->writeShort(fieldIdx, line);
        break;
    }
    default:
        return false;
    }
    return true;
}

// ============================================================
// lowerMiscOps — 异常 / 写回 / 其他（TRY_*/THROW/LOAD_EXCEPTION/
//                PUSH_JUMP_TARGET/FINALLY_END/WRITEBACK_*/PRINT/POP/DUP/
//                LOAD_MUTATED/TYPE_CHECK）
// ============================================================
bool RegisterBytecodeBackend::lowerMiscOps(const IRInstruction& instr, const IRFunction& ir) {
    int line = instr.line;
    switch (instr.op) {
    // ---- 异常 ----
    case IROp::TRY_BEGIN: {
        if (instr.operands.empty())
            return false;
        chunk_->writeOp(RegOp::REG_TRY_BEGIN, line);
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index});
        chunk_->writeShort(0, line); // 占位符
        break;
    }
    case IROp::TRY_END: {
        chunk_->writeOp(RegOp::REG_TRY_END, line);
        break;
    }
    case IROp::THROW: {
        if (instr.operands.empty())
            return false;
        uint8_t src = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_THROW, line);
        chunk_->writeReg(src, line);
        break;
    }
    case IROp::LOAD_EXCEPTION: {
        // P1-4 fix: catch 块起始加载 pendingException_ 到寄存器
        if (instr.operands.empty())
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_EXCEPTION, line);
        chunk_->writeReg(dst, line);
        break;
    }
    // AUDIT-P1.1 fix: break/continue finally 续跳 lowering
    case IROp::PUSH_JUMP_TARGET: {
        // operands: [label_idx]
        // → REG_PUSH_JUMP_TARGET target(2B 绝对偏移，第二遍回填)
        if (instr.operands.empty())
            return false;
        chunk_->writeOp(RegOp::REG_PUSH_JUMP_TARGET, line);
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index});
        chunk_->writeShort(0, line); // 占位符，第二遍回填
        break;
    }
    case IROp::FINALLY_END: {
        // 无操作数 → REG_FINALLY_END
        chunk_->writeOp(RegOp::REG_FINALLY_END, line);
        break;
    }

    // ---- 写回 ----
    case IROp::WRITEBACK_MEMBER_VAR: {
        if (instr.operands.size() < 2)
            return false;
        chunk_->writeOp(RegOp::REG_WRITEBACK_MEMBER_VAR, line);
        // C-1 fix: 区分 GLOBAL_SLOT(IMM_UINT) 和 GLOBAL_NAME，与 LOAD_GLOBAL 一致用高 bit 标记
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->writeShort(static_cast<uint16_t>(instr.operands[0].index), line);
        } else {
            uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[0].index), ir);
            chunk_->writeShort(nameIdx | 0x8000, line);
        }
        chunk_->writeShort(addStringConstant(globalName(ir, instr.operands[1].index), ir), line);
        break;
    }
    case IROp::WRITEBACK_MEMBER_LOCAL: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].index >= 32) {
            Logger::Error("RegBytecodeBackend: WRITEBACK_MEMBER_LOCAL local slot 超出 32 寄存器上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "RegBackend");
            return false;
        }
        chunk_->writeOp(RegOp::REG_WRITEBACK_MEMBER_LOCAL, line);
        chunk_->writeReg(static_cast<uint8_t>(instr.operands[0].index), line);
        chunk_->writeShort(addStringConstant(globalName(ir, instr.operands[1].index), ir), line);
        break;
    }
    case IROp::WRITEBACK_INDEX_VAR: {
        if (instr.operands.empty())
            return false;
        chunk_->writeOp(RegOp::REG_WRITEBACK_INDEX_VAR, line);
        // C-1 fix: 区分 GLOBAL_SLOT(IMM_UINT) 和 GLOBAL_NAME，与 LOAD_GLOBAL 一致用高 bit 标记
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->writeShort(static_cast<uint16_t>(instr.operands[0].index), line);
        } else {
            uint16_t nameIdx = addStringConstant(globalName(ir, instr.operands[0].index), ir);
            chunk_->writeShort(nameIdx | 0x8000, line);
        }
        break;
    }
    case IROp::WRITEBACK_INDEX_LOCAL: {
        if (instr.operands.empty())
            return false;
        if (instr.operands[0].index >= 32) {
            Logger::Error("RegBytecodeBackend: WRITEBACK_INDEX_LOCAL local slot 超出 32 寄存器上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "RegBackend");
            return false;
        }
        chunk_->writeOp(RegOp::REG_WRITEBACK_INDEX_LOCAL, line);
        chunk_->writeReg(static_cast<uint8_t>(instr.operands[0].index), line);
        break;
    }
    case IROp::WRITEBACK_MEMBER_UPVALUE: {
        // #7 fix: 寄存器式 upvalue 写回（uvIdx 在运行时由 frame.upvalues 解释，不映射到寄存器）
        if (instr.operands.size() < 2)
            return false;
        // Bug-14 同型修复：uvIdx 超 255 静默截断
        if (instr.operands[0].index >= 256) {
            Logger::Error("RegBytecodeBackend: WRITEBACK_MEMBER_UPVALUE upvalue 索引超出 255 上限 (uvIdx=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "RegBackend");
            return false;
        }
        chunk_->writeOp(RegOp::REG_WRITEBACK_MEMBER_UPVALUE, line);
        chunk_->writeByte(static_cast<uint8_t>(instr.operands[0].index), line);
        chunk_->writeShort(addStringConstant(globalName(ir, instr.operands[1].index), ir), line);
        break;
    }
    case IROp::WRITEBACK_INDEX_UPVALUE: {
        if (instr.operands.empty())
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error("RegBytecodeBackend: WRITEBACK_INDEX_UPVALUE upvalue 索引超出 255 上限 (uvIdx=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "RegBackend");
            return false;
        }
        chunk_->writeOp(RegOp::REG_WRITEBACK_INDEX_UPVALUE, line);
        chunk_->writeByte(static_cast<uint8_t>(instr.operands[0].index), line);
        break;
    }

    // ---- 其他 ----
    case IROp::PRINT: {
        if (instr.operands.empty())
            return false;
        uint8_t src = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_PRINT, line);
        chunk_->writeReg(src, line);
        break;
    }
    case IROp::POP: {
        // 寄存器式中无栈，POP 是 no-op
        break;
    }
    case IROp::DUP: {
        // IR: DUP dest, src → REG_MOVE dest, src
        if (instr.operands.size() < 2)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_MOVE, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }
    case IROp::LOAD_MUTATED: {
        // MEDIUM-1/2 fix: dest = reg(lastMutatedReceiverReg_)
        if (instr.operands.empty())
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_MUTATED, line);
        chunk_->writeReg(dst, line);
        break;
    }

    // 2026-06-29: 类型注解运行时检查
    // operands: [src_vreg, type_const_idx]
    case IROp::TYPE_CHECK: {
        if (instr.operands.size() < 2)
            return false;
        uint8_t src = vregToReg(instr.operands[0].index);
        uint16_t typeIdx = static_cast<uint16_t>(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_TYPE_CHECK, line);
        chunk_->writeReg(src, line);
        chunk_->writeShort(typeIdx, line);
        break;
    }
    // R133: 软类型测试（与 TYPE_CHECK 同语义但写 bool 到 dst 而非抛错）
    // operands: [dest_vreg, src_vreg, type_const_idx]
    case IROp::TYPE_TEST: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        uint16_t typeIdx = static_cast<uint16_t>(instr.operands[2].index);
        chunk_->writeOp(RegOp::REG_TYPE_TEST, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        chunk_->writeShort(typeIdx, line);
        break;
    }

    default:
        Logger::Error("RegisterBytecodeBackend: 未支持的 IR 指令 " + std::to_string(static_cast<int>(instr.op)),
                      "RegIR");
        return false;
    }
    return true;
}

bool RegisterBytecodeBackend::patchJumps() {
    for (const auto& pj : pendingJumps_) {
        auto it = labelToOffset_.find(pj.targetLabel);
        if (it == labelToOffset_.end()) {
            Logger::Error("RegisterBytecodeBackend: 未找到标签 " + std::to_string(pj.targetLabel), "RegIR");
            return false;
        }
        // 与 BytecodeIRBackend::patchJumps 对齐：字节码体积超过 64KB 时跳转目标截断，
        // 会导致 RegisterVM 跳到错误地址执行任意指令。
        if (it->second > 65535) {
            Logger::Error("RegisterBytecodeBackend: 跳转目标偏移超过 64KB 限制 (offset=" + std::to_string(it->second) +
                              ", label=" + std::to_string(pj.targetLabel) + ")",
                          "RegIR");
            return false;
        }
        uint16_t target = static_cast<uint16_t>(it->second);
        if (pj.codeOffset + 1 >= chunk_->code.size()) {
            Logger::Error("RegisterBytecodeBackend: 跳转操作数越界", "RegIR");
            return false;
        }
        chunk_->code[pj.codeOffset] = static_cast<uint8_t>(target & 0xFF);
        chunk_->code[pj.codeOffset + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }
    return true;
}

bool RegisterBytecodeBackend::lowerModule(const IRModule& module) {
    resetState();

    // lower main 函数
    if (!module.mainFunction) {
        Logger::Error("RegisterBytecodeBackend: module 无 mainFunction", "RegIR");
        return false;
    }
    if (!lower(*module.mainFunction)) {
        return false;
    }

    // R103 W1 fix: mainFunction 的 chunk 保留在 chunk_ 中，由调用方 takeChunk() 获取，
    // 不再放入 functionChunks_。原实现把 mainChunk 放入 functionChunks_["main"]，
    // 当用户源码含 `fun main() {...}` 时，子函数 "main" 会覆盖 mainFunction 的 chunk，
    // 导致 compileViaRegisterIR 的 find("main") 拿到用户函数而非顶层代码，
    // RegVM 执行用户函数体而非顶层代码，输出为空。
    // 对齐 BytecodeIRBackend::lowerModule 的设计：mainFunction → chunk_，子函数 → functionChunks_。
    chunk_->writeOp(RegOp::REG_RETURN_NULL, 0);
    chunk_->buildIpMap();

    // lower 子函数（每个子函数独立 backend 实例，避免覆盖状态）
    for (const auto& fn : module.functions) {
        RegisterBytecodeBackend subBackend;
        if (!subBackend.lower(*fn)) {
            return false;
        }
        auto subChunk = subBackend.takeChunk();
        subChunk->writeOp(RegOp::REG_RETURN_NULL, 0);
        subChunk->buildIpMap();
        functionChunks_[fn->name] = std::move(*subChunk);
    }

    return true;
}