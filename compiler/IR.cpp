#include "compiler/IR.h"
#include "Logger.h"
#include "ast/ASTNode.h"
#include "ast/ModuleIsolation.h"      // BUG-AUDIT-MOD-2: IR 模块隔离（非导出顶层名前缀化）
#include "common/RuntimeLimits.h"     // BUG-AUDIT-MOD-3: MAX_RECURSION_DEPTH
#include "interpreter/NumericUtils.h" // #14: OverflowCheck
#include "interpreter/Value.h"
#include "lexer/Lexer.h"   // VM-IMPORT: 模块源码词法分析
#include "parser/Parser.h" // VM-IMPORT: 模块源码语法分析
#include <cassert>
#include <cmath>
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

/// 表达式语句是否需要 emit POP：与 Compiler.cpp:365-386 的 16 种节点类型对齐。
/// IR 路径的 BytecodeIRBackend lowering 会将这些节点的 vreg 物化为 StackVM 栈值
/// （LOAD_CONST→OP_INT、ADD→OP_ADD、CALL→OP_CALL 等均压栈），
/// 表达式语句的结果不被消费，需 POP 防止栈泄漏。
/// 声明/控制流节点（VarDecl/IfStmt/WhileStmt 等）已自行平衡栈，不需 POP。
bool needsPopForExprStmt(NodeType nt) {
    switch (nt) {
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
    case NodeType::NODE_INTERPOLATED_STRING:
        return true;
    default:
        return false;
    }
}

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
        visitNode(stmt.get());
        // BUG-FIX: import 失败后立即中止，避免基于不完整状态继续生成坏 IR
        if (hasError_)
            return nullptr;
        // 表达式语句的返回值未被消费，需 emit POP 防止栈式 VM (BytecodeIRBackend) 栈泄漏。
        // RegisterBytecodeBackend 中 POP 是 no-op，不影响寄存器式 VM。
        // BytecodeIRBackend 将所有表达式 vreg 物化为 StackVM 栈值
        // （LOAD_CONST→OP_INT、ADD→OP_ADD、CALL→OP_CALL 等均压栈），
        // 故需对全部 16 种表达式语句节点 emit POP，与 Compiler.cpp:365-386 对齐。
        // visitAssignment 对 GLOBAL 存储已 emit LOAD 重载值，确保 POP 安全。
        if (needsPopForExprStmt(stmt->nodeType)) {
            emitIR(IROp::POP, {}, stmt->line);
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
    module_->mainFunction = std::move(ir_);
    // 返回 ir_（转移所有权给调用者；module_ 通过 getModule() 仍可访问 functions）
    return std::move(module_->mainFunction);
}

// ---- 全局槽位管理（限制3 / B4: 已内联到 IR.h，委托给 globalSlotAllocator_）----

