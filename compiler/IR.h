/**
 * @file compiler/IR.h
 * @brief 中间表示层（ARCH-06 完整实现）。
 *
 * ARCH-06 fix: 引入 IR 抽象层，打破 Compiler 直接生成字节码的紧耦合。
 * 长期目标：AST → IR → 后端（VM 字节码 / 寄存器字节码 / 未来 JS / WASM）。
 *
 * 数据结构层次：
 *   - IROp：IR 指令操作码（约 50 种，覆盖赋值/算术/比较/跳转/调用/
 *     返回/闭包/类/异常/模块等）
 *   - IROperand：操作数（vreg/常量/标签/全局槽位/字段名等）
 *   - IRInstruction：单条 IR 指令（op + operands + 行号）
 *   - IRBasicBlock：基本块（label + 指令列表 + 后继块）
 *   - IRFunction：函数 IR（基本块列表 + vreg 分配器 + 字段映射）
 *   - IRModule：模块 IR（函数表 + 全局变量 + 类信息 + 模块依赖）
 *
 * 接口层：
 *   - IRBuilder：抽象接口，将 AST 转换为 IRModule
 *   - AstIRBuilder：具体实现，AST → IRModule
 *   - IRBackend：抽象接口，将 IRModule 转换为目标后端字节码
 *   - BytecodeIRBackend：IR → 栈式 VM 字节码 BytecodeChunk
 *   - RegisterBytecodeBackend：IR → 寄存器式 VM 字节码 RegBytecodeChunk
 *
 * IR 优化 pass（C4 增强）：
 *   - constantFoldingPass：常量折叠（编译期可计算的算术/比较）
 *   - copyPropagationPass：复制传播（vreg_a = vreg_b 后续用 vreg_b 替代 vreg_a）
 *   - deadCodeEliminationPass：死代码消除（无副作用指令的 dest vreg 无引用则删除）
 *   - commonSubexpressionEliminationPass：公共子表达式消除（基本块内值编号）
 *   - loopUnrollingPass：循环展开（for 循环体 ≤20 指令且 N≤4 时展开）
 *   - optimizeIR：组合 pass，按 round 重复执行直到收敛（最多 3 轮）
 *
 * 设计原则：
 *   - IR 与现有字节码解耦，可独立扩展为 SSA / Dataflow 等高级形式
 *   - 现有 AST→字节码路径保持不变，IR 作为可选中间层（Compiler::setUseIR(true)）
 *   - IRBuilder/IRBackend 为抽象接口，便于未来添加新前端和新后端
 *
 * 安全约束：
 *   - CSE 默认 false（栈式 VM 后端不安全，CSE 替换 dest vreg 引用后
 *     原指令变为死指令但仍 emit，导致栈上残留未被 POP 的值，与
 *     BUG-IR-DCE-1 同源问题）。仅寄存器式后端可显式传 true 启用。
 *   - DCE 不能删除算术指令（除零/溢出副作用），仅删除无副作用的
 *     纯赋值/拷贝指令。
 *
 * @see Compiler AstIRBuilder BytecodeIRBackend RegisterBytecodeBackend
 */
#pragma once

// ============================================================
// IR.h - 中间表示层（ARCH-06 完整实现）
// ============================================================
// ARCH-06 fix: 引入 IR 抽象层，打破 Compiler 直接生成字节码的紧耦合。
// 长期目标：AST → IR → 后端（VM 字节码 / 未来 JS / WASM）。
//
// 本次实现：
//   1. IROp/IROperand/IRInstruction/IRBasicBlock/IRFunction 数据结构
//   2. IRBuilder 接口 + AstIRBuilder 具体实现（AST → IR）
//   3. IRBackend 接口 + BytecodeIRBackend 具体实现（IR → VM 字节码）
//   4. IRToString 调试输出
//
// 设计原则：
//   - IR 与现有字节码解耦，可独立扩展为 SSA / Dataflow 等高级形式
//   - 现有 AST→字节码路径保持不变，IR 作为可选中间层（Compiler::setUseIR(true)）
//   - IRBuilder/IRBackend 为抽象接口，便于未来添加新前端和新后端
// ============================================================

#include <cstdint>
#include <cstring>  // BUG-AUDIT-VAL-1: std::memcpy for scalarKey float 位模式编码
#include <memory>
#include <functional>  // VM-IMPORT: std::function for moduleLoader_
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>  // perf3 fix: addGlobal/addConstant hash 侧表
#include <unordered_set>
#include "compiler/Bytecode.h"  // IRBackend lowering 到 BytecodeChunk + UpvalueDesc
#include "compiler/GlobalSlotAllocator.h"  // B4: 全局槽位分配器
#include "ast/ASTNode.h"  // VM-IMPORT: Block 完整定义（moduleAsts_ 需要 unique_ptr<Block> 析构）

// ============================================================
// IR 操作数
// ============================================================

/// IR 操作数类型
enum class IROperandKind : uint8_t {
    CONSTANT,     // 常量（int/float/string/bool/null，索引到 IRFunction.constants）
    VIRTUAL,      // 虚拟寄存器（SSA-like，由 IRBuilder 分配，lowering 到栈槽）
    LABEL,        // 基本块标签（跳转目标）
    GLOBAL_NAME,  // 全局变量名（索引到 IRFunction.globalNames）
    LOCAL_SLOT,   // 局部变量槽位（函数内的栈槽编号）
    UPVALUE_IDX,  // upvalue 索引（闭包捕获描述符）
    FIELD_NAME,   // 字段名（索引到 IRFunction.globalNames，复用名称池）
    FUNC_NAME,    // 函数名（索引到 IRFunction.globalNames，复用名称池）
    IMM_UINT,     // 立即数（uint32_t，用于 arg_count/slot 等）
};

