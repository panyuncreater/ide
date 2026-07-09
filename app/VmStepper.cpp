#include "VmStepper.h"
#include "Logger.h"
#include <QApplication>     // P3.6 fix: activeWindow + repaint 替代 processEvents
#include <QCoreApplication>
#include <QWidget>          // P3.6 fix: QWidget::repaint

// ============================================================
// VmStepper — VM 单步执行状态机实现（ARCH-11 拆分自 IdeController）
// ------------------------------------------------------------
// A1 fix: 双后端分派。所有 stepOnce/isFinished/initExecution/resetState
// 调用通过 stepOnceActive() / isActiveFinished() / initActiveExecution() /
// resetActiveState() 四个 helper 转发，根据 useRegister_ 选择目标 VM。
// 状态机逻辑（步进循环/断点检查/RUN 异步分批）完全复用，与后端解耦。
// ============================================================

VmStepper::VmStepper(QObject* parent) : QObject(parent) {
    // QT-R-01 fix: 创建 RUN 模式分批执行定时器（0ms 间隔 = 尽快触发但让出事件循环）
    vmRunTimer_ = new QTimer(this);
    vmRunTimer_->setInterval(0);
    connect(vmRunTimer_, &QTimer::timeout, this, &VmStepper::runBatch);
}

VmStepper::~VmStepper() {
    // QT-R-01 fix: 停止 VM RUN 定时器，避免析构后触发回调
    if (vmRunTimer_) {
        vmRunTimer_->stop();
    }
}

// ============================================================
// A1 fix: 后端分派辅助实现
// ============================================================

/// 在“活动执行”模式下执行单条 VM 指令，返回是否已抵达终止条件（断点/结束）。
VMResult VmStepper::stepOnceActive() {
    return useRegister_ ? regVm_.stepOnce() : vm_.stepOnce();
}

/// 返回 VM 活动执行模式是否已运行至结束或断点。
bool VmStepper::isActiveFinished() const {
    return useRegister_ ? regVm_.isFinished() : vm_.isFinished();
}

/// 初始化活动执行状态：设定起始帧与终止条件（步过/步出/断点）。
bool VmStepper::initActiveExecution() {
    if (useRegister_) {
        // A1 fix: RegisterVM 路径必须有 lastRegCompileResult_
        if (!lastRegCompileResult_ || lastRegCompileResult_->mainChunk.code.empty()) {
            // P1-5 fix: 原 silent no-op 让 caller 无条件设 isVmInitialized_=true，
            // 后续 stepOnceActive() 操作未初始化 VM（regVm_.stepOnce() 返回 VM_RUNTIME_ERROR
            // "VM 未初始化"）。改为返回 false，caller 据此不标记 initialized。
            // 同时清理活跃 VM 状态避免上一轮残留。
            regVm_.resetState();
            return false;
        }
        regVm_.initExecution(*lastRegCompileResult_);
    } else {
        // P1-5 fix: 栈式 VM 路径同样防御性检查 lastCompileResult_
        if (!lastCompileResult_ || lastCompileResult_->mainChunk.code.empty()) {
            vm_.resetState();
            return false;
        }
        vm_.initExecution(*lastCompileResult_);
    }
    return true;
}

/// 复位 VM 单步的活动执行状态机，准备下一次调试会话。
void VmStepper::resetActiveState() {
    if (useRegister_) {
        regVm_.resetState();
    } else {
        vm_.resetState();
    }
}

// ============================================================
// A4 fix: VM 步进状态机实现
// ------------------------------------------------------------
// 与 Interpreter DebugController::shouldPauseForStepping 逻辑对齐：
// - STEP_IN: 执行一条指令即返回（同步）
// - STEP_OVER: 执行直到 frameCount <= startFrameCount 且行号变化（同步）
// - STEP_OUT: 执行直到 frameCount < startFrameCount（同步）
// - RUN: 全速执行直到命中断点/结束/错误
//   QT-R-01 fix: RUN 改为异步 QTimer 分批执行，避免主线程 while(true) 冻结 UI
// 暂停条件检查在每次 stepOnce 后进行，避免回调中状态不一致。
// 循环上限保护：防止恶意输入（如死循环无断点）卡死 UI。
// A1 fix: 通过 stepOnceActive() 等分派 helper 复用同一状态机逻辑。
// ============================================================