void AstIRBuilder::preScanTopLevelDecls(Block& program, bool isMainModule) {
    // 遍历顶层语句，为 VarDecl/ClassDecl/FunDecl/ExportStmt 分配全局槽位
    // BUG-FIX: 原实现漏掉 NODE_FUN_DECL 和 NODE_EXPORT_STMT，导致前向函数引用
    // 和主程序 export 声明在 IR 路径下全局槽位未预分配。对齐 Compiler::preScanModuleGlobals。
    // BUG-IR-PRES-1 fix: 主模块不预扫描 FunDecl（对齐 compile() 的有意不对称设计）。
    // compile() 不预扫描 FunDecl 是为了避免 `var f = funName` 静默 null——函数声明
    // 在运行时才将函数值写入全局槽位，预扫描会提前分配槽位但值为 null，导致前向引用
    // 返回 null 而非报错。模块预扫描仍包含 FunDecl（visitImportStmt 用 lookupGlobalSlot
    // 验证导出名是否存在）。
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
            if (!isMainModule) { // BUG-IR-PRES-1 fix: 主模块跳过 FunDecl
                allocateGlobalSlot(static_cast<FunDecl*>(stmt.get())->name);
            }
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
                    if (!isMainModule) { // BUG-IR-PRES-1 fix: 主模块跳过 FunDecl
                        allocateGlobalSlot(static_cast<FunDecl*>(decl)->name);
                    }
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

void AstIRBuilder::handleImportStmt(ImportStmt& node) {
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

    // 1. 路径规范化与安全校验（对齐 Compiler::normalizeModulePath）
    std::string path = node.modulePath;
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    if (path.size() >= 2 && path[0] == '.' && path[1] == '/')
        path.erase(0, 2);
    if (path.empty()) {
        hasError_ = true;
        errorMessage_ = "模块路径不能为空";
        errorLine_ = node.line;
        return;
    }
    // BUG-MOD-1 fix: 与 Compiler::normalizeModulePath 保持一致——拒绝所有 'X:' 开头形式，
    // 覆盖 'C:/'、'C:foo'、'D:path' 等 Windows 驱动器路径。
    if (path[0] == '/' || (path.size() >= 2 && path[1] == ':')) {
        hasError_ = true;
        errorMessage_ = "模块路径不能为绝对路径: " + path;
        errorLine_ = node.line;
        return;
    }
    // ".." 路径段检测
    for (size_t pos = 0; pos < path.size();) {
        size_t next = path.find('/', pos);
        std::string seg = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        if (seg == "..") {
            hasError_ = true;
            errorMessage_ = "模块路径不能包含父目录引用 '..': " + path;
            errorLine_ = node.line;
            return;
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }

    // 2. run-once 检查
    if (linkedModuleSet_.count(path)) {
        // BUG-AUDIT-MOD-1 fix: 已编译模块的具名导入验证也检查 export 集合（对齐 Interpreter）
        if (!node.importAll && !node.names.empty()) {
            auto expIt = moduleExports_.find(path);
            if (expIt != moduleExports_.end()) {
                for (const auto& name : node.names) {
                    if (expIt->second.find(name) == expIt->second.end()) {
                        hasError_ = true;
                        errorMessage_ = "模块 " + path + " 中未导出名称: " + name;
                        errorLine_ = node.line;
                        return;
                    }
                }
            }
        }
        return;
    }

    // 3. 循环依赖检测
    if (moduleLoadingSet_.count(path)) {
        hasError_ = true;
        errorMessage_ = "检测到循环依赖: " + path;
        errorLine_ = node.line;
        return;
    }

    // BUG-AUDIT-MOD-3 fix: 模块加载深度保护（对齐 InterpreterModules.cpp:76-78）
    if (moduleLoadingStack_.size() >= RuntimeLimits::MAX_RECURSION_DEPTH) {
        hasError_ = true;
        errorMessage_ = "模块导入深度超过限制 (" + std::to_string(RuntimeLimits::MAX_RECURSION_DEPTH) + ")";
        errorLine_ = node.line;
        return;
    }

    // 4. 检查模块加载器
    if (!moduleLoader_) {
        hasError_ = true;
        errorMessage_ = "VM 编译需要模块加载器（moduleLoader 未设置）";
        errorLine_ = node.line;
        return;
    }

    // 5. 加载源码
    std::string source = moduleLoader_(path);
    if (source.empty()) {
        hasError_ = true;
        errorMessage_ = "无法加载模块: " + path + "（文件不存在或为空）";
        errorLine_ = node.line;
        return;
    }

    // 6. 解析模块 AST
    Lexer lexer;
    auto tokens = lexer.scan(source);
    // BUG-FIX: 检查词法错误（原实现吞掉 Lexer 错误）
    if (lexer.getDiagnostics().hasErrors()) {
        const auto& diags = lexer.getDiagnostics().all();
        std::string msg = "模块 '" + path + "' 词法错误";
        if (!diags.empty()) {
            msg += ": " + diags.front().message;
        }
        hasError_ = true;
        errorMessage_ = msg;
        errorLine_ = node.line;
        return;
    }
    Parser parser;
    auto parsed = parser.parse(tokens);
    // BUG-FIX: 检查语法错误（原实现仅检查 AST 是否为空）
    if (parser.hasErrors()) {
        const auto& diags = parser.getDiagnostics().all();
        std::string msg = "模块 '" + path + "' 语法错误";
        if (!diags.empty()) {
            msg += ": " + diags.front().message;
        }
        hasError_ = true;
        errorMessage_ = msg;
        errorLine_ = node.line;
        return;
    }
    if (!parsed) {
        hasError_ = true;
        errorMessage_ = "模块解析失败: " + path;
        errorLine_ = node.line;
        return;
    }
    std::unique_ptr<Block> moduleAst = std::move(parsed);

    // 6.5 BUG-AUDIT-MOD-2 fix: 模块隔离——重命名非导出顶层名为 `__mod_<hash>__<name>`
    // 在预扫描前重写 AST，确保重命名后的名字进入全局槽位分配与 export 集合
    ModuleTopLevelRenamer::rename(*moduleAst, path);

    // 7. 标记为正在加载（循环检测 + 深度保护）
    moduleLoadingSet_.insert(path);
    moduleLoadingStack_.push_back(path);

    // 8. 预扫描模块顶层声明，分配全局槽位
    preScanTopLevelDecls(*moduleAst);
    // 注：preScanTopLevelDecls 已处理 ExportStmt 包装的声明（BUG-FIX 后对齐 Compiler）

    // 8.5 BUG-AUDIT-MOD-1 fix: 收集模块导出名称（对齐 Compiler::visitImportStmt 第 7.5 步）
    {
        std::unordered_set<std::string> exports;
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
        moduleExports_[path] = std::move(exports);
    }

    // 9. 内联 visitNode 模块语句（递归处理模块自身的 import）
    for (auto& stmt : moduleAst->statements) {
        if (stmt) {
            visitNode(stmt.get());
            // BUG-FIX: 嵌套 import 失败后立即中止，避免基于不完整状态继续生成坏 IR
            if (hasError_) {
                moduleLoadingSet_.erase(path);
                moduleLoadingStack_.pop_back(); // BUG-AUDIT-MOD-3
                return;
            }
            // BUG-IR-POP-1 fix: 表达式语句的返回值需 POP（对齐 build() L137-139 中的逻辑）
            if (needsPopForExprStmt(stmt->nodeType)) {
                emitIR(IROp::POP, {}, stmt->line);
            }
        }
    }

    // 10. 保留模块 AST，从加载集移除
    moduleAsts_.push_back(std::move(moduleAst));
    moduleLoadingSet_.erase(path);
    moduleLoadingStack_.pop_back(); // BUG-AUDIT-MOD-3
    linkedModuleSet_.insert(path);

    // 11. BUG-AUDIT-MOD-1 fix: 具名导入验证改为检查 export 集合（对齐 Interpreter）
    if (!node.importAll && !node.names.empty()) {
        auto expIt = moduleExports_.find(path);
        if (expIt != moduleExports_.end()) {
            for (const auto& name : node.names) {
                if (expIt->second.find(name) == expIt->second.end()) {
                    hasError_ = true;
                    errorMessage_ = "模块 " + path + " 中未导出名称: " + name;
                    errorLine_ = node.line;
                    return;
                }
            }
        }
    }
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
    uint32_t typeIdx = ir_->addConstant(Value(typeAnnotation));
    emitIR(IROp::TYPE_CHECK, {val, IROperand::constant(typeIdx)}, line);
}

AstIRBuilder::VarInfo AstIRBuilder::resolveVar(const std::string& name) {
    // 1. 优先查 varMap_（当前作用域已声明的变量）
    auto it = varMap_.find(name);
    if (it != varMap_.end())
        return it->second;

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
    visitNode(node);
    if (needsPopForExprStmt(node->nodeType)) {
        emitIR(IROp::POP, {}, node->line);
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
        visitFunDecl(static_cast<FunDecl*>(node));
        return IROperand::vreg(0);
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
    default:
        // 落空会导致 dest vreg 已分配但无指令 emit，后续 lowering 栈深度映射缺失，
        // 静默产生坏代码。用 assert 兜底，Release 构建中 assert 被剥离时返回空 vreg
        // 至少不会 emit 错误指令。
        // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
        // BUG-IR-VISITNODE-ERR fix: 同时设置 hasError_ 阻止 build() 继续生成坏 IR。
        Logger::Error(
            "AstIRBuilder::visitNode: 未支持的 AST 节点类型 " + std::to_string(static_cast<int>(node->nodeType)), "IR");
        hasError_ = true;
        errorMessage_ = "AstIRBuilder: 未支持的 AST 节点类型 " + std::to_string(static_cast<int>(node->nodeType));
        errorLine_ = node->line;
        assert(false && "AstIRBuilder::visitNode: 未支持的 AST 节点类型");
        return IROperand::vreg(0);
    }
}

IROperand AstIRBuilder::visitBinaryOp(BinaryOp* node) {
    // AND/OR 短路求值：用临时 local slot 汇合两条路径的结果。
    // 旧实现短路路径返回未定义的 right vreg（right 从未被 visitNode 求值），
    // 导致消费者读取脏 vreg。现在两条路径都 STORE_LOCAL 到 tempSlot，
    // 汇合后 LOAD_LOCAL 到 dest，保证 dest 在所有路径都有定义。
    if (node->opType == BinOpType::BIN_AND) {
        IROperand left = visitNode(node->left.get());
        uint32_t shortCircuitLabel = ir_->allocLabel();
        uint32_t endLabel = ir_->allocLabel();
        uint32_t tempSlot = nextLocalSlot_++; // 临时 slot 存放结果
        // 左值为假则短路（结果 = left）
        // L1 fix: JUMP_IF_FALSE 在 StackVM lowering 中用 OP_JUMP_IF_FALSE（peek 不 pop），
        // STORE_LOCAL 用 OP_SET_LOCAL（peek 不 pop）。两条路径都会残留 left/right 在栈上，
        // 导致 endLabel 汇合点栈深度不一致（非短路路径 [left,right] vs 短路路径 [left]）。
        // 修复：每个 STORE_LOCAL 后显式 emit POP 消费残留值，保证两路径在 endLabel 栈深度均为 0。
        emitIR(IROp::JUMP_IF_FALSE, {left, IROperand::label(shortCircuitLabel)}, node->line);
        // 非短路路径：left 在栈顶，存入 tempSlot，POP 消费残留
        emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), left}, node->line);
        emitIR(IROp::POP, {}, node->line);
        IROperand right = visitNode(node->right.get());
        emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), right}, node->line);
        emitIR(IROp::POP, {}, node->line);
        emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);
        // 短路路径：left 在栈顶（JUMP_IF_FALSE peek），存入 tempSlot，POP 消费残留
        emitIR(IROp::LABEL, {IROperand::label(shortCircuitLabel)}, node->line);
        emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), left}, node->line);
        emitIR(IROp::POP, {}, node->line);
        // 汇合：加载结果到 dest（两条路径栈均已空，LOAD_LOCAL push dest）
        emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
        IROperand dest = ir_->allocVReg();
        emitIR(IROp::LOAD_LOCAL, {dest, IROperand::local(tempSlot)}, node->line);
        return dest;
    }
    if (node->opType == BinOpType::BIN_OR) {
        IROperand left = visitNode(node->left.get());
        uint32_t evalRightLabel = ir_->allocLabel();
        uint32_t endLabel = ir_->allocLabel();
        uint32_t tempSlot = nextLocalSlot_++; // 临时 slot 存放结果
        // 左值为假则去求值右操作数
        // L1 fix: 同 BIN_AND，STORE_LOCAL 残留值需 POP 消费，保证汇合点栈深度一致。
        emitIR(IROp::JUMP_IF_FALSE, {left, IROperand::label(evalRightLabel)}, node->line);
        // 左值为真，短路（结果 = left）：left 在栈顶，存入 tempSlot，POP 消费残留
        emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), left}, node->line);
        emitIR(IROp::POP, {}, node->line);
        emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);
        // 非短路路径：left 在栈顶（JUMP_IF_FALSE peek），存入 tempSlot，POP 消费残留
        emitIR(IROp::LABEL, {IROperand::label(evalRightLabel)}, node->line);
        emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), left}, node->line);
        emitIR(IROp::POP, {}, node->line);
        IROperand right = visitNode(node->right.get());
        emitIR(IROp::STORE_LOCAL, {IROperand::local(tempSlot), right}, node->line);
        emitIR(IROp::POP, {}, node->line);
        // 汇合：加载结果到 dest
        emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
        IROperand dest = ir_->allocVReg();
        emitIR(IROp::LOAD_LOCAL, {dest, IROperand::local(tempSlot)}, node->line);
        return dest;
    }
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
        hasError_ = true;
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
    emitStoreVar(node->name, val, node->line);
    if (isGlobalStore) {
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
        // 函数内：注册为 LOCAL（限制5：记录到当前 BlockScope）
        uint32_t slot = nextLocalSlot_++;
        // BUG-IDE-12 fix: 记录 slot→name 映射（跨作用域累积，不随块退出清除）
        if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
            localSlotNames_.resize(slot + 1);
        }
        localSlotNames_[slot] = node->name;
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
        emitIR(IROp::POP, {}, node->line);
        // 记录到当前 BlockScope 的 localSlots
        if (!blockScopes_.empty()) {
            blockScopes_.back().localSlots.push_back(slot);
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
    emitIR(IROp::POP, {}, node->line);
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
    emitIR(IROp::POP, {}, node->line);
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
    emitIR(IROp::POP, {}, node->line); // 循环体路径：POP 消费条件值
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
    emitIR(IROp::POP, {}, node->line); // 循环退出路径：POP 消费条件值
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
        visitNode(node->initializer.get());
        // 表达式初始化器（如 i = 0）的返回值未被消费，需 POP。
        // VarDecl 初始化器已自行平衡栈（STORE_LOCAL+POP 或 DEFINE_GLOBAL pop），不需 POP。
        if (needsPopForExprStmt(node->initializer->nodeType)) {
            emitIR(IROp::POP, {}, node->line);
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
    if (node->condition) {
        IROperand cond = visitNode(node->condition.get());
        // L1 fix: JUMP_IF_FALSE peek 不 pop，条件值残留在栈上。
        // 同 visitWhileStmt 的双标签模式：exitLabel（条件假路径，需 POP）和
        // endLabel（break 路径，栈已空，无需 POP）。
        emitIR(IROp::JUMP_IF_FALSE, {cond, IROperand::label(exitLabel)}, node->line);
        emitIR(IROp::POP, {}, node->line); // 循环体路径：POP 消费条件值
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
        visitNode(node->update.get());
        // update 表达式（如 i = i + 1）的返回值未被消费，需 POP。
        // 与 Compiler.cpp visitForStmt 对齐：update 后 emit OP_POP。
        if (needsPopForExprStmt(node->update->nodeType)) {
            emitIR(IROp::POP, {}, node->line);
        }
    }
    emitIR(IROp::JUMP, {IROperand::label(startLabel)}, node->line);
    if (node->condition) {
        // 条件假路径：条件值在栈上（JUMP_IF_FALSE peek），POP 消费
        emitIR(IROp::LABEL, {IROperand::label(exitLabel)}, node->line);
        emitIR(IROp::POP, {}, node->line); // 循环退出路径：POP 消费条件值
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

void AstIRBuilder::visitFunDecl(FunDecl* node) {
    // 限制1：完整闭包 upvalue 捕获实现
    // Bug 5 fix: 标记当前块作用域含嵌套函数（闭包）。
    // 仅 inFunction_==true 时（即嵌套函数声明，非顶层函数）才标记，
    // 使 leaveBlockScope 不回收本块局部槽位——闭包可能通过 upvalue 捕获了这些 slot。
    if (inFunction_ && !blockScopes_.empty()) {
        blockScopes_.back().hasNestedFunction = true;
    }
    // 1. 保存父函数编译状态
    std::unique_ptr<IRFunction> savedIr = std::move(ir_);
    IRBasicBlock* savedBlock = currentBlock_;
    auto savedVarMap = std::move(varMap_);
    auto savedVarTypes = std::move(varTypes_); // 2026-06-29: 类型注解快照
    bool savedInFunction = inFunction_;
    uint32_t savedLocalSlot = nextLocalSlot_;
    // BUG-IDE-12 fix: 保存父函数的 slot→name 映射，子函数独立维护
    auto savedLocalSlotNames = std::move(localSlotNames_);
    auto savedLoopStack = std::move(loopStack_);
    auto savedBlockScopes = std::move(blockScopes_);
    int savedBlockDepth = blockDepth_;
    // BUG-EXC-2 fix: 保存 tryDepth_，子函数内 try 嵌套不泄漏到父函数
    int savedTryDepth = tryDepth_;
    // R7 fix: 保存 compilingMethod_/compilingClassName_。原实现遗漏这两个字段，
    // 导致类方法内声明的嵌套函数（非方法）被误当作方法处理：预留 slot 0 给 this、
    // arity/requiredArity += 1，调用时实参数量错位且 this 槽为垃圾值。
    bool savedCompilingMethod = compilingMethod_;
    std::string savedCompilingClassName = compilingClassName_;
    // BUG-TYPE-1 fix (P1): 保存当前函数返回类型注解，供 visitReturnStmt 发射 TYPE_CHECK IR
    std::string savedCurrentFunctionReturnType = std::move(currentFunctionReturnType_);

    // 保存外层变量/upvalue/内嵌函数信息（用于子函数捕获）
    auto savedOuterLocalSlots = std::move(outerLocalSlots_);
    auto savedOuterUpvalueNames = std::move(outerUpvalueNames_);
    auto savedOuterFunctions = std::move(outerFunctions_);
    auto savedCurrentUpvalues = std::move(currentUpvalues_);
    auto savedCurrentUpvalueNames = std::move(currentUpvalueNames_);
    auto savedInnerFunctions = std::move(innerFunctions_);
    auto savedInnerFunctionSlots = std::move(innerFunctionSlots_);

    // 2. 新建子 IRFunction
    // 异常安全：645-727 行的子函数编译可能抛出（bad_alloc/IR 构建错误），
    // 若不恢复 15 个 saved* 成员，父函数状态将停留在 moved-from 状态。
    // 对齐 Compiler.cpp 的 CompileContextGuard RAII 模式：try/catch 保护 + 重抛前恢复。
    try {
        ir_ = std::make_unique<IRFunction>();
        ir_->name = node->name;
        ir_->arity = static_cast<int>(node->params.size());
        ir_->requiredArity = node->requiredParamCount;
        inFunction_ = true;
        nextLocalSlot_ = 0;
        varMap_.clear();
        varTypes_.clear();       // 2026-06-29: 清空子函数类型注解
        localSlotNames_.clear(); // BUG-IDE-12 fix: 清空 slot→name 映射
        loopStack_.clear();
        blockScopes_.clear();
        blockDepth_ = 0;
        tryDepth_ = 0; // BUG-EXC-2 fix: 子函数内 try 嵌套从 0 开始
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
        for (const auto& kv : savedVarMap) {
            if (kv.second.kind == VarInfo::Kind::LOCAL) {
                outerLocalSlots_[kv.first] = static_cast<int>(kv.second.index);
            } else if (kv.second.kind == VarInfo::Kind::UPVALUE) {
                outerUpvalueNames_[kv.first] = static_cast<int>(kv.second.index);
            }
        }
        outerFunctions_.clear();

        // 4. 参数注册为 LOCAL slot 0..n-1
        for (size_t i = 0; i < node->params.size(); ++i) {
            uint32_t slot = nextLocalSlot_++;
            varMap_[node->params[i]] = {VarInfo::Kind::LOCAL, slot};
            // BUG-IDE-12 fix: 记录参数 slot→name
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = node->params[i];
        }

        // CRITICAL-1 fix: 前向自由变量分析。在编译函数体前，先收集所有自由变量，
        // 为每个能在外层捕获的变量预建 upvalue。这确保中间函数即使不直接引用某变量，
        // 也会捕获它供更内层函数透传（对齐 Interpreter 的 computeFreeVariables 传播逻辑）。
        // 仅对有外层作用域的嵌套函数执行（顶层函数的自由变量都是全局，无需 upvalue）。
        if (inFunction_ || blockDepth_ > 0 || !outerLocalSlots_.empty() || !outerUpvalueNames_.empty()) {
            auto freeVars = computeFreeVars(*node);
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
        //    成功则 addConstant 记录索引；失败则记录 0xFFFF（表示复杂表达式，不支持）
        for (auto& dv : node->defaultValues) {
            if (dv) {
                Value constVal;
                if (extractConstant(dv.get(), constVal)) {
                    uint32_t constIdx = ir_->addConstant(constVal);
                    ir_->defaultConstIndices.push_back(static_cast<uint16_t>(constIdx));
                } else {
                    ir_->defaultConstIndices.push_back(0xFFFF);
                }
            }
        }

        // 6. 创建初始基本块
        uint32_t entryLabel = ir_->allocLabel();
        currentBlock_ = &ir_->addBlock(entryLabel);
        currentBlock_->instructions.emplace_back(IROp::LABEL, std::vector<IROperand>{IROperand::label(entryLabel)},
                                                 node->line);

        // 7. 编译函数体
        // BUG-TYPE-1 fix (P1): 设置当前函数返回类型注解，供 visitReturnStmt 发射 TYPE_CHECK
        currentFunctionReturnType_ = node->returnType;
        if (node->body)
            visitNode(node->body.get());

        // 8. 末尾隐式 RETURN_NULL
        emitIR(IROp::RETURN_NULL, {}, node->line);

        // 9. 设置 localCount 和 upvalues
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

        // 10. 将子 IRFunction 添加到 module_（不再丢弃 childIr）
        module_->addFunction(std::move(ir_));
    } catch (...) {
        // 异常路径：恢复父函数的 15 个成员状态，避免 moved-from 状态被上层复用
        ir_ = std::move(savedIr);
        currentBlock_ = savedBlock;
        varMap_ = std::move(savedVarMap);
        varTypes_ = std::move(savedVarTypes); // 2026-06-29
        inFunction_ = savedInFunction;
        nextLocalSlot_ = savedLocalSlot;
        localSlotNames_ = std::move(savedLocalSlotNames); // BUG-IDE-12 fix
        loopStack_ = std::move(savedLoopStack);
        blockScopes_ = std::move(savedBlockScopes);
        blockDepth_ = savedBlockDepth;
        tryDepth_ = savedTryDepth; // BUG-EXC-2 fix
        // R7 fix: 恢复 compilingMethod_/compilingClassName_
        compilingMethod_ = savedCompilingMethod;
        compilingClassName_ = std::move(savedCompilingClassName);
        currentFunctionReturnType_ = std::move(savedCurrentFunctionReturnType); // BUG-TYPE-1 fix
        outerLocalSlots_ = std::move(savedOuterLocalSlots);
        outerUpvalueNames_ = std::move(savedOuterUpvalueNames);
        outerFunctions_ = std::move(savedOuterFunctions);
        currentUpvalues_ = std::move(savedCurrentUpvalues);
        currentUpvalueNames_ = std::move(savedCurrentUpvalueNames);
        innerFunctions_ = std::move(savedInnerFunctions);
        innerFunctionSlots_ = std::move(savedInnerFunctionSlots);
        throw;
    }

    // 11. 恢复父函数状态
    ir_ = std::move(savedIr);
    currentBlock_ = savedBlock;
    varMap_ = std::move(savedVarMap);
    varTypes_ = std::move(savedVarTypes); // 2026-06-29
    inFunction_ = savedInFunction;
    nextLocalSlot_ = savedLocalSlot;
    localSlotNames_ = std::move(savedLocalSlotNames); // BUG-IDE-12 fix
    loopStack_ = std::move(savedLoopStack);
    blockScopes_ = std::move(savedBlockScopes);
    blockDepth_ = savedBlockDepth;
    tryDepth_ = savedTryDepth; // BUG-EXC-2 fix
    // R7 fix: 恢复 compilingMethod_/compilingClassName_
    compilingMethod_ = savedCompilingMethod;
    compilingClassName_ = std::move(savedCompilingClassName);
    currentFunctionReturnType_ = std::move(savedCurrentFunctionReturnType); // BUG-TYPE-1 fix
    outerLocalSlots_ = std::move(savedOuterLocalSlots);
    outerUpvalueNames_ = std::move(savedOuterUpvalueNames);
    outerFunctions_ = std::move(savedOuterFunctions);
    currentUpvalues_ = std::move(savedCurrentUpvalues);
    currentUpvalueNames_ = std::move(savedCurrentUpvalueNames);
    innerFunctions_ = std::move(savedInnerFunctions);
    innerFunctionSlots_ = std::move(savedInnerFunctionSlots);

    // 12. 在父 IR 中 emit MAKE_CLOSURE（含 upvalue 描述符列表）
    //     编码：operands [dest, name_idx, uv_count, isLocal1, idx1, isLocal2, idx2, ...]
    std::string fnName = node->name;
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
    emitIR(IROp::MAKE_CLOSURE, ops, node->line);

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
        innerFunctions_.insert(fnName);
        innerFunctionSlots_[fnName] = static_cast<int>(slot);
        emitIR(IROp::STORE_LOCAL, {IROperand::local(slot), dest}, node->line);
        // STORE_LOCAL → OP_SET_LOCAL（peek 不 pop），MAKE_CLOSURE 的值残留在栈上。
        // 需 POP 消费，与顶层路径的 POP 对齐，防止栈泄漏。
        emitIR(IROp::POP, {}, node->line);
    } else {
        // 顶层：注册为全局（有槽位则 GLOBAL_SLOT，否则 GLOBAL_NAME）
        // 顶层函数无外层作用域，MAKE_CLOSURE 创建的闭包 upvalue 列表为空，
        // visitFunCall 命中 GLOBAL_* 时走 CALL 命名调用，由 functionChunks_/
        // functionClosures_ 查找，无需把闭包值 STORE 到全局槽位。
        int slot = lookupGlobalSlot(fnName);
        if (slot >= 0) {
            varMap_[fnName] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(slot)};
        } else {
            uint32_t idx = ir_->addGlobal(fnName);
            varMap_[fnName] = {VarInfo::Kind::GLOBAL_NAME, idx};
        }
        // MAKE_CLOSURE 写到 dest vreg 后未消费——pop 释放避免栈泄漏
        // （栈式 VM MAKE_CLOSURE 会 push 到栈，RegisterVM 写入 dst reg）。
        emitIR(IROp::POP, {dest}, node->line);
    }
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
    // 必须走 CALL_EXPR 通过闭包值调用——MAKE_CLOSURE 创建的闭包存储在局部/upvalue 槽中，
    // 携带 upvalue 绑定。若走 CALL 命名调用，RegisterVM 的 functionClosures_ 不含
    // 嵌套闭包（仅栈式 VM MAKE_CLOSURE 才注册到 functionClosures_），且即便注册了
    // 也无法处理同名嵌套闭包按帧隔离的情况（对齐 Compiler.cpp H5 fix 的实现策略）。
    auto varIt = varMap_.find(node->name);
    if (varIt != varMap_.end() &&
        (varIt->second.kind == VarInfo::Kind::LOCAL || varIt->second.kind == VarInfo::Kind::UPVALUE)) {
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
    if (node->value) {
        IROperand val = visitNode(node->value.get());
        // BUG-TYPE-1 fix (P1): 函数有返回类型注解时，在 RETURN 前发射 TYPE_CHECK IR。
        // 对齐 Interpreter::visitReturnStmt 的 checkType + Compiler::visitReturnStmt 的 OP_TYPE_CHECK。
        if (!currentFunctionReturnType_.empty()) {
            emitTypeCheckIR(val, currentFunctionReturnType_, node->line);
        }
        emitIR(IROp::RETURN, {val}, node->line);
    } else {
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
        for (auto& s : node->statements) {
            if (!s)
                continue;
            visitNode(s.get());
            // 表达式语句的返回值未被消费，需 emit POP。与 build() 对齐使用
            // needsPopForExprStmt 覆盖全部 16 种表达式语句节点类型。
            if (needsPopForExprStmt(s->nodeType)) {
                emitIR(IROp::POP, {}, s->line);
            }
        }
        leaveBlockScope();
    } else {
        // 顶层块：实现全局变量遮蔽保护（对齐 Compiler.cpp:1362-1455）
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
            visitNode(s.get());
            // 表达式语句的返回值未被消费，需 emit POP。与 build() 对齐使用
            // needsPopForExprStmt 覆盖全部 16 种表达式语句节点类型。
            if (needsPopForExprStmt(s->nodeType)) {
                emitIR(IROp::POP, {}, s->line);
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
    // 对齐 Compiler.cpp 的 M1 fix：检测 node.object 是否是 IndexAccess(VarRef) 或 MemberAccess(VarRef)
    IndexAccess* outerIdx = (node->object && node->object->nodeType == NodeType::NODE_INDEX_ACCESS)
                                ? static_cast<IndexAccess*>(node->object.get())
                                : nullptr;
    MemberAccess* outerMem = (node->object && node->object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                                 ? static_cast<MemberAccess*>(node->object.get())
                                 : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());

    if (baseVar) {
        // 上一级 INDEX_SET 已将变异后的内层容器存入 lastMutatedReceiver_。
        // 此处重新加载基变量（全局槽/局部槽均未被修改，仍是原始值），
        // 然后读取 lastMutatedReceiver_，再发射外层 SET 将变异传播到基变量，
        // 最后用 WRITEBACK（整体替换语义）将变异后的基变量写回变量槽。
        // 栈序约束（栈式 VM）：
        //   - 外层 INDEX_SET 需 [obj, idx, val] → 先 base2，再 outerIdxVal，再 mut
        //   - 外层 MEMBER_SET 需 [obj, val]      → 先 base2，再 mut
        IROperand base2 = emitLoadVar(baseVar->name, node->line);
        IROperand mut = ir_->allocVReg();
        if (outerIdx) {
            // 外层是索引：base[outerIdx] = mut
            IROperand outerIdxVal = visitNode(outerIdx->index.get());
            emitIR(IROp::LOAD_MUTATED, {mut}, node->line);
            emitIR(IROp::INDEX_SET, {base2, outerIdxVal, mut}, node->line);
        } else {
            // 外层是成员：base.field = mut
            uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
            emitIR(IROp::LOAD_MUTATED, {mut}, node->line);
            emitIR(IROp::MEMBER_SET, {base2, IROperand::field(outerFieldIdx), mut}, node->line);
        }
        // 最终 WRITEBACK（整体替换语义，将变异后的 base 写回变量槽）
        VarInfo info = resolveVar(baseVar->name);
        if (info.kind == VarInfo::Kind::LOCAL) {
            if (outerIdx) {
                emitIR(IROp::WRITEBACK_INDEX_LOCAL, {IROperand::local(info.index)}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::WRITEBACK_MEMBER_LOCAL, {IROperand::local(info.index), IROperand::field(outerFieldIdx)},
                       node->line);
            }
        } else if (info.kind == VarInfo::Kind::GLOBAL_SLOT) {
            if (outerIdx) {
                emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::imm(info.index)}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::imm(info.index), IROperand::field(outerFieldIdx)},
                       node->line);
            }
        } else if (info.kind == VarInfo::Kind::GLOBAL_NAME) {
            if (outerIdx) {
                emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::global(info.index)}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::global(info.index), IROperand::field(outerFieldIdx)},
                       node->line);
            }
        } else if (info.kind == VarInfo::Kind::UPVALUE) {
            if (outerIdx) {
                emitIR(IROp::WRITEBACK_INDEX_UPVALUE, {IROperand::upvalue(info.index)}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::WRITEBACK_MEMBER_UPVALUE,
                       {IROperand::upvalue(info.index), IROperand::field(outerFieldIdx)}, node->line);
            }
        }
        return;
    }

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
    // 对齐 Compiler.cpp 的 M1 fix：检测 node.object 是否是 IndexAccess(VarRef) 或 MemberAccess(VarRef)
    IndexAccess* outerIdx = (node->object && node->object->nodeType == NodeType::NODE_INDEX_ACCESS)
                                ? static_cast<IndexAccess*>(node->object.get())
                                : nullptr;
    MemberAccess* outerMem = (node->object && node->object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                                 ? static_cast<MemberAccess*>(node->object.get())
                                 : nullptr;
    VarRef* baseVar = nullptr;
    if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerIdx->object.get());
    else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
        baseVar = static_cast<VarRef*>(outerMem->object.get());

    if (baseVar) {
        // 上一级 MEMBER_SET 已将变异后的内层容器存入 lastMutatedReceiver_。
        // 此处重新加载基变量，读取 lastMutatedReceiver_，发射外层 SET 传播变异，
        // 最后用 WRITEBACK 写回变量槽。
        // 栈序约束（栈式 VM）：
        //   - 外层 INDEX_SET 需 [obj, idx, val] → 先 base2，再 outerIdxVal，再 mut
        //   - 外层 MEMBER_SET 需 [obj, val]      → 先 base2，再 mut
        IROperand base2 = emitLoadVar(baseVar->name, node->line);
        IROperand mut = ir_->allocVReg();
        if (outerIdx) {
            // 外层是索引：base[outerIdx] = mut
            IROperand outerIdxVal = visitNode(outerIdx->index.get());
            emitIR(IROp::LOAD_MUTATED, {mut}, node->line);
            emitIR(IROp::INDEX_SET, {base2, outerIdxVal, mut}, node->line);
        } else {
            // 外层是成员：base.field = mut
            uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
            emitIR(IROp::LOAD_MUTATED, {mut}, node->line);
            emitIR(IROp::MEMBER_SET, {base2, IROperand::field(outerFieldIdx), mut}, node->line);
        }
        // 最终 WRITEBACK（整体替换语义，将变异后的 base 写回变量槽）
        VarInfo info = resolveVar(baseVar->name);
        if (info.kind == VarInfo::Kind::LOCAL) {
            if (outerIdx) {
                emitIR(IROp::WRITEBACK_INDEX_LOCAL, {IROperand::local(info.index)}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::WRITEBACK_MEMBER_LOCAL, {IROperand::local(info.index), IROperand::field(outerFieldIdx)},
                       node->line);
            }
        } else if (info.kind == VarInfo::Kind::GLOBAL_SLOT) {
            if (outerIdx) {
                emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::imm(info.index)}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::imm(info.index), IROperand::field(outerFieldIdx)},
                       node->line);
            }
        } else if (info.kind == VarInfo::Kind::GLOBAL_NAME) {
            if (outerIdx) {
                emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::global(info.index)}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::global(info.index), IROperand::field(outerFieldIdx)},
                       node->line);
            }
        } else if (info.kind == VarInfo::Kind::UPVALUE) {
            if (outerIdx) {
                emitIR(IROp::WRITEBACK_INDEX_UPVALUE, {IROperand::upvalue(info.index)}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::WRITEBACK_MEMBER_UPVALUE,
                       {IROperand::upvalue(info.index), IROperand::field(outerFieldIdx)}, node->line);
            }
        }
        return;
    }

    // C-P2-2 fix: 3+ 层嵌套或复杂表达式：不支持，报错而非静默丢失修改
    Logger::Error("AstIRBuilder: 不支持 3 层及以上或复杂表达式嵌套成员赋值", "IR");
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
    // CRITICAL-4 fix: 方法调用后写回变异后的接收者。
    // 栈式 VM 的变异内建方法（dispatchArrayBuiltin/dispatchDictBuiltin）在 recvVarIdx/recvSlot
    // 均为 0xFFFF/0xFF 时将变异后对象存入 lastMutatedReceiver_；实例方法（OP_RETURN）的
    // fallback 路径也设置 lastMutatedReceiver_。RegVM 的 callBuiltinMethod/executeReturnImpl
    // 将变异后对象存入 lastMutatedReceiverReg_。此处通过 LOAD_MUTATED 读取，再 STORE 写回。
    // 原 emitStoreVar(obj) 在栈式 VM 下会栈下溢（obj vreg 栈位置在 METHOD_CALL 后已被 pop），
    // 在 RegVM 下虽有效但与栈式 VM 行为不一致。统一改用 LOAD_MUTATED + STORE 模式。
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
            emitIR(IROp::POP, {}, node->line);
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
        emitIR(IROp::POP, {}, node->line);
    } else {
        // CRITICAL-4 extension: 嵌套接收者（IndexAccess(VarRef) 或 MemberAccess(VarRef)）
        // 方法调用后通过 LOAD_MUTATED + INDEX_SET/MEMBER_SET + WRITEBACK 链写回变异后接收者。
        // 对齐 fix-3b 的嵌套左值赋值模式：方法调用产生变异内层容器存入 lastMutatedReceiver_，
        // 随后重新加载基变量，通过 INDEX_SET/MEMBER_SET 将变异内层写回外层容器，
        // 最终 WRITEBACK 整体替换基变量。
        IndexAccess* outerIdx = (node->object && node->object->nodeType == NodeType::NODE_INDEX_ACCESS)
                                    ? static_cast<IndexAccess*>(node->object.get())
                                    : nullptr;
        MemberAccess* outerMem = (node->object && node->object->nodeType == NodeType::NODE_MEMBER_ACCESS)
                                     ? static_cast<MemberAccess*>(node->object.get())
                                     : nullptr;
        VarRef* baseVar = nullptr;
        if (outerIdx && outerIdx->object && outerIdx->object->nodeType == NodeType::NODE_VAR_REF)
            baseVar = static_cast<VarRef*>(outerIdx->object.get());
        else if (outerMem && outerMem->object && outerMem->object->nodeType == NodeType::NODE_VAR_REF)
            baseVar = static_cast<VarRef*>(outerMem->object.get());

        if (baseVar) {
            IROperand base2 = emitLoadVar(baseVar->name, node->line);
            IROperand mut = ir_->allocVReg();
            if (outerIdx) {
                IROperand outerIdxVal = visitNode(outerIdx->index.get());
                emitIR(IROp::LOAD_MUTATED, {mut}, node->line);
                emitIR(IROp::INDEX_SET, {base2, outerIdxVal, mut}, node->line);
            } else {
                uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                emitIR(IROp::LOAD_MUTATED, {mut}, node->line);
                emitIR(IROp::MEMBER_SET, {base2, IROperand::field(outerFieldIdx), mut}, node->line);
            }
            VarInfo info = resolveVar(baseVar->name);
            if (info.kind == VarInfo::Kind::LOCAL) {
                if (outerIdx) {
                    emitIR(IROp::WRITEBACK_INDEX_LOCAL, {IROperand::local(info.index)}, node->line);
                } else {
                    uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                    emitIR(IROp::WRITEBACK_MEMBER_LOCAL,
                           {IROperand::local(info.index), IROperand::field(outerFieldIdx)}, node->line);
                }
            } else if (info.kind == VarInfo::Kind::GLOBAL_SLOT) {
                if (outerIdx) {
                    emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::imm(info.index)}, node->line);
                } else {
                    uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                    emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::imm(info.index), IROperand::field(outerFieldIdx)},
                           node->line);
                }
            } else if (info.kind == VarInfo::Kind::GLOBAL_NAME) {
                if (outerIdx) {
                    emitIR(IROp::WRITEBACK_INDEX_VAR, {IROperand::global(info.index)}, node->line);
                } else {
                    uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                    emitIR(IROp::WRITEBACK_MEMBER_VAR, {IROperand::global(info.index), IROperand::field(outerFieldIdx)},
                           node->line);
                }
            } else if (info.kind == VarInfo::Kind::UPVALUE) {
                if (outerIdx) {
                    emitIR(IROp::WRITEBACK_INDEX_UPVALUE, {IROperand::upvalue(info.index)}, node->line);
                } else {
                    uint32_t outerFieldIdx = ir_->addGlobal(outerMem->fieldName);
                    emitIR(IROp::WRITEBACK_MEMBER_UPVALUE,
                           {IROperand::upvalue(info.index), IROperand::field(outerFieldIdx)}, node->line);
                }
            }
        }
        // else: 复杂表达式接收者，不写回（方法结果在 dest 中）
    }
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
    // 非字面量表达式保持 UINT32_MAX 标记 = null，对齐 Compiler.cpp:1843-1847 直接路径）
    // BUG-INH-IR-1 fix: 非字面量表达式不再降级为 null，改为在 DEFINE_CLASS 之前 emit 求值 IR 序列，
    // 将求值结果存入临时局部变量，DEFINE_CLASS 携带局部变量槽位，后端 lowering 时 emit OP_GET_LOCAL + OP_INIT_FIELD。
    // 这对齐 Compiler.cpp 直接路径的 compileNode(initializer) 栈传递模式，实现三后端一致性。
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
                    emitIR(IROp::POP, {}, vd->line); // 清栈（STORE_LOCAL peek 不 pop）
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
            compilingClassName_ = node->name; // P1 fix: 供 super 调用查找父类
            try {
                visitFunDecl(fd);
            } catch (...) {
                // 异常路径恢复 AST 节点状态，防止编译失败后 AST 被永久修改
                // （调用方可能重用同一 AST 重试编译，如 IDE/REPL 场景）。
                fd->name = origName;
                compilingMethod_ = false;
                compilingClassName_.clear();
                throw;
            }
            compilingMethod_ = false;
            compilingClassName_.clear();
            fd->name = origName;
        }
    }
}