/// IR 操作数
struct IROperand {
    IROperandKind kind;
    uint32_t index = 0;  // 由具体 kind 解释

    static IROperand constant(uint32_t idx) { return { IROperandKind::CONSTANT, idx }; }
    static IROperand vreg(uint32_t idx) { return { IROperandKind::VIRTUAL, idx }; }
    static IROperand label(uint32_t idx) { return { IROperandKind::LABEL, idx }; }
    static IROperand global(uint32_t idx) { return { IROperandKind::GLOBAL_NAME, idx }; }
    static IROperand local(uint32_t slot) { return { IROperandKind::LOCAL_SLOT, slot }; }
    static IROperand upvalue(uint32_t idx) { return { IROperandKind::UPVALUE_IDX, idx }; }
    static IROperand field(uint32_t idx) { return { IROperandKind::FIELD_NAME, idx }; }
    static IROperand funcName(uint32_t idx) { return { IROperandKind::FUNC_NAME, idx }; }
    static IROperand imm(uint32_t val) { return { IROperandKind::IMM_UINT, val }; }
};

// ============================================================
// IR 指令
// ============================================================

/// IR 操作码（三地址码风格，与 VM OpCode 解耦）
/// 覆盖 MiniLang 所有 AST 节点语义，支持完整 AST → IR → Bytecode 管线
enum class IROp : uint8_t {
    // ---- 常量加载 ----
    LOAD_CONST,      // dest = constants[idx]       operands: [dest_vreg, const_idx]
    LOAD_NULL,       // dest = null                 operands: [dest_vreg]
    LOAD_TRUE,       // dest = true                 operands: [dest_vreg]
    LOAD_FALSE,      // dest = false                operands: [dest_vreg]

    // ---- 变量访问（三态：local/global/upvalue/name）----
    LOAD_LOCAL,      // dest = local[slot]          operands: [dest_vreg, slot]
    STORE_LOCAL,     // local[slot] = src           operands: [slot, src_vreg]
    LOAD_GLOBAL,     // dest = globalNames[idx]     operands: [dest_vreg, global_idx]
    STORE_GLOBAL,    // globalNames[idx] = src      operands: [global_idx, src_vreg]
    DEFINE_GLOBAL,   // define globalNames[idx] = src  operands: [global_idx, src_vreg]
    DELETE_VAR,      // delete global var by name   operands: [global_idx]  BUG-IR-TRY-1 fix
    LOAD_UPVALUE,    // dest = upvalue[idx]         operands: [dest_vreg, uv_idx]
    STORE_UPVALUE,   // upvalue[idx] = src          operands: [uv_idx, src_vreg]
    CLOSE_UPVALUE,   // close all open upvalues with slot >= slot_base  operands: [slot_base]

    // ---- 算术运算（三地址码：dest = src1 OP src2）----
    ADD, SUB, MUL, DIV, MOD,
    NEGATE,          // dest = -src                 operands: [dest, src]

    // ---- 比较 ----
    EQ, NEQ, LT, GT, LTE, GTE,

    // ---- 逻辑（非短路，短路在 AST 层已展开为分支）----
    NOT,

    // ---- 控制流 ----
    JUMP,            // jump label                  operands: [label_idx]
    JUMP_IF_FALSE,   // if !src jump label          operands: [src_vreg, label_idx]
    LABEL,           // 基本块标记                   operands: [label_idx]

    // ---- 调用 ----
    CALL,            // dest = call name(args)      operands: [dest, name_idx, arg_count, arg1, arg2, ...]
    CALL_EXPR,       // dest = call(closure, args)  operands: [dest, closure_vreg, arg_count, arg1, ...]
    RETURN,          // return src                  operands: [src_vreg]
    RETURN_NULL,     // return null（隐式返回）

    // ---- 闭包 ----
    MAKE_CLOSURE,    // dest = closure(name, upvalues)  operands: [dest, name_idx, uv_count, uv1_isLocal, uv1_idx, ...]

    // ---- 容器 ----
    BUILD_ARRAY,     // dest = [args...]            operands: [dest, count, arg1, arg2, ...]
    BUILD_DICT,      // dest = {k1:v1, k2:v2, ...}  operands: [dest, pair_count, k1, v1, k2, v2, ...]
    INDEX_GET,       // dest = obj[idx]             operands: [dest, obj_vreg, idx_vreg]
    INDEX_SET,       // obj[idx] = val              operands: [obj_vreg, idx_vreg, val_vreg]

    // ---- 成员访问 ----
    MEMBER_GET,      // dest = obj.field            operands: [dest, obj_vreg, field_idx]
    MEMBER_SET,      // obj.field = val             operands: [obj_vreg, field_idx, val_vreg]
    SUPER_MEMBER_GET, // dest = super.field         operands: [dest, this_vreg, field_idx]

    // ---- 方法调用 ----
    METHOD_CALL,     // dest = obj.method(args)     operands: [dest, obj_vreg, method_idx, arg_count, args...]
    SUPER_CALL,      // dest = super.method(args)   operands: [dest, this_vreg, method_idx, class_idx, arg_count, args...]

    // ---- 类 ----
    DEFINE_CLASS,    // define class name           operands: [name_idx]
    CLASS_NEW,       // dest = new ClassName(args)  operands: [dest, name_idx, arg_count, args...]
    INIT_FIELD,      // init field name from stack  operands: [field_idx]

