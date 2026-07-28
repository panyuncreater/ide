#include "interpreter/Interpreter.h"
#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"
#include "common/RuntimeLimits.h"
#include "interpreter/BuiltinMethods.h"

// ============================================================
// InterpreterCoroutine.cpp — 协程/生成器（重放模式）实现
// ------------------------------------------------------------
// 拆分自 Interpreter.cpp，包含协程相关的所有方法：
//   - makeCoroutineValue: 构造 Coroutine 值
//   - callCoroutineNext: 重放执行 .next() 核心方法
//   - handleCoroutineMethod: 协程方法分发（.next/.done）
//
// 重放模式 vs 真挂起模式：
//   - Interpreter 用重放模式（无栈快照保存机制），每次 .next() 重新执行函数体
//   - StackVM/RegisterVM 用真挂起模式（D.5 实现），保存帧快照精确恢复执行点
//   - 四后端语义一致性：对无副作用的 yield 表达式，两种模式结果一致
// ============================================================

Value Interpreter::makeCoroutineValue(std::shared_ptr<FunDecl> generatorDecl, std::shared_ptr<Environment> closureEnv,
                                      std::vector<Value> argValues) {
    // CoroutineData 持有：生成器 AST + 定义时环境 + 调用参数 + yieldCount（编译期分配总数）
    // currentYieldId 初始为 0（下一次 .next() 要返回的 yieldId）
    int yc = generatorDecl->yieldCount;
    return Value::makeCoroutine(std::move(generatorDecl), std::move(closureEnv), std::move(argValues), yc);
}

Value Interpreter::callCoroutineNext(Value& coroVal) {
    // CoroutineData 是 Value 的私有嵌套类型，使用 auto* 推导避免显式声明类型名
    auto* cd = coroVal.coroutineData();

    // 已耗尽：返回 currentValue（最后一次 yield/return 的值），语义与 Python 一致
    if (cd->done) {
        return cd->currentValueBox.empty() ? Value::nullValue() : cd->currentValueBox.front();
    }

    // 保存调用方上下文（currentEnv_ / currentCoroutineTargetYieldId_ /
    // currentYieldExecutionCount_ / currentFunctionReturnType_）
    auto prevEnv = currentEnv_;
    int prevTargetYieldId = currentCoroutineTargetYieldId_;
    int prevYieldExecCount = currentYieldExecutionCount_;
    std::string prevReturnType = currentFunctionReturnType_;

    // RAII 守卫：异常路径同样恢复上下文
    // AUDIT-R3 P2-8 fix: 调用栈收缩并入守卫——原实现的弹帧循环位于 try/catch
    // 之后的普通代码路径，try 只捕获 YieldSignal/ReturnException；生成器体抛
    // RuntimeError/ThrowException/DebugStopException 时异常直接逃出函数，
    // callStack_ 残留陈旧帧（脚本 catch 后继续执行时调试栈面板错乱、
    // 反复失败的 next 调用持续累积帧）。
    struct CoroutineContextGuard {
        Interpreter& interp;
        std::shared_ptr<Environment> prevEnv;
        int prevTargetYieldId;
        int prevYieldExecCount;
        std::string prevReturnType;
        std::shared_ptr<Environment> funEnv;
        size_t savedStackDepth; // AUDIT-R3 P2-8 fix: 析构时统一收缩调用栈
        ~CoroutineContextGuard() {
            if (funEnv)
                funEnv->closeCapturedVariables();
            interp.currentEnv_ = prevEnv;
            interp.currentCoroutineTargetYieldId_ = prevTargetYieldId;
            interp.currentYieldExecutionCount_ = prevYieldExecCount;
            interp.currentFunctionReturnType_ = std::move(prevReturnType);
            // AUDIT-R3 P2-8 fix: 异常路径也恢复调用栈深度
            while (interp.callStack_.size() > savedStackDepth) {
                interp.callStack_.pop_back();
            }
        }
    } guard{*this, prevEnv, prevTargetYieldId, prevYieldExecCount, prevReturnType, nullptr, callStack_.size()};

    // S2 fix: 递归深度保护（重放也算递归调用，避免恶意嵌套生成器耗尽栈）
    if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
        runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH), 0, 0,
                     DiagCodes::kRecursionDepth);
    }
    RecursionGuard recursionGuard{recursionDepth_};

    Value result = Value::nullValue();
    bool reachedYield = false;

    // AUDIT-R3 P2-8 fix: savedStackDepth 已入 CoroutineContextGuard（构造时记录），
    // 任何退出路径（含 RuntimeError/ThrowException/DebugStopException 逃逸）
    // 均由守卫析构统一收缩调用栈，不再依赖 try/catch 之后的手动弹帧循环。

    try {
        // 创建生成器函数环境：父级为定义时闭包环境（若有），否则为当前环境
        std::shared_ptr<Environment> funEnv =
            std::make_shared<Environment>(cd->closureEnv ? cd->closureEnv : currentEnv_);
        guard.funEnv = funEnv; // 守卫持有以便异常路径 closeCapturedVariables

        // 绑定调用参数（重放时每次重新绑定，参数值在创建协程时已固定）
        for (size_t i = 0; i < cd->generatorDecl->params.size() && i < cd->args.size(); ++i) {
            funEnv->define(cd->generatorDecl->params[i], cd->args[i]);
        }

        // 压入调用栈（用于调试器显示调用层次）
        callStack_.emplace_back(cd->generatorDecl->name, funEnv, cd->generatorDecl->line, recursionDepth_);

        currentEnv_ = funEnv;
        currentCoroutineTargetYieldId_ = cd->currentYieldId;
        // R164 fix: 每次重放开始时重置运行时 yield 执行计数器
        currentYieldExecutionCount_ = 0;
        currentFunctionReturnType_ = cd->generatorDecl->returnType;

        // 重放执行函数体（从头开始）
        // B1 TCO: 生成器重放非蹦床，禁用尾调用上下文（信号不可穿透 .next() 边界）
        TcoScopeGuard tcoGuard{*this, nullptr, std::string(), /*isMethod=*/false, /*enabled=*/false};
        executeFunctionBody(static_cast<Block&>(*cd->generatorDecl->body));
        // 函数体自然结束（无 return，无未耗尽的 yield）：协程耗尽
        cd->done = true;
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
    } catch (const YieldSignal& e) {
        // 命中目标 yield：返回 yield 值，递增 currentYieldId
        result = e.yieldValue;
        reachedYield = true;
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
        cd->currentYieldId++;
        if (cd->currentYieldId >= cd->yieldCount) {
            cd->done = true;
        }
    } catch (const ReturnException& e) {
        // 生成器函数显式 return：协程耗尽，返回 return 值
        result = std::move(e.returnValue);
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
        cd->done = true;
    }

    // AUDIT-R3 P2-8 fix: 弹帧由 CoroutineContextGuard 析构统一处理（含异常路径）

    (void)reachedYield; // 防止未使用警告
    return result;
}

BuiltinMethodResult Interpreter::handleCoroutineMethod(const std::string& method, Value& obj,
                                                       const std::vector<Value>& args, int line, int col) {
    if (method == "next") {
        if (!args.empty()) {
            runtimeError("coroutine.next() 不接受参数", line, col);
        }
        Value result = callCoroutineNext(obj);
        return BuiltinMethodResult{std::move(result), true};
    }
    if (method == "done") {
        if (!args.empty()) {
            runtimeError("coroutine.done() 不接受参数", line, col);
        }
        auto* cd = obj.coroutineData();
        return BuiltinMethodResult{Value(cd->done), false};
    }
    runtimeError("coroutine 类型不支持方法 " + method, line, col);
}
