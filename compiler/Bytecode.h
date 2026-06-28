#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include "interpreter/Value.h"

// ============================================================
// 字节码指令定义
// ============================================================

/// 操作码枚举
enum class OpCode : uint8_t {
    OP_CONSTANT,     // [已废弃] 与 OP_INT/OP_FLOAT/OP_STRING 功能相同，编译器不再生成（D5 fix），保留枚举值避免后续操作码重新编号，VM 仍 dispatch 兼容旧字节码
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

    OP_AND,          // [已废弃/死代码] 编译器从不生成（逻辑与用 JUMP_IF_FALSE 短路实现），保留枚举值避免后续操作码重新编号
    OP_OR,           // [已废弃/死代码] 编译器从不生成（逻辑或用 JUMP_IF_FALSE 短路实现），保留枚举值避免后续操作码重新编号

    OP_PRINT,        // 输出栈顶
    OP_POP,          // 弹出栈顶

    OP_DEFINE_VAR,   // 定义变量（名称索引在常量池）
    OP_GET_VAR,      // 获取变量值
    OP_SET_VAR,      // 设置变量值
    OP_DELETE_VAR,   // 删除变量（用于块作用域退出时清理全局变量）

    OP_JUMP,         // 无条件跳转
    OP_JUMP_IF_FALSE,// 条件为假跳转
    OP_LOOP,         // 回跳（用于循环）

    OP_RETURN,       // 返回
    OP_CALL,         // 函数调用（参数个数）
    OP_CALL_EXPR,    // 表达式调用：栈顶为闭包，下方为参数（1字节参数个数）

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
    OP_DUP_N,        // 复制栈中第 N 个值到栈顶（操作数: depth(1B)），用于写回时保留索引值

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

    // O1: super 关键字指令
    OP_SUPER_CALL,       // super 方法调用（格式同 OP_METHOD_CALL，7字节），从父类开始方法查找
    OP_SUPER_MEMBER_GET, // super 成员取值（格式同 OP_MEMBER_GET，3字节），从父类查找

    // A2: 全局变量整数索引指令（编译期分配槽位，运行时 vector 直接访问）
    OP_GET_GLOBAL,       // 读取全局槽位（操作数: slot(2B)）
    OP_SET_GLOBAL,       // 写入全局槽位（操作数: slot(2B)）
    OP_DEFINE_GLOBAL,    // 定义全局槽位（操作数: slot(2B)）
    OP_DELETE_GLOBAL,    // 删除全局槽位（操作数: slot(2B)）

    // VM-05/06: 闭包 upvalue 操作码
    OP_GET_UPVALUE,      // 读取 upvalue（操作数: upvalueIndex(1B)）
    OP_SET_UPVALUE,      // 写入 upvalue（操作数: upvalueIndex(1B)）
    OP_CLOSE_UPVALUE,    // 关闭 upvalue（操作数: upvalueIndex(1B)）

    // F11: 异常处理操作码
    OP_TRY_BEGIN,        // try 块开始（操作数: catchOffset(2B)）
    OP_TRY_END,          // try 块正常结束（弹出 try 处理器）
    OP_THROW,            // 抛出异常（弹出栈顶值并触发异常传播）

    // #7 fix: upvalue 写回指令（嵌套左值变异 a.b.c=.. 或 a[i]=.. 其中 a 是 upvalue）
    OP_WRITEBACK_MEMBER_UPVALUE,  // 成员写回到 upvalue（uvIdx(1B) + fieldIdx(2B)）
    OP_WRITEBACK_INDEX_UPVALUE,   // 索引写回到 upvalue（uvIdx(1B)），索引从栈顶 pop
};

// ============================================================
// A3 fix: 统一 opcode 元数据表（名称/基础长度/是否变长）
// ------------------------------------------------------------
// 新增 opcode 时只需在此表登记一行，无需同步修改 opCodeName / instructionSize /
// instructionSizeAt 三处 switch（避免遗漏导致 dispatch 错位）。
// 表索引 = static_cast<uint8_t>(OpCode)，与 enum 顺序严格对齐。
// ============================================================
struct OpCodeInfo {
    const char* name;            // 操作码名称（调试用）
    uint8_t baseSize;            // 固定长度指令字节数；变长指令为最小长度
    bool isVariableLength;        // 是否变长（需 instructionSizeAt 按操作数计算实际长度）
};

/// 获取 opcode 元数据（name + baseSize + isVariableLength）
const OpCodeInfo& getOpCodeInfo(OpCode op);

/// 操作码 → 名称字符串（基于元数据表，O(1) 查表）
inline const char* opCodeName(OpCode op) { return getOpCodeInfo(op).name; }

/// 操作码 → 是否变长指令
inline bool isOpCodeVariableLength(OpCode op) { return getOpCodeInfo(op).isVariableLength; }

/// VM-05/06: Upvalue 描述符（编译期生成，描述闭包捕获的外层变量）
struct UpvalueDesc {
    int index;     // 捕获变量在调用者帧中的局部变量槽号（isLocal=true）或外层upvalue索引（isLocal=false）
    bool isLocal;  // true=直接捕获外层局部变量, false=透传外层函数的upvalue
};

