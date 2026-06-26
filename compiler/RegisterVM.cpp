// ============================================================
// RegisterVM.cpp — PERF-14: 寄存器式虚拟机实现
// ------------------------------------------------------------
// 执行 RegBytecodeChunk。核心指令：常量加载、算术比较、控制流、
// 全局/局部变量、函数调用、返回、容器、成员访问、闭包、类。
// ============================================================

#include "compiler/RegisterVM.h"
#include "compiler/RegisterBytecode.h"
#include "interpreter/NumericUtils.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/RuntimeExceptions.h"
#include "common/Utf8Utils.h"
#include "common/Logger.h"
#include <cassert>
#include <cmath>
#include <sstream>
#include <algorithm>

// ============================================================
// 构造 / 重置
// ============================================================

RegisterVM::RegisterVM() = default;

void RegisterVM::resetState() {
    frames_.clear();
    mainChunk_ = RegBytecodeChunk{};
    functionChunks_.clear();
    globals_.clear();
    globalSlots_.clear();
    globalNameToSlot_.clear();
    functionClosures_.clear();
    classInfo_.clear();
    openUpvalues_.clear();
    tryStack_.clear();
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

    // 初始化全局槽位
    globalSlots_.resize(result.globalSlotCount);
    for (size_t i = 0; i < result.globalSlotNames.size() && i < globalSlots_.size(); ++i) {
        globalNameToSlot_[result.globalSlotNames[i]] = static_cast<int>(i);
    }

    // 创建 main 帧
    RegCallFrame mainFrame;
    mainFrame.chunk = &mainChunk_;
    mainFrame.ip = 0;
    mainFrame.registers.resize(mainChunk_.registerCount);
    frames_.push_back(std::move(mainFrame));

    initialized_ = true;
}

VMResult RegisterVM::execute(const RegisterCompileResult& result) {
    initExecution(result);
    while (!frames_.empty() && !hasError_) {
        VMResult r = executeOneInstruction();
        if (r != VMResult::VM_OK) return r;
        // DoS 防护
        if (++instructionCount_ >= MAX_INSTRUCTIONS) {
            return runtimeError("指令数超出上限（可能存在死循环）");
        }
    }
    return hasError_ ? VMResult::VM_RUNTIME_ERROR : VMResult::VM_OK;
}

VMResult RegisterVM::stepOnce() {
    if (!initialized_) return runtimeError("VM 未初始化");
    if (frames_.empty()) return VMResult::VM_OK;
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;

    // 累计指令计数（防止通过循环调用 stepOnce 绕过 DoS 防护）
    if (++instructionCount_ >= MAX_INSTRUCTIONS) {
        return runtimeError("指令数超出上限（可能存在死循环）");
    }
    return executeOneInstruction();
}

bool RegisterVM::isFinished() const {
    return frames_.empty() || hasError_;
}

// ============================================================
// 辅助方法
// ============================================================

Value& RegisterVM::reg(uint8_t r) {
    assert(!frames_.empty() && "RegisterVM::reg() on empty frames");
    auto& frame = frames_.back();
    assert(r < frame.registers.size() && "寄存器号越界");
    return frame.registers[r];
}

const Value& RegisterVM::reg(uint8_t r) const {
    assert(!frames_.empty() && "RegisterVM::reg() on empty frames");
    const auto& frame = frames_.back();
    assert(r < frame.registers.size() && "寄存器号越界");
    return frame.registers[r];
}

VMResult RegisterVM::runtimeError(const std::string& msg) {
    hasError_ = true;
    lastError_ = msg;
    // 获取当前行号
    if (!frames_.empty()) {
        const auto& frame = frames_.back();
        if (frame.chunk && frame.ip < frame.chunk->lines.size()) {
            lastErrorLine_ = frame.chunk->lines[frame.ip];
        }
    }
    diagnostics_.addError(msg, lastErrorLine_, 0, DiagSource::VM);
    Logger::Error("RegisterVM: " + msg, "RegVM");
    return VMResult::VM_RUNTIME_ERROR;
}

// ============================================================
// 单步指令分发
// ============================================================