// AUDIT-P1.1 fix: break/continue finally 续跳 IR 发射。
// 若 break/continue 在 try-finally 内，发射 PUSH_JUMP_TARGET + JUMP 续跳 IR。
// IR 路径用 label 而非 ip，label 在 visitTryStmt 进入时就分配，无需 deferred patch。
bool AstIRBuilder::emitFinallyJumpIR(int line, uint32_t realTargetLabel) {
    // 收集所有 enclosing try-finally 块（从内到外）
    std::vector<size_t> finallyIndices;
    for (size_t i = tryFinallyStack_.size(); i > 0; --i) {
        if (tryFinallyStack_[i - 1].hasFinally) {
            finallyIndices.push_back(i - 1);
        }
    }
    if (finallyIndices.empty())
        return false;

    // 从最外层到最内层，依次发射 PUSH_JUMP_TARGET
    for (size_t i = finallyIndices.size(); i > 0; --i) {
        size_t ctxIdx = finallyIndices[i - 1];
        uint32_t targetLabel;
        if (i == 1) {
            // 最内层：push 真实跳转目标（breakTarget/continueTarget）
            targetLabel = realTargetLabel;
        } else {
            // 外层：push 下一个（更内层）finally 入口 label
            size_t nextCtxIdx = finallyIndices[i - 2];
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
        // R7 fix: 循环外 break 静默无操作会导致语义错误未报告。
        // 对齐 Compiler.cpp visitBreakStmt 的 error() 行为，避免两路径不一致。
        Logger::Error("break 只能在循环体内使用", "IR");
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
        // R7 fix: 循环外 continue 静默无操作会导致语义错误未报告。
        // 对齐 Compiler.cpp visitContinueStmt 的 error() 行为，避免两路径不一致。
        Logger::Error("continue 只能在循环体内使用", "IR");
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
    uint32_t finallyCatchLabel = 0;
    uint32_t finallyEndLabel = 0;
    uint32_t finallyEntryLabel = 0; // AUDIT-P1.1 fix: finally 正常路径入口 label
    bool hasFinally = (node->finallyBlock != nullptr);
    if (hasFinally) {
        finallyCatchLabel = ir_->allocLabel();
        finallyEndLabel = ir_->allocLabel();
        finallyEntryLabel = ir_->allocLabel(); // AUDIT-P1.1 fix
        emitIR(IROp::TRY_BEGIN, {IROperand::label(finallyCatchLabel)}, node->line);
        ++tryDepth_; // 外层 try 计入深度（break/continue 多发一个 TRY_END）
    }
    // AUDIT-P1.1 fix: push try-finally 编译期上下文（label 在此分配，break/continue 可直接引用）
    tryFinallyStack_.push_back({hasFinally, finallyEntryLabel, {}, {}});
    // BUG-AUDIT-FINALLY-1: try-finally（无 catch）路径。
    // catchVarName 为空时跳过内层 TRY_BEGIN/TRY_END/catchLabel，
    // 外层 TRY_BEGIN(finallyCatchLabel) 捕获异常 → 执行 finally → rethrow。
    uint32_t catchLabel = 0;
    uint32_t endLabel = ir_->allocLabel();
    if (!node->catchVarName.empty()) {
        catchLabel = ir_->allocLabel();
        // try 块开始，记录 catch 跳转目标
        emitIR(IROp::TRY_BEGIN, {IROperand::label(catchLabel)}, node->line);
        // BUG-EXC-2 fix: 跟踪 try 嵌套深度，break/continue 需为差额层级发射 TRY_END
        ++tryDepth_;
        if (node->tryBlock)
            visitNode(node->tryBlock.get());
        --tryDepth_;
        emitIR(IROp::TRY_END, {}, node->line);
        emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);
        // catch 块
        emitIR(IROp::LABEL, {IROperand::label(catchLabel)}, node->line);
    } else {
        // try-finally（无 catch）：只编译 try 块，不发射内层 try-catch。
        if (node->tryBlock)
            visitNode(node->tryBlock.get());
    }
    // P1-4 fix: catch 块起始，将异常值加载到 vreg 并绑定到 catch 变量。
    // RegisterVM: throwException 将异常存入 pendingException_，REG_LOAD_EXCEPTION 读取。
    // 栈式 VM: throwException 将异常推入栈顶，BytecodeIRBackend 的 LOAD_EXCEPTION 为 no-op（值已在栈上）。
    if (!node->catchVarName.empty()) {
        IROperand excVreg = ir_->allocVReg();
        emitIR(IROp::LOAD_EXCEPTION, {excVreg}, node->line);
        // 将 catch 变量声明为局部/global 并存储异常值
        if (inFunction_) {
            // 函数内：分配局部 slot 并绑定
            uint32_t slot = nextLocalSlot_++;
            // BUG-IDE-12 fix: 记录 catch 变量 slot→name 映射
            if (static_cast<size_t>(slot) >= localSlotNames_.size()) {
                localSlotNames_.resize(slot + 1);
            }
            localSlotNames_[slot] = node->catchVarName;
            // AUDIT-BUG-F7 fix: 保存 varMap_ 旧条目，catch 块编译后恢复。
            // 原实现 catch 变量绑定到外层 varMap_ 且不恢复，catch 块后仍可引用，
            // 与 Interpreter/StackVM 语义不一致（后者 catch 变量仅 catch 块内可见）。
            auto savedIt_f7 = varMap_.find(node->catchVarName);
            VarInfo savedInfo_f7;
            bool hadSaved_f7 = (savedIt_f7 != varMap_.end());
            if (hadSaved_f7)
                savedInfo_f7 = savedIt_f7->second;
            varMap_[node->catchVarName] = {VarInfo::Kind::LOCAL, slot};
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
            emitIR(IROp::STORE_LOCAL, {IROperand::local(slot), excVreg}, node->line);
            // L1 fix: STORE_LOCAL → OP_SET_LOCAL（peek 不 pop），异常值残留在栈上。
            // 对齐直接 Compiler.cpp visitTryStmt 的模式：OP_SET_LOCAL 后紧跟 OP_POP 消费异常值。
            // 顶层 GLOBAL 路径用 DEFINE_GLOBAL → OP_DEFINE_VAR（pop），无需额外 POP。
            emitIR(IROp::POP, {}, node->line);
            // Bug 5 fix: 记录到当前 BlockScope，使 leaveBlockScope 能清除 catchVar 的 varMap_ 条目。
            // catch 变量作用域限于 catch 块，块退出后不应再被引用。
            if (!blockScopes_.empty()) {
                blockScopes_.back().localSlots.push_back(slot);
            }
            // AUDIT-BUG-F7 fix: catch 块在此编译（而非延后到统一位置），编译后立即恢复 varMap_。
            if (node->catchBlock)
                visitNode(node->catchBlock.get());
            // AUDIT-P2-CORRECT fix: catch 块退出后立即关闭 catch 变量的 upvalue，
            // 对齐 Compiler.cpp visitTryStmt（行 2141-2144）在 catch 块后立即发射 OP_CLOSE_UPVALUE。
            // 原实现将 catch 变量 slot 记录到外层 BlockScope.localSlots，依赖外层块退出时
            // leaveBlockScope 统一关闭，导致闭包捕获 catch 变量时 upvalue 快照时机晚于直接路径，
            // 三后端语义不一致（StackVM 直接路径立即关闭，IR 路径延迟到外层块退出）。
            // CLOSE_UPVALUE(slot) 关闭所有 slot >= catchVarSlot 的 open upvalue，
            // 此时 catch 块内部 slot 已由 catch 块的 leaveBlockScope 关闭，仅 catch 变量本身未关闭。
            emitIR(IROp::CLOSE_UPVALUE, {IROperand::imm(slot)}, node->line);
            if (hadSaved_f7) {
                varMap_[node->catchVarName] = std::move(savedInfo_f7);
            } else {
                varMap_.erase(node->catchVarName);
            }
            emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
            // BUG-AUDIT-FINALLY-1: finally 块 IR 发射
            if (hasFinally) {
                --tryDepth_;
                emitIR(IROp::TRY_END, {}, node->line);
                // AUDIT-P1.1 fix: 标记 finally 正常路径入口，供 break/continue 续跳
                emitIR(IROp::LABEL, {IROperand::label(finallyEntryLabel)}, node->line);
                visitNode(node->finallyBlock.get());
                // AUDIT-P1.1 fix: finally 末尾追加 FINALLY_END（break/continue 续跳或正常完成）
                emitIR(IROp::FINALLY_END, {}, node->line);
                emitIR(IROp::JUMP, {IROperand::label(finallyEndLabel)}, node->line);
                emitIR(IROp::LABEL, {IROperand::label(finallyCatchLabel)}, node->line);
                IROperand excVregF = ir_->allocVReg();
                emitIR(IROp::LOAD_EXCEPTION, {excVregF}, node->line);
                visitNode(node->finallyBlock.get());
                emitIR(IROp::THROW, {excVregF}, node->line);
                emitIR(IROp::LABEL, {IROperand::label(finallyEndLabel)}, node->line);
            }
            tryFinallyStack_.pop_back(); // AUDIT-P1.1 fix
            return; // AUDIT-BUG-F7: catch 块已编译，提前返回
        } else {
            // 顶层：检查 catchVarName 是否与全局槽位变量同名（对齐 Compiler.cpp:1272-1311）
            int existingSlot = lookupGlobalSlot(node->catchVarName);
            if (existingSlot >= 0) {
                // BUG-IR-SHADOW-SAVE fix: 遮蔽保护——原值保存到临时 name-based 全局变量。
                // 原实现用 vreg(saved) 保存，但 StackVM 后端的 LOAD_EXCEPTION 是 no-op
                // （异常值已在栈上），LOAD_GLOBAL 再 push 会使 DEFINE_GLOBAL pop 错误值
                // （saved 而非 exception）。改用临时全局变量对齐 Compiler.cpp 的
                // __catch_save_<counter>_<name> 模式，值存储在 globals_ 中不受栈变化影响。
                // 1. 加载原值到栈顶（StackVM: push; RegisterVM: 写 vreg）
                IROperand saved = ir_->allocVReg();
                emitIR(IROp::LOAD_GLOBAL, {saved, IROperand::imm(static_cast<uint32_t>(existingSlot))}, node->line);
                // 2. 保存到临时全局变量 __catch_save_<counter>_<catchVarName>
                std::string saveName = "__catch_save_" + std::to_string(catchSaveCounter_++) + "_" + node->catchVarName;
                uint32_t saveIdx = ir_->addGlobal(saveName);
                emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(saveIdx), saved}, node->line);
                // 3. 移除映射 + 定义 catch 变量（exception 在栈顶，DEFINE_GLOBAL pop）
                globalSlotAllocator_.removeMapping(node->catchVarName);
                uint32_t idx = ir_->addGlobal(node->catchVarName);
                varMap_[node->catchVarName] = {VarInfo::Kind::GLOBAL_NAME, idx};
                emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(idx), excVreg}, node->line);
                // BUG-IR-TRY-CATCH-WRAP fix: 用内层 TRY_BEGIN 包装 catch 块，确保 catch 块内
                // throw 时 cleanup IR（恢复遮蔽全局原值）仍执行。对齐 Compiler.cpp
                // visitTryStmt L1794-L1858 的 needsCleanupWrap 模式。
                uint32_t cleanupThrowLabel = ir_->allocLabel();
                emitIR(IROp::TRY_BEGIN, {IROperand::label(cleanupThrowLabel)}, node->line);
                // BUG-AUDIT-EXC-CLEANUP-TRYDEPTH fix: cleanup wrap 的内层 TRY_BEGIN
                // 必须计入 tryDepth_，使 catch 块内的 break/continue 能发射对应 TRY_END，
                // 避免 tryStack_ handler 残留导致后续异常被错误捕获到已失效的 cleanupThrowLabel。
                ++tryDepth_;
                if (node->catchBlock)
                    visitNode(node->catchBlock.get());
                --tryDepth_;
                emitIR(IROp::TRY_END, {}, node->line);
                // 正常路径：恢复映射 + varMap_ + 原值（从临时全局变量重载）
                globalSlotAllocator_.restoreMapping(node->catchVarName, existingSlot);
                varMap_[node->catchVarName] = {VarInfo::Kind::GLOBAL_SLOT, static_cast<uint32_t>(existingSlot)};
                IROperand savedRestore = ir_->allocVReg();
                emitIR(IROp::LOAD_GLOBAL, {savedRestore, IROperand::global(saveIdx)}, node->line);
                emitIR(IROp::STORE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(existingSlot)), savedRestore},
                       node->line);
                emitIR(IROp::DELETE_VAR, {IROperand::global(saveIdx)}, node->line);
                emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);
                // 异常路径：从临时全局变量重载原值 + 恢复 + rethrow
                // StackVM: 异常值在栈顶，LOAD_EXCEPTION 为 no-op，LOAD_GLOBAL push savedRestore2，
                //          STORE_GLOBAL pop savedRestore2，THROW(OP_THROW) pop 栈顶异常值 rethrow。
                // RegisterVM: 异常值在 pendingException_，LOAD_EXCEPTION 加载到 excVreg2，
                //             LOAD_GLOBAL/STORE_GLOBAL 操作寄存器，THROW(REG_THROW) 读取 excVreg2 rethrow。
                emitIR(IROp::LABEL, {IROperand::label(cleanupThrowLabel)}, node->line);
                IROperand excVreg2 = ir_->allocVReg();
                emitIR(IROp::LOAD_EXCEPTION, {excVreg2}, node->line);
                IROperand savedRestore2 = ir_->allocVReg();
                emitIR(IROp::LOAD_GLOBAL, {savedRestore2, IROperand::global(saveIdx)}, node->line);
                emitIR(IROp::STORE_GLOBAL, {IROperand::imm(static_cast<uint32_t>(existingSlot)), savedRestore2},
                       node->line);
                emitIR(IROp::DELETE_VAR, {IROperand::global(saveIdx)}, node->line);
                emitIR(IROp::THROW, {excVreg2}, node->line);
                emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
                // BUG-AUDIT-FINALLY-1: finally 块 IR 发射
                if (hasFinally) {
                    --tryDepth_;
                    emitIR(IROp::TRY_END, {}, node->line);
                    // AUDIT-P1.1 fix: 标记 finally 正常路径入口
                    emitIR(IROp::LABEL, {IROperand::label(finallyEntryLabel)}, node->line);
                    visitNode(node->finallyBlock.get());
                    emitIR(IROp::FINALLY_END, {}, node->line); // AUDIT-P1.1 fix
                    emitIR(IROp::JUMP, {IROperand::label(finallyEndLabel)}, node->line);
                    emitIR(IROp::LABEL, {IROperand::label(finallyCatchLabel)}, node->line);
                    IROperand excVregF = ir_->allocVReg();
                    emitIR(IROp::LOAD_EXCEPTION, {excVregF}, node->line);
                    visitNode(node->finallyBlock.get());
                    emitIR(IROp::THROW, {excVregF}, node->line);
                    emitIR(IROp::LABEL, {IROperand::label(finallyEndLabel)}, node->line);
                }
                tryFinallyStack_.pop_back(); // AUDIT-P1.1 fix
                return; // catch 块已编译，提前返回
            } else {
                // 无遮蔽：直接定义为全局变量
                // BUG-IR-TRY-1 fix: 使用 GLOBAL_NAME kind（→ OP_DEFINE_VAR）而非 IMM_UINT
                // kind（→ OP_DEFINE_GLOBAL slot）。原因：OP_DELETE_VAR 对 slot-based 变量置
                // null（仍可访问），对 name-based 变量从 globals_ 擦除（→ undefined）。
                // 对齐 StackVM Compiler.cpp visitTryStmt L1770-L1772 的 OP_DEFINE_VAR 语义。
                uint32_t nameIdx = ir_->addGlobal(node->catchVarName);
                varMap_[node->catchVarName] = {VarInfo::Kind::GLOBAL_NAME, nameIdx};
                emitIR(IROp::DEFINE_GLOBAL, {IROperand::global(nameIdx), excVreg}, node->line);
                // BUG-IR-TRY-CATCH-WRAP fix: 用内层 TRY_BEGIN 包装 catch 块，确保 catch 块内
                // throw 时 cleanup IR（DELETE_VAR 清理 catch 变量）仍执行。
                uint32_t cleanupThrowLabel = ir_->allocLabel();
                emitIR(IROp::TRY_BEGIN, {IROperand::label(cleanupThrowLabel)}, node->line);
                // BUG-AUDIT-EXC-CLEANUP-TRYDEPTH fix: 同步 ++tryDepth_/--tryDepth_，
                // 使 catch 块内 break/continue 正确发射 TRY_END。
                ++tryDepth_;
                if (node->catchBlock)
                    visitNode(node->catchBlock.get());
                --tryDepth_;
                emitIR(IROp::TRY_END, {}, node->line);
                // 正常路径：移除 varMap_ 映射 + 清理 catch 变量
                varMap_.erase(node->catchVarName);
                emitIR(IROp::DELETE_VAR, {IROperand::global(nameIdx)}, node->line);
                emitIR(IROp::JUMP, {IROperand::label(endLabel)}, node->line);
                // 异常路径：清理 catch 变量 + rethrow
                emitIR(IROp::LABEL, {IROperand::label(cleanupThrowLabel)}, node->line);
                IROperand excVreg2 = ir_->allocVReg();
                emitIR(IROp::LOAD_EXCEPTION, {excVreg2}, node->line);
                emitIR(IROp::DELETE_VAR, {IROperand::global(nameIdx)}, node->line);
                emitIR(IROp::THROW, {excVreg2}, node->line);
                emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
                // BUG-AUDIT-FINALLY-1: finally 块 IR 发射
                if (hasFinally) {
                    --tryDepth_;
                    emitIR(IROp::TRY_END, {}, node->line);
                    // AUDIT-P1.1 fix: 标记 finally 正常路径入口
                    emitIR(IROp::LABEL, {IROperand::label(finallyEntryLabel)}, node->line);
                    visitNode(node->finallyBlock.get());
                    emitIR(IROp::FINALLY_END, {}, node->line); // AUDIT-P1.1 fix
                    emitIR(IROp::JUMP, {IROperand::label(finallyEndLabel)}, node->line);
                    emitIR(IROp::LABEL, {IROperand::label(finallyCatchLabel)}, node->line);
                    IROperand excVregF = ir_->allocVReg();
                    emitIR(IROp::LOAD_EXCEPTION, {excVregF}, node->line);
                    visitNode(node->finallyBlock.get());
                    emitIR(IROp::THROW, {excVregF}, node->line);
                    emitIR(IROp::LABEL, {IROperand::label(finallyEndLabel)}, node->line);
                }
                tryFinallyStack_.pop_back(); // AUDIT-P1.1 fix
                return; // AUDIT-BUG-F7: catch 块已编译，提前返回
            }
        }
    }
    // BUG-AUDIT-FINALLY-1: catchVarName 为空时（try-finally 无 catch），
    // 不发射内层 TRY_BEGIN，异常不在栈上，无需 POP。
    // 原 L1 fix 的 POP 仅在 catchVarName 非空但 catchBlock 为空时才需要
    // （但 Parser 要求 catch 必须有变量名，所以 catchVarName 非空时 catchBlock 也非空）。
    if (node->catchBlock)
        visitNode(node->catchBlock.get());
    emitIR(IROp::LABEL, {IROperand::label(endLabel)}, node->line);
    // BUG-AUDIT-FINALLY-1: finally 块 IR 发射
    if (hasFinally) {
        --tryDepth_;
        emitIR(IROp::TRY_END, {}, node->line);
        // AUDIT-P1.1 fix: 标记 finally 正常路径入口
        emitIR(IROp::LABEL, {IROperand::label(finallyEntryLabel)}, node->line);
        visitNode(node->finallyBlock.get());
        emitIR(IROp::FINALLY_END, {}, node->line); // AUDIT-P1.1 fix
        emitIR(IROp::JUMP, {IROperand::label(finallyEndLabel)}, node->line);
        emitIR(IROp::LABEL, {IROperand::label(finallyCatchLabel)}, node->line);
        IROperand excVregF = ir_->allocVReg();
        emitIR(IROp::LOAD_EXCEPTION, {excVregF}, node->line);
        visitNode(node->finallyBlock.get());
        emitIR(IROp::THROW, {excVregF}, node->line);
        emitIR(IROp::LABEL, {IROperand::label(finallyEndLabel)}, node->line);
    }
    tryFinallyStack_.pop_back(); // AUDIT-P1.1 fix
}

