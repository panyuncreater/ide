// ============================================================
// RegisterVM.cpp — PERF-14: 寄存器式虚拟机实现
// ------------------------------------------------------------
// 执行 RegBytecodeChunk。核心指令：常量加载、算术比较、控制流、
// 全局/局部变量、函数调用、返回、容器、成员访问、闭包、类。
//
// 与栈式 VM(VM.cpp) 的关键差异（理解本文件的前提）：
//   · 无操作数栈。所有中间结果存于每个调用帧的固定寄存器窗口
//     RegCallFrame::registers（std::array<Value, MAX_REGISTERS=32>），
//     用 reg(slot) 访问。因此 POP/DUP 退化为 REG_MOVE 或 no-op；
//     函数局部变量 = 寄存器号（localCount 个寄存器从 0 开始）。
//   · 调用帧帧布局：新帧 registers[0..argCount-1] 填实参（方法调用时
//     registers[0] 是 this），其余寄存器由被调用者自由使用；
//     returnReg/returnIp 记录调用者的目标寄存器与返回地址。
//   · upvalue 全局地址编码：stackSlot = frameIdx * MAX_REGISTERS + slot，
//     使每一帧的每个寄存器槽有全局唯一地址（C-3 fix）。closeUpvaluesFrom/
//     resolveOpenUpvalueSlot 据此跨帧定位，解决了多层嵌套闭包透传错位。
//   · 嵌套左值变异：MEMBER_SET/INDEX_SET 把接收者寄存器号记入
//     lastMutatedReceiverReg_，后续 WRITEBACK_* 整体替换回原处
//     （全局槽/局部寄存器/upvalue），与栈式 VM 的 lastMutatedReceiver_ 对应。
//   · 注册式 VM 把 hasError_ 纳入 isFinished 判定，且 execute()/stepOnce()
//     共用成员 instructionCount_ 作为 DoS 防护预算（与栈式 VM 的局部计数不同）。
// ============================================================

#include "compiler/RegisterVM.h"
#include "common/ErrorFormat.h"   // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
#include "common/ErrorMessages.h" // R97 #1: 三后端共享错误消息常量
#include "common/Logger.h"
#include "common/RuntimeLimits.h" // P3-14: NO_INDEX/NO_SLOT 哨兵常量
#include "common/TypeChecker.h"   // 2026-06-29: typeMatchValue（REG_TYPE_CHECK）
#include "common/Utf8Utils.h"
#include "compiler/RegisterBytecode.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/GcManager.h" // AUDIT-R4 BUG-10: CallbackSuppressor
#include "interpreter/NumericUtils.h"
#include <algorithm>
#include <cassert>
#include <cmath>

// ============================================================
// 构造 / 重置
// ============================================================

RegisterVM::RegisterVM() = default;

void RegisterVM::resetState() {
    frames_.clear();
    mainChunk_ = RegBytecodeChunk{};
    functionChunks_.clear();
    callCache_.clear(); // PERF-AUDIT-5: 失效内联缓存
    globals_.clear();
    globalSlots_.clear();
    globalNameToSlot_.clear();
    // BUG-REGVM-3 fix: functionClosures_ 已删除（死代码字段）
    classInfo_.clear();
    enumRegistry_.clear(); // R99 enum 校验：同步清理
    openUpvalues_.clear();
    upvalueOpenSeq_ = 0;     // AUDIT-R5 R1 fix: 重置开启序号计数器
    methodUpvalues_.clear(); // W3-2-Bug2 fix
    tryStack_.clear();
    pendingException_ = Value::nullValue(); // P1-4 fix: 清理异常值
    pendingJumpStack_.clear();              // AUDIT-P1.1 fix: 清理续跳栈
    hasError_ = false;
    // P2-12: lastError_/lastErrorLine_ 已移除，错误信息统一由 diagnostics_ 承载
    diagnostics_.clear();
    instructionCount_ = 0;
    initialized_ = false;
}

// ============================================================
// R114 阶段 3：restoreFromSnapshot — 从快照恢复 RegisterVM 状态（状态回滚）
// ============================================================
bool RegisterVM::restoreFromSnapshot(const std::vector<Value>& registerValues,
                                     const std::vector<std::pair<std::string, Value>>& globalsValues, size_t targetIp,
                                     size_t targetFrameCount) {
    if (!initialized_) {
        // P2-12: 错误信息直接写入 diagnostics_，消除 lastError_ 字段
        hasError_ = true;
        diagnostics_.addError("RegisterVM 未初始化，无法回滚", 0, 0, DiagSource::RegisterVM);
        return false;
    }
    if (targetFrameCount > frames_.size()) {
        hasError_ = true;
        diagnostics_.addError("回滚目标帧数超过当前帧数", 0, 0, DiagSource::RegisterVM);
        return false;
    }

    // (1) 截断 frames_ 到 targetFrameCount（仅截断不扩展）
    while (frames_.size() > targetFrameCount) {
        frames_.pop_back();
    }
    // 设置栈顶帧 ip = targetIp（若仍有帧）
    if (!frames_.empty()) {
        currentFrame().ip = targetIp;
    }

    // (2) 将 registerValues 写入栈顶帧的 registers[]（截断到 MAX_REGISTERS=32）
    if (!frames_.empty()) {
        RegCallFrame& frame = currentFrame();
        size_t copyCount = std::min(registerValues.size(), static_cast<size_t>(RegCallFrame::MAX_REGISTERS));
        for (size_t i = 0; i < copyCount; ++i) {
            frame.registers[i] = registerValues[i];
        }
        // 更新 registerCount 以覆盖所有写入的寄存器（避免 reg() 边界检查拒绝访问）
        frame.registerCount = static_cast<uint8_t>(copyCount);
    }

    // (3) 按 name 更新 globalSlots_/globals_（已有变量覆盖，新变量忽略）
    for (const auto& kv : globalsValues) {
        const std::string& name = kv.first;
        const Value& val = kv.second;
        auto slotIt = globalNameToSlot_.find(name);
        if (slotIt != globalNameToSlot_.end()) {
            int slot = slotIt->second;
            if (slot >= 0 && slot < static_cast<int>(globalSlots_.size())) {
                globalSlots_[slot] = val;
            }
        } else {
            auto it = globals_.find(name);
            if (it != globals_.end()) {
                it->second = val;
            }
            // 不创建新变量——回滚不应引入编译期未注册的变量
        }
    }

    // (4) 清理 openUpvalues_（寄存器被覆盖，原 open upvalue 的 stackSlot 已指向新值）
    // RegisterVM 的 openUpvalues_ 按 registerIndex 索引（每帧独立 0..31），
    // 回滚后栈顶帧的寄存器全部被覆盖，所有 open upvalue 都应清理。
    openUpvalues_.clear();

    // (5) 清理 tryStack_（frameIndex >= targetFrameCount 的 handler 已悬垂）
    while (!tryStack_.empty() && tryStack_.back().frameIndex >= targetFrameCount) {
        tryStack_.pop_back();
    }

    // (6) 保守清空 pendingJumpStack_ + pendingException_（跨回滚未定义）
    pendingJumpStack_.clear();
    pendingException_ = Value::nullValue();

    // (7) 清理错误/暂存状态
    // P2-12: lastError_/lastErrorLine_ 已移除，错误信息统一由 diagnostics_ 承载
    hasError_ = false;
    diagnostics_.clear();
    lastMutatedReceiverReg_ = 0;

    return true;
}

