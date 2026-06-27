// ============================================================
// RegisterBytecode.cpp — PERF-14: 寄存器式字节码实现
// ============================================================

#include "compiler/RegisterBytecode.h"
#include <cassert>

// ---- 操作码名称 ----
const char* regOpName(RegOp op) {
    switch (op) {
    case RegOp::REG_LOAD_CONST:         return "REG_LOAD_CONST";
    case RegOp::REG_LOAD_NULL:          return "REG_LOAD_NULL";
    case RegOp::REG_LOAD_TRUE:          return "REG_LOAD_TRUE";
    case RegOp::REG_LOAD_FALSE:         return "REG_LOAD_FALSE";
    case RegOp::REG_MOVE:               return "REG_MOVE";
    case RegOp::REG_LOAD_GLOBAL:        return "REG_LOAD_GLOBAL";
    case RegOp::REG_STORE_GLOBAL:       return "REG_STORE_GLOBAL";
    case RegOp::REG_DEFINE_GLOBAL:      return "REG_DEFINE_GLOBAL";
    case RegOp::REG_DELETE_GLOBAL:      return "REG_DELETE_GLOBAL";
    case RegOp::REG_LOAD_UPVALUE:       return "REG_LOAD_UPVALUE";
    case RegOp::REG_STORE_UPVALUE:      return "REG_STORE_UPVALUE";
    case RegOp::REG_CLOSE_UPVALUE:      return "REG_CLOSE_UPVALUE";
    case RegOp::REG_ADD:                return "REG_ADD";
    case RegOp::REG_SUB:                return "REG_SUB";
    case RegOp::REG_MUL:                return "REG_MUL";
    case RegOp::REG_DIV:                return "REG_DIV";
    case RegOp::REG_MOD:                return "REG_MOD";
    case RegOp::REG_NEGATE:             return "REG_NEGATE";
    case RegOp::REG_EQ:                 return "REG_EQ";
    case RegOp::REG_NEQ:                return "REG_NEQ";
    case RegOp::REG_LT:                 return "REG_LT";
    case RegOp::REG_GT:                 return "REG_GT";
    case RegOp::REG_LTE:                return "REG_LTE";
    case RegOp::REG_GTE:                return "REG_GTE";
    case RegOp::REG_NOT:                return "REG_NOT";
    case RegOp::REG_JUMP:               return "REG_JUMP";
    case RegOp::REG_JUMP_IF_FALSE:      return "REG_JUMP_IF_FALSE";
    case RegOp::REG_RETURN:             return "REG_RETURN";
    case RegOp::REG_RETURN_NULL:        return "REG_RETURN_NULL";
    case RegOp::REG_CALL:               return "REG_CALL";
    case RegOp::REG_CALL_EXPR:          return "REG_CALL_EXPR";
    case RegOp::REG_METHOD_CALL:        return "REG_METHOD_CALL";
    case RegOp::REG_MAKE_CLOSURE:       return "REG_MAKE_CLOSURE";
    case RegOp::REG_BUILD_ARRAY:        return "REG_BUILD_ARRAY";
    case RegOp::REG_BUILD_DICT:         return "REG_BUILD_DICT";
    case RegOp::REG_INDEX_GET:          return "REG_INDEX_GET";
    case RegOp::REG_INDEX_SET:          return "REG_INDEX_SET";
    case RegOp::REG_MEMBER_GET:         return "REG_MEMBER_GET";
    case RegOp::REG_MEMBER_SET:         return "REG_MEMBER_SET";
    case RegOp::REG_CLASS_NEW:          return "REG_CLASS_NEW";
    case RegOp::REG_DEFINE_CLASS:       return "REG_DEFINE_CLASS";
    case RegOp::REG_INIT_FIELD:         return "REG_INIT_FIELD";
    case RegOp::REG_SUPER_CALL:         return "REG_SUPER_CALL";
    case RegOp::REG_SUPER_MEMBER_GET:   return "REG_SUPER_MEMBER_GET";
    case RegOp::REG_TRY_BEGIN:          return "REG_TRY_BEGIN";
    case RegOp::REG_TRY_END:            return "REG_TRY_END";
    case RegOp::REG_THROW:              return "REG_THROW";
    case RegOp::REG_PRINT:              return "REG_PRINT";
    case RegOp::REG_WRITEBACK_MEMBER_VAR:   return "REG_WRITEBACK_MEMBER_VAR";
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL: return "REG_WRITEBACK_MEMBER_LOCAL";
    case RegOp::REG_WRITEBACK_INDEX_VAR:    return "REG_WRITEBACK_INDEX_VAR";
    case RegOp::REG_WRITEBACK_INDEX_LOCAL:  return "REG_WRITEBACK_INDEX_LOCAL";
    case RegOp::REG_WRITEBACK_MEMBER_UPVALUE: return "REG_WRITEBACK_MEMBER_UPVALUE";
    case RegOp::REG_WRITEBACK_INDEX_UPVALUE:  return "REG_WRITEBACK_INDEX_UPVALUE";
    }
    return "UNKNOWN";
}

