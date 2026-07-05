// ============================================================
// ModuleIsolation.cpp — BUG-AUDIT-MOD-2 完整统一：VM/IR 模块隔离
// ------------------------------------------------------------
// 实现细节见 ModuleIsolation.h 文件头注释。
// ============================================================

#include "ast/ModuleIsolation.h"
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace {

/// 提取顶层声明节点的 name（VarDecl/FunDecl/ClassDecl），非顶层声明返回空
std::string getTopLevelDeclName(const ASTNode* node) {
    if (!node) return "";
    switch (node->nodeType) {
    case NodeType::NODE_VAR_DECL:  return static_cast<const VarDecl*>(node)->name;
    case NodeType::NODE_FUN_DECL:  return static_cast<const FunDecl*>(node)->name;
    case NodeType::NODE_CLASS_DECL:return static_cast<const ClassDecl*>(node)->name;
    default: return "";
    }
}

} // namespace

// ============================================================
// 公共接口
// ============================================================

void ModuleTopLevelRenamer::rename(Block& moduleAst, const std::string& modulePath) {
    ModuleTopLevelRenamer renamer(modulePath);
    // 第 1 步：收集非导出顶层名 + 构建 renameMap
    renamer.collectNonExportTopLevelNames(moduleAst);
    if (renamer.renameMap_.empty()) return;  // 无非导出顶层名，跳过重写
    // 第 2 步：递归重命名声明 + 引用
    renamer.renameInBlock(&moduleAst);
}

// ============================================================
// 私有实现
// ============================================================

ModuleTopLevelRenamer::ModuleTopLevelRenamer(const std::string& modulePath)
    : modulePath_(modulePath) {
    // 模块顶层作用域（栈底）
    scopeStack_.emplace_back();
}

std::string ModuleTopLevelRenamer::pathHash(const std::string& modulePath) {
    // FNV-1a 32-bit hash，取低 32 位，输出 8 字符 hex
    uint32_t h = 2166136261u;
    for (char c : modulePath) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(8) << h;
    return oss.str();
}

void ModuleTopLevelRenamer::collectNonExportTopLevelNames(Block& moduleAst) {
    // 第 1 步：收集所有导出名（ExportStmt 包装的声明名）
    std::unordered_set<std::string> exportNames;
    for (const auto& stmt : moduleAst.statements) {
        if (!stmt || stmt->nodeType != NodeType::NODE_EXPORT_STMT) continue;
        auto* exp = static_cast<ExportStmt*>(stmt.get());
        if (!exp->declaration) continue;
        std::string n = getTopLevelDeclName(exp->declaration.get());
        if (!n.empty()) exportNames.insert(n);
    }

    // 第 2 步：收集所有非导出顶层声明名，构建 renameMap
    const std::string prefix = "__mod_" + pathHash(modulePath_) + "__";
    for (const auto& stmt : moduleAst.statements) {
        if (!stmt) continue;
        // 跳过 ExportStmt（其内部声明是导出的，不重命名）
        if (stmt->nodeType == NodeType::NODE_EXPORT_STMT) continue;
        // 跳过 ImportStmt（嵌套模块有自己的命名空间）
        if (stmt->nodeType == NodeType::NODE_IMPORT_STMT) continue;
        std::string n = getTopLevelDeclName(stmt.get());
        if (n.empty()) continue;
        if (exportNames.count(n)) continue;  // 导出名不重命名
        renameMap_[n] = prefix + n;
        // 同时登记到模块顶层作用域
        scopeStack_.front().insert(n);
    }
    // 导出名也登记到模块顶层作用域（用于作用域分析，但不重命名）
    for (const auto& name : exportNames) {
        scopeStack_.front().insert(name);
    }
}

bool ModuleTopLevelRenamer::resolveToModuleTopLevel(const std::string& name) const {
    // 从内向外查找（栈顶到栈底），跳过栈底（模块顶层）
    if (scopeStack_.size() <= 1) return true;  // 仅在模块顶层作用域
    for (size_t i = scopeStack_.size() - 1; i >= 1; --i) {
        if (scopeStack_[i].count(name)) return false;  // 内层作用域定义了同名变量
    }
    return true;  // 所有内层作用域都未定义，解析到模块顶层
}

void ModuleTopLevelRenamer::renameInBlock(Block* node) {
    if (!node) return;
    // Block 进入新作用域（除非是模块顶层 Block，已在构造时压栈）
    bool isModuleTopLevel = (scopeStack_.size() == 1);
    if (!isModuleTopLevel) pushScope();
    for (const auto& stmt : node->statements) {
        renameInNode(stmt.get());
    }
    if (!isModuleTopLevel) popScope();
}