void RegisterVM::setOutputCallback(std::function<void(const std::string&)> cb) {
    outputCallback_ = std::move(cb);
}

void RegisterVM::setInputCallback(std::function<std::string(const std::string&)> cb) {
    inputCallback_ = std::move(cb);
}

// ============================================================
// 执行入口
// ============================================================

void RegisterVM::initExecution(const RegisterCompileResult& result) {
    resetState();
    mainChunk_ = result.mainChunk;
    functionChunks_ = result.functionChunks;

    // 预分配到 MAX_FRAMES 上限：push_back 前已有 frames_.size() < MAX_FRAMES 检查，
    // reserve 到上限可保证 push_back 永不 realloc / 抛 bad_alloc，消除帧推入异常安全窗口。
    frames_.reserve(MAX_FRAMES);

    // 初始化全局槽位
    globalSlots_.resize(result.globalSlotCount);
    for (size_t i = 0; i < result.globalSlotNames.size() && i < globalSlots_.size(); ++i) {
        globalNameToSlot_[result.globalSlotNames[i]] = static_cast<int>(i);
    }

    // R99 enum 校验：加载 enum 元信息到 enumRegistry_ 供 REG_BUILD_ENUM_VARIANT 校验
    for (const auto& info : result.enumInfos) {
        enumRegistry_[info.name] = info;
    }

    // 创建 main 帧
    RegCallFrame mainFrame;
    mainFrame.chunk = &mainChunk_;
    mainFrame.ip = 0;
    // #8 fix: 定长 array，无需 resize，仅记录激活数量
    mainFrame.registerCount =
        static_cast<uint8_t>(mainChunk_.registerCount <= RegCallFrame::MAX_REGISTERS ? mainChunk_.registerCount
                                                                                     : RegCallFrame::MAX_REGISTERS);
    frames_.push_back(std::move(mainFrame));

    initialized_ = true;
}

VMResult RegisterVM::execute(const RegisterCompileResult& result) {
    initExecution(result);
    // AUDIT-R4 BUG-10 fix: 与 StackVM/JIT 对齐——执行期间抑制增量 GC 触发回调。
    // 寄存器帧/全局槽上的临时 Value 不在 GcManager 根集中，回调中途触发会
    // 误回收仅经寄存器可达的容器。详见 VM.cpp execute() 同名注释。
    GcManager::CallbackSuppressor gcSuppressor;
    // PERF-03 fix: 在循环入口加载一次指令预算上限到局部变量，避免主循环每条指令
    // 都执行 RuntimeConfig::instance().maxInstructions()（含 atomic load + 单例访问）。
    // 语义等价性：RuntimeConfig 设计为执行前配置（教学场景可调），execute() 期间
    // 不应被修改；stepOnce() 路径仍逐次读取以支持 IDE 单步调试时实时调整预算。
    const int64_t dynMaxInstr = RuntimeLimits::RuntimeConfig::instance().maxInstructions();
    while (!frames_.empty() && !hasError_) {
        VMResult r = executeOneInstruction();
        // L14: VM_EXCEPTION_THROW 表示 runtimeError 被 try/catch 捕获，ip 已设置为 catchIp。
        // 不视为错误，继续执行（不递增 ip，catchIp 由 throwException 写入 frame.ip）。
        if (r == VMResult::VM_EXCEPTION_THROW)
            continue;
        if (r != VMResult::VM_OK)
            return r;
        // DoS 防护：instructionCount_ 为成员字段，execute() 与 stepOnce() 共享同一预算。
        // 与栈式 VM 不同（其 execute 用局部计数器、stepOnce 用独立成员），RegisterVM
        // 让全速执行与单步执行共用累计计数，保证两种模式下的指令上限语义一致。
        // PERF-03 fix: dynMaxInstr 已在循环入口缓存，避免每条指令 atomic load。
        if (++instructionCount_ >= dynMaxInstr) {
            return runtimeError("指令数超出上限（可能存在死循环）");
        }
    }
    return hasError_ ? VMResult::VM_RUNTIME_ERROR : VMResult::VM_OK;
}

