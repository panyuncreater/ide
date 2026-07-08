#pragma once

#include <memory>
#include <functional>  // VM-IMPORT: std::function for moduleLoader_
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

    // VM-IMPORT: 模块加载器（使 VM 编译路径支持 import 语句）
    // 语义对齐 Interpreter::setModuleLoader：IDE 在编译前注入基于当前文件路径的加载回调。
    // 直接路径在 visitImportStmt 中使用；IR/寄存器路径在 compileViaIR/compileViaRegisterIR
    // 中转发给 AstIRBuilder。
    void setModuleLoader(std::function<std::string(const std::string&)> loader) { moduleLoader_ = std::move(loader); }
    void setCurrentFilePath(const std::string& path) { currentFilePath_ = path; }

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
    // BUG-IDE-12 fix: 局部变量槽位→名称映射（索引即 slot），跨作用域累积（不随作用域退出清除）。
    // 函数最终化时复制到 chunk_.localSlotNames，供 VM 条件断点求值反查变量名。
    std::vector<std::string> localSlotNames_;
    std::unordered_map<std::string, std::string> varTypes_;  // 2026-06-29: 变量名→类型注解（local+global）
    bool inFunction_ = false;                       // 是否在函数体内
    std::string currentFunctionReturnType_;          // BUG-TYPE-1 fix: 当前函数返回类型注解（empty 表示无注解）
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
        // AUDIT-P2-CORRECT fix: break 需发射 OP_CLOSE_UPVALUE 关闭循环体内声明的
        // 闭包捕获变量的 upvalue（对齐正常迭代退出时的 OP_CLOSE_UPVALUE 发射）。
        // OP_CLOSE_UPVALUE bodySlotBase 关闭 slot >= bodySlotBase 的全部 open upvalues，
        // 含循环体内嵌套块声明的变量（嵌套块 slot >= bodySlotBase）。
        size_t bodySlotBase = 0;       // 循环体 slot 基址
        bool needCloseUpvalue = false; // 是否需要关闭 upvalue（inFunction_）
    };
    std::vector<LoopContext> loopStack_;

    // P0-4 fix: 跟踪当前 try 块嵌套深度，break/continue 跳出 try 块时需发射 OP_TRY_END
    int tryDepth_ = 0;

    // VM-IMPORT: 模块系统状态（对齐 Interpreter 的 moduleCache_/moduleLoadingSet_ 设计）
    // 直接路径与 IR 路径共用此状态，确保模块只编译一次（run-once 语义）。
    std::function<std::string(const std::string&)> moduleLoader_;  // 模块源码加载回调
    std::string currentFilePath_;                                  // 当前文件路径（相对路径解析基准）
    std::unordered_set<std::string> moduleLoadingSet_;             // 正在编译中的模块（循环依赖检测）
    std::vector<std::string> moduleLoadingStack_;                  // BUG-AUDIT-MOD-3: 模块加载栈（深度保护，对齐 Interpreter 的 moduleLoadingStack_）
    std::unordered_set<std::string> linkedModuleSet_;              // 已完成编译的模块（run-once 语义）
    std::vector<std::unique_ptr<Block>> moduleAsts_;               // 保留模块 AST（确保函数/类定义指针在编译期有效）
    // BUG-AUDIT-MOD-1: 模块导出名称集合（key=模块路径，value=该模块 export 的名称集合）
    // 对齐 InterpreterModules.cpp 的 export 检查——具名导入只能导入显式 export 的名称
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExports_;

    /// VM-IMPORT: 模块路径规范化与安全校验（对齐 InterpreterModules.cpp SEC-1 防护）
    /// 返回空字符串表示路径非法（调用方应报错）
    std::string normalizeModulePath(const std::string& rawPath) const;

    /// VM-IMPORT: 加载并解析模块源码，返回模块 AST（nullptr 表示失败）
    std::unique_ptr<Block> loadAndParseModule(const std::string& modulePath, int line);

    /// VM-IMPORT: 预扫描模块顶层声明，分配全局槽位
    ///（VarDecl/ClassDecl/FunDecl/ExportStmt(VarDecl|ClassDecl|FunDecl)）。
    /// 注意：与 compile() 的 pre-scan 存在有意的不对称——此处预扫描 FunDecl，
    /// 因为模块函数导入验证（visitImportStmt）使用 lookupGlobalSlot 检查导出名。
    void preScanModuleGlobals(Block& moduleAst);

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
        std::string currentFunctionReturnType;  // BUG-TYPE-1 fix: 当前函数返回类型注解
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

    /// BUG-UV-1 fix: 前向自由变量分析。对齐 IR 路径 computeFreeVars 的语义，
    /// 在编译子函数体前预建 upvalue，使中间函数捕获内层引用的变量供透传。
    /// 实现：递归遍历 AST，收集所有 VarRef/Assignment 中引用但未在函数内定义的变量名。
    /// 嵌套函数的自由变量若不在外层作用域定义，传播为外层自由变量。
    std::unordered_set<std::string> computeFreeVars(const FunDecl& fn);
    void collectFreeVars(const ASTNode& node,
                         std::vector<std::unordered_set<std::string>>& scopes,
                         std::unordered_set<std::string>& freeVars);
    bool isDefinedInScopes(const std::vector<std::unordered_set<std::string>>& scopes,
                           const std::string& name) const;

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