VMResult RegisterVM::executeOneInstruction() {
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;
    if (frames_.empty()) return VMResult::VM_OK;

    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;
    size_t& ip = frame.ip;

    if (ip >= chunk.code.size()) {
        return runtimeError("指令指针越界");
    }

    RegOp op = static_cast<RegOp>(chunk.code[ip]);
    uint8_t instrSize = chunk.instructionSizeAt(ip);
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
    case RegOp::REG_PRINT:
    case RegOp::REG_WRITEBACK_MEMBER_VAR:
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL:
    case RegOp::REG_WRITEBACK_INDEX_VAR:
    case RegOp::REG_WRITEBACK_INDEX_LOCAL:
    case RegOp::REG_SUPER_CALL:
        return executeMisc(op, ip);

    default:
        return runtimeError("未知寄存器操作码: " + std::to_string(static_cast<int>(op)));
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
                return runtimeError("整数溢出");
            }
            reg(dst) = Value(-v.intVal());
        } else if (v.isFloat()) {
            reg(dst) = Value(-v.floatVal());
        } else {
            return runtimeError("一元负号需要数值类型");
        }
        ip += 3;
    } else {
        // 二元算术
        uint8_t dst = chunk.code[ip + 1];
        uint8_t s1 = chunk.code[ip + 2];
        uint8_t s2 = chunk.code[ip + 3];
        const Value& a = reg(s1);
        const Value& b = reg(s2);

        if (!a.isNumber() || !b.isNumber()) {
            return runtimeError("算术运算需要数值类型");
        }

        Value result;
        switch (op) {
        case RegOp::REG_ADD: {
            if (a.isInt() && b.isInt()) {
                if (OverflowCheck::addOverflow(a.intVal(), b.intVal())) {
                    return runtimeError("整数加法溢出");
                }
                result = Value(a.intVal() + b.intVal());
            } else {
                result = Value(a.toDouble() + b.toDouble());
            }
            break;
        }
        case RegOp::REG_SUB: {
            if (a.isInt() && b.isInt()) {
                if (OverflowCheck::subOverflow(a.intVal(), b.intVal())) {
                    return runtimeError("整数减法溢出");
                }
                result = Value(a.intVal() - b.intVal());
            } else {
                result = Value(a.toDouble() - b.toDouble());
            }
            break;
        }
        case RegOp::REG_MUL: {
            if (a.isInt() && b.isInt()) {
                if (OverflowCheck::mulOverflow(a.intVal(), b.intVal())) {
                    return runtimeError("整数乘法溢出");
                }
                result = Value(a.intVal() * b.intVal());
            } else {
                result = Value(a.toDouble() * b.toDouble());
            }
            break;
        }
        case RegOp::REG_DIV: {
            if (b.isInt() && b.intVal() == 0) {
                return runtimeError("除零错误");
            }
            if (b.isFloat() && b.floatVal() == 0.0) {
                return runtimeError("除零错误");
            }
            if (a.isInt() && b.isInt() && a.intVal() % b.intVal() == 0) {
                result = Value(a.intVal() / b.intVal());
            } else {
                result = Value(a.toDouble() / b.toDouble());
            }
            break;
        }
        case RegOp::REG_MOD: {
            if (b.isInt() && b.intVal() == 0) {
                return runtimeError("模零错误");
            }
            if (a.isInt() && b.isInt()) {
                // 检查溢出（INT64_MIN % -1）
                if (a.intVal() == INT64_MIN && b.intVal() == -1) {
                    return runtimeError("整数模运算溢出");
                }
                result = Value(a.intVal() % b.intVal());
            } else {
                result = Value(std::fmod(a.toDouble(), b.toDouble()));
            }
            break;
        }
        default:
            return runtimeError("executeArith: 未知操作码");
        }
        reg(dst) = std::move(result);
        ip += 4;
    }

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
        if (!v.isBool()) {
            return runtimeError("逻辑非需要布尔类型");
        }
        reg(dst) = Value(!v.boolVal());
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
        case RegOp::REG_EQ:  result = a.equals(b); break;
        case RegOp::REG_NEQ: result = !a.equals(b); break;
        case RegOp::REG_LT:
            if (isStringCompare) result = a.stringVal() < b.stringVal();
            else if (a.isNumber() && b.isNumber()) result = a.toDouble() < b.toDouble();
            else return runtimeError("比较运算需要数值或字符串类型");
            break;
        case RegOp::REG_GT:
            if (isStringCompare) result = a.stringVal() > b.stringVal();
            else if (a.isNumber() && b.isNumber()) result = a.toDouble() > b.toDouble();
            else return runtimeError("比较运算需要数值或字符串类型");
            break;
        case RegOp::REG_LTE:
            if (isStringCompare) result = a.stringVal() <= b.stringVal();
            else if (a.isNumber() && b.isNumber()) result = a.toDouble() <= b.toDouble();
            else return runtimeError("比较运算需要数值或字符串类型");
            break;
        case RegOp::REG_GTE:
            if (isStringCompare) result = a.stringVal() >= b.stringVal();
            else if (a.isNumber() && b.isNumber()) result = a.toDouble() >= b.toDouble();
            else return runtimeError("比较运算需要数值或字符串类型");
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
                    return runtimeError("未定义的全局变量: " + name);
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
            // 从调用者帧的寄存器读取
            if (frames_.size() >= 2) {
                auto& callerFrame = frames_[frames_.size() - 2];
                if (uv->stackSlot < callerFrame.registers.size()) {
                    reg(dst) = callerFrame.registers[uv->stackSlot];
                } else {
                    return runtimeError("upvalue 栈槽越界");
                }
            } else {
                return runtimeError("upvalue 无调用者帧");
            }
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
            if (frames_.size() >= 2) {
                auto& callerFrame = frames_[frames_.size() - 2];
                if (uv->stackSlot < callerFrame.registers.size()) {
                    callerFrame.registers[uv->stackSlot] = reg(src);
                }
            }
        }
        ip += 3;
        break;
    }
    case RegOp::REG_CLOSE_UPVALUE: {
        uint8_t uvIdx = chunk.code[ip + 1];
        if (uvIdx >= frame.upvalues.size()) {
            return runtimeError("upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        if (!uv->isClosed) {
            if (frames_.size() >= 2) {
                auto& callerFrame = frames_[frames_.size() - 2];
                if (uv->stackSlot < callerFrame.registers.size()) {
                    uv->value = callerFrame.registers[uv->stackSlot];
                }
            }
            uv->isClosed = true;
        }
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
        ip = offset;
        break;
    }
    case RegOp::REG_JUMP_IF_FALSE: {
        uint8_t src = chunk.code[ip + 1];
        uint16_t offset = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        const Value& v = reg(src);
        if (v.isBool() && !v.boolVal()) {
            ip = offset;
        } else if (!v.isBool()) {
            return runtimeError("条件跳转需要布尔类型");
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
            size_t base = 3 + i * 2;
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
            if (!idx.isInt()) return runtimeError("数组索引必须是整数");
            int64_t i = idx.intVal();
            const auto& arr = obj.arrayVal();
            if (i < 0) i += arr.size();
            if (i < 0 || i >= static_cast<int64_t>(arr.size())) {
                return runtimeError("数组索引越界");
            }
            reg(dst) = arr[static_cast<size_t>(i)];
        } else if (obj.isDict()) {
            if (!idx.isString()) return runtimeError("字典键必须是字符串");
            const auto& dict = obj.dictVal();
            auto it = dict.find(idx.stringVal());
            if (it == dict.end()) return runtimeError("键不存在: " + idx.stringVal());
            reg(dst) = it->second;
        } else if (obj.isString()) {
            if (!idx.isInt()) return runtimeError("字符串索引必须是整数");
            int64_t i = idx.intVal();
            const auto& str = obj.stringVal();
            int64_t len = Utf8::codepointCount(str);
            if (i < 0) i += len;
            if (i < 0 || i >= len) {
                return runtimeError("字符串索引越界");
            }
            size_t bytePos = Utf8::codepointToByteIndex(str, i);
            int charLen = Utf8::byteLength(static_cast<unsigned char>(str[bytePos]));
            reg(dst) = Value(str.substr(bytePos, static_cast<size_t>(charLen)));
        } else {
            return runtimeError("不支持索引访问的类型");
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
            if (!idx.isInt()) return runtimeError("数组索引必须是整数");
            int64_t i = idx.intVal();
            auto& arr = obj.arrayVal();
            if (i < 0) i += arr.size();
            if (i < 0 || i >= static_cast<int64_t>(arr.size())) {
                return runtimeError("数组索引越界");
            }
            arr[static_cast<size_t>(i)] = val;
        } else if (obj.isDict()) {
            if (!idx.isString()) return runtimeError("字典键必须是字符串");
            obj.dictVal()[idx.stringVal()] = val;
        } else {
            return runtimeError("不支持索引赋值的类型");
        }
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
            if (it == fields.end()) return runtimeError("字段不存在: " + fieldName);
            reg(dst) = it->second;
        } else if (obj.isDict()) {
            const auto& dict = obj.dictVal();
            auto it = dict.find(fieldName);
            if (it == dict.end()) return runtimeError("键不存在: " + fieldName);
            reg(dst) = it->second;
        } else {
            return runtimeError("不支持成员访问的类型");
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
        ip += 5;
        break;
    }
    case RegOp::REG_INIT_FIELD: {
        uint16_t fieldIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (fieldIdx >= chunk.constants.size() || !chunk.constants[fieldIdx].isString()) {
            return runtimeError("字段名索引无效");
        }
        // 记录字段顺序（用于 init 方法）
        // 简化：暂不实现 fieldOrder 跟踪
        ip += 3;
        break;
    }
    case RegOp::REG_SUPER_MEMBER_GET: {
        // 简化：与 REG_MEMBER_GET 相同（未来需查找父类方法）
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
            if (it == fields.end()) return runtimeError("字段不存在: " + fieldName);
            reg(dst) = it->second;
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
        return executeReturnImpl(ip, std::move(result));
    }
    case RegOp::REG_RETURN_NULL: {
        return executeReturnImpl(ip, Value::nullValue());
    }
    case RegOp::REG_CALL: {
        uint8_t dst = chunk.code[ip + 1];
        uint16_t nameIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        uint8_t argCount = chunk.code[ip + 4];
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("函数名索引无效");
        }
        const std::string& funName = chunk.constants[nameIdx].stringVal();
        std::vector<uint8_t> argRegs;
        argRegs.reserve(argCount);
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 5 + i]);
        }
        size_t newIp = ip;  // 保存当前 ip（调用返回后更新）
        VMResult r = executeCallImpl(newIp, funName, argCount, dst, argRegs);
        if (r != VMResult::VM_OK) return r;
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
        std::vector<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 4 + i]);
        }
        // 简化：通过闭包名查找函数 chunk
        const std::string& funName = callee.closureName();
        size_t newIp = ip;
        VMResult r = executeCallImpl(newIp, funName, argCount, dst, argRegs);
        if (r != VMResult::VM_OK) return r;
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
        std::vector<uint8_t> uvSpecs;
        for (uint8_t i = 0; i < uvCount; ++i) {
            uvSpecs.push_back(chunk.code[ip + 5 + i * 2]);       // isLocal
            uvSpecs.push_back(chunk.code[ip + 5 + i * 2 + 1]);   // idx
        }
        size_t newIp = ip;
        VMResult r = executeClosureImpl(newIp, name, uvCount, uvSpecs, dst);
        if (r != VMResult::VM_OK) return r;
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
        std::vector<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 6 + i]);
        }
        size_t newIp = ip;
        VMResult r = executeMethodCallImpl(newIp, methodName, argCount, dst, objReg, argRegs);
        if (r != VMResult::VM_OK) return r;
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
        std::vector<uint8_t> argRegs;
        for (uint8_t i = 0; i < argCount; ++i) {
            argRegs.push_back(chunk.code[ip + 5 + i]);
        }
        size_t newIp = ip;
        VMResult r = executeClassNewImpl(newIp, className, argCount, dst, argRegs);
        if (r != VMResult::VM_OK) return r;
        ip = newIp;
        break;
    }
    case RegOp::REG_DEFINE_CLASS: {
        uint16_t nameIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
            return runtimeError("类名索引无效");
        }
        const std::string& className = chunk.constants[nameIdx].stringVal();
        // 注册类（简化：仅记录类名，方法定义在函数 chunk 中）
        classInfo_[className].name = className;
        ip += 3;
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
        tryStack_.push_back({catchOffset, frames_.size() - 1});
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
        return throwException(std::move(thrown));
    }
    case RegOp::REG_WRITEBACK_MEMBER_VAR:
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL:
    case RegOp::REG_WRITEBACK_INDEX_VAR:
    case RegOp::REG_WRITEBACK_INDEX_LOCAL:
    case RegOp::REG_SUPER_CALL:
        // 简化：暂不实现完整写回逻辑
        return runtimeError("写回/super 指令未实现");
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

