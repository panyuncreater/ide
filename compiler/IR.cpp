#include "compiler/IR.h"
#include "Logger.h"
#include <sstream>

// ============================================================
// IR 中间表示层实现
// ============================================================
// ARCH-06 fix: 轻量 IR 框架实现。
// BytecodeIRBackend::lower 演示 IR → 字节码 lowering，
// 验证 IR 表达力足够覆盖现有 VM 语义。
// ============================================================

namespace {

/// 辅助：向 chunk.code 写入 uint16_t 操作数（小端序）
void emitUint16(std::vector<uint8_t>& code, uint16_t v) {
    code.push_back(static_cast<uint8_t>(v & 0xFF));
    code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

/// 辅助：IR 操作码 → 字符串（调试用）
const char* irOpName(IROp op) {
    switch (op) {
    case IROp::LOAD_CONST:   return "LOAD_CONST";
    case IROp::LOAD_GLOBAL:  return "LOAD_GLOBAL";
    case IROp::STORE_GLOBAL: return "STORE_GLOBAL";
    case IROp::ADD:          return "ADD";
    case IROp::SUB:          return "SUB";
    case IROp::MUL:          return "MUL";
    case IROp::DIV:          return "DIV";
    case IROp::MOD:          return "MOD";
    case IROp::NEGATE:       return "NEGATE";
    case IROp::EQ:           return "EQ";
    case IROp::NEQ:          return "NEQ";
    case IROp::LT:           return "LT";
    case IROp::GT:           return "GT";
    case IROp::LTE:          return "LTE";
    case IROp::GTE:          return "GTE";
    case IROp::NOT:          return "NOT";
    case IROp::JUMP:         return "JUMP";
    case IROp::JUMP_IF_FALSE:return "JUMP_IF_FALSE";
    case IROp::LABEL:        return "LABEL";
    case IROp::CALL:         return "CALL";
    case IROp::RETURN:       return "RETURN";
    case IROp::PRINT:        return "PRINT";
    case IROp::POP:          return "POP";
    default:                 return "?";
    }
}

} // anonymous namespace

// ============================================================
// BytecodeIRBackend::lower — IR → BytecodeChunk
// ============================================================

bool BytecodeIRBackend::lower(const IRFunction& ir) {
    if (!chunk_) {
        chunk_ = std::make_unique<BytecodeChunk>();
    }
    chunk_->code.clear();
    chunk_->constants.clear();
    chunk_->lines.clear();
    chunk_->name = ir.name;
    vregStackDepth_.clear();

    // 复制常量池
    chunk_->constants = ir.constants;

    // 标签 → 字节码偏移映射（两遍扫描：第一遍记录标签位置，第二遍修正跳转目标）
    // 简化实现：单遍扫描，跳转目标用占位符，第二遍回填
    struct PendingJump {
        size_t codeOffset;     // jump 指令操作数在 chunk_->code 中的位置
        uint32_t targetLabel;  // 目标标签索引
    };
    std::vector<PendingJump> pendingJumps;
    std::unordered_map<uint32_t, size_t> labelToOffset;  // label idx → code offset

    // 遍历所有基本块的所有指令
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            size_t startOffset = chunk_->code.size();

            switch (instr.op) {
            case IROp::LABEL:
                // 记录标签对应的字节码偏移
                if (!instr.operands.empty() && instr.operands[0].kind == IROperandKind::LABEL) {
                    labelToOffset[instr.operands[0].index] = chunk_->code.size();
                }
                break;

            case IROp::LOAD_CONST: {
                // LOAD_CONST dest, const_idx → OP_CONSTANT const_idx
                if (instr.operands.size() < 2) return false;
                uint32_t constIdx = instr.operands[1].index;
                // dest vreg 进入栈，记录栈深度
                vregStackDepth_[instr.operands[0].index] = chunk_->code.size();
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CONSTANT));
                emitUint16(chunk_->code, static_cast<uint16_t>(constIdx));
                break;
            }

