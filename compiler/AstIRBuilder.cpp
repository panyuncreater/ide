#include "ast/ASTNode.h"
#include "ast/ModuleIsolation.h" // BUG-AUDIT-MOD-2: IR 模块隔离（非导出顶层名前缀化）
#include "common/Logger.h"
#include "common/ModulePath.h"      // AUDIT-R5 R6 fix: 模块路径缓存键规范化单一事实源
#include "common/RuntimeLimits.h"   // BUG-AUDIT-MOD-3: MAX_RECURSION_DEPTH
#include "common/TCO.h"             // R109 TCO: isTailRecursiveReturn（与 Compiler.cpp 共享识别逻辑）
#include "common/TypeChecker.h"     // R163 泛型扩展: isTypeParameter（emitTypeCheckIR 擦除）
#include "compiler/BytecodeCache.h" // L11: 预编译模块 .minic 加载
#include "compiler/ExprStmtPop.h"   // AUDIT-R4 BUG-04: 表达式语句 POP 共享谓词
#include "compiler/IR.h"
#include "interpreter/NumericUtils.h" // #14: OverflowCheck
#include "interpreter/Value.h"
#include "lexer/Lexer.h"   // VM-IMPORT: 模块源码词法分析
#include "parser/Parser.h" // VM-IMPORT: 模块源码语法分析
#include <algorithm>       // P2-11: std::sort for namespace import
#include <cassert>
#include <cmath>
#include <filesystem> // L11: 源码路径推导
#include <functional> // 穷尽性检查: std::function 递归 lambda
#include <sstream>
#include <unordered_set>

// ============================================================
// IR 中间表示层实现（ARCH-06 完整实现）
// ============================================================
// 本文件实现三层：
//   1. AstIRBuilder：AST → IR（Visitor 模式，覆盖全部核心 AST 节点）
//   2. BytecodeIRBackend：IR → VM 字节码（lowering）
//   3. IRToString：调试输出
//
// 已补齐 5 个已知限制：
//   1. 闭包 upvalue 捕获（MAKE_CLOSURE 编码 upvalue 描述符列表）
//   2. 写回指令（WRITEBACK_MEMBER_VAR/LOCAL, WRITEBACK_INDEX_VAR/LOCAL）
//   3. 全局槽位分配（OP_GET_GLOBAL/OP_SET_GLOBAL/OP_DEFINE_GLOBAL 槽位版）
//   4. 默认参数值（defaultConstIndices 记录常量索引）
//   5. 块作用域区分（enterBlockScope/leaveBlockScope）
// ============================================================

namespace {

/// 简化版常量提取（限制4）：支持字面量和负数字面量
/// 成功返回 true 并输出常量值；失败返回 false
bool extractConstant(ASTNode* node, Value& result) {
    if (!node)
        return false;
    switch (node->nodeType) {
    case NodeType::NODE_NUMBER_LITERAL:
        result = static_cast<NumberLiteral*>(node)->getValue();
        return true;
    case NodeType::NODE_STRING_LITERAL:
        result = static_cast<StringLiteral*>(node)->getValue();
        return true;
    case NodeType::NODE_BOOL_LITERAL:
        result = static_cast<BoolLiteral*>(node)->getValue();
        return true;
    case NodeType::NODE_NULL_LITERAL:
        result = Value::nullValue();
        return true;
    case NodeType::NODE_UNARY_OP: {
        // 仅支持 NEGATE 作用于数字字面量
        auto* un = static_cast<UnaryOp*>(node);
        if (un->opType == UnaryOp::UnaryOpType::UOP_NEGATE) {
            Value operand;
            if (extractConstant(un->operand.get(), operand) && operand.isNumber()) {
                if (operand.isInt()) {
                    // R7 fix: -INT64_MIN 是有符号整数溢出（C++ UB），对齐
                    // Compiler::tryFoldUnary 和 IR foldUnary 用 OverflowCheck 防护。
                    if (OverflowCheck::negateOverflow(operand.intVal())) {
                        return false;
                    }
                    result = Value(-operand.intVal());
                } else {
                    result = Value(-operand.floatVal());
                }
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

/// 表达式语句是否需要 emit POP：
/// AUDIT-R4 BUG-04 fix: 本地实现已提取到共享头文件 compiler/ExprStmtPop.h，
/// 与 Compiler::compileStatement（栈式直接路径）共用同一谓词，消除双份
/// 维护漂移（历史上 R134 仅在本路径补入 3 种节点，栈式直接路径遗漏）。
/// IR 路径的 BytecodeIRBackend lowering 会将这些节点的 vreg 物化为 StackVM 栈值
/// （LOAD_CONST→OP_INT、ADD→OP_ADD、CALL→OP_CALL 等均压栈），
/// 表达式语句的结果不被消费，需 POP 防止栈泄漏。
/// 声明/控制流节点（VarDecl/IfStmt/WhileStmt 等）已自行平衡栈，不需 POP。

} // anonymous namespace

// ============================================================
// AstIRBuilder 实现
// ============================================================

AstIRBuilder::AstIRBuilder() {
    // 初始化 ir_ 为新 IRFunction（main chunk）
    ir_ = std::make_unique<IRFunction>();
    ir_->name = "main";
    // 初始化 module_（收集所有函数）
    module_ = std::make_unique<IRModule>();
    // 创建初始基本块
    uint32_t entryLabel = ir_->allocLabel();
    currentBlock_ = &ir_->addBlock(entryLabel);
    currentBlock_->instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(entryLabel)}, 0);
}

std::unique_ptr<IRFunction> AstIRBuilder::build(Block& program) {
    // 限制3：预扫描顶层声明，分配全局槽位
    // BUG-IR-PRES-1 fix: 主模块不预扫描 FunDecl（对齐 compile() 的有意不对称设计）
    preScanTopLevelDecls(program, true);
    // 遍历顶层语句，逐个转换
    for (auto& stmt : program.statements) {
        if (!stmt)
            continue;
        IROperand stmtResult = visitNode(stmt.get());
        // P2-12 fix (错误恢复): 仅致命错误中止整个编译；可恢复错误记录后继续处理后续语句。
        // 致命错误场景：模块加载失败、内部不变量违反（未支持的 AST 节点/BinOp）等。
        // 可恢复错误场景：变量重定义、循环外 break/continue、enum variant 参数超限等——
        // 这些错误仅影响当前节点，后续兄弟节点可从干净状态继续编译。
        // 原 fail-fast 设计对任何错误立即 return nullptr，导致用户无法一次性看到所有错误。
        if (irDiagnostics_.hasFatalErrors())
            return nullptr;
        // Phase-5 fix: POP 携带所消费的 vreg，使 DCE 看到消费关系，不会误删表达式语句的纯计算指令。
        // AUDIT-R5 BUG-01 fix: 改用节点级重载，覆盖匿名 lambda 表达式语句的闭包值 POP。
        if (needsPopForExprStmt(stmt.get())) {
            emitIR(IROp::POP, {stmtResult}, stmt->line);
        }
    }
    // 将 ir_ 作为 mainFunction 存入 module_
    // 更新 main 的 localCount（顶层代码的 AND/OR 短路等会用 nextLocalSlot_ 分配临时 slot）
    ir_->localCount = static_cast<int>(nextLocalSlot_);
    // P2-1 fix: localCount 超 32 寄存器硬上限会在 RegisterBytecodeBackend.lower()
    // 设置 chunk_->registerCount > 32，导致 RegCallFrame::registers[32+] OOB 访问。
    // 此处早期告警；RegisterBytecodeBackend::lower 会硬失败阻止生成损坏字节码。
    if (ir_->localCount > 32) {
        Logger::Error("AstIRBuilder: localCount 超出 32 寄存器上限 (" + std::to_string(ir_->localCount) + "，函数 " +
                          ir_->name + ")，RegisterVM 将无法加载此函数",
                      "IR");
    }
    // BUG-IDE-12 fix: 保存 main 函数 slot→name 映射（顶层局部变量）
    ir_->localSlotNames = localSlotNames_;
    // L1 fix: 保存 main 函数 IP 范围表
    ir_->slotNameRanges = slotNameRanges_;
    module_->mainFunction = std::move(ir_);
    // 返回 ir_（转移所有权给调用者；module_ 通过 getModule() 仍可访问 functions）
    return std::move(module_->mainFunction);
}

// ---- 全局槽位管理（限制3 / B4: 已内联到 IR.h，委托给 globalSlotAllocator_）----

void AstIRBuilder::preScanTopLevelDecls(Block& program, bool /*isMainModule*/) {
    // 遍历顶层语句，为 VarDecl/ClassDecl/FunDecl/ExportStmt 分配全局槽位
    // BUG-FIX: 原实现漏掉 NODE_FUN_DECL 和 NODE_EXPORT_STMT，导致前向函数引用
    // 和主程序 export 声明在 IR 路径下全局槽位未预分配。对齐 Compiler::preScanModuleGlobals。
    // R98 W2 fix: 原"主模块不预扫描 FunDecl"的 BUG-IR-PRES-1 设计已废弃——
    // IR 路径的 emitFunctionClosureRegistration 现已用 DEFINE_GLOBAL 将闭包值写入
    // globalSlots_[slot]（替代原 POP 丢弃），使函数名可作为值引用
    // （map/filter/reduce/forEach/find 高阶函数的闭包参数路径）。原"var f = funName
    // 静默 null"的顾虑已不适用——slot 现在持有真实闭包值。主模块与模块预扫描行为统一。
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
            auto* exp = static_cast<ExportStmt*>(stmt.get());
            if (exp->declaration) {
                ASTNode* decl = exp->declaration.get();
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

// ============================================================
// VM-IMPORT: 模块系统（IR 路径）
// ============================================================
// 对齐 Compiler::visitImportStmt 的语义：编译期内联模块代码。
// 模块源码被加载、解析为 AST，然后逐条 visitNode 内联到当前 IR 中。
// 全局槽位由 globalSlotAllocator_ 统一分配，模块的顶层声明自然成为主程序的全局变量。

/// handleImportStmt 的子阶段返回值：路径解析后的下一步动作。
/// 类型定义在 IR.h AstIRBuilder 类内（enum class AstIRBuilder::ImportPathStatus）。
/// 旧实现曾在 IR.cpp 文件级定义独立 enum，与类内枚举重复；R98 t9 修复声明遗漏时
/// 统一到类内枚举，避免全局命名空间污染。

/// 1. 路径规范化与安全校验（对齐 Compiler::normalizeModulePath）
///    + run-once 检查（已加载则返回 kAlreadyLoaded）
///    + 循环导入延迟加载（P2-14：返回 kCircularLoading）
///    + 模块加载深度保护（BUG-AUDIT-MOD-3）
/// 成功时 outPath 填入规范化路径，返回 kContinue / kAlreadyLoaded；
/// 失败时设置 hasError_/errorMessage_/errorLine_ 并返回 kError。
AstIRBuilder::ImportPathStatus AstIRBuilder::resolveImportPath(ImportStmt& node, std::string& outLoaderPath,
                                                               std::string& outCacheKey) {
    // BUG-AUDIT-MOD-2 fix: 模块隔离（AST 重写 + 作用域分析）
    // -----------------------------------------------------------
    // VM/IR 编译期模块内联——加载模块源码、解析 AST、IR lowering 模块语句。
    // 模块代码在编译期被"展开"到主程序 IR 中，VM/RegisterVM 运行时无需模块加载机制。
    //
    // 在 IR lowering 前对模块 AST 调用 ModuleTopLevelRenamer::rename，将模块的
    // 非导出顶层声明名前缀化为 `__mod_<hash>__<name>`，并递归重写模块内引用。
    // 导入方无法用原名访问模块的非导出名，与 Interpreter 的模块隔离语义对齐。
    // - run-once: 同一模块多次 import 时仅编译/lowering 一次（linkedModuleSet_ 保证）
    // - 安全保障: export 标记检查（BUG-AUDIT-MOD-1）+ 深度限制（BUG-AUDIT-MOD-3）
    //   + AST 隔离（BUG-AUDIT-MOD-2，本处实施）

    // AUDIT-R5 R6/BUG-M4 fix: 路径规范化用 normalizeModulePathKey（保留大小写，供 loader），
    // 去重键用 moduleCacheKey（Windows 小写折叠），使同一文件的不同大小写拼写去重为同一模块。
    std::string path = normalizeModulePathKey(node.modulePath);
    if (path.empty()) {
        irDiagnostics_.addErrorFatal("模块路径不能为空", node.line, 0, DiagSource::Compiler);
        return ImportPathStatus::kError;
    }
    // BUG-MOD-1 fix: 与 Compiler::normalizeModulePath 保持一致——拒绝所有 'X:' 开头形式，
    // 覆盖 'C:/'、'C:foo'、'D:path' 等 Windows 驱动器路径。
    if (path[0] == '/' || (path.size() >= 2 && path[1] == ':')) {
        irDiagnostics_.addErrorFatal("模块路径不能为绝对路径: " + path, node.line, 0, DiagSource::Compiler);
        return ImportPathStatus::kError;
    }
    // ".." 路径段检测
    for (size_t pos = 0; pos < path.size();) {
        size_t next = path.find('/', pos);
        std::string seg = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        if (seg == "..") {
            irDiagnostics_.addErrorFatal("模块路径不能包含父目录引用 '..': " + path, node.line, 0,
                                         DiagSource::Compiler);
            return ImportPathStatus::kError;
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    // BUG-M4 fix: 安全校验完成后拆分为 loaderPath（大小写保留）与 cacheKey（去重键）。
    outLoaderPath = path;
    const std::string cacheKey = moduleCacheKey(path);

    // run-once 检查：已加载模块不再重新加载，仅校验具名导入
    if (linkedModuleSet_.count(cacheKey)) {
        // BUG-AUDIT-MOD-1 fix: 已编译模块的具名导入验证也检查 export 集合（对齐 Interpreter）
        if (!node.importAll && !node.names.empty()) {
            auto expIt = moduleExports_.find(cacheKey);
            if (expIt != moduleExports_.end()) {
                for (const auto& name : node.names) {
                    if (expIt->second.find(name) == expIt->second.end()) {
                        irDiagnostics_.addErrorFatal("模块 " + path + " 中未导出名称: " + name, node.line, 0,
                                                     DiagSource::Compiler);
                        return ImportPathStatus::kError;
                    }
                }
            }
        }
        outCacheKey = cacheKey;
        return ImportPathStatus::kAlreadyLoaded;
    }

    // P2-14 循环导入延迟加载：不报错，返回 kCircularLoading 让 handleImportStmt 跳过本次内联编译
    // 模块的全局槽位已由首次加载路径的 preScanTopLevelDecls 预扫描分配（值为 null），
    // moduleExports_ 已由首次加载路径的第 5 步填充。首次加载路径会继续内联编译模块语句，
    // 填充全局槽位。循环回路闭合时，访问未初始化的导出名运行时读到 null。
    if (moduleLoadingSet_.count(cacheKey)) {
        outCacheKey = cacheKey;
        return ImportPathStatus::kCircularLoading;
    }

    // BUG-AUDIT-MOD-3 fix: 模块加载深度保护（对齐 InterpreterModules.cpp:76-78）
    if (moduleLoadingStack_.size() >= RuntimeLimits::MAX_RECURSION_DEPTH) {
        irDiagnostics_.addErrorFatal("模块导入深度超过限制 (" + std::to_string(RuntimeLimits::MAX_RECURSION_DEPTH) +
                                         ")",
                                     node.line, 0, DiagSource::Compiler);
        return ImportPathStatus::kError;
    }

    outCacheKey = cacheKey;
    return ImportPathStatus::kContinue;
}

/// 2. 加载模块源码 + 解析为 AST + 模块隔离重命名。
/// 成功时 outAst 持有重命名后的模块 AST；失败时设置 hasError_ 并返回 false。
bool AstIRBuilder::loadImportedModule(ImportStmt& node, const std::string& loaderPath, const std::string& cacheKey,
                                      std::unique_ptr<Block>& outAst) {
    // 检查模块加载器
    if (!moduleLoader_) {
        irDiagnostics_.addErrorFatal("VM 编译需要模块加载器（moduleLoader 未设置）", node.line, 0,
                                     DiagSource::Compiler);
        return false;
    }

    // 加载源码（BUG-M4 fix: 用 loaderPath 保留大小写，供大小写敏感的 loader 正确解析）
    std::string source = moduleLoader_(loaderPath);
    if (source.empty()) {
        irDiagnostics_.addErrorFatal("无法加载模块: " + loaderPath + "（文件不存在或为空）", node.line, 0,
                                     DiagSource::Compiler);
        return false;
    }

    // 解析模块 AST
    Lexer lexer;
    auto tokens = lexer.scan(source);
    // BUG-FIX: 检查词法错误（原实现吞掉 Lexer 错误）
    if (lexer.getDiagnostics().hasErrors()) {
        const auto& diags = lexer.getDiagnostics().all();
        std::string msg = "模块 '" + loaderPath + "' 词法错误";
        if (!diags.empty()) {
            msg += ": " + diags.front().message;
        }
        irDiagnostics_.addErrorFatal(msg, node.line, 0, DiagSource::Compiler);
        return false;
    }
    Parser parser;
    auto parsed = parser.parse(tokens);
    // BUG-FIX: 检查语法错误（原实现仅检查 AST 是否为空）
    if (parser.hasErrors()) {
        const auto& diags = parser.getDiagnostics().all();
        std::string msg = "模块 '" + loaderPath + "' 语法错误";
        if (!diags.empty()) {
            msg += ": " + diags.front().message;
        }
        irDiagnostics_.addErrorFatal(msg, node.line, 0, DiagSource::Compiler);
        return false;
    }
    if (!parsed) {
        irDiagnostics_.addErrorFatal("模块解析失败: " + loaderPath, node.line, 0, DiagSource::Compiler);
        return false;
    }
    outAst = std::move(parsed);

    // BUG-AUDIT-MOD-2 fix: 模块隔离——重命名非导出顶层名为 `__mod_<hash>__<name>`
    // BUG-M4 fix: 用 cacheKey 生成隔离前缀，与缓存去重键一致（Windows 小写折叠）。
    ModuleTopLevelRenamer::rename(*outAst, cacheKey);
    return true;
}

/// 3. 具名导入验证：检查 node.names 中的每个名字是否在模块的 export 集合中。
/// 对齐 Interpreter 的具名导入验证（BUG-AUDIT-MOD-1 fix）。
void AstIRBuilder::bindImportedNames(ImportStmt& node, const std::string& path) {
    // BUG-AUDIT-MOD-1 fix: 具名导入验证改为检查 export 集合（对齐 Interpreter）
    if (!node.importAll && !node.names.empty()) {
        auto expIt = moduleExports_.find(path);
        if (expIt != moduleExports_.end()) {
            for (const auto& name : node.names) {
                if (expIt->second.find(name) == expIt->second.end()) {
                    irDiagnostics_.addError("模块 " + path + " 中未导出名称: " + name, node.line, 0,
                                            DiagSource::Compiler);
                    return;
                }
            }
        }
    }
}

void AstIRBuilder::emitNamespaceImportIR(ImportStmt& node, const std::string& path) {
    // P2-11: import * as ns from "path" — 生成 IR 构造命名空间字典对象
    // 对每个 export 名 emit: LOAD_CONST(key字符串) + LOAD_GLOBAL(value)，
    // 然后 BUILD_DICT + DEFINE_GLOBAL(namespaceAlias)。
    // 导出名排序确保三后端字典 key 顺序一致（对齐 Compiler 路径）。
    if (node.namespaceAlias.empty())
        return;

    auto expIt = moduleExports_.find(path);
    if (expIt == moduleExports_.end() || expIt->second.empty()) {
        // 空模块：emit BUILD_DICT(pairCount=0) + DEFINE_GLOBAL
        IROperand dictVReg = ir_->allocVReg();
        emitIR(IROp::BUILD_DICT, {dictVReg, IROperand::imm(0u)}, node.line);
        // 分配 namespaceAlias 全局槽位并 DEFINE_GLOBAL
        int slot = allocateGlobalSlot(node.namespaceAlias);
        varMap_[node.namespaceAlias] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot)};
        emitIR(IROp::DEFINE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(slot)), dictVReg}, node.line);
        return;
    }

    // 收集导出名并排序（确保三后端字典 key 顺序一致）
    std::vector<std::string> sortedExports(expIt->second.begin(), expIt->second.end());
    std::sort(sortedExports.begin(), sortedExports.end());
    if (sortedExports.size() > 255) {
        irDiagnostics_.addError("命名空间导入的导出数量超过 255 上限", node.line, 0, DiagSource::Compiler);
        return;
    }

    // 逐个 emit LOAD_CONST(key) + LOAD_GLOBAL(value)
    std::vector<IROperand> ops;
    IROperand dictVReg = ir_->allocVReg();
    ops.push_back(dictVReg);
    ops.push_back(IROperand::imm(static_cast<uint32_t>(sortedExports.size())));
    for (const auto& name : sortedExports) {
        // key: 字符串常量
        ops.push_back(emitConst(Value(name), node.line));
        // value: 从全局槽位加载（export 名在模块内联编译后已分配全局槽位）
        // AUDIT-R5 R5 fix: 优先从记录的模块导出槽位直接 LOAD_GLOBAL（detach 后 emitLoadVar 解析失效），
        // 未记录时回退到 emitLoadVar（循环导入等未 detach 场景）。
        int nsSlot = -1;
        if (auto msIt = moduleExportSlots_.find(path); msIt != moduleExportSlots_.end()) {
            if (auto sIt = msIt->second.find(name); sIt != msIt->second.end())
                nsSlot = sIt->second;
        }
        if (nsSlot >= 0) {
            IROperand valReg = ir_->allocVReg();
            emitIR(IROp::LOAD_GLOBAL, {valReg, IROperand::imm(static_cast<uint32_t>(nsSlot))}, node.line);
            ops.push_back(valReg);
        } else {
            ops.push_back(emitLoadVar(name, node.line));
        }
    }
    emitIR(IROp::BUILD_DICT, ops, node.line);

    // DEFINE_GLOBAL: 将字典存入 namespaceAlias 全局槽位
    int slot = allocateGlobalSlot(node.namespaceAlias);
    varMap_[node.namespaceAlias] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot)};
    emitIR(IROp::DEFINE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(slot)), dictVReg}, node.line);
}

// ============================================================
// AUDIT-R5 R5 fix：可变导出量导入快照语义（IR/RegVM 路径，与 Compiler 同策略）
// ------------------------------------------------------------
// 模块内联 lowering 后记录导出名的模块槽位并 detach 名称，导入方另分配新槽位
// 并在导入点 emit LOAD_GLOBAL(模块槽)→DEFINE_GLOBAL(新槽) 拷贝快照，同时更新 varMap_
// 使导入方后续引用解析到新槽位。模块函数在内联期 lowering（捕获模块槽），
// 故实际可观差异仅对可变变量生效（与 Interpreter 快照语义一致）。
// ============================================================

void AstIRBuilder::recordModuleExportSlots(const std::string& path) {
    // AUDIT-R5 R5 fix: 仅记录导出“可变变量”的槽位（函数/类导出不快照，保持名称解析）。
    auto varIt = moduleExportVars_.find(path);
    if (varIt == moduleExportVars_.end())
        return;
    auto& slotMap = moduleExportSlots_[path];
    // AUDIT-R6 F6 fix: 先清空本模块旧条目再记录（与 Compiler 对齐；防残留旧槽号）。
    slotMap.clear();
    for (const auto& name : varIt->second) {
        int slot = lookupGlobalSlot(name);
        if (slot >= 0)
            slotMap[name] = slot;
    }
}

void AstIRBuilder::detachModuleExportSlots(const std::string& path) {
    auto slotIt = moduleExportSlots_.find(path);
    if (slotIt == moduleExportSlots_.end())
        return;
    for (const auto& kv : slotIt->second) {
        globalSlotAllocator_.detachName(kv.first, "__modexp_" + std::to_string(kv.second) + "_" + kv.first);
    }
}

void AstIRBuilder::emitImportSnapshotCopyIR(ImportStmt& node, const std::string& path) {
    // namespace 导入由 emitNamespaceImportIR 构造字典快照，此处跳过。
    if (!node.namespaceAlias.empty())
        return;
    auto slotIt = moduleExportSlots_.find(path);
    if (slotIt == moduleExportSlots_.end())
        return;
    const auto& slotMap = slotIt->second;
    auto emitCopy = [&](const std::string& name) {
        auto it = slotMap.find(name);
        if (it == slotMap.end())
            return;
        int moduleSlot = it->second;
        int mainSlot = allocateGlobalSlot(name); // name 已 detach，分配新槽位；循环未 detach 时返回原槽位
        if (mainSlot == moduleSlot)
            return; // 循环导入未 detach：同槽位，跳过自拷贝（保持既有共享行为）
        IROperand valReg = ir_->allocVReg();
        emitIR(IROp::LOAD_GLOBAL, {valReg, IROperand::imm(static_cast<uint32_t>(moduleSlot))}, node.line);
        emitIR(IROp::DEFINE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(mainSlot)), valReg}, node.line);
        // 更新 varMap_：导入方后续引用解析到新槽位（模块函数已在内联期固化为模块槽）。
        varMap_[name] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(mainSlot)};
    };
    if (node.importAll) {
        for (const auto& kv : slotMap)
            emitCopy(kv.first);
    } else {
        for (const auto& name : node.names)
            emitCopy(name);
    }
}

