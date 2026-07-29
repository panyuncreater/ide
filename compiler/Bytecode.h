#pragma once

#include "interpreter/Value.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// 字节码指令定义
// ============================================================

/// 操作码枚举
enum class OpCode : uint8_t {
    OP_CONSTANT, // [已废弃] 与 OP_INT/OP_FLOAT/OP_STRING 功能相同，编译器不再生成（D5
                 // fix），保留枚举值避免后续操作码重新编号，VM 仍 dispatch 兼容旧字节码
    OP_INT,      // 加载整数常量
    OP_FLOAT,    // 加载浮点常量
    OP_STRING,   // 加载字符串常量
    OP_NULL,     // 加载 null
    OP_TRUE,     // 加载 true
    OP_FALSE,    // 加载 false

    OP_ADD,      // 加法
    OP_SUBTRACT, // 减法
    OP_MULTIPLY, // 乘法
    OP_DIVIDE,   // 除法
    OP_MODULO,   // 取模
    OP_NEGATE,   // 一元取负
    OP_NOT,      // 逻辑取反

    OP_EQUAL,         // ==
    OP_NOT_EQUAL,     // !=
    OP_LESS,          // <
    OP_GREATER,       // >
    OP_LESS_EQUAL,    // <=
    OP_GREATER_EQUAL, // >=

    OP_AND, // [已废弃/死代码] 编译器从不生成（逻辑与用 JUMP_IF_FALSE 短路实现），保留枚举值避免后续操作码重新编号
    OP_OR,  // [已废弃/死代码] 编译器从不生成（逻辑或用 JUMP_IF_FALSE 短路实现），保留枚举值避免后续操作码重新编号

    OP_PRINT, // 输出栈顶
    OP_POP,   // 弹出栈顶

    OP_DEFINE_VAR, // 定义变量（名称索引在常量池）
    OP_GET_VAR,    // 获取变量值
    OP_SET_VAR,    // 设置变量值
    OP_DELETE_VAR, // 删除变量（用于块作用域退出时清理全局变量）

    OP_JUMP,          // 无条件跳转
    OP_JUMP_IF_FALSE, // 条件为假跳转
    OP_LOOP,          // 回跳（用于循环）

    OP_RETURN,    // 返回
    OP_CALL,      // 函数调用（参数个数）
    OP_CALL_EXPR, // 表达式调用：闭包在参数下方（编译器先push闭包再push参数）（1字节参数个数）

    OP_BUILD_ARRAY,      // 构建数组（元素个数）
    OP_BUILD_DICT,       // 构建字典（键值对个数）
    OP_BUILD_TUPLE,      // R98 元组与解构：构建元组（元素个数，immutable）
    OP_INDEX_GET,        // 索引取值
    OP_INDEX_SET,        // 索引赋值（旧：空操作）
    OP_INDEX_SET_VAR,    // 索引赋值到变量（nameIdx(2B)）：直接修改 globals_[varName]
    OP_INDEX_SET_LOCAL,  // 索引赋值到局部变量（slot(1B)）：直接修改 stack_[bp+slot]
    OP_MEMBER_GET,       // 成员取值（名称索引）
    OP_MEMBER_SET,       // 成员赋值（名称索引）（旧：空操作）
    OP_MEMBER_SET_VAR,   // 成员赋值到变量（nameIdx(2B)+fieldNameIdx(2B)）：直接修改 globals_[varName].fields
    OP_MEMBER_SET_LOCAL, // 成员赋值到局部变量（slot(1B)+fieldNameIdx(2B)）：直接修改 stack_[bp+slot].fields
    OP_METHOD_CALL,      // 方法调用（名称索引 + 参数个数 + 接收者变量名索引）

    OP_DUP,   // 复制栈顶
    OP_DUP_N, // 复制栈中第 N 个值到栈顶（操作数: depth(1B)），用于写回时保留索引值