    // ---- 异常处理 ----
    TRY_BEGIN,       // try block begin             operands: [catch_label_idx]
    TRY_END,         // try block end
    THROW,           // throw src                   operands: [src_vreg]
    LOAD_EXCEPTION,  // dest = pendingException     operands: [dest_vreg]  P1-4 fix: catch 块起始加载异常值

    // ---- 写回指令（嵌套左值变异，B1/B6 fix）----
    // 当左值为 a.b.c 或 a[i] 时，变异结果需写回到原始变量/字段
    WRITEBACK_MEMBER_VAR,    // 成员写回到全局变量   operands: [var_idx, field_idx]
    WRITEBACK_MEMBER_LOCAL,  // 成员写回到局部变量   operands: [slot, field_idx]
    WRITEBACK_INDEX_VAR,     // 索引写回到全局变量   operands: [var_idx]
    WRITEBACK_INDEX_LOCAL,   // 索引写回到局部变量   operands: [slot]
    // #7 fix: upvalue 写回（嵌套左值变异 a[i]=.. / a.f=.. 其中 a 是 upvalue）
    WRITEBACK_MEMBER_UPVALUE, // 成员写回到 upvalue  operands: [uv_idx, field_idx]
    WRITEBACK_INDEX_UPVALUE,  // 索引写回到 upvalue  operands: [uv_idx]

    // ---- 其他 ----
    PRINT,           // print src                   operands: [src_vreg]
    POP,             // 释放 src                    operands: [src_vreg]
    DUP,             // 复制 src 到 dest             operands: [dest, src_vreg]
    LOAD_MUTATED,    // dest = lastMutatedReceiver_  operands: [dest_vreg]
                     // MEDIUM-1/2 fix: 嵌套左值写回链中读取上一级 SET 产生的变异后容器。
                     // 不清除 lastMutatedReceiver_，后续 SET/WRITEBACK 会覆盖。

    // 2026-06-29: 运行时类型注解检查
    // operands: [src_vreg, type_const_idx]  type_const_idx 为 CONSTANT 类型（字符串注解）
    // lower 到 OP_TYPE_CHECK（栈式）/ REG_TYPE_CHECK（寄存器式）
    TYPE_CHECK,
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
    std::vector<std::string> globalNames;      // 全局变量名池（复用存储字段名/函数名）
    // perf3 fix: hash 侧表加速 addGlobal/addConstant 去重（O(n²)→O(n)）。
    // 仅经 addGlobal/addConstant 维护，constants/globalNames 不被外部直接修改，保持一致。
    std::unordered_map<std::string, uint32_t> globalNameIdx_;
    std::unordered_map<std::string, uint32_t> stringConstIdx_;
    // P2-1 fix: 非字符串常量（int/float/bool）hash 侧表。key 编码方式见 scalarKey()
    std::unordered_map<std::string, uint32_t> scalarConstIdx_;
    uint32_t nextVReg = 0;                     // 下一个虚拟寄存器号
    uint32_t nextLabel = 0;                    // 下一个标签号

    // 函数元数据（lowering 时使用）
    int arity = 0;                  // 参数个数
    int requiredArity = 0;          // 必需参数个数
    int localCount = 0;             // 局部变量总槽位数
    std::vector<uint16_t> defaultConstIndices;  // 默认参数值的常量索引
    std::vector<UpvalueDesc> upvalues;           // 闭包 upvalue 描述符列表
    // BUG-IDE-12 fix: 局部变量槽位→名称映射（索引即 slot），供 RegisterVM 条件断点求值反查。
    // 由 AstIRBuilder 在分配 LOCAL slot 时增量维护，函数最终化时复制到 ir_->localSlotNames。
    // 限制：槽位复用（兄弟作用域）时后声明的变量名覆盖先前的，属于已知限制。
    std::vector<std::string> localSlotNames;

    /// 分配虚拟寄存器
    IROperand allocVReg() { return IROperand::vreg(nextVReg++); }
    /// 分配标签
    uint32_t allocLabel() { return nextLabel++; }
    /// P2-1 fix: 将 int/float/bool 标量常量编码为字符串 key（含类型标识避免 int 1 == bool true 等误判）
    /// BUG-AUDIT-VAL-1 fix: float 改用位模式编码，避免 std::to_string(double) 仅 6 位小数导致
    /// 不同位模式的 double（如 0.1000001 与 0.1000002）被错误合并为同一常量。
    static std::string scalarKey(const Value& v) {
        if (v.isInt())   return "I:" + std::to_string(v.intVal());
        if (v.isFloat()) {
            double d = v.floatVal();
            uint64_t bits;
            std::memcpy(&bits, &d, sizeof(double));
            return "F:" + std::to_string(bits);
        }
        if (v.isBool())  return v.boolVal() ? "B:1" : "B:0";
        return {};  // 不会触达
    }
    /// 添加常量，返回索引（已存在则复用）
    uint32_t addConstant(const Value& v) {
        // P2-1 fix: 字符串与非字符串常量都用 hash 侧表 O(1) 查找。
        // 字符串：以 stringVal() 为 key（O(n) 内容比较的 equals 在大常量池下退化）
        // 数值/布尔：以 std::pair<int64_t,double>+类型 标识为 key，避免线性扫描。
        // null/Instance/Array/Dict 等复杂类型：保留线性扫描（此类常量极少出现）
        if (v.isString()) {
            auto it = stringConstIdx_.find(v.stringVal());
            if (it != stringConstIdx_.end()) return it->second;
        } else if (v.isInt() || v.isFloat() || v.isBool()) {
            auto it = scalarConstIdx_.find(scalarKey(v));
            if (it != scalarConstIdx_.end()) return it->second;
        } else {
            for (size_t i = 0; i < constants.size(); ++i) {
                if (constants[i].equals(v)) return static_cast<uint32_t>(i);
            }
        }
        // 与 BytecodeChunk/RegBytecodeChunk 一致：常量池索引需 fit 到 uint16_t，
        // 超限抛异常以避免调用方 static_cast<uint16_t> 静默截断。
        if (constants.size() >= 65535) {
            throw std::runtime_error("IR 常量池索引超出 65535 上限");
        }
        constants.push_back(v);
        uint32_t idx = static_cast<uint32_t>(constants.size() - 1);
        if (v.isString()) {
            stringConstIdx_[v.stringVal()] = idx;
        } else if (v.isInt() || v.isFloat() || v.isBool()) {
            scalarConstIdx_[scalarKey(v)] = idx;
        }
        return idx;
    }
    /// 添加全局变量名/字段名/函数名，返回索引
    uint32_t addGlobal(const std::string& name) {
        // perf3 fix: hash 侧表 O(1) 查找替代 O(n) 线性扫描
        auto it = globalNameIdx_.find(name);
        if (it != globalNameIdx_.end()) return it->second;
        // P2-2 fix: 与 addConstant 对齐——globalNames 索引经 writeShort 编码为 uint16_t，
        // 超限抛异常以避免调用方 static_cast<uint16_t> 静默截断（读写错误全局名）。
        if (globalNames.size() >= 65535) {
            throw std::runtime_error("IR 全局名池索引超出 65535 上限");
        }
        globalNames.push_back(name);
        uint32_t idx = static_cast<uint32_t>(globalNames.size() - 1);
        globalNameIdx_[name] = idx;
        return idx;
    }
    /// 添加基本块，返回引用
    IRBasicBlock& addBlock(uint32_t labelIdx) {
        blocks.push_back({});
        blocks.back().labelIndex = labelIdx;
        return blocks.back();
    }
};

