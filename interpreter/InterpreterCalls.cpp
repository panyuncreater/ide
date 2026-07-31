// ============================================================
// InterpreterCalls.cpp — 函数调用相关方法实现（visitFunCall 及其分派辅助方法）
// ============================================================
// S6 fix: 从 Interpreter.cpp 拆分，降低单文件复杂度。
// ============================================================

#include "common/ErrorFormat.h"    // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
#include "common/ErrorMessages.h"  // R97 #11 fix: 三后端共享错误消息常量
#include "common/RuntimeLimits.h"  // B1 TCO: MAX_LOOP_ITERATIONS 尾调用迭代上限
#include "debug/DebugController.h" // R104: checkFunctionBreakpoint 完整定义
#include "interpreter/BuiltinMethods.h"
#include "interpreter/Interpreter.h"
#include <unordered_set>

// 依赖说明：
// - BuiltinMethods.h：callBuiltinFunction 调用 executeSharedBuiltinFunction + Result<Value>
// - <unordered_set>：constructClassInstance 中的 visitedClasses 循环继承检测
// 其余类型（FunCall/FunDecl/Environment/RecursionGuard/ClassInfo 等）由 Interpreter.h 传递包含。

// ============================================================
// P1-6 fix: 闭包 capturedVars 快照重建/回写辅助函数
// ============================================================
// 消除 callClosureValue 和 callNamedFunction 中的重复逻辑。

namespace {

/// 从闭包的 capturedVars 快照重建环境（C1 fix: 闭包环境 weak_ptr 失效时使用）
void rebuildEnvFromSnapshot(Value& closureVal, std::shared_ptr<Environment>& funEnv) {
    funEnv = std::make_shared<Environment>(nullptr);
    // AUDIT-R6 F5 fix: 用 const 访问器读 capturedVars——非 const 重载的 ensureUnique 在
    // refCount>1（表达式 callee 如 fns[0] 求值产生的临时拷贝与容器内原闭包共享）时
    // 分离 ClosureData，后续 writeBackCapturedVars 写入的是临时副本，容器内原闭包的
    // capturedVars 保持陈旧，跨调用变异丢失（实证：Interpreter=1 vs VM=2）。
    const auto& captured = const_cast<const Value&>(closureVal).capturedVars();
    for (const auto& kv : captured) {
        funEnv->define(kv.first, kv.second);
        // AUDIT-P2-CORRECT fix: 标记为捕获变量，用于 writeBackCapturedVars 区分
        // "捕获变量被赋值修改"与"捕获变量被 var 重声明"
        funEnv->markAsCaptured(kv.first);
    }
}

/// 将变异后的捕获变量写回 capturedVars（C1/P0-6 fix）
/// 仅写回原始 capturedVars 中已存在的键，跳过参数和局部变量，避免污染下次调用
/// AUDIT-P2-CORRECT fix: 仅写回仍在 capturedVarNames_ 中的变量（未被 var 重声明的捕获变量），
/// 防止闭包内 var 重声明捕获变量后，重声明的局部变量被写回 capturedVars 污染下次调用。
void writeBackCapturedVars(Value& closureVal, std::shared_ptr<Environment>& funEnv) {
    // AUDIT-P1 fix: 应用 closeCapturedVariables 的 const_cast 模式绕过 COW。
    // capturedVars() 的非 const 重载会 ensureUnique → refCount>1 时创建副本，
    // 导致修改写到副本而非原件，closures 数组中的原始闭包 capturedVars 保持陈旧
    // （与 Environment.h 行 306-319 的 B1 fix 同一不变量）。
    const auto& constCaptured = const_cast<const Value&>(closureVal).capturedVars();
    auto& captured = const_cast<std::unordered_map<std::string, Value>&>(constCaptured);
    for (const auto& kv : funEnv->localVariables()) {
        // AUDIT-P2-CORRECT fix: 仅写回未被 var 重声明的捕获变量
        if (funEnv->isCapturedVar(kv.first) && captured.find(kv.first) != captured.end()) {
            captured[kv.first] = kv.second;
        }
    }
}

} // anonymous namespace

void Interpreter::visitFunCall(FunCall& node) {
    checkBreak(&node);

    // P1 重构：按调用类型分派到辅助方法
    if (node.callee) {
        lastValue_ = callClosureValue(node);
        return;
    }
    if (node.name == "dict" || node.name == "array") {
        lastValue_ = callBuiltinConstructor(node);
        return;
    }
    if (classRegistry_.find(node.name) != classRegistry_.end()) {
        lastValue_ = constructClassInstance(node);
        return;
    }
    // input() 函数（需要回调，单独处理）
    // E3 fix: 改用共享层 executeSharedInput，统一与 VM 的 input() 语义。
    // WorkerManager 的超时回调会抛 std::runtime_error，被 executeSharedInput
    // 捕获并附上调用点行号/列号上抛 RuntimeError，调用方得到明确的错误信息
    // 而非静默返回空串继续执行。
    if (node.name == "input") {
        std::vector<Value> argValues;
        argValues.reserve(node.arguments.size());
        // P2-3 fix: bad_alloc 转化为带行号的 RuntimeError，便于用户定位
        try {
            for (auto& arg : node.arguments) {
                argValues.push_back(evaluate(arg.get()));
            }
        } catch (const std::bad_alloc&) {
            runtimeError("内存不足：input() 参数收集失败", node.line, node.column);
        }
        // A6 fix: 加锁拷贝 callback 后解锁调用，避免持锁回调导致死锁
        std::function<std::string(const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            cb = inputCallback_;
        }
        auto r = executeSharedInput(cb, argValues.data(), argValues.size(), node.line, node.column);
        if (r.is_err()) {
            throw to_runtime_error(r); // A1 fix: 自由函数模板
        }
        lastValue_ = std::move(r.value());
        return;
    }
    // 顶层内置函数（用户自定义函数/类优先，仅当未定义时才使用内置）
    if (isBuiltinFunction(node.name)) {
        const Value* calleePtr = currentEnv_->get(node.name);
        if (!calleePtr || !calleePtr->isClosure()) {
            lastValue_ = callBuiltinFunction(node);
            return;
        }
    }
    // R98 W2: 高阶内置函数 map/filter/reduce/forEach/find
    // 必须在 isBuiltinFunction 之后、callNamedFunction 之前拦截，
    // 因为用户可能定义同名函数覆盖内置（与 isBuiltinFunction 检查一致：
    // 仅当环境中无同名闭包时才使用内置）。
    if (isHigherOrderBuiltin(node.name)) {
        const Value* calleePtr = currentEnv_->get(node.name);
        if (!calleePtr || !calleePtr->isClosure()) {
            lastValue_ = callHigherOrderBuiltin(node);
            return;
        }
    }
    // R136: spawn(fn, args...) — 需要后端注入 ClosureInvoker，走独立路径
    // 与 input()/高阶函数一样，用户可定义同名函数覆盖（仅在环境中无同名闭包时拦截）
    if (node.name == "spawn") {
        const Value* calleePtr = currentEnv_->get(node.name);
        if (!calleePtr || !calleePtr->isClosure()) {
            lastValue_ = callSpawnBuiltin(node);
            return;
        }
    }
    lastValue_ = callNamedFunction(node);
    return;
}