VMResult RegisterVM::stepOnce() {
    if (!initialized_)
        return runtimeError("VM 未初始化");
    if (frames_.empty())
        return VMResult::VM_OK;
    if (hasError_)
        return VMResult::VM_RUNTIME_ERROR;

    // AUDIT-R4 BUG-10 fix: 单步执行期间同样抑制增量 GC 回调（同 execute()）。
    GcManager::CallbackSuppressor gcSuppressor;

    // 累计指令计数（防止通过循环调用 stepOnce 绕过 DoS 防护）
    // L7 fix: 改为读取 RuntimeConfig 运行时配置（教学场景可调）
    if (++instructionCount_ >= RuntimeLimits::RuntimeConfig::instance().maxInstructions()) {
        return runtimeError("指令数超出上限（可能存在死循环）");
    }
    // L14: VM_EXCEPTION_THROW 表示异常被 try/catch 捕获，ip 已在 catchIp。
    // 对单步调用方而言此步已完成（非错误），转换为 VM_OK 保持向后兼容。
    VMResult r = executeOneInstruction();
    return (r == VMResult::VM_EXCEPTION_THROW) ? VMResult::VM_OK : r;
}

bool RegisterVM::isFinished() const {
    // 与栈式 VM 不同，RegisterVM 将 hasError_ 纳入"完成"判定：单步模式下一旦出错
    // 立即视为执行结束，使 IDE 能在错误指令处停住而非继续空转已损坏的状态。
    return frames_.empty() || hasError_;
}

// ============================================================
// 辅助方法
// ============================================================

// PERF-AUDIT-5 fix: reg() 已内联到 RegisterVM.h 头文件中。
// 原实现定义在 .cpp 中，每次寄存器访问是跨翻译单元函数调用，
// 阻断编译器内联优化。在 fib(24) 基准中约 150-225 万次非内联调用，
// 是 RegisterVM 慢于 StackVM 的主因。

VMResult RegisterVM::runtimeError(const std::string& msg, const std::string& diagCode) {
    // BUG-016 审计结论（2026-07-18）：不在 runtimeError 中调用 stepCallback_ 是
    // 设计决策，与 StackVM 的 runtimeError() 不调用 notifyStep() 行为一致，非 Bug。
    // 原因：
    //   1. stepCallback_/notifyStep 语义为"单条指令成功执行后的步进事件通知"，
    //      用于调试器在单步模式下推进 IP 显示。错误路径不应触发步进事件——
    //      指令并未成功执行，IP 未推进，调试器不应在错误指令处"步进"。
    //   2. 错误传播路径：runtimeError 设置 hasError_=true → 下一次
    //      executeOneInstruction 入口短路返回 VM_RUNTIME_ERROR（见行 182-183）
    //      → execute()/stepOnce() 返回错误码 → IDE 控制器检测到错误，
    //      通过 diagnostics_（getLastError()）显示错误信息（而非通过 stepCallback）。
    //   3. isFinished() 将 hasError_ 纳入完成判定（行 138），单步模式下错误后
    //      立即视为执行结束，IDE 在错误指令处停住而非继续空转。
    //   4. 三后端一致性：StackVM VM.cpp:101 runtimeError、Interpreter::runtimeError
    //      （抛 RuntimeError 异常）、RegisterVM runtimeError 均不走步进回调路径。
    //      若在 runtimeError 中添加 stepCallback 通知，反而会破坏三后端一致性，
    //      且让调试器误以为指令成功执行。
    // 因此各 execute* 函数中 `return runtimeError(...)` 路径不调用 stepCallback_
    // 是正确的设计，BUG-016 为误报。
    // P2-12: lastError_/lastErrorLine_ 字段已移除，错误信息直接写入 diagnostics_，
    // getLastError()/getLastErrorLine() 从 diagnostics_ 派生（与 VM 对齐）。
    // L14: try/catch 捕获 runtimeError — 若存在活动 try handler，将运行时错误转换为
    // 可捕获的异常（string Value），对齐 StackVM 与 Interpreter 语义。
    if (!tryStack_.empty() && !frames_.empty()) {
        size_t currentFrameIdx = frames_.size() - 1;
        if (tryStack_.back().frameIndex <= currentFrameIdx) {
            return throwException(Value(msg));
        }
    }
    hasError_ = true;
    // 获取当前行号 + 列号
    int errorLine = 0;
    int col = 0;
    if (!frames_.empty()) {
        const auto& frame = frames_.back();
        if (frame.chunk && frame.ip < frame.chunk->lines.size()) {
            errorLine = frame.chunk->lines[frame.ip];
        }
        // BUG-IBACKEND-2: 从 chunk 读取列号（Compiler 未传入列号时默认 0）
        if (frame.chunk && frame.ip < frame.chunk->columns.size()) {
            col = frame.chunk->columns[frame.ip];
        }
    }
    // BUG-IBACKEND-3: DiagSource 改为 RegisterVM 与 StackVM 区分；Logger 标签统一为 "RegisterVM"
    // P2 fix (错误码优先匹配): 透传稳定诊断码到 addError，供 ErrorHintEngine 按 code 精确匹配
    diagnostics_.addError(msg, errorLine, col, DiagSource::RegisterVM, diagCode);
    Logger::Error("RegisterVM: " + msg, "RegisterVM");
    return VMResult::VM_RUNTIME_ERROR;
}

// ============================================================
// 单步指令分发
// ============================================================

