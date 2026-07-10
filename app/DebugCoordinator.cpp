#include "DebugCoordinator.h"
#include "common/Logger.h" // E1 fix: 条件断点求值异常记录告警
#include "lexer/Lexer.h"
#include "parser/Parser.h"

// ============================================================
// DebugCoordinator — 调试状态协调实现（ARCH-11 拆分自 IdeController）
// ============================================================

DebugCoordinator::DebugCoordinator(std::shared_ptr<Interpreter> interpreter, std::shared_ptr<DebugController> debugger,
                                   QObject* parent)
    : QObject(parent), interpreter_(std::move(interpreter)), debugger_(std::move(debugger)) {
    // 转发调试器暂停信号
    // 2026-06-29 审计修复 R3: 显式指定 Qt::QueuedConnection。
    // pausedAt 在 worker 线程的 doPause 中 emit，DebugCoordinator 在主线程。
    // 原 AutoConnection 虽会自动升级为 QueuedConnection，但依赖接收方线程亲和性
    // 不变的隐式假设。显式 QueuedConnection 防止未来重构（如将 DebugController
    // move 到 worker 线程）破坏跨线程投递语义，导致信号在错误线程直接执行。
    connect(debugger_.get(), &DebugController::pausedAt, this, &DebugCoordinator::pausedAt, Qt::QueuedConnection);
}

DebugCoordinator::~DebugCoordinator() {
    // #10 fix: 反注册 setupDebug 注册的回调，避免 worker 线程调用悬垂 this。
    // 传空 std::function 使后续调用变为 no-op（DebugController 在锁内拷贝再调用）。
    // AUDIT-P1 fix: 清空 callback 后必须 spin-wait 等待正在执行的 callback 完成，
    // 否则 worker 线程可能在锁外 cb() 调用中持已失效的 interpreter_ shared_ptr → UAF。
    // 采用 RCU 优雅期模式：activeCallbackCount_ 原子计数，cb() 期间 >0，归零后安全析构。
    // R54-9 fix: 先调用 stop() 唤醒可能阻塞在 pauseExecution 的 worker 线程。
    // 原实现仅清空回调 + waitCallbacksIdle，但 worker 若阻塞在 pauseCV_.wait()，
    // 不在 callback 中（activeCallbackCount_==0），waitCallbacksIdle 立即返回。
    // 随后 interpreter_ 被释放，worker 醒来后访问已释放 interpreter → UAF。
    // stop() 设 stopped_=true 并 notify_one，worker 醒来后抛 DebugStopException 终止。
    if (debugger_) {
        debugger_->stop();
        debugger_->setConditionEvaluator({});
        debugger_->setVariableCallback({});
        debugger_->setCallStackCallback({});
        debugger_->waitCallbacksIdle(); // 等待所有 callback 完成后再析构
    }
}

// ============================================================
// 调试操作
// ============================================================

