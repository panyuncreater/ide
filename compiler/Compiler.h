#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include "ast/ASTNode.h"
#include "compiler/Bytecode.h"
#include "compiler/GlobalSlotAllocator.h"  // B4: 全局槽位分配器
#include "compiler/IR.h"  // ARCH-06: IR 中间层（AstIRBuilder + BytecodeIRBackend）
#include "compiler/RegisterBytecode.h"  // PERF-14: 寄存器式字节码
#include "Diagnostic.h"
#include "interpreter/Visitor.h"  // 继承 DefaultVisitor，统一 AST 分派为 Visitor 模式
#include "common/RuntimeLimits.h"
#include "common/TypeChecker.h"  // A4 fix: TypeChecker stub 接入 pipeline

// ============================================================
// Compiler 字节码编译器
// ============================================================

/// 将 AST 编译为字节码
class Compiler : public DefaultVisitor {
public:
    Compiler();

    /// 编译 AST 块到字节码
    CompileResult compile(Block& program);

    /// ARCH-06: IR 中间层编译路径（实验性）
    /// AST → IR（AstIRBuilder）→ Bytecode（BytecodeIRBackend）
    /// 当 useIR_=true 时由 compile() 调用。简化实现，不支持高级特性。
    CompileResult compileViaIR(Block& program);

    /// PERF-14: 寄存器式编译路径
    /// AST → IR（AstIRBuilder）→ RegisterBytecode（RegisterBytecodeBackend）
    /// 当 useRegisterVM_=true 时由 compile() 调用。
    RegisterCompileResult compileViaRegisterIR(Block& program);

    /// 获取编译错误信息
    std::string getLastError() const;

    /// 获取编译过程中的诊断信息
    const DiagnosticBag& getDiagnostics() const { return diagnostics_; }

    // ARCH-06: IR 中间层开关（实验性，默认关闭）
    // 启用后 compile() 走 AST → IR → Bytecode 路径（AstIRBuilder + BytecodeIRBackend）。
    // IR 路径已支持闭包 upvalue、写回指令、全局槽位分配、默认参数、块作用域等完整特性。
    void setUseIR(bool enabled) { useIR_ = enabled; }
    bool getUseIR() const { return useIR_; }

    /// 方向二：IR 优化 pass 开关（仅当 useIR_=true 时生效）
    /// 启用后在 IR lowering 前执行：常量折叠 → 复制传播 → 死代码消除
    void setIROptimize(bool enabled) { irOptimize_ = enabled; }
    bool getIROptimize() const { return irOptimize_; }

    /// PERF-14: 寄存器式 VM 开关（默认关闭）
    /// 启用后 compile() 走 AST → IR → RegisterBytecode 路径，
    /// 配合 RegisterVM 执行。PERF-15 复制传播在寄存器式下安全启用。
    void setUseRegisterVM(bool enabled) { useRegisterVM_ = enabled; }
    bool getUseRegisterVM() const { return useRegisterVM_; }

    /// A4 fix: 启用/禁用静态类型检查 pass（在 codegen 前运行，默认关闭）
    /// 当前 TypeChecker 为 stub 实现（返回空诊断），不影响编译结果，
    /// 但提供了未来类型检查接入点。
    void setEnableTypeCheck(bool enabled) {
        enableTypeCheck_ = enabled;
        if (enabled && !typeChecker_) {
            typeChecker_ = std::make_unique<minilang::MiniLangTypeChecker>();
        } else if (!enabled) {
            typeChecker_.reset();
        }
    }
    bool getEnableTypeCheck() const { return enableTypeCheck_; }

    /// A4 fix: 获取类型检查器实例（用于配置 strictMode 等参数）
    /// 返回 nullptr 表示未启用类型检查
    minilang::TypeChecker* getTypeChecker() const { return typeChecker_.get(); }

    /// PERF-14: 寄存器式编译结果（compileViaRegisterIR 后有效）
    const RegisterCompileResult& getLastRegisterResult() const { return lastRegisterResult_; }

    /// ARCH-06: 获取最近一次 IR 构建结果（用于调试/可视化）。
    /// 仅当 useIR_=true 且 compile() 成功后有效。返回 nullptr 表示未启用 IR 或构建失败。
    const IRFunction* getLastIR() const { return lastIR_.get(); }

