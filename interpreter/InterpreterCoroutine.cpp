#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"
#include "common/RuntimeLimits.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/CoroutineFiber.h"
#include "interpreter/Interpreter.h"

// ============================================================
// InterpreterCoroutine.cpp — 协程/生成器实现（重放 + 真挂起双模式）
// ------------------------------------------------------------
// 拆分自 Interpreter.cpp，包含协程相关的所有方法：
//   - makeCoroutineValue: 构造 Coroutine 值
//   - callCoroutineNext: .next() 核心方法（挂起/重放双模式）
//   - coroutineSuspend / coroutineFiberBody: 真挂起路径（MINILANG_CORO_FIBER=1）
//   - handleCoroutineMethod: 协程方法分发（.next/.done）
//
// 模式选择（MINILANG_CORO_FIBER，见 interpreter/CoroutineFiber.h）：
//   - =1（Windows fiber）：真挂起。yield 保存执行位置切回调用方，.next() 从
//     挂起点恢复——前缀副作用只执行一次，n 次 .next() 总代价 O(n)。递归
//     树遍历解释器的 C++ 栈即延续，fiber 切换零语义损失（含 yield 位于
//     嵌套函数调用/表达式中间的场景）。
//   - =0（其他平台）：重放模式。每次 .next() 从头重新执行函数体，用运行时
//     yield 执行计数器跳过已消费的 yield（见 callCoroutineNext 重放分支）。
//
// 历史注记：本文件曾注释"StackVM/RegisterVM 用真挂起模式（D.5 实现）"——
// 该描述与代码不符（四后端原均为重放模式，VM 的 OP_YIELD/REG_YIELD 同样
// 走计数器跳过），第四轮优化起三后端在本宏启用的平台上同步落地真挂起，
// 未启用平台上保留四后端一致的重放语义（ADR-002：不允许单后端分叉）。
//
// yield 表达式值语义（三端统一）：被跳过（重放）或恢复后（挂起）的 yield
// 表达式求值为其自身 yield 值——重放由"push(yieldValue)/lastValue_=yieldValue"
// 实现，挂起由恢复路径的同一赋值实现，两种模式的可观察行为逐点一致，
// 唯一差异是挂起模式副作用只执行一次。
// ============================================================

Value Interpreter::makeCoroutineValue(std::shared_ptr<FunDecl> generatorDecl, std::shared_ptr<Environment> closureEnv,
                                      std::vector<Value> argValues) {
    // CoroutineData 持有：生成器 AST + 定义时环境 + 调用参数 + yieldCount（编译期分配总数）
    // currentYieldId 初始为 0（下一次 .next() 要返回的 yieldId）
    int yc = generatorDecl->yieldCount;
    return Value::makeCoroutine(std::move(generatorDecl), std::move(closureEnv), std::move(argValues), yc);
}

#if MINILANG_CORO_FIBER
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// —— Windows Fiber API 包装（仅本 TU 使用；见 CoroutineFiber.h 的隔离说明）——
namespace minilang_fiber {
using FiberHandle = void*;
// 幂等转换：已是 fiber（同线程二次转换）时 ConvertThreadToFiberEx 返回 NULL
// 并置 ERROR_ALREADY_FIBER——复用当前 fiber。
inline FiberHandle convertThreadToFiber() {
    void* primary = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    return primary ? primary : GetCurrentFiber();
}
// reserve 4MB（虚拟预留，按需 commit）：生成器体内深度递归的 C++ 栈安全余量
// （递归深度受 MAX_RECURSION_DEPTH 保护）
inline FiberHandle createFiber(void (*proc)(void*), void* arg) {
    return CreateFiberEx(0, 4 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, reinterpret_cast<LPFIBER_START_ROUTINE>(proc),
                         arg);
}
inline void switchToFiber(FiberHandle target) {
    SwitchToFiber(target);
}
inline void deleteFiber(FiberHandle fiber) {
    DeleteFiber(fiber);
}
inline FiberHandle currentFiber() {
    return GetCurrentFiber();
}
} // namespace minilang_fiber

namespace {
// CoroutineData 为 Value 私有嵌套类型，经公有访问器 coroutineData() 的返回类型
// 推导取得指针类别（避免在 Value 之外命名该类型）
using CoroutineDataRawPtr = std::decay_t<decltype(*std::declval<Value&>().coroutineData())>*;
} // namespace

