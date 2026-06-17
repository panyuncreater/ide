#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include "interpreter/Value.h"

// ============================================================
// 字节码指令定义
// ============================================================

/// 操作码枚举
enum class OpCode : uint8_t {    OP_CONSTANT,     // 加载常量到栈顶
    OP_INT,          // 加载整数常量
    OP_FLOAT,        // 加载浮点常量
    OP_STRING,       // 加载字符串常量
    OP_NULL,         // 加载 null
    OP_TRUE,         // 加载 true
    OP_FALSE,        // 加载 false

    OP_ADD,          // 加法
    OP_SUBTRACT,     // 减法
    OP_MULTIPLY,     // 乘法
    OP_DIVIDE,       // 除法
    OP_MODULO,       // 取模
    OP_NEGATE,       // 一元取负
    OP_NOT,          // 逻辑取反

    OP_EQUAL,        // ==
    OP_NOT_EQUAL,    // !=
    OP_LESS,         // <
    OP_GREATER,      // >
    OP_LESS_EQUAL,   // <=
    OP_GREATER_EQUAL,// >=

    OP_AND,          // 逻辑与
    OP_OR,           // 逻辑或

    OP_PRINT,        // 输出栈顶
    OP_POP,          // 弹出栈顶

    OP_DEFINE_VAR,   // 定义变量（名称索引在常量池）
    OP_GET_VAR,      // 获取变量值
    OP_SET_VAR,      // 设置变量值

    OP_JUMP,         // 无条件跳转
    OP_JUMP_IF_FALSE,// 条件为假跳转
    OP_LOOP,         // 回跳（用于循环）

    OP_RETURN,       // 返回
    OP_CALL,         // 函数调用（参数个数）

    OP_BUILD_ARRAY,  // 构建数组（元素个数）
    OP_BUILD_DICT,   // 构建字典（键值对个数）
    OP_INDEX_GET,    // 索引取值
    OP_INDEX_SET,    // 索引赋值（旧：空操作）
    OP_INDEX_SET_VAR,// 索引赋值到变量（nameIdx(2B)）：直接修改 globals_[varName]
    OP_INDEX_SET_LOCAL,// 索引赋值到局部变量（slot(1B)）：直接修改 stack_[bp+slot]
    OP_MEMBER_GET,   // 成员取值（名称索引）
    OP_MEMBER_SET,   // 成员赋值（名称索引）（旧：空操作）
    OP_MEMBER_SET_VAR,// 成员赋值到变量（nameIdx(2B)+fieldNameIdx(2B)）：直接修改 globals_[varName].fields
    OP_MEMBER_SET_LOCAL,// 成员赋值到局部变量（slot(1B)+fieldNameIdx(2B)）：直接修改 stack_[bp+slot].fields
    OP_METHOD_CALL,  // 方法调用（名称索引 + 参数个数 + 接收者变量名索引）

    OP_DUP,          // 复制栈顶

    // 新增指令
    OP_CLOSURE,      // 创建闭包值（操作数: nameIdx(2B) + argCount(1B)）
    OP_GET_LOCAL,    // 读取当前帧局部变量（操作数: slot(1B)）
    OP_SET_LOCAL,    // 写入当前帧局部变量（操作数: slot(1B)）
    OP_CLASS_NEW,    // 类构造调用（操作数: nameIdx(2B) + argCount(1B)）
    OP_INIT_FIELD,   // 初始化实例字段（操作数: fieldNameIdx(2B)）：从栈顶 pop 值设置到实例的字段
    OP_DEFINE_CLASS, // 定义类（操作数: nameIdx(2B)）：从栈顶 pop 模板实例，提取类信息，注册到 VM

    // 嵌套访问变异方法写回指令（从 lastMutatedReceiver_ 取值写回基对象）
    OP_WRITEBACK_MEMBER_VAR,   // 成员写回到全局变量（varIdx(2B) + fieldIdx(2B)）
    OP_WRITEBACK_MEMBER_LOCAL, // 成员写回到局部变量（slot(1B) + fieldIdx(2B)）
    OP_WRITEBACK_INDEX_VAR,    // 索引写回到全局变量（varIdx(2B)），索引从栈顶 pop
    OP_WRITEBACK_INDEX_LOCAL,  // 索引写回到局部变量（slot(1B)），索引从栈顶 pop
};

/// 操作码 → 名称字符串（统一映射，避免多处手工维护）
inline const char* opCodeName(OpCode op) {
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
    case OpCode::OP_JUMP:          return "OP_JUMP";
    case OpCode::OP_JUMP_IF_FALSE: return "OP_JUMP_IF_FALSE";
    case OpCode::OP_LOOP:          return "OP_LOOP";
    case OpCode::OP_RETURN:        return "OP_RETURN";
    case OpCode::OP_CALL:          return "OP_CALL";
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
    }
    return "OP_UNKNOWN";
}

/// 字节码块：一段连续的指令
struct BytecodeChunk {
    std::vector<uint8_t> code;       // 指令字节流
    std::vector<Value> constants;    // 常量池
    std::vector<int> lines;         // 每条指令对应的行号
    std::string name;               // chunk 名称（函数名）
    int arity = 0;                  // 参数个数
    std::vector<int> ipToInstrIndex; // 预计算：字节偏移 → 指令索引映射
    std::vector<std::string> fieldOrder; // 方法所属类的字段声明顺序（用于 OP_METHOD_CALL 栈布局）
    int localCount = 0;              // 局部变量总槽位数（含参数/this/字段/方法体内var声明），用于 VM 帧创建时预分配栈空间

    BytecodeChunk() = default;
    explicit BytecodeChunk(const std::string& chunkName, int argCount = 0)
        : name(chunkName), arity(argCount) {}