    /// 方向四：获取 main 函数的 IR 指令 → 字节码偏移映射（IR 调试器集成）。
    /// 仅当 useIR_=true 且 compile() 成功后有效。
    /// 返回空 vector 表示未启用 IR 路径或构建失败。
    const std::vector<std::pair<size_t, size_t>>& getLastIRToBytecodeOffset() const { return lastIRToBytecodeOffset_; }

private:
    // ARCH-06: IR 中间层状态
    bool useIR_ = false;  // 是否启用 IR 路径（实验性，默认关闭）
    bool irOptimize_ = false;  // 是否启用 IR 优化 pass（方向二）
    std::unique_ptr<IRFunction> lastIR_;  // 最近一次 IR 构建结果
    std::vector<std::pair<size_t, size_t>> lastIRToBytecodeOffset_;  // 方向四：IR→字节码偏移映射

    // PERF-14: 寄存器式 VM 状态
    bool useRegisterVM_ = false;  // 是否启用寄存器式 VM（默认关闭）
    RegisterCompileResult lastRegisterResult_;  // 最近一次寄存器式编译结果

    // A4 fix: TypeChecker stub 状态
    bool enableTypeCheck_ = false;  // 是否启用类型检查 pass（默认关闭）
    std::unique_ptr<minilang::TypeChecker> typeChecker_;  // 类型检查器实例

    BytecodeChunk chunk_;                           // 当前字节码块
    std::unordered_map<std::string, uint16_t> varIndex_;  // 变量名 → 常量池索引
    // PERF-29 fix: 字符串字面量去重 map，避免相同字符串重复存入常量池。
    // 仅对字符串字面量去重（数字字面量本身类型不同，去重收益小）。
    std::unordered_map<std::string, uint16_t> stringConstIndex_;
    DiagnosticBag diagnostics_;                      // 诊断收集器
    // MEM-03/MEM-04 fix: 与 CompileResult/VM 保持一致使用 std::map，
    // 使 std::move 给 CompileResult 时类型匹配。
    std::map<std::string, BytecodeChunk> functionChunks_;  // 函数字节码块
    std::unordered_map<std::string, int> currentLocals_;  // 当前函数的局部变量槽位映射
    std::unordered_map<std::string, std::string> varTypes_;  // 2026-06-29: 变量名→类型注解（local+global）
    bool inFunction_ = false;                       // 是否在函数体内
    std::unordered_map<std::string, std::vector<std::string>> classFieldNames_;  // 类名 → 字段名列表（含继承字段）
    std::unordered_map<std::string, int> outerLocals_;  // 外层函数的局部变量（用于检测闭包捕获）
    // VM-05/06: 闭包 upvalue 编译期追踪
    std::vector<UpvalueDesc> currentUpvalues_;       // 当前函数正在构建的 upvalue 描述符列表
    std::unordered_map<std::string, int> currentUpvalueNames_; // 变量名→upvalue索引（去重用）
    std::vector<UpvalueDesc> outerUpvalues_;         // 外层函数的 upvalue 描述符（用于透传检测）
    std::unordered_map<std::string, int> outerUpvalueNames_; // 外层函数的 upvalue 名称映射
    std::unordered_map<std::string, int> outerFunctions_; // 外层作用域的函数名→槽位号（内嵌函数捕获用）
    int writebackCounter_ = 0;  // B6: 写回计数器，生成唯一缓存变量名避免索引重复求值
    int peakLocals_ = 0;        // VMBUG-2: 函数编译期间局部变量槽位峰值（含被块作用域回收的变量）
    int blockDepth_ = 0;        // VMBUG-1: 顶层块作用域嵌套深度（仅在 inFunction_==false 时有效）
    int blockSaveCounter_ = 0;  // L11 fix: 块作用域保存计数器（成员变量，编译间重置）
    // P1 fix: 编译递归深度计数器，防止深度嵌套 AST 导致 C++ 栈溢出
    int compileDepth_ = 0;
    static constexpr int MAX_COMPILE_DEPTH = RuntimeLimits::MAX_COMPILE_DEPTH;
    std::string currentClassName_;  // B1 fix: 当前正在编译的类名（供 OP_SUPER_CALL 编码类上下文）

