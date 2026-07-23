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
#include "common/TypeChecker.h" // 2026-06-29: typeMatchValue（REG_TYPE_CHECK）
#include "common/Utf8Utils.h"
#include "compiler/RegisterBytecode.h"
#include "interpreter/BuiltinMethods.h"
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
    tryStack_.clear();
    pendingException_ = Value::nullValue(); // P1-4 fix: 清理异常值
    pendingJumpStack_.clear();              // AUDIT-P1.1 fix: 清理续跳栈
    hasError_ = false;
    lastError_.clear();
    lastErrorLine_ = 0;
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
        lastError_ = "RegisterVM 未初始化，无法回滚";
        hasError_ = true;
        return false;
    }
    if (targetFrameCount > frames_.size()) {
        lastError_ = "回滚目标帧数超过当前帧数";
        hasError_ = true;
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
    lastError_.clear();
    lastErrorLine_ = 0;
    hasError_ = false;
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
    while (!frames_.empty() && !hasError_) {
        VMResult r = executeOneInstruction();
        if (r != VMResult::VM_OK)
            return r;
        // DoS 防护：instructionCount_ 为成员字段，execute() 与 stepOnce() 共享同一预算。
        // 与栈式 VM 不同（其 execute 用局部计数器、stepOnce 用独立成员），RegisterVM
        // 让全速执行与单步执行共用累计计数，保证两种模式下的指令上限语义一致。
        // L7 fix: 改为读取 RuntimeConfig 运行时配置（教学场景可调）
        if (++instructionCount_ >= RuntimeLimits::RuntimeConfig::instance().maxInstructions()) {
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

    // 累计指令计数（防止通过循环调用 stepOnce 绕过 DoS 防护）
    // L7 fix: 改为读取 RuntimeConfig 运行时配置（教学场景可调）
    if (++instructionCount_ >= RuntimeLimits::RuntimeConfig::instance().maxInstructions()) {
        return runtimeError("指令数超出上限（可能存在死循环）");
    }
    return executeOneInstruction();
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

VMResult RegisterVM::runtimeError(const std::string& msg) {
    // BUG-016 审计结论（2026-07-18）：不在 runtimeError 中调用 stepCallback_ 是
    // 设计决策，与 StackVM 的 runtimeError() 不调用 notifyStep() 行为一致，非 Bug。
    // 原因：
    //   1. stepCallback_/notifyStep 语义为"单条指令成功执行后的步进事件通知"，
    //      用于调试器在单步模式下推进 IP 显示。错误路径不应触发步进事件——
    //      指令并未成功执行，IP 未推进，调试器不应在错误指令处"步进"。
    //   2. 错误传播路径：runtimeError 设置 hasError_=true → 下一次
    //      executeOneInstruction 入口短路返回 VM_RUNTIME_ERROR（见行 182-183）
    //      → execute()/stepOnce() 返回错误码 → IDE 控制器检测到错误，
    //      通过 lastError_/diagnostics_ 显示错误信息（而非通过 stepCallback）。
    //   3. isFinished() 将 hasError_ 纳入完成判定（行 138），单步模式下错误后
    //      立即视为执行结束，IDE 在错误指令处停住而非继续空转。
    //   4. 三后端一致性：StackVM VM.cpp:101 runtimeError、Interpreter::runtimeError
    //      （抛 RuntimeError 异常）、RegisterVM runtimeError 均不走步进回调路径。
    //      若在 runtimeError 中添加 stepCallback 通知，反而会破坏三后端一致性，
    //      且让调试器误以为指令成功执行。
    // 因此各 execute* 函数中 `return runtimeError(...)` 路径不调用 stepCallback_
    // 是正确的设计，BUG-016 为误报。
    hasError_ = true;
    lastError_ = msg;
    // 获取当前行号 + 列号
    int col = 0;
    if (!frames_.empty()) {
        const auto& frame = frames_.back();
        if (frame.chunk && frame.ip < frame.chunk->lines.size()) {
            lastErrorLine_ = frame.chunk->lines[frame.ip];
        }
        // BUG-IBACKEND-2: 从 chunk 读取列号（Compiler 未传入列号时默认 0）
        if (frame.chunk && frame.ip < frame.chunk->columns.size()) {
            col = frame.chunk->columns[frame.ip];
        }
    }
    // BUG-IBACKEND-3: DiagSource 改为 RegisterVM 与 StackVM 区分；Logger 标签统一为 "RegisterVM"
    diagnostics_.addError(msg, lastErrorLine_, col, DiagSource::RegisterVM);
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
        return executeCoroutineOps(op, ip);

    default:
        return runtimeError(ErrorFormat::format("未知寄存器操作码: %d", static_cast<int>(op)));
    }
}

// ============================================================
// 常量加载指令
// ============================================================

VMResult RegisterVM::executeConstants(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;
    int line = (ip < chunk.lines.size()) ? chunk.lines[ip] : 0;

    switch (op) {
    case RegOp::REG_LOAD_CONST: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t constIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (constIdx >= chunk.constants.size()) {
            return runtimeError("常量池索引越界");
        }
        reg(dst) = chunk.constants[constIdx];
        ip += 4;
        break;
    }
    case RegOp::REG_LOAD_NULL: {
        uint8_t dst = chunk.code[ip + 1];
        reg(dst) = Value::nullValue();
        ip += 2;
        break;
    }
    case RegOp::REG_LOAD_TRUE: {
        uint8_t dst = chunk.code[ip + 1];
        reg(dst) = Value(true);
        ip += 2;
        break;
    }
    case RegOp::REG_LOAD_FALSE: {
        uint8_t dst = chunk.code[ip + 1];
        reg(dst) = Value(false);
        ip += 2;
        break;
    }
    case RegOp::REG_MOVE: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t src = chunk.code[ip + 2];
        reg(dst) = reg(src);
        ip += 3;
        break;
    }
    default:
        return runtimeError("executeConstants: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 算术指令
// ============================================================

VMResult RegisterVM::executeArith(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    if (op == RegOp::REG_NEGATE) {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t src = chunk.code[ip + 2];
        const Value& v = reg(src);
        if (v.isInt()) {
            if (OverflowCheck::negateOverflow(v.intVal())) {
                // BUG-OVF-1 fix: 错误消息与 Interpreter.cpp:802 / VM.cpp:1160 一致
                return runtimeError("整数溢出：无法对最小值取负");
            }
            reg(dst) = Value(-v.intVal());
        } else if (v.isFloat()) {
            reg(dst) = Value(-v.floatVal());
        } else {
            return runtimeError("一元减运算需要数值类型");
        }
        ip += 3;
    } else {
        // 二元算术
        uint8_t dst = chunk.code[ip + 1];
        uint8_t s1 = chunk.code[ip + 2];
        uint8_t s2 = chunk.code[ip + 3];
        const Value& a = reg(s1);
        const Value& b = reg(s2);

        // P1 fix: ADD 支持字符串拼接（与栈式 VM OP_ADD 语义一致）。
        // 任一操作数为字符串即触发拼接；非字符串侧用 toString() 转换。
        // BUG-REGVM-1 fix: 字符串拼接路径不可直接 return，需落入函数末尾统一
        // stepCallback_ 调用点，否则调试器单步模式丢失步进事件。改用 goto 跳转。
        if (op == RegOp::REG_ADD && (a.isString() || b.isString())) {
            std::string concat;
            if (a.isString() && b.isString()) {
                const auto& ls = a.stringVal();
                const auto& rs = b.stringVal();
                concat.reserve(ls.size() + rs.size());
                concat.append(ls).append(rs);
            } else if (a.isString()) {
                const auto& ls = a.stringVal();
                auto rs = b.toString();
                concat.reserve(ls.size() + rs.size());
                concat.append(ls).append(rs);
            } else {
                auto ls = a.toString();
                const auto& rs = b.stringVal();
                concat.reserve(ls.size() + rs.size());
                concat.append(ls).append(rs);
            }
            reg(dst) = Value(std::move(concat));
            ip += 4;
            goto arithDone;
        }

        if (!a.isNumber() || !b.isNumber()) {
            return runtimeError("算术运算需要数值类型");
        }

        {
            NumericOps::ArithOp arithOp;
            switch (op) {
            case RegOp::REG_ADD:
                arithOp = NumericOps::ArithOp::Add;
                break;
            case RegOp::REG_SUB:
                arithOp = NumericOps::ArithOp::Sub;
                break;
            case RegOp::REG_MUL:
                arithOp = NumericOps::ArithOp::Mul;
                break;
            case RegOp::REG_DIV:
                arithOp = NumericOps::ArithOp::Div;
                break;
            case RegOp::REG_MOD:
                arithOp = NumericOps::ArithOp::Mod;
                break;
            default:
                return runtimeError("executeArith: 未知操作码");
            }
            auto r = NumericOps::computeArith(arithOp, a.isInt(), a.isInt() ? a.intVal() : 0, a.toDouble(), b.isInt(),
                                              b.isInt() ? b.intVal() : 0, b.toDouble());
            switch (r.status) {
            case NumericOps::ArithStatus::DivByZero:
                return runtimeError("除零错误");
            case NumericOps::ArithStatus::IntOverflow:
                return runtimeError("整数运算溢出");
            case NumericOps::ArithStatus::NotNumeric:
                return runtimeError("算术运算需要数值类型");
            case NumericOps::ArithStatus::OK:
                reg(dst) = r.isIntResult ? Value(r.intVal) : Value(r.floatVal);
                break;
            // Bug-6 同型修复：与 VM.cpp:389 ArithStatus switch 对齐。落空时 result
            // 保持默认值（VAL_NULL），下方 reg(dst) = std::move(result) 会写入脏结果。
            default:
                return runtimeError("内部错误: 未知算术状态");
            }
        }
        ip += 4;
    }

arithDone:
    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 比较指令
// ============================================================

VMResult RegisterVM::executeCompare(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    if (op == RegOp::REG_NOT) {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t src = chunk.code[ip + 2];
        const Value& v = reg(src);
        // 与栈式 VM OP_NOT 对齐：使用 isTruthy() 而非严格要求 bool 类型。
        reg(dst) = Value(!v.isTruthy());
        ip += 3;
    } else {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t s1 = chunk.code[ip + 2];
        uint8_t s2 = chunk.code[ip + 3];
        const Value& a = reg(s1);
        const Value& b = reg(s2);

        bool result = false;
        bool isStringCompare = a.isString() && b.isString();

        switch (op) {
        case RegOp::REG_EQ:
            result = a.equals(b);
            break;
        case RegOp::REG_NEQ:
            result = !a.equals(b);
            break;
        case RegOp::REG_LT:
            if (isStringCompare)
                result = a.stringVal() < b.stringVal();
            else if (a.isNumber() && b.isNumber())
                result = a.toDouble() < b.toDouble();
            else
                return runtimeError("比较运算需要数值或字符串类型");
            break;
        case RegOp::REG_GT:
            if (isStringCompare)
                result = a.stringVal() > b.stringVal();
            else if (a.isNumber() && b.isNumber())
                result = a.toDouble() > b.toDouble();
            else
                return runtimeError("比较运算需要数值或字符串类型");
            break;
        case RegOp::REG_LTE:
            if (isStringCompare)
                result = a.stringVal() <= b.stringVal();
            else if (a.isNumber() && b.isNumber())
                result = a.toDouble() <= b.toDouble();
            else
                return runtimeError("比较运算需要数值或字符串类型");
            break;
        case RegOp::REG_GTE:
            if (isStringCompare)
                result = a.stringVal() >= b.stringVal();
            else if (a.isNumber() && b.isNumber())
                result = a.toDouble() >= b.toDouble();
            else
                return runtimeError("比较运算需要数值或字符串类型");
            break;
        default:
            return runtimeError("executeCompare: 未知操作码");
        }
        reg(dst) = Value(result);
        ip += 4;
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 变量指令
// ============================================================

VMResult RegisterVM::executeVars(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case RegOp::REG_LOAD_GLOBAL: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t slotOrName = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (slotOrName & 0x8000) {
            // 名称版
            uint16_t nameIdx = slotOrName & 0x7FFF;
            if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                return runtimeError("全局变量名称索引无效");
            }
            const std::string& name = chunk.constants[nameIdx].stringVal();
            auto gsIt = globalNameToSlot_.find(name);
            if (gsIt != globalNameToSlot_.end() && gsIt->second < static_cast<int>(globalSlots_.size())) {
                reg(dst) = globalSlots_[gsIt->second];
            } else {
                auto it = globals_.find(name);
                if (it != globals_.end()) {
                    reg(dst) = it->second;
                } else {
                    // R97 #11 fix: 三后端 super 错误消息统一（与 VM.cpp OP_GET_VAR 对齐）
                    if (name == "this") {
                        return runtimeError(ErrorMessages::kSuperOutsideMethod);
                    }
                    return runtimeError("未定义的变量: " + name);
                }
            }
        } else {
            // 槽位版
            if (slotOrName >= globalSlots_.size()) {
                return runtimeError("全局变量槽位越界");
            }
            reg(dst) = globalSlots_[slotOrName];
        }
        ip += 4;
        break;
    }
    case RegOp::REG_STORE_GLOBAL: {
        // R164 fixup2: 赋值（非声明）路径——未定义变量必须报错，对齐 Interpreter/StackVM。
        // 原先与 REG_DEFINE_GLOBAL 共用 case，导致未定义变量赋值被静默"自动声明"。
        uint8_t src = chunk.code[ip + 1];
        uint16_t slotOrName = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (slotOrName & 0x8000) {
            uint16_t nameIdx = slotOrName & 0x7FFF;
            if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                return runtimeError("全局变量名称索引无效");
            }
            const std::string& name = chunk.constants[nameIdx].stringVal();
            auto gsIt = globalNameToSlot_.find(name);
            if (gsIt != globalNameToSlot_.end() && gsIt->second < static_cast<int>(globalSlots_.size())) {
                globalSlots_[gsIt->second] = reg(src);
            } else {
                // 槽位表未命中，回退到 globals_ 名称表查找（已声明过的全局变量）
                auto it = globals_.find(name);
                if (it == globals_.end()) {
                    return runtimeError("未定义的变量: " + name);
                }
                it->second = reg(src);
            }
        } else {
            if (slotOrName >= globalSlots_.size()) {
                return runtimeError("全局变量槽位越界");
            }
            globalSlots_[slotOrName] = reg(src);
        }
        ip += 4;
        break;
    }
    case RegOp::REG_DEFINE_GLOBAL: {
        // 声明路径——允许创建新全局变量（var 声明语义）
        uint8_t src = chunk.code[ip + 1];
        uint16_t slotOrName = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (slotOrName & 0x8000) {
            uint16_t nameIdx = slotOrName & 0x7FFF;
            if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                return runtimeError("全局变量名称索引无效");
            }
            const std::string& name = chunk.constants[nameIdx].stringVal();
            auto gsIt = globalNameToSlot_.find(name);
            if (gsIt != globalNameToSlot_.end() && gsIt->second < static_cast<int>(globalSlots_.size())) {
                globalSlots_[gsIt->second] = reg(src);
            } else {
                globals_[name] = reg(src);
            }
        } else {
            if (slotOrName >= globalSlots_.size()) {
                return runtimeError("全局变量槽位越界");
            }
            globalSlots_[slotOrName] = reg(src);
        }
        ip += 4;
        break;
    }
    case RegOp::REG_DELETE_GLOBAL: {
        uint16_t nameIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("全局变量名称索引无效");
        }
        const std::string& name = chunk.constants[nameIdx].stringVal();
        globals_.erase(name);
        auto gsIt = globalNameToSlot_.find(name);
        if (gsIt != globalNameToSlot_.end()) {
            if (gsIt->second < static_cast<int>(globalSlots_.size())) {
                globalSlots_[gsIt->second] = Value::nullValue();
            }
        }
        ip += 3;
        break;
    }
    case RegOp::REG_LOAD_UPVALUE: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t uvIdx = chunk.code[ip + 2];
        if (uvIdx >= frame.upvalues.size()) {
            return runtimeError("upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        if (uv->isClosed) {
            reg(dst) = uv->value;
        } else {
            // C-3 fix: 通过编码的 frameIdx 定位真实拥有该槽的帧（支持多层嵌套/passthrough）
            Value* slot = resolveOpenUpvalueSlot(*uv);
            if (!slot)
                return VMResult::VM_RUNTIME_ERROR;
            reg(dst) = *slot;
        }
        ip += 3;
        break;
    }
    case RegOp::REG_STORE_UPVALUE: {
        uint8_t src = chunk.code[ip + 1];
        uint8_t uvIdx = chunk.code[ip + 2];
        if (uvIdx >= frame.upvalues.size()) {
            return runtimeError("upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        if (uv->isClosed) {
            uv->value = reg(src);
        } else {
            // C-3 fix: 通过编码的 frameIdx 定位真实拥有该槽的帧
            Value* slot = resolveOpenUpvalueSlot(*uv);
            if (!slot)
                return VMResult::VM_RUNTIME_ERROR;
            *slot = reg(src);
            // BUG-UPVAL-1 fix: 对齐 StackVM OP_SET_UPVALUE 的 V-P1-6 fix——
            // 若修改的是外层方法帧的 registers[0]（this 槽），标记 fieldsModified，
            // 确保 executeReturnImpl 同步 methodThis 到 caller 的 receiverReg。
            //
            // BUG-006 审计结论（2026-07-18）：本检查对 RegisterVM 完整且正确，无需扩展 slot 范围。
            // 原因：RegisterVM 与 StackVM 的字段存储模型根本不同——
            //   - StackVM 栈布局: [bp+0=this, bp+1..bp+1+N=字段槽, ...locals]，
            //     其中 N = chunk.fieldOrder.size()。V-P1-6 fix 必须检查
            //     [bp+1, bp+1+N) 区间，因为字段以 Value 副本形式存在栈槽中。
            //   - RegisterVM 寄存器布局: [reg0=this, reg1..argCount=args, ...locals]，
            //     **没有字段槽区域**。字段直接存储在 InstanceData.fields() 内，
            //     通过 reg0 指向的 InstanceData 指针访问（REG_MEMBER_SET 直接调用
            //     obj.fields()[name] = val，会触发 COW detach 更新 obj 的 box_）。
            // 因此只有 reg0（this 槽）被整体替换时才需要标记 fieldsModified，
            // 因为这相当于替换了 caller 的 receiverReg 应当看到的实例对象。
            // slot > 0 的寄存器只能是参数或局部变量，与字段同步无关。
            //
            // 不变量：若未来 RegisterVM 引入字段槽区域（如 [reg1, reg1+N)），
            // 必须在此扩展检查范围，并对 REG_WRITEBACK_*_UPVALUE 三处同步更新。
            size_t frameIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slotIdx = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            if (slotIdx == 0 && frameIdx < frames_.size() && frames_[frameIdx].isMethodCall) {
                frames_[frameIdx].fieldsModified = true;
            }
        }
        ip += 3;
        break;
    }
    case RegOp::REG_CLOSE_UPVALUE: {
        // B1 fix: 关闭所有指向 slot >= frameIdx*MAX_REGISTERS+slotBase 的 open upvalues，
        // 使块作用域退出时闭包捕获退出时刻的值快照（对齐 Interpreter per-block env）。
        uint8_t slotBase = chunk.code[ip + 1];
        size_t currentFrameIdx = frames_.size() - 1;
        // AUDIT-P2.5 fix: closeUpvaluesFrom 返回错误时立即终止，避免在损坏状态上继续
        if (closeUpvaluesFrom(currentFrameIdx * RegCallFrame::MAX_REGISTERS + slotBase) != VMResult::VM_OK)
            return VMResult::VM_RUNTIME_ERROR;
        ip += 2;
        break;
    }
    default:
        return runtimeError("executeVars: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 控制流指令
// ============================================================

VMResult RegisterVM::executeControl(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case RegOp::REG_JUMP: {
        uint16_t offset = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        // 与栈式 VM 对齐：检查跳转目标越界，防止损坏字节码导致 ip 越界读取。
        if (offset >= chunk.code.size()) {
            return runtimeError("跳转目标越界");
        }
        ip = offset;
        break;
    }
    case RegOp::REG_JUMP_IF_FALSE: {
        uint8_t src = chunk.code[ip + 1];
        uint16_t offset = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        const Value& v = reg(src);
        // 与栈式 VM OP_JUMP_IF_FALSE 对齐：使用 isTruthy() 而非严格要求 bool 类型。
        // 原 impl 仅接受 bool，导致 `if (5)` / `while ("str")` 等合法 MiniLang 代码报错。
        if (!v.isTruthy()) {
            if (offset >= chunk.code.size()) {
                return runtimeError("跳转目标越界");
            }
            ip = offset;
        } else {
            ip += 4;
        }
        break;
    }
    default:
        return runtimeError("executeControl: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 容器指令
// ============================================================

// R120 重构：原 executeArrayOps 224 行单 switch 拆为 thin dispatcher + 3 个 helper。
// 拆分依据：与 R118 StackVM executeContainerOps 同模式（VM 类 execute*Ops 系列 + 共享状态全
// 成员 + 3 大语义分组：BUILD/INDEX/ENUM_QUERY）。原 case 平均 32 行（最大 58 行 REG_INDEX_GET），
// 不属于 R107 准则定义的扁平 dispatch switch。helper 签名 (RegOp op, size_t& ip) 即可，
// 共享状态传递成本为零——chunk 通过 currentFrame().chunk 获取，所有可变状态均为成员变量
// （reg() / lastMutatedReceiverReg_ / enumRegistry_ / stepCallback_ / frames_）。
// 注：原函数名 executeArrayOps 是历史命名（实际同时处理 array/tuple/index/enum variant），
// 沿用父级 executeContainers 调用约定不重命名，仅内部拆分。REG_BUILD_ENUM_VARIANT 归入
// BUILD 类（与 R118 StackVM OP_BUILD_ENUM_VARIANT → executeContainerBuildOps 同构）。
VMResult RegisterVM::executeArrayOps(RegOp op, size_t& ip) {
    switch (op) {
    case RegOp::REG_BUILD_ARRAY:
    case RegOp::REG_BUILD_TUPLE:
    case RegOp::REG_BUILD_ENUM_VARIANT:
        return executeArrayBuildOps(op, ip);
    case RegOp::REG_INDEX_GET:
    case RegOp::REG_INDEX_SET:
    case RegOp::REG_LEN: // R134: 容器长度（与 INDEX_GET 同属索引访问语义族）
        return executeArrayIndexOps(op, ip);
    case RegOp::REG_ENUM_VARIANT_NAME:
    case RegOp::REG_ENUM_VARIANT_FIELD:
        return executeArrayEnumQueryOps(op, ip);
    default:
        return runtimeError("executeArrayOps: 未知操作码");
    }
}

// ============================================================
// executeArrayBuildOps - 容器构造指令（BUILD 类）
// REG_BUILD_ARRAY / REG_BUILD_TUPLE / REG_BUILD_ENUM_VARIANT
// ============================================================
VMResult RegisterVM::executeArrayBuildOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_BUILD_ARRAY: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t count = chunk.code[ip + 2];
        std::vector<Value> elems;
        elems.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t elemReg = chunk.code[ip + 3 + i];
            elems.push_back(reg(elemReg));
        }
        reg(dst) = Value(std::move(elems));
        ip += 3 + count;
        break;
    }
    case RegOp::REG_BUILD_TUPLE: {
        // R98 元组与解构：构建 immutable 元组（结构与 REG_BUILD_ARRAY 相同）
        uint8_t dst = chunk.code[ip + 1];
        uint8_t count = chunk.code[ip + 2];
        std::vector<Value> elems;
        elems.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t elemReg = chunk.code[ip + 3 + i];
            elems.push_back(reg(elemReg));
        }
        reg(dst) = Value::makeTuple(std::move(elems));
        ip += 3 + count;
        break;
    }
    // R99 枚举与 ADT：构造 enum variant（变长指令）
    // 布局: [op, dst, eLo, eHi, vLo, vHi, argCount, arg1, arg2, ...]
    case RegOp::REG_BUILD_ENUM_VARIANT: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t enumNameIdx = chunk.code[ip + 2] | (static_cast<uint16_t>(chunk.code[ip + 3]) << 8);
        uint16_t variantNameIdx = chunk.code[ip + 4] | (static_cast<uint16_t>(chunk.code[ip + 5]) << 8);
        uint8_t argCount = chunk.code[ip + 6];
        if (enumNameIdx >= chunk.constants.size() || variantNameIdx >= chunk.constants.size()) {
            return runtimeError("REG_BUILD_ENUM_VARIANT: 常量池索引越界");
        }
        const std::string& enumName = chunk.constants[enumNameIdx].stringVal();
        const std::string& variantName = chunk.constants[variantNameIdx].stringVal();

        // R99 enum 校验：运行时检查 enum 已声明、variant 存在、参数 arity 一致。
        // 对齐 VM::executeContainerOps 的 OP_BUILD_ENUM_VARIANT 校验逻辑，保证三后端一致。
        auto enumIt = enumRegistry_.find(enumName);
        if (enumIt == enumRegistry_.end()) {
            return runtimeError(ErrorFormat::format("未定义的 enum: %s", enumName.c_str()));
        }
        const VMEnumInfo& info = enumIt->second;
        const VMEnumVariantInfo* varInfo = nullptr;
        for (const auto& v : info.variants) {
            if (v.name == variantName) {
                varInfo = &v;
                break;
            }
        }
        if (varInfo == nullptr) {
            return runtimeError(
                ErrorFormat::format("enum '%s' 没有 variant '%s'", enumName.c_str(), variantName.c_str()));
        }
        if (static_cast<int>(argCount) != varInfo->arity) {
            return runtimeError(ErrorFormat::format("enum variant '%s.%s' 期望 %d 个参数，得到 %d 个", enumName.c_str(),
                                                    variantName.c_str(), varInfo->arity, static_cast<int>(argCount)));
        }

        std::vector<Value> fields;
        fields.reserve(argCount);
        for (uint8_t i = 0; i < argCount; ++i) {
            uint8_t argReg = chunk.code[ip + 7 + i];
            fields.push_back(reg(argReg));
        }
        reg(dst) = Value::makeEnumVariant(enumName, variantName, std::move(fields));
        ip += 7 + argCount;
        break;
    }
    default:
        return runtimeError("executeArrayBuildOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// executeArrayIndexOps - 索引访问指令（INDEX 类，跨 array/dict/string/tuple 多态）
// REG_INDEX_GET / REG_INDEX_SET
// ============================================================
VMResult RegisterVM::executeArrayIndexOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_INDEX_GET: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t objReg = chunk.code[ip + 2];
        uint8_t idxReg = chunk.code[ip + 3];
        const Value& obj = reg(objReg);
        const Value& idx = reg(idxReg);
        if (obj.isArray()) {
            if (!idx.isInt())
                return runtimeError(ErrorMessages::kArrayIndexMustBeInt);
            int64_t i = idx.intVal();
            const auto& arr = obj.arrayVal();
            // P1-6 fix: 对齐 Interpreter/栈式VM——负索引直接报错，不做 Python 式 wraparound
            if (i < 0 || i >= static_cast<int64_t>(arr.size())) {
                return runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                                                        static_cast<long long>(i), arr.size()));
            }
            reg(dst) = arr[static_cast<size_t>(i)];
        } else if (obj.isDict()) {
            // L4 fix: 字典键支持 string/int/bool/float
            auto dk = Value::dictKeyFromValue(idx);
            if (!dk)
                return runtimeError(ErrorMessages::kDictKeyInvalidType);
            const auto& dict = obj.dictVal();
            auto it = dict.find(*dk);
            // P1-7 fix: 对齐 Interpreter/栈式VM——字典键不存在时返回 null 而非 throw
            if (it == dict.end()) {
                reg(dst) = Value::nullValue();
            } else {
                reg(dst) = it->second;
            }
        } else if (obj.isString()) {
            if (!idx.isInt())
                return runtimeError(ErrorMessages::kStringIndexMustBeInt);
            int64_t i = idx.intVal();
            const auto& str = obj.stringVal();
            int64_t len = obj.codepointCount(); // perf1 fix: 带缓存的码位计数
            // P1-6 fix: 对齐 Interpreter/栈式VM——负索引直接报错
            if (i < 0 || i >= len) {
                return runtimeError(ErrorFormat::format("字符串索引越界: %lld, 有效范围 [0, %lld)",
                                                        static_cast<long long>(i), static_cast<long long>(len)));
            }
            size_t bytePos = Utf8::codepointToByteIndex(str, i);
            int charLen = Utf8::byteLength(static_cast<unsigned char>(str[bytePos]));
            reg(dst) = Value(str.substr(bytePos, static_cast<size_t>(charLen)));
        } else if (obj.isTuple()) {
            // R98 元组与解构：元组索引访问（immutable）
            if (!idx.isInt())
                return runtimeError("元组索引必须是整数");
            int64_t i = idx.intVal();
            const auto& tup = obj.tupleVal();
            if (i < 0 || i >= static_cast<int64_t>(tup.size())) {
                return runtimeError(ErrorFormat::format("元组索引越界: %lld, 有效范围 [0, %zu)",
                                                        static_cast<long long>(i), tup.size()));
            }
            reg(dst) = tup[static_cast<size_t>(i)];
        } else {
            return runtimeError(ErrorMessages::kTypeNotIndexable);
        }
        ip += 4;
        break;
    }
    case RegOp::REG_INDEX_SET: {
        uint8_t objReg = chunk.code[ip + 1];
        uint8_t idxReg = chunk.code[ip + 2];
        uint8_t valReg = chunk.code[ip + 3];
        Value& obj = reg(objReg);
        const Value& idx = reg(idxReg);
        const Value& val = reg(valReg);
        if (obj.isArray()) {
            if (!idx.isInt())
                return runtimeError(ErrorMessages::kArrayIndexMustBeInt);
            int64_t i = idx.intVal();
            auto& arr = obj.arrayVal();
            // P1-6 fix: 对齐 Interpreter/栈式VM——负索引直接报错
            if (i < 0 || i >= static_cast<int64_t>(arr.size())) {
                return runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                                                        static_cast<long long>(i), arr.size()));
            }
            arr[static_cast<size_t>(i)] = val;
        } else if (obj.isDict()) {
            // L4 fix: 字典键支持 string/int/bool/float
            auto dk = Value::dictKeyFromValue(idx);
            if (!dk)
                return runtimeError(ErrorMessages::kDictKeyInvalidType);
            obj.dictVal()[*dk] = val;
        } else {
            return runtimeError(ErrorMessages::kTypeNotIndexAssignable);
        }
        // C-1 fix: 记录接收者寄存器，供紧随其后的 WRITEBACK_* 读取变异后的容器
        lastMutatedReceiverReg_ = objReg;
        ip += 4;
        break;
    }
    // R134 模式匹配扩展：容器长度（与 StackVM OP_LEN 三后端一致）
    // 操作数: op(1B) + dst(1B) + src(1B)  语义: reg(dst) = len(reg(src))
    // 支持 array/dict/string/tuple（与 StackVM OP_LEN / IR LEN 同语义）。
    case RegOp::REG_LEN: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t src = chunk.code[ip + 2];
        const Value& obj = reg(src);
        int64_t len = 0;
        if (obj.isArray()) {
            len = static_cast<int64_t>(obj.arrayVal().size());
        } else if (obj.isDict()) {
            len = static_cast<int64_t>(obj.dictVal().size());
        } else if (obj.isString()) {
            len = obj.codepointCount(); // 码位计数（与 OP_LEN 一致）
        } else if (obj.isTuple()) {
            len = static_cast<int64_t>(obj.tupleVal().size());
        } else {
            return runtimeError("REG_LEN: 寄存器值不是容器类型（array/dict/string/tuple）");
        }
        reg(dst) = Value(len);
        ip += 3;
        break;
    }
    default:
        return runtimeError("executeArrayIndexOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// executeArrayEnumQueryOps - 枚举 variant 查询指令（ENUM_QUERY 类）
// REG_ENUM_VARIANT_NAME / REG_ENUM_VARIANT_FIELD
// ============================================================
VMResult RegisterVM::executeArrayEnumQueryOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    // R99 枚举与 ADT：检查 scrut 是否为指定 enum variant
    // 布局: [op, dst, scrut, eLo, eHi, vLo, vHi]
    case RegOp::REG_ENUM_VARIANT_NAME: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t scrutReg = chunk.code[ip + 2];
        uint16_t enumNameIdx = chunk.code[ip + 3] | (static_cast<uint16_t>(chunk.code[ip + 4]) << 8);
        uint16_t variantNameIdx = chunk.code[ip + 5] | (static_cast<uint16_t>(chunk.code[ip + 6]) << 8);
        if (enumNameIdx >= chunk.constants.size() || variantNameIdx >= chunk.constants.size()) {
            return runtimeError("REG_ENUM_VARIANT_NAME: 常量池索引越界");
        }
        const Value& scrut = reg(scrutReg);
        const std::string& enumName = chunk.constants[enumNameIdx].stringVal();
        const std::string& variantName = chunk.constants[variantNameIdx].stringVal();
        bool matched = false;
        if (scrut.isEnumVariant()) {
            matched = (scrut.enumVariantEnumName() == enumName) && (scrut.enumVariantName() == variantName);
        }
        reg(dst) = Value(matched);
        ip += 7;
        break;
    }
    // R99 枚举与 ADT：取 enum variant 字段
    // 布局: [op, dst, scrut, idx]
    case RegOp::REG_ENUM_VARIANT_FIELD: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t scrutReg = chunk.code[ip + 2];
        uint8_t idxReg = chunk.code[ip + 3];
        const Value& scrut = reg(scrutReg);
        const Value& idx = reg(idxReg);
        if (!scrut.isEnumVariant()) {
            return runtimeError("REG_ENUM_VARIANT_FIELD: 寄存器值不是 enum variant");
        }
        if (!idx.isInt()) {
            return runtimeError("REG_ENUM_VARIANT_FIELD: 索引必须是整数");
        }
        int64_t i = idx.intVal();
        const auto& fields = scrut.enumVariantFields();
        if (i < 0 || static_cast<size_t>(i) >= fields.size()) {
            return runtimeError(ErrorFormat::format("REG_ENUM_VARIANT_FIELD: 索引越界 %lld, 有效范围 [0, %zu)",
                                                    static_cast<long long>(i), fields.size()));
        }
        reg(dst) = fields[static_cast<size_t>(i)];
        ip += 4;
        break;
    }
    default:
        return runtimeError("executeArrayEnumQueryOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeDictOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_BUILD_DICT: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t pairCount = chunk.code[ip + 2];
        // L4 fix: 字典键支持 string/int/bool/float
        // R97 #2 fix: 改用 Value::DictMap（含 DictKeyEqual 透明比较器）
        Value::DictMap entries;
        for (uint8_t i = 0; i < pairCount; ++i) {
            // C-7 fix: 原代码漏加 ip 偏移，从 chunk 起始处读取操作数，
            // 导致非 offset-0 的字典字面量读取错误字节（越界/类型错误）。
            size_t base = ip + 3 + i * 2;
            uint8_t keyReg = chunk.code[base];
            uint8_t valReg = chunk.code[base + 1];
            const Value& keyVal = reg(keyReg);
            auto dk = Value::dictKeyFromValue(keyVal);
            if (!dk) {
                return runtimeError(ErrorMessages::kDictKeyInvalidType);
            }
            entries.emplace(std::move(*dk), reg(valReg));
        }
        reg(dst) = Value(std::move(entries));
        ip += 3 + pairCount * 2;
        break;
    }
    default:
        return runtimeError("executeDictOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeMemberOps(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case RegOp::REG_MEMBER_GET: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t objReg = chunk.code[ip + 2];
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (fieldIdx >= chunk.constants.size() || !chunk.constants[fieldIdx].isString()) {
            return runtimeError("字段名索引无效");
        }
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        const Value& obj = reg(objReg);
        if (obj.isInstance()) {
            const auto& fields = obj.fields();
            auto it = fields.find(fieldName);
            if (it != fields.end()) {
                reg(dst) = it->second;
            } else {
                // 方法回退：沿继承链查找方法（与 StackVM OP_MEMBER_GET 一致）
                std::string searchClass = obj.className();
                bool methodFound = false;
                for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
                    auto classIt = classInfo_.find(searchClass);
                    if (classIt == classInfo_.end())
                        break;
                    auto methodIt = classIt->second.methods.find(fieldName);
                    if (methodIt != classIt->second.methods.end()) {
                        reg(dst) = Value("method:" + obj.className() + "." + fieldName);
                        methodFound = true;
                        break;
                    }
                    searchClass = classIt->second.parent;
                }
                if (!methodFound) {
                    return runtimeError("类 " + obj.className() + " 没有字段或方法 '" + fieldName + "'");
                }
            }
        } else if (obj.isDict()) {
            const auto& dict = obj.dictVal();
            // L4 fix: 字典 member access 用 DictKey{string}
            auto it = dict.find(Value::DictKey{fieldName});
            // P1-7 fix: 对齐 Interpreter/栈式VM——字典键不存在时返回 null 而非 throw
            if (it == dict.end()) {
                reg(dst) = Value::nullValue();
            } else {
                reg(dst) = it->second;
            }
        } else {
            return runtimeError(ErrorMessages::kTypeNotMemberAccessible);
        }
        ip += 5;
        break;
    }
    case RegOp::REG_MEMBER_SET: {
        uint8_t objReg = chunk.code[ip + 1];
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t valReg = chunk.code[ip + 4];
        if (fieldIdx >= chunk.constants.size() || !chunk.constants[fieldIdx].isString()) {
            return runtimeError("字段名索引无效");
        }
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        Value& obj = reg(objReg);
        const Value& val = reg(valReg);
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
            frame.fieldsModified = true;
        } else if (obj.isDict()) {
            // L4 fix: 字典 member-set 用 DictKey{string}
            obj.dictVal()[Value::DictKey{fieldName}] = val;
        } else {
            return runtimeError(ErrorMessages::kTypeNotMemberAssignable);
        }
        // C-1 fix: 记录接收者寄存器，供紧随其后的 WRITEBACK_* 读取变异后的容器
        lastMutatedReceiverReg_ = objReg;
        ip += 5;
        break;
    }
    case RegOp::REG_INIT_FIELD: {
        uint16_t fieldIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (fieldIdx >= chunk.constants.size() || !chunk.constants[fieldIdx].isString()) {
            return runtimeError("字段名索引无效");
        }
        // P1-1 fix: fieldOrder 已在 REG_DEFINE_CLASS 中从类元数据完整填充（见 executeNewOps），
        // 此处无需重复记录。IR 层 visitClassDecl 不发射 INIT_FIELD（字段默认值由 init 方法
        // 通过 MEMBER_SET 设置），此 case 仅为防御性处理保留。
        ip += 3;
        break;
    }
    case RegOp::REG_SUPER_MEMBER_GET: {
        // P1-2 fix: super.field 语义上等同于 this.field —— 字段存储在实例上，
        // 父类 init 方法已通过 MEMBER_SET 将父类字段写入 this 实例。
        // 因此与 REG_MEMBER_GET 实现一致，从实例字段表查找即可。
        uint8_t dst = chunk.code[ip + 1];
        uint8_t objReg = chunk.code[ip + 2];
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (fieldIdx >= chunk.constants.size() || !chunk.constants[fieldIdx].isString()) {
            return runtimeError("字段名索引无效");
        }
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        const Value& obj = reg(objReg);
        if (obj.isInstance()) {
            const auto& fields = obj.fields();
            auto it = fields.find(fieldName);
            if (it != fields.end()) {
                reg(dst) = it->second;
            } else {
                // 方法回退：沿继承链查找方法（与 StackVM OP_SUPER_MEMBER_GET 一致）
                std::string searchClass = obj.className();
                bool methodFound = false;
                for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
                    auto classIt = classInfo_.find(searchClass);
                    if (classIt == classInfo_.end())
                        break;
                    auto methodIt = classIt->second.methods.find(fieldName);
                    if (methodIt != classIt->second.methods.end()) {
                        reg(dst) = Value("method:" + obj.className() + "." + fieldName);
                        methodFound = true;
                        break;
                    }
                    searchClass = classIt->second.parent;
                }
                if (!methodFound) {
                    return runtimeError("类 " + obj.className() + " 没有字段或方法 '" + fieldName + "'");
                }
            }
        } else {
            return runtimeError("super 成员访问需要实例类型");
        }
        ip += 5;
        break;
    }
    default:
        return runtimeError("executeMemberOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeContainers(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;
    (void)chunk; // chunk 在子函数中独立获取；主函数仅 dispatch

    switch (op) {
    case RegOp::REG_BUILD_ARRAY:
    case RegOp::REG_BUILD_TUPLE:
    case RegOp::REG_INDEX_GET:
    case RegOp::REG_INDEX_SET:
        // 数组/元组构建与索引访问（INDEX_GET/SET 多态：同时处理 array/dict/string/tuple）
        return executeArrayOps(op, ip);
    case RegOp::REG_BUILD_ENUM_VARIANT: // R99 枚举与 ADT
    case RegOp::REG_ENUM_VARIANT_NAME:
    case RegOp::REG_ENUM_VARIANT_FIELD:
    case RegOp::REG_LEN: // R134 模式匹配扩展：容器长度（与 INDEX_GET 同属索引访问语义族）
        // enum variant 构造/检查/取字段（与数组 ops 同属容器指令子集）
        return executeArrayOps(op, ip);
    case RegOp::REG_BUILD_DICT:
        // 字典字面量构建
        return executeDictOps(op, ip);
    case RegOp::REG_MEMBER_GET:
    case RegOp::REG_MEMBER_SET:
    case RegOp::REG_INIT_FIELD:
    case RegOp::REG_SUPER_MEMBER_GET:
        // 成员访问/赋值/字段初始化/super 成员访问
        return executeMemberOps(op, ip);
    default:
        return runtimeError("executeContainers: 未知操作码");
    }
}