void AstIRBuilder::handleImportStmt(ImportStmt& node) {
    // 模块加载编排：路径解析 → 加载模块 → 预扫描/export 收集 → 内联 lowering → 具名导入绑定。
    // 各阶段失败时设置 hasError_ 并提前返回；嵌套 import 失败需回滚 moduleLoadingSet_/Stack。

    // 七特性 MVP 阶段 6：沙箱拦截——禁用文件 import 时仅放行 std/ 内建模块
    //（IR 路径覆盖 StackVM-IR 与 RegisterVM，与 Interpreter/Compiler 同口径）。
    if (RuntimeLimits::RuntimeConfig::instance().sandboxBlocksImport()) {
        const std::string& p = node.modulePath;
        if (p.rfind("std/", 0) != 0) {
            irDiagnostics_.addErrorFatal("沙箱模式禁止导入文件模块: " + p, node.line, node.column,
                                         DiagSource::Compiler);
            return;
        }
    }

    // 1. 路径解析 + 安全校验 + run-once / 循环导入延迟加载 / 深度保护
    // BUG-M4 fix: loaderPath 保留大小写（供 loader/预编译解析器），cacheKey 为去重键。
    std::string loaderPath;
    std::string cacheKey;
    auto status = resolveImportPath(node, loaderPath, cacheKey);
    if (status == ImportPathStatus::kError) {
        return;
    }
    if (status == ImportPathStatus::kAlreadyLoaded) {
        // 模块已加载，resolveImportPath 已校验具名导入；不再重复绑定
        // AUDIT-R5 R5 fix: run-once 重复导入从已记录的模块槽位重新拷贝快照（对齐 Interpreter 重导入）
        emitImportSnapshotCopyIR(node, cacheKey);
        // P2-11: run-once 路径也需构造命名空间字典（全局槽位已存在，LOAD_GLOBAL 可直接读取）
        emitNamespaceImportIR(node, cacheKey);
        return;
    }
    if (status == ImportPathStatus::kCircularLoading) {
        // P2-14: 循环导入延迟加载——跳过本次内联编译（避免无限递归）
        // 模块的全局槽位已由首次加载路径的 preScanTopLevelDecls 预扫描分配（值为 null），
        // moduleExports_ 已由首次加载路径的第 5 步填充。
        // 具名导入验证：检查 export 集合（对齐 kAlreadyLoaded 路径）
        bindImportedNames(node, cacheKey);
        // AUDIT-R5 R5 fix: 循环导入未 detach，emitImportSnapshotCopyIR 内部因同槽位而跳过自拷贝（保持共享）
        emitImportSnapshotCopyIR(node, cacheKey);
        // P2-11: 循环导入场景也需构造命名空间字典（全局槽位已预扫描分配）
        emitNamespaceImportIR(node, cacheKey);
        return;
    }

    // L11 预编译模块（RegisterVM）：优先尝试加载 MLRC 格式 .minic 文件
    // 成功时记录 pending 加载（Compiler 在 post-lowering 阶段合并字节码），
    // 并在 IR 层 emit CALL 指令调用模块初始化函数。
    // 失败时回退到下方源码编译路径。
    if (precompiledModuleResolver_) {
        std::string minicPath = precompiledModuleResolver_(loaderPath);
        if (!minicPath.empty()) {
            // L12: 推导源码路径（与 .minic 同目录、同 stem、.mini 扩展名）
            std::string sourcePath;
            {
                namespace fs = std::filesystem;
                fs::path minicP(minicPath);
                if (minicP.has_stem()) {
                    fs::path candidate = minicP.parent_path() / (minicP.stem().string() + ".mini");
                    std::error_code ec;
                    if (fs::exists(candidate, ec)) {
                        sourcePath = candidate.string();
                    }
                }
            }

            BytecodeCache cache;
            auto moduleResult = cache.tryLoadRegisterFromFile(minicPath, sourcePath);
            if (moduleResult) {
                // 加载成功：分配全局槽位 + 填充 moduleExports_ + emit CALL init + 记录 pending
                // 生成模块前缀（对齐 Compiler::loadPrecompiledRegisterModule 的哈希算法）
                // BUG-M4 fix: 哈希用 loaderPath，与 pending 存的 loaderPath 及 Compiler 后续哈希一致。
                uint64_t hash = 0xcbf29ce484222325ULL;
                for (unsigned char c : loaderPath) {
                    hash ^= c;
                    hash *= 0x100000001b3ULL;
                }
                std::string modPrefix = "__mod_" + std::to_string(hash) + "_";
                std::string initFnName = modPrefix + "_init";

                // 构建导出名称集合
                std::unordered_set<std::string> exportSet(moduleResult->moduleExports.begin(),
                                                          moduleResult->moduleExports.end());

                // 为模块的全局槽位在主程序分配对应槽位
                for (int i = 0; i < moduleResult->globalSlotCount; ++i) {
                    const std::string& name = moduleResult->globalSlotNames[static_cast<size_t>(i)];
                    std::string mainName = (exportSet.count(name) > 0) ? name : (modPrefix + name);
                    allocateGlobalSlot(mainName);
                }

                // 填充 moduleExports_（供后续具名导入验证和命名空间字典构造）
                std::unordered_set<std::string> exports(moduleResult->moduleExports.begin(),
                                                        moduleResult->moduleExports.end());
                moduleExports_[cacheKey] = std::move(exports);

                // emit IR CALL 调用模块初始化函数（argCount=0，返回值 POP 丢弃）
                // IR CALL: dest = call name(args)
                // RegisterBytecodeBackend lowers to REG_CALL，运行时按名查找 functionChunks_
                IROperand dest = ir_->allocVReg();
                uint32_t nameIdx = ir_->addGlobal(initFnName);
                std::vector<IROperand> callOps;
                callOps.push_back(dest);
                callOps.push_back(IROperand::funcName(nameIdx));
                callOps.push_back(IROperand::imm(0u)); // argCount = 0
                emitIR(IROp::CALL, callOps, node.line);
                // POP 丢弃返回值（import 是语句，不产生值）
                emitIR(IROp::POP, {dest}, node.line);

                // 记录 pending 加载（Compiler post-lowering 阶段调用 loadPrecompiledRegisterModule）
                // BUG-M4 fix: 存 loaderPath（保留大小写），Compiler 据此解析 .minic 并哈希生成同一前缀。
                pendingRegPrecompiledModules_.push_back({loaderPath, initFnName, node.line});

                // 标记为已链接（run-once 语义）
                linkedModuleSet_.insert(cacheKey);

                // AUDIT-R5 R5 fix: 记录导出槽位并 detach，为快照拷贝做准备（预编译路径与源码路径一致）
                recordModuleExportSlots(cacheKey);
                detachModuleExportSlots(cacheKey);

                // 具名导入验证
                bindImportedNames(node, cacheKey);

                // AUDIT-R5 R5 fix: 快照拷贝（.minic 路径也需要）
                emitImportSnapshotCopyIR(node, cacheKey);

                // 命名空间导入 IR 生成
                emitNamespaceImportIR(node, cacheKey);

                LOG_INFO("IR 预编译模块加载成功（pending）: " + loaderPath + " → " + minicPath + " (" +
                             std::to_string(moduleResult->functionChunks.size()) + " 函数chunk)",
                         "IR-RegPrecompiled");
                return; // .minic 加载成功，跳过源码编译
            }
            // .minic 加载失败，回退到源码编译路径
        }
    }

    // 2. 加载源码 + 解析 AST + 模块隔离重命名
    std::unique_ptr<Block> moduleAst;
    if (!loadImportedModule(node, loaderPath, cacheKey, moduleAst)) {
        return;
    }

    // 3. 标记为正在加载（循环检测 + 深度保护）
    moduleLoadingSet_.insert(cacheKey);
    moduleLoadingStack_.push_back(cacheKey);

    // 4. 预扫描模块顶层声明，分配全局槽位
    preScanTopLevelDecls(*moduleAst);
    // 注：preScanTopLevelDecls 已处理 ExportStmt 包装的声明（BUG-FIX 后对齐 Compiler）

    // 5. BUG-AUDIT-MOD-1 fix: 收集模块导出名称（对齐 Compiler::visitImportStmt 第 7.5 步）
    {
        std::unordered_set<std::string> exports;
        std::unordered_set<std::string> exportVars; // AUDIT-R5 R5 fix: 仅 VarDecl 导出（快照适用）
        for (auto& stmt : moduleAst->statements) {
            if (!stmt || stmt->nodeType != NodeType::NODE_EXPORT_STMT)
                continue;
            auto* exp = static_cast<ExportStmt*>(stmt.get());
            if (!exp->declaration)
                continue;
            ASTNode* decl = exp->declaration.get();
            switch (decl->nodeType) {
            case NodeType::NODE_VAR_DECL:
                exports.insert(static_cast<VarDecl*>(decl)->name);
                exportVars.insert(static_cast<VarDecl*>(decl)->name);
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
        moduleExports_[cacheKey] = std::move(exports);
        moduleExportVars_[cacheKey] = std::move(exportVars);
    }
    // AUDIT-R5 R5 fix: 记录导出名的模块槽位（preScan 已分配），供快照拷贝与命名空间字典读取；
    // 也使本模块内联期间的循环导入能命中已记录槽位。
    recordModuleExportSlots(cacheKey);

    // 6. 内联 visitNode 模块语句（递归处理模块自身的 import）
    for (auto& stmt : moduleAst->statements) {
        if (stmt) {
            IROperand stmtResult = visitNode(stmt.get());
            // P2-12 fix (错误恢复): 嵌套模块语句的致命错误中止当前模块加载。
            // 可恢复错误（如模块内变量重定义）不中止模块加载——模块其他部分仍可用。
            // 但模块加载失败本身（路径/解析错误）在前面步骤已用 addErrorFatal 标记。
            if (irDiagnostics_.hasFatalErrors()) {
                moduleLoadingSet_.erase(cacheKey);
                moduleLoadingStack_.pop_back(); // BUG-AUDIT-MOD-3
                return;
            }
            // BUG-IR-POP-1 fix: 表达式语句的返回值需 POP（对齐 build() L137-139 中的逻辑）
            // Phase-5 fix: POP 携带所消费的 vreg，使 DCE 看到消费关系。
            // AUDIT-R5 BUG-01 fix: 改用节点级重载（覆盖匿名 lambda）。
            if (needsPopForExprStmt(stmt.get())) {
                emitIR(IROp::POP, {stmtResult}, stmt->line);
            }
        }
    }

    // 7. 保留模块 AST，从加载集移除
    moduleAsts_.push_back(std::move(moduleAst));
    moduleLoadingSet_.erase(cacheKey);
    moduleLoadingStack_.pop_back(); // BUG-AUDIT-MOD-3
    linkedModuleSet_.insert(cacheKey);

    // AUDIT-R5 R5 fix: detach 导出名（使导入方可另分配新槽位承载快照副本）
    // AUDIT-R6 B4 fix: detach 前重新 record（幂等，先清后填）——模块内联期间的
    // 嵌套同名导入会把名重绑到新槽，内联前记录的槽号过期（与 Compiler 对齐）。
    recordModuleExportSlots(cacheKey);
    detachModuleExportSlots(cacheKey);

    // 8. 具名导入验证
    bindImportedNames(node, cacheKey);

    // AUDIT-R5 R5 fix: 具名/全量导入的快照拷贝（模块槽位 → 导入方新槽位，并更新 varMap_）
    emitImportSnapshotCopyIR(node, cacheKey);

    // 9. P2-11: 命名空间导入 IR 生成（import * as ns from "path"）
    emitNamespaceImportIR(node, cacheKey);
}

// ---- 块作用域（限制5）----

void AstIRBuilder::enterBlockScope() {
    BlockScope bs;
    bs.slotBase = nextLocalSlot_; // 记录进入块时的槽位基址，退出时回收到此
    blockScopes_.push_back(std::move(bs));
    blockDepth_++;
}

void AstIRBuilder::leaveBlockScope() {
    if (blockScopes_.empty())
        return;
    // 取出栈顶 BlockScope
    BlockScope scope = std::move(blockScopes_.back());
    blockScopes_.pop_back();
    if (blockDepth_ > 0)
        blockDepth_--;
    // 在回收前，确保 localCount 捕获了本块的峰值槽位使用。
    // 回收后 nextLocalSlot_ 降低，但已 emit 的字节码仍引用被回收的 slot 编号，
    // VM 帧必须足够大以容纳所有曾使用的 slot。用 max 更新避免回退。
    if (static_cast<int>(nextLocalSlot_) > ir_->localCount) {
        ir_->localCount = static_cast<int>(nextLocalSlot_);
        // P2-1 fix: 同 build()，对 leaveBlockScope 路径的 localCount 更新做上限告警
        if (ir_->localCount > 32) {
            Logger::Error("AstIRBuilder: localCount 超出 32 寄存器上限 (" + std::to_string(ir_->localCount) +
                              "，函数 " + ir_->name + ")，RegisterVM 将无法加载此函数",
                          "IR");
        }
    }
    // B1 fix: 块作用域退出时关闭指向本块局部 slot 的 open upvalues，使闭包捕获
    // 退出时刻的值快照（而非函数返回时的终值）。对齐 Interpreter visitBlock 每次执行
    // 创建独立 blockEnv 的语义。仅函数内、有局部 slot 的块 emit（顶层无 upvalue）。
    // closeUpvaluesFrom(slotBase) 关闭 slot >= slotBase 的全部 open upvalues，
    // 若无匹配 upvalue 则为 no-op，运行时开销 O(log n + k)。
    if (inFunction_ && !scope.localSlots.empty()) {
        emitIR(IROp::CLOSE_UPVALUE, {IROperand::imm(scope.slotBase)}, 0);
        // L1 fix: 回填本块内声明的变量 range 的 endInstr。CLOSE_UPVALUE 已 emit，
        // 当前 ir_ 指令总数即为 endInstr（变量在 [startInstr, endInstr) 范围内有效）。
        size_t endIdx = 0;
        for (const auto& blk : ir_->blocks)
            endIdx += blk.instructions.size();
        for (auto& range : slotNameRanges_) {
            if (range.endInstr == 0 && range.slot >= scope.slotBase) {
                range.endInstr = endIdx;
            }
        }
    }
    // 回收局部变量槽位：仅当本块内未创建嵌套函数（闭包）时才回收。
    // 闭包可能捕获了本块的局部变量 slot（通过 upvalue isLocal=true 指向 slot 编号），
    // 回收会导致后续块复用同一 slot 编号，闭包指向错误数据。
    // hasNestedFunction 在 visitFunDecl 中被设置（仅 inFunction_==true 时）。
    if (!scope.hasNestedFunction) {
        nextLocalSlot_ = scope.slotBase;
        // 清除 varMap_ 中属于本块的局部变量条目，避免块外引用到已回收的 slot。
        // 注意：临时 slot（AND/OR 短路）未记录在 localSlots，仅靠 slotBase 回收即可，
        // 因其 varMap_ 无对应条目（临时 slot 不绑定变量名）。
        for (uint32_t slot : scope.localSlots) {
            for (auto it = varMap_.begin(); it != varMap_.end(); ++it) {
                if (it->second.kind == VarInfo::Kind::LOCAL && it->second.index == slot) {
                    varMap_.erase(it);
                    break;
                }
            }
        }
    }
    // CRITICAL-2 fix: 恢复外层绑定。无论是否回收槽位，都需恢复被遮蔽的旧 varMap_ 条目，
    // 否则外层变量在块退出后不可达（原 bug：varMap_ 无作用域栈，内块覆盖后外层丢失）。
    // 遍历顺序：逆序恢复以处理多重遮蔽（虽实践中单层遮蔽居多）。
    for (auto it = scope.shadowedVars.rbegin(); it != scope.shadowedVars.rend(); ++it) {
        if (it->hadOld) {
            varMap_[it->name] = it->info;
        } else {
            // 块外原本无此变量，删除块内新增的条目
            varMap_.erase(it->name);
        }
    }
}

// ---- 辅助方法 ----

IRBasicBlock& AstIRBuilder::newBlock() {
    uint32_t label = ir_->allocLabel();
    IRBasicBlock& blk = ir_->addBlock(label);
    currentBlock_ = &blk;
    // 新块起始插入 LABEL 指令，便于 lowering 记录偏移
    currentBlock_->instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(label)}, 0);
    return *currentBlock_;
}

void AstIRBuilder::emitIR(IROp op, std::vector<IROperand> operands, int line) {
    currentBlock_->instructions.emplace_back(op, std::move(operands), line);
}

IROperand AstIRBuilder::emitConst(const Value& v, int line) {
    uint32_t idx = ir_->addConstant(v);
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::LOAD_CONST, {dest, IROperand::constant(idx)}, line);
    return dest;
}

IROperand AstIRBuilder::emitLoadVar(const std::string& name, int line) {
    VarInfo info = resolveVar(name);
    IROperand dest = ir_->allocVReg();
    switch (info.kind) {
    case VarInfo::Kind::LOCAL:
        emitIR(IROp::LOAD_LOCAL, {dest, IROperand::local(info.index)}, line);
        break;
    case VarInfo::Kind::GLOBAL_SLOT:
        // 槽位版：用 IMM_UINT 存槽位号
        emitIR(IROp::LOAD_GLOBAL, {dest, IROperand::imm(info.index)}, line);
        break;
    case VarInfo::Kind::GLOBAL_NAME:
        // 名称版：用 GLOBAL_NAME 存名称索引
        emitIR(IROp::LOAD_GLOBAL, {dest, IROperand::global(info.index)}, line);
        break;
    case VarInfo::Kind::UPVALUE:
        emitIR(IROp::LOAD_UPVALUE, {dest, IROperand::upvalue(info.index)}, line);
        break;
    case VarInfo::Kind::MEMBER: {
        // L7 fix: 方法体内裸字段读取 → dest = this.field
        // this 在 slot 0（emitFunctionPrologue 中 varMap_["this"] = LOCAL(0)）
        IROperand thisReg = ir_->allocVReg();
        emitIR(IROp::LOAD_LOCAL, {thisReg, IROperand::local(0)}, line);
        emitIR(IROp::MEMBER_GET, {dest, thisReg, IROperand::field(info.index)}, line);
        break;
    }
    default:
        // VarInfo::Kind 是封闭枚举，落空会导致 dest vreg 已分配但无指令 emit，
        // 后续 lowering 栈深度映射缺失，静默产生坏代码。
        // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
        Logger::Error("AstIRBuilder::emitLoadVar: 未知 VarInfo::Kind " + std::to_string(static_cast<int>(info.kind)),
                      "IR");
        assert(false && "未知 VarInfo::Kind");
        break;
    }
    return dest;
}

void AstIRBuilder::emitStoreVar(const std::string& name, IROperand val, int line) {
    VarInfo info = resolveVar(name);
    switch (info.kind) {
    case VarInfo::Kind::LOCAL:
        emitIR(IROp::STORE_LOCAL, {IROperand::local(info.index), val}, line);
        break;
    case VarInfo::Kind::GLOBAL_SLOT:
        emitIR(IROp::STORE_GLOBAL, {IROperand::imm(info.index), val}, line);
        break;
    case VarInfo::Kind::GLOBAL_NAME:
        emitIR(IROp::STORE_GLOBAL, {IROperand::global(info.index), val}, line);
        break;
    case VarInfo::Kind::UPVALUE:
        emitIR(IROp::STORE_UPVALUE, {IROperand::upvalue(info.index), val}, line);
        break;
    case VarInfo::Kind::MEMBER: {
        // L7 fix: 方法体内裸字段赋值 → this.field = val
        // this 在 slot 0（emitFunctionPrologue 中 varMap_["this"] = LOCAL(0)）。
        //
        // 使用 MEMBER_SET_LOCAL 直接修改 slot 0 (this)，避免 MEMBER_SET 的 COW 副本问题：
        //   MEMBER_SET 弹出 obj 副本修改 → COW detach 副本，原 slot 0 的 this 不变 → 赋值丢失。
        //   MEMBER_SET_LOCAL 直接修改 stack_[bp+0] / reg(0)，COW detach 发生在实际槽位，
        //   后续读取 this.field 能看到新值。
        //
        // val 已在栈顶（由调用方 visitAssignment/emitMethodCallWriteback 推入，TYPE_CHECK 为 peek 不影响）。
        // StackVM: OP_MEMBER_SET_LOCAL pops val, modifies stack_[bp+0].field
        // RegisterVM: REG_MEMBER_SET reg(0), field, val_reg — modifies reg(0).field in place
        // 调用方须感知 val 已被消费（visitAssignment 对 MEMBER 需 reload，类似 GLOBAL 路径）。
        emitIR(IROp::MEMBER_SET_LOCAL, {IROperand::local(0), IROperand::field(info.index), val}, line);
        break;
    }
    default:
        // 落空会导致 store 被静默丢弃，变量赋值失效。
        // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
        Logger::Error("AstIRBuilder::emitStoreVar: 未知 VarInfo::Kind " + std::to_string(static_cast<int>(info.kind)),
                      "IR");
        assert(false && "未知 VarInfo::Kind");
        break;
    }
}

// 2026-06-29: 发射 TYPE_CHECK IR — 将类型注解字符串加入常量池，发射 TYPE_CHECK 指令
// 注意：直接用 addConstant + IROperand::constant，不走 emitConst（后者会额外发射 LOAD_CONST
// 并返回 vreg，而 TYPE_CHECK 的 lowering 期望 operands[1] 为常量池索引而非 vreg）。
void AstIRBuilder::emitTypeCheckIR(IROperand val, const std::string& typeAnnotation, int line) {
    if (typeAnnotation.empty())
        return;
    // R163 泛型扩展：类型参数运行时擦除——不发射 TYPE_CHECK IR，对齐 Compiler 的 emitTypeCheck 跳过逻辑
    if (!currentTypeParams_.empty() && minilang::isTypeParameter(typeAnnotation, currentTypeParams_)) {
        return;
    }
    uint32_t typeIdx = ir_->addConstant(Value(typeAnnotation));
    emitIR(IROp::TYPE_CHECK, {val, IROperand::constant(typeIdx)}, line);
}

AstIRBuilder::VarInfo AstIRBuilder::resolveVar(const std::string& name) {
    // 1. 优先查 varMap_（当前作用域已声明的变量）
    auto it = varMap_.find(name);
    if (it != varMap_.end())
        return it->second;

    // L7 fix: 方法体内裸字段访问 → 解析为 this.field（MEMBER_GET/MEMBER_SET on this slot 0）。
    // 不注册为 LOCAL slot，避免 StackVM（推字段槽）与 RegisterVM（不推字段槽）帧布局不一致。
    // currentFunctionIsMethod_ 在 emitFunctionPrologue 中由 compilingMethod_ 快照，
    // 仅对方法体直接生效（嵌套函数 currentFunctionIsMethod_=false，裸字段走 upvalue/global 路径）。
    // compilingClassFieldNames_ 含继承字段（父类字段在前），由 visitClassDecl 构建。
    if (currentFunctionIsMethod_ && !compilingClassFieldNames_.empty()) {
        auto fieldIt = std::find(compilingClassFieldNames_.begin(), compilingClassFieldNames_.end(), name);
        if (fieldIt != compilingClassFieldNames_.end()) {
            uint32_t fieldIdx = ir_->addGlobal(name);
            VarInfo info{VarInfo::Kind::MEMBER, fieldIdx};
            varMap_[name] = info; // 缓存，后续 resolveVar 直接命中 varMap_
            return info;
        }
    }

    // 2. 函数内：尝试 upvalue 捕获（限制1）
    if (inFunction_) {
        // 检查是否为内嵌函数（当前函数内声明的函数）
        auto innerIt = innerFunctions_.find(name);
        if (innerIt != innerFunctions_.end()) {
            auto slotIt = innerFunctionSlots_.find(name);
            if (slotIt != innerFunctionSlots_.end()) {
                return {VarInfo::Kind::LOCAL, static_cast<uint32_t>(slotIt->second)};
            }
        }
        // 尝试添加为 upvalue
        addUpvalue(name);
        // 检查是否成功添加
        auto uvNameIt = currentUpvalueNames_.find(name);
        if (uvNameIt != currentUpvalueNames_.end()) {
            VarInfo info{VarInfo::Kind::UPVALUE, static_cast<uint32_t>(uvNameIt->second)};
            varMap_[name] = info;
            return info;
        }
        // 未找到 upvalue，继续向下检查全局
    }

    // 3. 顶层/函数内回退：检查全局槽位（限制3）
    int slot = lookupGlobalSlot(name);
    if (slot >= 0) {
        VarInfo info{VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot)};
        varMap_[name] = info;
        return info;
    }

    // 4. 回退：名称索引（GLOBAL_NAME）
    uint32_t idx = ir_->addGlobal(name);
    VarInfo info{VarInfo::Kind::GLOBAL_NAME, idx};
    varMap_[name] = info;
    return info;
}

uint32_t AstIRBuilder::addUpvalue(const std::string& name) {
    // 检查是否已在 currentUpvalues_ 中
    auto it = currentUpvalueNames_.find(name);
    if (it != currentUpvalueNames_.end())
        return static_cast<uint32_t>(it->second);

    // 检查是否为外层局部变量（isLocal=true，直接捕获）
    auto localIt = outerLocalSlots_.find(name);
    if (localIt != outerLocalSlots_.end()) {
        uint32_t idx = static_cast<uint32_t>(currentUpvalues_.size());
        currentUpvalues_.push_back({idx, true, localIt->second, name}); // 条件断点修复(R75): 记录name
        currentUpvalueNames_[name] = static_cast<int>(idx);
        return idx;
    }

    // 检查是否为外层 upvalue（isLocal=false，透传）
    auto uvIt = outerUpvalueNames_.find(name);
    if (uvIt != outerUpvalueNames_.end()) {
        uint32_t idx = static_cast<uint32_t>(currentUpvalues_.size());
        currentUpvalues_.push_back({idx, false, uvIt->second, name}); // 条件断点修复(R75): 记录name
        currentUpvalueNames_[name] = static_cast<int>(idx);
        return idx;
    }

    // 检查是否为外层函数（isLocal=true，按局部变量捕获）
    auto fnIt = outerFunctions_.find(name);
    if (fnIt != outerFunctions_.end()) {
        uint32_t idx = static_cast<uint32_t>(currentUpvalues_.size());
        currentUpvalues_.push_back({idx, true, fnIt->second, name}); // 条件断点修复(R75): 记录name
        currentUpvalueNames_[name] = static_cast<int>(idx);
        return idx;
    }

    return 0; // 未找到
}

// CRITICAL-1 fix: 前向自由变量分析。对齐 Interpreter::computeFreeVariables 的语义，
// 但用于 IR 层——在编译子函数体前预建 upvalue，使中间函数捕获内层引用的变量供透传。
// 实现：递归遍历 AST，收集所有 VarRef/Assignment 中引用但未在函数内定义的变量名。
// 嵌套函数的自由变量若不在外层作用域定义，传播为外层自由变量。
bool AstIRBuilder::isDefinedInScopes(const std::vector<std::unordered_set<std::string>>& scopes,
                                     const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (it->count(name))
            return true;
    }
    return false;
}