// 挂起状态定义（Interpreter.h 内前向声明的嵌套类型；对 Value.h 不透明，
// CoroutineData::interpreterFiber 以 shared_ptr<void> 持有，deleter 在本 TU
// 完成类型完整的平台资源释放）
struct Interpreter::CoroutineFiberState {
    Interpreter* interp = nullptr;
    CoroutineDataRawPtr cd = nullptr; // 仅 fiber 活跃期间由 .next() 调用方的 coroVal 引用计数保持存活
    minilang_fiber::FiberHandle fiber = nullptr;
    minilang_fiber::FiberHandle resumer = nullptr;
    bool completed = false; // 生成器体执行终结（自然结束/显式 return/错误逃逸）
    bool failed = false;    // 错误逃逸（fiber 不可复用；.next() 重抛后丢弃重建，对齐重放语义）
    std::exception_ptr pendingError;
    Value handoffValue;           // yield 值交接（挂起→恢复）
    size_t resumerStackDepth = 0; // .next() 入口时 callStack_ 深度（挂起截断基线，=守卫 savedStackDepth）
    // 挂起期间的解释器上下文快照（env/帧持 GC 根：防 suspendedEnv 被 envPool 复用
    // 或仅由 fiber C++ 栈持有的环境链在 mark-sweep 中失根）
    std::shared_ptr<Environment> suspendedEnv;
    std::vector<CallFrame> suspendedCallStack;
    std::string suspendedReturnType;
    int suspendedRecursionDepth = 0;
    std::shared_ptr<Environment> funEnv; // 生成器函数环境（终结时 closeCapturedVariables，对齐重放守卫）
    ~CoroutineFiberState() {
        if (fiber)
            minilang_fiber::deleteFiber(fiber); // 栈内存直接释放：其上 C++ 析构不执行，
                                                // 未释放的 shared_ptr 引用计数一并遗忘——
                                                // 仅发生在协程弃用/解释器销毁路径的有界泄漏
    }
};

void Interpreter::coroutineFiberTrampoline(void* arg) {
    static_cast<CoroutineFiberState*>(arg)->interp->coroutineFiberBody();
}

void Interpreter::coroutineFiberBody() {
    auto* fs = static_cast<CoroutineFiberState*>(activeCoroutineFiber_);
    auto* cd = fs->cd;
    try {
        // S2 对齐：fiber 生命周期内持有递归深度一层（恶意嵌套生成器耗尽栈防护）
        if (recursionDepth_ + 1 >= MAX_RECURSION_DEPTH) {
            runtimeError(ErrorFormat::formatStd(ErrorMessages::kRecursionDepthExceededFmtStd, MAX_RECURSION_DEPTH), 0,
                         0, DiagCodes::kRecursionDepth);
        }
        RecursionGuard recursionGuard{recursionDepth_};
        // 生成器环境：父级为定义时闭包环境（若有），否则为 .next() 调用点环境（对齐重放建帧）
        auto funEnv = std::make_shared<Environment>(cd->closureEnv ? cd->closureEnv : currentEnv_);
        fs->funEnv = funEnv;
        for (size_t i = 0; i < cd->generatorDecl->params.size() && i < cd->args.size(); ++i) {
            funEnv->define(cd->generatorDecl->params[i], cd->args[i]);
        }
        callStack_.emplace_back(cd->generatorDecl->name, funEnv, cd->generatorDecl->line, recursionDepth_);
        currentEnv_ = funEnv;
        currentFunctionReturnType_ = cd->generatorDecl->returnType;
        // B1 TCO: 生成器执行非蹦床，禁用尾调用上下文（信号不可穿透 .next() 边界）
        TcoScopeGuard tcoGuard{*this, nullptr, std::string(), /*isMethod=*/false, /*enabled=*/false};
        executeFunctionBody(static_cast<Block&>(*cd->generatorDecl->body));
        // 函数体自然结束（无 return、无未耗尽 yield）：协程耗尽，最终值为 null（对齐重放）
        cd->done = true;
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(Value::nullValue());
        funEnv->closeCapturedVariables();
    } catch (const ReturnException& e) {
        // 生成器函数显式 return：协程耗尽，返回 return 值
        cd->done = true;
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(std::move(e.returnValue));
        if (fs->funEnv)
            fs->funEnv->closeCapturedVariables();
    } catch (...) {
        // 错误逃逸（RuntimeError/ThrowException/DebugStopException）：封送后由
        // callCoroutineNext 重抛——调用方上下文由其守卫恢复，done 不置位（对齐重放）
        fs->failed = true;
        fs->pendingError = std::current_exception();
        if (fs->funEnv)
            fs->funEnv->closeCapturedVariables();
    }
    fs->completed = true;
    minilang_fiber::switchToFiber(fs->resumer);
    // 不可达：fiber 终结后仅会被 deleteFiber 回收（CoroutineData 析构或 .next() 内 reset）
}