void DebugCoordinator::setupDebug(const QSet<int>& breakpoints, const QMap<int, std::string>& conditions) {
    debugger_->setBreakpoints(breakpoints);

    // 同步断点条件
    for (int line : breakpoints) {
        auto it = conditions.find(line);
        if (it != conditions.end() && !it.value().empty()) {
            debugger_->setBreakpointCondition(line, it.value());
        }
    }

    // GUI-03 fix: 使用 evaluateCondition 安全求值条件断点
    // #10 fix: 捕获 interpreter_（shared_ptr 副本）而非裸 this——回调仅访问
    // interpreter_，且 DebugController 可能比 DebugCoordinator 存活更久，
    // 捕获 shared_ptr 避免 worker 线程调用悬垂 this 的 UAF。
    debugger_->setConditionEvaluator([interpreter = interpreter_](const std::string& condition) -> bool {
        try {
            // 条件断点修复(R86): 自动补充分号——用户输入的条件表达式（如 i%2==1）
            // 通常不带分号，但 Parser::parse() 的 expressionStatement() 要求分号，
            // 缺失分号导致解析失败 → 条件求值返回 false → 断点永不触发。
            std::string condExpr = condition;
            if (!condExpr.empty() && condExpr.back() != ';') {
                condExpr += ';';
            }
            Lexer condLexer;
            auto tokens = condLexer.scan(condExpr);
            Parser condParser;
            auto block = condParser.parse(tokens);
            if (!condParser.getDiagnostics().hasErrors() && block && !block->statements.empty()) {
                Value result = interpreter->evaluateCondition(block->statements[0].get());
                return result.isTruthy();
            }
        } catch (const std::exception& e) {
            // E1 fix: 条件断点求值异常记录告警，便于用户排查（条件 + 错误信息）。
            // 上层 DebugController::shouldPauseAtBreakpoint 也会再次记录行号。
            // AUDIT-BUG-C8 fix: 改用 LOG_* 宏，先检查级别再构造消息（懒求值）。
            LOG_WARNING("条件断点求值异常: " + std::string(e.what()) + "（条件: " + condition + "），视为条件不满足",
                        "Debugger");
        } catch (...) {
            LOG_WARNING("条件断点求值发生未知异常（条件: " + condition + "），视为条件不满足", "Debugger");
        }
        return false;
    });

    // 调试变量回调
    debugger_->setVariableCallback([interpreter = interpreter_]() -> std::vector<VariableSnapshot> {
        std::vector<VariableSnapshot> result;
        // AUDIT-BUG-C4 fix: 调试回调必须用 try/catch 包裹防止崩溃（硬约束 #16）。
        // 同文件 conditionEvaluator 已有 try/catch，原 variableCallback 遗漏。
        // Interpreter 异常态下遍历 Environment 链/拷贝容器可能抛 bad_alloc，
        // 未捕获会传播到 UI 线程导致 IDE 崩溃。
        // AUDIT-P1 fix: 用 currentEnvironmentShared() 获取 shared_ptr 副本，
        // 遍历 parent 链时通过 shared_ptr 赋值延长每个节点的生命周期，
        // 防止 GUI 线程遍历期间 worker 线程修改 currentEnv_ 或析构 Environment → UAF。
        try {
            auto currentShared = interpreter->currentEnvironmentShared();
            if (currentShared) {
                int depth = 0;
                while (currentShared) {
                    // R54-11 fix: 用 snapshotLocalVariables() 返回拷贝，避免遍历
                    // 期间 worker 线程修改 variables map 导致迭代器失效。
                    // shared_ptr 防止 Environment 析构，但不防止 map 内容被修改。
                    auto locals = currentShared->snapshotLocalVariables();
                    for (const auto& kv : locals) {
                        VariableSnapshot snap;
                        snap.name = kv.first;
                        snap.value = kv.second;
                        snap.scope = (currentShared->parent == nullptr) ? "全局" : (depth == 0) ? "局部" : "外层";
                        result.push_back(snap);
                    }
                    currentShared = currentShared->parent; // shared_ptr 赋值，延长 parent 生命周期
                    depth++;
                }
            }
        } catch (const std::exception& e) {
            Logger::Warning(std::string("变量快照回调异常: ") + e.what(), "Debugger");
        } catch (...) {
            Logger::Warning("变量快照回调发生未知异常", "Debugger");
        }
        return result;
    });

    debugger_->setCallStackCallback([interpreter = interpreter_]() -> std::vector<CallStackEntry> {
        std::vector<CallStackEntry> result;
        // AUDIT-BUG-C5 fix: 调试回调必须用 try/catch 包裹防止崩溃（硬约束 #16）。
        // AUDIT-P1 fix: 用 getCallStackSnapshot() 返回值拷贝，避免 const 引用跨线程
        // 遍历期间 worker 线程 push_back/pop_back 导致迭代器失效。
        // 对齐 variableCallback 的 currentEnvironmentShared() 修复模式。
        try {
            auto stack = interpreter->getCallStackSnapshot();
            for (const auto& frame : stack) {
                CallStackEntry entry;
                entry.functionName = frame.functionName;
                entry.line = frame.line;
                entry.depth = frame.depth;
                if (frame.env) {
                    // P0-2 fix: 用 snapshotLocalVariables() 获取值拷贝，避免直接遍历
                    // Environment::variables 的 const 引用。UI 线程遍历引用的 map 期间，
                    // worker 线程可能并发修改变量赋值/声明/作用域清理，导致迭代器失效或
                    // 读取撕裂值（unordered_map 并发读写是 UB）。对齐 variableCallback
                    // 的 snapshotLocalVariables() 修复模式。
                    auto locals = frame.env->snapshotLocalVariables();
                    for (const auto& kv : locals) {
                        entry.locals.emplace_back(kv.first, kv.second);
                    }
                }
                result.push_back(entry);
            }
        } catch (const std::exception& e) {
            Logger::Warning(std::string("调用栈回调异常: ") + e.what(), "Debugger");
        } catch (...) {
            Logger::Warning("调用栈回调发生未知异常", "Debugger");
        }
        return result;
    });

    debugger_->reset();
    debugger_->stepIn();
}
