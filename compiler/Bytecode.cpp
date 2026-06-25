#include "compiler/Bytecode.h"
#include <sstream>
#include <string>

// ============================================================
// Bytecode.cpp — 反汇编实现（D9 fix: 从 Bytecode.h 提取）
// ------------------------------------------------------------
// 将 opCodeName / disassemble / disassembleInstruction 三个大型
// switch 实现从头文件移到此处，减少 Bytecode.h 体积（约 400 行），
// 加快编译速度，避免每个包含 Bytecode.h 的 TU 都解析这些 switch。
// ============================================================

/// 操作码 → 名称字符串（统一映射，避免多处手工维护）
const char* opCodeName(OpCode op) {
    switch (op) {
    case OpCode::OP_CONSTANT:      return "OP_CONSTANT";
    case OpCode::OP_INT:            return "OP_INT";
    case OpCode::OP_FLOAT:          return "OP_FLOAT";
    case OpCode::OP_STRING:        return "OP_STRING";
    case OpCode::OP_NULL:          return "OP_NULL";
    case OpCode::OP_TRUE:          return "OP_TRUE";
    case OpCode::OP_FALSE:         return "OP_FALSE";
    case OpCode::OP_ADD:           return "OP_ADD";
    case OpCode::OP_SUBTRACT:      return "OP_SUBTRACT";
    case OpCode::OP_MULTIPLY:      return "OP_MULTIPLY";
    case OpCode::OP_DIVIDE:        return "OP_DIVIDE";
    case OpCode::OP_MODULO:        return "OP_MODULO";
    case OpCode::OP_NEGATE:        return "OP_NEGATE";
    case OpCode::OP_NOT:           return "OP_NOT";
    case OpCode::OP_EQUAL:         return "OP_EQUAL";
    case OpCode::OP_NOT_EQUAL:     return "OP_NOT_EQUAL";
    case OpCode::OP_LESS:          return "OP_LESS";
    case OpCode::OP_GREATER:       return "OP_GREATER";
    case OpCode::OP_LESS_EQUAL:    return "OP_LESS_EQUAL";
    case OpCode::OP_GREATER_EQUAL: return "OP_GREATER_EQUAL";
    case OpCode::OP_AND:           return "OP_AND";
    case OpCode::OP_OR:            return "OP_OR";
    case OpCode::OP_PRINT:         return "OP_PRINT";
    case OpCode::OP_POP:           return "OP_POP";
    case OpCode::OP_DEFINE_VAR:    return "OP_DEFINE_VAR";
    case OpCode::OP_GET_VAR:       return "OP_GET_VAR";
    case OpCode::OP_SET_VAR:       return "OP_SET_VAR";
    case OpCode::OP_DELETE_VAR:    return "OP_DELETE_VAR";
    case OpCode::OP_JUMP:          return "OP_JUMP";
    case OpCode::OP_JUMP_IF_FALSE: return "OP_JUMP_IF_FALSE";
    case OpCode::OP_LOOP:          return "OP_LOOP";
    case OpCode::OP_RETURN:        return "OP_RETURN";
    case OpCode::OP_CALL:          return "OP_CALL";
    case OpCode::OP_CALL_EXPR:     return "OP_CALL_EXPR";
    case OpCode::OP_BUILD_ARRAY:   return "OP_BUILD_ARRAY";
    case OpCode::OP_BUILD_DICT:    return "OP_BUILD_DICT";
    case OpCode::OP_INDEX_GET:    return "OP_INDEX_GET";
    case OpCode::OP_INDEX_SET:    return "OP_INDEX_SET";
    case OpCode::OP_INDEX_SET_VAR: return "OP_INDEX_SET_VAR";
    case OpCode::OP_INDEX_SET_LOCAL: return "OP_INDEX_SET_LOCAL";
    case OpCode::OP_MEMBER_GET:   return "OP_MEMBER_GET";
    case OpCode::OP_MEMBER_SET:   return "OP_MEMBER_SET";
    case OpCode::OP_MEMBER_SET_VAR: return "OP_MEMBER_SET_VAR";
    case OpCode::OP_MEMBER_SET_LOCAL: return "OP_MEMBER_SET_LOCAL";
    case OpCode::OP_METHOD_CALL:  return "OP_METHOD_CALL";
    case OpCode::OP_DUP:          return "OP_DUP";
    case OpCode::OP_DUP_N:        return "OP_DUP_N";
    case OpCode::OP_CLOSURE:      return "OP_CLOSURE";
    case OpCode::OP_GET_LOCAL:    return "OP_GET_LOCAL";
    case OpCode::OP_SET_LOCAL:    return "OP_SET_LOCAL";
    case OpCode::OP_CLASS_NEW:    return "OP_CLASS_NEW";
    case OpCode::OP_INIT_FIELD:   return "OP_INIT_FIELD";
    case OpCode::OP_DEFINE_CLASS: return "OP_DEFINE_CLASS";
    case OpCode::OP_WRITEBACK_MEMBER_VAR: return "OP_WRITEBACK_MEMBER_VAR";
    case OpCode::OP_WRITEBACK_MEMBER_LOCAL: return "OP_WRITEBACK_MEMBER_LOCAL";
    case OpCode::OP_WRITEBACK_INDEX_VAR: return "OP_WRITEBACK_INDEX_VAR";
    case OpCode::OP_WRITEBACK_INDEX_LOCAL: return "OP_WRITEBACK_INDEX_LOCAL";
    case OpCode::OP_SUPER_CALL:       return "OP_SUPER_CALL";
    case OpCode::OP_SUPER_MEMBER_GET: return "OP_SUPER_MEMBER_GET";
    case OpCode::OP_GET_GLOBAL:       return "OP_GET_GLOBAL";
    case OpCode::OP_SET_GLOBAL:       return "OP_SET_GLOBAL";
    case OpCode::OP_DEFINE_GLOBAL:    return "OP_DEFINE_GLOBAL";
    case OpCode::OP_DELETE_GLOBAL:    return "OP_DELETE_GLOBAL";
    case OpCode::OP_GET_UPVALUE:      return "OP_GET_UPVALUE";
    case OpCode::OP_SET_UPVALUE:      return "OP_SET_UPVALUE";
    case OpCode::OP_CLOSE_UPVALUE:    return "OP_CLOSE_UPVALUE";
    case OpCode::OP_TRY_BEGIN:        return "OP_TRY_BEGIN";
    case OpCode::OP_TRY_END:          return "OP_TRY_END";
    case OpCode::OP_THROW:            return "OP_THROW";
    }
    return "OP_UNKNOWN";
}

