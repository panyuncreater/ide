#include "compiler/Compiler.h"
#include "ast/ModuleIsolation.h" // BUG-AUDIT-MOD-2: VM 模块隔离（非导出顶层名前缀化）
#include "common/Logger.h"
#include "common/RuntimeLimits.h"             // BUG-AUDIT-MOD-3: MAX_RECURSION_DEPTH
#include "common/TCO.h"                       // R109 TCO: 尾递归自调用识别
#include "compiler/RegisterBytecodeBackend.h" // PERF-14: 寄存器式后端
#include "interpreter/NumericUtils.h"         // 共享溢出检查（B6 fix）
#include "lexer/Lexer.h"                      // VM-IMPORT: 模块源码词法分析
#include "parser/Parser.h"                    // VM-IMPORT: 模块源码语法分析
#include <algorithm>
#include <cstdint>
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
    compileDepth_ = 0;            // P1 fix: 编译间重置递归深度计数器
    globalSlotAllocator_.clear(); // B4: 重置全局槽位分配器
    innerFunctions_.clear();      // H5 fix: 重置内嵌函数追踪
    innerFunctionSlots_.clear();  // H5 fix
    // VM-IMPORT: 清理模块系统状态（每次编译重置，避免跨编译复用旧缓存）
    linkedModuleSet_.clear();
    moduleLoadingSet_.clear();
    moduleLoadingStack_.clear(); // BUG-AUDIT-MOD-3: 深度保护栈
    moduleExports_.clear();      // BUG-AUDIT-MOD-1: export 名称集合
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
    linkedModuleSet_.clear();
    moduleLoadingSet_.clear();
    moduleAsts_.clear();
    lastIR_ = irBuilder.build(program);
    // BUG-INH-AUDIT-1 fix: 先检查 hasError() 再检查 !lastIR_。
    // build() 在 hasError_ 时返回 nullptr，原顺序下 !lastIR_ 先触发 "IR 构建失败"，
    // 吞掉具体错误消息（如 super 编译错误）。改为先检查 hasError() 传播具体消息。
    if (irBuilder.hasError()) {
        error(irBuilder.errorMessage().empty() ? "IR 构建失败" : irBuilder.errorMessage(), irBuilder.errorLine(), 0);
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
    // BUG-IR-DCE-2 fix: 栈式后端 DCE 不安全（POP operands 为空导致误删 LOAD_CONST），
    // 显式传 enableDCE=false。
    // BUG-IR-OPT-1 fix: 不仅优化 main 函数，还需遍历 module->functions 优化所有子函数，
    // 否则子函数错过常量折叠等优化，与栈式非 IR 路径行为不一致。
    if (irOptimize_) {
        optimizeIR(*module->mainFunction, /*enableCopyPropagation=*/false, /*enableDCE=*/false);
        for (auto& fn : module->functions) {
            if (fn)
                optimizeIR(*fn, /*enableCopyPropagation=*/false, /*enableDCE=*/false);
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
    // P2-B fix: 对齐 compile() 直接路径，补齐 moduleLoadingStack_ 和 moduleExports_ 的清理
    linkedModuleSet_.clear();
    moduleLoadingSet_.clear();
    moduleLoadingStack_.clear();
    moduleExports_.clear();
    moduleAsts_.clear();
    lastIR_ = irBuilder.build(program);
    // BUG-INH-AUDIT-1 fix: 先检查 hasError() 再检查 !lastIR_（同 compile IR 路径）。
    if (irBuilder.hasError()) {
        error(irBuilder.errorMessage().empty() ? "IR 构建失败" : irBuilder.errorMessage(), irBuilder.errorLine(), 0);
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
        optimizeIR(*module->mainFunction, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
        for (auto& fn : module->functions) {
            if (fn)
                optimizeIR(*fn, /*enableCopyPropagation=*/false, /*enableDCE=*/true);
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
        size_t ctxIdx = finallyIndices[i - 1];
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
    switch (node->nodeType) {
    case NodeType::NODE_ASSIGNMENT:
    case NodeType::NODE_FUN_CALL:
    case NodeType::NODE_METHOD_CALL:
    case NodeType::NODE_BINARY_OP:
    case NodeType::NODE_UNARY_OP:
    case NodeType::NODE_VAR_REF:
    case NodeType::NODE_MEMBER_ACCESS:
    case NodeType::NODE_INDEX_ACCESS:
    case NodeType::NODE_NUMBER_LITERAL:
    case NodeType::NODE_STRING_LITERAL:
    case NodeType::NODE_BOOL_LITERAL:
    case NodeType::NODE_NULL_LITERAL:
    case NodeType::NODE_SUPER_EXPR:
    case NodeType::NODE_ARRAY_LITERAL:
    case NodeType::NODE_DICT_LITERAL:
    case NodeType::NODE_INTERPOLATED_STRING: // C5 fix: 插值字符串产生栈值
        chunk_.writeOp(OpCode::OP_POP, node->line);
        break;
    case NodeType::NODE_FUN_DECL: {
        // R98 W3: 匿名 lambda 作为表达式语句（如 `fun(x){...};` 或 `fun(x){...}(5);`）
        // 会留下闭包值在栈上（lambda 路径不 OP_POP/OP_DEFINE_GLOBAL）。
        // 具名函数声明（fun f(){...}）不会进入 expressionStatement（parser 走 declaration），
        // 所以这里只处理 lambda。但 FunCall 包裹 lambda 的情况（fun(){...}(5);）
        // 已被 NODE_FUN_CALL 分支处理（FunCall 自身 POP 调用结果），不会到这里。
        // 仅裸 lambda 表达式语句（fun(){...};）需要此处 POP。
        const auto& fnDecl = static_cast<const FunDecl&>(*node);
        if (fnDecl.name.empty()) {
            chunk_.writeOp(OpCode::OP_POP, node->line);
        }
        break;
    }
    default:
        break;
    }
}

void Compiler::visitBinaryOp(BinaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldBinary(node.opType, node.left.get(), node.right.get(), folded, node.line)) {
        emitConstant(folded, node.line);
        return;
    }

    // 短路运算特殊处理
    if (node.opType == BinOpType::BIN_AND) {
        compileNode(node.left.get());
        // 如果左操作数为假，跳过右操作数
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);           // 占位
        chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出左操作数
        compileNode(node.right.get());
        // 修补跳转地址
        uint16_t jumpTarget = safeCodeOffset();
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(jumpTarget & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((jumpTarget >> 8) & 0xFF);
        // M1 fix: 移除 NOT NOT 双重取反，保留操作数原始值（JS 语义）
        return;
    }

    if (node.opType == BinOpType::BIN_OR) {
        compileNode(node.left.get());
        // 如果左操作数为真，跳过右操作数，保留左值
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line); // 占位（先跳到 or 右侧）
        // 如果到这里，左操作数为真 — 保留左值，跳到末尾
        size_t jumpEnd = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line); // 占位
        // 修补第一个跳转：左操作数为假，求值右操作数
        uint16_t rightStart = safeCodeOffset();
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(rightStart & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((rightStart >> 8) & 0xFF);
        // 弹出左操作数，求值右操作数
        chunk_.writeOp(OpCode::OP_POP, node.line);
        compileNode(node.right.get());
        // 修补第二个跳转
        uint16_t endTarget = safeCodeOffset();
        chunk_.code[jumpEnd + 1] = static_cast<uint8_t>(endTarget & 0xFF);
        chunk_.code[jumpEnd + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
        // M1 fix: 移除 NOT NOT 双重取反，保留操作数原始值（JS 语义）
        return;
    }

    // 普通二元运算
    compileNode(node.left.get());
    compileNode(node.right.get());

    OpCode op = OpCode::OP_ADD;
    switch (node.opType) {
    case BinOpType::BIN_ADD:
        op = OpCode::OP_ADD;
        break;
    case BinOpType::BIN_SUB:
        op = OpCode::OP_SUBTRACT;
        break;
    case BinOpType::BIN_MUL:
        op = OpCode::OP_MULTIPLY;
        break;
    case BinOpType::BIN_DIV:
        op = OpCode::OP_DIVIDE;
        break;
    case BinOpType::BIN_MOD:
        op = OpCode::OP_MODULO;
        break;
    case BinOpType::BIN_EQ:
        op = OpCode::OP_EQUAL;
        break;
    case BinOpType::BIN_NEQ:
        op = OpCode::OP_NOT_EQUAL;
        break;
    case BinOpType::BIN_LT:
        op = OpCode::OP_LESS;
        break;
    case BinOpType::BIN_GT:
        op = OpCode::OP_GREATER;
        break;
    case BinOpType::BIN_LTE:
        op = OpCode::OP_LESS_EQUAL;
        break;
    case BinOpType::BIN_GTE:
        op = OpCode::OP_GREATER_EQUAL;
        break;
    default:
        error("不支持的运算符: " + std::string(BinaryOp::opTypeStr(node.opType)), node.line, node.column);
        return;
    }

    chunk_.writeOp(op, node.line);
    return;
}

void Compiler::visitUnaryOp(UnaryOp& node) {
    // 常量折叠：编译期求值常量表达式
    Value folded;
    if (tryFoldUnary(node.opType, node.operand.get(), folded, node.line)) {
        emitConstant(folded, node.line);
        return;
    }

    compileNode(node.operand.get());
    switch (node.opType) {
    case UnaryOp::UnaryOpType::UOP_NEGATE:
        chunk_.writeOp(OpCode::OP_NEGATE, node.line);
        break;
    case UnaryOp::UnaryOpType::UOP_NOT:
        chunk_.writeOp(OpCode::OP_NOT, node.line);
        break;
    case UnaryOp::UnaryOpType::UOP_PLUS:
        break; // 一元 + 恒等操作，操作数已在栈上
    default:
        break;
    }
    return;
}

void Compiler::visitNumberLiteral(NumberLiteral& node) {
    // A1 fix: 用 getValue() 按需构造 Value（节点存储为标量）
    Value v = node.getValue();
    uint16_t idx = chunk_.addConstant(v);
    if (v.isInt()) {
        chunk_.writeOp(OpCode::OP_INT, node.line);
    } else {
        chunk_.writeOp(OpCode::OP_FLOAT, node.line);
    }
    chunk_.writeShort(idx, node.line);
    return;
}

void Compiler::visitStringLiteral(StringLiteral& node) {
    // PERF-29 fix: 字符串字面量去重，避免相同字符串（如多次出现的 "hello"）重复存入常量池。
    // 常量池大小减少 20-40%（视程序），字节码加载稍快。
    auto it = stringConstIndex_.find(node.value);
    uint16_t idx;
    if (it != stringConstIndex_.end()) {
        idx = it->second; // 复用已有常量池索引
    } else {
        idx = chunk_.addConstant(node.getValue()); // A1 fix: getValue()
        stringConstIndex_[node.value] = idx;
    }
    chunk_.writeOp(OpCode::OP_STRING, node.line);
    chunk_.writeShort(idx, node.line);
    return;
}

// C5 fix: 插值字符串编译 — 发射字面量 + 表达式 + OP_ADD 链
// 等价于原 BinaryOp(BIN_ADD) 链的字节码，但 AST 结构得以保留
void Compiler::visitInterpolatedString(InterpolatedString& node) {
    // 发射首个字面量片段到栈顶
    if (!node.literals.empty()) {
        uint16_t idx = chunk_.addConstant(Value(node.literals[0]));
        chunk_.writeOp(OpCode::OP_STRING, node.line);
        chunk_.writeShort(idx, node.line);
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }

    // 交替发射: 表达式 → OP_ADD → 字面量 → OP_ADD
    for (size_t i = 0; i < node.expressions.size(); ++i) {
        compileNode(node.expressions[i].get());
        chunk_.writeOp(OpCode::OP_ADD, node.line);
        if (i + 1 < node.literals.size() && !node.literals[i + 1].empty()) {
            uint16_t idx = chunk_.addConstant(Value(node.literals[i + 1]));
            chunk_.writeOp(OpCode::OP_STRING, node.line);
            chunk_.writeShort(idx, node.line);
            chunk_.writeOp(OpCode::OP_ADD, node.line);
        }
    }
    return;
}

void Compiler::visitBoolLiteral(BoolLiteral& node) {
    chunk_.writeOp(node.value ? OpCode::OP_TRUE : OpCode::OP_FALSE, node.line);
    return;
}

void Compiler::visitVarDecl(VarDecl& node) {
    // 编译初始化表达式
    if (node.initializer) {
        compileNode(node.initializer.get());
    } else if (!node.typeAnnotation.empty() && classFieldNames_.find(node.typeAnnotation) != classFieldNames_.end()) {
        // S2 fix: 类名类型注解无初始化器 → 自动构造实例（与解释器 visitVarDecl 一致）
        uint16_t classNameIdx = identifierIndex(node.typeAnnotation);
        chunk_.writeOp(OpCode::OP_CLASS_NEW, node.line);
        chunk_.writeShort(classNameIdx, node.line);
        chunk_.write(static_cast<uint8_t>(0), node.line); // argCount = 0
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }

    // 2026-06-29: 类型注解运行时检查（三后端统一强制）
    // 在 SET 操作前发射 OP_TYPE_CHECK，peek 栈顶值检查类型兼容性
    // 类名注解也检查（实例类型匹配），null 兼容所有类型
    emitTypeCheck(node.typeAnnotation, node.line);
    // 记录类型注解，供后续 Assignment 检查
    if (!node.typeAnnotation.empty()) {
        varTypes_[node.name] = node.typeAnnotation;
    }

    // 在函数体内使用局部变量
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it == currentLocals_.end()) {
            // 新局部变量，分配槽位
            int slot = static_cast<int>(currentLocals_.size());
            // C-P1-2 fix: 局部变量槽位上限 255（uint8_t 编码限制）
            if (slot > 255) {
                error("函数局部变量数量超过限制（最大 256 个，含 this/参数/字段）", node.line, node.column);
                return;
            }
            currentLocals_[node.name] = slot;
            peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
            // BUG-IDE-12 fix: 记录 slot→name 映射（跨作用域累积，不随作用域退出清除）
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = node.name;
            // L1 fix: 记录 (slot, name, startIp) 到 slotNameRanges_，endIp 暂设为 0
            // （由 closeSlotRanges 在块退出时回填）。resolveSlotName 按 ip 反查变量名，
            // 解决兄弟作用域槽位复用导致的变量名错位。
            slotNameRanges_.push_back({static_cast<uint8_t>(slot), node.name, chunk_.code.size(), 0});
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(slot), node.line);
        } else {
            // B2 fix: 与解释器行为一致，同作用域重复声明报错
            error("变量 '" + node.name + "' 已在当前作用域中定义", node.line, node.column);
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
        }
        // OP_SET_LOCAL 用 peek(0) 不消费栈顶值，补发 OP_POP 平衡栈，
        // 避免函数内循环 var 声明累积导致栈溢出。
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // A2: use integer slot for known globals
        // Bug2 fix: 块作用域内不使用预分配的全局槽位，避免覆盖外层全局变量
        int slot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(node.name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
        // B4: topLevelGlobals_ 已删除（write-only 死代码，从未被读取）
    }
    return;
}

void Compiler::visitAssignment(Assignment& node) {
    compileNode(node.value.get());

    // 2026-06-29: 类型注解运行时检查（赋值时强制）
    // 查找变量声明的类型注解，有则发射 OP_TYPE_CHECK（peek 栈顶，不弹栈）
    const std::string* varType = findVarType(node.name);
    if (varType) {
        emitTypeCheck(*varType, node.line);
    }

    // C2 fix: 赋值作为表达式应产生值。先 DUP 保留一份在栈上，
    // SET 操作消费原始值后，DUP 的副本留在栈顶供外层表达式使用。
    chunk_.writeOp(OpCode::OP_DUP, node.line);

    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            // OP_SET_LOCAL 用 peek(0) 不消费栈顶值，配合 OP_POP 清理 DUP 的副本
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            chunk_.writeOp(OpCode::OP_POP, node.line);
        } else {
            // VM-05/06: 尝试解析为 upvalue（闭包捕获外层变量赋值）
            int uvIdx = resolveUpvalue(node.name, node.line);
            if (uvIdx >= 0) {
                chunk_.writeOp(OpCode::OP_SET_UPVALUE, node.line);
                chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
                chunk_.writeOp(OpCode::OP_POP, node.line); // 清理 DUP 副本
                return;
            }
            // A2: use integer slot for known globals
            int slot = lookupGlobalSlot(node.name);
            if (slot >= 0) {
                chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
                chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
            } else {
                uint16_t nameIdx = identifierIndex(node.name);
                chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            }
        }
    } else {
        // A2: use integer slot for known globals
        int slot = lookupGlobalSlot(node.name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    }
    return;
}

// VM-05/06: 解析闭包捕获变量为 upvalue 索引
// 返回 upvalue 在 currentUpvalues_ 中的索引，-1 表示不是闭包变量
int Compiler::resolveUpvalue(const std::string& name, int line) {
    // 1. 检查是否已在当前 upvalue 列表中（去重）
    auto existIt = currentUpvalueNames_.find(name);
    if (existIt != currentUpvalueNames_.end()) {
        return existIt->second;
    }

    // 2. 检查外层局部变量（直接捕获，isLocal=true）
    auto outerIt = outerLocals_.find(name);
    if (outerIt != outerLocals_.end()) {
        UpvalueDesc desc;
        desc.isLocal = true;
        desc.index = outerIt->second;
        desc.name = name; // 条件断点修复(R75): 记录upvalue变量名
        int idx = static_cast<int>(currentUpvalues_.size());
        currentUpvalues_.push_back(desc);
        currentUpvalueNames_[name] = idx;
        return idx;
    }

    // C-P1-3 fix: 3. 检查外层函数的 upvalue（透传捕获，isLocal=false）
    // 原代码将此检查嵌套在步骤 2 内部，导致永远不会执行（变量不可能同时是外层局部变量和 upvalue）
    auto outerUvIt = outerUpvalueNames_.find(name);
    if (outerUvIt != outerUpvalueNames_.end()) {
        UpvalueDesc desc;
        desc.isLocal = false;
        desc.index = outerUvIt->second; // 外层 upvalue 索引
        desc.name = name;               // 条件断点修复(R75): 记录upvalue变量名
        int idx = static_cast<int>(currentUpvalues_.size());
        currentUpvalues_.push_back(desc);
        currentUpvalueNames_[name] = idx;
        return idx;
    }
    return -1;
}

// BUG-UV-1 fix: 前向自由变量分析实现（对齐 IR 路径 AstIRBuilder::computeFreeVars）
// 在编译子函数体前预建 upvalue，使中间函数捕获内层引用的变量供透传。
// 解决 3+ 层嵌套闭包问题：fun outer(){var x=1; fun mid(){ fun inner(){return x;} ... }}
// mid 不直接引用 x，但 inner 需要，mid 必须捕获 x 供 inner 透传。
bool Compiler::isDefinedInScopes(const std::vector<std::unordered_set<std::string>>& scopes,
                                 const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (it->count(name))
            return true;
    }
    return false;
}

void Compiler::collectFreeVars(const ASTNode& node, std::vector<std::unordered_set<std::string>>& scopes,
                               std::unordered_set<std::string>& freeVars) {
    switch (node.nodeType) {
    case NodeType::NODE_VAR_REF: {
        const auto& ref = static_cast<const VarRef&>(node);
        if (!isDefinedInScopes(scopes, ref.name)) {
            freeVars.insert(ref.name);
        }
        break;
    }
    case NodeType::NODE_ASSIGNMENT: {
        const auto& assign = static_cast<const Assignment&>(node);
        if (!isDefinedInScopes(scopes, assign.name)) {
            freeVars.insert(assign.name);
        }
        if (assign.value)
            collectFreeVars(*assign.value, scopes, freeVars);
        break;
    }
    case NodeType::NODE_VAR_DECL: {
        const auto& decl = static_cast<const VarDecl&>(node);
        if (decl.initializer)
            collectFreeVars(*decl.initializer, scopes, freeVars);
        scopes.back().insert(decl.name);
        break;
    }
    case NodeType::NODE_FUN_DECL: {
        const auto& nestedFn = static_cast<const FunDecl&>(node);
        for (const auto& dv : nestedFn.defaultValues) {
            if (dv)
                collectFreeVars(*dv, scopes, freeVars);
        }
        std::vector<std::unordered_set<std::string>> nestedScopes;
        nestedScopes.emplace_back();
        for (const auto& p : nestedFn.params)
            nestedScopes.back().insert(p);
        // R98 W3: 匿名 lambda（name 为空）不插入空名到作用域
        if (!nestedFn.name.empty()) {
            nestedScopes.back().insert(nestedFn.name);
        }
        std::unordered_set<std::string> nestedFree;
        if (nestedFn.body)
            collectFreeVars(*nestedFn.body, nestedScopes, nestedFree);
        for (const auto& name : nestedFree) {
            if (!isDefinedInScopes(scopes, name)) {
                freeVars.insert(name);
            }
        }
        // R98 W3: 匿名 lambda 不注册到外层作用域
        if (!nestedFn.name.empty()) {
            scopes.back().insert(nestedFn.name);
        }
        break;
    }
    case NodeType::NODE_FUN_CALL: {
        const auto& call = static_cast<const FunCall&>(node);
        if (call.callee) {
            collectFreeVars(*call.callee, scopes, freeVars);
        } else if (!call.name.empty()) {
            if (!isDefinedInScopes(scopes, call.name)) {
                freeVars.insert(call.name);
            }
        }
        for (const auto& arg : call.arguments) {
            if (arg)
                collectFreeVars(*arg, scopes, freeVars);
        }
        break;
    }
    case NodeType::NODE_BLOCK: {
        const auto& block = static_cast<const Block&>(node);
        scopes.emplace_back();
        for (const auto& stmt : block.statements) {
            if (stmt)
                collectFreeVars(*stmt, scopes, freeVars);
        }
        scopes.pop_back();
        break;
    }
    case NodeType::NODE_FOR_STMT: {
        const auto& forStmt = static_cast<const ForStmt&>(node);
        scopes.emplace_back();
        if (forStmt.initializer)
            collectFreeVars(*forStmt.initializer, scopes, freeVars);
        if (forStmt.condition)
            collectFreeVars(*forStmt.condition, scopes, freeVars);
        if (forStmt.update)
            collectFreeVars(*forStmt.update, scopes, freeVars);
        if (forStmt.body)
            collectFreeVars(*forStmt.body, scopes, freeVars);
        scopes.pop_back();
        break;
    }
    case NodeType::NODE_TRY_STMT: {
        const auto& tryStmt = static_cast<const TryStmt&>(node);
        if (tryStmt.tryBlock)
            collectFreeVars(*tryStmt.tryBlock, scopes, freeVars);
        if (tryStmt.catchBlock) {
            scopes.emplace_back();
            if (!tryStmt.catchVarName.empty())
                scopes.back().insert(tryStmt.catchVarName);
            collectFreeVars(*tryStmt.catchBlock, scopes, freeVars);
            scopes.pop_back();
        }
        break;
    }
    case NodeType::NODE_CLASS_DECL: {
        const auto& cls = static_cast<const ClassDecl&>(node);
        scopes.back().insert(cls.name);
        break;
    }
    default:
        for (auto* child : node.children()) {
            if (child)
                collectFreeVars(*child, scopes, freeVars);
        }
        break;
    }
}