VMResult RegisterVM::executeOneInstruction() {
    // 单条寄存器指令执行的核心分派点，execute() 与 stepOnce() 共用。
    // 流程：① 判空帧/hasError_ 短路；② 取 RegOp；③ 用 instructionSizeAt 计算
    // 完整指令长度（变长指令据操作数展开）并做字节码截断边界检查；
    // ④ 按类别 switch 到 executeXxx 方法，再由各方法按具体 RegOp 处理。
    // 注意 REG_TYPE_CHECK / REG_SUPER_CALL 归入 executeMisc（杂项）——它们与
    // 异常、I/O 共用同一分发桶，仅因历史归类，不影响语义。
    if (hasError_)
        return VMResult::VM_RUNTIME_ERROR;
    if (frames_.empty())
        return VMResult::VM_OK;

    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;
    size_t& ip = frame.ip;

    if (ip >= chunk.code.size()) {
        return runtimeError("指令指针越界");
    }

    RegOp op = static_cast<RegOp>(chunk.code[ip]);
    size_t instrSize = chunk.instructionSizeAt(ip);
    if (ip + instrSize > chunk.code.size()) {
        return runtimeError("字节码截断: 指令不完整");
    }

    // 按类别分发
    switch (op) {
    // 常量加载
    case RegOp::REG_LOAD_CONST:
    case RegOp::REG_LOAD_NULL:
    case RegOp::REG_LOAD_TRUE:
    case RegOp::REG_LOAD_FALSE:
    case RegOp::REG_MOVE:
        return executeConstants(op, ip);

    // 算术
    case RegOp::REG_ADD:
    case RegOp::REG_SUB:
    case RegOp::REG_MUL:
    case RegOp::REG_DIV:
    case RegOp::REG_MOD:
    case RegOp::REG_NEGATE:
        return executeArith(op, ip);

    // 比较
    case RegOp::REG_EQ:
    case RegOp::REG_NEQ:
    case RegOp::REG_LT:
    case RegOp::REG_GT:
    case RegOp::REG_LTE:
    case RegOp::REG_GTE:
    case RegOp::REG_NOT:
        return executeCompare(op, ip);

    // 变量
    case RegOp::REG_LOAD_GLOBAL:
    case RegOp::REG_STORE_GLOBAL:
    case RegOp::REG_DEFINE_GLOBAL:
    case RegOp::REG_DELETE_GLOBAL:
    case RegOp::REG_LOAD_UPVALUE:
    case RegOp::REG_STORE_UPVALUE:
    case RegOp::REG_CLOSE_UPVALUE:
        return executeVars(op, ip);

    // 调用
    case RegOp::REG_CALL:
    case RegOp::REG_CALL_EXPR:
    case RegOp::REG_TAIL_CALL: // L18 eng-tailcall
    case RegOp::REG_RETURN:
    case RegOp::REG_RETURN_NULL:
    case RegOp::REG_MAKE_CLOSURE:
    case RegOp::REG_METHOD_CALL:
    case RegOp::REG_CLASS_NEW:
    case RegOp::REG_DEFINE_CLASS:
        return executeCalls(op, ip);

    // 容器
    case RegOp::REG_BUILD_ARRAY:
    case RegOp::REG_BUILD_DICT:
    case RegOp::REG_BUILD_TUPLE: // R98 元组与解构
    case RegOp::REG_INDEX_GET:
    case RegOp::REG_INDEX_SET:
    case RegOp::REG_BUILD_ENUM_VARIANT: // R99 枚举与 ADT
    case RegOp::REG_ENUM_VARIANT_NAME:
    case RegOp::REG_ENUM_VARIANT_FIELD:
    case RegOp::REG_MEMBER_GET:
    case RegOp::REG_MEMBER_SET:
    case RegOp::REG_SUPER_MEMBER_GET:
    case RegOp::REG_INIT_FIELD:
    case RegOp::REG_LEN: // R134 模式匹配扩展：容器长度
        return executeContainers(op, ip);

    // 控制流
    case RegOp::REG_JUMP:
    case RegOp::REG_JUMP_IF_FALSE:
        return executeControl(op, ip);

    // 异常与 I/O
    case RegOp::REG_TRY_BEGIN:
    case RegOp::REG_TRY_END:
    case RegOp::REG_THROW:
    case RegOp::REG_LOAD_EXCEPTION:
    case RegOp::REG_PRINT:
    case RegOp::REG_WRITEBACK_MEMBER_VAR:
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL:
    case RegOp::REG_WRITEBACK_INDEX_VAR:
    case RegOp::REG_WRITEBACK_INDEX_LOCAL:
    case RegOp::REG_WRITEBACK_MEMBER_UPVALUE:
    case RegOp::REG_WRITEBACK_INDEX_UPVALUE:
    case RegOp::REG_LOAD_MUTATED:
    case RegOp::REG_SUPER_CALL:
    case RegOp::REG_TYPE_CHECK:
    case RegOp::REG_TYPE_TEST: // R134: 软类型测试（push bool 而非抛错）
    case RegOp::REG_PUSH_JUMP_TARGET:
    case RegOp::REG_FINALLY_END:
        return executeMisc(op, ip);

    // R164 协程/生成器
    case RegOp::REG_YIELD:
    // 七特性 MVP 阶段 4：await 同步 drain
    case RegOp::REG_AWAIT:
        return executeCoroutineOps(op, ip);

    default:
        return runtimeError(ErrorFormat::formatStd("未知寄存器操作码: {}", static_cast<int>(op)));
    }
}

// ============================================================
// 异常处理
// ============================================================