void AstIRBuilder::visitThrowStmt(ThrowStmt* node) {
    IROperand val = node->expression ? visitNode(node->expression.get()) : IROperand::vreg(0);
    emitIR(IROp::THROW, {val}, node->line);
}

// ============================================================
// BytecodeIRBackend 实现
// ============================================================

BytecodeIRBackend::BytecodeIRBackend() : chunk_(nullptr) {}

bool BytecodeIRBackend::lower(const IRFunction& ir) {
    // 全新 chunk，避免旧哈希表/状态残留
    chunk_ = std::make_unique<BytecodeChunk>();
    chunk_->name = ir.name;
    // 栈式 VM 约定：方法的 arity/requiredArity 不含 this（this 作为 slot 0 单独推送）。
    // 但 IR 在 visitFunDecl 中为 RegVM 约定对方法 arity/requiredArity += 1（含 this）。
    // 此处还原为栈式 VM 约定：方法名含 '.'（ClassName.method）即视为方法，减去 this。
    // 否则栈 VM 的 executeCall/executeClassNew 会因 argCount < requiredArity 触发
    // "构造函数 init 期望 1-1 个参数，但传入了 0 个" 错误（回归：NestedMemberAccessPush）。
    const bool isMethod = ir.name.find('.') != std::string::npos;
    chunk_->arity = isMethod ? (ir.arity > 0 ? ir.arity - 1 : 0) : ir.arity;
    chunk_->requiredArity = isMethod ? (ir.requiredArity > 0 ? ir.requiredArity - 1 : 0) : ir.requiredArity;
    chunk_->localCount = ir.localCount;
    chunk_->defaultConstIndices = ir.defaultConstIndices;
    chunk_->upvalues = ir.upvalues; // VM-05/06: 复制 upvalue 描述符
    // BUG-IDE-12 fix: 复制 slot→name 映射，供栈式 VM 条件断点求值反查变量名
    chunk_->localSlotNames = ir.localSlotNames;
    vregStackDepth_.clear();
    labelToOffset_.clear();
    pendingJumps_.clear();
    irToBytecodeOffset_.clear(); // 方向四：重置映射

    // 通过 addConstant 复制常量池（保持索引一致，并填充哈希表以便后续去重）
    for (const auto& c : ir.constants) {
        chunk_->addConstant(c);
    }

    // 遍历所有基本块的所有指令，逐条 lowering
    size_t instrIndex = 0; // 方向四：展平的 IR 指令序号（与 IrViewer 一致）
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            // 方向四：记录 IR 指令 → 当前字节码偏移映射（lowering 前）
            irToBytecodeOffset_.push_back({instrIndex, chunk_->code.size()});
            if (!lowerInstruction(instr, ir)) {
                return false;
            }
            // 填充行号表（每条 IR 指令对应若干字节，统一用 instr.line）
            // BUG-IBACKEND-2: 同步填充 columns（IRInstruction 暂无 column 字段，默认 0）
            while (chunk_->lines.size() < chunk_->code.size()) {
                chunk_->lines.push_back(instr.line);
                chunk_->columns.push_back(0);
            }
            ++instrIndex;
        }
    }

    // 第二遍：回填跳转目标
    return patchJumps();
}

