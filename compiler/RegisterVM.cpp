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
#include "common/Logger.h"
#include "common/TypeChecker.h" // 2026-06-29: typeMatchValue（REG_TYPE_CHECK）
#include "common/Utf8Utils.h"
#include "compiler/RegisterBytecode.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/ErrorFormat.h" // P3 fix: runtimeErrorFmt 替代 std::to_string 拼接
#include "interpreter/NumericUtils.h"
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
    openUpvalues_.clear();
    tryStack_.clear();
    pendingException_ = Value::nullValue(); // P1-4 fix: 清理异常值
    pendingJumpStack_.clear();               // AUDIT-P1.1 fix: 清理续跳栈
    hasError_ = false;
    lastError_.clear();
    lastErrorLine_ = 0;
    diagnostics_.clear();
    instructionCount_ = 0;
    initialized_ = false;
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
        if (++instructionCount_ >= MAX_INSTRUCTIONS) {
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
    if (++instructionCount_ >= MAX_INSTRUCTIONS) {
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
    case RegOp::REG_INDEX_GET:
    case RegOp::REG_INDEX_SET:
    case RegOp::REG_MEMBER_GET:
    case RegOp::REG_MEMBER_SET:
    case RegOp::REG_SUPER_MEMBER_GET:
    case RegOp::REG_INIT_FIELD:
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
    case RegOp::REG_PUSH_JUMP_TARGET:
    case RegOp::REG_FINALLY_END:
        return executeMisc(op, ip);

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
    case RegOp::REG_STORE_GLOBAL:
    case RegOp::REG_DEFINE_GLOBAL: {
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
            // RegisterVM 无独立字段槽，字段直接存在 instance.fields() 中，
            // 故只需检查 slot == 0（this 槽）。
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

VMResult RegisterVM::executeContainers(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

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
    case RegOp::REG_BUILD_DICT: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t pairCount = chunk.code[ip + 2];
        std::unordered_map<std::string, Value> entries;
        for (uint8_t i = 0; i < pairCount; ++i) {
            // C-7 fix: 原代码漏加 ip 偏移，从 chunk 起始处读取操作数，
            // 导致非 offset-0 的字典字面量读取错误字节（越界/类型错误）。
            size_t base = ip + 3 + i * 2;
            uint8_t keyReg = chunk.code[base];
            uint8_t valReg = chunk.code[base + 1];
            const Value& keyVal = reg(keyReg);
            if (!keyVal.isString()) {
                return runtimeError("字典键必须是字符串");
            }
            entries[keyVal.stringVal()] = reg(valReg);
        }
        reg(dst) = Value(std::move(entries));
        ip += 3 + pairCount * 2;
        break;
    }
    case RegOp::REG_INDEX_GET: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t objReg = chunk.code[ip + 2];
        uint8_t idxReg = chunk.code[ip + 3];
        const Value& obj = reg(objReg);
        const Value& idx = reg(idxReg);
        if (obj.isArray()) {
            if (!idx.isInt())
                return runtimeError("数组索引必须是整数");
            int64_t i = idx.intVal();
            const auto& arr = obj.arrayVal();
            // P1-6 fix: 对齐 Interpreter/栈式VM——负索引直接报错，不做 Python 式 wraparound
            if (i < 0 || i >= static_cast<int64_t>(arr.size())) {
                return runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                                                        static_cast<long long>(i), arr.size()));
            }
            reg(dst) = arr[static_cast<size_t>(i)];
        } else if (obj.isDict()) {
            if (!idx.isString())
                return runtimeError("字典键必须是字符串");
            const auto& dict = obj.dictVal();
            auto it = dict.find(idx.stringVal());
            // P1-7 fix: 对齐 Interpreter/栈式VM——字典键不存在时返回 null 而非 throw
            if (it == dict.end()) {
                reg(dst) = Value::nullValue();
            } else {
                reg(dst) = it->second;
            }
        } else if (obj.isString()) {
            if (!idx.isInt())
                return runtimeError("字符串索引必须是整数");
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
        } else {
            return runtimeError("该类型不支持索引访问");
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
                return runtimeError("数组索引必须是整数");
            int64_t i = idx.intVal();
            auto& arr = obj.arrayVal();
            // P1-6 fix: 对齐 Interpreter/栈式VM——负索引直接报错
            if (i < 0 || i >= static_cast<int64_t>(arr.size())) {
                return runtimeError(ErrorFormat::format("数组索引越界: %lld, 有效范围 [0, %zu)",
                                                        static_cast<long long>(i), arr.size()));
            }
            arr[static_cast<size_t>(i)] = val;
        } else if (obj.isDict()) {
            if (!idx.isString())
                return runtimeError("字典键必须是字符串");
            obj.dictVal()[idx.stringVal()] = val;
        } else {
            return runtimeError("不支持索引赋值的类型");
        }
        // C-1 fix: 记录接收者寄存器，供紧随其后的 WRITEBACK_* 读取变异后的容器
        lastMutatedReceiverReg_ = objReg;
        ip += 4;
        break;
    }
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
            auto it = dict.find(fieldName);
            // P1-7 fix: 对齐 Interpreter/栈式VM——字典键不存在时返回 null 而非 throw
            if (it == dict.end()) {
                reg(dst) = Value::nullValue();
            } else {
                reg(dst) = it->second;
            }
        } else {
            return runtimeError("该类型不支持成员访问");
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
            obj.dictVal()[fieldName] = val;
        } else {
            return runtimeError("不支持成员赋值的类型");
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
        // P1-1 fix: fieldOrder 已在 REG_DEFINE_CLASS 中从类元数据完整填充（见 executeCalls），
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
        return runtimeError("executeContainers: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 调用指令（占位实现，需完整实现）
// ============================================================

VMResult RegisterVM::executeCalls(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

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
        for (auto& kv : classInfo_) {
            if (kv.first == className)
                continue; // 跳过当前类
            std::string cur = kv.second.parent;
            for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
                if (cur == className) {
                    kv.second.flattenedComputed = false;
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
        return runtimeError("executeCalls: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 其他指令
// ============================================================

VMResult RegisterVM::executeMisc(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

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
            return runtimeError(ErrorFormat::format("类型注解违反: 期望类型 %s，实际为 %s", annotation.c_str(),
                                                    val.typeName().c_str()));
        }
        ip += 4; // op(1B) + src(1B) + typeIdx(2B)
        break;
    }
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
        VMResult cr = executeCallImpl(newIp, *foundFunNamePtr, argCount + 1, dst, fullArgRegs, 8u + argCount);
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
        return runtimeError("executeMisc: 未知操作码");
    }

    if (stepCallbackEnabled_ && stepCallback_) {
        stepCallback_({ip, op, frames_.size()});
    }
    return VMResult::VM_OK;
}

// ============================================================
// 函数调用实现
// ============================================================

VMResult RegisterVM::executeCallImpl(size_t& ip, const std::string& funName, uint8_t argCount, uint8_t dstReg,
                                     const SmallArgs<uint8_t>& argRegs, size_t returnOffset,
                                     const Value* closureValue) {
    if (frames_.size() >= MAX_FRAMES) {
        return runtimeError("调用栈溢出");
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
    const RegBytecodeChunk* cachedChunk = nullptr;
    const std::string* namePtr = &funName;
    if (!closureValue) {
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

            auto result = executeSharedBuiltinFunction(funName, args.data(), argCount, line, 0);
            if (result.is_ok()) {
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
            return runtimeError("未定义的函数: " + funName);
        }
    }

    // PERF-AUDIT-5: 缓存未命中时写入缓存（仅 REG_CALL 路径，funName 来自常量池）
    if (!cachedChunk && !closureValue) {
        callCache_[namePtr] = &it->second;
    }

    const RegBytecodeChunk& calleeChunk = cachedChunk ? *cachedChunk : it->second;

    // C-2 fix: 从闭包值提取 upvalues（若有），填入新帧供 LOAD/STORE_UPVALUE 访问
    auto populateUpvalues = [&closureData](RegCallFrame& newFrame) {
        if (closureData && !closureData->upvalues.empty()) {
            newFrame.upvalues = closureData->upvalues;
        }
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
            return runtimeError(ErrorFormat::format("函数 %s 期望 %d-%d 个参数，但传入了 %d 个", funName.c_str(),
                                                    static_cast<int>(calleeChunk.requiredArity),
                                                    static_cast<int>(calleeChunk.arity), static_cast<int>(argCount)));
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
        return runtimeError(ErrorFormat::format("函数 %s 期望 %d-%d 个参数，但传入了 %d 个", funName.c_str(),
                                                static_cast<int>(calleeChunk.requiredArity),
                                                static_cast<int>(calleeChunk.arity), static_cast<int>(argCount)));
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
        // P1-1 fix: 沿继承链查找方法（子类优先，找不到则查父类）
        std::string searchClass = className;
        for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
            auto classIt = classInfo_.find(searchClass);
            if (classIt == classInfo_.end())
                break;
            auto methodIt = classIt->second.methods.find(methodName);
            if (methodIt != classIt->second.methods.end()) {
                const std::string& funName = methodIt->second;
                // 构造参数列表（this 作为第一个参数）
                SmallArgs<uint8_t> fullArgRegs;
                fullArgRegs.push_back(objReg);
                for (uint8_t i = 0; i < argCount; ++i) {
                    fullArgRegs.push_back(argRegs[i]);
                }
                // 调用方法函数
                size_t newIp = ip;
                // C-8 fix: REG_METHOD_CALL 指令长度 = 6 + argCount（argCount 不含 this）。
                // executeCallImpl 收到 argCount+1（含 this），但 returnOffset 用原始 argCount 计算。
                // 防御性检查：argCount+1 溢出 uint8_t（backend 应已在编译期拦截）
                if (argCount >= 255)
                    return runtimeError("方法调用参数数量超过上限");
                VMResult cr = executeCallImpl(newIp, funName, argCount + 1, dstReg, fullArgRegs, 6u + argCount);
                if (cr != VMResult::VM_OK)
                    return cr;
                // 标记为方法调用（用于字段同步）
                // P0-1/P1-5 fix: 对齐栈式 VM (VMCalls.cpp:734)——所有 init 调用都设置 isInitCall，
                // 使 executeReturnImpl 在多层 super.init() 链中即使 fieldsModified=false 也捕获 methodThis 并同步。
                if (!frames_.empty()) {
                    frames_.back().isMethodCall = true;
                    frames_.back().isInitCall = (methodName == "init");
                    frames_.back().receiverReg = objReg;
                    frames_.back().receiverVarName.clear();
                }
                ip = newIp;
                return VMResult::VM_OK;
            }
            searchClass = classIt->second.parent;
        }
        // BUG-INH-3 fix: 错误消息与 Interpreter/StackVM 一致（"类 X 没有方法 Y"）
        return runtimeError("类 " + className + " 没有方法 " + methodName);
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
        VMResult r = executeCallImpl(newIp, info.resolvedInitFunName, argCount + 1, dstReg, fullArgRegs, 5u + argCount);
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

bool RegisterVM::callBuiltinMethod(Value& obj, const std::string& methodName, SmallArgs<Value>& args, Value& result) {
    // #20 fix: 复用共享 classifyBuiltinMethod 枚举分发，消除长串字符串比较。
    // 原实现按 obj 类型分支后逐个 if (methodName == "...")，最坏需 N 次字符串比较。
    // 改为单次 classify + switch 分发，与栈式 VM 行为一致。
    BuiltinMethod method = classifyBuiltinMethod(methodName);
    if (method == BuiltinMethod::UNKNOWN)
        return false;

    if (obj.isArray()) {
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
    } else if (obj.isDict()) {
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
            obj.dictVal().erase(args[0].toString());
            result = Value::nullValue();
            return true;
        }
        case BuiltinMethod::DICT_SET: {
            if (args.size() != 2) {
                runtimeError("set 期望 2 个参数(键, 值)");
                return true;
            }
            obj.dictVal()[args[0].toString()] = args[1];
            result = Value::nullValue();
            return true;
        }
        default:
            // 2026-06-29 BUG-1 fix: 已知内置方法但不适用于字典（如 arr.push 对字典调用）
            runtimeError("字典没有方法 " + methodName);
            return true;
        }
    } else if (obj.isString()) {
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
                                 std::to_string(targetFrame.registerCount) + " (frameIdx=" + std::to_string(frameIdx) +
                                 ")");
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
    // 遍历 chunk 的 localRegNames，从寄存器窗口反查值
    const auto& names = frame.chunk->localRegNames;
    for (size_t reg = 0; reg < names.size() && reg < frame.registers.size(); ++reg) {
        if (names[reg].empty())
            continue;
        result[names[reg]] = frame.registers[reg];
    }
    return result;
}
