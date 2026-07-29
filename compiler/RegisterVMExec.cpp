// ============================================================
// RegisterVMExec.cpp — 寄存器式虚拟机：指令执行方法（非调用类）
// ------------------------------------------------------------
// 从 RegisterVM.cpp 拆分而来（编译速度优化，无逻辑变更）。
// 包含：executeConstants, executeArith, executeCompare, executeVars,
//       executeControl, executeContainers（及子方法）, executeMisc（及子方法）,
//       executeCoroutineOps (REG_YIELD)。
// ============================================================

#include "common/ErrorFormat.h"
#include "common/ErrorMessages.h"
#include "common/Logger.h"
#include "common/RuntimeLimits.h"
#include "common/TypeChecker.h"
#include "common/Utf8Utils.h"
#include "compiler/RegisterBytecode.h"
#include "compiler/RegisterVM.h"
#include "interpreter/BuiltinMethods.h"
#include "interpreter/NumericUtils.h"
#include <algorithm>
#include <cassert>
#include <cmath>

// ============================================================
// 常量加载指令
// ============================================================

VMResult RegisterVM::executeConstants(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

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

    notifyStep(ip, op);
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
            return runtimeError("一元减运算需要数值类型", DiagCodes::kTypeMismatch);
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
            // 拓展二期·语言（运算符重载）：报错前先尝试 instance dunder 分派——
            // reg(s1) 为类实例且定义了 __add 等方法时注入方法帧（与
            // Interpreter/StackVM 的同名分派点保持三后端一致）。
            // 已分派时直接 return（新帧已 push，不推进旧帧 ip，不走 arithDone
            // 的 notifyStep——分派内部已按方法调用模式 notifyStep）。
            {
                VMResult ovr;
                if (tryOperatorOverload(ip, op, dst, s1, s2, ovr)) {
                    return ovr;
                }
            }
            return runtimeError("算术运算需要数值类型", DiagCodes::kTypeMismatch);
        }

        // P-6 perf: int-int 快速路径，避免两次 toDouble() + ArithOp 枚举分发。
        // 对齐 StackVM 的 OP_ADD_INT_SPEC / OP_SUB_INT_SPEC / OP_MUL_INT_SPEC 特化。
        if (a.isInt() && b.isInt()) {
            int64_t ia = a.intVal(), ib = b.intVal();
            Value intResult;
            switch (op) {
            case RegOp::REG_ADD:
                if (OverflowCheck::addOverflow(ia, ib))
                    return runtimeError("整数运算溢出");
                intResult = Value(ia + ib);
                break;
            case RegOp::REG_SUB:
                if (OverflowCheck::subOverflow(ia, ib))
                    return runtimeError("整数运算溢出");
                intResult = Value(ia - ib);
                break;
            case RegOp::REG_MUL:
                if (OverflowCheck::mulOverflow(ia, ib))
                    return runtimeError("整数运算溢出");
                intResult = Value(ia * ib);
                break;
            case RegOp::REG_DIV:
                if (ib == 0)
                    return runtimeError("除零错误", DiagCodes::kDivisionByZero);
                if (OverflowCheck::divOverflow(ia, ib))
                    return runtimeError("整数运算溢出");
                intResult = Value(ia / ib);
                break;
            case RegOp::REG_MOD:
                if (ib == 0)
                    return runtimeError("除零错误", DiagCodes::kDivisionByZero);
                if (OverflowCheck::modOverflow(ia, ib))
                    return runtimeError("整数运算溢出");
                intResult = Value(ia % ib);
                break;
            default:
                return runtimeError("executeArith: 未知操作码");
            }
            reg(dst) = intResult;
            ip += 4;
            goto arithDone;
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
                return runtimeError("除零错误", DiagCodes::kDivisionByZero);
            case NumericOps::ArithStatus::IntOverflow:
                return runtimeError("整数运算溢出");
            case NumericOps::ArithStatus::NotNumeric:
                return runtimeError("算术运算需要数值类型", DiagCodes::kTypeMismatch);
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
    notifyStep(ip, op);
    return VMResult::VM_OK;
}