    // 新增指令
    OP_CLOSURE,      // 创建闭包值（操作数: nameIdx(2B) + upvalueCount(1B) + upvalueDescs[2B each]）
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
    OP_GET_GLOBAL,    // 读取全局槽位（操作数: slot(2B)）
    OP_SET_GLOBAL,    // 写入全局槽位（操作数: slot(2B)）
    OP_DEFINE_GLOBAL, // 定义全局槽位（操作数: slot(2B)）
    OP_DELETE_GLOBAL, // 删除全局槽位（操作数: slot(2B)）

    // VM-05/06: 闭包 upvalue 操作码
    OP_GET_UPVALUE,   // 读取 upvalue（操作数: upvalueIndex(1B)）
    OP_SET_UPVALUE,   // 写入 upvalue（操作数: upvalueIndex(1B)）
    OP_CLOSE_UPVALUE, // 关闭 upvalue（操作数: upvalueIndex(1B)）

    // F11: 异常处理操作码
    OP_TRY_BEGIN, // try 块开始（操作数: catchOffset(2B)）
    OP_TRY_END,   // try 块正常结束（弹出 try 处理器）
    OP_THROW,     // 抛出异常（弹出栈顶值并触发异常传播）

    // #7 fix: upvalue 写回指令（嵌套左值变异 a.b.c=.. 或 a[i]=.. 其中 a 是 upvalue）
    OP_WRITEBACK_MEMBER_UPVALUE, // 成员写回到 upvalue（uvIdx(1B) + fieldIdx(2B)）
    OP_WRITEBACK_INDEX_UPVALUE,  // 索引写回到 upvalue（uvIdx(1B)），索引从栈顶 pop

    // MEDIUM-1/2 fix: 读取 lastMutatedReceiver_ 到栈顶（不清除，供嵌套左值写回链使用）
    OP_LOAD_MUTATED, // push(lastMutatedReceiver_)  无操作数

    // 2026-06-29: 运行时类型注解检查（三后端统一强制）
    // 操作数: typeAnnotationConstIdx(2B) — 常量池中类型注解字符串的索引
    // 语义: peek 栈顶值，检查是否兼容类型注解，不匹配则 runtimeError。不弹栈。
    OP_TYPE_CHECK, // peek(stack_top) vs constants[typeIdx]

    // AUDIT-P1.1 fix: break/continue finally 续跳机制（三后端一致性）
    // break/continue 在 try-finally 内时，先 push 真实跳转目标，再 jump 到 finally 入口；
    // finally 块末尾的 OP_FINALLY_END 从栈取出目标并跳转，实现"先执行 finally 再跳转"。
    OP_PUSH_JUMP_TARGET, // push 跳转目标到 pendingJumpStack_（操作数: target(2B)）
    OP_FINALLY_END,      // 从 pendingJumpStack_ pop 目标并跳转；栈空则继续执行（无操作数）

    // R99 枚举与 ADT + match
    // 操作数: enumNameConstIdx(2B) + variantNameConstIdx(2B) + argCount(1B)
    // 语义: 从栈顶依次 pop argCount 个参数（按声明顺序，最后一个参数在最顶），
    //       构造 EnumVariantData(enumName, variantName, fields) 并 push 到栈顶。
    OP_BUILD_ENUM_VARIANT,
    // 操作数: enumNameConstIdx(2B) + variantNameConstIdx(2B)
    // 语义: pop 栈顶 scrutinee，检查是否为 enum variant 且 enum/variant 名匹配，
    //       匹配 push true，否则 push false。
    OP_ENUM_VARIANT_NAME,
    // 无操作数。语义: pop 栈顶 index，pop 栈顶 scrutinee，push scrutinee.fields[index]。
    //       若 scrutinee 非 enum variant 或 index 越界，runtimeError。
    OP_ENUM_VARIANT_FIELD,
    // R99 match 表达式：交换栈顶两个值（无操作数，1B）。
    // 用于 case body 完成后将 [scrutinee, body_result] 变为 [body_result, scrutinee]，
    // 随后 OP_POP 弹出 scrutinee，留下 body_result 作为 match 结果。
    OP_SWAP,
    // R134 模式匹配扩展：获取容器长度（无操作数，1B）。
    // 语义: pop 栈顶容器（array/dict/string/tuple），push 长度（int）。
    //       用于 TUPLE pattern 编译期元素数检查。
    OP_LEN,
    // R134 模式匹配扩展：软类型测试（与 OP_TYPE_CHECK 区别：不抛错，push bool）。
    // 操作数: typeAnnotationConstIdx(2B) — 常量池中类型注解字符串的索引
    // 语义: pop 栈顶值，typeMatch 检查（含实例继承链），push bool。
    //       用于 TUPLE pattern 类型检查（不匹配时 fall through 而非抛错）。
    OP_TYPE_TEST,