/// IR 模块（包含 main 函数和所有子函数）
/// 用于多函数 lowering：AstIRBuilder 收集所有函数 IR，BytecodeIRBackend 逐个 lowering
struct IRModule {
    std::unique_ptr<IRFunction> mainFunction;           // 主函数（顶层代码）
    std::vector<std::unique_ptr<IRFunction>> functions;  // 子函数列表
    std::unordered_map<std::string, size_t> functionIndex;  // 函数名 → functions 索引
    // BUG-NEW fix: 全局槽位名表（slot → name），供 BytecodeIRBackend lowering 时
    // 将 WRITEBACK_*_VAR 的 GLOBAL_SLOT (IMM_UINT) 转换为名称常量索引。
    // 栈式 VM 的 OP_WRITEBACK_*_VAR 将 varIdx 当作常量池索引处理（取 stringVal()），
    // 若直接 emit 槽位号会误读为常量索引，导致 "未定义的变量" 或类型断言失败。
    // RegisterBytecodeBackend 用高 bit 标记区分 SLOT/NAME，无需此表。
    std::vector<std::string> globalSlotNames;

    void addFunction(std::unique_ptr<IRFunction> fn) {
        functionIndex[fn->name] = functions.size();
        functions.push_back(std::move(fn));
    }
    IRFunction* findFunction(const std::string& name) const {
        auto it = functionIndex.find(name);
        if (it == functionIndex.end()) return nullptr;
        return functions[it->second].get();
    }
};

// ============================================================
// IRBuilder 接口（前端：AST → IR）
// ============================================================

class Block;

/// IRBuilder 抽象接口。
/// 子类实现具体 AST 节点到 IR 的转换逻辑。
class IRBuilder {
public:
    virtual ~IRBuilder() = default;
    /// 从 AST 构建 IR。返回 IRFunction（main chunk）。
    /// 子类必须实现具体转换逻辑。
    virtual std::unique_ptr<IRFunction> build(Block& program) = 0;
};

// ============================================================
// AstIRBuilder：AST → IR 具体实现
// ============================================================

/// 将 AST 转换为 IR 的具体构建器。
/// 覆盖核心 AST 节点：表达式（BinaryOp/UnaryOp/字面量/VarRef）、
/// 控制流（If/While/For）、函数（FunDecl/FunCall/Return）、
/// 变量声明（VarDecl/Assignment）、容器（Array/Dict/Index）、
/// 成员访问（MemberAccess/MethodCall）、类（ClassDecl）、异常（Try/Throw）。
///
/// 设计说明：
///   - 采用 Visitor 模式遍历 AST，但为避免与 Interpreter 的 Visitor 耦合，
///     使用独立的 visit* 方法（不继承 DefaultVisitor）。
///   - 变量解析四态（local/global/upvalue/name）在 IR 层用不同 IROp 表达，
///     lowering 时映射到对应的 VM OpCode。
///   - 闭包 upvalue 在 IR 层用 MAKE_CLOSURE 指令显式建模捕获列表。
///   - 全局变量支持槽位分配（OP_GET_GLOBAL/OP_SET_GLOBAL）和名称索引（OP_GET_VAR/OP_SET_VAR）。
///   - 块作用域：函数内块嵌套时跟踪深度，块退出回收局部变量槽位。
///   - 写回指令：嵌套左值（a.b.c = x / a[i] = x）编译为 WRITEBACK_* 指令。
class AstIRBuilder : public IRBuilder {
public:
    AstIRBuilder();
    ~AstIRBuilder() override = default;

    std::unique_ptr<IRFunction> build(Block& program) override;

    /// 获取构建的 IR 模块（含所有子函数）。build() 后有效。
    // D3 fix: 补 const 重载，便于编译后只读检查 IR 模块（如 IRTransformPanel 渲染）
    IRModule* getModule() { return module_.get(); }
    const IRModule* getModule() const { return module_.get(); }