void ModuleTopLevelRenamer::renameInNode(ASTNode* node) {
    if (!node) return;
    switch (node->nodeType) {
    // ---- 顶层声明节点：重命名 name（如果在 renameMap 且当前是模块顶层）----
    case NodeType::NODE_VAR_DECL: {
        auto* n = static_cast<VarDecl*>(node);
        // 模块顶层的 VarDecl 才重命名；内层 VarDecl 是局部变量，登记到当前作用域
        if (scopeStack_.size() == 1 && renameMap_.count(n->name)) {
            n->name = renameMap_[n->name];
        }
        defineInCurrentScope(n->name);
        if (n->initializer) renameInNode(n->initializer.get());
        break;
    }
    case NodeType::NODE_FUN_DECL: {
        auto* n = static_cast<FunDecl*>(node);
        if (scopeStack_.size() == 1 && renameMap_.count(n->name)) {
            n->name = renameMap_[n->name];
        }
        defineInCurrentScope(n->name);
        // 函数体进入新作用域，参数登记到函数作用域
        pushScope();
        for (const auto& param : n->params) defineInCurrentScope(param);
        if (n->body) renameInNode(n->body.get());
        popScope();
        // 默认参数表达式在函数作用域外求值，但也需要重命名其中的引用
        for (const auto& dv : n->defaultValues) {
            if (dv) renameInNode(dv.get());
        }
        break;
    }
    case NodeType::NODE_CLASS_DECL: {
        auto* n = static_cast<ClassDecl*>(node);
        if (scopeStack_.size() == 1 && renameMap_.count(n->name)) {
            n->name = renameMap_[n->name];
        }
        // 父类名引用：如果父类是模块顶层非导出类，重命名
        if (!n->superClassName.empty() && resolveToModuleTopLevel(n->superClassName)
            && renameMap_.count(n->superClassName)) {
            n->superClassName = renameMap_[n->superClassName];
        }
        defineInCurrentScope(n->name);
        // 类成员（方法）进入新作用域，this 隐式绑定
        pushScope();
        defineInCurrentScope("this");
        for (const auto& m : n->members) {
            if (m) renameInNode(m.get());
        }
        popScope();
        break;
    }
    // ---- 引用节点：根据作用域分析决定是否重命名 ----
    case NodeType::NODE_VAR_REF: {
        auto* n = static_cast<VarRef*>(node);
        if (resolveToModuleTopLevel(n->name) && renameMap_.count(n->name)) {
            n->name = renameMap_[n->name];
        }
        break;
    }
    case NodeType::NODE_ASSIGNMENT: {
        auto* n = static_cast<Assignment*>(node);
        if (resolveToModuleTopLevel(n->name) && renameMap_.count(n->name)) {
            n->name = renameMap_[n->name];
        }
        if (n->value) renameInNode(n->value.get());
        break;
    }
    case NodeType::NODE_FUN_CALL: {
        auto* n = static_cast<FunCall*>(node);
        // FunCall.name 引用全局函数（当 callee 为 null 时）
        if (!n->callee && resolveToModuleTopLevel(n->name) && renameMap_.count(n->name)) {
            n->name = renameMap_[n->name];
        }
        if (n->callee) renameInNode(n->callee.get());
        for (const auto& arg : n->arguments) {
            if (arg) renameInNode(arg.get());
        }
        break;
    }
    // ---- 控制流节点：递归处理子节点 ----
    case NodeType::NODE_IF_STMT: {
        auto* n = static_cast<IfStmt*>(node);
        if (n->condition) renameInNode(n->condition.get());
        if (n->thenBranch) renameInNode(n->thenBranch.get());
        if (n->elseBranch) renameInNode(n->elseBranch.get());
        break;
    }
    case NodeType::NODE_WHILE_STMT: {
        auto* n = static_cast<WhileStmt*>(node);
        if (n->condition) renameInNode(n->condition.get());
        if (n->body) renameInNode(n->body.get());
        break;
    }
    case NodeType::NODE_FOR_STMT: {
        auto* n = static_cast<ForStmt*>(node);
        // for (init; cond; upd) { body }
        // init 可能是 VarDecl，进入新作用域
        pushScope();
        if (n->initializer) renameInNode(n->initializer.get());
        if (n->condition) renameInNode(n->condition.get());
        if (n->update) renameInNode(n->update.get());
        if (n->body) renameInNode(n->body.get());
        popScope();
        break;
    }
    case NodeType::NODE_BLOCK: {
        renameInBlock(static_cast<Block*>(node));
        break;
    }
    case NodeType::NODE_RETURN_STMT: {
        auto* n = static_cast<ReturnStmt*>(node);
        if (n->value) renameInNode(n->value.get());
        break;
    }
    case NodeType::NODE_PRINT_STMT: {
        auto* n = static_cast<PrintStmt*>(node);
        for (const auto& v : n->values) {
            if (v) renameInNode(v.get());
        }
        break;
    }
    // ---- 表达式节点：递归处理子节点 ----
    case NodeType::NODE_BINARY_OP: {
        auto* n = static_cast<BinaryOp*>(node);
        if (n->left) renameInNode(n->left.get());
        if (n->right) renameInNode(n->right.get());
        break;
    }
    case NodeType::NODE_UNARY_OP: {
        auto* n = static_cast<UnaryOp*>(node);
        if (n->operand) renameInNode(n->operand.get());
        break;
    }
    case NodeType::NODE_ARRAY_LITERAL: {
        auto* n = static_cast<ArrayLiteral*>(node);
        for (const auto& e : n->elements) {
            if (e) renameInNode(e.get());
        }
        break;
    }
    case NodeType::NODE_DICT_LITERAL: {
        auto* n = static_cast<DictLiteral*>(node);
        for (const auto& p : n->pairs) {
            if (p.first) renameInNode(p.first.get());
            if (p.second) renameInNode(p.second.get());
        }
        break;
    }
    case NodeType::NODE_INDEX_ACCESS: {
        auto* n = static_cast<IndexAccess*>(node);
        if (n->object) renameInNode(n->object.get());
        if (n->index) renameInNode(n->index.get());
        break;
    }
    case NodeType::NODE_INDEX_ASSIGN: {
        auto* n = static_cast<IndexAssign*>(node);
        if (n->object) renameInNode(n->object.get());
        if (n->index) renameInNode(n->index.get());
        if (n->value) renameInNode(n->value.get());
        break;
    }
    case NodeType::NODE_MEMBER_ACCESS: {
        auto* n = static_cast<MemberAccess*>(node);
        if (n->object) renameInNode(n->object.get());
        // fieldName 不是变量名，不重命名
        break;
    }
    case NodeType::NODE_MEMBER_ASSIGN: {
        auto* n = static_cast<MemberAssign*>(node);
        if (n->object) renameInNode(n->object.get());
        if (n->value) renameInNode(n->value.get());
        break;
    }
    case NodeType::NODE_METHOD_CALL: {
        auto* n = static_cast<MethodCall*>(node);
        if (n->object) renameInNode(n->object.get());
        for (const auto& a : n->arguments) {
            if (a) renameInNode(a.get());
        }
        // methodName 不是变量名，不重命名
        break;
    }
    case NodeType::NODE_TRY_STMT: {
        auto* n = static_cast<TryStmt*>(node);
        if (n->tryBlock) renameInNode(n->tryBlock.get());
        // catch 块进入新作用域，catchVarName 是局部变量
        pushScope();
        if (!n->catchVarName.empty()) defineInCurrentScope(n->catchVarName);
        if (n->catchBlock) renameInNode(n->catchBlock.get());
        popScope();
        break;
    }
    case NodeType::NODE_THROW_STMT: {
        auto* n = static_cast<ThrowStmt*>(node);
        if (n->expression) renameInNode(n->expression.get());
        break;
    }
    case NodeType::NODE_INTERPOLATED_STRING: {
        auto* n = static_cast<InterpolatedString*>(node);
        for (const auto& expr : n->expressions) {
            if (expr) renameInNode(expr.get());
        }
        break;
    }
    // ---- 不处理的节点 ----
    case NodeType::NODE_IMPORT_STMT:
        // 嵌套 import 不递归（嵌套模块有自己的命名空间）
        break;
    case NodeType::NODE_EXPORT_STMT:
        // ExportStmt 在 collectNonExportTopLevelNames 中已处理（导出名不重命名）
        // 但其内部声明的 initializer 需要递归处理
        if (auto* exp = static_cast<ExportStmt*>(node); exp->declaration) {
            // 导出声明本身不重命名 name，但其 initializer/value 中的引用需要重写
            // 例如: export var x = internalVar + 1; — internalVar 需要重命名
            renameInNode(exp->declaration.get());
        }
        break;
    // ---- 叶子节点：无子节点 ----
    case NodeType::NODE_NUMBER_LITERAL:
    case NodeType::NODE_STRING_LITERAL:
    case NodeType::NODE_BOOL_LITERAL:
    case NodeType::NODE_NULL_LITERAL:
    case NodeType::NODE_SUPER_EXPR:
    case NodeType::NODE_BREAK_STMT:
    case NodeType::NODE_CONTINUE_STMT:
        break;
    }
}
