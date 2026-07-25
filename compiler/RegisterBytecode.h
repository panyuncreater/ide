#pragma once

// ============================================================
// RegisterBytecode.h — PERF-14: 寄存器式字节码 ISA
// ------------------------------------------------------------
// 与现有栈式 BytecodeChunk 并存，通过 RegisterVM 执行。
// IR 三地址码（dest, src1, src2）直接 lowering 为寄存器式指令，
// 无需栈式序列化（LOAD_CONST × 2 + ADD → REG_ADD r1, r2, r3）。
//
// 寄存器模型：
//   - 每个调用帧 32 个虚拟寄存器（R0-R31），8 位编码
//   - R0..R(arity-1)：参数寄存器
//   - R(arity)..R(localCount-1)：局部变量寄存器
//   - R(localCount)..R31：临时寄存器（IR vreg 分配）
//   - 寄存器窗口大小 = max(localCount, nextVReg)，上限 32
//
// 指令编码（变长但紧凑）：
//   1B op + 按需操作数（每寄存器 1B，常量索引/slot 2B）
//   例：REG_ADD dst(1B) src1(1B) src2(1B) = 4B
//       REG_LOAD_CONST dst(1B) constIdx(2B) = 4B
//       REG_RETURN src(1B) = 2B
// ============================================================

#include "compiler/Bytecode.h" // 复用 UpvalueDesc
#include "interpreter/Value.h"
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
// 寄存器式操作码
// ============================================================
enum class RegOp : uint8_t {
    // ---- 常量加载 ----
    REG_LOAD_CONST, // dst, constIdx(2B)         加载常量池[idx]到 dst
    REG_LOAD_NULL,  // dst                        加载 null
    REG_LOAD_TRUE,  // dst                        加载 true
    REG_LOAD_FALSE, // dst                        加载 false

    // ---- 寄存器间移动 ----
    REG_MOVE, // dst, src                   dst = src

    // ---- 全局变量 ----
    REG_LOAD_GLOBAL,   // dst, slot(2B)              dst = globalSlots[slot]
    REG_STORE_GLOBAL,  // src, slot(2B)              globalSlots[slot] = src
    REG_DEFINE_GLOBAL, // src, slot(2B)              同 STORE_GLOBAL（语义保留）
    REG_DELETE_GLOBAL, // nameConstIdx(2B)           删除全局变量

    // ---- upvalue ----
    REG_LOAD_UPVALUE,  // dst, uvIdx(1B)             dst = upvalues[uvIdx]
    REG_STORE_UPVALUE, // src, uvIdx(1B)             upvalues[uvIdx] = src
    REG_CLOSE_UPVALUE, // uvIdx(1B)                  关闭 upvalue

    // ---- 算术 ----
    REG_ADD,    // dst, src1, src2            dst = src1 + src2
    REG_SUB,    // dst, src1, src2
    REG_MUL,    // dst, src1, src2
    REG_DIV,    // dst, src1, src2
    REG_MOD,    // dst, src1, src2
    REG_NEGATE, // dst, src                   dst = -src

    // ---- 比较 ----
    REG_EQ,  // dst, src1, src2
    REG_NEQ, // dst, src1, src2
    REG_LT,  // dst, src1, src2
    REG_GT,  // dst, src1, src2
    REG_LTE, // dst, src1, src2
    REG_GTE, // dst, src1, src2
    REG_NOT, // dst, src                   dst = !src

    // ---- 控制流 ----
    REG_JUMP,          // offset(2B)                 无条件跳转
    REG_JUMP_IF_FALSE, // src, offset(2B)            src 为假则跳转
    REG_RETURN,        // src                         返回 src
    REG_RETURN_NULL,   // (无操作数)                  返回 null

    // R164 协程/生成器：yield 表达式
    // REG_YIELD dst, src — dst 接收 yield 表达式结果（重放模式 = src），src 提供 yield 值
    REG_YIELD, // dst(1B), src(1B)