// ============================================================
// 调用指令（占位实现，需完整实现）
// ============================================================

VMResult RegisterVM::executeCallOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_CALL: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t nameIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t argCount = chunk.code[ip + 4];
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("函数名索引无效");
        }
        const std::string& funName = chunk.constants[nameIdx].stringVal();
        SmallArgs<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 5 + i]);
        }
        size_t newIp = ip; // 保存当前 ip（调用返回后更新）
        // C-8 fix: REG_CALL 指令长度 = 5 + argCount
        VMResult r = executeCallImpl(newIp, funName, argCount, dst, argRegs, 5u + argCount);
        if (r != VMResult::VM_OK)
            return r;
        // 调用未返回（同步调用）：ip 不变，由新帧接管执行
        ip = newIp;
        break;
    }
    case RegOp::REG_CALL_EXPR: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t calleeReg = chunk.code[ip + 2];
        uint8_t argCount = chunk.code[ip + 3];
        const Value& callee = reg(calleeReg);
        if (!callee.isClosure()) {
            return runtimeError("调用非闭包值");
        }
        SmallArgs<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 4 + i]);
        }
        // C-2 fix: 传入闭包值，executeCallImpl 从其 vmClosure 提取 upvalues 填入新帧
        const std::string& funName = callee.closureName();
        size_t newIp = ip;
        // C-8 fix: REG_CALL_EXPR 指令长度 = 4 + argCount（原硬编码 5+argCount 偏移 +1）
        VMResult r = executeCallImpl(newIp, funName, argCount, dst, argRegs, 4u + argCount, &callee);
        if (r != VMResult::VM_OK)
            return r;
        ip = newIp;
        break;
    }
    case RegOp::REG_MAKE_CLOSURE: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t nameIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t uvCount = chunk.code[ip + 4];
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("闭包名索引无效");
        }
        const std::string& name = chunk.constants[nameIdx].stringVal();
        SmallArgs<uint8_t> uvSpecs;
        for (uint8_t i = 0; i < uvCount; ++i) {
            uvSpecs.push_back(chunk.code[ip + 5 + i * 2]);     // isLocal
            uvSpecs.push_back(chunk.code[ip + 5 + i * 2 + 1]); // idx
        }
        size_t newIp = ip;
        VMResult r = executeClosureImpl(newIp, name, uvCount, uvSpecs, dst);
        if (r != VMResult::VM_OK)
            return r;
        ip = newIp;
        break;
    }
    default:
        return runtimeError("executeCallOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeMethodCallOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_METHOD_CALL: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t objReg = chunk.code[ip + 2];
        uint16_t methodIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        uint8_t argCount = chunk.code[ip + 5];
        if (methodIdx >= chunk.constants.size() || !chunk.constants[methodIdx].isString()) {
            return runtimeError("方法名索引无效");
        }
        const std::string& methodName = chunk.constants[methodIdx].stringVal();
        SmallArgs<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 6 + i]);
        }
        size_t newIp = ip;
        VMResult r = executeMethodCallImpl(newIp, methodName, argCount, dst, objReg, argRegs);
        if (r != VMResult::VM_OK)
            return r;
        ip = newIp;
        break;
    }
    default:
        return runtimeError("executeMethodCallOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeNewOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_CLASS_NEW: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t nameIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t argCount = chunk.code[ip + 4];
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("类名索引无效");
        }
        const std::string& className = chunk.constants[nameIdx].stringVal();
        SmallArgs<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 5 + i]);
        }
        size_t newIp = ip;
        VMResult r = executeClassNewImpl(newIp, className, argCount, dst, argRegs);
        if (r != VMResult::VM_OK)
            return r;
        ip = newIp;
        break;
    }
    case RegOp::REG_DEFINE_CLASS: {
        // C-9 fix: 完整填充 classInfo_ 的 name/parent/fieldOrder/methods。
        // 原实现仅设置 .name，导致方法调用/构造全部失败。
        // BUG-INH-1 fix: 新增字段默认值常量索引
        // BUG-INH-IR-1 fix: 新增字段表达式寄存器（非字面量默认值从寄存器读取）
        // 编码：op + nameIdx(2B) + parentIdx(2B) + fieldCount(1B)
        //      + [fieldIdx(2B) + defaultConstIdx(2B) + exprReg(1B)]×F
        //      + methodCount(1B) + [methodIdx(2B)+funIdx(2B)]×M
        // exprReg: 0xFF=使用常量/null, 否则从 reg(exprReg) 读取运行时求值结果
        uint16_t nameIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t parentIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("类名索引无效");
        }
        const std::string& className = chunk.constants[nameIdx].stringVal();

        auto& info = classInfo_[className];
        info.name = className;
        info.fieldOrder.clear();
        info.methods.clear();
        info.fieldDefaults.clear();     // BUG-INH-1 fix
        info.flattenedComputed = false; // perf2 fix: 重定义时使预计算缓存失效

        // BUG-INH-AUDIT-7 fix: 父类重定义时，所有依赖此父类的子类缓存失效。
        // 遍历所有已注册类，若其继承链包含当前重定义的类，置 flattenedComputed=false
        // 强制下次访问时重新计算 flattenedFieldOrder/flattenedFieldDefaults/hasInit。
        // R133: 同步清空 methodCache（per-class 方法分发内联缓存），与 StackVM
        // BUG-INH-AUDIT-7 fix (VMCalls.cpp:1222-1240) 对齐。父类方法集合可能变化,
        // 子类缓存的 methodName→funName 映射可能失效（如父类新增/删除/重命名方法）。
        for (auto& kv : classInfo_) {
            if (kv.first == className)
                continue; // 跳过当前类
            std::string cur = kv.second.parent;
            for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
                if (cur == className) {
                    kv.second.flattenedComputed = false;
                    kv.second.methodCache.clear(); // R133: 失效方法分发缓存
                    break;
                }
                auto it = classInfo_.find(cur);
                if (it == classInfo_.end())
                    break;
                cur = it->second.parent;
            }
        }

        if (parentIdx != 0xFFFF) {
            if (parentIdx >= chunk.constants.size() || !chunk.constants[parentIdx].isString()) {
                return runtimeError("父类名索引无效");
            }
            info.parent = chunk.constants[parentIdx].stringVal();
            // BUG-INH-AUDIT-8 fix: 父类存在性检查。StackVM 的 executeDefineClass
            // （VMCalls.cpp）在定义时立即检查父类是否已注册，RegisterVM 原实现静默通过，
            // 延迟到构造时才报错（或永不报错），三后端错误检测时机不一致。
            if (classInfo_.find(info.parent) == classInfo_.end()) {
                return runtimeError("未定义的父类: " + info.parent);
            }
            // BUG-INH-AUDIT-9 fix: 循环继承检测。StackVM 在定义时沿继承链构建 chain，
            // guard 耗尽后 cur 仍非空则报循环。RegisterVM 原实现延迟到 lazy flattened
            // 计算时检测，若循环类从未构造则永不报错。此处对齐 StackVM 在定义时检测。
            std::string cur = info.parent;
            for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
                if (cur == className) {
                    return runtimeError("类继承链过深或存在循环继承: " + className + " -> " + cur);
                }
                auto it = classInfo_.find(cur);
                if (it == classInfo_.end())
                    break;
                cur = it->second.parent;
            }
        } else {
            info.parent.clear();
        }

        size_t cursor = ip + 5;
        if (cursor >= chunk.code.size()) {
            return runtimeError("DEFINE_CLASS: 字段计数截断");
        }
        uint8_t fieldCount = chunk.code[cursor];
        cursor += 1;
        for (uint8_t i = 0; i < fieldCount; ++i) {
            if (cursor + 4 >= chunk.code.size()) {
                return runtimeError("DEFINE_CLASS: 字段名/默认值/表达式寄存器截断");
            }
            uint16_t fIdx = chunk.code[cursor] | (chunk.code[cursor + 1] << 8);
            uint16_t defaultIdx = chunk.code[cursor + 2] | (chunk.code[cursor + 3] << 8);
            uint8_t exprReg = chunk.code[cursor + 4];
            if (fIdx >= chunk.constants.size() || !chunk.constants[fIdx].isString()) {
                return runtimeError("字段名索引无效");
            }
            info.fieldOrder.push_back(chunk.constants[fIdx].stringVal());
            // BUG-INH-IR-1 fix: 非字面量表达式从寄存器读取运行时求值结果
            if (exprReg != 0xFF) {
                info.fieldDefaults.push_back(reg(exprReg));
            } else if (defaultIdx == 0xFFFF) {
                info.fieldDefaults.push_back(Value::nullValue());
            } else {
                if (defaultIdx >= chunk.constants.size()) {
                    return runtimeError("字段默认值索引无效");
                }
                info.fieldDefaults.push_back(chunk.constants[defaultIdx]);
            }
            cursor += 5; // fieldIdx(2B) + defaultIdx(2B) + exprReg(1B)
        }

        if (cursor >= chunk.code.size()) {
            return runtimeError("DEFINE_CLASS: 方法计数截断");
        }
        uint8_t methodCount = chunk.code[cursor];
        cursor += 1;
        for (uint8_t i = 0; i < methodCount; ++i) {
            if (cursor + 3 >= chunk.code.size()) {
                return runtimeError("DEFINE_CLASS: 方法/函数名索引截断");
            }
            uint16_t mIdx = chunk.code[cursor] | (chunk.code[cursor + 1] << 8);
            uint16_t fIdx = chunk.code[cursor + 2] | (chunk.code[cursor + 3] << 8);
            if (mIdx >= chunk.constants.size() || !chunk.constants[mIdx].isString() || fIdx >= chunk.constants.size() ||
                !chunk.constants[fIdx].isString()) {
                return runtimeError("方法/函数名索引无效");
            }
            info.methods[chunk.constants[mIdx].stringVal()] = chunk.constants[fIdx].stringVal();
            cursor += 4;
        }

        ip = cursor;
        break;
    }
    default:
        return runtimeError("executeNewOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeCalls(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_RETURN: {
        uint8_t src = chunk.code[ip + 1];
        Value result = reg(src);
        // BUG-REGVM-2 fix: executeReturnImpl 成功返回时帧已弹出、ip 已更新到调用者，
        // 需调用 stepCallback_ 反映调用者帧状态，否则调试器单步丢失步进事件。
        VMResult r = executeReturnImpl(ip, std::move(result));
        if (r == VMResult::VM_OK && stepCallbackEnabled_ && stepCallback_) {
            stepCallback_({ip, op, frames_.size()});
        }
        return r;
    }
    case RegOp::REG_RETURN_NULL: {
        VMResult r = executeReturnImpl(ip, Value::nullValue());
        if (r == VMResult::VM_OK && stepCallbackEnabled_ && stepCallback_) {
            stepCallback_({ip, op, frames_.size()});
        }
        return r;
    }
    case RegOp::REG_CALL:
    case RegOp::REG_CALL_EXPR:
    case RegOp::REG_MAKE_CLOSURE:
        // 普通函数调用 / 闭包值调用 / 闭包构造
        return executeCallOps(op, ip);
    case RegOp::REG_METHOD_CALL:
        // 实例方法调用（含内置方法分发）
        return executeMethodCallOps(op, ip);
    case RegOp::REG_CLASS_NEW:
    case RegOp::REG_DEFINE_CLASS:
        // 类构造 / 类定义
        return executeNewOps(op, ip);
    default:
        return runtimeError("executeCalls: 未知操作码");
    }
}