VMResult RegisterVM::throwException(Value thrownValue) {
    // AUDIT-P1.1 fix: 异常传播中断 break/continue 续跳链，清空 pendingJumpStack_（与 StackVM 对齐）。
    pendingJumpStack_.clear();
    while (!tryStack_.empty()) {
        RegTryHandler handler = tryStack_.back();
        if (handler.frameIndex >= frames_.size()) {
            tryStack_.pop_back();
            continue;
        }
        // 跳转到 catch 块
        while (frames_.size() > handler.frameIndex + 1) {
            // C-3 fix: 弹帧前关闭指向该帧的 open upvalues，防止异常展开导致悬垂引用
            size_t unwindFrameIdx = frames_.size() - 1;
            // AUDIT-P2.5 fix: closeUpvaluesFrom 返回错误时立即终止异常展开，避免在损坏状态上继续
            if (closeUpvaluesFrom(unwindFrameIdx * RegCallFrame::MAX_REGISTERS) != VMResult::VM_OK)
                return VMResult::VM_RUNTIME_ERROR;
            frames_.pop_back();
        }
        if (!frames_.empty()) {
            // R7 fix: 跨帧异常展开后 handler.catchIp 可能相对于当前 chunk 越界
            // （handler 在子帧注册，但展开到 caller 后 caller 的 chunk 不同）。
            // defense-in-depth：入栈已校验，此处再校验当前 chunk 边界。
            auto& curFrame = currentFrame();
            if (handler.catchIp >= curFrame.chunk->code.size()) {
                return runtimeError("throwException: catchIp 相对当前 chunk 越界");
            }
            // AUDIT-R5 R1 fix: 同帧 catch 命中时仅关闭 try 开始后开启的 upvalue
            // （序号 >= handler.upvalueSeqFloor）。原 Bug #47 方案用帧基址 0 全量关闭，
            // 会把 try 之前创建、catch 后仍存活的闭包 upvalue 误关为快照，
            // 导致 catch 后闭包与变量不再共享（与 Interpreter/StackVM 不一致）。
            // try 期间开启的 upvalue（含嵌套作用域复用低槽位的情形）仍全部关闭，
            // 防止 catch 块复用寄存器时闭包读到错误值（保留 BUG-EXC-5 目标）。
            // AUDIT-P2.5 fix: 关闭失败时立即终止异常处理，不跳转 catchIp
            if (closeFrameUpvaluesSince(handler.frameIndex, handler.upvalueSeqFloor) != VMResult::VM_OK)
                return VMResult::VM_RUNTIME_ERROR;
            curFrame.ip = handler.catchIp;
            // P1-4 fix: 异常值存入 pendingException_，由 REG_LOAD_EXCEPTION 读取到指定寄存器。
            // 原方案固定写 R0 会覆盖用户变量/this（方法中 R0 是 this）。
            pendingException_ = std::move(thrownValue);
        }
        tryStack_.pop_back();
        // L14: 返回 VM_EXCEPTION_THROW 而非 VM_OK，通知调用方 ip 已被设置为 catchIp，
        // 不应再递增 ip。dispatch 循环将此结果视为"异常已捕获，继续执行"。
        return VMResult::VM_EXCEPTION_THROW;
    }
    // 未捕获的异常
    // BUG-IBACKEND-4 fix: 三后端消息一致——统一 toString + 200 字符截断
    // （对齐 StackVM VM.cpp:130-132 与 Interpreter）
    std::string str = thrownValue.toString();
    ErrorFormat::truncateForError(str);
    return runtimeError("未捕获的异常: " + str);
}

VMResult RegisterVM::closeUpvaluesFrom(size_t fromSlot) {
    // C-3 fix: stackSlot 编码为 frameIdx*32+slot，按全局地址范围 [fromSlot, ∞) 关闭。
    // 从对应帧（由 frameIdx 解码）读取当前值并标记为已关闭，防止帧弹出后悬垂引用。
    // AUDIT-P2.5 fix: 返回类型 void→VMResult，调用方可 fail-fast 传播错误。
    bool hadError = false;
    auto it = openUpvalues_.lower_bound(fromSlot);
    while (it != openUpvalues_.end()) {
        auto uv = it->second.uv.lock(); // AUDIT-R5 R1 fix: 条目改为携带序号的结构体
        if (uv && !uv->isClosed) {
            size_t frameIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slot = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            if (frameIdx < frames_.size()) {
                auto& targetFrame = frames_[frameIdx];
                if (slot < targetFrame.registerCount) {
                    uv->value = targetFrame.registers[slot];
                } else {
                    // AUDIT-P2.4 fix: slot 越界改为 runtimeError 以 fail-fast 暴露问题。
                    // 原实现仅打 Warning 继续执行，闭包静默捕获 null 难以排查根因。
                    // runtimeError 设置 hasError_=true，VM 在后续指令分发终止执行；
                    // 仍执行 isClosed=true + erase 以关闭 upvalue（保留默认 null 值），防止悬垂引用。
                    // AUDIT-P2.5 fix: 记录错误标志，函数末尾返回 VM_RUNTIME_ERROR，调用方立即 return。
                    (void)runtimeError("closeUpvaluesFrom: slot " + std::to_string(slot) + " >= registerCount " +
                                       std::to_string(targetFrame.registerCount) +
                                       " (frameIdx=" + std::to_string(frameIdx) + ")");
                    hadError = true;
                }
            } else {
                // AUDIT-P2.4 fix: upvalue 指向已弹出的帧改为 runtimeError 以 fail-fast 暴露问题。
                // 当前调用契约保证 frameIdx < frames_.size()（closeUpvaluesFrom 在帧弹出前调用），
                // 此分支不可达。若触达说明调用契约被破坏，原实现仅打 Warning 会继续执行，
                // upvalue 保留默认 null 值导致闭包读取错误值且难以排查。
                // runtimeError 设置 hasError_=true，VM 在后续指令分发终止执行。
                // AUDIT-P2.5 fix: 记录错误标志，函数末尾返回 VM_RUNTIME_ERROR，调用方立即 return。
                (void)runtimeError("closeUpvaluesFrom: upvalue 指向已弹出的帧 (frameIdx=" + std::to_string(frameIdx) +
                                   ", frames_.size()=" + std::to_string(frames_.size()) + ")");
                hadError = true;
            }
            uv->isClosed = true;
        }
        ++it;
    }
    openUpvalues_.erase(openUpvalues_.lower_bound(fromSlot), openUpvalues_.end());
    return hadError ? VMResult::VM_RUNTIME_ERROR : VMResult::VM_OK;
}