void BytecodeIRBackend::emitUint16(std::vector<uint8_t>& code, uint16_t v) {
    code.push_back(static_cast<uint8_t>(v & 0xFF));
    code.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

uint16_t BytecodeIRBackend::addStringConstant(const std::string& s, const IRFunction& /*ir*/) {
    // 将字符串名加入常量池（复用 chunk_ 的哈希去重），返回索引
    return chunk_->addConstant(Value(s));
}

bool BytecodeIRBackend::slotToNameConstant(uint32_t slot, const IRFunction& ir, uint16_t& outIdx) {
    // BUG-NEW fix: GLOBAL_SLOT (IMM_UINT) → 变量名 → 字符串常量索引
    // globalSlotNames_ 由 lowerModule 从 IRModule 设置；若未设置（独立 lower 调用），
    // 回退到空名并记录错误，避免静默产生坏字节码。
    // BUG-IR-SLOTNAME-1 fix: 失败时不再生成占位名 "__unknown_slot_X__" 静默产生坏字节码，
    // 改为返回 false 让调用方中止 lowering 并向上传递错误。
    if (!globalSlotNames_ || slot >= globalSlotNames_->size()) {
        Logger::Error("BytecodeIRBackend: slotToNameConstant 槽位名表缺失或越界 (slot=" + std::to_string(slot) + ")",
                      "IR");
        return false;
    }
    const std::string& name = (*globalSlotNames_)[slot];
    (void)ir; // ir 仅用于签名一致性，addStringConstant 不依赖 ir
    outIdx = chunk_->addConstant(Value(name));
    return true;
}

bool BytecodeIRBackend::lowerModule(const IRModule& module) {
    // 重置状态
    resetState();

    // BUG-NEW fix: 保存全局槽位名表，供 lowerInstruction 中 WRITEBACK_*_VAR
    // IMM_UINT 分支将 slot→name 转为字符串常量索引。
    globalSlotNames_ = &module.globalSlotNames;

    // 降低 main 函数 → chunk_
    if (module.mainFunction) {
        if (!lower(*module.mainFunction))
            return false;
    } else {
        chunk_ = std::make_unique<BytecodeChunk>();
        chunk_->name = "main";
    }

    // 降低所有子函数 → functionChunks_
    // 使用独立 backend 避免覆盖 chunk_（main）
    for (const auto& fn : module.functions) {
        if (!fn)
            continue;
        BytecodeIRBackend fnBackend;
        // 子函数共享同一全局槽位名表
        fnBackend.globalSlotNames_ = globalSlotNames_;
        if (!fnBackend.lower(*fn))
            return false;
        auto fnChunk = fnBackend.takeChunk();
        if (fnChunk) {
            functionChunks_[fn->name] = std::move(*fnChunk);
        }
    }

    // 全局槽位名表从 AstIRBuilder.globalSlotAllocator_ 获取。
    return true;
}

void BytecodeIRBackend::resetState() {
    chunk_.reset();
    functionChunks_.clear();
    vregStackDepth_.clear();
    labelToOffset_.clear();
    pendingJumps_.clear();
    irToBytecodeOffset_.clear(); // 方向四
}

bool BytecodeIRBackend::lowerInstruction(const IRInstruction& instr, const IRFunction& ir) {
    // ============================================================
    // 单条 IR 指令 → 栈式 VM 字节码 的 lowering 核心。
    // ------------------------------------------------------------
    // 设计要点（关键算法，逐步说明）：
    //   1. 栈式 VM 是「操作数栈机」：每条 IROp 对应固定次数的压栈/弹栈。
    //      例如 ADD 弹 2 压 1，LOAD_CONST 压 1，POP 弹 1，JUMP 不碰栈。
    //      因为 IR 是 SSA 风格（每个 vreg 只被定义一次、按定义顺序被使用），
    //      操作数在运行时的入栈顺序与 IR 指令顺序天然一致，lowering 无需
    //      显式维护「vreg→栈偏移」映射也能保证栈平衡——这正是栈式后端比
    //      寄存器式后端简单的原因（寄存器式需在 vregToReg 阶段物化每个 vreg）。
    //   2. 发射布局：每个 case 直接 push_back opcode，再按该指令的定长格式
    //      push 操作数（2 字节小端 short 或 1 字节 slot）。变长指令（仅
    //      OP_CLOSURE）在编译期已展开为「nameIdx + upvalueCount + 2*描述符」。
    //   3. vregStackDepth_ 辅助表：对每条产生 dest vreg 的 load 指令，记录其
    //      发射位置 (code offset)。它实际不用于正确性判定（见要点 1），而是作为
    //      调试/潜在栈深度分析的辅助信息维护；绝不可据其反向推算栈偏移。
    //   4. 控制流两遍法：LABEL 不产生字节码，仅写入 labelToOffset_；跳转类
    //      （JUMP / JUMP_IF_FALSE）先 push opcode 并占位 2 字节操作数，把待回填
    //      项加入 pendingJumps_。本函数（第一遍）结束后由 patchJumps()（第二遍）
    //      查 labelToOffset_ 回填绝对/相对偏移（见 BUG-EXC-1：OP_TRY_BEGIN 用
    //      相对偏移，其余用绝对偏移）。
    //   5. 防御性边界检查：槽位/索引 >=256 或常量池索引越界时 Logger::Error
    //      并返回 false，让 lower() 中止并向上传播错误，避免静态截断生成坏字节码。
    //   6. WRITEBACK_*_VAR 的特殊处理：其 GLOBAL_SLOT 以 IMM_UINT 编码槽位号，
    //      但栈式 VM 的 OP_WRITEBACK_*_VAR 把操作数当作「常量池中的变量名索引」，
    //      故需经 slotToNameConstant() 转换为变量名常量索引（否则误读为未定义变量）。
    // ============================================================
    // 辅助：从 globalNames 取名（索引越界时返回空串）
    auto globalName = [&](uint32_t idx) -> std::string {
        return idx < ir.globalNames.size() ? ir.globalNames[idx] : std::string{};
    };

    switch (instr.op) {
    // ---- 常量加载 ----
    case IROp::LOAD_CONST: {
        // 按 Value 类型分发 OP_INT/OP_FLOAT/OP_STRING
        if (instr.operands.size() < 2)
            return false;
        uint32_t constIdx = instr.operands[1].index;
        if (constIdx >= ir.constants.size())
            return false;
        const Value& v = ir.constants[constIdx];
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        switch (v.getType()) {
        case ValueType::VAL_INT:
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INT));
            break;
        case ValueType::VAL_FLOAT:
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FLOAT));
            break;
        case ValueType::VAL_STRING:
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_STRING));
            break;
        default:
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CONSTANT));
            break;
        }
        emitUint16(chunk_->code, static_cast<uint16_t>(constIdx));
        break;
    }
    case IROp::LOAD_NULL: {
        if (instr.operands.empty())
            return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
        break;
    }
    case IROp::LOAD_TRUE: {
        if (instr.operands.empty())
            return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TRUE));
        break;
    }
    case IROp::LOAD_FALSE: {
        if (instr.operands.empty())
            return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FALSE));
        break;
    }

    // ---- 变量访问 ----
    case IROp::LOAD_LOCAL: {
        if (instr.operands.size() < 2)
            return false;
        // slot 编码为 1 字节，超 255 静默截断会读写错误栈槽
        if (instr.operands[1].index >= 256) {
            Logger::Error(
                "IR lowering: 局部变量槽位索引超出 255 上限 (slot=" + std::to_string(instr.operands[1].index) + ")",
                "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index)); // slot(1B)
        break;
    }
    case IROp::STORE_LOCAL: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error(
                "IR lowering: 局部变量槽位索引超出 255 上限 (slot=" + std::to_string(instr.operands[0].index) + ")",
                "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // slot(1B)
        // 注意：OP_SET_LOCAL 用 peek(0) 不消费栈顶值（与 Compiler.cpp 一致）。
        // IR 的 and/or 短路逻辑依赖此行为：STORE_LOCAL 将值写入 temp slot 后值仍在栈上，
        // 后续 LOAD_LOCAL 从同一 slot 读取。需要消费源值的场景（如 super.method() 的
        // LOAD_MUTATED + STORE_LOCAL）应在 IR 层面显式 emit IROp::POP。
        break;
    }
    case IROp::LOAD_GLOBAL: {
        // 限制3：按操作数 kind 分发
        //   IMM_UINT → OP_GET_GLOBAL slot(2B)（槽位版）
        //   GLOBAL_NAME → OP_GET_VAR nameIdx(2B)（名称版）
        if (instr.operands.size() < 2)
            return false;
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        if (instr.operands[1].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[1].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::STORE_GLOBAL: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[0].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::DEFINE_GLOBAL: {
        // 限制3：按操作数 kind 分发
        //   IMM_UINT → OP_DEFINE_GLOBAL slot(2B)（槽位版）
        //   GLOBAL_NAME → OP_DEFINE_VAR nameIdx(2B)（名称版）
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_GLOBAL));
            emitUint16(chunk_->code, static_cast<uint16_t>(instr.operands[0].index));
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::DELETE_VAR: {
        // BUG-IR-TRY-1 fix: 删除全局变量（catch 块退出后清理 catch 变量）
        // operands: [global_idx]，kind 可为 IMM_UINT (GLOBAL_SLOT) 或 GLOBAL_NAME
        // → OP_DELETE_VAR nameConstIdx(2B)（栈式 VM 按名称常量删除）
        if (instr.operands.size() < 1)
            return false;
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            // BUG-IR-SLOTNAME-1 fix: slot→name 转换可能失败，需检查返回值
            uint16_t nameConstIdx = 0;
            if (!slotToNameConstant(instr.operands[0].index, ir, nameConstIdx))
                return false;
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DELETE_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DELETE_VAR));
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::LOAD_UPVALUE: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[1].index >= 256) {
            Logger::Error(
                "IR lowering: upvalue 索引超出 255 上限 (idx=" + std::to_string(instr.operands[1].index) + ")", "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index));
        break;
    }
    case IROp::STORE_UPVALUE: {
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error(
                "IR lowering: upvalue 索引超出 255 上限 (idx=" + std::to_string(instr.operands[0].index) + ")", "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SET_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index));
        // 注意：OP_SET_UPVALUE 用 peek(0) 不消费栈顶值（与 STORE_LOCAL 一致）。
        // 需要消费源值的场景应在 IR 层面显式 emit IROp::POP。
        break;
    }
    case IROp::CLOSE_UPVALUE: {
        // B1 fix: operand 现为 slot_base（块作用域基址），非 upvalue 索引。
        // 运行时关闭所有指向 slot >= basePointer+slot_base 的 open upvalues。
        if (instr.operands.empty())
            return false;
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: CLOSE_UPVALUE slot_base 超出 255 上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLOSE_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index));
        break;
    }

    // ---- 算术 ----
    case IROp::ADD:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_ADD));
        break;
    case IROp::SUB:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SUBTRACT));
        break;
    case IROp::MUL:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MULTIPLY));
        break;
    case IROp::DIV:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DIVIDE));
        break;
    case IROp::MOD:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MODULO));
        break;
    case IROp::NEGATE:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NEGATE));
        break;

    // ---- 比较 ----
    case IROp::EQ:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_EQUAL));
        break;
    case IROp::NEQ:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NOT_EQUAL));
        break;
    case IROp::LT:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LESS));
        break;
    case IROp::GT:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GREATER));
        break;
    case IROp::LTE:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LESS_EQUAL));
        break;
    case IROp::GTE:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GREATER_EQUAL));
        break;

    // ---- 逻辑 ----
    case IROp::NOT:
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NOT));
        break;

    // ---- 控制流 ----
    case IROp::LABEL: {
        // 记录标签对应的字节码偏移（不产生字节码）
        if (!instr.operands.empty() && instr.operands[0].kind == IROperandKind::LABEL) {
            labelToOffset_[instr.operands[0].index] = chunk_->code.size();
        }
        break;
    }
    case IROp::JUMP: {
        if (instr.operands.empty())
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_JUMP));
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index, false});
        emitUint16(chunk_->code, 0); // 占位符，第二遍回填
        break;
    }
    case IROp::JUMP_IF_FALSE: {
        if (instr.operands.size() < 2)
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_JUMP_IF_FALSE));
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[1].index, false});
        emitUint16(chunk_->code, 0); // 占位符
        break;
    }

    // ---- 调用 ----
    case IROp::CALL: {
        // CALL dest, name_idx, arg_count, args... → OP_CALL nameIdx argCount
        if (instr.operands.size() < 3)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限，对齐 RegisterBytecodeBackend L410-414
        if (instr.operands[2].index > 255) {
            Logger::Error("BytecodeIRBackend: CALL argCount 超出 255 上限 (" + std::to_string(instr.operands[2].index) +
                              ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF)); // argCount
        break;
    }
    case IROp::CALL_EXPR: {
        if (instr.operands.size() < 3)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限，对齐 RegisterBytecodeBackend L435-439
        if (instr.operands[2].index > 255) {
            Logger::Error("BytecodeIRBackend: CALL_EXPR argCount 超出 255 上限 (" +
                              std::to_string(instr.operands[2].index) + ")",
                          "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CALL_EXPR));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF)); // argCount
        break;
    }
    case IROp::RETURN: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_RETURN));
        break;
    }
    case IROp::RETURN_NULL: {
        // RETURN_NULL 前先 emit OP_NULL（栈顶作为返回值）
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_RETURN));
        break;
    }

    // ---- 闭包 ----
    case IROp::MAKE_CLOSURE: {
        // MAKE_CLOSURE dest, name_idx, uv_count, [isLocal, idx]×uv_count
        if (instr.operands.size() < 3)
            return false;
        // AUDIT-STACKCLOSURE fix: 对齐 RegisterBytecodeBackend 的 P2-3 fix，
        // uvCount/isLocal/idx 编码为 1 字节，原 & 0xFF 静默截断会生成错误 upvalue 描述符。
        if (instr.operands[2].index >= 256) {
            Logger::Error("BytecodeIRBackend: MAKE_CLOSURE uvCount 超出 255 上限 (" +
                              std::to_string(instr.operands[2].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLOSURE));
        emitUint16(chunk_->code, nameConstIdx);
        uint32_t uvCount = instr.operands[2].index;
        chunk_->code.push_back(static_cast<uint8_t>(uvCount));
        for (uint32_t i = 0; i < uvCount; ++i) {
            size_t base = 3 + i * 2;
            if (base + 1 >= instr.operands.size())
                return false;
            if (instr.operands[base].index >= 256) {
                Logger::Error("BytecodeIRBackend: MAKE_CLOSURE isLocal 超出 255 上限 (" +
                                  std::to_string(instr.operands[base].index) + ")",
                              "IR");
                return false;
            }
            if (instr.operands[base + 1].index >= 256) {
                Logger::Error("BytecodeIRBackend: MAKE_CLOSURE upvalue idx 超出 255 上限 (" +
                                  std::to_string(instr.operands[base + 1].index) + ")",
                              "IR");
                return false;
            }
            chunk_->code.push_back(static_cast<uint8_t>(instr.operands[base].index));     // isLocal
            chunk_->code.push_back(static_cast<uint8_t>(instr.operands[base + 1].index)); // idx
        }
        break;
    }

    // ---- 容器 ----
    case IROp::BUILD_ARRAY: {
        if (instr.operands.size() < 2)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 count 8 位上限，对齐 RegisterBytecodeBackend L556-560
        if (instr.operands[1].index > 255) {
            Logger::Error("BytecodeIRBackend: BUILD_ARRAY count 超出 255 上限 (" +
                              std::to_string(instr.operands[1].index) + ")",
                          "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_BUILD_ARRAY));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF)); // count
        break;
    }
    case IROp::BUILD_DICT: {
        if (instr.operands.size() < 2)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 pairCount 8 位上限，对齐 RegisterBytecodeBackend L577-581
        if (instr.operands[1].index > 255) {
            Logger::Error("BytecodeIRBackend: BUILD_DICT pairCount 超出 255 上限 (" +
                              std::to_string(instr.operands[1].index) + ")",
                          "IR");
            return false;
        }
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_BUILD_DICT));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[1].index & 0xFF)); // pairCount
        break;
    }
    case IROp::INDEX_GET: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INDEX_GET));
        if (!instr.operands.empty()) {
            vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        }
        break;
    }
    case IROp::INDEX_SET: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INDEX_SET));
        break;
    }

    // ---- 成员访问 ----
    case IROp::MEMBER_GET: {
        if (instr.operands.size() < 3)
            return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MEMBER_GET));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }
    case IROp::MEMBER_SET: {
        if (instr.operands.size() < 3)
            return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_MEMBER_SET));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }
    case IROp::SUPER_MEMBER_GET: {
        // P1 fix: super.field → OP_SUPER_MEMBER_GET（与 OP_MEMBER_GET 格式相同）
        if (instr.operands.size() < 3)
            return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SUPER_MEMBER_GET));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }

    // ---- 方法调用 ----
    case IROp::METHOD_CALL: {
        // METHOD_CALL dest, obj, method_idx, arg_count, args...
        // → OP_METHOD_CALL nameIdx(2B) argCount(1B) recvVarIdx(2B) recvSlot(1B)
        if (instr.operands.size() < 4)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限（含 this 共 255），对齐 RegisterBytecodeBackend L460-464
        if (instr.operands[3].index > 254) {
            Logger::Error("BytecodeIRBackend: METHOD_CALL argCount 超出 254 上限（含 this 共 255）(" +
                              std::to_string(instr.operands[3].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_METHOD_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[3].index & 0xFF)); // argCount
        emitUint16(chunk_->code, 0xFFFF);                                             // recvVarIdx（简化占位）
        chunk_->code.push_back(0xFF);                                                 // recvSlot（简化占位）
        break;
    }
    case IROp::SUPER_CALL: {
        // P1 fix: super.method(args) → OP_SUPER_CALL
        // operands: [dest, this_vreg, method_idx, class_idx, arg_count, args...]
        // → OP_SUPER_CALL nameIdx(2B) argCount(1B) recvVarIdx(2B) recvSlot(1B) classIdx(2B)
        if (instr.operands.size() < 5)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限（含 this 共 255），对齐 RegisterBytecodeBackend L487-491
        if (instr.operands[4].index > 254) {
            Logger::Error("BytecodeIRBackend: SUPER_CALL argCount 超出 254 上限（含 this 共 255）(" +
                              std::to_string(instr.operands[4].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[2].index), ir);
        uint16_t classConstIdx = addStringConstant(globalName(instr.operands[3].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_SUPER_CALL));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[4].index & 0xFF)); // argCount
        emitUint16(chunk_->code, 0xFFFF);                                             // recvVarIdx（简化占位）
        chunk_->code.push_back(0xFF);                                                 // recvSlot（简化占位）
        emitUint16(chunk_->code, classConstIdx);                                      // classIdx
        break;
    }

    // ---- 类 ----
    case IROp::DEFINE_CLASS: {
        // 操作数（BUG-INH-1 fix: 新增字段默认值常量索引；
        //   BUG-INH-IR-1 fix: 新增字段表达式临时局部变量槽位）:
        //   [0]=className(FUNC_NAME), [1]=parentName(IMM_UINT, UINT32_MAX=无父类)
        //   [2] fieldCount (IMM_UINT)
        //   [3+i*3] field name (FIELD_NAME)
        //   [3+i*3+1] fieldDefaultConstIdx (IMM_UINT, UINT32_MAX=null/无默认值/有表达式)
        //   [3+i*3+2] fieldExprLocalSlot (IMM_UINT, UINT32_MAX=使用常量或null, 否则使用临时 local slot)
        //   [3+3F] methodCount (IMM_UINT)
        //   [3+3F+1 .. ] (methodName FIELD_NAME, funName FUNC_NAME) × M
        if (instr.operands.size() < 2)
            return false;
        uint32_t nameIdx = instr.operands[0].index;
        uint16_t nameConstIdx = addStringConstant(globalName(nameIdx), ir);

        // 与 Compiler.cpp visitClassDecl (1792-1823) 对齐：stack VM 的 executeDefineClass
        // 期望栈顶有模板实例（OP_CLASS_NEW 推入），并从 pendingFieldOrder_ 提取字段顺序
        // （OP_INIT_FIELD 填充）。若仅 emit OP_DEFINE_CLASS，pop() 会读到错误栈值导致
        // "模板值不是实例" 运行时错误（回归：NestedMemberAccessPush）。
        // BUG-INH-1 fix: 原实现所有字段默认值为 null，现在从 IR 常量池提取字面量默认值，
        // 对齐 Compiler.cpp:1843-1847 直接路径。
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLASS_NEW));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(0)); // argCount = 0

        // 为每个字段 emit 默认值 + OP_INIT_FIELD（按 IR 操作数中的字段名顺序）
        if (instr.operands.size() >= 3) {
            uint32_t fieldCount = instr.operands[2].index;
            for (uint32_t fi = 0; fi < fieldCount; ++fi) {
                size_t nameOpIdx = 3 + fi * 3;
                size_t defaultOpIdx = 3 + fi * 3 + 1;
                size_t exprLocalSlotOpIdx = 3 + fi * 3 + 2;
                if (exprLocalSlotOpIdx >= instr.operands.size())
                    return false;
                uint32_t fieldGlobalIdx = instr.operands[nameOpIdx].index;
                uint16_t fieldConstIdx = addStringConstant(globalName(fieldGlobalIdx), ir);
                // BUG-INH-1 fix: 从 IR 常量池提取字段默认值
                uint32_t defaultConstIdx = instr.operands[defaultOpIdx].index;
                // BUG-INH-IR-1 fix: 非字面量表达式，从临时 local slot 读取求值结果
                uint32_t exprLocalSlot = instr.operands[exprLocalSlotOpIdx].index;
                if (exprLocalSlot != UINT32_MAX) {
                    // 非字面量表达式：emit OP_GET_LOCAL 将求值结果压栈
                    // （visitClassDecl 已在 DEFINE_CLASS 之前 emit STORE_LOCAL 存入 slot）
                    if (exprLocalSlot >= 256) {
                        Logger::Error("BytecodeIRBackend: DEFINE_CLASS 字段临时局部变量槽位超出 255 上限", "IR");
                        return false;
                    }
                    chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_GET_LOCAL));
                    chunk_->code.push_back(static_cast<uint8_t>(exprLocalSlot));
                } else if (defaultConstIdx == UINT32_MAX) {
                    chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
                } else {
                    if (defaultConstIdx >= ir.constants.size()) {
                        Logger::Error("BytecodeIRBackend: DEFINE_CLASS 字段默认值常量索引越界", "IR");
                        return false;
                    }
                    const Value& defaultVal = ir.constants[defaultConstIdx];
                    // 根据 Value 类型 emit 对应字节码（对齐 Compiler.cpp 直接路径）
                    if (defaultVal.isInt()) {
                        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INT));
                        uint16_t constIdx = chunk_->addConstant(defaultVal);
                        emitUint16(chunk_->code, constIdx);
                    } else if (defaultVal.isFloat()) {
                        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FLOAT));
                        uint16_t constIdx = chunk_->addConstant(defaultVal);
                        emitUint16(chunk_->code, constIdx);
                    } else if (defaultVal.isString()) {
                        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_STRING));
                        uint16_t constIdx = chunk_->addConstant(defaultVal);
                        emitUint16(chunk_->code, constIdx);
                    } else if (defaultVal.isBool()) {
                        chunk_->code.push_back(
                            static_cast<uint8_t>(defaultVal.boolVal() ? OpCode::OP_TRUE : OpCode::OP_FALSE));
                    } else {
                        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_NULL));
                    }
                }
                chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INIT_FIELD));
                emitUint16(chunk_->code, fieldConstIdx);
            }
        }

        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DEFINE_CLASS));
        emitUint16(chunk_->code, nameConstIdx);
        // 与 Compiler.cpp:1810-1817 对齐：编码父类名索引
        // 0xFFFF 表示无父类；否则为父类名在常量池中的索引
        uint32_t parentIdx = instr.operands[1].index;
        if (parentIdx == UINT32_MAX) {
            emitUint16(chunk_->code, 0xFFFF);
        } else {
            uint16_t superConstIdx = addStringConstant(globalName(parentIdx), ir);
            emitUint16(chunk_->code, superConstIdx);
        }
        break;
    }
    case IROp::CLASS_NEW: {
        if (instr.operands.size() < 3)
            return false;
        // BUG-IR-BOUND-1 fix: 检查 argCount 8 位上限（含 this 共 255），对齐 RegisterBytecodeBackend L748-752
        if (instr.operands[2].index > 254) {
            Logger::Error("BytecodeIRBackend: CLASS_NEW argCount 超出 254 上限（含 this 共 255）(" +
                              std::to_string(instr.operands[2].index) + ")",
                          "IR");
            return false;
        }
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_CLASS_NEW));
        emitUint16(chunk_->code, nameConstIdx);
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[2].index & 0xFF)); // argCount
        break;
    }
    case IROp::INIT_FIELD: {
        if (instr.operands.empty())
            return false;
        uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_INIT_FIELD));
        emitUint16(chunk_->code, nameConstIdx);
        break;
    }

    // ---- 异常 ----
    case IROp::TRY_BEGIN: {
        if (instr.operands.empty())
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TRY_BEGIN));
        // BUG-EXC-1 fix: 标记 isTryBegin=true，patchJumps 对 TRY_BEGIN 写相对偏移
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index, false, true});
        emitUint16(chunk_->code, 0); // catchOffset 占位符，第二遍回填
        break;
    }
    case IROp::TRY_END: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TRY_END));
        break;
    }
    case IROp::THROW: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_THROW));
        break;
    }
    case IROp::LOAD_EXCEPTION: {
        // P1-4 fix: 栈式 VM 的 throwException 已将异常值推入栈顶，
        // 此处为 no-op（不发射字节码）。后续 STORE_LOCAL/DEFINE_GLOBAL 从栈顶 pop。
        break;
    }
    // AUDIT-P1.1 fix: break/continue finally 续跳 lowering
    case IROp::PUSH_JUMP_TARGET: {
        // operands: [label_idx]
        // → OP_PUSH_JUMP_TARGET target(2B 绝对偏移，第二遍回填)
        if (instr.operands.empty())
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_PUSH_JUMP_TARGET));
        // 与 OP_JUMP 一致：绝对偏移（isTryBegin=false），patchJumps 回填 label→绝对 IP
        pendingJumps_.push_back({chunk_->code.size(), instr.operands[0].index, false});
        emitUint16(chunk_->code, 0); // 占位符，第二遍回填
        break;
    }
    case IROp::FINALLY_END: {
        // 无操作数 → OP_FINALLY_END（1 字节）
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_FINALLY_END));
        break;
    }

    // ---- 写回指令（限制2）----
    case IROp::WRITEBACK_MEMBER_VAR: {
        // operands: [var_idx, field_idx]
        // → OP_WRITEBACK_MEMBER_VAR varIdx(2B) fieldIdx(2B)
        if (instr.operands.size() < 2)
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_MEMBER_VAR));
        // varIdx(2B)：栈式 VM 的 OP_WRITEBACK_*_VAR 将 varIdx 当作常量池索引处理
        // （取 chunk.constants[varIdx].stringVal() 作为变量名）。GLOBAL_SLOT (IMM_UINT)
        // 携带的是槽位号，不是常量索引，直接 emit 会导致误读常量池。
        // BUG-NEW fix: 对 IMM_UINT 分支查 globalSlotNames_ 将 slot→name，再以字符串常量
        // 索引形式 emit，与 GLOBAL_NAME 路径统一。
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            uint16_t nameConstIdx = 0;
            if (!slotToNameConstant(instr.operands[0].index, ir, nameConstIdx))
                return false;
            emitUint16(chunk_->code, nameConstIdx);
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            emitUint16(chunk_->code, nameConstIdx);
        }
        // fieldIdx(2B)：字段名常量索引
        uint16_t fieldConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        emitUint16(chunk_->code, fieldConstIdx);
        break;
    }
    case IROp::WRITEBACK_MEMBER_LOCAL: {
        // operands: [slot, field_idx]
        // → OP_WRITEBACK_MEMBER_LOCAL slot(1B) fieldIdx(2B)
        if (instr.operands.size() < 2)
            return false;
        // Bug-14 同型修复：slot 超 255 静默截断会读写错误栈槽
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: WRITEBACK_MEMBER_LOCAL 槽位索引超出 255 上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_MEMBER_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // slot(1B)
        uint16_t fieldConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        emitUint16(chunk_->code, fieldConstIdx);
        break;
    }
    case IROp::WRITEBACK_INDEX_VAR: {
        // operands: [var_idx]
        // → OP_WRITEBACK_INDEX_VAR varIdx(2B)
        if (instr.operands.size() < 1)
            return false;
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_INDEX_VAR));
        // BUG-NEW fix: 同 WRITEBACK_MEMBER_VAR，IMM_UINT 需转 slot→name 常量索引
        if (instr.operands[0].kind == IROperandKind::IMM_UINT) {
            uint16_t nameConstIdx = 0;
            if (!slotToNameConstant(instr.operands[0].index, ir, nameConstIdx))
                return false;
            emitUint16(chunk_->code, nameConstIdx);
        } else {
            uint16_t nameConstIdx = addStringConstant(globalName(instr.operands[0].index), ir);
            emitUint16(chunk_->code, nameConstIdx);
        }
        break;
    }
    case IROp::WRITEBACK_INDEX_LOCAL: {
        // operands: [slot]
        // → OP_WRITEBACK_INDEX_LOCAL slot(1B)
        if (instr.operands.size() < 1)
            return false;
        // Bug-14 同型修复：slot 超 255 静默截断
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: WRITEBACK_INDEX_LOCAL 槽位索引超出 255 上限 (slot=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_INDEX_LOCAL));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // slot(1B)
        break;
    }
    case IROp::WRITEBACK_MEMBER_UPVALUE: {
        // operands: [uv_idx, field_idx]
        // → OP_WRITEBACK_MEMBER_UPVALUE uvIdx(1B) fieldIdx(2B)
        if (instr.operands.size() < 2)
            return false;
        // Bug-14 同型修复：uvIdx 超 255 静默截断
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: WRITEBACK_MEMBER_UPVALUE upvalue 索引超出 255 上限 (uvIdx=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_MEMBER_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // uvIdx(1B)
        uint16_t fieldConstIdx = addStringConstant(globalName(instr.operands[1].index), ir);
        emitUint16(chunk_->code, fieldConstIdx);
        break;
    }
    case IROp::WRITEBACK_INDEX_UPVALUE: {
        // operands: [uv_idx]
        // → OP_WRITEBACK_INDEX_UPVALUE uvIdx(1B)
        if (instr.operands.size() < 1)
            return false;
        // Bug-14 同型修复：uvIdx 超 255 静默截断
        if (instr.operands[0].index >= 256) {
            Logger::Error("IR lowering: WRITEBACK_INDEX_UPVALUE upvalue 索引超出 255 上限 (uvIdx=" +
                              std::to_string(instr.operands[0].index) + ")",
                          "IR");
            return false;
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_WRITEBACK_INDEX_UPVALUE));
        chunk_->code.push_back(static_cast<uint8_t>(instr.operands[0].index)); // uvIdx(1B)
        break;
    }

    // ---- 其他 ----
    case IROp::PRINT: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_PRINT));
        break;
    }
    case IROp::POP: {
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_POP));
        break;
    }
    case IROp::DUP: {
        if (!instr.operands.empty()) {
            vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_DUP));
        break;
    }
    case IROp::LOAD_MUTATED: {
        // MEDIUM-1/2 fix: 读取 lastMutatedReceiver_ 到栈顶（不清除）
        if (!instr.operands.empty()) {
            vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        }
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_LOAD_MUTATED));
        break;
    }

    // 2026-06-29: 类型注解运行时检查
    // operands: [src_vreg, type_const_idx]  src_vreg 已在栈顶，peek 不弹栈
    case IROp::TYPE_CHECK: {
        if (instr.operands.size() < 2) {
            Logger::Error("BytecodeIRBackend: TYPE_CHECK 操作数不足", "IR");
            return false;
        }
        // type_const_idx 已在常量池复制阶段（lower() 的 for 循环）同步到 BytecodeChunk
        uint16_t typeIdx = static_cast<uint16_t>(instr.operands[1].index);
        // AUDIT-P2 fix: 与 DUP/LOAD_MUTATED 保持一致，更新 src_vreg 的栈深度映射。
        // 虽然 OP_TYPE_CHECK 是 peek 不 push，但记录 src_vreg 在此处"仍然有效"的栈深度，
        // 避免后续指令通过 vregStackDepth_ 查询时拿到过期记录点。
        vregStackDepth_[instr.operands[0].index] = static_cast<uint32_t>(chunk_->code.size());
        chunk_->code.push_back(static_cast<uint8_t>(OpCode::OP_TYPE_CHECK));
        chunk_->code.push_back(static_cast<uint8_t>(typeIdx & 0xFF));
        chunk_->code.push_back(static_cast<uint8_t>((typeIdx >> 8) & 0xFF));
        break;
    }

    default:
        Logger::Error("BytecodeIRBackend: 未知 IR 操作码 " + std::to_string(static_cast<int>(instr.op)), "IR");
        return false;
    }
    return true;
}