void AstIRBuilder::collectFreeVars(const ASTNode& node, std::vector<std::unordered_set<std::string>>& scopes,
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
        nestedScopes.back().insert(nestedFn.name);
        std::unordered_set<std::string> nestedFree;
        if (nestedFn.body)
            collectFreeVars(*nestedFn.body, nestedScopes, nestedFree);
        for (const auto& name : nestedFree) {
            if (!isDefinedInScopes(scopes, name)) {
                freeVars.insert(name);
            }
        }
        scopes.back().insert(nestedFn.name);
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
        // BUG-IR-FV-1 fix: 类名先入当前作用域，使方法体/字段初始化器可引用类名（如 new C()）
        scopes.back().insert(cls.name);
        // 递归分析类成员：方法（FunDecl）在嵌套作用域中分析并向上传播自由变量，
        // 字段初始化器（VarDecl）在当前作用域中分析。
        // 原实现仅插入类名后 break，不递归成员，导致类方法体中引用的外层变量
        // 未被识别为外层函数的自由变量，upvalue 传递链断裂。
        for (const auto& member : cls.members) {
            if (member)
                collectFreeVars(*member, scopes, freeVars);
        }
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

std::unordered_set<std::string> AstIRBuilder::computeFreeVars(const FunDecl& fn) {
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

// ---- AST 节点转换：分派 ----

// BUG-IR-POP-2/3 fix: 语句上下文统一 POP 处理。
// 对齐 Compiler::compileStatement 的语义：表达式语句的返回值需 POP 消费。
// 在 if then/else、while body、for body 等语句上下文使用此方法，
// 避免手动 POP 遗漏导致栈泄漏（循环内单表达式语句）或栈不平衡（then/else 路径栈深度不一致）。
void AstIRBuilder::visitStatement(ASTNode* node) {
    if (!node)
        return;
    IROperand result = visitNode(node);
    // AUDIT-R5 BUG-01 fix: 改用节点级重载（覆盖匿名 lambda）。
    if (needsPopForExprStmt(node)) {
        emitIR(IROp::POP, {result}, node->line);
    }
}

IROperand AstIRBuilder::visitNode(ASTNode* node) {
    if (!node)
        return IROperand::vreg(0); // 空节点返回空 vreg
    switch (node->nodeType) {
    // 表达式（返回 vreg）
    case NodeType::NODE_BINARY_OP:
        return visitBinaryOp(static_cast<BinaryOp*>(node));
    case NodeType::NODE_UNARY_OP:
        return visitUnaryOp(static_cast<UnaryOp*>(node));
    case NodeType::NODE_NUMBER_LITERAL:
        return visitNumberLiteral(static_cast<NumberLiteral*>(node));
    case NodeType::NODE_STRING_LITERAL:
        return visitStringLiteral(static_cast<StringLiteral*>(node));
    case NodeType::NODE_BOOL_LITERAL:
        return visitBoolLiteral(static_cast<BoolLiteral*>(node));
    case NodeType::NODE_NULL_LITERAL:
        return visitNullLiteral(static_cast<NullLiteral*>(node));
    case NodeType::NODE_VAR_REF:
        return visitVarRef(static_cast<VarRef*>(node));
    case NodeType::NODE_ASSIGNMENT:
        return visitAssignment(static_cast<Assignment*>(node));
    case NodeType::NODE_FUN_CALL:
        return visitFunCall(static_cast<FunCall*>(node));
    case NodeType::NODE_ARRAY_LITERAL:
        return visitArrayLiteral(static_cast<ArrayLiteral*>(node));
    case NodeType::NODE_DICT_LITERAL:
        return visitDictLiteral(static_cast<DictLiteral*>(node));
    // R98 元组与解构
    case NodeType::NODE_TUPLE_LITERAL:
        return visitTupleLiteral(static_cast<TupleLiteral*>(node));
    // R99 枚举与 ADT + match
    case NodeType::NODE_ENUM_VARIANT_EXPR:
        return visitEnumVariantExpr(static_cast<EnumVariantExpr*>(node));
    case NodeType::NODE_MATCH_EXPR:
        return visitMatchExpr(static_cast<MatchExpr*>(node));
    case NodeType::NODE_INDEX_ACCESS:
        return visitIndexAccess(static_cast<IndexAccess*>(node));
    case NodeType::NODE_MEMBER_ACCESS:
        return visitMemberAccess(static_cast<MemberAccess*>(node));
    case NodeType::NODE_METHOD_CALL:
        return visitMethodCall(static_cast<MethodCall*>(node));
    // 语句（返回空 vreg）
    case NodeType::NODE_VAR_DECL:
        visitVarDecl(static_cast<VarDecl*>(node));
        return IROperand::vreg(0);
    // R98 元组与解构：解构绑定语句
    case NodeType::NODE_DESTRUCTURE_BINDING:
        visitDestructureBinding(static_cast<DestructureBinding*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_IF_STMT:
        visitIfStmt(static_cast<IfStmt*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_WHILE_STMT:
        visitWhileStmt(static_cast<WhileStmt*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_FOR_STMT:
        visitForStmt(static_cast<ForStmt*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_FUN_DECL:
        // R98 W3: visitFunDecl 返回 lambda 的 dest vreg（具名函数返回 vreg(0) 哨兵）
        return visitFunDecl(static_cast<FunDecl*>(node));
    case NodeType::NODE_RETURN_STMT:
        visitReturnStmt(static_cast<ReturnStmt*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_PRINT_STMT:
        visitPrintStmt(static_cast<PrintStmt*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_BLOCK:
        visitBlock(static_cast<Block*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_INDEX_ASSIGN:
        visitIndexAssign(static_cast<IndexAssign*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_CLASS_DECL:
        visitClassDecl(static_cast<ClassDecl*>(node));
        return IROperand::vreg(0);
    // R99 枚举与 ADT
    case NodeType::NODE_ENUM_DECL:
        visitEnumDecl(static_cast<EnumDecl*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_MEMBER_ASSIGN:
        visitMemberAssign(static_cast<MemberAssign*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_BREAK_STMT:
        visitBreakStmt(static_cast<BreakStmt*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_CONTINUE_STMT:
        visitContinueStmt(static_cast<ContinueStmt*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_TRY_STMT:
        visitTryStmt(static_cast<TryStmt*>(node));
        return IROperand::vreg(0);
    case NodeType::NODE_THROW_STMT:
        visitThrowStmt(static_cast<ThrowStmt*>(node));
        return IROperand::vreg(0);
    // P1 fix: 插值字符串 → IR 字符串拼接链
    case NodeType::NODE_INTERPOLATED_STRING:
        return visitInterpolatedString(static_cast<InterpolatedString*>(node));
    // R7 fix: EXPORT 语义为编译其子声明（与 Compiler.cpp visitExportStmt 对齐）。
    // 原实现静默跳过导致 export var x = 1 中的 x 在 IR/寄存器式路径整体丢失。
    case NodeType::NODE_EXPORT_STMT: {
        auto* exp = static_cast<ExportStmt*>(node);
        if (exp->declaration) {
            visitNode(exp->declaration.get());
        }
        return IROperand::vreg(0);
    }
    // VM-IMPORT: IR 路径支持 import 语句的内联编译。
    // 对齐 Compiler::visitImportStmt 的语义：加载模块源码、解析 AST、
    // 预扫描全局槽位、内联 visitNode 模块语句。
    case NodeType::NODE_IMPORT_STMT: {
        auto* importNode = static_cast<ImportStmt*>(node);
        handleImportStmt(*importNode);
        return IROperand::vreg(0);
    }
    case NodeType::NODE_SUPER_EXPR: {
        // BUG-IR-SUPER-1 fix: super 表达式编译为加载 this（对齐 Compiler::visitSuperExpr
        // 发射 OP_GET_LOCAL 0 的语义）。调用点（visitMethodCall/visitMemberAccess）
        // 检测 super 对象并使用父类查找。
        // 原实现落入 default case 不发射任何 IR，Release 构建中 assert 被剥离后
        // 返回空 vreg，后续 emit POP 无对应压栈导致栈下溢。
        //
        // 注：super 在非方法上下文中的错误为运行时错误（非编译期），由 VM 在
        // emitLoadVar("this") 回退到 GLOBAL_NAME 查找失败时报 "未定义的变量: this"。
        // 此运行时错误不可被 try/catch 捕获（在 try 块进入前即触发），与 Interpreter
        // 报 "super 只能在类方法中使用" 消息不同——文档化为已知三后端差异。
        // 顶层 / 普通函数内 super 的运行时错误行为由 ConsistencyDiff.H7b 与
        // AuditSuper_RuntimeErrorNotCatchableByTryCatch 测试覆盖。
        return emitLoadVar("this", node->line);
    }
    // R164 协程/生成器：yield 表达式 → IR YIELD 指令
    case NodeType::NODE_YIELD_EXPR:
        return visitYieldExpr(static_cast<YieldExpr*>(node));
    // 七特性 MVP 阶段 4：await 表达式 → IR AWAIT 指令
    case NodeType::NODE_AWAIT_EXPR:
        return visitAwaitExpr(static_cast<AwaitExpr*>(node));
    // 七特性 MVP 阶段 2：宏系统（parse 期已展开，IR 路径直接处理 expanded 子树）
    case NodeType::NODE_MACRO_DECL:
        // 声明运行期 no-op（与 Compiler::visitMacroDecl 对齐）
        return IROperand::vreg(0);
    // 七特性 MVP 阶段 3：trait 声明运行期 no-op（方法 parse 期已合入类）
    case NodeType::NODE_TRAIT_DECL:
        return IROperand::vreg(0);
    case NodeType::NODE_MACRO_CALL: {
        auto* mc = static_cast<MacroCallExpr*>(node);
        if (!mc->expanded) {
            irDiagnostics_.addErrorFatal("宏调用 " + mc->name + "! 缺少展开子树（AST 构造错误）", mc->line,
                                         mc->column, DiagSource::Compiler);
            return IROperand::vreg(0);
        }
        IROperand r = visitNode(mc->expanded.get());
        // 七特性宏升级：语句宏（expanded 为 Block）visitBlock 返回 vreg(0) 无净值，
        // 分配新 vreg 载入 null 作为表达式值（与 StackVM 补 OP_NULL 一致，保证语句位置 POP 平衡）。
        if (!mc->producesValue) {
            IROperand nullReg = ir_->allocVReg();
            emitIR(IROp::LOAD_NULL, {nullReg}, mc->line);
            return nullReg;
        }
        return r;
    }
    default:
        // 落空会导致 dest vreg 已分配但无指令 emit，后续 lowering 栈深度映射缺失，
        // 静默产生坏代码。用 assert 兜底，Release 构建中 assert 被剥离时返回空 vreg
        // 至少不会 emit 错误指令。
        // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
        // BUG-IR-VISITNODE-ERR fix: 同时设置 hasError_ 阻止 build() 继续生成坏 IR。
        Logger::Error(
            "AstIRBuilder::visitNode: 未支持的 AST 节点类型 " + std::to_string(static_cast<int>(node->nodeType)), "IR");
        // P2-12 fix (错误恢复): 内部不变量违反——未支持的 AST 节点会产生悬空 dest vreg，
        // 后续 lowering 栈深度映射缺失，静默产生坏 IR。标记为致命错误确保 build() 在下一条
        // 语句边界中止，避免污染整个 IRModule。
        irDiagnostics_.addErrorFatal("AstIRBuilder: 未支持的 AST 节点类型 " +
                                         std::to_string(static_cast<int>(node->nodeType)),
                                     node->line, 0, DiagSource::Compiler);
        assert(false && "AstIRBuilder::visitNode: 未支持的 AST 节点类型");
        return IROperand::vreg(0);
    }
}

// R111 重构：BIN_AND 短路求值提取为单一职责 helper。
// 语义：左值为假时短路返回左值原值（与 Interpreter 的 `and` 操作数原值语义对齐），
// 左值为真时返回右操作数原值。用临时 local slot 汇合两条路径保证 dest 总有定义。
// L1 fix: JUMP_IF_FALSE 在 StackVM lowering 用 OP_JUMP_IF_FALSE（peek 不 pop），
// STORE_LOCAL 用 OP_SET_LOCAL（peek 不 pop）。两条路径都残留 left/right 在栈上，
// 导致 endLabel 汇合点栈深度不一致。修复：每个 STORE_LOCAL 后显式 POP 消费残留。
IROperand AstIRBuilder::emitShortCircuitAnd(BinaryOp* node) {
    IROperand left = visitNode(node->left.get());
    uint32_t shortCircuitLabel = ir_->allocLabel();
    uint32_t endLabel = ir_->allocLabel();
    uint32_t tempSlot = nextLocalSlot_++; // 临时 slot 存放结果
    // 左值为假则短路（结果 = left）
    emitIR(IROp::JUMP_IF_FALSE, {left, IROperand::label(shortCircuitLabel)}, node->line);
    // 非短路路径：left 在栈顶，存入 tempSlot，POP 消费残留
    emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), left}, node->line);
    emitIR(IROp::POP, {left}, node->line);
    IROperand right = visitNode(node->right.get());
    emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), right}, node->line);
    emitIR(IROp::POP, {right}, node->line);
    emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);
    // 短路路径：left 在栈顶（JUMP_IF_FALSE peek），存入 tempSlot，POP 消费残留
    emitIR(IROp::LABEL, {IROperand::label(shortCircuitLabel)}, node->line);
    emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), left}, node->line);
    emitIR(IROp::POP, {left}, node->line);
    // 汇合：加载结果到 dest（两条路径栈均已空，LOAD_LOCAL push dest）
    emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::LOAD_LOCAL, {dest, IROperand::local(tempSlot)}, node->line);
    return dest;
}

// R111 重构：BIN_OR 短路求值提取为单一职责 helper。
// 语义：左值为真时短路返回左值原值（与 Interpreter 的 `or` 操作数原值语义对齐），
// 左值为假时返回右操作数原值。栈深度一致性处理与 emitShortCircuitAnd 相同。
IROperand AstIRBuilder::emitShortCircuitOr(BinaryOp* node) {
    IROperand left = visitNode(node->left.get());
    uint32_t evalRightLabel = ir_->allocLabel();
    uint32_t endLabel = ir_->allocLabel();
    uint32_t tempSlot = nextLocalSlot_++; // 临时 slot 存放结果
    // 左值为假则去求值右操作数
    emitIR(IROp::JUMP_IF_FALSE, {left, IROperand::label(evalRightLabel)}, node->line);
    // 左值为真，短路（结果 = left）：left 在栈顶，存入 tempSlot，POP 消费残留
    emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), left}, node->line);
    emitIR(IROp::POP, {left}, node->line);
    emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);
    // 非短路路径：left 在栈顶（JUMP_IF_FALSE peek），存入 tempSlot，POP 消费残留
    emitIR(IROp::LABEL, {IROperand::label(evalRightLabel)}, node->line);
    emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), left}, node->line);
    emitIR(IROp::POP, {left}, node->line);
    IROperand right = visitNode(node->right.get());
    emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), right}, node->line);
    emitIR(IROp::POP, {right}, node->line);
    // 汇合：加载结果到 dest
    emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::LOAD_LOCAL, {dest, IROperand::local(tempSlot)}, node->line);
    return dest;
}

IROperand AstIRBuilder::visitBinaryOp(BinaryOp* node) {
    // AND/OR 短路求值：用临时 local slot 汇合两条路径的结果。
    // 旧实现短路路径返回未定义的 right vreg（right 从未被 visitNode 求值），
    // 导致消费者读取脏 vreg。现在两条路径都 STORE_LOCAL 到 tempSlot，
    // 汇合后 LOAD_LOCAL 到 dest，保证 dest 在所有路径都有定义。
    // R111 重构：AND/OR 短路逻辑提取到 emitShortCircuitAnd/emitShortCircuitOr。
    if (node->opType == BinOpType::BIN_AND)
        return emitShortCircuitAnd(node);
    if (node->opType == BinOpType::BIN_OR)
        return emitShortCircuitOr(node);
    // 非短路二元运算
    IROperand left = visitNode(node->left.get());
    IROperand right = visitNode(node->right.get());
    IROperand dest = ir_->allocVReg();
    switch (node->opType) {
    case BinOpType::BIN_ADD:
        emitIR(IROp::ADD, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_SUB:
        emitIR(IROp::SUB, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_MUL:
        emitIR(IROp::MUL, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_DIV:
        emitIR(IROp::DIV, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_MOD:
        emitIR(IROp::MOD, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_EQ:
        emitIR(IROp::EQ, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_NEQ:
        emitIR(IROp::NEQ, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_LT:
        emitIR(IROp::LT, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_GT:
        emitIR(IROp::GT, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_LTE:
        emitIR(IROp::LTE, {dest, left, right}, node->line);
        break;
    case BinOpType::BIN_GTE:
        emitIR(IROp::GTE, {dest, left, right}, node->line);
        break;
    default:
        // R7 fix: 落空会导致 dest vreg 已分配但无指令 emit，后续 lowering 栈深度映射缺失，
        // 静默产生坏代码。用 assert 兜底，Release 构建中 assert 被剥离时 dest 仍返回（至少不崩溃）。
        // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
        // AUDIT-P1 fix: 仅 Logger::Error + assert 在 Release 下不阻断控制流，dest 悬空 vreg
        // 仍返回，污染整个 IRModule。新增 hasError_ = true 确保调用方（visitNode 行 133）
        // 检测到错误并中止 IR 构建。
        Logger::Error(
            "AstIRBuilder::visitBinaryOp: 未处理的 BinOpType " + std::to_string(static_cast<int>(node->opType)), "IR");
        // P2-12 fix (错误恢复): 内部不变量违反——未处理的 BinOpType 会导致 dest vreg 已分配
        // 但无对应指令 emit，后续 lowering 栈深度映射缺失，静默产生坏 IR。标记为致命错误
        // 确保 build() 在下一条语句边界中止。
        irDiagnostics_.addErrorFatal("AstIRBuilder::visitBinaryOp: 未处理的 BinOpType " +
                                         std::to_string(static_cast<int>(node->opType)),
                                     node->line, 0, DiagSource::Compiler);
        assert(false && "未处理的 BinOpType");
        break;
    }
    return dest;
}

IROperand AstIRBuilder::visitUnaryOp(UnaryOp* node) {
    IROperand operand = visitNode(node->operand.get());
    // 一元正号直接返回操作数
    if (node->opType == UnaryOp::UnaryOpType::UOP_PLUS) {
        return operand;
    }
    IROperand dest = ir_->allocVReg();
    if (node->opType == UnaryOp::UnaryOpType::UOP_NEGATE) {
        emitIR(IROp::NEGATE, {dest, operand}, node->line);
    } else if (node->opType == UnaryOp::UnaryOpType::UOP_NOT) {
        emitIR(IROp::NOT, {dest, operand}, node->line);
    }
    return dest;
}

IROperand AstIRBuilder::visitNumberLiteral(NumberLiteral* node) {
    return emitConst(node->getValue(), node->line);
}

IROperand AstIRBuilder::visitStringLiteral(StringLiteral* node) {
    return emitConst(node->getValue(), node->line);
}

IROperand AstIRBuilder::visitBoolLiteral(BoolLiteral* node) {
    IROperand dest = ir_->allocVReg();
    if (node->value) {
        emitIR(IROp::LOAD_TRUE, {dest}, node->line);
    } else {
        emitIR(IROp::LOAD_FALSE, {dest}, node->line);
    }
    return dest;
}

IROperand AstIRBuilder::visitNullLiteral(NullLiteral* node) {
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::LOAD_NULL, {dest}, node->line);
    return dest;
}

IROperand AstIRBuilder::visitVarRef(VarRef* node) {
    return emitLoadVar(node->name, node->line);
}

IROperand AstIRBuilder::visitAssignment(Assignment* node) {
    IROperand val = visitNode(node->value.get());
    // 2026-06-29: 类型注解运行时检查（赋值时强制）
    const std::string* varType = findVarType(node->name);
    if (varType) {
        emitTypeCheckIR(val, *varType, node->line);
    }
    // STORE_GLOBAL lowers to OP_SET_GLOBAL which pops the value off the stack,
    // while STORE_LOCAL/STORE_UPVALUE use peek (don't pop). To keep the
    // assignment expression's result consistently on the stack (matching the
    // normal Compiler path which uses OP_DUP), reload the value for global stores.
    // This ensures expression-statement POP is always safe and assignment-as-
    // sub-expression works for globals.
    VarInfo info = resolveVar(node->name);
    bool isGlobalStore = (info.kind == VarInfo::Kind::GLOBAL_SLOT || info.kind == VarInfo::Kind::GLOBAL_NAME);
    // L7 fix: MEMBER 路径的 emitStoreVar 通过 MEMBER_SET 消费 val（类似 GLOBAL 的 OP_SET_GLOBAL pop），
    // 调用方需 reload 赋值后的值作为表达式结果（供表达式语句 POP）。
    bool isMemberStore = (info.kind == VarInfo::Kind::MEMBER);
    emitStoreVar(node->name, val, node->line);
    if (isGlobalStore || isMemberStore) {
        return emitLoadVar(node->name, node->line);
    }
    return val; // LOCAL/UPVALUE: value still on stack (peek), return original vreg
}

void AstIRBuilder::visitVarDecl(VarDecl* node) {
    IROperand val;
    if (node->initializer) {
        val = visitNode(node->initializer.get());
    } else if (!node->typeAnnotation.empty() &&
               definedClassNames_.find(node->typeAnnotation) != definedClassNames_.end()) {
        // BUG-IR-VARDECL-1 fix: 类名类型注解无初始化器 → 自动构造实例，
        // 对齐 Compiler.cpp visitVarDecl L620-626 的 S2 fix 语义。
        // 原实现统一初始化为 null，导致 IR 路径 `var x MyClass;` 得到 null
        // 而 Compiler 路径得到 new MyClass()，三后端不一致。
        val = ir_->allocVReg();
        uint32_t nameIdx = ir_->addGlobal(node->typeAnnotation);
        emitIR(IROp::CLASS_NEW, {val, IROperand::funcName(nameIdx), IROperand::imm(0)}, node->line);
    } else {
        // 无初始化器：初始化为 null
        val = ir_->allocVReg();
        emitIR(IROp::LOAD_NULL, {val}, node->line);
    }
    // 2026-06-29: 类型注解运行时检查（在 STORE 前检查 val）
    emitTypeCheckIR(val, node->typeAnnotation, node->line);
    if (!node->typeAnnotation.empty()) {
        varTypes_[node->name] = node->typeAnnotation;
    }
    if (inFunction_) {
        // R164 fixup2: 同作用域变量重定义检测（对齐 Interpreter tryDefineNew + Compiler
        // currentLocals_ 检查）。检查当前块作用域的 declaredNames（含参数），允许嵌套块遮蔽。
        if (!blockScopes_.empty() && blockScopes_.back().declaredNames.count(node->name)) {
            irDiagnostics_.addError("变量 '" + node->name + "' 已在当前作用域中定义", node->line, 0,
                                    DiagSource::Compiler);
            return;
        }
        // 函数内：注册为 LOCAL（限制5：记录到当前 BlockScope）
        uint32_t slot = nextLocalSlot_++;
        // BUG-IDE-12 fix: 记录 slot→name 映射（跨作用域累积，不随块退出清除）
        if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
            localSlotNames_.resize(slot + 1);
        }
        localSlotNames_[slot] = node->name;
        // L1 fix: 记录 (slot, name, startInstr) 到 slotNameRanges_，endInstr 暂设为 0
        // （由 leaveBlockScope 在块退出时回填）。resolveSlotName 按 instr 范围反查变量名，
        // 解决兄弟作用域槽位复用导致的变量名错位。currentInstrIndex 由 ir_ 的指令总数计算。
        size_t curIdx = 0;
        for (const auto& blk : ir_->blocks)
            curIdx += blk.instructions.size();
        slotNameRanges_.push_back({slot, node->name, curIdx, 0});
        // CRITICAL-2 fix: 覆盖 varMap_ 前保存旧条目，供 leaveBlockScope 恢复外层绑定
        if (!blockScopes_.empty()) {
            auto it = varMap_.find(node->name);
            if (it != varMap_.end()) {
                blockScopes_.back().shadowedVars.push_back({node->name, it->second, true});
            } else {
                blockScopes_.back().shadowedVars.push_back({node->name, VarInfo{}, false});
            }
        }
        varMap_[node->name] = {VarInfo::Kind::LOCAL, slot};
        emitIR(IROp::STORE_LOCAL, {IROperand::local(slot), val}, node->line);
        // L1 fix: STORE_LOCAL 在 StackVM lowering 中用 OP_SET_LOCAL（peek 不 pop），
        // 初始化值残留在栈上。函数内 var 声明是语句，结果值不被消费，需 POP 消费。
        // （顶层 GLOBAL_SLOT/GLOBAL_NAME 用 DEFINE_GLOBAL → OP_DEFINE_GLOBAL 已 pop，无需 POP）
        emitIR(IROp::POP, {val}, node->line);
        // 记录到当前 BlockScope 的 localSlots
        if (!blockScopes_.empty()) {
            blockScopes_.back().localSlots.push_back(slot);
            // R164 fixup2: 记录到 declaredNames 供后续同作用域重定义检测
            blockScopes_.back().declaredNames.insert(node->name);
        }
    } else {
        // 顶层：使用全局槽位（限制3）
        int slot = lookupGlobalSlot(node->name);
        if (slot >= 0) {
            // 有槽位：GLOBAL_SLOT，emit DEFINE_GLOBAL（槽位版）
            varMap_[node->name] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot)};
            emitIR(IROp::DEFINE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(slot)), val}, node->line);
        } else {
            // 无槽位：回退名称索引
            uint32_t idx = ir_->addGlobal(node->name);
            varMap_[node->name] = {VarInfo::Kind::GLOBAL_NAME, idx};
            emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(idx), val}, node->line);
        }
    }
}

void AstIRBuilder::visitIfStmt(IfStmt* node) {
    IROperand cond = visitNode(node->condition.get());
    uint32_t elseLabel = ir_->allocLabel();
    uint32_t endLabel = ir_->allocLabel();
    // 条件为假跳到 else
    // L1 fix: JUMP_IF_FALSE 在 StackVM lowering 中用 OP_JUMP_IF_FALSE（peek 不 pop），
    // 条件值残留在栈上。对齐直接 Compiler.cpp visitIfStmt 的模式：在 then 和 else
    // 两条路径各自 emit POP 消费条件值，保证 endLabel 汇合点栈深度一致。
    emitIR(IROp::JUMP_IF_FALSE, {cond, IROperand::label(elseLabel)}, node->line);
    // then 分支：先 POP 消费条件值（peek 残留），再编译 then 体
    emitIR(IROp::POP, {cond}, node->line);
    if (inFunction_)
        enterBlockScope();
    // BUG-IR-POP-3 fix: 使用 visitStatement 统一处理表达式语句 POP，
    // 避免无花括号单语句体（如 `if (c) foo();`）栈不平衡。
    visitStatement(node->thenBranch.get());
    if (inFunction_)
        leaveBlockScope();
    emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);
    // else 分支：先 POP 消费条件值（peek 残留），再编译 else 体
    emitIR(IROp::LABEL, {IROperand::label(elseLabel)}, node->line);
    emitIR(IROp::POP, {cond}, node->line);
    if (node->elseBranch) {
        if (inFunction_)
            enterBlockScope();
        // BUG-IR-POP-3 fix: 使用 visitStatement 统一处理表达式语句 POP
        visitStatement(node->elseBranch.get());
        if (inFunction_)
            leaveBlockScope();
    }
    emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
}

void AstIRBuilder::visitWhileStmt(WhileStmt* node) {
    uint32_t startLabel = ir_->allocLabel();
    uint32_t exitLabel = ir_->allocLabel(); // L1 fix: JUMP_IF_FALSE 目标（条件值在栈上）
    uint32_t endLabel = ir_->allocLabel();  // break 目标（栈已空，无需 POP）
    // AUDIT-P1 fix: continue 目标必须在 leaveBlockScope（CLOSE_UPVALUE）之后，
    // 否则 continue 跳过 upvalue 关闭，闭包捕获的循环局部变量变为 by-reference。
    // 对齐 for 循环的 continueLabel（在 leaveBlockScope 之后）。
    uint32_t continueLabel = ir_->allocLabel();
    loopStack_.push_back({startLabel, endLabel, continueLabel, tryDepth_});
    emitIR(IROp::LABEL, {IROperand::label(startLabel)}, node->line);
    IROperand cond = visitNode(node->condition.get());
    // L1 fix: JUMP_IF_FALSE peek 不 pop，条件值残留在栈上。
    // 需要两个出口标签：exitLabel（条件假路径，条件值在栈上，需 POP）和
    // endLabel（break 路径，循环体已清空栈，无需 POP）。对齐直接 Compiler.cpp 的
    // exitTarget + breakTarget 双目标模式。
    emitIR(IROp::JUMP_IF_FALSE, {cond, IROperand::label(exitLabel)}, node->line);
    emitIR(IROp::POP, {cond}, node->line); // 循环体路径：POP 消费条件值
    // 循环体（限制5：块作用域包裹）
    if (inFunction_)
        enterBlockScope();
    // AUDIT-P2-CORRECT fix: 记录 body block scope 的 slotBase 供 visitBreakStmt
    // 发射 CLOSE_UPVALUE（对齐 Compiler.cpp 的 bodySlotBase 记录）
    if (inFunction_ && !blockScopes_.empty()) {
        loopStack_.back().bodySlotBase = blockScopes_.back().slotBase;
        loopStack_.back().needCloseUpvalue = true;
    }
    // BUG-IR-POP-2 fix: 使用 visitStatement 统一处理表达式语句 POP，
    // 避免无花括号单语句体（如 `while (c) foo();`）循环内栈泄漏导致栈溢出。
    visitStatement(node->body.get());
    // AUDIT-P1-CORRECT fix: continueLabel 必须在 leaveBlockScope 之前，
    // 使 continue 跳到 leaveBlockScope（CLOSE_UPVALUE）执行后再 JUMP。
    // 第三十八轮将 continueLabel 放在 leaveBlockScope 之后是方向性错误——
    // continue 跳过 leaveBlockScope 导致 upvalue 不关闭。
    emitIR(IROp::LABEL, {IROperand::label(continueLabel)}, node->line);
    if (inFunction_)
        leaveBlockScope();
    emitIR(IROp::JUMP, {IROperand::label(startLabel)}, node->line);
    // 条件假路径：条件值在栈上（JUMP_IF_FALSE peek），POP 消费
    emitIR(IROp::LABEL, {IROperand::label(exitLabel)}, node->line);
    emitIR(IROp::POP, {cond}, node->line); // 循环退出路径：POP 消费条件值
    // break 目标：循环体已清空栈，无需 POP
    emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
    loopStack_.pop_back();
}

void AstIRBuilder::visitForStmt(ForStmt* node) {
    // BUG-IR-SCOPE-1 fix: 对整个 for 语句包裹单一 block scope，
    // 使 initializer 声明的变量（如 `for (var i = 0; ...)`）作用域限定在循环内，
    // 循环结束后从 varMap_ 移除，对齐 Compiler.cpp 的 savedLocals 语义。
    // 原实现仅对 body 包裹 block scope，initializer 在外层作用域编译，
    // 导致 `for (var i...) {}` 后 `i` 仍可达（三后端不一致）。
    if (inFunction_)
        enterBlockScope();
    // 编译初始化表达式
    if (node->initializer) {
        IROperand initResult = visitNode(node->initializer.get());
        // 表达式初始化器（如 i = 0）的返回值未被消费，需 POP。
        // VarDecl 初始化器已自行平衡栈（STORE_LOCAL+POP 或 DEFINE_GLOBAL pop），不需 POP。
        if (needsPopForExprStmt(node->initializer.get())) {
            emitIR(IROp::POP, {initResult}, node->line);
        }
    }
    uint32_t startLabel = ir_->allocLabel();
    uint32_t endLabel = ir_->allocLabel(); // break 目标（栈已空，无需 POP）
    uint32_t continueLabel = ir_->allocLabel();
    // L1 fix: exitLabel 仅在条件存在时分配（条件假路径，条件值在栈上需 POP）
    uint32_t exitLabel = node->condition ? ir_->allocLabel() : 0;
    // continue 目标 = update 块（continueLabel 在 body 之后、update 之前）
    loopStack_.push_back({startLabel, endLabel, continueLabel, tryDepth_});
    emitIR(IROp::LABEL, {IROperand::label(startLabel)}, node->line);
    // 条件（nullptr 时视为永真，不 emit 任何指令——循环体由 body 内的 break 控制退出）
    // C-10 fix: 原 else 分支 emit LOAD_TRUE 分配 vreg 但永不消费，
    // 栈式 VM 后端 lowering 时每次循环迭代压一个 true 入栈而永不弹出 → 死循环程序栈溢出。
    IROperand cond; // Phase-5: 提升作用域以便 exit 路径引用
    if (node->condition) {
        cond = visitNode(node->condition.get());
        // L1 fix: JUMP_IF_FALSE peek 不 pop，条件值残留在栈上。
        // 同 visitWhileStmt 的双标签模式：exitLabel（条件假路径，需 POP）和
        // endLabel（break 路径，栈已空，无需 POP）。
        emitIR(IROp::JUMP_IF_FALSE, {cond, IROperand::label(exitLabel)}, node->line);
        emitIR(IROp::POP, {cond}, node->line); // 循环体路径：POP 消费条件值
    }
    // 编译循环体（限制5：块作用域包裹）
    if (inFunction_)
        enterBlockScope();
    // AUDIT-P2-CORRECT fix: 记录 body block scope 的 slotBase 供 visitBreakStmt
    // 发射 CLOSE_UPVALUE（对齐 Compiler.cpp 的 bodySlotBase 记录）
    if (inFunction_ && !blockScopes_.empty()) {
        loopStack_.back().bodySlotBase = blockScopes_.back().slotBase;
        loopStack_.back().needCloseUpvalue = true;
    }
    // BUG-IR-POP-2 fix: 使用 visitStatement 统一处理表达式语句 POP，
    // 避免无花括号单语句体（如 `for (...) foo();`）循环内栈泄漏导致栈溢出。
    visitStatement(node->body.get());
    // AUDIT-P1-CORRECT fix: continueLabel 必须在 leaveBlockScope 之前，
    // 使 continue 跳到 leaveBlockScope（CLOSE_UPVALUE）执行后再执行 update。
    // 第三十八轮将 continueLabel 放在 leaveBlockScope 之后是方向性错误——
    // continue 跳过 leaveBlockScope 导致 upvalue 不关闭。
    emitIR(IROp::LABEL, {IROperand::label(continueLabel)}, node->line);
    if (inFunction_)
        leaveBlockScope();
    // 编译 update 表达式
    if (node->update) {
        IROperand updateResult = visitNode(node->update.get());
        // update 表达式（如 i = i + 1）的返回值未被消费，需 POP。
        // 与 Compiler.cpp visitForStmt 对齐：update 后 emit OP_POP。
        if (needsPopForExprStmt(node->update.get())) {
            emitIR(IROp::POP, {updateResult}, node->line);
        }
    }
    emitIR(IROp::JUMP, {IROperand::label(startLabel)}, node->line);
    if (node->condition) {
        // 条件假路径：条件值在栈上（JUMP_IF_FALSE peek），POP 消费
        emitIR(IROp::LABEL, {IROperand::label(exitLabel)}, node->line);
        emitIR(IROp::POP, {cond}, node->line); // 循环退出路径：POP 消费条件值
    }
    // break 目标：循环体已清空栈，无需 POP
    emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
    loopStack_.pop_back();
    // BUG-IR-SCOPE-1 fix: 整个 for 语句的 block scope 在此退出，
    // 回收 initializer 声明的局部变量槽位并从 varMap_ 移除。
    if (inFunction_) {
        leaveBlockScope();
    } else {
        // 顶层 for 循环：对齐 Compiler.cpp visitForStmt L1131-L1147 的语义，
        // 循环退出后从 varMap_ 移除 initializer 声明的变量并发射 DELETE_VAR 清理全局。
        // 原实现仅对 inFunction_ 路径用 block scope 清理，顶层路径遗留 varMap_ 映射，
        // 导致 `for (var i...) {}` 后 `i` 仍可引用（与 StackVM/Interpreter 不一致）。
        std::vector<std::string> cleanupNames;
        // 收集 initializer 中声明的变量名（VarDecl 节点）
        if (node->initializer && node->initializer->nodeType == NodeType::NODE_VAR_DECL) {
            const auto* varDecl = static_cast<const VarDecl*>(node->initializer.get());
            cleanupNames.push_back(varDecl->name);
        }
        for (const auto& name : cleanupNames) {
            // 仅清理 name-based 全局变量（无预分配槽位），对齐 StackVM L1141 检查
            auto it = varMap_.find(name);
            if (it != varMap_.end() && it->second.kind == VarInfo::Kind::GLOBAL_NAME) {
                uint32_t nameIdx = it->second.index;
                emitIR(IROp::DELETE_VAR, {IROperand::global(nameIdx)}, node->line);
                varMap_.erase(it);
            }
        }
    }
}

