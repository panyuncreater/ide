#include "DebugCoordinator.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

// ============================================================
// DebugCoordinator — 调试状态协调实现（ARCH-11 拆分自 IdeController）
// ============================================================

DebugCoordinator::DebugCoordinator(std::shared_ptr<Interpreter> interpreter,
                                   std::shared_ptr<DebugController> debugger,
                                   QObject* parent)
    : QObject(parent)
    , interpreter_(std::move(interpreter))
    , debugger_(std::move(debugger)) {
    // 转发调试器暂停信号
    connect(debugger_.get(), &DebugController::pausedAt, this, &DebugCoordinator::pausedAt);
}

DebugCoordinator::~DebugCoordinator() {
    // #10 fix: 反注册 setupDebug 注册的回调，避免 worker 线程调用悬垂 this。
    // 传空 std::function 使后续调用变为 no-op（DebugController 在锁内拷贝再调用）。
    if (debugger_) {
        debugger_->setConditionEvaluator({});
        debugger_->setVariableCallback({});
        debugger_->setCallStackCallback({});
    }
}

// ============================================================
// 调试操作
// ============================================================

void DebugCoordinator::setupDebug(const QSet<int>& breakpoints,
                                  const QMap<int, std::string>& conditions) {
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
            Lexer condLexer;
            auto tokens = condLexer.scan(condition);
            Parser condParser;
            auto block = condParser.parse(tokens);
            if (!condParser.getDiagnostics().hasErrors() && block && !block->statements.empty()) {
                Value result = interpreter->evaluateCondition(block->statements[0].get());
                return result.isTruthy();
            }
        } catch (...) {
            // 条件求值失败视为 false（不暂停）
        }
        return false;
    });

    // 调试变量回调
    debugger_->setVariableCallback([interpreter = interpreter_]() -> std::vector<VariableSnapshot> {
        std::vector<VariableSnapshot> result;
        Environment* env = interpreter->currentEnvironment();
        if (env) {
            int depth = 0;
            Environment* current = env;
            while (current) {
                const auto& locals = current->localVariables();
                for (const auto& kv : locals) {
                    VariableSnapshot snap;
                    snap.name = kv.first;
                    snap.value = kv.second;
                    snap.scope = (depth == 0) ? "局部" : (current->parent ? "外层" : "全局");
                    result.push_back(snap);
                }
                current = current->parent.get();
                depth++;
            }
        }
        return result;
    });

    debugger_->setCallStackCallback([interpreter = interpreter_]() -> std::vector<CallStackEntry> {
        std::vector<CallStackEntry> result;
        const auto& stack = interpreter->getCallStack();
        for (const auto& frame : stack) {
            CallStackEntry entry;
            entry.functionName = frame.functionName;
            entry.line = frame.line;
            entry.depth = frame.depth;
            if (frame.env) {
                for (const auto& kv : frame.env->localVariables()) {
                    entry.locals.emplace_back(kv.first, kv.second);
                }
            }
            result.push_back(entry);
        }
        return result;
    });

    debugger_->reset();
    debugger_->stepIn();
}
