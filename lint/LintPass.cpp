// ============================================================
// lint/LintPass.cpp - MiniLang 静态分析器实现
// ------------------------------------------------------------
// R162 工具链拓展：8 项检查规则的实现
// ============================================================

#include "lint/LintPass.h"

#include "ast/ASTNode.h"

#include <cctype>

namespace minilang::lint {

// ============================================================
// 构造与重置
// ============================================================

LintPass::LintPass(const LintOptions& opts) : options_(opts) {}

void LintPass::resetState() {
    diagnostics_.clear();
    scopeStack_.clear();
    functions_.clear();
    classes_.clear();
    enums_.clear(); // 拓展二期：穷尽性检查的枚举表
    unreachableInCurrentBlock_ = false;
    currentComplexity_ = 0;
    currentFunctionName_.clear();
    currentFunctionLine_ = 0;
    currentFunctionColumn_ = 0;
    inFunctionBody_ = false;
}

LintResult LintPass::analyze(Block& program) {
    resetState();
    enterScope(); // 全局作用域
    program.accept(*this);
    // 全局作用域退出前检查未使用变量
    if (!scopeStack_.empty()) {
        checkUnusedInScope(scopeStack_.back());
        scopeStack_.pop_back();
    }
    // 检查未使用函数
    if (isRuleEnabled(LintRule::UnusedFunction)) {
        for (const auto& kv : functions_) {
            if (!kv.second.used && !kv.second.name.empty()) {
                // 跳过 main/init 等约定入口函数
                const auto& name = kv.second.name;
                if (name == "main" || name == "init" || name == "test") {
                    continue;
                }
                reportWarning(LintRule::UnusedFunction, "函数 '" + name + "' 已定义但未被调用", kv.second.line,
                              kv.second.column);
            }
        }
    }
    LintResult result;
    result.diagnostics = std::move(diagnostics_);
    result.warningCount = result.diagnostics.warningCount();
    result.errorCount = result.diagnostics.errorCount();
    result.ok = result.errorCount == 0;
    return result;
}

// ============================================================
// DefaultVisitor override
// ============================================================

void LintPass::defaultVisit(ASTNode& node) {
    traverseChildren(node);
}

void LintPass::traverseChildren(ASTNode& node) {
    for (ASTNode* child : node.children()) {
        if (child) {
            child->accept(*this);
        }
    }
}

// ============================================================
// 作用域管理
// ============================================================

void LintPass::enterScope() {
    scopeStack_.emplace_back();
}

void LintPass::exitScope() {
    if (!scopeStack_.empty()) {
        checkUnusedInScope(scopeStack_.back());
        scopeStack_.pop_back();
    }
}

void LintPass::declareVar(const std::string& name, int line, int col, bool isParam) {
    if (scopeStack_.empty()) {
        enterScope();
    }
    // 覆盖同名变量（变量遮蔽）
    scopeStack_.back()[name] = VarInfo{name, line, col, false, isParam};
}

void LintPass::useVar(const std::string& name) {
    // 从内向外查找变量
    for (auto it = scopeStack_.rbegin(); it != scopeStack_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            found->second.used = true;
            return;
        }
    }
    // 未找到变量不报错（可能是内置函数或类名），交给运行时处理
}

void LintPass::declareFun(const std::string& name, int line, int col, const std::vector<std::string>& params,
                          const std::vector<std::string>& paramTypes) {
    functions_[name] = FunInfo{name, line, col, false, params, paramTypes};
}

void LintPass::useFun(const std::string& name) {
    auto it = functions_.find(name);
    if (it != functions_.end()) {
        it->second.used = true;
    }
}

void LintPass::checkUnusedInScope(std::unordered_map<std::string, VarInfo>& scope) {
    if (!isRuleEnabled(LintRule::UnusedVariable)) {
        return;
    }
    for (const auto& kv : scope) {
        const auto& info = kv.second;
        if (!info.used) {
            LintRule rule = info.isParameter ? LintRule::UnusedParameter : LintRule::UnusedVariable;
            if (!isRuleEnabled(rule)) {
                continue;
            }
            std::string kind = info.isParameter ? "参数" : "变量";
            reportWarning(rule, kind + " '" + info.name + "' 已声明但未使用", info.line, info.column);
        }
    }
}

// ============================================================
// 诊断报告
// ============================================================

bool LintPass::isRuleEnabled(LintRule rule) const {
    return options_.disabledRules.find(rule) == options_.disabledRules.end();
}