    // R164 协程/生成器：yield 表达式（无操作数，1B）。
    // 语义: pop 栈顶 yield 值，递增运行时 yield 执行计数器。
    //   - 若计数器 == 当前重放目标 yieldId：抛出 YieldSignal(yieldValue)，被 .next() 捕获
    //   - 若计数器 < 目标：push yieldValue 回栈（作为 yield 表达式的结果），继续执行
    //   - 若计数器 > 目标：不可能（计数器从 0 单调递增，target 首次命中即抛出）
    // 注：VM 使用与 Interpreter 相同的重放模式，保证四后端语义一致。
    OP_YIELD,

    // PERF: 类型特化算术操作码（跳过运行时类型检查）
    // 编译器在静态确定两个操作数均为 int 时生成，消除热循环中的分支预测开销。
    OP_ADD_INT_SPEC, // int + int → int（无类型检查）
    OP_SUB_INT_SPEC, // int - int → int（无类型检查）
    OP_MUL_INT_SPEC, // int * int → int（无类型检查）
    OP_LT_INT_SPEC,  // int < int → bool（无类型检查，最常见的循环比较）

    // L18 eng-tailcall: 互递归尾调用 return g(args)。字节布局与 OP_CALL 完全
    // 相同（nameIdx 2B + argCount 1B，共 4 字节），编译器在其后紧跟 OP_RETURN。
    // 运行时：目标命中 functionChunks_ 普通函数且当前帧可复用 → 帧复用 TCO
    // （跳过后随 OP_RETURN）；否则降级为 OP_CALL 语义（返回后执行 OP_RETURN）。
    OP_TAIL_CALL, // nameIdx(2B) + argCount(1B)
};

// ============================================================
// A3 fix: 统一 opcode 元数据表（名称/基础长度/是否变长）
// ------------------------------------------------------------
// 新增 opcode 时只需在此表登记一行，无需同步修改 opCodeName / instructionSize /
// instructionSizeAt 三处 switch（避免遗漏导致 dispatch 错位）。
// 表索引 = static_cast<uint8_t>(OpCode)，与 enum 顺序严格对齐。
// ============================================================
struct OpCodeInfo {
    const char* name;      // 操作码名称（调试用）
    uint8_t baseSize;      // 固定长度指令字节数；变长指令为最小长度
    bool isVariableLength; // 是否变长（需 instructionSizeAt 按操作数计算实际长度）
};

/// 获取 opcode 元数据（name + baseSize + isVariableLength）
const OpCodeInfo& getOpCodeInfo(OpCode op);

/// 操作码 → 名称字符串（基于元数据表，O(1) 查表）
inline const char* opCodeName(OpCode op) {
    return getOpCodeInfo(op).name;
}

/// 操作码 → 是否变长指令
inline bool isOpCodeVariableLength(OpCode op) {
    return getOpCodeInfo(op).isVariableLength;
}

