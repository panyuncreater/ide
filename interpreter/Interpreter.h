#pragma once

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

#include "interpreter/Value.h"
#include "interpreter/Environment.h"
#include "interpreter/Visitor.h"
#include "interpreter/RuntimeExceptions.h"  // S6 fix: 异常类提取到独立头文件
#include "interpreter/CallFrame.h"  // ARCH-02 fix: CallFrame 拆出，避免传递依赖
#include "ast/ASTNode.h"
#include "Diagnostic.h"
#include "common/RuntimeLimits.h"

// ============================================================
// Interpreter 解释器
// ============================================================

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
    // A3 fix: shared_ptr 持有方法 AST 所有权，避免 AST 重建后裸指针悬垂
    std::unordered_map<std::string, std::shared_ptr<FunDecl>> methods; // 方法表
    std::unordered_map<std::string, Value> fields;     // 默认字段值
    std::shared_ptr<Environment> closureEnv;           // O5: 类定义时的环境（闭包捕获）
    // C6 fix: 方法分派缓存。沿继承链查找是 O(depth)，热路径上每次方法调用重复查找。
    // 缓存 methodName → (shared_ptr<FunDecl>, cacheGen_)。cacheGen_ 与 Interpreter::classRegistryGen_
    // 比较，不匹配则视为未命中（任何类重定义都会递增 gen，使全部缓存条目失效，
    // 解决子类缓存指向已被重定义父类的旧方法指针的悬垂问题）。
    // A3 fix: 缓存值改为 shared_ptr，与 methods 表所有权一致，避免悬垂。
    mutable std::unordered_map<std::string, std::pair<std::shared_ptr<FunDecl>, int>> methodCache_;
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

    /// P1-4 fix: execute/executeRepl 共享的语句执行 + 异常处理逻辑
    Value runStatementsWithExceptionHandling(Block& program);

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
    // MEM-01 fix: 改用 shared_ptr 共享所有权，使 worker 线程持有的 Interpreter
    // 保持 debugger 存活，避免 IdeController 析构后 debugger_ 悬垂。
    void setDebugger(std::shared_ptr<DebugController> dbg);

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

    // 新增 9 个 visit 方法
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