void Interpreter::coroutineSuspend(Value yieldValue) {
    auto* fs = static_cast<CoroutineFiberState*>(activeCoroutineFiber_);
    auto* cd = fs->cd;
    fs->handoffValue = yieldValue;
    // currentValueBox / currentYieldId / done 判定逐点对齐重放 catch 分支：
    // 最终 yield（currentYieldId 达到 yieldCount）后协程耗尽，尾部语句不执行
    cd->currentValueBox.clear();
    cd->currentValueBox.push_back(yieldValue);
    cd->currentYieldId++;
    if (cd->currentYieldId >= cd->yieldCount)
        cd->done = true;
    // 保存上下文并截断调用栈到调用方基线（调用方在两次 .next() 之间继续执行，
    // 不得看到生成器的陈旧帧；帧随快照保活并持 GC 根）
    fs->suspendedEnv = currentEnv_;
    fs->suspendedCallStack.assign(std::make_move_iterator(callStack_.begin() + fs->resumerStackDepth),
                                  std::make_move_iterator(callStack_.end()));
    callStack_.resize(fs->resumerStackDepth);
    fs->suspendedReturnType = currentFunctionReturnType_;
    fs->suspendedRecursionDepth = recursionDepth_;
    minilang_fiber::switchToFiber(fs->resumer);
    // —— 恢复点：下一次 .next() 切回。此刻解释器成员为调用方状态，先还原 ——
    currentEnv_ = std::move(fs->suspendedEnv);
    currentFunctionReturnType_ = std::move(fs->suspendedReturnType);
    recursionDepth_ = fs->suspendedRecursionDepth;
    if (!fs->suspendedCallStack.empty()) {
        callStack_.insert(callStack_.end(), std::make_move_iterator(fs->suspendedCallStack.begin()),
                          std::make_move_iterator(fs->suspendedCallStack.end()));
        fs->suspendedCallStack.clear();
    }
    // yield 表达式值 = 自身 yield 值（对齐重放 skip 语义：lastValue_ = yieldValue）
    lastValue_ = fs->handoffValue;
}
#endif // MINILANG_CORO_FIBER

Value Interpreter::callCoroutineNext(Value& coroVal) {
    // CoroutineData 是 Value 的私有嵌套类型，使用 auto* 推导避免显式声明类型名
    auto* cd = coroVal.coroutineData();

    // 已耗尽：返回 currentValue（最后一次 yield/return 的值），语义与 Python 一致
    if (cd->done) {
#if MINILANG_CORO_FIBER
        // 最终 yield 触发的 done：fiber 停在 yield 点永不再恢复，弃用之
        // （完成路径的 done 此处 reset 为 no-op）
        cd->interpreterFiber.reset();
#endif
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

#if MINILANG_CORO_FIBER
    // —— 真挂起路径（Windows fiber）：yield 保存执行位置切回，副作用只执行一次。
    // 成功返回/失败重抛均经守卫析构恢复调用方上下文（prevEnv/target/计数/returnType
    // + 调用栈收缩；挂起路径的调用栈已由 coroutineSuspend 截断，收缩为 no-op）。
    {
        auto* fs = static_cast<CoroutineFiberState*>(cd->interpreterFiber.get());
        if (!fs) {
            minilang_fiber::convertThreadToFiber(); // 幂等：已转换线程复用当前 fiber
            fs = new CoroutineFiberState();
            fs->interp = this;
            fs->cd = cd;
            cd->interpreterFiber = {fs, [](void* p) { delete static_cast<CoroutineFiberState*>(p); }};
            fs->fiber = minilang_fiber::createFiber(&coroutineFiberTrampoline, fs);
        }
        if (activeCoroutineFiber_ == fs) {
            runtimeError("生成器不能在自身执行体内调用 .next()（真挂起模式不支持自恢复）", 0, 0);
        }
        int prevRecursion = recursionDepth_;
        fs->resumerStackDepth = callStack_.size();
        CoroutineFiberState* prevActive = activeCoroutineFiber_;
        activeCoroutineFiber_ = fs; // visitYieldExpr 据此进入挂起分支（含生成器体内嵌套函数的 yield）
        fs->resumer = minilang_fiber::currentFiber();
        minilang_fiber::switchToFiber(fs->fiber);
        // 返回点：fiber 挂起（yield）或终结（自然结束/return/错误逃逸）
        activeCoroutineFiber_ = prevActive;
        recursionDepth_ = prevRecursion;
        if (fs->failed) {
            auto err = fs->pendingError;
            cd->interpreterFiber.reset(); // 死 fiber 弃用：下次 .next() 全新重建（对齐重放重新执行）
            std::rethrow_exception(err);
        }
        if (fs->completed)
            cd->interpreterFiber.reset(); // fiber 生命周期终结（deleter 删平台资源）
        // yield 值 / return 值 / 自然耗尽 null 均已写入 currentValueBox
        return cd->currentValueBox.empty() ? Value::nullValue() : cd->currentValueBox.front();
    }
#else
    // —— 重放路径（MINILANG_CORO_FIBER=0 平台）：见文件头"模式选择"——
    // Windows 下此路径不编译（fiber 分支全路径 return，重放代码不可达）。

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
#endif // MINILANG_CORO_FIBER
}

#if !MINILANG_CORO_FIBER
void Interpreter::coroutineSuspend(Value) {
    // 不可达：非 fiber 平台 activeCoroutineFiber_ 恒为 nullptr（visitYieldExpr
    // 挂起分支不触发）；此定义仅为满足符号引用
}
#endif

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