/// 反汇编：输出字节码文本
std::string BytecodeChunk::disassemble() const {
    std::string result;
    if (!name.empty()) {
        result += "== " + name + " (arity=" + std::to_string(arity) + ") ==\n";
    }
    size_t offset = 0;
    while (offset < code.size()) {
        result += disassembleInstruction(offset);
        result += "\n";
    }
    return result;
}

/// 反汇编单条指令
std::string BytecodeChunk::disassembleInstruction(size_t& offset) const {
    OpCode op = static_cast<OpCode>(code[offset]);
    int line = getLine(offset);
    std::string str = std::to_string(offset) + " L" + std::to_string(line) + " ";

    switch (op) {
    case OpCode::OP_CONSTANT: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_CONSTANT " + std::to_string(idx) + " (" + constants[idx].toString() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_INT: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_INT " + std::to_string(idx) + " (" + std::to_string(constants[idx].intVal()) + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_FLOAT: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        std::ostringstream oss;
        oss << constants[idx].floatVal();
        str += "OP_FLOAT " + std::to_string(idx) + " (" + oss.str() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_STRING: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_STRING " + std::to_string(idx) + " (\"" + constants[idx].stringVal() + "\")";
        offset += 3;
        break;
    }
    case OpCode::OP_NULL:    str += "OP_NULL"; offset += 1; break;
    case OpCode::OP_TRUE:    str += "OP_TRUE"; offset += 1; break;
    case OpCode::OP_FALSE:   str += "OP_FALSE"; offset += 1; break;
    case OpCode::OP_ADD:     str += "OP_ADD"; offset += 1; break;
    case OpCode::OP_SUBTRACT: str += "OP_SUBTRACT"; offset += 1; break;
    case OpCode::OP_MULTIPLY: str += "OP_MULTIPLY"; offset += 1; break;
    case OpCode::OP_DIVIDE:  str += "OP_DIVIDE"; offset += 1; break;
    case OpCode::OP_MODULO:  str += "OP_MODULO"; offset += 1; break;
    case OpCode::OP_NEGATE:  str += "OP_NEGATE"; offset += 1; break;
    case OpCode::OP_NOT:     str += "OP_NOT"; offset += 1; break;
    case OpCode::OP_EQUAL:   str += "OP_EQUAL"; offset += 1; break;
    case OpCode::OP_NOT_EQUAL: str += "OP_NOT_EQUAL"; offset += 1; break;
    case OpCode::OP_LESS:    str += "OP_LESS"; offset += 1; break;
    case OpCode::OP_GREATER: str += "OP_GREATER"; offset += 1; break;
    case OpCode::OP_LESS_EQUAL: str += "OP_LESS_EQUAL"; offset += 1; break;
    case OpCode::OP_GREATER_EQUAL: str += "OP_GREATER_EQUAL"; offset += 1; break;
    case OpCode::OP_AND:     str += "OP_AND"; offset += 1; break;
    case OpCode::OP_OR:      str += "OP_OR"; offset += 1; break;
    case OpCode::OP_PRINT:   str += "OP_PRINT"; offset += 1; break;
    case OpCode::OP_POP:     str += "OP_POP"; offset += 1; break;
    case OpCode::OP_DEFINE_VAR: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_DEFINE_VAR " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_GET_VAR: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_GET_VAR " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_SET_VAR: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_SET_VAR " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_DELETE_VAR: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_DELETE_VAR " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_JUMP: {
        uint16_t jump = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_JUMP " + std::to_string(jump);
        offset += 3;
        break;
    }
    case OpCode::OP_JUMP_IF_FALSE: {
        uint16_t jump = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_JUMP_IF_FALSE " + std::to_string(jump);
        offset += 3;
        break;
    }
    case OpCode::OP_LOOP: {
        uint16_t loop = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_LOOP " + std::to_string(loop);
        offset += 3;
        break;
    }
    case OpCode::OP_RETURN:  str += "OP_RETURN"; offset += 1; break;
    case OpCode::OP_CALL: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        uint8_t argCount = code[offset + 3];
        str += "OP_CALL " + std::to_string(idx) + " (" + constants[idx].stringVal() + ") " + std::to_string(argCount);
        offset += 4;
        break;
    }
    case OpCode::OP_CALL_EXPR: {
        uint8_t argCount = code[offset + 1];
        str += "OP_CALL_EXPR " + std::to_string(argCount);
        offset += 2;
        break;
    }
    case OpCode::OP_BUILD_ARRAY: {
        uint8_t count = code[offset + 1];
        str += "OP_BUILD_ARRAY " + std::to_string(count);
        offset += 2;
        break;
    }
    case OpCode::OP_BUILD_DICT: {
        uint8_t count = code[offset + 1];
        str += "OP_BUILD_DICT " + std::to_string(count);
        offset += 2;
        break;
    }
    case OpCode::OP_INDEX_GET: str += "OP_INDEX_GET"; offset += 1; break;
    case OpCode::OP_INDEX_SET: str += "OP_INDEX_SET"; offset += 1; break;
    case OpCode::OP_INDEX_SET_VAR: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_INDEX_SET_VAR " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_INDEX_SET_LOCAL: {
        uint8_t slot = code[offset + 1];
        str += "OP_INDEX_SET_LOCAL slot=" + std::to_string(slot);
        offset += 2;
        break;
    }
    case OpCode::OP_MEMBER_GET: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_MEMBER_GET " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_SUPER_MEMBER_GET: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_SUPER_MEMBER_GET " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_MEMBER_SET: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_MEMBER_SET " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_MEMBER_SET_VAR: {
        uint16_t varIdx = code[offset + 1] | (code[offset + 2] << 8);
        uint16_t fieldIdx = code[offset + 3] | (code[offset + 4] << 8);
        str += "OP_MEMBER_SET_VAR " + std::to_string(varIdx) + " (" + constants[varIdx].stringVal() + ") ." + constants[fieldIdx].stringVal();
        offset += 5;
        break;
    }
    case OpCode::OP_MEMBER_SET_LOCAL: {
        uint8_t slot = code[offset + 1];
        uint16_t fieldIdx = code[offset + 2] | (code[offset + 3] << 8);
        str += "OP_MEMBER_SET_LOCAL slot=" + std::to_string(slot) + " ." + constants[fieldIdx].stringVal();
        offset += 4;
        break;
    }
    case OpCode::OP_METHOD_CALL: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        uint8_t argCount = code[offset + 3];
        uint16_t receiverIdx = code[offset + 4] | (code[offset + 5] << 8);
        uint8_t localSlot = code[offset + 6];
        str += "OP_METHOD_CALL " + std::to_string(idx) + " (" + constants[idx].stringVal() + ") " + std::to_string(argCount);
        if (receiverIdx != 0xFFFF && receiverIdx < constants.size()) str += " recv=" + constants[receiverIdx].stringVal();
        if (localSlot != 0xFF) str += " slot=" + std::to_string(localSlot);
        offset += 7;
        break;
    }
    case OpCode::OP_SUPER_CALL: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        uint8_t argCount = code[offset + 3];
        uint16_t receiverIdx = code[offset + 4] | (code[offset + 5] << 8);
        uint8_t localSlot = code[offset + 6];
        uint16_t classIdx = code[offset + 7] | (code[offset + 8] << 8);  // B1 fix
        str += "OP_SUPER_CALL " + std::to_string(idx) + " (" + constants[idx].stringVal() + ") " + std::to_string(argCount);
        if (receiverIdx != 0xFFFF && receiverIdx < constants.size()) str += " recv=" + constants[receiverIdx].stringVal();
        if (localSlot != 0xFF) str += " slot=" + std::to_string(localSlot);
        if (classIdx < constants.size()) str += " class=" + constants[classIdx].stringVal();
        offset += 9;
        break;
    }
    case OpCode::OP_DUP: str += "OP_DUP"; offset += 1; break;
    case OpCode::OP_DUP_N: {
        uint8_t depth = code[offset + 1];
        str += "OP_DUP_N depth=" + std::to_string(depth);
        offset += 2;
        break;
    }
    case OpCode::OP_CLOSURE: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        uint8_t upvalueCount = code[offset + 3];
        str += "OP_CLOSURE " + std::to_string(idx);
        if (idx < constants.size()) str += " (" + constants[idx].stringVal() + ")";
        str += " upvalues=" + std::to_string(upvalueCount);
        offset += 4 + static_cast<size_t>(upvalueCount) * 2;  // M1 fix: 跳过 upvalue 描述符
        break;
    }
    case OpCode::OP_GET_LOCAL: {
        uint8_t slot = code[offset + 1];
        str += "OP_GET_LOCAL " + std::to_string(slot);
        offset += 2;
        break;
    }
    case OpCode::OP_SET_LOCAL: {
        uint8_t slot = code[offset + 1];
        str += "OP_SET_LOCAL " + std::to_string(slot);
        offset += 2;
        break;
    }
    case OpCode::OP_CLASS_NEW: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        uint8_t argCount = code[offset + 3];
        str += "OP_CLASS_NEW " + std::to_string(idx) + " (" + constants[idx].stringVal() + ") " + std::to_string(argCount);
        offset += 4;
        break;
    }
    case OpCode::OP_INIT_FIELD: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_INIT_FIELD " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_DEFINE_CLASS: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        uint16_t superIdx = code[offset + 3] | (code[offset + 4] << 8);
        str += "OP_DEFINE_CLASS " + std::to_string(idx) + " (" + constants[idx].stringVal() + ")";
        if (superIdx != 0xFFFF && superIdx < constants.size()) {
            str += " extends " + constants[superIdx].stringVal();
        }
        offset += 5;
        break;
    }
    case OpCode::OP_WRITEBACK_MEMBER_VAR: {
        uint16_t varIdx = code[offset + 1] | (code[offset + 2] << 8);
        uint16_t fieldIdx = code[offset + 3] | (code[offset + 4] << 8);
        str += "OP_WRITEBACK_MEMBER_VAR " + std::to_string(varIdx) + " (" + constants[varIdx].stringVal() + ") ." + constants[fieldIdx].stringVal();
        offset += 5;
        break;
    }
    case OpCode::OP_WRITEBACK_MEMBER_LOCAL: {
        uint8_t slot = code[offset + 1];
        uint16_t fieldIdx = code[offset + 2] | (code[offset + 3] << 8);
        str += "OP_WRITEBACK_MEMBER_LOCAL slot=" + std::to_string(slot) + " ." + constants[fieldIdx].stringVal();
        offset += 4;
        break;
    }
    case OpCode::OP_WRITEBACK_INDEX_VAR: {
        uint16_t varIdx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_WRITEBACK_INDEX_VAR " + std::to_string(varIdx) + " (" + constants[varIdx].stringVal() + ")";
        offset += 3;
        break;
    }
    case OpCode::OP_WRITEBACK_INDEX_LOCAL: {
        uint8_t slot = code[offset + 1];
        str += "OP_WRITEBACK_INDEX_LOCAL slot=" + std::to_string(slot);
        offset += 2;
        break;
    }
    case OpCode::OP_GET_GLOBAL: {
        uint16_t slot = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_GET_GLOBAL slot=" + std::to_string(slot);
        offset += 3;
        break;
    }
    case OpCode::OP_SET_GLOBAL: {
        uint16_t slot = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_SET_GLOBAL slot=" + std::to_string(slot);
        offset += 3;
        break;
    }
    case OpCode::OP_DEFINE_GLOBAL: {
        uint16_t slot = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_DEFINE_GLOBAL slot=" + std::to_string(slot);
        offset += 3;
        break;
    }
    case OpCode::OP_DELETE_GLOBAL: {
        uint16_t slot = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_DELETE_GLOBAL slot=" + std::to_string(slot);
        offset += 3;
        break;
    }
    case OpCode::OP_GET_UPVALUE: {
        uint8_t idx = code[offset + 1];
        str += "OP_GET_UPVALUE " + std::to_string(idx);
        offset += 2;
        break;
    }
    case OpCode::OP_SET_UPVALUE: {
        uint8_t idx = code[offset + 1];
        str += "OP_SET_UPVALUE " + std::to_string(idx);
        offset += 2;
        break;
    }
    case OpCode::OP_CLOSE_UPVALUE: {
        uint8_t idx = code[offset + 1];
        str += "OP_CLOSE_UPVALUE " + std::to_string(idx);
        offset += 2;
        break;
    }
    case OpCode::OP_TRY_BEGIN: {
        uint16_t off = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_TRY_BEGIN catchOffset=" + std::to_string(off);
        offset += 3;
        break;
    }
    case OpCode::OP_TRY_END:
        str += "OP_TRY_END";
        offset += 1;
        break;
    case OpCode::OP_THROW:
        str += "OP_THROW";
        offset += 1;
        break;
    default:
        str += "OP_UNKNOWN(" + std::to_string(static_cast<int>(op)) + ")";
        offset += 1;
        break;
    }

    return str;
}
