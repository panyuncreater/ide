#pragma once

// ============================================================
// IR.h - 中间表示层（轻量框架）
// ============================================================
// ARCH-06 fix: 引入 IR 抽象层，打破 Compiler 直接生成字节码的紧耦合。
// 长期目标：AST → IR → 后端（VM 字节码 / 未来 JS / WASM）。
//
// 本次提供轻量框架：
//   1. IROp/IROperand/IRInstruction/IRBasicBlock/IRFunction 数据结构
//   2. IRBuilder 接口（AST → IR 构建器，子类化扩展）
//   3. IRBackend 接口（IR → 目标代码 lowering，子类化扩展）
//   4. BytecodeIRBackend：演示性 IR → 字节码 lowering，验证 IR 表达力
//
// 设计原则：
//   - IR 与现有字节码解耦，可独立扩展为 SSA / Dataflow 等高级形式
//   - 现有 AST→字节码路径保持不变，IR 作为可选中间层
//   - IRBuilder/IRBackend 为抽象接口，便于未来添加新前端（如直接解析 JSON 生成 IR）
//     和新后端（如 JS emit）
// ============================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>
#include "compiler/Bytecode.h"  // IRBackend lowering 到 BytecodeChunk

// ============================================================
// IR 操作数
// ============================================================

/// IR 操作数类型
enum class IROperandKind : uint8_t {
    CONSTANT,   // 常量（int/float/string/bool/null，索引到 IRFunction.constants）
    VIRTUAL,    // 虚拟寄存器（SSA-like，由 IRBuilder 分配， lowering 到栈槽）
    LABEL,      // 基本块标签（跳转目标）
    GLOBAL_NAME,// 全局变量名（索引到 IRFunction.globalNames）
};

/// IR 操作数
struct IROperand {
    IROperandKind kind;
    uint32_t index = 0;  // 由具体 kind 解释：CONSTANT→constants 索引；VIRTUAL→虚拟寄存器号；
                         // LABEL→基本块索引；GLOBAL_NAME→globalNames 索引

    static IROperand constant(uint32_t idx) { return { IROperandKind::CONSTANT, idx }; }
    static IROperand vreg(uint32_t idx) { return { IROperandKind::VIRTUAL, idx }; }
    static IROperand label(uint32_t idx) { return { IROperandKind::LABEL, idx }; }
    static IROperand global(uint32_t idx) { return { IROperandKind::GLOBAL_NAME, idx }; }
};

// ============================================================
// IR 指令
// ============================================================

/// IR 操作码（三地址码风格，与 VM OpCode 解耦）
enum class IROp : uint8_t {
    // 常量加载
    LOAD_CONST,      // dest = constants[idx]    operands: [dest_vreg, const_idx]
    LOAD_GLOBAL,     // dest = globalNames[idx]  operands: [dest_vreg, global_idx]
    STORE_GLOBAL,    // globalNames[idx] = src   operands: [global_idx, src_vreg]

    // 算术运算（三地址码：dest = src1 OP src2）
    ADD, SUB, MUL, DIV, MOD,
    NEGATE,          // dest = -src              operands: [dest, src]

    // 比较
    EQ, NEQ, LT, GT, LTE, GTE,

    // 逻辑（非短路，短路在 AST 层已展开为分支）
    NOT,

    // 控制流
    JUMP,            // jump label               operands: [label_idx]
    JUMP_IF_FALSE,   // if !src jump label       operands: [src_vreg, label_idx]
    LABEL,           // 基本块标记               operands: [label_idx]

    // 调用
    CALL,            // dest = call name(args)   operands: [dest, name_idx, arg_count, arg1, arg2, ...]

    // 返回
    RETURN,          // return src               operands: [src_vreg]

    // 其他
    PRINT,           // print src                operands: [src_vreg]
    POP,             // 释放 src                 operands: [src_vreg]
};

/// IR 指令
struct IRInstruction {
    IROp op;
    std::vector<IROperand> operands;
    int line = 0;  // 源码行号（用于调试）