IROperand AstIRBuilder::visitFunDecl(FunDecl* node) {
    // 限制1：完整闭包 upvalue 捕获实现
    // Bug 5 fix: 标记当前块作用域含嵌套函数（闭包）。
    // 仅 inFunction_==true 时（即嵌套函数声明，非顶层函数）才标记，
    // 使 leaveBlockScope 不回收本块局部槽位——闭包可能通过 upvalue 捕获了这些 slot。
    if (inFunction_ && !blockScopes_.empty()) {
        blockScopes_.back().hasNestedFunction = true;
    }

    // 1. 保存父函数编译状态到 FunctionEmitCtx（22 个字段）
    FunctionEmitCtx ctx;
    // R98 W3 fix: 提前生成 lambda 合成名并递增计数器，避免嵌套 lambda 在
    // emitFunctionPrologue（读计数器）与 emitFunctionClosureRegistration（递增计数器）
    // 之间产生重名。原实现两处分别读取/递增 lambdaCounter_，嵌套 lambda 共享同一计数器值
    // 导致 module_ 中 addFunction 重名覆盖（外层覆盖内层），运行时 MAKE_CLOSURE 找不到函数。
    if (node->name.empty()) {
        ctx.lambdaName = "$lambda_" + std::to_string(lambdaCounter_++);
    }
    ctx.savedIr = std::move(ir_);
    ctx.savedBlock = currentBlock_;
    ctx.savedVarMap = std::move(varMap_);
    ctx.savedVarTypes = std::move(varTypes_); // 2026-06-29: 类型注解快照
    ctx.savedInFunction = inFunction_;
    ctx.savedLocalSlot = nextLocalSlot_;
    ctx.savedLocalSlotNames = std::move(localSlotNames_); // BUG-IDE-12 fix
    ctx.savedSlotNameRanges = std::move(slotNameRanges_); // L1 fix
    ctx.savedLoopStack = std::move(loopStack_);
    ctx.savedBlockScopes = std::move(blockScopes_);
    ctx.savedBlockDepth = blockDepth_;
    ctx.savedTryDepth = tryDepth_;                          // BUG-EXC-2 fix
    ctx.savedTryFinallyStack = std::move(tryFinallyStack_); // L4 fix
    ctx.savedCompilingMethod = compilingMethod_;            // R7 fix
    // R98 t9 fix: 不能用 std::move——类方法的 compilingClassName_ 由 visitClassDecl 设置，
    // 方法体内 super 调用（visitMethodCall 行 2326）需读取此字段查找父类。
    // std::move 会清空 compilingClassName_，导致 SUPER_CALL 携带空类名，
    // VM/RegisterVM 报"类没有父类，不能使用 super"。改为复制保存，保持方法体内可读。
    ctx.savedCompilingClassName = compilingClassName_;
    ctx.savedCurrentFunctionReturnType = std::move(currentFunctionReturnType_); // BUG-TYPE-1 fix
    ctx.savedOuterLocalSlots = std::move(outerLocalSlots_);
    ctx.savedOuterUpvalueNames = std::move(outerUpvalueNames_);
    ctx.savedOuterFunctions = std::move(outerFunctions_);
    ctx.savedCurrentUpvalues = std::move(currentUpvalues_);
    ctx.savedCurrentUpvalueNames = std::move(currentUpvalueNames_);
    ctx.savedInnerFunctions = std::move(innerFunctions_);
    ctx.savedInnerFunctionSlots = std::move(innerFunctionSlots_);
    // R109 TCO: 保存父函数 TCO 状态（4 字段）。子函数编译期间 emitFunctionPrologue
    // 会覆盖这 4 字段为子函数的状态，restoreParentFunctionState 恢复为父函数状态。
    ctx.savedCurrentFunctionName = std::move(currentFunctionName_);
    ctx.savedCurrentFunctionDecl = currentFunctionDecl_;
    ctx.savedCurrentFunctionEntryLabel = currentFunctionEntryLabel_;
    ctx.savedCurrentFunctionIsMethod = currentFunctionIsMethod_;

    // 2-10. 编译子函数（异常安全：try/catch 保护 + 重抛前恢复父状态）
    // 异常安全：子函数编译可能抛出（bad_alloc/IR 构建错误），若不恢复 22 个 saved 字段，
    // 父函数状态将停留在 moved-from 状态。对齐 Compiler.cpp 的 CompileContextGuard RAII 模式。
    try {
        emitFunctionPrologue(*node, ctx); // 步骤 2-7：IRFunction 创建/参数注册/free var/默认值/entry block
        emitFunctionBody(*node, ctx);     // 步骤 8：编译函数体 + 隐式 RETURN_NULL
        emitFunctionEpilogue(*node, ctx); // 步骤 9-10：localCount/upvalues/addFunction
    } catch (...) {
        restoreParentFunctionState(ctx);
        throw;
    }

    // 11. 恢复父函数状态
    restoreParentFunctionState(ctx);

    // 12-13. 在父 IR 中 emit MAKE_CLOSURE + 注册该函数（供后续引用）
    // R98 W3: 返回 dest vreg——lambda 作为表达式时调用方需要此 vreg
    // R98 W3 fix: 传递 ctx.lambdaName（visitFunDecl 入口提前生成的合成名）
    return emitFunctionClosureRegistration(*node, ctx.lambdaName);
}

void AstIRBuilder::emitFunctionPrologue(FunDecl& node, FunctionEmitCtx& ctx) {
    // 步骤 2-7：新建子 IRFunction、设置外层捕获信息、注册参数、free var 分析、
    // 默认参数值、创建初始基本块。所有操作在子函数上下文中进行（inFunction_=true）。
    // R109 TCO: 在 compilingMethod_ 被下面 R7 fix 重置为 false 之前提前快照
    // "当前是否为类方法"——visitReturnStmt 据此拒绝 TCO（方法 slot 0 是 this）。
    currentFunctionIsMethod_ = compilingMethod_;
    ir_ = std::make_unique<IRFunction>();
    // R98 W3 fix: 使用 visitFunDecl 入口提前生成的 ctx.lambdaName（已递增计数器），
    // 保证嵌套 lambda 的 ir_->name 唯一。具名函数（ctx.lambdaName 为空）用 node.name。
    ir_->name = ctx.lambdaName.empty() ? node.name : ctx.lambdaName;
    ir_->arity = static_cast<int>(node.params.size());
    ir_->requiredArity = node.requiredParamCount;
    // R164 协程/生成器：复制生成器标记和 yieldCount 到 IR
    ir_->isGenerator = node.isGenerator;
    ir_->yieldCount = node.yieldCount;
    inFunction_ = true;
    nextLocalSlot_ = 0;
    varMap_.clear();
    varTypes_.clear();       // 2026-06-29: 清空子函数类型注解
    localSlotNames_.clear(); // BUG-IDE-12 fix: 清空 slot→name 映射
    slotNameRanges_.clear(); // L1 fix: 清空 IP 范围表
    loopStack_.clear();
    blockScopes_.clear();
    blockDepth_ = 0;
    tryDepth_ = 0;            // BUG-EXC-2 fix: 子函数内 try 嵌套从 0 开始
    tryFinallyStack_.clear(); // L4 fix: 子函数不继承外层 try-finally 上下文
    currentUpvalues_.clear();
    currentUpvalueNames_.clear();
    innerFunctions_.clear();
    innerFunctionSlots_.clear();

    // C-9 fix: 类方法有隐式 this 参数（由调用方传入 slot 0）。
    // 预留 slot 0 给 this，声明参数从 slot 1 开始；同步调整 arity/requiredArity
    // 使 executeCallImpl 的默认参数填充逻辑（argCount < arity 触发）正确工作。
    if (compilingMethod_) {
        nextLocalSlot_ = 1;
        varMap_["this"] = {VarInfo::Kind::LOCAL, 0};
        // BUG-IDE-12 fix: 记录 slot 0 → "this"
        localSlotNames_.resize(1);
        localSlotNames_[0] = "this";
        ir_->arity += 1;
        ir_->requiredArity += 1;
        // L7 fix: 不将字段注册为 LOCAL slot。改为在 resolveVar 中将方法体内裸字段访问
        //   解析为 MEMBER（this.field → MEMBER_GET/MEMBER_SET on this slot 0）。
        //   原因：StackVM 方法帧布局为 [this | field0..N | args | locals]（caller 推字段槽），
        //   RegisterVM 方法帧布局为 [this | args | locals]（字段通过 this.field 访问）。
        //   若注册为 LOCAL slot，两后端帧布局不一致：StackVM 推字段值到栈槽，RegisterVM
        //   不推导致 reg[1..N] 错位（arg0 被当作 field0）。MEMBER_GET on this 对两后端统一：
        //   StackVM lowering → OP_MEMBER_GET，RegisterVM lowering → REG_MEMBER_GET，均从实例对象
        //   读取字段，不依赖帧布局。对齐 Compiler.cpp 直接路径的 varMap_["this"] = LOCAL(0) +
        //   方法体内 this.field 显式访问（直接路径字段虽注册为 LOCAL，但 StackVM 推字段槽；
        //   IR 路径不推字段槽故不能用 LOCAL）。
        // R7 fix: 处理完类方法 this 预留后立即重置 compilingMethod_。
        // 这样方法体内声明的嵌套函数（递归 visitFunDecl）不会误被当作方法处理
        // （避免嵌套函数预留 slot 0 给 this、arity += 1 导致参数错位）。
        // visitClassDecl 每次调用 visitFunDecl 前会重新设置 compilingMethod_=true。
        // 注意：compilingClassName_ 保留，供方法体内 super 调用查找父类使用。
        compilingMethod_ = false;
    }

    // 3. 设置外层局部/upvalue 信息（供 addUpvalue 查找）
    //    outerLocalSlots_ = 父函数的 LOCAL 变量（子函数可捕获为 isLocal=true）
    //    outerUpvalueNames_ = 父函数的 UPVALUE（子函数可透传为 isLocal=false）
    outerLocalSlots_.clear();
    outerUpvalueNames_.clear();
    for (const auto& kv : ctx.savedVarMap) {
        if (kv.second.kind == VarInfo::Kind::LOCAL) {
            outerLocalSlots_[kv.first] = static_cast<int>(kv.second.index);
        } else if (kv.second.kind == VarInfo::Kind::UPVALUE) {
            outerUpvalueNames_[kv.first] = static_cast<int>(kv.second.index);
        }
    }
    outerFunctions_.clear();

    // 4. 参数注册为 LOCAL slot 0..n-1
    for (size_t i = 0; i < node.params.size(); ++i) {
        uint32_t slot = nextLocalSlot_++;
        varMap_[node.params[i]] = {VarInfo::Kind::LOCAL, slot};
        // BUG-IDE-12 fix: 记录参数 slot→name
        if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
            localSlotNames_.resize(slot + 1);
        }
        localSlotNames_[slot] = node.params[i];
    }
    // R164 fixup2: 缓存参数名，供 visitBlock 进入函数体块时注入 declaredNames，
    // 使 visitVarDecl 能检测参数重定义（对齐 Interpreter tryDefineNew 行为）
    pendingFunctionParams_ = node.params;

    // CRITICAL-1 fix: 前向自由变量分析。在编译函数体前，先收集所有自由变量，
    // 为每个能在外层捕获的变量预建 upvalue。这确保中间函数即使不直接引用某变量，
    // 也会捕获它供更内层函数透传（对齐 Interpreter 的 computeFreeVariables 传播逻辑）。
    // 仅对有外层作用域的嵌套函数执行（顶层函数的自由变量都是全局，无需 upvalue）。
    if (inFunction_ || blockDepth_ > 0 || !outerLocalSlots_.empty() || !outerUpvalueNames_.empty()) {
        auto freeVars = computeFreeVars(node);
        for (const auto& name : freeVars) {
            // addUpvalue 仅在 outerLocalSlots_/outerUpvalueNames_ 命中时才真正捕获。
            // 预建后立即写入 varMap_，使后续嵌套函数的 savedVarMap 能传播 UPVALUE 信息，
            // 否则 inner 的 outerUpvalueNames_ 不会有 x（mid 的 upvalue 未记录到 varMap_）。
            addUpvalue(name);
            auto uvNameIt = currentUpvalueNames_.find(name);
            if (uvNameIt != currentUpvalueNames_.end()) {
                varMap_[name] = {VarInfo::Kind::UPVALUE, static_cast<uint32_t>(uvNameIt->second)};
            }
            // 未命中的（全局变量/全局函数）不写入 varMap_，resolveVar 后续走 GLOBAL_NAME
        }
    }

    // 5. 编译默认参数值（限制4）
    //    对每个非 null 的默认值表达式，尝试用 extractConstant 提取常量
    //    成功则 addConstant 记录索引；失败则记录 NO_INDEX（表示复杂表达式，不支持）
    for (auto& dv : node.defaultValues) {
        if (dv) {
            Value constVal;
            if (extractConstant(dv.get(), constVal)) {
                uint32_t constIdx = ir_->addConstant(constVal);
                ir_->defaultConstIndices.push_back(static_cast<uint16_t>(constIdx));
            } else {
                ir_->defaultConstIndices.push_back(RuntimeLimits::NO_INDEX);
            }
        }
    }

    // 6. 创建初始基本块
    uint32_t entryLabel = ir_->allocLabel();
    currentBlock_ = &ir_->addBlock(entryLabel);
    currentBlock_->instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(entryLabel)},
                                             node.line);

    // R109/L15 TCO: 设置子函数 TCO 状态（currentFunctionIsMethod_ 已在函数入口提前快照）。
    // visitReturnStmt 据此判断 return f(args) / return this.method(args) 是否可优化为
    // "参数赋值 + JUMP 函数入口 label"。
    //   - currentFunctionName_: 用于匹配 FunCall.name / MethodCall.methodName
    //     L15: 类方法名经 visitClassDecl 改写为 "ClassName.method"，此处剥离前缀
    //     只保留简单方法名，使 SelfMethod 匹配 methodCall->methodName == currentFunctionName_ 成立。
    //   - currentFunctionDecl_: 提供 params 列表（检查参数数量 == 形参数量）
    //   - currentFunctionEntryLabel_: 函数体入口 basic block 的 label
    std::string nameForTCO = ctx.lambdaName.empty() ? node.name : ctx.lambdaName;
    if (currentFunctionIsMethod_) {
        // L15: 剥离 "ClassName." 前缀，使方法自调用 return this.method(args) 能匹配
        size_t dotPos = nameForTCO.rfind('.');
        if (dotPos != std::string::npos) {
            nameForTCO = nameForTCO.substr(dotPos + 1);
        }
    }
    currentFunctionName_ = nameForTCO;
    currentFunctionDecl_ = &node;
    currentFunctionEntryLabel_ = entryLabel;
}

void AstIRBuilder::emitFunctionBody(FunDecl& node, FunctionEmitCtx& /*ctx*/) {
    // 步骤 7-8：编译函数体 + 末尾隐式 RETURN_NULL
    // BUG-TYPE-1 fix (P1): 设置当前函数返回类型注解，供 visitReturnStmt 发射 TYPE_CHECK
    currentFunctionReturnType_ = node.returnType;
    // R163 泛型扩展：设置当前函数的类型参数，供 emitTypeCheckIR 擦除类型参数注解。
    // 类方法合并类泛型参数 + 方法泛型参数，对齐 Compiler/Interpreter。
    // 注意：使用 currentFunctionIsMethod_ 而非 compilingMethod_，因为 emitFunctionPrologue
    // 已在 L1613 将 compilingMethod_ 重置为 false，但 currentFunctionIsMethod_ 在重置前
    // 已快照（L1575: currentFunctionIsMethod_ = compilingMethod_）。
    if (currentFunctionIsMethod_) {
        currentTypeParams_ = compilingClassTypeParams_;
        for (const auto& tp : node.typeParams) {
            currentTypeParams_.push_back(tp);
        }
    } else {
        currentTypeParams_ = node.typeParams;
    }
    if (node.body)
        visitNode(node.body.get());
    // R164 fixup2: 清除未消费的 pendingFunctionParams_（函数体为空时不会被 visitBlock 消费）
    pendingFunctionParams_.clear();

    // 8. 末尾隐式 RETURN_NULL
    emitIR(IROp::RETURN_NULL, {}, node.line);
}

void AstIRBuilder::emitFunctionEpilogue(FunDecl& /*node*/, FunctionEmitCtx& /*ctx*/) {
    // 步骤 9-10：设置 localCount 和 upvalues，将子 IRFunction 添加到 module_
    // Bug 5 fix: 用 max 更新而非直接赋值。leaveBlockScope 回收 slot 后 nextLocalSlot_ 可能
    // 低于实际峰值，但已 emit 的字节码仍引用被回收的 slot 编号，VM 帧需容纳所有曾使用的 slot。
    if (static_cast<int>(nextLocalSlot_) > ir_->localCount) {
        ir_->localCount = static_cast<int>(nextLocalSlot_);
        // P2-1 fix: 同 build()，对 visitFunDecl 路径的 localCount 更新做上限告警
        if (ir_->localCount > 32) {
            Logger::Error("AstIRBuilder: localCount 超出 32 寄存器上限 (" + std::to_string(ir_->localCount) +
                              "，函数 " + ir_->name + ")，RegisterVM 将无法加载此函数",
                          "IR");
        }
    }
    ir_->upvalues.clear();
    for (const auto& uv : currentUpvalues_) {
        ir_->upvalues.push_back({uv.outerIdx, uv.isLocal, uv.name}); // 条件断点修复(R75): 传递name
    }
    // BUG-IDE-12 fix: 保存 slot→name 映射到 IRFunction，供 RegisterBytecodeBackend 复制到 chunk
    ir_->localSlotNames = localSlotNames_;
    // L1 fix: 保存 IP 范围表到 IRFunction，供 lowering 时翻译为字节码 IP 范围
    ir_->slotNameRanges = slotNameRanges_;

    // 10. 将子 IRFunction 添加到 module_（不再丢弃 childIr）
    module_->addFunction(std::move(ir_));
}

void AstIRBuilder::restoreParentFunctionState(FunctionEmitCtx& ctx) {
    // 恢复父函数的 22 个成员状态，避免 moved-from 状态被上层复用。
    // 被 visitFunDecl 的 catch 路径和正常完成路径共用（消除原 15+7 字段恢复代码的重复）。
    ir_ = std::move(ctx.savedIr);
    currentBlock_ = ctx.savedBlock;
    varMap_ = std::move(ctx.savedVarMap);
    varTypes_ = std::move(ctx.savedVarTypes); // 2026-06-29
    inFunction_ = ctx.savedInFunction;
    nextLocalSlot_ = ctx.savedLocalSlot;
    localSlotNames_ = std::move(ctx.savedLocalSlotNames); // BUG-IDE-12 fix
    slotNameRanges_ = std::move(ctx.savedSlotNameRanges); // L1 fix
    loopStack_ = std::move(ctx.savedLoopStack);
    blockScopes_ = std::move(ctx.savedBlockScopes);
    blockDepth_ = ctx.savedBlockDepth;
    tryDepth_ = ctx.savedTryDepth;                          // BUG-EXC-2 fix
    tryFinallyStack_ = std::move(ctx.savedTryFinallyStack); // L4 fix
    compilingMethod_ = ctx.savedCompilingMethod;            // R7 fix
    compilingClassName_ = std::move(ctx.savedCompilingClassName);
    currentFunctionReturnType_ = std::move(ctx.savedCurrentFunctionReturnType); // BUG-TYPE-1 fix
    currentTypeParams_ = std::move(ctx.savedCurrentTypeParams);                 // R163 泛型扩展
    compilingClassTypeParams_ = std::move(ctx.savedCompilingClassTypeParams);   // R163 泛型扩展
    outerLocalSlots_ = std::move(ctx.savedOuterLocalSlots);
    outerUpvalueNames_ = std::move(ctx.savedOuterUpvalueNames);
    outerFunctions_ = std::move(ctx.savedOuterFunctions);
    currentUpvalues_ = std::move(ctx.savedCurrentUpvalues);
    currentUpvalueNames_ = std::move(ctx.savedCurrentUpvalueNames);
    innerFunctions_ = std::move(ctx.savedInnerFunctions);
    innerFunctionSlots_ = std::move(ctx.savedInnerFunctionSlots);
    // R109 TCO: 恢复父函数 TCO 状态（4 字段）。子函数编译期间这 4 字段被子函数
    // 覆盖，编译完成后必须复原，否则父函数后续 return 会误判为子函数的 TCO。
    currentFunctionName_ = std::move(ctx.savedCurrentFunctionName);
    currentFunctionDecl_ = ctx.savedCurrentFunctionDecl;
    currentFunctionEntryLabel_ = ctx.savedCurrentFunctionEntryLabel;
    currentFunctionIsMethod_ = ctx.savedCurrentFunctionIsMethod;
}

IROperand AstIRBuilder::emitFunctionClosureRegistration(FunDecl& node, const std::string& overrideName) {
    // 步骤 12-13：在父 IR 中 emit MAKE_CLOSURE + 注册该函数（供后续引用）。
    // 在父函数上下文中执行（ir_ 已由 restoreParentFunctionState 恢复为父 IRFunction）。
    // 12. emit MAKE_CLOSURE（含 upvalue 描述符列表）
    //     编码：operands [dest, name_idx, uv_count, isLocal1, idx1, isLocal2, idx2, ...]
    // R98 W3 fix: 使用 visitFunDecl 入口提前生成的 overrideName（已递增计数器），
    // 避免嵌套 lambda 在 emitFunctionPrologue（读计数器）与 emitFunctionClosureRegistration
    // （递增计数器）之间产生重名。原实现两处分别读取/递增 lambdaCounter_，嵌套 lambda
    // 共享同一计数器值导致 module_ 中 addFunction 重名覆盖（外层覆盖内层），
    // 运行时 MAKE_CLOSURE 在 functionClosures_ 中找不到内层 lambda 报错。
    std::string fnName = overrideName.empty() ? node.name : overrideName;
    bool isLambda = fnName.empty() || fnName.rfind("$lambda_", 0) == 0;
    uint32_t nameIdx = ir_->addGlobal(fnName);
    IROperand dest = ir_->allocVReg();
    IRFunction* fnIr = module_->findFunction(fnName);
    uint32_t uvCount = fnIr ? static_cast<uint32_t>(fnIr->upvalues.size()) : 0;
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::funcName(nameIdx));
    ops.push_back(IROperand::imm(uvCount));
    if (fnIr) {
        for (const auto& uv : fnIr->upvalues) {
            ops.push_back(IROperand::imm(uv.isLocal ? 1 : 0));
            ops.push_back(IROperand::imm(static_cast<uint32_t>(uv.index)));
        }
    }
    emitIR(IROp::MAKE_CLOSURE, ops, node.line);

    // R98 W3: 匿名 lambda 不存储到任何变量槽——闭包值留在 vreg dest 中作为表达式结果。
    // 调用方（如 var f = fun(x){...}; 或 map(arr, fun(x){...});）从 dest 取值。
    // MAKE_CLOSURE 在 StackVM lowering 中 push 到栈，RegisterVM lowering 中写入 dst reg，
    // 栈/寄存器平衡由调用方负责（varDecl 的 STORE_LOCAL/DEFINE_GLOBAL 会消费）。
    if (isLambda) {
        return dest;
    }

    // 13. 在父函数中注册该函数（供后续引用）
    if (inFunction_) {
        // 函数内：注册为 LOCAL，并把闭包值从 dest vreg 写入对应 slot，
        // 否则后续 LOAD_LOCAL 读到未初始化的槽（CRITICAL-1 fix 配套——
        // visitFunCall 现在通过 LOAD_LOCAL + CALL_EXPR 调用嵌套闭包）。
        uint32_t slot = nextLocalSlot_++;
        varMap_[fnName] = {VarInfo::Kind::LOCAL, slot};
        // BUG-IDE-12 fix: 记录 slot→name 映射
        if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
            localSlotNames_.resize(slot + 1);
        }
        localSlotNames_[slot] = fnName;
        // L1 fix: 记录内嵌函数变量到 slotNameRanges_（endInstr=0，函数级变量由 fallback 处理）
        size_t curIdx = 0;
        for (const auto& blk : ir_->blocks)
            curIdx += blk.instructions.size();
        slotNameRanges_.push_back({slot, fnName, curIdx, 0});
        innerFunctions_.insert(fnName);
        innerFunctionSlots_[fnName] = static_cast<int>(slot);
        emitIR(IROp::STORE_LOCAL, {IROperand::local(slot), dest}, node.line);
        // STORE_LOCAL → OP_SET_LOCAL（peek 不 pop），MAKE_CLOSURE 的值残留在栈上。
        // 需 POP 消费，与顶层路径的 POP 对齐，防止栈泄漏。
        emitIR(IROp::POP, {dest}, node.line);
    } else {
        // 顶层：注册为全局（有槽位则 GLOBAL_SLOT，否则 GLOBAL_NAME）
        // 顶层函数无外层作用域，MAKE_CLOSURE 创建的闭包 upvalue 列表为空，
        // visitFunCall 命中 GLOBAL_* 时走 CALL 命名调用，由 functionChunks_/
        // functionClosures_ 查找。但 R98 W2 高阶函数 map/filter/reduce/forEach/find
        // 需要将函数名作为值参数传递（如 map(arr, double)），visitVarRef 会编译为
        // LOAD_GLOBAL <slot>，若全局槽位为 null（闭包值被 POP 丢弃），运行时取到 null
        // 导致 invokeClosureSync 失败。修复：将闭包值 DEFINE_GLOBAL 到全局槽位，
        // 使函数名可作为值引用（对齐 Compiler.cpp visitFunDecl 的 R98 W2 fix）。
        int slot = lookupGlobalSlot(fnName);
        if (slot >= 0) {
            varMap_[fnName] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot)};
            // R98 W2 fix: 顶层函数闭包值存储到全局槽位（替代原 POP 丢弃）。
            // DEFINE_GLOBAL 在 StackVM lowering 中用 OP_DEFINE_GLOBAL（pop），
            // 在 RegisterVM lowering 中用 REG_STORE_GLOBAL（写寄存器），栈/寄存器平衡已处理。
            emitIR(IROp::DEFINE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(slot)), dest}, node.line);
        } else {
            uint32_t idx = ir_->addGlobal(fnName);
            varMap_[fnName] = {VarInfo::Kind::GLOBAL_NAME, idx};
            // 无全局槽位时退化为 POP 保持向后兼容（与原行为一致）
            emitIR(IROp::POP, {dest}, node.line);
        }
        // R103 W1 fix: 记录顶层 FunDecl 名字，供 CRITICAL-1 fix 扩展 GLOBAL_SLOT
        // 检查时排除——即使闭包值已存入全局槽位，直接调用 double(5) 仍应走 CALL 命名调用
        // （functionClosures_ 查找，携带 upvalue 绑定），而非 LOAD_GLOBAL + CALL_EXPR
        // （CALL_EXPR 路径的 invokeClosureSync 无法处理模块导入闭包等复杂场景）。
        // 仅 var 持闭包值的 GLOBAL_SLOT 走 CALL_EXPR。
        topLevelFunDeclNames_.insert(fnName);
    }
    // R98 W3: 具名函数返回 vreg(0) 哨兵（具名函数不作为表达式求值）
    return IROperand::vreg(0);
}