private:
    // 运行时限制常量 — 统一引用 common/RuntimeLimits.h
    // S1 fix: 消除散布在 7 个文件的重复定义，避免对齐遗漏
    static constexpr int MAX_RECURSION_DEPTH = RuntimeLimits::MAX_RECURSION_DEPTH;
    static constexpr int MAX_INHERITANCE_DEPTH = RuntimeLimits::MAX_INHERITANCE_DEPTH;
    static constexpr int64_t MAX_LOOP_ITERATIONS = RuntimeLimits::MAX_LOOP_ITERATIONS;

    std::shared_ptr<Environment> globalEnv_;        // 全局环境
    std::shared_ptr<Environment> currentEnv_;       // 当前环境
    std::vector<CallFrame> callStack_;              // 调用栈
    // PERF-07 fix: Environment 对象池。visitBlock 退出时若块作用域未被闭包捕获
    // （use_count==1），回收并 reset 后供下次 visitBlock 复用，避免重复堆分配。
    // execute() 开头清空（旧环境链已销毁，池中 Environment 可能被新链引用作 parent）。
    std::vector<std::shared_ptr<Environment>> envPool_;
    // MEM-01 fix: shared_ptr 共享所有权，worker 线程持有的 Interpreter 保持 debugger 存活
    std::shared_ptr<DebugController> debugger_;              // 调试控制器（可为 nullptr）
    // QT-R-02 fix: debugMode_ 改为 atomic，消除主线程 setDebugMode() 与 worker 线程
    // checkBreak() 读操作之间的数据竞争。A6 fix 已确保主线程不在 worker 运行时
    // 调用 setDebugMode，但 atomic 提供额外的内存可见性保证和防御性保护。
    std::atomic<bool> debugMode_{false};            // 是否处于调试模式（快速跳过 checkBreak）
    std::function<void(const std::string&)> outputCallback_; // 输出回调
    std::function<std::string(const std::string&)> inputCallback_; // 输入回调（input() 函数）
    std::function<std::string(const std::string&)> moduleLoader_; // F12: 模块加载回调
    // A6 fix: callback 跨线程 mutex 保护。同一 Interpreter 实例被 worker 线程（execute）
    // 和主线程（REPL/条件断点求值/callback 设置）同时访问，std::function 成员无 mutex
    // 保护会导致数据竞争。setter 加锁写入，invocation 加锁拷贝后解锁调用（避免持锁回调）。
    mutable std::mutex callbackMutex_;
    std::string currentFilePath_;                             // F12: 当前文件路径
    std::unordered_map<std::string, std::shared_ptr<Environment>> moduleCache_; // F12: 模块缓存
    std::unordered_map<std::string, std::unordered_set<std::string>> moduleExports_; // F12: 模块导出名称缓存
    std::vector<std::string> moduleLoadingStack_;             // F12: 模块加载栈（顺序管理 + 深度保护）
    std::unordered_set<std::string> moduleLoadingSet_;        // D19 fix: 模块加载集合（O(1) 循环依赖检测，与 moduleLoadingStack_ 同步维护）
    std::unordered_set<std::string> exportedNames_;           // F12: 当前模块的导出名称集合
    DiagnosticBag diagnostics_;                        // 诊断收集器
    int recursionDepth_ = 0;                        // 递归深度
    // A3 fix: shared_ptr 持有函数 AST 所有权，避免 AST 重建后裸指针悬垂
    std::unordered_map<std::string, std::shared_ptr<FunDecl>> funRegistry_; // 函数注册表
    int funRegistryGen_ = 0;  // M7: 注册表代数，函数重定义时递增使 FunCall 缓存失效
    std::unordered_map<std::string, ClassInfo> classRegistry_; // 类注册表
    int classRegistryGen_ = 0;  // C6 fix: 类注册表代数，任何类定义/重定义时递增，使方法分派缓存失效
    std::vector<std::string> classContextStack_; // super 解析用：当前执行的方法所属类名栈
    std::string currentFunctionReturnType_;         // 当前函数的返回类型
    std::vector<std::unique_ptr<Block>> replAsts_;  // REPL 模式下保留 AST，确保 funRegistry_/classRegistry_ 指针有效

    // REPL 状态暂存（saveReplState/restoreReplState）
    std::shared_ptr<Environment> savedGlobalEnv_;
    std::unordered_map<std::string, ClassInfo> savedClassRegistry_;
    std::vector<std::unique_ptr<Block>> savedReplAsts_;
    // BUG7 fix: 保存 funRegistry_ 以避免 Run→REPL 切换后函数注册表丢失
    // A3 fix: 与 funRegistry_ 一致，使用 shared_ptr 持有所有权
    std::unordered_map<std::string, std::shared_ptr<FunDecl>> savedFunRegistry_;
    int savedFunRegistryGen_ = 0;
    // P1-1 fix: 模块相关状态暂存（避免 Run→REPL 切换后悬垂指针）
    std::unordered_map<std::string, std::shared_ptr<Environment>> savedModuleCache_;
    std::unordered_map<std::string, std::unordered_set<std::string>> savedModuleExports_;
    std::unordered_set<std::string> savedExportedNames_;
    std::vector<std::string> savedModuleLoadingStack_;
    std::unordered_set<std::string> savedModuleLoadingSet_;  // D19 fix: 与 savedModuleLoadingStack_ 配对

    // S2 fix: RAII 递归深度守卫 — 统一 constructClassInstance/callNamedFunction/callInstanceMethod
    // 的递归深度管理，消除手动递减在异常路径下的遗漏风险
    struct RecursionGuard {
        int& depth;
        bool dismissed = false;
        explicit RecursionGuard(int& d) : depth(d) { ++depth; }
        ~RecursionGuard() { if (!dismissed) --depth; }
        void dismiss() { dismissed = true; }
    };

    // B3 fix: RAII 调用帧守卫 — 统一 5 处调用帧状态管理（currentFunctionReturnType_ +
    // callStack_ + classContextStack_），消除异常路径和成功路径中重复的手动恢复代码。
    // 构造时保存 currentFunctionReturnType_ 并设置新值；析构时恢复 returnType，
    // 并将 callStack_/classContextStack_ 弹出到构造时的深度（处理异常路径自动清理）。
    // currentEnv_ 不由此守卫管理（各调用点的 env 保存/恢复时机不同）。
    struct CallFrameGuard {
        Interpreter& interp;
        std::string savedReturnType;
        size_t savedStackDepth;
        size_t savedClassContextDepth;
        bool manageClassContext;
        CallFrameGuard(Interpreter& i, const std::string& newReturnType, bool manageCtx = false)
            : interp(i), savedReturnType(i.currentFunctionReturnType_),
              savedStackDepth(i.callStack_.size()),
              savedClassContextDepth(i.classContextStack_.size()),
              manageClassContext(manageCtx) {
            interp.currentFunctionReturnType_ = newReturnType;
        }
        ~CallFrameGuard() {
            while (interp.callStack_.size() > savedStackDepth) {
                interp.callStack_.pop_back();
            }
            if (manageClassContext) {
                while (interp.classContextStack_.size() > savedClassContextDepth) {
                    interp.classContextStack_.pop_back();
                }
            }
            interp.currentFunctionReturnType_ = std::move(savedReturnType);
        }
        CallFrameGuard(const CallFrameGuard&) = delete;
        CallFrameGuard& operator=(const CallFrameGuard&) = delete;
    };

    /// 执行单个节点
    // A1 fix: Visitor::accept 返回 void，evaluate() 通过 lastValue_ 获取结果。
    // evaluate() 保持返回 Value 的签名，所有调用方无需修改。
    Value evaluate(ASTNode* node);
    Value lastValue_;  // A1 fix: visit 方法的结果载体（替代 accept 返回值）

    /// 检查调试断点
    void checkBreak(ASTNode* node);

    /// 输出字符串
    void output(const std::string& text);

    /// 数值二元运算（含类型提升）
    // PERF-06 fix: 改为按值接收，使调用方 move 临时 Value 进入，
    // 函数内可检测独占所有权（tryGetMutableString）做原地 append。
    Value numericBinaryOp(BinOpType opType, Value left, Value right,
                          int line, int col);

    /// P1-2 fix: 比较运算（LT/GT/LTE/GTE）共用模板，消除 4 处重复样板
    template<typename Cmp>
    Value compareNumericOrString(BinaryOp& node, Cmp cmp);

    /// 报告运行时错误
    [[noreturn]] void runtimeError(const std::string& msg, int line, int col);

    /// 查找类的方法（含继承链）
    FunDecl* findMethod(const ClassInfo& cls, const std::string& methodName);

    /// 查找类的字段默认值（含继承链）
    Value findFieldDefault(const ClassInfo& cls, const std::string& fieldName);

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
    std::vector<Value> evaluateArguments(const std::vector<std::shared_ptr<ASTNode>>& args);

    // ---- B1 fix: 闭包仅捕获自由变量（静态分析 AST）----

    /// 计算函数的自由变量集合（函数体引用但未在函数内定义的变量）。
    /// 用于 visitFunDecl 时仅捕获实际需要的变量，而非整个环境快照。
    /// 递归处理嵌套函数：嵌套函数的自由变量若在外层函数内定义则不算外层自由变量，
    /// 否则归入外层自由变量。类方法不分析（使用 closureEnv 而非 capturedVars）。
    std::unordered_set<std::string> computeFreeVariables(const FunDecl& fn);

    /// collectFreeVars 的递归辅助函数（作用于作用域栈）。
    void collectFreeVars(const ASTNode& node,
                         std::vector<std::unordered_set<std::string>>& scopes,
                         std::unordered_set<std::string>& freeVars);
};