// ============================================================
// 其他指令
// ============================================================

VMResult RegisterVM::executeTryThrowOps(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case RegOp::REG_TRY_BEGIN: {
        uint16_t catchOffset = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        // R7 fix: catchOffset 是绝对字节偏移，越界时 throwException 会跳到错误地址静默执行。
        // 与栈式 VM OP_TRY_BEGIN 对齐，入栈前校验边界，防止字节码损坏导致静默错误行为。
        if (catchOffset >= chunk.code.size()) {
            return runtimeError("REG_TRY_BEGIN: catch 目标越界");
        }
        // BUG-EXC-5 fix: 记录 try 块开始时的寄存器数，throwException 命中 handler 时
        // 关闭 [registerBase, registerCount) 范围的 open upvalues。
        uint8_t regBase = currentFrame().registerCount;
        tryStack_.push_back({catchOffset, frames_.size() - 1, regBase});
        ip += 3;
        break;
    }
    case RegOp::REG_TRY_END: {
        if (!tryStack_.empty() && tryStack_.back().frameIndex == frames_.size() - 1) {
            tryStack_.pop_back();
        }
        ip += 1;
        break;
    }
    case RegOp::REG_THROW: {
        uint8_t src = chunk.code[ip + 1];
        Value thrown = reg(src);
        ip += 2;
        // BUG-REGVM-2 fix: throwException 成功跳转到 catch 块时需调用 stepCallback_，
        // 反映 catch 块状态；未捕获异常返回 VM_RUNTIME_ERROR 时无需调用。
        VMResult r = throwException(std::move(thrown));
        if (r == VMResult::VM_OK && stepCallbackEnabled_ && stepCallback_) {
            stepCallback_({ip, op, frames_.size()});
        }
        return r;
    }
    case RegOp::REG_LOAD_EXCEPTION: {
        // P1-4 fix: catch 块起始加载 pendingException_ 到目标寄存器
        uint8_t dst = chunk.code[ip + 1];
        reg(dst) = pendingException_;
        ip += 2;
        break;
    }
    case RegOp::REG_PUSH_JUMP_TARGET: {
        // AUDIT-P1.1 fix: push 跳转目标到 pendingJumpStack_（与 StackVM 对齐）。
        uint16_t target = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (target >= chunk.code.size())
            return runtimeError("REG_PUSH_JUMP_TARGET: 跳转目标越界");
        pendingJumpStack_.push_back(target);
        ip += 3;
        break;
    }
    case RegOp::REG_FINALLY_END: {
        // AUDIT-P1.1 fix: finally 块正常路径末尾（与 StackVM 对齐）。
        if (!pendingJumpStack_.empty()) {
            size_t target = pendingJumpStack_.back();
            pendingJumpStack_.pop_back();
            if (target >= chunk.code.size())
                return runtimeError("REG_FINALLY_END: 跳转目标越界");
            ip = target;
        } else {
            ip += 1;
        }
        break;
    }
    default:
        return runtimeError("executeTryThrowOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeWritebackOps(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case RegOp::REG_WRITEBACK_INDEX_LOCAL: {
        // C-1 fix: 将 lastMutatedReceiverReg_ 指向的变异后容器写回局部变量槽
        uint8_t slot = chunk.code[ip + 1];
        if (slot >= frame.registerCount) {
            return runtimeError("WRITEBACK_INDEX_LOCAL: 局部槽越界");
        }
        frame.registers[slot] = reg(lastMutatedReceiverReg_);
        ip += 2;
        break;
    }
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL: {
        // C-1 fix: 字段索引仅用于反汇编，运行时整体替换（与 INDEX 版本同语义）
        uint8_t slot = chunk.code[ip + 1];
        if (slot >= frame.registerCount) {
            return runtimeError("WRITEBACK_MEMBER_LOCAL: 局部槽越界");
        }
        frame.registers[slot] = reg(lastMutatedReceiverReg_);
        ip += 4;
        break;
    }
    case RegOp::REG_WRITEBACK_INDEX_VAR: {
        // C-1 fix: 写回全局变量（slot < 0x8000 → globalSlots_，否则 globals_[name]）
        uint16_t slotOrName = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        const Value& mutated = reg(lastMutatedReceiverReg_);
        if (slotOrName & 0x8000) {
            uint16_t nameIdx = slotOrName & 0x7FFF;
            if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                return runtimeError("WRITEBACK_INDEX_VAR: 名称索引无效");
            }
            const std::string& name = chunk.constants[nameIdx].stringVal();
            auto gsIt = globalNameToSlot_.find(name);
            if (gsIt != globalNameToSlot_.end() && gsIt->second < static_cast<int>(globalSlots_.size())) {
                globalSlots_[gsIt->second] = mutated;
            } else {
                globals_[name] = mutated;
            }
        } else {
            if (slotOrName >= globalSlots_.size()) {
                return runtimeError("WRITEBACK_INDEX_VAR: 全局槽越界");
            }
            globalSlots_[slotOrName] = mutated;
        }
        ip += 3;
        break;
    }
    case RegOp::REG_WRITEBACK_MEMBER_VAR: {
        // C-1 fix: 字段索引仅用于反汇编，运行时整体替换
        uint16_t slotOrName = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        const Value& mutated = reg(lastMutatedReceiverReg_);
        if (slotOrName & 0x8000) {
            uint16_t nameIdx = slotOrName & 0x7FFF;
            if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                return runtimeError("WRITEBACK_MEMBER_VAR: 名称索引无效");
            }
            const std::string& name = chunk.constants[nameIdx].stringVal();
            auto gsIt = globalNameToSlot_.find(name);
            if (gsIt != globalNameToSlot_.end() && gsIt->second < static_cast<int>(globalSlots_.size())) {
                globalSlots_[gsIt->second] = mutated;
            } else {
                globals_[name] = mutated;
            }
        } else {
            if (slotOrName >= globalSlots_.size()) {
                return runtimeError("WRITEBACK_MEMBER_VAR: 全局槽越界");
            }
            globalSlots_[slotOrName] = mutated;
        }
        ip += 5;
        break;
    }
    case RegOp::REG_WRITEBACK_INDEX_UPVALUE: {
        // #7/C-1 fix: 整体替换 upvalue 槽位（不 pop 索引，不查字段）
        uint8_t uvIdx = chunk.code[ip + 1];
        if (uvIdx >= frame.upvalues.size()) {
            return runtimeError("WRITEBACK_INDEX_UPVALUE: upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        const Value& mutated = reg(lastMutatedReceiverReg_);
        if (uv->isClosed) {
            uv->value = mutated;
        } else {
            // C-3 fix: 通过编码 frameIdx 定位真实帧（原硬编码 size-2 在多层嵌套下错位）
            Value* slot = resolveOpenUpvalueSlot(*uv);
            if (!slot)
                return VMResult::VM_RUNTIME_ERROR;
            *slot = mutated;
            // BUG-UPVAL-2 fix: 对齐 StackVM OP_WRITEBACK_INDEX_UPVALUE 的 P0-3 fix——
            // 若修改的是外层方法帧的 registers[0]（this 槽），标记 fieldsModified，
            // 确保 executeReturnImpl 同步 methodThis 到 caller 的 receiverReg。
            size_t frameIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slotIdx = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            if (slotIdx == 0 && frameIdx < frames_.size() && frames_[frameIdx].isMethodCall) {
                frames_[frameIdx].fieldsModified = true;
            }
        }
        ip += 2;
        break;
    }
    case RegOp::REG_WRITEBACK_MEMBER_UPVALUE: {
        // #7/C-1 fix: 整体替换 upvalue 槽位（字段索引仅用于反汇编）
        uint8_t uvIdx = chunk.code[ip + 1];
        if (uvIdx >= frame.upvalues.size()) {
            return runtimeError("WRITEBACK_MEMBER_UPVALUE: upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        const Value& mutated = reg(lastMutatedReceiverReg_);
        if (uv->isClosed) {
            uv->value = mutated;
        } else {
            // C-3 fix: 通过编码 frameIdx 定位真实帧
            Value* slot = resolveOpenUpvalueSlot(*uv);
            if (!slot)
                return VMResult::VM_RUNTIME_ERROR;
            *slot = mutated;
            // BUG-UPVAL-2 fix: 对齐 StackVM OP_WRITEBACK_MEMBER_UPVALUE 的 P0-3 fix——
            // 若修改的是外层方法帧的 registers[0]（this 槽），标记 fieldsModified，
            // 确保 executeReturnImpl 同步 methodThis 到 caller 的 receiverReg。
            size_t frameIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slotIdx = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            if (slotIdx == 0 && frameIdx < frames_.size() && frames_[frameIdx].isMethodCall) {
                frames_[frameIdx].fieldsModified = true;
            }
        }
        ip += 4;
        break;
    }
    default:
        return runtimeError("executeWritebackOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeTypeCheckOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_LOAD_MUTATED: {
        // MEDIUM-1/2 fix: 读取 lastMutatedReceiverReg_ 到目标寄存器（不清除）。
        // 嵌套左值写回链中，上一级 MEMBER_SET/INDEX_SET 将变异后容器所在寄存器
        // 记录到 lastMutatedReceiverReg_，此处读取供下一级 SET 作为 val 使用。
        uint8_t dst = chunk.code[ip + 1];
        reg(dst) = reg(lastMutatedReceiverReg_);
        ip += 2;
        break;
    }
    // 2026-06-29: 运行时类型注解检查（三后端统一强制）
    case RegOp::REG_TYPE_CHECK: {
        uint8_t src = chunk.code[ip + 1];
        uint16_t typeIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (typeIdx >= chunk.constants.size()) {
            return runtimeError("REG_TYPE_CHECK: 类型注解常量索引越界");
        }
        const std::string& annotation = chunk.constants[typeIdx].stringVal();
        const Value& val = reg(src);
        if (!minilang::typeMatchValue(val, annotation)) {
            // 实例继承链检查
            if (val.isInstance() && !annotation.empty()) {
                auto classIt = classInfo_.find(val.className());
                int depth = 0;
                bool found = false;
                while (classIt != classInfo_.end() && depth < 64) {
                    if (classIt->second.name == annotation) {
                        found = true;
                        break;
                    }
                    if (classIt->second.parent.empty())
                        break;
                    classIt = classInfo_.find(classIt->second.parent);
                    ++depth;
                }
                if (found) {
                    ip += 4;
                    break;
                }
            }
            return runtimeError(ErrorFormat::format(ErrorMessages::kTypeAnnotationViolationFmt, annotation.c_str(),
                                                    val.typeName().c_str()));
        }
        ip += 4; // op(1B) + src(1B) + typeIdx(2B)
        break;
    }
    // R134: 软类型测试（与 REG_TYPE_CHECK 区别：不抛错，写 bool 到 dst）
    // 编码: op(1B) + dst(1B) + src(1B) + typeIdx(2B) = 5 字节
    // 语义: 匹配则 dst=true，不匹配则 dst=false（含实例继承链 fallback）
    case RegOp::REG_TYPE_TEST: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t src = chunk.code[ip + 2];
        uint16_t typeIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (typeIdx >= chunk.constants.size()) {
            return runtimeError("REG_TYPE_TEST: 类型注解常量索引越界");
        }
        const std::string& annotation = chunk.constants[typeIdx].stringVal();
        const Value& val = reg(src);
        bool matched = minilang::typeMatchValue(val, annotation);
        // 实例继承链 fallback（与 REG_TYPE_CHECK 一致）
        if (!matched && val.isInstance() && !annotation.empty()) {
            auto classIt = classInfo_.find(val.className());
            int depth = 0;
            while (classIt != classInfo_.end() && depth < 64) {
                if (classIt->second.name == annotation) {
                    matched = true;
                    break;
                }
                if (classIt->second.parent.empty())
                    break;
                classIt = classInfo_.find(classIt->second.parent);
                ++depth;
            }
        }
        reg(dst) = Value(matched);
        ip += 5; // op(1B) + dst(1B) + src(1B) + typeIdx(2B)
        break;
    }
    default:
        return runtimeError("executeTypeCheckOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeSuperCallOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_SUPER_CALL: {
        // P1-3 fix: super.method(args) 实现
        // 编码: op + dst(1B) + nameIdx(2B) + argCount(1B) + recvReg(1B) + classIdx(2B) + args...
        uint8_t dst = chunk.code[ip + 1];
        uint16_t nameIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t argCount = chunk.code[ip + 4];
        uint8_t recvReg = chunk.code[ip + 5];
        uint16_t classIdx = chunk.code[ip + 6] | (chunk.code[ip + 7] << 8);

        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("super 调用方法名索引无效");
        }
        if (classIdx >= chunk.constants.size() || !chunk.constants[classIdx].isString()) {
            return runtimeError("super 调用类名索引无效");
        }
        const std::string& methodName = chunk.constants[nameIdx].stringVal();
        const std::string& curClassName = chunk.constants[classIdx].stringVal();

        // 查找当前类的父类
        auto curIt = classInfo_.find(curClassName);
        if (curIt == classInfo_.end() || curIt->second.parent.empty()) {
            return runtimeError("类 " + curClassName + " 没有父类，不能使用 super");
        }
        // 从父类开始沿继承链查找方法
        std::string searchClass = curIt->second.parent;
        // PERF-AUDIT-5: 用 const std::string* 直接指向 classInfo_ map 中的稳定字符串，
        // 避免 foundFunName 局部副本导致 callCache_ 键指针悬垂。
        const std::string* foundFunNamePtr = nullptr;
        bool found = false;
        for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
            auto clsIt = classInfo_.find(searchClass);
            if (clsIt == classInfo_.end())
                break;
            auto methodIt = clsIt->second.methods.find(methodName);
            if (methodIt != clsIt->second.methods.end()) {
                foundFunNamePtr = &methodIt->second;
                found = true;
                break;
            }
            searchClass = clsIt->second.parent;
        }
        if (!found) {
            // BUG-INH-3 fix: 错误消息与 Interpreter/StackVM 一致（"类 X 没有方法 Y"）
            return runtimeError("类 " + curClassName + " 没有方法 " + methodName);
        }

        // 构造参数列表（this/recvReg 作为第一个参数）
        SmallArgs<uint8_t> fullArgRegs;
        fullArgRegs.push_back(recvReg);
        for (uint8_t i = 0; i < argCount; ++i) {
            fullArgRegs.push_back(chunk.code[ip + 8 + i]);
        }
        size_t newIp = ip;
        // REG_SUPER_CALL 指令长度 = 8 + argCount
        // 防御性检查：argCount+1 溢出 uint8_t（backend 应已在编译期拦截）
        if (argCount >= 255)
            return runtimeError("super 调用参数数量超过上限");
        VMResult cr =
            executeCallImpl(newIp, *foundFunNamePtr, argCount + 1, dst, fullArgRegs, 8u + argCount, nullptr, true);
        if (cr != VMResult::VM_OK)
            return cr;
        // 标记为方法调用（用于字段同步）
        // P0-1/P1-5 fix: 对齐栈式 VM (VMCalls.cpp:734)——super.init() 调用也需设置 isInitCall，
        // 使 executeReturnImpl 在多层 super.init() 链中即使 fieldsModified=false 也捕获 methodThis 并同步。
        if (!frames_.empty()) {
            frames_.back().isMethodCall = true;
            frames_.back().isInitCall = (methodName == "init");
            frames_.back().receiverReg = recvReg;
            frames_.back().receiverVarName.clear();
        }
        ip = newIp;
        break;
    }
    default:
        return runtimeError("executeSuperCallOps: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeMisc(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_PRINT: {
        uint8_t src = chunk.code[ip + 1];
        const Value& v = reg(src);
        if (outputCallback_) {
            outputCallback_(v.toString());
        }
        ip += 2;
        break;
    }
    case RegOp::REG_TRY_BEGIN:
    case RegOp::REG_TRY_END:
    case RegOp::REG_THROW:
    case RegOp::REG_LOAD_EXCEPTION:
    case RegOp::REG_PUSH_JUMP_TARGET:
    case RegOp::REG_FINALLY_END:
        // 异常处理类：子函数自包含 stepCallback_ 调用。
        // 注意 REG_THROW 在子函数内部直接 return r（已调用 stepCallback_），其他 case 走到子函数末尾。
        return executeTryThrowOps(op, ip);
    case RegOp::REG_WRITEBACK_INDEX_LOCAL:
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL:
    case RegOp::REG_WRITEBACK_INDEX_VAR:
    case RegOp::REG_WRITEBACK_MEMBER_VAR:
    case RegOp::REG_WRITEBACK_INDEX_UPVALUE:
    case RegOp::REG_WRITEBACK_MEMBER_UPVALUE:
        // 变异后容器写回（局部槽/全局变量/upvalue）
        return executeWritebackOps(op, ip);
    case RegOp::REG_TYPE_CHECK:
    case RegOp::REG_TYPE_TEST: // R134: 软类型测试（与 REG_TYPE_CHECK 同属 typeCheckOps）
    case RegOp::REG_LOAD_MUTATED:
        // 运行时类型注解检查 + 变异容器寄存器读取
        return executeTypeCheckOps(op, ip);
    case RegOp::REG_SUPER_CALL:
        // super.method(args) 调用
        return executeSuperCallOps(op, ip);
    default:
        return runtimeError("executeMisc: 未知操作码");
    }

    // 仅 REG_PRINT 走到这里（其他 case 均已 return 到子函数）
    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 函数调用实现