VMResult RegisterVM::executeCallImpl(size_t& ip, const std::string& funName,
                                     uint8_t argCount, uint8_t dstReg,
                                     const std::vector<uint8_t>& argRegs) {
    if (frames_.size() >= MAX_FRAMES) {
        return runtimeError("调用栈溢出");
    }

    // 查找函数 chunk
    auto it = functionChunks_.find(funName);
    if (it == functionChunks_.end()) {
        // 查找闭包
        auto closureIt = functionClosures_.find(funName);
        if (closureIt != functionClosures_.end()) {
            // 闭包调用：使用绑定的函数 chunk
            const Value& closure = closureIt->second;
            if (closure.isClosure()) {
                const std::string& closureFunName = closure.closureName();
                auto cit = functionChunks_.find(closureFunName);
                if (cit != functionChunks_.end()) {
                    it = cit;
                }
            }
        }
        if (it == functionChunks_.end()) {
            // 检查内建函数
            std::vector<Value> args;
            args.reserve(argCount);
            for (uint8_t i = 0; i < argCount; ++i) {
                args.push_back(reg(argRegs[i]));
            }
            auto result = executeSharedBuiltinFunction(funName, args.data(), argCount);
            if (result.is_ok()) {
                reg(dstReg) = result.value();
                // 内建函数同步返回，ip 前进到下一条指令
                const RegBytecodeChunk& chunk = *currentFrame().chunk;
                ip += 5 + argCount;
                return VMResult::VM_OK;
            }
            return runtimeError("未定义的函数: " + funName);
        }
    }

    const RegBytecodeChunk& calleeChunk = it->second;

    // 检查参数数量
    if (argCount < calleeChunk.requiredArity) {
        // 尝试填充默认参数
        uint8_t adjustedArgCount = argCount;
        std::vector<Value> defaults;
        if (!fillDefaultArgs(calleeChunk, adjustedArgCount, funName, defaults)) {
            return runtimeError("参数数量不足: " + funName + " 需要 " +
                               std::to_string(calleeChunk.requiredArity) + " 个");
        }
        // 创建新帧
        RegCallFrame newFrame;
        newFrame.chunk = &calleeChunk;
        newFrame.ip = 0;
        newFrame.returnIp = ip + 5 + argCount;
        newFrame.returnReg = dstReg;
        newFrame.registers.resize(calleeChunk.registerCount);
        // 填充参数
        for (uint8_t i = 0; i < argCount; ++i) {
            if (i < newFrame.registers.size()) {
                newFrame.registers[i] = reg(argRegs[i]);
            }
        }
        for (uint8_t i = argCount; i < adjustedArgCount; ++i) {
            if (i < newFrame.registers.size() && i - argCount < defaults.size()) {
                newFrame.registers[i] = defaults[i - argCount];
            }
        }
        frames_.push_back(std::move(newFrame));
        return VMResult::VM_OK;
    }

    // 创建新帧
    RegCallFrame newFrame;
    newFrame.chunk = &calleeChunk;
    newFrame.ip = 0;
    newFrame.returnIp = ip + 5 + argCount;
    newFrame.returnReg = dstReg;
    newFrame.registers.resize(calleeChunk.registerCount);

    // 填充参数
    for (uint8_t i = 0; i < argCount && i < newFrame.registers.size(); ++i) {
        newFrame.registers[i] = reg(argRegs[i]);
    }

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
    bool fieldsModified = frames_.back().fieldsModified;
    int receiverReg = frames_.back().receiverReg;
    std::string receiverVarName = std::move(frames_.back().receiverVarName);

    size_t returningFrameIdx = frames_.size() - 1;
    frames_.pop_back();

    // 清理属于返回帧的 tryStack_ handler
    while (!tryStack_.empty() && tryStack_.back().frameIndex >= returningFrameIdx) {
        tryStack_.pop_back();
    }

    // 方法调用字段同步
    if (wasMethodCall && fieldsModified && !frames_.empty()) {
        if (receiverReg >= 0 && static_cast<size_t>(receiverReg) < currentFrame().registers.size()) {
            Value& thisVal = currentFrame().registers[receiverReg];
            if (thisVal.isInstance() && result.isInstance()) {
                // 同步字段
                for (const auto& field : result.fields()) {
                    thisVal.fields()[field.first] = field.second;
                }
            }
        }
        // 写回到接收者的原始位置（全局变量）
        if (!receiverVarName.empty() && result.isInstance()) {
            auto gsIt = globalNameToSlot_.find(receiverVarName);
            if (gsIt != globalNameToSlot_.end() && gsIt->second < static_cast<int>(globalSlots_.size())) {
                Value& globalVal = globalSlots_[gsIt->second];
                if (globalVal.isInstance()) {
                    for (const auto& field : result.fields()) {
                        globalVal.fields()[field.first] = field.second;
                    }
                }
            }
        }
    }

    // 写入返回值到调用者的目标寄存器
    if (!frames_.empty() && returnReg >= 0) {
        if (static_cast<size_t>(returnReg) < currentFrame().registers.size()) {
            currentFrame().registers[returnReg] = std::move(result);
        }
    }

    // 恢复调用者 ip
    if (!frames_.empty()) {
        currentFrame().ip = returnIp;
    }

    return VMResult::VM_OK;
}