    IRInstruction(IROp o, std::vector<IROperand> ops, int ln = 0)
        : op(o), operands(std::move(ops)), line(ln) {}
};

// ============================================================
// IR 基本块与函数
// ============================================================

/// IR 基本块（指令序列 + 终结指令）
struct IRBasicBlock {
    std::vector<IRInstruction> instructions;
    uint32_t labelIndex = 0;  // 对应 IRFunction.labels 中的索引
};

/// IR 函数（含 main chunk）
struct IRFunction {
    std::string name;
    std::vector<IRBasicBlock> blocks;
    std::vector<Value> constants;              // 常量池
    std::vector<std::string> globalNames;      // 全局变量名池
    uint32_t nextVReg = 0;                     // 下一个虚拟寄存器号
    uint32_t nextLabel = 0;                    // 下一个标签号

    /// 分配虚拟寄存器
    IROperand allocVReg() { return IROperand::vreg(nextVReg++); }
    /// 分配标签
    uint32_t allocLabel() { return nextLabel++; }
    /// 添加常量，返回索引（已存在则复用）
    uint32_t addConstant(const Value& v) {
        // 简单去重：线性扫描相同值
        for (size_t i = 0; i < constants.size(); ++i) {
            if (constants[i].equals(v)) return static_cast<uint32_t>(i);
        }
        constants.push_back(v);
        return static_cast<uint32_t>(constants.size() - 1);
    }
    /// 添加全局变量名，返回索引
    uint32_t addGlobal(const std::string& name) {
        for (size_t i = 0; i < globalNames.size(); ++i) {
            if (globalNames[i] == name) return static_cast<uint32_t>(i);
        }
        globalNames.push_back(name);
        return static_cast<uint32_t>(globalNames.size() - 1);
    }
};

// ============================================================
// IRBuilder 接口（前端：AST → IR）
// ============================================================

/// IRBuilder 抽象接口。
/// 子类实现具体 AST 节点到 IR 的转换逻辑。
/// 默认实现为空（返回空 IRFunction），由具体子类覆盖。
class IRBuilder {
public:
    virtual ~IRBuilder() = default;
    /// 从 AST 构建 IR。默认实现返回空 IR（演示框架，具体转换由子类实现）。
    virtual std::unique_ptr<IRFunction> build() { return std::make_unique<IRFunction>(); }
};

// ============================================================
// IRBackend 接口（后端：IR → 目标代码）
// ============================================================

/// IRBackend 抽象接口。
/// 子类实现 IR 到具体后端（VM 字节码 / JS / WASM）的 lowering。
class IRBackend {
public:
    virtual ~IRBackend() = default;
    /// 将 IRFunction lowering 为目标代码。返回成功标志。
    virtual bool lower(const IRFunction& ir) = 0;
};

// ============================================================
// BytecodeIRBackend：IR → VM 字节码 lowering（演示性实现）
// ============================================================

/// 将 IRFunction lowering 为 BytecodeChunk。
/// 演示 IR 表达力足够覆盖现有字节码语义。
/// 虚拟寄存器通过栈槽映射：每个 vreg 在栈上占一个槽位，lowering 时维护 vreg→栈深度映射。
class BytecodeIRBackend : public IRBackend {
public:
    BytecodeIRBackend() : chunk_(std::make_unique<BytecodeChunk>()) {}
    ~BytecodeIRBackend() override = default;

    bool lower(const IRFunction& ir) override;

    /// 取生成的字节码 chunk（lower 成功后有效）
    std::unique_ptr<BytecodeChunk> takeChunk() { return std::move(chunk_); }

private:
    std::unique_ptr<BytecodeChunk> chunk_;
    // vreg → 栈深度映射（演示性，简化为线性映射：vreg i → 栈深度 i+1）
    std::unordered_map<uint32_t, uint32_t> vregStackDepth_;
};

// ============================================================
// IR 打印（调试用）
// ============================================================

/// 将 IRFunction 格式化为可读字符串（用于调试和 IR 可视化）
std::string IRToString(const IRFunction& ir);