void LintPass::reportWarning(LintRule rule, const std::string& msg, int line, int col) {
    diagnostics_.addWarning(msg, line, col, DiagSource::Lint, ruleCode(rule));
}

void LintPass::reportInfo(LintRule rule, const std::string& msg, int line, int col) {
    diagnostics_.addInfo(msg, line, col, DiagSource::Lint, ruleCode(rule));
}

std::string LintPass::ruleCode(LintRule rule) {
    switch (rule) {
    case LintRule::UnusedVariable:
        return "lint-unused-variable";
    case LintRule::UnusedFunction:
        return "lint-unused-function";
    case LintRule::UnusedParameter:
        return "lint-unused-parameter";
    case LintRule::AssignmentInCondition:
        return "lint-assignment-in-condition";
    case LintRule::DeadCodeAfterReturn:
        return "lint-dead-code";
    case LintRule::EmptyBlock:
        return "lint-empty-block";
    case LintRule::CyclomaticComplexity:
        return "lint-cyclomatic-complexity";
    case LintRule::NamingConvention:
        return "lint-naming-convention";
    case LintRule::NonExhaustiveMatch:
        return "lint-non-exhaustive-match";
    case LintRule::Count:
        return "";
    }
    return "";
}

std::string LintPass::ruleName(LintRule rule) {
    switch (rule) {
    case LintRule::UnusedVariable:
        return "UnusedVariable";
    case LintRule::UnusedFunction:
        return "UnusedFunction";
    case LintRule::UnusedParameter:
        return "UnusedParameter";
    case LintRule::AssignmentInCondition:
        return "AssignmentInCondition";
    case LintRule::DeadCodeAfterReturn:
        return "DeadCodeAfterReturn";
    case LintRule::EmptyBlock:
        return "EmptyBlock";
    case LintRule::CyclomaticComplexity:
        return "CyclomaticComplexity";
    case LintRule::NamingConvention:
        return "NamingConvention";
    case LintRule::NonExhaustiveMatch:
        return "NonExhaustiveMatch";
    case LintRule::Count:
        return "";
    }
    return "";
}

// ============================================================
// 命名规范检查
// ============================================================