VMResult RegisterVM::executeMethodCallImpl(size_t& ip, const std::string& methodName,
                                           uint8_t argCount, uint8_t dstReg,
                                           uint8_t objReg, const std::vector<uint8_t>& argRegs) {
    Value& obj = reg(objReg);

    // 尝试内建方法
    std::vector<Value> args;
    args.reserve(argCount);
    for (uint8_t i = 0; i < argCount; ++i) {
        args.push_back(reg(argRegs[i]));
    }

    Value builtinResult;
    VMResult r = callBuiltinMethod(obj, methodName, args, builtinResult);
    if (r == VMResult::VM_OK) {
        reg(dstReg) = std::move(builtinResult);
        ip += 6 + argCount;
        return VMResult::VM_OK;
    }
    if (hasError_) return r;

    // 实例方法调用
    if (obj.isInstance()) {
        const std::string& className = obj.className();
        auto classIt = classInfo_.find(className);
        if (classIt != classInfo_.end()) {
            auto methodIt = classIt->second.methods.find(methodName);
            if (methodIt != classIt->second.methods.end()) {
                const std::string& funName = methodIt->second;
                // 构造参数列表（this 作为第一个参数）
                std::vector<uint8_t> fullArgRegs;
                fullArgRegs.push_back(objReg);
                for (uint8_t i = 0; i < argCount; ++i) {
                    fullArgRegs.push_back(argRegs[i]);
                }
                // 调用方法函数
                size_t newIp = ip;
                VMResult cr = executeCallImpl(newIp, funName, argCount + 1, dstReg, fullArgRegs);
                if (cr != VMResult::VM_OK) return cr;
                // 标记为方法调用（用于字段同步）
                if (!frames_.empty()) {
                    frames_.back().isMethodCall = true;
                    frames_.back().receiverReg = objReg;
                    frames_.back().receiverVarName.clear();
                }
                ip = newIp;
                return VMResult::VM_OK;
            }
        }
        return runtimeError("类 " + className + " 无方法: " + methodName);
    }

    return runtimeError("不支持方法调用的类型");
}