// ---- P1 重构：链式调用 / 表达式调用 callee(args) ----
Value Interpreter::callClosureValue(FunCall& node) {
    Value calleeVal = evaluate(node.callee.get());
    if (!calleeVal.isClosure()) {
        runtimeError("表达式求值结果不是函数，无法调用", node.line, node.column);
    }

    FunDecl* funDecl = calleeVal.closureBody();
    auto closureEnv = calleeVal.closureEnv();
    // AUDIT-R6 F5 fix（根因）: 用 const 重载读 closureName——非 const 重载的 ensureUnique
    // 在 refCount>1（calleeVal 与容器内原闭包共享）时静默分离 ClosureData，后续
    // rebuild/writeBack 全部作用于分离副本，跨调用变异丢失（实证：数组元素
    // cd 与 rebuild cd 不同；Interpreter=11 vs VM=12）。命名路径（const 引用）无此问题。
    std::string effectiveName = const_cast<const Value&>(calleeVal).closureName();

    // R104 Function Breakpoint：函数调用入口检查（闭包调用路径）
    // AUDIT-R4 BUG-15 fix: atomic load 到局部变量
    if (auto dbg = debugger()) {
        dbg->checkFunctionBreakpoint(effectiveName, node.line);
    }

    // BUG-DBG-AUDIT-1 fix: VM 后端创建的闭包（OP_CLOSURE / REG_CLOSURE）仅持有
    // vmClosure（VMClosureData，含 chunkPtr）而无 AST body（closureBody() 返回 nullptr）。
    // 当条件断点求值沙箱注入 VM 全局变量（含此类闭包）后，条件表达式中调用该闭包会
    // 在下方 funDecl->requiredParamCount 处解引用空指针崩溃（segfault，无法被 try/catch 捕获）。
    // 此处给出明确的运行时错误而非崩溃。
    if (!funDecl) {
        runtimeError(ErrorFormat::formatStd("无法在条件断点沙箱中调用 VM 闭包 {}（缺少 AST body，仅 VM 后端可调用）",

                                            effectiveName),
                     node.line, node.column);
    }

    // F10: 支持默认参数
    size_t argCount = node.arguments.size();
    if (argCount < static_cast<size_t>(funDecl->requiredParamCount) || argCount > funDecl->params.size()) {
        runtimeError(ErrorFormat::formatStd("函数 {} 期望 {}-{} 个参数，但传入了 {} 个", effectiveName,

                                            funDecl->requiredParamCount, funDecl->params.size(), argCount),
                     node.line, node.column, DiagCodes::kArityMismatch);
    }

    std::vector<Value> argValues;
    argValues.reserve(argCount);
    // P2-3 fix: bad_alloc 转化为带行号的 RuntimeError
    try {
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }
    } catch (const std::bad_alloc&) {
        runtimeError("内存不足：闭包调用参数收集失败", node.line, node.column);
    }

    // F10: 为缺失的参数填充默认值（在闭包环境中求值）
    // BUGFIX-P2 fix: 使用 RAII guard 恢复 currentEnv_，防止 evaluate() 抛异常时泄漏错误 scope
    if (argCount < funDecl->params.size()) {
        auto savedEnv = currentEnv_;
        struct EnvGuard {
            Interpreter& interp;
            std::shared_ptr<Environment> prev;
            ~EnvGuard() { interp.currentEnv_ = prev; }
        } guard{*this, currentEnv_};
        if (closureEnv) {
            currentEnv_ = closureEnv;
        }
        for (size_t i = argCount; i < funDecl->params.size(); ++i) {
            if (funDecl->defaultValues[i]) {
                argValues.push_back(evaluate(funDecl->defaultValues[i].get()));
            } else {
                argValues.push_back(Value::nullValue());
            }
        }
    }

    // R164 协程/生成器：生成器函数调用拦截（闭包路径）
    // 与 callNamedFunction 对称：检测 FunDecl.isGenerator，返回 Coroutine 值。
    // 闭包路径覆盖匿名 lambda 生成器（fun*(){...}()）和将生成器赋值给变量后调用的场景。
    // 注：funDecl 此处为裸指针（callClosureValue 内部使用），拦截前需获取 shared_ptr
    // 版本以便 CoroutineData 持有 AST 所有权（与 callNamedFunction 路径一致）。
    if (funDecl->isGenerator) {
        auto funDeclShared = calleeVal.closureBodyShared();
        if (!funDeclShared) {
            // 闭包 body 缺失（VM 闭包路径），回退到 funRegistry_
            auto it = funRegistry_.find(effectiveName);
            if (it == funRegistry_.end()) {
                runtimeError("生成器闭包缺少 AST body 且未在 funRegistry_ 中找到: " + effectiveName, node.line,
                             node.column);
            }
            funDeclShared = it->second;
        }
        // R164 fixup2: 若闭包环境已过期（weak_ptr 失效），从 capturedVars 重建。
        // 与 callNamedFunction 路径保持一致，确保多层嵌套闭包在协程重放时能访问捕获变量。
        if (!closureEnv) {
            closureEnv = std::make_shared<Environment>(nullptr);
            for (const auto& kv : calleeVal.capturedVars()) {
                closureEnv->define(kv.first, kv.second);
                closureEnv->markAsCaptured(kv.first);
            }
        }
        return makeCoroutineValue(std::move(funDeclShared), closureEnv, std::move(argValues));
    }

    // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ 的保存/恢复
    // R163 泛型扩展：传入 funDecl->typeParams，使函数体内的类型参数注解（如 x: T）跳过类型校验
    CallFrameGuard frameGuard{*this, funDecl->returnType, /*manageCtx=*/false, funDecl->typeParams};
    auto prevEnv = currentEnv_;

    // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH),
                     node.line, node.column);
    }
    RecursionGuard recursionGuard{recursionDepth_};

    std::shared_ptr<Environment> funEnv;
    bool envFromSnapshot = false;
    Value result = Value::nullValue();

    // RA-A fix: 用 RAII 守卫统一管理 funEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
    // 消除原 catch(...) + throw; 的 rethrow（减少 First-chance Exception 日志噪声）。
    // 守卫在正常路径和异常路径都执行清理，逻辑与原代码严格一致。
    // AUDIT-R6 F5 fix: throw 逃逸路径也执行快照写回（writeBackCapturedVars）——原实现
    // 仅正常/return 路径写回，闭包内先赋值后 throw 时修改丢失（实证：Interpreter=1
    // vs StackVM/RegVM=2，VM 共享 cell 在 throw 前的赋值立即可见）。写回须在
    // closeCapturedVariables 之前（与正常路径顺序一致）。
    struct FunEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& prev;
        Value* closureVal;        // AUDIT-R6 F5: 快照写回目标闭包（可为 null）
        const bool* fromSnapshot; // AUDIT-R6 F5: 是否经 rebuildEnvFromSnapshot 重建
        bool dismissed = false;
        ~FunEnvGuard() {
            if (!dismissed) {
                if (env && closureVal && fromSnapshot && *fromSnapshot)
                    writeBackCapturedVars(*closureVal, env);
                if (env)
                    env->closeCapturedVariables();
                interp.currentEnv_ = prev;
            }
        }
    } envGuard{*this, funEnv, prevEnv, &calleeVal, &envFromSnapshot};

    try {
        funEnv = std::make_shared<Environment>(closureEnv ? closureEnv : currentEnv_);

        // C1 fix: 若闭包环境已过期（weak_ptr 失效），从 capturedVars 快照重建
        if (!closureEnv) {
            rebuildEnvFromSnapshot(calleeVal, funEnv);
            envFromSnapshot = true;
        }

        for (size_t i = 0; i < funDecl->params.size(); ++i) {
            // P1-4 fix: 补充参数类型检查（与 callNamedFunction 一致）
            if (i < funDecl->paramTypes.size() && !funDecl->paramTypes[i].empty()) {
                checkType(
                    argValues[i], funDecl->paramTypes[i],
                    [&] { return "函数 " + effectiveName + " 的参数 " + funDecl->params[i]; }, node.line, node.column);
            }
            // AUDIT-P2.1 fix: 参数遮蔽捕获变量时，从 capturedVarNames_ 移除标记。
            // rebuildEnvFromSnapshot 会将所有 capturedVars 键 markAsCaptured，若参数与
            // 某个捕获变量同名（如 func f(x){...} 中 x 是捕获变量），writeBackCapturedVars
            // 会因 isCapturedVar("x") 仍为 true 而把参数值写回 capturedVars 污染下次调用。
            funEnv->unmarkCaptured(funDecl->params[i]);
            funEnv->define(funDecl->params[i], std::move(argValues[i]));
        }

        callStack_.emplace_back(effectiveName, funEnv, node.line, recursionDepth_);
        currentEnv_ = funEnv;
        // B1 TCO: 非蹦床调用点，禁用尾调用上下文（信号不可逃逸出本边界）。
        // 体内的自尾递归经 callNamedFunction 自建蹦床，深度仍恒定。
        TcoScopeGuard tcoGuard{*this, nullptr, std::string(), /*isMethod=*/false, /*enabled=*/false};
        executeFunctionBody(static_cast<Block&>(*funDecl->body));
    } catch (ReturnException& e) {
        result = std::move(e.returnValue);
    }
    // RA-A fix: 不再需要 catch(...) + throw; — envGuard 析构会恢复 env + closeCaptured，
    // 异常自然向上传播。B3/S2 由 CallFrameGuard/RecursionGuard 自动恢复。

    // C1 fix: 从快照恢复环境时，将变异写回 capturedVars，使后续调用可见
    // P0-6 fix: 仅写回原始 capturedVars 中已存在的键（捕获变量），
    // 跳过参数和函数内新建的局部变量，避免污染下次调用
    if (!closureEnv && envFromSnapshot) {
        writeBackCapturedVars(calleeVal, funEnv);
    }

    // B1 fix: 关闭捕获 — 函数返回时将函数局部变量的最终值写回闭包 capturedVars。
    // 闭包若捕获了函数参数或局部变量（如 func outer() { var x=1; func f(){return x;} return f; }），
    // 需在 funEnv 销毁前将最终值快照到 capturedVars，否则 weak_ptr 失效后无法访问。
    // RA-A fix: 由 envGuard 析构统一执行，此处 dismiss 避免重复
    envGuard.dismissed = true;
    if (funEnv)
        funEnv->closeCapturedVariables();
    currentEnv_ = prevEnv;

    return result;
}