// ============================================================
// 拓展二期·语言：运算符重载（instance 算术 dunder 分派）
// ------------------------------------------------------------
// 方法查找仿 executeMethodCallImpl 的继承链循环（含 methodCache），
// 命中后直接复用 executeCallImpl 注入方法帧：fullArgRegs=[s1,s2]
// （this=左操作数寄存器，arg=右操作数寄存器），returnReg=dst，
// returnOffset=4（REG_ADD 系列固定 4 字节）。方法 REG_RETURN 时
// 结果自动写入调用者帧的 reg(dst)。
// ============================================================
bool RegisterVM::tryOperatorOverload(size_t& ip, RegOp op, uint8_t dst, uint8_t s1, uint8_t s2, VMResult& outResult) {
    const Value& left = reg(s1);
    if (!left.isInstance()) {
        return false;
    }
    const char* dunderName = nullptr;
    switch (op) {
    case RegOp::REG_ADD:
        dunderName = "__add";
        break;
    case RegOp::REG_SUB:
        dunderName = "__sub";
        break;
    case RegOp::REG_MUL:
        dunderName = "__mul";
        break;
    case RegOp::REG_DIV:
        dunderName = "__div";
        break;
    case RegOp::REG_MOD:
        dunderName = "__mod";
        break;
    default:
        return false;
    }

    // 沿继承链查找 dunder 方法（仿 executeMethodCallImpl，含 methodCache）
    const std::string& className = left.className();
    auto classInfoIt = classInfo_.find(className);
    std::string foundFunName;
    bool methodFound = false;
    if (classInfoIt != classInfo_.end()) {
        auto cacheIt = classInfoIt->second.methodCache.find(dunderName);
        if (cacheIt != classInfoIt->second.methodCache.end()) {
            if (cacheIt->second.empty()) {
                return false; // 缓存命中“无此方法” → 回退报错
            }
            foundFunName = cacheIt->second;
            methodFound = true;
        }
    }
    if (!methodFound) {
        std::string searchClass = className;
        for (int guard = 0; guard < 64 && !searchClass.empty(); ++guard) {
            auto classIt = classInfo_.find(searchClass);
            if (classIt == classInfo_.end())
                break;
            auto methodIt = classIt->second.methods.find(dunderName);
            if (methodIt != classIt->second.methods.end()) {
                foundFunName = methodIt->second;
                methodFound = true;
                break;
            }
            searchClass = classIt->second.parent;
        }
        if (classInfoIt != classInfo_.end()) {
            classInfoIt->second.methodCache[dunderName] = methodFound ? foundFunName : std::string{};
        }
        if (!methodFound) {
            return false; // 无 dunder 方法 → 回退到“算术运算需要数值类型”报错
        }
    }

    // 与 Interpreter/StackVM 一致：运算符方法必须恰好 1 个参数
    // （RegisterVM 方法 chunk 的 arity 含 this，故要求 arity==2）
    auto chunkIt = functionChunks_.find(foundFunName);
    if (chunkIt != functionChunks_.end() && (chunkIt->second.arity != 2 || chunkIt->second.requiredArity != 2)) {
        outResult = runtimeError(std::string("运算符方法 ") + dunderName + " 必须恰好接受 1 个参数");
        return true;
    }

    // 经 executeCallImpl 注入方法帧：[s1,s2] = [this,arg]，returnOffset=4
    SmallArgs<uint8_t> fullArgRegs;
    fullArgRegs.push_back(s1);
    fullArgRegs.push_back(s2);
    size_t newIp = ip;
    VMResult cr = executeCallImpl(newIp, foundFunName, 2, dst, fullArgRegs, 4u, nullptr, true);
    if (cr != VMResult::VM_OK) {
        outResult = cr;
        return true;
    }
    if (!frames_.empty()) {
        frames_.back().isMethodCall = true;
        frames_.back().isInitCall = false;
        // receiverReg 保持 -1：运算符方法内 this 字段变异不写回接收者
        // （与 Interpreter 左值拷贝/StackVM receiverLocalSlot=-1 语义一致）
    }
    ip = newIp;
    notifyStep(ip, op);
    outResult = VMResult::VM_OK;
    return true;
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
                return runtimeError("比较运算需要数值或字符串类型", DiagCodes::kTypeMismatch);
            break;
        case RegOp::REG_GT:
            if (isStringCompare)
                result = a.stringVal() > b.stringVal();
            else if (a.isNumber() && b.isNumber())
                result = a.toDouble() > b.toDouble();
            else
                return runtimeError("比较运算需要数值或字符串类型", DiagCodes::kTypeMismatch);
            break;
        case RegOp::REG_LTE:
            if (isStringCompare)
                result = a.stringVal() <= b.stringVal();
            else if (a.isNumber() && b.isNumber())
                result = a.toDouble() <= b.toDouble();
            else
                return runtimeError("比较运算需要数值或字符串类型", DiagCodes::kTypeMismatch);
            break;
        case RegOp::REG_GTE:
            if (isStringCompare)
                result = a.stringVal() >= b.stringVal();
            else if (a.isNumber() && b.isNumber())
                result = a.toDouble() >= b.toDouble();
            else
                return runtimeError("比较运算需要数值或字符串类型", DiagCodes::kTypeMismatch);
            break;
        default:
            return runtimeError("executeCompare: 未知操作码");
        }
        reg(dst) = Value(result);
        ip += 4;
    }

    notifyStep(ip, op);
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
            if (gsIt != globalNameToSlot_.end() && gsIt->second >= 0 &&
                gsIt->second < static_cast<int>(globalSlots_.size())) {
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
                    return runtimeError("未定义的变量: " + name, DiagCodes::kUndefinedVariable);
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
            if (gsIt != globalNameToSlot_.end() && gsIt->second >= 0 &&
                gsIt->second < static_cast<int>(globalSlots_.size())) {
                globalSlots_[gsIt->second] = reg(src);
            } else {
                // 槽位表未命中，回退到 globals_ 名称表查找（已声明过的全局变量）
                auto it = globals_.find(name);
                if (it == globals_.end()) {
                    return runtimeError("未定义的变量: " + name, DiagCodes::kUndefinedVariable);
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
            if (gsIt != globalNameToSlot_.end() && gsIt->second >= 0 &&
                gsIt->second < static_cast<int>(globalSlots_.size())) {
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
            size_t frameIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slotIdx = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            // AUDIT-P1 fix: 对齐 StackVM VM.cpp OP_SET_UPVALUE 的字段范围检查。
            // 原仅 slotIdx==0 (this) 触发同步，字段槽 R1..R(fieldCount) 修改不触发。
            // slot 0 = this: 无条件标记（保留 BUG-UPVAL-1 语义）；
            // slots 1..fieldCount = 字段槽: 检查是否在 fieldOrder 范围内。
            if (frameIdx < frames_.size() && frames_[frameIdx].isMethodCall) {
                const auto* fchunk = frames_[frameIdx].chunk;
                if (slotIdx == 0 || (fchunk && !fchunk->fieldOrder.empty() && slotIdx <= fchunk->fieldOrder.size())) {
                    frames_[frameIdx].fieldsModified = true;
                }
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

    notifyStep(ip, op);
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

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

// ============================================================
// 容器指令
// ============================================================

// R120 重构：原 executeArrayOps 224 行单 switch 拆为 thin dispatcher + 3 个 helper。
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
        auto enumIt = enumRegistry_.find(enumName);
        if (enumIt == enumRegistry_.end()) {
            return runtimeError(ErrorFormat::formatStd("未定义的 enum: {}", enumName), "undefined-function");
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
            return runtimeError(ErrorFormat::formatStd("enum '{}' 没有 variant '{}'", enumName, variantName),
                                "undefined-function");
        }
        if (static_cast<int>(argCount) != varInfo->arity) {
            return runtimeError(ErrorFormat::formatStd("enum variant '{}.{}' 期望 {} 个参数，得到 {} 个", enumName,

                                                       variantName, varInfo->arity, static_cast<int>(argCount)),
                                DiagCodes::kArityMismatch);
        }

        std::vector<Value> fields;
        fields.reserve(argCount);
        for (uint8_t i = 0; i < argCount; ++i) {
            uint8_t argReg = chunk.code[ip + 7 + i];
            fields.push_back(reg(argReg));
        }
        // AUDIT-R6 F3 fix: 字段类型注解校验（对齐 Interpreter::visitEnumVariantExpr 与
        // StackVM OP_BUILD_ENUM_VARIANT）。泛型类型参数擦除；实例继承链 fallback
        // 对齐 REG_TYPE_CHECK 的 classInfo_ 查找模式。
        for (size_t fi = 0; fi < fields.size() && fi < varInfo->paramTypes.size(); ++fi) {
            const std::string& expectedType = varInfo->paramTypes[fi];
            if (expectedType.empty() || minilang::isTypeParameter(expectedType, info.typeParams))
                continue;
            if (!minilang::typeMatchValue(fields[fi], expectedType)) {
                bool inheritOk = false;
                if (fields[fi].isInstance()) {
                    auto classIt = classInfo_.find(fields[fi].className());
                    int depth = 0;
                    while (classIt != classInfo_.end() && depth < 64) {
                        if (classIt->second.name == expectedType) {
                            inheritOk = true;
                            break;
                        }
                        if (classIt->second.parent.empty())
                            break;
                        classIt = classInfo_.find(classIt->second.parent);
                        ++depth;
                    }
                }
                if (!inheritOk) {
                    return runtimeError(
                        ErrorFormat::formatStd("enum variant '{}.{}' 第 {} 个参数类型不匹配：期望 {}，得到 {}",
                                               enumName, variantName, fi + 1, expectedType, fields[fi].typeName()),
                        DiagCodes::kTypeMismatch);
                }
            }
        }
        reg(dst) = Value::makeEnumVariant(enumName, variantName, std::move(fields));
        ip += 7 + argCount;
        break;
    }
    default:
        return runtimeError("executeArrayBuildOps: 未知操作码");
    }

    notifyStep(ip, op);
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
                return runtimeError(ErrorMessages::kArrayIndexMustBeInt, DiagCodes::kTypeMismatch);
            int64_t i = idx.intVal();
            const auto& arr = obj.arrayVal();
            if (i < 0 || i >= static_cast<int64_t>(arr.size())) {
                return runtimeError(
                    ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})", static_cast<long long>(i), arr.size()),
                    DiagCodes::kIndexOutOfBounds);
            }
            reg(dst) = arr[static_cast<size_t>(i)];
        } else if (obj.isDict()) {
            auto dk = Value::dictKeyFromValue(idx);
            if (!dk)
                return runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch);
            const auto& dict = obj.dictVal();
            auto it = dict.find(*dk);
            if (it == dict.end()) {
                reg(dst) = Value::nullValue();
            } else {
                reg(dst) = it->second;
            }
        } else if (obj.isString()) {
            if (!idx.isInt())
                return runtimeError(ErrorMessages::kStringIndexMustBeInt, DiagCodes::kTypeMismatch);
            int64_t i = idx.intVal();
            const auto& str = obj.stringVal();
            int64_t len = obj.codepointCount();
            if (i < 0 || i >= len) {
                return runtimeError(ErrorFormat::formatStd("字符串索引越界: {}, 有效范围 [0, {})",

                                                           static_cast<long long>(i), static_cast<long long>(len)),
                                    DiagCodes::kIndexOutOfBounds);
            }
            size_t bytePos = Utf8::codepointToByteIndex(str, i);
            int charLen = Utf8::byteLength(static_cast<unsigned char>(str[bytePos]));
            reg(dst) = Value(str.substr(bytePos, static_cast<size_t>(charLen)));
        } else if (obj.isTuple()) {
            if (!idx.isInt())
                return runtimeError("元组索引必须是整数", DiagCodes::kTypeMismatch);
            int64_t i = idx.intVal();
            const auto& tup = obj.tupleVal();
            if (i < 0 || i >= static_cast<int64_t>(tup.size())) {
                return runtimeError(
                    ErrorFormat::formatStd("元组索引越界: {}, 有效范围 [0, {})", static_cast<long long>(i), tup.size()),
                    DiagCodes::kIndexOutOfBounds);
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
                return runtimeError(ErrorMessages::kArrayIndexMustBeInt, DiagCodes::kTypeMismatch);
            int64_t i = idx.intVal();
            auto& arr = obj.arrayVal();
            if (i < 0 || i >= static_cast<int64_t>(arr.size())) {
                return runtimeError(
                    ErrorFormat::formatStd("数组索引越界: {}, 有效范围 [0, {})", static_cast<long long>(i), arr.size()),
                    DiagCodes::kIndexOutOfBounds);
            }
            arr[static_cast<size_t>(i)] = val;
        } else if (obj.isDict()) {
            auto dk = Value::dictKeyFromValue(idx);
            if (!dk)
                return runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch);
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
            len = obj.codepointCount();
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

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

// ============================================================
// executeArrayEnumQueryOps - 枚举 variant 查询指令（ENUM_QUERY 类）
// REG_ENUM_VARIANT_NAME / REG_ENUM_VARIANT_FIELD
// ============================================================
VMResult RegisterVM::executeArrayEnumQueryOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
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
            return runtimeError(ErrorFormat::formatStd("REG_ENUM_VARIANT_FIELD: 索引越界 {}, 有效范围 [0, {})",

                                                       static_cast<long long>(i), fields.size()),
                                DiagCodes::kIndexOutOfBounds);
        }
        reg(dst) = fields[static_cast<size_t>(i)];
        ip += 4;
        break;
    }
    default:
        return runtimeError("executeArrayEnumQueryOps: 未知操作码");
    }

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeDictOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_BUILD_DICT: {
        uint8_t dst = chunk.code[ip + 1];
        uint8_t pairCount = chunk.code[ip + 2];
        Value::DictMap entries;
        for (uint8_t i = 0; i < pairCount; ++i) {
            size_t base = ip + 3 + i * 2;
            uint8_t keyReg = chunk.code[base];
            uint8_t valReg = chunk.code[base + 1];
            const Value& keyVal = reg(keyReg);
            auto dk = Value::dictKeyFromValue(keyVal);
            if (!dk) {
                return runtimeError(ErrorMessages::kDictKeyInvalidType, DiagCodes::kTypeMismatch);
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

    notifyStep(ip, op);
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
            auto it = dict.find(Value::DictKey{fieldName});
            if (it == dict.end()) {
                reg(dst) = Value::nullValue();
            } else {
                reg(dst) = it->second;
            }
        } else if (obj.isNull()) {
            return runtimeError("不能在 null 值上访问属性或调用方法", DiagCodes::kNullAccess);
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
            obj.dictVal()[Value::DictKey{fieldName}] = val;
        } else {
            return runtimeError(ErrorMessages::kTypeNotMemberAssignable);
        }
        lastMutatedReceiverReg_ = objReg;
        ip += 5;
        break;
    }
    case RegOp::REG_INIT_FIELD: {
        uint16_t fieldIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (fieldIdx >= chunk.constants.size() || !chunk.constants[fieldIdx].isString()) {
            return runtimeError("字段名索引无效");
        }
        ip += 3;
        break;
    }
    case RegOp::REG_SUPER_MEMBER_GET: {
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
                    return runtimeError("类 " + obj.className() + " 没有字段或方法 '" + fieldName + "'",
                                        "undefined-function");
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

    notifyStep(ip, op);
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
        return executeArrayOps(op, ip);
    case RegOp::REG_BUILD_ENUM_VARIANT:
    case RegOp::REG_ENUM_VARIANT_NAME:
    case RegOp::REG_ENUM_VARIANT_FIELD:
    case RegOp::REG_LEN:
        return executeArrayOps(op, ip);
    case RegOp::REG_BUILD_DICT:
        return executeDictOps(op, ip);
    case RegOp::REG_MEMBER_GET:
    case RegOp::REG_MEMBER_SET:
    case RegOp::REG_INIT_FIELD:
    case RegOp::REG_SUPER_MEMBER_GET:
        return executeMemberOps(op, ip);
    default:
        return runtimeError("executeContainers: 未知操作码");
    }
}

