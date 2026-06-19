#pragma once

#include <string>
#include <memory>
#include <vector>
#include "ast/ASTNode.h"
#include "lexer/Token.h"

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
    bool semicolons = true;            // 语句末尾加分号

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
class Formatter {
public:
    Formatter();

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
    static constexpr int MAX_FORMAT_DEPTH = 256;  // D5 fix: 最大嵌套深度
    std::vector<Token> comments_; // F1 fix: 注释 token 列表
    size_t commentIndex_ = 0;    // 当前注释游标

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

    /// 格式化 AST 节点
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
};
