#include "compiler/Compiler.h"
#include "ast/ModuleIsolation.h" // BUG-AUDIT-MOD-2: VM 模块隔离（非导出顶层名前缀化）
#include "common/Logger.h"
#include "common/RuntimeLimits.h"             // BUG-AUDIT-MOD-3: MAX_RECURSION_DEPTH
#include "common/TCO.h"                       // R109 TCO: 尾递归自调用识别
#include "compiler/BytecodeCache.h"           // P2-11: .minic 文件加载
#include "compiler/ExprStmtPop.h"             // AUDIT-R4 BUG-04: 表达式语句 POP 共享谓词
#include "compiler/IRSSA.h"                   // P2-10: gvnPass/licmPass/inlinePass
#include "compiler/RegisterBytecodeBackend.h" // PERF-14: 寄存器式后端
#include "interpreter/NumericUtils.h"         // 共享溢出检查（B6 fix）
#include "lexer/Lexer.h"                      // VM-IMPORT: 模块源码词法分析
#include "parser/Parser.h"                    // VM-IMPORT: 模块源码语法分析
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <sstream>

// ============================================================
// Compiler 字节码编译器实现
// ============================================================

/**
 * Compiler —— MiniLang 前端字节码编译器总入口
 *
 * 职责：将解析后的 AST（Block 语句列表）翻译为可在 VM 上执行的字节码。
 * 它是纯单遍（single-pass）递归下降式代码生成器：边遍历 AST 边 emit 指令，
 * 不构建独立的中间 AST 再遍历（栈式 VM 直接路径）。三条后端路径共享同一套
 * visit* 访问者，因此语义天然一致。
 *
 * ── 三条后端路径（由编译开关选择，见 compile()）─────────────────────────
 *  1) 栈式 VM 直接路径（默认）：visit* 直接 emit 栈式 OpCode，常量池写入主 chunk。
 *  2) IR 路径（useIR_）：AST → IR（AstIRBuilder）→ [可选优化 pass] → 栈式字节码
 *     （BytecodeIRBackend）。指令以操作数栈为模型，与路径 1 等价。
 *  3) 寄存器式 VM 路径（useRegisterVM_）：AST → IR → 寄存器式字节码
 *     （RegisterBytecodeBackend）。指令以固定寄存器窗口为模型，无操作数栈。
 *  路径 1 与 2/3 的关键差异在于 IR 路径经过统一的 AstIRBuilder / *IRBackend
 *  中间层，便于做常量折叠、复制传播、DCE 等优化 pass。
 *
 * ── 派发机制 ──────────────────────────────────────────────────────────
 *  compileNode(node) 通过 Visitor 模式调用 node->accept(*this)，回调对应
 *  visit* 方法。compileStatement() 在表达式语句之后补发 OP_POP 以平衡栈
 *  （见下“栈平衡纪律”）。
 *
 * ── 栈平衡纪律（仅栈式路径相关）──────────────────────────────────────
 *  每个 visit* 必须保证“离开时栈高度与进入时一致”，除非该节点作为表达式被调用，
 *  此时约定“产生恰好一个栈顶值，由调用方消费”。compileStatement 对产生栈值
 *  的表达式节点（赋值/调用/二元运算/字面量等）补发 OP_POP，避免顶层或循环体内
 *  栈无限累积导致运行时栈溢出。声明与控制流节点已自行平衡，不在其列。
 *
 * ── 闭包与自由变量 ────────────────────────────────────────────────────
 *  嵌套函数通过 collectFreeVars/computeFreeVars 前向分析出捕获的自由变量，
 *  再由 resolveUpvalue 在编译函数体时登记 UpvalueDesc（区分 isLocal 直接捕获
 *  与 isLocal=false 透传捕获），支撑 3+ 层嵌套闭包沿外层函数透传 upvalue。
 *
 * ── 常量折叠 ──────────────────────────────────────────────────────────
 *  tryFoldBinary/tryFoldUnary 在 visit* 中优先尝试编译期求值；仅当操作数
 *  均为编译期常量（extractConstant 递归提取）时折叠，并统一经 emitConstant
 *  写入常量池，避免对带副作用表达式误折叠。
 *
 * ── 模块系统 ──────────────────────────────────────────────────────────
 *  import/export 通过模块加载器与 ModuleIsolation 实现跨文件编译；每次编译
 *  都会重置 linkedModuleSet_/moduleLoadingSet_/moduleExports_ 等状态，
 *  防止跨次编译复用旧缓存（见 compile() 与 compileVia* 的清理段）。
 */

Compiler::Compiler() {}

