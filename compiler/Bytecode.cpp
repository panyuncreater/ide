#include "compiler/Bytecode.h"
#include <sstream>
#include <string>

// ============================================================
// Bytecode.cpp — 反汇编实现（D9 fix: 从 Bytecode.h 提取）
// ------------------------------------------------------------
// A3 fix: opCodeName 已内联到 Bytecode.h（基于元数据表查表）。
// 本文件保留 disassemble / disassembleInstruction 实现，并定义元数据表。
// ============================================================

// A3 fix: 统一 opcode 元数据表（名称/基础长度/是否变长）。
// 索引与 OpCode enum 严格对齐（OP_CONSTANT=0, ..., OP_WRITEBACK_INDEX_UPVALUE=70）。
// 新增 opcode 时只需在此表追加一行，无需修改 opCodeName/instructionSize/instructionSizeAt。
//
// ---- MiniLang 字节码格式约定（编解码通用）----
//   · code：连续的 uint8_t 指令流。首字节为 opcode，后续字节为操作数（小端）。
//   · 常量池 constants：所有字面量（数字/字符串/变量名/类型注解）集中存放，
//     指令中以 16 位小端索引（code[ip+1] | code[ip+2]<<8）引用，0xFFFF 常作"无"哨兵。
//   · 局部变量槽为 8 位；全局变量槽为 16 位；跳转/循环目标为绝对字节偏移（同样 16 位小端），
//     OP_JUMP_IF_FALSE 不消费条件值（由编译器额外生成 OP_POP）。
//   · 唯一变长指令是 OP_CLOSURE：基础 4 字节 +
//     upvalueCount * 2 字节描述符（每对 = isLocal(1B) + uvIndex(1B)）。
//   · 反汇编器 disassembleInstruction 把 offset 作为输入输出参数按实际字节数前进，
//     据此可线性遍历整个 code 流；其长度解析必须与 kOpCodeInfo / instructionSizeAt 完全一致。
namespace {
constexpr OpCodeInfo kOpCodeInfo[] = {
    /*  0 OP_CONSTANT            */ {"OP_CONSTANT", 3, false},
    /*  1 OP_INT                 */ {"OP_INT", 3, false},
    /*  2 OP_FLOAT               */ {"OP_FLOAT", 3, false},
    /*  3 OP_STRING              */ {"OP_STRING", 3, false},
    /*  4 OP_NULL                */ {"OP_NULL", 1, false},
    /*  5 OP_TRUE                */ {"OP_TRUE", 1, false},
    /*  6 OP_FALSE               */ {"OP_FALSE", 1, false},
    /*  7 OP_ADD                 */ {"OP_ADD", 1, false},
    /*  8 OP_SUBTRACT            */ {"OP_SUBTRACT", 1, false},
    /*  9 OP_MULTIPLY            */ {"OP_MULTIPLY", 1, false},
    /* 10 OP_DIVIDE              */ {"OP_DIVIDE", 1, false},
    /* 11 OP_MODULO              */ {"OP_MODULO", 1, false},
    /* 12 OP_NEGATE              */ {"OP_NEGATE", 1, false},
    /* 13 OP_NOT                 */ {"OP_NOT", 1, false},
    /* 14 OP_EQUAL               */ {"OP_EQUAL", 1, false},
    /* 15 OP_NOT_EQUAL           */ {"OP_NOT_EQUAL", 1, false},
    /* 16 OP_LESS                */ {"OP_LESS", 1, false},
    /* 17 OP_GREATER             */ {"OP_GREATER", 1, false},
    /* 18 OP_LESS_EQUAL          */ {"OP_LESS_EQUAL", 1, false},
    /* 19 OP_GREATER_EQUAL       */ {"OP_GREATER_EQUAL", 1, false},
    /* 20 OP_AND                 */ {"OP_AND", 1, false},
    /* 21 OP_OR                  */ {"OP_OR", 1, false},
    /* 22 OP_PRINT               */ {"OP_PRINT", 1, false},
    /* 23 OP_POP                 */ {"OP_POP", 1, false},
    /* 24 OP_DEFINE_VAR          */ {"OP_DEFINE_VAR", 3, false},
    /* 25 OP_GET_VAR             */ {"OP_GET_VAR", 3, false},
    /* 26 OP_SET_VAR             */ {"OP_SET_VAR", 3, false},
    /* 27 OP_DELETE_VAR          */ {"OP_DELETE_VAR", 3, false},
    /* 28 OP_JUMP                */ {"OP_JUMP", 3, false},
    /* 29 OP_JUMP_IF_FALSE       */ {"OP_JUMP_IF_FALSE", 3, false},
    /* 30 OP_LOOP                */ {"OP_LOOP", 3, false},
    /* 31 OP_RETURN              */ {"OP_RETURN", 1, false},
    /* 32 OP_CALL                */ {"OP_CALL", 4, false},
    /* 33 OP_CALL_EXPR           */ {"OP_CALL_EXPR", 2, false},
    /* 34 OP_BUILD_ARRAY         */ {"OP_BUILD_ARRAY", 2, false},
    /* 35 OP_BUILD_DICT          */ {"OP_BUILD_DICT", 2, false},
    /* 36 OP_INDEX_GET           */ {"OP_INDEX_GET", 1, false},
    /* 37 OP_INDEX_SET           */ {"OP_INDEX_SET", 1, false},
    /* 38 OP_INDEX_SET_VAR       */ {"OP_INDEX_SET_VAR", 3, false},
    /* 39 OP_INDEX_SET_LOCAL     */ {"OP_INDEX_SET_LOCAL", 2, false},
    /* 40 OP_MEMBER_GET          */ {"OP_MEMBER_GET", 3, false},
    /* 41 OP_MEMBER_SET          */ {"OP_MEMBER_SET", 3, false},
    /* 42 OP_MEMBER_SET_VAR      */ {"OP_MEMBER_SET_VAR", 5, false},
    /* 43 OP_MEMBER_SET_LOCAL    */ {"OP_MEMBER_SET_LOCAL", 4, false},
    /* 44 OP_METHOD_CALL         */ {"OP_METHOD_CALL", 7, false},
    /* 45 OP_DUP                 */ {"OP_DUP", 1, false},
    /* 46 OP_DUP_N               */ {"OP_DUP_N", 2, false},
    /* 47 OP_CLOSURE             */ {"OP_CLOSURE", 4, true}, // 变长: 4 + 2*upvalueCount
    /* 48 OP_GET_LOCAL           */ {"OP_GET_LOCAL", 2, false},
    /* 49 OP_SET_LOCAL           */ {"OP_SET_LOCAL", 2, false},
    /* 50 OP_CLASS_NEW           */ {"OP_CLASS_NEW", 4, false},
    /* 51 OP_INIT_FIELD          */ {"OP_INIT_FIELD", 3, false},
    /* 52 OP_DEFINE_CLASS        */ {"OP_DEFINE_CLASS", 5, false},
    /* 53 OP_WRITEBACK_MEMBER_VAR   */ {"OP_WRITEBACK_MEMBER_VAR", 5, false},
    /* 54 OP_WRITEBACK_MEMBER_LOCAL */ {"OP_WRITEBACK_MEMBER_LOCAL", 4, false},
    /* 55 OP_WRITEBACK_INDEX_VAR    */ {"OP_WRITEBACK_INDEX_VAR", 3, false},
    /* 56 OP_WRITEBACK_INDEX_LOCAL  */ {"OP_WRITEBACK_INDEX_LOCAL", 2, false},
    /* 57 OP_SUPER_CALL             */ {"OP_SUPER_CALL", 9, false}, // B1 fix: opcode(1B) + nameIdx(2B) + argCount(1B) +
                                                                    // receiverVarIdx(2B) + receiverLocalSlot(1B) +
                                                                    // classIdx(2B)
    /* 58 OP_SUPER_MEMBER_GET       */ {"OP_SUPER_MEMBER_GET", 3, false},
    /* 59 OP_GET_GLOBAL             */ {"OP_GET_GLOBAL", 3, false},
    /* 60 OP_SET_GLOBAL             */ {"OP_SET_GLOBAL", 3, false},
    /* 61 OP_DEFINE_GLOBAL          */ {"OP_DEFINE_GLOBAL", 3, false},
    /* 62 OP_DELETE_GLOBAL          */ {"OP_DELETE_GLOBAL", 3, false},
    /* 63 OP_GET_UPVALUE            */ {"OP_GET_UPVALUE", 2, false},
    /* 64 OP_SET_UPVALUE            */ {"OP_SET_UPVALUE", 2, false},
    /* 65 OP_CLOSE_UPVALUE          */ {"OP_CLOSE_UPVALUE", 2, false},
    /* 66 OP_TRY_BEGIN              */ {"OP_TRY_BEGIN", 3, false},
    /* 67 OP_TRY_END                */ {"OP_TRY_END", 1, false},
    /* 68 OP_THROW                  */ {"OP_THROW", 1, false},
    /* 69 OP_WRITEBACK_MEMBER_UPVALUE */ {"OP_WRITEBACK_MEMBER_UPVALUE", 4, false},
    /* 70 OP_WRITEBACK_INDEX_UPVALUE  */ {"OP_WRITEBACK_INDEX_UPVALUE", 2, false},
    /* 71 OP_LOAD_MUTATED             */ {"OP_LOAD_MUTATED", 1, false},
    /* 72 OP_TYPE_CHECK               */ {"OP_TYPE_CHECK", 3, false},       // opcode(1B) + typeAnnotationConstIdx(2B)
    /* 73 OP_PUSH_JUMP_TARGET         */ {"OP_PUSH_JUMP_TARGET", 3, false}, // opcode(1B) + target(2B)
    /* 74 OP_FINALLY_END              */ {"OP_FINALLY_END", 1, false},
};
} // anonymous namespace

