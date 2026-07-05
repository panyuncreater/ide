// ============================================================
// RegisterBytecode.cpp — PERF-14: 寄存器式字节码实现
// ============================================================

#include "compiler/RegisterBytecode.h"
#include <cassert>
#include <cstdlib>  // AUDIT-BUG-R1: std::abort 替代 assert（Release 构建保护）

// ============================================================
// A3 fix: 统一 RegOp 元数据表（名称/基础长度/是否变长）
// ------------------------------------------------------------
// 索引与 RegOp enum 严格对齐（REG_LOAD_CONST=0, ..., REG_LOAD_MUTATED=末位）。
// 新增 RegOp 时只需在此表追加一行，无需修改 regOpName/instructionSize/instructionSizeAt。
// regOpName() 已内联到头文件基于此表查询；instructionSize 也改用此表。
// ============================================================
namespace {
constexpr RegOpInfo kRegOpInfo[] = {
    /* REG_LOAD_CONST              */ {"REG_LOAD_CONST",                4, false},
    /* REG_LOAD_NULL               */ {"REG_LOAD_NULL",                 2, false},
    /* REG_LOAD_TRUE               */ {"REG_LOAD_TRUE",                 2, false},
    /* REG_LOAD_FALSE              */ {"REG_LOAD_FALSE",                2, false},
    /* REG_MOVE                    */ {"REG_MOVE",                      3, false},
    /* REG_LOAD_GLOBAL             */ {"REG_LOAD_GLOBAL",               4, false},
    /* REG_STORE_GLOBAL            */ {"REG_STORE_GLOBAL",              4, false},
    /* REG_DEFINE_GLOBAL           */ {"REG_DEFINE_GLOBAL",            4, false},
    /* REG_DELETE_GLOBAL           */ {"REG_DELETE_GLOBAL",            3, false},
    /* REG_LOAD_UPVALUE            */ {"REG_LOAD_UPVALUE",              3, false},  // C-12 fix
    /* REG_STORE_UPVALUE           */ {"REG_STORE_UPVALUE",             3, false},  // C-12 fix
    /* REG_CLOSE_UPVALUE           */ {"REG_CLOSE_UPVALUE",             2, false},
    /* REG_ADD                     */ {"REG_ADD",                       4, false},
    /* REG_SUB                     */ {"REG_SUB",                       4, false},
    /* REG_MUL                     */ {"REG_MUL",                       4, false},
    /* REG_DIV                     */ {"REG_DIV",                       4, false},
    /* REG_MOD                     */ {"REG_MOD",                       4, false},
    /* REG_NEGATE                  */ {"REG_NEGATE",                    3, false},
    /* REG_EQ                      */ {"REG_EQ",                        4, false},
    /* REG_NEQ                     */ {"REG_NEQ",                       4, false},
    /* REG_LT                      */ {"REG_LT",                        4, false},
    /* REG_GT                      */ {"REG_GT",                        4, false},
    /* REG_LTE                     */ {"REG_LTE",                       4, false},
    /* REG_GTE                     */ {"REG_GTE",                       4, false},
    /* REG_NOT                     */ {"REG_NOT",                       3, false},
    /* REG_JUMP                    */ {"REG_JUMP",                      3, false},
    /* REG_JUMP_IF_FALSE           */ {"REG_JUMP_IF_FALSE",             4, false},
    /* REG_RETURN                  */ {"REG_RETURN",                    2, false},
    /* REG_RETURN_NULL             */ {"REG_RETURN_NULL",               1, false},
    /* REG_CALL                    */ {"REG_CALL",                      5, true},   // 变长: 5 + argCount
    /* REG_CALL_EXPR               */ {"REG_CALL_EXPR",                4, true},   // 变长: 4 + argCount
    /* REG_METHOD_CALL             */ {"REG_METHOD_CALL",               6, true},   // 变长: 6 + argCount
    /* REG_MAKE_CLOSURE            */ {"REG_MAKE_CLOSURE",              5, true},   // 变长: 5 + 2*uvCount (op+dst+nameIdx(2B)+uvCount)
    /* REG_BUILD_ARRAY             */ {"REG_BUILD_ARRAY",               3, true},   // 变长: 3 + count
    /* REG_BUILD_DICT              */ {"REG_BUILD_DICT",                3, true},   // 变长: 3 + 2*pairCount
    /* REG_INDEX_GET               */ {"REG_INDEX_GET",                 4, false},
    /* REG_INDEX_SET               */ {"REG_INDEX_SET",                 4, false},
    /* REG_MEMBER_GET              */ {"REG_MEMBER_GET",                5, false},
    /* REG_MEMBER_SET              */ {"REG_MEMBER_SET",                5, false},
    /* REG_CLASS_NEW               */ {"REG_CLASS_NEW",                 5, true},   // 变长: 5 + argCount
    /* REG_DEFINE_CLASS            */ {"REG_DEFINE_CLASS",              7, true},   // 变长: 7 + 字段/方法元数据
    /* REG_INIT_FIELD              */ {"REG_INIT_FIELD",                3, false},
    /* REG_SUPER_CALL              */ {"REG_SUPER_CALL",                8, true},   // 变长: 8 + argCount
    /* REG_SUPER_MEMBER_GET        */ {"REG_SUPER_MEMBER_GET",          5, false},
    /* REG_TRY_BEGIN               */ {"REG_TRY_BEGIN",                 3, false},
    /* REG_TRY_END                 */ {"REG_TRY_END",                   1, false},
    /* REG_THROW                   */ {"REG_THROW",                     2, false},
    /* REG_LOAD_EXCEPTION          */ {"REG_LOAD_EXCEPTION",            2, false},
    /* REG_PRINT                   */ {"REG_PRINT",                     2, false},
    /* REG_WRITEBACK_MEMBER_VAR    */ {"REG_WRITEBACK_MEMBER_VAR",     5, false},
    /* REG_WRITEBACK_MEMBER_LOCAL  */ {"REG_WRITEBACK_MEMBER_LOCAL",   4, false},
    /* REG_WRITEBACK_INDEX_VAR     */ {"REG_WRITEBACK_INDEX_VAR",      3, false},
    /* REG_WRITEBACK_INDEX_LOCAL   */ {"REG_WRITEBACK_INDEX_LOCAL",    2, false},
    /* REG_WRITEBACK_MEMBER_UPVALUE*/ {"REG_WRITEBACK_MEMBER_UPVALUE", 4, false},
    /* REG_WRITEBACK_INDEX_UPVALUE */ {"REG_WRITEBACK_INDEX_UPVALUE",  2, false},
    /* REG_LOAD_MUTATED            */ {"REG_LOAD_MUTATED",             2, false},
    /* REG_TYPE_CHECK              */ {"REG_TYPE_CHECK",               4, false},  // op(1B) + src(1B) + typeAnnotationConstIdx(2B)
};
} // anonymous namespace

