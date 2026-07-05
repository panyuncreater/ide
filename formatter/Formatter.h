/**
 * @file formatter/Formatter.h
 * @brief 代码格式化器（AST → 标准代码文本）。
 *
 * 将 AST 重新格式化为标准代码文本，统一缩进/空格/换行风格。
 * 继承 DefaultVisitor，统一 AST 分派为 Visitor 模式：
 * formatNode 通过 node->accept(*this) 分派到对应的 visit* 方法，
 * visit* 方法内部调用原 format* 逻辑并将结果存入 lastFormatResult_。
 *
 * 核心特性：
 *   - 32 种 AST 节点类型的格式化（覆盖全部 NodeType）
 *   - 三种花括号风格预设（compact / allman / tabbed）
 *   - F1 fix: 注释保留（setComments 注入注释 Token，按行号插入）
 *   - 运算符优先级正确加括号（needsParens + opPrecedence）
 *   - 字符串插值保留（C5 fix: formatInterpolatedString）
 *
 * 往返等价性约束（formatter_audit 测试套件）：
 *   - format(parse(source)) 解析后 AST 结构等价于 parse(source)
 *   - 缩进双重计算、括号保留规则、AST 结构不等价是已知高频 Bug
 *
 * DoS 防护：
 *   - MAX_FORMAT_DEPTH（256）：递归深度上限
 *
 * @see Parser ASTNode DefaultVisitor
 */
#pragma once

#include <string>
#include <memory>
#include <vector>
#include "ast/ASTNode.h"
#include "lexer/Token.h"
#include "interpreter/Visitor.h"
#include "common/RuntimeLimits.h"

// ARCH-01 fix: 移除不必要的 #include "interpreter/Value.h"。
// Formatter 的 visit 方法返回 void，lastFormatResult_ 是 std::string，
// 头文件不使用 Value 类型。Formatter.cpp 调用 getValue().toString() 时
// 自行 include Value.h。这使 Formatter 成为一个独立的工具模块，
// 修改 Value.h 不再触发 Formatter 重编译。

class ASTNode;
class Block;
class BinaryOp;
class UnaryOp;
class NumberLiteral;
class StringLiteral;
class BoolLiteral;
class VarDecl;
class Assignment;
class VarRef;
class IfStmt;
class WhileStmt;
class ForStmt;
class FunDecl;
class FunCall;
class ReturnStmt;
class PrintStmt;
class ArrayLiteral;
class DictLiteral;
class IndexAccess;
class IndexAssign;
class ClassDecl;
class MemberAccess;
class MemberAssign;
class MethodCall;
class NullLiteral;
class SuperExpr;
class BreakStmt;
class ContinueStmt;

// ============================================================
// Formatter 代码格式化器
// ============================================================

/// 花括号风格
enum class BraceStyle {
    SAME_LINE,    // K&R 风格：if (x) {
    NEXT_LINE     // Allman 风格：if (x)\n{
};

/// 格式化选项
struct FormatOptions {
    int indentSize = 4;              // 缩进空格数
    bool useTabs = false;            // 使用 tab 缩进（true 时忽略 indentSize）
    BraceStyle braceStyle = BraceStyle::SAME_LINE;
    bool spaceAroundOperators = true;  // 二元运算符两侧加空格
    bool spaceAfterComma = true;       // 逗号后加空格
    bool blankLineBetweenFunctions = true;  // 函数/类声明之间加空行
    // AUDIT-FMT-P1 fix: 移除 semicolons 字段——它是死代码（声明但从未被读取）。
    // MiniLang 解析器严格要求语句以 ';' 结束（Parser.cpp 中 var/expr/break/continue
    // 等均 consume(TK_SEMICOLON)），若实现 semicolons=false 会产生无法重新解析的代码，
    // 破坏往返不变量。原配置项误导用户以为可选关闭分号，移除以避免误用。

    /// 预设：紧凑风格
    static FormatOptions compact() {
        FormatOptions o;
        o.indentSize = 2;
        o.blankLineBetweenFunctions = false;
        return o;
    }

    /// 预设：Allman 风格
    static FormatOptions allman() {
        FormatOptions o;
        o.braceStyle = BraceStyle::NEXT_LINE;
        return o;
    }

    /// 预设：Tab 缩进
    static FormatOptions tabbed() {
        FormatOptions o;
        o.useTabs = true;
        o.indentSize = 1;
        return o;
    }
};