std::unordered_set<std::string> Compiler::computeFreeVars(const FunDecl& fn) {
    std::vector<std::unordered_set<std::string>> scopes;
    scopes.emplace_back();
    for (const auto& param : fn.params)
        scopes.back().insert(param);
    scopes.back().insert(fn.name);

    std::unordered_set<std::string> freeVars;
    for (const auto& dv : fn.defaultValues) {
        if (dv)
            collectFreeVars(*dv, scopes, freeVars);
    }
    if (fn.body)
        collectFreeVars(*fn.body, scopes, freeVars);
    return freeVars;
}

void Compiler::visitVarRef(VarRef& node) {
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            return;
        }
        // VM-05/06: 尝试解析为 upvalue（闭包捕获外层变量）
        int uvIdx = resolveUpvalue(node.name, node.line);
        if (uvIdx >= 0) {
            chunk_.writeOp(OpCode::OP_GET_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
            return;
        }
    }
    // A2: use integer slot for known globals
    int slot = lookupGlobalSlot(node.name);
    if (slot >= 0) {
        chunk_.writeOp(OpCode::OP_GET_GLOBAL, node.line);
        chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
    } else {
        uint16_t nameIdx = identifierIndex(node.name);
        chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
    }
    return;
}

void Compiler::visitIfStmt(IfStmt& node) {
    // C17 fix: 死代码消除 — 若条件可在编译期求值为常量布尔值（无副作用），
    // 直接编译存活分支，跳过条件求值与跳转指令，消除死代码。
    Value condConst;
    if (extractConstant(node.condition.get(), condConst, node.line) && condConst.isBool()) {
        auto savedLocals = currentLocals_;
        size_t blockSlotBase = savedLocals.size();
        if (condConst.boolVal()) {
            // 条件恒真：仅编译 then 分支
            compileStatement(node.thenBranch.get());
        } else if (node.elseBranch) {
            // 条件恒假：仅编译 else 分支
            compileStatement(node.elseBranch.get());
        }
        closeSlotRanges(blockSlotBase); // L1 fix: 回填本块内声明的变量 range 的 endIp
        currentLocals_ = savedLocals;
        return;
    }

    compileNode(node.condition.get());

    // 条件为假跳转到 else 分支
    size_t elseJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    // 保存外层作用域，then 分支内声明的变量不泄漏
    auto savedLocals = currentLocals_;

    // BUG-AUDIT-CLOSE-1 fix: 记录 then/else 分支的 slot 基址，分支退出时关闭 upvalue。
    // 对齐 IR 路径 leaveBlockScope 的 CLOSE_UPVALUE 发射语义。
    // 仅函数内（inFunction_）需要——顶层 if 块用 OP_DEFINE_VAR/OP_DELETE_VAR 操作 globals_，
    // 闭包不会捕获全局变量为 upvalue。对齐 IR.cpp leaveBlockScope 的 inFunction_ 守卫。
    size_t branchSlotBase = currentLocals_.size();
    bool needCloseUpvalue = inFunction_;

    // 编译 then 分支
    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值
    compileStatement(node.thenBranch.get());
    // BUG-AUDIT-CLOSE-1 fix: then 分支退出时关闭指向本分支 slot 的 open upvalues
    if (needCloseUpvalue && currentLocals_.size() > branchSlotBase && branchSlotBase <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(branchSlotBase), node.line);
    }
    closeSlotRanges(branchSlotBase); // L1 fix: 回填 then 分支内变量 range 的 endIp

    // then 分支变量不泄漏到 else/后续代码
    currentLocals_ = savedLocals;

    // 跳过 else 分支
    size_t endJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, node.line);
    chunk_.writeShort(0, node.line);

    // 修补 else 跳转
    uint16_t elseStart = safeCodeOffset();
    chunk_.code[elseJumpPatch + 1] = static_cast<uint8_t>(elseStart & 0xFF);
    chunk_.code[elseJumpPatch + 2] = static_cast<uint8_t>((elseStart >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值

    // 编译 else 分支（使用同样的 savedLocals，then 分支变量不可见）
    if (node.elseBranch) {
        compileStatement(node.elseBranch.get());
        // BUG-AUDIT-CLOSE-1 fix: else 分支退出时同样关闭 upvalue
        if (needCloseUpvalue && currentLocals_.size() > branchSlotBase && branchSlotBase <= 255) {
            chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(branchSlotBase), node.line);
        }
        closeSlotRanges(branchSlotBase); // L1 fix: 回填 else 分支内变量 range 的 endIp
    }

    // else 分支变量也不泄漏
    currentLocals_ = savedLocals;

    // 修补 end 跳转
    uint16_t endTarget = safeCodeOffset();
    chunk_.code[endJumpPatch + 1] = static_cast<uint8_t>(endTarget & 0xFF);
    chunk_.code[endJumpPatch + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
    return;
}

void Compiler::visitWhileStmt(WhileStmt& node) {
    // V3 fix: 保存局部变量映射，while 循环体内声明的变量不泄漏
    auto savedLocals = currentLocals_;

    size_t loopStart = chunk_.code.size();

    // 编译条件
    compileNode(node.condition.get());

    // 条件为假跳出循环
    size_t exitJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值（true 路径）

    // break/continue 循环上下文：while 的 continue 跳回 loopStart（条件检查）
    // break 跳转目标在循环编译完成后回填（跳过出口 OP_POP，因 break 时条件值已弹出）
    loopStack_.push_back({loopStart, exitJumpPatch, {}, {}, false, 0, tryDepth_});

    // BUG-AUDIT-CLOSE-1 fix: 记录循环体 slot 基址，循环体每次迭代退出时关闭 upvalue。
    // 对齐 IR 路径 leaveBlockScope 的 CLOSE_UPVALUE 发射语义。
    // 闭包捕获循环局部变量时，每次迭代退出时关闭 upvalue 产生该次迭代的快照（by-value），
    // 否则所有闭包指向同一 slot，最终都返回最后一次迭代的值（by-reference）。
    size_t bodySlotBase = currentLocals_.size();
    bool needCloseUpvalue = inFunction_;
    // AUDIT-P2-CORRECT fix: 记录到 LoopContext 供 visitBreakStmt 发射 OP_CLOSE_UPVALUE
    loopStack_.back().bodySlotBase = bodySlotBase;
    loopStack_.back().needCloseUpvalue = needCloseUpvalue;

    // 编译循环体
    compileStatement(node.body.get());
    closeSlotRanges(bodySlotBase); // L1 fix: 回填循环体内变量 range 的 endIp

    // AUDIT-P1-CORRECT fix: continueTarget 必须在 OP_CLOSE_UPVALUE 之前，
    // 使 continue 跳到 OP_CLOSE_UPVALUE 执行后再 OP_LOOP。
    // 第三十八轮将 continueTarget 放在 OP_CLOSE_UPVALUE 之后是方向性错误——
    // continue 跳过 OP_CLOSE_UPVALUE 导致 upvalue 不关闭，闭包捕获变 by-reference。
    size_t continueTarget = chunk_.code.size();

    // BUG-AUDIT-CLOSE-1 fix: 循环体每次迭代退出时关闭 upvalue
    if (needCloseUpvalue && currentLocals_.size() > bodySlotBase && bodySlotBase <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(bodySlotBase), node.line);
    }

    // 取出本层循环的 break/continue 跳转列表
    auto ctx = std::move(loopStack_.back());
    loopStack_.pop_back();

    // 回跳到条件检查
    uint16_t loopOffset = safeCodeOffset(loopStart);
    chunk_.writeOp(OpCode::OP_LOOP, node.line);
    chunk_.writeShort(loopOffset, node.line);

    // 修补退出跳转（正常退出：条件为假，条件值仍在栈上）
    uint16_t exitTarget = safeCodeOffset();
    chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
    chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值（false 路径）

    // 回填 continue 跳转：跳到 continueTarget（OP_CLOSE_UPVALUE 之前，执行关闭后再 OP_LOOP）
    uint16_t contTarget = safeCodeOffset(continueTarget);
    for (size_t patch : ctx.continueJumps) {
        chunk_.code[patch + 1] = static_cast<uint8_t>(contTarget & 0xFF);
        chunk_.code[patch + 2] = static_cast<uint8_t>((contTarget >> 8) & 0xFF);
    }

    // 回填 break 跳转：跳到当前偏移（OP_POP 之后，break 时栈上无条件值）
    uint16_t breakTarget = safeCodeOffset();
    for (size_t patch : ctx.breakJumps) {
        chunk_.code[patch + 1] = static_cast<uint8_t>(breakTarget & 0xFF);
        chunk_.code[patch + 2] = static_cast<uint8_t>((breakTarget >> 8) & 0xFF);
    }

    // V3+ fix: 顶层循环退出时清理循环体内声明的全局变量
    // Bug2 fix: 跳过有预分配全局槽位的变量（避免清空外层全局值）
    if (!inFunction_) {
        for (auto& [name, _] : currentLocals_) {
            if (savedLocals.find(name) == savedLocals.end()) {
                if (lookupGlobalSlot(name) < 0) {
                    uint16_t nameIdx = identifierIndex(name);
                    chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
                    chunk_.writeShort(nameIdx, node.line);
                }
            }
        }
    }

    // V3 fix: 恢复局部变量映射（P28: swap 避免二次拷贝）
    currentLocals_.swap(savedLocals);
    return;
}

void Compiler::visitForStmt(ForStmt& node) {
    // ── for 循环编译形状 ───────────────────────────────────────────────
    //   <initializer>                (statement，自带栈平衡)
    // loopStart:
    //   <condition> | OP_TRUE        (无条件循环用永真)
    //   OP_JUMP_IF_FALSE <exit>      (假则跳出)
    //   OP_POP                        (弹条件值，true 路径)
    //   <body>
    //   <update>                      ← continue 跳转目标（先执行更新再判条件）
    //   OP_JUMP <loopStart>
    // exit:
    //   OP_POP                        (弹条件值，false 路径)
    // 通过 loopStack_ 登记 loopStart/exitJumpPatch，使循环体内的 break 回填到 exit、
    // continue 回填到 update 之后；tryDepth_ 一并记录，保证 try 内 break/continue
    // 先发射 OP_TRY_END 弹出异常处理器（见 visitTryStmt）。

    // V3 fix: 保存局部变量映射，for 循环内声明的变量不泄漏到外层作用域
    auto savedLocals = currentLocals_;

    // 编译初始化
    if (node.initializer) {
        compileStatement(node.initializer.get());
    }

    size_t loopStart = chunk_.code.size();

    // 编译条件（无条件循环使用 OP_TRUE 作为永真条件）
    if (node.condition) {
        compileNode(node.condition.get());
    } else {
        chunk_.writeOp(OpCode::OP_TRUE, node.line);
    }

    size_t exitJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);
    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值（true 路径）

    // break/continue 循环上下文
    // continue 目标：有 update 时跳到 updateStart，否则跳到 loopStart
    // break 目标：循环编译完成后回填（跳过出口 OP_POP）
    bool hasUpdate = (node.update != nullptr);
    loopStack_.push_back({loopStart, exitJumpPatch, {}, {}, hasUpdate, 0, tryDepth_});

    // BUG-AUDIT-CLOSE-1 fix: 记录循环体 slot 基址，循环体每次迭代退出时关闭 upvalue。
    // 对齐 IR 路径 leaveBlockScope 的 CLOSE_UPVALUE 发射语义。
    size_t bodySlotBase = currentLocals_.size();
    bool needCloseUpvalue = inFunction_;
    // AUDIT-P2-CORRECT fix: 记录到 LoopContext 供 visitBreakStmt 发射 OP_CLOSE_UPVALUE
    loopStack_.back().bodySlotBase = bodySlotBase;
    loopStack_.back().needCloseUpvalue = needCloseUpvalue;

    // 编译循环体
    compileStatement(node.body.get());
    closeSlotRanges(bodySlotBase); // L1 fix: 回填循环体内变量 range 的 endIp

    // AUDIT-P1-CORRECT fix: updateStart（continue 目标）必须在 OP_CLOSE_UPVALUE 之前，
    // 使 continue 跳到 OP_CLOSE_UPVALUE 执行后再执行 update。
    // 第三十八轮将 updateStart 放在 OP_CLOSE_UPVALUE 之后是方向性错误——
    // continue 跳过 OP_CLOSE_UPVALUE 导致 upvalue 不关闭。
    size_t updateStart = chunk_.code.size();

    // BUG-AUDIT-CLOSE-1 fix: 循环体每次迭代退出时关闭 upvalue（在 update 之前）
    if (needCloseUpvalue && currentLocals_.size() > bodySlotBase && bodySlotBase <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(bodySlotBase), node.line);
    }

    // 取出本层循环的 break/continue 跳转列表
    auto ctx = std::move(loopStack_.back());
    loopStack_.pop_back();

    // 编译更新表达式（compileStatement 会自动 POP 赋值留下的栈值）
    if (node.update) {
        compileStatement(node.update.get());
    }

    // 回跳到条件检查
    chunk_.writeOp(OpCode::OP_LOOP, node.line);
    chunk_.writeShort(safeCodeOffset(loopStart), node.line);

    // 修补退出跳转（正常退出：条件为假，条件值仍在栈上）
    uint16_t exitTarget = safeCodeOffset();
    chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
    chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);
    chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出条件值（false 路径）

    // 回填 continue 跳转：有 update 跳到 updateStart，否则跳到 loopStart
    uint16_t contTarget = ctx.hasUpdate ? safeCodeOffset(updateStart) : safeCodeOffset(ctx.loopStart);
    for (size_t patch : ctx.continueJumps) {
        chunk_.code[patch + 1] = static_cast<uint8_t>(contTarget & 0xFF);
        chunk_.code[patch + 2] = static_cast<uint8_t>((contTarget >> 8) & 0xFF);
    }

    // 回填 break 跳转：跳到当前偏移（OP_POP 之后，break 时栈上无条件值）
    uint16_t breakTarget = safeCodeOffset();
    for (size_t patch : ctx.breakJumps) {
        chunk_.code[patch + 1] = static_cast<uint8_t>(breakTarget & 0xFF);
        chunk_.code[patch + 2] = static_cast<uint8_t>((breakTarget >> 8) & 0xFF);
    }

    // V3+ fix: 顶层循环退出时清理循环变量
    // Bug2 fix: 跳过有预分配全局槽位的变量（避免清空外层全局值）
    if (!inFunction_) {
        std::vector<std::string> cleanupVars;
        for (auto& [name, _] : currentLocals_) {
            if (savedLocals.find(name) == savedLocals.end()) {
                cleanupVars.push_back(name);
            }
        }
        for (const auto& name : cleanupVars) {
            if (lookupGlobalSlot(name) < 0) {
                uint16_t nameIdx = identifierIndex(name);
                chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            }
        }
    }

    // V3 fix: 恢复局部变量映射（P28: swap）
    currentLocals_.swap(savedLocals);
    return;
}

void Compiler::visitFunDecl(FunDecl& node) {
    // ── 函数编译总策略 ─────────────────────────────────────────────────
    // 每个 FunDecl 编译为一个独立的 BytecodeChunk（函数体），并被闭包化：
    //   emit OP_CLOSURE <arity> <upvalueCount> <upvalue表> 把函数 chunk 包成运行时闭包值。
    // 函数体帧布局遵循调用约定 [this][fields][args][locals]（见 VMCalls.cpp）：
    //   形参按声明顺序从局部槽 0 起分配；必需参数个数写入 chunk_.requiredArity，
    //   默认参数在调用侧（visitFunCall）按需补发，这里只登记其常量索引。
    // 嵌套函数（isInner）会先把外层 currentLocals_ 降级为 outerLocals_，供
    //   内层 collectFreeVars/resolveUpvalue 检测并透传闭包捕获（见上文文件头）。
    // 关键不变量：函数编译是完全可重入的——所有编译上下文（chunk_/currentLocals_/
    // currentUpvalues_/loopStack_/tryDepth_ 等）必须在进入/离开本函数时正确保存与
    // 恢复，否则嵌套定义会污染外层上下文。
    //
    // 拆分说明（原 224 行单函数 → orchestrator + 3 子阶段）：
    //   - declareFunction：设置 outerLocals_/outerUpvalues_、创建函数 chunk、参数槽位、
    //                      BUG-UV-1 前向自由变量分析（预建 upvalue）
    //   - compileFunctionBody：编译 body + 隐式 OP_NULL/OP_RETURN + 保存元数据
    //   - emitDefaultValues：F10 默认参数值常量化（与 emitMethodBody 共用）
    // 三子阶段均在 CompileContextGuard 作用域内执行，guard 析构自动恢复外层上下文。

    // H5 fix: 记录是否为内嵌函数（在函数体内定义的函数）
    bool isInner = inFunction_;

    // R98 W3: 匿名 lambda（node.name 为空）使用合成名 `$lambda_N` 作为内部 key。
    // 合成名用于：functionChunks_ 存储、identifierIndex 常量池、innerFunctionSlots_ 注册。
    // 合成名不暴露给用户——错误消息中显示 `<lambda>`，不参与 OP_CALL 按名查找。
    // $ 不在标识符首字符集中（Lexer 不接受 $），合成名不会与用户变量名冲突。
    std::string effectiveName = node.name;
    bool isLambda = node.name.empty();
    if (isLambda) {
        effectiveName = "$lambda_" + std::to_string(lambdaCounter_++);
    }

    // C3 fix: 使用 CompileContextGuard RAII 自动保存/恢复 15 个编译上下文成员变量。
    // 原代码 22 处 std::move 保存 + 22 处手动恢复（含异常路径），极易遗漏。
    // 守卫在块作用域结束时自动恢复，包括 early return 和异常路径。
    int upvalueCount = 0;
    {
        CompileContextGuard guard(*this);

        if (!declareFunction(node, guard.saved, effectiveName)) {
            return; // guard 自动恢复上下文（参数超限）
        }
        compileFunctionBody(node);
        emitDefaultValues(node);

        // R164 协程/生成器：标记生成器 chunk + 复制 yieldCount
        // VM 在 OP_CALL 时检测 isGenerator，若为 true 则创建协程值而非直接调用。
        // yieldCount 从 AST 复制（静态数或 kDynamicYieldCount=INT_MAX 表示循环内 yield）。
        chunk_.isGenerator = node.isGenerator;
        chunk_.yieldCount = node.yieldCount;

        // VM-05/06: 将 upvalue 描述符附加到函数 chunk
        chunk_.upvalues = std::move(currentUpvalues_);

        // 存储函数 chunk（用 effectiveName 作为 key，支持匿名 lambda）
        functionChunks_[effectiveName] = std::move(chunk_);

        // 记录 upvalue 数量（块外需要用于 OP_CLOSURE 发射）
        upvalueCount = static_cast<int>(functionChunks_[effectiveName].upvalues.size());

        // guard 在块结束时自动恢复外层上下文
    }

    // 此时已恢复外层上下文，在主 chunk 中 emit OP_CLOSURE
    uint16_t nameIdx = identifierIndex(effectiveName);
    const BytecodeChunk& funChunk = functionChunks_[effectiveName];
    // AUDIT-BUG-C3 fix: upvalueCount 经 static_cast<uint8_t> 编码，
    // > 255 时静默截断低 8 位，解码端按截断值读取 upvalue 描述符导致闭包捕获错误变量集。
    // 与 RegisterBytecodeBackend.cpp:489 对齐，发射前显式检查上限，避免半成品字节码。
    if (upvalueCount > 255) {
        error("闭包 upvalue 数量超过 255 上限", node.line, 0);
        return;
    }
    for (int i = 0; i < upvalueCount; ++i) {
        if (funChunk.upvalues[i].index > 255) {
            error("闭包 upvalue 索引超过 255 上限", node.line, 0);
            return;
        }
    }
    chunk_.writeOp(OpCode::OP_CLOSURE, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(upvalueCount), node.line);
    // 写入每个 upvalue 的描述符
    for (int i = 0; i < upvalueCount; ++i) {
        chunk_.write(funChunk.upvalues[i].isLocal ? 1 : 0, node.line);
        chunk_.write(static_cast<uint8_t>(funChunk.upvalues[i].index), node.line);
    }
    // R98 W3: 匿名 lambda 不存储到任何变量槽——闭包值留在栈上作为表达式结果。
    // 调用方（如 var f = fun(x){...}; 或 map(arr, fun(x){...});）从栈顶取值。
    if (isLambda) {
        // lambda 作为表达式求值，闭包值留在栈上，不 OP_POP 也不 OP_SET_LOCAL
        // 注意：若 lambda 出现在表达式语句上下文（如 `fun(x){...}(5);` 立即调用），
        // 调用结果会被外层 expressionStatement 的 POP 清理。
        return;
    }
    // H5 fix: 内嵌函数存储为局部变量（供 OP_CALL_EXPR 使用），全局函数直接弹出
    if (isInner) {
        // 分配局部变量槽位存储闭包值
        int slot = static_cast<int>(currentLocals_.size());
        currentLocals_[node.name] = slot;
        peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
        innerFunctions_.insert(node.name);
        innerFunctionSlots_[node.name] = slot;
        chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
        chunk_.write(static_cast<uint8_t>(slot), node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // R98 W2 fix: 全局函数的闭包值存储到全局槽位（而非 POP 丢弃）。
        // 原 H5 fix 假设"顶层函数仅通过 OP_CALL 按名查找 functionChunks_/functionClosures_，
        // 永不作为值使用"，但 W2 高阶函数 map/filter/reduce/forEach/find 打破此假设——
        // map(arr, double) 中 double 作为参数值被 visitVarRef 编译为 OP_GET_GLOBAL <slot>，
        // 若全局槽位为 null（闭包值被 POP 丢弃），OP_GET_GLOBAL 取到 null，
        // invokeClosureSync 报"高阶函数的参数必须是函数"或上游报"未定义的变量: double"。
        // 修复：顶层函数也用 OP_DEFINE_GLOBAL 将闭包值存入全局槽位，使函数名可作为值引用。
        // 安全性：(1) OP_CALL 仍通过 functionClosures_ 按名查找，不读取 globalSlots_，不受影响；
        // (2) W1-BUG2 fix 的 isClassName/isInFunctionChunks 分流仍有效——直接调用 double(5)
        // 因 isInFunctionChunks=true 走 OP_CALL，不读 globalSlots_；
        // (3) 函数重定义 fun f(){} fun f(){} 第二次 OP_DEFINE_GLOBAL 覆盖第一次，语义正确。
        int globalSlot = lookupGlobalSlot(node.name);
        if (globalSlot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(globalSlot), node.line);
        } else {
            // 无全局槽位（pre-scan 未注册）时退化为 OP_POP 保持向后兼容
            chunk_.writeOp(OpCode::OP_POP, node.line);
        }
    }

    // VM-05/06: 如果在函数内，将本函数注册到 outerLocals_ 和 outerFunctions_ 供更内层捕获
    if (inFunction_) {
        // H5 fix: 内嵌函数已有真实槽位，使用它；全局函数使用虚拟槽位
        int slot = isInner ? innerFunctionSlots_[node.name] : peakLocals_;
        outerLocals_[node.name] = slot;
        outerFunctions_[node.name] = slot;
    }
    return;
}

