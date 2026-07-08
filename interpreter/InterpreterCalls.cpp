// ============================================================
// InterpreterCalls.cpp — 函数调用相关方法实现（visitFunCall 及其分派辅助方法）
// ============================================================
// S6 fix: 从 Interpreter.cpp 拆分，降低单文件复杂度。
// ============================================================

#include "interpreter/Interpreter.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/ErrorFormat.h"  // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
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
    for (const auto& kv : closureVal.capturedVars()) {
        funEnv->define(kv.first, kv.second);
    }
}

/// 将变异后的捕获变量写回 capturedVars（C1/P0-6 fix）
/// 仅写回原始 capturedVars 中已存在的键，跳过参数和局部变量，避免污染下次调用
void writeBackCapturedVars(Value& closureVal, std::shared_ptr<Environment>& funEnv) {
    // AUDIT-P1 fix: 应用 closeCapturedVariables 的 const_cast 模式绕过 COW。
    // capturedVars() 的非 const 重载会 ensureUnique → refCount>1 时创建副本，
    // 导致修改写到副本而非原件，closures 数组中的原始闭包 capturedVars 保持陈旧
    // （与 Environment.h 行 306-319 的 B1 fix 同一不变量）。
    const auto& constCaptured = const_cast<const Value&>(closureVal).capturedVars();
    auto& captured = const_cast<std::unordered_map<std::string, Value>&>(constCaptured);
    for (const auto& kv : funEnv->localVariables()) {
        if (captured.find(kv.first) != captured.end()) {
            captured[kv.first] = kv.second;
        }
    }
}

} // anonymous namespace


void Interpreter::visitFunCall(FunCall& node) {
    checkBreak(&node);

    // P1 重构：按调用类型分派到辅助方法
    if (node.callee) { lastValue_ = callClosureValue(node); return; }
    if (node.name == "dict" || node.name == "array") { lastValue_ = callBuiltinConstructor(node); return; }
    if (classRegistry_.find(node.name) != classRegistry_.end()) { lastValue_ = constructClassInstance(node); return; }
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
        auto r = executeSharedInput(cb, argValues.data(), argValues.size(),
                                     node.line, node.column);
        if (r.is_err()) {
            throw to_runtime_error(r);  // A1 fix: 自由函数模板
        }
        lastValue_ = std::move(r.value());
        return;
    }
    // 顶层内置函数（用户自定义函数/类优先，仅当未定义时才使用内置）
    if (isBuiltinFunction(node.name)) {
        const Value* calleePtr = currentEnv_->get(node.name);
        if (!calleePtr || !calleePtr->isClosure()) {
            lastValue_ = callBuiltinFunction(node); return;
        }
    }
    lastValue_ = callNamedFunction(node); return;
}