bool BytecodeIRBackend::patchJumps() {
    // 遍历所有待回填跳转，查 labelToOffset 写回目标偏移
    for (const auto& pj : pendingJumps_) {
        auto it = labelToOffset_.find(pj.targetLabel);
        if (it == labelToOffset_.end()) {
            Logger::Error("BytecodeIRBackend: 未找到标签 " + std::to_string(pj.targetLabel), "IR");
            return false;
        }
        // BUG-EXC-1 fix: OP_JUMP/OP_JUMP_IF_FALSE 用绝对偏移（ip = target），
        // 但 OP_TRY_BEGIN 用相对偏移（catchIp = ip + 3 + catchOffset）。
        // pj.codeOffset 指向操作数首字节（opcode 在 codeOffset-1），
        // StackVM 执行时 ip = codeOffset-1，故相对偏移 = target - (ip+3) = target - (codeOffset+2)。
        size_t targetOffset = it->second;
        uint16_t target;
        if (pj.isTryBegin) {
            if (targetOffset < pj.codeOffset + 2) {
                Logger::Error(
                    "BytecodeIRBackend: TRY_BEGIN catch 目标在 TRY_BEGIN 之前 (catch=" + std::to_string(targetOffset) +
                        ", tryBegin=" + std::to_string(pj.codeOffset - 1) + ")",
                    "IR");
                return false;
            }
            // BUG-IR-PATCH-1 fix: 检查 catch 相对偏移 64KB 上限，对齐 Compiler.cpp visitTryStmt 的 cleanupThrowOffset >
            // 65535 检查。 原实现 static_cast<uint16_t> 静默截断，导致 VM
            // 跳转到错误位置执行乱码字节码（三后端不一致）。
            size_t relOff = targetOffset - (pj.codeOffset + 2);
            if (relOff > 65535) {
                Logger::Error(
                    "BytecodeIRBackend: TRY_BEGIN catch 相对偏移超过 64KB 限制 (relOff=" + std::to_string(relOff) + ")",
                    "IR");
                return false;
            }
            target = static_cast<uint16_t>(relOff);
        } else {
            // 16-bit 编码限制：字节码体积超过 64KB 时跳转目标会截断，需显式检查
            // （对齐 Compiler::safeCodeOffset() 的 65535 上限保护）
            if (targetOffset > 65535) {
                Logger::Error("BytecodeIRBackend: 跳转目标偏移超过 64KB 限制 (offset=" + std::to_string(targetOffset) +
                                  ")",
                              "IR");
                return false;
            }
            target = static_cast<uint16_t>(targetOffset);
        }
        if (pj.codeOffset + 1 >= chunk_->code.size()) {
            Logger::Error("BytecodeIRBackend: 跳转操作数越界", "IR");
            return false;
        }
        chunk_->code[pj.codeOffset] = static_cast<uint8_t>(target & 0xFF);
        chunk_->code[pj.codeOffset + 1] = static_cast<uint8_t>((target >> 8) & 0xFF);
    }
    return true;
}

// ============================================================
// IRToString — 调试输出
// ============================================================

// Dedup-4E: 公共 API，供 IrViewer 与 IRToString 共用（避免 IrViewer 维护过时副本）
const char* irOpName(IROp op) {
    switch (op) {
    // 常量加载
    case IROp::LOAD_CONST:
        return "LOAD_CONST";
    case IROp::LOAD_NULL:
        return "LOAD_NULL";
    case IROp::LOAD_TRUE:
        return "LOAD_TRUE";
    case IROp::LOAD_FALSE:
        return "LOAD_FALSE";
    // 变量访问
    case IROp::LOAD_LOCAL:
        return "LOAD_LOCAL";
    case IROp::STORE_LOCAL:
        return "STORE_LOCAL";
    case IROp::LOAD_GLOBAL:
        return "LOAD_GLOBAL";
    case IROp::STORE_GLOBAL:
        return "STORE_GLOBAL";
    case IROp::DEFINE_GLOBAL:
        return "DEFINE_GLOBAL";
    case IROp::DELETE_VAR:
        return "DELETE_VAR";
    case IROp::LOAD_UPVALUE:
        return "LOAD_UPVALUE";
    case IROp::STORE_UPVALUE:
        return "STORE_UPVALUE";
    case IROp::CLOSE_UPVALUE:
        return "CLOSE_UPVALUE";
    // 算术
    case IROp::ADD:
        return "ADD";
    case IROp::SUB:
        return "SUB";
    case IROp::MUL:
        return "MUL";
    case IROp::DIV:
        return "DIV";
    case IROp::MOD:
        return "MOD";
    case IROp::NEGATE:
        return "NEGATE";
    // 比较
    case IROp::EQ:
        return "EQ";
    case IROp::NEQ:
        return "NEQ";
    case IROp::LT:
        return "LT";
    case IROp::GT:
        return "GT";
    case IROp::LTE:
        return "LTE";
    case IROp::GTE:
        return "GTE";
    // 逻辑
    case IROp::NOT:
        return "NOT";
    // 控制流
    case IROp::JUMP:
        return "JUMP";
    case IROp::JUMP_IF_FALSE:
        return "JUMP_IF_FALSE";
    case IROp::LABEL:
        return "LABEL";
    // 调用
    case IROp::CALL:
        return "CALL";
    case IROp::CALL_EXPR:
        return "CALL_EXPR";
    case IROp::RETURN:
        return "RETURN";
    case IROp::RETURN_NULL:
        return "RETURN_NULL";
    // 闭包
    case IROp::MAKE_CLOSURE:
        return "MAKE_CLOSURE";
    // 容器
    case IROp::BUILD_ARRAY:
        return "BUILD_ARRAY";
    case IROp::BUILD_DICT:
        return "BUILD_DICT";
    case IROp::INDEX_GET:
        return "INDEX_GET";
    case IROp::INDEX_SET:
        return "INDEX_SET";
    // 成员访问
    case IROp::MEMBER_GET:
        return "MEMBER_GET";
    case IROp::MEMBER_SET:
        return "MEMBER_SET";
    case IROp::SUPER_MEMBER_GET:
        return "SUPER_MEMBER_GET";
    // 方法调用
    case IROp::METHOD_CALL:
        return "METHOD_CALL";
    case IROp::SUPER_CALL:
        return "SUPER_CALL";
    // 类
    case IROp::DEFINE_CLASS:
        return "DEFINE_CLASS";
    case IROp::CLASS_NEW:
        return "CLASS_NEW";
    case IROp::INIT_FIELD:
        return "INIT_FIELD";
    // 异常
    case IROp::TRY_BEGIN:
        return "TRY_BEGIN";
    case IROp::TRY_END:
        return "TRY_END";
    case IROp::THROW:
        return "THROW";
    case IROp::LOAD_EXCEPTION:
        return "LOAD_EXCEPTION";
    // AUDIT-P1.1 fix: finally 续跳
    case IROp::PUSH_JUMP_TARGET:
        return "PUSH_JUMP_TARGET";
    case IROp::FINALLY_END:
        return "FINALLY_END";
    // 写回指令（限制2）
    case IROp::WRITEBACK_MEMBER_VAR:
        return "WRITEBACK_MEMBER_VAR";
    case IROp::WRITEBACK_MEMBER_LOCAL:
        return "WRITEBACK_MEMBER_LOCAL";
    case IROp::WRITEBACK_INDEX_VAR:
        return "WRITEBACK_INDEX_VAR";
    case IROp::WRITEBACK_INDEX_LOCAL:
        return "WRITEBACK_INDEX_LOCAL";
    case IROp::WRITEBACK_MEMBER_UPVALUE:
        return "WRITEBACK_MEMBER_UPVALUE";
    case IROp::WRITEBACK_INDEX_UPVALUE:
        return "WRITEBACK_INDEX_UPVALUE";
    // 其他
    case IROp::PRINT:
        return "PRINT";
    case IROp::POP:
        return "POP";
    case IROp::DUP:
        return "DUP";
    case IROp::LOAD_MUTATED:
        return "LOAD_MUTATED";
    case IROp::TYPE_CHECK:
        return "TYPE_CHECK";
    // Bug-6 同型修复：枚举扩展时静默走 "?"，加 default + assert 兜底
    default:
        // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
        Logger::Error("irOpName: 未处理的 IROp 枚举值 " + std::to_string(static_cast<int>(op)), "IR");
        assert(false && "irOpName: 未处理的 IROp 枚举值");
        return "?";
    }
}

// Dedup-4F: 单条 IR 指令格式化，IRToString 与 IrViewer 共用（避免 IrViewer 维护过时副本）
std::string formatIRInstruction(const IRInstruction& instr) {
    std::ostringstream oss;
    oss << irOpName(instr.op);
    for (const auto& operand : instr.operands) {
        const char* kindStr = "?";
        switch (operand.kind) {
        case IROperandKind::CONSTANT:
            kindStr = "c";
            break;
        case IROperandKind::VIRTUAL:
            kindStr = "v";
            break;
        case IROperandKind::LABEL:
            kindStr = "L";
            break;
        case IROperandKind::GLOBAL_NAME:
            kindStr = "g";
            break;
        case IROperandKind::LOCAL_SLOT:
            kindStr = "s";
            break;
        case IROperandKind::UPVALUE_IDX:
            kindStr = "u";
            break;
        case IROperandKind::FIELD_NAME:
            kindStr = "f";
            break;
        case IROperandKind::FUNC_NAME:
            kindStr = "fn";
            break;
        case IROperandKind::IMM_UINT:
            kindStr = "#";
            break;
        default:
            kindStr = "?";
            break; // Bug-6: 缺 default 防新增枚举静默走 ?
        }
        oss << " " << kindStr << operand.index;
    }
    if (instr.line > 0)
        oss << "  ; line " << instr.line;
    return oss.str();
}

std::string IRToString(const IRFunction& ir) {
    std::ostringstream oss;
    oss << "IRFunction \"" << ir.name << "\" {\n";
    oss << "  constants: " << ir.constants.size() << "\n";
    oss << "  globals: " << ir.globalNames.size() << "\n";
    oss << "  vregs: " << ir.nextVReg << "\n";
    oss << "  labels: " << ir.nextLabel << "\n";
    oss << "  blocks: " << ir.blocks.size() << "\n\n";

    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        const auto& block = ir.blocks[bi];
        oss << "  BB" << bi << " (label=" << block.labelIndex << "):\n";
        for (const auto& instr : block.instructions) {
            oss << "    " << formatIRInstruction(instr) << "\n";
        }
        oss << "\n";
    }
    oss << "}\n";
    return oss.str();
}