            case IROp::LOAD_GLOBAL: {
                // LOAD_GLOBAL dest, name_idx → OP_GET_VAR const_name_idx
                if (instr.operands.size() < 2) return false;
                // 全局变量名作为字符串常量加入常量池
                std::string name = instr.operands[1].index < ir.globalNames.size()
                    ? ir.globalNames[instr.operands[1].index] : std::string{};
                uint16_t constIdx = static_cast<uint16_t>(chunk_->constants.size());
                chunk_->constants.push_back(Value(name));
                vregStackDepth_[instr.operands[0].index] = chunk_->code.size();
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_VAR));
                emitUint16(chunk_->code, constIdx);
                break;
            }

            case IROp::STORE_GLOBAL: {
                // STORE_GLOBAL name_idx, src → OP_SET_VAR const_name_idx
                if (instr.operands.size() < 2) return false;
                std::string name = instr.operands[0].index < ir.globalNames.size()
                    ? ir.globalNames[instr.operands[0].index] : std::string{};
                uint16_t constIdx = static_cast<uint16_t>(chunk_->constants.size());
                chunk_->constants.push_back(Value(name));
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_VAR));
                emitUint16(chunk_->code, constIdx);
                break;
            }

            case IROp::ADD:
            case IROp::SUB:
            case IROp::MUL:
            case IROp::DIV:
            case IROp::MOD: {
                // 二元算术：dest = src1 OP src2 → OP_ADD/SUB/MUL/DIV/MOD（栈式）
                // 简化：假设 src1 和 src2 已在栈上，直接 emit 算术指令
                OpCode vmOp;
                switch (instr.op) {
                case IROp::ADD: vmOp = OpCode::OP_ADD; break;
                case IROp::SUB: vmOp = OpCode::OP_SUBTRACT; break;
                case IROp::MUL: vmOp = OpCode::OP_MULTIPLY; break;
                case IROp::DIV: vmOp = OpCode::OP_DIVIDE; break;
                case IROp::MOD: vmOp = OpCode::OP_MODULO; break;
                default: return false;
                }
                chunk_->code.push_back(static_cast<uint8_t>(vmOp));
                break;
            }

            case IROp::NEGATE:
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NEGATE));
                break;

            case IROp::EQ:  chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_EQUAL)); break;
            case IROp::NEQ: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NOT_EQUAL)); break;
            case IROp::LT:  chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LESS)); break;
            case IROp::GT:  chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GREATER)); break;
            case IROp::LTE: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LESS_EQUAL)); break;
            case IROp::GTE: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GREATER_EQUAL)); break;
            case IROp::NOT: chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NOT)); break;

            case IROp::JUMP: {
                if (instr.operands.empty()) return false;
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_JUMP));
                pendingJumps.push_back({ chunk_->code.size(), instr.operands[0].index });
                emitUint16(chunk_->code, 0);  // 占位符，第二遍回填
                break;
            }

            case IROp::JUMP_IF_FALSE: {
                if (instr.operands.size() < 2) return false;
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_JUMP_IF_FALSE));
                pendingJumps.push_back({ chunk_->code.size(), instr.operands[1].index });
                emitUint16(chunk_->code, 0);  // 占位符
                break;
            }

            case IROp::RETURN:
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_RETURN));
                break;

            case IROp::PRINT:
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_PRINT));
                break;

            case IROp::POP:
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_POP));
                break;

            case IROp::CALL: {
                // CALL dest, name_idx, arg_count, args... → OP_CALL name_idx arg_count
                if (instr.operands.size() < 3) return false;
                std::string name = instr.operands[1].index < ir.globalNames.size()
                    ? ir.globalNames[instr.operands[1].index] : std::string{};
                uint16_t constIdx = static_cast<uint16_t>(chunk_->constants.size());
                chunk_->constants.push_back(Value(name));
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CALL));
                emitUint16(chunk_->code, constIdx);
                chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index));  // arg count
                break;
            }

            default:
                Logger::Error("BytecodeIRBackend: 未知 IR 操作码 " +
                              std::to_string(static_cast<int>(instr.op)), "IR");
                return false;
            }

            // 填充行号表（每条 IR 指令对应一行字节码）
            while (chunk_->lines.size() < chunk_->code.size()) {
                chunk_->lines.push_back(instr.line);
            }
            (void)startOffset;
        }
    }

    // 第二遍：回填跳转目标
    for (const auto& pj : pendingJumps) {
        auto it = labelToOffset.find(pj.targetLabel);
        if (it == labelToOffset.end()) {
            Logger::Error("BytecodeIRBackend: 未找到标签 " + std::to_string(pj.targetLabel), "IR");
            return false;
        }
        // 写入跳转目标偏移（相对跳转或绝对偏移，取决于 VM 约定）
        // 现有 VM OP_JUMP 用绝对偏移（ip = target）
        uint16_t target = static_cast<uint16_t>(it->second);
        chunk_->code[pj.codeOffset] = static_cast<uint8_t>(target & 0xFF);
        chunk_->code[pj.codeOffset + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }

    return true;
}

// ============================================================
// IRToString — 调试输出
// ============================================================

std::string IRToString(const IRFunction& ir) {
    std::ostringstream oss;
    oss << "IRFunction \"" << ir.name << "\" {\n";
    oss << "  constants: " << ir.constants.size() << "\n";
    oss << "  globals: " << ir.globalNames.size() << "\n";
    oss << "  vregs: " << ir.nextVReg << "\n";
    oss << "  blocks: " << ir.blocks.size() << "\n\n";

    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        const auto& block = ir.blocks[bi];
        oss << "  BB" << bi << " (label=" << block.labelIndex << "):\n";
        for (const auto& instr : block.instructions) {
            oss << "    " << irOpName(instr.op);
            for (const auto& operand : instr.operands) {
                const char* kindStr = "?";
                switch (operand.kind) {
                case IROperandKind::CONSTANT:    kindStr = "c"; break;
                case IROperandKind::VIRTUAL:     kindStr = "v"; break;
                case IROperandKind::LABEL:       kindStr = "L"; break;
                case IROperandKind::GLOBAL_NAME: kindStr = "g"; break;
                }
                oss << " " << kindStr << operand.index;
            }
            if (instr.line > 0) oss << "  ; line " << instr.line;
            oss << "\n";
        }
        oss << "\n";
    }
    oss << "}\n";
    return oss.str();
}
