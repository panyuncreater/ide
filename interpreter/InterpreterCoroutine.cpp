#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"
#include "common/Logger.h"
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
#include <thread>
#if defined(_WIN32)
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
#elif defined(__linux__)
#include <cstdint>
#include <cstdlib>
#include <ucontext.h>

// —— POSIX ucontext 包装（Linux；见 CoroutineFiber.h 的隔离说明）——
// 切换协议与 Windows fiber 对称：currentFiber 由 thread_local 登记维护，
// switchToFiber 经 swapcontext 双向保存/恢复（每个 FiberContext 持有自己的
// ucontext 记录），首次进入经 fiberEntry 登记后转入用户过程。
namespace minilang_fiber {

// 与 Windows fiber 同余量（4MB）：生成器体内深度递归的 C++ 栈安全余量
// （递归深度受 MAX_RECURSION_DEPTH 保护）
constexpr size_t kFiberStackSize = 4 * 1024 * 1024;

// 每个 fiber 一个堆上控制块：ucontext 记录 + 首次入口参数 + 栈所有权
// （主线程上下文 stack 为 nullptr）。FiberHandle 即指向控制块。
struct FiberContext {
    ucontext_t ctx{};
    void (*proc)(void*) = nullptr;
    void* arg = nullptr;
    unsigned char* stack = nullptr;
    size_t stackSize = 0;
};

using FiberHandle = FiberContext*;

// ASan fiber 标注（ADR-007 平台覆盖要求）：备用栈切换必须告知 ASan，
// 否则 mark-sweep/栈扫描按主栈边界误报（linux-asan 门禁验证该路径）。
// 符号为 ASan 运行时 ABI 稳定接口，直接 extern "C" 声明避免头文件依赖差异。
#if defined(__SANITIZE_ADDRESS__)
#define MINILANG_FIBER_ASAN 1
#elif defined(__has_feature)
#define MINILANG_FIBER_ASAN __has_feature(address_sanitizer)
#else
#define MINILANG_FIBER_ASAN 0
#endif
#if MINILANG_FIBER_ASAN
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, size_t* size_old);
}
#endif

// 线程当前 fiber 登记（switchToFiber 双向保存/恢复协议的锚点）
thread_local FiberContext* t_currentFiber = nullptr;

// 首次进入协程 fiber：完成 ASan 到栈标注（fake stack 从无到有，无需保存），
// 登记当前 fiber，转入用户过程。proc 按协议不返回（终结前 switchToFiber
// 回 resumer，见 coroutineFiberBody 尾部注释）。
void fiberEntry(int lo, int hi) {
#if MINILANG_FIBER_ASAN
    __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
    uintptr_t addr = (static_cast<uintptr_t>(static_cast<uint32_t>(hi)) << 32) | static_cast<uint32_t>(lo);
    auto* self = reinterpret_cast<FiberContext*>(addr);
    t_currentFiber = self;
    self->proc(self->arg);
    // 不可达（对齐 Windows fiber proc 返回语义的防御收紧）：协议破坏时
    // 显式终止而非静默线程退出/未定义回落（uc_link 为 nullptr）
    std::abort();
}