// ============================================================

VMResult RegisterVM::executeCallImpl(size_t& ip, const std::string& funName, uint8_t argCount, uint8_t dstReg,
                                     const SmallArgs<uint8_t>& argRegs, size_t returnOffset, const Value* closureValue,
                                     bool isMethodCall) {
    if (frames_.size() >= MAX_FRAMES) {
        // R97 #11 fix: 三后端递归深度消息统一
        return runtimeError(
            ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt, static_cast<int>(MAX_FRAMES)));
    }

    // C-2 fix: 若调用者通过 REG_CALL_EXPR 传入闭包值，优先使用其绑定的函数名查找 chunk，
    // 并在创建新帧时从 vmClosure 提取 upvalues 填入 newFrame.upvalues。
    // 若 closureValue 为空（REG_CALL 命名调用），回退到原查找路径。
    const std::shared_ptr<VMClosureData>& closureData =
        (closureValue && closureValue->isClosure()) ? closureValue->vmClosure() : nullptr;

    // PERF-AUDIT-5 fix: 内联缓存快速路径。仅对 REG_CALL（closureValue==nullptr）缓存，
    // 因 funName 来自常量池（稳定指针）；REG_CALL_EXPR 的 funName 来自闭包对象
    // （callee.closureName()），闭包销毁后指针悬垂，不可缓存。与 StackVM callCache_
    // 只在 OP_CALL 路径缓存、OP_CALL_EXPR 不缓存的模式一致。
    // R133 fix: 方法调用路径（isMethodCall=true）也不缓存,因 funName 来自 methods map/
    // classInfo_/局部变量,不是常量池稳定指针。用 &funName 作 callCache_ key 时,
    // 栈帧复用会导致错误命中（IC10 测试用例的 bug 根因）。
    const RegBytecodeChunk* cachedChunk = nullptr;
    const std::string* namePtr = &funName;
    if (!closureValue && !isMethodCall) {
        auto ccIt = callCache_.find(namePtr);
        if (ccIt != callCache_.end()) {
            cachedChunk = ccIt->second;
        }
    }

    // 查找函数 chunk
    auto it = cachedChunk ? functionChunks_.end() // 缓存命中，跳过 O(log n) 查找
                          : functionChunks_.find(funName);

    if (!cachedChunk && it == functionChunks_.end()) {
        // BUG-REGVM-3 fix: 删除 functionClosures_ 死代码路径。该字段无任何写入点
        // （仅 resetState clear、此处 find），是死代码。若被激活，REG_CALL 命中此路径
        // 时 closureData 为 null，populateUpvalues 不填充任何 upvalue，方法体内
        // REG_LOAD_UPVALUE/REG_STORE_UPVALUE 报"upvalue 索引越界"。
        // 闭包调用通过 REG_CALL_EXPR + MAKE_CLOSURE 正确处理（closureValue 非空）。
        {
            // 收集参数（寄存器顺序: argRegs[0..argCount-1]）
            SmallArgs<Value> args;
            for (uint8_t i = 0; i < argCount; ++i) {
                args.push_back(reg(argRegs[i]));
            }
            // AUDIT-BUG-F9 fix: 传入实际源码行号，对齐 StackVM 路径（VMCalls.cpp:380-386）。
            const RegBytecodeChunk& curChunk = *currentFrame().chunk;
            int line = (ip < curChunk.lines.size()) ? curChunk.lines[ip] : 0;

            // BUG-IBACKEND-1 fix: input() 函数特殊处理（镜像 VMCalls.cpp:343-367）。
            // 原实现 inputCallback_ 字段仅在 setInputCallback 赋值，无任何调用点（死代码），
            // 导致 RegisterVM 路径下 input() 报"未定义的函数: input"。
            if (funName == "input") {
                auto r = executeSharedInput(inputCallback_, args.begin(), argCount, line, 0);
                if (r.is_err()) {
                    return runtimeError(r.error().message);
                }
                reg(dstReg) = std::move(r.value());
                ip += returnOffset;
                return VMResult::VM_OK;
            }

            // R98 W2: 高阶函数 map/filter/reduce/forEach/find 优先拦截
            // （在 executeSharedBuiltinFunction 之前，因为这 5 个名字不在其注册表中，
            // 且需要通过 invokeClosureSync 调用用户传入的闭包值）。
            // 镜像 VMCalls.cpp:419-482 的 StackVM 拦截模式。
            if (isHigherOrderBuiltin(funName)) {
                // 构造闭包调用回调——dstReg 作为返回值中转寄存器（调用者已分配，
                // 闭包执行期间 caller 帧不执行指令，无副作用）
                ClosureInvoker invoke = [this, dstReg, line](const Value& closure, const Value* a, size_t ac,
                                                             int /*ln*/, int /*col*/) -> Result<Value> {
                    Value res;
                    VMResult r = invokeClosureSync(closure, a, ac, dstReg, line, 0, res);
                    if (r != VMResult::VM_OK) {
                        return Result<Value>::err(lastError_, line, 0);
                    }
                    return Result<Value>::ok(std::move(res));
                };
                // 按函数名分派到共享算法层
                Result<Value> hoResult = Result<Value>::ok(Value::nullValue());
                if (funName == "map") {
                    if (argCount != 2) {
                        return runtimeError(ErrorFormat::format("map 期望 2 个参数，但传入了 %d 个", argCount));
                    }
                    hoResult = executeSharedMap(args[0], args[1], invoke, line, 0);
                } else if (funName == "filter") {
                    if (argCount != 2) {
                        return runtimeError(ErrorFormat::format("filter 期望 2 个参数，但传入了 %d 个", argCount));
                    }
                    hoResult = executeSharedFilter(args[0], args[1], invoke, line, 0);
                } else if (funName == "reduce") {
                    if (argCount != 3) {
                        return runtimeError(ErrorFormat::format("reduce 期望 3 个参数，但传入了 %d 个", argCount));
                    }
                    hoResult = executeSharedReduce(args[0], args[1], args[2], invoke, line, 0);
                } else if (funName == "forEach") {
                    if (argCount != 2) {
                        return runtimeError(ErrorFormat::format("forEach 期望 2 个参数，但传入了 %d 个", argCount));
                    }
                    hoResult = executeSharedForEach(args[0], args[1], invoke, line, 0);
                } else if (funName == "find") {
                    if (argCount != 2) {
                        return runtimeError(ErrorFormat::format("find 期望 2 个参数，但传入了 %d 个", argCount));
                    }
                    hoResult = executeSharedFind(args[0], args[1], invoke, line, 0);
                }
                if (hoResult.is_err()) {
                    return runtimeError(hoResult.error().message);
                }
                reg(dstReg) = std::move(hoResult.value());
                ip += returnOffset;
                return VMResult::VM_OK;
            }

            // R136: spawn(fn, args...) — 需要后端注入 ClosureInvoker，走独立路径
            // 镜像 VMCalls.cpp executeCallSpawn 的 StackVM 拦截模式
            if (funName == "spawn") {
                if (argCount < 1) {
                    return runtimeError("spawn 期望至少 1 个参数（函数），但传入了 0 个");
                }
                // 构造闭包调用回调：通过 spawnMutex_ 序列化，避免寄存器帧数据竞争
                ClosureInvoker invoke = [this, line](const Value& closure, const Value* a, size_t ac, int /*ln*/,
                                                     int /*col*/) -> Result<Value> {
                    std::lock_guard<std::mutex> lock(spawnMutex_);
                    Value res;
                    // R136 fix: 保存调用者帧 reg(0)，因为 invokeClosureSync 会把闭包返回值
                    // 写入 reg(dstReg=0)。若调用者帧 reg(0) 持有线程对象（如 spawn 返回的 t），
                    // join() 路径会破坏线程对象导致后续 t.isJoinable() 报类型错误。
                    // 闭包返回值已由 invokeClosureSync 末尾 result = reg(dstReg) 拷入 res，
                    // 此处恢复 reg(0) 不影响 res 的正确性。
                    Value savedReg0 = reg(0);
                    VMResult r = invokeClosureSync(closure, a, ac, 0, line, 0, res);
                    reg(0) = std::move(savedReg0);
                    if (r != VMResult::VM_OK) {
                        return Result<Value>::err(lastError_, line, 0);
                    }
                    return Result<Value>::ok(std::move(res));
                };
                auto r = executeSharedSpawn(args[0], args.data() + 1, argCount - 1, invoke, line, 0);
                if (r.is_err()) {
                    return runtimeError(r.error().message);
                }
                reg(dstReg) = std::move(r.value());
                ip += returnOffset;
                return VMResult::VM_OK;
            }

            // R164 fixup3: 用 isBuiltinFunction 过滤后再调用 executeSharedBuiltinFunction，
            // 并添加 is_err() 分支。原代码对所有未命中 functionChunks_ 的函数名都调用
            // executeSharedBuiltinFunction，但缺失 is_err() 检查，导致内置函数运行时错误
            // （如 int("hello") 的"无法将字符串转换为整数"）被吞掉，最终报"未定义的函数: int"。
            // 与 StackVM（VMCalls.cpp:312-314）的拦截顺序对齐：builtin → class → 未定义。
            if (isBuiltinFunction(funName)) {
                auto result = executeSharedBuiltinFunction(funName, args.data(), argCount, line, 0);
                if (result.is_err()) {
                    return runtimeError(result.error().message);
                }
                reg(dstReg) = result.value();
                // C-8 fix: 内建函数同步返回，用调用者传入的 returnOffset 前进 ip
                ip += returnOffset;
                return VMResult::VM_OK;
            }
            // C-9 fix: 当函数名与已注册类名匹配时，视为类构造调用。
            // visitFunCall 对 ClassName() 语法发射普通 CALL，这里回退到 executeClassNewImpl，
            // 与旧 VM 在 executeCallOps 中检查 classInfo_ 的行为一致。
            auto classIt = classInfo_.find(funName);
            if (classIt != classInfo_.end()) {
                return executeClassNewImpl(ip, funName, argCount, dstReg, argRegs);
            }
            return runtimeError(ErrorFormat::format(ErrorMessages::kUndefinedFunctionFmt, funName.c_str()));
        }
    }

    // PERF-AUDIT-5: 缓存未命中时写入缓存（仅 REG_CALL 路径，funName 来自常量池）
    // R133 fix: 方法调用路径不写入,避免局部变量地址作 key 导致悬垂/错误命中。
    if (!cachedChunk && !closureValue && !isMethodCall) {
        callCache_[namePtr] = &it->second;
    }

    const RegBytecodeChunk& calleeChunk = cachedChunk ? *cachedChunk : it->second;

    // R164 协程/生成器：生成器函数调用拦截（fun* 声明的函数）
    // 命中 isGenerator 标志时不直接执行函数体，而是创建协程值写入 dstReg。
    // 与 StackVM::VM::createCoroutineValue 对称（VMCalls.cpp:1561）。
    // 关键：生成器调用不push新帧，必须手动推进 ip 越过 CALL 指令，
    // 否则主循环会反复执行同一条 CALL 指令导致无限循环。
    if (calleeChunk.isGenerator) {
        ip += returnOffset; // 推进 ip 到 CALL 指令之后（无新帧 push，returnIp 机制不适用）
        return createCoroutineValue(calleeChunk, funName, argCount, dstReg, argRegs, closureValue);
    }

    // C-2 fix: 从闭包值提取 upvalues（若有），填入新帧供 LOAD/STORE_UPVALUE 访问
    auto populateUpvalues = [&closureData](RegCallFrame& newFrame) {
        if (closureData && !closureData->upvalues.empty()) {
            newFrame.upvalues = closureData->upvalues;
        }
    };

    // R164 fixup2: 方法/构造函数调用的参数计数错误消息对齐 StackVM。
    // RegisterVM 的 argCount/arity/requiredArity 包含 this（首个寄存器），
    // 但用户可见的参数计数不应包含 this。StackVM 的方法/构造函数路径
    // 均不将 this 计入参数（receiver 单独处理）。
    // 此 lambda 在报错时减去 this（1），并使用 "构造函数 init"/"方法 X" 替代 "函数 ClassName.X"。
    auto formatParamError = [&]() -> std::string {
        int offset = isMethodCall ? 1 : 0;
        int userArgCount = std::max(0, static_cast<int>(argCount) - offset);
        int userRequired = std::max(0, static_cast<int>(calleeChunk.requiredArity) - offset);
        int userArity = std::max(0, static_cast<int>(calleeChunk.arity) - offset);
        if (isMethodCall) {
            auto dotPos = funName.rfind('.');
            std::string methodName = (dotPos != std::string::npos) ? funName.substr(dotPos + 1) : funName;
            if (methodName == "init") {
                return ErrorFormat::format("构造函数 init 期望 %d-%d 个参数，但传入了 %d 个", userRequired, userArity,
                                           userArgCount);
            }
            return ErrorFormat::format("方法 %s 期望 %d-%d 个参数，但传入了 %d 个", methodName.c_str(), userRequired,
                                       userArity, userArgCount);
        }
        return ErrorFormat::format("函数 %s 期望 %d-%d 个参数，但传入了 %d 个", funName.c_str(),
                                   static_cast<int>(calleeChunk.requiredArity), static_cast<int>(calleeChunk.arity),
                                   static_cast<int>(argCount));
    };

    // 检查参数数量
    // C-11 fix: 原条件 argCount < requiredArity 使 fillDefaultArgs 必返回 false（报错），
    // 而真正需要填默认值的区间 requiredArity <= argCount < arity 反而落入正常分支，
    // 导致默认参数寄存器保持 null（默认值表达式被静默丢弃）。改为 < arity。
    if (argCount < calleeChunk.arity) {
        // 尝试填充默认参数
        uint8_t adjustedArgCount = argCount;
        std::vector<Value> defaults;
        if (!fillDefaultArgs(calleeChunk, adjustedArgCount, funName, defaults)) {
            return runtimeError(formatParamError());
        }
        // 创建新帧
        RegCallFrame newFrame;
        newFrame.chunk = &calleeChunk;
        newFrame.ip = 0;
        newFrame.returnIp = ip + returnOffset; // C-8 fix: 用 returnOffset 替代硬编码 5+argCount
        newFrame.returnReg = dstReg;
        // #8 fix: 定长 array，仅记录激活数量
        newFrame.registerCount = static_cast<uint8_t>(calleeChunk.registerCount <= RegCallFrame::MAX_REGISTERS
                                                          ? calleeChunk.registerCount
                                                          : RegCallFrame::MAX_REGISTERS);
        // 填充参数
        for (uint8_t i = 0; i < argCount; ++i) {
            if (i < newFrame.registerCount) {
                newFrame.registers[i] = reg(argRegs[i]);
            }
        }
        for (uint8_t i = argCount; i < adjustedArgCount; ++i) {
            if (i < newFrame.registerCount && i - argCount < defaults.size()) {
                newFrame.registers[i] = defaults[i - argCount];
            }
        }
        populateUpvalues(newFrame);
        frames_.push_back(std::move(newFrame));
        return VMResult::VM_OK;
    }

    // P1-1 fix: 缺失 argCount > arity 检查。栈式 VM (VMCalls.cpp:397-403) 有显式检查，
    // 原 RegisterVM 实现直接落入正常路径，虽然 for 循环的 i < newFrame.registerCount
    // 保护了寄存器不溢出，但语义错误——多出的参数被静默丢弃而非报错。
    if (argCount > calleeChunk.arity) {
        return runtimeError(formatParamError());
    }

    // 创建新帧
    RegCallFrame newFrame;
    newFrame.chunk = &calleeChunk;
    newFrame.ip = 0;
    newFrame.returnIp = ip + returnOffset; // C-8 fix: 用 returnOffset 替代硬编码 5+argCount
    newFrame.returnReg = dstReg;
    // #8 fix: 定长 array，仅记录激活数量
    newFrame.registerCount =
        static_cast<uint8_t>(calleeChunk.registerCount <= RegCallFrame::MAX_REGISTERS ? calleeChunk.registerCount
                                                                                      : RegCallFrame::MAX_REGISTERS);

    // 填充参数
    for (uint8_t i = 0; i < argCount && i < newFrame.registerCount; ++i) {
        newFrame.registers[i] = reg(argRegs[i]);
    }

    populateUpvalues(newFrame);
    frames_.push_back(std::move(newFrame));
    return VMResult::VM_OK;
}