// ============================================================
// visitFunDecl 子阶段实现
// ============================================================

// R164 协程/生成器：yield 表达式编译
// 语义: 编译 yield 值表达式到栈顶（无值时 push null），然后发射 OP_YIELD。
// VM 在重放模式下比较运行时 yield 执行计数器与目标 yieldId：
//   - 命中目标: 抛出 VMYieldSignal 返回 yield 值
//   - 未命中: push yield 值回栈作为 yield 表达式结果，继续执行
void Compiler::visitYieldExpr(YieldExpr& node) {
    if (node.value) {
        compileNode(node.value.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    chunk_.writeOp(OpCode::OP_YIELD, node.line);
}

bool Compiler::declareFunction(FunDecl& node, const CompileContext& saved, const std::string& effectiveName) {
    // 如果当前在函数内，将当前函数的局部变量保存为外层局部变量（供嵌套函数检测闭包捕获）
    // C-P2-7 fix: 使用 saved.currentLocals（含外层函数局部变量）
    if (saved.inFunction) {
        outerLocals_ = saved.currentLocals;
        outerUpvalues_ = saved.currentUpvalues;
        outerUpvalueNames_ = saved.currentUpvalueNames;
    } else {
        outerLocals_.clear();
        outerUpvalues_.clear();
        outerUpvalueNames_.clear();
    }
    outerFunctions_.clear();
    if (saved.inFunction) {
        outerFunctions_ = saved.outerFunctions;
    }

    // 设置函数编译上下文
    // R161 fix: chunk_.name 必须用 effectiveName（匿名 lambda 为 `$lambda_N`），
    // 与 functionChunks_ 的 key 保持一致。JIT compileAllChunks 通过 chunk.name 反查
    // funcTable（key 同为 effectiveName），若 chunk.name 为空则找不到入口 Label 绑定
    // 导致 asmjit 跳转到未绑定 Label 崩溃。StackVM 不依赖 chunk.name（用 map key 查找），
    // 故此修改对 StackVM/RegisterVM/Interpreter 无影响。
    chunk_ = BytecodeChunk(effectiveName, static_cast<int>(node.params.size()));
    chunk_.reserveCode(256);
    // F10: 设置必需参数个数和默认值常量索引
    chunk_.requiredArity = node.requiredParamCount;
    varIndex_.clear();
    currentLocals_.clear();
    currentUpvalues_.clear();     // VM-05/06: 新的 upvalue 列表
    currentUpvalueNames_.clear(); // VM-05/06: 新的 upvalue 名称映射
    localSlotNames_.clear();      // BUG-IDE-12 fix: 清空槽位名映射
    slotNameRanges_.clear();      // L1 fix: 清空 IP 范围表
    stringConstIndex_.clear();    // R164 fixup2: 清空字符串常量去重缓存（新 chunk 有新常量池）
    inFunction_ = true;
    // C-P0-1/C-P0-3 fix: 函数体的循环栈和 try 深度从 0 开始
    loopStack_.clear();
    tryDepth_ = 0;
    // BUG-TYPE-1 fix (P1): 保存当前函数返回类型注解，供 visitReturnStmt 发射 OP_TYPE_CHECK。
    // 对齐 Interpreter 的 currentFunctionReturnType_ + CallFrameGuard 机制。
    currentFunctionReturnType_ = node.returnType;
    // R163 泛型扩展：记录当前函数的类型参数，供 emitTypeCheck 擦除类型参数注解
    currentTypeParams_ = node.typeParams;
    // TCO: 记录当前函数名与 FunDecl 指针，供 visitReturnStmt 识别 return f(args)。
    // 入口 IP 在 compileFunctionBody 中编译函数体首指令前记录。
    currentFunctionName_ = node.name;
    currentFunctionDecl_ = &node;

    // 编译参数到局部变量槽位
    // C-P1-2 fix: 参数数量上限 255（uint8_t 编码限制）
    if (node.params.size() > 255) {
        error("函数参数数量超过限制（最大 255 个）: " + node.name, node.line, 0);
        return false; // guard 自动恢复上下文
    }
    for (int i = 0; i < static_cast<int>(node.params.size()); ++i) {
        currentLocals_[node.params[i]] = i;
        // BUG-IDE-12 fix: 记录参数 slot→name
        if (static_cast<size_t>(i) >= localSlotNames_.size()) {
            localSlotNames_.resize(i + 1);
        }
        localSlotNames_[i] = node.params[i];
    }
    peakLocals_ = static_cast<int>(node.params.size());

    // BUG-UV-1 fix: 前向自由变量分析——在编译函数体前预建 upvalue。
    // 解决 3+ 层嵌套闭包问题：中间函数即使不直接引用外层变量，也需捕获供更内层函数透传。
    // 对齐 IR 路径 AstIRBuilder::visitFunDecl 的 computeFreeVars 调用。
    // 实现要点：computeFreeVars 递归遍历 AST 收集自由变量（含嵌套函数传播），
    // 然后对每个自由变量调用 resolveUpvalue 预建 upvalue 条目。
    // 注意：resolveUpvalue 依赖 outerLocals_/outerUpvalueNames_/outerFunctions_，
    // 这些已在上方 saved.inFunction 分支中正确设置。
    if (saved.inFunction) {
        auto freeVars = computeFreeVars(node);
        for (const auto& name : freeVars) {
            resolveUpvalue(name, node.line);
        }
    }

    return true;
}

void Compiler::compileFunctionBody(FunDecl& node) {
    // TCO: 记录函数体入口 IP（函数体首条字节码指令的偏移）。
    // 此时 chunk_.code 中可能仅有 OP_TRY_BEGIN 等外层指令（顶层函数无），
    // 但对函数体而言，compileNode(node.body) 写入的首条指令即为入口。
    // 必须在编译 body 之前记录，确保 visitReturnStmt 中 OP_JUMP 能回到此处。
    currentFunctionEntryIp_ = chunk_.code.size();

    // 编译函数体
    if (node.body) {
        compileNode(node.body.get());
    }

    // 末尾添加隐式返回 null
    chunk_.writeOp(OpCode::OP_NULL, node.line);
    chunk_.writeOp(OpCode::OP_RETURN, node.line);

    // 记录局部变量总槽位数
    chunk_.localCount = peakLocals_;
    // BUG-IDE-12 fix: 保存 slot→name 映射到 chunk，供 VM 条件断点求值
    chunk_.localSlotNames = localSlotNames_;
    // L1 fix: 保存基于 IP 范围的 slot→name 反查表，解决兄弟作用域槽位复用导致的
    // 变量名错位。调试器按 frame.ip 在 [startIp, endIp) 范围内反查变量名。
    chunk_.slotNameRanges = slotNameRanges_;
}

void Compiler::emitDefaultValues(FunDecl& node) {
    // F10: 编译默认参数值为常量
    // 仅支持字面量（Number/String/Bool/Null）和负数字面量，复杂表达式需通过 Interpreter 执行
    // 本函数由 visitFunDecl 与 emitMethodBody 共用，确保两条路径默认参数语义一致。
    for (size_t i = 0; i < node.defaultValues.size(); ++i) {
        if (node.defaultValues[i]) {
            const ASTNode* dv = node.defaultValues[i].get();
            Value constVal;
            bool isConst = false;

            if (dv->nodeType == NodeType::NODE_NUMBER_LITERAL) {
                constVal = static_cast<const NumberLiteral*>(dv)->getValue(); // A1 fix: getValue()
                isConst = true;
            } else if (dv->nodeType == NodeType::NODE_STRING_LITERAL) {
                constVal = static_cast<const StringLiteral*>(dv)->getValue(); // A1 fix: getValue()
                isConst = true;
            } else if (dv->nodeType == NodeType::NODE_BOOL_LITERAL) {
                constVal = static_cast<const BoolLiteral*>(dv)->getValue(); // A1 fix: getValue()
                isConst = true;
            } else if (dv->nodeType == NodeType::NODE_NULL_LITERAL) {
                constVal = Value::nullValue();
                isConst = true;
            } else if (dv->nodeType == NodeType::NODE_UNARY_OP) {
                // 支持负数字面量: -42, -3.14, --5 (双重否定)
                // C-P2-2 fix: 递归折叠嵌套一元运算，使 --5 等价于 5
                const ASTNode* cur = dv;
                int negateCount = 0;
                while (cur && cur->nodeType == NodeType::NODE_UNARY_OP) {
                    const auto* unary = static_cast<const UnaryOp*>(cur);
                    if (unary->opType != UnaryOp::UnaryOpType::UOP_NEGATE)
                        break;
                    ++negateCount;
                    cur = unary->operand.get();
                }
                if (cur && cur->nodeType == NodeType::NODE_NUMBER_LITERAL && negateCount > 0) {
                    // A1 fix: NumberLiteral 存储标量，用 isInt()/intVal() 直接访问
                    const NumberLiteral* numNode = static_cast<const NumberLiteral*>(cur);
                    if (numNode->isInt()) {
                        int64_t v = numNode->intVal();
                        // 奇数次取反为负，偶数次为正
                        constVal = Value((negateCount % 2 == 1) ? -v : v);
                    } else {
                        double v = numNode->floatVal(); // A1 fix: numNode 标量访问
                        constVal = Value((negateCount % 2 == 1) ? -v : v);
                    }
                    isConst = true;
                }
            }

            if (isConst) {
                uint16_t constIdx = chunk_.addConstant(constVal);
                chunk_.defaultConstIndices.push_back(constIdx);
            } else {
                // 复杂表达式默认值：VM 不支持，记录无效索引（0xFFFF）
                // Interpreter 路径仍可正常执行
                chunk_.defaultConstIndices.push_back(0xFFFF);
            }
        }
    }
}

void Compiler::visitFunCall(FunCall& node) {
    // 链式调用 / 表达式调用: callee(args)
    if (node.callee) {
        // C-P1-1 fix: 参数数量检查移到编译参数之前，避免截断后栈损坏
        if (node.arguments.size() > 255) {
            error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
            return;
        }
        // 编译被调用表达式（结果应为闭包值，推入栈顶）
        compileNode(node.callee.get());
        // 编译参数
        for (auto& arg : node.arguments) {
            compileNode(arg.get());
        }
        // OP_CALL_EXPR: 栈顶 N 个参数下方为闭包值
        chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
        chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
        return;
    }

    // H5 fix: 内嵌函数通过局部变量中的闭包值调用，避免 functionClosures_ 按名称覆盖
    auto innerIt = innerFunctionSlots_.find(node.name);
    if (innerIt != innerFunctionSlots_.end()) {
        // C-P1-1 fix: 参数数量检查移到编译参数之前
        if (node.arguments.size() > 255) {
            error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
            return;
        }
        // 先 push 闭包值（从局部变量获取）
        chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
        chunk_.write(static_cast<uint8_t>(innerIt->second), node.line);
        // 再编译参数
        for (auto& arg : node.arguments) {
            compileNode(arg.get());
        }
        chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
        chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
        return;
    }

    // W1 fix (R103): 对齐 IR.cpp CRITICAL-1 fix——若 node.name 是当前函数的
    // LOCAL 或 UPVALUE（持有闭包值，如 `var g = f; g();`），走 OP_CALL_EXPR 路径
    // 通过闭包值调用。原 StackVM 直接路径仅检查 innerFunctionSlots_（嵌套函数声明 slot），
    // 不覆盖 `var g = f;` 的普通局部变量闭包值场景，导致 StackVM 直接路径报
    // "未定义的函数: g"。IR 路径已通过 CRITICAL-1 fix 解决，直接路径未对齐。
    if (inFunction_) {
        auto localIt = currentLocals_.find(node.name);
        if (localIt != currentLocals_.end()) {
            if (node.arguments.size() > 255) {
                error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
                return;
            }
            chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
            for (auto& arg : node.arguments) {
                compileNode(arg.get());
            }
            chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
            chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
            return;
        }
        int uvIdx = resolveUpvalue(node.name, node.line);
        if (uvIdx >= 0) {
            if (node.arguments.size() > 255) {
                error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
                return;
            }
            chunk_.writeOp(OpCode::OP_GET_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(uvIdx), node.line);
            for (auto& arg : node.arguments) {
                compileNode(arg.get());
            }
            chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
            chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
            return;
        }
    } else {
        // W1 fix 扩展：顶层 var 持闭包值调用 (var g = makeAdder(10); g(32);)
        // 顶层 var 注册到 globalSlotAllocator_，此处通过 OP_GET_GLOBAL 加载闭包值
        // 后走 OP_CALL_EXPR。原 StackVM 直接路径走 OP_CALL 按名查找 functionChunks_，
        // 找不到 var 持有的闭包值，报"未定义的函数: g"。
        //
        // W1-BUG fix (R104): compile() pre-scan 步骤也将顶层 ClassDecl 注册到
        // globalSlotAllocator_（见第 142-143 行），但 `Foo()` 类实例化必须走 OP_CALL
        // （VM 运行时检测到名称是类时执行 OP_CLASS_NEW + 字段初始化）。
        // 原 W1 fix 扩展未区分 class 名与普通 var 闭包值名，导致 `Foo()` 被错误编译为
        // OP_GET_GLOBAL + OP_CALL_EXPR，VM 在 OP_CALL_EXPR 中找不到类构造入口而失败。
        // 修复：先用 classFieldNames_ 判定是否为类名，若是则跳过 W1 fix 扩展，
        // 落到下方 OP_CALL 路径（VM 运行时按名查找 functionChunks_ 或 classRegistry_）。
        //
        // W1-BUG2 fix (R104): 模块导入的 FunDecl（如 `import { counter } from "m";`）
        // 也被 preScanModuleGlobals 注册到 globalSlotAllocator_，但其闭包值在
        // visitFunDecl 顶层路径被 OP_POP 丢弃，全局槽位为 null。原 W1 fix 扩展未排除
        // 此场景，counter() 被错误编译为 OP_GET_GLOBAL + OP_CALL_EXPR，运行时
        // globalSlots_[slot] 为 null，OP_CALL_EXPR 报"表达式调用需要函数值"。
        // 修复：追加 functionChunks_ 查找——若名称在 functionChunks_ 中（已编译的
        // 函数声明，含模块导入的 FunDecl），走 OP_CALL 命名调用通过 functionChunks_
        // 查找。仅 var 持闭包值（不在 functionChunks_ 中）走 OP_CALL_EXPR。
        bool isClassName = (classFieldNames_.find(node.name) != classFieldNames_.end());
        bool isInFunctionChunks = (functionChunks_.find(node.name) != functionChunks_.end());
        int slot = (isClassName || isInFunctionChunks) ? -1 : lookupGlobalSlot(node.name);
        if (slot >= 0) {
            if (node.arguments.size() > 255) {
                error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
                return;
            }
            chunk_.writeOp(OpCode::OP_GET_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
            for (auto& arg : node.arguments) {
                compileNode(arg.get());
            }
            chunk_.writeOp(OpCode::OP_CALL_EXPR, node.line);
            chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
            return;
        }
    }

    // C-P1-1 fix: 参数数量检查移到编译参数之前
    if (node.arguments.size() > 255) {
        error("函数调用参数数量超过限制（最大 255 个）", node.line, 0);
        return;
    }

    // 编译参数
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }

    // 函数名作为常量
    uint16_t nameIdx = identifierIndex(node.name);

    // 使用 OP_CALL 指令
    chunk_.writeOp(OpCode::OP_CALL, node.line);
    chunk_.write(static_cast<uint8_t>(nameIdx & 0xFF), node.line);
    chunk_.write(static_cast<uint8_t>((nameIdx >> 8) & 0xFF), node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    return;
}

void Compiler::visitReturnStmt(ReturnStmt& node) {
    if (!inFunction_) {
        error("return 只能在函数体内使用", node.line, 0);
        return;
    }
    // R109 TCO: 尾调用优化。识别 return f(args) 形态的自递归调用，
    // 编译为"参数求值 + 逆序 OP_SET_LOCAL 覆盖参数槽 + OP_JUMP 函数入口"。
    // 跳过 OP_RETURN 的帧弹出，复用当前帧执行下一轮递归，深度无界。
    //
    // 优化条件（保守策略，与 IR 路径严格对齐）：
    //   (1) AST 结构匹配（TCO::isTailRecursiveReturn：return f(args) 且 f == 当前函数名）
    //   (2) 不在 try 块内（tryDepth_ == 0，否则 tryStack_ handler 不清理 + finally/catch 语义破坏）
    //   (3) 函数无闭包 upvalue（currentUpvalues_.empty()，否则跳转后 upvalue 指向被覆盖的栈槽）
    //   (4) 参数数量等于形参数量（无默认参数填充，避免参数槽位错位）
    //   (5) 不是类方法（currentClassName_.empty()，方法 slot 0 是 this 不能被覆盖）
    //   (6) currentFunctionDecl_ 非空（用于获取形参数量）
    //
    // 参数求值顺序：先全部求值到栈顶，再逆序 OP_SET_LOCAL + OP_POP 覆盖参数槽。
    // 逆序保证：args[i] 可能引用 params[j]（j<i），若顺序赋值会破坏 params[j]。
    // 栈布局变化：[..., val_0, val_1, ..., val_N-1] → [...] → OP_JUMP（栈深度恢复）
    if (!currentFunctionName_.empty() && tryDepth_ == 0 && currentUpvalues_.empty() && currentClassName_.empty() &&
        currentFunctionDecl_ != nullptr && TCO::isTailRecursiveReturn(&node, currentFunctionName_)) {
        auto* call = static_cast<FunCall*>(node.value.get());
        if (call->arguments.size() == currentFunctionDecl_->params.size()) {
            // 编译所有参数表达式，结果压栈
            for (auto& arg : call->arguments) {
                compileNode(arg.get());
            }
            // 逆序 OP_SET_LOCAL + OP_POP 覆盖参数槽位
            for (int i = static_cast<int>(call->arguments.size()) - 1; i >= 0; --i) {
                chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(i), node.line);
                chunk_.writeOp(OpCode::OP_POP, node.line);
            }
            // 跳转到函数入口（OP_JUMP 是绝对跳转）
            chunk_.writeOp(OpCode::OP_JUMP, node.line);
            chunk_.writeShort(static_cast<uint16_t>(currentFunctionEntryIp_), node.line);
            return;
        }
    }
    if (node.value) {
        compileNode(node.value.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    // BUG-TYPE-1 fix (P1): 函数有返回类型注解时，在 OP_RETURN 前发射 OP_TYPE_CHECK
    // 检查返回值类型兼容性。对齐 Interpreter::visitReturnStmt 的 checkType 逻辑。
    // 原实现仅 Interpreter 检查返回类型，StackVM/RegisterVM 静默通过，导致类型安全绕过。
    // R109 TCO: TCO 路径跳过 TYPE_CHECK 是安全的——递归调用的 return 会再次触发检查。
    if (!currentFunctionReturnType_.empty()) {
        emitTypeCheck(currentFunctionReturnType_, node.line);
    }
    chunk_.writeOp(OpCode::OP_RETURN, node.line);
    return;
}

void Compiler::visitBreakStmt(BreakStmt& node) {
    if (loopStack_.empty()) {
        error("break 只能在循环体内使用", node.line, 0);
        return;
    }
    // C-P0-2 fix: 只弹出循环内部的 try handler（与 continue 一致），
    // 之前使用 tryDepth_（全局深度）会错误弹出入层函数/外层循环的 try 帧
    int tryDepthInLoop = tryDepth_ - loopStack_.back().tryDepthAtStart;
    for (int i = 0; i < tryDepthInLoop; ++i) {
        chunk_.writeOp(OpCode::OP_TRY_END, node.line);
    }
    // AUDIT-P2-CORRECT fix: break 跳出循环时需关闭循环体内声明的闭包捕获变量的
    // upvalue，对齐正常迭代退出时的 OP_CLOSE_UPVALUE 发射。OP_CLOSE_UPVALUE
    // bodySlotBase 关闭 slot >= bodySlotBase 的全部 open upvalues（含嵌套块变量）。
    // 原 visitBreakStmt 直接发 OP_JUMP 跳到 breakTarget（在 OP_CLOSE_UPVALUE 之后），
    // 跳过 upvalue 关闭，导致闭包捕获变 by-reference（三后端不一致）。
    const LoopContext& loopCtx = loopStack_.back();
    if (loopCtx.needCloseUpvalue && loopCtx.bodySlotBase <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(loopCtx.bodySlotBase), node.line);
    }
    // AUDIT-P1.1 fix: 若 break 在 try-finally 内，发射续跳字节码（先 push 真实目标，再 jump 到 finally 入口）。
    // finally 末尾的 OP_FINALLY_END 从 pendingJumpStack_ 取出真实目标续跳。
    // 若不在 try-finally 内，走常规路径（直接 OP_JUMP 到 breakTarget）。
    std::vector<size_t> realTargetPatches;
    if (emitFinallyJump(node.line, realTargetPatches)) {
        // 续跳路径：realTargetPatches 中的 patch 待回填到 breakTarget
        for (size_t patch : realTargetPatches) {
            loopStack_.back().breakJumps.push_back(patch);
        }
    } else {
        // 常规路径：发射 OP_JUMP，目标在循环编译完成后回填
        size_t patch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);
        loopStack_.back().breakJumps.push_back(patch);
    }
    return;
}

void Compiler::visitContinueStmt(ContinueStmt& node) {
    if (loopStack_.empty()) {
        error("continue 只能在循环体内使用", node.line, 0);
        return;
    }
    // BUG1 fix: 只弹出循环内部的 try handler
    int tryDepthInLoop = tryDepth_ - loopStack_.back().tryDepthAtStart;
    for (int i = 0; i < tryDepthInLoop; ++i) {
        chunk_.writeOp(OpCode::OP_TRY_END, node.line);
    }
    // AUDIT-P1.1 fix: 若 continue 在 try-finally 内，发射续跳字节码（同 visitBreakStmt）。
    std::vector<size_t> realTargetPatches;
    if (emitFinallyJump(node.line, realTargetPatches)) {
        for (size_t patch : realTargetPatches) {
            loopStack_.back().continueJumps.push_back(patch);
        }
    } else {
        // 常规路径：发射 OP_JUMP，目标在循环编译完成后回填
        size_t patch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);
        loopStack_.back().continueJumps.push_back(patch);
    }
    return;
}

void Compiler::visitThrowStmt(ThrowStmt& node) {
    // 编译抛出表达式，将值推入栈顶
    if (node.expression) {
        compileNode(node.expression.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    // OP_THROW 弹出栈顶值并触发异常传播
    chunk_.writeOp(OpCode::OP_THROW, node.line);
    return;
}

void Compiler::visitImportStmt(ImportStmt& node) {
    // VM-IMPORT: 编译期模块内联——加载模块源码、解析 AST、预扫描全局槽位、
    // 内联编译模块语句。模块代码在编译期被"展开"到主程序中，VM 运行时无需模块加载机制。
    //
    // BUG-AUDIT-MOD-2 fix: 模块隔离（AST 重写 + 作用域分析）
    // 在内联编译前，对模块 AST 调用 ModuleTopLevelRenamer::rename，将模块的
    // 非导出顶层声明名前缀化为 `__mod_<hash>__<name>`，并递归重写模块内对这些
    // 名字的引用。导入方无法用原名访问模块的非导出名，与 Interpreter 的模块
    // 隔离语义（独立 Environment）对齐。导出名保持原名，导入方正常访问。
    // - run-once: 同一模块多次 import 时仅编译/执行一次（linkedModuleSet_ 保证）
    // - 安全保障: export 标记检查（BUG-AUDIT-MOD-1）+ 深度限制（BUG-AUDIT-MOD-3）
    //   + AST 隔离（BUG-AUDIT-MOD-2，本处实施）

    // 1. 路径规范化与安全校验（SEC-1: 路径遍历防护）
    std::string modulePath = normalizeModulePath(node.modulePath);
    if (modulePath.empty()) {
        error("模块路径无效: " + node.modulePath, node.line, 0);
        return;
    }

    // 2. run-once 检查：已编译的模块跳过（全局槽位已定义）
    if (linkedModuleSet_.count(modulePath)) {
        // BUG-AUDIT-MOD-1 fix: 已编译模块的具名导入验证也检查 export 集合（对齐 Interpreter）
        if (!node.importAll && !node.names.empty()) {
            auto expIt = moduleExports_.find(modulePath);
            if (expIt != moduleExports_.end()) {
                for (const auto& name : node.names) {
                    if (expIt->second.find(name) == expIt->second.end()) {
                        error("模块 " + modulePath + " 中未导出名称: " + name, node.line, 0);
                        return;
                    }
                }
            }
        }
        return;
    }

    // 3. 循环依赖检测
    if (moduleLoadingSet_.count(modulePath)) {
        error("检测到循环依赖: " + modulePath, node.line, 0);
        return;
    }

    // BUG-AUDIT-MOD-3 fix: 模块加载深度保护（对齐 InterpreterModules.cpp:76-78）
    // Interpreter 有 moduleLoadingStack_.size() >= MAX_RECURSION_DEPTH 检查，
    // VM/IR 路径原缺失此检查，深嵌套导入链可能 C++ 栈溢出崩溃。
    if (moduleLoadingStack_.size() >= RuntimeLimits::MAX_RECURSION_DEPTH) {
        error("模块导入深度超过限制 (" + std::to_string(RuntimeLimits::MAX_RECURSION_DEPTH) + ")", node.line, 0);
        return;
    }

    // 4. 检查模块加载器
    if (!moduleLoader_) {
        error("VM 编译需要模块加载器（moduleLoader 未设置），请通过文件路径运行", node.line, 0);
        return;
    }

    // 5. 标记为正在加载（循环检测 + 深度保护）
    moduleLoadingSet_.insert(modulePath);
    moduleLoadingStack_.push_back(modulePath);

    // 6. 加载并解析模块
    auto moduleAst = loadAndParseModule(modulePath, node.line);
    if (!moduleAst) {
        moduleLoadingSet_.erase(modulePath);
        moduleLoadingStack_.pop_back(); // BUG-AUDIT-MOD-3
        return;                         // loadAndParseModule 已调用 error()
    }

    // 6.5 BUG-AUDIT-MOD-2 fix: 模块隔离——重命名非导出顶层名为 `__mod_<hash>__<name>`
    // 在预扫描前重写 AST，确保重命名后的名字进入全局槽位分配与 export 集合
    ModuleTopLevelRenamer::rename(*moduleAst, modulePath);

    // 7. 预扫描模块顶层声明，分配全局槽位
    preScanModuleGlobals(*moduleAst);

    // 7.5 BUG-AUDIT-MOD-1 fix: 收集模块导出名称（对齐 InterpreterModules.cpp:172-188）
    // 仅 ExportStmt 包装的声明名计入导出集合，普通顶层声明不算导出
    collectModuleExports(modulePath, *moduleAst);

    // 8. 内联编译模块语句（递归处理模块自身的 import）
    for (auto& stmt : moduleAst->statements) {
        if (stmt)
            compileStatement(stmt.get());
    }

    // 9. 保留模块 AST（函数/类定义指针在字节码中以常量池索引引用，AST 必须存活）
    moduleAsts_.push_back(std::move(moduleAst));

    // 10. 从加载集移除，标记为已链接
    moduleLoadingSet_.erase(modulePath);
    moduleLoadingStack_.pop_back(); // BUG-AUDIT-MOD-3
    linkedModuleSet_.insert(modulePath);

    // 11. BUG-AUDIT-MOD-1 fix: 具名导入验证改为检查 export 集合（对齐 Interpreter）
    // 原实现仅检查 lookupGlobalSlot(name) < 0（名称存在即通过），
    // 导致非导出名称可被导入，违反模块封装语义。
    if (!node.importAll && !node.names.empty()) {
        auto expIt = moduleExports_.find(modulePath);
        if (expIt != moduleExports_.end()) {
            for (const auto& name : node.names) {
                if (expIt->second.find(name) == expIt->second.end()) {
                    // BUG-AUDIT-MOD-6 fix: 错误消息对齐 Interpreter（"模块 X 中未导出名称: Y"）
                    error("模块 " + modulePath + " 中未导出名称: " + name, node.line, 0);
                    return;
                }
            }
        }
    }
}

void Compiler::visitExportStmt(ExportStmt& node) {
    // VM-IMPORT: export 在 VM 中等价于普通声明编译（导出语义在编译期内联时自然满足——
    // 模块的所有顶层声明对导入方可见）。记录导出名供 import 验证使用。
    if (node.declaration) {
        compileNode(node.declaration.get());
    }
    return;
}

// ============================================================
// VM-IMPORT: 模块系统辅助方法
// ============================================================

std::string Compiler::normalizeModulePath(const std::string& rawPath) const {
    // 对齐 InterpreterModules.cpp:20-53 的路径规范化与 SEC-1 安全校验
    std::string path = rawPath;
    // 统一路径分隔符为 '/'
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    // 去除 "./" 前缀
    if (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
        path.erase(0, 2);
    }
    // 空路径
    if (path.empty())
        return "";
    // 绝对路径检测（Unix '/' 或 Windows 驱动器路径 'X:...'）
    // BUG-MOD-1 fix: 原实现仅检测 'C:/' 形式，未拒绝 'C:foo'（Windows 驱动器相对路径），
    // 可能被 moduleLoader_ 解析到模块目录外的文件。修复：拒绝所有 'X:' 开头形式
    // （X 为任意字符），覆盖 'C:/'、'C:foo'、'D:path' 等。
    // 注：反斜杠已在上方统一转为正斜杠，无需再检测 '\\'。
    if (path[0] == '/' || (path.size() >= 2 && path[1] == ':')) {
        return "";
    }
    // ".." 路径段检测
    size_t pos = 0;
    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        std::string segment = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        if (segment == "..")
            return "";
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return path;
}

std::unique_ptr<Block> Compiler::loadAndParseModule(const std::string& modulePath, int line) {
    // 调用模块加载器获取源码
    std::string source = moduleLoader_(modulePath);
    if (source.empty()) {
        error("无法加载模块: " + modulePath + "（文件不存在或为空）", line, 0);
        return nullptr;
    }
    // 词法分析
    Lexer lexer;
    auto tokens = lexer.scan(source);
    // BUG-FIX: 检查词法错误并转发到编译诊断（原实现吞掉 Lexer 错误）
    if (lexer.getDiagnostics().hasErrors()) {
        const auto& diags = lexer.getDiagnostics().all();
        if (!diags.empty()) {
            const auto& d = diags.front();
            error("模块 '" + modulePath + "' 词法错误: " + d.message, d.line, d.column);
        } else {
            error("模块 '" + modulePath + "' 词法错误", line, 0);
        }
        return nullptr;
    }
    // 语法分析
    Parser parser;
    auto moduleAst = parser.parse(tokens);
    // BUG-FIX: 检查语法错误并转发到编译诊断（原实现仅检查 AST 是否为空，
    // 可恢复错误的 AST 会被当作成功加载）
    if (parser.hasErrors()) {
        const auto& diags = parser.getDiagnostics().all();
        if (!diags.empty()) {
            const auto& d = diags.front();
            error("模块 '" + modulePath + "' 语法错误: " + d.message, d.line, d.column);
        } else {
            error("模块 '" + modulePath + "' 语法错误", line, 0);
        }
        return nullptr;
    }
    if (!moduleAst) {
        error("模块解析失败: " + modulePath, line, 0);
        return nullptr;
    }
    return moduleAst;
}

void Compiler::preScanModuleGlobals(Block& moduleAst) {
    // 预扫描模块顶层声明，分配全局槽位。
    // BUG-PRES-1 fix: 覆盖 ExportStmt（解包 VarDecl/ClassDecl/FunDecl 内部声明）。
    //
    // R98 W2 fix: 原"与 compile() 的 pre-scan 存在有意的不对称"已消除——
    // compile() 现在也预扫描 FunDecl（visitFunDecl 顶层路径已用 OP_DEFINE_GLOBAL
    // 写入 globalSlots_[slot]，使函数名可作为值引用，详见 compile() 的 pre-scan 注释）。
    // 两个 pre-scan 现在行为一致，均预扫描 VarDecl/ClassDecl/FunDecl。
    for (auto& stmt : moduleAst.statements) {
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
}

void Compiler::collectModuleExports(const std::string& modulePath, Block& moduleAst) {
    // visitImportStmt 子阶段：收集模块导出名称集合
    // 仅 ExportStmt 包装的声明名（VarDecl/ClassDecl/FunDecl）计入导出集合，
    // 普通顶层声明不算导出。对齐 InterpreterModules.cpp:172-188 的语义。
    // 输出：moduleExports_[modulePath] = exports 集合
    std::unordered_set<std::string> exports;
    for (auto& stmt : moduleAst.statements) {
        if (!stmt || stmt->nodeType != NodeType::NODE_EXPORT_STMT)
            continue;
        auto* exp = static_cast<ExportStmt*>(stmt.get());
        if (!exp->declaration)
            continue;
        ASTNode* decl = exp->declaration.get();
        switch (decl->nodeType) {
        case NodeType::NODE_VAR_DECL:
            exports.insert(static_cast<VarDecl*>(decl)->name);
            break;
        case NodeType::NODE_CLASS_DECL:
            exports.insert(static_cast<ClassDecl*>(decl)->name);
            break;
        case NodeType::NODE_FUN_DECL:
            exports.insert(static_cast<FunDecl*>(decl)->name);
            break;
        default:
            break;
        }
    }
    moduleExports_[modulePath] = std::move(exports);
}

void Compiler::visitTryStmt(TryStmt& node) {
    // 编译模式:
    //   OP_TRY_BEGIN <catchOffset>
    //   <try block>
    //   OP_TRY_END
    //   OP_JUMP <afterCatch>
    // catchIp:
    //   <bind exception to catchVar>
    //   <catch block>
    // afterCatch:
    //
    // BUG-AUDIT-FINALLY-1: 如果有 finally 块，外层再包一个 OP_TRY_BEGIN/END：
    //   OP_TRY_BEGIN <finallyCatchOffset>    ← 外层 try（捕获异常路径）
    //     <内层 try-catch>
    //   OP_TRY_END                            ← 弹出外层 handler
    //   <finally block>                       ← 正常路径执行 finally
    //   OP_JUMP <afterFinally>
    // finallyCatchIp:                         ← 异常路径
    //   <finally block>（重复一次）
    //   OP_THROW                              ← re-throw（异常值在栈顶）
    // afterFinally:
    //
    // 已知限制：break/continue 时 finally 块三后端不一致——Interpreter 会执行
    // finally（break/continue 走 loopFlow_ 状态标志，try 块正常完成后继续执行
    // finally），StackVM/RegisterVM 跳过 finally（break/continue 发射 OP_TRY_END
    // 弹出 handler 后直接跳转到循环目标）。return 三后端均跳过 finally（一致）。
    // 异常值在异常路径的 finally 执行期间保留在栈顶（Block 是栈平衡的），
    // OP_THROW pop 并 re-throw。若 finally 自身 throw，throwException 会截断
    // 栈到外层 handler 的 stackBase（丢弃原异常值），新异常正常传播。
    //
    // AUDIT-P1.1 fix: 上述"已知限制"已修复。break/continue 在 try-finally 内时，
    // 先 push 真实跳转目标到 pendingJumpStack_，再 jump 到 finally 入口。
    // finally 末尾的 OP_FINALLY_END 从栈取出目标续跳，实现三后端一致性。
    //
    // 拆分说明（原 357 行单函数 → orchestrator + 3 子阶段）：
    //   - emitTryBlock：OP_TRY_BEGIN + try body + OP_TRY_END + skip-catch OP_JUMP
    //   - emitCatchBlock：catchOffset 回填 + catch 变量绑定 + catch body（含 cleanup wrap）
    //                     + cleanup + OP_CLOSE_UPVALUE + 恢复 currentLocals_ + 回填 afterCatch
    //   - emitFinallyBlock：OP_TRY_END + finallyEntry 回填 + finally body + OP_FINALLY_END
    //                       + 异常路径 + 回填 afterFinally
    // 控制流不变量：emitCatchBlock 返回 false 时直接 return（不 pop tryFinallyStack_，
    // 对齐原实现 catch 块 early return 行为）；emitFinallyBlock 早返回时不 pop
    // tryFinallyStack_，由本函数统一 pop（单次 pop 语义）。

    // AUDIT-P1.1 fix: push try-finally 编译期上下文
    tryFinallyStack_.push_back({node.finallyBlock != nullptr, 0, {}, {}});

    // 0. 如果有 finally，发射外层 OP_TRY_BEGIN
    size_t outerTryBeginIp = 0;
    size_t finallyCatchOffsetPatch = std::string::npos;
    if (node.finallyBlock) {
        outerTryBeginIp = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_TRY_BEGIN, node.line);
        finallyCatchOffsetPatch = chunk_.code.size();
        chunk_.writeShort(0, node.line); // 占位
        ++tryDepth_;                     // 外层 try 计入深度，使 break/continue 发射对应 OP_TRY_END
    }

    // BUG-AUDIT-FINALLY-1: try-finally（无 catch）路径。
    // catchVarName 为空表示无 catch 子句，异常不应被捕获。
    // 外层 OP_TRY_BEGIN（finallyCatchOffset）会捕获异常 → 执行 finally → rethrow。
    // 跳过内层 try-catch 的全部字节码（OP_TRY_BEGIN/catchOffset/catch 变量绑定/cleanup）。
    if (!node.catchVarName.empty()) {
        TryCatchPatchInfo patchInfo;
        emitTryBlock(node, patchInfo);
        if (!emitCatchBlock(node, patchInfo)) {
            // 保留原始控制流：原 catch 块 early return 不 pop tryFinallyStack_。
            // tryFinallyStack_ 在此路径下不 pop（与原实现一致）。
            return;
        }
    } else {
        // try-finally（无 catch）：只编译 try 块，不发射内层 try-catch。
        // 外层 OP_TRY_BEGIN（finallyCatchOffset）会捕获异常 → 执行 finally → rethrow。
        if (node.tryBlock) {
            compileNode(node.tryBlock.get());
        }
    }

    // 9. BUG-AUDIT-FINALLY-1: finally 块字节码
    if (node.finallyBlock) {
        emitFinallyBlock(node, outerTryBeginIp, finallyCatchOffsetPatch);
    }

    // AUDIT-P1.1 fix: pop try-finally 编译期上下文
    tryFinallyStack_.pop_back();

    return;
}

// ============================================================
// visitTryStmt 子阶段实现
// ============================================================

void Compiler::emitTryBlock(TryStmt& node, TryCatchPatchInfo& info) {
    // 1. 发射 OP_TRY_BEGIN（catchOffset 占位，稍后由 emitCatchBlock 回填）
    info.tryBeginIp = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_TRY_BEGIN, node.line);
    info.catchOffsetPatch = chunk_.code.size();
    chunk_.writeShort(0, node.line); // 占位

    // 2. 编译 try 块
    // P0-4 fix: 增加 tryDepth_，使 break/continue 能发射 OP_TRY_END
    ++tryDepth_;
    if (node.tryBlock) {
        compileNode(node.tryBlock.get());
    }
    --tryDepth_;

    // 3. try 块正常结束：弹出 try 处理器，跳过 catch 块
    chunk_.writeOp(OpCode::OP_TRY_END, node.line);
    info.skipCatchJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, node.line);
    chunk_.writeShort(0, node.line); // 占位，由 emitCatchBlock 回填为 afterCatch
}

bool Compiler::emitCatchBlock(TryStmt& node, const TryCatchPatchInfo& info) {
    // 4. 回填 catchOffset
    size_t catchIp = chunk_.code.size();
    size_t catchOffset = catchIp - (info.tryBeginIp + 3);
    // P1-3 fix: 检查 catchOffset 是否溢出 uint16_t
    if (catchOffset > 65535) {
        error("try 块过大，catch 偏移溢出 65535", node.line, 0);
        return false;
    }
    chunk_.code[info.catchOffsetPatch] = static_cast<uint8_t>(catchOffset & 0xFF);
    chunk_.code[info.catchOffsetPatch + 1] = static_cast<uint8_t>((catchOffset >> 8) & 0xFF);

    // 5. 在 catchIp 处：异常值已在栈顶，绑定到 catch 变量
    // BUG 7a/7b/7c fix: catch 变量应 shadow 外层同名变量，不覆盖其值；
    // 顶层 catch 变量在 catch 块结束后清理，不泄漏到外层作用域
    auto savedCatchLocals = currentLocals_;
    bool needCatchVarCleanup = false;
    bool hasShadowedGlobal = false;
    int shadowedGlobalSlot = -1;
    std::string shadowedSaveName;
    // BUG-AUDIT-EXC-CATCH-CLOSE fix: 记录 catch 变量 slot，catch 块退出时
    // 发射 OP_CLOSE_UPVALUE 关闭指向该 slot 的 open upvalue，防止 slot 复用后
    // 闭包读取错误值（对齐 IR 路径 leaveBlockScope 和 Interpreter closeCapturedVariables）。
    int catchVarSlot = -1;

    if (inFunction_) {
        // 函数内：始终分配新局部变量槽位（shadow 外层同名变量，不覆盖其值）
        int slot = static_cast<int>(currentLocals_.size());
        if (slot > 255) {
            error("函数局部变量数量超过限制", node.line, 0);
            return false;
        }
        currentLocals_[node.catchVarName] = slot;
        peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
        catchVarSlot = slot;
        // BUG-IDE-12 fix: 记录 catch 变量 slot→name
        if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
            localSlotNames_.resize(slot + 1);
        }
        localSlotNames_[slot] = node.catchVarName;
        // L1 fix: 记录 catch 变量到 slotNameRanges_，endIp 暂设为 0（由 closeSlotRanges
        // 在 catch 块退出时回填）。catch 变量的 slot 在 catch 块退出后可能被复用，
        // 需按 IP 范围精确反查避免变量名错位。
        slotNameRanges_.push_back({static_cast<uint8_t>(slot), node.catchVarName, chunk_.code.size(), 0});
        chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
        chunk_.write(static_cast<uint8_t>(slot), node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // 顶层：使用块作用域变量（OP_DEFINE_VAR），不覆盖已有全局变量
        shadowedGlobalSlot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(node.catchVarName);
        if (shadowedGlobalSlot >= 0) {
            // 保存被遮蔽的全局值到临时变量
            hasShadowedGlobal = true;
            shadowedSaveName = "__catch_save_" + std::to_string(blockSaveCounter_++) + "_" + node.catchVarName;
            uint16_t saveIdx = identifierIndex(shadowedSaveName);
            chunk_.writeOp(OpCode::OP_GET_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(shadowedGlobalSlot), node.line);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
            chunk_.writeShort(saveIdx, node.line);
            globalSlotAllocator_.removeMapping(node.catchVarName); // B4: 临时遮蔽
        }
        // 定义 catch 变量
        uint16_t nameIdx = identifierIndex(node.catchVarName);
        chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
        needCatchVarCleanup = true;
    }

    // 6. 编译 catch 块
    // BUG-TRY-1 fix: 若 catch 块内 throw，原实现跳过清理代码，导致 catch 变量泄漏、
    // 被遮蔽的全局值未恢复。修复：用 OP_TRY_BEGIN 包装 catch 块，捕获内层 throw，
    // 跳到 cleanupThrowIp 执行清理代码后 OP_THROW rethrow。
    // 字节码布局：
    //   catchIp: <bind exception>
    //     OP_TRY_BEGIN <cleanupThrowOffset>
    //     <catch block>
    //     OP_TRY_END
    //     <cleanup code>           ← 正常路径
    //     OP_JUMP <afterCatch>
    //   cleanupThrowIp:
    //     <cleanup code>           ← 异常路径（复制）
    //     OP_THROW                 ← rethrow（异常值已在栈顶）
    //   afterCatch:
    //
    // cleanup 字节码栈平衡为 0（OP_DELETE_VAR 不影响栈；OP_GET_VAR+OP_SET_GLOBAL+OP_DELETE_VAR = 0），
    // 异常值保持在栈顶，OP_THROW 可正确 rethrow。
    bool needsCleanupWrap = needCatchVarCleanup || hasShadowedGlobal;
    size_t innerTryBeginIp = 0;
    size_t innerCatchOffsetPatch = std::string::npos;
    if (needsCleanupWrap) {
        innerTryBeginIp = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_TRY_BEGIN, node.line);
        innerCatchOffsetPatch = chunk_.code.size();
        chunk_.writeShort(0, node.line); // 占位，稍后回填为 cleanupThrowOffset
        // BUG-AUDIT-EXC-CLEANUP-TRYDEPTH fix: cleanup wrap 的内层 OP_TRY_BEGIN
        // 必须计入 tryDepth_，使 catch 块内的 break/continue 能发射对应的 OP_TRY_END，
        // 避免 tryStack_ handler 残留导致后续异常被错误捕获到已失效的 cleanupThrowIp。
        ++tryDepth_;
    }
    if (node.catchBlock) {
        compileNode(node.catchBlock.get());
    }
    if (needsCleanupWrap) {
        // BUG-AUDIT-EXC-CLEANUP-TRYDEPTH fix: 对应的 --tryDepth_
        --tryDepth_;
        chunk_.writeOp(OpCode::OP_TRY_END, node.line);
    }

    // 7. 清理顶层 catch 变量并恢复被遮蔽的全局值（正常路径）
    // cleanup 字节码发射逻辑提取为 lambda，正常路径和异常路径各调用一次
    auto emitCleanupBytecode = [&]() {
        if (needCatchVarCleanup) {
            uint16_t nameIdx = identifierIndex(node.catchVarName);
            chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
        if (hasShadowedGlobal) {
            uint16_t saveIdx = identifierIndex(shadowedSaveName);
            chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
            chunk_.writeShort(saveIdx, node.line);
            chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(shadowedGlobalSlot), node.line);
            chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
            chunk_.writeShort(saveIdx, node.line);
        }
    };
    emitCleanupBytecode();

    size_t skipCleanupThrowJumpPatch = std::string::npos;
    if (needsCleanupWrap) {
        // 正常路径：跳过 cleanupThrow 块
        skipCleanupThrowJumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line); // 占位，稍后回填为 afterCatch

        // 异常路径：cleanupThrowIp
        size_t cleanupThrowIp = chunk_.code.size();
        size_t cleanupThrowOffset = cleanupThrowIp - (innerTryBeginIp + 3);
        if (cleanupThrowOffset > 65535) {
            error("catch 块过大，cleanupThrow 偏移溢出 65535", node.line, 0);
            // BUG-TRY-LEAK-1 fix: early return 前必须恢复编译期状态，否则
            // globalSlotAllocator_ 状态不一致 + currentLocals_ 泄漏 catch 变量。
            // 对齐 L1840-L1845 的正常路径恢复逻辑。
            if (hasShadowedGlobal) {
                globalSlotAllocator_.restoreMapping(node.catchVarName, shadowedGlobalSlot);
            }
            currentLocals_ = std::move(savedCatchLocals);
            return false;
        }
        chunk_.code[innerCatchOffsetPatch] = static_cast<uint8_t>(cleanupThrowOffset & 0xFF);
        chunk_.code[innerCatchOffsetPatch + 1] = static_cast<uint8_t>((cleanupThrowOffset >> 8) & 0xFF);

        // 异常路径：发射 cleanup 字节码 + OP_THROW rethrow
        // 此时异常值在栈顶，cleanup 字节码栈平衡为 0，异常值保持栈顶
        emitCleanupBytecode();
        chunk_.writeOp(OpCode::OP_THROW, node.line);
    }

    // restoreMapping 是编译期操作（修改 slots_ map），不影响运行时字节码，只调用一次
    if (hasShadowedGlobal) {
        globalSlotAllocator_.restoreMapping(node.catchVarName, shadowedGlobalSlot); // B4: 恢复遮蔽
    }

    // BUG-AUDIT-EXC-CATCH-CLOSE fix: 函数内 catch 变量 slot 在恢复 currentLocals_ 前
    // 必须发射 OP_CLOSE_UPVALUE 关闭指向该 slot 的 open upvalue。否则后续代码声明新
    // 局部变量会复用该 slot 覆盖原值，逃逸的闭包通过 upvalue 读取到错误值（等价悬垂引用）。
    // 对齐 IR 路径 leaveBlockScope（IR.cpp:439-441）和 Interpreter CatchEnvGuard 析构
    // 调用 closeCapturedVariables 的语义。顶层 catch 变量用 OP_DELETE_VAR 清理，无需此处理。
    if (inFunction_ && catchVarSlot >= 0 && catchVarSlot <= 255) {
        chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
        chunk_.write(static_cast<uint8_t>(catchVarSlot), node.line);
    }

    // L1 fix: 回填 catch 变量及 catch 块内声明的变量 range 的 endIp。
    // savedCatchLocals.size() 是 catch 作用域的 slot 基址，关闭 slot >= 该基址
    // 的全部 open ranges。closeSlotRanges 必须在 currentLocals_ 恢复前调用，
    // 否则 endIp 会被错误地保留为 0（未关闭）。
    closeSlotRanges(savedCatchLocals.size());

    // 恢复 currentLocals_，使 catch 变量不泄漏到外层作用域
    currentLocals_ = std::move(savedCatchLocals);

    // 8. 回填跳过 catch 块的跳转目标（OP_JUMP 使用绝对地址）
    size_t afterCatch = chunk_.code.size();
    // P1-3 fix: 检查 afterCatch 是否溢出 uint16_t
    if (afterCatch > 65535) {
        error("代码量过大，跳转目标溢出 65535", node.line, 0);
        return false;
    }
    uint16_t afterCatchTarget = static_cast<uint16_t>(afterCatch);
    chunk_.code[info.skipCatchJumpPatch + 1] = static_cast<uint8_t>(afterCatchTarget & 0xFF);
    chunk_.code[info.skipCatchJumpPatch + 2] = static_cast<uint8_t>((afterCatchTarget >> 8) & 0xFF);
    if (skipCleanupThrowJumpPatch != std::string::npos) {
        chunk_.code[skipCleanupThrowJumpPatch + 1] = static_cast<uint8_t>(afterCatchTarget & 0xFF);
        chunk_.code[skipCleanupThrowJumpPatch + 2] = static_cast<uint8_t>((afterCatchTarget >> 8) & 0xFF);
    }

    return true;
}

