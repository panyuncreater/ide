#pragma once

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "interpreter/Visitor.h"
#include "ast/ASTNode.h"
#include "Diagnostic.h"

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

/// break 语句专用异常（用于跳出循环体）
class BreakException : public std::runtime_error {
public:
    BreakException() : std::runtime_error("break") {}
};

/// continue 语句专用异常（用于跳到循环下一次迭代）
class ContinueException : public std::runtime_error {
public:
    ContinueException() : std::runtime_error("continue") {}
};

/// throw 语句专用异常（用于 try/catch 捕获）
class ThrowException : public std::runtime_error {
public:
    Value thrownValue;

    ThrowException(Value val)
        : std::runtime_error("throw"), thrownValue(std::move(val)) {}
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
    std::shared_ptr<Environment> closureEnv;           // O5: 类定义时的环境（闭包捕获）
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

    /// REPL 模式下保留 AST 所有权（确保 classRegistry_/funRegistry_ 中的裸指针持续有效）
    void retainReplAst(std::unique_ptr<Block> ast);

    /// 保存 REPL 状态（Run 前调用），Run 结束后调用 restoreReplState() 恢复
    void saveReplState();
    void restoreReplState();

    /// 设置输出回调
    void setOutputCallback(std::function<void(const std::string&)> callback);

    /// 设置输入回调（用于 input() 函数）
    /// 回调接收提示字符串，返回用户输入的字符串
    void setInputCallback(std::function<std::string(const std::string&)> callback);

    /// F12: 设置模块加载回调（用于 import 语句）
    /// 回调接收模块路径，返回模块源代码内容。若模块不存在则返回空字符串。
    void setModuleLoader(std::function<std::string(const std::string&)> loader);

    /// F12: 设置当前文件路径（用于解析相对 import 路径）
    void setCurrentFilePath(const std::string& path);

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

    /// GUI-03 fix: 安全求值条件断点表达式（保存/恢复所有可变状态，防止重入损坏）
    Value evaluateCondition(ASTNode* node);

    /// 获取诊断信息
    const DiagnosticBag& getDiagnostics() const { return diagnostics_; }

    /// 获取诊断信息（简短访问器）
    const DiagnosticBag& diagnostics() const { return diagnostics_; }

    /// 清空诊断信息
    void clearDiagnostics() { diagnostics_.clear(); }

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
    Value visitSuperExpr(SuperExpr& node) override;
    Value visitBreakStmt(BreakStmt& node) override;
    Value visitContinueStmt(ContinueStmt& node) override;
    Value visitTryStmt(TryStmt& node) override;
    Value visitThrowStmt(ThrowStmt& node) override;
    Value visitImportStmt(ImportStmt& node) override;
    Value visitExportStmt(ExportStmt& node) override;

private:
    // 运行时限制常量（替代散布在代码中的魔法数字）
    // S-10 fix: 与 VM 的 MAX_FRAMES=256 对齐，避免正常递归程序被误杀
    static constexpr int MAX_RECURSION_DEPTH = 256;      // 最大递归深度
    static constexpr int MAX_INHERITANCE_DEPTH = 64;    // 最大继承链深度
    // S-01 fix: 循环迭代次数上限，防止 while(true){} 等无限循环导致 DoS
    // P0-13 fix: 从 1 亿降至 1000 万，将单次循环 CPU 占用从 2-3 秒降至 ~0.3 秒
    static constexpr int64_t MAX_LOOP_ITERATIONS = 10000000;  // 1000 万次（约 0.3 秒）

    std::shared_ptr<Environment> globalEnv_;        // 全局环境
    std::shared_ptr<Environment> currentEnv_;       // 当前环境
    std::vector<CallFrame> callStack_;              // 调用栈
    DebugController* debugger_;                     // 调试控制器（可为 nullptr）
    bool debugMode_ = false;                        // 是否处于调试模式（快速跳过 checkBreak）
    std::function<void(const std::string&)> outputCallback_; // 输出回调
    std::function<std::string(const std::string&)> inputCallback_; // 输入回调（input() 函数）
    std::function<std::string(const std::string&)> moduleLoader_; // F12: 模块加载回调
    std::string currentFilePath_;                             // F12: 当前文件路径
    std::unordered_map<std::string, std::shared_ptr<Environment>> moduleCache_; // F12: 模块缓存
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExports_; // F12: 模块导出名称缓存
    std::vector<std::string> moduleLoadingStack_;             // F12: 模块加载栈（循环依赖检测）
    std::unordered_set<std::string> exportedNames_;           // F12: 当前模块的导出名称集合
    DiagnosticBag diagnostics_;                        // 诊断收集器
    int recursionDepth_ = 0;                        // 递归深度
    std::unordered_map<std::string, FunDecl*> funRegistry_; // 函数注册表
    int funRegistryGen_ = 0;  // M7: 注册表代数，函数重定义时递增使 FunCall 缓存失效
    std::unordered_map<std::string, ClassInfo> classRegistry_; // 类注册表
    std::vector<std::string> classContextStack_; // super 解析用：当前执行的方法所属类名栈
    std::string currentFunctionReturnType_;         // 当前函数的返回类型
    std::vector<std::unique_ptr<Block>> replAsts_;  // REPL 模式下保留 AST，确保 funRegistry_/classRegistry_ 指针有效