// ============================================================
// R98 W2: 高阶函数闭包同步调用
// ------------------------------------------------------------
// 镜像 VM::invokeClosureSync（VMCalls.cpp:1289）的寄存器版实现。
// 手动构造 RegCallFrame + 内部指令循环，执行闭包体直到帧弹出。
// 返回值通过 returnReg=dstReg 由 executeReturnImpl 写入调用者寄存器，
// 循环结束后从 reg(dstReg) 读取到 result。
//
// 关键不变量：
//   1. 调用前后 frames_.size() 不变（push 一次，pop 一次）
//   2. 调用前后 caller 的寄存器状态：仅 dstReg 被写入（作为闭包返回值中转）；
//      闭包执行期间 caller 帧不执行指令，dstReg 不被其他路径读取
//   3. DoS 防护：内部循环使用本地计数器，限制为 maxInstructions
//      （与外层 execute() 独立计数，总上限 2x maxInstructions）
//   4. 错误传播：hasError_ + lastError_ 由内部 executeOneInstruction 设置，
//      调用方检测 VM_RUNTIME_ERROR 后通过 lastError_ 获取错误信息
// ============================================================
VMResult RegisterVM::invokeClosureSync(const Value& closure, const Value* args, size_t argCount, uint8_t dstReg,
                                       int line, int /*column*/, Value& result) {
    if (!closure.isClosure()) {
        return runtimeError("高阶函数的参数必须是函数");
    }

    // 查找闭包 chunk（RegisterVM 的 VMClosureData 不设置 regChunkPtr，
    // 与 StackVM 不同——通过函数名查找 RegBytecodeChunk）
    const RegBytecodeChunk* targetChunkPtr = nullptr;
    auto chunkIt = functionChunks_.find(closure.closureName());
    if (chunkIt != functionChunks_.end()) {
        targetChunkPtr = &chunkIt->second;
    }
    if (!targetChunkPtr) {
        return runtimeError("未找到函数: " + closure.closureName());
    }
    const RegBytecodeChunk& targetChunk = *targetChunkPtr;

    // 参数数量检查
    if (argCount < static_cast<size_t>(targetChunk.requiredArity) ||
        argCount > static_cast<size_t>(targetChunk.arity)) {
        return runtimeError(ErrorFormat::format("函数 %s 期望 %d-%d 个参数，但传入了 %zu 个",
                                                closure.closureName().c_str(), targetChunk.requiredArity,
                                                targetChunk.arity, argCount));
    }

    // MAX_FRAMES 检查
    if (frames_.size() >= MAX_FRAMES) {
        return runtimeError(
            ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt, static_cast<int>(MAX_FRAMES)));
    }

    // 检查 dstReg 在调用者帧中有效（用于接收返回值）
    if (frames_.empty() || dstReg >= currentFrame().registerCount) {
        return runtimeError("invokeClosureSync: dstReg 越界");
    }

    // 构造调用帧
    RegCallFrame newFrame;
    newFrame.chunk = &targetChunk;
    newFrame.ip = 0;
    newFrame.returnIp = 0;                         // 哨兵值——内部循环检测帧弹出，不依赖 returnIp
    newFrame.returnReg = static_cast<int>(dstReg); // 闭包返回值写入调用者 dstReg
    newFrame.registerCount =
        static_cast<uint8_t>(targetChunk.registerCount <= RegCallFrame::MAX_REGISTERS ? targetChunk.registerCount
                                                                                      : RegCallFrame::MAX_REGISTERS);
    // 填充参数
    uint8_t effectiveArgCount = static_cast<uint8_t>(argCount);
    for (uint8_t i = 0; i < effectiveArgCount && i < newFrame.registerCount; ++i) {
        newFrame.registers[i] = args[i];
    }
    // 填充默认参数
    if (effectiveArgCount < targetChunk.arity) {
        uint8_t missingCount = targetChunk.arity - effectiveArgCount;
        int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx < 0) {
            return runtimeError("函数 " + closure.closureName() + " 默认参数索引越界");
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = targetChunk.defaultConstIndices[i];
            if (constIdx == 0xFFFF || constIdx >= targetChunk.constants.size()) {
                return runtimeError("函数 " + closure.closureName() + " 默认参数无效");
            }
            uint8_t regIdx = effectiveArgCount++;
            if (regIdx < newFrame.registerCount) {
                newFrame.registers[regIdx] = targetChunk.constants[constIdx];
            }
        }
    }
    // 填充 upvalues
    if (closure.vmClosure() && !closure.vmClosure()->upvalues.empty()) {
        newFrame.upvalues = closure.vmClosure()->upvalues;
    }

    size_t savedFrameCount = frames_.size();
    frames_.push_back(std::move(newFrame));

    // 内部指令循环：执行直到帧弹出
    // DoS 防护：本地计数器限制为 maxInstructions（与外层 execute() 独立计数）
    int64_t localInstrCount = 0;
    int64_t maxInstr = RuntimeLimits::RuntimeConfig::instance().maxInstructions();
    while (frames_.size() > savedFrameCount && !hasError_) {
        if (++localInstrCount > maxInstr) {
            runtimeError("指令执行数超过上限，疑似无限循环");
            break;
        }
        VMResult r = executeOneInstruction();
        if (r != VMResult::VM_OK || hasError_) {
            break;
        }
    }

    if (hasError_) {
        // 错误已设置到 lastError_，调用方检测 VM_RUNTIME_ERROR 后读取
        return VMResult::VM_RUNTIME_ERROR;
    }

    // 闭包返回值已由 executeReturnImpl 写入 caller.registers[dstReg]
    result = reg(dstReg);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeReturnImpl(size_t& ip, Value result) {
    if (frames_.empty()) {
        return runtimeError("空帧返回");
    }

    int returnReg = frames_.back().returnReg;
    size_t returnIp = frames_.back().returnIp;
    bool wasMethodCall = frames_.back().isMethodCall;
    bool isInitCall = frames_.back().isInitCall;
    bool fieldsModified = frames_.back().fieldsModified;
    int receiverReg = frames_.back().receiverReg;
    std::string receiverVarName = std::move(frames_.back().receiverVarName);

    // CRITICAL-4 fix: 方法返回时记录 receiverReg 到 lastMutatedReceiverReg_，
    // 供 IR 路径中 visitMethodCall 的 LOAD_MUTATED + STORE 写回变异后接收者。
    // 对于只读方法，receiverReg 持有原值（未被修改），写回原值等于不写，安全。
    // 对于变异方法，executeReturnImpl 的字段同步已将变异后实例写入 receiverReg。
    // 必须在所有 wasMethodCall 路径设置，否则只读方法的 LOAD_MUTATED 会读到
    // 上一次设置的错误值，导致写回错误对象（回归：ClassMethodNoThis 等）。
    if (wasMethodCall && receiverReg >= 0) {
        lastMutatedReceiverReg_ = static_cast<uint8_t>(receiverReg);
    }

    // C-9/C-6 fix: 捕获方法帧的 this（slot 0）用于字段同步。
    // - init 方法：隐式返回 this 实例（而非 null），避免 caller 的 dstReg 被 null 覆盖。
    //   无论是否直接修改字段都需捕获——super.init 可能已通过字段同步更新了 this。
    // - 普通方法：仅当 fieldsModified 时捕获（避免不必要的拷贝），用于同步字段修改。
    // 原实现仅依赖 result.isInstance() 判断，导致返回非实例的方法修改 this 字段后被丢弃。
    Value methodThis;
    bool hasMethodThis = false;
    bool shouldCaptureThis = isInitCall || fieldsModified;
    if (wasMethodCall && shouldCaptureThis && frames_.back().registerCount > 0 &&
        frames_.back().registers[0].isInstance()) {
        methodThis = frames_.back().registers[0]; // copy（not move），frame 仍需 closeUpvalues
        hasMethodThis = true;
    }

    // C-9 fix: init 方法隐式返回 this 实例（而非 null）。
    if (isInitCall && !result.isInstance() && hasMethodThis) {
        result = methodThis;
    }

    size_t returningFrameIdx = frames_.size() - 1;
    // C-3 fix: 弹帧前关闭指向该帧寄存器的 open upvalues，捕获当前值到 uv->value，
    // 防止帧销毁后闭包持有悬垂引用。编码范围 [idx*32, (idx+1)*32) 覆盖该帧全部寄存器槽。
    // AUDIT-P2.5 fix: closeUpvaluesFrom 返回错误时立即终止返回，避免在损坏状态上继续
    if (closeUpvaluesFrom(returningFrameIdx * RegCallFrame::MAX_REGISTERS) != VMResult::VM_OK)
        return VMResult::VM_RUNTIME_ERROR;
    frames_.pop_back();

    // 清理属于返回帧的 tryStack_ handler
    while (!tryStack_.empty() && tryStack_.back().frameIndex >= returningFrameIdx) {
        tryStack_.pop_back();
    }

    // C-6 fix: 方法调用字段同步。
    // 原实现仅当 result.isInstance() 时同步，导致返回非实例值的方法（如 increment 返回数字）
    // 修改 this 字段后被丢弃。改为从 methodThis（方法帧 slot 0 的 this）同步字段。
    // 优先使用 methodThis（C-6），回退到 result（保留原行为供返回实例的方法使用）。
    // P0-1 fix: isInitCall 也需同步——多层 super.init() 链中中间层可能不直接写字段
    // (fieldsModified=false)，但父类 init 修改的 this 必须传播回 caller。
    if (wasMethodCall && (fieldsModified || isInitCall) && !frames_.empty()) {
        const Value* syncSource = nullptr;
        if (hasMethodThis) {
            syncSource = &methodThis;
        } else if (result.isInstance()) {
            syncSource = &result;
        }

        if (syncSource) {
            // Bug4 fix: 当 syncSource 是 methodThis 时，直接替换接收者 Value，
            // 避免 thisVal.fields() 触发 ensureUnique 深拷贝整个 InstanceData
            // （大字段类+循环方法调用场景的高频分配热点）。
            // methodThis 是方法帧 slot 0 的 COW 副本，已包含方法的字段修改，
            // 直接替换与逐字段同步语义等价（MiniLang 不支持字段删除）。
            // 当 syncSource 是 result（回退路径，方法返回实例）时，
            // 保留逐字段同步以维持原语义（result 可能是不同实例）。
            bool directReplace = hasMethodThis;

            // 同步字段到 caller 帧的接收者寄存器
            if (receiverReg >= 0 && static_cast<size_t>(receiverReg) < currentFrame().registerCount) {
                Value& thisVal = currentFrame().registers[receiverReg];
                if (thisVal.isInstance()) {
                    if (directReplace) {
                        thisVal = *syncSource; // 零 COW，仅指针+refCount 操作
                    } else {
                        auto& targetFields = thisVal.fields(); // ensureUnique 一次
                        for (const auto& field : syncSource->fields()) {
                            targetFields[field.first] = field.second;
                        }
                    }
                }
            }
            // 写回到接收者的原始位置（全局变量）
            if (!receiverVarName.empty()) {
                auto gsIt = globalNameToSlot_.find(receiverVarName);
                if (gsIt != globalNameToSlot_.end() && gsIt->second < static_cast<int>(globalSlots_.size())) {
                    Value& globalVal = globalSlots_[gsIt->second];
                    if (globalVal.isInstance()) {
                        if (directReplace) {
                            globalVal = *syncSource; // 零 COW
                        } else {
                            auto& targetFields = globalVal.fields(); // ensureUnique 一次
                            for (const auto& field : syncSource->fields()) {
                                targetFields[field.first] = field.second;
                            }
                        }
                    }
                }
            }
        }
    }

    // 写入返回值到调用者的目标寄存器
    if (!frames_.empty() && returnReg >= 0) {
        if (static_cast<size_t>(returnReg) < currentFrame().registerCount) {
            currentFrame().registers[returnReg] = std::move(result);
        }
    } else if (returnReg < 0) {
        // R164 D.7 fix: 协程重放模式下生成器帧 returnReg=-1，返回值保存到邮箱供 callCoroutineNext 读取。
        // 对齐 StackVM 的 OP_RETURN 将返回值 push 到栈、callCoroutineNext pop 获取的语义。
        coroutineReturnValue_ = std::move(result);
    }

    // 恢复调用者 ip
    if (!frames_.empty()) {
        currentFrame().ip = returnIp;
    }

    return VMResult::VM_OK;
}