/// 执行一次 VM 单步（命令式）：按当前模式推进指令指针并采集快照。
VmStepper::VmStepResult VmStepper::step() {
    // 保持向后兼容：等价于 stepIn 模式
    return stepByMode(VmStepMode::STEP_IN);
}

/// 按当前单步模式（stepIn/Over/Out/Run）分派到对应推进策略。
VmStepper::VmStepResult VmStepper::stepByMode(VmStepMode mode) {
    if (isVmRunning_)
        return VmStepResult::NOT_READY;

    // A1 fix: 编译结果存在性检查（双后端）
    bool hasCompileResult = useRegister_ ? (lastRegCompileResult_ && !lastRegCompileResult_->mainChunk.code.empty())
                                         : (lastCompileResult_ && !lastCompileResult_->mainChunk.code.empty());
    if (!hasCompileResult) {
        return VmStepResult::NOT_READY;
    }

    // A-P1-4 fix: 异常安全保护，确保 isVmRunning_/isVmInitialized_ 在异常时回滚
    try {
        // 首次点击：初始化 VM 执行环境
        if (!isVmInitialized_) {
            // P1-5 fix: 检查 initActiveExecution 返回值。原实现即使 init 为 silent no-op
            // 也设 isVmInitialized_=true，导致后续 stepOnceActive() 操作未初始化 VM。
            if (!initActiveExecution()) {
                return VmStepResult::NOT_READY;
            }
            isVmInitialized_ = true;
            vmLastPausedLine_ = 0;
        }

        isVmRunning_ = true;
        vmStepMode_ = mode;
        vmStepStartFrameCount_ = getFrameCount();
        // AUDIT-BUG-F3 fix: 每次步进调用前重置 vmCrossedDeeper_。原实现仅在 stop() 中重置，
        // 导致首次 STEP_OVER 跨帧后标志残留 true，后续每条指令立即暂停（STEP_OVER 退化为 STEP_IN）。
        vmCrossedDeeper_ = false;
        // R54-6 fix: 同步重置 vmCrossedLine_。原实现仅在 stop()（L409）和断点命中路径
        // （L230）重置，步进暂停路径（L288-292）不重置。若上次步进跨行后暂停，
        // vmCrossedLine_ 残留 true，下次步进首条指令若同行则不刷新（L221-224 仅在行号
        // 变化时置 true），导致断点检查 L226 的 (currentLine != vmLastPausedLine_ || vmCrossedLine_)
        // 中 vmCrossedLine_ 为残留 true，断点立即重复触发，用户卡在当前行无法步进。
        vmCrossedLine_ = false;

        // #3 fix: pre-execution 断点检查 — 首次初始化后检查首行是否为断点行。
        // 原实现直接进入 stepOnce 循环，导致首行断点被先执行再检测（post-execution），
        // 与 Interpreter 的 pre-execution 语义不一致。此处补齐：若首行是断点且
        // 之前未在该行暂停过（vmLastPausedLine_ 刚被重置为 0），则执行前先暂停。
        if (!vmBreakpoints_.isEmpty()) {
            int initLine = getCurrentLine();
            if (checkBreakpointHit(initLine) && initLine != vmLastPausedLine_) {
                // AUDIT-P3-ROUND50 fix: 预执行命中分支未递增 hitCount，与循环内断点
                // 命中路径（L223-229）不一致。对齐循环内路径递增 hitCount。
                vmBreakpointHitCounts_[initLine]++;
                vmLastPausedLine_ = initLine;
                isVmRunning_ = false;
                return VmStepResult::PAUSED_AT_BREAKPOINT;
            }
        }

        // A4 fix: 步进循环期间禁用 stepCallback（避免每条指令 emit 信号拖慢 UI）。
        // UI 更新由 stepByMode 返回后调用方一次性完成。
        vm_.setStepCallbackEnabled(false);
        regVm_.setStepCallbackEnabled(false);

        // QT-R-01 fix: RUN 模式改为异步分批执行，启动 QTimer 后立即返回 RUNNING。
        // 暂停时通过 vmRunPaused 信号通知 UI，避免主线程 while(true) 循环冻结 UI。
        if (mode == VmStepMode::RUN) {
            vmRunStepCount_ = 0;
            vmRunTimer_->start();
            return VmStepResult::RUNNING;
        }

        // STEP_IN/OVER/OUT: 同步执行（快速操作，不阻塞 UI）
        // #5 注：VM 断点检查在 stepOnce 之后（post-execution），即执行完一条指令后
        // 检查 IP 指向的下一条指令是否在断点行。变量快照反映的是上一条指令执行后
        // 的状态（= 断点行的前置状态）。这与 Interpreter 的 pre-execution 暂停
        // （节点副作用未应用）在语义上等价——用户看到的是"即将执行这行前的状态"。
        // 唯一差异：多语句行（a=1; b=2;）VM 可能在执行完 a=1 后才检测到 b=2 所在行。
        constexpr int64_t MAX_STEP_LOOP = 1000000;
        int64_t stepCount = 0;

        while (true) {
            VMResult result = stepOnceActive();
            ++stepCount;

            // BUG-IDE-18 fix: STEP_OVER/OUT 在深递归或长循环上同步执行可达数十万步，
            // 期间不处理任何事件会让 UI 看似冻结（标题栏"无响应"、面板不重绘）。
            // 每 2000 步让出事件循环处理绘制事件（ExcludeUserInputEvents
            // 排除用户输入事件以避免重入触发 stop/step 等槽函数）。若期间 VM 被异步
            // 停止（isVmRunning_ 被置 false），立即返回 OK 让 UI 更新。
            //
            // P3.6 fix: 原实现调用 processEvents(ExcludeUserInputEvents) 会派发 QTimer
            // 事件，虽然各面板已有 isVmRunning() 守卫，但定时器重入仍是潜在风险。
            // 改用 sendPostedEvents(DeferredDelete) 处理对象生命周期 + 遍历所有可见
            // 顶层窗口调用 repaint() 强制重绘，彻底避免定时器事件派发。
            // repaint() 同步处理 paint event 不进入事件循环，无重入风险。
            // 注：sendPostedEvents(nullptr, QEvent::DeferredDelete) 仅处理 DeferredDelete
            // 类型的 posted events（QObject 删除），不处理 QTimer/Socket 等事件。
            if (stepCount % 2000 == 0) {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                // 强制重绘所有可见顶层窗口（主窗口 + 浮动 dock 容器）
                if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
                    for (QWidget* w : app->topLevelWidgets()) {
                        if (w->isVisible()) {
                            w->repaint();
                        }
                    }
                }
                if (!isVmRunning_) {
                    return VmStepResult::OK;
                }
            }

            if (result == VMResult::VM_RUNTIME_ERROR) {
                isVmInitialized_ = false;
                isVmRunning_ = false;
                return VmStepResult::ERROR;
            }

            if (isActiveFinished()) {
                isVmInitialized_ = false;
                isVmRunning_ = false;
                return VmStepResult::FINISHED;
            }

            // A4 fix: 所有模式的循环上限保护
            if (stepCount >= MAX_STEP_LOOP) {
                isVmRunning_ = false;
                return VmStepResult::OK; // 返回 OK 让 UI 更新，用户可继续
            }

            // A4 fix: 检查是否应暂停
            int currentLine = getCurrentLine();
            size_t currentFrameCount = getFrameCount();

            // 断点命中检查（所有模式都检查，使 RUN 能停在断点）
            // #4 fix: 改用 checkBreakpointHit 支持条件断点求值
            // BUG-DBG-2 fix: 移植 DebugController::crossedLine_ 机制。
            // 原实现仅用 currentLine != vmLastPausedLine_ 去重，单行循环断点
            // （如 for (...; ...; ...) print(i);）首次命中后永不再触发。
            // crossedLine_ 在行号变化时置 true，允许同行断点在跨行后重新触发；
            // 仅在断点真正命中（含条件满足）时清 false，与 DebugController F4 修复一致。
            if (currentLine > 0) {
                if (currentLine != vmLastSeenLine_) {
                    vmCrossedLine_ = true;
                }
                vmLastSeenLine_ = currentLine;
            }
            if (checkBreakpointHit(currentLine) && (currentLine != vmLastPausedLine_ || vmCrossedLine_)) {
                // AUDIT-P2-CORRECT fix: hitCount 递增移到过滤条件通过后，避免过度递增
                vmBreakpointHitCounts_[currentLine]++;
                vmLastPausedLine_ = currentLine;
                vmCrossedLine_ = false; // 命中后重置，同行后续指令不再触发
                isVmRunning_ = false;
                return VmStepResult::PAUSED_AT_BREAKPOINT;
            }

            // 根据步进模式判断是否暂停
            bool shouldPause = false;
            switch (mode) {
            case VmStepMode::STEP_IN:
                // 单步：执行一条即暂停
                shouldPause = true;
                break;
            case VmStepMode::STEP_OVER:
                // AUDIT-BUG-D2 fix: 跟踪是否进入过更深的帧（crossedDeeper）。
                // 同行函数调用（如 foo(); bar(); 同在第5行）STEP_OVER foo() 后
                // currentLine 仍为 5 == vmLastPausedLine_，原逻辑不暂停直接执行 bar()。
                // crossedDeeper 标志确保从更深帧返回后即使行号不变也暂停。
                if (currentFrameCount > vmStepStartFrameCount_) {
                    vmCrossedDeeper_ = true;
                }
                // 帧深度回到起始或更浅，且行号变化（行号为 0 时仅按帧深度判断）
                if (currentFrameCount <= vmStepStartFrameCount_) {
                    if (currentLine == 0) {
                        // 无行号信息（如 OP_CLOSURE 等辅助指令），仅当帧深度变化时暂停
                        if (currentFrameCount != vmStepStartFrameCount_) {
                            shouldPause = true;
                        }
                    } else if (currentLine != vmLastPausedLine_ || vmCrossedDeeper_) {
                        shouldPause = true;
                    }
                    // BUG-DBG-13 fix: crossedDeeper_ 在每次 stepByMode 入口处重置
                    // （见上方 AUDIT-BUG-F3 fix: vmCrossedDeeper_ = false），而非不存在的
                    // resetVmStepState 函数。原注释引用的函数从未定义，误导维护者。
                }
                break;
            case VmStepMode::STEP_OUT:
                // 帧深度比起始更浅
                if (currentFrameCount < vmStepStartFrameCount_) {
                    shouldPause = true;
                }
                // A4 fix: 若已在栈底无法跨出（frameCount == startFrameCount == 1），
                // 执行到下一条有行号的指令即暂停（避免死循环）
                // BUG-DBG-3 fix: 顶层 STEP_OUT 行为与 STEP_OVER 顶层不一致——
                // STEP_OVER 用 crossedDeeper_ 允许同行暂停，STEP_OUT 顶层仅用行号变化判断，
                // 单行循环（如 for (...; ...; ...) foo();）STEP_OUT 后永不暂停（行号不变），
                // 直到循环结束才停止。修复：与 STEP_OVER 顶层对齐，使用 crossedLine_ 机制
                // 允许跨行后同行暂停。
                // R54-7 fix: 注释原提及"vmCrossedDeeper_"但代码实际使用 vmCrossedLine_，
                // 已更正注释。STEP_OUT 在栈底时无处可"跨出"，降级为"跨行后暂停"语义
                //（与 STEP_IN 行级粒度一致），使用 vmCrossedLine_ 是正确设计。
                else if (currentFrameCount <= 1 && currentLine > 0 &&
                         (currentLine != vmLastPausedLine_ || vmCrossedLine_)) {
                    shouldPause = true;
                }
                break;
            case VmStepMode::RUN:
                // RUN 模式：仅断点命中才暂停（已在上方检查）
                // QT-R-01 fix: 不会走到这里（RUN 在上方异步分支返回）
                break;
            }

            if (shouldPause) {
                vmLastPausedLine_ = currentLine;
                isVmRunning_ = false;
                return VmStepResult::OK;
            }
        }
    } catch (...) {
        // 异常时重置所有状态，避免永久卡死
        isVmInitialized_ = false;
        isVmRunning_ = false;
        throw;
    }
}