CompileResult Compiler::compile(Block& program) {
    // A4 fix: 可选类型检查 pass（在 codegen 前运行）
    // 2026-06-29: 接线——将 TypeChecker 诊断合并到 diagnostics_ 输出为警告（不阻断编译）
    // 注意：typeDiag 必须在 diagnostics_.clear() 之后合并，否则会被各路径的
    // clear() 清空。此处先收集，待 clear 后再合并。
    DiagnosticBag typeDiag;
    if (enableTypeCheck_ && typeChecker_) {
        typeDiag = typeChecker_->check(program);
    }

    // PERF-14: 寄存器式 VM 路径（AST → IR → RegisterBytecode）
    // 启用后走 AstIRBuilder + RegisterBytecodeBackend，配合 RegisterVM 执行
    if (useRegisterVM_) {
        lastRegisterResult_ = compileViaRegisterIR(program);
        // compileViaRegisterIR 已清空 diagnostics_，此处合并类型检查诊断
        for (const auto& d : typeDiag.all()) {
            diagnostics_.add(d);
        }
        // 寄存器式路径返回空 CompileResult（调用方应使用 getLastRegisterResult()）
        CompileResult emptyResult;
        return emptyResult;
    }

    // ARCH-06: 可选 IR 中间层路径（AST → IR → Bytecode）
    // 启用后走 AstIRBuilder + BytecodeIRBackend（含完整特性 + 可选优化 pass）
    if (useIR_) {
        CompileResult irResult = compileViaIR(program);
        // compileViaIR 已清空 diagnostics_，此处合并类型检查诊断
        for (const auto& d : typeDiag.all()) {
            diagnostics_.add(d);
        }
        return irResult;
    }

    chunk_ = BytecodeChunk();
    chunk_.name = "main";
    chunk_.arity = 0;
    chunk_.reserveCode(1024); // C21: 预分配字节码空间
    varIndex_.clear();
    stringConstIndex_.clear(); // PERF-29 fix: 清空字符串常量去重 map
    diagnostics_.clear();
    // 合并类型检查诊断（在 clear 之后，确保不被清空）
    for (const auto& d : typeDiag.all()) {
        diagnostics_.add(d);
    }
    functionChunks_.clear();
    currentLocals_.clear();
    inFunction_ = false;
    classFieldNames_.clear();
    outerLocals_.clear();
    writebackCounter_ = 0; // #24: reset for clean variable names across compilations
    peakLocals_ = 0;
    blockDepth_ = 0;
    blockSaveCounter_ = 0;        // L11 fix: 编译间重置块保存计数器
    compilingMethodBody_ = false; // AUDIT-R7 F5: 编译间重置方法体标志
    compileDepth_ = 0;            // P1 fix: 编译间重置递归深度计数器
    globalSlotAllocator_.clear(); // B4: 重置全局槽位分配器
    innerFunctions_.clear();      // H5 fix: 重置内嵌函数追踪
    innerFunctionSlots_.clear();  // H5 fix
    // VM-IMPORT: 清理模块系统状态（每次编译重置，避免跨编译复用旧缓存）
    linkedModuleSet_.clear();
    moduleLoadingSet_.clear();
    moduleLoadingStack_.clear(); // BUG-AUDIT-MOD-3: 深度保护栈
    moduleExports_.clear();      // BUG-AUDIT-MOD-1: export 名称集合
    moduleExportSlots_.clear();  // AUDIT-R6 F6 fix: R5 快照槽位记录（漏清则残留旧槽号，
    moduleExportVars_.clear();   // 实例复用时 emitImportSnapshotCopy 按旧槽越界读）
    moduleAsts_.clear();
    pendingEnumInfos_.clear(); // R99 enum 校验：编译间重置

    // A2: pre-scan top-level declarations to assign global slots (eliminates forward-reference issues)
    // BUG-PRES-1 fix: 与 preScanModuleGlobals() 保持一致，覆盖 ExportStmt（解包 VarDecl/ClassDecl 内部声明）。
    // 原实现仅覆盖 VarDecl + ClassDecl，导致顶层 `export var x` / `export class C` 使用慢路径（OP_DEFINE_VAR），
    // 且前向引用行为不一致（普通变量返回 null，导出变量报运行时错误）。
    // R98 W2 fix: 现在也预扫描 FunDecl/ExportStmt(FunDecl)——visitFunDecl 顶层路径已用
    // OP_DEFINE_GLOBAL 将闭包值写入 globalSlots_[slot]（替代原 OP_POP 丢弃），使函数名可作为值引用
    // （map/filter/reduce/forEach/find 高阶函数的闭包参数路径）。原"不预扫描 FunDecl"的顾虑
    // （预扫描会导致 var f = funName 从"报错"变为"静默 null"）已不适用——slot 现在持有真实闭包值。
    for (auto& stmt : program.statements) {
        if (!stmt)
            continue;
        switch (stmt->nodeType) {
        case NodeType::NODE_VAR_DECL:
            allocateGlobalSlot(static_cast<VarDecl*>(stmt.get())->name);
            break;
        case NodeType::NODE_CLASS_DECL:
            allocateGlobalSlot(static_cast<ClassDecl*>(stmt.get())->name);
            break;
        case NodeType::NODE_FUN_DECL:
            allocateGlobalSlot(static_cast<FunDecl*>(stmt.get())->name);
            break;
        case NodeType::NODE_EXPORT_STMT: {
            // ExportStmt 包装内部声明——提取声明名并分配槽位
            auto* exportNode = static_cast<ExportStmt*>(stmt.get());
            if (exportNode->declaration) {
                ASTNode* decl = exportNode->declaration.get();
                switch (decl->nodeType) {
                case NodeType::NODE_VAR_DECL:
                    allocateGlobalSlot(static_cast<VarDecl*>(decl)->name);
                    break;
                case NodeType::NODE_CLASS_DECL:
                    allocateGlobalSlot(static_cast<ClassDecl*>(decl)->name);
                    break;
                case NodeType::NODE_FUN_DECL:
                    allocateGlobalSlot(static_cast<FunDecl*>(decl)->name);
                    break;
                default:
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    // 编译所有顶层语句（不调用 compileBlock，确保 blockDepth_=0 为真正顶层）
    for (auto& stmt : program.statements) {
        compileStatement(stmt.get());
    }

    // 末尾添加 null + RETURN（main chunk 必须有返回值，否则 OP_RETURN 弹栈下溢）
    chunk_.writeOp(OpCode::OP_NULL, 0);
    chunk_.writeOp(OpCode::OP_RETURN, 0);

    CompileResult result;
    result.mainChunk = std::move(chunk_);
    result.functionChunks = std::move(functionChunks_);
    result.globalSlotCount = globalSlotAllocator_.count();
    result.globalSlotNames = std::move(globalSlotAllocator_.mutableNames()); // B4: move 避免深拷贝
    result.enumInfos = std::move(pendingEnumInfos_);                         // R99 enum 校验：编译期→运行时传递

    // 预计算 ip→指令索引映射（用于调试高亮 O(1) 查找）
    result.mainChunk.buildIpMap();
    for (auto& kv : result.functionChunks) {
        kv.second.buildIpMap();
    }

    // Perf-LazyLog: 改用 LOG_INFO 宏，级别过滤后不构造消息字符串（避免 std::to_string 的 locale 查询 + 堆分配）
    LOG_INFO("字节码编译完成: " + std::to_string(result.globalSlotCount) + " 全局槽, " +
                 std::to_string(result.functionChunks.size()) + " 函数chunk",
             "Compiler");
    return result;
}

// ============================================================
// ARCH-06: IR 中间层编译路径
// ============================================================
// AST → IR（AstIRBuilder）→ [可选优化 pass] → Bytecode（BytecodeIRBackend）
// IR 路径已支持完整特性：闭包 upvalue、写回指令、全局槽位分配、默认参数、块作用域。
// 方向二：当 irOptimize_=true 时，在 lowering 前执行常量折叠/复制传播/死代码消除。
CompileResult Compiler::compileViaIR(Block& program) {
    diagnostics_.clear();

    // 阶段 1：AST → IR
    AstIRBuilder irBuilder;
    // VM-IMPORT: 转发模块加载器给 AstIRBuilder，使 IR 路径也支持 import
    irBuilder.setModuleLoader(moduleLoader_);
    // 清理上次编译的模块状态（对齐 compile() 直接路径的清理）
    // P3-A6: 补齐 moduleLoadingStack_ 和 moduleExports_ 的清理（与 compileViaRegisterIR
    // 的 P2-B fix 对齐，原 P2-B 修复时漏掉了 compileViaIR 路径，三路径状态重置不对称）
    linkedModuleSet_.clear();
    moduleLoadingSet_.clear();
    moduleLoadingStack_.clear();
    moduleExports_.clear();
    moduleExportSlots_.clear(); // AUDIT-R6 F6 fix: R5 新增成员同步清理（与 compile() 对齐）
    moduleExportVars_.clear();  // AUDIT-R6 F6 fix
    moduleAsts_.clear();
    lastIR_ = irBuilder.build(program);
    // BUG-INH-AUDIT-1 fix: 先检查 hasError() 再检查 !lastIR_。
    // build() 在错误时返回 nullptr，原顺序下 !lastIR_ 先触发 "IR 构建失败"，
    // 吞掉具体错误消息（如 super 编译错误）。改为先检查 hasError() 传播具体消息。
    // P2-12 fix: 合并 AstIRBuilder 的 DiagnosticBag 到 Compiler::diagnostics_，
    // 消除"私有字段→手动 error() 转化"的冗余路径。
    if (irBuilder.hasError()) {
        auto irDiags = irBuilder.takeDiagnostics();
        for (const auto& d : irDiags.all()) {
            diagnostics_.add(d);
        }
        if (!irDiags.hasErrors()) {
            error("IR 构建失败", 0, 0); // 防御性兜底
        }
        CompileResult emptyResult;
        return emptyResult;
    }
    if (!lastIR_) {
        error("IR 构建失败", 0, 0);
        CompileResult emptyResult;
        return emptyResult;
    }
    // VM-IMPORT: 收集 AstIRBuilder 保留的模块 AST（确保函数/类定义指针在编译期有效）
    auto irModuleAsts = irBuilder.takeModuleAsts();
    for (auto& ast : irModuleAsts) {
        moduleAsts_.push_back(std::move(ast));
    }

    // 把 mainFunction 放回 module_ 供 lowerModule 使用
    // build() 返回时已 move 出 module_->mainFunction，需临时放回
    IRModule* module = irBuilder.getModule();
    module->mainFunction = std::move(lastIR_);
    // BUG-NEW fix: 在 lowerModule 前填充全局槽位名表，供 BytecodeIRBackend
    // 将 WRITEBACK_*_VAR 的 GLOBAL_SLOT 转换为变量名常量索引。
    module->globalSlotNames = irBuilder.getGlobalSlotNames();

    // 方向二：IR 优化 pass（可选）
    // Phase-5 fix: 栈式后端现可安全启用 DCE+CSE。POP 携带所消费的 vreg 操作数，
    // DCE 看到消费关系不会误删；isPureCompute 扩展含算术指令，
    // CSE 产生的冗余算术可被 DCE 安全清理。复制传播仍禁用（删除 LOAD_CONST 会导致栈下溢）。
    if (irOptimize_) {
        optimizeIR(*module->mainFunction, /*enableCopyPropagation=*/false, /*enableDCE=*/true, /*enableCSE=*/true);
        for (auto& fn : module->functions) {
            if (fn)
                optimizeIR(*fn, /*enableCopyPropagation=*/false, /*enableDCE=*/true, /*enableCSE=*/true);
        }
    }

    // 阶段 2：IR → Bytecode（整个 module: main + 子函数）
    BytecodeIRBackend backend;
    if (!backend.lowerModule(*module)) {
        error("IR lowering 失败", 0, 0);
        CompileResult emptyResult;
        return emptyResult;
    }
    auto mainChunk = backend.takeChunk();
    if (!mainChunk) {
        error("IR lowering 未生成字节码", 0, 0);
        CompileResult emptyResult;
        return emptyResult;
    }

    // 末尾添加 null + RETURN（main chunk 必须有返回值）
    mainChunk->writeOp(OpCode::OP_NULL, 0);
    mainChunk->writeOp(OpCode::OP_RETURN, 0);
    mainChunk->buildIpMap();

    CompileResult result;
    result.mainChunk = std::move(*mainChunk);
    result.functionChunks = backend.takeFunctionChunks();
    result.globalSlotNames = irBuilder.getGlobalSlotNames();
    result.globalSlotCount = static_cast<int>(result.globalSlotNames.size());
    result.enumInfos = irBuilder.takeEnumInfos(); // R99 enum 校验

    // 为函数 chunks 构建 ipMap（用于调试高亮）
    for (auto& kv : result.functionChunks) {
        kv.second.buildIpMap();
    }

    // 恢复 lastIR_ 供调试/可视化使用
    lastIR_ = std::move(module->mainFunction);

    // 方向四：保存 main 函数的 IR→字节码偏移映射（用于 VM 单步时高亮 IR 指令）
    lastIRToBytecodeOffset_ = backend.irToBytecodeOffset();

    // Perf-LazyLog: LOG_INFO 宏级别过滤后跳过字符串构造
    LOG_INFO("IR 编译完成: " + std::to_string(lastIR_->blocks.size()) + " 基本块, " +
                 std::to_string(lastIR_->constants.size()) + " 常量, " + std::to_string(lastIR_->nextVReg) + " vreg, " +
                 std::to_string(result.functionChunks.size()) + " 函数chunk, " +
                 std::to_string(result.globalSlotCount) + " 全局槽" + (irOptimize_ ? " (已优化)" : ""),
             "Compiler-IR");
    return result;
}

// ============================================================
// PERF-14: 寄存器式 VM 编译路径
// AST → IR（AstIRBuilder）→ [可选优化 pass] → RegisterBytecode（RegisterBytecodeBackend）
// ============================================================
RegisterCompileResult Compiler::compileViaRegisterIR(Block& program) {
    diagnostics_.clear();

    // 阶段 1：AST → IR
    AstIRBuilder irBuilder;
    // VM-IMPORT: 转发模块加载器给 AstIRBuilder，使寄存器式路径也支持 import
    irBuilder.setModuleLoader(moduleLoader_);
    // L11: 转发预编译模块解析器（仅 RegisterVM 路径，compileViaIR 不转发）
    irBuilder.setPrecompiledModuleResolver(precompiledModuleResolver_);
    // P2-B fix: 对齐 compile() 直接路径，补齐 moduleLoadingStack_ 和 moduleExports_ 的清理
    linkedModuleSet_.clear();
    moduleLoadingSet_.clear();
    moduleLoadingStack_.clear();
    moduleExports_.clear();
    moduleExportSlots_.clear(); // AUDIT-R6 F6 fix: R5 新增成员同步清理（与 compile() 对齐）
    moduleExportVars_.clear();  // AUDIT-R6 F6 fix
    moduleAsts_.clear();
    lastIR_ = irBuilder.build(program);
    // BUG-INH-AUDIT-1 fix: 先检查 hasError() 再检查 !lastIR_（同 compile IR 路径）。
    // P2-12 fix: 合并 AstIRBuilder 的 DiagnosticBag 到 Compiler::diagnostics_
    if (irBuilder.hasError()) {
        auto irDiags = irBuilder.takeDiagnostics();
        for (const auto& d : irDiags.all()) {
            diagnostics_.add(d);
        }
        if (!irDiags.hasErrors()) {
            error("IR 构建失败", 0, 0); // 防御性兜底
        }
        RegisterCompileResult emptyResult;
        return emptyResult;
    }
    if (!lastIR_) {
        error("IR 构建失败", 0, 0);
        RegisterCompileResult emptyResult;
        return emptyResult;
    }
    // VM-IMPORT: 收集 AstIRBuilder 保留的模块 AST
    auto irModuleAsts = irBuilder.takeModuleAsts();
    for (auto& ast : irModuleAsts) {
        moduleAsts_.push_back(std::move(ast));
    }

    // 把 mainFunction 放回 module_ 供 lowerModule 使用
    IRModule* module = irBuilder.getModule();
    module->mainFunction = std::move(lastIR_);

    // PERF-15: IR 优化 pass
    // AUDIT-BUG-E4 fix: 寄存器式下复制传播不安全——copyPropagationPass 将 VIRTUAL
    // 操作数替换为 CONSTANT kind，但 RegisterBytecodeBackend::lowerInstruction 不检查
    // 操作数 kind，直接 vregToReg(operand.index) 把常量索引当 vreg 编号，读错寄存器。
    // 修复方向：在 RegisterBytecodeBackend 中对 CONSTANT kind 操作数先 emit REG_LOAD_CONST
    // 到临时寄存器。在此修复落地前，寄存器路径禁用复制传播（常量折叠+DCE仍安全）。
    // BUG-IR-DCE-2 fix: 解耦 DCE 控制——寄存器式后端 DCE 安全，启用 enableDCE=true。
    // BUG-IR-OPT-1 fix: 不仅优化 main 函数，还需遍历 module->functions 优化所有子函数，
    // 否则子函数错过常量折叠/DCE 等优化。
    if (irOptimize_) {
        // P2-10 IR SSA 基础设施：函数内联在 optimizeIR 之前执行，
        // 使内联后的代码能被常量折叠/DCE 进一步优化。
        // inlinePass 操作整个 IRModule（需访问被调用函数 IR）。
        if (irSSAOptimize_) {
            inlinePass(*module);
        }
        optimizeIR(*module->mainFunction, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
        for (auto& fn : module->functions) {
            if (fn)
                optimizeIR(*fn, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
        }
        // P2-10 IR SSA 基础设施：GVN + LICM 在 optimizeIR 之后执行，
        // 对已优化的 IR 进行跨基本块值编号和循环不变外提。
        // 与 CSE 同源约束：仅寄存器式后端安全（替换 dest vreg 引用后无栈残留）。
        // P2-10 fix: ssaConstructPass/ssaDestructPass 必须包裹 GVN/LICM——
        // GVN 的跨块 CSE 和 LICM 的循环不变外提依赖 PHI 节点跟踪变量在不同
        // 控制流路径的值。不构造 SSA 时，LOAD_LOCAL/STORE_LOCAL 语义被保留
        // 但变量值无法跨块追踪，LICM 误判不变表达式导致语义错误。
        //
        // P2-10 fix2: 仅当 ssaConstructPass 返回 true（实际插入 PHI）时才运行
        // GVN/LICM。ssaConstructPass 仅处理 LOCAL 变量（STORE_LOCAL），当函数只
        // 含 GLOBAL 变量（如顶层代码 var/print）时返回 false，此时 GVN 的跨块 CSE
        // 会创建跨循环 live range（vreg 在外层循环定义、内层循环使用），而
        // RegisterBytecodeBackend 的 collectVRegLastUse 是线性扫描不识别回边，
        // 会在首次迭代的"最后使用"点释放寄存器并复用，后续迭代读到错误值。
        // SSA 构造+析构路径安全：PHI 在 ssaDestruct 还原为 LOAD_LOCAL，每轮迭代
        // 重新加载，无跨循环 live range 问题。
        if (irSSAOptimize_) {
            bool ssaBuilt = ssaConstructPass(*module->mainFunction);
            if (ssaBuilt) {
                gvnPass(*module->mainFunction);
                licmPass(*module->mainFunction);
                ssaDestructPass(*module->mainFunction);
                // SSA 析构后清理死 LOAD_LOCAL（ssaDestruct 将 PHI 还原为 LOAD_LOCAL，
                // 部分 LOAD_LOCAL dest 可能无引用，DCE 安全删除）
                optimizeIR(*module->mainFunction, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
            }
            for (auto& fn : module->functions) {
                if (fn) {
                    bool fnSsaBuilt = ssaConstructPass(*fn);
                    if (fnSsaBuilt) {
                        gvnPass(*fn);
                        licmPass(*fn);
                        ssaDestructPass(*fn);
                        optimizeIR(*fn, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
                    }
                }
            }
        }
    }

    // 阶段 2：IR → RegisterBytecode（整个 module: main + 子函数）
    RegisterBytecodeBackend backend;
    if (!backend.lowerModule(*module)) {
        error("Register IR lowering 失败", 0, 0);
        RegisterCompileResult emptyResult;
        return emptyResult;
    }

    RegisterCompileResult result;
    // R103 W1 fix: mainFunction 的 chunk 保留在 chunk_ 中，通过 takeChunk() 获取。
    // 原实现把 mainChunk 放入 functionChunks_["main"]，当用户源码含 `fun main()`
    // 时子函数 "main" 覆盖 mainFunction 的 chunk，导致 RegVM 执行用户函数体而非
    // 顶层代码。对齐 BytecodeIRBackend 的设计：mainFunction → chunk_，子函数 → functionChunks_。
    auto mainChunk = backend.takeChunk();
    if (!mainChunk) {
        error("Register IR lowering 未生成 main 字节码", 0, 0);
        RegisterCompileResult emptyResult;
        return emptyResult;
    }
    result.mainChunk = std::move(*mainChunk);
    result.functionChunks = backend.takeFunctionChunks();
    result.globalSlotNames = irBuilder.getGlobalSlotNames();
    result.globalSlotCount = static_cast<int>(result.globalSlotNames.size());
    result.enumInfos = irBuilder.takeEnumInfos(); // R99 enum 校验

    // 恢复 lastIR_ 供调试/可视化使用
    lastIR_ = std::move(module->mainFunction);

    // L11 预编译模块（RegisterVM）：post-lowering 合并 pending 模块
    // AstIRBuilder::handleImportStmt 对每个 .minic 模块：
    //   1. 在 IR builder 的 globalSlotAllocator_ 中分配全局槽位（与 Compiler 的 allocator 分离）
    //   2. emit IR CALL 调用模块初始化函数（lowering 后为 REG_CALL）
    //   3. 记录 pending 模块（path, initFnName, line）
    // post-lowering 阶段加载 .minic 文件，将模块 RegBytecodeChunk 合并到结果。
    //
    // 关键不变量：sync Compiler::globalSlotAllocator_ 与 IR builder 的全局槽位名表。
    // loadPrecompiledRegisterModule 通过 allocateGlobalSlot（Compiler 的 allocator）分配
    // 模块全局槽位并构建 relocationMap。若不同步，Compiler 的 allocator 为空，会分配
    // 重复槽位（从 0 开始），导致 relocationMap 指向错误槽位，模块全局变量读写错位。
    auto pendingModules = irBuilder.takePendingRegPrecompiledModules();
    if (!pendingModules.empty()) {
        // 同步 Compiler::globalSlotAllocator_ 与 IR builder 的全局槽位名表
        globalSlotAllocator_.clear();
        for (const auto& gname : result.globalSlotNames) {
            globalSlotAllocator_.allocate(gname);
        }

        // 将 result 移到 lastRegisterResult_（loadPrecompiledRegisterModule 操作此成员）
        lastRegisterResult_ = std::move(result);

        // 逐个加载 pending 模块（.minic → RegBytecodeChunk 合并）
        for (const auto& pending : pendingModules) {
            if (!loadPrecompiledRegisterModule(pending.modulePath, pending.initFnName, pending.line)) {
                LOG_INFO("Register VM 预编译模块加载失败，可能需回退源码编译: " + pending.modulePath,
                         "Compiler-RegPrecompiled");
            }
        }

        // 合并 pendingEnumInfos_ 到结果（loadPrecompiledRegisterModule 收集模块 enum 元信息）
        for (auto& enumInfo : pendingEnumInfos_) {
            lastRegisterResult_.enumInfos.push_back(std::move(enumInfo));
        }
        pendingEnumInfos_.clear();

        // 从 allocator 更新全局槽位名表（理论上无新增，因 IR builder 已预分配；防御性同步）
        lastRegisterResult_.globalSlotNames = globalSlotAllocator_.names();
        lastRegisterResult_.globalSlotCount = static_cast<int>(lastRegisterResult_.globalSlotNames.size());

        // 移回 result 以复用下方 LOG_INFO + return
        result = std::move(lastRegisterResult_);
    }

    // Perf-LazyLog: LOG_INFO 宏级别过滤后跳过字符串构造
    LOG_INFO("Register IR 编译完成: " + std::to_string(result.mainChunk.code.size()) + " 字节, " +
                 std::to_string(result.mainChunk.registerCount) + " 寄存器, " +
                 std::to_string(result.functionChunks.size()) + " 函数chunk, " +
                 std::to_string(result.globalSlotCount) + " 全局槽" + (irOptimize_ ? " (已优化)" : ""),
             "Compiler-RegIR");
    return result;
}

std::string Compiler::getLastError() const {
    // 从 diagnostics_ 派生最后一条错误信息
    const auto& diags = diagnostics_.all();
    for (auto it = diags.rbegin(); it != diags.rend(); ++it) {
        if (it->isError()) {
            return it->format();
        }
    }
    return {};
}

// A2/B4: global slot management 已内联到 Compiler.h，委托给 globalSlotAllocator_

uint16_t Compiler::identifierIndex(const std::string& name) {
    auto it = varIndex_.find(name);
    if (it != varIndex_.end())
        return it->second;
    uint16_t idx = chunk_.addConstant(Value(name));
    varIndex_[name] = idx;
    return idx;
}

// 2026-06-29: 发射 OP_TYPE_CHECK — 将类型注解字符串加入常量池，发射检查指令
// 语义：peek 栈顶值，检查是否兼容类型注解，不弹栈。在 SET 操作前调用。
void Compiler::emitTypeCheck(const std::string& typeAnnotation, int line) {
    if (typeAnnotation.empty())
        return;
    // R163 泛型扩展：类型参数运行时擦除——不发射 OP_TYPE_CHECK，对齐 Interpreter 的 isTypeParameter 跳过逻辑
    if (!currentTypeParams_.empty() && minilang::isTypeParameter(typeAnnotation, currentTypeParams_)) {
        return;
    }
    uint16_t typeIdx = chunk_.addConstant(Value(typeAnnotation));
    chunk_.writeOp(OpCode::OP_TYPE_CHECK, line);
    chunk_.writeShort(typeIdx, line);
}

// AUDIT-P1.1 fix: break/continue finally 续跳机制。
// 若 break/continue 在 try-finally 内，发射 OP_PUSH_JUMP_TARGET + OP_JUMP 续跳字节码。
// 编译期布局（两层 try-finally 示例，A 内层、B 外层）：
//   OP_PUSH_JUMP_TARGET <breakTarget>       ← 最外层 finally 执行完后跳转
//   OP_PUSH_JUMP_TARGET <B.finallyEntry>    ← 内层 finally 执行完后跳转
//   OP_JUMP <A.finallyEntry>                ← 跳到最内层 finally
// 执行顺序：A.finally → OP_FINALLY_END(pop B.finallyEntry, jump) → B.finally →
//           OP_FINALLY_END(pop breakTarget, jump) → breakTarget
// 多层嵌套同理：从外到内依次 push，从内到外依次执行 finally 后续跳。
bool Compiler::emitFinallyJump(int line, std::vector<size_t>& realTargetPatches) {
    // 收集所有 enclosing try-finally 块（从内到外）
    // finallyIndices[0] = innermost, finallyIndices[N-1] = outermost
    std::vector<size_t> finallyIndices;
    for (size_t i = tryFinallyStack_.size(); i > 0; --i) {
        if (tryFinallyStack_[i - 1].hasFinally) {
            finallyIndices.push_back(i - 1);
        }
    }
    if (finallyIndices.empty())
        return false;

    // 从最外层到最内层，依次发射 OP_PUSH_JUMP_TARGET
    // 循环 i 从 N 递减到 1，对应 ctxIdx 从 finallyIndices[N-1]（outermost）到 finallyIndices[0]（innermost）。
    // push 顺序：outermost → innermost，使 pendingJumpStack_ 弹出顺序为 innermost → outermost，
    // 与 finally 执行顺序（内层先执行）匹配。
    // BUG-025 fix: 原代码 `if (i == 1)` 与 `finallyIndices[i - 2]` 索引反转。
    //   原意：最外层（i==N）push realTarget，内层 push 下一个外层 finally 的 entry。
    //   实际：最内层（i==1）push realTarget，外层 push 下一个内层 finally 的 entry。
    //   修复：判据改为 `if (i == finallyIndices.size())`，索引改为 `finallyIndices[i]`。
    for (size_t i = finallyIndices.size(); i > 0; --i) {
        chunk_.writeOp(OpCode::OP_PUSH_JUMP_TARGET, line);
        size_t patch = chunk_.code.size();
        chunk_.writeShort(0, line); // 占位，待回填

        if (i == finallyIndices.size()) {
            // 最外层：push 真实跳转目标（breakTarget/continueTarget），由循环编译完成后回填。
            // BUG-026 fix: realTargetPatches 进入 breakJumps/continueJumps 后，回填代码使用
            //   `code[patch+1]`/`code[patch+2]`（patch 为 opcode 位置）。
            //   但此处 patch 是 opcode 之后的位置（第一操作数字节），需回退一字节到 opcode 位置。
            realTargetPatches.push_back(patch - 1);
        } else {
            // 内层：push 下一个（更外层）finally 入口。finallyIndices[i] 比 finallyIndices[i-1] 更外。
            size_t nextCtxIdx = finallyIndices[i];
            tryFinallyStack_[nextCtxIdx].pendingTargetPatches.push_back(patch);
        }
    }

    // 发射 OP_JUMP 到最内层 finally 入口（待回填）
    chunk_.writeOp(OpCode::OP_JUMP, line);
    size_t jumpPatch = chunk_.code.size();
    chunk_.writeShort(0, line); // 占位
    tryFinallyStack_[finallyIndices[0]].pendingJumpPatches.push_back(jumpPatch);

    return true;
}

// ============================================================
// C3 fix: 编译上下文 RAII 守卫实现
// ============================================================
// visitFunDecl 需保存/恢复 15 个成员变量。原代码手动 std::move 保存 +
// 手动恢复（含异常路径），极易遗漏。CompileContextGuard 构造时调用
// saveCompileContext()，析构时调用 restoreCompileContext()，自动覆盖
// early return 和异常路径。

Compiler::CompileContext Compiler::saveCompileContext() {
    CompileContext ctx;
    ctx.chunk = std::move(chunk_);
    ctx.varIndex = std::move(varIndex_);
    ctx.currentLocals = std::move(currentLocals_);
    ctx.varTypes = std::move(varTypes_); // 2026-06-29: 类型注解快照
    ctx.inFunction = inFunction_;
    ctx.outerLocals = std::move(outerLocals_);
    ctx.peakLocals = peakLocals_;
    ctx.outerUpvalues = std::move(outerUpvalues_);
    ctx.outerUpvalueNames = std::move(outerUpvalueNames_);
    ctx.outerFunctions = std::move(outerFunctions_);
    ctx.innerFunctions = std::move(innerFunctions_);
    ctx.innerFunctionSlots = std::move(innerFunctionSlots_);
    ctx.currentUpvalues = std::move(currentUpvalues_);
    ctx.currentUpvalueNames = std::move(currentUpvalueNames_);
    ctx.loopStack = std::move(loopStack_);
    ctx.tryDepth = tryDepth_;
    ctx.tryFinallyStack = std::move(tryFinallyStack_);          // AUDIT-P1.1 fix
    ctx.currentFunctionReturnType = currentFunctionReturnType_; // BUG-TYPE-1 fix
    ctx.currentTypeParams = currentTypeParams_;                 // R163 泛型扩展
    // TCO: 保存当前函数 TCO 状态
    ctx.currentFunctionName = currentFunctionName_;
    ctx.currentFunctionDecl = currentFunctionDecl_;
    ctx.currentFunctionEntryIp = currentFunctionEntryIp_;
    // L1 fix: 保存局部变量名映射和 IP 范围表，避免嵌套函数污染外层函数的调试器元数据
    ctx.localSlotNames = std::move(localSlotNames_);
    ctx.slotNameRanges = std::move(slotNameRanges_);
    // R164 fixup2: 保存字符串字面量去重缓存，避免函数 chunk 的索引泄漏到外层 chunk
    ctx.stringConstIndex = std::move(stringConstIndex_);
    return ctx;
}

void Compiler::restoreCompileContext(CompileContext&& ctx) {
    chunk_ = std::move(ctx.chunk);
    varIndex_ = std::move(ctx.varIndex);
    currentLocals_ = std::move(ctx.currentLocals);
    varTypes_ = std::move(ctx.varTypes); // 2026-06-29: 类型注解恢复
    inFunction_ = ctx.inFunction;
    outerLocals_ = std::move(ctx.outerLocals);
    peakLocals_ = ctx.peakLocals;
    outerUpvalues_ = std::move(ctx.outerUpvalues);
    outerUpvalueNames_ = std::move(ctx.outerUpvalueNames);
    outerFunctions_ = std::move(ctx.outerFunctions);
    innerFunctions_ = std::move(ctx.innerFunctions);
    innerFunctionSlots_ = std::move(ctx.innerFunctionSlots);
    currentUpvalues_ = std::move(ctx.currentUpvalues);
    currentUpvalueNames_ = std::move(ctx.currentUpvalueNames);
    loopStack_ = std::move(ctx.loopStack);
    tryDepth_ = ctx.tryDepth;
    tryFinallyStack_ = std::move(ctx.tryFinallyStack);                     // AUDIT-P1.1 fix
    currentFunctionReturnType_ = std::move(ctx.currentFunctionReturnType); // BUG-TYPE-1 fix
    currentTypeParams_ = std::move(ctx.currentTypeParams);                 // R163 泛型扩展
    // TCO: 恢复当前函数 TCO 状态
    currentFunctionName_ = std::move(ctx.currentFunctionName);
    currentFunctionDecl_ = ctx.currentFunctionDecl;
    currentFunctionEntryIp_ = ctx.currentFunctionEntryIp;
    // L1 fix: 恢复外层函数的局部变量名映射和 IP 范围表
    localSlotNames_ = std::move(ctx.localSlotNames);
    slotNameRanges_ = std::move(ctx.slotNameRanges);
    // R164 fixup2: 恢复外层 chunk 的字符串字面量去重缓存
    stringConstIndex_ = std::move(ctx.stringConstIndex);
}

void Compiler::compileNode(ASTNode* node) {
    if (!node)
        return;

    // P1 fix: 递归深度保护，防止深度嵌套 AST 导致 C++ 栈溢出
    if (compileDepth_ >= MAX_COMPILE_DEPTH) {
        error("编译嵌套过深（超过 " + std::to_string(MAX_COMPILE_DEPTH) + " 层）", 0, 0);
        return;
    }
    compileDepth_++;
    struct CompileDepthGuard {
        int& d;
        ~CompileDepthGuard() { d--; }
    } guard{compileDepth_};

    // 通过 Visitor 模式分派：accept 会回调对应的 visit* 方法
    node->accept(*this);
}

void Compiler::compileStatement(ASTNode* node) {
    if (!node)
        return;

    compileNode(node);

    // 表达式语句（如 foo(); 或 a = 5;）会产生一个栈上返回值但不会被消费，
    // 若不弹出会导致栈无限累积（main chunk 无外层帧清理，循环内泄漏必然触发栈溢出）。
    // 声明/控制流节点（VarDecl/IfStmt/WhileStmt 等）已自行平衡栈，
    // IndexAssign/MemberAssign 也已自行平衡，此处对产生栈值的表达式补发 OP_POP。
    // AUDIT-R4 BUG-04 fix: 改用共享谓词 needsPopForExprStmt（ExprStmtPop.h）——
    // 原本地 switch 清单与 IR 路径双份维护，R134 仅在 IR 路径补入
    // MATCH_EXPR/ENUM_VARIANT_EXPR/TUPLE_LITERAL 三种节点，本处漂移遗漏——
    // match 表达式/enum variant/元组字面量作为裸表达式语句时结果未 POP，
    // 循环内逐次栈泄漏。
    // AUDIT-R5 BUG-01 fix: 改用节点级重载，匿名 lambda（fun(x){...};）的 POP
    // 特判并入共享谓词（与 IR 路径单一事实源）：具名函数声明不会进入
    // expressionStatement（parser 走 declaration），FunCall 包裹 lambda（fun(){...}(5);）
    // 由 NODE_FUN_CALL 分支处理，仅裸 lambda 表达式语句命中 FUN_DECL+空名分支。
    if (needsPopForExprStmt(node)) {
        chunk_.writeOp(OpCode::OP_POP, node->line);
    }
}