// ---- P1 重构：内置构造函数 dict() / array() ----
Value Interpreter::callBuiltinConstructor(FunCall& node) {
    // 内置函数: dict() 和 array()
    if (node.name == "dict") {
        // dict() 或 dict({"a": 1, ...})
        if (node.arguments.empty()) {
            return Value(std::unordered_map<std::string, Value>());
        }
        // 有参数时，求值参数（期望是字典字面量）
        std::vector<Value> args;
        for (auto& arg : node.arguments) {
            args.push_back(evaluate(arg.get()));
        }
        if (args.size() == 1 && args[0].isDict()) {
            return args[0];
        }
        runtimeError("dict() 期望无参数或一个字典参数", node.line, node.column);
    }
    if (node.name == "array") {
        // array() 或 array(1, 2, 3)
        if (node.arguments.empty()) {
            return Value(std::vector<Value>());
        }
        std::vector<Value> args;
        for (auto& arg : node.arguments) {
            args.push_back(evaluate(arg.get()));
        }
        return Value(args);
    }
    return Value::nullValue();
}

// ---- 顶层内置函数 len/type/str/int/abs/min/max/range/sum ----
Value Interpreter::callBuiltinFunction(FunCall& node) {
    // 求值参数
    std::vector<Value> argValues;
    argValues.reserve(node.arguments.size());
    // P2-3 fix: bad_alloc 转化为带行号的 RuntimeError
    try {
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }
    } catch (const std::bad_alloc&) {
        runtimeError("内存不足：内置函数参数收集失败", node.line, node.column);
    }

    // 调用共享纯函数层
    auto r = executeSharedBuiltinFunction(node.name, argValues.data(), argValues.size(), node.line, node.column);

    if (r.is_err()) {
        throw to_runtime_error(r); // A1 fix: 自由函数模板
    }
    return std::move(r.value());
}

// ============================================================
// R98 W2: 高阶内置函数 map/filter/reduce/forEach/find
// ============================================================
// 拦截这 5 个名字，求值参数后调用共享算法层（executeSharedMap 等）。
// 闭包调用通过 invokeClosureSync 回调执行——Interpreter 路径直接从闭包
// 提取 AST body + env，构造新 Environment 绑定参数，执行函数体并捕获
// ReturnException 返回值。与 VM 路径的帧压栈+run loop 不同，但语义等价。

