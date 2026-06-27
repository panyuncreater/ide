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

RegisterBytecodeBackend::RegisterBytecodeBackend() = default;

void RegisterBytecodeBackend::resetState() {
    chunk_.reset();
    functionChunks_.clear();
    irToBytecodeOffset_.clear();
    vregToReg_.clear();
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
    if (it != vregToReg_.end()) return it->second;
    // vreg N → 寄存器 localCount + N
    int reg = chunk_->localCount + static_cast<int>(vreg);
    if (reg >= 32) {
        // 溢出：设置错误标志，返回 R0 作为占位（lower 末尾会因 hasError_ 失败，
        // 已 emit 的指令不会被执行）。不静默生成损坏的字节码。
        Logger::Error("RegisterBytecodeBackend: vreg " + std::to_string(vreg) +
                      " 映射到寄存器 " + std::to_string(reg) +
                      " 超出 32 上限（需减少局部变量/简化表达式或实现寄存器溢出）", "RegIR");
        hasError_ = true;
        return 0;  // 占位值，lower 会失败
    }
    uint8_t regByte = static_cast<uint8_t>(reg);
    vregToReg_[vreg] = regByte;
    if (reg + 1 > chunk_->registerCount) {
        chunk_->registerCount = reg + 1;
    }
    return regByte;
}

bool RegisterBytecodeBackend::lower(const IRFunction& ir) {
    chunk_ = std::make_unique<RegBytecodeChunk>();
    chunk_->name = ir.name;
    chunk_->arity = ir.arity;
    chunk_->requiredArity = ir.requiredArity;
    chunk_->localCount = ir.localCount;
    chunk_->registerCount = ir.localCount;  // 初始为 localCount，vreg 分配后增长
    chunk_->defaultConstIndices = ir.defaultConstIndices;
    chunk_->upvalues = ir.upvalues;

    // 复制常量池（保持索引一致）
    for (const auto& c : ir.constants) {
        chunk_->constants.push_back(c);
    }

    // 辅助：从 globalNames 取名
    auto globalName = [&](uint32_t idx) -> std::string {
        return idx < ir.globalNames.size() ? ir.globalNames[idx] : std::string{};
    };

    // 遍历所有基本块的所有指令，逐条 lowering
    size_t instrIndex = 0;
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            irToBytecodeOffset_.push_back({instrIndex, chunk_->code.size()});
            if (!lowerInstruction(instr, ir) || hasError_) {
                return false;  // lowering 失败或 vreg 溢出：不生成损坏的字节码
            }
            // 填充行号表
            while (static_cast<int>(chunk_->lines.size()) < static_cast<int>(chunk_->code.size())) {
                chunk_->lines.push_back(instr.line);
            }
            ++instrIndex;
        }
    }
    return patchJumps();
}