    // break/continue 循环上下文栈
    // 每层循环编译时压入，记录 break 跳转目标（循环出口）和 continue 跳转目标（循环起始/更新）
    // 的待回填跳转指令偏移列表，循环编译完成后统一回填
    struct LoopContext {
        size_t loopStart;              // 循环起始字节码偏移（continue 跳转目标）
        size_t loopEndPatch;           // 循环出口跳转指令偏移（break 跳转目标，循环结束时回填）
        std::vector<size_t> breakJumps;    // break 语句的 OP_JUMP 偏移列表（待回填到循环出口）
        std::vector<size_t> continueJumps; // continue 语句的 OP_JUMP/OP_LOOP 偏移列表（待回填到循环起始）
        bool hasUpdate;                // for 循环有 update 表达式，continue 应跳到 update 而非 loopStart
        size_t updateStart;            // for 循环 update 表达式起始偏移（hasUpdate=true 时有效）
        int tryDepthAtStart;           // BUG1 fix: 循环开始时的 tryDepth_，break/continue 只弹循环内 try handler
    };
    std::vector<LoopContext> loopStack_;

    // P0-4 fix: 跟踪当前 try 块嵌套深度，break/continue 跳出 try 块时需发射 OP_TRY_END
    int tryDepth_ = 0;

    // ---- C3 fix: 编译上下文 RAII 守卫 ----
    // visitFunDecl 需保存/恢复 15 个成员变量。原代码手动 std::move 保存 + 手动恢复
    // （含异常路径），极易遗漏。CompileContextGuard 构造时保存，析构时恢复（含异常路径）。

    /// 编译上下文快照（函数编译时保存的外层状态）
    struct CompileContext {
        BytecodeChunk chunk;
        std::unordered_map<std::string, uint16_t> varIndex;
        std::unordered_map<std::string, int> currentLocals;
        std::unordered_map<std::string, std::string> varTypes;  // 2026-06-29: 类型注解快照
        bool inFunction = false;
        std::unordered_map<std::string, int> outerLocals;
        int peakLocals = 0;
        std::vector<UpvalueDesc> outerUpvalues;
        std::unordered_map<std::string, int> outerUpvalueNames;
        std::unordered_map<std::string, int> outerFunctions;
        std::unordered_set<std::string> innerFunctions;
        std::unordered_map<std::string, int> innerFunctionSlots;
        std::vector<UpvalueDesc> currentUpvalues;
        std::unordered_map<std::string, int> currentUpvalueNames;
        std::vector<LoopContext> loopStack;
        int tryDepth = 0;
    };

    /// 保存当前编译上下文（move 语义，调用后成员变量处于 moved-from 状态）
    CompileContext saveCompileContext();
    /// 从快照恢复编译上下文
    void restoreCompileContext(CompileContext&& ctx);

    /// RAII 守卫：构造时保存上下文，析构时自动恢复（含异常路径）
    struct CompileContextGuard {
        Compiler& compiler;
        CompileContext saved;
        bool dismissed = false;
        explicit CompileContextGuard(Compiler& c) : compiler(c), saved(c.saveCompileContext()) {}
        ~CompileContextGuard() { if (!dismissed) compiler.restoreCompileContext(std::move(saved)); }
        void dismiss() { dismissed = true; }
        CompileContextGuard(const CompileContextGuard&) = delete;
        CompileContextGuard& operator=(const CompileContextGuard&) = delete;
    };

    // H5 fix: 内嵌函数闭包追踪 — 内嵌函数存储为局部变量，通过 OP_CALL_EXPR 调用
    std::unordered_set<std::string> innerFunctions_;           // 当前作用域中的内嵌函数名
    std::unordered_map<std::string, int> innerFunctionSlots_;  // 内嵌函数名 → 局部变量槽位号

    // A2/B4: 全局变量整数槽位管理（委托给 GlobalSlotAllocator）
    GlobalSlotAllocator globalSlotAllocator_;

    /// 添加变量名到常量池，返回索引
    uint16_t identifierIndex(const std::string& name);

    /// A2: 分配全局槽位（委托给 globalSlotAllocator_）
    int allocateGlobalSlot(const std::string& name) { return globalSlotAllocator_.allocate(name); }

    /// A2: 查找全局槽位（委托给 globalSlotAllocator_）
    int lookupGlobalSlot(const std::string& name) const { return globalSlotAllocator_.lookup(name); }

