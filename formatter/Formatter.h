#pragma once

#include <string>
#include <memory>

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

/// 将 AST 重新格式化为标准代码文本
class Formatter {
public:
    Formatter();

    /// 格式化 AST 为代码文本
    std::string format(Block& program);

    /// 设置缩进大小
    void setIndentSize(int size);

private:
    int indentSize_ = 4;         // 缩进空格数
    int currentIndent_ = 0;      // 当前缩进级别

    /// 生成缩进字符串
    std::string indent() const;

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
    std::string formatBlock(Block& node, bool isTopLevel = false);

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
