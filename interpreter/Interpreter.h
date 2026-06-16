#pragma once

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <memory>
#include <unordered_map>

#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "interpreter/Visitor.h"
#include "ast/ASTNode.h"

// ============================================================
// 运行时错误异常
// ============================================================

/// 运行时错误
class RuntimeError : public std::runtime_error {
public:
    int line;
    int column;

    RuntimeError(const std::string& msg, int ln = 0, int col = 0)
        : std::runtime_error(msg), line(ln), column(col) {}
};

/// return 语句专用异常（用于跳出函数体）
class ReturnException : public std::runtime_error {
public:
    Value returnValue;

    ReturnException(Value val)
        : std::runtime_error("return"), returnValue(std::move(val)) {}
};

/// 调试终止异常（用户点击停止按钮时抛出）
class DebugStopException : public std::exception {
public:
    const char* what() const noexcept override { return "调试终止"; }
};

// ============================================================
// 调用帧
// ============================================================

/// 函数调用帧
struct CallFrame {
    std::string functionName;                      // 函数名
    std::shared_ptr<Environment> env = nullptr;     // 该帧对应的环境
    int line = 0;                                   // 调用行号
    int depth = 0;                                  // 调用深度

    CallFrame() = default;
    CallFrame(const std::string& name, std::shared_ptr<Environment> e, int ln, int d)
        : functionName(name), env(e), line(ln), depth(d) {}
};

// ============================================================
// DebugController 前向声明
// ============================================================
class DebugController;

// ============================================================
// 类定义信息
// ============================================================

/// 类定义信息结构
struct ClassInfo {
    std::string name;                                  // 类名
    std::string superClassName;                         // 父类名（空表示无父类）
    std::unordered_map<std::string, FunDecl*> methods; // 方法表
    std::unordered_map<std::string, Value> fields;     // 默认字段值
    // 注意：不再存储 superClass 裸指针，运行时通过 superClassName 在 classRegistry_ 中查找
    // 避免 unordered_map rehash 导致指针悬空
};

// ============================================================
// Interpreter 解释器
// ============================================================

/// 访问者模式解释执行器
class Interpreter : public Visitor {
public:
    Interpreter();
    ~Interpreter();

    /// 执行程序（AST 根节点）
    Value execute(Block& program);

    /// REPL 模式执行（不重置环境，保留变量/函数/类定义）
    Value executeRepl(Block& program);

    /// 设置输出回调
    void setOutputCallback(std::function<void(const std::string&)> callback);

    /// 设置调试控制器
    void setDebugger(DebugController* dbg);

    /// 设置调试模式（启用/禁用 checkBreak 调用）
    void setDebugMode(bool enabled);

    /// 获取当前环境（用于调试面板）
    Environment* currentEnvironment() const;

    /// 获取调用栈（用于调试面板）
    const std::vector<CallFrame>& getCallStack() const;

    /// 在当前环境中求值单个表达式（用于条件断点，不触发调试检查）
    Value evaluateExpr(ASTNode* node);

    // ---- 25 个 visit 方法实现 ----

    Value visitBinaryOp(BinaryOp& node) override;
    Value visitUnaryOp(UnaryOp& node) override;
    Value visitNumberLiteral(NumberLiteral& node) override;
    Value visitStringLiteral(StringLiteral& node) override;
    Value visitBoolLiteral(BoolLiteral& node) override;
    Value visitVarDecl(VarDecl& node) override;
    Value visitAssignment(Assignment& node) override;
    Value visitVarRef(VarRef& node) override;
    Value visitIfStmt(IfStmt& node) override;
    Value visitWhileStmt(WhileStmt& node) override;
    Value visitForStmt(ForStmt& node) override;
    Value visitFunDecl(FunDecl& node) override;
    Value visitFunCall(FunCall& node) override;
    Value visitReturnStmt(ReturnStmt& node) override;
    Value visitPrintStmt(PrintStmt& node) override;
    Value visitBlock(Block& node) override;

    // 新增 9 个 visit 方法
    Value visitArrayLiteral(ArrayLiteral& node) override;
    Value visitDictLiteral(DictLiteral& node) override;
    Value visitIndexAccess(IndexAccess& node) override;
    Value visitIndexAssign(IndexAssign& node) override;
    Value visitClassDecl(ClassDecl& node) override;
    Value visitMemberAccess(MemberAccess& node) override;
    Value visitMemberAssign(MemberAssign& node) override;
    Value visitMethodCall(MethodCall& node) override;
    Value visitNullLiteral(NullLiteral& node) override;

private:
    std::shared_ptr<Environment> globalEnv_;        // 全局环境
    std::shared_ptr<Environment> currentEnv_;       // 当前环境
    std::vector<CallFrame> callStack_;              // 调用栈
    DebugController* debugger_;                     // 调试控制器（可为 nullptr）
    bool debugMode_ = false;                        // 是否处于调试模式（快速跳过 checkBreak）
    std::function<void(const std::string&)> outputCallback_; // 输出回调
    int recursionDepth_ = 0;                        // 递归深度
    std::unordered_map<std::string, FunDecl*> funRegistry_; // 函数注册表
    std::unordered_map<std::string, ClassInfo> classRegistry_; // 类注册表
    std::unordered_map<std::string, std::string> typeAnnotations_; // 变量类型注解
    std::string currentFunctionReturnType_;         // 当前函数的返回类型

    /// 执行单个节点
    Value evaluate(ASTNode* node);

    /// 检查调试断点
    void checkBreak(ASTNode* node);

    /// 输出字符串
    void output(const std::string& text);

    /// 数值二元运算（含类型提升）
    Value numericBinaryOp(const std::string& op, const Value& left, const Value& right,
                          int line, int col);

    /// 报告运行时错误
    [[noreturn]] void runtimeError(const std::string& msg, int line, int col);

    /// 查找类的方法（含继承链）
    FunDecl* findMethod(ClassInfo& cls, const std::string& methodName);

    /// 查找类的字段默认值（含继承链）
    Value findFieldDefault(ClassInfo& cls, const std::string& fieldName);

    /// 写回左值（链式求值，避免重复求值副作用）
    /// isIndexAssign=true 时为索引赋值，indexNode 为索引表达式节点；否则为成员赋值，fieldName 为字段名
    /// valueNode 为赋值右值表达式节点，在 writeBack 内部按 object→index→value 顺序求值
    Value writeBack(ASTNode* objectNode, bool isIndexAssign, ASTNode* indexNode,
                    const std::string& fieldName, ASTNode* valueNode, int line, int col);
    /// 写回已修改的值（用于方法调用等已自行修改对象的场景，链式求值避免重复求值）
    void writeBack(ASTNode* objectNode, const Value& modifiedValue, int line, int col);

    /// 检查值是否匹配类型注解
    bool typeMatch(const Value& val, const std::string& annotation) const;

    /// 类型检查，不匹配则报运行时错误
    void checkType(const Value& val, const std::string& annotation,
                   const std::string& context, int line, int col);

    /// 查找变量的类型注解
    std::string findTypeAnnotation(const std::string& varName) const;
};