// 幂等转换：ucontext 无需线程级转换——惰性构造主线程控制块并登记
// （无专有栈：主线程栈即延续）。对齐 Windows 版"同线程二次转换复用"语义。
inline FiberHandle convertThreadToFiber() {
    if (!t_currentFiber) {
        t_currentFiber = new FiberContext();
        getcontext(&t_currentFiber->ctx);
    }
    return t_currentFiber;
}
// 栈内存新分配（惰性页分配，未触碰页不占 RSS）：对齐 Windows 4MB 余量
inline FiberHandle createFiber(void (*proc)(void*), void* arg) {
    auto* fc = new FiberContext();
    fc->proc = proc;
    fc->arg = arg;
    fc->stack = new unsigned char[kFiberStackSize];
    fc->stackSize = kFiberStackSize;
    getcontext(&fc->ctx);
    fc->ctx.uc_stack.ss_sp = fc->stack;
    fc->ctx.uc_stack.ss_size = fc->stackSize;
    fc->ctx.uc_link = nullptr; // 终结经 switchToFiber(resumer) 离开，不回落
    // makecontext 仅能传 int 宽参数：控制块指针拆高低 32 位（x86-64/Linux）
    uintptr_t addr = reinterpret_cast<uintptr_t>(fc);
    makecontext(&fc->ctx, reinterpret_cast<void (*)()>(&fiberEntry), 2, static_cast<int>(addr & 0xFFFFFFFFu),
                static_cast<int>(addr >> 32));
    return fc;
}
inline void switchToFiber(FiberHandle target) {
    FiberContext* self = t_currentFiber;
#if MINILANG_FIBER_ASAN
    void* fakeStackSave = nullptr;
    __sanitizer_start_switch_fiber(&fakeStackSave, target->stack, target->stackSize);
#endif
    swapcontext(&self->ctx, &target->ctx);
#if MINILANG_FIBER_ASAN
    __sanitizer_finish_switch_fiber(fakeStackSave, nullptr, nullptr);
#endif
    t_currentFiber = self;
}
inline void deleteFiber(FiberHandle fc) {
    // 栈内存直接释放（对齐 Windows DeleteFiber 语义）：其上 C++ 析构不执行，
    // 未释放的 shared_ptr 引用计数一并遗忘——仅发生在协程弃用/解释器销毁
    // 路径的有界泄漏（见 CoroutineFiberState 析构注释）
    delete[] fc->stack;
    delete fc;
}
inline FiberHandle currentFiber() {
    return t_currentFiber;
}
} // namespace minilang_fiber
#else
#error "MINILANG_CORO_FIBER=1 平台缺少 fiber 平台层（支持：Windows fiber / Linux ucontext）"
#endif // _WIN32 / __linux__

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
    std::thread::id ownerThread; // 创建本 fiber 的线程（§3.8 跨线程恢复防御：fiber 只能由创建线程恢复）
    bool completed = false; // 生成器体执行终结（自然结束/显式 return/错误逃逸）
    bool failed = false;    // 错误逃逸（fiber 不可复用；保留至协程终结，后续 .next() 持续重抛同一错误对齐重放）
    std::exception_ptr pendingError;
    Value handoffValue;           // yield 值交接（挂起→恢复）
    size_t resumerStackDepth = 0; // .next() 入口时 callStack_ 深度（挂起截断基线，=守卫 savedStackDepth）
    // 挂起期间的解释器上下文快照（P0 fix 2026-10-21：快照持 shared_ptr 仅保对象存活，
    // 不构成 mark-sweep 根——根集经 CoroutineData::gcRootContributor →
    // collectCoroutineFiberGcRoots 贡献。此前注释声称"env/帧持 GC 根"但从未接线，
    // 挂起窗口的增量 GC 会清空仅被快照环境持有的容器元素）
    std::shared_ptr<Environment> suspendedEnv;
    std::vector<CallFrame> suspendedCallStack;
    std::string suspendedReturnType;
    int suspendedRecursionDepth = 0;
    // P1 fix（2026-10-22）：TCO 五成员纳入挂起/恢复协议（对称于 returnType/depth）——
    // 守卫跨挂起边界滞留曾致 ① 挂起窗口 disabled 态滞留（调用方尾调用退化普通递归，
    // 深度叠加可虚假触发 256 上限）② 生成器在异构上下文终结时守卫析构写回首次进入时
    // 的陈旧蹦床快照（尾调用被误分类为 SelfFunction 无 target → 终结上下文函数体携
    // 错误实参重跑、被调函数体从未执行）。
    FunDecl* suspendedTcoDecl = nullptr;
    std::string suspendedTcoName;
    bool suspendedTcoIsMethod = false;
    bool suspendedTcoEnabled = false;
    int suspendedTcoTryDepth = 0;
    std::shared_ptr<Environment> funEnv; // 生成器函数环境（终结时 closeCapturedVariables，对齐重放守卫）
    ~CoroutineFiberState() {
        if (!fiber)
            return;
        if (ownerThread == std::this_thread::get_id()) {
            minilang_fiber::deleteFiber(fiber); // 栈内存直接释放：其上 C++ 析构不执行，
                                                // 未释放的 shared_ptr 引用计数一并遗忘——
                                                // 仅发生在协程弃用/解释器销毁路径的有界泄漏
        } else {
            // §9.2 清偿（2026-10-22）：跨线程析构防御。DeleteFiber 的执行线程 =
            // CoroutineData 最后一次释放线程（RefCounted atomic 归零侧），IDE 可达
            // （ReplPanel future.get() 主线程析构挂起协程 Value / restoreReplState 同构），
            // Windows 跨线程 DeleteFiber 为未文档化行为。非创建线程析构时有意弃置
            // fiber（不调用 deleteFiber，泄漏其栈约 4MB/协程，有界且仅命中 parked
            // fiber，无 UAF），与 ADR-007 Known Limitation 弃置语义对齐；其余成员
            // 照常析构（cd 保活语义不变）。语言层不可达（单线程执行模型 + §3.8 恢复
            // 防御），仅嵌入层可达。
            Logger::Warning("协程 fiber 被非创建线程析构：有意弃置（泄漏栈内存，避免跨线程 DeleteFiber 未定义行为）",
                            "Coroutine");
        }
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
    // P1 fix（2026-10-22）：TCO 五成员随快照保存，恢复点对称还原——生成器体恢复执行
    // 时回到挂起前的 TCO 语境（生成器体为 disabled 态），不再读到调用方真值。
    fs->suspendedTcoDecl = tcoDecl_;
    fs->suspendedTcoName = tcoName_;
    fs->suspendedTcoIsMethod = tcoIsMethod_;
    fs->suspendedTcoEnabled = tcoEnabled_;
    fs->suspendedTcoTryDepth = tcoTryDepth_;
    minilang_fiber::switchToFiber(fs->resumer);
    // —— 恢复点：下一次 .next() 切回。此刻解释器成员为调用方状态，先还原 ——
    currentEnv_ = std::move(fs->suspendedEnv);
    currentFunctionReturnType_ = std::move(fs->suspendedReturnType);
    recursionDepth_ = fs->suspendedRecursionDepth;
    tcoDecl_ = fs->suspendedTcoDecl;
    tcoName_ = std::move(fs->suspendedTcoName);
    tcoIsMethod_ = fs->suspendedTcoIsMethod;
    tcoEnabled_ = fs->suspendedTcoEnabled;
    tcoTryDepth_ = fs->suspendedTcoTryDepth;
    if (!fs->suspendedCallStack.empty()) {
        callStack_.insert(callStack_.end(), std::make_move_iterator(fs->suspendedCallStack.begin()),
                          std::make_move_iterator(fs->suspendedCallStack.end()));
        fs->suspendedCallStack.clear();
    }
    // yield 表达式值 = 自身 yield 值（对齐重放 skip 语义：lastValue_ = yieldValue）
    lastValue_ = fs->handoffValue;
}