/// VM-05/06: Upvalue 描述符（编译期生成，描述闭包捕获的外层变量）
struct UpvalueDesc {
    int index;        // 捕获变量在调用者帧中的局部变量槽号（isLocal=true）或外层upvalue索引（isLocal=false）
    bool isLocal;     // true=直接捕获外层局部变量, false=透传外层函数的upvalue
    std::string name; // 条件断点修复(R75): upvalue对应的变量名，用于调试器从VM帧反查闭包变量
};

/// 字节码块：一段连续的指令
struct BytecodeChunk {
    std::vector<uint8_t> code;                 // 指令字节流
    std::vector<Value> constants;              // 常量池
    std::vector<int> lines;                    // 每条指令对应的行号
    std::vector<int> columns;                  // BUG-IBACKEND-2: 每条指令对应的列号（与 lines 平行，默认 0）
    std::string name;                          // chunk 名称（函数名）
    int arity = 0;                             // 参数个数（F10: 含默认参数的总数）
    int requiredArity = 0;                     // F10: 必需参数个数（无默认值的前缀参数数量）
    std::vector<uint16_t> defaultConstIndices; // F10: 默认参数值的常量池索引（仅尾部有默认值的参数）
    std::vector<int> ipToInstrIndex;           // 预计算：字节偏移 → 指令索引映射
    std::vector<std::string> fieldOrder;       // 方法所属类的字段声明顺序（用于 OP_METHOD_CALL 栈布局）
    int localCount = 0; // 局部变量总槽位数（含参数/this/字段/方法体内var声明），用于 VM 帧创建时预分配栈空间
    std::vector<UpvalueDesc> upvalues; // VM-05/06: 闭包捕获的 upvalue 描述符列表
    // BUG-IDE-12 fix: 局部变量槽位→名称映射（索引即 slot）。
    // 用于 VM 条件断点求值：从当前帧的栈槽反查变量名，注入临时 Interpreter 环境。
    // 限制：槽位复用时（兄弟作用域）后声明的变量名覆盖先前的，属于已知限制。
    std::vector<std::string> localSlotNames;
    // L1 fix（2026-07-19）: 基于IP范围的槽位→名称反查表，解决兄弟作用域槽位复用导致
    // 的变量名错位。每个 SlotNameRange 记录变量名在哪个 IP 范围 [startIp, endIp) 内
    // 占用 slot。调试器反查时按 frame.ip 在范围内查找，精确到当前执行点。
    // 保留 localSlotNames 作为 fallback（向后兼容，且覆盖无 range 信息的场景）。
    struct SlotNameRange {
        uint8_t slot = 0;
        std::string name;
        size_t startIp = 0;
        size_t endIp = 0;
    };
    std::vector<SlotNameRange> slotNameRanges;

    /// L1 fix: 按 ip 反查 slot 对应的变量名。优先在 slotNameRanges 中查找
    /// startIp <= ip < endIp && slot == N 的 range；未命中则回退到 localSlotNames。
    const std::string& resolveSlotName(size_t slot, size_t ip) const {
        for (const auto& range : slotNameRanges) {
            if (range.slot == slot && range.startIp <= ip && ip < range.endIp) {
                return range.name;
            }
        }
        if (slot < localSlotNames.size()) {
            return localSlotNames[slot];
        }
        static const std::string empty;
        return empty;
    }
    // #11 fix: fieldOrder 的字段名→索引懒缓存。OP_MEMBER_SET_LOCAL 在 slot==0 时
    // 原线性扫描 fieldOrder（每次 this.field=v 都 O(n)）；改为首次访问时建 map，
    // 后续 O(1) 查找。mutable 因访问发生在 const 上下文（VM 执行 const chunk）。
    // 安全性：VM 单线程执行，无数据竞争；BytecodeChunk 拷贝时缓存随之复制且仍与
    // fieldOrder 一致（缓存纯派生自 fieldOrder）。
    mutable std::unordered_map<std::string, size_t> fieldIndexCache_;
    mutable bool fieldIndexCacheBuilt_ = false;