VMResult RegisterVM::executeClosureImpl(size_t& ip, const std::string& name,
                                        uint8_t uvCount, const std::vector<uint8_t>& uvSpecs,
                                        uint8_t dstReg) {
    // 简化：创建闭包值，upvalue 绑定暂不完整实现
    // 完整实现需要从调用者帧捕获 upvalue
    std::vector<std::shared_ptr<VMUpvalue>> upvalues;
    upvalues.reserve(uvCount);
    for (uint8_t i = 0; i < uvCount; ++i) {
        uint8_t isLocal = uvSpecs[i * 2];
        uint8_t idx = uvSpecs[i * 2 + 1];
        auto uv = std::make_shared<VMUpvalue>();
        uv->isClosed = false;
        uv->stackSlot = idx;  // 简化：直接用 idx 作为栈槽
        upvalues.push_back(uv);
    }

    // 创建闭包值（使用 Value 的闭包工厂方法，但这里简化为存储函数名）
    // 注意：RegisterVM 的闭包支持是简化版本
    reg(dstReg) = Value::makeClosure(name, nullptr, {}, nullptr);

    ip += 5 + uvCount * 2;
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeClassNewImpl(size_t& ip, const std::string& className,
                                         uint8_t argCount, uint8_t dstReg,
                                         const std::vector<uint8_t>& argRegs) {
    auto classIt = classInfo_.find(className);
    if (classIt == classInfo_.end()) {
        return runtimeError("未定义的类: " + className);
    }

    // 创建实例
    Value instance = Value::makeInstance(className);

    // 初始化字段
    for (const auto& fieldName : classIt->second.fieldOrder) {
        instance.fields()[fieldName] = Value::nullValue();
    }

    reg(dstReg) = instance;

    // 调用 init 方法（如果存在）
    auto initIt = classIt->second.methods.find("init");
    if (initIt != classIt->second.methods.end()) {
        const std::string& initFunName = initIt->second;
        std::vector<uint8_t> fullArgRegs;
        fullArgRegs.push_back(dstReg);
        for (uint8_t i = 0; i < argCount; ++i) {
            fullArgRegs.push_back(argRegs[i]);
        }
        size_t newIp = ip;
        VMResult r = executeCallImpl(newIp, initFunName, argCount + 1, dstReg, fullArgRegs);
        if (r != VMResult::VM_OK) return r;
        if (!frames_.empty()) {
            frames_.back().isMethodCall = true;
            frames_.back().isInitCall = true;
            frames_.back().receiverReg = dstReg;
        }
        ip = newIp;
    } else {
        ip += 5 + argCount;
    }

    return VMResult::VM_OK;
}

bool RegisterVM::fillDefaultArgs(const RegBytecodeChunk& chunk, uint8_t& argCount,
                                 const std::string& funName, std::vector<Value>& defaults) {
    if (argCount >= chunk.arity) return true;
    if (argCount < chunk.requiredArity) return false;

    for (size_t i = argCount; i < chunk.defaultConstIndices.size() + chunk.requiredArity; ++i) {
        size_t defaultIdx = i - chunk.requiredArity;
        if (defaultIdx >= chunk.defaultConstIndices.size()) break;
        uint16_t constIdx = chunk.defaultConstIndices[defaultIdx];
        if (constIdx >= chunk.constants.size()) return false;
        defaults.push_back(chunk.constants[constIdx]);
    }
    argCount = chunk.arity;
    return true;
}

// ============================================================
// 内建方法
// ============================================================

VMResult RegisterVM::callBuiltinMethod(const Value& obj, const std::string& methodName,
                                       std::vector<Value>& args, Value& result) {
    if (obj.isArray()) {
        if (methodName == "len") {
            auto r = executeSharedLen(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "push") {
            if (args.size() != 1) return runtimeError("push 需要 1 个参数");
            const_cast<Value&>(obj).arrayVal().push_back(args[0]);
            result = obj;
            return VMResult::VM_OK;
        }
        if (methodName == "pop") {
            auto& arr = const_cast<Value&>(obj).arrayVal();
            if (arr.empty()) return runtimeError("pop 空数组");
            result = arr.back();
            arr.pop_back();
            return VMResult::VM_OK;
        }
        if (methodName == "contains") {
            auto r = executeSharedArrayContains(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
    } else if (obj.isDict()) {
        if (methodName == "len") {
            auto r = executeSharedLen(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "keys") {
            auto r = executeSharedDictKeys(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "values") {
            auto r = executeSharedDictValues(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "has") {
            auto r = executeSharedDictHas(obj, methodName, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
    } else if (obj.isString()) {
        if (methodName == "len") {
            auto r = executeSharedLen(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "upper") {
            auto r = executeSharedStrUpper(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "lower") {
            auto r = executeSharedStrLower(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "contains") {
            auto r = executeSharedStrContains(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "startsWith") {
            auto r = executeSharedStrStartsWith(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
        if (methodName == "endsWith") {
            auto r = executeSharedStrEndsWith(obj, args.data(), args.size());
            if (r.is_ok()) { result = r.value(); return VMResult::VM_OK; }
            return runtimeError(r.error().message);
        }
    }
    // 未识别为内建方法（非错误，调用方继续查找用户定义方法）
    return VMResult::VM_OK;
}

// ============================================================
// 异常处理
// ============================================================

VMResult RegisterVM::throwException(Value thrownValue) {
    while (!tryStack_.empty()) {
        RegTryHandler handler = tryStack_.back();
        if (handler.frameIndex >= frames_.size()) {
            tryStack_.pop_back();
            continue;
        }
        // 跳转到 catch 块
        while (frames_.size() > handler.frameIndex + 1) {
            frames_.pop_back();
        }
        if (!frames_.empty()) {
            currentFrame().ip = handler.catchIp;
            // 将异常值存入某个寄存器（简化：R0）
            if (!currentFrame().registers.empty()) {
                currentFrame().registers[0] = std::move(thrownValue);
            }
        }
        tryStack_.pop_back();
        return VMResult::VM_OK;
    }
    // 未捕获的异常
    std::string msg = "未捕获的异常";
    if (thrownValue.isString()) msg = thrownValue.stringVal();
    else if (thrownValue.isInstance()) msg = thrownValue.className() + " 异常";
    return runtimeError(msg);
}

void RegisterVM::closeUpvaluesFrom(size_t fromSlot) {
    auto it = openUpvalues_.lower_bound(fromSlot);
    while (it != openUpvalues_.end()) {
        auto uv = it->second.lock();
        if (uv && !uv->isClosed) {
            // 从调用者帧读取值并关闭
            if (frames_.size() >= 2) {
                auto& callerFrame = frames_[frames_.size() - 2];
                if (uv->stackSlot < callerFrame.registers.size()) {
                    uv->value = callerFrame.registers[uv->stackSlot];
                }
            }
            uv->isClosed = true;
        }
        ++it;
    }
    openUpvalues_.erase(openUpvalues_.lower_bound(fromSlot), openUpvalues_.end());
}

// ============================================================
// 调试接口
// ============================================================

std::vector<Value> RegisterVM::getRegisters() const {
    if (frames_.empty()) return {};
    return currentFrame().registers;
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
    if (frames_.empty()) return 0;
    return currentFrame().ip;
}

RegOp RegisterVM::getCurrentOpCode() const {
    if (frames_.empty()) return RegOp::REG_RETURN_NULL;
    const auto& frame = currentFrame();
    if (frame.ip >= frame.chunk->code.size()) return RegOp::REG_RETURN_NULL;
    return static_cast<RegOp>(frame.chunk->code[frame.ip]);
}

int RegisterVM::getCurrentLine() const {
    if (frames_.empty()) return 0;
    const auto& frame = currentFrame();
    if (frame.ip < frame.chunk->lines.size()) {
        return frame.chunk->lines[frame.ip];
    }
    return 0;
}

std::string RegisterVM::getCurrentChunkName() const {
    if (frames_.empty()) return "";
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