    /// 获取全局槽位名表（build() 后有效，供 Compiler 填充 CompileResult）
    const std::vector<std::string>& getGlobalSlotNames() const { return globalSlotAllocator_.names(); }

    /// BUG-MOD-1 fix: IR 构建错误报告接口。
    /// AstIRBuilder 不直接访问 Compiler::diagnostics_，通过 hasError/errorMessage 暴露错误状态。
    /// compileViaIR/compileViaRegisterIR 在 build() 后检查并转化为用户可见 diagnostic。
    bool hasError() const { return hasError_; }
    const std::string& errorMessage() const { return errorMessage_; }
    int errorLine() const { return errorLine_; }

    // VM-IMPORT: 模块加载器（使 IR 路径也支持 import 语句的内联编译）
    // Compiler 在 compileViaIR/compileViaRegisterIR 中调用 setModuleLoader 注入。
    // build() 后调用 takeModuleAsts() 转移模块 AST 所有权给 Compiler 保留。
    void setModuleLoader(std::function<std::string(const std::string&)> loader) { moduleLoader_ = std::move(loader); }
    std::vector<std::unique_ptr<Block>> takeModuleAsts() { return std::move(moduleAsts_); }

private:
    std::unique_ptr<IRFunction> ir_;
    IRBasicBlock* currentBlock_ = nullptr;  // 当前基本块（指令追加目标）
    std::unique_ptr<IRModule> module_;       // IR 模块（收集所有函数）
    bool hasError_ = false;                  // BUG-MOD-1: IR 构建错误标志
    std::string errorMessage_;               // BUG-MOD-1: 错误消息
    int errorLine_ = 0;                      // BUG-MOD-1: 错误行号

    // 变量解析状态
    struct VarInfo {
        enum class Kind { LOCAL, GLOBAL_SLOT, GLOBAL_NAME, UPVALUE } kind;
        uint32_t index;  // LOCAL→slot, GLOBAL_SLOT→槽位号, GLOBAL_NAME→globalNames idx, UPVALUE→uv idx
    };
    std::unordered_map<std::string, VarInfo> varMap_;
    std::unordered_map<std::string, std::string> varTypes_;  // 2026-06-29: 变量名→类型注解
    bool inFunction_ = false;
    uint32_t nextLocalSlot_ = 0;
    // BUG-IDE-12 fix: 局部变量 slot→name 映射（索引即 slot），跨作用域累积（不随块退出清除）。
    // 函数最终化时复制到 ir_->localSlotNames，供 RegisterVM 条件断点求值反查变量名。
    std::vector<std::string> localSlotNames_;
    // C-9 fix: 编译类方法时为 true。visitFunDecl 检查此标记，
    // 预留 slot 0 给隐式 this 参数，并将 varMap_["this"] 绑定到 slot 0。
    // 调用方（executeMethodCallImpl/executeClassNewImpl）将 this 作为第一个参数传入。
    bool compilingMethod_ = false;
    // P1 fix: 当前编译的类名（visitClassDecl 设置，供 super 调用查找父类）。
    // compilingMethod_=true 时有效，编译完类方法后清空。
    std::string compilingClassName_;
    // BUG-IR-VARDECL-1 fix: 已定义的类名集合（visitClassDecl 时填充）。
    // visitVarDecl 无初始化器时检查类型注解是否为类名，若是则自动构造实例，
    // 对齐 Compiler.cpp visitVarDecl L620-626 的 S2 fix 语义。
    std::unordered_set<std::string> definedClassNames_;

    // 块作用域跟踪（限制5）
    struct BlockScope {
        std::vector<uint32_t> localSlots;  // 本块声明的局部变量槽位（退出时回收）
        uint32_t slotBase = 0;             // 进入块时的 nextLocalSlot_ 值（退出时回收到此）
        bool hasNestedFunction = false;     // 本块内是否创建了嵌套函数（闭包），
                                            // 若有则不回收槽位（闭包可能捕获了本块的局部变量）
        // CRITICAL-2 fix: 块作用域遮蔽保存栈。当内块 var x 与外块同名时，
        // visitVarDecl 覆盖 varMap_ 前将旧条目压入此栈，leaveBlockScope 时恢复。
        // 保证外层绑定在块退出后可达，对齐 Interpreter 的作用域链语义。
        struct ShadowedVar {
            std::string name;
            VarInfo info;
            bool hadOld;  // varMap_ 中是否已有同名旧条目（无则块退出时删除）
        };
        std::vector<ShadowedVar> shadowedVars;
    };
    std::vector<BlockScope> blockScopes_;
    int blockDepth_ = 0;  // 当前块嵌套深度（仅在 inFunction_==true 时有效）

    // 全局槽位管理（限制3 / B4: 委托给 GlobalSlotAllocator）
    GlobalSlotAllocator globalSlotAllocator_;

    int allocateGlobalSlot(const std::string& name) { return globalSlotAllocator_.allocate(name); }
    int lookupGlobalSlot(const std::string& name) const { return globalSlotAllocator_.lookup(name); }

    // VM-IMPORT: 模块系统状态（对齐 Compiler 的 moduleLoadingSet_/linkedModuleSet_）
    std::function<std::string(const std::string&)> moduleLoader_;
    std::unordered_set<std::string> moduleLoadingSet_;  // 正在编译中（循环检测）
    std::vector<std::string> moduleLoadingStack_;       // BUG-AUDIT-MOD-3: 深度保护栈
    std::unordered_set<std::string> linkedModuleSet_;   // 已完成（run-once）
    std::vector<std::unique_ptr<Block>> moduleAsts_;    // 保留模块 AST
    // BUG-AUDIT-MOD-1: 模块导出名称集合（对齐 Compiler::moduleExports_）
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExports_;

