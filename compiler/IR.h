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
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include "compiler/Bytecode.h"  // IRBackend lowering 到 BytecodeChunk + UpvalueDesc
#include "compiler/GlobalSlotAllocator.h"  // B4: 全局槽位分配器

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
    LOAD_UPVALUE,    // dest = upvalue[idx]         operands: [dest_vreg, uv_idx]
    STORE_UPVALUE,   // upvalue[idx] = src          operands: [uv_idx, src_vreg]
    CLOSE_UPVALUE,   // close upvalue[idx]          operands: [uv_idx]

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

    // ---- 方法调用 ----
    METHOD_CALL,     // dest = obj.method(args)     operands: [dest, obj_vreg, method_idx, arg_count, args...]

    // ---- 类 ----
    DEFINE_CLASS,    // define class name           operands: [name_idx]
    CLASS_NEW,       // dest = new ClassName(args)  operands: [dest, name_idx, arg_count, args...]
    INIT_FIELD,      // init field name from stack  operands: [field_idx]

    // ---- 异常处理 ----
    TRY_BEGIN,       // try block begin             operands: [catch_label_idx]
    TRY_END,         // try block end
    THROW,           // throw src                   operands: [src_vreg]

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
    uint32_t nextVReg = 0;                     // 下一个虚拟寄存器号
    uint32_t nextLabel = 0;                    // 下一个标签号

    // 函数元数据（lowering 时使用）
    int arity = 0;                  // 参数个数
    int requiredArity = 0;          // 必需参数个数
    int localCount = 0;             // 局部变量总槽位数
    std::vector<uint16_t> defaultConstIndices;  // 默认参数值的常量索引
    std::vector<UpvalueDesc> upvalues;           // 闭包 upvalue 描述符列表

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
    /// 添加全局变量名/字段名/函数名，返回索引
    uint32_t addGlobal(const std::string& name) {
        for (size_t i = 0; i < globalNames.size(); ++i) {
            if (globalNames[i] == name) return static_cast<uint32_t>(i);
        }
        globalNames.push_back(name);
        return static_cast<uint32_t>(globalNames.size() - 1);
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
    IRModule* getModule() { return module_.get(); }

    /// 获取全局槽位名表（build() 后有效，供 Compiler 填充 CompileResult）
    const std::vector<std::string>& getGlobalSlotNames() const { return globalSlotAllocator_.names(); }

private:
    std::unique_ptr<IRFunction> ir_;
    IRBasicBlock* currentBlock_ = nullptr;  // 当前基本块（指令追加目标）
    std::unique_ptr<IRModule> module_;       // IR 模块（收集所有函数）

    // 变量解析状态
    struct VarInfo {
        enum class Kind { LOCAL, GLOBAL_SLOT, GLOBAL_NAME, UPVALUE } kind;
        uint32_t index;  // LOCAL→slot, GLOBAL_SLOT→槽位号, GLOBAL_NAME→globalNames idx, UPVALUE→uv idx
    };
    std::unordered_map<std::string, VarInfo> varMap_;
    bool inFunction_ = false;
    uint32_t nextLocalSlot_ = 0;
    // C-9 fix: 编译类方法时为 true。visitFunDecl 检查此标记，
    // 预留 slot 0 给隐式 this 参数，并将 varMap_["this"] 绑定到 slot 0。
    // 调用方（executeMethodCallImpl/executeClassNewImpl）将 this 作为第一个参数传入。
    bool compilingMethod_ = false;

    // 块作用域跟踪（限制5）
    struct BlockScope {
        std::vector<uint32_t> localSlots;  // 本块声明的局部变量槽位（退出时回收）
    };
    std::vector<BlockScope> blockScopes_;
    int blockDepth_ = 0;  // 当前块嵌套深度（仅在 inFunction_==true 时有效）

    // 全局槽位管理（限制3 / B4: 委托给 GlobalSlotAllocator）
    GlobalSlotAllocator globalSlotAllocator_;

    int allocateGlobalSlot(const std::string& name) { return globalSlotAllocator_.allocate(name); }
    int lookupGlobalSlot(const std::string& name) const { return globalSlotAllocator_.lookup(name); }

    // 闭包 upvalue 追踪（限制1）
    struct UpvalueInfo { uint32_t index; bool isLocal; int outerIdx; };
    std::vector<UpvalueInfo> currentUpvalues_;
    std::unordered_map<std::string, int> currentUpvalueNames_;
    std::vector<std::string> outerLocals_;
    std::unordered_map<std::string, int> outerLocalSlots_;
    std::vector<UpvalueDesc> outerUpvalues_;
    std::unordered_map<std::string, int> outerUpvalueNames_;
    std::unordered_map<std::string, int> outerFunctions_;
    std::unordered_set<std::string> innerFunctions_;
    std::unordered_map<std::string, int> innerFunctionSlots_;

    // 循环上下文（break/continue 跳转目标）
    struct LoopContext {
        uint32_t startLabel;
        uint32_t endLabel;
        uint32_t continueLabel;  // continue 目标（for 的 update 块）
    };
    std::vector<LoopContext> loopStack_;

    // ---- 辅助方法 ----
    IRBasicBlock& newBlock();
    // 注意：方法名用 emitIR 而非 emit，避免与 Qt 的 emit 宏（Q_EMIT）冲突
    void emitIR(IROp op, std::vector<IROperand> operands = {}, int line = 0);
    IROperand emitConst(const Value& v, int line = 0);
    IROperand emitLoadVar(const std::string& name, int line = 0);
    void emitStoreVar(const std::string& name, IROperand val, int line = 0);
    VarInfo resolveVar(const std::string& name);
    uint32_t addUpvalue(const std::string& name);
    void enterBlockScope();
    void leaveBlockScope();
    void preScanTopLevelDecls(Block& program);

    // ---- AST 节点转换 ----
    IROperand visitNode(class ASTNode* node);
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
    bool lowerModule(const IRModule& module);

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
    struct PendingJump { size_t codeOffset; uint32_t targetLabel; bool isLoop; };
    std::vector<PendingJump> pendingJumps_;

    // 辅助
    void emitUint16(std::vector<uint8_t>& code, uint16_t v);
    uint16_t addStringConstant(const std::string& s, const IRFunction& ir);
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

/// 运行全部优化 pass（常量折叠 → [复制传播] → 死代码消除）
/// @param enableCopyPropagation 是否启用复制传播。
///   - 栈式 VM 后端：false（删除 LOAD_CONST 会导致栈下溢）
///   - 寄存器式 VM 后端：true（LOAD_CONST 写寄存器，无引用时 DCE 安全删除）
/// 返回：是否修改了 IR
bool optimizeIR(IRFunction& ir, bool enableCopyPropagation = false);

// ============================================================
// IR 打印（调试用）
// ============================================================

/// 将 IRFunction 格式化为可读字符串（用于调试和 IR 可视化）
std::string IRToString(const IRFunction& ir);