Result<Value> Interpreter::invokeClosureSync(const Value& closure, const Value* args, size_t argCount, int line,
                                             int column) {
    if (!closure.isClosure()) {
        return Result<Value>::err("高阶函数的参数必须是函数", line, column);
    }

    FunDecl* funDecl = closure.closureBody();
    if (!funDecl) {
        // VM 创建的闭包（无 AST body）无法在 Interpreter 中调用
        return Result<Value>::err("无法在 Interpreter 中调用 VM 闭包 " + closure.closureName(), line, column);
    }

    auto closureEnv = closure.closureEnv();
    const std::string& effectiveName = closure.closureName();

    // 参数数量检查
    if (argCount < static_cast<size_t>(funDecl->requiredParamCount) || argCount > funDecl->params.size()) {
        return Result<Value>::err(ErrorFormat::formatStd("函数 {} 期望 {}-{} 个参数，但传入了 {} 个",

                                                         effectiveName, funDecl->requiredParamCount,

                                                         funDecl->params.size(), argCount),
                                  line, column);
    }

    // 收集参数 + 填充默认值
    std::vector<Value> argValues;
    argValues.reserve(funDecl->params.size());
    for (size_t i = 0; i < argCount; ++i) {
        argValues.push_back(args[i]);
    }
    if (argCount < funDecl->params.size()) {
        auto savedEnv = currentEnv_;
        struct EnvGuard {
            Interpreter& interp;
            std::shared_ptr<Environment> prev;
            ~EnvGuard() { interp.currentEnv_ = prev; }
        } guard{*this, currentEnv_};
        if (closureEnv) {
            currentEnv_ = closureEnv;
        }
        for (size_t i = argCount; i < funDecl->params.size(); ++i) {
            if (funDecl->defaultValues[i]) {
                argValues.push_back(evaluate(funDecl->defaultValues[i].get()));
            } else {
                argValues.push_back(Value::nullValue());
            }
        }
    }

    // 递归深度检查
    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        return Result<Value>::err(
            ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH), line, column);
    }

    // R163 泛型扩展：传入 funDecl->typeParams，使函数体内的类型参数注解跳过类型校验
    CallFrameGuard frameGuard{*this, funDecl->returnType, /*manageCtx=*/false, funDecl->typeParams};
    auto prevEnv = currentEnv_;
    RecursionGuard recursionGuard{recursionDepth_};

    std::shared_ptr<Environment> funEnv;
    bool envFromSnapshot = false;
    Value result = Value::nullValue();

    struct FunEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& prev;
        Value* closureVal;        // AUDIT-R6 F5: 快照写回目标闭包
        const bool* fromSnapshot; // AUDIT-R6 F5
        bool dismissed = false;
        ~FunEnvGuard() {
            if (!dismissed) {
                // AUDIT-R6 F5 fix: throw/RuntimeError 逃逸路径也执行快照写回（先写回后 close）
                if (env && closureVal && fromSnapshot && *fromSnapshot)
                    writeBackCapturedVars(*closureVal, env);
                if (env)
                    env->closeCapturedVariables();
                interp.currentEnv_ = prev;
            }
        }
    } envGuard{*this, funEnv, prevEnv, &const_cast<Value&>(closure), &envFromSnapshot};

    try {
        funEnv = std::make_shared<Environment>(closureEnv ? closureEnv : currentEnv_);

        if (!closureEnv) {
            rebuildEnvFromSnapshot(const_cast<Value&>(closure), funEnv);
            envFromSnapshot = true;
        }

        for (size_t i = 0; i < funDecl->params.size(); ++i) {
            if (i < funDecl->paramTypes.size() && !funDecl->paramTypes[i].empty()) {
                checkType(
                    argValues[i], funDecl->paramTypes[i],
                    [&] { return "函数 " + effectiveName + " 的参数 " + funDecl->params[i]; }, line, column);
            }
            funEnv->unmarkCaptured(funDecl->params[i]);
            funEnv->define(funDecl->params[i], std::move(argValues[i]));
        }

        callStack_.emplace_back(effectiveName, funEnv, line, recursionDepth_);
        currentEnv_ = funEnv;
        // B1 TCO: 高阶回调/spawn 同步调用非蹦床，禁用尾调用上下文
        TcoScopeGuard tcoGuard{*this, nullptr, std::string(), /*isMethod=*/false, /*enabled=*/false};
        executeFunctionBody(static_cast<Block&>(*funDecl->body));
    } catch (ReturnException& e) {
        result = std::move(e.returnValue);
    } catch (RuntimeError& e) {
        // 高阶函数闭包内的运行时错误向上传播，转为 Result::err
        // AUDIT-R6 F5 fix: 错误路径也写回快照变异（与守卫析构逻辑一致，手动路径同步）
        envGuard.dismissed = true;
        if (!closureEnv && envFromSnapshot && funEnv)
            writeBackCapturedVars(const_cast<Value&>(closure), funEnv);
        if (funEnv)
            funEnv->closeCapturedVariables();
        currentEnv_ = prevEnv;
        return Result<Value>::err(e.what(), line, column);
    }

    if (!closureEnv && envFromSnapshot) {
        writeBackCapturedVars(const_cast<Value&>(closure), funEnv);
    }

    envGuard.dismissed = true;
    if (funEnv)
        funEnv->closeCapturedVariables();
    currentEnv_ = prevEnv;

    return Result<Value>::ok(std::move(result));
}

Value Interpreter::callHigherOrderBuiltin(FunCall& node) {
    // 求值参数
    std::vector<Value> argValues;
    argValues.reserve(node.arguments.size());
    try {
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }
    } catch (const std::bad_alloc&) {
        runtimeError("内存不足：高阶函数参数收集失败", node.line, node.column);
    }

    // 构造闭包调用回调
    ClosureInvoker invoke = [this, line = node.line, column = node.column](const Value& closure, const Value* args,
                                                                           size_t argCount, int /*invLine*/,
                                                                           int /*invColumn*/) -> Result<Value> {
        return invokeClosureSync(closure, args, argCount, line, column);
    };

    // 按函数名分派到共享算法层
    Result<Value> r = Result<Value>::ok(Value::nullValue());
    if (node.name == "map") {
        if (argValues.size() != 2) {
            runtimeError(ErrorFormat::formatStd("map 期望 2 个参数，但传入了 {} 个", argValues.size()), node.line,
                         node.column);
        }
        r = executeSharedMap(argValues[0], argValues[1], invoke, node.line, node.column);
    } else if (node.name == "filter") {
        if (argValues.size() != 2) {
            runtimeError(ErrorFormat::formatStd("filter 期望 2 个参数，但传入了 {} 个", argValues.size()), node.line,
                         node.column);
        }
        r = executeSharedFilter(argValues[0], argValues[1], invoke, node.line, node.column);
    } else if (node.name == "reduce") {
        if (argValues.size() != 3) {
            runtimeError(ErrorFormat::formatStd("reduce 期望 3 个参数，但传入了 {} 个", argValues.size()), node.line,
                         node.column);
        }
        r = executeSharedReduce(argValues[0], argValues[1], argValues[2], invoke, node.line, node.column);
    } else if (node.name == "forEach") {
        if (argValues.size() != 2) {
            runtimeError(ErrorFormat::formatStd("forEach 期望 2 个参数，但传入了 {} 个", argValues.size()), node.line,
                         node.column);
        }
        r = executeSharedForEach(argValues[0], argValues[1], invoke, node.line, node.column);
    } else if (node.name == "find") {
        if (argValues.size() != 2) {
            runtimeError(ErrorFormat::formatStd("find 期望 2 个参数，但传入了 {} 个", argValues.size()), node.line,
                         node.column);
        }
        r = executeSharedFind(argValues[0], argValues[1], invoke, node.line, node.column);
    } else {
        runtimeError("未知的高阶函数: " + node.name, node.line, node.column);
    }

    if (r.is_err()) {
        throw to_runtime_error(r);
    }
    return std::move(r.value());
}