    // REPL 状态暂存（saveReplState/restoreReplState）
    std::shared_ptr<Environment> savedGlobalEnv_;
    std::unordered_map<std::string, ClassInfo> savedClassRegistry_;
    std::vector<std::unique_ptr<Block>> savedReplAsts_;
    // BUG7 fix: 保存 funRegistry_ 以避免 Run→REPL 切换后函数注册表丢失
    std::unordered_map<std::string, FunDecl*> savedFunRegistry_;
    int savedFunRegistryGen_ = 0;
    // P1-1 fix: 模块相关状态暂存（避免 Run→REPL 切换后悬垂指针）
    std::unordered_map<std::string, std::shared_ptr<Environment>> savedModuleCache_;
    std::unordered_map<std::string, std::unordered_set<std::string>> savedModuleExports_;
    std::unordered_set<std::string> savedExportedNames_;
    std::vector<std::string> savedModuleLoadingStack_;

    /// 执行单个节点
    Value evaluate(ASTNode* node);

    /// 检查调试断点
    void checkBreak(ASTNode* node);

    /// 输出字符串
    void output(const std::string& text);

    /// 数值二元运算（含类型提升）
    Value numericBinaryOp(BinOpType opType, const Value& left, const Value& right,
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

    /// 链收集与求值结果
    struct ChainInfo {
        std::vector<ASTNode*> chain;
        std::vector<Value> vals;
        std::vector<Value> idxs;
        VarRef* varRef;  // nullptr if root is not a VarRef
    };

    /// 收集从 objectNode 到 VarRef 的节点链，并从外到内逐级求值
    /// errorOnNonVarRef=true 时，非 VarRef 根节点报错；否则 varRef 设为 nullptr
    ChainInfo collectAndEvaluateChain(ASTNode* objectNode, bool errorOnNonVarRef, int line, int col);

    /// 从内到外逐级写回修改后的值，最终写回变量
    // P1 fix: 改为非 const 引用，使 std::move 真正生效（const T&& 退化为拷贝）
    void writeBackChain(ChainInfo& info, Value innermost, int line, int col);

    /// 检查值是否匹配类型注解
    bool typeMatch(const Value& val, const std::string& annotation) const;

    /// 类型检查，不匹配则报运行时错误
    /// P20 fix: 模板化 contextBuilder 消除 std::function 堆分配
    template<typename ContextBuilder>
    void checkType(const Value& val, const std::string& annotation,
                   ContextBuilder&& contextBuilder, int line, int col) {
        if (!typeMatch(val, annotation)) {
            runtimeError(contextBuilder() + " 期望类型 " + annotation + "，实际为 " + val.typeName(), line, col);
        }
    }

    /// 查找变量的类型注解（返回指针，避免字符串拷贝）
    const std::string* findTypeAnnotation(const std::string& varName) const;

    // ---- P1 重构：visitFunCall 分派器辅助方法 ----

    /// 链式调用 / 表达式调用 callee(args)
    Value callClosureValue(FunCall& node);

    /// 内置构造函数 dict() / array()
    Value callBuiltinConstructor(FunCall& node);

    /// 顶层内置函数 len/type/str/int/abs/min/max/range/sum
    Value callBuiltinFunction(FunCall& node);

    /// 类构造调用 ClassName(args)
    Value constructClassInstance(FunCall& node);

    /// 普通函数/闭包调用 name(args)
    Value callNamedFunction(FunCall& node);

    // ---- P1 重构：visitMethodCall 辅助方法 ----

    /// 类实例方法调用（含 super.method() 处理）
    Value callInstanceMethod(MethodCall& node, Value& obj);

    /// 求值参数列表（消除 visitMethodCall 中重复的参数求值逻辑）
    std::vector<Value> evaluateArguments(const std::vector<std::unique_ptr<ASTNode>>& args);
};