    // P-1 perf: per-chunk 全局变量解析缓存（平坦数组，按常量池索引直接寻址）。
    // 替代 VM 级 globalCache_（unordered_map<const std::string*, ...>），
    // 将每次 OP_GET_VAR/OP_SET_VAR 的哈希查找降为 O(1) 数组下标访问。
    // mutable：执行期在 const chunk 上下文中惰性填充（同 fieldIndexCache_ 模式）。
    struct VarCacheEntry {
        int resolvedSlot = -2;     // >=0: globalSlots_ 下标; -1: 非 slot 变量; -2: 未解析
        Value* valuePtr = nullptr; // globals_ 中的值指针（slot==-1 时有效）
        uint32_t version = 0;      // 缓存写入时的 globalsVersion 快照
    };
    mutable std::vector<VarCacheEntry> varCache_;

    // R164 协程/生成器：标记此 chunk 为生成器函数体（fun* 声明）。
    // VM 在 OP_CALL 时检测此标志：若为 true，不直接 setupFunctionCallFrame，
    // 而是构造 Coroutine 值返回调用方，由 .next() 触发重放执行。
    bool isGenerator = false;
    // R164 协程/生成器：yield 总数（从 FunDecl.yieldCount 复制）。
    // 静态 yield 数（如 3 个 yield 语句 = 3）或 kDynamicYieldCount（INT_MAX，
    // 表示存在循环内 yield，done 由函数体自然结束路径判定）。
    int yieldCount = 0;
    // R164 协程/生成器：动态 yieldCount 标记值（与 FunDecl::kDynamicYieldCount 一致）
    static constexpr int kDynamicYieldCount = 2147483647; // INT_MAX

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
    BytecodeChunk(const BytecodeChunk&) = default;
    BytecodeChunk(BytecodeChunk&&) noexcept = default;
    BytecodeChunk& operator=(const BytecodeChunk&) = default;
    BytecodeChunk& operator=(BytecodeChunk&&) noexcept = default;

    /// C21: 预分配字节码空间，避免编译期间频繁 realloc
    void reserveCode(size_t estimatedBytes) {
        code.reserve(estimatedBytes);
        lines.reserve(estimatedBytes);
        columns.reserve(estimatedBytes);
    }

    /// 追加一个字节
    void write(uint8_t byte, int line, int column = 0) {
        code.push_back(byte);
        lines.push_back(line);
        columns.push_back(column);
    }

    /// 追加一个操作码
    void writeOp(OpCode op, int line, int column = 0) { write(static_cast<uint8_t>(op), line, column); }

    /// 追加一个 16 位操作数（小端序）
    void writeShort(uint16_t value, int line, int column = 0) {
        write(static_cast<uint8_t>(value & 0xFF), line, column);
        write(static_cast<uint8_t>((value >> 8) & 0xFF), line, column);
    }

    /// 添加常量，返回索引（哈希去重，O(1) 均摊）
    /// BUG-CP-1 fix: 必须额外检查 getType() 严格匹配——Value::equals 允许
    /// int/float 跨类型比较（Value(0).equals(Value(0.0)) == true），若仅依赖
    /// equals 会导致 int 0 和 float 0.0 在常量池中错误合并为同一索引，
    /// 后续 OP_INT/OP_FLOAT 加载到的 Value 类型与编译期预期不符，
    /// 触发 toString/format/类型注解等路径的语义错误甚至 VM 崩溃。
    uint16_t addConstant(const Value& val) {
        size_t h = hashValue(val);
        auto& bucket = constantHashMap_[h];
        for (uint16_t i : bucket) {
            if (constants[i].getType() == val.getType() && constants[i].equals(val))
                return i;
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
        case ValueType::VAL_INT:
            return std::hash<int64_t>{}(val.intVal());
        case ValueType::VAL_FLOAT:
            return std::hash<double>{}(val.floatVal());
        case ValueType::VAL_STRING:
            return std::hash<std::string>{}(val.stringVal());
        case ValueType::VAL_BOOL:
            return std::hash<bool>{}(val.boolVal());
        case ValueType::VAL_NULL:
            return 0;
        default:
            return static_cast<size_t>(val.getType());
        }
    }