// ---- RegBytecodeChunk 实现 ----

uint16_t RegBytecodeChunk::addConstant(const Value& value) {
    // 简单线性扫描去重（常量池通常较小）
    for (size_t i = 0; i < constants.size(); ++i) {
        if (constants[i].equals(value)) {
            return static_cast<uint16_t>(i);
        }
    }
    if (constants.size() >= 65535) {
        // 超出索引上限，抛异常（与 BytecodeChunk 行为一致）
        throw std::runtime_error("常量池索引超出 65535 上限");
    }
    constants.push_back(value);
    return static_cast<uint16_t>(constants.size() - 1);
}

void RegBytecodeChunk::writeOp(RegOp op, int line) {
    code.push_back(static_cast<uint8_t>(op));
    while (static_cast<int>(lines.size()) < static_cast<int>(code.size())) {
        lines.push_back(line);
    }
}

void RegBytecodeChunk::writeReg(uint8_t reg, int line) {
    assert(reg < 32 && "寄存器号超出 32 上限");
    code.push_back(reg);
    while (static_cast<int>(lines.size()) < static_cast<int>(code.size())) {
        lines.push_back(line);
    }
}

void RegBytecodeChunk::writeShort(uint16_t v, int line) {
    code.push_back(static_cast<uint8_t>(v & 0xFF));
    code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    while (static_cast<int>(lines.size()) < static_cast<int>(code.size())) {
        lines.push_back(line);
    }
}

void RegBytecodeChunk::writeByte(uint8_t v, int line) {
    code.push_back(v);
    while (static_cast<int>(lines.size()) < static_cast<int>(code.size())) {
        lines.push_back(line);
    }
}