void Compiler::emitFinallyBlock(TryStmt& node, size_t outerTryBeginIp, size_t finallyCatchOffsetPatch) {
    --tryDepth_;
    chunk_.writeOp(OpCode::OP_TRY_END, node.line); // 弹出外层 try 处理器

    // AUDIT-P1.1 fix: 记录 finally 入口地址，回填 break/continue 的续跳 patches。
    // finallyEntryIp 是正常路径 finally 块的入口（OP_TRY_END 之后的第一条指令）。
    size_t finallyEntryIp = chunk_.code.size();
    tryFinallyStack_.back().finallyEntryIp = finallyEntryIp;
    // 回填所有 pendingJumpPatches（break/continue 的 OP_JUMP 目标 = finallyEntryIp）
    for (size_t patch : tryFinallyStack_.back().pendingJumpPatches) {
        if (finallyEntryIp > 65535) {
            error("finally 块入口偏移溢出 65535", node.line, 0);
            // 不 pop tryFinallyStack_，由 visitTryStmt 统一 pop（单次 pop 语义）
            return;
        }
        uint16_t target = static_cast<uint16_t>(finallyEntryIp);
        chunk_.code[patch] = static_cast<uint8_t>(target & 0xFF);
        chunk_.code[patch + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }
    // 回填所有 pendingTargetPatches（内层 break/continue 的 OP_PUSH_JUMP_TARGET 目标 = finallyEntryIp）
    for (size_t patch : tryFinallyStack_.back().pendingTargetPatches) {
        if (finallyEntryIp > 65535) {
            error("finally 块入口偏移溢出 65535", node.line, 0);
            // 不 pop tryFinallyStack_，由 visitTryStmt 统一 pop（单次 pop 语义）
            return;
        }
        uint16_t target = static_cast<uint16_t>(finallyEntryIp);
        chunk_.code[patch] = static_cast<uint8_t>(target & 0xFF);
        chunk_.code[patch + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }

    // 正常路径：执行 finally
    compileNode(node.finallyBlock.get());
    // AUDIT-P1.1 fix: finally 块末尾追加 OP_FINALLY_END。
    // 若 pendingJumpStack_ 非空（break/continue 触发），pop 目标并跳转；
    // 否则继续执行（正常完成）。异常路径的 finally 末尾保持 OP_THROW（re-throw）。
    chunk_.writeOp(OpCode::OP_FINALLY_END, node.line);

    // 跳过异常路径
    size_t skipFinallyExceptionJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, node.line);
    chunk_.writeShort(0, node.line); // 占位

    // 异常路径：finallyCatchIp
    size_t finallyCatchIp = chunk_.code.size();
    size_t finallyCatchOffset = finallyCatchIp - (outerTryBeginIp + 3);
    if (finallyCatchOffset > 65535) {
        error("try-finally 块过大，finallyCatch 偏移溢出 65535", node.line, 0);
        // 不 pop tryFinallyStack_，由 visitTryStmt 统一 pop（单次 pop 语义）
        return;
    }
    chunk_.code[finallyCatchOffsetPatch] = static_cast<uint8_t>(finallyCatchOffset & 0xFF);
    chunk_.code[finallyCatchOffsetPatch + 1] = static_cast<uint8_t>((finallyCatchOffset >> 8) & 0xFF);

    // 执行 finally（异常路径，重复一次）
    // 异常值已在栈顶（throwException push），finally 块作为 Block 是栈平衡的，
    // 执行后异常值仍在栈顶，OP_THROW 会 pop 并 re-throw。
    // 异常路径末尾不追加 OP_FINALLY_END——异常路径的 finally 是 throwException 触发的，
    // 执行完毕后应 re-throw（OP_THROW），而非续跳（break/continue 已被异常中断）。
    compileNode(node.finallyBlock.get());
    chunk_.writeOp(OpCode::OP_THROW, node.line);

    // afterFinally
    size_t afterFinally = chunk_.code.size();
    if (afterFinally > 65535) {
        error("代码量过大，跳转目标溢出 65535", node.line, 0);
        // 不 pop tryFinallyStack_，由 visitTryStmt 统一 pop（单次 pop 语义）
        return;
    }
    uint16_t afterFinallyTarget = static_cast<uint16_t>(afterFinally);
    chunk_.code[skipFinallyExceptionJumpPatch + 1] = static_cast<uint8_t>(afterFinallyTarget & 0xFF);
    chunk_.code[skipFinallyExceptionJumpPatch + 2] = static_cast<uint8_t>((afterFinallyTarget >> 8) & 0xFF);
}

void Compiler::visitPrintStmt(PrintStmt& node) {
    // C2 fix: 与解释器行为对齐 — 多参数用空格拼接后单次输出
    if (node.values.empty()) {
        // print() → 输出空行（与解释器 output("") 一致）
        uint16_t emptyIdx = chunk_.addConstant(Value(std::string("")));
        // D5 fix: 使用 OP_STRING 替代 OP_CONSTANT（二者功能相同，OP_CONSTANT 已废弃）
        chunk_.writeOp(OpCode::OP_STRING, node.line);
        chunk_.writeShort(emptyIdx, node.line);
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    } else if (node.values.size() == 1) {
        compileNode(node.values[0].get());
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    } else {
        // 多值：用 OP_ADD 拼接为单字符串（OP_ADD 已支持 string+non-string 拼接）
        compileNode(node.values[0].get());
        // C-P2-6 fix: 空格常量索引提升到循环外，避免每次迭代重复构造和哈希查找
        uint16_t spaceIdx = chunk_.addConstant(Value(std::string(" ")));
        for (size_t i = 1; i < node.values.size(); ++i) {
            // push " " + OP_ADD → 左侧被 toString 后与空格拼接
            // D5 fix: 使用 OP_STRING 替代 OP_CONSTANT
            chunk_.writeOp(OpCode::OP_STRING, node.line);
            chunk_.writeShort(spaceIdx, node.line);
            chunk_.writeOp(OpCode::OP_ADD, node.line);
            // push next value + OP_ADD
            compileNode(node.values[i].get());
            chunk_.writeOp(OpCode::OP_ADD, node.line);
        }
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    }
    return;
}

void Compiler::visitBlock(Block& node) {
    auto savedLocals = currentLocals_;

    if (!inFunction_) {
        blockDepth_++;

        // 收集块作用域中将要声明的变量名，以便在编译前保存被遮蔽的全局变量
        std::vector<std::pair<std::string, std::string>> shadowedSaves; // (blockVarName, tempSaveName)
        std::vector<std::pair<std::string, int>> removedSlots;          // H1: (name, slot) 用于恢复
        for (auto& stmt : node.statements) {
            if (stmt && stmt->nodeType == NodeType::NODE_VAR_DECL) {
                VarDecl* vd = static_cast<VarDecl*>(stmt.get());
                int gsSlot = lookupGlobalSlot(vd->name);
                if (gsSlot >= 0) {
                    std::string saveName = "__blk_save_" + std::to_string(blockDepth_) + "_" +
                                           std::to_string(blockSaveCounter_++) + "_" + vd->name;
                    shadowedSaves.push_back({vd->name, saveName});
                    // A2: 使用槽位操作码保存全局变量值
                    uint16_t saveIdx = identifierIndex(saveName);
                    chunk_.writeOp(OpCode::OP_GET_GLOBAL, vd->line);
                    chunk_.writeShort(static_cast<uint16_t>(gsSlot), vd->line);
                    chunk_.writeOp(OpCode::OP_DEFINE_VAR, vd->line);
                    chunk_.writeShort(saveIdx, vd->line);
                }
            }
        }

        // H1 fix: 保存全局值后，移除被遮蔽的全局槽位条目
        // 使块内 compileVarDecl/compileVarRef/compileAssignment 不命中全局槽位，
        // 改用 OP_DEFINE_VAR/OP_GET_VAR/OP_SET_VAR → globals_ 路径
        for (auto& [varName, saveName] : shadowedSaves) {
            int slot = globalSlotAllocator_.removeMapping(varName); // B4: 临时遮蔽
            if (slot >= 0) {
                removedSlots.push_back({varName, slot});
            }
        }

        // 编译块体
        for (auto& stmt : node.statements) {
            compileStatement(stmt.get());
        }

        // 找出块作用域内新声明的变量
        std::vector<std::string> blockVars;
        for (auto& [name, slot] : currentLocals_) {
            if (savedLocals.find(name) == savedLocals.end()) {
                blockVars.push_back(name);
            }
        }

        // 恢复外层作用域（P28: swap）
        currentLocals_.swap(savedLocals);
        blockDepth_--;

        // H1 fix: 恢复被移除的全局槽位条目（必须在恢复字节码之前，使 lookupGlobalSlot 正确）
        for (auto& [name, slot] : removedSlots) {
            globalSlotAllocator_.restoreMapping(name, slot); // B4: 恢复遮蔽
        }

        // 清理块作用域变量并恢复被遮蔽的全局变量
        for (auto& [varName, saveName] : shadowedSaves) {
            // A2: 使用槽位操作码恢复全局变量值
            int gsSlot = lookupGlobalSlot(varName);
            uint16_t saveIdx = identifierIndex(saveName);
            chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
            chunk_.writeShort(saveIdx, node.line);
            if (gsSlot >= 0) {
                chunk_.writeOp(OpCode::OP_SET_GLOBAL, node.line);
                chunk_.writeShort(static_cast<uint16_t>(gsSlot), node.line);
            } else {
                uint16_t origIdx = identifierIndex(varName);
                chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
                chunk_.writeShort(origIdx, node.line);
            }
            // 删除临时保存变量
            chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
            chunk_.writeShort(saveIdx, node.line);
        }

        // 删除块作用域变量（含被遮蔽变量的 globals_ 残留值）
        for (auto& name : blockVars) {
            uint16_t nameIdx = identifierIndex(name);
            chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    } else {
        // 函数内块作用域：局部变量使用栈槽。
        // BUG-AUDIT-CLOSE-1 fix: 块退出时需发射 OP_CLOSE_UPVALUE，关闭指向本块 slot 的
        // open upvalues，使闭包捕获块退出时刻的值快照（by-value），对齐 IR 路径
        // leaveBlockScope（IR.cpp:439-441）和 Interpreter 的 closeCapturedVariables。
        // 原实现注释"VM 帧退出时自动释放"是误解——closeUpvaluesFrom 在 OP_RETURN 时
        // 关闭 upvalue 产生的是函数返回时刻的快照（by-reference），与块退出快照语义不一致。
        size_t slotBase = currentLocals_.size(); // 块内第一个新 slot 的编号
        for (auto& stmt : node.statements) {
            compileStatement(stmt.get());
        }
        // 块退出时关闭指向 slot >= slotBase 的全部 open upvalues
        if (currentLocals_.size() > slotBase && slotBase <= 255) {
            chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(slotBase), node.line);
        }
        closeSlotRanges(slotBase);        // L1 fix: 回填本块内变量 range 的 endIp
        currentLocals_.swap(savedLocals); // P28: swap
    }
    return;
}

// ---- 新增节点编译 ----

void Compiler::visitArrayLiteral(ArrayLiteral& node) {
    // 检查元素数量上限（uint8_t 编码限制）
    if (node.elements.size() > 255) {
        error("数组元素数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有元素
    for (auto& elem : node.elements) {
        compileNode(elem.get());
    }
    // 构建数组指令
    chunk_.writeOp(OpCode::OP_BUILD_ARRAY, node.line);
    chunk_.write(static_cast<uint8_t>(node.elements.size()), node.line);
    return;
}

// R98 元组与解构：元组字面量编译
// 栈布局：依次 push 各 element，OP_BUILD_TUPLE count 弹出 count 个值并 push 元组。
void Compiler::visitTupleLiteral(TupleLiteral& node) {
    if (node.elements.size() > 255) {
        error("元组元素数量超过 255 个上限", node.line, node.column);
        return;
    }
    for (auto& elem : node.elements) {
        compileNode(elem.get());
    }
    chunk_.writeOp(OpCode::OP_BUILD_TUPLE, node.line);
    chunk_.write(static_cast<uint8_t>(node.elements.size()), node.line);
    return;
}

// R98 元组与解构：解构绑定 var (a, b, c) = expr 编译
// 策略：
//   1. 编译 initializer，栈顶为元组值
//   2. 对每个 name，按位置 i 生成：
//        DUP（复制元组到栈顶）+ OP_INT i + OP_INDEX_GET（取第 i 个元素）
//        + 变量绑定（OP_DEFINE_VAR / OP_SET_LOCAL）
//   3. 最后 OP_POP 清理元组（除非在表达式上下文，但解构绑定是语句，无返回值消费）
//   4. 末尾 OP_POP 清理 DUP 产生的栈顶元组副本
// 注意：与 visitVarDecl 不同，解构绑定是语句，不保留栈上值。
void Compiler::visitDestructureBinding(DestructureBinding& node) {
    if (node.names.size() > 255) {
        error("解构绑定变量数超过 255 个上限", node.line, node.column);
        return;
    }

    // 编译 initializer，栈顶为元组
    compileNode(node.initializer.get());

    // 为每个变量取元素并绑定
    for (size_t i = 0; i < node.names.size(); ++i) {
        // DUP 复制元组到栈顶（保留原元组供下一轮使用）
        chunk_.writeOp(OpCode::OP_DUP, node.line);
        // push 索引 i
        uint16_t idxConst = chunk_.addConstant(Value(static_cast<int64_t>(i)));
        chunk_.writeOp(OpCode::OP_INT, node.line);
        chunk_.writeShort(idxConst, node.line);
        // 取元素：tuple[i]
        chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);
        // 栈顶为 tuple[i]，绑定到变量 name
        bindDestructureVar(node.names[i], node.line, node.column);
    }

    // 清理栈顶元组（DUP 已被消费，仅剩原始元组）
    chunk_.writeOp(OpCode::OP_POP, node.line);
    return;
}

// R98 元组与解构：解构绑定单个变量的绑定逻辑（复用 visitVarDecl 的局部/全局分支）
void Compiler::bindDestructureVar(const std::string& name, int line, int col) {
    if (inFunction_) {
        auto it = currentLocals_.find(name);
        if (it == currentLocals_.end()) {
            int slot = static_cast<int>(currentLocals_.size());
            if (slot > 255) {
                error("函数局部变量数量超过限制（最大 256 个，含 this/参数/字段）", line, col);
                return;
            }
            currentLocals_[name] = slot;
            peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = name;
            slotNameRanges_.push_back({static_cast<uint8_t>(slot), name, chunk_.code.size(), 0});
            chunk_.writeOp(OpCode::OP_SET_LOCAL, line);
            chunk_.write(static_cast<uint8_t>(slot), line);
        } else {
            error("变量 '" + name + "' 已在当前作用域中定义", line, col);
            chunk_.writeOp(OpCode::OP_SET_LOCAL, line);
            chunk_.write(static_cast<uint8_t>(it->second), line);
        }
        // OP_SET_LOCAL 用 peek(0) 不消费栈顶，补发 OP_POP 清理元素
        chunk_.writeOp(OpCode::OP_POP, line);
    } else {
        int slot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, line);
            chunk_.writeShort(static_cast<uint16_t>(slot), line);
        } else {
            uint16_t nameIdx = identifierIndex(name);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, line);
            chunk_.writeShort(nameIdx, line);
        }
    }
}

