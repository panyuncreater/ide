#pragma once

#include "Diagnostic.h"
#include "ast/ASTNode.h"
#include "common/ModulePath.h" // AUDIT-R5 R6 fix: 模块路径缓存键规范化单一事实源
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

    /// P2-11 预编译模块：独立编译模块源码为 CompileResult。
    /// 与 compile() 的区别：
    ///   - 模块全局槽位从 0 开始独立编号（不与主程序共享）
    ///   - 收集 export 语句导出的名称到 result.moduleExports
    ///   - 用于生成 .minic 预编译模块文件
    /// @param source 模块源码
    /// @param modulePath 模块路径（用于 export 集合索引与诊断）
    /// @return CompileResult；编译失败时 diagnostics_ 含错误
    CompileResult compileModule(const std::string& source, const std::string& modulePath);

    /// L11 预编译模块（RegisterVM）：独立编译模块源码为 RegisterCompileResult。
    /// 与 compileViaRegisterIR 的区别：
    ///   - 模块全局槽位从 0 开始独立编号（不与主程序共享）
    ///   - 收集 export 语句导出的名称到 result.moduleExports
    ///   - 用于生成 MLRC 格式 .minic 预编译模块文件（通过 BytecodeCache::storeRegisterToFile）
    /// @param source 模块源码
    /// @param modulePath 模块路径（用于 export 集合索引与诊断）
    /// @return RegisterCompileResult；编译失败时 diagnostics_ 含错误
    RegisterCompileResult compileModuleViaRegisterIR(const std::string& source, const std::string& modulePath);

    /// P2-11 预编译模块：设置 .minic 文件路径解析器。
    /// 回调接收模块路径，返回对应的 .minic 文件系统路径（空表示无预编译文件）。
    /// 未设置时 visitImportStmt 不尝试加载 .minic，退化为源码编译。
    void setPrecompiledModuleResolver(std::function<std::string(const std::string&)> resolver) {
        precompiledModuleResolver_ = std::move(resolver);
    }

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

    /// P1-7: 清空诊断信息(字节码缓存命中时调用,避免陈旧诊断)
    void clearDiagnostics() { diagnostics_.clear(); }

    // ARCH-06: IR 中间层开关（实验性，默认关闭）
    // 启用后 compile() 走 AST → IR → Bytecode 路径（AstIRBuilder + BytecodeIRBackend）。
    // IR 路径已支持闭包 upvalue、写回指令、全局槽位分配、默认参数、块作用域等完整特性。
    void setUseIR(bool enabled) { useIR_ = enabled; }
    bool getUseIR() const { return useIR_; }

    /// 方向二：IR 优化 pass 开关（仅当 useIR_=true 时生效）
    /// 启用后在 IR lowering 前执行：常量折叠 → 复制传播 → 死代码消除
    void setIROptimize(bool enabled) { irOptimize_ = enabled; }
    bool getIROptimize() const { return irOptimize_; }

    /// P2-10 IR SSA 高级优化 pass 开关（仅当 irOptimize_=true 且寄存器式后端时生效）
    /// 启用后在 optimizeIR 后额外执行：GVN（全局值编号）+ LICM（循环不变外提）+ 函数内联。
    /// 与 CSE 同源约束：GVN/LICM 替换 dest vreg 引用后栈式后端栈残留 → OP_POP 栈下溢，
    /// 因此仅对寄存器式后端安全（compileViaRegisterIR 路径）。
    void setIRSSAOptimize(bool enabled) { irSSAOptimize_ = enabled; }
    bool getIRSSAOptimize() const { return irSSAOptimize_; }

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

    /// L17: 缓存注入接口。PipelineRunner 字节码缓存命中时，跳过 compileViaRegisterIR，
    /// 直接将反序列化的 RegisterCompileResult 注入到 lastRegisterResult_，使后续
    /// getLastRegisterResult() 返回缓存结果。调用方需自行 clearDiagnostics()。
    void setLastRegisterResult(RegisterCompileResult result) { lastRegisterResult_ = std::move(result); }

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
    bool useIR_ = false;                 // 是否启用 IR 路径（实验性，默认关闭）
    bool irOptimize_ = true;             // 是否启用 IR 优化 pass（方向二）。
                                         // L17: 默认启用（eng-gvnlicm）。此前默认关的根因是
                                         // PerfBenchmark.Ackermann_RegisterVM 挂死——licmPass 对
                                         // 尾递归→循环转换产生的 header=entry 多循环缺少 preheader
                                         // 支配性校验，候选指令在回边源块间乒乓迁移不收敛。
                                         // 已修复（IRSSA.cpp licmPass：dominates(preheader,header)
                                         // 前提校验 + 不动点燃料上限），回归锁：IROptRegression.*。
                                         // 注意：仅当 useIR_ / useRegisterVM_ 启用时生效，
                                         // 默认 compile() 直接路径不受影响。
    bool irSSAOptimize_ = true;          // L16: SSA 高级优化默认启用（GVN/LICM/内联，仅寄存器式后端）。
                                         //   仅当 irOptimize_=true 时生效。
                                         //   验证机制：ssaConstructPass 返回 false（无 LOCAL 变量需 PHI）时
                                         //   自动跳过 GVN/LICM，仅保留 SSA 构造+析构往返（语义等价）。
                                         //   回退：用户可显式 setIRSSAOptimize(false) 禁用 SSA 优化。
    std::unique_ptr<IRFunction> lastIR_; // 最近一次 IR 构建结果
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
    std::vector<std::string>
        currentTypeParams_; // R163 泛型扩展：当前函数/方法的类型参数（empty=非泛型），用于 emitTypeCheck 擦除
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
    // AUDIT-R7 F5 fix: 当前是否直接处于类方法体编译中（emitMethodBody 置位，
    // visitFunDecl 编译嵌套函数时清除）。供 visitReturnStmt 的 TCO isMethod 判据——
    // 不能用 currentClassName_ 非空（它在嵌套函数内保留以支持 super 调用）。
    bool compilingMethodBody_ = false;

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
    // AUDIT-R5 R5 fix（快照导入）：模块导出名 → 模块内部槽位（内联期记录，detach 前）。
    // 导入方从此槽位拷贝快照到自身新分配的槽位，实现与 Interpreter 一致的快照语义
    // （模块内变异不经导入方名称可见）。run-once 路径的重复导入也从此处拷贝。
    std::unordered_map<std::string, std::unordered_map<std::string, int>> moduleExportSlots_;
    // AUDIT-R5 R5 fix：模块导出的“可变变量”名（仅 ExportStmt 包装的 VarDecl）。
    // 快照只适用于变量导出；函数/类导出经名称解析（functionChunks_/类注册），
    // 不拷贝不 detach（否则类实例化 OP_CLASS_NEW / 函数调用会因 varMap_ 重定向而损坏）。
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExportVars_;
    // R99 enum 校验：编译期收集的 enum 元信息，compile() 完成时写入 CompileResult.enumInfos。
    // 直接路径在 visitEnumDecl 中收集，IR/RegVM 路径通过 irBuilder.takeEnumInfos() 获取。
    std::vector<VMEnumInfo> pendingEnumInfos_;

    // P2-11 预编译模块：.minic 文件路径解析器（IDE 注入，将模块路径映射到 .minic 文件系统路径）
    std::function<std::string(const std::string&)> precompiledModuleResolver_;

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

    /// AUDIT-R5 R5 fix（快照导入）：记录模块导出名的内部槽位到 moduleExportSlots_。
    /// 在槽位分配完成后（源码路径：preScan+collectModuleExports 之后；预编译路径：
    /// loadPrecompiledModule 填充 moduleExports_ 之后）调用。
    void recordModuleExportSlots(const std::string& modulePath);

    /// AUDIT-R5 R5 fix：detach 模块导出名（使导入方可另分配新槽位承载快照副本）。
    /// 在模块内联编译完成后调用一次（模块内部引用已按槽位索引固化，不受影响）。
    void detachModuleExportSlots(const std::string& modulePath);

    /// AUDIT-R5 R5 fix：为具名/全量导入 emit 快照拷贝（模块槽位 → 导入方新槽位）。
    /// namespace 导入由 emitNamespaceDict 构造字典快照，此处跳过。
    /// 循环导入未 detach 场景下 allocate 返回模块原槽位，自拷贝被跳过（保持既有共享行为）。
    void emitImportSnapshotCopy(const std::string& modulePath, ImportStmt& node);

    /// P2-11 预编译模块：尝试加载 .minic 并合并到当前编译。
    /// 在 visitImportStmt 中优先调用，失败时返回 false 以回退到源码编译。
    /// 合并策略：模块 mainChunk 转为函数 chunk（__mod_<hash>__init），
    /// 通过 OP_CLOSURE + OP_CALL_EXPR 调用初始化；模块函数 chunk 按导出/非导出
    /// 决定是否重命名前缀化，避免与主程序同名冲突。全局槽位通过 relocationMap 重映射。
    /// @param modulePath 规范化后的模块路径
    /// @param node ImportStmt 节点（用于错误报告与命名空间字典构造）
    /// @return true=合并成功；false=无 .minic 或加载失败（调用方应回退到源码编译）
    bool loadPrecompiledModule(const std::string& modulePath, ImportStmt& node);

    /// L11 预编译模块（RegisterVM）：加载 MLRC 格式 .minic 并合并到 lastRegisterResult_。
    /// 在 compileViaRegisterIR 的 post-lowering 阶段调用，处理 AstIRBuilder 记录的
    /// pending 预编译模块加载。合并策略对齐 loadPrecompiledModule：
    ///   - 模块 mainChunk 转为函数 chunk（__mod_<hash>___init），重命名非导出函数引用，
    ///     重定位全局槽位
    ///   - 模块 functionChunks 重命名+重定位后合并到 lastRegisterResult_.functionChunks
    ///   - 主 chunk 追加 REG_CALL 调用模块初始化函数
    ///   - 填充 moduleExports_ 供后续具名导入验证
    /// @param modulePath 规范化后的模块路径
    /// @param initFnName 模块初始化函数名（__mod_<hash>___init）
    /// @param line 导入语句行号（用于错误报告）
    /// @return true=合并成功；false=加载失败（调用方应报错或回退）
    bool loadPrecompiledRegisterModule(const std::string& modulePath, const std::string& initFnName, int line);

    /// P2-11 预编译模块：重命名 chunk 常量池中被 OP_CLOSURE/OP_CALL 引用的函数名。
    /// 遍历 code 中的 OP_CLOSURE / OP_CALL 指令，获取 nameIdx → constants[nameIdx].stringVal()，
    /// 若名称在 moduleFunctions 中且不在 exportSet 中，则前缀化为 `__mod_<hash>_<name>` 并更新常量。
    /// 同时处理 OP_CALL 是为了模块隔离：模块内调用非导出函数（如 `main` 调 `helper`）时，
    /// OP_CALL 按名查找需重定向到前缀化后的模块函数名，否则会命中主程序同名函数。
    /// @param chunk 要处理的字节码块（就地修改 constants）
    /// @param prefix 重命名前缀（如 `__mod_<hash>_`）
    /// @param exportSet 导出名称集合（在集合中的名称不重命名）
    /// @param moduleFunctions 模块定义的所有函数名（导出+非导出），用于精确判断
    ///                        OP_CALL 引用的是模块内函数还是内置函数
    static void renameClosureRefs(BytecodeChunk& chunk, const std::string& prefix,
                                  const std::unordered_set<std::string>& exportSet,
                                  const std::unordered_set<std::string>& moduleFunctions);

    /// L11 预编译模块（RegisterVM）：重命名 RegBytecodeChunk 常量池中被
    /// REG_CALL/REG_MAKE_CLOSURE 引用的函数名。对齐 renameClosureRefs 语义：
    /// 遍历 code 中的 REG_CALL/REG_MAKE_CLOSURE 指令，获取 nameIdx →
    /// constants[nameIdx].stringVal()，若名称在 moduleFunctions 中且不在 exportSet 中，
    /// 则前缀化为 `__mod_<hash>_<name>` 并更新常量。
    /// 指令格式：
    ///   REG_CALL:        [op(1B), dst(1B), nameIdx(2B LE), argCount(1B), args...]
    ///   REG_MAKE_CLOSURE: [op(1B), dst(1B), nameIdx(2B LE), uvCount(1B), uvDescs...]
    static void renameRegClosureRefs(RegBytecodeChunk& chunk, const std::string& prefix,
                                     const std::unordered_set<std::string>& exportSet,
                                     const std::unordered_set<std::string>& moduleFunctions);

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
    // L18 lang-constfun: 顶层 const fun 注册表（name → 非拥有 FunDecl 指针，
    // AST 编译期间存活）。visitFunCall 据此对实参全字面量调用编译期折叠。
    std::unordered_map<std::string, FunDecl*> constFunDecls_;

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

    /// PERF: 判断表达式是否可证明为整数类型（保守策略：仅当 100% 确定时返回 true）
    /// 用于编译期决定是否发射类型特化算术操作码（OP_ADD_INT_SPEC 等）
    bool isExprKnownInt(const ASTNode* expr) const;

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
    ///
    /// L27 重构：原 200 行单函数按阶段拆分为 5 个子函数分发，
    /// 降低圈复杂度便于启用 readability-function-cognitive-complexity 检查。
    /// 所有 BUG-7a/7b/7c/BUG-AUDIT-EXC-*/BUG-TRY-1/L1 fix 不变量原样保留。
    bool emitCatchBlock(TryStmt& node, const TryCatchPatchInfo& info);

    /// catch 变量绑定状态（emitCatchBlock 子阶段间传递）
    struct CatchVarBindInfo {
        std::unordered_map<std::string, int> savedCatchLocals; // currentLocals_ 快照（函数内 catch 变量恢复用）
        std::string catchVarName;                              // catch 变量名（cleanup 字节码 OP_DELETE_VAR 用）
        bool needCatchVarCleanup = false;                      // 顶层 catch 变量需 OP_DELETE_VAR 清理
        bool hasShadowedGlobal = false;                        // 顶层 catch 变量遮蔽已有全局
        int shadowedGlobalSlot = -1;                           // 被遮蔽的全局槽位
        std::string shadowedSaveName;                          // 临时保存被遮蔽全局值的变量名
        int catchVarSlot = -1;                                 // 函数内 catch 变量 slot（OP_CLOSE_UPVALUE 用）
    };

    /// emitCatchBlock 子阶段 1：回填 catchOffset（OP_TRY_BEGIN 占位）
    /// 溢出 65535 时返回 false 并发 error。
    bool patchCatchOffset(TryStmt& node, const TryCatchPatchInfo& info);
    /// emitCatchBlock 子阶段 2：绑定 catch 变量到栈顶异常值
    /// 函数内：分配局部 slot + OP_SET_LOCAL + OP_POP；顶层：OP_DEFINE_VAR + 可选遮蔽保护。
    /// 返回 false 表示函数局部变量数量超限（slot > 255），调用方应 early return。
    bool bindCatchVariable(TryStmt& node, CatchVarBindInfo& bind);
    /// emitCatchBlock 子阶段 3：编译 catch body（含 cleanup wrap）
    /// needsCleanupWrap 时用 OP_TRY_BEGIN/END 包装 catch 块，捕获内层 throw 跳到
    /// cleanupThrow 路径执行清理后 rethrow。
    /// @param outSkipCleanupThrowJumpPatch 输出 cleanup 跳转 patch（无 wrap 时为 npos）
    /// @return false 表示 cleanupThrowOffset 溢出 65535（已恢复 currentLocals_，调用方 early return）
    bool emitCatchBodyWithCleanup(TryStmt& node, const CatchVarBindInfo& bind, size_t& outSkipCleanupThrowJumpPatch);
    /// emitCatchBlock 子阶段 4：恢复 catch 作用域
    /// restoreMapping + OP_CLOSE_UPVALUE + closeSlotRanges + 恢复 currentLocals_。
    void restoreCatchScope(TryStmt& node, const CatchVarBindInfo& bind);
    /// emitCatchBlock 子阶段 5：回填 afterCatch 跳转目标
    /// 回填 skipCatchJumpPatch 与 skipCleanupThrowJumpPatch。溢出 65535 时返回 false。
    bool patchSkipCatchJumps(TryStmt& node, const TryCatchPatchInfo& info, size_t skipCleanupThrowJumpPatch);
    /// emitCatchBlock 子阶段共用：发射 cleanup 字节码（OP_DELETE_VAR + 恢复遮蔽全局）
    void emitCatchCleanupBytecode(const CatchVarBindInfo& bind, int line);

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
    /// 复杂表达式记录无效索引 NO_INDEX（VM 不支持，Interpreter 路径仍可执行）。
    void emitDefaultValues(FunDecl& node);

    /// visitMatchExpr 子阶段：编译单个 match case 的模式检查
    /// 处理 WILDCARD（无操作）/LITERAL（DUP+literal+OP_EQUAL+JUMP_IF_FALSE+POP）
    /// /VARIANT（DUP+OP_ENUM_VARIANT_NAME+JUMP_IF_FALSE+POP+字段绑定）三种模式。
    /// 匹配失败跳转 patch 追加到 caseSkipPatches 由调用方回填。
    /// 栈布局不变量：调用前 [scrut]，调用后 [scrut]（保持 scrut 在栈顶供后续 case 复用）。
    ///
    /// L27 重构：原 211 行单函数按 MatchPatternKind 拆分为 6 个子函数分发，
    /// 降低圈复杂度便于启用 readability-function-cognitive-complexity 检查。
    /// 所有 R133 模式匹配不变量（栈布局/失败 patch 回填/嵌套递归语义）原样保留。
    void emitMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches);

    /// emitMatchPattern 子阶段：LITERAL 模式（DUP+literal+OP_EQUAL+JUMP_IF_FALSE+POP）
    void emitLiteralMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches);
    /// emitMatchPattern 子阶段：VARIABLE 模式（DUP+bindDestructureVar 绑定整个 scrut）
    void emitVariableMatchPattern(const MatchPattern& p);
    /// emitMatchPattern 子阶段：VARIANT 模式（OP_ENUM_VARIANT_NAME 检查 + 字段递归匹配）
    void emitVariantMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches);
    /// emitMatchPattern 子阶段：TUPLE 模式（TYPE_TEST + LEN 检查 + 元素递归匹配）
    void emitTupleMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches);
    /// emitMatchPattern 子阶段：OR 模式（依次尝试子 pattern，任一成功跳到 OR 末尾）
    void emitOrMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches);
    /// emitMatchPattern 子阶段：VARIANT/TUPLE 子 pattern 共用的失败路径发射
    /// 成功路径 JUMP 跳过失败代码 + 失败路径 POP elem/field + JUMP caseSkip + 回填成功 JUMP。
    /// subFailPatches 调用后将被清空（已回填）。调用前须已 emit 子 pattern 与成功路径 POP。
    void emitSubPatternFailPath(std::vector<size_t>& subFailPatches, std::vector<size_t>& caseSkipPatches, int line);

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
    /// cachedIndexVar: IndexAccess 接收者的预缓存索引变量名（__wb_idx_N），空表示无缓存。
    void emitMethodCallWriteback(const ASTNode* receiver, int line, const std::string& cachedIndexVar);

    /// 发射变量加载指令（OP_GET_LOCAL / OP_GET_UPVALUE / OP_GET_GLOBAL / OP_GET_VAR）。
    /// 根据 currentLocals_ / upvalue / global slot 解析变量位置并发射对应加载指令。
    void emitLoadVariable(const std::string& name, int line);

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