// ============================================================
// 其他指令（Misc）及子方法
// ============================================================

VMResult RegisterVM::executeTryThrowOps(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case RegOp::REG_TRY_BEGIN: {
        uint16_t catchOffset = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (catchOffset >= chunk.code.size()) {
            return runtimeError("REG_TRY_BEGIN: catch 目标越界");
        }
        // Bug #47 fix + AUDIT-R5 R1 fix: 历史方案的 registerBase 水位（registerCount）
        // 会漏关 try 内嵌套作用域复用的低槽位 upvalue；后改 regBase=0 全量关闭
        // 又把 try 之前创建的闭包 upvalue 误关为快照。现改为记录当前
        // upvalueOpenSeq_ 水位，catch 命中时仅关闭 try 期间开启的 upvalue
        // （见 RegisterVM::closeFrameUpvaluesSince），与槽位号无关。
        tryStack_.push_back({catchOffset, frames_.size() - 1, upvalueOpenSeq_});
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
        VMResult r = throwException(std::move(thrown));
        if (r == VMResult::VM_OK && stepCallbackEnabled_ && stepCallback_) {
            stepCallback_({ip, op, frames_.size()});
        }
        return r;
    }
    case RegOp::REG_LOAD_EXCEPTION: {
        uint8_t dst = chunk.code[ip + 1];
        reg(dst) = pendingException_;
        ip += 2;
        break;
    }
    case RegOp::REG_PUSH_JUMP_TARGET: {
        uint16_t target = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (target >= chunk.code.size())
            return runtimeError("REG_PUSH_JUMP_TARGET: 跳转目标越界");
        // AUDIT-R7 F1 fix: 附带当前帧索引（防跨帧误跳，与 StackVM 对齐）
        pendingJumpStack_.push_back({static_cast<size_t>(target), frames_.size() - 1});
        ip += 3;
        break;
    }
    case RegOp::REG_FINALLY_END: {
        // AUDIT-R7 F1 fix: 只消费本帧条目，惰性丢弃已返回深帧残留；栈顶属更浅帧
        // （调用方在途续跳）时不消费（实证：原实现跨帧误跳致寄存器越界 C++ 异常逃逸）。
        size_t curFrameIdx = frames_.size() - 1;
        while (!pendingJumpStack_.empty() && pendingJumpStack_.back().frameIndex > curFrameIdx) {
            pendingJumpStack_.pop_back();
        }
        if (!pendingJumpStack_.empty() && pendingJumpStack_.back().frameIndex == curFrameIdx) {
            size_t target = pendingJumpStack_.back().target;
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

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeWritebackOps(RegOp op, size_t& ip) {
    RegCallFrame& frame = currentFrame();
    const RegBytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case RegOp::REG_WRITEBACK_INDEX_LOCAL: {
        uint8_t slot = chunk.code[ip + 1];
        if (slot >= frame.registerCount) {
            return runtimeError("WRITEBACK_INDEX_LOCAL: 局部槽越界");
        }
        frame.registers[slot] = reg(lastMutatedReceiverReg_);
        ip += 2;
        break;
    }
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL: {
        uint8_t slot = chunk.code[ip + 1];
        if (slot >= frame.registerCount) {
            return runtimeError("WRITEBACK_MEMBER_LOCAL: 局部槽越界");
        }
        frame.registers[slot] = reg(lastMutatedReceiverReg_);
        ip += 4;
        break;
    }
    case RegOp::REG_WRITEBACK_INDEX_VAR: {
        uint16_t slotOrName = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        const Value& mutated = reg(lastMutatedReceiverReg_);
        if (slotOrName & 0x8000) {
            uint16_t nameIdx = slotOrName & 0x7FFF;
            if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                return runtimeError("WRITEBACK_INDEX_VAR: 名称索引无效");
            }
            const std::string& name = chunk.constants[nameIdx].stringVal();
            auto gsIt = globalNameToSlot_.find(name);
            if (gsIt != globalNameToSlot_.end() && gsIt->second >= 0 &&
                gsIt->second < static_cast<int>(globalSlots_.size())) {
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
        uint16_t slotOrName = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        const Value& mutated = reg(lastMutatedReceiverReg_);
        if (slotOrName & 0x8000) {
            uint16_t nameIdx = slotOrName & 0x7FFF;
            if (nameIdx >= chunk.constants.size() || !chunk.constants[nameIdx].isString()) {
                return runtimeError("WRITEBACK_MEMBER_VAR: 名称索引无效");
            }
            const std::string& name = chunk.constants[nameIdx].stringVal();
            auto gsIt = globalNameToSlot_.find(name);
            if (gsIt != globalNameToSlot_.end() && gsIt->second >= 0 &&
                gsIt->second < static_cast<int>(globalSlots_.size())) {
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
        uint8_t uvIdx = chunk.code[ip + 1];
        if (uvIdx >= frame.upvalues.size()) {
            return runtimeError("WRITEBACK_INDEX_UPVALUE: upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        const Value& mutated = reg(lastMutatedReceiverReg_);
        if (uv->isClosed) {
            uv->value = mutated;
        } else {
            Value* slot = resolveOpenUpvalueSlot(*uv);
            if (!slot)
                return VMResult::VM_RUNTIME_ERROR;
            *slot = mutated;
            size_t frameIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slotIdx = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            // AUDIT-P1 fix: 对齐 StackVM VM.cpp OP_SET_UPVALUE 的字段范围检查。
            // 原仅 slotIdx==0 (this) 触发同步，字段槽 R1..R(fieldCount) 修改不触发。
            // slot 0 = this: 无条件标记（保留 BUG-UPVAL-1 语义）；
            // slots 1..fieldCount = 字段槽: 检查是否在 fieldOrder 范围内。
            if (frameIdx < frames_.size() && frames_[frameIdx].isMethodCall) {
                const auto* fchunk = frames_[frameIdx].chunk;
                if (slotIdx == 0 || (fchunk && !fchunk->fieldOrder.empty() && slotIdx <= fchunk->fieldOrder.size())) {
                    frames_[frameIdx].fieldsModified = true;
                }
            }
        }
        ip += 2;
        break;
    }
    case RegOp::REG_WRITEBACK_MEMBER_UPVALUE: {
        uint8_t uvIdx = chunk.code[ip + 1];
        if (uvIdx >= frame.upvalues.size()) {
            return runtimeError("WRITEBACK_MEMBER_UPVALUE: upvalue 索引越界");
        }
        auto& uv = frame.upvalues[uvIdx];
        const Value& mutated = reg(lastMutatedReceiverReg_);
        if (uv->isClosed) {
            uv->value = mutated;
        } else {
            Value* slot = resolveOpenUpvalueSlot(*uv);
            if (!slot)
                return VMResult::VM_RUNTIME_ERROR;
            *slot = mutated;
            size_t frameIdx = uv->stackSlot / RegCallFrame::MAX_REGISTERS;
            size_t slotIdx = uv->stackSlot % RegCallFrame::MAX_REGISTERS;
            // AUDIT-P1 fix: 对齐 StackVM VM.cpp OP_SET_UPVALUE 的字段范围检查。
            // 原仅 slotIdx==0 (this) 触发同步，字段槽 R1..R(fieldCount) 修改不触发。
            // slot 0 = this: 无条件标记（保留 BUG-UPVAL-1 语义）；
            // slots 1..fieldCount = 字段槽: 检查是否在 fieldOrder 范围内。
            if (frameIdx < frames_.size() && frames_[frameIdx].isMethodCall) {
                const auto* fchunk = frames_[frameIdx].chunk;
                if (slotIdx == 0 || (fchunk && !fchunk->fieldOrder.empty() && slotIdx <= fchunk->fieldOrder.size())) {
                    frames_[frameIdx].fieldsModified = true;
                }
            }
        }
        ip += 4;
        break;
    }
    default:
        return runtimeError("executeWritebackOps: 未知操作码");
    }

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeTypeCheckOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_LOAD_MUTATED: {
        uint8_t dst = chunk.code[ip + 1];
        reg(dst) = reg(lastMutatedReceiverReg_);
        ip += 2;
        break;
    }
    case RegOp::REG_TYPE_CHECK: {
        uint8_t src = chunk.code[ip + 1];
        uint16_t typeIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (typeIdx >= chunk.constants.size()) {
            return runtimeError("REG_TYPE_CHECK: 类型注解常量索引越界");
        }
        const std::string& annotation = chunk.constants[typeIdx].stringVal();
        const Value& val = reg(src);
        if (!minilang::typeMatchValue(val, annotation)) {
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
            return runtimeError(
                ErrorFormat::formatStd(ErrorMessages::kTypeAnnotationViolationFmtStd, annotation, val.typeName()),
                DiagCodes::kTypeMismatch);
        }
        ip += 4;
        break;
    }
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
        ip += 5;
        break;
    }
    default:
        return runtimeError("executeTypeCheckOps: 未知操作码");
    }

    notifyStep(ip, op);
    return VMResult::VM_OK;
}

VMResult RegisterVM::executeSuperCallOps(RegOp op, size_t& ip) {
    const RegBytecodeChunk& chunk = *currentFrame().chunk;

    switch (op) {
    case RegOp::REG_SUPER_CALL: {
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

        auto curIt = classInfo_.find(curClassName);
        if (curIt == classInfo_.end() || curIt->second.parent.empty()) {
            return runtimeError("类 " + curClassName + " 没有父类，不能使用 super");
        }
        std::string searchClass = curIt->second.parent;
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
            return runtimeError("类 " + curClassName + " 没有方法 " + methodName, "undefined-function");
        }

        SmallArgs<uint8_t> fullArgRegs;
        fullArgRegs.push_back(recvReg);
        for (uint8_t i = 0; i < argCount; ++i) {
            fullArgRegs.push_back(chunk.code[ip + 8 + i]);
        }
        size_t newIp = ip;
        if (argCount >= 255)
            return runtimeError("super 调用参数数量超过上限");
        VMResult cr =
            executeCallImpl(newIp, *foundFunNamePtr, argCount + 1, dst, fullArgRegs, 8u + argCount, nullptr, true);
        if (cr != VMResult::VM_OK)
            return cr;
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

    // Bug #80: notifyStep 在 executeCallImpl 返回后调用，此时 frameCount 已反映调用栈变化。
    // 与 REG_RETURN 处理模式一致：notifyStep 观察的是指令执行后的帧状态，而非执行前。
    notifyStep(ip, op);
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
        return executeTryThrowOps(op, ip);
    case RegOp::REG_WRITEBACK_INDEX_LOCAL:
    case RegOp::REG_WRITEBACK_MEMBER_LOCAL:
    case RegOp::REG_WRITEBACK_INDEX_VAR:
    case RegOp::REG_WRITEBACK_MEMBER_VAR:
    case RegOp::REG_WRITEBACK_INDEX_UPVALUE:
    case RegOp::REG_WRITEBACK_MEMBER_UPVALUE:
        return executeWritebackOps(op, ip);
    case RegOp::REG_TYPE_CHECK:
    case RegOp::REG_TYPE_TEST:
    case RegOp::REG_LOAD_MUTATED:
        return executeTypeCheckOps(op, ip);
    case RegOp::REG_SUPER_CALL:
        return executeSuperCallOps(op, ip);
    default:
        return runtimeError("executeMisc: 未知操作码");
    }

    // 仅 REG_PRINT 走到这里（其他 case 均已 return 到子函数）
    notifyStep(ip, op);
    return VMResult::VM_OK;
}

// ============================================================
// R164 协程/生成器：REG_YIELD 指令执行（重放模式）
// ============================================================
VMResult RegisterVM::executeCoroutineOps(RegOp op, size_t& ip) {
    if (op != RegOp::REG_YIELD) {
        return runtimeError(ErrorFormat::formatStd("未知协程操作码: {}", static_cast<int>(op)));
    }
    if (currentCoroutineTargetYieldId_ < 0) {
        return runtimeError("yield 只能在 fun* 生成器函数体内出现");
    }
    const RegBytecodeChunk& chunk = *currentFrame().chunk;
    uint8_t dst = chunk.code[ip + 1];
    uint8_t src = chunk.code[ip + 2];
    Value yieldValue = reg(src);
    int thisExecutionId = currentYieldExecutionCount_++;
    if (thisExecutionId == currentCoroutineTargetYieldId_) {
        throw VMYieldSignal(std::move(yieldValue));
    }
    reg(dst) = std::move(yieldValue);
    ip += 3;
    return VMResult::VM_OK;
}