// R164 协程/生成器：yield 表达式 → IR YIELD 指令
// 语义：编译 yield 值表达式（无值时 LOAD_NULL），emit YIELD dest, src
// 后端 lowering 时：
//   - StackVM IR 路径：lower 到 OP_YIELD（1 字节无操作数，值在栈顶）
//   - RegisterVM 路径：lower 到 REG_YIELD dst, src（3 字节）
// 重放模式：VM 执行 YIELD 时比较运行时计数器与目标 yieldId，
//   命中目标则抛出 VMYieldSignal 返回 yield 值；否则 dest = src 继续执行。
IROperand AstIRBuilder::visitYieldExpr(YieldExpr* node) {
    IROperand dest = ir_->allocVReg();
    IROperand src;
    if (node->value) {
        src = visitNode(node->value.get());
    } else {
        src = ir_->allocVReg();
        emitIR(IROp::LOAD_NULL, {src}, node->line);
    }
    emitIR(IROp::YIELD, {dest, src}, node->line);
    return dest;
}

// 七特性 MVP 阶段 4：await 表达式 → IR AWAIT 指令
// 语义：编译 operand，emit AWAIT dest, src（同步 drain）。
// 后端 lowering：
//   - StackVM IR 路径：lower 到 OP_AWAIT（1 字节无操作数，值在栈顶）
//   - RegisterVM 路径：lower 到 REG_AWAIT dst, src（3 字节）
IROperand AstIRBuilder::visitAwaitExpr(AwaitExpr* node) {
    IROperand dest = ir_->allocVReg();
    IROperand src = visitNode(node->operand.get());
    emitIR(IROp::AWAIT, {dest, src}, node->line);
    return dest;
}

IROperand AstIRBuilder::visitFunCall(FunCall* node) {
    IROperand dest = ir_->allocVReg();
    if (node->callee) {
        // 表达式调用：编译 callee + 参数，emit CALL_EXPR
        IROperand calleeVreg = visitNode(node->callee.get());
        std::vector<IROperand> ops;
        ops.push_back(dest);
        ops.push_back(calleeVreg);
        ops.push_back(IROperand::imm(static_cast<uint32_t>(node->arguments.size())));
        for (auto& arg : node->arguments) {
            ops.push_back(visitNode(arg.get()));
        }
        emitIR(IROp::CALL_EXPR, ops, node->line);
        return dest;
    }
    // CRITICAL-1 fix: 命名调用若 name 解析为 LOCAL 或 UPVALUE（嵌套闭包值），
    // 必须走 CALL_EXPR 通过闭包值调用——MAKE_CLOSURE 创建的闭包存储在局部/upvalue 槽中,
    // 携带 upvalue 绑定。若走 CALL 命名调用，RegisterVM 的 functionClosures_ 不含
    // 嵌套闭包（仅栈式 VM MAKE_CLOSURE 才注册到 functionClosures_），且即便注册了
    // 也无法处理同名嵌套闭包按帧隔离的情况（对齐 Compiler.cpp H5 fix 的实现策略）。
    //
    // R103 W1 fix 扩展：追加 GLOBAL_SLOT 检查，覆盖顶层 var 持闭包值调用场景
    // （如 `var g = makeAdder(10); g(32);`）。原 CRITICAL-1 fix 仅覆盖 LOCAL/UPVALUE，
    // 顶层 var 持闭包值调用时 IR 路径走 CALL 命名调用，但 functionChunks_ 不含 var
    // 持有的闭包，报"未定义的函数: g"。StackVM 直接路径已通过 W1 fix 扩展解决，
    // 此处对齐。
    // 关键排除：topLevelFunDeclNames_ 中的名字（顶层 FunDecl）不走 CALL_EXPR——
    // 其全局槽位为 null（闭包值被 POP 丢弃），必须走 CALL 命名调用通过 functionChunks_
    // 查找。仅 var 持闭包值的 GLOBAL_SLOT 走 CALL_EXPR。
    auto varIt = varMap_.find(node->name);
    if (varIt != varMap_.end() &&
        (varIt->second.kind == VarInfo::Kind::LOCAL || varIt->second.kind == VarInfo::Kind::UPVALUE ||
         (varIt->second.kind == VarInfo::Kind::GLOBAL_SLOT &&
          topLevelFunDeclNames_.find(node->name) == topLevelFunDeclNames_.end()))) {
        IROperand calleeVreg = emitLoadVar(node->name, node->line);
        std::vector<IROperand> ops;
        ops.push_back(dest);
        ops.push_back(calleeVreg);
        ops.push_back(IROperand::imm(static_cast<uint32_t>(node->arguments.size())));
        for (auto& arg : node->arguments) {
            ops.push_back(visitNode(arg.get()));
        }
        emitIR(IROp::CALL_EXPR, ops, node->line);
        return dest;
    }
    // 全局命名调用：emit CALL nameIdx argCount
    uint32_t nameIdx = ir_->addGlobal(node->name);
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::funcName(nameIdx));
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->arguments.size())));
    for (auto& arg : node->arguments) {
        ops.push_back(visitNode(arg.get()));
    }
    emitIR(IROp::CALL, ops, node->line);
    return dest;
}

void AstIRBuilder::visitReturnStmt(ReturnStmt* node) {
    // R109/L15 TCO: 尾调用优化。识别 return f(args) 或 return this.method(args) 形态的
    // 自递归调用，编译为"参数求值 → vreg → 逆序 STORE_LOCAL 覆盖参数槽 + POP 清栈 → JUMP 函数入口 label"。
    // 跳过 RETURN 的帧弹出，复用当前帧执行下一轮递归，深度无界。
    //
    // L15 扩展（与 Compiler.cpp StackVM 直接路径严格对齐）：
    //   - SelfFunction: 放宽 upvalue 限制（同函数重用闭包，upvalue 指向外层栈不变）
    //   - SelfFunction: 支持默认参数（args.size() < params.size() 时用默认值填充）
    //   - SelfMethod: 类方法自调用 return this.method(args)
    //     IR 路径方法帧布局为 [this | args | locals]（字段通过 this.field 访问，不占 slot），
    //     故 paramSlotBase = 1（仅跳过 slot 0 = this）。
    //
    // 仍要求的条件：
    //   (1) AST 结构匹配（TCO::identifyTailCall 返回非 None）
    //   (2) 不在 try 块内（tryDepth_ == 0）
    //   (3) currentFunctionDecl_ 非空（用于获取形参/默认值）
    //   (4) args.size() <= params.size()（超出视为非尾调用）
    //
    // 三后端 lowering 行为：
    //   - StackVM 后端：STORE_LOCAL → OP_SET_LOCAL（peek(0) 不消费），POP → OP_POP（清栈）
    //   - RegisterVM 后端：STORE_LOCAL → REG_MOVE slot_reg, src_reg，POP → no-op（无栈）
    // 两后端语义等价：参数槽被覆盖，栈深度恢复（StackVM）或无栈概念（RegisterVM）。
    TCO::TailCallInfo tco = TCO::identifyTailCall(node, currentFunctionName_, currentFunctionIsMethod_);
    // B1-Shadow fix（与直接路径对齐）：SelfFunction 名称被本函数局部变量/参数遮蔽
    // （var f = other; return f(x);）时不做 TCO——varMap_ 中 Kind::LOCAL 即被遮蔽；
    // UPVALUE 是嵌套函数自引用（合法 TCO），GLOBAL_* 是未遮蔽的全局自递归。
    if (tco.kind == TCO::TailCallInfo::Kind::SelfFunction) {
        auto shadowIt = varMap_.find(tco.call->name);
        if (shadowIt != varMap_.end() && shadowIt->second.kind == VarInfo::Kind::LOCAL) {
            tco.kind = TCO::TailCallInfo::Kind::None;
        }
    }
    // L18 eng-tailcall: 互递归/一般尾调用 return g(args)（与直接路径对齐）。
    // 策略：正常 visitNode 编译调用（继承遮蔽/upvalue/全局分流），若当前块
    // 尾部恰为同名 CALL 则原地改写为 TAIL_CALL（operands 布局相同），再 emit
    // RETURN dest。后端 lowering 为 OP_TAIL_CALL/REG_TAIL_CALL，运行时帧复用或降级。
    // 限制条件与直接路径一致：非方法体/无返回类型注解/非生成器/不在 try 内。
    if (tco.kind == TCO::TailCallInfo::Kind::GeneralCall && tryDepth_ == 0 && inFunction_ &&
        !currentFunctionIsMethod_ && currentFunctionReturnType_.empty() &&
        (currentFunctionDecl_ == nullptr || !currentFunctionDecl_->isGenerator)) {
        IROperand val = visitNode(node->value.get());
        auto& instrs = currentBlock_->instructions;
        if (!instrs.empty()) {
            IRInstruction& last = instrs.back();
            if (last.op == IROp::CALL && last.operands.size() >= 3 &&
                last.operands[1].index < ir_->globalNames.size() &&
                ir_->globalNames[last.operands[1].index] == tco.call->name &&
                last.operands[2].index == tco.call->arguments.size() && !last.operands.empty() &&
                last.operands[0].kind == IROperandKind::VIRTUAL && last.operands[0].index == val.index) {
                last.op = IROp::TAIL_CALL;
            }
        }
        emitIR(IROp::RETURN, {val}, node->line);
        return;
    }
    if (tco.kind == TCO::TailCallInfo::Kind::GeneralCall) {
        tco.kind = TCO::TailCallInfo::Kind::None; // 条件不满足：回退普通 return 路径
    }
    if (tco.kind != TCO::TailCallInfo::Kind::None && tryDepth_ == 0 && currentFunctionDecl_ != nullptr) {
        const size_t paramCount = currentFunctionDecl_->params.size();
        // IR 路径方法帧布局：slot 0 = this，slot 1.. = params（字段不占 slot）
        // SelfFunction: paramSlotBase = 0；SelfMethod: paramSlotBase = 1
        size_t paramSlotBase = (tco.kind == TCO::TailCallInfo::Kind::SelfMethod) ? 1 : 0;
        const std::vector<std::shared_ptr<ASTNode>>* args = nullptr;
        if (tco.kind == TCO::TailCallInfo::Kind::SelfFunction) {
            args = &tco.call->arguments;
        } else { // SelfMethod
            args = &tco.methodCall->arguments;
        }
        if (args->size() <= paramCount && paramSlotBase + paramCount <= 255) {
            // 1. 编译所有实参表达式到 vreg（StackVM lowering 后压栈 N 个值）
            std::vector<IROperand> argVregs;
            argVregs.reserve(paramCount);
            for (auto& arg : *args) {
                argVregs.push_back(visitNode(arg.get()));
            }
            // L15: 缺失参数用默认值或 null 填充
            for (size_t i = args->size(); i < paramCount; ++i) {
                if (i < currentFunctionDecl_->defaultValues.size() && currentFunctionDecl_->defaultValues[i]) {
                    argVregs.push_back(visitNode(currentFunctionDecl_->defaultValues[i].get()));
                } else {
                    IROperand nullVreg = ir_->allocVReg();
                    emitIR(IROp::LOAD_NULL, {nullVreg}, node->line);
                    argVregs.push_back(nullVreg);
                }
            }
            // 2. 逆序 STORE_LOCAL + POP 覆盖参数槽并清栈
            //    IR 操作数顺序：[slot, src_vreg]（与 visitVarDecl/emitStoreVar 一致）
            //    POP 紧跟 STORE_LOCAL：StackVM 后端 OP_SET_LOCAL 用 peek(0) 不消费栈顶，
            //    需 OP_POP 显式清栈；RegisterVM 后端 POP 是 no-op（无栈）。
            //
            //    迭代必须逆序（i = paramCount-1 → 0）：
            //    实参压栈后栈布局为 [val_0, ..., val_{N-1}]（val_{N-1} 在栈顶）。
            //    StackVM 后端 STORE_LOCAL lowers to OP_SET_LOCAL（peek(0) 读栈顶），
            //    正向迭代会把 val_{N-1-i} 赋给 slot i（参数顺序反转）。
            //    逆序迭代保证 slot i ← val_i（栈顶对应最后一个参数，先弹出赋值）。
            //    RegisterVM 后端用 REG_MOVE slot_reg, src_vreg_reg（直接寄存器拷贝），
            //    无栈顺序问题，逆序/正序均正确。
            // AUDIT-R7 F3 fix: 覆盖前先关闭 slot >= paramSlotBase 的 open upvalues（与
            // 直接路径对齐）——帧复用前让本轮闭包快照当轮捕获值（三后端一致性）。
            emitIR(IROp::CLOSE_UPVALUE, {IROperand::imm(static_cast<uint32_t>(paramSlotBase))}, node->line);
            for (size_t i = paramCount; i > 0; --i) {
                size_t idx = i - 1;
                emitIR(IROp::STORE_LOCAL, {IROperand::local(static_cast<uint32_t>(paramSlotBase + idx)), argVregs[idx]},
                       node->line);
                emitIR(IROp::POP, {argVregs[idx]}, node->line);
            }
            // 3. 跳转到函数入口 basic block（JUMP+LABEL 机制，由 BytecodeIRBackend/RegisterBytecodeBackend
            //    lowering 为 OP_JUMP/REG_JUMP 绝对跳转 + 第二遍回填 label→字节码偏移）
            emitIR(IROp::JUMP, {IROperand::label(currentFunctionEntryLabel_)}, node->line);
            return;
        }
    }
    if (node->value) {
        IROperand val = visitNode(node->value.get());
        // BUG-TYPE-1 fix (P1): 函数有返回类型注解时，在 RETURN 前发射 TYPE_CHECK IR。
        // 对齐 Interpreter::visitReturnStmt 的 checkType + Compiler::visitReturnStmt 的 OP_TYPE_CHECK。
        // R109 TCO: TCO 路径跳过 TYPE_CHECK 是安全的——递归调用的 return 会再次触发检查。
        if (!currentFunctionReturnType_.empty()) {
            emitTypeCheckIR(val, currentFunctionReturnType_, node->line);
        }
        // L4 fix: return 在 try-finally 块内时，续跳到 finally 入口执行 finally 块。
        // IR 路径用 label 机制：分配 returnLandingLabel，emitFinallyJumpIR 续跳到 finally 入口，
        // finally 末尾 FINALLY_END 续跳到 returnLandingLabel，执行 RETURN。
        // vreg 是静态分配的，finally 块不影响 val vreg 的值（与 StackVM 栈顶返回值对齐）。
        uint32_t returnLandingLabel = ir_->allocLabel();
        if (emitFinallyJumpIR(node->line, returnLandingLabel)) {
            emitIR(IROp::LABEL, {IROperand::label(returnLandingLabel)}, node->line);
        }
        emitIR(IROp::RETURN, {val}, node->line);
    } else {
        // L4 fix: return; (无值) 在 try-finally 块内时也需续跳执行 finally。
        uint32_t returnLandingLabel = ir_->allocLabel();
        if (emitFinallyJumpIR(node->line, returnLandingLabel)) {
            emitIR(IROp::LABEL, {IROperand::label(returnLandingLabel)}, node->line);
        }
        emitIR(IROp::RETURN_NULL, {}, node->line);
    }
}

void AstIRBuilder::visitPrintStmt(PrintStmt* node) {
    // BUG-AUDIT-PRINT-MULTI fix: 多值 print 必须用 ADD 拼接为单字符串后发射单个 PRINT，
    // 与 Compiler.cpp 直接路径和 Interpreter 路径对齐（空格分隔，单次 output 带换行）。
    // 原实现逐个值 emit PRINT 导致 IR 路径输出多行（每个值后跟换行），
    // 与 Interpreter/StackVM 直接路径的 "v1 v2 v3\n" 单行输出不一致。
    if (node->values.empty()) {
        // print() → 输出空行（与解释器 output("") 一致）
        IROperand emptyStr = emitConst(Value(std::string("")), node->line);
        emitIR(IROp::PRINT, {emptyStr}, node->line);
        return;
    }
    if (node->values.size() == 1) {
        IROperand val = visitNode(node->values[0].get());
        emitIR(IROp::PRINT, {val}, node->line);
        return;
    }
    // 多值：acc = values[0]; for each subsequent: acc = acc + " " + values[i]
    // ADD 已支持 string+non-string 拼接（与 Compiler.cpp OP_ADD 链一致）
    // 栈平衡说明：IR lowering 中 ADD 依赖操作数在栈顶（仅 emit OP_ADD 不重排栈），
    // 故每次迭代需重新 emitConst(" ") 把空格压栈，不能复用上一次的 space vreg
    // （上一次 ADD 已消费栈上的 space 值）。
    IROperand acc = visitNode(node->values[0].get());
    for (size_t i = 1; i < node->values.size(); ++i) {
        // acc = acc + " "（每次迭代重新加载空格常量到栈顶）
        IROperand space = emitConst(Value(std::string(" ")), node->line);
        IROperand sum1 = ir_->allocVReg();
        emitIR(IROp::ADD, {sum1, acc, space}, node->line);
        // acc = sum1 + next
        IROperand next = visitNode(node->values[i].get());
        IROperand sum2 = ir_->allocVReg();
        emitIR(IROp::ADD, {sum2, sum1, next}, node->line);
        acc = sum2;
    }
    emitIR(IROp::PRINT, {acc}, node->line);
}

void AstIRBuilder::visitBlock(Block* node) {
    // 限制5：函数内时 enterBlockScope + 编译语句 + leaveBlockScope
    if (inFunction_) {
        enterBlockScope();
        // R164 fixup2: 函数体块进入时注入参数名到 declaredNames，
        // 使 visitVarDecl 能检测参数重定义（pendingFunctionParams_ 由 visitFunDecl 填充）
        if (!pendingFunctionParams_.empty() && !blockScopes_.empty()) {
            for (const auto& param : pendingFunctionParams_) {
                blockScopes_.back().declaredNames.insert(param);
            }
            pendingFunctionParams_.clear();
        }
        for (auto& s : node->statements) {
            if (!s)
                continue;
            IROperand sResult = visitNode(s.get());
            // 表达式语句的返回值未被消费，需 emit POP。与 build() 对齐使用
            // needsPopForExprStmt 覆盖全部表达式语句节点类型（AUDIT-R3 P2-7：以枚举为准）。
            // AUDIT-R5 BUG-01 fix: 改用节点级重载（覆盖匿名 lambda）。
            if (needsPopForExprStmt(s.get())) {
                emitIR(IROp::POP, {sResult}, s->line);
            }
        }
        leaveBlockScope();
    } else {
        // 顶层块：实现全局变量遮蔽保护（对齐 CompilerStmt.cpp visitBlock）
        // CRITICAL-3 fix: 原实现仅当 lookupGlobalSlot(name) >= 0 时 save/restore，
        // 导致嵌套顶层块（外层已 removeMapping）的内层块不 save，内层块 var 写入
        // 污染外层块的全局槽位且退出不 restore。修复：基于 varMap_ 判断是否需要遮蔽，
        // 支持 GLOBAL_SLOT 和 GLOBAL_NAME 两种旧绑定。
        blockDepth_++;
        struct ShadowedSave {
            std::string varName;
            VarInfo oldInfo;     // varMap_ 旧条目
            IROperand savedVreg; // 保存运行时原值的 vreg
            bool hadOld;         // varMap_ 是否有旧条目
            bool wasGlobalSlot;  // 旧条目是否为 GLOBAL_SLOT（需要 removeMapping/restoreMapping）
            int slot;            // GLOBAL_SLOT 时的 slot 编号
            uint32_t nameIdx;    // GLOBAL_NAME 时的名称索引
        };
        std::vector<ShadowedSave> shadowedSaves;
        for (auto& s : node->statements) {
            if (s && s->nodeType == NodeType::NODE_VAR_DECL) {
                VarDecl* vd = static_cast<VarDecl*>(s.get());
                auto it = varMap_.find(vd->name);
                if (it != varMap_.end()) {
                    ShadowedSave sv;
                    sv.varName = vd->name;
                    sv.oldInfo = it->second;
                    sv.hadOld = true;
                    sv.savedVreg = ir_->allocVReg();
                    if (it->second.kind == VarInfo::Kind::GLOBAL_SLOT) {
                        sv.wasGlobalSlot = true;
                        sv.slot = static_cast<int>(it->second.index);
                        sv.nameIdx = 0;
                        emitIR(IROp::LOAD_GLOBAL, {sv.savedVreg, IROperand::imm(static_cast<uint32_t>(sv.slot))},
                               vd->line);
                    } else if (it->second.kind == VarInfo::Kind::GLOBAL_NAME) {
                        sv.wasGlobalSlot = false;
                        sv.slot = -1;
                        sv.nameIdx = it->second.index;
                        emitIR(IROp::LOAD_GLOBAL, {sv.savedVreg, IROperand::global(sv.nameIdx)}, vd->line);
                    } else {
                        // LOCAL/UPVALUE 在顶层不应出现（inFunction_==false），跳过
                        sv.hadOld = false;
                    }
                    shadowedSaves.push_back(std::move(sv));
                }
            }
        }
        // 临时移除 GLOBAL_SLOT 映射，使块内 visitVarDecl 走 GLOBAL_NAME 路径
        // （GLOBAL_NAME 旧条目无需 removeMapping，本就不在 globalSlotAllocator_ 中）
        for (auto& sv : shadowedSaves) {
            if (sv.hadOld && sv.wasGlobalSlot) {
                globalSlotAllocator_.removeMapping(sv.varName);
            }
        }
        // 编译块体
        for (auto& s : node->statements) {
            if (!s)
                continue;
            IROperand sResult = visitNode(s.get());
            // 表达式语句的返回值未被消费，需 emit POP。与 build() 对齐使用
            // needsPopForExprStmt 覆盖全部表达式语句节点类型（AUDIT-R3 P2-7）。
            // AUDIT-R5 BUG-01 fix: 改用节点级重载（覆盖匿名 lambda）。
            if (needsPopForExprStmt(s.get())) {
                emitIR(IROp::POP, {sResult}, s->line);
            }
        }
        // 恢复：varMap_ 旧条目 + 全局槽位映射 + 运行时原值
        // 逆序恢复以处理多重嵌套遮蔽
        for (auto svIt = shadowedSaves.rbegin(); svIt != shadowedSaves.rend(); ++svIt) {
            auto& sv = *svIt;
            if (!sv.hadOld)
                continue;
            if (sv.wasGlobalSlot) {
                globalSlotAllocator_.restoreMapping(sv.varName, sv.slot);
                varMap_[sv.varName] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(sv.slot)};
                emitIR(IROp::STORE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(sv.slot)), sv.savedVreg}, node->line);
            } else {
                varMap_[sv.varName] = {VarInfo::Kind::GLOBAL_NAME, sv.nameIdx};
                emitIR(IROp::STORE_GLOBAL, {IROperand::global(sv.nameIdx), sv.savedVreg}, node->line);
            }
        }
        blockDepth_--;
    }
}

IROperand AstIRBuilder::visitArrayLiteral(ArrayLiteral* node) {
    IROperand dest = ir_->allocVReg();
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->elements.size())));
    for (auto& e : node->elements) {
        ops.push_back(visitNode(e.get()));
    }
    emitIR(IROp::BUILD_ARRAY, ops, node->line);
    return dest;
}

// R98 元组与解构：元组字面量 IR 生成
IROperand AstIRBuilder::visitTupleLiteral(TupleLiteral* node) {
    IROperand dest = ir_->allocVReg();
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->elements.size())));
    for (auto& e : node->elements) {
        ops.push_back(visitNode(e.get()));
    }
    emitIR(IROp::BUILD_TUPLE, ops, node->line);
    return dest;
}

// R99 枚举与 ADT：enum 声明 IR 生成
// 对齐 Compiler::visitEnumDecl：将 enum 名绑定为标记值 "enum:Name"。
// 收集 variant 元信息到 enumInfos_，由 Compiler 写入 CompileResult.enumInfos，
// VM 启动时加载到 enumRegistry_ 供 OP_BUILD_ENUM_VARIANT 校验。
void AstIRBuilder::visitEnumDecl(EnumDecl* node) {
    std::string marker = std::string("enum:") + node->name;
    IROperand markerVal = emitConst(Value(marker), node->line);

    // R99 enum 校验：收集 enum 元信息（与 Compiler::visitEnumDecl 对齐）
    VMEnumInfo info;
    info.name = node->name;
    // AUDIT-R6 F3 fix: 携带字段类型注解与泛型参数（对齐 Interpreter 字段类型校验）。
    info.typeParams = node->typeParams;
    info.variants.reserve(node->variants.size());
    for (const auto& v : node->variants) {
        VMEnumVariantInfo vi;
        vi.name = v.name;
        vi.arity = static_cast<int>(v.paramTypes.size());
        vi.paramTypes = v.paramTypes;
        info.variants.push_back(std::move(vi));
    }
    enumInfos_.push_back(std::move(info));

    if (inFunction_) {
        // 函数内：注册为 LOCAL
        uint32_t slot = nextLocalSlot_++;
        if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
            localSlotNames_.resize(slot + 1);
        }
        localSlotNames_[slot] = node->name;
        size_t curIdx = 0;
        for (const auto& blk : ir_->blocks)
            curIdx += blk.instructions.size();
        slotNameRanges_.push_back({slot, node->name, curIdx, 0});
        if (!blockScopes_.empty()) {
            auto it = varMap_.find(node->name);
            if (it != varMap_.end()) {
                blockScopes_.back().shadowedVars.push_back({node->name, it->second, true});
            } else {
                blockScopes_.back().shadowedVars.push_back({node->name, VarInfo{}, false});
            }
        }
        varMap_[node->name] = {VarInfo::Kind::LOCAL, slot};
        emitIR(IROp::STORE_LOCAL, {IROperand::local(slot), markerVal}, node->line);
        emitIR(IROp::POP, {markerVal}, node->line); // STORE_LOCAL peek 不 pop，补 POP
        if (!blockScopes_.empty()) {
            blockScopes_.back().localSlots.push_back(slot);
        }
    } else {
        // 顶层：使用全局槽位
        int slot = lookupGlobalSlot(node->name);
        if (slot >= 0) {
            varMap_[node->name] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot)};
            emitIR(IROp::DEFINE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(slot)), markerVal}, node->line);
        } else {
            uint32_t idx = ir_->addGlobal(node->name);
            varMap_[node->name] = {VarInfo::Kind::GLOBAL_NAME, idx};
            emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(idx), markerVal}, node->line);
        }
    }
}

// R99 枚举与 ADT：enum variant 构造 IR 生成
// IR: BUILD_ENUM_VARIANT dest, enumNameConstIdx, variantNameConstIdx, argCount, arg1, arg2, ...
IROperand AstIRBuilder::visitEnumVariantExpr(EnumVariantExpr* node) {
    if (node->arguments.size() > 255) {
        Logger::Error("AstIRBuilder::visitEnumVariantExpr: enum variant 参数数量超过 255 上限", "IR");
        irDiagnostics_.addError("enum variant 参数数量超过 255 上限", node->line, 0, DiagSource::Compiler);
        return IROperand::vreg(0);
    }
    IROperand dest = ir_->allocVReg();
    uint32_t enumNameConstIdx = ir_->addConstant(Value(node->enumName));
    uint32_t variantNameConstIdx = ir_->addConstant(Value(node->variantName));
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::constant(enumNameConstIdx));
    ops.push_back(IROperand::constant(variantNameConstIdx));
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->arguments.size())));
    for (auto& arg : node->arguments) {
        ops.push_back(visitNode(arg.get()));
    }
    emitIR(IROp::BUILD_ENUM_VARIANT, ops, node->line);
    return dest;
}