// ============================================================
// R99 枚举与 ADT + match 表达式
// ============================================================

// enum 声明编译：将 enum 名绑定为标记值 "enum:Name"。
// 不需要向 VM 注册 variant 元信息——OP_BUILD_ENUM_VARIANT 通过
// 常量池中的 enumName/variantName 字符串直接构造 variant 值，
// 类型校验由 Interpreter（visitEnumVariantExpr）和 TypeChecker 负责。
void Compiler::visitEnumDecl(EnumDecl& node) {
    // 编译期：在当前作用域定义 enum 名为标记值 "enum:Name"
    // 运行时：OP_STRING + OP_DEFINE_VAR/OP_DEFINE_GLOBAL
    std::string marker = std::string("enum:") + node.name;
    uint16_t markerIdx = chunk_.addConstant(Value(marker));
    chunk_.writeOp(OpCode::OP_STRING, node.line);
    chunk_.writeShort(markerIdx, node.line);

    if (inFunction_) {
        // 函数内：使用局部变量槽位
        auto it = currentLocals_.find(node.name);
        if (it == currentLocals_.end()) {
            int slot = static_cast<int>(currentLocals_.size());
            if (slot > 255) {
                error("函数局部变量数量超过限制（最大 256 个，含 this/参数/字段）", node.line, node.column);
                chunk_.writeOp(OpCode::OP_POP, node.line); // 平衡栈
                return;
            }
            currentLocals_[node.name] = slot;
            peakLocals_ = std::max(peakLocals_, static_cast<int>(currentLocals_.size()));
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = node.name;
            slotNameRanges_.push_back({static_cast<uint8_t>(slot), node.name, chunk_.code.size(), 0});
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(slot), node.line);
            chunk_.writeOp(OpCode::OP_POP, node.line); // OP_SET_LOCAL 用 peek，需补 POP
        } else {
            // 重定义：直接写入已有槽位
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            chunk_.writeOp(OpCode::OP_POP, node.line);
        }
    } else {
        // 顶层：使用全局变量槽位或慢路径
        int slot = (blockDepth_ > 0) ? -1 : lookupGlobalSlot(node.name);
        if (slot >= 0) {
            chunk_.writeOp(OpCode::OP_DEFINE_GLOBAL, node.line);
            chunk_.writeShort(static_cast<uint16_t>(slot), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    }

    // R99 enum 校验：收集 enum 元信息到 pendingEnumInfos_，compile() 完成时
    // 写入 CompileResult.enumInfos 供 VM 启动时加载到 enumRegistry_。
    // 对齐 Interpreter::visitEnumDecl 的 enumRegistry_ 注册，使 OP_BUILD_ENUM_VARIANT
    // 能在运行时校验 variant 名存在性 + 参数 arity 一致性，保证三后端一致。
    VMEnumInfo info;
    info.name = node.name;
    info.variants.reserve(node.variants.size());
    for (const auto& v : node.variants) {
        VMEnumVariantInfo vi;
        vi.name = v.name;
        vi.arity = static_cast<int>(v.paramTypes.size());
        info.variants.push_back(std::move(vi));
    }
    pendingEnumInfos_.push_back(std::move(info));
    return;
}

// 枚举 variant 构造编译：EnumName.VariantName(arg1, arg2, ...)
// 栈布局：依次 push 各参数（按声明顺序），然后 OP_BUILD_ENUM_VARIANT 消费 argCount 个参数。
void Compiler::visitEnumVariantExpr(EnumVariantExpr& node) {
    if (node.arguments.size() > 255) {
        error("enum variant 参数数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有参数（按顺序 push）
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }
    // OP_BUILD_ENUM_VARIANT enumNameIdx variantNameIdx argCount
    uint16_t enumIdx = chunk_.addConstant(Value(node.enumName));
    uint16_t varIdx = chunk_.addConstant(Value(node.variantName));
    chunk_.writeOp(OpCode::OP_BUILD_ENUM_VARIANT, node.line);
    chunk_.writeShort(enumIdx, node.line);
    chunk_.writeShort(varIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    return;
}

// match 表达式编译：
//   1. 编译 scrutinee，push 到栈
//   2. 对每个 case：
//      - WILDCARD/DEFAULT：直接执行 body
//      - LITERAL：DUP + literal + OP_EQUAL + JUMP_IF_FALSE → next_case
//      - VARIANT：DUP + OP_ENUM_VARIANT_NAME + JUMP_IF_FALSE → next_case
//                 然后对每个 binding：DUP + OP_INT i + OP_ENUM_VARIANT_FIELD + bind
//      - 编译 body（push 结果）
//      - OP_SWAP（结果与 scrutinee 交换）+ OP_POP（删除 scrutinee）
//      - JUMP → end
//   3. 所有 case 未匹配：OP_POP（删 scrutinee）+ OP_NULL（push null 作为结果）
//   4. end：
void Compiler::visitMatchExpr(MatchExpr& node) {
    // 编译 scrutinee，栈顶为 scrutinee 值
    compileNode(node.scrutinee.get());

    // 收集所有 case 的 JUMP_IF_FALSE 占位（用于跳到 next case）
    std::vector<size_t> caseSkipPatches;
    // 收集所有 case body 末尾的 JUMP（跳到 end）占位
    std::vector<size_t> endJumps;

    auto savedLocals = currentLocals_;
    size_t branchSlotBase = currentLocals_.size();
    bool needCloseUpvalue = inFunction_;

    for (auto& mc : node.cases) {
        // case 匹配检查（emitMatchPattern 处理 WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR 六种模式）
        if (!mc.isDefault && mc.pattern) {
            emitMatchPattern(*mc.pattern, caseSkipPatches);
        }
        // default case 无需 pattern 检查
        // R133: guard 编译——pattern 匹配成功后求值 guard，false 则跳到下一 case
        if (mc.guard) {
            compileNode(mc.guard.get());
            size_t guardSkipPatch = chunk_.code.size();
            chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, mc.guard->line);
            chunk_.writeShort(0, mc.guard->line);
            caseSkipPatches.push_back(guardSkipPatch);
            chunk_.writeOp(OpCode::OP_POP, mc.guard->line); // 弹出 guard bool
        }

        // 编译 case body（push 结果到栈），body 可能是 Block 或单表达式
        compileMatchBody(mc.body.get(), node.line);

        // 关闭 case 作用域内声明的 upvalue（与 visitIfStmt 一致）
        if (needCloseUpvalue && currentLocals_.size() > branchSlotBase && branchSlotBase <= 255) {
            chunk_.writeOp(OpCode::OP_CLOSE_UPVALUE, node.line);
            chunk_.write(static_cast<uint8_t>(branchSlotBase), node.line);
        }
        closeSlotRanges(branchSlotBase);
        currentLocals_ = savedLocals;

        // 栈布局：[scrutinee, body_result] → OP_SWAP → [body_result, scrutinee] → OP_POP → [body_result]
        chunk_.writeOp(OpCode::OP_SWAP, node.line); // 交换 scrut 和 result
        chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出 scrut

        // JUMP → end
        size_t endJump = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);
        endJumps.push_back(endJump);

        // R133: 回填当前 case 的所有 skip patch（pattern + guard）跳到下一 case 起始
        // 原 R99 代码仅回填单个 patch；扩展后每个 case 可能有多个 patch（嵌套 pattern + guard）
        if (!caseSkipPatches.empty()) {
            uint16_t nextCaseStart = safeCodeOffset();
            while (!caseSkipPatches.empty()) {
                size_t skipPatch = caseSkipPatches.back();
                caseSkipPatches.pop_back();
                chunk_.code[skipPatch + 1] = static_cast<uint8_t>(nextCaseStart & 0xFF);
                chunk_.code[skipPatch + 2] = static_cast<uint8_t>((nextCaseStart >> 8) & 0xFF);
            }
            // JUMP_IF_FALSE 不消费条件值，跳过时栈上仍 bool，补 OP_POP
            chunk_.writeOp(OpCode::OP_POP, node.line);
        }
    }

    // 所有 case 未匹配：OP_POP 删 scrut + OP_NULL push null
    chunk_.writeOp(OpCode::OP_POP, node.line);
    chunk_.writeOp(OpCode::OP_NULL, node.line);

    // 回填所有 end jumps
    uint16_t endTarget = safeCodeOffset();
    for (size_t endJump : endJumps) {
        chunk_.code[endJump + 1] = static_cast<uint8_t>(endTarget & 0xFF);
        chunk_.code[endJump + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
    }
    return;
}

// ============================================================
// visitMatchExpr 子阶段实现
// ============================================================

void Compiler::emitMatchPattern(const MatchPattern& p, std::vector<size_t>& caseSkipPatches) {
    // R133 模式匹配扩展：递归处理六种 pattern（WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）。
    // 栈布局不变量：调用前 [scrut]，调用后 [scrut]（保持 scrut 在栈顶供后续 case 复用）。
    // 匹配失败跳转 patch 追加到 caseSkipPatches 由调用方回填到下一 case 起始。
    switch (p.kind) {
    case MatchPatternKind::WILDCARD:
        // 永远匹配，无需检查
        break;
    case MatchPatternKind::LITERAL: {
        // DUP scrutinee + literal + OP_EQUAL + JUMP_IF_FALSE → next + OP_POP
        chunk_.writeOp(OpCode::OP_DUP, p.line);
        compileNode(p.literal.get());
        chunk_.writeOp(OpCode::OP_EQUAL, p.line);
        size_t skipPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, p.line);
        chunk_.writeShort(0, p.line);
        caseSkipPatches.push_back(skipPatch);
        chunk_.writeOp(OpCode::OP_POP, p.line);
        break;
    }
    case MatchPatternKind::VARIABLE: {
        // R133: DUP scrutinee + 绑定整个 scrut 到 variableName
        // DUP 让 scrut 留在栈顶（保持栈不变量），副本绑定到 variableName
        chunk_.writeOp(OpCode::OP_DUP, p.line);
        bindDestructureVar(p.variableName, p.line, p.column);
        break;
    }
    case MatchPatternKind::VARIANT: {
        // DUP scrutinee + OP_ENUM_VARIANT_NAME + JUMP_IF_FALSE → next + OP_POP
        chunk_.writeOp(OpCode::OP_DUP, p.line);
        uint16_t enumIdx = chunk_.addConstant(Value(p.enumName));
        uint16_t varIdx = chunk_.addConstant(Value(p.variantName));
        chunk_.writeOp(OpCode::OP_ENUM_VARIANT_NAME, p.line);
        chunk_.writeShort(enumIdx, p.line);
        chunk_.writeShort(varIdx, p.line);
        size_t skipPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, p.line);
        chunk_.writeShort(0, p.line);
        caseSkipPatches.push_back(skipPatch);
        chunk_.writeOp(OpCode::OP_POP, p.line); // 弹出 bool
        // 绑定 variant 字段（递归子 pattern）
        // R133: subPatterns 替代 R99 的 bindings（vector<string>），支持嵌套
        for (size_t i = 0; i < p.subPatterns.size(); ++i) {
            // DUP scrutinee + OP_INT i + OP_ENUM_VARIANT_FIELD + 递归 emitMatchPattern(sub, ...)
            chunk_.writeOp(OpCode::OP_DUP, p.line);
            uint16_t idxConst = chunk_.addConstant(Value(static_cast<int64_t>(i)));
            chunk_.writeOp(OpCode::OP_INT, p.line);
            chunk_.writeShort(idxConst, p.line);
            chunk_.writeOp(OpCode::OP_ENUM_VARIANT_FIELD, p.line);
            // 栈顶是 fields[i]，递归匹配子 pattern
            // R133 fix: 子 pattern fail 用独立 subFailPatches，失败时先 POP field 再跳到 caseSkip（同 TUPLE pattern）
            // R133 fix-2: 成功路径 OP_POP 后必须 JUMP 跳过失败路径代码（同 TUPLE pattern）
            {
                std::vector<size_t> subFailPatches;
                emitMatchPattern(*p.subPatterns[i], subFailPatches);
                chunk_.writeOp(OpCode::OP_POP, p.line); // 成功路径 POP field
                if (!subFailPatches.empty()) {
                    // 成功路径 JUMP 跳过失败路径代码（占位，稍后回填）
                    size_t successJumpOver = chunk_.code.size();
                    chunk_.writeOp(OpCode::OP_JUMP, p.line);
                    chunk_.writeShort(0, p.line);
                    // 失败路径入口
                    uint16_t subFailPos = safeCodeOffset();
                    for (auto& sp : subFailPatches) {
                        chunk_.code[sp + 1] = static_cast<uint8_t>(subFailPos & 0xFF);
                        chunk_.code[sp + 2] = static_cast<uint8_t>((subFailPos >> 8) & 0xFF);
                    }
                    chunk_.writeOp(OpCode::OP_POP, p.line); // 失败路径 POP field
                    size_t jumpToSkip = chunk_.code.size();
                    chunk_.writeOp(OpCode::OP_JUMP, p.line);
                    chunk_.writeShort(0, p.line);
                    caseSkipPatches.push_back(jumpToSkip);
                    // 回填成功路径 JUMP
                    uint16_t afterFail = safeCodeOffset();
                    chunk_.code[successJumpOver + 1] = static_cast<uint8_t>(afterFail & 0xFF);
                    chunk_.code[successJumpOver + 2] = static_cast<uint8_t>((afterFail >> 8) & 0xFF);
                }
            }
        }
        break;
    }
    case MatchPatternKind::TUPLE: {
        // R133: 元组模式 - 检查是 tuple + 元素数匹配 + 递归匹配每个元素
        // 软类型检查：DUP + OP_TYPE_TEST "tuple"（push bool 不抛错）+ JUMP_IF_FALSE next + POP
        // 注：用 OP_TYPE_TEST 而非 OP_TYPE_CHECK——后者不匹配时 runtimeError，
        // 而 TUPLE pattern 不匹配时应 fall through 到下一 case 而非抛错。
        chunk_.writeOp(OpCode::OP_DUP, p.line);
        uint16_t tupleTypeNameConst = chunk_.addConstant(Value(std::string("tuple")));
        chunk_.writeOp(OpCode::OP_TYPE_TEST, p.line);
        chunk_.writeShort(tupleTypeNameConst, p.line);
        size_t typeSkipPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, p.line);
        chunk_.writeShort(0, p.line);
        caseSkipPatches.push_back(typeSkipPatch);
        chunk_.writeOp(OpCode::OP_POP, p.line);
        // 元素数检查：DUP + OP_LEN + OP_INT size + OP_EQUAL + JUMP_IF_FALSE next + POP
        chunk_.writeOp(OpCode::OP_DUP, p.line);
        chunk_.writeOp(OpCode::OP_LEN, p.line);
        uint16_t sizeConst = chunk_.addConstant(Value(static_cast<int64_t>(p.subPatterns.size())));
        chunk_.writeOp(OpCode::OP_INT, p.line);
        chunk_.writeShort(sizeConst, p.line);
        chunk_.writeOp(OpCode::OP_EQUAL, p.line);
        size_t sizeSkipPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, p.line);
        chunk_.writeShort(0, p.line);
        caseSkipPatches.push_back(sizeSkipPatch);
        chunk_.writeOp(OpCode::OP_POP, p.line);
        // 递归匹配每个元素
        for (size_t i = 0; i < p.subPatterns.size(); ++i) {
            // DUP tuple + OP_INT i + OP_INDEX_GET + 递归 emitMatchPattern(sub, ...) + POP
            chunk_.writeOp(OpCode::OP_DUP, p.line);
            uint16_t idxConst = chunk_.addConstant(Value(static_cast<int64_t>(i)));
            chunk_.writeOp(OpCode::OP_INT, p.line);
            chunk_.writeShort(idxConst, p.line);
            chunk_.writeOp(OpCode::OP_INDEX_GET, p.line);
            // R133 fix: 子 pattern 的 fail 用独立 subFailPatches，失败时先 POP element 再跳到 caseSkip。
            // 原实现把子 pattern fail 直接加入 caseSkipPatches，失败时栈上有 [scrut, element, bool]，
            // caseSkip 处只 POP bool，element 残留导致后续 case 栈错乱。
            // 正确做法：子 pattern fail 回填到"POP element + JUMP caseSkip"，先清栈再跳转。
            // R133 fix-2: 成功路径 OP_POP 后必须 JUMP 跳过失败路径代码，否则会跌入失败路径
            // 的 OP_POP+OP_JUMP，导致栈下溢。结构：
            //   [子 pattern body] → [scrut, elem, bool]
            //   OP_JUMP_IF_FALSE subFailPos  (失败跳走，栈 [scrut, elem, bool])
            //   OP_POP  (成功路径 LITERAL 自己的 pop bool) → [scrut, elem]
            //   OP_POP  (成功路径 TUPLE loop 的 pop elem) → [scrut]
            //   OP_JUMP afterFail   ← 成功路径跳过失败代码
            //   subFailPos:
            //   OP_POP  (失败路径 pop bool) → [scrut, elem]
            //   OP_JUMP caseSkip → [scrut, elem] (caseSkip 处 OP_POP 消费 elem)
            //   afterFail: (下一 sub-pattern)
            {
                std::vector<size_t> subFailPatches;
                emitMatchPattern(*p.subPatterns[i], subFailPatches);
                // 成功路径：POP element 继续
                chunk_.writeOp(OpCode::OP_POP, p.line);
                // 失败路径：回填 subFailPatches 到此位置，POP element 后跳到 caseSkip
                if (!subFailPatches.empty()) {
                    // 成功路径 JUMP 跳过失败路径代码（占位，稍后回填）
                    size_t successJumpOver = chunk_.code.size();
                    chunk_.writeOp(OpCode::OP_JUMP, p.line);
                    chunk_.writeShort(0, p.line);
                    // 失败路径入口
                    uint16_t subFailPos = safeCodeOffset();
                    for (auto& sp : subFailPatches) {
                        chunk_.code[sp + 1] = static_cast<uint8_t>(subFailPos & 0xFF);
                        chunk_.code[sp + 2] = static_cast<uint8_t>((subFailPos >> 8) & 0xFF);
                    }
                    chunk_.writeOp(OpCode::OP_POP, p.line); // 失败路径 POP element
                    size_t jumpToSkip = chunk_.code.size();
                    chunk_.writeOp(OpCode::OP_JUMP, p.line);
                    chunk_.writeShort(0, p.line);
                    caseSkipPatches.push_back(jumpToSkip);
                    // 回填成功路径 JUMP 到此（下一 sub-pattern 或 TUPLE 末尾）
                    uint16_t afterFail = safeCodeOffset();
                    chunk_.code[successJumpOver + 1] = static_cast<uint8_t>(afterFail & 0xFF);
                    chunk_.code[successJumpOver + 2] = static_cast<uint8_t>((afterFail >> 8) & 0xFF);
                }
            }
        }
        break;
    }
    case MatchPatternKind::OR: {
        // R133: OR pattern - 任一子 pattern 匹配即成功
        // 编译策略：依次尝试每个子 pattern，任一成功跳到 OR 成功位置（OR 末尾）；
        // 全部失败时 emit 永假检查让 case 整体跳到下一 case。
        std::vector<size_t> successJumps; // 各子 pattern 成功后跳到 OR 末尾
        for (size_t i = 0; i < p.subPatterns.size(); ++i) {
            // 编译子 pattern，失败的 patch 暂存到 subPatches
            std::vector<size_t> subPatches;
            emitMatchPattern(*p.subPatterns[i], subPatches);
            // 子 pattern 成功：跳到 OR 末尾
            size_t successJump = chunk_.code.size();
            chunk_.writeOp(OpCode::OP_JUMP, p.line);
            chunk_.writeShort(0, p.line);
            successJumps.push_back(successJump);
            // 子 pattern 失败位置：所有 subPatches 回填到同一位置，然后只写一次 POP
            // R133 fix: 原实现每个 sp 都 writeOp POP，导致 failPos 处有多个 POP，
            // 第一个 sp 跳转到 failPos 后会连续执行多个 POP，栈下溢。
            // 正确做法：所有 patch 共享同一 failPos，只写一次 POP。
            uint16_t failPos = safeCodeOffset();
            for (auto& sp : subPatches) {
                chunk_.code[sp + 1] = static_cast<uint8_t>(failPos & 0xFF);
                chunk_.code[sp + 2] = static_cast<uint8_t>((failPos >> 8) & 0xFF);
            }
            if (!subPatches.empty()) {
                // 失败路径有 bool 残留需 POP（JUMP_IF_FALSE 不消费条件值）
                chunk_.writeOp(OpCode::OP_POP, p.line);
            }
        }
        // 所有子 pattern 失败：OR 失败
        // R133 fix: caseSkip handler 约定——所有 fail patch 跳转时栈须为 [scrut, X]
        // （X 为 JUMP_IF_FALSE 残留的 bool 或 TUPLE/VARIANT sub-pattern fail 残留的 elem）。
        // handler 执行一次 OP_POP 消费 X，回到 [scrut] 供下一 case 使用。
        // OR 整体失败时栈已回到 [scrut]（sub-pattern failPos 已 POP bool），无残值 X，
        // 故 push 一个 OP_NULL 作为占位残值，让 caseSkip handler 的 OP_POP 正确消费。
        chunk_.writeOp(OpCode::OP_NULL, p.line);
        size_t orFailJump = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, p.line);
        chunk_.writeShort(0, p.line);
        caseSkipPatches.push_back(orFailJump);
        // OR 成功位置：回填所有 successJumps 跳到这里
        uint16_t orSuccess = safeCodeOffset();
        for (auto& sj : successJumps) {
            chunk_.code[sj + 1] = static_cast<uint8_t>(orSuccess & 0xFF);
            chunk_.code[sj + 2] = static_cast<uint8_t>((orSuccess >> 8) & 0xFF);
        }
        break;
    }
    }
}