// ---- P1 重构：链式调用 / 表达式调用 callee(args) ----
Value Interpreter::callClosureValue(FunCall& node) {
    Value calleeVal = evaluate(node.callee.get());
    if (!calleeVal.isClosure()) {
        runtimeError("表达式求值结果不是函数，无法调用", node.line, node.column);
    }

    FunDecl* funDecl = calleeVal.closureBody();
    auto closureEnv = calleeVal.closureEnv();
    std::string effectiveName = calleeVal.closureName();

    // BUG-DBG-AUDIT-1 fix: VM 后端创建的闭包（OP_CLOSURE / REG_CLOSURE）仅持有
    // vmClosure（VMClosureData，含 chunkPtr）而无 AST body（closureBody() 返回 nullptr）。
    // 当条件断点求值沙箱注入 VM 全局变量（含此类闭包）后，条件表达式中调用该闭包会
    // 在下方 funDecl->requiredParamCount 处解引用空指针崩溃（segfault，无法被 try/catch 捕获）。
    // 此处给出明确的运行时错误而非崩溃。
    if (!funDecl) {
        runtimeError(ErrorFormat::format(
            "无法在条件断点沙箱中调用 VM 闭包 %s（缺少 AST body，仅 VM 后端可调用）",
            effectiveName.c_str()), node.line, node.column);
    }

    // F10: 支持默认参数
    size_t argCount = node.arguments.size();
    if (argCount < static_cast<size_t>(funDecl->requiredParamCount) ||
        argCount > funDecl->params.size()) {
        runtimeError(ErrorFormat::format("函数 %s 期望 %d-%zu 个参数，但传入了 %zu 个",
            effectiveName.c_str(), funDecl->requiredParamCount,
            funDecl->params.size(), argCount),
            node.line, node.column);
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
            Interpreter& interp; std::shared_ptr<Environment> prev;
            ~EnvGuard() { interp.currentEnv_ = prev; }
        } guard{ *this, currentEnv_ };
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

    // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ 的保存/恢复
    CallFrameGuard frameGuard{ *this, funDecl->returnType };
    auto prevEnv = currentEnv_;

    // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        runtimeError(ErrorFormat::format("递归深度超过限制 (%d)", MAX_RECURSION_DEPTH), node.line, node.column);
    }
    RecursionGuard recursionGuard{ recursionDepth_ };

    std::shared_ptr<Environment> funEnv;
    bool envFromSnapshot = false;
    Value result = Value::nullValue();

    // RA-A fix: 用 RAII 守卫统一管理 funEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
    // 消除原 catch(...) + throw; 的 rethrow（减少 First-chance Exception 日志噪声）。
    // 守卫在正常路径和异常路径都执行清理，逻辑与原代码严格一致。
    struct FunEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& prev;
        bool dismissed = false;
        ~FunEnvGuard() {
            if (!dismissed) {
                if (env) env->closeCapturedVariables();
                interp.currentEnv_ = prev;
            }
        }
    } envGuard{ *this, funEnv, prevEnv };

    try {
        funEnv = std::make_shared<Environment>(
            closureEnv ? closureEnv : currentEnv_);

        // C1 fix: 若闭包环境已过期（weak_ptr 失效），从 capturedVars 快照重建
        if (!closureEnv) {
            rebuildEnvFromSnapshot(calleeVal, funEnv);
            envFromSnapshot = true;
        }

        for (size_t i = 0; i < funDecl->params.size(); ++i) {
            // P1-4 fix: 补充参数类型检查（与 callNamedFunction 一致）
            if (i < funDecl->paramTypes.size() && !funDecl->paramTypes[i].empty()) {
                checkType(argValues[i], funDecl->paramTypes[i],
                    [&] { return "函数 " + effectiveName + " 的参数 " + funDecl->params[i]; },
                    node.line, node.column);
            }
            funEnv->define(funDecl->params[i], std::move(argValues[i]));
        }

        callStack_.emplace_back(effectiveName, funEnv, node.line, recursionDepth_);
        currentEnv_ = funEnv;
        executeFunctionBody(static_cast<Block&>(*funDecl->body));
    }
    catch (ReturnException& e) {
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
    if (funEnv) funEnv->closeCapturedVariables();
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
    auto r = executeSharedBuiltinFunction(
        node.name, argValues.data(), argValues.size(), node.line, node.column);

    if (r.is_err()) {
        throw to_runtime_error(r);  // A1 fix: 自由函数模板
    }
    return std::move(r.value());
}