    /// VM-IMPORT: 处理 import 语句（内联编译模块代码到当前 IR）
    void handleImportStmt(ImportStmt& node);

    // 闭包 upvalue 追踪（限制1）
    struct UpvalueInfo { uint32_t index; bool isLocal; int outerIdx; };
    std::vector<UpvalueInfo> currentUpvalues_;
    std::unordered_map<std::string, int> currentUpvalueNames_;
    std::unordered_map<std::string, int> outerLocalSlots_;
    std::unordered_map<std::string, int> outerUpvalueNames_;
    std::unordered_map<std::string, int> outerFunctions_;
    std::unordered_set<std::string> innerFunctions_;
    std::unordered_map<std::string, int> innerFunctionSlots_;

    // 循环上下文（break/continue 跳转目标）
    // BUG-EXC-2 fix: tryDepthAtStart 记录循环开始时的 try 嵌套深度，
    // break/continue 时需为差额层级的 try 发射 TRY_END 弹出 handler，
    // 对齐 Compiler.cpp visitBreakStmt 的 tryDepthInLoop 逻辑。
    struct LoopContext {
        uint32_t startLabel;
        uint32_t endLabel;
        uint32_t continueLabel;  // continue 目标（for 的 update 块）
        int tryDepthAtStart = 0;
    };
    std::vector<LoopContext> loopStack_;
    int tryDepth_ = 0;  // 当前 try 嵌套深度（BUG-EXC-2 fix）

    // ---- 辅助方法 ----
    IRBasicBlock& newBlock();
    // 注意：方法名用 emitIR 而非 emit，避免与 Qt 的 emit 宏（Q_EMIT）冲突
    void emitIR(IROp op, std::vector<IROperand> operands = {}, int line = 0);
    IROperand emitConst(const Value& v, int line = 0);
    IROperand emitLoadVar(const std::string& name, int line = 0);
    void emitStoreVar(const std::string& name, IROperand val, int line = 0);
    VarInfo resolveVar(const std::string& name);

    /// 2026-06-29: 查找变量类型注解
    const std::string* findVarType(const std::string& name) const {
        auto it = varTypes_.find(name);
        return it != varTypes_.end() ? &it->second : nullptr;
    }
    /// 2026-06-29: 发射 TYPE_CHECK IR（检查 val 是否兼容类型注解）
    void emitTypeCheckIR(IROperand val, const std::string& typeAnnotation, int line);
    uint32_t addUpvalue(const std::string& name);
    void enterBlockScope();
    void leaveBlockScope();
    void preScanTopLevelDecls(Block& program, bool isMainModule = false);

    // CRITICAL-1 fix: 前向自由变量分析（对齐 Interpreter::computeFreeVariables）。
    // 在编译子函数体前，先收集所有自由变量名，为每个能在外层捕获的变量预建 upvalue。
    // 这确保中间函数即使不直接引用某变量，也会捕获它供更内层函数透传。
    std::unordered_set<std::string> computeFreeVars(const class FunDecl& fn);
    void collectFreeVars(const class ASTNode& node,
                         std::vector<std::unordered_set<std::string>>& scopes,
                         std::unordered_set<std::string>& freeVars);
    bool isDefinedInScopes(const std::vector<std::unordered_set<std::string>>& scopes,
                           const std::string& name) const;

    // ---- AST 节点转换 ----
    IROperand visitNode(class ASTNode* node);
    // BUG-IR-POP-2/3 fix: 统一处理语句上下文的表达式语句 POP。
    // 对齐 Compiler::compileStatement 的语义：调用 visitNode 后，
    // 若节点是表达式语句（needsPopForExprStmt 返回 true）则 emit POP 消费结果值。
    // 在所有"语句上下文"（if then/else、while body、for body）使用此方法，
    // 避免手动 POP 遗漏导致栈泄漏/不平衡。
    void visitStatement(class ASTNode* node);
    IROperand visitBinaryOp(class BinaryOp* node);
    IROperand visitUnaryOp(class UnaryOp* node);
    IROperand visitNumberLiteral(class NumberLiteral* node);
    IROperand visitStringLiteral(class StringLiteral* node);
    IROperand visitBoolLiteral(class BoolLiteral* node);
    IROperand visitNullLiteral(class NullLiteral* node);
    IROperand visitVarRef(class VarRef* node);
    IROperand visitAssignment(class Assignment* node);
    void visitVarDecl(class VarDecl* node);
    void visitIfStmt(class IfStmt* node);
    void visitWhileStmt(class WhileStmt* node);
    void visitForStmt(class ForStmt* node);
    void visitFunDecl(class FunDecl* node);
    IROperand visitFunCall(class FunCall* node);
    void visitReturnStmt(class ReturnStmt* node);
    void visitPrintStmt(class PrintStmt* node);
    void visitBlock(class Block* node);
    IROperand visitArrayLiteral(class ArrayLiteral* node);
    IROperand visitDictLiteral(class DictLiteral* node);
    IROperand visitIndexAccess(class IndexAccess* node);
    void visitIndexAssign(class IndexAssign* node);
    IROperand visitMemberAccess(class MemberAccess* node);
    void visitMemberAssign(class MemberAssign* node);
    IROperand visitMethodCall(class MethodCall* node);
    IROperand visitInterpolatedString(class InterpolatedString* node);
    void visitClassDecl(class ClassDecl* node);
    void visitBreakStmt(class BreakStmt* node);
    void visitContinueStmt(class ContinueStmt* node);
    void visitTryStmt(class TryStmt* node);
    void visitThrowStmt(class ThrowStmt* node);
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
// BytecodeIRBackend：IR → VM 字节码 lowering
// ============================================================

/// 将 IRFunction lowering 为 BytecodeChunk。
/// 虚拟寄存器通过栈槽映射：lowering 时维护 vreg→栈深度映射。
/// 支持完整的 IR 指令集到 VM OpCode 的映射，包括写回指令和全局槽位。
class BytecodeIRBackend : public IRBackend {
public:
    BytecodeIRBackend();
    ~BytecodeIRBackend() override = default;