// ---- R136: spawn(fn, args...) 内置函数 ----
Value Interpreter::callSpawnBuiltin(FunCall& node) {
    if (node.arguments.empty()) {
        runtimeError("spawn 期望至少 1 个参数（函数），但传入了 0 个", node.line, node.column);
    }
    // 求值参数：第 1 个是闭包，其余是闭包参数
    std::vector<Value> argValues;
    argValues.reserve(node.arguments.size());
    try {
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }
    } catch (const std::bad_alloc&) {
        runtimeError("内存不足：spawn 参数收集失败", node.line, node.column);
    }

    Value closure = std::move(argValues[0]);
    // 构造闭包调用回调：通过 spawnMutex_ 序列化，避免 callStack_/currentEnv_ 数据竞争
    // 注：mutex 捕获引用安全——Interpreter 实例生命周期覆盖所有 spawn 出的子线程
    // （子线程持有的 closureCopy/argsCopy/invokerCopy 是值副本，但 invoker 内部
    // 通过 this 指针访问 Interpreter，需确保 Interpreter 未被析构。
    // 用户层约束：thread.join() 必须在 Interpreter 析构前调用，否则 UAF。
    // detach 风险自担——文档需明确说明。)
    ClosureInvoker invoke = [this, line = node.line, column = node.column](const Value& clos, const Value* args,
                                                                           size_t argCount, int /*invLine*/,
                                                                           int /*invColumn*/) -> Result<Value> {
        std::lock_guard<std::mutex> lock(spawnMutex_);
        return invokeClosureSync(clos, args, argCount, line, column);
    };

    auto r = executeSharedSpawn(closure, argValues.data() + 1, argValues.size() - 1, invoke, node.line, node.column);
    if (r.is_err()) {
        throw to_runtime_error(r);
    }
    return std::move(r.value());
}

// ---- P1 重构：类构造调用 ----
// R133-D fix: 拆为 thin orchestrator + 3 helper（copyInheritedClassFields / setupInitEnvironment /
// runInitMethodBody）。主函数保留类查找 + 参数校验 + 求值 + 重定义再校验 + 递归检查 + 实例创建 +
// 分派，helper 按子任务分组共享 Interpreter 全部成员状态。
Value Interpreter::constructClassInstance(FunCall& node) {
    // 检查是否是类构造调用
    auto classIt = classRegistry_.find(node.name);
    if (classIt == classRegistry_.end()) {
        return Value::nullValue();
    }
    ClassInfo* cls = &classIt->second; // #2 fix: 用指针代替引用，便evaluate后重绑定

    // 检查参数数量（构造函数为 init 方法）
    FunDecl* initMethod = findMethod(*cls, "init");

    // P0-2 fix: 支持默认参数，参数数量可在 [requiredParamCount, params.size()] 范围内
    if (initMethod && (node.arguments.size() < static_cast<size_t>(initMethod->requiredParamCount) ||
                       node.arguments.size() > initMethod->params.size())) {
        runtimeError(ErrorFormat::formatStd("构造函数 init 期望 {}-{} 个参数，但传入了 {} 个",

                                            initMethod->requiredParamCount, initMethod->params.size(),

                                            node.arguments.size()),
                     node.line, node.column);
    }
    if (!initMethod && !node.arguments.empty()) {
        runtimeError(
            ErrorFormat::formatStd("类 {} 没有 init 方法，但传入了 {} 个参数", cls->name, node.arguments.size()),
            node.line, node.column);
    }

    // 求值参数
    std::vector<Value> argValues;
    argValues.reserve(node.arguments.size());
    std::string className = cls->name; // #2 fix: 缓存类名
    // P2-3 fix: bad_alloc 转化为带行号的 RuntimeError
    try {
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }
    } catch (const std::bad_alloc&) {
        runtimeError("内存不足：类构造参数收集失败", node.line, node.column);
    }

    // #2 fix: evaluate后重新查找
    classIt = classRegistry_.find(className);
    cls = &classIt->second; // 重绑定指针到新位置
    initMethod = findMethod(*cls, "init");

    // AUDIT-P2-CORRECT fix: 类重定义后参数签名可能变化，需重新校验 argValues 数量。
    // 原实现仅在求值前用旧 initMethod 校验，求值后重新查找 initMethod 但未重新校验，
    // 导致新 init 参数更少时多余参数被静默忽略，参数更多时缺失参数走 null 路径。
    if (initMethod && (argValues.size() < static_cast<size_t>(initMethod->requiredParamCount) ||
                       argValues.size() > initMethod->params.size())) {
        runtimeError(
            ErrorFormat::formatStd("类 {} 在构造期间被重定义，参数数量不匹配（init 期望 {}-{} 个，但传入了 {} 个）",

                                   cls->name, initMethod->requiredParamCount, initMethod->params.size(),

                                   argValues.size()),
            node.line, node.column);
    }

    // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH),
                     node.line, node.column);
    }
    RecursionGuard recursionGuard{recursionDepth_};

    // 创建实例 + 复制继承链默认字段（R133-D fix: 提取为 copyInheritedClassFields）
    Value instance = Value::makeInstance(cls->name);
    copyInheritedClassFields(instance, cls);

    // 如果有 init 方法，执行它（R133-D fix: 提取为 setupInitEnvironment + runInitMethodBody）
    if (initMethod) {
        auto initEnv = setupInitEnvironment(cls, initMethod, argValues, instance, node);
        runInitMethodBody(cls, initMethod, initEnv, instance, node);
    }

    // B1 fix: recursionDepth_ 由 RAII guard 自动恢复（无需手动递减）
    return instance;
}

// ============================================================
// R133-D fix: constructClassInstance 分组 helper 实现
// ------------------------------------------------------------
// 按子任务分组：copyInheritedClassFields 复制继承链字段（含循环检测）；
// setupInitEnvironment 准备 init 执行环境（绑定 this/参数/默认值 + bindInstance）；
// runInitMethodBody 执行 init 体（调用帧 + 环境切换 + 类上下文 + 读回 this + 关闭捕获）。
// 所有 helper 共享 Interpreter 全部成员状态（classRegistry_ / currentEnv_ / callStack_ /
// classContextStack_ / recursionDepth_），签名仅传递子任务局部数据。
// ============================================================

void Interpreter::copyInheritedClassFields(Value& instance, ClassInfo* cls) {
    // 复制类默认字段值（含继承链）— B8 fix: 加入循环检测
    ClassInfo* curCls = cls;
    std::unordered_set<std::string> visitedClasses;
    while (curCls) {
        if (!visitedClasses.insert(curCls->name).second)
            break; // 检测到循环继承
        for (const auto& kv : curCls->fields) {
            // 子类字段覆盖父类
            // Perf-Finding: 缓存 find 迭代器，避免 operator[] 二次 hash 查找同键
            auto& flds = instance.fields();
            auto it = flds.find(kv.first);
            if (it == flds.end()) {
                flds.emplace(kv.first, kv.second);
            }
        }
        if (!curCls->superClassName.empty()) {
            auto superIt = classRegistry_.find(curCls->superClassName);
            curCls = (superIt != classRegistry_.end()) ? &superIt->second : nullptr;
        } else {
            curCls = nullptr;
        }
    }
}