// ============================================================
// IR 优化 Pass 实现（方向二）
// ============================================================
// 各优化 pass 共同依赖的 IR 表示约定（理解下方算法的前提）：
//   · IR 以基本块（IRBlock::instructions）为单位，函数 IRFunction 持有若干基本块；
//     当前 pass 多为"块内"局部分析（不跨块传播），仅保守地在块边界重置摘要信息。
//   · 指令 IRInstruction 形如 op + 操作数列表 operands。操作数约定：
//       - 操作数[0]（首操作数）通常是"目标"dest vreg（纯计算/加载类指令）；
//         STORE_*/JUMP 等指令首操作数可能是 slot/label 而非 vreg。
//       - 其余操作数是源：VIRTUAL=SSA 风格虚拟寄存器(vreg，allocVReg 单调递增不复用)，
//         CONSTANT=常量池索引，LOCAL_SLOT=局部槽，LABEL=跳转标签，GLOBAL_NAME/GLOBAL_SLOT=全局。
//   · vreg 的 SSA 不变量：每个 vreg 应只被"定义"一次（见 loopUnrollingPass 的 vreg 重命名修复）。
//     多数 pass 通过"建立 vreg→定义/值"映射并替换引用"来化简，而非修改指令语义。
//   · 折叠/化简必须保守：带运行时副作用的指令（除零、溢出、LOAD_GLOBAL 未定义、字符串拼接、
//     ADD 的 COW detach）不可被 DCE 随意删除，否则会抑制本应抛出的运行时错误（见 isPureCompute）。

namespace {

/// 辅助：判断指令是否为"常量加载"（LOAD_CONST/LOAD_NULL/LOAD_TRUE/LOAD_FALSE）
/// 若是，返回 true 并输出对应的常量值
bool isConstLoad(const IRInstruction& instr, const IRFunction& ir, Value& outVal) {
    switch (instr.op) {
    case IROp::LOAD_CONST:
        if (instr.operands.size() < 2)
            return false;
        if (instr.operands[1].index >= ir.constants.size())
            return false;
        outVal = ir.constants[instr.operands[1].index];
        return true;
    case IROp::LOAD_NULL:
        outVal = Value::nullValue();
        return true;
    case IROp::LOAD_TRUE:
        outVal = Value(true);
        return true;
    case IROp::LOAD_FALSE:
        outVal = Value(false);
        return true;
    default:
        return false;
    }
}

/// 辅助：判断指令是否为纯计算（无副作用，仅定义 dest vreg）
bool isPureCompute(IROp op) {
    switch (op) {
    case IROp::LOAD_CONST:
    case IROp::LOAD_NULL:
    case IROp::LOAD_TRUE:
    case IROp::LOAD_FALSE:
    case IROp::LOAD_LOCAL:
    // #23 fix: LOAD_GLOBAL/LOAD_UPVALUE 不列为纯计算——它们在运行时会检查
    // 变量是否定义并可能抛"未定义的变量"错误。若 DCE 删除 dest 未引用的加载，
    // 会抑制该错误（如 `undefinedVar;` 表达式语句本应报错却被静默删除）。
    // LOAD_LOCAL 可保留：局部变量由编译期静态绑定，不存在运行时未定义。
    // BUG-IR-DCE-1 fix: 算术指令 ADD/SUB/MUL/DIV/MOD/NEGATE 不列为纯计算——
    // 它们在运行时可能触发副作用：整数溢出检查、除零错误、字符串拼接（ADD）。
    // 若 DCE 删除 dest 未引用的算术指令（如 `1/0;` 表达式语句），会抑制运行时错误。
    // 比较指令 EQ/NEQ/LT/GT/LTE/GTE 与逻辑 NOT 可保留：MiniLang 语义中无副作用。
    case IROp::NOT:
    case IROp::EQ:
    case IROp::NEQ:
    case IROp::LT:
    case IROp::GT:
    case IROp::LTE:
    case IROp::GTE:
    case IROp::DUP:
        return true;
    default:
        return false;
    }
}

/// 辅助：对两个 Value 执行算术运算，成功返回 true
bool foldArith(IROp op, const Value& lhs, const Value& rhs, Value& result) {
    // 整数算术
    if (lhs.isInt() && rhs.isInt()) {
        int64_t a = lhs.intVal(), b = rhs.intVal();
        switch (op) {
        // #14 fix: 整数 ADD/SUB/MUL 折叠需检查溢出（与 RegisterVM::executeArith 一致），
        // 溢出时不折叠（返回 false），留待运行时按 OverflowCheck 路径报错。
        // 原实现直接 a+b/a-b/a*b 在溢出时是 UB（int64_t 有符号溢出）。
        case IROp::ADD:
            if (OverflowCheck::addOverflow(a, b))
                return false;
            result = Value(a + b);
            return true;
        case IROp::SUB:
            if (OverflowCheck::subOverflow(a, b))
                return false;
            result = Value(a - b);
            return true;
        case IROp::MUL:
            if (OverflowCheck::mulOverflow(a, b))
                return false;
            result = Value(a * b);
            return true;
        case IROp::DIV:
            if (b == 0)
                return false; // 除零不折叠，留待运行时报错
            if (OverflowCheck::divOverflow(a, b))
                return false; // INT64_MIN / -1
            result = Value(a / b);
            return true;
        case IROp::MOD:
            if (b == 0)
                return false;
            if (OverflowCheck::modOverflow(a, b))
                return false; // INT64_MIN % -1
            result = Value(a % b);
            return true;
        default:
            return false;
        }
    }
    // 浮点算术（至少一方为 float）
    if (lhs.isNumber() && rhs.isNumber()) {
        double a = lhs.toDouble(), b = rhs.toDouble();
        switch (op) {
        case IROp::ADD:
            result = Value(a + b);
            return true;
        case IROp::SUB:
            result = Value(a - b);
            return true;
        case IROp::MUL:
            result = Value(a * b);
            return true;
        case IROp::DIV:
            if (b == 0.0)
                return false;
            result = Value(a / b);
            return true;
        case IROp::MOD:
            if (b == 0.0)
                return false;
            // C++ fmod
            result = Value(std::fmod(a, b));
            return true;
        default:
            return false;
        }
    }
    return false;
}

/// 辅助：对两个 Value 执行比较运算，成功返回 true
bool foldCompare(IROp op, const Value& lhs, const Value& rhs, Value& result) {
    // 同类型比较
    if (lhs.isInt() && rhs.isInt()) {
        int64_t a = lhs.intVal(), b = rhs.intVal();
        switch (op) {
        case IROp::EQ:
            result = Value(a == b);
            return true;
        case IROp::NEQ:
            result = Value(a != b);
            return true;
        case IROp::LT:
            result = Value(a < b);
            return true;
        case IROp::GT:
            result = Value(a > b);
            return true;
        case IROp::LTE:
            result = Value(a <= b);
            return true;
        case IROp::GTE:
            result = Value(a >= b);
            return true;
        default:
            return false;
        }
    }
    if (lhs.isNumber() && rhs.isNumber()) {
        double a = lhs.toDouble(), b = rhs.toDouble();
        switch (op) {
        case IROp::EQ:
            result = Value(a == b);
            return true;
        case IROp::NEQ:
            result = Value(a != b);
            return true;
        case IROp::LT:
            result = Value(a < b);
            return true;
        case IROp::GT:
            result = Value(a > b);
            return true;
        case IROp::LTE:
            result = Value(a <= b);
            return true;
        case IROp::GTE:
            result = Value(a >= b);
            return true;
        default:
            return false;
        }
    }
    if (lhs.isBool() && rhs.isBool()) {
        bool a = lhs.boolVal(), b = rhs.boolVal();
        switch (op) {
        case IROp::EQ:
            result = Value(a == b);
            return true;
        case IROp::NEQ:
            result = Value(a != b);
            return true;
        default:
            return false; // bool 不支持 < > <= >=
        }
    }
    if (lhs.isNull() && rhs.isNull()) {
        switch (op) {
        case IROp::EQ:
            result = Value(true);
            return true;
        case IROp::NEQ:
            result = Value(false);
            return true;
        default:
            return false;
        }
    }
    return false;
}

/// 辅助：对 Value 执行一元运算
bool foldUnary(IROp op, const Value& operand, Value& result) {
    switch (op) {
    case IROp::NEGATE:
        // #14 fix: -INT64_MIN 是 UB，需检查（与 RegisterVM::executeArith 一致）。
        if (operand.isInt()) {
            if (OverflowCheck::negateOverflow(operand.intVal()))
                return false;
            result = Value(-operand.intVal());
            return true;
        }
        if (operand.isFloat()) {
            result = Value(-operand.floatVal());
            return true;
        }
        return false;
    case IROp::NOT:
        if (operand.isBool()) {
            result = Value(!operand.boolVal());
            return true;
        }
        return false;
    default:
        return false;
    }
}

} // anonymous namespace

// ---- 常量折叠 ----
bool constantFoldingPass(IRFunction& ir) {
    bool modified = false;
    // 建立 vreg → 定义指令的映射（仅纯计算指令，且 dest 为首操作数）
    // vregDefs[v] = {blockIdx, instrIdx}
    std::unordered_map<uint32_t, std::pair<size_t, size_t>> vregDefs;
    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        for (size_t ii = 0; ii < ir.blocks[bi].instructions.size(); ++ii) {
            const auto& instr = ir.blocks[bi].instructions[ii];
            if (instr.operands.empty())
                continue;
            if (instr.operands[0].kind == IROperandKind::VIRTUAL) {
                vregDefs[instr.operands[0].index] = {bi, ii};
            }
        }
    }

    // 遍历所有算术/比较/一元指令，若操作数均为常量加载则折叠
    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        for (size_t ii = 0; ii < ir.blocks[bi].instructions.size(); ++ii) {
            auto& instr = ir.blocks[bi].instructions[ii];
            // 二元运算：ADD/SUB/MUL/DIV/MOD/EQ/NEQ/LT/GT/LTE/GTE
            if (instr.operands.size() == 3) {
                IROp op = instr.op;
                bool isArith =
                    (op == IROp::ADD || op == IROp::SUB || op == IROp::MUL || op == IROp::DIV || op == IROp::MOD);
                bool isCompare = (op == IROp::EQ || op == IROp::NEQ || op == IROp::LT || op == IROp::GT ||
                                  op == IROp::LTE || op == IROp::GTE);
                if (!isArith && !isCompare)
                    continue;

                // dest, src1, src2
                IROperand dest = instr.operands[0];
                IROperand src1 = instr.operands[1];
                IROperand src2 = instr.operands[2];
                if (src1.kind != IROperandKind::VIRTUAL || src2.kind != IROperandKind::VIRTUAL)
                    continue;

                // 查找 src1/src2 的定义
                auto it1 = vregDefs.find(src1.index);
                auto it2 = vregDefs.find(src2.index);
                if (it1 == vregDefs.end() || it2 == vregDefs.end())
                    continue;

                // 获取定义指令
                const auto& def1 = ir.blocks[it1->second.first].instructions[it1->second.second];
                const auto& def2 = ir.blocks[it2->second.first].instructions[it2->second.second];

                Value v1, v2;
                if (!isConstLoad(def1, ir, v1) || !isConstLoad(def2, ir, v2))
                    continue;

                // 尝试折叠
                Value folded;
                bool ok = isArith ? foldArith(op, v1, v2, folded) : foldCompare(op, v1, v2, folded);
                if (!ok)
                    continue;

                // 替换：原指令改为 LOAD_CONST dest, constIdx
                uint32_t constIdx = ir.addConstant(folded);
                instr.op = IROp::LOAD_CONST;
                instr.operands = {dest, IROperand::constant(constIdx)};
                // 更新 vregDefs 中 dest 的定义
                vregDefs[dest.index] = {bi, ii};
                modified = true;
            }
            // 一元运算：NEGATE/NOT
            else if (instr.operands.size() == 2) {
                IROp op = instr.op;
                if (op != IROp::NEGATE && op != IROp::NOT)
                    continue;

                IROperand dest = instr.operands[0];
                IROperand src = instr.operands[1];
                if (src.kind != IROperandKind::VIRTUAL)
                    continue;

                auto it = vregDefs.find(src.index);
                if (it == vregDefs.end())
                    continue;
                const auto& def = ir.blocks[it->second.first].instructions[it->second.second];

                Value v;
                if (!isConstLoad(def, ir, v))
                    continue;

                Value folded;
                if (!foldUnary(op, v, folded))
                    continue;

                uint32_t constIdx = ir.addConstant(folded);
                instr.op = IROp::LOAD_CONST;
                instr.operands = {dest, IROperand::constant(constIdx)};
                vregDefs[dest.index] = {bi, ii};
                modified = true;
            }
        }
    }
    return modified;
}

// ---- 死代码消除 ----
bool deadCodeEliminationPass(IRFunction& ir) {
    // 第一遍：收集所有被引用的 vreg
    std::unordered_set<uint32_t> usedVRegs;
    for (const auto& block : ir.blocks) {
        for (const auto& instr : block.instructions) {
            // dest（首操作数）不算"被引用"，只有后续指令引用才算
            for (size_t i = 0; i < instr.operands.size(); ++i) {
                // 对于纯计算指令，首操作数是 dest，跳过
                // 对于 STORE_* 等指令，首操作数可能是 slot/uv_idx 而非 vreg
                if (i == 0 && isPureCompute(instr.op))
                    continue;
                if (instr.operands[i].kind == IROperandKind::VIRTUAL) {
                    usedVRegs.insert(instr.operands[i].index);
                }
            }
        }
    }

    // 第二遍：删除 dest 未被引用的纯计算指令
    bool modified = false;
    for (auto& block : ir.blocks) {
        std::vector<IRInstruction> kept;
        kept.reserve(block.instructions.size());
        for (auto& instr : block.instructions) {
            if (isPureCompute(instr.op) && !instr.operands.empty() &&
                instr.operands[0].kind == IROperandKind::VIRTUAL) {
                uint32_t destVReg = instr.operands[0].index;
                if (usedVRegs.find(destVReg) == usedVRegs.end()) {
                    // dest 从未被引用，删除此指令
                    modified = true;
                    continue;
                }
            }
            kept.push_back(std::move(instr));
        }
        block.instructions = std::move(kept);
    }
    return modified;
}

// ---- 复制传播 ----
// PERF-15: 寄存器式 lowering 已启用，复制传播可以安全使用。
// 规则：
//   1. 记录每个 vreg 的"定义"：LOAD_CONST c → 该 vreg 等价于 constant c
//   2. 后续指令引用该 vreg 时，直接用 constant 替换
//   3. 跨 STORE_LOCAL/STORE_GLOBAL/STORE_UPVALUE 后不再传播（变量可能被修改）
//   4. 在基本块边界重置（保守策略，避免跨块分析复杂性）
// 注意：复制传播替换操作数后，原 LOAD_CONST 指令的 dest 变为无引用，
//       由后续的死代码消除 pass 删除。
bool copyPropagationPass(IRFunction& ir) {
    bool modified = false;
    // vreg → (常量索引, 类型) 映射
    // 类型标记区分 LOAD_CONST / LOAD_NULL / LOAD_TRUE / LOAD_FALSE
    enum class ConstKind { NONE, CONST, NULL_, TRUE_, FALSE_ };
    struct ConstDef {
        ConstKind kind = ConstKind::NONE;
        uint32_t constIdx = 0;
    };
    std::unordered_map<uint32_t, ConstDef> vregConst;

    for (auto& block : ir.blocks) {
        vregConst.clear(); // 基本块边界重置
        for (auto& instr : block.instructions) {
            // 替换操作数中的 vreg 为等价常量
            for (size_t i = 0; i < instr.operands.size(); ++i) {
                if (instr.operands[i].kind == IROperandKind::VIRTUAL) {
                    auto it = vregConst.find(instr.operands[i].index);
                    if (it != vregConst.end() && it->second.kind != ConstKind::NONE) {
                        // 替换为等价常量
                        switch (it->second.kind) {
                        case ConstKind::CONST:
                            instr.operands[i] = IROperand::constant(it->second.constIdx);
                            modified = true;
                            break;
                        case ConstKind::NULL_:
                            // 不替换为 LOAD_NULL 的操作数（需要新增指令）
                            // 保持 vreg 引用，由 DCE 删除 LOAD_NULL
                            break;
                        case ConstKind::TRUE_:
                        case ConstKind::FALSE_:
                            // 同上，不替换
                            break;
                        case ConstKind::NONE:
                            break;
                        // Bug-6 同型修复：枚举扩展时静默跳过替换，留下不一致状态
                        default:
                            // P1-2 fix: assert 在 Release 构建中被剥离，改为同时 Logger::Error 留痕。
                            Logger::Error("copyPropagationPass: 未处理的 ConstKind 枚举值", "IR");
                            assert(false && "copyPropagationPass: 未处理的 ConstKind 枚举值");
                            break;
                        }
                    }
                }
            }

            // 记录本指令的 dest 定义
            if (!instr.operands.empty() && instr.operands[0].kind == IROperandKind::VIRTUAL) {
                uint32_t destVReg = instr.operands[0].index;
                switch (instr.op) {
                case IROp::LOAD_CONST:
                    if (instr.operands.size() >= 2) {
                        vregConst[destVReg] = {ConstKind::CONST, instr.operands[1].index};
                    }
                    break;
                case IROp::LOAD_NULL:
                    vregConst[destVReg] = {ConstKind::NULL_, 0};
                    break;
                case IROp::LOAD_TRUE:
                    vregConst[destVReg] = {ConstKind::TRUE_, 0};
                    break;
                case IROp::LOAD_FALSE:
                    vregConst[destVReg] = {ConstKind::FALSE_, 0};
                    break;
                default:
                    // 其他指令定义 dest，清除等价关系
                    vregConst.erase(destVReg);
                    break;
                }
            }

            // STORE_* 指令可能修改外部状态，清除所有等价关系（保守策略）
            // 实际上只需清除被修改变量的等价关系，但简化处理
            if (instr.op == IROp::STORE_LOCAL || instr.op == IROp::STORE_GLOBAL || instr.op == IROp::STORE_UPVALUE ||
                instr.op == IROp::DEFINE_GLOBAL) {
                // 清除局部变量的等价关系（简化：不清除，因为 vreg 是 SSA 风格）
                // 实际上 vreg 不会被 STORE 修改，只有 LOCAL_SLOT 概念会
            }
        }
    }
    return modified;
}