// R99 枚举与 ADT：match 表达式 IR 生成
// 设计：用临时 local slot 汇合所有 case body 的结果（参考 visitBinaryOp BIN_AND/OR 模式），
// 最后 LOAD_LOCAL 到 dest vreg。
//   scrut_v = visit(scrutinee)
//   tempSlot = nextLocalSlot_++
//   for each case (按顺序):
//     pattern_check_label:
//       - WILDCARD/DEFAULT: 无条件跳到 body_label
//       - LITERAL: EQ scrut_v, literal → JUMP_IF_FALSE next_case_label
//       - VARIANT: ENUM_VARIANT_NAME scrut_v, e, v → JUMP_IF_FALSE next_case_label
//                  for each binding i: ENUM_VARIANT_FIELD scrut_v, i → STORE_LOCAL b_i
//     body_label:
//       body_v = visit(body)
//       STORE_LOCAL tempSlot, body_v
//       POP
//       JUMP end_label
//     next_case_label:
//       // fall through to next case
//   // 所有 case 未匹配：tempSlot = null
//   LOAD_NULL tempNull
//   STORE_LOCAL tempSlot, tempNull
//   POP
//   end_label:
//   dest = LOAD_LOCAL tempSlot
//   return dest
//
// 注意：与栈式 VM 不同，IR/RegVM 不需要 SWAP——scrut_v 在寄存器中，
// body 结果直接 STORE_LOCAL 到 tempSlot，无需栈操作。
// R110 重构：visitMatchExpr 提取两个单一职责 helper（pattern 检查 + case body 编译）。
// 原 visitMatchExpr 173 行拆为 53 行主函数 + emitMatchPattern（~80 行）+ emitMatchCaseBody（~35 行）。
// 所有 case 内逻辑原样保留至各 helper，仅做"剪切-粘贴 + 函数签名包装"，无语义变更。

bool AstIRBuilder::emitMatchPattern(const MatchPattern& p, uint32_t scrutSlot, int line,
                                    std::vector<uint32_t>& failLabels) {
    // R134 模式匹配扩展：递归处理 6 种 pattern（WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）。
    // 栈平衡不变量：调用前 [scrut 在 slot 中]，调用后 [scrut 仍在 slot 中]（每次按需 LOAD_LOCAL）。
    // failLabels 收集所有"此 case 匹配失败需跳过"的目标 label（OR/TUPLE/嵌套会产生多个）。
    // 调用方需在每个 failLabel 处 emit LABEL + POP 残留 check 值。
    // 设计：IR 层用 LOAD_LOCAL 重新加载 scrut（不像 StackVM 用 DUP 保留原值在栈上），
    //       因为 IR 的 scrut 存在 slot 中，每次按需加载到独立 vreg。
    bool hasAnyCheck = false;
    (void)line; // 主要使用 p.line，line 仅作为潜在扩展参数保留

    switch (p.kind) {
    case MatchPatternKind::WILDCARD:
        // 永远匹配，无检查，直接执行 body
        break;
    case MatchPatternKind::LITERAL: {
        IROperand sVReg = ir_->allocVReg();
        emitIR(IROp::LOAD_LOCAL, {sVReg, IROperand::local(scrutSlot)}, p.line);
        IROperand litVReg = visitNode(p.literal.get());
        IROperand eqVReg = ir_->allocVReg();
        emitIR(IROp::EQ, {eqVReg, sVReg, litVReg}, p.line);
        uint32_t failLabel = ir_->allocLabel();
        emitIR(IROp::JUMP_IF_FALSE, {eqVReg, IROperand::label(failLabel)}, p.line);
        emitIR(IROp::POP, {eqVReg}, p.line); // 成功路径消费 eq_v
        failLabels.push_back(failLabel);
        hasAnyCheck = true;
        break;
    }
    case MatchPatternKind::VARIABLE: {
        // R134: 绑定整个 scrut 到 variableName（无检查，永远匹配）
        // 语义对齐 StackVM Compiler: DUP scrut + bindDestructureVar（DUP 让 scrut 留栈顶）
        // IR 层: LOAD_LOCAL 创建 vreg_s 副本，绑定到 variableName
        IROperand sVReg = ir_->allocVReg();
        emitIR(IROp::LOAD_LOCAL, {sVReg, IROperand::local(scrutSlot)}, p.line);
        const std::string& bindName = p.variableName;
        // 绑定到变量（复用 visitVarDecl 模式：函数内 LOCAL，顶层 GLOBAL）
        if (inFunction_) {
            uint32_t bslot = nextLocalSlot_++;
            if (static_cast<size_t>(bslot) >= localSlotNames_.size()) {
                localSlotNames_.resize(bslot + 1);
            }
            localSlotNames_[bslot] = bindName;
            size_t curIdx = 0;
            for (const auto& blk : ir_->blocks)
                curIdx += blk.instructions.size();
            slotNameRanges_.push_back({bslot, bindName, curIdx, 0});
            if (!blockScopes_.empty()) {
                auto it = varMap_.find(bindName);
                if (it != varMap_.end()) {
                    blockScopes_.back().shadowedVars.push_back({bindName, it->second, true});
                } else {
                    blockScopes_.back().shadowedVars.push_back({bindName, VarInfo{}, false});
                }
            }
            varMap_[bindName] = {VarInfo::Kind::LOCAL, bslot};
            emitIR(IROp::STORE_LOCAL, {IROperand::local(bslot), sVReg}, p.line);
            emitIR(IROp::POP, {sVReg}, p.line);
            if (!blockScopes_.empty()) {
                blockScopes_.back().localSlots.push_back(bslot);
            }
        } else {
            int bslot = lookupGlobalSlot(bindName);
            if (bslot >= 0) {
                varMap_[bindName] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(bslot)};
                emitIR(IROp::DEFINE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(bslot)), sVReg}, p.line);
            } else {
                uint32_t idx = ir_->addGlobal(bindName);
                varMap_[bindName] = {VarInfo::Kind::GLOBAL_NAME, idx};
                emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(idx), sVReg}, p.line);
            }
        }
        break;
    }
    case MatchPatternKind::VARIANT: {
        IROperand sVReg = ir_->allocVReg();
        emitIR(IROp::LOAD_LOCAL, {sVReg, IROperand::local(scrutSlot)}, p.line);
        IROperand matchVReg = ir_->allocVReg();
        uint32_t enumNameConstIdx = ir_->addConstant(Value(p.enumName));
        uint32_t variantNameConstIdx = ir_->addConstant(Value(p.variantName));
        emitIR(IROp::ENUM_VARIANT_NAME,
               {matchVReg, sVReg, IROperand::constant(enumNameConstIdx), IROperand::constant(variantNameConstIdx)},
               p.line);
        uint32_t failLabel = ir_->allocLabel();
        emitIR(IROp::JUMP_IF_FALSE, {matchVReg, IROperand::label(failLabel)}, p.line);
        emitIR(IROp::POP, {matchVReg}, p.line); // 成功路径消费 matchVReg
        failLabels.push_back(failLabel);
        hasAnyCheck = true;
        // R134: 递归匹配 variant 字段（subPatterns 替代 R99 的 bindings 字符串列表）
        // 设计：加载 fields[i] 到临时 slot，递归 emitMatchPattern 匹配子 pattern。
        // 与 StackVM Compiler 的 DUP+OP_INT i+OP_ENUM_VARIANT_FIELD+递归+POP 模式语义等价。
        for (size_t i = 0; i < p.subPatterns.size(); ++i) {
            if (p.subPatterns[i]->kind == MatchPatternKind::WILDCARD) {
                continue; // 通配不绑定，无需加载字段
            }
            // 加载 fields[i] 到临时 slot（供递归 emitMatchPattern 用作子 scrut）
            uint32_t fieldSlot = nextLocalSlot_++;
            IROperand sVReg2 = ir_->allocVReg();
            emitIR(IROp::LOAD_LOCAL, {sVReg2, IROperand::local(scrutSlot)}, p.line);
            IROperand idxVReg = emitConst(Value(static_cast<int64_t>(i)), p.line);
            IROperand fieldVReg = ir_->allocVReg();
            emitIR(IROp::ENUM_VARIANT_FIELD, {fieldVReg, sVReg2, idxVReg}, p.line);
            emitIR(IROp::STORE_LOCAL, {IROperand::local(fieldSlot), fieldVReg}, p.line);
            emitIR(IROp::POP, {fieldVReg}, p.line); // STORE_LOCAL peek，补 POP
            // 递归匹配子 pattern（子 scrut 是 fieldSlot）
            bool subHasCheck = emitMatchPattern(*p.subPatterns[i], fieldSlot, p.line, failLabels);
            hasAnyCheck = hasAnyCheck || subHasCheck;
        }
        break;
    }
    case MatchPatternKind::TUPLE: {
        // R134: 元组模式 - 检查是 tuple + 元素数匹配 + 递归匹配每个元素
        // 类型检查：LOAD_LOCAL + TYPE_TEST "tuple"（push bool）+ JUMP_IF_FALSE fail + POP
        // 注意：用 TYPE_TEST（软检查，push bool）而非 TYPE_CHECK（硬检查，不匹配抛错）。
        //       TUPLE pattern 类型不匹配时应 fall through 到下一 case，而非抛 runtimeError。
        IROperand sVReg = ir_->allocVReg();
        emitIR(IROp::LOAD_LOCAL, {sVReg, IROperand::local(scrutSlot)}, p.line);
        IROperand typeTestVReg = ir_->allocVReg();
        uint32_t tupleTypeConstIdx = ir_->addConstant(Value(std::string("tuple")));
        emitIR(IROp::TYPE_TEST, {typeTestVReg, sVReg, IROperand::constant(tupleTypeConstIdx)}, p.line);
        uint32_t typeFailLabel = ir_->allocLabel();
        emitIR(IROp::JUMP_IF_FALSE, {typeTestVReg, IROperand::label(typeFailLabel)}, p.line);
        emitIR(IROp::POP, {typeTestVReg}, p.line); // 成功路径消费 type_test bool
        failLabels.push_back(typeFailLabel);
        hasAnyCheck = true;
        // 元素数检查：LOAD_LOCAL + LEN + OP_INT size + EQ + JUMP_IF_FALSE fail + POP
        IROperand sVReg2 = ir_->allocVReg();
        emitIR(IROp::LOAD_LOCAL, {sVReg2, IROperand::local(scrutSlot)}, p.line);
        IROperand lenVReg = ir_->allocVReg();
        emitIR(IROp::LEN, {lenVReg, sVReg2}, p.line);
        IROperand sizeVReg = emitConst(Value(static_cast<int64_t>(p.subPatterns.size())), p.line);
        IROperand sizeEqVReg = ir_->allocVReg();
        emitIR(IROp::EQ, {sizeEqVReg, lenVReg, sizeVReg}, p.line);
        uint32_t sizeFailLabel = ir_->allocLabel();
        emitIR(IROp::JUMP_IF_FALSE, {sizeEqVReg, IROperand::label(sizeFailLabel)}, p.line);
        emitIR(IROp::POP, {sizeEqVReg}, p.line); // 成功路径消费 size_eq 结果
        failLabels.push_back(sizeFailLabel);
        // 递归匹配每个元素
        for (size_t i = 0; i < p.subPatterns.size(); ++i) {
            if (p.subPatterns[i]->kind == MatchPatternKind::WILDCARD) {
                continue; // 通配不绑定
            }
            // 加载 tuple[i] 到临时 slot（供递归 emitMatchPattern 用作子 scrut）
            uint32_t elemSlot = nextLocalSlot_++;
            IROperand sVReg3 = ir_->allocVReg();
            emitIR(IROp::LOAD_LOCAL, {sVReg3, IROperand::local(scrutSlot)}, p.line);
            IROperand idxVReg = emitConst(Value(static_cast<int64_t>(i)), p.line);
            IROperand elemVReg = ir_->allocVReg();
            emitIR(IROp::INDEX_GET, {elemVReg, sVReg3, idxVReg}, p.line);
            emitIR(IROp::STORE_LOCAL, {IROperand::local(elemSlot), elemVReg}, p.line);
            emitIR(IROp::POP, {elemVReg}, p.line); // STORE_LOCAL peek，补 POP
            bool subHasCheck = emitMatchPattern(*p.subPatterns[i], elemSlot, p.line, failLabels);
            hasAnyCheck = hasAnyCheck || subHasCheck;
        }
        break;
    }
    case MatchPatternKind::OR: {
        // R134: OR pattern - 任一子 pattern 匹配即成功
        // 编译策略（与 StackVM Compiler 语义等价）：
        //   1. 依次尝试每个子 pattern，失败的 patch 暂存到 subFailLabels
        //   2. 子 pattern 成功后立即 JUMP 到 orEndLabel（跳过剩余子 pattern）
        //   3. 每个子 pattern 失败位置：所有 subFailLabels 共享同一 offset，emit 一次 POP
        //      （LABEL lowering 不产生字节码，多个 LABEL+POP 会 fall through 累积 POP 导致栈下溢）
        //   4. 所有子 pattern 失败：emit LOAD_FALSE + JUMP_IF_FALSE 加入 failLabels（永假让 case 失败）
        //   5. orEndLabel 处为 OR 成功位置
        uint32_t orEndLabel = ir_->allocLabel();
        for (size_t i = 0; i < p.subPatterns.size(); ++i) {
            std::vector<uint32_t> subFailLabels;
            emitMatchPattern(*p.subPatterns[i], scrutSlot, p.line, subFailLabels);
            // 子 pattern 成功：跳到 OR 末尾
            emitIR(IROp::JUMP, {IROperand::label(orEndLabel)}, p.line);
            // 子 pattern 失败位置：所有 subFailLabels 映射到同一 offset，emit 一次 POP 消费 check 残留
            // （不同失败位置跳来时栈顶都是 1 个 bool，POP 一次即可清空，跳到下一子 pattern）
            for (uint32_t sfl : subFailLabels) {
                emitIR(IROp::LABEL, {IROperand::label(sfl)}, p.line);
            }
            if (!subFailLabels.empty()) {
                emitIR(IROp::POP, {}, p.line); // 只 POP 一次（消费 check 残留 bool）
            }
            hasAnyCheck = hasAnyCheck || !subFailLabels.empty();
        }
        // 所有子 pattern 失败：OR 整体失败，emit 永假检查让 case 跳到下一 case
        IROperand falseVReg = ir_->allocVReg();
        emitIR(IROp::LOAD_FALSE, {falseVReg}, p.line);
        uint32_t orFailLabel = ir_->allocLabel();
        emitIR(IROp::JUMP_IF_FALSE, {falseVReg, IROperand::label(orFailLabel)}, p.line);
        emitIR(IROp::POP, {falseVReg}, p.line); // 成功路径（永不到达）消费 false_v
        failLabels.push_back(orFailLabel);
        hasAnyCheck = true;
        // OR 成功位置
        emitIR(IROp::LABEL, {IROperand::label(orEndLabel)}, p.line);
        break;
    }
    }
    return hasAnyCheck;
}

IROperand AstIRBuilder::emitMatchCaseBody(ASTNode* body, int nodeLine) {
    // 编译 case body，push 结果到 bodyVReg。
    // R99 L2 fix: Block body 需保留最后一条表达式语句的值作为 body 结果。
    // 原 visitNode(Block) 调用 visitBlock（void 返回），最后表达式值被 POP 消费丢失。
    // 这里对齐 StackVM Compiler 的处理：编译除最后一条外的所有语句（带 POP），
    // 最后一条用 visitNode 保留值；若最后一条非表达式语句则补 LOAD_NULL。
    IROperand bodyVReg = ir_->allocVReg();
    if (body) {
        if (body->nodeType == NodeType::NODE_BLOCK) {
            Block* blk = static_cast<Block*>(body);
            if (inFunction_) {
                enterBlockScope();
            }
            if (blk->statements.empty()) {
                emitIR(IROp::LOAD_NULL, {bodyVReg}, nodeLine);
            } else {
                // 编译除最后一条外的所有语句（表达式语句补 POP）
                for (size_t i = 0; i + 1 < blk->statements.size(); ++i) {
                    IROperand midResult = visitNode(blk->statements[i].get());
                    if (needsPopForExprStmt(blk->statements[i].get())) {
                        emitIR(IROp::POP, {midResult}, blk->statements[i]->line);
                    }
                }
                // 最后一条语句作为表达式编译（保留值在栈顶）
                ASTNode* last = blk->statements.back().get();
                if (needsPopForExprStmt(last)) {
                    bodyVReg = visitNode(last);
                } else {
                    // 非表达式语句（VarDecl/IfStmt 等）：编译后补 LOAD_NULL 作为 body 值
                    visitNode(last);
                    bodyVReg = ir_->allocVReg();
                    emitIR(IROp::LOAD_NULL, {bodyVReg}, nodeLine);
                }
            }
            if (inFunction_) {
                leaveBlockScope();
            }
        } else {
            bodyVReg = visitNode(body);
        }
    } else {
        emitIR(IROp::LOAD_NULL, {bodyVReg}, nodeLine);
    }
    return bodyVReg;
}

IROperand AstIRBuilder::visitMatchExpr(MatchExpr* node) {
    // ---- 穷尽性检查（编译期）----
    // 扫描所有 case 的 VARIANT pattern，确定匹配的 enum 名称，
    // 然后检查是否覆盖了所有 variant（或含 wildcard/default）。
    {
        std::string matchedEnumName;
        std::unordered_set<std::string> coveredVariants;
        bool hasWildcardOrDefault = false;

        for (const auto& mc : node->cases) {
            if (mc.isDefault || !mc.pattern) {
                hasWildcardOrDefault = true;
                continue;
            }
            // 递归收集 VARIANT pattern 覆盖的 variant 名
            std::function<void(const MatchPattern&)> collectVariants = [&](const MatchPattern& pat) {
                if (pat.kind == MatchPatternKind::WILDCARD || pat.kind == MatchPatternKind::VARIABLE) {
                    hasWildcardOrDefault = true;
                } else if (pat.kind == MatchPatternKind::VARIANT) {
                    if (matchedEnumName.empty() && !pat.enumName.empty())
                        matchedEnumName = pat.enumName;
                    if (pat.enumName == matchedEnumName)
                        coveredVariants.insert(pat.variantName);
                } else if (pat.kind == MatchPatternKind::OR) {
                    for (const auto& sub : pat.subPatterns)
                        if (sub)
                            collectVariants(*sub);
                }
            };
            collectVariants(*mc.pattern);
        }

        // 仅当匹配的是已知 enum 且无 wildcard/default 时检查穷尽性
        if (!matchedEnumName.empty() && !hasWildcardOrDefault) {
            for (const auto& info : enumInfos_) {
                if (info.name == matchedEnumName) {
                    std::vector<std::string> missing;
                    for (const auto& v : info.variants) {
                        if (coveredVariants.find(v.name) == coveredVariants.end())
                            missing.push_back(v.name);
                    }
                    if (!missing.empty()) {
                        std::string msg = "match 未覆盖 enum " + matchedEnumName + " 的全部变体，缺少: ";
                        for (size_t i = 0; i < missing.size(); ++i) {
                            if (i > 0)
                                msg += ", ";
                            msg += missing[i];
                        }
                        irDiagnostics_.addWarning(msg, node->line, node->column, DiagSource::Compiler,
                                                  "non-exhaustive-match");
                    }
                    break;
                }
            }
        }
    }

    IROperand scrutVReg = visitNode(node->scrutinee.get());
    // R99 L1 fix: 将 scrut 存入临时 slot，每个 case 通过 LOAD_LOCAL 重新加载。
    // 原实现直接引用 scrutVReg，但 IR→StackVM lowering 假设 vreg 已在栈上。
    // 第一次 ENUM_VARIANT_NAME/EQ 会 pop scrut，后续 case 的检查无 scrut 可用 → 栈下溢。
    // 用 tempSlot + LOAD_LOCAL 模式让每个 case 独立加载 scrut（参考 BIN_AND tempSlot 模式）。
    uint32_t scrutSlot = nextLocalSlot_++;
    emitIR(IROp::STORE_LOCAL, {IROperand::local(scrutSlot), scrutVReg}, node->line);
    emitIR(IROp::POP, {scrutVReg}, node->line); // STORE_LOCAL peek，补 POP 消费栈顶 scrut
    uint32_t tempSlot = nextLocalSlot_++;       // 汇合所有 case 结果的临时槽位
    uint32_t endLabel = ir_->allocLabel();
    bool hasDefault = false;

    for (size_t ci = 0; ci < node->cases.size(); ++ci) {
        MatchCase& mc = node->cases[ci];
        // R134: failLabels 收集所有"此 case 匹配失败需跳过"的目标 label
        // （pattern + guard 都会产生 fail label，OR/TUPLE/嵌套会有多个）
        std::vector<uint32_t> failLabels;
        bool hasCheck = false;

        if (!mc.isDefault && mc.pattern) {
            // R110 重构：原 ~85 行 pattern 检查逻辑提取到 emitMatchPattern。
            // R134 扩展：emitMatchPattern 现返回 bool hasAnyCheck，failLabels 通过 out param 收集。
            hasCheck = emitMatchPattern(*mc.pattern, scrutSlot, node->line, failLabels);
        } else {
            // default case 或无 pattern
            hasDefault = true;
        }

        // R134: guard 编译——pattern 匹配成功后求值 guard，false 则跳到下一 case
        // 语义对齐 StackVM Compiler: compileNode(guard) + OP_JUMP_IF_FALSE + OP_POP
        if (mc.guard) {
            IROperand guardVReg = visitNode(mc.guard.get());
            uint32_t guardFailLabel = ir_->allocLabel();
            emitIR(IROp::JUMP_IF_FALSE, {guardVReg, IROperand::label(guardFailLabel)}, mc.guard->line);
            emitIR(IROp::POP, {guardVReg}, mc.guard->line); // 成功路径消费 guard bool
            failLabels.push_back(guardFailLabel);
            hasCheck = true;
        }

        // 编译 case body（push 结果到 vreg）
        // R110 重构：原 ~37 行 body 编译逻辑提取到 emitMatchCaseBody。
        IROperand bodyVReg = emitMatchCaseBody(mc.body.get(), node->line);
        emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), bodyVReg}, node->line);
        emitIR(IROp::POP, {bodyVReg}, node->line); // STORE_LOCAL peek，补 POP
        emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);

        // R134: 回填所有 failLabels（pattern + guard + 嵌套 OR/TUPLE）跳到此处
        // 所有 failLabels 共享同一 offset，emit 一次 POP 消费 check 残留 bool
        // （LABEL lowering 不产生字节码，多个 LABEL+POP 会 fall through 累积 POP 导致栈下溢）
        for (uint32_t fl : failLabels) {
            emitIR(IROp::LABEL, {IROperand::label(fl)}, node->line);
        }
        if (!failLabels.empty()) {
            emitIR(IROp::POP, {}, node->line); // 只 POP 一次（消费 check 残留 bool）
        }
        (void)hasCheck; // hasCheck 已通过 failLabels 非空体现，保留用于未来扩展
    }

    // L6 fix: 所有 case 未匹配时，对齐 Interpreter 的 runtimeError 行为（抛异常而非静默返回 null）
    // 无 default 分支时：LOAD_CONST(msg) + THROW 抛出异常
    if (!hasDefault) {
        uint32_t errConstIdx = ir_->addConstant(Value(std::string("match 表达式没有匹配的 case")));
        IROperand errVReg = ir_->allocVReg();
        emitIR(IROp::LOAD_CONST, {errVReg, IROperand::constant(errConstIdx)}, node->line);
        emitIR(IROp::THROW, {errVReg}, node->line);
    }

    // end：加载 tempSlot 到 dest
    emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::LOAD_LOCAL, {dest, IROperand::local(tempSlot)}, node->line);
    return dest;
}

// R98 元组与解构：解构绑定 IR 生成
// 对齐 Compiler::visitDestructureBinding 的 DUP+INDEX_GET+绑定 模式：
//   1. 编译 initializer 得到 tuple vreg（栈顶为 tuple）
//   2. 对每个 name i：
//        DUP（复制 tuple 到栈顶，dest_i vreg）
//        LOAD_CONST i（push 索引常量）
//        INDEX_GET（pop tuple 副本和 idx，push tuple[i]）
//        按 visitVarDecl 模式 STORE 到 LOCAL/GLOBAL_SLOT/GLOBAL_NAME
//        函数内 LOCAL 路径额外 POP（OP_SET_LOCAL peek 不 pop，需补 POP 清栈）
//   3. 末尾 POP 消费原 tuple
// 注意：IR DUP 的 StackVM lowering 仅 emit OP_DUP（复制栈顶），不读 src_vreg；
// 因此每个循环开始时栈顶必须是 tuple（前一轮的 STORE+POP 已清理 elem_i）。
void AstIRBuilder::visitDestructureBinding(DestructureBinding* node) {
    if (node->names.size() > 255) {
        Logger::Error("AstIRBuilder::visitDestructureBinding: 解构绑定变量数超过 255 上限", "IR");
        return;
    }
    IROperand tupleVReg = visitNode(node->initializer.get());

    for (size_t i = 0; i < node->names.size(); ++i) {
        // DUP 复制 tuple 到栈顶（dest_i vreg）。OP_DUP 复制当前栈顶值。
        IROperand dupDest = ir_->allocVReg();
        emitIR(IROp::DUP, {dupDest, tupleVReg}, node->line);
        // 加载索引 i
        IROperand idxVReg = emitConst(Value(static_cast<int64_t>(i)), node->line);
        // INDEX_GET 取元素：tuple[i]
        IROperand elemVReg = ir_->allocVReg();
        emitIR(IROp::INDEX_GET, {elemVReg, dupDest, idxVReg}, node->line);

        // 类型注解检查（对齐 visitVarDecl 中的 emitTypeCheckIR）
        // AUDIT-R6 F4 fix: per-name 类型注解运行时校验（对齐 Interpreter L20 checkType 与
        // StackVM 直接路径）。原注释称“整体检查在 Interpreter 层完成”已过时——
        // Interpreter 实际按 nameTypeAt(i) 逐元素校验，IR 路径缺失导致三后端不一致。
        // TYPE_CHECK 在栈式 lowering 为 peek（elem 仍在栈顶），寄存器路径读 src vreg。
        emitTypeCheckIR(elemVReg, node->nameTypeAt(i), node->line);

        // 按 visitVarDecl 模式绑定变量
        const std::string& name = node->names[i];
        if (inFunction_) {
            // 函数内：注册为 LOCAL
            uint32_t slot = nextLocalSlot_++;
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = name;
            // L1 fix: 记录 (slot, name, startInstr) 到 slotNameRanges_
            size_t curIdx = 0;
            for (const auto& blk : ir_->blocks)
                curIdx += blk.instructions.size();
            slotNameRanges_.push_back({slot, name, curIdx, 0});
            // CRITICAL-2 fix: 覆盖 varMap_ 前保存旧条目
            if (!blockScopes_.empty()) {
                auto it = varMap_.find(name);
                if (it != varMap_.end()) {
                    blockScopes_.back().shadowedVars.push_back({name, it->second, true});
                } else {
                    blockScopes_.back().shadowedVars.push_back({name, VarInfo{}, false});
                }
            }
            varMap_[name] = {VarInfo::Kind::LOCAL, slot};
            emitIR(IROp::STORE_LOCAL, {IROperand::local(slot), elemVReg}, node->line);
            // STORE_LOCAL 在 StackVM lowering 中用 OP_SET_LOCAL（peek 不 pop），
            // 元素值残留在栈上。需 POP 消费。
            emitIR(IROp::POP, {elemVReg}, node->line);
            if (!blockScopes_.empty()) {
                blockScopes_.back().localSlots.push_back(slot);
            }
        } else {
            // 顶层：使用全局槽位
            int slot = lookupGlobalSlot(name);
            if (slot >= 0) {
                varMap_[name] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot)};
                emitIR(IROp::DEFINE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(slot)), elemVReg}, node->line);
            } else {
                uint32_t idx = ir_->addGlobal(name);
                varMap_[name] = {VarInfo::Kind::GLOBAL_NAME, idx};
                emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(idx), elemVReg}, node->line);
            }
            // DEFINE_GLOBAL 在 StackVM lowering 中用 OP_DEFINE_GLOBAL（pop），
            // 元素值已被消费，无需额外 POP。
        }
    }

    // 末尾 POP 消费原 tuple（initializer 压栈的值）
    emitIR(IROp::POP, {tupleVReg}, node->line);
}

IROperand AstIRBuilder::visitDictLiteral(DictLiteral* node) {
    IROperand dest = ir_->allocVReg();
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->pairs.size())));
    for (auto& p : node->pairs) {
        ops.push_back(visitNode(p.first.get()));  // key
        ops.push_back(visitNode(p.second.get())); // value
    }
    emitIR(IROp::BUILD_DICT, ops, node->line);
    return dest;
}