std::shared_ptr<Environment> Interpreter::setupInitEnvironment(ClassInfo* cls, FunDecl* initMethod,
                                                               std::vector<Value>& argValues, Value& instance,
                                                               FunCall& node) {
    // O5: 使用类定义时捕获的环境作为父级（闭包）
    auto parentEnv = cls->closureEnv ? cls->closureEnv : currentEnv_;
    auto initEnv = std::make_shared<Environment>(parentEnv);

    // 绑定 this
    initEnv->define("this", instance);

    // P5 fix: 绑定实例而非深拷贝所有字段
    // 绑定参数
    for (size_t i = 0; i < initMethod->params.size(); ++i) {
        if (i < argValues.size()) {
            // AUDIT-P1 fix: 补充参数类型检查（与 callClosureValue/callNamedFunction/
            // callInstanceMethod 的 P1-4 fix 一致）。原实现仅 define 不检查类型注解，
            // 导致类型注解在类构造路径被静默跳过，三后端语义不一致。
            if (i < initMethod->paramTypes.size() && !initMethod->paramTypes[i].empty()) {
                checkType(
                    argValues[i], initMethod->paramTypes[i],
                    [&] { return "类 " + node.name + " 的 init 参数 " + initMethod->params[i]; }, node.line,
                    node.column);
            }
            initEnv->define(initMethod->params[i], std::move(argValues[i]));
        } else {
            // P0-2 fix: 填充缺失的默认参数值
            // defaultValues 与 params 一一对应（无默认值的位置为 nullptr）
            if (i < initMethod->defaultValues.size() && initMethod->defaultValues[i]) {
                // I-P1-1 fix: 默认值表达式应在类定义的闭包环境中求值
                // （与 callNamedFunction/callClosureValue/callInstanceMethod 保持一致）
                // BUGFIX-P2 fix: RAII guard 防止 evaluate() 抛异常时 currentEnv_ 不恢复
                auto savedEnv = currentEnv_;
                struct EnvGuard {
                    Interpreter& interp;
                    std::shared_ptr<Environment> prev;
                    ~EnvGuard() { interp.currentEnv_ = prev; }
                } guard{*this, currentEnv_};
                if (cls->closureEnv) {
                    currentEnv_ = cls->closureEnv;
                }
                Value defaultVal = evaluate(initMethod->defaultValues[i].get());
                // AUDIT-P1 fix: 默认值同样需类型检查
                if (i < initMethod->paramTypes.size() && !initMethod->paramTypes[i].empty()) {
                    checkType(
                        defaultVal, initMethod->paramTypes[i],
                        [&] { return "类 " + node.name + " 的 init 参数 " + initMethod->params[i]; }, node.line,
                        node.column);
                }
                initEnv->define(initMethod->params[i], std::move(defaultVal));
            } else {
                initEnv->define(initMethod->params[i], Value::nullValue());
            }
        }
    }

    // H-新2 fix: bindInstance 必须在所有 define 之后
    Value* thisInEnv = const_cast<Value*>(initEnv->get("this"));
    if (thisInEnv)
        initEnv->bindInstance(thisInEnv);

    return initEnv;
}

void Interpreter::runInitMethodBody(ClassInfo* cls, FunDecl* initMethod, std::shared_ptr<Environment> initEnv,
                                    Value& instance, FunCall& node) {
    // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ + classContextStack_
    // AUDIT-BUG-F1 fix: CallFrameGuard 必须在 emplace_back 之前构造，使其保存的
    // savedStackDepth = N（不含新条目），析构时才能正确弹回到 N。若顺序颠倒，
    // savedStackDepth = N+1，析构时不弹出 init 帧，循环构造实例导致 callStack_ 泄漏。
    // R163 泛型扩展：合并类泛型参数 + init 方法泛型参数，使 init 体内的类型参数注解跳过类型校验
    std::vector<std::string> mergedTypeParams = cls->typeParams;
    for (const auto& tp : initMethod->typeParams) {
        mergedTypeParams.push_back(tp);
    }
    CallFrameGuard frameGuard{*this, initMethod->returnType, /*manageCtx=*/true, mergedTypeParams};

    // 压入调用帧
    callStack_.emplace_back(node.name + ".init", initEnv, node.line, recursionDepth_);

    // 切换环境
    auto prevEnv = currentEnv_;
    currentEnv_ = initEnv;

    // 压入类上下文（super 解析用）
    // AUDIT-P1-CORRECT fix: 压入"init 方法定义所在类"而非"实例类"。
    // 当实例类未定义 init 而继承父类的 init 时，压入实例类名会导致
    // init 体内的 super 调用从错误的类开始搜索（应为定义 init 的类的父类）。
    classContextStack_.push_back(findMethodDefiningClassName(*cls, "init"));

    // RA-A fix: RAII 守卫统一管理 initEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
    // 消除原 catch(...) + throw; 的 rethrow。
    struct InitEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& prev;
        bool dismissed = false;
        ~InitEnvGuard() {
            if (!dismissed) {
                env->closeCapturedVariables();
                interp.currentEnv_ = prev;
            }
        }
    } envGuard{*this, initEnv, prevEnv};

    try {
        // B1 TCO: init 构造路径非蹦床，禁用尾调用上下文
        TcoScopeGuard tcoGuard{*this, nullptr, std::string(), /*isMethod=*/false, /*enabled=*/false};
        executeFunctionBody(static_cast<Block&>(*initMethod->body));
    } catch (const ReturnException&) {
        // init 方法的返回值忽略，但更新实例字段
    }
    // RA-A fix: 不再需要 catch(...) + throw; — envGuard 析构统一恢复

    // 从 init 环境中读取 this 的更新值
    auto* thisPtr = initEnv->get("this");
    if (thisPtr) {
        instance = *thisPtr;
    }

    // M3 fix: 移除冗余的局部变量→字段同步（同 VarDecl 路径，P5 bindInstance 已处理）

    // B1 fix: 关闭捕获 — init 正常退出时将局部变量最终值写回闭包 capturedVars
    // RA-A fix: 由 envGuard 析构统一执行，此处 dismiss 避免重复
    envGuard.dismissed = true;
    initEnv->closeCapturedVariables();
    currentEnv_ = prevEnv;
}