// QT-R-01 fix: RUN 模式 QTimer 分批执行回调。
// 每次执行 BATCH_SIZE 步，然后让出控制权给事件循环（处理 UI 事件/重绘）。
// 暂停条件命中时停止定时器并通过 vmRunPaused 信号通知 UI。
// A1 fix: 通过 stepOnceActive() 等分派 helper 复用同一批处理逻辑。
void VmStepper::runBatch() {
    // QT-R-01 fix: 每批执行 2000 步（约 1-2ms），在批与批之间 Qt 处理 UI 事件
    constexpr int BATCH_SIZE = 2000;
    // 总量上限 100 万步（与原同步模式一致），防止死循环程序无限消耗 CPU
    constexpr int64_t MAX_TOTAL_STEPS = 1000000;

    // P1-5 fix: runBatch 缺少 hasCompileResult 保护。原实现假设 caller stepByMode
    // 已通过 initActiveExecution 检查，但若 VM 状态被外部清空（如 stop() 后定时器
    // 仍在 pending），会调用 stepOnceActive() 操作未初始化 VM。
    bool hasCompileResult = useRegister_ ? (lastRegCompileResult_ && !lastRegCompileResult_->mainChunk.code.empty())
                                         : (lastCompileResult_ && !lastCompileResult_->mainChunk.code.empty());
    if (!hasCompileResult || !isVmInitialized_) {
        vmRunTimer_->stop();
        isVmRunning_ = false;
        emit vmRunPaused(VmStepResult::NOT_READY);
        return;
    }

    try {
        for (int i = 0; i < BATCH_SIZE; ++i) {
            VMResult result = stepOnceActive();
            ++vmRunStepCount_;

            if (result == VMResult::VM_RUNTIME_ERROR) {
                vmRunTimer_->stop();
                isVmInitialized_ = false;
                isVmRunning_ = false;
                emit vmRunPaused(VmStepResult::ERROR);
                return;
            }

            if (isActiveFinished()) {
                vmRunTimer_->stop();
                isVmInitialized_ = false;
                isVmRunning_ = false;
                emit vmRunPaused(VmStepResult::FINISHED);
                return;
            }

            // 总量上限保护
            if (vmRunStepCount_ >= MAX_TOTAL_STEPS) {
                vmRunTimer_->stop();
                isVmRunning_ = false;
                emit vmRunPaused(VmStepResult::OK); // 返回 OK 让 UI 更新，用户可继续
                return;
            }

            // 断点命中检查
            // #4 fix: 改用 checkBreakpointHit 支持条件断点求值
            // BUG-DBG-2 fix: 移植 DebugController::crossedLine_ 机制（与 stepByMode 对齐）。
            // 原实现仅用 currentLine != vmLastPausedLine_ 去重，单行循环断点
            // （如 for (...; ...; ...) print(i);）首次命中后永不再触发。
            // crossedLine_ 在行号变化时置 true，允许同行断点在跨行后重新触发；
            // 仅在断点真正命中（含条件满足）时清 false。
            //
            // BUG-IDE-19（已知限制）：条件断点求值（checkBreakpointHit →
            // vmConditionEvaluator_）在主线程同步执行，每次命中都会创建临时
            // Interpreter + Lexer + Parser + Environment 拷贝全局变量。当条件表达式
            // 复杂或全局变量规模大时，单次求值可达毫秒级，循环内频繁命中条件断点会
            // 拖慢 RUN 模式。这是用户主动设置的功能，性能可接受；彻底修复需要将求值
            // 移到独立线程或缓存求值环境，工程量大，暂列为已知限制。
            int currentLine = getCurrentLine();
            if (currentLine > 0) {
                if (currentLine != vmLastSeenLine_) {
                    vmCrossedLine_ = true;
                }
                vmLastSeenLine_ = currentLine;
            }
            if (checkBreakpointHit(currentLine) && (currentLine != vmLastPausedLine_ || vmCrossedLine_)) {
                // AUDIT-P2-CORRECT fix: hitCount 递增移到过滤条件通过后，避免过度递增
                vmBreakpointHitCounts_[currentLine]++;
                vmRunTimer_->stop();
                vmLastPausedLine_ = currentLine;
                vmCrossedLine_ = false; // 命中后重置，同行后续指令不再触发
                isVmRunning_ = false;
                emit vmRunPaused(VmStepResult::PAUSED_AT_BREAKPOINT);
                return;
            }
            // RUN 模式：仅断点命中才暂停，否则继续执行下一批
        }
    } catch (...) {
        // 异常时停止定时器并重置状态
        vmRunTimer_->stop();
        isVmInitialized_ = false;
        isVmRunning_ = false;
        emit vmRunPaused(VmStepResult::ERROR);
    }
}