    std::unordered_map<size_t, std::vector<uint16_t>> constantHashMap_;

public:
    /// 获取指令行号
    int getLine(size_t offset) const {
        if (offset < lines.size())
            return lines[offset];
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
        if (offset >= code.size())
            return 1;
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
                return 1; // 截断，返回 1 避免无限循环
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
    static size_t instructionSize(OpCode op) { return getOpCodeInfo(op).baseSize; }

    /// BUG-IBACKEND-2: 获取指令列号
    int getColumn(size_t offset) const {
        if (offset < columns.size())
            return columns[offset];
        return 0;
    }

    /// 反汇编：输出字节码文本（D9 fix: 实现移至 Bytecode.cpp）
    std::string disassemble() const;

    /// 反汇编单条指令（D9 fix: 实现移至 Bytecode.cpp）
    std::string disassembleInstruction(size_t& offset) const;

    /// P2-11 预编译模块：全局槽位重定位。
    /// 遍历字节码流，将 OP_GET_GLOBAL/OP_SET_GLOBAL/OP_DEFINE_GLOBAL/OP_DELETE_GLOBAL
    /// 的操作数 slot 按 relocationMap 重映射。
    /// @param relocationMap[moduleSlot] = mainSlot（-1 表示不重定位）
    void relocateGlobalSlots(const std::vector<int>& relocationMap);
};

/// R99 enum variant 元信息（编译期→运行时传递，供 VM 校验 OP_BUILD_ENUM_VARIANT）
struct VMEnumVariantInfo {
    std::string name; // variant 名（如 "Red"/"Some"）
    int arity = 0;    // 期望参数数量（无参 variant 为 0）
    // AUDIT-R6 F3 fix: 字段类型注解（对齐 Interpreter::visitEnumVariantExpr 的 typeMatch
    // 校验）。原 VM/RegisterVM 仅校验 arity，E.V("s") 对 V(int) 静默构造成功——三后端不一致。
    std::vector<std::string> paramTypes;
};

/// R99 enum 元信息（编译期→运行时传递）
struct VMEnumInfo {
    std::string name;                        // enum 名（如 "Color"/"Option"）
    std::vector<VMEnumVariantInfo> variants; // variant 列表
    // AUDIT-R6 F3 fix: 泛型类型参数（运行时擦除，字段类型为类型参数时跳过校验，
    // 对齐 Interpreter 的 isTypeParameter 豁免）。
    std::vector<std::string> typeParams;
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
    // R99 enum 校验：编译期收集的 enum 元信息，VM 启动时加载到 enumRegistry_，
    // 供 OP_BUILD_ENUM_VARIANT 校验 variant 名存在性与参数 arity 一致性。
    // 对齐 Interpreter::enumRegistry_ 的运行时校验语义，保证三后端一致。
    std::vector<VMEnumInfo> enumInfos;
    // P2-11 预编译模块：模块导出名称集合。
    // 非空时表示此 CompileResult 是一个独立编译的模块（.minic），
    // 包含模块通过 export 语句导出的所有名称。
    // 用于 import 时具名导入验证 + 命名空间字典构造。
    std::vector<std::string> moduleExports;

    CompileResult() = default;
    CompileResult(CompileResult&&) noexcept = default;
    CompileResult& operator=(CompileResult&&) noexcept = default;
    CompileResult(const CompileResult&) = default;
    CompileResult& operator=(const CompileResult&) = default;
};