VMResult RegisterVM::executeMethodCallImpl(size_t& ip, const std::string& methodName, uint8_t argCount, uint8_t dstReg,
                                           uint8_t objReg, const SmallArgs<uint8_t>& argRegs) {
    Value& obj = reg(objReg);

    // 尝试内建方法
    SmallArgs<Value> args;
    for (uint8_t i = 0; i < argCount; ++i) {
        args.push_back(reg(argRegs[i]));
    }

    Value builtinResult;
    // C-9 fix: callBuiltinMethod 返回 bool（true=已处理），原返回 VMResult 导致
    // instance 类型 fallthrough 到末尾 return VM_OK，caller 误以为已处理而静默返回 null。
    bool handled = callBuiltinMethod(obj, methodName, args, builtinResult);
    if (handled) {
        if (hasError_)
            return VMResult::VM_RUNTIME_ERROR;
        // CRITICAL-4 fix: 内建方法（push/pop/set 等）通过引用修改 obj（即 reg(objReg)）。
        // 若 objReg 持有 COW 共享副本（如来自 LOAD_GLOBAL），修改产生的新实例仅存在于 objReg，
        // 原始位置（全局变量槽/upvalue）未更新。记录 objReg 到 lastMutatedReceiverReg_，
        // 供后续 LOAD_MUTATED + STORE 写回到原始位置（VarRef 路径）。
        lastMutatedReceiverReg_ = objReg;
        reg(dstReg) = std::move(builtinResult);
        ip += 6 + argCount;
        return VMResult::VM_OK;
    }

    // 实例方法调用
    if (obj.isInstance()) {
        const std::string& className = obj.className();
        // R133 Inline Cache: 先查 per-class methodCache（O(1) hash + 字符串比较）。
        // 镜像 StackVM VMClassInfo::methodCache 的设计：命中（含空字符串负缓存）
        // 直接跳过沿继承链查找。负缓存语义：空字符串 = "沿继承链未找到方法"，
        // 此时直接报"类 X 没有方法 Y" 错误（与下方未命中路径一致）。
        // 缓存 key: methodName（局部变量,稳定）
        // 缓存 value: funName（classInfo_ 内 methods map 的 value 副本,std::string 拥有独立所有权）
        // 生命周期安全：methodCache 随 RegClassInfo 一起存储在 classInfo_ 中,
        //              classInfo_ 在 resetState 时 clear(),不会悬垂。
        // 失效条件：REG_DEFINE_CLASS 父类重定义时遍历子类置 methodCache.clear()。
        auto classInfoIt = classInfo_.find(className);
        if (classInfoIt != classInfo_.end()) {
            auto cacheIt = classInfoIt->second.methodCache.find(methodName);
            if (cacheIt != classInfoIt->second.methodCache.end()) {
                const std::string& cachedFunName = cacheIt->second;
                if (cachedFunName.empty()) {
                    // 负缓存：沿继承链未找到方法
                    return runtimeError("类 " + className + " 没有方法 " + methodName);
                }
                // 正缓存命中：直接调用
                SmallArgs<uint8_t> fullArgRegs;
                fullArgRegs.push_back(objReg);
                for (uint8_t i = 0; i < argCount; ++i) {
                    fullArgRegs.push_back(argRegs[i]);
                }
                size_t newIp = ip;
                if (argCount >= 255)
                    return runtimeError("方法调用参数数量超过上限");
                VMResult cr = executeCallImpl(newIp, cachedFunName, argCount + 1, dstReg, fullArgRegs, 6u + argCount,
                                              nullptr, true);
                if (cr != VMResult::VM_OK)
                    return cr;
                if (!frames_.empty()) {
                    frames_.back().isMethodCall = true;
                    frames_.back().isInitCall = (methodName == "init");
                    frames_.back().receiverReg = objReg;
                    frames_.back().receiverVarName.clear();
                }
                ip = newIp;
                return VMResult::VM_OK;
            }
        }
        // IC 未命中：沿继承链查找方法（P1-1 fix 原路径,子类优先,找不到则查父类）
        std::string searchClass = className;
        std::string foundFunName; // 用于 IC 写入
        bool methodFound = false;
        for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
            auto classIt = classInfo_.find(searchClass);
            if (classIt == classInfo_.end())
                break;
            auto methodIt = classIt->second.methods.find(methodName);
            if (methodIt != classIt->second.methods.end()) {
                foundFunName = methodIt->second;
                methodFound = true;
                break;
            }
            searchClass = classIt->second.parent;
        }
        // R133 IC 写入：将查找结果（正缓存 funName 或负缓存空字符串）写入当前类
        // 的 methodCache。注意：写入点为 className（实例实际类）的 cache,
        // 与 StackVM findMethodChunk 的"在起始类缓存"模式对齐（即使方法在父类找到,
        // 缓存仍记录在子类,避免下次同类型实例再次沿继承链查找）。
        if (classInfoIt != classInfo_.end()) {
            classInfoIt->second.methodCache[methodName] = methodFound ? foundFunName : std::string{};
        }
        if (!methodFound) {
            // BUG-INH-3 fix: 错误消息与 Interpreter/StackVM 一致（"类 X 没有方法 Y"）
            return runtimeError("类 " + className + " 没有方法 " + methodName);
        }
        // 正路径:调用方法函数
        SmallArgs<uint8_t> fullArgRegs;
        fullArgRegs.push_back(objReg);
        for (uint8_t i = 0; i < argCount; ++i) {
            fullArgRegs.push_back(argRegs[i]);
        }
        size_t newIp = ip;
        if (argCount >= 255)
            return runtimeError("方法调用参数数量超过上限");
        VMResult cr =
            executeCallImpl(newIp, foundFunName, argCount + 1, dstReg, fullArgRegs, 6u + argCount, nullptr, true);
        if (cr != VMResult::VM_OK)
            return cr;
        if (!frames_.empty()) {
            frames_.back().isMethodCall = true;
            frames_.back().isInitCall = (methodName == "init");
            frames_.back().receiverReg = objReg;
            frames_.back().receiverVarName.clear();
        }
        ip = newIp;
        return VMResult::VM_OK;
    }

    // AUDIT-ERRPATH fix: obj 不是 instance 且方法不是已识别的内置方法。
    // 原消息"方法调用需要类实例"对 null/int/float/bool/closure 具有误导性。
    // 与 callBuiltinMethod 的 final fallthrough 对齐，发出准确的类型错误。
    return runtimeError("类型 " + obj.typeName() + " 不支持方法 " + methodName);
}