/// 停止 VM 执行：中断运行循环并复位执行状态。
void VmStepper::stop() {
    // QT-R-01 fix: 停止 RUN 模式定时器
    if (vmRunTimer_)
        vmRunTimer_->stop();
    // A1 fix: 重置当前活跃后端（非活跃后端已在 reset() 中重置，此处仅清理活跃方）
    resetActiveState();
    isVmInitialized_ = false;
    isVmRunning_ = false;
    vmStepMode_ = VmStepMode::STEP_IN;
    vmLastPausedLine_ = 0;
    vmCrossedDeeper_ = false; // AUDIT-BUG-D2 fix: reset 重置
    // BUG-DBG-2 fix: 同步重置 crossedLine_ 状态，避免下一轮运行残留旧状态
    vmLastSeenLine_ = -1;
    vmCrossedLine_ = false;
}

// #4 fix: 检查断点命中（含条件求值）
// 返回 true 表示断点匹配且条件满足（应在此行暂停）。
// AUDIT-P2-CORRECT fix: 此函数改为纯查询，不递增 hitCount。
// 原 BUG-DBG-AUDIT-2 fix 在此处递增 hitCount，但由于调用方使用 C++ 短路求值
// `checkBreakpointHit(line) && (过滤条件)`，当过滤条件为 false 时 hitCount
// 已被递增但断点未实际暂停，导致 hitCount 远超实际命中次数（每批次递增一次）。
// 现改为纯查询，hitCount 递增移到调用方过滤条件通过之后。
bool VmStepper::checkBreakpointHit(int line) {
    if (vmBreakpoints_.isEmpty() || line <= 0 || !vmBreakpoints_.contains(line)) {
        return false;
    }
    // #4 fix: 检查是否有条件表达式
    auto condIt = vmBreakpointConditions_.find(line);
    if (condIt == vmBreakpointConditions_.end() || condIt->empty()) {
        return true; // 无条件断点：直接命中
    }
    // #4 fix: 条件断点：调用求值器（由 IdeController 注入，使用临时 Interpreter + VM 全局变量）
    if (vmConditionEvaluator_) {
        return vmConditionEvaluator_(condIt.value());
    }
    // 无求值器时视为条件不满足（不暂停）——与 DebugEvaluator::evaluate 语义一致。
    // AUDIT-BUG-D1 fix: 原返回 true 会导致条件断点被当作无条件断点，
    // 用户设置的条件被完全忽略。返回 false 更安全（不暂停而非总是暂停）。
    return false;
}