// ---- 公共子表达式消除（CSE，局部 — 基本块内）----
// C4: 在每个基本块内对纯计算指令做值编号，相同表达式 dest 复用。
// 与 BUG-IR-DCE-1 的关键差异：本 pass 不删除指令，仅替换后续引用。
// 算术指令（含副作用：除零/溢出）即使 dest 已被替换，仍保留原指令执行。
bool commonSubexpressionEliminationPass(IRFunction& ir) {
    bool modified = false;
    // 值编号 key：将 (op, operands) 编码为字符串
    auto encodeKey = [](const IRInstruction& instr) -> std::string {
        std::string key;
        key.push_back(static_cast<char>(instr.op));
        key.push_back('|');
        for (const auto& op : instr.operands) {
            key.push_back(static_cast<char>(op.kind));
            key.push_back(':');
            key.append(std::to_string(op.index));
            key.push_back(',');
        }
        return key;
    };
    // 判断指令是否可参与 CSE（纯计算 + 有 dest vreg）
    auto isCSEable = [](const IRInstruction& instr) -> bool {
        if (instr.operands.empty())
            return false;
        if (instr.operands[0].kind != IROperandKind::VIRTUAL)
            return false;
        // 包含算术（保留原指令执行副作用，仅替换 dest 引用）
        switch (instr.op) {
        case IROp::ADD:
        case IROp::SUB:
        case IROp::MUL:
        case IROp::DIV:
        case IROp::MOD:
        case IROp::NEGATE:
        case IROp::EQ:
        case IROp::NEQ:
        case IROp::LT:
        case IROp::GT:
        case IROp::LTE:
        case IROp::GTE:
        case IROp::NOT:
        case IROp::DUP:
            return true;
        default:
            return false;
        }
    };

    for (auto& block : ir.blocks) {
        // 块内值编号表：key → 已记录的 dest vreg
        std::unordered_map<std::string, uint32_t> valueMap;
        // 替换映射：被替换的 dest vreg → 替代 vreg
        std::unordered_map<uint32_t, uint32_t> replacements;
        // 待标记为"dest 已替换"的指令索引（保留指令以触发副作用，但 dest 不再被引用）
        // 实际操作：在第二轮遍历中替换后续指令对该 dest 的引用

        // 第一遍：构建替换映射
        for (auto& instr : block.instructions) {
            // 先按替换映射更新本指令的 operands（处理级联替换）
            for (auto& op : instr.operands) {
                if (op.kind == IROperandKind::VIRTUAL) {
                    auto it = replacements.find(op.index);
                    if (it != replacements.end()) {
                        op.index = it->second;
                        modified = true;
                    }
                }
            }

            if (!isCSEable(instr))
                continue;
            // 二元/一元运算的 dest vreg
            uint32_t destVReg = instr.operands[0].index;
            std::string key = encodeKey(instr);
            auto it = valueMap.find(key);
            if (it != valueMap.end()) {
                // 已存在相同表达式：本指令 dest 引用替换为已存在的 vreg
                replacements[destVReg] = it->second;
                // 不删除本指令（保留副作用），后续对该 dest 的引用会在循环开头被替换
            } else {
                valueMap[key] = destVReg;
            }
        }
    }
    return modified;
}

// ---- 循环展开（保守策略 — 仅展开常量边界的小循环）----
// C4: 检测 `for (var i = 0; i < N; i = i + 1) { body }` 模式，N 为小整常量时展开 N 次。
// 实现：扫描 IR 指令序列，识别循环模式（LABEL start; cond; JUMP_IF_FALSE exit; POP; body;
// counter update; JUMP start; LABEL exit; POP），将 body 复制 N 份替换原循环。
bool loopUnrollingPass(IRFunction& ir) {
    constexpr int kMaxUnrollCount = 4;        // 最大展开次数（避免代码膨胀）
    constexpr size_t kMaxUnrollBodySize = 20; // body 指令数上限
    bool modified = false;

    for (auto& block : ir.blocks) {
        const auto& instrs = block.instructions;
        if (instrs.size() < 12)
            continue; // 最小循环长度估算

        bool unrolled = false;
        for (size_t i = 0; i + 11 < instrs.size(); ++i) {
            // 模式检测：
            // [i+0] LABEL L1
            // [i+1] LOAD_LOCAL slot        (i)
            // [i+2] LOAD_CONST N
            // [i+3] LT dest, i, N
            // [i+4] JUMP_IF_FALSE dest, L_exit
            // [i+5] POP
            // [i+6..i+6+bodySize-1] body
            // [i+6+bodySize] LOAD_LOCAL slot  (i)
            // [i+6+bodySize+1] LOAD_CONST 1
            // [i+6+bodySize+2] ADD dest, i, 1
            // [i+6+bodySize+3] STORE_LOCAL slot, dest
            // [i+6+bodySize+4] JUMP L1
            // [i+6+bodySize+5] LABEL L_exit
            // [i+6+bodySize+6] POP

            const auto& labelStart = instrs[i];
            if (labelStart.op != IROp::LABEL)
                continue;
            uint32_t startLabel = labelStart.operands[0].index;

            const auto& loadI1 = instrs[i + 1];
            if (loadI1.op != IROp::LOAD_LOCAL || loadI1.operands.size() < 2)
                continue;
            if (loadI1.operands[1].kind != IROperandKind::LOCAL_SLOT)
                continue;
            uint32_t counterSlot = loadI1.operands[1].index;

            const auto& loadN = instrs[i + 2];
            if (loadN.op != IROp::LOAD_CONST || loadN.operands.size() < 2)
                continue;
            if (loadN.operands[1].kind != IROperandKind::CONSTANT)
                continue;
            uint32_t nConstIdx = loadN.operands[1].index;
            if (nConstIdx >= ir.constants.size())
                continue;
            const Value& nVal = ir.constants[nConstIdx];
            if (!nVal.isInt())
                continue;
            int64_t nInt = nVal.intVal();
            if (nInt < 1 || nInt > kMaxUnrollCount)
                continue;

            const auto& ltInstr = instrs[i + 3];
            if (ltInstr.op != IROp::LT || ltInstr.operands.size() < 3)
                continue;

            const auto& jumpFalse = instrs[i + 4];
            if (jumpFalse.op != IROp::JUMP_IF_FALSE || jumpFalse.operands.size() < 2)
                continue;
            uint32_t exitLabel = jumpFalse.operands[1].index;

            const auto& pop1 = instrs[i + 5];
            if (pop1.op != IROp::POP)
                continue;

            // 在剩余指令中找模式尾部：LOAD_LOCAL slot; LOAD_CONST 1; ADD; STORE_LOCAL slot; JUMP L1; LABEL L_exit; POP
            size_t tailStart = i + 6;
            // bodySize 上限保护：从 tailStart 开始最多扫 kMaxUnrollBodySize 条
            size_t bodyEnd = tailStart;
            bool foundTail = false;
            for (size_t probe = 0; probe < kMaxUnrollBodySize && bodyEnd + 7 <= instrs.size(); ++probe, ++bodyEnd) {
                const auto& t0 = instrs[bodyEnd];
                const auto& t1 = instrs[bodyEnd + 1];
                const auto& t2 = instrs[bodyEnd + 2];
                const auto& t3 = instrs[bodyEnd + 3];
                const auto& t4 = instrs[bodyEnd + 4];
                const auto& t5 = instrs[bodyEnd + 5];
                const auto& t6 = instrs[bodyEnd + 6];
                if (t0.op != IROp::LOAD_LOCAL || t0.operands.size() < 2)
                    continue;
                if (t0.operands[1].kind != IROperandKind::LOCAL_SLOT)
                    continue;
                if (t0.operands[1].index != counterSlot)
                    continue;
                if (t1.op != IROp::LOAD_CONST)
                    continue;
                if (t2.op != IROp::ADD)
                    continue;
                if (t3.op != IROp::STORE_LOCAL || t3.operands.size() < 2)
                    continue;
                if (t3.operands[0].kind != IROperandKind::LOCAL_SLOT)
                    continue;
                if (t3.operands[0].index != counterSlot)
                    continue;
                if (t4.op != IROp::JUMP || t4.operands.size() < 1)
                    continue;
                if (t4.operands[0].kind != IROperandKind::LABEL)
                    continue;
                if (t4.operands[0].index != startLabel)
                    continue;
                if (t5.op != IROp::LABEL || t5.operands.size() < 1)
                    continue;
                if (t5.operands[0].index != exitLabel)
                    continue;
                if (t6.op != IROp::POP)
                    continue;
                foundTail = true;
                break;
            }
            if (!foundTail)
                continue;

            // body 范围：[tailStart, bodyEnd)
            size_t bodySize = bodyEnd - tailStart;
            if (bodySize == 0 || bodySize > kMaxUnrollBodySize)
                continue;

            // body 必须不包含 break/continue/return/throw
            bool bodySafe = true;
            for (size_t j = tailStart; j < bodyEnd; ++j) {
                IROp op = instrs[j].op;
                if (op == IROp::RETURN || op == IROp::RETURN_NULL || op == IROp::THROW || op == IROp::JUMP) {
                    bodySafe = false;
                    break;
                }
            }
            if (!bodySafe)
                continue;

            // 展开：生成 nInt 份 body 副本 + 计数器初始化（LOAD_CONST 0; STORE_LOCAL slot）
            // 注意：原模式不包含 i 的初始化（在循环外），展开时需补上 i=0
            //
            // BUG-IR-OPT-AUDIT-5 fix: vreg 重命名。
            // 原实现直接复制 body 指令（含 operands）N 次，导致同一 vreg 在多个迭代中
            // 被重复定义（每个迭代的 ADD dest 都用同一个 vreg），违反 IR 的 SSA-like
            // 不变量（每个 vreg 应只被赋值一次）。后续 CSE/DCE 等优化 pass 会因重复
            // 定义而误判（如 DCE 看到 dest 被多个指令定义时无法正确删除无引用指令，
            // CSE 的 valueMap 会因 dest vreg 已被覆盖而误命中）。
            // 修复：对 iter >= 1 的副本，收集 body 内定义的 vreg（作为纯计算指令的 dest
            // 或 LOAD_LOCAL/LOAD_CONST 的 dest），分配 fresh vreg 并重命名副本中所有引用
            // （包括 dest 和 src），保证每个迭代的 vreg 唯一。
            std::vector<IRInstruction> newInstrs;
            newInstrs.reserve(static_cast<size_t>(nInt) * bodySize + 4);
            // 补 i=0 初始化
            uint32_t zeroConst = ir.addConstant(Value(static_cast<int64_t>(0)));
            newInstrs.emplace_back(
                IROp::LOAD_CONST,
                std::vector<IROperand>{IROperand::vreg(ir.nextVReg++), IROperand::constant(zeroConst)},
                labelStart.line);
            newInstrs.emplace_back(
                IROp::STORE_LOCAL,
                std::vector<IROperand>{IROperand::local(counterSlot), IROperand::vreg(ir.nextVReg - 1)},
                labelStart.line);
            // 收集 body 内定义的 vreg（dest 为 VIRTUAL 的指令：纯计算 / LOAD_LOCAL / LOAD_CONST 等）
            std::unordered_set<uint32_t> bodyDefinedVRegs;
            for (size_t j = tailStart; j < bodyEnd; ++j) {
                const auto& instr = instrs[j];
                if (instr.operands.empty())
                    continue;
                if (instr.operands[0].kind == IROperandKind::VIRTUAL) {
                    bodyDefinedVRegs.insert(instr.operands[0].index);
                }
            }
            // 生成 nInt 份 body
            for (int64_t iter = 0; iter < nInt; ++iter) {
                // BUG-IR-OPT-AUDIT-5: 为本次迭代构建 vreg 重命名表
                // iter 0 用原 vreg（与原 body 一致，保持 SSA 单赋值），
                // iter >= 1 分配 fresh vreg 替换 body 内定义的 vreg，避免重复赋值。
                std::unordered_map<uint32_t, uint32_t> vregRemap;
                if (iter > 0) {
                    for (uint32_t v : bodyDefinedVRegs) {
                        vregRemap[v] = ir.nextVReg++;
                    }
                }
                for (size_t j = tailStart; j < bodyEnd; ++j) {
                    IRInstruction copy = instrs[j]; // 复制指令（含 operands）
                    // 重命名所有 VIRTUAL 操作数（dest 和 src 都需重命名）
                    for (auto& op : copy.operands) {
                        if (op.kind == IROperandKind::VIRTUAL) {
                            auto it = vregRemap.find(op.index);
                            if (it != vregRemap.end()) {
                                op.index = it->second;
                            }
                        }
                    }
                    newInstrs.push_back(std::move(copy));
                }
                // 计数器递增：LOAD_LOCAL slot; LOAD_CONST 1; ADD; STORE_LOCAL slot
                uint32_t oneConst = ir.addConstant(Value(static_cast<int64_t>(1)));
                uint32_t iReg = ir.nextVReg++;
                uint32_t oneReg = ir.nextVReg++;
                uint32_t sumReg = ir.nextVReg++;
                newInstrs.emplace_back(IROp::LOAD_LOCAL,
                                       std::vector<IROperand>{IROperand::vreg(iReg), IROperand::local(counterSlot)},
                                       instrs[bodyEnd].line);
                newInstrs.emplace_back(IROp::LOAD_CONST,
                                       std::vector<IROperand>{IROperand::vreg(oneReg), IROperand::constant(oneConst)},
                                       instrs[bodyEnd].line);
                newInstrs.emplace_back(
                    IROp::ADD,
                    std::vector<IROperand>{IROperand::vreg(sumReg), IROperand::vreg(iReg), IROperand::vreg(oneReg)},
                    instrs[bodyEnd].line);
                newInstrs.emplace_back(IROp::STORE_LOCAL,
                                       std::vector<IROperand>{IROperand::local(counterSlot), IROperand::vreg(sumReg)},
                                       instrs[bodyEnd].line);
            }

            // 替换原循环结构 [i, bodyEnd+7) 为 newInstrs
            auto& blockInstrs = block.instructions;
            size_t replaceEnd = bodyEnd + 7;
            blockInstrs.erase(blockInstrs.begin() + i, blockInstrs.begin() + replaceEnd);
            blockInstrs.insert(blockInstrs.begin() + i, newInstrs.begin(), newInstrs.end());
            modified = true;
            unrolled = true;
            break; // 本块已修改，跳出内层循环重新扫描
        }
        if (unrolled) {
            // 块已修改，外层 for 会继续扫描后续块
        }
    }
    return modified;
}

// ---- 运行全部优化 pass ----
// BUG-IR-OPT-AUDIT-1/2/3/4 fix: IR 优化日志统一
// - 统一 source tag 为 "IR-Opt"（原 "IR" / "IR-Optimize" 混用）
// - 每个 pass 在 modified=true 时发射 LOG_DEBUG（原 4/5 pass 完全静默）
// - 统一术语：常量折叠 / 复制传播 / CSE / 循环展开 / DCE（原中英混用）
// - 汇总日志改用 LOG_INFO + 旗标矩阵（原仅报告启用旗标，缺失修改事实）
bool optimizeIR(IRFunction& ir, bool enableCopyPropagation, bool enableDCE, bool enableCSE, bool enableLoopUnroll) {
    bool modified = false;
    // PERF-15: 常量折叠 → [复制传播] → [CSE] → [循环展开] → 死代码消除
    // 复制传播仅在寄存器式后端启用（栈式后端删除 LOAD_CONST 会导致栈下溢）
    // BUG-IR-DCE-2 fix: DCE 与复制传播控制解耦——DCE 通过 enableDCE 独立控制，
    // 避免禁用复制传播时连带禁用 DCE（寄存器式后端复制传播暂禁但 DCE 仍可安全启用）。
    // C4: CSE 与循环展开默认禁用；详见 IR.h 的安全约束文档。
    //
    // BUG-IR-OPT-AUDIT-6 fix: 防御性检查——CSE 在栈式 VM 后端（enableDCE=false）不安全。
    // CSE 替换后续指令对 dest vreg 的引用，但保留原指令（不删除）；栈式后端 lowering
    // 仍 emit OP_ADD 等压栈指令，未被消费的栈值会残留 → 后续 OP_POP 栈下溢。
    // 详见 IR.h:720-723 的安全约束文档。此处自动禁用并告警，防止误用。
    if (enableCSE && !enableDCE) {
        LOG_ERROR("optimizeIR: CSE 在栈式 VM 后端（enableDCE=false）不安全"
                  "（栈残留 → 后续 OP_POP 栈下溢），已自动禁用 CSE",
                  "IR-Opt");
        enableCSE = false;
    }
    // 同理：复制传播在栈式后端也不安全（删除 LOAD_CONST 会导致栈下溢），
    // 但寄存器式后端的复制传播因 RegisterBytecodeBackend 不检查操作数 kind 也暂禁（见 IR.h:720-723）。
    // 此处仅做 CSE 的强校验；复制传播的默认禁用由调用方控制（Compiler.cpp:197/295 均传 false）。

    // BUG-IR-OPT-AUDIT-1: 启动日志记录启用旗标
    LOG_DEBUG("optimizeIR: 启用旗标 copyProp=" + std::string(enableCopyPropagation ? "Y" : "N") +
                  " dce=" + std::string(enableDCE ? "Y" : "N") + " cse=" + std::string(enableCSE ? "Y" : "N") +
                  " loopUnroll=" + std::string(enableLoopUnroll ? "Y" : "N"),
              "IR-Opt");

    // 多轮迭代直到收敛（最多 3 轮，避免无限循环）
    int totalRounds = 0;
    for (int round = 0; round < 3; ++round) {
        bool m1 = constantFoldingPass(ir);
        // BUG-IR-OPT-AUDIT-2: 每个 pass 修改时发射 LOG_DEBUG（原 4/5 pass 静默）
        if (m1)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " 常量折叠 修改", "IR-Opt");
        bool m2 = enableCopyPropagation ? copyPropagationPass(ir) : false;
        if (m2)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " 复制传播 修改", "IR-Opt");
        bool m3 = enableCSE ? commonSubexpressionEliminationPass(ir) : false;
        if (m3)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " CSE 修改", "IR-Opt");
        bool m4 = enableLoopUnroll ? loopUnrollingPass(ir) : false;
        if (m4)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " 循环展开 修改", "IR-Opt");
        // DCE 仅在寄存器式后端启用（enableDCE=true）。
        // 栈式后端中 POP 的 operands 为空，DCE 无法看到 POP 对 vreg 的消费关系，
        // 会删除仅被 POP 消费的 LOAD_CONST 等纯计算指令，导致栈式 VM OP_POP 栈下溢。
        bool m5 = enableDCE ? deadCodeEliminationPass(ir) : false;
        if (m5)
            LOG_DEBUG("optimizeIR: round " + std::to_string(round) + " DCE 修改", "IR-Opt");
        modified = modified || m1 || m2 || m3 || m4 || m5;
        ++totalRounds;
        if (!m1 && !m2 && !m3 && !m4 && !m5)
            break; // 收敛
    }
    // BUG-IR-OPT-AUDIT-3/4: 汇总日志统一术语 + 报告迭代轮数与最终规模
    if (modified) {
        // Perf-LazyLog: LOG_INFO 宏级别过滤后跳过字符串构造
        LOG_INFO("IR 优化完成: " + std::to_string(totalRounds) + " 轮迭代, " + std::to_string(ir.constants.size()) +
                     " 常量, " + std::to_string(ir.nextVReg) + " vreg" +
                     (enableCopyPropagation ? " [copyProp=on]" : "") + (enableCSE ? " [CSE=on]" : "") +
                     (enableLoopUnroll ? " [loopUnroll=on]" : "") + (enableDCE ? " [DCE=on]" : ""),
                 "IR-Opt");
    } else {
        LOG_DEBUG("IR 优化完成: 无修改 (rounds=" + std::to_string(totalRounds) + ")", "IR-Opt");
    }
    return modified;
}