    bool lower(const IRFunction& ir) override;

    /// 取生成的字节码 chunk（lower 成功后有效）
    std::unique_ptr<BytecodeChunk> takeChunk() { return std::move(chunk_); }

    /// lower 整个 IRModule（main + 子函数），返回 CompileResult 兼容的结构
    /// 成功后 takeChunk() 返回 main chunk，takeFunctionChunks() 返回函数 chunks
    bool lowerModule(const IRModule& module);  // BUG-NEW: module.globalSlotNames 用于 WRITEBACK_*_VAR slot→name 转换

    /// 取生成的函数 chunks（lowerModule 后有效）
    std::map<std::string, BytecodeChunk> takeFunctionChunks() { return std::move(functionChunks_); }

    /// 方向四：取 main 函数的 IR 指令 → 字节码偏移映射（lower 后有效）
    /// first = IR 指令展平序号（与 IrViewer 的 rowToInstrIndex_ 一致）
    /// second = 该 IR 指令 lowering 后在 main chunk 中的字节码偏移
    /// 用于 VM 单步执行时高亮对应的 IR 指令
    const std::vector<std::pair<size_t, size_t>>& irToBytecodeOffset() const { return irToBytecodeOffset_; }

private:
    std::unique_ptr<BytecodeChunk> chunk_;
    std::map<std::string, BytecodeChunk> functionChunks_;  // 函数名 → chunk
    std::vector<std::pair<size_t, size_t>> irToBytecodeOffset_;  // 方向四：IR→字节码偏移映射

    // vreg → 栈深度映射
    std::unordered_map<uint32_t, uint32_t> vregStackDepth_;
    // 标签 → 字节码偏移映射
    std::unordered_map<uint32_t, size_t> labelToOffset_;
    // 待回填跳转
    // BUG-EXC-1 fix: isTryBegin=true 时 patchJumps 写相对偏移（target - (codeOffset+2)），
    // 因为 StackVM OP_TRY_BEGIN 用 `catchIp = ip + 3 + catchOffset` 解码（相对偏移），
    // 而 OP_JUMP/OP_JUMP_IF_FALSE 用 `ip = jump`（绝对偏移）。
    struct PendingJump { size_t codeOffset; uint32_t targetLabel; bool isLoop; bool isTryBegin = false; };
    std::vector<PendingJump> pendingJumps_;
    // BUG-NEW fix: 全局槽位名表指针（lowerModule 设置，lowerInstruction 中
    // WRITEBACK_*_VAR IMM_UINT 分支用其将 slot→name 转为字符串常量索引）
    const std::vector<std::string>* globalSlotNames_ = nullptr;

