#pragma once

#include "Diagnostic.h"
#include "ast/ASTNode.h"
#include "common/RuntimeLimits.h"
#include "common/TCO.h"         // TCO: 共享尾递归识别函数
#include "common/TypeChecker.h" // A4 fix: TypeChecker stub 接入 pipeline
#include "compiler/Bytecode.h"
#include "compiler/GlobalSlotAllocator.h" // B4: 全局槽位分配器
#include "compiler/IR.h"                  // ARCH-06: IR 中间层（AstIRBuilder + BytecodeIRBackend）
#include "compiler/RegisterBytecode.h"    // PERF-14: 寄存器式字节码
#include "interpreter/Visitor.h"          // 继承 DefaultVisitor，统一 AST 分派为 Visitor 模式
#include <functional>                     // VM-IMPORT: std::function for moduleLoader_
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
    bool useIR_ = false;                                            // 是否启用 IR 路径（实验性，默认关闭）
    bool irOptimize_ = false;                                       // 是否启用 IR 优化 pass（方向二）
    std::unique_ptr<IRFunction> lastIR_;                            // 最近一次 IR 构建结果
    std::vector<std::pair<size_t, size_t>> lastIRToBytecodeOffset_; // 方向四：IR→字节码偏移映射

    // PERF-14: 寄存器式 VM 状态
    bool useRegisterVM_ = false;               // 是否启用寄存器式 VM（默认关闭）
    RegisterCompileResult lastRegisterResult_; // 最近一次寄存器式编译结果

    // A4 fix: TypeChecker stub 状态
    bool enableTypeCheck_ = false;                       // 是否启用类型检查 pass（默认关闭）
    std::unique_ptr<minilang::TypeChecker> typeChecker_; // 类型检查器实例

    BytecodeChunk chunk_;                                // 当前字节码块
    std::unordered_map<std::string, uint16_t> varIndex_; // 变量名 → 常量池索引
    // PERF-29 fix: 字符串字面量去重 map，避免相同字符串重复存入常量池。
    // 仅对字符串字面量去重（数字字面量本身类型不同，去重收益小）。
    std::unordered_map<std::string, uint16_t> stringConstIndex_;
    DiagnosticBag diagnostics_; // 诊断收集器
    // MEM-03/MEM-04 fix: 与 CompileResult/VM 保持一致使用 std::map，
    // 使 std::move 给 CompileResult 时类型匹配。
    std::map<std::string, BytecodeChunk> functionChunks_; // 函数字节码块
    std::unordered_map<std::string, int> currentLocals_;  // 当前函数的局部变量槽位映射
    // BUG-IDE-12 fix: 局部变量槽位→名称映射（索引即 slot），跨作用域累积（不随作用域退出清除）。
    // 函数最终化时复制到 chunk_.localSlotNames，供 VM 条件断点求值反查变量名。
    std::vector<std::string> localSlotNames_;
    // L1 fix（2026-07-19）: 基于 IP 范围的槽位→名称映射，解决兄弟作用域槽位复用导致的
    // 变量名错位。每个 range 记录变量名在 [startIp, endIp) 范围内占用 slot。
    // 函数最终化时复制到 chunk_.slotNameRanges，供 VM 调试器按 frame.ip 精确反查。
    std::vector<BytecodeChunk::SlotNameRange> slotNameRanges_;
    std::unordered_map<std::string, std::string> varTypes_; // 2026-06-29: 变量名→类型注解（local+global）
    bool inFunction_ = false;                               // 是否在函数体内
    std::string currentFunctionReturnType_;                 // BUG-TYPE-1 fix: 当前函数返回类型注解（empty 表示无注解）
    std::vector<std::string> currentTypeParams_;            // R163 泛型扩展：当前函数/方法的类型参数（empty=非泛型），用于 emitTypeCheck 擦除
    // TCO: 尾调用优化状态。visitFunDecl 入口设置 currentFunctionName_ /
    // currentFunctionDecl_ / currentFunctionEntryIp_，visitReturnStmt 据此
    // 判断 return f(args) 是否可优化为"参数赋值 + 跳转到函数入口"。
    // 不变量：三字段由 CompileContext 统一保存/恢复，嵌套函数编译后外层状态复原。
    std::string currentFunctionName_;              // TCO: 当前函数名（空表示非函数体）
    const FunDecl* currentFunctionDecl_ = nullptr; // TCO: 当前函数 FunDecl 指针（非拥有，AST 生命周期内有效）
    size_t currentFunctionEntryIp_ = 0;            // TCO: 当前函数体入口 IP（OP_JUMP 目标）
    std::unordered_map<std::string, std::vector<std::string>> classFieldNames_; // 类名 → 字段名列表（含继承字段）
    std::unordered_map<std::string, int> outerLocals_; // 外层函数的局部变量（用于检测闭包捕获）
    // VM-05/06: 闭包 upvalue 编译期追踪
    std::vector<UpvalueDesc> currentUpvalues_;                 // 当前函数正在构建的 upvalue 描述符列表
    std::unordered_map<std::string, int> currentUpvalueNames_; // 变量名→upvalue索引（去重用）
    std::vector<UpvalueDesc> outerUpvalues_;                   // 外层函数的 upvalue 描述符（用于透传检测）
    std::unordered_map<std::string, int> outerUpvalueNames_;   // 外层函数的 upvalue 名称映射
    std::unordered_map<std::string, int> outerFunctions_;      // 外层作用域的函数名→槽位号（内嵌函数捕获用）
    int writebackCounter_ = 0;                                 // B6: 写回计数器，生成唯一缓存变量名避免索引重复求值
    int peakLocals_ = 0;       // VMBUG-2: 函数编译期间局部变量槽位峰值（含被块作用域回收的变量）
    int blockDepth_ = 0;       // VMBUG-1: 顶层块作用域嵌套深度（仅在 inFunction_==false 时有效）
    int blockSaveCounter_ = 0; // L11 fix: 块作用域保存计数器（成员变量，编译间重置）
    // P1 fix: 编译递归深度计数器，防止深度嵌套 AST 导致 C++ 栈溢出
    int compileDepth_ = 0;
    static constexpr int MAX_COMPILE_DEPTH = RuntimeLimits::MAX_COMPILE_DEPTH;
    std::string currentClassName_; // B1 fix: 当前正在编译的类名（供 OP_SUPER_CALL 编码类上下文）

    // break/continue 循环上下文栈
    // 每层循环编译时压入，记录 break 跳转目标（循环出口）和 continue 跳转目标（循环起始/更新）
    // 的待回填跳转指令偏移列表，循环编译完成后统一回填
    struct LoopContext {
        size_t loopStart;                  // 循环起始字节码偏移（continue 跳转目标）
        size_t loopEndPatch;               // 循环出口跳转指令偏移（break 跳转目标，循环结束时回填）
        std::vector<size_t> breakJumps;    // break 语句的 OP_JUMP 偏移列表（待回填到循环出口）
        std::vector<size_t> continueJumps; // continue 语句的 OP_JUMP/OP_LOOP 偏移列表（待回填到循环起始）
        bool hasUpdate;                    // for 循环有 update 表达式，continue 应跳到 update 而非 loopStart
        size_t updateStart;                // for 循环 update 表达式起始偏移（hasUpdate=true 时有效）
        int tryDepthAtStart;               // BUG1 fix: 循环开始时的 tryDepth_，break/continue 只弹循环内 try handler
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

    // AUDIT-P1.1 fix: try-finally 编译期上下文栈。
    // break/continue 在 try-finally 内时，不直接跳到循环目标，而是先 push 真实跳转目标
    // 到 pendingJumpStack_，再 jump 到 finally 入口。finally 末尾的 OP_FINALLY_END 从栈
    // 取出真实目标并跳转，实现"先执行 finally 再跳转"的三后端一致性。
    struct TryFinallyContext {
        bool hasFinally = false;
        size_t finallyEntryIp = 0;              // finally 块入口地址（编译期确定后回填 pendingJumpPatches）
        std::vector<size_t> pendingJumpPatches; // OP_JUMP 目标待回填到 finallyEntryIp
        std::vector<size_t>
            pendingTargetPatches; // OP_PUSH_JUMP_TARGET 目标待回填到 finallyEntryIp（来自内层 break/continue）
    };
    std::vector<TryFinallyContext> tryFinallyStack_;

    // VM-IMPORT: 模块系统状态（对齐 Interpreter 的 moduleCache_/moduleLoadingSet_ 设计）
    // 直接路径与 IR 路径共用此状态，确保模块只编译一次（run-once 语义）。
    std::function<std::string(const std::string&)> moduleLoader_; // 模块源码加载回调
    std::string currentFilePath_;                                 // 当前文件路径（相对路径解析基准）
    std::unordered_set<std::string> moduleLoadingSet_;            // 正在编译中的模块（循环依赖检测）
    std::vector<std::string>
        moduleLoadingStack_; // BUG-AUDIT-MOD-3: 模块加载栈（深度保护，对齐 Interpreter 的 moduleLoadingStack_）
    std::unordered_set<std::string> linkedModuleSet_; // 已完成编译的模块（run-once 语义）
    std::vector<std::unique_ptr<Block>> moduleAsts_;  // 保留模块 AST（确保函数/类定义指针在编译期有效）
    // BUG-AUDIT-MOD-1: 模块导出名称集合（key=模块路径，value=该模块 export 的名称集合）
    // 对齐 InterpreterModules.cpp 的 export 检查——具名导入只能导入显式 export 的名称
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExports_;
    // R99 enum 校验：编译期收集的 enum 元信息，compile() 完成时写入 CompileResult.enumInfos。
    // 直接路径在 visitEnumDecl 中收集，IR/RegVM 路径通过 irBuilder.takeEnumInfos() 获取。
    std::vector<VMEnumInfo> pendingEnumInfos_;

    /// VM-IMPORT: 模块路径规范化与安全校验（对齐 InterpreterModules.cpp SEC-1 防护）
    /// 返回空字符串表示路径非法（调用方应报错）
    std::string normalizeModulePath(const std::string& rawPath) const;

    /// VM-IMPORT: 加载并解析模块源码，返回模块 AST（nullptr 表示失败）
    std::unique_ptr<Block> loadAndParseModule(const std::string& modulePath, int line);

    /// VM-IMPORT: 预扫描模块顶层声明，分配全局槽位
    /// （VarDecl/ClassDecl/FunDecl/ExportStmt(VarDecl|ClassDecl|FunDecl)）。
    /// 注意：与 compile() 的 pre-scan 存在有意的不对称——此处预扫描 FunDecl，
    /// 因为模块函数导入验证（visitImportStmt）使用 lookupGlobalSlot 检查导出名。
    void preScanModuleGlobals(Block& moduleAst);

    /// visitImportStmt 子阶段：收集模块导出名称集合
    /// 仅 ExportStmt 包装的声明名（VarDecl/ClassDecl/FunDecl）计入导出集合，
    /// 普通顶层声明不算导出。对齐 InterpreterModules.cpp:172-188 的语义。
    /// 输出：moduleExports_[modulePath] = exports 集合
    void collectModuleExports(const std::string& modulePath, Block& moduleAst);

    // ---- C3 fix: 编译上下文 RAII 守卫 ----
    // visitFunDecl 需保存/恢复 15 个成员变量。原代码手动 std::move 保存 + 手动恢复
    // （含异常路径），极易遗漏。CompileContextGuard 构造时保存，析构时恢复（含异常路径）。

    /// 编译上下文快照（函数编译时保存的外层状态）
    struct CompileContext {
        BytecodeChunk chunk;
        std::unordered_map<std::string, uint16_t> varIndex;
        std::unordered_map<std::string, int> currentLocals;
        std::unordered_map<std::string, std::string> varTypes; // 2026-06-29: 类型注解快照
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
        std::vector<TryFinallyContext> tryFinallyStack; // AUDIT-P1.1 fix
        std::string currentFunctionReturnType;          // BUG-TYPE-1 fix: 当前函数返回类型注解
        std::vector<std::string> currentTypeParams;     // R163 泛型扩展：当前函数/方法的类型参数
        // TCO: 保存当前函数名 / FunDecl 指针 / 入口 IP，使嵌套函数编译后外层 TCO 状态复原。
        std::string currentFunctionName;
        const FunDecl* currentFunctionDecl = nullptr;
        size_t currentFunctionEntryIp = 0;
        // L1 fix（2026-07-19）: 保存局部变量名映射和 IP 范围表，使嵌套函数编译后
        // 外层函数的 localSlotNames_/slotNameRanges_ 不被内层函数污染。原代码遗漏
        // 这两个字段，导致外层函数最终化时 chunk_.localSlotNames 含内层函数的变量名。
        std::vector<std::string> localSlotNames;
        std::vector<BytecodeChunk::SlotNameRange> slotNameRanges;
        // R164 fixup2: 保存字符串字面量去重缓存，使嵌套函数编译后外层 chunk 的
        // stringConstIndex_ 不被内层函数 chunk 的索引污染。原代码遗漏此字段，
        // 导致函数体内添加的字符串常量索引（指向函数 chunk 常量池）泄漏到外层，
        // 外层 chunk 编译同名字符串时错误复用函数 chunk 的索引，而外层 chunk
        // 该索引可能指向函数名常量，造成字典字符串键查找失败（d["a"] 变成 d["mk"]）。
        std::unordered_map<std::string, uint16_t> stringConstIndex;
    };

    /// 保存当前编译上下文（move 语义，调用后成员变量处于 moved-from 状态）
    CompileContext saveCompileContext();
    /// 从快照恢复编译上下文
    void restoreCompileContext(CompileContext&& ctx);

    /// @brief RAII 守卫：构造时保存编译上下文，析构时自动恢复（含异常路径）。
    ///
    /// R97 #9 fix: 用于 visitClassDecl/visitFunDecl 等需要临时切换编译上下文的场景。
    /// 原实现手动 save/restore 15+ 个成员变量，错误路径极易遗漏恢复步骤。
    /// 使用 guard 后，无论正常返回还是异常返回，析构函数都会自动恢复上下文。
    ///
    /// @code
    ///   CompileContextGuard guard(*this);     // 保存
    ///   chunk_ = BytecodeChunk(...);           // 修改上下文
    ///   if (error) return;                     // 析构自动恢复
    ///   // 正常路径也自动恢复
    /// @endcode
    ///
    /// @note 不可拷贝（避免双重恢复）。如需在正常路径跳过恢复，调用 dismiss()。
    /// @warning currentClassName_ 不在 CompileContext 中，需单独手动保存/恢复。
    struct CompileContextGuard {
        Compiler& compiler;
        CompileContext saved;
        bool dismissed = false;
        explicit CompileContextGuard(Compiler& c) : compiler(c), saved(c.saveCompileContext()) {}
        ~CompileContextGuard() {
            if (!dismissed)
                compiler.restoreCompileContext(std::move(saved));
        }
        void dismiss() { dismissed = true; }
        CompileContextGuard(const CompileContextGuard&) = delete;
        CompileContextGuard& operator=(const CompileContextGuard&) = delete;
    };

    // H5 fix: 内嵌函数闭包追踪 — 内嵌函数存储为局部变量，通过 OP_CALL_EXPR 调用
    std::unordered_set<std::string> innerFunctions_;          // 当前作用域中的内嵌函数名
    std::unordered_map<std::string, int> innerFunctionSlots_; // 内嵌函数名 → 局部变量槽位号

    // R98 W3: Lambda 表达式合成名计数器——匿名 lambda 的 FunDecl.name 为空，
    // 编译时用 `$lambda_N` 作为 functionChunks_/identifierIndex 的内部 key
    // （$ 不在标识符首字符集中，合成名不会与用户变量名冲突）
    int lambdaCounter_ = 0;

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

    /// AUDIT-P1.1 fix: 发射 break/continue 的 finally 续跳字节码。
    /// 若 break/continue 在 try-finally 内，先 push 真实跳转目标到 pendingJumpStack_，
    /// 再 jump 到最内层 finally 入口；finally 末尾的 OP_FINALLY_END 从栈取出目标续跳。
    /// realTargetPatch 输出参数：OP_PUSH_JUMP_TARGET 的目标 patch（待回填到 breakTarget/continueTarget）。
    /// 返回 true 表示已发射续跳字节码（调用方不应再发射常规 OP_JUMP），false 表示无 finally（常规路径）。
    bool emitFinallyJump(int line, std::vector<size_t>& realTargetPatches);

    /// 编译 AST 节点（通过 Visitor 模式的 accept 分派）
    void compileNode(ASTNode* node);

    /// 编译节点作为语句（确保栈平衡：纯表达式语句会补发 OP_POP 弹出返回值）
    void compileStatement(ASTNode* node);

    /// 编译各个节点类型（Visitor 模式：由 accept 分派调用，返回 Value 统一接口）
    /// L1 fix: 关闭 slot >= slotBase 的变量名 range（回填 endIp 为当前 code.size()）。
    /// 在块作用域退出（currentLocals_ = savedLocals 之前）调用，记录变量生命周期边界。
    void closeSlotRanges(size_t slotBase) {
        size_t endIp = chunk_.code.size();
        for (auto& range : slotNameRanges_) {
            if (range.endIp == 0 && range.slot >= slotBase) {
                range.endIp = endIp;
            }
        }
    }
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
    void visitInterpolatedString(InterpolatedString& node) override; // C5 fix
    // R98 元组与解构
    void visitTupleLiteral(TupleLiteral& node) override;
    void visitDestructureBinding(DestructureBinding& node) override;
    /// 解构绑定单个变量绑定逻辑（复用 visitVarDecl 的局部/全局分支）
    void bindDestructureVar(const std::string& name, int line, int col);

    // R99 枚举与 ADT + match
    void visitEnumDecl(EnumDecl& node) override;
    void visitEnumVariantExpr(EnumVariantExpr& node) override;
    void visitMatchExpr(MatchExpr& node) override;

    // R164 协程/生成器：yield 表达式编译
    void visitYieldExpr(YieldExpr& node) override;

    // ============================================================
    // 超长函数拆分：visitTryStmt / visitFunDecl / visitClassDecl 子阶段
    // 拆分目的：降低单函数圈复杂度，便于审计与回归测试。
    // 关键约束：所有 BUG-025 / BUG-026 / AUDIT-* / PERF-* 审计注释与不变量
    // 必须原样保留；每条 emit 路径的 push/pop 平衡、CompileContextGuard
    // RAII、breakJumps/continueJumps/finallyIndices 索引不变量不得改变。
    // ============================================================

    /// try-catch 编译期 patch 信息（emitTryBlock 输出 → emitCatchBlock 输入）
    struct TryCatchPatchInfo {
        size_t tryBeginIp = 0;
        size_t catchOffsetPatch = std::string::npos;
        size_t skipCatchJumpPatch = std::string::npos;
    };

    /// visitTryStmt 子阶段：编译 try 块
    /// 发射 OP_TRY_BEGIN（catchOffset 占位）、try body（含 tryDepth_ 增减）、
    /// OP_TRY_END、skip-catch OP_JUMP。无错误返回路径。
    void emitTryBlock(TryStmt& node, TryCatchPatchInfo& info);

    /// visitTryStmt 子阶段：编译 catch 块
    /// 回填 catchOffset、绑定 catch 变量（shadow/cleanup 状态）、编译 catch body
    /// （含 cleanup wrap：OP_TRY_BEGIN/END + cleanupThrow 路径）、cleanup 字节码、
    /// OP_CLOSE_UPVALUE、closeSlotRanges、恢复 currentLocals_、回填 afterCatch。
    /// 返回 false 表示发生溢出错误，调用方应跳过 finally 块直接返回
    /// （保留原始控制流：原实现 catch 块 early return 不 pop tryFinallyStack_）。
    bool emitCatchBlock(TryStmt& node, const TryCatchPatchInfo& info);

    /// visitTryStmt 子阶段：编译 finally 块
    /// 发射 OP_TRY_END（弹外层 handler）、记录 finallyEntryIp 回填续跳 patches、
    /// 编译 finally body（正常路径）+ OP_FINALLY_END + skip-exception OP_JUMP、
    /// 回填 finallyCatchOffset、编译 finally body（异常路径）+ OP_THROW、回填 afterFinally。
    /// 早返回时不 pop tryFinallyStack_（由 visitTryStmt 统一 pop，保持单次 pop 语义）。
    void emitFinallyBlock(TryStmt& node, size_t outerTryBeginIp, size_t finallyCatchOffsetPatch);

    /// visitFunDecl 子阶段：函数声明
    /// 基于 CompileContextGuard.saved 设置 outerLocals_/outerUpvalues_/outerFunctions_，
    /// 创建函数 chunk、设置参数槽位（含 localSlotNames_）、BUG-UV-1 前向自由变量分析。
    /// 返回 false 表示参数数量超限，调用方应在 guard 作用域内直接 return。
    /// @param effectiveName 函数有效名称（匿名 lambda 用 `$lambda_N` 合成名，与 functionChunks_ key 一致）
    bool declareFunction(FunDecl& node, const CompileContext& saved, const std::string& effectiveName);

    /// visitFunDecl 子阶段：函数体编译
    /// 编译 body、追加隐式 OP_NULL/OP_RETURN、保存 localCount/localSlotNames/slotNameRanges。
    void compileFunctionBody(FunDecl& node);

    /// visitFunDecl / emitMethodBody 共用子阶段：默认参数值 emit
    /// 仅支持字面量（Number/String/Bool/Null）和负数字面量（含 --N 嵌套折叠），
    /// 复杂表达式记录无效索引 0xFFFF（VM 不支持，Interpreter 路径仍可执行）。
    void emitDefaultValues(FunDecl& node);

    /// visitMatchExpr 子阶段：编译单个 match case 的模式检查
    /// 处理 WILDCARD（无操作）/LITERAL（DUP+literal+OP_EQUAL+JUMP_IF_FALSE+POP）
    /// /VARIANT（DUP+OP_ENUM_VARIANT_NAME+JUMP_IF_FALSE+POP+字段绑定）三种模式。
    /// 匹配失败跳转 patch 追加到 caseSkipPatches 由调用方回填。
    /// 栈布局不变量：调用前 [scrut]，调用后 [scrut]（保持 scrut 在栈顶供后续 case 复用）。
    void emitMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches);

    /// visitMatchExpr 子阶段：编译 case body 并保留值在栈顶
    /// 处理 Block body（编译除最后一条外的所有语句带 POP，最后一条用 compileNode 保留值；
    /// 非表达式语句补 OP_NULL）与单表达式 body 两种情况。
    /// 空 body 推 OP_NULL。栈布局不变量：调用前 []，调用后 [body_result]。
    void compileMatchBody(ASTNode* body, int line);

    /// visitClassDecl 子阶段：类成员编译循环
    /// 遍历 node.members，对每个 FunDecl 创建 CompileContextGuard 并调用 emitMethodBody。
    /// currentClassName_ 不在 CompileContext 中，由本函数在每次迭代前后手动保存/恢复。
    void compileClassMembers(ClassDecl& node, const std::vector<std::string>& allFieldNames);

    /// visitClassDecl 子阶段：单个方法体编译
    /// 设置方法 chunk（this/字段/参数槽位布局）、编译 body、保存元数据、emit 默认参数值、
    /// 移交 chunk_ 到 functionChunks_[methodKey]。slot 越界时 early return（调用方继续下一成员）。
    /// currentClassName_ 由调用方（compileClassMembers）在迭代结束时恢复。
    /// R163 泛型扩展：classTypeParams 为类的类型参数列表，与 method.typeParams 合并后注入 currentTypeParams_。
    void emitMethodBody(FunDecl& method, const std::string& className, const std::vector<std::string>& allFieldNames,
                        CompileContextGuard& guard, const std::vector<std::string>& classTypeParams = {});

    /// visitMethodCall 子阶段：嵌套访问变异方法写回
    /// 当接收者是 MemberAccess/IndexAccess 且基对象是 VarRef 时，方法调用后发射写回指令，
    /// 确保 this.arr.push(42) 等嵌套调用的修改不丢失。VM 在变异方法调用时将修改后的
    /// 对象暂存到 lastMutatedReceiver_，写回指令从中取值写回基对象的字段/索引位置。
    /// 仅依赖 receiver AST 与 currentLocals_（成员），无其他外部状态。
    void emitMethodCallWriteback(const ASTNode* receiver, int line);

    /// 发出编译错误
    void error(const std::string& msg, int line, int col);

    /// VM-05/06: 解析闭包捕获变量为 upvalue 索引（返回 -1 表示未找到）
    int resolveUpvalue(const std::string& name, int line);

    /// BUG-UV-1 fix: 前向自由变量分析。对齐 IR 路径 computeFreeVars 的语义，
    /// 在编译子函数体前预建 upvalue，使中间函数捕获内层引用的变量供透传。
    /// 实现：递归遍历 AST，收集所有 VarRef/Assignment 中引用但未在函数内定义的变量名。
    /// 嵌套函数的自由变量若不在外层作用域定义，传播为外层自由变量。
    std::unordered_set<std::string> computeFreeVars(const FunDecl& fn);
    void collectFreeVars(const ASTNode& node, std::vector<std::unordered_set<std::string>>& scopes,
                         std::unordered_set<std::string>& freeVars);
    bool isDefinedInScopes(const std::vector<std::unordered_set<std::string>>& scopes, const std::string& name) const;

    /// 安全获取当前字节码偏移量（溢出检查）
    uint16_t safeCodeOffset() { return safeCodeOffset(chunk_.code.size()); }

    /// 安全将 size_t 偏移量转为 uint16_t（溢出检查）
    uint16_t safeCodeOffset(size_t offset) {
        if (offset > 65535) {
            throw std::runtime_error("编译错误: 字节码超出 64KB 限制");
        }
        return static_cast<uint16_t>(offset);
    }

    /// 常量折叠：尝试在编译期求值二元运算，成功返回 true 并输出结果
    bool tryFoldBinary(BinOpType opType, ASTNode* left, ASTNode* right, Value& result, int line);

    /// 常量折叠：尝试在编译期求值一元运算，成功返回 true 并输出结果
    bool tryFoldUnary(UnaryOp::UnaryOpType opType, ASTNode* operand, Value& result, int line);

    /// D8 fix: 从 AST 节点递归提取常量值。支持字面量节点和嵌套的 BinaryOp/UnaryOp
    /// 常量表达式（如 (1+2)*3）。成功返回 true 并输出常量值。
    bool extractConstant(ASTNode* node, Value& result, int line);

    /// 发射常量值指令（根据 Value 类型选择 OP_INT/OP_FLOAT/OP_STRING/OP_TRUE/OP_FALSE/OP_NULL）
    void emitConstant(const Value& val, int line);
};