void Interpreter::collectCoroutineFiberGcRoots(const CoroutineFiberState& fs, std::vector<const void*>& roots) {
    // P0 fix（2026-10-21）：挂起协程快照根集——suspendedEnv/funEnv/suspendedCallStack
    // 各帧环境链中的变量（tracked 容器）全部贡献为 GC 根。链间去重交给 mark 阶段的
    // marked 集（重复根无害，见 triggerIncrementalGc 同类注释）。
    auto collectChain = [&roots](const Environment* env) {
        int depth = 0;
        while (env && depth++ < 1024) {
            collectEnvLocalGcRoots(env, roots);
            env = env->parent.get();
        }
    };
    if (fs.suspendedEnv)
        collectChain(fs.suspendedEnv.get());
    if (fs.funEnv)
        collectChain(fs.funEnv.get());
    for (const auto& frame : fs.suspendedCallStack) {
        if (frame.env)
            collectChain(frame.env.get());
    }
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
        cd->gcRootContributor = nullptr; // P0 fix：丢弃快照同步置空根集贡献回调
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
        // P1 fix（2026-10-22）：TCO 五成员纳入 .next() 退出恢复——fiber 体守卫在终结
        // 路径的析构写回的是首次进入时的陈旧蹦床快照，本恢复在其后以调用方真值覆盖，
        // 消除蹦床误派发（终结上下文的尾调用被误分类为 SelfFunction 无 target →
        // 函数体携错误实参重跑）；挂起窗口的 disabled 滞留由 coroutineSuspend 的
        // 对称保存/恢复处理。
        FunDecl* prevTcoDecl;
        std::string prevTcoName;
        bool prevTcoIsMethod;
        bool prevTcoEnabled;
        int prevTcoTryDepth;
        ~CoroutineContextGuard() {
            if (funEnv)
                funEnv->closeCapturedVariables();
            interp.currentEnv_ = prevEnv;
            interp.currentCoroutineTargetYieldId_ = prevTargetYieldId;
            interp.currentYieldExecutionCount_ = prevYieldExecCount;
            interp.currentFunctionReturnType_ = std::move(prevReturnType);
            interp.tcoDecl_ = prevTcoDecl;
            interp.tcoName_ = std::move(prevTcoName);
            interp.tcoIsMethod_ = prevTcoIsMethod;
            interp.tcoEnabled_ = prevTcoEnabled;
            interp.tcoTryDepth_ = prevTcoTryDepth;
            // AUDIT-R3 P2-8 fix: 异常路径也恢复调用栈深度
            while (interp.callStack_.size() > savedStackDepth) {
                interp.callStack_.pop_back();
            }
        }
    } guard{*this, prevEnv, prevTargetYieldId, prevYieldExecCount, prevReturnType, nullptr, callStack_.size(),
            tcoDecl_, tcoName_, tcoIsMethod_, tcoEnabled_, tcoTryDepth_};

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
            fs->ownerThread = std::this_thread::get_id(); // §3.8：登记创建线程
            cd->interpreterFiber = {fs, [](void* p) { delete static_cast<CoroutineFiberState*>(p); }};
            fs->fiber = minilang_fiber::createFiber(&coroutineFiberTrampoline, fs);
            // P0 fix（2026-10-21）：注册挂起快照 GC 根集贡献回调。捕获裸 fs：fs 由
            // cd->interpreterFiber 持有，mark 到达本 cd 时必存活；两处丢弃 fiber 的
            // reset 均同步置空回调（见下）。
            cd->gcRootContributor = [fs](std::vector<const void*>& roots) {
                fs->interp->collectCoroutineFiberGcRoots(*fs, roots);
            };
        }
        // §3.8 跨线程恢复防御（2026-10-22）：fiber 只能由创建线程恢复——非创建线程
        // 切入时 Windows SwitchToFiber 为跨线程未定义行为、Linux ucontext 层
        // t_currentFiber 未登记（空指针解引用）；且挂起快照的解释器状态只与创建
        // 线程的调用栈/上下文绑定。显式走 runtimeError 通道报错，而非进入未定义
        // 路径（重放模式 =0 无 fiber 面，不经过本分支，天然不受影响）。
        if (fs->ownerThread != std::this_thread::get_id()) {
            runtimeError("协程不可跨线程恢复", 0, 0);
        }
        if (fs->failed) {
            // P1 fix（2026-10-21）：失败 fiber 不再弃用重建。原实现 reset 后下次
            // .next() 从头重跑函数体，coroutineSuspend 无条件递增 currentYieldId，
            // 与重放模式"跳过已消费 yield 后同点再抛"逐点分歧（首查即提前置 done、
            // 返回错误前首个 yield 值、前缀副作用重复）。保留失败 fiber：后续
            // .next() 持续重抛同一错误——值/错误序列与重放逐点一致，前缀副作用
            // 只执行一次（ADR-007 挂起 vs 重放的唯一既定差异口径）。
            // 实现要点：重抛必须在切入前（此处）——fiber 体已终结，switch 进去即重入
            // 死栈 = UB；首次失败在 switch 返回后重抛（见下方返回点检查）。
            std::rethrow_exception(fs->pendingError);
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
            // 首次失败到达此处（fiber 体内设 failed 后切回）：重抛并保留 fiber
            // （后续 .next() 走上方前置检查直接重抛，不再重建）。
            std::rethrow_exception(fs->pendingError);
        }
        if (fs->completed) {
            cd->gcRootContributor = nullptr; // P0 fix：丢弃快照同步置空根集贡献回调
            cd->interpreterFiber.reset(); // fiber 生命周期终结（deleter 删平台资源）
        }
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