IROperand AstIRBuilder::visitIndexAccess(IndexAccess* node) {
    IROperand obj = visitNode(node->object.get());
    IROperand idx = visitNode(node->index.get());
    IROperand dest = ir_->allocVReg();
    emitIR(IROp::INDEX_GET, {dest, obj, idx}, node->line);
    return dest;
}

// R111 重构：嵌套左值变异写回共享 helper。
// 调用方：visitIndexAssign / visitMemberAssign / emitMethodCallWriteback 三处共用。
// 处理 IndexAccess(VarRef) | MemberAccess(VarRef) 形态的接收者：
//   1. baseVar 检测（从 object 取 IndexAccess(VarRef) 或 MemberAccess(VarRef)）
//   2. base2 = emitLoadVar(baseVar->name)
//   3. mut = allocVReg
//   4. if outerIdx: outerIdxVal = visitNode(outerIdx->index); emit LOAD_MUTATED + INDEX_SET
//      else: outerFieldIdx = addGlobal(outerMem->fieldName); emit LOAD_MUTATED + MEMBER_SET
//   5. info = resolveVar(baseVar->name); 4 种 info.kind × outerIdx/outerMem 分支 emit WRITEBACK
// 返回值：true 表示已处理写回（调用方据此 return），false 表示是复杂表达式未处理（baseVar 为 nullptr）。
// 语义对齐：上一级 INDEX_SET/MEMBER_SET/METHOD_CALL 已将变异后内层容器存入 lastMutatedReceiver_，
// 本处读取并通过外层 SET 传播到基变量，最终 WRITEBACK 整体替换基变量。
// 栈序约束（栈式 VM）：外层 INDEX_SET 需 [obj, idx, val]，外层 MEMBER_SET 需 [obj, val]。
bool AstIRBuilder::emitNestedAssignWriteback(ASTNode* object, int line) {
    // 检测嵌套左值：IndexAccess(VarRef) | MemberAccess(VarRef)
    IndexAccess* outerIdx =
        (object && object->nodeType == NodeType::NODE_INDEX_ACCESS) ? static_cast<IndexAccess*>(object) : nullptr;
    MemberAccess* outerMem =
        (object && object->nodeType == NodeType::NODE_MEMBER_ACCESS) ? static_cast<MemberAccess*>(object) : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());
    if (!baseVar) {
        // 复杂表达式接收者，不写回（结果在 dest 中）
        return false;
    }
    // 上一级 SET 已将变异后的内层容器存入 lastMutatedReceiver_。
    // 此处重新加载基变量（全局槽/局部槽均未被修改，仍是原始值），
    // 然后读取 lastMutatedReceiver_，再发射外层 SET 将变异传播到基变量，
    // 最后用 WRITEBACK（整体替换语义）将变异后的基变量写回变量槽。
    // 栈序约束（栈式 VM）：
    //   - 外层 INDEX_SET 需 [obj, idx, val] → 先 base2，再 outerIdxVal，再 mut
    //   - 外层 MEMBER_SET 需 [obj, val]      → 先 base2，再 mut
    IROperand base2 = emitLoadVar(baseVar->name, line);
    IROperand mut = ir_->allocVReg();
    if (outerIdx) {
        // 外层是索引：base[outerIdx] = mut
        IROperand outerIdxVal = visitNode(outerIdx->index.get());
        emitIR(IROp::LOAD_MUTATED, {mut}, line);
        emitIR(IROp::INDEX_SET, {base2, outerIdxVal, mut}, line);
    } else {
        // 外层是成员：base.field = mut
        uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
        emitIR(IROp::LOAD_MUTATED, {mut}, line);
        emitIR(IROp::MEMBER_SET, {base2, IROperand::field(outerFieldIdx), mut}, line);
    }
    // 最终 WRITEBACK（整体替换语义，将变异后的 base 写回变量槽）
    VarInfo info = resolveVar(baseVar->name);
    if (info.kind == VarInfo::Kind::LOCAL) {
        if (outerIdx) {
            emitIR(IROp::WRITEBACK_INDEX_LOCAL, {IROperand::local(info.index)}, line);
        } else {
            uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
            emitIR(IROp::WRITEBACK_MEMBER_LOCAL, {IROperand::local(info.index), IROperand::field(outerFieldIdx)}, line);
        }
    } else if (info.kind == VarInfo::Kind::GLOBAL_SLOT) {
        if (outerIdx) {
            emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::imm(info.index)}, line);
        } else {
            uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
            emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::imm(info.index), IROperand::field(outerFieldIdx)}, line);
        }
    } else if (info.kind == VarInfo::Kind::GLOBAL_NAME) {
        if (outerIdx) {
            emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::global(info.index)}, line);
        } else {
            uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
            emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::global(info.index), IROperand::field(outerFieldIdx)}, line);
        }
    } else if (info.kind == VarInfo::Kind::UPVALUE) {
        if (outerIdx) {
            emitIR(IROp::WRITEBACK_INDEX_UPVALUE, {IROperand::upvalue(info.index)}, line);
        } else {
            uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
            emitIR(IROp::WRITEBACK_MEMBER_UPVALUE, {IROperand::upvalue(info.index), IROperand::field(outerFieldIdx)},
                   line);
        }
    }
    return true;
}

void AstIRBuilder::visitIndexAssign(IndexAssign* node) {
    // 限制2：检测 object 是否为简单 VarRef。若是，emit INDEX_SET + WRITEBACK_INDEX_VAR/LOCAL
    bool isVarRef = node->object && node->object->nodeType == NodeType::NODE_VAR_REF;
    IROperand obj = visitNode(node->object.get());
    IROperand idx = visitNode(node->index.get());
    IROperand val = visitNode(node->value.get());
    emitIR(IROp::INDEX_SET, {obj, idx, val}, node->line);
    // 仅当 object 是 VarRef 时 emit 写回指令
    if (isVarRef) {
        VarRef* vr = static_cast<VarRef*>(node->object.get());
        VarInfo info = resolveVar(vr->name);
        if (info.kind == VarInfo::Kind::LOCAL) {
            emitIR(IROp::WRITEBACK_INDEX_LOCAL, {IROperand::local(info.index)}, node->line);
        } else if (info.kind == VarInfo::Kind::GLOBAL_SLOT) {
            emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::imm(info.index)}, node->line);
        } else if (info.kind == VarInfo::Kind::GLOBAL_NAME) {
            emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::global(info.index)}, node->line);
        } else if (info.kind == VarInfo::Kind::UPVALUE) {
            // #7 fix: upvalue 容器赋值时需写回，否则 COW ensureUnique 产生的新容器
            // 副本会丢失，导致闭包内 arr[i]=v 修改被静默吞掉。
            emitIR(IROp::WRITEBACK_INDEX_UPVALUE, {IROperand::upvalue(info.index)}, node->line);
        }
        return;
    }

    // MEDIUM-1/2 fix: 2 层嵌套左值（base.outer[i] = val）
    // R111 重构：嵌套路径写回逻辑提取到共享 helper emitNestedAssignWriteback。
    // helper 处理 IndexAccess(VarRef) | MemberAccess(VarRef) 形态的接收者，
    // 返回 true 表示已处理写回（提前 return），false 表示复杂表达式未处理。
    if (emitNestedAssignWriteback(node->object.get(), node->line))
        return;

    // C-P2-2 fix: 3+ 层嵌套或复杂表达式：不支持，报错而非静默丢失修改
    Logger::Error("AstIRBuilder: 不支持 3 层及以上或复杂表达式嵌套索引赋值", "IR");
}

IROperand AstIRBuilder::visitMemberAccess(MemberAccess* node) {
    // P1 fix: super.field → SUPER_MEMBER_GET（字段查找从 this 实例，实例已含继承字段）
    bool isSuper = node->object && node->object->nodeType == NodeType::NODE_SUPER_EXPR;
    // 注：super 在非方法上下文中的错误为运行时错误（详见 NODE_SUPER_EXPR 注释）。
    // visitMemberAccess 直接 emitLoadVar("this") 而非走 visitNode(SUPER_EXPR)，
    // 但 emitLoadVar 在顶层回退到 GLOBAL_NAME 查找，运行时报 "未定义的变量: this"。
    // SuperExpr 本身不产生值，super 解析为当前 this 实例
    IROperand obj = isSuper ? emitLoadVar("this", node->line) : visitNode(node->object.get());
    IROperand dest = ir_->allocVReg();
    uint32_t fieldIdx = ir_->addGlobal(node->fieldName);
    if (isSuper) {
        emitIR(IROp::SUPER_MEMBER_GET, {dest, obj, IROperand::field(fieldIdx)}, node->line);
    } else {
        emitIR(IROp::MEMBER_GET, {dest, obj, IROperand::field(fieldIdx)}, node->line);
    }
    return dest;
}

void AstIRBuilder::visitMemberAssign(MemberAssign* node) {
    // 限制2：检测 object 是否为简单 VarRef。若是，emit MEMBER_SET + WRITEBACK_MEMBER_VAR/LOCAL
    bool isVarRef = node->object && node->object->nodeType == NodeType::NODE_VAR_REF;
    IROperand obj = visitNode(node->object.get());
    IROperand val = visitNode(node->value.get());
    uint32_t fieldIdx = ir_->addGlobal(node->fieldName);
    emitIR(IROp::MEMBER_SET, {obj, IROperand::field(fieldIdx), val}, node->line);
    // 仅当 object 是 VarRef 时 emit 写回指令
    if (isVarRef) {
        VarRef* vr = static_cast<VarRef*>(node->object.get());
        VarInfo info = resolveVar(vr->name);
        if (info.kind == VarInfo::Kind::LOCAL) {
            emitIR(IROp::WRITEBACK_MEMBER_LOCAL, {IROperand::local(info.index), IROperand::field(fieldIdx)},
                   node->line);
        } else if (info.kind == VarInfo::Kind::GLOBAL_SLOT) {
            emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::imm(info.index), IROperand::field(fieldIdx)}, node->line);
        } else if (info.kind == VarInfo::Kind::GLOBAL_NAME) {
            emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::global(info.index), IROperand::field(fieldIdx)}, node->line);
        } else if (info.kind == VarInfo::Kind::UPVALUE) {
            // #7 fix: upvalue 容器字段赋值时需写回，否则 COW ensureUnique 产生的新容器
            // 副本会丢失，导致闭包内 obj.f=v 修改被静默吞掉。
            emitIR(IROp::WRITEBACK_MEMBER_UPVALUE, {IROperand::upvalue(info.index), IROperand::field(fieldIdx)},
                   node->line);
        }
        return;
    }

    // MEDIUM-1/2 fix: 2 层嵌套左值（base.outer.field = val）
    // R111 重构：嵌套路径写回逻辑提取到共享 helper emitNestedAssignWriteback（与 visitIndexAssign 共用）。
    if (emitNestedAssignWriteback(node->object.get(), node->line))
        return;

    // C-P2-2 fix: 3+ 层嵌套或复杂表达式：不支持，报错而非静默丢失修改
    Logger::Error("AstIRBuilder: 不支持 3 层及以上或复杂表达式嵌套成员赋值", "IR");
}

// R110 重构：visitMethodCall 提取变异后接收者写回块为单一职责 helper。
// 原 visitMethodCall 127 行拆为 ~37 行主函数 + emitMethodCallWriteback（~90 行）。
// 三类写回路径：(1) VarRef 接收者直接 LOAD_MUTATED+STORE；(2) super 调用写回 this 槽 0；
// (3) 嵌套接收者（IndexAccess(VarRef) | MemberAccess(VarRef)）通过 LOAD_MUTATED+INDEX_SET/MEMBER_SET+WRITEBACK 链。
// 逻辑对齐 compiler/Compiler.cpp::emitMethodCallWriteback（同名 helper，处理嵌套接收者路径），
// IR 路径额外处理 VarRef/SuperCall 因 IR 的 LOAD_MUTATED 语义统一覆盖三类。
void AstIRBuilder::emitMethodCallWriteback(MethodCall* node) {
    // CRITICAL-4 fix: 方法调用后写回变异后的接收者。
    // 栈式 VM 的变异内建方法（dispatchArrayBuiltin/dispatchDictBuiltin）在 recvVarIdx/recvSlot
    // 均为 NO_INDEX/NO_SLOT 时将变异后对象存入 lastMutatedReceiver_；实例方法（OP_RETURN）的
    // fallback 路径也设置 lastMutatedReceiver_。RegVM 的 callBuiltinMethod/executeReturnImpl
    // 将变异后对象存入 lastMutatedReceiverReg_。此处通过 LOAD_MUTATED 读取，再 STORE 写回。
    // 原 emitStoreVar(obj) 在栈式 VM 下会栈下溢（obj vreg 栈位置在 METHOD_CALL 后已被 pop），
    // 在 RegVM 下虽有效但与栈式 VM 行为不一致。统一改用 LOAD_MUTATED + STORE 模式。
    bool isVarRef = node->object && node->object->nodeType == NodeType::NODE_VAR_REF;
    bool isSuperCall = node->object && node->object->nodeType == NodeType::NODE_SUPER_EXPR;
    if (isVarRef) {
        VarRef* vr = static_cast<VarRef*>(node->object.get());
        IROperand mutated = ir_->allocVReg();
        emitIR(IROp::LOAD_MUTATED, {mutated}, node->line);
        emitStoreVar(vr->name, mutated, node->line);
        // BUG-INH-4 fix: LOAD_MUTATED push 值到栈顶，emitStoreVar 对 LOCAL/UPVALUE 使用
        // OP_SET_LOCAL/OP_SET_UPVALUE（peek 不 pop），值残留在栈上 dest 之上。后续基于 dest
        // 的 POP/RETURN 会错误地弹出此残留值。对 LOCAL/UPVALUE 显式 emit IROp::POP 消费。
        // GLOBAL_SLOT/GLOBAL_NAME 的 OP_SET_GLOBAL 已 pop，无需额外 POP。
        VarInfo vi = resolveVar(vr->name);
        if (vi.kind == VarInfo::Kind::LOCAL || vi.kind == VarInfo::Kind::UPVALUE) {
            emitIR(IROp::POP, {mutated}, node->line);
        }
    } else if (isSuperCall) {
        // super.method() 的接收者是 this（局部槽 0）。
        // executeReturnImpl 的字段同步只写回 objReg，不会写回槽 0。
        // 通过 LOAD_MUTATED + STORE("this") 把变异后的 this 写回槽 0。
        IROperand mutated = ir_->allocVReg();
        emitIR(IROp::LOAD_MUTATED, {mutated}, node->line);
        emitStoreVar("this", mutated, node->line);
        // BUG-INH-4 fix: 同 isVarRef 分支，LOAD_MUTATED 的残留值需 POP 消费。
        // this 是 LOCAL（槽 0），OP_SET_LOCAL 不 pop，必须补 POP。
        emitIR(IROp::POP, {mutated}, node->line);
    } else {
        // CRITICAL-4 extension: 嵌套接收者（IndexAccess(VarRef) 或 MemberAccess(VarRef)）
        // 方法调用后通过 LOAD_MUTATED + INDEX_SET/MEMBER_SET + WRITEBACK 链写回变异后接收者。
        // R111 重构：嵌套接收者写回逻辑提取到共享 helper emitNestedAssignWriteback
        // （与 visitIndexAssign / visitMemberAssign 三处共用，消除约 194 行重复代码）。
        // helper 返回 false 表示复杂表达式接收者，不写回（方法结果在 dest 中）。
        (void)emitNestedAssignWriteback(node->object.get(), node->line);
    }
}

IROperand AstIRBuilder::visitMethodCall(MethodCall* node) {
    // C-6 fix: 检测 object 是否为简单 VarRef。方法调用可能通过 COW 修改接收者实例
    // （this.field = ... 或 arr.push(...)），产生新的 InstanceData/ArrayData。
    // 若接收者是 VarRef，需在 METHOD_CALL 后 emit STORE 将变异后的接收者写回变量槽，
    // 否则全局/upvalue 中的原值不变，下次读取得到旧值（字段修改丢失）。
    bool isVarRef = node->object && node->object->nodeType == NodeType::NODE_VAR_REF;
    // P1 fix: super.method() → SUPER_CALL，方法查找从父类开始
    bool isSuperCall = node->object && node->object->nodeType == NodeType::NODE_SUPER_EXPR;
    // 注：super 在非方法上下文中的错误为运行时错误（详见 NODE_SUPER_EXPR 注释）。
    // visitMethodCall 直接 emitLoadVar("this") 而非走 visitNode(SUPER_EXPR)，
    // 但 emitLoadVar 在顶层回退到 GLOBAL_NAME 查找，运行时报 "未定义的变量: this"。
    // SuperExpr 本身不产生值，super 解析为当前 this 实例
    IROperand obj = isSuperCall ? emitLoadVar("this", node->line) : visitNode(node->object.get());
    IROperand dest = ir_->allocVReg();
    uint32_t methodIdx = ir_->addGlobal(node->methodName);
    std::vector<IROperand> ops;
    ops.push_back(dest);
    ops.push_back(obj);
    ops.push_back(IROperand::field(methodIdx));
    if (isSuperCall) {
        // SUPER_CALL 额外携带当前类名索引，供 VM 查找父类
        uint32_t classIdx = ir_->addGlobal(compilingClassName_);
        ops.push_back(IROperand::funcName(classIdx));
    }
    ops.push_back(IROperand::imm(static_cast<uint32_t>(node->arguments.size())));
    for (auto& a : node->arguments) {
        ops.push_back(visitNode(a.get()));
    }
    emitIR(isSuperCall ? IROp::SUPER_CALL : IROp::METHOD_CALL, ops, node->line);
    // R110 重构：原 ~90 行写回逻辑提取到 emitMethodCallWriteback。
    // helper 内部按 isVarRef/isSuperCall/嵌套接收者三类分支处理。
    // 注：isVarRef/isSuperCall 在 helper 内部重新计算（避免传递额外参数，保持 helper 自包含）。
    (void)isVarRef;
    (void)isSuperCall;
    emitMethodCallWriteback(node);
    return dest;
}

IROperand AstIRBuilder::visitInterpolatedString(InterpolatedString* node) {
    // P1 fix: 插值字符串 → IR 字符串拼接链
    // 语义：literals[0] + str(expr[0]) + literals[1] + ... + str(expr[n-1]) + literals[n]
    // 发射首个字面量片段（或 null 若为空）
    IROperand result;
    if (!node->literals.empty()) {
        result = emitConst(Value(node->literals[0]), node->line);
    } else {
        result = ir_->allocVReg();
        emitIR(IROp::LOAD_NULL, {result}, node->line);
    }
    // 交替发射：表达式 → ADD → 字面量 → ADD
    for (size_t i = 0; i < node->expressions.size(); ++i) {
        IROperand expr = visitNode(node->expressions[i].get());
        IROperand sum = ir_->allocVReg();
        emitIR(IROp::ADD, {sum, result, expr}, node->line);
        result = sum;
        if (i + 1 < node->literals.size() && !node->literals[i + 1].empty()) {
            IROperand lit = emitConst(Value(node->literals[i + 1]), node->line);
            IROperand sum2 = ir_->allocVReg();
            emitIR(IROp::ADD, {sum2, result, lit}, node->line);
            result = sum2;
        }
    }
    return result;
}

void AstIRBuilder::visitClassDecl(ClassDecl* node) {
    // C-9 fix: 完整携带类元数据（父类、字段顺序、方法名→函数名映射），
    // 使 REG_DEFINE_CLASS 能填充 classInfo_ 的 methods/fieldOrder/parent。
    // 原实现仅 emit DEFINE_CLASS{name}，导致 executeMethodCallImpl/executeClassNewImpl
    // 查 classInfo_[cls].methods 永远为空，类系统完全不可用。
    // BUG-IR-VARDECL-1 fix: 记录类名，供 visitVarDecl 检查类型注解是否为类名。
    definedClassNames_.insert(node->name);
    uint32_t nameIdx = ir_->addGlobal(node->name);
    uint32_t parentIdx = node->superClassName.empty() ? UINT32_MAX : ir_->addGlobal(node->superClassName);

    // 收集字段名和默认值常量索引（BUG-INH-1 fix: 原实现丢失字段默认值表达式，
    // 所有字段在 IR 路径下被初始化为 null。现在对字面量初始值提取 Value 并存入常量池，
    // 非字面量表达式保持 UINT32_MAX 标记 = null，对齐 CompilerClass.cpp visitClassDecl 直接路径）
    // BUG-INH-IR-1 fix: 非字面量表达式不再降级为 null，改为在 DEFINE_CLASS 之前 emit 求值 IR 序列，
    // 将求值结果存入临时局部变量，DEFINE_CLASS 携带局部变量槽位，后端 lowering 时 emit OP_GET_LOCAL + OP_INIT_FIELD。
    // 这对齐 Compiler.cpp 直接路径的 compileNode(initializer) 栈传递模式，实现三后端一致性。
    // L7 fix: 同时构建 allFieldNames（含继承字段，父类字段在前），供方法编译时注册为局部变量。
    //   对齐 Compiler.cpp visitClassDecl L4509-4529 的字段继承逻辑。
    std::vector<std::string> ownFieldNames;
    for (auto& m : node->members) {
        if (m && m->nodeType == NodeType::NODE_VAR_DECL) {
            ownFieldNames.push_back(static_cast<VarDecl*>(m.get())->name);
        }
    }
    // L7 fix: 构建含继承字段的完整列表（父类字段在前，子类字段在后，跳过覆盖的同名字段）
    std::vector<std::string> allFieldNames;
    if (!node->superClassName.empty()) {
        auto it = classFieldNames_.find(node->superClassName);
        if (it != classFieldNames_.end()) {
            allFieldNames = it->second; // 父类字段在前
        } else {
            // C-P2-9 fix 对齐: 父类未找到时报错
            irDiagnostics_.addError("类 '" + node->name + "' 的父类 '" + node->superClassName +
                                        "' 未定义（不支持前向引用，请确保父类在子类之前声明）",
                                    node->line, 0, DiagSource::Compiler);
            return;
        }
    }
    for (const auto& fn : ownFieldNames) {
        if (std::find(allFieldNames.begin(), allFieldNames.end(), fn) == allFieldNames.end()) {
            allFieldNames.push_back(fn); // 子类字段在后（跳过覆盖的同名字段）
        }
    }
    // L7 fix: 注册类字段名（供子类编译时查找父类字段顺序）
    classFieldNames_[node->name] = allFieldNames;
    // L7 fix: 设置当前编译类的字段列表，供 emitFunctionPrologue 在 compilingMethod_ 分支消费
    compilingClassFieldNames_ = allFieldNames;

    std::vector<uint32_t> fieldIdxs;
    std::vector<uint32_t> fieldDefaultConstIdxs; // BUG-INH-1 fix
    std::vector<uint32_t> fieldExprLocalSlots;   // BUG-INH-IR-1 fix: 非字面量表达式的临时局部变量槽位
    for (auto& m : node->members) {
        if (m && m->nodeType == NodeType::NODE_VAR_DECL) {
            VarDecl* vd = static_cast<VarDecl*>(m.get());
            fieldIdxs.push_back(ir_->addGlobal(vd->name));
            // BUG-INH-1 fix: 提取字面量默认值
            uint32_t defaultIdx = UINT32_MAX; // 默认 null
            uint32_t exprSlot = UINT32_MAX;   // BUG-INH-IR-1 fix: 非字面量表达式临时槽位
            if (vd->initializer) {
                Value defaultVal;
                if (extractConstant(vd->initializer.get(), defaultVal)) {
                    defaultIdx = ir_->addConstant(defaultVal);
                } else {
                    // BUG-INH-IR-1 fix: 非字面量表达式，emit 求值 IR 序列，存入临时局部变量。
                    // 对齐 BIN_AND/BIN_OR 短路路径的 STORE_LOCAL + POP 模式（IR.cpp visitBinaryOp 的 BIN_AND 分支）：
                    //   visitNode 推 vreg 到栈顶 → STORE_LOCAL peek 写入 slot → POP 清栈
                    // DEFINE_CLASS lowering 时 emit OP_GET_LOCAL slot 读取并 OP_INIT_FIELD 设置。
                    // local slot 在当前函数帧内有效，class 声明通常在顶层，slot 生命周期足够。
                    IROperand val = visitNode(vd->initializer.get());
                    exprSlot = nextLocalSlot_++;
                    emitIR(IROp::STORE_LOCAL, {IROperand::local(exprSlot), val}, vd->line);
                    emitIR(IROp::POP, {val}, vd->line); // 清栈（STORE_LOCAL peek 不 pop）
                }
            }
            fieldDefaultConstIdxs.push_back(defaultIdx);
            fieldExprLocalSlots.push_back(exprSlot);
        }
    }

    // 收集方法名 → 函数名映射。函数名使用 "ClassName.method" 作用域命名，
    // 避免多类同名方法（如 init/display）在 functionChunks_ 中冲突。
    std::vector<std::pair<uint32_t, uint32_t>> methodIdxs;
    for (auto& m : node->members) {
        if (m && m->nodeType == NodeType::NODE_FUN_DECL) {
            FunDecl* fd = static_cast<FunDecl*>(m.get());
            uint32_t methodIdx = ir_->addGlobal(fd->name);
            std::string scopedFunName = node->name + "." + fd->name;
            uint32_t funIdx = ir_->addGlobal(scopedFunName);
            methodIdxs.push_back({methodIdx, funIdx});
        }
    }

    // DEFINE_CLASS 操作数布局（BUG-INH-1 fix: 新增字段默认值常量索引；
    //   BUG-INH-IR-1 fix: 新增字段表达式临时局部变量槽位）：
    //   [0] className (FUNC_NAME)
    //   [1] parentName (IMM_UINT, UINT32_MAX=无父类)
    //   [2] fieldCount (IMM_UINT)
    //   [3+i*3] field name (FIELD_NAME)
    //   [3+i*3+1] fieldDefaultConstIdx (IMM_UINT, UINT32_MAX=null/无默认值/有表达式)
    //   [3+i*3+2] fieldExprLocalSlot (IMM_UINT, UINT32_MAX=使用常量或null, 否则使用临时局部变量)
    //   [3+3F] methodCount (IMM_UINT)
    //   [3+3F+1 .. ] (methodName FIELD_NAME, funName FUNC_NAME) × M
    std::vector<IROperand> ops;
    ops.push_back(IROperand::funcName(nameIdx));
    ops.push_back(IROperand::imm(parentIdx));
    ops.push_back(IROperand::imm(static_cast<uint32_t>(fieldIdxs.size())));
    for (size_t i = 0; i < fieldIdxs.size(); ++i) {
        ops.push_back(IROperand::field(fieldIdxs[i]));
        ops.push_back(IROperand::imm(fieldDefaultConstIdxs[i])); // BUG-INH-1 fix
        ops.push_back(IROperand::imm(fieldExprLocalSlots[i]));   // BUG-INH-IR-1 fix
    }
    ops.push_back(IROperand::imm(static_cast<uint32_t>(methodIdxs.size())));
    for (auto& mp : methodIdxs) {
        ops.push_back(IROperand::field(mp.first));
        ops.push_back(IROperand::funcName(mp.second));
    }
    emitIR(IROp::DEFINE_CLASS, std::move(ops), node->line);

    // 编译方法函数体：临时改名为 "ClassName.method" 以作用域隔离，
    // 设置 compilingMethod_ 标记让 visitFunDecl 预留 slot 0 给 this。
    for (auto& m : node->members) {
        if (m && m->nodeType == NodeType::NODE_FUN_DECL) {
            FunDecl* fd = static_cast<FunDecl*>(m.get());
            std::string origName = fd->name;
            fd->name = node->name + "." + origName;
            compilingMethod_ = true;
            compilingClassName_ = node->name;             // P1 fix: 供 super 调用查找父类
            compilingClassTypeParams_ = node->typeParams; // R163 泛型扩展: 供 emitFunctionBody 合并
            // L7 fix: compilingClassFieldNames_ 已在上方构建 allFieldNames 时设置，
            //   emitFunctionPrologue 会消费它将字段注册为局部变量
            try {
                visitFunDecl(fd);
            } catch (...) {
                // 异常路径恢复 AST 节点状态，防止编译失败后 AST 被永久修改
                // （调用方可能重用同一 AST 重试编译，如 IDE/REPL 场景）。
                fd->name = origName;
                compilingMethod_ = false;
                compilingClassName_.clear();
                compilingClassTypeParams_.clear();
                compilingClassFieldNames_.clear(); // L7 fix
                throw;
            }
            compilingMethod_ = false;
            compilingClassName_.clear();
            compilingClassTypeParams_.clear();
            fd->name = origName;
        }
    }
    // L7 fix: compilingClassFieldNames_ 在循环外（L3268）设置一次，供所有方法共享。
    //   原实现在此 clear 放在循环内，导致第一个方法编译后字段列表被清空，后续方法
    //   （如 norm()）无法解析裸字段访问 → "未定义的变量"。移到循环外确保所有方法
    //   都能命中 resolveVar 的 MEMBER 分支。
    compilingClassFieldNames_.clear();
}

