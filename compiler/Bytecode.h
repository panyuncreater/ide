#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "interpreter/Value.h"

// ============================================================
// 字节码指令定义
// ============================================================

/// 操作码枚举
enum class OpCode : uint8_t {
    OP_CONSTANT,     // 加载常量到栈顶
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
    OP_INDEX_GET,    // 索引取值
    OP_INDEX_SET,    // 索引赋值

    OP_MEMBER_GET,   // 成员取值（名称索引）
    OP_MEMBER_SET,   // 成员赋值（名称索引）
    OP_METHOD_CALL,  // 方法调用（名称索引 + 参数个数）

    OP_DUP,          // 复制栈顶
};

/// 字节码块：一段连续的指令
struct BytecodeChunk {
    std::vector<uint8_t> code;       // 指令字节流
    std::vector<Value> constants;    // 常量池
    std::vector<int> lines;         // 每条指令对应的行号

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

    /// 添加常量，返回索引
    uint16_t addConstant(const Value& val) {
        // 检查是否已存在相同常量
        for (uint16_t i = 0; i < static_cast<uint16_t>(constants.size()); ++i) {
            if (constants[i].equals(val)) return i;
        }
        constants.push_back(val);
        return static_cast<uint16_t>(constants.size() - 1);
    }

    /// 获取指令行号
    int getLine(size_t offset) const {
        if (offset < lines.size()) return lines[offset];
        return 0;
    }

    /// 反汇编：输出字节码文本
    std::string disassemble() const {
        std::string result;
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
            str += "OP_INT " + std::to_string(idx) + " (" + std::to_string(constants[idx].intVal) + ")";
            offset += 3;
            break;
        }
        case OpCode::OP_FLOAT: {
            uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
            std::ostringstream oss;
            oss << constants[idx].floatVal;
            str += "OP_FLOAT " + std::to_string(idx) + " (" + oss.str() + ")";
            offset += 3;
            break;
        }
        case OpCode::OP_STRING: {
            uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
            str += "OP_STRING " + std::to_string(idx) + " (\"" + constants[idx].stringVal + "\")";
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
            str += "OP_DEFINE_VAR " + std::to_string(idx) + " (" + constants[idx].stringVal + ")";
            offset += 3;
            break;
        }
        case OpCode::OP_GET_VAR: {
            uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
            str += "OP_GET_VAR " + std::to_string(idx) + " (" + constants[idx].stringVal + ")";
            offset += 3;
            break;
        }
        case OpCode::OP_SET_VAR: {
            uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
            str += "OP_SET_VAR " + std::to_string(idx) + " (" + constants[idx].stringVal + ")";
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
            uint8_t argCount = code[offset + 1];
            str += "OP_CALL " + std::to_string(argCount);
            offset += 2;
            break;
        }
        case OpCode::OP_BUILD_ARRAY: {
            uint8_t count = code[offset + 1];
            str += "OP_BUILD_ARRAY " + std::to_string(count);
            offset += 2;
            break;
        }
        case OpCode::OP_INDEX_GET: str += "OP_INDEX_GET"; offset += 1; break;
        case OpCode::OP_INDEX_SET: str += "OP_INDEX_SET"; offset += 1; break;
        case OpCode::OP_MEMBER_GET: {
            uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
            str += "OP_MEMBER_GET " + std::to_string(idx) + " (" + constants[idx].stringVal + ")";
            offset += 3;
            break;
        }
        case OpCode::OP_MEMBER_SET: {
            uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
            str += "OP_MEMBER_SET " + std::to_string(idx) + " (" + constants[idx].stringVal + ")";
            offset += 3;
            break;
        }
        case OpCode::OP_METHOD_CALL: {
            uint16_t idx = code[offset + 1] | (code[offset + 2] << 8);
            uint8_t argCount = code[offset + 3];
            str += "OP_METHOD_CALL " + std::to_string(idx) + " (" + constants[idx].stringVal + ") " + std::to_string(argCount);
            offset += 4;
            break;
        }
        case OpCode::OP_DUP: str += "OP_DUP"; offset += 1; break;
        default:
            str += "OP_UNKNOWN(" + std::to_string(static_cast<int>(op)) + ")";
            offset += 1;
            break;
        }

        return str;
    }
};