void Compiler::compileMatchBody(ASTNode* body, int line) {
    // 编译 case body 并保留值在栈顶（栈不变量：调用前 []，调用后 [body_result]）。
    // - Block body：编译除最后一条外的所有语句（带 POP），最后一条用 compileNode 保留值；
    //   非表达式语句补 OP_NULL。
    // - 单表达式 body：直接 compileNode。
    // - 空 body：push OP_NULL。
    // 16 种表达式节点类型判定（与 IR.cpp visitMatchExpr 的 needsPopForExprStmt 对齐）：
    //   ASSIGNMENT/FUN_CALL/METHOD_CALL/BINARY_OP/UNARY_OP/VAR_REF/MEMBER_ACCESS/INDEX_ACCESS
    //   /NUMBER/STRING/BOOL/NULL_LITERAL/SUPER_EXPR/ARRAY/DICT/TUPLE/INTERPOLATED/ENUM_VARIANT/MATCH
    if (!body) {
        // 空 body：push null
        chunk_.writeOp(OpCode::OP_NULL, line);
        return;
    }

    if (body->nodeType != NodeType::NODE_BLOCK) {
        compileNode(body);
        return;
    }

    // Block：执行所有语句，最后一条语句的值作为结果
    Block* blk = static_cast<Block*>(body);
    if (blk->statements.empty()) {
        chunk_.writeOp(OpCode::OP_NULL, line);
        return;
    }
    // 编译除最后一条外的所有语句（compileStatement 会消费表达式值）
    for (size_t i = 0; i + 1 < blk->statements.size(); ++i) {
        compileStatement(blk->statements[i].get());
    }
    // 最后一条语句用 compileNode 保留值在栈顶（不弹）
    ASTNode* last = blk->statements.back().get();
    compileNode(last);
    // 若 last 非表达式节点（如 VarDecl/IfStmt），不会产生栈值，补 push null
    switch (last->nodeType) {
    case NodeType::NODE_ASSIGNMENT:
    case NodeType::NODE_FUN_CALL:
    case NodeType::NODE_METHOD_CALL:
    case NodeType::NODE_BINARY_OP:
    case NodeType::NODE_UNARY_OP:
    case NodeType::NODE_VAR_REF:
    case NodeType::NODE_MEMBER_ACCESS:
    case NodeType::NODE_INDEX_ACCESS:
    case NodeType::NODE_NUMBER_LITERAL:
    case NodeType::NODE_STRING_LITERAL:
    case NodeType::NODE_BOOL_LITERAL:
    case NodeType::NODE_NULL_LITERAL:
    case NodeType::NODE_SUPER_EXPR:
    case NodeType::NODE_ARRAY_LITERAL:
    case NodeType::NODE_DICT_LITERAL:
    case NodeType::NODE_TUPLE_LITERAL:
    case NodeType::NODE_INTERPOLATED_STRING:
    case NodeType::NODE_ENUM_VARIANT_EXPR:
    case NodeType::NODE_MATCH_EXPR:
        // 表达式节点：值已在栈顶
        break;
    default:
        // 声明/控制流：无值产生，补 null 作为 case 结果
        chunk_.writeOp(OpCode::OP_NULL, line);
        break;
    }
}

