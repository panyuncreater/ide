#pragma once

#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include "lexer/Token.h"
#include "ast/ASTNode.h"
#include "Diagnostic.h"
#include "common/RuntimeLimits.h"

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
    /// 构造函数
    Parser();

    /// 解析 Token 流，返回 AST 根节点（Block）
    std::unique_ptr<Block> parse(const std::vector<Token>& tokens);

    /// 是否有解析错误
    bool hasErrors() const { return diagnostics_.hasErrors(); }

    /// 获取解析过程中的诊断信息
    const DiagnosticBag& getDiagnostics() const { return diagnostics_; }

    // C4 fix: 统一的递归深度 RAII 守卫，消除 6 处局部结构体重复定义。
    // 构造时递增深度计数器，析构时递减（异常路径也自动递减，防止深度计数泄漏）。
    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    };

private:
    const std::vector<Token>* tokens_ = nullptr;  // Token 流（引用，避免深拷贝）
    int current_ = 0;                        // 当前位置
    DiagnosticBag diagnostics_;      // 诊断收集器
    int parseDepth_ = 0;             // P15 fix: 递归深度计数器
    int blockDepth_ = 0;             // P0-1 fix: 块嵌套深度计数器
    static constexpr int MAX_PARSE_DEPTH = RuntimeLimits::MAX_PARSE_DEPTH;
    static constexpr int MAX_BLOCK_DEPTH = RuntimeLimits::MAX_BLOCK_DEPTH;

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

    /// 检查下一个 Token 是否为指定类型（用于 -> 语法检测）
    bool checkNext(TokenType type) const;

    /// 如果当前 Token 匹配任一类型则前进（C++17 折叠表达式，零分配）
    template<typename... Ts>
    bool match(Ts... types) {
        return ((check(static_cast<TokenType>(types)) ? (advance(), true) : false) || ...);
    }

    /// 消耗当前 Token，必须匹配指定类型，否则抛异常
    const Token& consume(TokenType type, const std::string& message);

    /// 消耗标识符或类型关键字（允许 dict/array/int/float/string/bool 作为名称）
    const Token& consumeIdentifierOrType(const std::string& message);

    /// 检查当前 token 是否是标识符或类型关键字
    bool isIdentifierOrType() const;

    /// 检查当前 token 是否是类型关键字（int/float/bool/string/dict/array）
    /// 消除 6 处重复的类型关键字检查模式
    bool isTypeKeyword() const;

    /// 检查当前位置是否是类类型声明起始：ClassName paramName 或 ClassName[] paramName
    /// 用于 parseParamList / forStmt 初始化中识别 Point p / Point[] arr 模式
    bool isClassTypeDeclStart() const;

    /// 解析类型注解（内置类型或类名，可选 [] 数组后缀）
    /// 当前 token 必须是类型关键字或标识符，消耗类型 token 并可选地消耗 []
    /// 使用安全回溯：仅在 [ 后紧跟 ] 时才消费，否则回退 [
    /// 返回如 "int", "int[]", "ClassName", "ClassName[]" 等字符串
    std::string parseTypeAnnotation();

    // P2-1 fix: 移除未实现的 parseReturnType() 声明（死代码）
    // 实际的 -> type 解析逻辑在 funDecl() 和 classDecl() 中内联实现

    // ---- 声明与语句 ----

    /// 声明（变量声明 / 类型注解声明 / 函数声明 / 类声明 / 语句）
    std::unique_ptr<ASTNode> declaration();

    /// 变量声明: var name = expr;
    std::unique_ptr<VarDecl> varDecl();

    /// 带类型注解的变量声明: int a = 10; int[] arr = [1,2,3]; dict d = {"a":1};
    std::unique_ptr<VarDecl> typedVarDecl(const std::string& typeAnn);

    /// 函数声明: fun name(params) { body } 或 function name(params): type { body }
    std::unique_ptr<FunDecl> funDecl();

    /// 带返回类型的函数声明: int fib(int n) { ... }
    std::unique_ptr<FunDecl> typedFunDecl(const std::string& returnType);

    /// 类声明: class Name { members } 或 class Name extends Super { members }
    std::unique_ptr<ClassDecl> classDecl();

    /// 解析参数列表（支持C风格类型注解 int a 和冒号风格 a: int）
    /// 假设调用前已 consume '('
    /// F10: defaultValues 收集默认参数值表达式（与 params 一一对应，无默认值时为 nullptr）
    void parseParamList(std::vector<std::string>& params, std::vector<std::string>& paramTypes,
                        std::vector<std::shared_ptr<ASTNode>>& defaultValues);

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

    /// break 语句
    std::unique_ptr<BreakStmt> breakStmt();

    /// continue 语句
    std::unique_ptr<ContinueStmt> continueStmt();

    /// try-catch 语句
    std::unique_ptr<TryStmt> tryStmt();

    /// throw 语句
    std::unique_ptr<ThrowStmt> throwStmt();

    /// import 语句
    std::unique_ptr<ImportStmt> importStmt();

    /// export 语句
    std::unique_ptr<ExportStmt> exportStmt();

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

    /// F7: 解析插值字符串 "Hello {name}, age {age}"
    /// 将字符串字面量和表达式拼接为 BinaryOp(BIN_ADD) 链
    /// @param first 第一个字符串片段（已解析的 StringLiteral）
    std::unique_ptr<ASTNode> parseInterpolatedString(std::unique_ptr<ASTNode> first);

    // ---- 错误恢复 ----

    /// 同步到下一个声明边界
    void synchronize();
};