    // ---- 调用 ----
    REG_CALL,        // dst, nameIdx(2B), argCount(1B), arg1, arg2, ...  命名函数调用
    REG_CALL_EXPR,   // dst, callee, argCount(1B), arg1, ...             表达式调用
    REG_METHOD_CALL, // dst, obj, methodIdx(2B), argCount(1B), args...  方法调用

    // ---- 闭包 ----
    REG_MAKE_CLOSURE, // dst, nameIdx(2B), uvCount(1B), [isLocal(1B), idx(1B)]×uvCount

    // ---- 容器 ----
    REG_BUILD_ARRAY, // dst, count(1B), elem1, elem2, ...
    REG_BUILD_DICT,  // dst, pairCount(1B), k1, v1, k2, v2, ...
    REG_BUILD_TUPLE, // R98 元组与解构：dst, count(1B), elem1, elem2, ... (immutable)
    REG_INDEX_GET,   // dst, obj, idx
    REG_INDEX_SET,   // obj, idx, val

    // R99 枚举与 ADT + match：enum variant 构造/检查/取字段
    // dst, enumNameConstIdx(2B), variantNameConstIdx(2B), argCount(1B), arg1, arg2, ... (变长 7+argCount)
    REG_BUILD_ENUM_VARIANT,
    // dst, scrut, enumNameConstIdx(2B), variantNameConstIdx(2B) — dest = (scrut 是指定 enum variant)
    REG_ENUM_VARIANT_NAME,
    // dst, scrut, idx — dest = scrut.fields[idx]
    REG_ENUM_VARIANT_FIELD,

    // ---- 成员访问 ----
    REG_MEMBER_GET, // dst, obj, fieldIdx(2B)
    REG_MEMBER_SET, // obj, fieldIdx(2B), val

    // ---- 类 ----
    REG_CLASS_NEW,    // dst, nameIdx(2B), argCount(1B), args...
    REG_DEFINE_CLASS, // nameIdx(2B)
    REG_INIT_FIELD,   // fieldIdx(2B)               记录字段顺序

    // ---- super ----
    REG_SUPER_CALL,       // dst, nameIdx(2B), argCount(1B), recvReg, classIdx(2B), args...
    REG_SUPER_MEMBER_GET, // dst, obj, fieldIdx(2B)

    // ---- 异常 ----
    REG_TRY_BEGIN,      // catchOffset(2B)
    REG_TRY_END,        // (无操作数)
    REG_THROW,          // src
    REG_LOAD_EXCEPTION, // dst                        P1-4 fix: 从 pendingException_ 加载异常值到寄存器

    // ---- I/O ----
    REG_PRINT, // src                         输出 src

    // ---- 写回（嵌套左值变异）----
    REG_WRITEBACK_MEMBER_VAR,   // varIdx(2B), fieldIdx(2B)
    REG_WRITEBACK_MEMBER_LOCAL, // localReg, fieldIdx(2B)
    REG_WRITEBACK_INDEX_VAR,    // varIdx(2B)
    REG_WRITEBACK_INDEX_LOCAL,  // localReg
    // #7 fix: upvalue 写回
    REG_WRITEBACK_MEMBER_UPVALUE, // uvIdx(1B), fieldIdx(2B)
    REG_WRITEBACK_INDEX_UPVALUE,  // uvIdx(1B)
    // MEDIUM-1/2 fix: 读取 lastMutatedReceiverReg_ 到目标寄存器（不清除）
    REG_LOAD_MUTATED, // dst(1B)

    // 2026-06-29: 运行时类型注解检查（三后端统一强制）
    // 操作数: src(1B) + typeAnnotationConstIdx(2B)
    // 语义: 检查 reg[src] 是否兼容类型注解，不匹配则 runtimeError
    REG_TYPE_CHECK, // src(1B), typeAnnotationConstIdx(2B)

    // AUDIT-P1.1 fix: break/continue finally 续跳机制（三后端一致性）
    REG_PUSH_JUMP_TARGET, // push 跳转目标到 pendingJumpStack_（操作数: target(2B)）
    REG_FINALLY_END,      // 从 pendingJumpStack_ pop 目标并跳转；栈空则继续执行（无操作数）