void Compiler::visitDictLiteral(DictLiteral& node) {
    // 检查键值对数量上限（uint8_t 编码限制）
    if (node.pairs.size() > 255) {
        error("字典键值对数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有键值对（先键后值，与 OP_BUILD_DICT 消费顺序一致）
    for (auto& pair : node.pairs) {
        compileNode(pair.first.get());
        compileNode(pair.second.get());
    }
    // 构建字典指令：弹出 2*count 个值，构建字典，push 到栈
    chunk_.writeOp(OpCode::OP_BUILD_DICT, node.line);
    chunk_.write(static_cast<uint8_t>(node.pairs.size()), node.line);
    return;
}

void Compiler::visitIndexAccess(IndexAccess& node) {
    compileNode(node.object.get());
    compileNode(node.index.get());
    chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);
    return;
}

void Compiler::visitIndexAssign(IndexAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                         ? static_cast<VarRef*>(node.object.get())
                         : nullptr;

    if (objVar) {
        // 简单路径：arr[i] = val（基是 VarRef）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
        } else {
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint16_t nameIdx = identifierIndex(objVar->name);
            chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
        return;
    }

    // M1 fix: 嵌套索引赋值（2 层）
    // 检测 node.object 是否是 IndexAccess(VarRef) 或 MemberAccess(VarRef)
    IndexAccess* outerIdx = (node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS)
                                ? static_cast<IndexAccess*>(node.object.get())
                                : nullptr;
    MemberAccess* outerMem = (node.object && node.object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                                 ? static_cast<MemberAccess*>(node.object.get())
                                 : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());

    if (baseVar) {
        auto localIt = currentLocals_.find(baseVar->name);
        bool isLocal = (localIt != currentLocals_.end());

        // 编译外层表达式 → push base[outerIdx] 或 base.field（正确求值中间值）
        // 注意：不再 push base 变量本身——WRITEBACK_*_LOCAL/VAR 和 MEMBER_SET_LOCAL/VAR
        // 均不需要 base 在栈上，原 push base 会导致栈泄漏（每次嵌套赋值泄漏 1 个 Value）。
        compileNode(node.object.get());
        // 编译内层索引 → push innerIdx
        compileNode(node.index.get());
        // 编译值 → push val
        compileNode(node.value.get());
        // OP_INDEX_SET: 弹出 val/innerIdx/outerValue → 修改 → lastMutatedReceiver_
        chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
        // 将变异后的内层容器写回基变量。
        // 对齐 IR 路径（IR.cpp emitNestedAssignWriteback L2696-2708）：
        //   IR 路径用 LOAD_MUTATED + INDEX_SET(base2, outerIdx, mut) + WRITEBACK 链传播变异；
        //   非 IR 路径改用更简洁的 LOAD_MUTATED + INDEX_SET_LOCAL/VAR 直接原地修改基变量。
        // R156 fix: 原 outerIdx 路径用 WRITEBACK_INDEX_LOCAL/VAR 做整体替换，
        // 将 arr 变量替换为变异后的 arr[i]（而非 arr[i] = mutated_arr[i]），
        // 导致 arr[i][j]=val 在非 IR 路径（StackVM 非 IR + JIT）全部失败。
        // outerMem 路径（base.field[idx]=val）已在 BUG-INH-AUDIT-4 fix 中修正为
        // LOAD_MUTATED + MEMBER_SET_LOCAL/VAR，此路径正确无需修改。
        if (isLocal) {
            if (outerIdx) {
                // base[outerIdx] = mutated → 重新求值 outerIdx + LOAD_MUTATED + INDEX_SET_LOCAL
                // INDEX_SET_LOCAL 直接修改 stack_[bp+slot][outerIdx] = mutated（原地）
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
            } else {
                // base.field = mutated → LOAD_MUTATED + MEMBER_SET_LOCAL（BUG-INH-AUDIT-4 fix，不变）
                uint16_t fieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
                chunk_.writeShort(fieldIdx, node.line);
            }
        } else {
            if (outerIdx) {
                // base[outerIdx] = mutated → 重新求值 outerIdx + LOAD_MUTATED + INDEX_SET_VAR
                uint16_t nameIdx = identifierIndex(baseVar->name);
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            } else {
                // base.field = mutated → LOAD_MUTATED + MEMBER_SET_VAR（不变）
                uint16_t varIdx = identifierIndex(baseVar->name);
                uint16_t fieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
                chunk_.writeShort(varIdx, node.line);
                chunk_.writeShort(fieldIdx, node.line);
            }
        }
        return;
    }

    // C-P2-2 fix: 3+ 层嵌套或复杂表达式：不支持，报错而非静默丢失修改
    error("不支持 3 层及以上嵌套索引赋值（如 a[b[c]] = d）", node.line, node.column);
    compileNode(node.object.get());
    compileNode(node.index.get());
    compileNode(node.value.get());
    chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
    return;
}

void Compiler::visitClassDecl(ClassDecl& node) {
    // ── 类编译总策略 ───────────────────────────────────────────────────
    // 类在运行时是一个闭包值（携带方法表）。编译分三步：
    //   1) 字段布局：先收集父类字段（classFieldNames_ 中查 superClassName），再追加
    //      子类自有字段，得到 [父类字段..., 子类字段...] 完整顺序——这是运行时的实例
    //      槽布局，决定 OP_GET_FIELD/OP_SET_FIELD 的索引含义（见 VMContainers.cpp）。
    //   2) 发射 OP_CLASS_NEW <fieldCount> 创建空实例，随后对每个实例字段补发
    //      OP_INIT_FIELD <fieldIdx> 写入由字段初始化表达式求值得到的默认值。
    //   3) 方法编译：每个方法作为嵌套 FunDecl 编译为独立 chunk，并以闭包形式挂到实例
    //      方法表（供 OP_METHOD_CALL 通过 this 派发），同时记录到 classFieldNames_
    //      供其子类继承字段顺序。
    // 类声明本身 emit 一个"类对象值"到栈顶，由调用方（声明语句）消费或 POP。
    //
    // 拆分说明（原 221 行单函数 → orchestrator + 2 子阶段）：
    //   - compileClassMembers：方法编译循环，每迭代创建 CompileContextGuard 并调用
    //                          emitMethodBody；currentClassName_ 在循环内手动保存/恢复
    //   - emitMethodBody：单个方法体编译（chunk 设置 + this/字段/参数槽位 + body +
    //                     元数据保存 + 默认参数值 emit）
    // 不变量保留：R97 #9 fix CompileContextGuard、B1 fix currentClassName_ 保存/恢复、
    // C-P2-3/4 fix slot>255 early return、BUG 6a fix 默认参数折叠（由 emitDefaultValues 统一）。

    // 类声明：发射 OP_CLASS_NEW + OP_INIT_FIELD 初始化字段 + 编译方法
    uint16_t nameIdx = identifierIndex(node.name);

    // 收集当前类的自有字段名
    std::vector<std::string> ownFieldNames;
    for (auto& member : node.members) {
        if (member->nodeType == NodeType::NODE_VAR_DECL) {
            ownFieldNames.push_back(static_cast<VarDecl*>(member.get())->name);
        }
    }

    // 构建含继承字段的完整字段列表（父类字段在前，子类字段在后）
    std::vector<std::string> allFieldNames;
    if (!node.superClassName.empty()) {
        auto it = classFieldNames_.find(node.superClassName);
        if (it != classFieldNames_.end()) {
            allFieldNames = it->second; // 父类字段在前
        } else {
            // C-P2-9 fix: 父类未找到时报错，而非静默丢失继承字段（导致 slot 布局错误）
            error("类 '" + node.name + "' 的父类 '" + node.superClassName +
                      "' 未定义（不支持前向引用，请确保父类在子类之前声明）",
                  node.line, node.column);
        }
    }
    for (const auto& fn : ownFieldNames) {
        if (std::find(allFieldNames.begin(), allFieldNames.end(), fn) == allFieldNames.end()) {
            allFieldNames.push_back(fn); // 子类字段在后（跳过覆盖的同名字段）
        }
    }

    // 注册类字段名（供子类编译时查找）
    classFieldNames_[node.name] = allFieldNames;

    // 先编译所有方法为独立 chunk
    compileClassMembers(node, allFieldNames);

    // 主 chunk 中：发射 OP_CLASS_NEW（0 参数构造，字段由 OP_INIT_FIELD 设置）
    chunk_.writeOp(OpCode::OP_CLASS_NEW, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(0), node.line); // argCount = 0（字段由 OP_INIT_FIELD 初始化）

    // 此时栈顶是刚创建的空实例，发射 OP_INIT_FIELD 设置每个字段的默认值
    for (auto& member : node.members) {
        if (member->nodeType != NodeType::NODE_VAR_DECL)
            continue;
        VarDecl* varDecl = static_cast<VarDecl*>(member.get());
        // 编译字段默认值表达式
        if (varDecl->initializer) {
            compileNode(varDecl->initializer.get());
        } else {
            chunk_.writeOp(OpCode::OP_NULL, varDecl->line);
        }
        // 发射 OP_INIT_FIELD：pop 默认值，设置到栈顶实例的字段中
        uint16_t fieldIdx = identifierIndex(varDecl->name);
        chunk_.writeOp(OpCode::OP_INIT_FIELD, varDecl->line);
        chunk_.writeShort(fieldIdx, varDecl->line);
    }

    // 将类注册为全局变量（OP_DEFINE_CLASS 从栈上 pop 模板实例并注册类信息）
    // 操作数: nameIdx(2B) + superNameIdx(2B)
    //   superNameIdx == 0xFFFF 表示无父类；否则为父类名在常量池中的索引
    chunk_.writeOp(OpCode::OP_DEFINE_CLASS, node.line);
    chunk_.writeShort(nameIdx, node.line);
    if (node.superClassName.empty()) {
        chunk_.writeShort(0xFFFF, node.line); // 无父类标记
    } else {
        uint16_t superIdx = identifierIndex(node.superClassName);
        chunk_.writeShort(superIdx, node.line);
    }

    // 栈上的模板实例已被 OP_DEFINE_CLASS 消费，无需额外 OP_POP
    return;
}

// ============================================================
// visitClassDecl 子阶段实现
// ============================================================

void Compiler::compileClassMembers(ClassDecl& node, const std::vector<std::string>& allFieldNames) {
    for (auto& member : node.members) {
        if (member->nodeType != NodeType::NODE_FUN_DECL)
            continue;
        FunDecl* funDecl = static_cast<FunDecl*>(member.get());
        // R97 #9 fix: 使用 CompileContextGuard RAII 自动保存/恢复 15 个编译上下文成员变量
        // （含异常路径），替代原 15 行 std::move 保存 + 30 行手动恢复（正常路径 + slot 越界错误路径）。
        // currentClassName_ 不在 CompileContext 中（仅 visitClassDecl 使用），单独手动保存。
        CompileContextGuard guard(*this);
        std::string savedClassName = currentClassName_; // B1 fix

        emitMethodBody(*funDecl, node.name, allFieldNames, guard, node.typeParams);

        // R97 #9 fix: guard 析构自动恢复 15 个上下文变量；currentClassName_ 单独手动恢复。
        // emitMethodBody 可能 early return（slot 越界），currentClassName_ 必须在所有路径恢复，
        // 故放在 emitMethodBody 返回之后统一执行（原实现分别在正常路径与错误路径恢复）。
        currentClassName_ = savedClassName;
    }
}

void Compiler::emitMethodBody(FunDecl& method, const std::string& className,
                              const std::vector<std::string>& allFieldNames, CompileContextGuard& guard,
                              const std::vector<std::string>& classTypeParams) {
    std::string methodKey = className + "." + method.name;

    chunk_ = BytecodeChunk(methodKey, static_cast<int>(method.params.size()));
    chunk_.reserveCode(256); // C21: 预分配方法字节码空间
    // F10: 设置必需参数个数
    chunk_.requiredArity = method.requiredParamCount;
    varIndex_.clear();
    currentLocals_.clear();
    currentUpvalues_.clear(); // C-P2-10 fix: 方法编译使用独立的 upvalue 列表
    currentUpvalueNames_.clear();
    localSlotNames_.clear();       // BUG-IDE-12 fix: 清空槽位名映射
    slotNameRanges_.clear();       // L1 fix: 清空 IP 范围表
    stringConstIndex_.clear();     // R164 fixup2: 清空字符串常量去重缓存（新 chunk 有新常量池）
    currentClassName_ = className; // B1 fix: 记录当前类名供 super 使用
    // R163 泛型扩展：合并类泛型参数 + 方法泛型参数，供 emitTypeCheck 擦除（对齐 Interpreter invokeMethod 的 mergedTypeParams）
    currentTypeParams_ = classTypeParams;
    for (const auto& tp : method.typeParams) {
        currentTypeParams_.push_back(tp);
    }
    // O5: 如果类定义在函数内，设置 outerLocals_ 以检测不支持的闭包捕获
    // C-P2-10 fix: 与 visitFunDecl 一致——使用 guard.saved 引用外层状态
    if (guard.saved.inFunction) {
        outerLocals_ = guard.saved.currentLocals;
        outerUpvalues_ = guard.saved.currentUpvalues;
        outerUpvalueNames_ = guard.saved.currentUpvalueNames;
    } else {
        outerLocals_.clear();
        outerUpvalues_.clear();
        outerUpvalueNames_.clear();
    }
    inFunction_ = true;
    // C-P0-1/C-P0-3 fix: 方法体的循环栈和 try 深度从 0 开始
    loopStack_.clear();
    tryDepth_ = 0;

    // 局部变量映射：slot 0 = this，slot 1..N = 字段（含继承字段），slot N+1.. = 参数
    int slot = 0;
    currentLocals_["this"] = slot++;   // slot 0: this
    localSlotNames_.push_back("this"); // BUG-IDE-12 fix
    for (const auto& fieldName : allFieldNames) {
        currentLocals_[fieldName] = slot++;   // slot 1..N: 实例字段（含继承）
        localSlotNames_.push_back(fieldName); // BUG-IDE-12 fix
    }
    for (int i = 0; i < static_cast<int>(method.params.size()); ++i) {
        currentLocals_[method.params[i]] = slot++;   // slot N+1..: 参数
        localSlotNames_.push_back(method.params[i]); // BUG-IDE-12 fix
    }
    peakLocals_ = slot;
    // C-P1-2 fix: 方法局部变量槽位上限 255（uint8_t 编码限制，含 this/字段/参数）
    // C-P2-3/4 fix: >256 改为 >255（slot==256 截断为 0 与 this 碰撞），并提前 return 避免用截断 slot 继续编译
    if (slot > 255) {
        error("类 '" + className + "' 方法 '" + method.name + "' 局部变量槽位超过限制（最大 256，当前 " +
                  std::to_string(slot) + "，含 this/字段/参数）",
              method.line, 0);
        // currentClassName_ 由 compileClassMembers 在迭代结束时恢复
        return;
    }

    // 记录字段声明顺序（含继承字段），供 VM OP_METHOD_CALL 按序推入
    chunk_.fieldOrder = allFieldNames;

    if (method.body) {
        compileNode(method.body.get());
    }

    chunk_.writeOp(OpCode::OP_NULL, method.line);
    chunk_.writeOp(OpCode::OP_RETURN, method.line);

    // 记录局部变量总槽位数（含 this/字段/参数和方法体内 var 声明），供 VM 预分配栈空间
    chunk_.localCount = peakLocals_;
    // BUG-IDE-12 fix: 保存 slot→name 映射到 chunk，供 VM 条件断点求值
    chunk_.localSlotNames = localSlotNames_;
    // L1 fix: 保存 IP 范围表到 chunk，供 VM 调试器按 frame.ip 反查变量名
    chunk_.slotNameRanges = slotNameRanges_;

    // F10: 编译默认参数值为常量（与 visitFunDecl 一致，复用 emitDefaultValues）
    // BUG 6a fix: 方法默认参数值同样递归折叠嵌套一元取反（由 emitDefaultValues 统一实现）
    emitDefaultValues(method);

    functionChunks_[methodKey] = std::move(chunk_);
}

void Compiler::visitMemberAccess(MemberAccess& node) {
    compileNode(node.object.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    // O1: super.field — 字段已在构造时通过继承链复制到实例中，与 this.field 等价
    // 方法调用走 OP_SUPER_CALL，此处仅处理字段读取
    bool isSuperAccess = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
    chunk_.writeOp(isSuperAccess ? OpCode::OP_SUPER_MEMBER_GET : OpCode::OP_MEMBER_GET, node.line);
    chunk_.writeShort(nameIdx, node.line);
    return;
}

void Compiler::visitMemberAssign(MemberAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                         ? static_cast<VarRef*>(node.object.get())
                         : nullptr;

    if (objVar) {
        // 简单路径：obj.field = val（基是 VarRef）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            uint16_t fieldIdx = identifierIndex(node.fieldName);
            chunk_.writeOp(OpCode::OP_MEMBER_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
            chunk_.writeShort(fieldIdx, node.line);
            return;
        }
        compileNode(node.value.get());
        uint16_t varIdx = identifierIndex(objVar->name);
        uint16_t fieldIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
        chunk_.writeShort(varIdx, node.line);
        chunk_.writeShort(fieldIdx, node.line);
        return;
    }

    // M1 fix: 嵌套成员赋值（2 层）
    // 检测 node.object 是否是 IndexAccess(VarRef) 或 MemberAccess(VarRef)
    IndexAccess* outerIdx = (node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS)
                                ? static_cast<IndexAccess*>(node.object.get())
                                : nullptr;
    MemberAccess* outerMem = (node.object && node.object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                                 ? static_cast<MemberAccess*>(node.object.get())
                                 : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());

    if (baseVar) {
        auto localIt = currentLocals_.find(baseVar->name);
        bool isLocal = (localIt != currentLocals_.end());

        // R156 fix: 移除 compileNode(baseVar) —— 不再 push base 变量本身。
        // WRITEBACK_*_LOCAL/VAR 和 MEMBER_SET_LOCAL/VAR / INDEX_SET_LOCAL/VAR 均不需要
        // base 在栈上，原 push base 会导致栈泄漏（每次嵌套赋值泄漏 1 个 Value）。
        // 对齐 visitIndexAssign 的修复模式（见上文 L3470-3471 注释）。
        // 编译外层表达式 → push base[outerIdx] 或 base.field
        compileNode(node.object.get());
        // 编译值 → push val
        compileNode(node.value.get());
        // OP_MEMBER_SET: 弹出 val/outerValue → 修改 → lastMutatedReceiver_
        uint16_t fieldNameIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
        chunk_.writeShort(fieldNameIdx, node.line);
        // 将变异后的内层容器写回基变量。
        // 对齐 IR 路径（IR.cpp emitNestedAssignWriteback L2696-2708）与 visitIndexAssign 修复模式。
        // R156 fix: 原 outerIdx 路径用 WRITEBACK_INDEX_LOCAL/VAR 整体替换 base 为变异后的 base[outerIdx]
        // （而非 base[outerIdx] = mutated），导致 arr[i].field=val 在非 IR 路径失败。
        // 原 outerMem 路径用 WRITEBACK_MEMBER_LOCAL/VAR 整体替换 base 为变异后的 base.field
        // （而非 base.field = mutated），导致 obj.field.field=val 在非 IR 路径失败。
        // 两条路径均改用 LOAD_MUTATED + INDEX_SET_LOCAL/VAR 或 MEMBER_SET_LOCAL/VAR 直接原地修改。
        if (isLocal) {
            if (outerIdx) {
                // base[outerIdx] = mutated → 重新求值 outerIdx + LOAD_MUTATED + INDEX_SET_LOCAL
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
            } else {
                // base.field = mutated → LOAD_MUTATED + MEMBER_SET_LOCAL
                uint16_t outerFieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET_LOCAL, node.line);
                chunk_.write(static_cast<uint8_t>(localIt->second), node.line);
                chunk_.writeShort(outerFieldIdx, node.line);
            }
        } else {
            if (outerIdx) {
                // base[outerIdx] = mutated → 重新求值 outerIdx + LOAD_MUTATED + INDEX_SET_VAR
                uint16_t nameIdx = identifierIndex(baseVar->name);
                compileNode(outerIdx->index.get());
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
                chunk_.writeShort(nameIdx, node.line);
            } else {
                // base.field = mutated → LOAD_MUTATED + MEMBER_SET_VAR
                uint16_t varIdx = identifierIndex(baseVar->name);
                uint16_t outerFieldIdx = identifierIndex(outerMem->fieldName);
                chunk_.writeOp(OpCode::OP_LOAD_MUTATED, node.line);
                chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
                chunk_.writeShort(varIdx, node.line);
                chunk_.writeShort(outerFieldIdx, node.line);
            }
        }
        return;
    }

    // C-P2-2 fix: 3+ 层嵌套或复杂表达式：不支持，报错而非静默丢失修改
    error("不支持 3 层及以上嵌套成员赋值（如 a.b.c.d = e）", node.line, node.column);
    compileNode(node.object.get());
    compileNode(node.value.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
    chunk_.writeShort(nameIdx, node.line);
    return;
}

void Compiler::visitMethodCall(MethodCall& node) {
    // C-P2-11 fix: 参数数量检查移到最前面，避免字节码已发射后报错导致 __wb_idx_ 缓存变量泄漏
    if (node.arguments.size() > 255) {
        error("方法调用参数数量超过限制（最大 255 个）", node.line, 0);
        return;
    }
    // 检查接收者是否为简单变量（VarRef），用于 writeBack
    uint16_t receiverVarIdx = 0xFFFF; // 0xFFFF = 无全局变量 writeBack
    uint8_t receiverLocalSlot = 0xFF; // 0xFF = 无局部变量 writeBack
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                         ? static_cast<VarRef*>(node.object.get())
                         : nullptr;
    if (objVar) {
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt == currentLocals_.end()) {
            // 全局变量：记录变量名索引供 VM writeBack 使用
            receiverVarIdx = identifierIndex(objVar->name);
        } else {
            // 局部变量：记录 slot 号供 VM writeBack 使用
            receiverLocalSlot = static_cast<uint8_t>(localIt->second);
        }
    }

    // H4 fix: super.method() 的接收者是 this（始终在 slot 0），设置写回目标
    if (!objVar && node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR && inFunction_) {
        receiverLocalSlot = 0; // slot 0 = this
    }

    // B6 fix: 如果接收者是 IndexAccess，预缓存索引值避免写回时重复求值
    // （对 arr[expr()].method() 这类调用，expr() 只执行一次）
    std::string cachedIndexVar;
    if (!objVar && node.object && node.object->nodeType == NodeType::NODE_INDEX_ACCESS) {
        auto* ia = static_cast<IndexAccess*>(node.object.get());
        if (ia->object && ia->object->nodeType == NodeType::NODE_VAR_REF && ia->index) {
            cachedIndexVar = "__wb_idx_" + std::to_string(writebackCounter_++);
            compileNode(ia->index.get());
            uint16_t cacheIdx = identifierIndex(cachedIndexVar);
            chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line); // #9 fix: DEFINE 而非 SET（首次创建变量）
            chunk_.writeShort(cacheIdx, node.line);
        }
    }

    // B6 fix: 如果索引已缓存，手动内联 IndexAccess 编译（用缓存值代替重新求值）
    if (!cachedIndexVar.empty()) {
        auto* ia = static_cast<IndexAccess*>(node.object.get());
        compileNode(ia->object.get()); // push base (e.g., arr)
        uint16_t cacheIdx = identifierIndex(cachedIndexVar);
        chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
        chunk_.writeShort(cacheIdx, node.line);          // push cached index
        chunk_.writeOp(OpCode::OP_INDEX_GET, node.line); // base[cachedIndex]
    } else {
        compileNode(node.object.get());
    }
    // C-P2-1 fix: 方法调用参数数量检查（已前移到函数开头，C-P2-11 fix 避免缓存变量泄漏）
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }
    uint16_t nameIdx = identifierIndex(node.methodName);
    // O1: super.method() 使用 OP_SUPER_CALL（从父类开始方法查找）
    bool isSuperCall = (node.object && node.object->nodeType == NodeType::NODE_SUPER_EXPR);
    chunk_.writeOp(isSuperCall ? OpCode::OP_SUPER_CALL : OpCode::OP_METHOD_CALL, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    chunk_.writeShort(receiverVarIdx, node.line); // 接收者全局变量名索引（0xFFFF = 无全局 writeBack）
    chunk_.write(receiverLocalSlot, node.line);   // 接收者局部变量 slot（0xFF = 无局部 writeBack）
    if (isSuperCall) {
        // B1 fix: 编码当前类名索引，VM 用它查找父类（而非运行时实例类名）
        uint16_t classIdx = identifierIndex(currentClassName_);
        chunk_.writeShort(classIdx, node.line);
    }

    // 嵌套访问变异方法写回：当接收者是 MemberAccess/IndexAccess 且基对象是 VarRef 时，
    // 方法调用后发射写回指令，确保 this.arr.push(42) 等嵌套调用的修改不丢失。
    // VM 在变异方法调用时将修改后的对象暂存到 lastMutatedReceiver_，
    // 写回指令从中取值写回基对象的字段/索引位置。
    // 若接收者是 VarRef（objVar != nullptr），helper 内部 MemberAccess/IndexAccess
    // 类型检查均不匹配，等价于 no-op（原 if (!objVar && node.object) 守卫的等价简化）。
    emitMethodCallWriteback(node.object.get(), node.line);

    // #9 fix: 清理 __wb_idx_ 缓存变量，防止永久泄漏到 globals_
    if (!cachedIndexVar.empty()) {
        uint16_t cacheIdx = identifierIndex(cachedIndexVar);
        chunk_.writeOp(OpCode::OP_DELETE_VAR, node.line);
        chunk_.writeShort(cacheIdx, node.line);
    }
    return;
}