bool RegisterBytecodeBackend::lowerInstruction(const IRInstruction& instr, const IRFunction& ir) {
    auto globalName = [&](uint32_t idx) -> std::string {
        return idx < ir.globalNames.size() ? ir.globalNames[idx] : std::string{};
    };

    int line = instr.line;
    switch (instr.op) {
    // ---- 常量加载 ----
    case IROp::LOAD_CONST: {
        if (instr.operands.size() < 2) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t constIdx = static_cast<uint16_t>(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_LOAD_CONST, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(constIdx, line);
        break;
    }
    case IROp::LOAD_NULL: {
        if (instr.operands.empty()) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_NULL, line);
        chunk_->writeReg(dst, line);
        break;
    }
    case IROp::LOAD_TRUE: {
        if (instr.operands.empty()) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_TRUE, line);
        chunk_->writeReg(dst, line);
        break;
    }
    case IROp::LOAD_FALSE: {
        if (instr.operands.empty()) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_FALSE, line);
        chunk_->writeReg(dst, line);
        break;
    }

    // ---- 局部变量（在寄存器式中，local slot 即寄存器）----
    case IROp::LOAD_LOCAL: {
        // IR: LOAD_LOCAL dest_vreg, slot
        // → REG_MOVE dest_vreg_reg, slot_reg
        if (instr.operands.size() < 2) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = static_cast<uint8_t>(instr.operands[1].index & 0x1F);
        chunk_->writeOp(RegOp::REG_MOVE, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }
    case IROp::STORE_LOCAL: {
        // IR: STORE_LOCAL slot, src_vreg
        // → REG_MOVE slot_reg, src_vreg_reg
        if (instr.operands.size() < 2) return false;
        uint8_t dst = static_cast<uint8_t>(instr.operands[0].index & 0x1F);
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
        if (instr.operands.size() < 2) return false;
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
            chunk_->writeShort(nameIdx | 0x8000, line);  // 高 bit 置 1 表示名称索引
        }
        break;
    }
    case IROp::STORE_GLOBAL: {
        if (instr.operands.size() < 2) return false;
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
        if (instr.operands.size() < 2) return false;
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

    // ---- upvalue ----
    case IROp::LOAD_UPVALUE: {
        if (instr.operands.size() < 2) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_LOAD_UPVALUE, line);
        chunk_->writeReg(dst, line);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF));
        while (static_cast<int>(chunk_->lines.size()) < static_cast<int>(chunk_->code.size()))
            chunk_->lines.push_back(line);
        break;
    }
    case IROp::STORE_UPVALUE: {
        if (instr.operands.size() < 2) return false;
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_STORE_UPVALUE, line);
        chunk_->writeReg(src, line);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index & 0xFF));
        while (static_cast<int>(chunk_->lines.size()) < static_cast<int>(chunk_->code.size()))
            chunk_->lines.push_back(line);
        break;
    }
    case IROp::CLOSE_UPVALUE: {
        if (instr.operands.empty()) return false;
        chunk_->writeOp(RegOp::REG_CLOSE_UPVALUE, line);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index & 0xFF));
        while (static_cast<int>(chunk_->lines.size()) < static_cast<int>(chunk_->code.size()))
            chunk_->lines.push_back(line);
        break;
    }

    // ---- 算术 ----
    case IROp::ADD:
    case IROp::SUB:
    case IROp::MUL:
    case IROp::DIV:
    case IROp::MOD: {
        if (instr.operands.size() < 3) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t s1 = vregToReg(instr.operands[1].index);
        uint8_t s2 = vregToReg(instr.operands[2].index);
        RegOp op = RegOp::REG_ADD;
        switch (instr.op) {
        case IROp::ADD: op = RegOp::REG_ADD; break;
        case IROp::SUB: op = RegOp::REG_SUB; break;
        case IROp::MUL: op = RegOp::REG_MUL; break;
        case IROp::DIV: op = RegOp::REG_DIV; break;
        case IROp::MOD: op = RegOp::REG_MOD; break;
        default: break;
        }
        chunk_->writeOp(op, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(s1, line);
        chunk_->writeReg(s2, line);
        break;
    }
    case IROp::NEGATE: {
        if (instr.operands.size() < 2) return false;
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
        if (instr.operands.size() < 3) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t s1 = vregToReg(instr.operands[1].index);
        uint8_t s2 = vregToReg(instr.operands[2].index);
        RegOp op = RegOp::REG_EQ;
        switch (instr.op) {
        case IROp::EQ:  op = RegOp::REG_EQ;  break;
        case IROp::NEQ: op = RegOp::REG_NEQ; break;
        case IROp::LT:  op = RegOp::REG_LT;  break;
        case IROp::GT:  op = RegOp::REG_GT;  break;
        case IROp::LTE: op = RegOp::REG_LTE; break;
        case IROp::GTE: op = RegOp::REG_GTE; break;
        default: break;
        }
        chunk_->writeOp(op, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(s1, line);
        chunk_->writeReg(s2, line);
        break;
    }
    case IROp::NOT: {
        if (instr.operands.size() < 2) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_NOT, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }

    // ---- 控制流 ----
    case IROp::LABEL: {
        if (instr.operands.empty()) return false;
        labelToOffset_[instr.operands[0].index] = chunk_->code.size();
        break;  // LABEL 不产生字节码
    }
    case IROp::JUMP: {
        if (instr.operands.empty()) return false;
        chunk_->writeOp(RegOp::REG_JUMP, line);
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index});
        chunk_->writeShort(0, line);  // 占位符
        break;
    }
    case IROp::JUMP_IF_FALSE: {
        if (instr.operands.size() < 2) return false;
        uint8_t src = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_JUMP_IF_FALSE, line);
        chunk_->writeReg(src, line);
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[1].index});
        chunk_->writeShort(0, line);  // 占位符
        break;
    }
    case IROp::RETURN: {
        if (instr.operands.empty()) return false;
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
        // IR: CALL dest, name_idx, arg_count, arg1, arg2, ...
        if (instr.operands.size() < 3) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t nameIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        uint8_t argCount = static_cast<uint8_t>(instr.operands[2].index & 0xFF);
        chunk_->writeOp(RegOp::REG_CALL, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(nameIdx, line);
        chunk_->writeReg(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (3 + i >= instr.operands.size()) return false;
            uint8_t argReg = vregToReg(instr.operands[3 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }
    case IROp::CALL_EXPR: {
        // IR: CALL_EXPR dest, closure_vreg, arg_count, arg1, ...
        if (instr.operands.size() < 3) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t callee = vregToReg(instr.operands[1].index);
        uint8_t argCount = static_cast<uint8_t>(instr.operands[2].index & 0xFF);
        chunk_->writeOp(RegOp::REG_CALL_EXPR, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(callee, line);
        chunk_->writeReg(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (3 + i >= instr.operands.size()) return false;
            uint8_t argReg = vregToReg(instr.operands[3 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }
    case IROp::METHOD_CALL: {
        // IR: METHOD_CALL dest, obj, method_idx, arg_count, args...
        if (instr.operands.size() < 4) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t obj = vregToReg(instr.operands[1].index);
        uint16_t methodIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        uint8_t argCount = static_cast<uint8_t>(instr.operands[3].index & 0xFF);
        chunk_->writeOp(RegOp::REG_METHOD_CALL, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(obj, line);
        chunk_->writeShort(methodIdx, line);
        chunk_->writeReg(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (4 + i >= instr.operands.size()) return false;
            uint8_t argReg = vregToReg(instr.operands[4 + i].index);
            chunk_->writeReg(argReg, line);
        }
        break;
    }

    // ---- 闭包 ----
    case IROp::MAKE_CLOSURE: {
        // IR: MAKE_CLOSURE dest, name_idx, uv_count, [isLocal, idx]×uv_count
        if (instr.operands.size() < 3) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t nameIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        uint8_t uvCount = static_cast<uint8_t>(instr.operands[2].index & 0xFF);
        chunk_->writeOp(RegOp::REG_MAKE_CLOSURE, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(nameIdx, line);
        chunk_->writeReg(uvCount, line);
        for (uint8_t i = 0; i < uvCount; ++i) {
            size_t base = 3 + i * 2;
            if (base + 1 >= instr.operands.size()) return false;
            chunk_->writeReg(static_cast<uint8_t>(instr.operands[base].index & 0xFF), line);       // isLocal
            chunk_->writeReg(static_cast<uint8_t>(instr.operands[base + 1].index & 0xFF), line);   // idx
        }
        break;
    }

    // ---- 容器 ----
    case IROp::BUILD_ARRAY: {
        // IR: BUILD_ARRAY dest, count, arg1, arg2, ...
        if (instr.operands.size() < 2) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t count = static_cast<uint8_t>(instr.operands[1].index & 0xFF);
        chunk_->writeOp(RegOp::REG_BUILD_ARRAY, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(count, line);
        for (uint8_t i = 0; i < count; ++i) {
            if (2 + i >= instr.operands.size()) return false;
            uint8_t elemReg = vregToReg(instr.operands[2 + i].index);
            chunk_->writeReg(elemReg, line);
        }
        break;
    }
    case IROp::BUILD_DICT: {
        // IR: BUILD_DICT dest, pair_count, k1, v1, k2, v2, ...
        if (instr.operands.size() < 2) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t pairCount = static_cast<uint8_t>(instr.operands[1].index & 0xFF);
        chunk_->writeOp(RegOp::REG_BUILD_DICT, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(pairCount, line);
        for (uint8_t i = 0; i < pairCount; ++i) {
            size_t kBase = 2 + i * 2;
            if (kBase + 1 >= instr.operands.size()) return false;
            chunk_->writeReg(vregToReg(instr.operands[kBase].index), line);       // key
            chunk_->writeReg(vregToReg(instr.operands[kBase + 1].index), line);   // value
        }
        break;
    }
    case IROp::INDEX_GET: {
        if (instr.operands.size() < 3) return false;
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
        if (instr.operands.size() < 3) return false;
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
        if (instr.operands.size() < 3) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t obj = vregToReg(instr.operands[1].index);
        uint16_t fieldIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        chunk_->writeOp(RegOp::REG_MEMBER_GET, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(obj, line);
        chunk_->writeShort(fieldIdx, line);
        break;
    }
    case IROp::MEMBER_SET: {
        if (instr.operands.size() < 3) return false;
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
        // 操作数布局（见 IR.cpp visitClassDecl）：
        //   [0] className (FUNC_NAME)
        //   [1] parentName (IMM_UINT, UINT32_MAX=无父类)
        //   [2] fieldCount (IMM_UINT)
        //   [3 .. 3+F-1] field names (FIELD_NAME)
        //   [3+F] methodCount (IMM_UINT)
        //   [3+F+1 .. ] (methodName FIELD_NAME, funName FUNC_NAME) × M
        if (instr.operands.size() < 3) return false;
        uint16_t nameIdx = addStringConstant(globalName(instr.operands[0].index), ir);

        uint32_t parentRaw = instr.operands[1].index;
        uint16_t parentIdx = (parentRaw == UINT32_MAX)
            ? 0xFFFF
            : addStringConstant(globalName(parentRaw), ir);

        uint32_t fieldCount = instr.operands[2].index;
        if (instr.operands.size() < 3 + fieldCount + 1) return false;
        uint32_t methodCount = instr.operands[3 + fieldCount].index;
        if (instr.operands.size() < 3 + fieldCount + 1 + methodCount * 2) return false;

        chunk_->writeOp(RegOp::REG_DEFINE_CLASS, line);
        chunk_->writeShort(nameIdx, line);
        chunk_->writeShort(parentIdx, line);
        chunk_->writeByte(static_cast<uint8_t>(fieldCount), line);
        for (uint32_t i = 0; i < fieldCount; ++i) {
            uint16_t fIdx = addStringConstant(globalName(instr.operands[3 + i].index), ir);
            chunk_->writeShort(fIdx, line);
        }
        chunk_->writeByte(static_cast<uint8_t>(methodCount), line);
        for (uint32_t i = 0; i < methodCount; ++i) {
            size_t base = 3 + fieldCount + 1 + i * 2;
            uint16_t mIdx = addStringConstant(globalName(instr.operands[base].index), ir);
            uint16_t fIdx = addStringConstant(globalName(instr.operands[base + 1].index), ir);
            chunk_->writeShort(mIdx, line);
            chunk_->writeShort(fIdx, line);
        }
        break;
    }
    case IROp::CLASS_NEW: {
        // IR: CLASS_NEW dest, name_idx, arg_count, args...
        if (instr.operands.size() < 3) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint16_t nameIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        uint8_t argCount = static_cast<uint8_t>(instr.operands[2].index & 0xFF);
        chunk_->writeOp(RegOp::REG_CLASS_NEW, line);
        chunk_->writeReg(dst, line);
        chunk_->writeShort(nameIdx, line);
        chunk_->writeReg(argCount, line);
        for (uint8_t i = 0; i < argCount; ++i) {
            if (3 + i >= instr.operands.size()) return false;
            chunk_->writeReg(vregToReg(instr.operands[3 + i].index), line);
        }
        break;
    }
    case IROp::INIT_FIELD: {
        if (instr.operands.empty()) return false;
        uint16_t fieldIdx = addStringConstant(globalName(instr.operands[0].index), ir);
        chunk_->writeOp(RegOp::REG_INIT_FIELD, line);
        chunk_->writeShort(fieldIdx, line);
        break;
    }

    // ---- 异常 ----
    case IROp::TRY_BEGIN: {
        if (instr.operands.empty()) return false;
        chunk_->writeOp(RegOp::REG_TRY_BEGIN, line);
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index});
        chunk_->writeShort(0, line);  // 占位符
        break;
    }
    case IROp::TRY_END: {
        chunk_->writeOp(RegOp::REG_TRY_END, line);
        break;
    }
    case IROp::THROW: {
        if (instr.operands.empty()) return false;
        uint8_t src = vregToReg(instr.operands[0].index);
        chunk_->writeOp(RegOp::REG_THROW, line);
        chunk_->writeReg(src, line);
        break;
    }

    // ---- 写回 ----
    case IROp::WRITEBACK_MEMBER_VAR: {
        if (instr.operands.size() < 2) return false;
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
        if (instr.operands.size() < 2) return false;
        chunk_->writeOp(RegOp::REG_WRITEBACK_MEMBER_LOCAL, line);
        chunk_->writeReg(static_cast<uint8_t>(instr.operands[0].index & 0x1F), line);
        chunk_->writeShort(addStringConstant(globalName(instr.operands[1].index), ir), line);
        break;
    }
    case IROp::WRITEBACK_INDEX_VAR: {
        if (instr.operands.empty()) return false;
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
        if (instr.operands.empty()) return false;
        chunk_->writeOp(RegOp::REG_WRITEBACK_INDEX_LOCAL, line);
        chunk_->writeReg(static_cast<uint8_t>(instr.operands[0].index & 0x1F), line);
        break;
    }
    case IROp::WRITEBACK_MEMBER_UPVALUE: {
        // #7 fix: 寄存器式 upvalue 写回（uvIdx 在运行时由 frame.upvalues 解释，不映射到寄存器）
        if (instr.operands.size() < 2) return false;
        chunk_->writeOp(RegOp::REG_WRITEBACK_MEMBER_UPVALUE, line);
        chunk_->writeByte(static_cast<uint8_t>(instr.operands[0].index & 0xFF), line);
        chunk_->writeShort(addStringConstant(globalName(instr.operands[1].index), ir), line);
        break;
    }
    case IROp::WRITEBACK_INDEX_UPVALUE: {
        if (instr.operands.empty()) return false;
        chunk_->writeOp(RegOp::REG_WRITEBACK_INDEX_UPVALUE, line);
        chunk_->writeByte(static_cast<uint8_t>(instr.operands[0].index & 0xFF), line);
        break;
    }

    // ---- 其他 ----
    case IROp::PRINT: {
        if (instr.operands.empty()) return false;
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
        if (instr.operands.size() < 2) return false;
        uint8_t dst = vregToReg(instr.operands[0].index);
        uint8_t src = vregToReg(instr.operands[1].index);
        chunk_->writeOp(RegOp::REG_MOVE, line);
        chunk_->writeReg(dst, line);
        chunk_->writeReg(src, line);
        break;
    }

    default:
        Logger::Error("RegisterBytecodeBackend: 未支持的 IR 指令 " +
                      std::to_string(static_cast<int>(instr.op)), "RegIR");
        return false;
    }
    return true;
}

bool RegisterBytecodeBackend::patchJumps() {
    for (const auto& pj : pendingJumps_) {
        auto it = labelToOffset_.find(pj.targetLabel);
        if (it == labelToOffset_.end()) {
            Logger::Error("RegisterBytecodeBackend: 未找到标签 " +
                          std::to_string(pj.targetLabel), "RegIR");
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
    if (!mainChunk) return false;

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