/// 字节码块：一段连续的指令
struct BytecodeChunk {
    std::vector<uint8_t> code;       // 指令字节流
    std::vector<Value> constants;    // 常量池
    std::vector<int> lines;         // 每条指令对应的行号
    std::string name;               // chunk 名称（函数名）
    int arity = 0;                  // 参数个数（F10: 含默认参数的总数）
    int requiredArity = 0;          // F10: 必需参数个数（无默认值的前缀参数数量）
    std::vector<uint16_t> defaultConstIndices;  // F10: 默认参数值的常量池索引（仅尾部有默认值的参数）
    std::vector<int> ipToInstrIndex; // 预计算：字节偏移 → 指令索引映射
    std::vector<std::string> fieldOrder; // 方法所属类的字段声明顺序（用于 OP_METHOD_CALL 栈布局）
    int localCount = 0;              // 局部变量总槽位数（含参数/this/字段/方法体内var声明），用于 VM 帧创建时预分配栈空间
    std::vector<UpvalueDesc> upvalues; // VM-05/06: 闭包捕获的 upvalue 描述符列表
    // #11 fix: fieldOrder 的字段名→索引懒缓存。OP_MEMBER_SET_LOCAL 在 slot==0 时
    // 原线性扫描 fieldOrder（每次 this.field=v 都 O(n)）；改为首次访问时建 map，
    // 后续 O(1) 查找。mutable 因访问发生在 const 上下文（VM 执行 const chunk）。
    // 安全性：VM 单线程执行，无数据竞争；BytecodeChunk 拷贝时缓存随之复制且仍与
    // fieldOrder 一致（缓存纯派生自 fieldOrder）。
    mutable std::unordered_map<std::string, size_t> fieldIndexCache_;
    mutable bool fieldIndexCacheBuilt_ = false;

    /// 返回 fieldName 在 fieldOrder 中的索引，未找到返回 SIZE_MAX
    size_t fieldSlotIndex(const std::string& fieldName) const {
        if (!fieldIndexCacheBuilt_) {
            for (size_t i = 0; i < fieldOrder.size(); ++i) {
                fieldIndexCache_[fieldOrder[i]] = i;
            }
            fieldIndexCacheBuilt_ = true;
        }
        auto it = fieldIndexCache_.find(fieldName);
        return it != fieldIndexCache_.end() ? it->second : SIZE_MAX;
    }

    BytecodeChunk() = default;
    explicit BytecodeChunk(const std::string& chunkName, int argCount = 0)
        : name(chunkName), arity(argCount), requiredArity(argCount) {}

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
            // D7 fix: 统一使用 instructionSizeAt 处理变长指令
            offset += instructionSizeAt(offset);
            instrIdx++;
        }
    }

    /// D7 fix: 获取指定偏移处指令的实际长度（字节数）。
    /// OP_CLOSURE 是唯一变长指令（4 + 2*upvalueCount 字节），
    /// 此方法封装变长逻辑，消除 buildIpMap/ide 遍历/TestCompiler 遍历 3 处特例处理。
    /// 若偏移越界或指令截断，返回 1 避免无限循环。
    // A3 fix: 通过 isOpCodeVariableLength 元数据判断变长，无需硬编码 OP_CLOSURE 特例
    size_t instructionSizeAt(size_t offset) const {
        if (offset >= code.size()) return 1;
        OpCode op = static_cast<OpCode>(code[offset]);
        if (isOpCodeVariableLength(op)) {
            // 变长指令按 opcode 分派计算实际长度
            switch (op) {
            case OpCode::OP_CLOSURE: {
                // OP_CLOSURE(1) + nameIdx(2) + upvalueCount(1) + upvalueCount*2
                if (offset + 3 < code.size()) {
                    uint8_t upvalueCount = code[offset + 3];
                    return 4 + static_cast<size_t>(upvalueCount) * 2;
                }
                return 1;  // 截断，返回 1 避免无限循环
            }
            default:
                // 元数据标记为变长但未实现实际长度计算的指令——返回 baseSize 兜底
                return instructionSize(op);
            }
        }
        return instructionSize(op);
    }

    /// 获取操作码对应的指令长度（字节数）
    /// A3 fix: 改用元数据表查表（与 opCodeName/isOpCodeVariableLength 共享 kOpCodeInfo）
    static size_t instructionSize(OpCode op) {
        return getOpCodeInfo(op).baseSize;
    }

    /// 反汇编：输出字节码文本（D9 fix: 实现移至 Bytecode.cpp）
    std::string disassemble() const;

    /// 反汇编单条指令（D9 fix: 实现移至 Bytecode.cpp）
    std::string disassembleInstruction(size_t& offset) const;
};

/// 编译结果：包含主 chunk 和函数 chunk
struct CompileResult {
    BytecodeChunk mainChunk;
    // MEM-03/MEM-04 fix: 改用 std::map 保证节点稳定性，使 VM 持有的
    // VMClosureData::chunkPtr 和 VMCallFrame::chunk 裸指针在后续插入时不悬垂。
    std::map<std::string, BytecodeChunk> functionChunks;
    // A2: 全局变量槽位映射（编译器→VM）
    int globalSlotCount = 0;
    std::vector<std::string> globalSlotNames;

    CompileResult() = default;
    CompileResult(CompileResult&&) noexcept = default;
    CompileResult& operator=(CompileResult&&) noexcept = default;
    CompileResult(const CompileResult&) = default;
    CompileResult& operator=(const CompileResult&) = default;
};