VMResult RegisterVM::executeClosureImpl(size_t& ip, const std::string& name, uint8_t uvCount,
                                        const SmallArgs<uint8_t>& uvSpecs, uint8_t dstReg) {
    // C-2 fix: 完整实现 upvalue 绑定。
    // stackSlot 编码为 frameIdx * 32 + slot（slot < 32），使每个寄存器槽拥有全局唯一地址，
    // 解决原代码硬编码 frames_[size-2] 导致的多层嵌套闭包 passthrough upvalue 错位问题。
    // - isLocal=true: 直接捕获外层帧（当前帧）的局部变量 slot idx
    // - isLocal=false: 透传，复用当前帧 upvalues[idx] 的 shared_ptr<VMUpvalue>（已含正确 frameIdx）
    auto& current = currentFrame();
    size_t currentFrameIdx = frames_.size() - 1;
    auto upvalues = std::make_shared<VMClosureData>();
    upvalues->functionName = name;
    upvalues->upvalues.reserve(uvCount);
    for (uint8_t i = 0; i < uvCount; ++i) {
        uint8_t isLocal = uvSpecs[i * 2];
        uint8_t idx = uvSpecs[i * 2 + 1];
        if (isLocal) {
            auto uv = std::make_shared<VMUpvalue>();
            uv->isClosed = false;
            uv->stackSlot = currentFrameIdx * RegCallFrame::MAX_REGISTERS + idx;
            upvalues->upvalues.push_back(uv);
            // 注册到 openUpvalues_ 供 closeUpvaluesFrom 按帧关闭
            openUpvalues_.insert({uv->stackSlot, std::weak_ptr<VMUpvalue>(uv)});
        } else {
            // 透传：复用外层闭包的 upvalue（shared_ptr 共享所有权，frameIdx 已正确编码）
            if (idx < current.upvalues.size()) {
                upvalues->upvalues.push_back(current.upvalues[idx]);
            } else {
                // 透传索引无效：降级为已关闭的 null upvalue
                auto uv = std::make_shared<VMUpvalue>();
                uv->isClosed = true;
                upvalues->upvalues.push_back(uv);
            }
        }
    }

    // 创建闭包值并绑定 vmClosure（供 REG_CALL_EXPR 调用时取出 upvalues 填入新帧）
    Value closureVal = Value::makeClosure(name, nullptr, {}, nullptr);
    closureVal.vmClosure() = upvalues;
    reg(dstReg) = std::move(closureVal);

    ip += 5 + uvCount * 2;
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeClassNewImpl(size_t& ip, const std::string& className, uint8_t argCount, uint8_t dstReg,
                                         const SmallArgs<uint8_t>& argRegs) {
    auto classIt = classInfo_.find(className);
    if (classIt == classInfo_.end()) {
        return runtimeError("未定义的类: " + className);
    }
    RegClassInfo& info = classIt->second;

    // perf2 fix: 懒预计算展平字段顺序 + 解析 init 函数名（首次构造时计算，后续复用）。
    // 原实现每次构造都遍历继承链两次（O(depth)），现降至 O(1) 查表。
    // 此时所有父类必已定义（顶层 REG_DEFINE_CLASS 已全部执行）。
    if (!info.flattenedComputed) {
        // 沿继承链构建链（子→父→祖...）
        std::vector<std::string> chain;
        std::string cur = className;
        for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
            auto it = classInfo_.find(cur);
            if (it == classInfo_.end())
                break;
            chain.push_back(cur);
            cur = it->second.parent;
        }
        // BUG-INH-AUDIT-3 fix: 循环继承检测 — guard 耗尽但 cur 仍非空说明存在继承环。
        // 对齐 StackVM executeDefineClass 的 V-P2 fix (VMCalls.cpp:1058)。
        // 原实现缺少此检查，模块化间接循环（a.mini: class A : B + b.mini: class B : A）
        // 绕过 Parser 静态检查后将导致后续方法查找/init 解析陷入死循环。
        if (!cur.empty()) {
            return runtimeError("类继承链过深或存在循环继承: " + className + " -> " + cur);
        }
        // 逆序初始化字段（父类字段在前，子类字段在后）
        // BUG-INH-1 fix: 同时收集字段默认值（对齐 StackVM 的 mergedDefaults 语义）。
        // BUG-INH-REG-1 fix: 去重——子类覆盖的父类同名字段不再重复入表。
        // 原实现沿继承链逆序遍历无去重，子类覆盖的父类同名字段在链中每个类都 push 一次，
        // 导致 flattenedFieldOrder 含重复项（无语义影响但浪费初始化迭代）。
        // 对齐 StackVM 的 mergedOrder "if not found then add" 语义：
        // 父类字段先入表，子类同名字段仅更新默认值不重复入表。
        info.flattenedFieldOrder.clear();
        info.flattenedFieldDefaults.clear();
        std::unordered_map<std::string, size_t> fieldIndexMap; // fieldName → flattenedFieldOrder 索引
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            auto clsIt = classInfo_.find(*it);
            if (clsIt != classInfo_.end()) {
                const auto& cls = clsIt->second;
                for (size_t i = 0; i < cls.fieldOrder.size(); ++i) {
                    const std::string& fieldName = cls.fieldOrder[i];
                    auto mapIt = fieldIndexMap.find(fieldName);
                    if (mapIt == fieldIndexMap.end()) {
                        // 新字段：添加到展平表
                        fieldIndexMap[fieldName] = info.flattenedFieldOrder.size();
                        info.flattenedFieldOrder.push_back(fieldName);
                        if (i < cls.fieldDefaults.size()) {
                            info.flattenedFieldDefaults.push_back(cls.fieldDefaults[i]);
                        } else {
                            info.flattenedFieldDefaults.push_back(Value::nullValue());
                        }
                    } else {
                        // 已存在（子类覆盖父类同名字段）：仅更新默认值
                        if (i < cls.fieldDefaults.size()) {
                            info.flattenedFieldDefaults[mapIt->second] = cls.fieldDefaults[i];
                        }
                    }
                }
            }
        }
        // 沿继承链解析 init（子类优先，与栈式 VM findMethodChunk 语义一致）
        info.hasInit = false;
        info.resolvedInitFunName.clear();
        std::string searchClass = className;
        for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
            auto clsIt = classInfo_.find(searchClass);
            if (clsIt == classInfo_.end())
                break;
            auto mIt = clsIt->second.methods.find("init");
            if (mIt != clsIt->second.methods.end()) {
                info.resolvedInitFunName = mIt->second;
                info.hasInit = true;
                break;
            }
            searchClass = clsIt->second.parent;
        }
        // BUG-INH-AUDIT-3 fix: 循环继承检测已在上方链构建循环中完成（guard 耗尽即报错）。
        // 此处 init 解析遍历相同的（已验证无环的）继承链，guard < 64 足以防止死循环。
        // 注意：不能在此处检查 !searchClass.empty()——init 找到时 break 退出，searchClass
        // 仍指向含 init 的类名（非空），会被误判为循环。hasInit 标志已正确记录查找结果。
        info.flattenedComputed = true;
    }

    // 创建实例并初始化所有字段（使用预计算的展平字段顺序 + 默认值）
    // BUG-INH-1 fix: 使用 flattenedFieldDefaults 而非硬编码 null，对齐 StackVM
    // 的 instance.fields() = cls.fieldDefaults 语义。
    Value instance = Value::makeInstance(className);
    for (size_t i = 0; i < info.flattenedFieldOrder.size(); ++i) {
        instance.fields()[info.flattenedFieldOrder[i]] = info.flattenedFieldDefaults[i];
    }

    reg(dstReg) = instance;

    // 调用 init 方法（使用预解析的函数名）
    if (info.hasInit) {
        SmallArgs<uint8_t> fullArgRegs;
        fullArgRegs.push_back(dstReg);
        for (uint8_t i = 0; i < argCount; ++i) {
            fullArgRegs.push_back(argRegs[i]);
        }
        size_t newIp = ip;
        // C-8 fix: REG_CLASS_NEW 指令长度 = 5 + argCount（argCount 不含 this）。
        // executeCallImpl 收到 argCount+1（含 instance），但 returnOffset 用原始 argCount 计算。
        // 原硬编码 ip+5+(argCount+1) 比真实下一条指令多 1，init 返回后调用者 ip 错位。
        // 防御性检查：argCount+1 溢出 uint8_t（backend 应已在编译期拦截）
        if (argCount >= 255)
            return runtimeError("类构造参数数量超过上限");
        VMResult r = executeCallImpl(newIp, info.resolvedInitFunName, argCount + 1, dstReg, fullArgRegs, 5u + argCount,
                                     nullptr, true);
        if (r != VMResult::VM_OK)
            return r;
        if (!frames_.empty()) {
            frames_.back().isMethodCall = true;
            frames_.back().isInitCall = true;
            frames_.back().receiverReg = dstReg;
        }
        ip = newIp;
    } else {
        // BUG-VM-01 fix (RegisterVM): 无 init 但有参数时报错，与 StackVM 行为一致。
        // 原实现静默忽略参数，三后端语义不一致。
        if (argCount > 0) {
            return runtimeError(ErrorFormat::format("类 %s 没有 init 方法，但传入了 %d 个参数", className.c_str(),
                                                    static_cast<int>(argCount)));
        }
        ip += 5 + argCount;
    }

    return VMResult::VM_OK;
}

bool RegisterVM::fillDefaultArgs(const RegBytecodeChunk& chunk, uint8_t& argCount, const std::string& funName,
                                 std::vector<Value>& defaults) {
    if (argCount >= chunk.arity)
        return true;
    if (argCount < chunk.requiredArity)
        return false;

    for (size_t i = argCount; i < chunk.defaultConstIndices.size() + chunk.requiredArity; ++i) {
        size_t defaultIdx = i - chunk.requiredArity;
        if (defaultIdx >= chunk.defaultConstIndices.size())
            break;
        uint16_t constIdx = chunk.defaultConstIndices[defaultIdx];
        // BUGFIX-P2 fix: 与 Stack VM (VM.cpp) 对齐，0xFFFF 表示非字面量默认表达式
        if (constIdx == 0xFFFF)
            return false;
        if (constIdx >= chunk.constants.size())
            return false;
        defaults.push_back(chunk.constants[constIdx]);
    }
    argCount = chunk.arity;
    return true;
}

// ============================================================
// 内建方法
// ============================================================