// ---- P1 重构：普通函数/闭包调用 ----
Value Interpreter::callNamedFunction(FunCall& node) {
    // 检查环境中是否有闭包值
    std::shared_ptr<Environment> closureEnv;
    Value* closureValPtr = nullptr; // C1 fix: 保存闭包值指针用于 capturedVars 回退
    // A3 fix: 使用 shared_ptr 持有 FunDecl，源（resolvedDecl/funRegistry_/closureBodyShared）
    // 均返回 shared_ptr，确保缓存写入时共享所有权
    std::shared_ptr<FunDecl> funDecl;
    std::string effectiveName = node.name; // 实际函数名（闭包时可能不同于调用变量名）

    // R104 Function Breakpoint：函数调用入口检查（命名调用路径）。
    // 使用 node.name 对齐 StackVM/RegisterVM 的 peekCalledFunctionName（OP_CALL/REG_CALL
    // 取常量池函数名，闭包路径另由 callClosureValue 检查 effectiveName）。
    // AUDIT-R4 BUG-15 fix: atomic load 到局部变量
    if (auto dbg = debugger()) {
        dbg->checkFunctionBreakpoint(node.name, node.line);
    }

    // R164 fix: 参数求值提前到函数查找之前，对齐 VM 求值顺序。
    // VM 编译期先 emit 参数求值指令，运行时先执行参数求值，再 OP_CALL 查找函数。
    // Interpreter 原先先查找函数（未定义则立即报错），导致"函数未定义 + 参数有运行时错误"
    // 场景下三后端报错类型不一致（fuzz mutate 发现的 2 个分歧）。提前求值后两者一致。
    // 注：参数求值不依赖 funDecl/closureEnv，可安全提前；若函数未定义，argValues 随栈展开销毁。
    size_t argCount = node.arguments.size();
    std::vector<Value> argValues;
    argValues.reserve(argCount);
    // P2-3 fix: bad_alloc 转化为带行号的 RuntimeError
    try {
        for (auto& arg : node.arguments) {
            argValues.push_back(evaluate(arg.get()));
        }
    } catch (const std::bad_alloc&) {
        runtimeError("内存不足：命名函数参数收集失败", node.line, node.column);
    }

    // 快速路径：使用缓存的函数体（跳过环境查找和 funRegistry_ 查找）
    // M7 fix: 检查代数是否匹配，函数重定义后缓存失效
    // P1-2 fix: 若变量已被重赋值为非闭包，缓存失效，回退慢路径
    auto cachedDecl = node.resolvedDecl.lock(); // Bug #11 fix: weak_ptr::lock()
    if (node.isResolved && cachedDecl && node.resolvedGen_ == funRegistryGen_) {
        const Value* calleePtr = currentEnv_->get(node.name);
        if (calleePtr && calleePtr->isClosure()) {
            funDecl = cachedDecl; // Bug #11 fix: use locked weak_ptr
            closureEnv = calleePtr->closureEnv();
            effectiveName = calleePtr->closureName();
            closureValPtr = const_cast<Value*>(calleePtr); // C1 fix
        } else {
            // 变量已被重赋值为非闭包（或未定义），缓存陈旧，失效并回退慢路径
            node.isResolved = false;
        }
    }

    if (!funDecl) {
        // 慢路径：完整解析（单次 get() 调用）
        const Value* calleePtr = currentEnv_->get(node.name);
        if (calleePtr && calleePtr->isClosure()) {
            closureEnv = calleePtr->closureEnv();
            effectiveName = calleePtr->closureName();
            closureValPtr = const_cast<Value*>(calleePtr); // C1 fix
            // 优先从闭包值中获取函数体（自包含，不依赖 funRegistry_）
            // A3 fix: 使用 closureBodyShared() 获取 shared_ptr 副本
            funDecl = calleePtr->closureBodyShared();
            if (!funDecl) {
                // 后备路径：从 funRegistry_ 查找（处理 AST 生命周期问题）
                auto it = funRegistry_.find(calleePtr->closureName());
                if (it != funRegistry_.end()) {
                    funDecl = it->second;
                }
            }
        }

        // P1-3 fix: 变量非闭包时不应从 funRegistry_ 后备调用旧函数体
        // funRegistry_ 仅用于闭包 body 缺失的后备，不能让重赋值后的变量仍调用旧函数
        if (!funDecl) {
            if (!calleePtr || !calleePtr->isClosure()) {
                // R164 fix: 三后端消息统一（fuzz mutate 发现的分歧）
                // 原报 "X 不是函数，无法调用"，StackVM/RegisterVM 报 "未定义的函数: X"
                runtimeError(ErrorFormat::formatStd(ErrorMessages::kUndefinedFunctionFmtStd, node.name), node.line,
                             node.column, "undefined-function");
            }
            // 仅在 calleePtr 是闭包但 body 缺失时回退到 funRegistry_
            auto it = funRegistry_.find(effectiveName);
            if (it == funRegistry_.end()) {
                runtimeError(ErrorFormat::formatStd(ErrorMessages::kUndefinedFunctionFmtStd, node.name), node.line,
                             node.column, "undefined-function");
            }
            funDecl = it->second;
        }

        // 缓存解析结果供后续调用使用
        node.resolvedDecl = funDecl;
        node.isResolved = true;
        node.resolvedGen_ = funRegistryGen_; // M7: 记录当前代数
    }

    // 检查参数数量（参数已在函数查找之前求值，R164 fix 求值顺序对齐）
    // F10: 支持默认参数，参数数量可在 [requiredParamCount, params.size()] 范围内
    if (argCount < static_cast<size_t>(funDecl->requiredParamCount) || argCount > funDecl->params.size()) {
        runtimeError(ErrorFormat::formatStd("函数 {} 期望 {}-{} 个参数，但传入了 {} 个", node.name,

                                            funDecl->requiredParamCount, funDecl->params.size(), argCount),
                     node.line, node.column, DiagCodes::kArityMismatch);
    }

    // F10: 为缺失的参数填充默认值
    // 默认值在函数定义时的闭包环境中求值（与函数体同级）
    // BUGFIX-P2 fix: 使用 RAII guard 恢复 currentEnv_，防止 evaluate() 抛异常时泄漏错误 scope
    // B1 TCO: 提取为 lambda——蹦床循环后续轮次（TailCallSignal 携带的实参
    // 可能少于形参）同样需要填充默认值。
    auto fillDefaultArgs = [&](std::vector<Value>& argsInOut) {
        if (argsInOut.size() >= funDecl->params.size())
            return;
        struct EnvGuard {
            Interpreter& interp;
            std::shared_ptr<Environment> prev;
            ~EnvGuard() { interp.currentEnv_ = prev; }
        } guard{*this, currentEnv_};
        // 切换到闭包环境（函数定义时的环境），使默认值表达式能访问外层变量
        if (closureEnv) {
            currentEnv_ = closureEnv;
        }
        for (size_t i = argsInOut.size(); i < funDecl->params.size(); ++i) {
            if (funDecl->defaultValues[i]) {
                argsInOut.push_back(evaluate(funDecl->defaultValues[i].get()));
            } else {
                // 不应发生（requiredParamCount 已校验），防御性处理
                argsInOut.push_back(Value::nullValue());
            }
        }
    };
    fillDefaultArgs(argValues);

    // R164 协程/生成器：生成器函数调用拦截
    // 检测 FunDecl.isGenerator，返回 Coroutine 值而非直接执行函数体。
    // 拦截点选择在参数求值和默认参数填充完成后：保证参数求值的副作用正常发生，
    // 但不进入函数体执行（生成器函数体只在 .next() 重放时执行）。
    // 注：拦截发生在 CallFrameGuard 之前，不污染调用帧状态。
    if (funDecl->isGenerator) {
        // R164 fixup2: 若闭包环境已过期（weak_ptr 失效），从 capturedVars 重建。
        // 与普通函数调用路径（下方 C1 fix rebuildEnvFromSnapshot）保持一致。
        // 否则 CoroutineData.closureEnv 为 nullptr，callCoroutineNext 创建的 funEnv
        // 缺少捕获变量（x/y），导致多层嵌套闭包在协程重放时报"未定义的变量"。
        if (!closureEnv && closureValPtr) {
            closureEnv = std::make_shared<Environment>(nullptr);
            const Value& closureValRef = *closureValPtr;
            for (const auto& kv : closureValRef.capturedVars()) {
                closureEnv->define(kv.first, kv.second);
                closureEnv->markAsCaptured(kv.first);
            }
        }
        return makeCoroutineValue(funDecl, closureEnv, std::move(argValues));
    }

    // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ 的保存/恢复
    // R163 泛型扩展：传入 funDecl->typeParams，使函数体内的类型参数注解跳过类型校验
    CallFrameGuard frameGuard{*this, funDecl->returnType, /*manageCtx=*/false, funDecl->typeParams};
    auto prevEnv = currentEnv_;

    // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH),
                     node.line, node.column);
    }
    RecursionGuard recursionGuard{recursionDepth_};

    Value result = Value::nullValue();
    std::shared_ptr<Environment> funEnv; // C1 fix: 声明在 try 外以便写回
    bool envFromSnapshot = false;
    // L18 eng-tailcall: 互递归目标闭包持有者（蹦床切换目标后 closureValPtr
    // 指向此处，生命周期覆盖整个蹦床循环）。
    Value tcoTargetClosureHolder;

    // RA-A fix: RAII 守卫统一管理 funEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
    // 消除原 catch(...) + throw; 的 rethrow。
    // AUDIT-R6 F5 fix: throw 逃逸路径也执行快照写回（同 callClosureValue 守卫）。
    struct FunEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& prev;
        Value*& closureVal;       // AUDIT-R6 F5: 快照写回目标闭包（可为 null）
                                  // L18: 改为指针引用——蹦床互递归切换目标后
                                  // closureValPtr 指向新目标，析构写回须跟随最新值。
        const bool* fromSnapshot; // AUDIT-R6 F5
        bool dismissed = false;
        ~FunEnvGuard() {
            if (!dismissed) {
                if (env && closureVal && fromSnapshot && *fromSnapshot)
                    writeBackCapturedVars(*closureVal, env);
                if (env)
                    env->closeCapturedVariables();
                interp.currentEnv_ = prev;
            }
        }
    } envGuard{*this, funEnv, prevEnv, closureValPtr, &envFromSnapshot};

    try {
        // B1 TCO 蹦床循环：TailCallSignal 触发帧复用（清理本轮 funEnv 后用新
        // 实参重建环境重新执行函数体），ReturnException 结束整个调用。
        // C++ 递归深度恒定，深尾递归不再受 MAX_RECURSION_DEPTH 限制（对齐 VM TCO）。
        int64_t tcoIterations = 0;
        while (true) {
            // 参数类型检查（每轮重新执行，与递归调用逐层检查等价）
            for (size_t i = 0; i < funDecl->params.size() && i < funDecl->paramTypes.size(); ++i) {
                if (!funDecl->paramTypes[i].empty()) {
                    checkType(
                        argValues[i], funDecl->paramTypes[i],
                        [&] { return "函数 " + node.name + " 的参数 " + funDecl->params[i]; }, node.line, node.column);
                }
            }

            // 创建新环境：使用闭包捕获的环境作为父级（如果有的话）。
            // B1 TCO: 非闭包分支用 prevEnv（调用入口环境）而非 currentEnv_，
            // 后续轮次 currentEnv_ 已是上一轮 funEnv，直接作父会错误链式嵌套。
            if (closureEnv) {
                funEnv = std::make_shared<Environment>(closureEnv);
            } else {
                funEnv = std::make_shared<Environment>(prevEnv);
            }

            // C1 fix: 若闭包环境已过期（weak_ptr 失效），从 capturedVars 快照重建
            if (!closureEnv && closureValPtr) {
                rebuildEnvFromSnapshot(*closureValPtr, funEnv);
                envFromSnapshot = true;
            }

            // 绑定参数（move 避免深拷贝）
            for (size_t i = 0; i < funDecl->params.size(); ++i) {
                // AUDIT-P2.1 fix: 与 callClosureValue 一致，参数遮蔽捕获变量时移除标记，
                // 防止 writeBackCapturedVars 将参数值写回 capturedVars 污染下次调用
                funEnv->unmarkCaptured(funDecl->params[i]);
                funEnv->define(funDecl->params[i], std::move(argValues[i]));
            }

            // 压入调用栈
            // BUG-DBG-4 fix: 使用 effectiveName 而非 node.name，与 callClosureValue (L195) 一致。
            // 原实现使用 node.name（调用变量名），闭包赋值给不同变量时显示变量名而非闭包名。
            callStack_.emplace_back(effectiveName, funEnv, node.line, recursionDepth_);

            // 切换环境
            currentEnv_ = funEnv;

            try {
                // B1 TCO: 启用蹦床上下文（仅本函数体内的自尾调用可触发信号）
                TcoScopeGuard tcoGuard{*this, funDecl.get(), effectiveName, /*isMethod=*/false, /*enabled=*/true};
                // 执行函数体（不求值返回值：无 return 语句时函数应返回 null）
                executeFunctionBody(static_cast<Block&>(*funDecl->body));
                break;
            } catch (TailCallSignal& sig) {
                // 帧复用：与正常返回路径一致的清理（快照写回 + 关闭捕获，
                // 使本轮内定义的闭包捕获语义与非 TCO 递归完全一致），
                // 弹出本轮调用栈条目，下一轮重新压入。
                if (++tcoIterations > RuntimeLimits::MAX_LOOP_ITERATIONS) {
                    runtimeError(
                        ErrorFormat::formatStd("尾调用迭代次数超过限制 ({})", RuntimeLimits::MAX_LOOP_ITERATIONS),
                        node.line, node.column);
                }
                if (!closureEnv && envFromSnapshot && closureValPtr && funEnv)
                    writeBackCapturedVars(*closureValPtr, funEnv);
                if (funEnv)
                    funEnv->closeCapturedVariables();
                callStack_.pop_back();
                argValues = std::move(sig.args);
                // L18 eng-tailcall: 互递归目标切换——信号携带目标 FunDecl 时，
                // 下一轮用目标函数重跑。目标闭包值持久化到 holder，环境/
                // 快照重建机制（closureEnv/rebuild）与普通调用入口完全一致。
                if (sig.target && sig.target.get() != funDecl.get()) {
                    funDecl = sig.target;
                    effectiveName = sig.targetName;
                    tcoTargetClosureHolder = std::move(sig.targetClosure);
                    closureEnv = tcoTargetClosureHolder.closureEnv();
                    closureValPtr = &tcoTargetClosureHolder;
                    envFromSnapshot = false;
                }
                fillDefaultArgs(argValues);
            }
        }
    } catch (ReturnException& e) {
        result = std::move(e.returnValue);
    }
    // RA-A fix: 不再需要 catch(...) + throw; — envGuard 析构统一恢复

    // C1 fix: 从快照恢复环境时，将变异写回 capturedVars，使后续调用可见
    // P0-6 fix: 仅写回原始 capturedVars 中已存在的键（捕获变量），
    // 跳过参数和函数内新建的局部变量，避免污染下次调用
    if (!closureEnv && envFromSnapshot && closureValPtr && funEnv) {
        writeBackCapturedVars(*closureValPtr, funEnv);
    }

    // B1 fix: 关闭捕获 — 函数返回时将函数局部变量的最终值写回闭包 capturedVars
    // RA-A fix: 由 envGuard 析构统一执行，此处 dismiss 避免重复
    envGuard.dismissed = true;
    if (funEnv)
        funEnv->closeCapturedVariables();
    currentEnv_ = prevEnv;

    return result;
}

// ---- P1 重构：求值参数列表 ----
std::vector<Value> Interpreter::evaluateArguments(const std::vector<std::shared_ptr<ASTNode>>& args) {
    std::vector<Value> argValues;
    argValues.reserve(args.size());
    for (auto& arg : args) {
        argValues.push_back(evaluate(arg.get()));
    }
    return argValues;
}