/// 获取 RegOp 元数据
/// A3 fix: 单一数据源，替代原 regOpName switch + instructionSize switch 两处维护
const RegOpInfo& getRegOpInfo(RegOp op) {
    auto idx = static_cast<uint8_t>(op);
    static const RegOpInfo kUnknown{"UNKNOWN", 1, false};
    if (idx < sizeof(kRegOpInfo) / sizeof(kRegOpInfo[0])) {
        return kRegOpInfo[idx];
    }
    return kUnknown;
}

// ---- RegBytecodeChunk 实现 ----

uint16_t RegBytecodeChunk::addConstant(const Value& value) {
    // 简单线性扫描去重（常量池通常较小）
    // BUG-CP-1 fix: 类型严格匹配，避免 Value::equals() 跨类型数值相等性
    //（如 Value(0).equals(Value(0.0)) == true）导致 int/float 常量被错误去重。
    for (size_t i = 0; i < constants.size(); ++i) {
        if (constants[i].getType() == value.getType() && constants[i].equals(value)) {
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

void RegBytecodeChunk::writeOp(RegOp op, int line, int column) {
    code.push_back(static_cast<uint8_t>(op));
    while (static_cast<int>(lines.size()) < static_cast<int>(code.size())) {
        lines.push_back(line);
        columns.push_back(column);  // BUG-IBACKEND-2
    }
}

void RegBytecodeChunk::writeReg(uint8_t reg, int line, int column) {
    assert(reg < 32 && "寄存器号超出 32 上限");
    code.push_back(reg);
    while (static_cast<int>(lines.size()) < static_cast<int>(code.size())) {
        lines.push_back(line);
        columns.push_back(column);  // BUG-IBACKEND-2
    }
}

void RegBytecodeChunk::writeShort(uint16_t v, int line, int column) {
    code.push_back(static_cast<uint8_t>(v & 0xFF));
    code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    while (static_cast<int>(lines.size()) < static_cast<int>(code.size())) {
        lines.push_back(line);
        columns.push_back(column);  // BUG-IBACKEND-2
    }
}

void RegBytecodeChunk::writeByte(uint8_t v, int line, int column) {
    code.push_back(v);
    while (static_cast<int>(lines.size()) < static_cast<int>(code.size())) {
        lines.push_back(line);
        columns.push_back(column);  // BUG-IBACKEND-2
    }
}

// ---- 指令长度表 ----
// PERF-14: constexpr 数组查表，与 BytecodeChunk::instructionSize 同模式
// A3 fix: 改用元数据表查表（与 regOpName/isRegOpVariableLength 共享 kRegOpInfo）
size_t RegBytecodeChunk::instructionSize(RegOp op) {
    return getRegOpInfo(op).baseSize;
}

size_t RegBytecodeChunk::instructionSizeAt(size_t offset) const {
    if (offset >= code.size()) return 1;
    RegOp op = static_cast<RegOp>(code[offset]);
    size_t baseSize = instructionSize(op);

    // 变长指令：根据操作数计算实际长度
    switch (op) {
    case RegOp::REG_CALL: {
        // op + dst + nameIdx(2B) + argCount + args
        if (offset + 4 < code.size()) {
            uint8_t argCount = code[offset + 4];
            return static_cast<size_t>(5 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_CALL_EXPR: {
        // op + dst + callee + argCount + args
        if (offset + 3 < code.size()) {
            uint8_t argCount = code[offset + 3];
            return static_cast<size_t>(4 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_METHOD_CALL: {
        // op + dst + obj + methodIdx(2B) + argCount + args
        if (offset + 5 < code.size()) {
            uint8_t argCount = code[offset + 5];
            return static_cast<size_t>(6 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_BUILD_ARRAY: {
        // op + dst + count + elems
        if (offset + 2 < code.size()) {
            uint8_t count = code[offset + 2];
            return static_cast<size_t>(3 + count);
        }
        return baseSize;
    }
    case RegOp::REG_BUILD_DICT: {
        // op + dst + pairCount + (k, v)×pairCount
        if (offset + 2 < code.size()) {
            uint8_t pairCount = code[offset + 2];
            return static_cast<size_t>(3 + static_cast<size_t>(pairCount) * 2);
        }
        return baseSize;
    }
    case RegOp::REG_MAKE_CLOSURE: {
        // op + dst + nameIdx(2B) + uvCount + [isLocal, idx]×uvCount
        if (offset + 4 < code.size()) {
            uint8_t uvCount = code[offset + 4];
            return static_cast<size_t>(5 + static_cast<size_t>(uvCount) * 2);
        }
        return baseSize;
    }
    case RegOp::REG_CLASS_NEW: {
        // op + dst + nameIdx(2B) + argCount + args
        if (offset + 4 < code.size()) {
            uint8_t argCount = code[offset + 4];
            return static_cast<size_t>(5 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_SUPER_CALL: {
        // op + dst + nameIdx(2B) + argCount + recvReg + classIdx(2B) + args
        if (offset + 7 < code.size()) {
            uint8_t argCount = code[offset + 4];
            return static_cast<size_t>(8 + argCount);
        }
        return baseSize;
    }
    case RegOp::REG_DEFINE_CLASS: {
        // C-9 fix: op + nameIdx(2B) + parentIdx(2B) + fieldCount(1B) + [fieldIdx(2B)×F]
        //        + methodCount(1B) + [methodIdx(2B)+funIdx(2B)]×M
        // BUG-INH-1 fix: 每个字段新增 defaultConstIdx(2B)，故字段段为 [fieldIdx(2B)+defaultIdx(2B)]×F
        // BUG-INH-IR-1 fix: 每个字段再新增 exprReg(1B)，故字段段为 [fieldIdx(2B)+defaultIdx(2B)+exprReg(1B)]×F
        // 新布局：op + nameIdx(2B) + parentIdx(2B) + fieldCount(1B)
        //       + [fieldIdx(2B)+defaultIdx(2B)+exprReg(1B)]×F
        //       + methodCount(1B) + [methodIdx(2B)+funIdx(2B)]×M
        // 最小 7B；需读取 fieldCount（offset+5）和 methodCount（offset+6+F*5）
        if (offset + 6 < code.size()) {
            uint8_t fieldCount = code[offset + 5];
            size_t methodCountPos = offset + 6 + static_cast<size_t>(fieldCount) * 5;
            if (methodCountPos < code.size()) {
                uint8_t methodCount = code[methodCountPos];
                return static_cast<size_t>(7 + static_cast<size_t>(fieldCount) * 5
                                           + static_cast<size_t>(methodCount) * 4);
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