    // 辅助
    void emitUint16(std::vector<uint8_t>& code, uint16_t v);
    uint16_t addStringConstant(const std::string& s, const IRFunction& ir);
    // BUG-NEW fix: 将全局槽位号转换为变量名字符串常量索引。
    // 栈式 VM 的 OP_WRITEBACK_*_VAR 把 varIdx 当作常量池索引取 stringVal()，
    // GLOBAL_SLOT 路径需查 globalSlotNames_ 得到变量名再入常量池。
    // BUG-IR-SLOTNAME-1 fix: 返回 bool，失败时不再生成占位名静默产生坏字节码。
    // 调用方需检查返回值，false 时中止 lowering 并向上传递错误。
    bool slotToNameConstant(uint32_t slot, const IRFunction& ir, uint16_t& outIdx);
    bool lowerInstruction(const IRInstruction& instr, const IRFunction& ir);
    bool patchJumps();
    void resetState();
};

// ============================================================
// IR 优化 Pass（方向二）
// ============================================================

/// 常量折叠：对常量间的算术/比较/逻辑运算在编译期求值
/// 例如：ADD(LOAD_CONST 1, LOAD_CONST 2) → LOAD_CONST 3
/// 支持：ADD/SUB/MUL/DIV/MOD（int/float）、NEGATE、EQ/NEQ/LT/GT/LTE/GTE、NOT
/// 折叠条件：两个操作数均来自 LOAD_CONST/LOAD_TRUE/LOAD_FALSE/LOAD_NULL 且结果可表示
/// 返回：是否修改了 IR（true 表示有折叠发生）
bool constantFoldingPass(IRFunction& ir);

/// 死代码消除：删除结果从未被使用的纯计算指令
/// 规则：
///   1. 收集所有被引用的 vreg（作为其他指令的操作数）
///   2. 删除 LOAD_CONST/LOAD_LOCAL/LOAD_GLOBAL/ADD/SUB 等结果无引用的指令
///   3. 保留有副作用的指令：STORE_*/CALL/RETURN/PRINT/JUMP/JUMP_IF_FALSE/THROW 等
/// 返回：是否修改了 IR（true 表示有指令被删除）
bool deadCodeEliminationPass(IRFunction& ir);

/// 复制传播：将 vreg = LOAD_* 后续使用替换为直接引用源
/// 规则：
///   1. 记录每个 vreg 的"定义"：LOAD_CONST c → 该 vreg 等价于 constant c
///   2. 后续指令引用该 vreg 时，直接用 constant 替换
///   3. 跨 STORE_LOCAL 后不再传播（局部变量可能被修改）
/// 返回：是否修改了 IR
bool copyPropagationPass(IRFunction& ir);

/// C4: 公共子表达式消除（CSE，局部 — 基本块内）
/// 规则：
///   1. 在每个基本块内维护"值编号"表：hash((op, operand1_kind, operand1_idx, operand2_kind, operand2_idx)) → dest_vreg
///   2. 遇到纯计算指令（EQ/NEQ/LT/GT/LTE/GTE/NOT/DUP/NEGATE/算术）时，若表达式已存在，
///      将本指令 dest vreg 的所有后续引用替换为已存在的 dest vreg，本指令标记为待删除
///   3. 块边界（JUMP/JUMP_IF_FALSE/RETURN/THROW 等）清空值编号表
/// 安全性：
///   - SSA 风格 vreg 不会被重定义，相同 operand 必产生相同结果
///   - 仅在基本块内有效（避免跨块分析复杂度）
///   - 算术指令也参与 CSE（与 BUG-IR-DCE-1 不同：CSE 不删除指令，只替换引用，副作用保留）
///     副作用指令即使 dest 已被替换引用，仍保留原指令执行以触发除零/溢出错误
/// 返回：是否修改了 IR（true 表示有引用被替换）
bool commonSubexpressionEliminationPass(IRFunction& ir);

/// C4: 循环展开（保守策略 — 仅展开常量边界的小循环）
/// 规则：
///   1. 识别模式：LABEL L1; cond_load; LOAD_CONST N; LT; JUMP_IF_FALSE L_exit; POP;
///      <body>; counter_load; LOAD_CONST 1; ADD; STORE_LOCAL slot; JUMP L1; LABEL L_exit; POP
///   2. 当 N 为常量且 1 ≤ N ≤ kMaxUnrollCount(默认 4) 时，展开 N 次循环体
///      （展开后删除原循环结构，直接生成 N 份顺序 body）
///   3. body 必须不包含 break/continue/return/throw（保守安全门）
///   4. body 指令数 ≤ kMaxUnrollBodySize(默认 20)，避免代码膨胀失控
/// 限制：
///   - 仅支持步长为 1 的整数计数 for 循环（最常见模式）
///   - 不支持嵌套循环展开（外层展开后内层不变）
///   - 不支持含 break/continue 的循环（保守跳过）
/// 返回：是否修改了 IR（true 表示有循环被展开）
bool loopUnrollingPass(IRFunction& ir);

/// 运行全部优化 pass（常量折叠 → [复制传播] → [CSE] → [循环展开] → 死代码消除）
/// @param enableCopyPropagation 是否启用复制传播。
///   - 栈式 VM 后端：false（删除 LOAD_CONST 会导致栈下溢）
///   - 寄存器式 VM 后端：true（LOAD_CONST 写寄存器，无引用时 DCE 安全删除）
/// @param enableDCE 是否启用死代码消除。
///   - 栈式 VM 后端：false（POP operands 为空，DCE 看不到 POP 对 vreg 的消费关系，
///     会删除仅被 POP 消费的 LOAD_CONST 等纯计算指令，导致栈式 VM OP_POP 栈下溢）
///   - 寄存器式 VM 后端：true（vreg 物化为寄存器，无引用时安全删除）
///   - BUG-IR-DCE-2 fix: 解耦 DCE 与复制传播控制，避免禁用复制传播时连带禁用 DCE
/// @param enableCSE 是否启用公共子表达式消除（C4 新增）。
///   - 栈式 VM 后端：必须 false（CSE 替换后续指令对 dest vreg 的引用，原 dest 变为
///     死指令但仍 emit，导致栈上残留未被 POP 的值，与 BUG-IR-DCE-1 同源问题）
///   - 寄存器式 VM 后端：可 true（vreg 物化为寄存器，死指令无栈影响，配合 DCE 安全清理）
///   - 默认 false：与 enableDCE 解耦（仅 DCE=true 的寄存器式后端显式传 true 启用）
/// @param enableLoopUnroll 是否启用循环展开（C4 新增）。
///   - 默认 false：循环展开改变代码结构，可能影响调试器行号映射，默认关闭
///   - 性能场景（ProfileDashboardPanel）可显式启用
/// 返回：是否修改了 IR
bool optimizeIR(IRFunction& ir, bool enableCopyPropagation = false, bool enableDCE = false,
                bool enableCSE = false, bool enableLoopUnroll = false);

// ============================================================
// IR 打印（调试用）
// ============================================================

/// 将 IRFunction 格式化为可读字符串（用于调试和 IR 可视化）
std::string IRToString(const IRFunction& ir);

/// IR 操作码 → 字符串名称（用于 IR 可视化面板，避免 IrViewer 重复维护一份映射）
/// Dedup-4E: 原本此函数在 IR.cpp 匿名命名空间中，IrViewer.cpp 复制了一份 localIrOpName
/// 因缺失 SUPER_MEMBER_GET/SUPER_CALL case 而产生显示 bug，现统一为公共 API。
const char* irOpName(IROp op);

/// 格式化单条 IR 指令为可读字符串（与 IRToString 中 per-instruction 格式一致）。
/// Dedup-4F: IrViewer.cpp 曾复制此实现，现统一为公共 API，避免格式漂移。
std::string formatIRInstruction(const IRInstruction& instr);