/// 获取 opcode 元数据（name + baseSize + isVariableLength）
/// A3 fix: 单一数据源，替代原 opCodeName switch + instructionSize 数组两处维护
const OpCodeInfo& getOpCodeInfo(OpCode op) {
    auto idx = static_cast<uint8_t>(op);
    static const OpCodeInfo kUnknown{"OP_UNKNOWN", 1, false};
    if (idx < sizeof(kOpCodeInfo) / sizeof(kOpCodeInfo[0])) {
        return kOpCodeInfo[idx];
    }
    return kUnknown;
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
/// 约定：参数 offset 为「输入/输出」参数——调用方传入本条指令起始偏移，
/// 函数内部按本指令实际字节数（含变长 OP_CLOSURE 的 upvalueCount*2）原地前进 offset，
/// 因此连续调用 disassembleInstruction 即可线性遍历整个 code 流而无需调用方自行累加长度。
/// 字节数与常量池索引的解析严格对齐 Bytecode.h 的 kOpCodeInfo 元数据表与 instructionSizeAt。
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
    case OpCode::OP_NULL:
        str += "OP_NULL";
        offset += 1;
        break;
    case OpCode::OP_TRUE:
        str += "OP_TRUE";
        offset += 1;
        break;
    case OpCode::OP_FALSE:
        str += "OP_FALSE";
        offset += 1;
        break;
    case OpCode::OP_ADD:
        str += "OP_ADD";
        offset += 1;
        break;
    case OpCode::OP_SUBTRACT:
        str += "OP_SUBTRACT";
        offset += 1;
        break;
    case OpCode::OP_MULTIPLY:
        str += "OP_MULTIPLY";
        offset += 1;
        break;
    case OpCode::OP_DIVIDE:
        str += "OP_DIVIDE";
        offset += 1;
        break;
    case OpCode::OP_MODULO:
        str += "OP_MODULO";
        offset += 1;
        break;
    case OpCode::OP_NEGATE:
        str += "OP_NEGATE";
        offset += 1;
        break;
    case OpCode::OP_NOT:
        str += "OP_NOT";
        offset += 1;
        break;
    case OpCode::OP_EQUAL:
        str += "OP_EQUAL";
        offset += 1;
        break;
    case OpCode::OP_NOT_EQUAL:
        str += "OP_NOT_EQUAL";
        offset += 1;
        break;
    case OpCode::OP_LESS:
        str += "OP_LESS";
        offset += 1;
        break;
    case OpCode::OP_GREATER:
        str += "OP_GREATER";
        offset += 1;
        break;
    case OpCode::OP_LESS_EQUAL:
        str += "OP_LESS_EQUAL";
        offset += 1;
        break;
    case OpCode::OP_GREATER_EQUAL:
        str += "OP_GREATER_EQUAL";
        offset += 1;
        break;
    case OpCode::OP_AND:
        str += "OP_AND";
        offset += 1;
        break;
    case OpCode::OP_OR:
        str += "OP_OR";
        offset += 1;
        break;
    case OpCode::OP_PRINT:
        str += "OP_PRINT";
        offset += 1;
        break;
    case OpCode::OP_POP:
        str += "OP_POP";
        offset += 1;
        break;
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
    case OpCode::OP_RETURN:
        str += "OP_RETURN";
        offset += 1;
        break;
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
    case OpCode::OP_INDEX_GET:
        str += "OP_INDEX_GET";
        offset += 1;
        break;
    case OpCode::OP_INDEX_SET:
        str += "OP_INDEX_SET";
        offset += 1;
        break;
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
        str += "OP_MEMBER_SET_VAR " + std::to_string(varIdx) + " (" + constants[varIdx].stringVal() + ") ." +
               constants[fieldIdx].stringVal();
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
        str += "OP_METHOD_CALL " + std::to_string(idx) + " (" + constants[idx].stringVal() + ") " +
               std::to_string(argCount);
        if (receiverIdx != 0xFFFF && receiverIdx < constants.size())
            str += " recv=" + constants[receiverIdx].stringVal();
        if (localSlot != 0xFF)
            str += " slot=" + std::to_string(localSlot);
        offset += 7;
        break;
    }
    case OpCode::OP_SUPER_CALL: {
        uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
        uint8_t argCount = code[offset + 3];
        uint16_t receiverIdx = code[offset + 4] | (code[offset + 5] << 8);
        uint8_t localSlot = code[offset + 6];
        uint16_t classIdx = code[offset + 7] | (code[offset + 8] << 8); // B1 fix
        str += "OP_SUPER_CALL " + std::to_string(idx) + " (" + constants[idx].stringVal() + ") " +
               std::to_string(argCount);
        if (receiverIdx != 0xFFFF && receiverIdx < constants.size())
            str += " recv=" + constants[receiverIdx].stringVal();
        if (localSlot != 0xFF)
            str += " slot=" + std::to_string(localSlot);
        if (classIdx < constants.size())
            str += " class=" + constants[classIdx].stringVal();
        offset += 9;
        break;
    }
    case OpCode::OP_DUP:
        str += "OP_DUP";
        offset += 1;
        break;
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
        if (idx < constants.size())
            str += " (" + constants[idx].stringVal() + ")";
        str += " upvalues=" + std::to_string(upvalueCount);
        offset += 4 + static_cast<size_t>(upvalueCount) * 2; // M1 fix: 跳过 upvalue 描述符
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
        str +=
            "OP_CLASS_NEW " + std::to_string(idx) + " (" + constants[idx].stringVal() + ") " + std::to_string(argCount);
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
        str += "OP_WRITEBACK_MEMBER_VAR " + std::to_string(varIdx) + " (" + constants[varIdx].stringVal() + ") ." +
               constants[fieldIdx].stringVal();
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
    case OpCode::OP_WRITEBACK_MEMBER_UPVALUE: {
        uint8_t uvIdx = code[offset + 1];
        uint16_t fieldIdx = code[offset + 2] | (code[offset + 3] << 8);
        str += "OP_WRITEBACK_MEMBER_UPVALUE uv=" + std::to_string(uvIdx) + " field=" + std::to_string(fieldIdx);
        offset += 4;
        break;
    }
    case OpCode::OP_WRITEBACK_INDEX_UPVALUE: {
        uint8_t uvIdx = code[offset + 1];
        str += "OP_WRITEBACK_INDEX_UPVALUE uv=" + std::to_string(uvIdx);
        offset += 2;
        break;
    }
    case OpCode::OP_LOAD_MUTATED:
        str += "OP_LOAD_MUTATED";
        offset += 1;
        break;
    case OpCode::OP_TYPE_CHECK: {
        uint16_t typeIdx = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_TYPE_CHECK typeIdx=" + std::to_string(typeIdx);
        if (typeIdx < constants.size()) {
            str += " (\"" + constants[typeIdx].toString() + "\")";
        }
        offset += 3;
        break;
    }
    case OpCode::OP_PUSH_JUMP_TARGET: {
        uint16_t target = code[offset + 1] | (code[offset + 2] << 8);
        str += "OP_PUSH_JUMP_TARGET target=" + std::to_string(target);
        offset += 3;
        break;
    }
    case OpCode::OP_FINALLY_END:
        str += "OP_FINALLY_END";
        offset += 1;
        break;
    default:
        str += "OP_UNKNOWN(" + std::to_string(static_cast<int>(op)) + ")";
        offset += 1;
        break;
    }

    return str;
}
