#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "ast/ASTNode.h"
#include "compiler/Bytecode.h"

// ============================================================
// Compiler 字节码编译器
// ============================================================

/// 将 AST 编译为字节码
class Compiler {
public:
    Compiler();

    /// 编译 AST 块到字节码
    CompileResult compile(Block& program);

    /// 获取编译错误信息
    std::string getLastError() const;

private:
    BytecodeChunk chunk_;                           // 当前字节码块
    std::unordered_map<std::string, uint16_t> varIndex_;  // 变量名 → 常量池索引
    std::string lastError_;                         // 最近一次编译错误
    std::unordered_map<std::string, BytecodeChunk> functionChunks_;  // 函数字节码块
    std::unordered_map<std::string, int> currentLocals_;  // 当前函数的局部变量槽位映射
    bool inFunction_ = false;                       // 是否在函数体内
    std::unordered_map<std::string, std::vector<std::string>> classFieldNames_;  // 类名 → 字段名列表（含继承字段）
    std::unordered_map<std::string, int> outerLocals_;  // 外层函数的局部变量（用于检测闭包捕获）

    /// 添加变量名到常量池，返回索引
    uint16_t identifierIndex(const std::string& name);

    /// 编译 AST 节点
    void compileNode(ASTNode* node);

    /// 编译各个节点类型
    void compileBinaryOp(BinaryOp& node);
    void compileUnaryOp(UnaryOp& node);
    void compileNumberLiteral(NumberLiteral& node);
    void compileStringLiteral(StringLiteral& node);
    void compileBoolLiteral(BoolLiteral& node);
    void compileVarDecl(VarDecl& node);
    void compileAssignment(Assignment& node);
    void compileVarRef(VarRef& node);
    void compileIfStmt(IfStmt& node);
    void compileWhileStmt(WhileStmt& node);
    void compileForStmt(ForStmt& node);
    void compileFunDecl(FunDecl& node);
    void compileFunCall(FunCall& node);
    void compileReturnStmt(ReturnStmt& node);
    void compilePrintStmt(PrintStmt& node);
    void compileBlock(Block& node);

    // 新增节点编译
    void compileArrayLiteral(ArrayLiteral& node);
    void compileDictLiteral(DictLiteral& node);
    void compileIndexAccess(IndexAccess& node);
    void compileIndexAssign(IndexAssign& node);
    void compileClassDecl(ClassDecl& node);
    void compileMemberAccess(MemberAccess& node);
    void compileMemberAssign(MemberAssign& node);
    void compileMethodCall(MethodCall& node);
    void compileNullLiteral(NullLiteral& node);

    /// 发出编译错误
    void error(const std::string& msg, int line, int col);
};