VMResult RegisterVM::closeFrameUpvaluesSince(size_t frameIdx, uint64_t seqFloor) {
    // AUDIT-R5 R1 fix: 仅关闭 frameIdx 帧内、开启序号 >= seqFloor 的 open upvalue。
    // 用于同帧 catch 命中：try 之前开启的 upvalue 保持 open（闭包与变量继续共享，
    // 对齐 Interpreter/StackVM），try 期间开启的全部关闭（含嵌套作用域复用
    // 低槽位的情形，Bug #47 场景），防止 catch 块复用寄存器时闭包读到错误值。
    bool hadError = false;
    const size_t frameBase = frameIdx * RegCallFrame::MAX_REGISTERS;
    auto it = openUpvalues_.lower_bound(frameBase);
    const auto rangeEnd = openUpvalues_.lower_bound(frameBase + RegCallFrame::MAX_REGISTERS);
    while (it != rangeEnd) {
        if (it->second.seq < seqFloor) {
            ++it; // try 之前开启：保持 open
            continue;
        }
        auto uv = it->second.uv.lock();
        if (uv && !uv->isClosed) {
            size_t fIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slot = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            if (fIdx < frames_.size() && slot < frames_[fIdx].registerCount) {
                uv->value = frames_[fIdx].registers[slot];
            } else {
                // 对齐 closeUpvaluesFrom 的 AUDIT-P2.4/P2.5 fail-fast 策略
                (void)runtimeError("closeFrameUpvaluesSince: upvalue 槽位越界 (frameIdx=" + std::to_string(fIdx) +
                                   ", slot=" + std::to_string(slot) + ")");
                hadError = true;
            }
            uv->isClosed = true;
        }
        it = openUpvalues_.erase(it);
    }
    return hadError ? VMResult::VM_RUNTIME_ERROR : VMResult::VM_OK;
}

Value* RegisterVM::resolveOpenUpvalueSlot(VMUpvalue& uv) {
    // C-3 fix: 解码 uv->stackSlot（frameIdx*32+slot）找到目标帧的寄存器。
    // 解决原代码硬编码 frames_[size-2] 导致多层嵌套/passthrough upvalue 访问错帧的问题。
    size_t frameIdx = uv.stackSlot / RegCallFrame::MAX_REGISTERS;
    size_t slot = uv.stackSlot % RegCallFrame::MAX_REGISTERS;
    if (frameIdx >= frames_.size()) {
        runtimeError("upvalue 帧索引越界");
        return nullptr;
    }
    auto& targetFrame = frames_[frameIdx];
    if (slot >= targetFrame.registerCount) {
        runtimeError("upvalue 栈槽越界");
        return nullptr;
    }
    return &targetFrame.registers[slot];
}

// ============================================================
// 调试接口
// ============================================================

std::vector<Value> RegisterVM::getRegisters() const {
    if (frames_.empty())
        return {};
    const auto& frame = currentFrame();
    // #8 fix: 从定长 array 中切出实际激活的寄存器子区间
    return std::vector<Value>(frame.registers.begin(), frame.registers.begin() + frame.registerCount);
}

std::unordered_map<std::string, Value> RegisterVM::getGlobals() const {
    std::unordered_map<std::string, Value> result;
    for (const auto& kv : globalNameToSlot_) {
        int slot = kv.second;
        if (slot >= 0 && slot < static_cast<int>(globalSlots_.size()) && !globalSlots_[slot].isNull()) {
            result[kv.first] = globalSlots_[slot];
        }
    }
    for (const auto& kv : globals_) {
        result[kv.first] = kv.second;
    }
    return result;
}

size_t RegisterVM::getCurrentIP() const {
    if (frames_.empty())
        return 0;
    return currentFrame().ip;
}

RegOp RegisterVM::getCurrentOpCode() const {
    if (frames_.empty())
        return RegOp::REG_RETURN_NULL;
    const auto& frame = currentFrame();
    if (frame.ip >= frame.chunk->code.size())
        return RegOp::REG_RETURN_NULL;
    return static_cast<RegOp>(frame.chunk->code[frame.ip]);
}

int RegisterVM::getCurrentLine() const {
    if (frames_.empty())
        return 0;
    const auto& frame = currentFrame();
    if (frame.ip < frame.chunk->lines.size()) {
        return frame.chunk->lines[frame.ip];
    }
    return 0;
}

std::string RegisterVM::getCurrentChunkName() const {
    if (frames_.empty())
        return "";
    return currentFrame().chunk->name;
}

// R104 Function Breakpoint：在 REG_CALL 执行前查询被调用函数名
std::string RegisterVM::peekCalledFunctionName() const {
    if (frames_.empty())
        return "";
    const auto& frame = currentFrame();
    if (!frame.chunk)
        return "";
    const auto& code = frame.chunk->code;
    size_t ip = frame.ip;
    if (ip >= code.size())
        return "";
    RegOp op = static_cast<RegOp>(code[ip]);
    if (op == RegOp::REG_CALL) {
        // REG_CALL 指令格式: [REG_CALL, dst, nameIdx_lo, nameIdx_hi, argCount, ...]
        if (ip + 4 >= code.size())
            return "";
        uint16_t nameIdx = code[ip + 2] | (code[ip + 3] << 8);
        const auto& constants = frame.chunk->constants;
        if (nameIdx >= constants.size() || !constants[nameIdx].isString())
            return "";
        return constants[nameIdx].stringVal();
    }
    if (op == RegOp::REG_CALL_EXPR) {
        // REG_CALL_EXPR 指令格式: [REG_CALL_EXPR, dst, calleeReg, argCount, ...]
        // 闭包名通过 calleeReg 的寄存器值获取
        if (ip + 3 >= code.size())
            return "";
        uint8_t calleeReg = code[ip + 2];
        if (calleeReg >= frame.registers.size())
            return "";
        const Value& callee = frame.registers[calleeReg];
        if (!callee.isClosure())
            return "";
        return callee.closureName();
    }
    return "";
}