    // R133 模式匹配扩展：获取容器长度（与 OP_LEN 对应，三后端一致）。
    // 操作数: dst(1B) + src(1B)  语义: dst = len(src)
    // 支持 array/dict/string/tuple（与 StackVM OP_LEN / IR LEN 同语义）。
    // 用于 TUPLE pattern 编译期元素数检查。
    REG_LEN,
    // R133 模式匹配扩展：软类型测试（与 OP_TYPE_TEST / IROp::TYPE_TEST 对应）。
    // 操作数: dst(1B) + src(1B) + typeIdx(2B)  语义: dst = typeMatch(src, annotation)
    // 与 REG_TYPE_CHECK 区别：不抛错，写 bool 到 dst。
    // 用于 TUPLE pattern 类型检查（不匹配时 fall through 而非抛错）。
    REG_TYPE_TEST,
};

// ============================================================
// A3 fix: 统一 RegOp 元数据表（名称/基础长度/是否变长）
// ------------------------------------------------------------
// 新增 RegOp 时只需在 RegisterBytecode.cpp 的 kRegOpInfo[] 登记一行，
// 无需同步修改 regOpName / instructionSize / instructionSizeAt 三处 switch。
// ============================================================
struct RegOpInfo {
    const char* name;      // 操作码名称（调试用）
    uint8_t baseSize;      // 固定长度指令字节数；变长指令为最小长度
    bool isVariableLength; // 是否变长（需 instructionSizeAt 按操作数计算实际长度）
};

/// 获取 RegOp 元数据
const RegOpInfo& getRegOpInfo(RegOp op);

/// 寄存器式操作码名称（调试用）
// A3 fix: 改用元数据表查表（基于 getRegOpInfo）
inline const char* regOpName(RegOp op) {
    return getRegOpInfo(op).name;
}

/// 寄存器式操作码 → 是否变长指令
inline bool isRegOpVariableLength(RegOp op) {
    return getRegOpInfo(op).isVariableLength;
}

// ============================================================
// 寄存器式字节码块
// ============================================================
struct RegBytecodeChunk {
    std::vector<uint8_t> code;    // 指令字节流
    std::vector<Value> constants; // 常量池
    std::vector<int> lines;       // 每条指令对应的行号（按字节偏移索引）
    std::vector<int> columns;     // BUG-IBACKEND-2: 每条指令对应的列号（与 lines 平行，默认 0）
    std::string name;             // chunk 名称
    int arity = 0;                // 参数个数
    int requiredArity = 0;        // 必需参数个数
    std::vector<uint16_t> defaultConstIndices;
    std::vector<int> ipToInstrIndex;
    std::vector<std::string> fieldOrder;
    int localCount = 0;    // 局部变量寄存器数量（不含临时寄存器）
    int registerCount = 0; // 总寄存器数量（localCount + 临时寄存器），上限 32
    std::vector<UpvalueDesc> upvalues;
    // BUG-IDE-12 fix: 局部变量寄存器→名称映射（索引即寄存器号）。
    // 用于 RegisterVM 条件断点求值：从当前帧的寄存器反查变量名，注入临时 Interpreter 环境。
    // 限制：槽位复用时（兄弟作用域）后声明的变量名覆盖先前的，属于已知限制。
    std::vector<std::string> localRegNames;
    // L1 fix（2026-07-19）: 基于IP范围的寄存器→名称反查表，解决兄弟作用域寄存器复用导致
    // 的变量名错位。语义同 BytecodeChunk::slotNameRanges，但 slot 字段表示寄存器号。
    struct SlotNameRange {
        uint8_t slot = 0;
        std::string name;
        size_t startIp = 0;
        size_t endIp = 0;
    };
    std::vector<SlotNameRange> slotNameRanges;

