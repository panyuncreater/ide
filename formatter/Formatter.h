/**
 * @file formatter/Formatter.h
 * @brief 代码格式化器（AST → 标准代码文本）。
 *
 * 将 AST 重新格式化为标准代码文本，统一缩进/空格/换行风格。
 * 继承 DefaultVisitor，统一 AST 分派为 Visitor 模式：
 * formatNode 通过 node->accept(*this) 分派到对应的 visit* 方法，
 * visit* 方法把格式化文本直接追加到成员输出缓冲 out_（OPT-12 单缓冲）。
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

#include "ast/ASTNode.h"
#include "common/RuntimeLimits.h"
#include "interpreter/Visitor.h"
#include "lexer/Token.h"
#include <memory>
#include <string>
#include <vector>

// ARCH-01 fix: 移除不必要的 #include "interpreter/Value.h"。
// Formatter 的 visit 方法返回 void，输出写入 out_ 缓冲，
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
class TupleLiteral;
class DestructureBinding;
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
    SAME_LINE, // K&R 风格：if (x) {
    NEXT_LINE  // Allman 风格：if (x)\n{
};

/// 格式化选项
struct FormatOptions {
    int indentSize = 4;   // 缩进空格数
    bool useTabs = false; // 使用 tab 缩进（true 时忽略 indentSize）
    BraceStyle braceStyle = BraceStyle::SAME_LINE;
    bool spaceAroundOperators = true;      // 二元运算符两侧加空格
    bool spaceAfterComma = true;           // 逗号后加空格
    bool blankLineBetweenFunctions = true; // 函数/类声明之间加空行
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
/// visit* 方法把格式化文本直接追加到成员输出缓冲 out_（OPT-12 单缓冲）。
class Formatter : public DefaultVisitor {
public:
    Formatter();

    // ---- Visitor 模式：visit* 方法（override）----
    // 每个 visit* 方法把对应节点的格式化文本追加到 out_（OPT-12 前为
    // 存入 lastFormatResult_），返回 void。统一 AST 分派为 Visitor 模式，
    // 替代原 25 路 switch。
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
    // R98 元组与解构
    void visitTupleLiteral(TupleLiteral& node) override;
    void visitDestructureBinding(DestructureBinding& node) override;
    // R99 枚举与 ADT + match
    void visitEnumDecl(EnumDecl& node) override;
    void visitEnumVariantExpr(EnumVariantExpr& node) override;
    void visitMatchExpr(MatchExpr& node) override;
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
    // AUDIT-R6 F7 fix: R164 yield 表达式（原缺失 override，yield 输出为残留文本）
    void visitYieldExpr(YieldExpr& node) override;
    // 七特性 MVP 阶段 2：宏声明与宏调用（打印原始语法，往返等价）
    void visitMacroDecl(MacroDecl& node) override;
    void visitMacroCallExpr(MacroCallExpr& node) override;
    // 七特性 MVP 阶段 3：trait 声明（打印完整 trait 体，往返等价）
    void visitTraitDecl(TraitDecl& node) override;
    // 七特性 MVP 阶段 4：await 表达式（打印 await <operand>）
    void visitAwaitExpr(AwaitExpr& node) override;
    void visitImportStmt(ImportStmt& node) override;
    void visitExportStmt(ExportStmt& node) override;
    void visitInterpolatedString(InterpolatedString& node) override; // C5 fix

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
    int currentIndent_ = 0; // 当前缩进级别
    int formatDepth_ = 0;   // D5 fix: 格式化递归深度计数器
    static constexpr int MAX_FORMAT_DEPTH = RuntimeLimits::MAX_FORMAT_DEPTH;
    std::vector<Token> comments_; // F1 fix: 注释 token 列表
    size_t commentIndex_ = 0;     // 当前注释游标

    // OPT-12: 单输出缓冲。所有 visit*/format* 方法把文本追加到 out_，
    // 不再为每个 AST 节点生成独立临时串（旧 lastFormatResult_ 架构在深层
    // 嵌套表达式下产生 O(深度×长度) 的分配/拷贝）。缓冲契约：
    //   - 公共入口 format() 负责清空、取出并返回 out_（O(1) swap）；
    //   - formatNode/format* 只向 out_ 尾部追加，不读取/改写已写内容。
    //     唯一例外：formatBlock 对刚追加的独立块语句删除其首字符前导空格；
    //   - 需要给子表达式包括号时，先按 AST 节点类型判定（needsParens 等为
    //     纯函数），再在追加子节点前写入 '('，避免中途插入；
    //   - formatNode 用追加前后的长度差判断 visit* 是否产出文本
    //     （替代旧 lastFormatResult_.empty() 检查），长度不变时补
    //     "/* unhandled node */"。
    std::string out_;

    // AUDIT-P2.4 fix: 多行节点（如插值字符串）的结束行号。
    // formatInterpolatedString 设置为 node.endLine；formatBlock 在 formatNode 调用前
    // 重置为 stmt->line。同行注释循环用 <= lastNodeEndLine_ 替代 == stmt->line，
    // 消费多行语句结束行上的注释（如多行插值字符串闭合引号后的尾注）。
    int lastNodeEndLine_ = 0;

    // P3 fix: 缓存频繁生成的小字符串，避免重复分配（mutable 因为 indent() 是 const）
    mutable std::string indentCache_;    // 当前缩进字符串缓存
    mutable int cachedIndentLevel_ = -1; // 缓存对应的缩进级别
    std::string commaCache_;             // 逗号分隔符缓存
    bool commaCacheValid_ = false;
    mutable std::string binOpKey_; // binOp 缓存键（上次使用的运算符）
    mutable std::string binOpVal_; // binOp 缓存值

    /// 生成缩进字符串（P3: 带缓存）
    std::string indent() const;

    /// 生成二元运算符（根据 spaceAroundOperators 选项）
    std::string binOp(const std::string& op) const;

    /// 生成逗号分隔符（P3: 带缓存）
    std::string comma();

    /// 生成开括号（根据 braceStyle 选项）
    std::string openBrace() const;

    /// 格式化 AST 节点（通过 Visitor 模式分派到 visit* 方法，文本追加到 out_）
    void formatNode(ASTNode* node);

    /// 格式化各种节点类型（文本追加到 out_）
    void formatBinaryOp(BinaryOp& node);
    void formatUnaryOp(UnaryOp& node);
    void formatNumberLiteral(NumberLiteral& node);
    void formatStringLiteral(StringLiteral& node);
    void formatBoolLiteral(BoolLiteral& node);
    void formatVarDecl(VarDecl& node);
    void formatAssignment(Assignment& node);
    void formatVarRef(VarRef& node);
    void formatIfStmt(IfStmt& node);
    void formatWhileStmt(WhileStmt& node);
    void formatForStmt(ForStmt& node);
    void formatFunDecl(FunDecl& node);
    void formatFunCall(FunCall& node);
    void formatReturnStmt(ReturnStmt& node);
    void formatPrintStmt(PrintStmt& node);
    void formatBlock(Block& node);

    // 新增节点格式化
    void formatArrayLiteral(ArrayLiteral& node);
    void formatDictLiteral(DictLiteral& node);
    // R98 元组与解构
    void formatTupleLiteral(TupleLiteral& node);
    void formatDestructureBinding(DestructureBinding& node);
    // R99 枚举与 ADT + match
    void formatEnumDecl(EnumDecl& node);
    void formatEnumVariantExpr(EnumVariantExpr& node);
    void formatMatchExpr(MatchExpr& node);
    // R134: 递归格式化 match pattern（6 种：WILDCARD/LITERAL/VARIABLE/VARIANT/TUPLE/OR）
    void formatMatchPattern(const MatchPattern& p);
    void formatIndexAccess(IndexAccess& node);
    void formatIndexAssign(IndexAssign& node);
    void formatClassDecl(ClassDecl& node);
    void formatMemberAccess(MemberAccess& node);
    void formatMemberAssign(MemberAssign& node);
    void formatMethodCall(MethodCall& node);
    void formatNullLiteral(NullLiteral& node);
    void formatInterpolatedString(InterpolatedString& node); // C5 fix
};