// ---- P1 重构：类构造调用 ----
Value Interpreter::constructClassInstance(FunCall& node) {
    // 检查是否是类构造调用
    auto classIt = classRegistry_.find(node.name);
    if (classIt != classRegistry_.end()) {
        ClassInfo* cls = &classIt->second;  // #2 fix: 用指针代替引用，便evaluate后重绑定

        // 检查参数数量（构造函数为 init 方法）
        FunDecl* initMethod = findMethod(*cls, "init");

        // P0-2 fix: 支持默认参数，参数数量可在 [requiredParamCount, params.size()] 范围内
        if (initMethod && (node.arguments.size() < initMethod->requiredParamCount ||
                           node.arguments.size() > initMethod->params.size())) {
            runtimeError(ErrorFormat::format("构造函数 init 期望 %d-%zu 个参数，但传入了 %zu 个",
                initMethod->requiredParamCount, initMethod->params.size(),
                node.arguments.size()),
                node.line, node.column);
        }
        if (!initMethod && !node.arguments.empty()) {
            runtimeError(ErrorFormat::format("类 %s 没有 init 方法，但传入了 %zu 个参数",
                cls->name.c_str(), node.arguments.size()),
                node.line, node.column);
        }

        // 求值参数
        std::vector<Value> argValues;
        argValues.reserve(node.arguments.size());
        std::string className = cls->name;  // #2 fix: 缓存类名
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
        cls = &classIt->second;  // 重绑定指针到新位置
        initMethod = findMethod(*cls, "init");

        // AUDIT-P2-CORRECT fix: 类重定义后参数签名可能变化，需重新校验 argValues 数量。
        // 原实现仅在求值前用旧 initMethod 校验，求值后重新查找 initMethod 但未重新校验，
        // 导致新 init 参数更少时多余参数被静默忽略，参数更多时缺失参数走 null 路径。
        if (initMethod && (argValues.size() < static_cast<size_t>(initMethod->requiredParamCount) ||
                           argValues.size() > initMethod->params.size())) {
            runtimeError(ErrorFormat::format("类 %s 在构造期间被重定义，参数数量不匹配（init 期望 %d-%zu 个，但传入了 %zu 个）",
                cls->name.c_str(), initMethod->requiredParamCount, initMethod->params.size(),
                argValues.size()),
                node.line, node.column);
        }

        // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
        if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
            runtimeError(ErrorFormat::format("递归深度超过限制 (%d)", MAX_RECURSION_DEPTH), node.line, node.column);
        }
        RecursionGuard recursionGuard{ recursionDepth_ };

        // 创建实例
        Value instance = Value::makeInstance(cls->name);

        // 复制类默认字段值（含继承链）— B8 fix: 加入循环检测
        ClassInfo* curCls = cls;  // cls is already a pointer
        std::unordered_set<std::string> visitedClasses;
        while (curCls) {
            if (!visitedClasses.insert(curCls->name).second) break; // 检测到循环继承
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
            }
            else {
                curCls = nullptr;
            }
        }

        // 如果有 init 方法，执行它
        if (initMethod) {
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
                        checkType(argValues[i], initMethod->paramTypes[i],
                            [&] { return "类 " + node.name + " 的 init 参数 " + initMethod->params[i]; },
                            node.line, node.column);
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
                            Interpreter& interp; std::shared_ptr<Environment> prev;
                            ~EnvGuard() { interp.currentEnv_ = prev; }
                        } guard{ *this, currentEnv_ };
                        if (cls->closureEnv) {
                            currentEnv_ = cls->closureEnv;
                        }
                        Value defaultVal = evaluate(initMethod->defaultValues[i].get());
                        // AUDIT-P1 fix: 默认值同样需类型检查
                        if (i < initMethod->paramTypes.size() && !initMethod->paramTypes[i].empty()) {
                            checkType(defaultVal, initMethod->paramTypes[i],
                                [&] { return "类 " + node.name + " 的 init 参数 " + initMethod->params[i]; },
                                node.line, node.column);
                        }
                        initEnv->define(initMethod->params[i], std::move(defaultVal));
                    } else {
                        initEnv->define(initMethod->params[i], Value::nullValue());
                    }
                }
            }

            // H-新2 fix: bindInstance 必须在所有 define 之后
            Value* thisInEnv = const_cast<Value*>(initEnv->get("this"));
            if (thisInEnv) initEnv->bindInstance(thisInEnv);

            // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ + classContextStack_
            // AUDIT-BUG-F1 fix: CallFrameGuard 必须在 emplace_back 之前构造，使其保存的
            // savedStackDepth = N（不含新条目），析构时才能正确弹回到 N。若顺序颠倒，
            // savedStackDepth = N+1，析构时不弹出 init 帧，循环构造实例导致 callStack_ 泄漏。
            CallFrameGuard frameGuard{ *this, initMethod->returnType, /*manageCtx=*/true };

            // 压入调用帧
            callStack_.emplace_back(node.name + ".init", initEnv, node.line, recursionDepth_);

            // 切换环境
            auto prevEnv = currentEnv_;
            currentEnv_ = initEnv;

            // 压入类上下文（super 解析用）
            classContextStack_.push_back(cls->name);

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
            } envGuard{ *this, initEnv, prevEnv };

            try {
                executeFunctionBody(static_cast<Block&>(*initMethod->body));
            }
            catch (const ReturnException&) {
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

        // B1 fix: recursionDepth_ 由 RAII guard 自动恢复（无需手动递减）
        return instance;
    }
    return Value::nullValue();
}

