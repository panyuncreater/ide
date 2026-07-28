// ============================================================
// lint/LintPass.h - MiniLang 静态分析器（Lint 工具）
// ------------------------------------------------------------
// R162 工具链拓展：基于 AST 的静态分析，检测常见代码问题。
// 继承 DefaultVisitor 遍历 AST，收集诊断到 DiagnosticBag。
//
// 检查规则（8 项）：
//   1. UnusedVariable       — 变量声明后未引用
//   2. UnusedFunction       — 函数定义后未调用
//   3. UnusedParameter      — 函数参数未使用
//   4. AssignmentInCondition — if/while 条件中含赋值
//   5. DeadCodeAfterReturn  — return 后的不可达语句
//   6. EmptyBlock           — 空块 {}
//   7. CyclomaticComplexity — 函数圈复杂度过高
//   8. NamingConvention     — 变量/函数/类命名规范
//
// 设计要点：
//   - 复用 Lexer → Parser → AST 管线，无需重新解析
//   - 输出到 DiagnosticBag（与现有诊断系统一致）
//   - 命名空间 minilang::lint 隔离
//   - 不依赖 Qt6 / Interpreter / Compiler，仅依赖 ast + common
//
// @see DiagnosticBag DefaultVisitor ASTNode
// ============================================================
#pragma once

#include "common/Diagnostic.h"
#include "interpreter/Visitor.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace minilang::lint {

/// Lint 规则枚举
enum class LintRule {
    UnusedVariable,        ///< 变量声明后未引用
    UnusedFunction,        ///< 函数定义后未调用
    UnusedParameter,       ///< 函数参数未使用
    AssignmentInCondition, ///< if/while 条件中含赋值
    DeadCodeAfterReturn,   ///< return 后的不可达语句
    EmptyBlock,            ///< 空块 {}
    CyclomaticComplexity,  ///< 函数圈复杂度过高
    NamingConvention,      ///< 变量/函数/类命名规范
    NonExhaustiveMatch,    ///< 拓展二期：match 枚举未覆盖全部 variant 且无 default/通配分支
    Count                  ///< 规则总数（用于遍历）
};

/// Lint 选项
struct LintOptions {
    /// 默认禁用的规则（空集合表示全启用）
    std::unordered_set<LintRule> disabledRules;
    /// 圈复杂度阈值（默认 10）
    int maxCyclomaticComplexity = 10;
    /// 是否检查变量名 camelCase
    bool checkCamelCaseVariables = true;
    /// 是否检查函数名 camelCase
    bool checkCamelCaseFunctions = true;
    /// 是否检查类名 PascalCase
    bool checkPascalCaseClasses = true;
};

/// Lint 分析结果
struct LintResult {
    DiagnosticBag diagnostics; ///< 诊断信息集合
    int errorCount = 0;        ///< 错误数（lint 通常只有 warning/info）
    int warningCount = 0;      ///< 警告数
    bool ok = true;            ///< 无错误时为 true（warnings 不影响 ok）
};

/// LintPass 静态分析器
///
/// 继承 DefaultVisitor 遍历 AST，实现 8 项检查规则。
/// 典型用法：
///   LintPass pass(options);
///   LintResult result = pass.analyze(*ast);
///   for (auto& d : result.diagnostics.all()) { ... }
class LintPass : public DefaultVisitor {
public:
    explicit LintPass(const LintOptions& opts = {});

    /// 分析 AST 根节点，返回诊断结果
    /// 调用后内部状态清空，可重复调用
    LintResult analyze(Block& program);

    // ---- DefaultVisitor override ----
    void defaultVisit(ASTNode& node) override;
    void visitVarDecl(VarDecl& node) override;
    void visitFunDecl(FunDecl& node) override;
    void visitIfStmt(IfStmt& node) override;
    void visitWhileStmt(WhileStmt& node) override;
    void visitForStmt(ForStmt& node) override;
    void visitBlock(Block& node) override;
    void visitVarRef(VarRef& node) override;
    void visitAssignment(Assignment& node) override;
    void visitFunCall(FunCall& node) override;
    void visitReturnStmt(ReturnStmt& node) override;
    void visitClassDecl(ClassDecl& node) override;
    void visitBreakStmt(BreakStmt& node) override;
    void visitContinueStmt(ContinueStmt& node) override;
    void visitBinaryOp(BinaryOp& node) override;
    // 拓展二期：穷尽性检查——收集 enum 声明 + 对照 match 覆盖集
    void visitEnumDecl(EnumDecl& node) override;
    void visitMatchExpr(MatchExpr& node) override;

private:
    LintOptions options_;
    DiagnosticBag diagnostics_;

    // ---- 变量使用追踪 ----
    struct VarInfo {
        std::string name;
        int line = 0;
        int column = 0;
        bool used = false;
        bool isParameter = false;
    };
    /// 作用域栈：每个作用域是一个 name→VarInfo 映射
    std::vector<std::unordered_map<std::string, VarInfo>> scopeStack_;

    // ---- 函数定义追踪 ----
    struct FunInfo {
        std::string name;
        int line = 0;
        int column = 0;
        bool used = false;
        std::vector<std::string> params;
        std::vector<std::string> paramTypes;
    };
    std::unordered_map<std::string, FunInfo> functions_;

    // ---- 类定义追踪 ----
    std::unordered_set<std::string> classes_;

    // ---- 拓展二期：枚举声明追踪（穷尽性检查用，name → variant 名列表）----
    std::unordered_map<std::string, std::vector<std::string>> enums_;

    // ---- 死代码检测 ----
    /// 当前块是否已遇到 return/break/continue
    bool unreachableInCurrentBlock_ = false;

    // ---- 圈复杂度 ----
    int currentComplexity_ = 0;
    std::string currentFunctionName_;
    int currentFunctionLine_ = 0;
    int currentFunctionColumn_ = 0;
    bool inFunctionBody_ = false;

    // ---- 辅助方法 ----
    void enterScope();
    void exitScope();
    void declareVar(const std::string& name, int line, int col, bool isParam = false);
    void useVar(const std::string& name);
    void declareFun(const std::string& name, int line, int col, const std::vector<std::string>& params,
                    const std::vector<std::string>& paramTypes);
    void useFun(const std::string& name);
    void checkUnusedInScope(std::unordered_map<std::string, VarInfo>& scope);
    bool isRuleEnabled(LintRule rule) const;
    void reportWarning(LintRule rule, const std::string& msg, int line, int col);
    void reportInfo(LintRule rule, const std::string& msg, int line, int col);
    void traverseChildren(ASTNode& node);
    void checkConditionForAssignment(ASTNode* cond, int line, int col);
    void resetState();
    static bool isCamelCase(const std::string& name);
    static bool isPascalCase(const std::string& name);
    static std::string ruleCode(LintRule rule);
    static std::string ruleName(LintRule rule);
};

} // namespace minilang::lint
