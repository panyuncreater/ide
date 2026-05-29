#pragma once

#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include "lexer/Token.h"
#include "ast/ASTNode.h"

// ============================================================
// Parser 语法分析器
// ============================================================

/// 语法错误异常
class ParseError : public std::runtime_error {
public:
    int line;
    int column;

    ParseError(const std::string& msg, int ln = 0, int col = 0)
        : std::runtime_error(msg), line(ln), column(col) {}
};

/// 递归下降语法分析器
class Parser {
public:
    Parser();

    /// 解析 Token 流，返回 AST 根节点（Block）
    std::unique_ptr<Block> parse(const std::vector<Token>& tokens);

private:
    std::vector<Token> tokens_;     // Token 流
    int current_ = 0;               // 当前位置

    // ---- 辅助方法 ----

    /// 当前 Token
    const Token& peek() const;

    /// 前一个 Token
    const Token& previous() const;

    /// 是否到达末尾
    bool isAtEnd() const;

    /// 前进一个 Token，返回前一个 Token
    const Token& advance();

    /// 检查当前 Token 是否为指定类型
    bool check(TokenType type) const;

    /// 如果当前 Token 匹配任一类型则前进
    bool match(std::initializer_list<TokenType> types);

    /// 消耗当前 Token，必须匹配指定类型，否则抛异常
    const Token& consume(TokenType type, const std::string& message);

    // ---- 声明与语句 ----

    /// 声明（变量声明 / 类型注解声明 / 函数声明 / 类声明 / 语句）
    std::unique_ptr<ASTNode> declaration();

    /// 变量声明: var name = expr;
    std::unique_ptr<VarDecl> varDecl();

    /// 带类型注解的变量声明: int a = 10; int[] arr = [1,2,3]; dict d = {"a":1};
    std::unique_ptr<VarDecl> typedVarDecl(const std::string& typeAnn);

    /// 函数声明: fun name(params) { body } 或 function name(params): type { body }
    std::unique_ptr<FunDecl> funDecl();

    /// 类声明: class Name { members } 或 class Name extends Super { members }
    std::unique_ptr<ClassDecl> classDecl();

    /// 语句
    std::unique_ptr<ASTNode> statement();

    /// if 语句
    std::unique_ptr<IfStmt> ifStmt();

    /// while 语句
    std::unique_ptr<WhileStmt> whileStmt();

    /// for 语句
    std::unique_ptr<ForStmt> forStmt();

    /// return 语句
    std::unique_ptr<ReturnStmt> returnStmt();

    /// print 语句
    std::unique_ptr<PrintStmt> printStmt();

    /// 代码块 { ... }
    std::unique_ptr<Block> block();

    /// 表达式语句（赋值）
    std::unique_ptr<ASTNode> expressionStatement();

    // ---- 表达式（按优先级从低到高）----

    /// 表达式入口
    std::unique_ptr<ASTNode> expression();

    /// 赋值（支持索引赋值和成员赋值）
    std::unique_ptr<ASTNode> assignment();

    /// or
    std::unique_ptr<ASTNode> or_();

    /// and
    std::unique_ptr<ASTNode> and_();

    /// 相等性 == !=
    std::unique_ptr<ASTNode> equality();

    /// 比较 < > <= >=
    std::unique_ptr<ASTNode> comparison();

    /// 加减 + -
    std::unique_ptr<ASTNode> term();

    /// 乘除 * / %
    std::unique_ptr<ASTNode> factor();

    /// 一元 not -
    std::unique_ptr<ASTNode> unary();

    /// 调用 fun(args)、obj.method(args)、arr[index]、obj.field
    std::unique_ptr<ASTNode> call();

    /// 基本字面量 / 标识符 / 分组
    std::unique_ptr<ASTNode> primary();

    // ---- 错误恢复 ----

    /// 同步到下一个声明边界
    void synchronize();
};
