#pragma once

#include "ast/ASTNode.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// ModuleIsolation — BUG-AUDIT-MOD-2 完整统一：VM/IR 模块隔离
// ------------------------------------------------------------
// 在 VM/IR 编译期内联模块时，将模块的非导出顶层声明名重命名为
// `__mod_<hash>__<name>`，并递归重写模块内对这些名字的引用。
// 导入方无法用原名访问模块的非导出名，与 Interpreter 的模块隔离语义对齐。
//
// 作用域分析：
// - 模块顶层作用域（栈底）：包含所有模块顶层声明名（导出+非导出）
// - 函数作用域：参数 + 函数体内 VarDecl
// - 块作用域：{} 内的 VarDecl
// - VarRef/Assignment/FunCall 从内向外查找，仅当解析到模块顶层且名字在
//   renameMap 中时重命名（避免误改局部变量）
//
// 边界情况：
// - 模块内嵌套定义同名局部变量：局部变量优先，引用不重命名
// - 模块的非导出顶层函数/类：声明名重命名，模块内调用点重命名
// - 模块的导出顶层声明：不重命名（导入方需要用原名访问）
// - 嵌套 import：不递归（嵌套模块有自己的命名空间）
// ============================================================

/// 模块顶层非导出名重命名工具
class ModuleTopLevelRenamer {
public:
    /// 重命名模块的非导出顶层名
    /// @param moduleAst 模块的顶层 Block
    /// @param modulePath 模块路径（用于生成唯一前缀）
    static void rename(Block& moduleAst, const std::string& modulePath);

private:
    std::unordered_map<std::string, std::string> renameMap_;  // oldName -> newName
    std::vector<std::unordered_set<std::string>> scopeStack_; // 作用域栈，栈底是模块顶层
    std::string modulePath_;                                  // 模块路径（用于生成前缀）
    // BUG-INH-AUDIT-5 fix: 标记当前正在处理类成员（字段声明）。
    // 字段声明（ClassDecl 内的 VarDecl）不应登记到作用域——字段只能通过
    // this.field 访问（MemberAccess），不参与变量名解析。原实现将字段名
    // 登记到类作用域，导致方法体内引用同名模块顶层变量时被遮蔽（不重命名），
    // 运行时报"未定义变量"。
    bool inClassBody_ = false;

    explicit ModuleTopLevelRenamer(const std::string& modulePath);

    /// 第 1 步：扫描模块顶层 statements，收集非导出顶层名并构建 renameMap
    void collectNonExportTopLevelNames(Block& moduleAst);

    /// 第 2 步：递归遍历模块 AST，重命名声明 + 引用
    void renameInBlock(Block* node);
    void renameInNode(ASTNode* node);
    /// AUDIT-R5 BUG-09 fix: 递归登记 match pattern 的绑定变量（VARIABLE 模式的
    /// variableName，VARIANT/TUPLE/OR 递归 subPatterns）到当前作用域，
    /// 避免 case body/guard 内引用绑定变量时被误判为模块顶层名而重命名。
    void collectMatchPatternBindings(const class MatchPattern* pattern);

    // 作用域管理
    void pushScope() { scopeStack_.emplace_back(); }
    void popScope() { scopeStack_.pop_back(); }
    void defineInCurrentScope(const std::string& name) { scopeStack_.back().insert(name); }
    /// 从内向外查找 name，返回是否解析到模块顶层（即所有内层作用域都未定义）
    bool resolveToModuleTopLevel(const std::string& name) const;

    /// 生成模块路径的简短 hash（8 字符 hex）
    static std::string pathHash(const std::string& modulePath);
};