// R104 Exception Breakpoint：检查当前指令是否为 REG_THROW
bool RegisterVM::isCurrentThrowInstruction() const {
    if (frames_.empty())
        return false;
    const auto& frame = currentFrame();
    if (!frame.chunk)
        return false;
    size_t ip = frame.ip;
    if (ip >= frame.chunk->code.size())
        return false;
    return static_cast<RegOp>(frame.chunk->code[ip]) == RegOp::REG_THROW;
}

// R161 Watchpoint：pre-execution peek 当前 IP 指令的写入目标（不执行指令）
WriteTarget RegisterVM::peekWriteTarget() const {
    WriteTarget wt;
    if (frames_.empty())
        return wt;
    const auto& frame = currentFrame();
    if (!frame.chunk)
        return wt;
    const auto& code = frame.chunk->code;
    size_t ip = frame.ip;
    if (ip >= code.size())
        return wt;

    RegOp op = static_cast<RegOp>(code[ip]);
    const auto& constants = frame.chunk->constants;
    auto readShort = [&code](size_t offset) -> uint16_t { return code[offset] | (code[offset + 1] << 8); };
    auto constStr = [&constants](uint16_t idx) -> std::string {
        return (idx < constants.size()) ? constants[idx].stringVal() : std::string{};
    };
    auto slotToGlobalName = [this](int slot) -> std::string {
        for (const auto& kv : globalNameToSlot_) {
            if (kv.second == slot)
                return kv.first;
        }
        return {};
    };
    auto regToLocalName = [&frame, ip](int reg) -> std::string { return frame.chunk->resolveSlotName(reg, ip); };

    switch (op) {
    // ---- 全局变量存储 ----
    case RegOp::REG_STORE_GLOBAL:
    case RegOp::REG_DEFINE_GLOBAL: {
        if (ip + 3 >= code.size())
            break;
        wt.isWrite = true;
        wt.varName = slotToGlobalName(readShort(ip + 2));
        break;
    }
    // ---- upvalue 存储 ----
    case RegOp::REG_STORE_UPVALUE: {
        if (ip + 2 >= code.size())
            break;
        wt.isWrite = true;
        uint8_t uvIdx = code[ip + 2];
        if (uvIdx < frame.chunk->upvalues.size()) {
            wt.varName = frame.chunk->upvalues[uvIdx].name;
        }
        break;
    }
    // ---- 字段写入 ----
    case RegOp::REG_MEMBER_SET:
    case RegOp::REG_INIT_FIELD: {
        if (ip + 3 >= code.size())
            break;
        wt.isWrite = true;
        wt.isFieldWrite = true;
        // REG_MEMBER_SET: [op, obj, fieldIdx_lo, fieldIdx_hi, val] — fieldIdx at ip+2
        // REG_INIT_FIELD: [op, fieldIdx_lo, fieldIdx_hi] — fieldIdx at ip+1
        if (op == RegOp::REG_MEMBER_SET) {
            wt.fieldName = constStr(readShort(ip + 2));
        } else {
            wt.fieldName = constStr(readShort(ip + 1));
        }
        break;
    }
    // ---- 字段写回（varIdx + fieldIdx）----
    case RegOp::REG_WRITEBACK_MEMBER_VAR: {
        if (ip + 4 >= code.size())
            break;
        wt.isWrite = true;
        wt.isFieldWrite = true;
        wt.varName = constStr(readShort(ip + 1));
        wt.fieldName = constStr(readShort(ip + 3));
        break;
    }
    // ---- 字段写回（localReg + fieldIdx）----
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL: {
        if (ip + 3 >= code.size())
            break;
        wt.isWrite = true;
        wt.isFieldWrite = true;
        wt.localSlot = code[ip + 1];
        wt.varName = regToLocalName(wt.localSlot);
        wt.fieldName = constStr(readShort(ip + 2));
        break;
    }
    // ---- 字段写回（uvIdx + fieldIdx）----
    case RegOp::REG_WRITEBACK_MEMBER_UPVALUE: {
        if (ip + 3 >= code.size())
            break;
        wt.isWrite = true;
        wt.isFieldWrite = true;
        uint8_t uvIdx = code[ip + 1];
        if (uvIdx < frame.chunk->upvalues.size()) {
            wt.varName = frame.chunk->upvalues[uvIdx].name;
        }
        wt.fieldName = constStr(readShort(ip + 2));
        break;
    }
    // ---- 索引写入 ----
    case RegOp::REG_INDEX_SET: {
        wt.isWrite = true;
        wt.isIndexWrite = true;
        break;
    }
    // ---- 索引写回（varIdx）----
    case RegOp::REG_WRITEBACK_INDEX_VAR: {
        if (ip + 2 >= code.size())
            break;
        wt.isWrite = true;
        wt.isIndexWrite = true;
        wt.varName = constStr(readShort(ip + 1));
        break;
    }
    // ---- 索引写回（localReg）----
    case RegOp::REG_WRITEBACK_INDEX_LOCAL: {
        if (ip + 1 >= code.size())
            break;
        wt.isWrite = true;
        wt.isIndexWrite = true;
        wt.localSlot = code[ip + 1];
        wt.varName = regToLocalName(wt.localSlot);
        break;
    }
    // ---- 索引写回（uvIdx）----
    case RegOp::REG_WRITEBACK_INDEX_UPVALUE: {
        if (ip + 1 >= code.size())
            break;
        wt.isWrite = true;
        wt.isIndexWrite = true;
        uint8_t uvIdx = code[ip + 1];
        if (uvIdx < frame.chunk->upvalues.size()) {
            wt.varName = frame.chunk->upvalues[uvIdx].name;
        }
        break;
    }
    default:
        break;
    }
    return wt;
}