// ---- 指令长度表 ----
// PERF-14: constexpr 数组查表，与 BytecodeChunk::instructionSize 同模式
uint8_t RegBytecodeChunk::instructionSize(RegOp op) {
    // 格式：操作码 1B + 操作数
    //   无操作数：1B
    //   1 reg：2B
    //   2 reg：3B
    //   3 reg：4B
    //   reg + short：4B
    //   short only：3B
    //   变长：REG_CALL/REG_CALL_EXPR/REG_METHOD_CALL/REG_BUILD_ARRAY/REG_BUILD_DICT/REG_MAKE_CLOSURE/REG_CLASS_NEW/REG_SUPER_CALL
    switch (op) {
    // 常量加载
    case RegOp::REG_LOAD_CONST:     return 4;  // op + dst(1B) + constIdx(2B)
    case RegOp::REG_LOAD_NULL:
    case RegOp::REG_LOAD_TRUE:
    case RegOp::REG_LOAD_FALSE:     return 2;  // op + dst(1B)
    case RegOp::REG_MOVE:           return 3;  // op + dst + src

    // 全局变量
    case RegOp::REG_LOAD_GLOBAL:
    case RegOp::REG_STORE_GLOBAL:
    case RegOp::REG_DEFINE_GLOBAL:  return 4;  // op + reg(1B) + slot(2B)
    case RegOp::REG_DELETE_GLOBAL:  return 3;  // op + nameIdx(2B)

    // upvalue
    // C-12 fix: LOAD/STORE_UPVALUE 实际编码为 op+reg(1B)+uvIdx(1B)=3B（与 CLOSE_UPVALUE 的 2B 不同）。
    // 原长度表统一返回 2，导致分发器边界检查少校验 1 字节、buildIpMap 映射错位。
    case RegOp::REG_LOAD_UPVALUE:
    case RegOp::REG_STORE_UPVALUE:  return 3;  // op + reg(1B) + uvIdx(1B)
    case RegOp::REG_CLOSE_UPVALUE:  return 2;  // op + uvIdx(1B)

    // 算术
    case RegOp::REG_ADD:
    case RegOp::REG_SUB:
    case RegOp::REG_MUL:
    case RegOp::REG_DIV:
    case RegOp::REG_MOD:            return 4;  // op + dst + src1 + src2
    case RegOp::REG_NEGATE:         return 3;  // op + dst + src

    // 比较
    case RegOp::REG_EQ:
    case RegOp::REG_NEQ:
    case RegOp::REG_LT:
    case RegOp::REG_GT:
    case RegOp::REG_LTE:
    case RegOp::REG_GTE:            return 4;  // op + dst + src1 + src2
    case RegOp::REG_NOT:            return 3;  // op + dst + src

    // 控制流
    case RegOp::REG_JUMP:           return 3;  // op + offset(2B)
    case RegOp::REG_JUMP_IF_FALSE:  return 4;  // op + src(1B) + offset(2B)
    case RegOp::REG_RETURN:         return 2;  // op + src
    case RegOp::REG_RETURN_NULL:    return 1;  // op

    // 容器
    case RegOp::REG_INDEX_GET:      return 4;  // op + dst + obj + idx
    case RegOp::REG_INDEX_SET:      return 4;  // op + obj + idx + val

    // 成员访问
    case RegOp::REG_MEMBER_GET:     return 5;  // op + dst + obj + fieldIdx(2B)
    case RegOp::REG_MEMBER_SET:     return 5;  // op + obj + fieldIdx(2B) + val

    // 异常
    case RegOp::REG_TRY_BEGIN:      return 3;  // op + catchOffset(2B)
    case RegOp::REG_TRY_END:        return 1;
    case RegOp::REG_THROW:          return 2;  // op + src

    // I/O
    case RegOp::REG_PRINT:          return 2;  // op + src

    // 类
    // C-9 fix: REG_DEFINE_CLASS 现为变长指令（携带父类/字段/方法元数据）。
    // 最小长度 = op(1) + nameIdx(2) + parentIdx(2) + fieldCount(1) + methodCount(1) = 7
    // 实际长度由 instructionSizeAt 按 fieldCount/methodCount 计算。
    case RegOp::REG_DEFINE_CLASS:   return 7;
    case RegOp::REG_INIT_FIELD:     return 3;  // op + fieldIdx(2B)

    // super
    case RegOp::REG_SUPER_MEMBER_GET: return 5;  // op + dst + obj + fieldIdx(2B)

    // 写回
    case RegOp::REG_WRITEBACK_MEMBER_VAR:   return 5;  // op + varIdx(2B) + fieldIdx(2B)
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL: return 4;  // op + localReg + fieldIdx(2B)
    case RegOp::REG_WRITEBACK_INDEX_VAR:    return 3;  // op + varIdx(2B)
    case RegOp::REG_WRITEBACK_INDEX_LOCAL:  return 2;  // op + localReg
    case RegOp::REG_WRITEBACK_MEMBER_UPVALUE: return 4;  // op + uvIdx(1B) + fieldIdx(2B)
    case RegOp::REG_WRITEBACK_INDEX_UPVALUE:  return 2;  // op + uvIdx(1B)

    // 变长指令（返回最小长度，实际长度由 instructionSizeAt 计算）
    case RegOp::REG_CALL:           return 5;  // op + dst(1B) + nameIdx(2B) + argCount(1B) + args...
    case RegOp::REG_CALL_EXPR:      return 4;  // op + dst(1B) + callee(1B) + argCount(1B) + args...
    case RegOp::REG_METHOD_CALL:    return 6;  // op + dst(1B) + obj(1B) + methodIdx(2B) + argCount(1B) + args...
    case RegOp::REG_BUILD_ARRAY:    return 3;  // op + dst(1B) + count(1B) + elems...
    case RegOp::REG_BUILD_DICT:     return 3;  // op + dst(1B) + pairCount(1B) + kvs...
    case RegOp::REG_MAKE_CLOSURE:   return 4;  // op + dst(1B) + nameIdx(2B) + uvCount(1B) + [isLocal,idx]×uvCount
    case RegOp::REG_CLASS_NEW:      return 5;  // op + dst(1B) + nameIdx(2B) + argCount(1B) + args...
    case RegOp::REG_SUPER_CALL:     return 8;  // op + dst(1B) + nameIdx(2B) + argCount(1B) + recvReg(1B) + classIdx(2B) + args...
    }
    return 1;
}

