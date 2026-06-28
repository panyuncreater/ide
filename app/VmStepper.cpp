#include "VmStepper.h"
#include "Logger.h"

// ============================================================
// VmStepper — VM 单步执行状态机实现（ARCH-11 拆分自 IdeController）
// ------------------------------------------------------------
// A1 fix: 双后端分派。所有 stepOnce/isFinished/initExecution/resetState
// 调用通过 stepOnceActive() / isActiveFinished() / initActiveExecution() /
// resetActiveState() 四个 helper 转发，根据 useRegister_ 选择目标 VM。
// 状态机逻辑（步进循环/断点检查/RUN 异步分批）完全复用，与后端解耦。
// ============================================================

VmStepper::VmStepper(QObject* parent)
    : QObject(parent) {
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

VMResult VmStepper::stepOnceActive() {
    return useRegister_ ? regVm_.stepOnce() : vm_.stepOnce();
}

bool VmStepper::isActiveFinished() const {
    return useRegister_ ? regVm_.isFinished() : vm_.isFinished();
}

void VmStepper::initActiveExecution() {
    if (useRegister_) {
        // A1 fix: RegisterVM 路径必须有 lastRegCompileResult_
        if (!lastRegCompileResult_ || lastRegCompileResult_->mainChunk.code.empty()) {
            // 让 stepByMode 的 NOT_READY 路径处理（虽然此处已进入 init 分支，
            // 但保持防御性——如果代码空，直接标记完成）
            return;
        }
        regVm_.initExecution(*lastRegCompileResult_);
    } else {
        vm_.initExecution(*lastCompileResult_);
    }
}

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

VmStepper::VmStepResult VmStepper::step() {
    // 保持向后兼容：等价于 stepIn 模式
    return stepByMode(VmStepMode::STEP_IN);
}

VmStepper::VmStepResult VmStepper::stepByMode(VmStepMode mode) {
    if (isVmRunning_) return VmStepResult::NOT_READY;

    // A1 fix: 编译结果存在性检查（双后端）
    bool hasCompileResult = useRegister_
        ? (lastRegCompileResult_ && !lastRegCompileResult_->mainChunk.code.empty())
        : (lastCompileResult_ && !lastCompileResult_->mainChunk.code.empty());
    if (!hasCompileResult) {
        return VmStepResult::NOT_READY;
    }

    // A-P1-4 fix: 异常安全保护，确保 isVmRunning_/isVmInitialized_ 在异常时回滚
    try {
        // 首次点击：初始化 VM 执行环境
        if (!isVmInitialized_) {
            initActiveExecution();
            isVmInitialized_ = true;
            vmLastPausedLine_ = 0;
        }

        isVmRunning_ = true;
        vmStepMode_ = mode;
        vmStepStartFrameCount_ = getFrameCount();

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
        constexpr int64_t MAX_STEP_LOOP = 1000000;
        int64_t stepCount = 0;

        while (true) {
            VMResult result = stepOnceActive();
            ++stepCount;

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
                return VmStepResult::OK;  // 返回 OK 让 UI 更新，用户可继续
            }

            // A4 fix: 检查是否应暂停
            int currentLine = getCurrentLine();
            size_t currentFrameCount = getFrameCount();

            // 断点命中检查（所有模式都检查，使 RUN 能停在断点）
            if (!vmBreakpoints_.isEmpty() && currentLine > 0
                && vmBreakpoints_.contains(currentLine)
                && currentLine != vmLastPausedLine_) {
                vmLastPausedLine_ = currentLine;
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
                // 帧深度回到起始或更浅，且行号变化（行号为 0 时仅按帧深度判断）
                if (currentFrameCount <= vmStepStartFrameCount_) {
                    if (currentLine == 0) {
                        // 无行号信息（如 OP_CLOSURE 等辅助指令），仅当帧深度变化时暂停
                        if (currentFrameCount != vmStepStartFrameCount_) {
                            shouldPause = true;
                        }
                    } else if (currentLine != vmLastPausedLine_) {
                        shouldPause = true;
                    }
                }
                break;
            case VmStepMode::STEP_OUT:
                // 帧深度比起始更浅
                if (currentFrameCount < vmStepStartFrameCount_) {
                    shouldPause = true;
                }
                // A4 fix: 若已在栈底无法跨出（frameCount == startFrameCount == 1），
                // 执行到下一条有行号的指令即暂停（避免死循环）
                else if (currentFrameCount <= 1 && currentLine > 0
                         && currentLine != vmLastPausedLine_) {
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
                emit vmRunPaused(VmStepResult::OK);  // 返回 OK 让 UI 更新，用户可继续
                return;
            }

            // 断点命中检查
            int currentLine = getCurrentLine();
            if (!vmBreakpoints_.isEmpty() && currentLine > 0
                && vmBreakpoints_.contains(currentLine)
                && currentLine != vmLastPausedLine_) {
                vmRunTimer_->stop();
                vmLastPausedLine_ = currentLine;
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

void VmStepper::stop() {
    // QT-R-01 fix: 停止 RUN 模式定时器
    if (vmRunTimer_) vmRunTimer_->stop();
    // A1 fix: 重置当前活跃后端（非活跃后端已在 reset() 中重置，此处仅清理活跃方）
    resetActiveState();
    isVmInitialized_ = false;
    isVmRunning_ = false;
    vmStepMode_ = VmStepMode::STEP_IN;
    vmLastPausedLine_ = 0;
}