// AUDIT-P1.1 fix: break/continue finally 续跳 IR 发射。
// 若 break/continue 在 try-finally 内，发射 PUSH_JUMP_TARGET + JUMP 续跳 IR。
// IR 路径用 label 而非 ip，label 在 visitTryStmt 进入时就分配，无需 deferred patch。
bool AstIRBuilder::emitFinallyJumpIR(int line, uint32_t realTargetLabel) {
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

    // 从最外层到最内层，依次发射 PUSH_JUMP_TARGET
    // 循环 i 从 N 递减到 1，对应 ctxIdx 从 finallyIndices[N-1]（outermost）到 finallyIndices[0]（innermost）。
    // push 顺序：outermost → innermost，使 pendingJumpStack_ 弹出顺序为 innermost → outermost，
    // 与 finally 执行顺序（内层先执行）匹配。
    // BUG-025 fix: 原代码 `if (i == 1)` 与 `finallyIndices[i - 2]` 索引反转。
    //   原意：最外层（i==N）push realTarget，内层 push 下一个外层 finally 的 entry。
    //   实际：最内层（i==1）push realTarget，外层 push 下一个内层 finally 的 entry。
    //   修复：判据改为 `if (i == finallyIndices.size())`，索引改为 `finallyIndices[i]`。
    //   IR 路径用 label 而非 ip，无 patch 位置问题（BUG-026 仅影响 StackVM 直接路径）。
    for (size_t i = finallyIndices.size(); i > 0; --i) {
        uint32_t targetLabel;
        if (i == finallyIndices.size()) {
            // 最外层：push 真实跳转目标（breakTarget/continueTarget）
            targetLabel = realTargetLabel;
        } else {
            // 内层：push 下一个（更外层）finally 入口 label。finallyIndices[i] 比 finallyIndices[i-1] 更外。
            size_t nextCtxIdx = finallyIndices[i];
            targetLabel = tryFinallyStack_[nextCtxIdx].finallyEntryLabel;
        }
        emitIR(IROp::PUSH_JUMP_TARGET, {IROperand::label(targetLabel)}, line);
    }

    // 发射 JUMP 到最内层 finally 入口
    emitIR(IROp::JUMP, {IROperand::label(tryFinallyStack_[finallyIndices[0]].finallyEntryLabel)}, line);
    return true;
}

void AstIRBuilder::visitBreakStmt(BreakStmt* node) {
    if (loopStack_.empty()) {
        // L1 fix: 循环外 break 必须报告编译错误，对齐 Compiler.cpp visitBreakStmt 的 error() 行为。
        // 原实现仅调用 Logger::Error 记录日志但不设置 irDiagnostics_，导致编译"成功"且
        // break 语句被静默忽略（不 emit 任何 IR），运行时产生空输出——三后端不一致。
        irDiagnostics_.addError("break 只能在循环体内使用", node->line, 0, DiagSource::Compiler);
        return;
    }
    // BUG-EXC-2 fix: break 跳出循环时，需为循环内的每个 try 块发射 TRY_END 弹出 handler，
    // 对齐 Compiler.cpp visitBreakStmt 的 tryDepthInLoop 逻辑，否则 tryStack_ handler 泄漏。
    int tryDepthInLoop = tryDepth_ - loopStack_.back().tryDepthAtStart;
    for (int i = 0; i < tryDepthInLoop; ++i) {
        emitIR(IROp::TRY_END, {}, node->line);
    }
    // AUDIT-P2-CORRECT fix: break 跳出循环时需关闭循环体内声明的闭包捕获变量的
    // upvalue，对齐正常迭代退出时的 leaveBlockScope（CLOSE_UPVALUE）发射。
    // 原 visitBreakStmt 直接发 JUMP 到 endLabel（在 leaveBlockScope 之后），
    // 跳过 upvalue 关闭，导致闭包捕获变 by-reference（三后端不一致）。
    const auto& loopCtx = loopStack_.back();
    if (loopCtx.needCloseUpvalue) {
        emitIR(IROp::CLOSE_UPVALUE, {IROperand::imm(loopCtx.bodySlotBase)}, node->line);
    }
    // AUDIT-P1.1 fix: 若 break 在 try-finally 内，发射续跳 IR；否则走常规 JUMP。
    if (!emitFinallyJumpIR(node->line, loopStack_.back().endLabel)) {
        emitIR(IROp::JUMP, {IROperand::label(loopStack_.back().endLabel)}, node->line);
    }
}

void AstIRBuilder::visitContinueStmt(ContinueStmt* node) {
    if (loopStack_.empty()) {
        // L1 fix: 循环外 continue 必须报告编译错误，对齐 Compiler.cpp visitContinueStmt 的 error() 行为。
        // 原实现仅调用 Logger::Error 记录日志但不设置 irDiagnostics_，导致编译"成功"且
        // continue 语句被静默忽略（不 emit 任何 IR），运行时产生空输出——三后端不一致。
        irDiagnostics_.addError("continue 只能在循环体内使用", node->line, 0, DiagSource::Compiler);
        return;
    }
    // BUG-EXC-2 fix: continue 跳回循环开头时，同样需为循环内的 try 块发射 TRY_END。
    int tryDepthInLoop = tryDepth_ - loopStack_.back().tryDepthAtStart;
    for (int i = 0; i < tryDepthInLoop; ++i) {
        emitIR(IROp::TRY_END, {}, node->line);
    }
    // AUDIT-P1.1 fix: 若 continue 在 try-finally 内，发射续跳 IR；否则走常规 JUMP。
    if (!emitFinallyJumpIR(node->line, loopStack_.back().continueLabel)) {
        emitIR(IROp::JUMP, {IROperand::label(loopStack_.back().continueLabel)}, node->line);
    }
}

void AstIRBuilder::visitTryStmt(TryStmt* node) {
    // BUG-AUDIT-FINALLY-1: 如果有 finally 块，外层包一层 TRY_BEGIN/TRY_END，
    // 捕获异常路径执行 finally 后 rethrow。对齐 Compiler.cpp visitTryStmt 的外层包装模式。
    //
    // 重构：按 try/catch/finally 三阶段拆分为 emitTryBlock / emitCatchBlock / emitFinallyBlock，
    // 通过 TryEmitCtx 传递 finally 相关 label 与 endLabel。三套 catch 路径
    // （函数内 / 顶层遮蔽保护 / 顶层无遮蔽）在 emitCatchBlock 内部分发，避免 finally
    // IR 发射逻辑重复 4 份。BUG-025/BUG-026/AUDIT-P1.1/AUDIT-BUG-F7 等审计点保留至各子方法。
    TryEmitCtx ctx;
    ctx.hasFinally = (node->finallyBlock != nullptr);
    if (ctx.hasFinally) {
        ctx.finallyCatchLabel = ir_->allocLabel();
        ctx.finallyEndLabel = ir_->allocLabel();
        ctx.finallyEntryLabel = ir_->allocLabel(); // AUDIT-P1.1 fix: finally 正常路径入口 label
        emitIR(IROp::TRY_BEGIN, {IROperand::label(ctx.finallyCatchLabel)}, node->line);
        ++tryDepth_; // 外层 try 计入深度（break/continue 多发一个 TRY_END）
    }
    // AUDIT-P1.1 fix: push try-finally 编译期上下文（label 在此分配，break/continue 可直接引用）
    tryFinallyStack_.push_back({ctx.hasFinally, ctx.finallyEntryLabel, {}, {}});
    // BUG-AUDIT-FINALLY-1: try-finally（无 catch）路径。
    // catchVarName 为空时跳过内层 TRY_BEGIN/TRY_END/catchLabel，
    // 外层 TRY_BEGIN(finallyCatchLabel) 捕获异常 → 执行 finally → rethrow。
    ctx.endLabel = ir_->allocLabel();

    if (!node->catchVarName.empty()) {
        // catch 路径：发射 try 块 + catch 块（含 finally 续跳链）。
        // emitCatchBlock 内部按 inFunction_/existingSlot 分发到三种 catch 变体，
        // 每个变体负责发射自己的 finally（如有）并 pop tryFinallyStack_。
        ctx.catchLabel = ir_->allocLabel();
        emitTryBlock(*node, ctx);
        // P1-4 fix: catch 块起始，将异常值加载到 vreg 并绑定到 catch 变量。
        // RegisterVM: throwException 将异常存入 pendingException_，REG_LOAD_EXCEPTION 读取。
        // 栈式 VM: throwException 将异常推入栈顶，BytecodeIRBackend 的 LOAD_EXCEPTION 为 no-op（值已在栈上）。
        IROperand excVreg = ir_->allocVReg();
        emitIR(IROp::LOAD_EXCEPTION, {excVreg}, node->line);
        emitCatchBlock(*node, excVreg, ctx);
        return;
    }
    // try-finally（无 catch）：只编译 try 块，不发射内层 try-catch。
    if (node->tryBlock)
        visitNode(node->tryBlock.get());
    // BUG-AUDIT-FINALLY-1: catchVarName 为空时（try-finally 无 catch），
    // 不发射内层 TRY_BEGIN，异常不在栈上，无需 POP。
    // 原 L1 fix 的 POP 仅在 catchVarName 非空但 catchBlock 为空时才需要
    // （但 Parser 要求 catch 必须有变量名，所以 catchVarName 非空时 catchBlock 也非空）。
    if (node->catchBlock)
        visitNode(node->catchBlock.get());
    emitIR(IROp::LABEL, {IROperand::label(ctx.endLabel)}, node->line);
    // BUG-AUDIT-FINALLY-1: finally 块 IR 发射
    if (ctx.hasFinally) {
        emitFinallyBlock(*node, ctx);
    }
    tryFinallyStack_.pop_back(); // AUDIT-P1.1 fix
}

void AstIRBuilder::emitTryBlock(TryStmt& node, TryEmitCtx& ctx) {
    // 发射 try 块的 TRY_BEGIN/tryBlock/TRY_END/JUMP endLabel/LABEL catchLabel 序列。
    // 仅在 catchVarName 非空时调用（ctx.catchLabel 已由 visitTryStmt 分配）。
    // BUG-EXC-2 fix: 跟踪 try 嵌套深度，break/continue 需为差额层级发射 TRY_END
    emitIR(IROp::TRY_BEGIN, {IROperand::label(ctx.catchLabel)}, node.line);
    ++tryDepth_;
    if (node.tryBlock)
        visitNode(node.tryBlock.get());
    --tryDepth_;
    emitIR(IROp::TRY_END, {}, node.line);
    emitIR(IROp::JUMP, {IROperand::label(ctx.endLabel)}, node.line);
    // catch 块入口
    emitIR(IROp::LABEL, {IROperand::label(ctx.catchLabel)}, node.line);
}

void AstIRBuilder::emitCatchBlock(TryStmt& node, IROperand excVreg, TryEmitCtx& ctx) {
    // catch 块分发：按 inFunction_ / existingSlot 选择三种变体之一。
    // 每个变体负责发射自己的 finally（通过 emitFinallyBlock）并 pop tryFinallyStack_。
    if (inFunction_) {
        emitCatchInFunction(node, excVreg, ctx);
        return;
    }
    // 顶层：检查 catchVarName 是否与全局槽位变量同名（对齐 CompilerStmt.cpp visitTryStmt）
    int existingSlot = lookupGlobalSlot(node.catchVarName);
    if (existingSlot >= 0) {
        emitCatchWithShadowSave(node, excVreg, existingSlot, ctx);
    } else {
        emitCatchGlobal(node, excVreg, ctx);
    }
}

void AstIRBuilder::emitCatchInFunction(TryStmt& node, IROperand excVreg, TryEmitCtx& ctx) {
    // 函数内 catch：分配局部 slot 并绑定 catch 变量。
    uint32_t slot = nextLocalSlot_++;
    // BUG-IDE-12 fix: 记录 catch 变量 slot→name 映射
    if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
        localSlotNames_.resize(slot + 1);
    }
    localSlotNames_[slot] = node.catchVarName;
    // L1 fix: 记录 catch 变量到 slotNameRanges_，endInstr 暂设为 0（CLOSE_UPVALUE 后回填）
    size_t curIdx = 0;
    for (const auto& blk : ir_->blocks)
        curIdx += blk.instructions.size();
    slotNameRanges_.push_back({slot, node.catchVarName, curIdx, 0});
    // AUDIT-BUG-F7 fix: 保存 varMap_ 旧条目，catch 块编译后恢复。
    // 原实现 catch 变量绑定到外层 varMap_ 且不恢复，catch 块后仍可引用，
    // 与 Interpreter/StackVM 语义不一致（后者 catch 变量仅 catch 块内可见）。
    auto savedIt_f7 = varMap_.find(node.catchVarName);
    VarInfo savedInfo_f7{};
    bool hadSaved_f7 = (savedIt_f7 != varMap_.end());
    if (hadSaved_f7)
        savedInfo_f7 = savedIt_f7->second;
    varMap_[node.catchVarName] = {VarInfo::Kind::LOCAL, slot};
    if (static_cast<int>(slot + 1) > ir_->localCount) {
        ir_->localCount = static_cast<int>(slot + 1);
        // AUDIT-TRYLOCAL fix: 对齐 build()/leaveBlockScope()/visitFunDecl() 的 P2-1 fix，
        // catch 变量 slot 绑定路径也需 localCount > 32 上限告警。
        // RegisterBytecodeBackend::lower() 的硬门会兜底拦截，此处仅早期留痕。
        if (ir_->localCount > 32) {
            Logger::Error("AstIRBuilder: localCount 超出 32 寄存器上限 (" + std::to_string(ir_->localCount) +
                              "，函数 " + ir_->name + ")，RegisterVM 将无法加载此函数",
                          "IR");
        }
    }
    emitIR(IROp::STORE_LOCAL, {IROperand::local(slot), excVreg}, node.line);
    // L1 fix: STORE_LOCAL → OP_SET_LOCAL（peek 不 pop），异常值残留在栈上。
    // 对齐直接 Compiler.cpp visitTryStmt 的模式：OP_SET_LOCAL 后紧跟 OP_POP 消费异常值。
    // 顶层 GLOBAL 路径用 DEFINE_GLOBAL → OP_DEFINE_VAR（pop），无需额外 POP。
    emitIR(IROp::POP, {excVreg}, node.line);
    // Bug 5 fix: 记录到当前 BlockScope，使 leaveBlockScope 能清除 catchVar 的 varMap_ 条目。
    // catch 变量作用域限于 catch 块，块退出后不应再被引用。
    if (!blockScopes_.empty()) {
        blockScopes_.back().localSlots.push_back(slot);
    }
    // AUDIT-BUG-F7 fix: catch 块在此编译（而非延后到统一位置），编译后立即恢复 varMap_。
    if (node.catchBlock)
        visitNode(node.catchBlock.get());
    // AUDIT-P2-CORRECT fix: catch 块退出后立即关闭 catch 变量的 upvalue，
    // 对齐 Compiler.cpp visitTryStmt（行 2141-2144）在 catch 块后立即发射 OP_CLOSE_UPVALUE。
    // 原实现将 catch 变量 slot 记录到外层 BlockScope.localSlots，依赖外层块退出时
    // leaveBlockScope 统一关闭，导致闭包捕获 catch 变量时 upvalue 快照时机晚于直接路径，
    // 三后端语义不一致（StackVM 直接路径立即关闭，IR 路径延迟到外层块退出）。
    // CLOSE_UPVALUE(slot) 关闭所有 slot >= catchVarSlot 的 open upvalue，
    // 此时 catch 块内部 slot 已由 catch 块的 leaveBlockScope 关闭，仅 catch 变量本身未关闭。
    emitIR(IROp::CLOSE_UPVALUE, {IROperand::imm(slot)}, node.line);
    // L1 fix: 回填 catch 变量 range 的 endInstr（CLOSE_UPVALUE 已 emit）
    {
        size_t endIdx = 0;
        for (const auto& blk : ir_->blocks)
            endIdx += blk.instructions.size();
        for (auto& range : slotNameRanges_) {
            if (range.endInstr == 0 && range.slot >= slot) {
                range.endInstr = endIdx;
            }
        }
    }
    if (hadSaved_f7) {
        varMap_[node.catchVarName] = std::move(savedInfo_f7);
    } else {
        varMap_.erase(node.catchVarName);
    }
    emitIR(IROp::LABEL, {IROperand::label(ctx.endLabel)}, node.line);
    // BUG-AUDIT-FINALLY-1: finally 块 IR 发射
    if (ctx.hasFinally) {
        emitFinallyBlock(node, ctx);
    }
    tryFinallyStack_.pop_back(); // AUDIT-P1.1 fix
    // AUDIT-BUG-F7: catch 块已编译，提前返回（由 emitCatchBlock 调用方语义保证）
}

void AstIRBuilder::emitCatchWithShadowSave(TryStmt& node, IROperand excVreg, int existingSlot, TryEmitCtx& ctx) {
    // BUG-IR-SHADOW-SAVE fix: 遮蔽保护——原值保存到临时 name-based 全局变量。
    // 原实现用 vreg(saved) 保存，但 StackVM 后端的 LOAD_EXCEPTION 是 no-op
    // （异常值已在栈上），LOAD_GLOBAL 再 push 会使 DEFINE_GLOBAL pop 错误值
    // （saved 而非 exception）。改用临时全局变量对齐 Compiler.cpp 的
    // __catch_save_<counter>_<name> 模式，值存储在 globals_ 中不受栈变化影响。
    // 1. 加载原值到栈顶（StackVM: push; RegisterVM: 写 vreg）
    IROperand saved = ir_->allocVReg();
    emitIR(IROp::LOAD_GLOBAL, {saved, IROperand::imm(static_cast<uint32_t>(existingSlot))}, node.line);
    // 2. 保存到临时全局变量 __catch_save_<counter>_<catchVarName>
    std::string saveName = "__catch_save_" + std::to_string(catchSaveCounter_++) + "_" + node.catchVarName;
    uint32_t saveIdx = ir_->addGlobal(saveName);
    emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(saveIdx), saved}, node.line);
    // 3. 移除映射 + 定义 catch 变量（exception 在栈顶，DEFINE_GLOBAL pop）
    globalSlotAllocator_.removeMapping(node.catchVarName);
    uint32_t idx = ir_->addGlobal(node.catchVarName);
    varMap_[node.catchVarName] = {VarInfo::Kind::GLOBAL_NAME, idx};
    emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(idx), excVreg}, node.line);
    // BUG-IR-TRY-CATCH-WRAP fix: 用内层 TRY_BEGIN 包装 catch 块，确保 catch 块内
    // throw 时 cleanup IR（恢复遮蔽全局原值）仍执行。对齐 Compiler.cpp
    // visitTryStmt L1794-L1858 的 needsCleanupWrap 模式。
    uint32_t cleanupThrowLabel = ir_->allocLabel();
    emitIR(IROp::TRY_BEGIN, {IROperand::label(cleanupThrowLabel)}, node.line);
    // BUG-AUDIT-EXC-CLEANUP-TRYDEPTH fix: cleanup wrap 的内层 TRY_BEGIN
    // 必须计入 tryDepth_，使 catch 块内的 break/continue 能发射对应 TRY_END，
    // 避免 tryStack_ handler 残留导致后续异常被错误捕获到已失效的 cleanupThrowLabel。
    ++tryDepth_;
    if (node.catchBlock)
        visitNode(node.catchBlock.get());
    --tryDepth_;
    emitIR(IROp::TRY_END, {}, node.line);
    // 正常路径：恢复映射 + varMap_ + 原值（从临时全局变量重载）
    globalSlotAllocator_.restoreMapping(node.catchVarName, existingSlot);
    varMap_[node.catchVarName] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(existingSlot)};
    IROperand savedRestore = ir_->allocVReg();
    emitIR(IROp::LOAD_GLOBAL, {savedRestore, IROperand::global(saveIdx)}, node.line);
    emitIR(IROp::STORE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(existingSlot)), savedRestore}, node.line);
    emitIR(IROp::DELETE_VAR, {IROperand::global(saveIdx)}, node.line);
    emitIR(IROp::JUMP, {IROperand::label(ctx.endLabel)}, node.line);
    // 异常路径：从临时全局变量重载原值 + 恢复 + rethrow
    // StackVM: 异常值在栈顶，LOAD_EXCEPTION 为 no-op，LOAD_GLOBAL push savedRestore2，
    //          STORE_GLOBAL pop savedRestore2，THROW(OP_THROW) pop 栈顶异常值 rethrow。
    // RegisterVM: 异常值在 pendingException_，LOAD_EXCEPTION 加载到 excVreg2，
    //             LOAD_GLOBAL/STORE_GLOBAL 操作寄存器，THROW(REG_THROW) 读取 excVreg2 rethrow。
    emitIR(IROp::LABEL, {IROperand::label(cleanupThrowLabel)}, node.line);
    IROperand excVreg2 = ir_->allocVReg();
    emitIR(IROp::LOAD_EXCEPTION, {excVreg2}, node.line);
    IROperand savedRestore2 = ir_->allocVReg();
    emitIR(IROp::LOAD_GLOBAL, {savedRestore2, IROperand::global(saveIdx)}, node.line);
    emitIR(IROp::STORE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(existingSlot)), savedRestore2}, node.line);
    emitIR(IROp::DELETE_VAR, {IROperand::global(saveIdx)}, node.line);
    emitIR(IROp::THROW, {excVreg2}, node.line);
    emitIR(IROp::LABEL, {IROperand::label(ctx.endLabel)}, node.line);
    // BUG-AUDIT-FINALLY-1: finally 块 IR 发射
    if (ctx.hasFinally) {
        emitFinallyBlock(node, ctx);
    }
    tryFinallyStack_.pop_back(); // AUDIT-P1.1 fix
    // catch 块已编译，提前返回（由 emitCatchBlock 调用方语义保证）
}

void AstIRBuilder::emitCatchGlobal(TryStmt& node, IROperand excVreg, TryEmitCtx& ctx) {
    // 无遮蔽：直接定义为全局变量
    // BUG-IR-TRY-1 fix: 使用 GLOBAL_NAME kind（→ OP_DEFINE_VAR）而非 IMM_UINT
    // kind（→ OP_DEFINE_GLOBAL slot）。原因：OP_DELETE_VAR 对 slot-based 变量置
    // null（仍可访问），对 name-based 变量从 globals_ 擦除（→ undefined）。
    // 对齐 StackVM Compiler.cpp visitTryStmt L1770-L1772 的 OP_DEFINE_VAR 语义。
    uint32_t nameIdx = ir_->addGlobal(node.catchVarName);
    varMap_[node.catchVarName] = {VarInfo::Kind::GLOBAL_NAME, nameIdx};
    emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(nameIdx), excVreg}, node.line);
    // BUG-IR-TRY-CATCH-WRAP fix: 用内层 TRY_BEGIN 包装 catch 块，确保 catch 块内
    // throw 时 cleanup IR（DELETE_VAR 清理 catch 变量）仍执行。
    uint32_t cleanupThrowLabel = ir_->allocLabel();
    emitIR(IROp::TRY_BEGIN, {IROperand::label(cleanupThrowLabel)}, node.line);
    // BUG-AUDIT-EXC-CLEANUP-TRYDEPTH fix: 同步 ++tryDepth_/--tryDepth_，
    // 使 catch 块内 break/continue 正确发射 TRY_END。
    ++tryDepth_;
    if (node.catchBlock)
        visitNode(node.catchBlock.get());
    --tryDepth_;
    emitIR(IROp::TRY_END, {}, node.line);
    // 正常路径：移除 varMap_ 映射 + 清理 catch 变量
    varMap_.erase(node.catchVarName);
    emitIR(IROp::DELETE_VAR, {IROperand::global(nameIdx)}, node.line);
    emitIR(IROp::JUMP, {IROperand::label(ctx.endLabel)}, node.line);
    // 异常路径：清理 catch 变量 + rethrow
    emitIR(IROp::LABEL, {IROperand::label(cleanupThrowLabel)}, node.line);
    IROperand excVreg2 = ir_->allocVReg();
    emitIR(IROp::LOAD_EXCEPTION, {excVreg2}, node.line);
    emitIR(IROp::DELETE_VAR, {IROperand::global(nameIdx)}, node.line);
    emitIR(IROp::THROW, {excVreg2}, node.line);
    emitIR(IROp::LABEL, {IROperand::label(ctx.endLabel)}, node.line);
    // BUG-AUDIT-FINALLY-1: finally 块 IR 发射
    if (ctx.hasFinally) {
        emitFinallyBlock(node, ctx);
    }
    tryFinallyStack_.pop_back(); // AUDIT-P1.1 fix
    // AUDIT-BUG-F7: catch 块已编译，提前返回（由 emitCatchBlock 调用方语义保证）
}

void AstIRBuilder::emitFinallyBlock(TryStmt& node, TryEmitCtx& ctx) {
    // BUG-AUDIT-FINALLY-1 / AUDIT-P1.1 fix: finally 块 IR 发射。
    // 仅在 ctx.hasFinally == true 时调用。发射序列：
    //   TRY_END（关闭外层 finally try）
    //   LABEL finallyEntryLabel（正常路径入口，供 break/continue 续跳）
    //   <finallyBlock>            （正常路径执行 finally）
    //   FINALLY_END                （break/continue 续跳出口 / 正常完成）
    //   JUMP finallyEndLabel       （跳过异常路径）
    //   LABEL finallyCatchLabel    （异常路径入口）
    //   LOAD_EXCEPTION excVregF    （加载捕获的异常）
    //   <finallyBlock>             （异常路径执行 finally）
    //   THROW excVregF             （rethrow 原异常）
    //   LABEL finallyEndLabel      （finally 块统一出口）
    // 注意：tryFinallyStack_ 的 pop 由调用方负责（与原实现的 pop 时机一致）。
    --tryDepth_;
    emitIR(IROp::TRY_END, {}, node.line);
    // AUDIT-R6 B1 fix(A): 发射 finally 体（两个副本）期间暂时弹出自身条目。
    // 原实现中 finally 体内的 break/continue/return 经 emitFinallyJumpIR 把自身当作
    // enclosing finally，发射 JUMP 回自身 finallyEntryLabel → 运行时 finally 无限重执行
    // （实证：死循环被指令数上限拦截）。弹出后 finally 体内的 break 正确链到更外层
    // finally 或直走常规跳转；结束后压回，保持调用方统一 pop 语义。
    TryFinallyContext selfFinallyCtx = std::move(tryFinallyStack_.back());
    tryFinallyStack_.pop_back();
    // AUDIT-P1.1 fix: 标记 finally 正常路径入口，供 break/continue 续跳
    emitIR(IROp::LABEL, {IROperand::label(ctx.finallyEntryLabel)}, node.line);
    visitNode(node.finallyBlock.get());
    // AUDIT-P1.1 fix: finally 末尾追加 FINALLY_END（break/continue 续跳或正常完成）
    emitIR(IROp::FINALLY_END, {}, node.line);
    emitIR(IROp::JUMP, {IROperand::label(ctx.finallyEndLabel)}, node.line);
    emitIR(IROp::LABEL, {IROperand::label(ctx.finallyCatchLabel)}, node.line);
    IROperand excVregF = ir_->allocVReg();
    emitIR(IROp::LOAD_EXCEPTION, {excVregF}, node.line);
    // AUDIT-R6 B1 fix(B): 异常值先暂存到唯一临时全局（栈式 lowering 为 OP_DEFINE_VAR，
    // 把 throwException 压栈的异常值 pop 走），finally 体在干净栈上执行；正常走完
    // 后 reload + rethrow。若 finally 体内 break/continue/return 跳出，reload+rethrow
    // 被跳过——待处理异常被丢弃（Java 式语义，与 Interpreter/StackVM 直接路径统一）
    // 且无栈残留。临时名按 try 站点命名（复用 catchSaveCounter_），机制对齐
    // emitCatchGlobal 的 GLOBAL_NAME 存取/DELETE_VAR 清理。
    uint32_t finallyExcIdx = ir_->addGlobal("__finally_exc_" + std::to_string(catchSaveCounter_++));
    emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(finallyExcIdx), excVregF}, node.line);
    visitNode(node.finallyBlock.get());
    IROperand excReload = ir_->allocVReg();
    emitIR(IROp::LOAD_GLOBAL, {excReload, IROperand::global(finallyExcIdx)}, node.line);
    emitIR(IROp::DELETE_VAR, {IROperand::global(finallyExcIdx)}, node.line);
    emitIR(IROp::THROW, {excReload}, node.line);
    emitIR(IROp::LABEL, {IROperand::label(ctx.finallyEndLabel)}, node.line);
    // AUDIT-R6 B1: 压回自身条目（调用方统一 pop）
    tryFinallyStack_.push_back(std::move(selfFinallyCtx));
}

void AstIRBuilder::visitThrowStmt(ThrowStmt* node) {
    IROperand val = node->expression ? visitNode(node->expression.get()) : IROperand::vreg(0);
    emitIR(IROp::THROW, {val}, node->line);
}