bool RegisterVM::callArrayBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName,
                                        SmallArgs<Value>& args, Value& result) {
    switch (method) {
    case BuiltinMethod::ARR_LEN: {
        auto r = executeSharedLen(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::ARR_PUSH:
        if (args.size() != 1) {
            runtimeError("push 需要 1 个参数");
            return true;
        }
        obj.arrayVal().push_back(args[0]);
        // R7 fix: 与栈式 VM 对齐返回 null。原返回 obj（数组本身）会导致
        // `var x = arr.push(1)` 在两后端产生不同值（栈式 VM x=null / RegisterVM x=arr）。
        result = Value::nullValue();
        return true;
    case BuiltinMethod::ARR_POP: {
        auto& arr = obj.arrayVal();
        if (arr.empty()) {
            runtimeError("对空数组调用 pop");
            return true;
        }
        result = arr.back();
        arr.pop_back();
        return true;
    }
    case BuiltinMethod::ARR_CONTAINS: {
        auto r = executeSharedArrayContains(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::ARR_JOIN: {
        // B3 fix: 对齐栈式 VM dispatchArrayBuiltin——原缺失此 case，
        // arr.join() 落入 default:break → 返回 false → 调用方误报"方法调用需要类实例"。
        auto r = executeSharedArrayJoin(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::ARR_REMOVE: {
        // B3 fix: 对齐栈式 VM dispatchArrayBuiltin 的 ARR_REMOVE 实现。
        // 原缺失此 case，arr.remove(i) 误报"方法调用需要类实例"。
        if (args.size() != 1) {
            runtimeError("remove 期望 1 个参数(索引)");
            return true;
        }
        if (!args[0].isInt()) {
            runtimeError("remove 参数必须是整数索引");
            return true;
        }
        int64_t ri = args[0].intVal();
        auto& arr = obj.arrayVal();
        if (ri < 0 || static_cast<size_t>(ri) >= arr.size()) {
            runtimeError(ErrorFormat::format("数组索引越界: %lld", static_cast<long long>(ri)));
            return true;
        }
        arr.erase(arr.begin() + static_cast<size_t>(ri));
        result = Value::nullValue();
        return true;
    }
    default:
        // 2026-06-29 BUG-1 fix: 已知内置方法但不适用于数组（如 dict.keys 对数组调用）
        runtimeError("数组没有方法 " + methodName);
        return true;
    }
}

bool RegisterVM::callDictBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName,
                                       SmallArgs<Value>& args, Value& result) {
    switch (method) {
    case BuiltinMethod::DICT_LEN:
    case BuiltinMethod::ARR_LEN: { // "len" 同时分类为 ARR_LEN，dict 也走此分支
        auto r = executeSharedLen(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::DICT_KEYS: {
        auto r = executeSharedDictKeys(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::DICT_VALUES: {
        auto r = executeSharedDictValues(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::DICT_HAS: {
        auto r = executeSharedDictHas(obj, methodName, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::DICT_GET: {
        // P1-7 fix: 对齐栈式VM——补齐 dict.get() 方法分发
        auto r = executeSharedDictGet(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::DICT_REMOVE: {
        if (args.size() != 1) {
            runtimeError("remove 期望 1 个参数(键)");
            return true;
        }
        // L4 fix: 字典键支持 string/int/bool/float
        auto dk = Value::dictKeyFromValue(args[0]);
        if (!dk) {
            runtimeError(ErrorMessages::kDictKeyInvalidType);
            return true;
        }
        obj.dictVal().erase(*dk);
        result = Value::nullValue();
        return true;
    }
    case BuiltinMethod::DICT_SET: {
        if (args.size() != 2) {
            runtimeError("set 期望 2 个参数(键, 值)");
            return true;
        }
        // L4 fix: 字典键支持 string/int/bool/float
        auto dk = Value::dictKeyFromValue(args[0]);
        if (!dk) {
            runtimeError(ErrorMessages::kDictKeyInvalidType);
            return true;
        }
        obj.dictVal()[*dk] = args[1];
        result = Value::nullValue();
        return true;
    }
    default:
        // 2026-06-29 BUG-1 fix: 已知内置方法但不适用于字典（如 arr.push 对字典调用）
        runtimeError("字典没有方法 " + methodName);
        return true;
    }
}

bool RegisterVM::callStringBuiltinMethod(Value& obj, BuiltinMethod method, const std::string& methodName,
                                         SmallArgs<Value>& args, Value& result) {
    switch (method) {
    case BuiltinMethod::STR_LEN:
    case BuiltinMethod::ARR_LEN: { // "len" 同时分类为 ARR_LEN，string 也走此分支
        auto r = executeSharedLen(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_UPPER: {
        auto r = executeSharedStrUpper(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_LOWER: {
        auto r = executeSharedStrLower(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_CONTAINS:
    case BuiltinMethod::ARR_CONTAINS: { // "contains" 分类为 ARR_CONTAINS，string 也走此分支
        auto r = executeSharedStrContains(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_STARTS_WITH: {
        auto r = executeSharedStrStartsWith(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_ENDS_WITH: {
        auto r = executeSharedStrEndsWith(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_REPLACE: {
        auto r = executeSharedStrReplace(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_SUBSTR: {
        auto r = executeSharedStrSubstr(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_INDEX_OF: {
        auto r = executeSharedStrIndexOf(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_SPLIT: {
        auto r = executeSharedStrSplit(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    case BuiltinMethod::STR_TRIM: {
        auto r = executeSharedStrTrim(obj, args.data(), args.size());
        if (r.is_ok()) {
            result = r.value();
            return true;
        }
        runtimeError(r.error().message);
        return true;
    }
    default:
        // 2026-06-29 BUG-1 fix: 已知内置方法但不适用于字符串（如 arr.push 对字符串调用）
        runtimeError("字符串没有方法 " + methodName);
        return true;
    }
}

bool RegisterVM::callBuiltinMethod(Value& obj, const std::string& methodName, SmallArgs<Value>& args, Value& result) {
    // #20 fix: 复用共享 classifyBuiltinMethod 枚举分发，消除长串字符串比较。
    // 原实现按 obj 类型分支后逐个 if (methodName == "...")，最坏需 N 次字符串比较。
    // 改为单次 classify + switch 分发，与栈式 VM 行为一致。
    BuiltinMethod method = classifyBuiltinMethod(methodName);

    // R136: 同步对象方法不通过 classifyBuiltinMethod 分类（方法名如 send/recv/lock
    // 等不在内建方法枚举中），此处按类型直接拦截。同步对象方法共享层抛 RuntimeError，
    // 用 try/catch 转为 runtimeError。
    if (obj.isChannel() || obj.isMutex() || obj.isRwLock() || obj.isThread()) {
        try {
            std::vector<Value> argsVec(args.begin(), args.end());
            auto br = handleSyncObjectMethod(methodName, obj, argsVec, 0, 0);
            result = std::move(br.result);
            return true;
        } catch (const RuntimeError& e) {
            runtimeError(e.what());
            return true;
        }
    }

    // R164 协程/生成器：协程方法 .next()/.done() 直接拦截，不通过 classifyBuiltinMethod 分类
    // （与 StackVM dispatchCoroutineBuiltin 对称，VMCalls.cpp:1599）
    if (obj.isCoroutine()) {
        return dispatchCoroutineBuiltin(obj, methodName, args, result);
    }

    if (method == BuiltinMethod::UNKNOWN)
        return false;

    if (obj.isArray()) {
        return callArrayBuiltinMethod(obj, method, methodName, args, result);
    }
    if (obj.isDict()) {
        return callDictBuiltinMethod(obj, method, methodName, args, result);
    }
    if (obj.isString()) {
        return callStringBuiltinMethod(obj, method, methodName, args, result);
    }
    // 2026-06-29 BUG-1 fix: obj 不是 array/dict/string。
    // - 若为实例：返回 false 让 caller 查找用户定义方法（用户类可能定义了与内置方法同名的方法）
    // - 若为原始类型(null/int/float/bool/closure)：原代码 return false 导致 caller 误报
    //   "方法调用需要类实例"，消息具有误导性。改为发出准确的类型错误消息。
    if (obj.isInstance()) {
        return false;
    }
    runtimeError("类型 " + obj.typeName() + " 不支持方法 " + methodName);
    return true;
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
            // BUG-EXC-5 fix: 关闭 catch 帧 try 块遗留的 open upvalues，对齐 StackVM
            // throwException 的 closeUpvaluesFrom(handler.stackBase)。try 块中声明的局部变量
            // （寄存器 [registerBase, registerCount)）若被闭包捕获，需在 catch 块覆盖前
            // 关闭 upvalue（拷贝值到 heap）。不重置 registerCount——catch 块的 vreg 映射
            // 可能引用 try 块之后分配的寄存器，重置会导致 reg() 边界检查失败。
            size_t fromSlot = handler.frameIndex * RegCallFrame::MAX_REGISTERS + handler.registerBase;
            // AUDIT-P2.5 fix: closeUpvaluesFrom 返回错误时立即终止异常处理，不跳转 catchIp
            if (closeUpvaluesFrom(fromSlot) != VMResult::VM_OK)
                return VMResult::VM_RUNTIME_ERROR;
            curFrame.ip = handler.catchIp;
            // P1-4 fix: 异常值存入 pendingException_，由 REG_LOAD_EXCEPTION 读取到指定寄存器。
            // 原方案固定写 R0 会覆盖用户变量/this（方法中 R0 是 this）。
            pendingException_ = std::move(thrownValue);
        }
        tryStack_.pop_back();
        return VMResult::VM_OK;
    }
    // 未捕获的异常
    // BUG-IBACKEND-4 fix: 三后端消息一致——统一 toString + 200 字符截断
    // （对齐 StackVM VM.cpp:130-132 与 Interpreter）
    std::string str = thrownValue.toString();
    if (str.size() > 200)
        str = str.substr(0, 200) + "...";
    return runtimeError("未捕获的异常: " + str);
}

VMResult RegisterVM::closeUpvaluesFrom(size_t fromSlot) {
    // C-3 fix: stackSlot 编码为 frameIdx*32+slot，按全局地址范围 [fromSlot, ∞) 关闭。
    // 从对应帧（由 frameIdx 解码）读取当前值并标记为已关闭，防止帧弹出后悬垂引用。
    // AUDIT-P2.5 fix: 返回类型 void→VMResult，调用方可 fail-fast 传播错误。
    bool hadError = false;
    auto it = openUpvalues_.lower_bound(fromSlot);
    while (it != openUpvalues_.end()) {
        auto uv = it->second.lock();
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
// R164 协程/生成器：REG_YIELD 指令执行（重放模式）
// ------------------------------------------------------------
// 与 StackVM::VM::executeCoroutineOps 对称（VMCalls.cpp:1527）
// 语义：
//   - 防御性检查 yield 在非生成器函数体中出现
//   - 读取 src 寄存器值作为 yield 值
//   - 运行时 yield 执行计数器递增，用于区分循环内同一 yield 节点的多次执行
//   - 命中目标 yieldId：抛 VMYieldSignal 被 callCoroutineNext 捕获
//   - 未命中：dst = src，继续执行函数体
// ============================================================
VMResult RegisterVM::executeCoroutineOps(RegOp op, size_t& ip) {
    if (op != RegOp::REG_YIELD) {
        return runtimeError(ErrorFormat::format("未知协程操作码: %d", static_cast<int>(op)));
    }
    // 防御性检查：yield 在非生成器函数体中出现（Compiler 已拦截，但字节码注入路径可能绕过）
    if (currentCoroutineTargetYieldId_ < 0) {
        return runtimeError("yield 只能在 fun* 生成器函数体内出现");
    }
    const RegBytecodeChunk& chunk = *currentFrame().chunk;
    uint8_t dst = chunk.code[ip + 1];
    uint8_t src = chunk.code[ip + 2];
    Value yieldValue = reg(src); // 拷贝（避免 src==dst 时移动导致后续读取未定义）
    // 运行时 yield 执行计数：每次 REG_YIELD 调用递增
    int thisExecutionId = currentYieldExecutionCount_++;
    if (thisExecutionId == currentCoroutineTargetYieldId_) {
        // 命中目标 yield：抛出 VMYieldSignal，被 callCoroutineNext 捕获
        throw VMYieldSignal(std::move(yieldValue));
    }
    // thisExecutionId < target：跳过此 yield（重放模式核心）
    // dst = src 作为 yield 表达式结果，继续执行
    reg(dst) = std::move(yieldValue);
    ip += 3; // REG_YIELD 是 3 字节指令（op + dst + src）
    return VMResult::VM_OK;
}

// ============================================================
// R164 协程/生成器：生成器函数调用拦截——创建协程值
// ------------------------------------------------------------
// 与 StackVM::VM::createCoroutineValue 对称（VMCalls.cpp:1561）
// 从寄存器读参数，填充默认参数，构造 Coroutine 值写入 dstReg。
// 由 executeCallImpl 在 calleeChunk.isGenerator 命中时调用。
// 注意：ip 推进由 executeCallImpl 在调用本函数前完成（ip += returnOffset），
// 本函数只负责创建协程值并写入 dstReg。
// ============================================================
VMResult RegisterVM::createCoroutineValue(const RegBytecodeChunk& genChunk, const std::string& funName,
                                          uint8_t argCount, uint8_t dstReg, const SmallArgs<uint8_t>& argRegs,
                                          const Value* closureValue) {
    // 收集参数（寄存器顺序: argRegs[0..argCount-1]）
    std::vector<Value> args;
    args.reserve(argCount);
    for (uint8_t i = 0; i < argCount; ++i) {
        args.push_back(reg(argRegs[i]));
    }
    // 填充默认参数
    if (argCount < static_cast<uint8_t>(genChunk.arity)) {
        int missingCount = genChunk.arity - argCount;
        int defaultStartIdx = static_cast<int>(genChunk.defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx < 0) {
            return runtimeError("生成器 " + funName + " 默认参数索引越界");
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = genChunk.defaultConstIndices[i];
            if (constIdx == 0xFFFF || constIdx >= genChunk.constants.size()) {
                return runtimeError("生成器 " + funName + " 默认参数无效");
            }
            args.push_back(genChunk.constants[constIdx]);
        }
    }
    // 提取闭包值（若通过 REG_CALL_EXPR 调用生成器）
    Value closureVal = closureValue ? *closureValue : Value::nullValue();
    // 创建协程值并写入 dstReg
    Value coro = Value::makeCoroutineRegVM(&genChunk, std::move(closureVal), std::move(args), genChunk.yieldCount);
    reg(dstReg) = std::move(coro);
    return VMResult::VM_OK;
}

// ============================================================
// R164 协程/生成器：协程方法分发（.next() / .done()）
// ------------------------------------------------------------
// 与 StackVM::VM::dispatchCoroutineBuiltin 对称（VMCalls.cpp:1599）
// 协程内部状态通过 CoroutineData* 共享指针修改，无需 writeBack。
// 调用方契约：返回 true=已处理（caller 应检查 hasError_），
//             false=未匹配协程方法（caller 继续查找）。
// 由 callBuiltinMethod 在 obj.isCoroutine() 时调用。
// ============================================================
bool RegisterVM::dispatchCoroutineBuiltin(Value& obj, const std::string& methodName, SmallArgs<Value>& args,
                                          Value& result) {
    if (methodName == "next") {
        if (!args.empty()) {
            runtimeError("coroutine.next() 不接受参数");
            return true;
        }
        result = callCoroutineNext(obj);
        return !hasError_;
    }
    if (methodName == "done") {
        if (!args.empty()) {
            runtimeError("coroutine.done() 不接受参数");
            return true;
        }
        auto* cd = obj.coroutineData();
        result = Value(cd->done);
        return true;
    }
    // 未知方法
    runtimeError("coroutine 类型不支持方法 " + methodName);
    return true;
}

// ============================================================
// R164 协程/生成器：协程 .next() 重放执行
// ------------------------------------------------------------
// 与 StackVM::VM::callCoroutineNext 对称（VMCalls.cpp:1651）
// 核心思路（重放模式）：
//   1. 若已耗尽（done=true），返回 currentValueBox 中的最后值
//   2. 保存当前帧状态，为生成器 body 设置新帧
//   3. 设置 currentCoroutineTargetYieldId_ = cd->currentYieldId，重置计数器
//   4. 运行内部指令循环，REG_YIELD 命中目标时抛出 VMYieldSignal
//   5. 捕获 VMYieldSignal → 保存 yield 值，递增 currentYieldId
//   6. 函数体自然结束（REG_RETURN）→ 标记 done=true，返回 return 值
//   7. 恢复帧状态，返回 yield/return 值
// ============================================================
Value RegisterVM::callCoroutineNext(Value& coroVal) {
    auto* cd = coroVal.coroutineData();

    // 已耗尽：返回最后一次 yield/return 的值
    if (cd->done) {
        return cd->currentValueBox.empty() ? Value::nullValue() : cd->currentValueBox.front();
    }

    const RegBytecodeChunk* genChunk = cd->regChunk;
    if (!genChunk) {
        runtimeError("协程缺少生成器字节码块");
        return Value::nullValue();
    }

    // MAX_FRAMES 检查
    if (frames_.size() >= MAX_FRAMES) {
        runtimeError(ErrorFormat::format(ErrorMessages::kRecursionDepthExceededFmt, static_cast<int>(MAX_FRAMES)));
        return Value::nullValue();
    }

    // 保存调用方上下文
    size_t savedFrameCount = frames_.size();
    int savedTargetYieldId = currentCoroutineTargetYieldId_;
    int savedYieldExecCount = currentYieldExecutionCount_;

    // 构造调用帧
    RegCallFrame newFrame;
    newFrame.chunk = genChunk;
    newFrame.ip = 0;
    newFrame.returnIp = currentFrame().ip;
    newFrame.returnReg = -1; // 重放模式不通过 returnReg 写回，直接捕获 REG_RETURN 值
    newFrame.registerCount = static_cast<uint8_t>(
        genChunk->registerCount <= RegCallFrame::MAX_REGISTERS ? genChunk->registerCount : RegCallFrame::MAX_REGISTERS);

    // 填充参数到寄存器 R0..R(arity-1)
    uint8_t argCount =
        static_cast<uint8_t>(std::min(cd->args.size(), static_cast<size_t>(std::numeric_limits<uint8_t>::max())));
    for (uint8_t i = 0; i < argCount && i < newFrame.registerCount; ++i) {
        newFrame.registers[i] = cd->args[i];
    }
    // 填充默认参数
    if (argCount < static_cast<uint8_t>(genChunk->arity)) {
        int missingCount = genChunk->arity - argCount;
        int defaultStartIdx = static_cast<int>(genChunk->defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx >= 0) {
            for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                uint8_t regIdx = static_cast<uint8_t>(argCount + (i - defaultStartIdx));
                if (regIdx >= newFrame.registerCount)
                    break;
                uint16_t constIdx = genChunk->defaultConstIndices[i];
                if (constIdx != 0xFFFF && constIdx < genChunk->constants.size()) {
                    newFrame.registers[regIdx] = genChunk->constants[constIdx];
                }
            }
        }
    }
    // 剩余寄存器初始化为 null（定长 array 默认初始化已为 null，此处显式确保）
    for (uint8_t i = argCount; i < newFrame.registerCount; ++i) {
        if (i >= genChunk->arity) {
            newFrame.registers[i] = Value::nullValue();
        }
    }
    // 绑定闭包 upvalues（与 StackVM 对称）
    if (!cd->vmClosureBox.empty()) {
        auto& closureVal = cd->vmClosureBox.front();
        if (closureVal.isClosure() && closureVal.vmClosure()) {
            newFrame.upvalues = closureVal.vmClosure()->upvalues;
        }
    }
    frames_.push_back(std::move(newFrame));

    // 设置协程重放上下文
    currentCoroutineTargetYieldId_ = cd->currentYieldId;
    currentYieldExecutionCount_ = 0;

    Value result = Value::nullValue();
    bool needCleanup = false;  // true = 需手动清理帧（yield 信号或错误路径）
    bool normalReturn = false; // true = 函数体自然结束（REG_RETURN）

    try {
        // 内部指令循环：执行直到生成器帧弹出
        int64_t localInstrCount = 0;
        int64_t maxInstr = RuntimeLimits::RuntimeConfig::instance().maxInstructions();
        while (frames_.size() > savedFrameCount && !hasError_) {
            if (++localInstrCount > maxInstr) {
                runtimeError("指令执行数超过上限，疑似无限循环");
                break;
            }
            VMResult r = executeOneInstruction();
            if (r != VMResult::VM_OK || hasError_) {
                break;
            }
        }

        if (hasError_) {
            needCleanup = true;
        } else {
            // 正常结束：REG_RETURN 已弹出帧。
            // R164 D.7 fix: executeReturnImpl 在 returnReg<0 时将返回值保存到 coroutineReturnValue_ 邮箱，
            // 此处读取以获取生成器函数体 return 语句的值（对齐 StackVM 的 OP_RETURN push 到栈语义）。
            result = std::move(coroutineReturnValue_);
            coroutineReturnValue_ = Value::nullValue(); // 清理邮箱
            normalReturn = true;
        }
    } catch (const VMYieldSignal& e) {
        // 命中目标 yield：保存 yield 值，递增 currentYieldId
        result = std::move(e.yieldValue);
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
        cd->currentYieldId++;
        // 若递增后达到 yieldCount，标记 done（下次 .next() 将返回 currentValue）
        if (cd->currentYieldId >= cd->yieldCount) {
            cd->done = true;
        }
        needCleanup = true; // 生成器帧仍在 frames_ 上，需手动清理
    }

    // 函数体自然结束：标记 done，缓存返回值作为最后一次 yield/return 的值
    if (normalReturn) {
        cd->done = true;
        cd->currentValueBox.clear();
        cd->currentValueBox.push_back(result);
    }

    // 手动清理（yield 信号或错误路径）：生成器帧可能仍在 frames_ 上
    if (needCleanup) {
        while (frames_.size() > savedFrameCount) {
            frames_.pop_back();
        }
        // 清理 try handler
        while (!tryStack_.empty() && tryStack_.back().frameIndex >= savedFrameCount) {
            tryStack_.pop_back();
        }
        // 关闭 open upvalues 指向生成器栈区域（按寄存器编码 frameIdx*32+slot）
        // savedFrameCount 之前的帧的 upvalues 保留，之后的清理
        closeUpvaluesFrom(savedFrameCount * RegCallFrame::MAX_REGISTERS);
    }

    // 恢复协程上下文
    currentCoroutineTargetYieldId_ = savedTargetYieldId;
    currentYieldExecutionCount_ = savedYieldExecCount;
    return result;
}