uint8_t RegBytecodeChunk::instructionSizeAt(size_t offset) const {
    if (offset >= code.size()) return 1;
    RegOp op = static_cast<RegOp>(code[offset]);
    uint8_t baseSize = instructionSize(op);

    // 变长指令：根据操作数计算实际长度
    switch (op) {
    case RegOp::REG_CALL: {
        // op + dst + nameIdx(2B) + argCount + args
        if (offset + 4 < code.size()) {
            uint8_t argCount = code[offset + 4];
            return static_cast<uint8_t>(5 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_CALL_EXPR: {
        // op + dst + callee + argCount + args
        if (offset + 3 < code.size()) {
            uint8_t argCount = code[offset + 3];
            return static_cast<uint8_t>(4 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_METHOD_CALL: {
        // op + dst + obj + methodIdx(2B) + argCount + args
        if (offset + 5 < code.size()) {
            uint8_t argCount = code[offset + 5];
            return static_cast<uint8_t>(6 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_BUILD_ARRAY: {
        // op + dst + count + elems
        if (offset + 2 < code.size()) {
            uint8_t count = code[offset + 2];
            return static_cast<uint8_t>(3 + count);
        }
        return baseSize;
    }
    case RegOp::REG_BUILD_DICT: {
        // op + dst + pairCount + (k, v)×pairCount
        if (offset + 2 < code.size()) {
            uint8_t pairCount = code[offset + 2];
            return static_cast<uint8_t>(3 + pairCount * 2);
        }
        return baseSize;
    }
    case RegOp::REG_MAKE_CLOSURE: {
        // op + dst + nameIdx(2B) + uvCount + [isLocal, idx]×uvCount
        if (offset + 4 < code.size()) {
            uint8_t uvCount = code[offset + 4];
            return static_cast<uint8_t>(5 + uvCount * 2);
        }
        return baseSize;
    }
    case RegOp::REG_CLASS_NEW: {
        // op + dst + nameIdx(2B) + argCount + args
        if (offset + 4 < code.size()) {
            uint8_t argCount = code[offset + 4];
            return static_cast<uint8_t>(5 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_SUPER_CALL: {
        // op + dst + nameIdx(2B) + argCount + recvReg + classIdx(2B) + args
        if (offset + 7 < code.size()) {
            uint8_t argCount = code[offset + 4];
            return static_cast<uint8_t>(8 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_DEFINE_CLASS: {
        // C-9 fix: op + nameIdx(2B) + parentIdx(2B) + fieldCount(1B) + [fieldIdx(2B)×F]
        //        + methodCount(1B) + [methodIdx(2B)+funIdx(2B)]×M
        // 最小 7B；需读取 fieldCount（offset+5）和 methodCount（offset+6+F*2）
        if (offset + 6 < code.size()) {
            uint8_t fieldCount = code[offset + 5];
            size_t methodCountPos = offset + 6 + static_cast<size_t>(fieldCount) * 2;
            if (methodCountPos < code.size()) {
                uint8_t methodCount = code[methodCountPos];
                return static_cast<uint8_t>(7 + fieldCount * 2 + methodCount * 4);
            }
        }
        return baseSize;
    }
    default:
        return baseSize;
    }
}

void RegBytecodeChunk::buildIpMap() {
    ipToInstrIndex.clear();
    ipToInstrIndex.resize(code.size() + 1, -1);
    int instrIndex = 0;
    size_t offset = 0;
    while (offset < code.size()) {
        ipToInstrIndex[offset] = instrIndex++;
        offset += instructionSizeAt(offset);
    }
    ipToInstrIndex[code.size()] = instrIndex;
}