    /// C21: 预分配字节码空间，避免编译期间频繁 realloc
    void reserveCode(size_t estimatedBytes) {
        code.reserve(estimatedBytes);
        lines.reserve(estimatedBytes);
    }

    /// 追加一个字节
    void write(uint8_t byte, int line) {
        code.push_back(byte);
        lines.push_back(line);
    }

    /// 追加一个操作码
    void writeOp(OpCode op, int line) {
        write(static_cast<uint8_t>(op), line);
    }

    /// 追加一个 16 位操作数（小端序）
    void writeShort(uint16_t value, int line) {
        write(static_cast<uint8_t>(value & 0xFF), line);
        write(static_cast<uint8_t>((value >> 8) & 0xFF), line);
    }

    /// 添加常量，返回索引（哈希去重，O(1) 均摊）
    uint16_t addConstant(const Value& val) {
        size_t h = hashValue(val);
        auto& bucket = constantHashMap_[h];
        for (uint16_t i : bucket) {
            if (constants[i].equals(val)) return i;
        }
        if (constants.size() >= 65535) {
            throw std::runtime_error("编译错误: 常量池超出限制 (65535)");
        }
        uint16_t idx = static_cast<uint16_t>(constants.size());
        constants.push_back(val);
        bucket.push_back(idx);
        return idx;
    }

private:
    /// 常量值哈希（用于 O(1) 去重）
    static size_t hashValue(const Value& val) {
        switch (val.getType()) {
        case ValueType::VAL_INT: return std::hash<int64_t>{}(val.intVal());
        case ValueType::VAL_FLOAT: return std::hash<double>{}(val.floatVal());
        case ValueType::VAL_STRING: return std::hash<std::string>{}(val.stringVal());
        case ValueType::VAL_BOOL: return std::hash<bool>{}(val.boolVal());
        case ValueType::VAL_NULL: return 0;
        default: return static_cast<size_t>(val.getType());
        }
    }

    std::unordered_map<size_t, std::vector<uint16_t>> constantHashMap_;

public:

    /// 获取指令行号
    int getLine(size_t offset) const {
        if (offset < lines.size()) return lines[offset];
        return 0;
    }

    /// 预计算 ip→指令索引映射（编译完成后调用一次）
    void buildIpMap() {
        ipToInstrIndex.assign(code.size(), -1);
        size_t offset = 0;
        int instrIdx = 0;
        while (offset < code.size()) {
            ipToInstrIndex[offset] = instrIdx;
            OpCode op = static_cast<OpCode>(code[offset]);
            offset += instructionSize(op);
            instrIdx++;
        }
    }

    /// 获取操作码对应的指令长度（字节数）
    static size_t instructionSize(OpCode op) {
        switch (op) {
        case OpCode::OP_CONSTANT:
        case OpCode::OP_INT:
        case OpCode::OP_FLOAT:
        case OpCode::OP_STRING:
        case OpCode::OP_DEFINE_VAR:
        case OpCode::OP_GET_VAR:
        case OpCode::OP_SET_VAR:
        case OpCode::OP_JUMP:
        case OpCode::OP_JUMP_IF_FALSE:
        case OpCode::OP_LOOP:
        case OpCode::OP_MEMBER_GET:
        case OpCode::OP_MEMBER_SET:
        case OpCode::OP_INDEX_SET_VAR:
        case OpCode::OP_INIT_FIELD:
            return 3;
        case OpCode::OP_DEFINE_CLASS:
            return 5;  // opcode(1B) + nameIdx(2B) + superNameIdx(2B)
        case OpCode::OP_INDEX_SET_LOCAL:
            return 2;
        case OpCode::OP_CALL:
        case OpCode::OP_CLOSURE:
        case OpCode::OP_CLASS_NEW:
            return 4;
        case OpCode::OP_BUILD_ARRAY:
        case OpCode::OP_BUILD_DICT:
        case OpCode::OP_GET_LOCAL:
        case OpCode::OP_SET_LOCAL:
            return 2;
        case OpCode::OP_MEMBER_SET_VAR:
            return 5;
        case OpCode::OP_MEMBER_SET_LOCAL:
            return 4;
        case OpCode::OP_METHOD_CALL:
            return 7;  // opcode(1B) + nameIdx(2B) + argCount(1B) + receiverVarIdx(2B) + receiverLocalSlot(1B)
        case OpCode::OP_WRITEBACK_MEMBER_VAR:
            return 5;  // opcode(1B) + varIdx(2B) + fieldIdx(2B)
        case OpCode::OP_WRITEBACK_MEMBER_LOCAL:
            return 4;  // opcode(1B) + slot(1B) + fieldIdx(2B)
        case OpCode::OP_WRITEBACK_INDEX_VAR:
            return 3;  // opcode(1B) + varIdx(2B)
        case OpCode::OP_WRITEBACK_INDEX_LOCAL:
            return 2;  // opcode(1B) + slot(1B)
        default:
            return 1;
        }
    }

    /// 反汇编：输出字节码文本
    std::string disassemble() const {
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
    std::string disassembleInstruction(size_t& offset) const {
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
        case OpCode::OP_DUP: str += "OP_DUP"; offset += 1; break;
        case OpCode::OP_CLOSURE: {
            uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
            uint8_t argCount = code[offset + 3];
            str += "OP_CLOSURE " + std::to_string(idx) + " (" + constants[idx].stringVal() + ") " + std::to_string(argCount);
            offset += 4;
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
        default:
            str += "OP_UNKNOWN(" + std::to_string(static_cast<int>(op)) + ")";
            offset += 1;
            break;
        }

        return str;
    }
};

/// 编译结果：包含主 chunk 和函数 chunk
struct CompileResult {
    BytecodeChunk mainChunk;
    std::unordered_map<std::string, BytecodeChunk> functionChunks;
};