std::vector<RegisterVM::RegCallStackEntry> RegisterVM::getCallStack() const {
    std::vector<RegCallStackEntry> result;
    result.reserve(frames_.size());
    for (const auto& frame : frames_) {
        RegCallStackEntry entry;
        entry.functionName = frame.chunk ? frame.chunk->name : "";
        entry.ip = frame.ip;
        int line = 0;
        if (frame.chunk && frame.ip < frame.chunk->lines.size()) {
            line = frame.chunk->lines[frame.ip];
        }
        entry.line = line;
        result.push_back(entry);
    }
    return result;
}

// BUG-IDE-12 fix: 获取当前帧的局部变量名→值映射
std::unordered_map<std::string, Value> RegisterVM::getCurrentFrameLocals() const {
    std::unordered_map<std::string, Value> result;
    if (frames_.empty())
        return result;
    const auto& frame = frames_.back();
    if (!frame.chunk)
        return result;
    // 条件断点修复(R75): 先注入upvalue（外层闭包变量），局部变量同名时遮蔽upvalue
    const auto& uvDescs = frame.chunk->upvalues;
    for (size_t i = 0; i < uvDescs.size() && i < frame.upvalues.size(); ++i) {
        const auto& desc = uvDescs[i];
        if (desc.name.empty())
            continue;
        const auto& uv = frame.upvalues[i];
        if (!uv)
            continue;
        if (uv->isClosed) {
            result[desc.name] = uv->value;
        } else {
            size_t frameIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slot = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            if (frameIdx < frames_.size() && slot < frames_[frameIdx].registerCount) {
                result[desc.name] = frames_[frameIdx].registers[slot];
            }
        }
    }
    // 遍历 chunk 的 localRegNames，从寄存器窗口反查值
    // L1 fix: 优先使用 resolveSlotName(reg, frame.ip) 按 IP 范围反查变量名，
    // 解决兄弟作用域寄存器复用导致的变量名错位。
    const auto& chunk = *frame.chunk;
    size_t curIp = frame.ip;
    for (size_t reg = 0; reg < chunk.localRegNames.size() && reg < frame.registers.size(); ++reg) {
        const std::string& name = chunk.resolveSlotName(reg, curIp);
        if (name.empty())
            continue;
        result[name] = frame.registers[reg];
    }
    return result;
}

// P2-3 fix: 获取指定帧的局部变量（用于调用栈面板显示各帧 locals）
std::unordered_map<std::string, Value> RegisterVM::getFrameLocalsAt(size_t frameIndex) const {
    std::unordered_map<std::string, Value> result;
    if (frameIndex >= frames_.size())
        return result;
    const auto& frame = frames_[frameIndex];
    if (!frame.chunk)
        return result;
    // 条件断点修复(R75): 先注入upvalue（外层闭包变量）
    const auto& uvDescs = frame.chunk->upvalues;
    for (size_t i = 0; i < uvDescs.size() && i < frame.upvalues.size(); ++i) {
        const auto& desc = uvDescs[i];
        if (desc.name.empty())
            continue;
        const auto& uv = frame.upvalues[i];
        if (!uv)
            continue;
        if (uv->isClosed) {
            result[desc.name] = uv->value;
        } else {
            size_t fi = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slot = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            if (fi < frames_.size() && slot < frames_[fi].registerCount) {
                result[desc.name] = frames_[fi].registers[slot];
            }
        }
    }
    // L1 fix: 优先使用 resolveSlotName(reg, frame.ip) 按 IP 范围反查变量名
    const auto& chunk = *frame.chunk;
    size_t curIp = frame.ip;
    for (size_t reg = 0; reg < chunk.localRegNames.size() && reg < frame.registers.size(); ++reg) {
        const std::string& name = chunk.resolveSlotName(reg, curIp);
        if (name.empty())
            continue;
        result[name] = frame.registers[reg];
    }
    return result;
}

// ============================================================
// 拓展·调试器 setVariable：调试暂停时写变量（与 VM.cpp 镜像）
// ============================================================
bool RegisterVM::setGlobalValue(const std::string& name, const Value& val) {
    // 与 getGlobals 同查找链：globalNameToSlot_ → globalSlots_，fallback globals_；
    // 不新建变量，调试改值仅覆盖已存在的全局。
    auto slotIt = globalNameToSlot_.find(name);
    if (slotIt != globalNameToSlot_.end() && slotIt->second >= 0 &&
        slotIt->second < static_cast<int>(globalSlots_.size())) {
        globalSlots_[slotIt->second] = val;
        return true;
    }
    auto it = globals_.find(name);
    if (it == globals_.end())
        return false;
    it->second = val;
    return true;
}

bool RegisterVM::setFrameLocalAt(size_t frameIndex, const std::string& name, const Value& val) {
    if (frameIndex >= frames_.size())
        return false;
    auto& frame = frames_[frameIndex];
    if (!frame.chunk)
        return false;
    // 与 getFrameLocalsAt 镜像：先按 resolveSlotName(reg, ip) 反查寄存器
    const auto& chunk = *frame.chunk;
    size_t curIp = frame.ip;
    for (size_t reg = 0; reg < chunk.localRegNames.size() && reg < frame.registers.size(); ++reg) {
        if (chunk.resolveSlotName(reg, curIp) == name) {
            frame.registers[reg] = val;
            return true;
        }
    }
    // 未命中：回退 upvalue（外层闭包变量）写入，与 getFrameLocalsAt 的
    // upvalue 读取分支镜像（closed 写 uv->value，open 写寄存器槽）
    const auto& uvDescs = frame.chunk->upvalues;
    for (size_t i = 0; i < uvDescs.size() && i < frame.upvalues.size(); ++i) {
        if (uvDescs[i].name != name)
            continue;
        auto& uv = frame.upvalues[i];
        if (!uv)
            continue;
        if (uv->isClosed) {
            uv->value = val;
            return true;
        }
        size_t fi = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
        size_t slot = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
        if (fi < frames_.size() && slot < frames_[fi].registerCount) {
            frames_[fi].registers[slot] = val;
            return true;
        }
    }
    return false;
}