// ---- P1 重构：普通函数/闭包调用 ----
Value Interpreter::callNamedFunction(FunCall& node) {
    // 检查环境中是否有闭包值
    std::shared_ptr<Environment> closureEnv;
    Value* closureValPtr = nullptr;  // C1 fix: 保存闭包值指针用于 capturedVars 回退
    // A3 fix: 使用 shared_ptr 持有 FunDecl，源（resolvedDecl/funRegistry_/closureBodyShared）
    // 均返回 shared_ptr，确保缓存写入时共享所有权
    std::shared_ptr<FunDecl> funDecl;
    std::string effectiveName = node.name;  // 实际函数名（闭包时可能不同于调用变量名）

    // 快速路径：使用缓存的函数体（跳过环境查找和 funRegistry_ 查找）
    // M7 fix: 检查代数是否匹配，函数重定义后缓存失效
    // P1-2 fix: 若变量已被重赋值为非闭包，缓存失效，回退慢路径
    if (node.isResolved && node.resolvedDecl && node.resolvedGen_ == funRegistryGen_) {
        const Value* calleePtr = currentEnv_->get(node.name);
        if (calleePtr && calleePtr->isClosure()) {
            funDecl = node.resolvedDecl;
            closureEnv = calleePtr->closureEnv();
            effectiveName = calleePtr->closureName();
            closureValPtr = const_cast<Value*>(calleePtr);  // C1 fix
        }
        else {
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
            closureValPtr = const_cast<Value*>(calleePtr);  // C1 fix
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
                runtimeError(node.name + " 不是函数，无法调用", node.line, node.column);
            }
            // 仅在 calleePtr 是闭包但 body 缺失时回退到 funRegistry_
            auto it = funRegistry_.find(effectiveName);
            if (it == funRegistry_.end()) {
                runtimeError("未定义的函数: " + node.name, node.line, node.column);
            }
            funDecl = it->second;
        }

        // 缓存解析结果供后续调用使用
        node.resolvedDecl = funDecl;
        node.isResolved = true;
        node.resolvedGen_ = funRegistryGen_;  // M7: 记录当前代数
    }

    // 检查参数数量
    // F10: 支持默认参数，参数数量可在 [requiredParamCount, params.size()] 范围内
    size_t argCount = node.arguments.size();
    if (argCount < static_cast<size_t>(funDecl->requiredParamCount) ||
        argCount > funDecl->params.size()) {
        runtimeError(ErrorFormat::format("函数 %s 期望 %d-%zu 个参数，但传入了 %zu 个",
            node.name.c_str(), funDecl->requiredParamCount,
            funDecl->params.size(), argCount),
            node.line, node.column);
    }

    // 求值参数
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

    // F10: 为缺失的参数填充默认值
    // 默认值在函数定义时的闭包环境中求值（与函数体同级）
    // BUGFIX-P2 fix: 使用 RAII guard 恢复 currentEnv_，防止 evaluate() 抛异常时泄漏错误 scope
    if (argCount < funDecl->params.size()) {
        auto savedEnv = currentEnv_;
        struct EnvGuard {
            Interpreter& interp; std::shared_ptr<Environment> prev;
            ~EnvGuard() { interp.currentEnv_ = prev; }
        } guard{ *this, currentEnv_ };
        // 切换到闭包环境（函数定义时的环境），使默认值表达式能访问外层变量
        if (closureEnv) {
            currentEnv_ = closureEnv;
        }
        for (size_t i = argCount; i < funDecl->params.size(); ++i) {
            if (funDecl->defaultValues[i]) {
                argValues.push_back(evaluate(funDecl->defaultValues[i].get()));
            } else {
                // 不应发生（requiredParamCount 已校验），防御性处理
                argValues.push_back(Value::nullValue());
            }
        }
    }

    // B3 fix: CallFrameGuard 自动管理 currentFunctionReturnType_ + callStack_ 的保存/恢复
    CallFrameGuard frameGuard{ *this, funDecl->returnType };
    auto prevEnv = currentEnv_;

    // S2 fix: 统一使用 RecursionGuard RAII 管理递归深度
    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        runtimeError(ErrorFormat::format("递归深度超过限制 (%d)", MAX_RECURSION_DEPTH), node.line, node.column);
    }
    RecursionGuard recursionGuard{ recursionDepth_ };

    Value result = Value::nullValue();
    std::shared_ptr<Environment> funEnv;  // C1 fix: 声明在 try 外以便写回
    bool envFromSnapshot = false;

    // RA-A fix: RAII 守卫统一管理 funEnv 的 closeCapturedVariables 与 currentEnv_ 恢复，
    // 消除原 catch(...) + throw; 的 rethrow。
    struct FunEnvGuard {
        Interpreter& interp;
        std::shared_ptr<Environment>& env;
        std::shared_ptr<Environment>& prev;
        bool dismissed = false;
        ~FunEnvGuard() {
            if (!dismissed) {
                if (env) env->closeCapturedVariables();
                interp.currentEnv_ = prev;
            }
        }
    } envGuard{ *this, funEnv, prevEnv };

    try {
        // 参数类型检查
        for (size_t i = 0; i < funDecl->params.size() && i < funDecl->paramTypes.size(); ++i) {
            if (!funDecl->paramTypes[i].empty()) {
                checkType(argValues[i], funDecl->paramTypes[i],
                    [&] { return "函数 " + node.name + " 的参数 " + funDecl->params[i]; },
                    node.line, node.column);
            }
        }

        // 创建新环境：使用闭包捕获的环境作为父级（如果有的话）
        if (closureEnv) {
            funEnv = std::make_shared<Environment>(closureEnv);
        }
        else {
            funEnv = std::make_shared<Environment>(currentEnv_);
        }

        // C1 fix: 若闭包环境已过期（weak_ptr 失效），从 capturedVars 快照重建
        if (!closureEnv && closureValPtr) {
            rebuildEnvFromSnapshot(*closureValPtr, funEnv);
            envFromSnapshot = true;
        }

        // 绑定参数（move 避免深拷贝）
        for (size_t i = 0; i < funDecl->params.size(); ++i) {
            funEnv->define(funDecl->params[i], std::move(argValues[i]));
        }

        // 压入调用栈
        // BUG-DBG-4 fix: 使用 effectiveName 而非 node.name，与 callClosureValue (L195) 一致。
        // 原实现使用 node.name（调用变量名），闭包赋值给不同变量时显示变量名而非闭包名。
        callStack_.emplace_back(effectiveName, funEnv, node.line, recursionDepth_);

        // 切换环境
        currentEnv_ = funEnv;

        // 执行函数体（不求值返回值：无 return 语句时函数应返回 null）
        executeFunctionBody(static_cast<Block&>(*funDecl->body));
    }
    catch (ReturnException& e) {
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
    if (funEnv) funEnv->closeCapturedVariables();
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