    // R164 协程/生成器：标记此 chunk 为生成器函数体（fun* 声明）。
    // RegisterVM 在 REG_CALL 时检测此标志：若为 true，创建协程值而非直接调用。
    bool isGenerator = false;
    // R164 协程/生成器：yield 总数（从 IRFunction.yieldCount 复制）。
    // 静态 yield 数或 kDynamicYieldCount（INT_MAX，表示存在循环内 yield）。
    int yieldCount = 0;
    static constexpr int kDynamicYieldCount = 2147483647; // INT_MAX

    /// L1 fix: 按 ip 反查 reg 对应的变量名。优先在 slotNameRanges 中查找
    /// startIp <= ip < endIp && slot == N 的 range；未命中则回退到 localRegNames。
    const std::string& resolveSlotName(size_t slot, size_t ip) const {
        for (const auto& range : slotNameRanges) {
            if (range.slot == slot && range.startIp <= ip && ip < range.endIp) {
                return range.name;
            }
        }
        if (slot < localRegNames.size()) {
            return localRegNames[slot];
        }
        static const std::string empty;
        return empty;
    }

    RegBytecodeChunk() = default;
    explicit RegBytecodeChunk(const std::string& chunkName, int argCount = 0)
        : name(chunkName), arity(argCount), requiredArity(argCount) {}

    /// 添加常量到常量池（哈希去重），返回索引
    uint16_t addConstant(const Value& value);

    /// 写入操作码
    void writeOp(RegOp op, int line, int column = 0);

    /// 写入单字节寄存器号
    void writeReg(uint8_t reg, int line, int column = 0);

    /// 写入 2 字节小端序
    void writeShort(uint16_t v, int line, int column = 0);

    /// 写入单字节原始值（用于非寄存器编号，如 upvalue 索引）
    void writeByte(uint8_t v, int line, int column = 0);

    /// 构建字节偏移 → 指令索引映射（调试器用）
    void buildIpMap();

    /// 获取操作码的指令长度（字节数）
    // Bug fix: 返回 size_t 而非 uint8_t——变长指令（REG_BUILD_DICT/REG_MAKE_CLOSURE/
    // REG_DEFINE_CLASS）实际长度可超过 255 字节，uint8_t 截断会导致 ip 推进错位
    static size_t instructionSize(RegOp op);

    /// 获取指定偏移处的指令长度（处理变长指令）
    size_t instructionSizeAt(size_t offset) const;

    /// L11 预编译模块：全局槽位重定位（对齐 BytecodeChunk::relocateGlobalSlots）。
    /// 遍历字节码流，将 REG_LOAD_GLOBAL/REG_STORE_GLOBAL/REG_DEFINE_GLOBAL 的
    /// slot 操作数按 relocationMap 重映射。
    /// REG_DELETE_GLOBAL 使用 nameConstIdx（常量池索引）而非 slot，无需重定位。
    /// @param relocationMap[moduleSlot] = mainSlot（-1 表示不重定位）
    void relocateGlobalSlots(const std::vector<int>& relocationMap);
};

// ============================================================
// 寄存器式编译结果
// ============================================================
struct RegisterCompileResult {
    RegBytecodeChunk mainChunk;
    std::map<std::string, RegBytecodeChunk> functionChunks;
    int globalSlotCount = 0;
    std::vector<std::string> globalSlotNames;
    // R99 enum 校验：与 CompileResult.enumInfos 对齐，RegisterVM 启动时加载。
    std::vector<VMEnumInfo> enumInfos;
    // L11 预编译模块：模块导出名称集合（对齐 CompileResult::moduleExports）。
    // 非空时表示此 RegisterCompileResult 是一个独立编译的模块（.minic），
    // 包含模块通过 export 语句导出的所有名称。
    // 用于 import 时具名导入验证 + 命名空间字典构造。
    std::vector<std::string> moduleExports;

    RegisterCompileResult() = default;
    RegisterCompileResult(RegisterCompileResult&&) noexcept = default;
    RegisterCompileResult& operator=(RegisterCompileResult&&) noexcept = default;
    RegisterCompileResult(const RegisterCompileResult&) = default;
    RegisterCompileResult& operator=(const RegisterCompileResult&) = default;
};
