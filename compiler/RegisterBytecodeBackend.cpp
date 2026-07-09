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
#include "Logger.h"

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
    std::vector<uint32_t> pendingMethodCallObjs;
    for (const auto& block : ir.blocks) {
        pendingMethodCallObjs.clear();
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
    // 构建反向映射：instrIndex → 在此处最后使用的 vreg 列表
    for (const auto& kv : vregLastUse_) {
        lastUseToVregs_[kv.second].push_back(kv.first);
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
    return patchJumps();
}

bool RegisterBytecodeBackend::lowerInstruction(const IRInstruction& instr, const IRFunction& ir) {
    // lowerInstruction 是整个后端的"翻译核心"：把一条 SSA-like IR 指令映射为若干条
    // 寄存器式字节码。统一的写码约定（理解下方所有 case 的关键）：
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
    if (hasError_)
        return false;
    auto globalName = [&](uint32_t idx) -> std::string {
        return idx < ir.globalNames.size() ? ir.globalNames[idx] : std::string{};
    };

    int line = instr.line;
    switch (instr.op) {
    // ---- 常量加载 ----
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
            uint16_t nameIdx = addStringConstant(globalName(instr.operands[1].index), ir);
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
            uint16_t nameIdx = addStringConstant(globalName(instr.operands[0].index), ir);
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
            uint16_t nameIdx = addStringConstant(globalName(instr.operands[0].index), ir);
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
            nameIdx = addStringConstant(globalName(instr.operands[0].index), ir);
        }
        chunk_->writeOp(RegOp::REG_DELETE_GLOBAL, line);
        chunk_->writeShort(nameIdx, line);
        break;
    }

    // ---- upvalue ----
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

    // ---- 算术 ----
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

    // ---- 比较 ----
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

    // ---- 控制流 ----
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

    // ---- 调用 ----
    case IROp::CALL: {
        // IR: CALL dest, fun_name_idx, arg_count, arg1, ...
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t nameIdx = addStringConstant(globalName(instr.operands[1].index), ir);
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
        uint16_t methodIdx = addStringConstant(globalName(instr.operands[2].index), ir);
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
        uint16_t methodIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        uint16_t classIdx = addStringConstant(globalName(instr.operands[3].index), ir);
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

    // ---- 闭包 ----
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
        uint16_t nameIdx = addStringConstant(globalName(instr.operands[1].index), ir);
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

    // ---- 容器 ----
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

    // ---- 成员访问 ----
    case IROp::MEMBER_GET: {
        if (instr.operands.size() < 3)
            return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t obj = vregToReg(instr.operands[1].index);
        uint16_t fieldIdx = addStringConstant(globalName(instr.operands[2].index), ir);
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
        uint16_t fieldIdx = addStringConstant(globalName(instr.operands[2].index), ir);
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
        uint16_t fieldIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        uint8_t val = vregToReg(instr.operands[2].index);
        chunk_->writeOp(RegOp::REG_MEMBER_SET, line);
        chunk_->writeReg(obj, line);
        chunk_->writeShort(fieldIdx, line);
        chunk_->writeReg(val, line);
        break;
    }

    // ---- 类 ----
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
        uint16_t nameIdx = addStringConstant(globalName(instr.operands[0].index), ir);

        uint32_t parentRaw = instr.operands[1].index;
        uint16_t parentIdx = (parentRaw == UINT32_MAX) ? 0xFFFF : addStringConstant(globalName(parentRaw), ir);

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
            uint16_t fIdx = addStringConstant(globalName(instr.operands[nameOpIdx].index), ir);
            chunk_->writeShort(fIdx, line);
            // BUG-INH-1 fix: 编码字段默认值常量索引（UINT32_MAX → 0xFFFF 表示 null）
            // BUG-INH-IR-1 fix: 若有非字面量表达式，default const 写 0xFFFF，
            //   额外编码 1 字节 exprReg（= local slot 号，RegisterVM 中 local slot = register）
            uint32_t defaultConstIdx = instr.operands[defaultOpIdx].index;
            uint32_t exprSlot = instr.operands[exprSlotOpIdx].index;
            if (exprSlot != UINT32_MAX) {
                // 非字面量表达式：default const 写 0xFFFF，exprReg 写 slot 号
                if (exprSlot >= 32) {
                    Logger::Error("RegisterBytecodeBackend: DEFINE_CLASS 字段临时局部变量槽位超出 32 寄存器上限",
                                  "RegIR");
                    hasError_ = true;
                    return false;
                }
                chunk_->writeShort(0xFFFF, line);
                chunk_->writeByte(static_cast<uint8_t>(exprSlot), line);
            } else if (defaultConstIdx == UINT32_MAX) {
                chunk_->writeShort(0xFFFF, line);
                chunk_->writeByte(0xFF, line); // 0xFF = 无表达式寄存器
            } else {
                if (defaultConstIdx >= ir.constants.size()) {
                    Logger::Error("RegisterBytecodeBackend: DEFINE_CLASS 字段默认值常量索引越界", "RegIR");
                    hasError_ = true;
                    return false;
                }
                const Value& defaultVal = ir.constants[defaultConstIdx];
                uint16_t constIdx = chunk_->addConstant(defaultVal);
                chunk_->writeShort(constIdx, line);
                chunk_->writeByte(0xFF, line); // 0xFF = 无表达式寄存器
            }
        }
        chunk_->writeByte(static_cast<uint8_t>(methodCount), line);
        for (uint32_t i = 0; i < methodCount; ++i) {
            size_t base = 3 + fieldCount * 3 + 1 + i * 2;
            uint16_t mIdx = addStringConstant(globalName(instr.operands[base].index), ir);
            uint16_t fIdx = addStringConstant(globalName(instr.operands[base + 1].index), ir);
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
        uint16_t nameIdx = addStringConstant(globalName(instr.operands[1].index), ir);
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
        uint16_t fieldIdx = addStringConstant(globalName(instr.operands[0].index), ir);
        chunk_->writeOp(RegOp::REG_INIT_FIELD, line);
        chunk_->writeShort(fieldIdx, line);
        break;
    }

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
            uint16_t nameIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            chunk_->writeShort(nameIdx | 0x8000, line);
        }
        chunk_->writeShort(addStringConstant(globalName(instr.operands[1].index), ir), line);
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
        chunk_->writeShort(addStringConstant(globalName(instr.operands[1].index), ir), line);
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
            uint16_t nameIdx = addStringConstant(globalName(instr.operands[0].index), ir);
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
        chunk_->writeShort(addStringConstant(globalName(instr.operands[1].index), ir), line);
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
    auto mainChunk = takeChunk();
    if (!mainChunk)
        return false;

    // 末尾添加 RETURN_NULL
    mainChunk->writeOp(RegOp::REG_RETURN_NULL, 0);
    mainChunk->buildIpMap();

    functionChunks_[mainChunk->name] = std::move(*mainChunk);
    chunk_.reset();

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

    // 恢复 chunk_ 指向 main（供 takeChunk() 调用）
    // 注意：lowerModule 后 takeChunk() 返回空，调用方应使用 takeFunctionChunks()
    return true;
}