void Compiler::emitMethodCallWriteback(const ASTNode* receiver, int line) {
    // visitMethodCall 子阶段：嵌套访问变异方法写回
    // 当接收者是 MemberAccess/IndexAccess 且基对象是 VarRef 时，发射写回指令，
    // 确保 this.arr.push(42) 等嵌套调用的修改不丢失。VM 在变异方法调用时将修改后的
    // 对象暂存到 lastMutatedReceiver_，写回指令从中取值写回基对象的字段/索引位置。
    // 仅依赖 receiver AST 与 currentLocals_（成员），无其他外部状态。
    // 若 receiver 为 nullptr / VarRef / SuperExpr 等非 MemberAccess/IndexAccess 类型，
    // 内部类型检查均不匹配，函数为 no-op。
    if (!receiver)
        return;
    if (receiver->nodeType == NodeType::NODE_MEMBER_ACCESS) {
        // MemberAccess 嵌套写回：如 this.arr.push(42)、obj.field.pop()
        auto* ma = static_cast<const MemberAccess*>(receiver);
        if (ma->object && ma->object->nodeType == NodeType::NODE_VAR_REF) {
            auto* baseVar = static_cast<VarRef*>(ma->object.get());
            uint16_t fieldIdx = identifierIndex(ma->fieldName);
            auto localIt = currentLocals_.find(baseVar->name);
            if (localIt != currentLocals_.end()) {
                // 局部变量成员写回：OP_WRITEBACK_MEMBER_LOCAL(slot, fieldIdx)
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_LOCAL, line);
                chunk_.write(static_cast<uint8_t>(localIt->second), line);
                chunk_.writeShort(fieldIdx, line);
            } else {
                // 全局变量成员写回：OP_WRITEBACK_MEMBER_VAR(varIdx, fieldIdx)
                chunk_.writeOp(OpCode::OP_WRITEBACK_MEMBER_VAR, line);
                chunk_.writeShort(identifierIndex(baseVar->name), line);
                chunk_.writeShort(fieldIdx, line);
            }
        }
    } else if (receiver->nodeType == NodeType::NODE_INDEX_ACCESS) {
        // IndexAccess 嵌套写回：如 arr[0].push(42)、dict["key"].remove("x")
        // B6 fix: 使用预缓存的索引值，避免重复求值（对 arr[expr()].method() 防止副作用执行两次）
        auto* ia = static_cast<const IndexAccess*>(receiver);
        if (ia->object && ia->object->nodeType == NodeType::NODE_VAR_REF) {
            auto* baseVar = static_cast<VarRef*>(ia->object.get());
            auto localIt = currentLocals_.find(baseVar->name);
            // BUGFIX-P1 fix: WRITEBACK_INDEX handler 已改为整体替换语义，不再 pop 索引，
            // 因此不再向栈推入索引值（原实现每次泄漏 1 个栈值）。
            if (localIt != currentLocals_.end()) {
                // 局部变量索引写回：OP_WRITEBACK_INDEX_LOCAL(slot)
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_LOCAL, line);
                chunk_.write(static_cast<uint8_t>(localIt->second), line);
            } else {
                // 全局变量索引写回：OP_WRITEBACK_INDEX_VAR(varIdx)
                chunk_.writeOp(OpCode::OP_WRITEBACK_INDEX_VAR, line);
                chunk_.writeShort(identifierIndex(baseVar->name), line);
            }
        }
    }
}

void Compiler::visitNullLiteral(NullLiteral& node) {
    chunk_.writeOp(OpCode::OP_NULL, node.line);
    return;
}

void Compiler::visitSuperExpr(SuperExpr& node) {
    // 注：super 在非方法上下文中的错误为运行时错误（非编译期）。
    // 直接 Compiler 路径 currentClassName_ 在 visitClassDecl 中设置，方法编译完后恢复。
    // 嵌套函数（FunDecl）内的 super 调用是合法的——visitFunDecl 入口保存 currentClassName_
    // 后不会清除，方法体内嵌套函数仍能继承外层方法名作为 super 上下文。
    // 顶层或非方法体内访问 super 时 currentClassName_ 为空。
    //
    // BUG-INH-AUDIT-6 fix: 原实现无条件 emit OP_GET_LOCAL 0，在顶层运行时
    // 报"内部错误: 局部变量槽越界 (slot 0)"（误导为 VM bug）。现改为：
    // - 方法上下文（currentClassName_ 非空）：emit OP_GET_LOCAL 0（this 槽）
    // - 非方法上下文（currentClassName_ 为空）：emit OP_GET_VAR "this"
    //   VM 在 GLOBAL_NAME 查找失败时报 "未定义的变量: this"，与 IR 路径对齐。
    // 此运行时错误不可被 try/catch 捕获（在 try 块进入前即触发）。
    // 顶层 / 普通函数内 super 的运行时错误行为由 ConsistencyDiff.H7b 与
    // AuditSuper_RuntimeErrorNotCatchableByTryCatch 测试覆盖。
    // 注：Interpreter 报 "super 只能在类方法中使用"（更友好），VM 路径报
    // "未定义的变量: this"——此 2-way 差异文档化为已知，由 H7b 测试 EXPECT_NE 覆盖。
    if (currentClassName_.empty()) {
        // 非方法上下文：emit OP_GET_VAR "this" → 运行时报 "未定义的变量: this"
        uint16_t nameIdx = chunk_.addConstant(Value(std::string("this")));
        chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
        chunk_.write(static_cast<uint8_t>(nameIdx & 0xFF), node.line);
        chunk_.write(static_cast<uint8_t>((nameIdx >> 8) & 0xFF), node.line);
    } else {
        // 方法上下文：emit OP_GET_LOCAL 0（this 槽）
        chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
        chunk_.write(0, node.line); // slot 0 = this
    }
    return;
}

void Compiler::error(const std::string& msg, int line, int col) {
    diagnostics_.addError(msg, line, col, DiagSource::Compiler);
}

// ---- 常量折叠 ----

void Compiler::emitConstant(const Value& val, int line) {
    if (val.isInt()) {
        uint16_t idx = chunk_.addConstant(val);
        chunk_.writeOp(OpCode::OP_INT, line);
        chunk_.writeShort(idx, line);
    } else if (val.isFloat()) {
        uint16_t idx = chunk_.addConstant(val);
        chunk_.writeOp(OpCode::OP_FLOAT, line);
        chunk_.writeShort(idx, line);
    } else if (val.isString()) {
        uint16_t idx = chunk_.addConstant(val);
        chunk_.writeOp(OpCode::OP_STRING, line);
        chunk_.writeShort(idx, line);
    } else if (val.isBool()) {
        chunk_.writeOp(val.boolVal() ? OpCode::OP_TRUE : OpCode::OP_FALSE, line);
    } else {
        chunk_.writeOp(OpCode::OP_NULL, line);
    }
}

bool Compiler::tryFoldBinary(BinOpType opType, ASTNode* left, ASTNode* right, Value& result, int line) {
    // D8 fix: 使用 extractConstant 递归提取常量值，支持嵌套常量表达式（如 (1+2)*3）
    Value lv, rv;
    if (!extractConstant(left, lv, line) || !extractConstant(right, rv, line)) {
        return false;
    }

    // 数值运算
    if (lv.isNumber() && rv.isNumber()) {
        // 类型提升：int+float → float
        bool useFloat = lv.isFloat() || rv.isFloat();
        double ld = useFloat ? (lv.isFloat() ? lv.floatVal() : static_cast<double>(lv.intVal())) : 0;
        double rd = useFloat ? (rv.isFloat() ? rv.floatVal() : static_cast<double>(rv.intVal())) : 0;
        int64_t li = lv.isInt() ? lv.intVal() : 0;
        int64_t ri = rv.isInt() ? rv.intVal() : 0;

        switch (opType) {
        case BinOpType::BIN_ADD:
            if (!useFloat) {
                // B6 fix: 统一使用 OverflowCheck
                if (OverflowCheck::addOverflow(li, ri))
                    return false;
                result = Value(li + ri);
            } else {
                result = Value(ld + rd);
            }
            return true;
        case BinOpType::BIN_SUB:
            if (!useFloat) {
                if (OverflowCheck::subOverflow(li, ri))
                    return false;
                result = Value(li - ri);
            } else {
                result = Value(ld - rd);
            }
            return true;
        case BinOpType::BIN_MUL:
            if (!useFloat) {
                if (OverflowCheck::mulOverflow(li, ri))
                    return false;
                result = Value(li * ri);
            } else {
                result = Value(ld * rd);
            }
            return true;
        case BinOpType::BIN_DIV: {
            double divisor = useFloat ? rd : static_cast<double>(ri);
            if (divisor == 0)
                return false; // 除零不折叠，保留运行时错误
            // B3 fix: INT64_MIN / -1 = 溢出 UB，不折叠
            if (!useFloat && OverflowCheck::divOverflow(li, ri))
                return false;
            result = useFloat ? Value(ld / rd) : Value(li / ri);
            return true;
        }
        case BinOpType::BIN_MOD:
            if (!useFloat && ri == 0)
                return false;
            // B3 fix: INT64_MIN % -1 = 溢出 UB，不折叠
            if (!useFloat && OverflowCheck::modOverflow(li, ri))
                return false;
            if (useFloat)
                return false;
            result = Value(li % ri);
            return true;
        case BinOpType::BIN_EQ:
            result = Value(useFloat ? (ld == rd) : (li == ri));
            return true;
        case BinOpType::BIN_NEQ:
            result = Value(useFloat ? (ld != rd) : (li != ri));
            return true;
        case BinOpType::BIN_LT:
            result = Value(useFloat ? (ld < rd) : (li < ri));
            return true;
        case BinOpType::BIN_GT:
            result = Value(useFloat ? (ld > rd) : (li > ri));
            return true;
        case BinOpType::BIN_LTE:
            result = Value(useFloat ? (ld <= rd) : (li <= ri));
            return true;
        case BinOpType::BIN_GTE:
            result = Value(useFloat ? (ld >= rd) : (li >= ri));
            return true;
        default:
            break;
        }
    }

    // 字符串拼接
    if (lv.isString() && rv.isString() && opType == BinOpType::BIN_ADD) {
        result = Value(lv.stringVal() + rv.stringVal());
        return true;
    }

    // 字符串比较
    if (lv.isString() && rv.isString()) {
        // PERF-30 fix: 补全字符串字典序比较折叠（<, >, <=, >=）
        // 原 BIN_EQ/BIN_NEQ 已支持，此处补齐 4 个关系运算符
        const std::string& ls = lv.stringVal();
        const std::string& rs = rv.stringVal();
        switch (opType) {
        case BinOpType::BIN_EQ:
            result = Value(ls == rs);
            return true;
        case BinOpType::BIN_NEQ:
            result = Value(ls != rs);
            return true;
        case BinOpType::BIN_LT:
            result = Value(ls < rs);
            return true;
        case BinOpType::BIN_GT:
            result = Value(ls > rs);
            return true;
        case BinOpType::BIN_LTE:
            result = Value(ls <= rs);
            return true;
        case BinOpType::BIN_GTE:
            result = Value(ls >= rs);
            return true;
        default:
            break;
        }
    }

    // 布尔逻辑（M1 fix: 短路语义，返回操作数原始值而非 bool）
    if (lv.isBool() && rv.isBool()) {
        if (opType == BinOpType::BIN_AND) {
            result = lv.isTruthy() ? rv : lv;
            return true;
        }
        if (opType == BinOpType::BIN_OR) {
            result = lv.isTruthy() ? lv : rv;
            return true;
        }
        if (opType == BinOpType::BIN_EQ) {
            result = Value(lv.boolVal() == rv.boolVal());
            return true;
        }
        if (opType == BinOpType::BIN_NEQ) {
            result = Value(lv.boolVal() != rv.boolVal());
            return true;
        }
    }

    return false;
}

bool Compiler::tryFoldUnary(UnaryOp::UnaryOpType opType, ASTNode* operand, Value& result, int line) {
    if (!operand)
        return false;

    // D8 fix: 使用 extractConstant 递归提取常量值，支持嵌套表达式（如 -(3+2)）
    Value val;
    if (!extractConstant(operand, val, line))
        return false;

    if (opType == UnaryOp::UnaryOpType::UOP_NEGATE) {
        if (val.isInt()) {
            // B3 fix: -INT64_MIN = 溢出 UB，不折叠
            if (OverflowCheck::negateOverflow(val.intVal()))
                return false;
            result = Value(-val.intVal());
            return true;
        }
        if (val.isFloat()) {
            result = Value(-val.floatVal());
            return true;
        }
    }
    if (opType == UnaryOp::UnaryOpType::UOP_NOT && val.isBool()) {
        result = Value(!val.boolVal());
        return true;
    }

    return false;
}

// D8 fix: 递归提取常量值。支持字面量 + 嵌套 BinaryOp/UnaryOp 常量表达式，
// 使 tryFoldBinary/tryFoldUnary 能折叠 (1+2)*3、-(-(5)) 等嵌套表达式。
bool Compiler::extractConstant(ASTNode* node, Value& result, int line) {
    if (!node)
        return false;

    switch (node->nodeType) {
    case NodeType::NODE_NUMBER_LITERAL:
        result = static_cast<NumberLiteral*>(node)->getValue(); // A1 fix: getValue()
        return true;
    case NodeType::NODE_STRING_LITERAL:
        result = static_cast<StringLiteral*>(node)->getValue(); // A1 fix: getValue()
        return true;
    case NodeType::NODE_BOOL_LITERAL:
        result = static_cast<BoolLiteral*>(node)->getValue(); // A1 fix: getValue()
        return true;
    case NodeType::NODE_NULL_LITERAL:
        result = Value::nullValue();
        return true;
    case NodeType::NODE_BINARY_OP: {
        auto* bin = static_cast<BinaryOp*>(node);
        return tryFoldBinary(bin->opType, bin->left.get(), bin->right.get(), result, line);
    }
    case NodeType::NODE_UNARY_OP: {
        auto* un = static_cast<UnaryOp*>(node);
        return tryFoldUnary(un->opType, un->operand.get(), result, line);
    }
    default:
        return false;
    }
}