/// 将 AST 重新格式化为标准代码文本
/// 继承 DefaultVisitor，统一 AST 分派为 Visitor 模式：
/// formatNode 通过 node->accept(*this) 分派到对应的 visit* 方法，
/// visit* 方法内部调用原 format* 逻辑并将结果存入 lastFormatResult_。
class Formatter : public DefaultVisitor {
public:
    Formatter();

    // ---- Visitor 模式：visit* 方法（override）----
    // 每个 visit* 方法调用对应的 format* 逻辑，将结果存入 lastFormatResult_，
    // 返回 Value::nullValue()。统一 AST 分派为 Visitor 模式，替代原 25 路 switch。
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
    void visitInterpolatedString(InterpolatedString& node) override;  // C5 fix

    /// 格式化 AST 为代码文本
    std::string format(Block& program);

    /// 设置注释 token 列表（F1 fix: 保留源代码中的注释）
    void setComments(const std::vector<Token>& tokens);

    /// 设置缩进大小（兼容旧接口）
    void setIndentSize(int size);

    /// 设置完整格式化选项
    void setOptions(const FormatOptions& options);

    /// 获取当前格式化选项
    const FormatOptions& getOptions() const;

    // 运算符优先级辅助（public 供自由函数 needsParens 使用）
    static int opPrecedence(BinOpType opType);
    static bool isRightAssoc(BinOpType opType);

private:
    FormatOptions options_;
    int currentIndent_ = 0;      // 当前缩进级别
    int formatDepth_ = 0;        // D5 fix: 格式化递归深度计数器
    static constexpr int MAX_FORMAT_DEPTH = RuntimeLimits::MAX_FORMAT_DEPTH;
    std::vector<Token> comments_; // F1 fix: 注释 token 列表
    size_t commentIndex_ = 0;    // 当前注释游标
    std::string lastFormatResult_;  // 存储 visit* 方法的格式化结果

    // P3 fix: 缓存频繁生成的小字符串，避免重复分配（mutable 因为 indent() 是 const）
    mutable std::string indentCache_;     // 当前缩进字符串缓存
    mutable int cachedIndentLevel_ = -1; // 缓存对应的缩进级别
    std::string commaCache_;     // 逗号分隔符缓存
    bool commaCacheValid_ = false;
    mutable std::string binOpKey_;       // binOp 缓存键（上次使用的运算符）
    mutable std::string binOpVal_;       // binOp 缓存值

    /// 生成缩进字符串（P3: 带缓存）
    std::string indent() const;

    /// 生成二元运算符（根据 spaceAroundOperators 选项）
    std::string binOp(const std::string& op) const;

    /// 生成逗号分隔符（P3: 带缓存）
    std::string comma();

    /// 生成开括号（根据 braceStyle 选项）
    std::string openBrace() const;

    /// 格式化 AST 节点（通过 Visitor 模式分派到 visit* 方法）
    std::string formatNode(ASTNode* node);

    /// 格式化各种节点类型
    std::string formatBinaryOp(BinaryOp& node);
    std::string formatUnaryOp(UnaryOp& node);
    std::string formatNumberLiteral(NumberLiteral& node);
    std::string formatStringLiteral(StringLiteral& node);
    std::string formatBoolLiteral(BoolLiteral& node);
    std::string formatVarDecl(VarDecl& node);
    std::string formatAssignment(Assignment& node);
    std::string formatVarRef(VarRef& node);
    std::string formatIfStmt(IfStmt& node);
    std::string formatWhileStmt(WhileStmt& node);
    std::string formatForStmt(ForStmt& node);
    std::string formatFunDecl(FunDecl& node);
    std::string formatFunCall(FunCall& node);
    std::string formatReturnStmt(ReturnStmt& node);
    std::string formatPrintStmt(PrintStmt& node);
    std::string formatBlock(Block& node);

    // 新增节点格式化
    std::string formatArrayLiteral(ArrayLiteral& node);
    std::string formatDictLiteral(DictLiteral& node);
    std::string formatIndexAccess(IndexAccess& node);
    std::string formatIndexAssign(IndexAssign& node);
    std::string formatClassDecl(ClassDecl& node);
    std::string formatMemberAccess(MemberAccess& node);
    std::string formatMemberAssign(MemberAssign& node);
    std::string formatMethodCall(MethodCall& node);
    std::string formatNullLiteral(NullLiteral& node);
    std::string formatInterpolatedString(InterpolatedString& node);  // C5 fix
};