    /// 2026-06-29: 查找变量的类型注解（沿 outerLocals_ 链不追踪，仅当前作用域）
    const std::string* findVarType(const std::string& name) const {
        auto it = varTypes_.find(name);
        return it != varTypes_.end() ? &it->second : nullptr;
    }

    /// 2026-06-29: 发射 OP_TYPE_CHECK 指令（检查栈顶值是否兼容类型注解）
    void emitTypeCheck(const std::string& typeAnnotation, int line);

    /// 编译 AST 节点（通过 Visitor 模式的 accept 分派）
    void compileNode(ASTNode* node);

    /// 编译节点作为语句（确保栈平衡：纯表达式语句会补发 OP_POP 弹出返回值）
    void compileStatement(ASTNode* node);

    /// 编译各个节点类型（Visitor 模式：由 accept 分派调用，返回 Value 统一接口）
    void visitBinaryOp(BinaryOp& node) override;
    void visitUnaryOp(UnaryOp& node) override;
    void visitNumberLiteral(NumberLiteral& node) override;
    void visitStringLiteral(StringLiteral& node) override;
    void visitBoolLiteral(BoolLiteral& node) override;
    void visitVarDecl(VarDecl& node) override;
    void visitAssignment(Assignment& node) override;
    void visitVarRef(VarRef& node) override;
    void visitIfStmt(IfStmt& node) override;
    void visitWhileStmt(WhileStmt& node) override;
    void visitForStmt(ForStmt& node) override;
    void visitFunDecl(FunDecl& node) override;
    void visitFunCall(FunCall& node) override;
    void visitReturnStmt(ReturnStmt& node) override;
    void visitPrintStmt(PrintStmt& node) override;
    void visitBlock(Block& node) override;

    // 新增节点编译
    void visitArrayLiteral(ArrayLiteral& node) override;
    void visitDictLiteral(DictLiteral& node) override;
    void visitIndexAccess(IndexAccess& node) override;
    void visitIndexAssign(IndexAssign& node) override;
    void visitClassDecl(ClassDecl& node) override;
    void visitMemberAccess(MemberAccess& node) override;
    void visitMemberAssign(MemberAssign& node) override;
    void visitMethodCall(MethodCall& node) override;
    void visitNullLiteral(NullLiteral& node) override;
    void visitSuperExpr(SuperExpr& node) override;
    void visitBreakStmt(BreakStmt& node) override;
    void visitContinueStmt(ContinueStmt& node) override;
    void visitTryStmt(TryStmt& node) override;
    void visitThrowStmt(ThrowStmt& node) override;
    void visitImportStmt(ImportStmt& node) override;
    void visitExportStmt(ExportStmt& node) override;
    void visitInterpolatedString(InterpolatedString& node) override;  // C5 fix

    /// 发出编译错误
    void error(const std::string& msg, int line, int col);

    /// VM-05/06: 解析闭包捕获变量为 upvalue 索引（返回 -1 表示未找到）
    int resolveUpvalue(const std::string& name, int line);

    /// 安全获取当前字节码偏移量（溢出检查）
    uint16_t safeCodeOffset() {
        return safeCodeOffset(chunk_.code.size());
    }

    /// 安全将 size_t 偏移量转为 uint16_t（溢出检查）
    uint16_t safeCodeOffset(size_t offset) {
        if (offset > 65535) {
            throw std::runtime_error("编译错误: 字节码超出 64KB 限制");
        }
        return static_cast<uint16_t>(offset);
    }

    /// 常量折叠：尝试在编译期求值二元运算，成功返回 true 并输出结果
    bool tryFoldBinary(BinOpType opType, ASTNode* left, ASTNode* right,
                       Value& result, int line);

    /// 常量折叠：尝试在编译期求值一元运算，成功返回 true 并输出结果
    bool tryFoldUnary(UnaryOp::UnaryOpType opType, ASTNode* operand,
                      Value& result, int line);

    /// D8 fix: 从 AST 节点递归提取常量值。支持字面量节点和嵌套的 BinaryOp/UnaryOp
    /// 常量表达式（如 (1+2)*3）。成功返回 true 并输出常量值。
    bool extractConstant(ASTNode* node, Value& result, int line);

    /// 发射常量值指令（根据 Value 类型选择 OP_INT/OP_FLOAT/OP_STRING/OP_TRUE/OP_FALSE/OP_NULL）
    void emitConstant(const Value& val, int line);
};