bool LintPass::isCamelCase(const std::string& name) {
    if (name.empty()) {
        return true;
    }
    // 首字母小写
    if (!std::islower(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    // 不含下划线（允许全大写常量如 MAX_VAL，但这里简化为不允许下划线）
    for (char c : name) {
        if (c == '_') {
            return false;
        }
    }
    return true;
}

bool LintPass::isPascalCase(const std::string& name) {
    if (name.empty()) {
        return true;
    }
    // 首字母大写
    if (!std::isupper(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    // 不含下划线
    for (char c : name) {
        if (c == '_') {
            return false;
        }
    }
    return true;
}

// ============================================================
// visit 方法实现
// ============================================================

void LintPass::visitVarDecl(VarDecl& node) {
    // 死代码检测
    if (unreachableInCurrentBlock_ && isRuleEnabled(LintRule::DeadCodeAfterReturn)) {
        reportWarning(LintRule::DeadCodeAfterReturn, "不可达代码：return/break/continue 之后的语句", node.line,
                      node.column);
    }

    // 命名规范检查
    if (isRuleEnabled(LintRule::NamingConvention) && options_.checkCamelCaseVariables) {
        // 跳过全大写常量（如 MAX_SIZE）
        bool allUpper = true;
        for (char c : node.name) {
            if (std::islower(static_cast<unsigned char>(c))) {
                allUpper = false;
                break;
            }
        }
        if (!allUpper && !isCamelCase(node.name)) {
            reportWarning(LintRule::NamingConvention, "变量名 '" + node.name + "' 不符合 camelCase 命名规范", node.line,
                          node.column);
        }
    }

    // 声明变量
    declareVar(node.name, node.line, node.column, false);

    // 遍历初始化表达式
    if (node.initializer) {
        node.initializer->accept(*this);
    }
}

void LintPass::visitFunDecl(FunDecl& node) {
    // 死代码检测
    if (unreachableInCurrentBlock_ && isRuleEnabled(LintRule::DeadCodeAfterReturn)) {
        reportWarning(LintRule::DeadCodeAfterReturn, "不可达代码：return/break/continue 之后的语句", node.line,
                      node.column);
    }

    // 命名规范检查
    if (isRuleEnabled(LintRule::NamingConvention) && options_.checkCamelCaseFunctions) {
        if (!isCamelCase(node.name)) {
            reportWarning(LintRule::NamingConvention, "函数名 '" + node.name + "' 不符合 camelCase 命名规范", node.line,
                          node.column);
        }
    }

    // 记录函数定义
    declareFun(node.name, node.line, node.column, node.params, node.paramTypes);

    // 保存上下文
    auto savedComplexity = currentComplexity_;
    auto savedName = currentFunctionName_;
    auto savedLine = currentFunctionLine_;
    auto savedCol = currentFunctionColumn_;
    auto savedInFunc = inFunctionBody_;
    auto savedUnreachable = unreachableInCurrentBlock_;

    // 进入函数体：圈复杂度基础 1
    currentComplexity_ = 1;
    currentFunctionName_ = node.name;
    currentFunctionLine_ = node.line;
    currentFunctionColumn_ = node.column;
    inFunctionBody_ = true;
    unreachableInCurrentBlock_ = false;

    // 进入函数作用域
    enterScope();

    // 声明参数
    for (const auto& param : node.params) {
        declareVar(param, node.line, node.column, true);
    }

    // 遍历默认参数值表达式（在函数作用域中求值）
    for (const auto& dv : node.defaultValues) {
        if (dv) {
            dv->accept(*this);
        }
    }

    // 遍历函数体
    if (node.body) {
        node.body->accept(*this);
    }

    // 退出函数作用域（检查未使用参数和局部变量）
    exitScope();

    // 检查圈复杂度
    if (isRuleEnabled(LintRule::CyclomaticComplexity) && currentComplexity_ > options_.maxCyclomaticComplexity) {
        reportWarning(LintRule::CyclomaticComplexity,
                      "函数 '" + currentFunctionName_ + "' 圈复杂度为 " + std::to_string(currentComplexity_) +
                          "，超过阈值 " + std::to_string(options_.maxCyclomaticComplexity),
                      currentFunctionLine_, currentFunctionColumn_);
    }

    // 恢复上下文
    currentComplexity_ = savedComplexity;
    currentFunctionName_ = savedName;
    currentFunctionLine_ = savedLine;
    currentFunctionColumn_ = savedCol;
    inFunctionBody_ = savedInFunc;
    unreachableInCurrentBlock_ = savedUnreachable;
}

void LintPass::visitIfStmt(IfStmt& node) {
    // 圈复杂度 +1
    if (inFunctionBody_) {
        currentComplexity_++;
    }

    // 条件中赋值检查
    checkConditionForAssignment(node.condition.get(), node.line, node.column);

    // 遍历条件
    if (node.condition) {
        node.condition->accept(*this);
    }

    // then 分支
    if (node.thenBranch) {
        // then 分支是新作用域
        enterScope();
        auto savedUnreachable = unreachableInCurrentBlock_;
        unreachableInCurrentBlock_ = false;
        node.thenBranch->accept(*this);
        exitScope();
        unreachableInCurrentBlock_ = savedUnreachable;
    }

    // else 分支
    if (node.elseBranch) {
        enterScope();
        auto savedUnreachable = unreachableInCurrentBlock_;
        unreachableInCurrentBlock_ = false;
        node.elseBranch->accept(*this);
        exitScope();
        unreachableInCurrentBlock_ = savedUnreachable;
    }
}

void LintPass::visitWhileStmt(WhileStmt& node) {
    // 圈复杂度 +1
    if (inFunctionBody_) {
        currentComplexity_++;
    }

    // 条件中赋值检查
    checkConditionForAssignment(node.condition.get(), node.line, node.column);

    if (node.condition) {
        node.condition->accept(*this);
    }

    if (node.body) {
        enterScope();
        auto savedUnreachable = unreachableInCurrentBlock_;
        unreachableInCurrentBlock_ = false;
        node.body->accept(*this);
        exitScope();
        unreachableInCurrentBlock_ = savedUnreachable;
    }
}

void LintPass::visitForStmt(ForStmt& node) {
    // 圈复杂度 +1
    if (inFunctionBody_) {
        currentComplexity_++;
    }

    // for 条件中赋值检查（虽不常见，但保持一致性）
    checkConditionForAssignment(node.condition.get(), node.line, node.column);

    enterScope();
    auto savedUnreachable = unreachableInCurrentBlock_;
    unreachableInCurrentBlock_ = false;

    if (node.initializer) {
        node.initializer->accept(*this);
    }
    if (node.condition) {
        node.condition->accept(*this);
    }
    if (node.update) {
        node.update->accept(*this);
    }
    if (node.body) {
        node.body->accept(*this);
    }

    exitScope();
    unreachableInCurrentBlock_ = savedUnreachable;
}

void LintPass::visitBlock(Block& node) {
    // 空块检查
    if (isRuleEnabled(LintRule::EmptyBlock) && node.statements.empty()) {
        reportWarning(LintRule::EmptyBlock, "空代码块 {}", node.line, node.column);
    }

    enterScope();
    auto savedUnreachable = unreachableInCurrentBlock_;
    unreachableInCurrentBlock_ = false;

    for (const auto& stmt : node.statements) {
        if (unreachableInCurrentBlock_ && isRuleEnabled(LintRule::DeadCodeAfterReturn)) {
            reportWarning(LintRule::DeadCodeAfterReturn, "不可达代码：return/break/continue 之后的语句", stmt->line,
                          stmt->column);
        }
        if (stmt) {
            stmt->accept(*this);
        }
    }

    exitScope();
    unreachableInCurrentBlock_ = savedUnreachable;
}

void LintPass::visitVarRef(VarRef& node) {
    useVar(node.name);
}

void LintPass::visitAssignment(Assignment& node) {
    // Bug #90 fix: 赋值左侧是写入而非读取，不应标记为"已使用"。
    // 否则 `x = 1;`（从未读取 x）会被误判为已使用，漏报 UnusedVariable。
    // 真正的读取由 visitVarRef 中的 useVar 处理。
    if (node.value) {
        node.value->accept(*this);
    }
}

void LintPass::visitFunCall(FunCall& node) {
    // 标记函数已使用
    useFun(node.name);

    // 死代码检测
    if (unreachableInCurrentBlock_ && isRuleEnabled(LintRule::DeadCodeAfterReturn)) {
        reportWarning(LintRule::DeadCodeAfterReturn, "不可达代码：return/break/continue 之后的语句", node.line,
                      node.column);
    }

    // 遍历参数
    for (const auto& arg : node.arguments) {
        if (arg) {
            arg->accept(*this);
        }
    }

    // 遍历 callee（闭包调用形式）
    if (node.callee) {
        node.callee->accept(*this);
    }
}

void LintPass::visitReturnStmt(ReturnStmt& node) {
    if (node.value) {
        node.value->accept(*this);
    }
    // 标记当前块后续语句不可达
    unreachableInCurrentBlock_ = true;
}

void LintPass::visitBreakStmt(BreakStmt& node) {
    unreachableInCurrentBlock_ = true;
    traverseChildren(node);
}

void LintPass::visitContinueStmt(ContinueStmt& node) {
    unreachableInCurrentBlock_ = true;
    traverseChildren(node);
}

void LintPass::visitClassDecl(ClassDecl& node) {
    // 死代码检测
    if (unreachableInCurrentBlock_ && isRuleEnabled(LintRule::DeadCodeAfterReturn)) {
        reportWarning(LintRule::DeadCodeAfterReturn, "不可达代码：return/break/continue 之后的语句", node.line,
                      node.column);
    }

    // 命名规范检查
    if (isRuleEnabled(LintRule::NamingConvention) && options_.checkPascalCaseClasses) {
        if (!isPascalCase(node.name)) {
            reportWarning(LintRule::NamingConvention, "类名 '" + node.name + "' 不符合 PascalCase 命名规范", node.line,
                          node.column);
        }
    }

    classes_.insert(node.name);

    // 遍历类成员
    for (const auto& member : node.members) {
        if (member) {
            member->accept(*this);
        }
    }
}

void LintPass::visitBinaryOp(BinaryOp& node) {
    // 圈复杂度：逻辑运算符 +1
    if (inFunctionBody_ && (node.opType == BinOpType::BIN_AND || node.opType == BinOpType::BIN_OR)) {
        currentComplexity_++;
    }

    if (node.left) {
        node.left->accept(*this);
    }
    if (node.right) {
        node.right->accept(*this);
    }
}

// ============================================================
// 辅助方法
// ============================================================

// ============================================================
// 拓展二期：match 穷尽性检查（NonExhaustiveMatch）
// ------------------------------------------------------------
// 判定规则：
//   1. 存在兜底分支（default，或无 guard 的 WILDCARD/VARIABLE pattern）
//      → 穷尽，不报。
//   2. 否则收集"完全覆盖"的 variant：VARIANT pattern 无 guard 且全部
//      子 pattern 不可失败（WILDCARD/VARIABLE）才计入（Some(1) 只覆盖
//      字面量 1，不算覆盖 Some）；OR pattern 递归展开。
//   3. 枚举类型取首个 VARIANT pattern 的 enumName；无 VARIANT pattern
//      （非枚举 match）或枚举声明不在本文件（跨模块）则跳过。
//   4. 差集非空 → 警告列出缺失 variant。
// 三后端行为不受影响（运行时未命中仍报统一错误），本规则提供
// 编译期预警（教学价值：展示 sealed 类型的穷尽性思维）。
// ============================================================

namespace {

/// pattern 是否不可失败（匹配任意值）
bool isIrrefutablePattern(const MatchPattern& p) {
    return p.kind == MatchPatternKind::WILDCARD || p.kind == MatchPatternKind::VARIABLE;
}

/// 递归收集 pattern 完全覆盖的 variant（含 OR 展开），并记录首个 enum 名
void collectCoveredVariants(const MatchPattern& p, std::string& enumName,
                            std::unordered_set<std::string>& covered) {
    if (p.kind == MatchPatternKind::VARIANT) {
        if (enumName.empty())
            enumName = p.enumName;
        if (p.enumName != enumName)
            return; // 混合枚举（异常代码）：保守跳过
        // 全部子 pattern 不可失败才算完全覆盖该 variant
        for (const auto& sp : p.subPatterns) {
            if (!sp || !isIrrefutablePattern(*sp))
                return;
        }
        covered.insert(p.variantName);
    } else if (p.kind == MatchPatternKind::OR) {
        for (const auto& sp : p.subPatterns) {
            if (sp)
                collectCoveredVariants(*sp, enumName, covered);
        }
    }
}

} // namespace

void LintPass::visitEnumDecl(EnumDecl& node) {
    // 收集枚举声明（同名重声明后者覆盖，与运行时注册表语义一致）
    std::vector<std::string> names;
    names.reserve(node.variants.size());
    for (const auto& v : node.variants) {
        names.push_back(v.name);
    }
    enums_[node.name] = std::move(names);
    traverseChildren(node);
}

void LintPass::visitMatchExpr(MatchExpr& node) {
    if (isRuleEnabled(LintRule::NonExhaustiveMatch)) {
        bool hasCatchAll = false;
        std::string enumName;
        std::unordered_set<std::string> covered;
        for (const auto& mc : node.cases) {
            // 兜底分支：default 或无 guard 的不可失败 pattern
            // （含 guard 的分支可能失败，不算兜底）
            if (mc.isDefault || !mc.pattern) {
                if (!mc.guard)
                    hasCatchAll = true;
                continue;
            }
            if (!mc.guard && isIrrefutablePattern(*mc.pattern)) {
                hasCatchAll = true;
                continue;
            }
            if (!mc.guard) {
                collectCoveredVariants(*mc.pattern, enumName, covered);
            } else if (enumName.empty() && mc.pattern->kind == MatchPatternKind::VARIANT) {
                // 含 guard 的 VARIANT 不计入覆盖，但可用于确定枚举类型
                enumName = mc.pattern->enumName;
            }
        }
        if (!hasCatchAll && !enumName.empty()) {
            auto it = enums_.find(enumName);
            if (it != enums_.end()) {
                std::string missing;
                for (const auto& v : it->second) {
                    if (covered.count(v) == 0) {
                        if (!missing.empty())
                            missing += ", ";
                        missing += enumName + "." + v;
                    }
                }
                if (!missing.empty()) {
                    reportWarning(LintRule::NonExhaustiveMatch,
                                  "match 未穷尽枚举 '" + enumName + "' 的全部 variant（缺少: " + missing +
                                      "），未命中时将抛运行时错误；补全 case 或添加 default 分支",
                                  node.line, node.column);
                }
            }
        }
    }
    // 继续默认遍历（scrutinee/body/guard 子节点，变量使用追踪不受影响）
    traverseChildren(node);
}

// Bug #89 fix: 递归检测条件表达式子树中的赋值节点。
// 原实现仅检查顶层节点，无法捕获 `if (a && (b = 1))` 这类嵌套赋值。
static bool containsAssignment(ASTNode* node) {
    if (!node)
        return false;
    if (node->nodeType == NodeType::NODE_ASSIGNMENT)
        return true;
    for (ASTNode* child : node->children()) {
        if (containsAssignment(child))
            return true;
    }
    return false;
}

void LintPass::checkConditionForAssignment(ASTNode* cond, int /*line*/, int /*col*/) {
    if (!cond || !isRuleEnabled(LintRule::AssignmentInCondition)) {
        return;
    }
    if (containsAssignment(cond)) {
        reportWarning(LintRule::AssignmentInCondition, "条件表达式中含赋值（可能误将 '==' 写成 '='）", cond->line,
                      cond->column);
    }
}

} // namespace minilang::lint
