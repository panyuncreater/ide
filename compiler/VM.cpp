#include "compiler/VM.h"
#include "interpreter/BuiltinMethods.h"  // 共享纯函数层（len/contains/has）
#include "interpreter/NumericUtils.h"    // 共享溢出检查（B6 fix）
#include "interpreter/ErrorFormat.h"     // Dedup-5A: ErrorFormat::format 替代 std::to_string 拼接
#include "common/Utf8Utils.h"            // P0-4 fix: UTF-8 码位工具
#include "Logger.h"
#include <sstream>
#include <climits>
#include <cstdint>
#include <cmath>  // BUG 8.1 fix: std::fmod
#include <algorithm>
#include <stdexcept>  // B3 fix: currentFrame 抛 std::runtime_error

// ============================================================
// VM 虚拟机实现
// ============================================================

VM::VM() {
    outputCallback_ = [](const std::string&) {};
    stepCallback_ = [](const VMStepInfo&) {};
}

void VM::push(const Value& val) {
    if (stack_.size() >= MAX_STACK_SIZE) {
        runtimeError("栈溢出");
        return;  // 溢出后不继续 push_back
    }
    stack_.push_back(val);
}

void VM::push(Value&& val) {
    if (stack_.size() >= MAX_STACK_SIZE) {
        runtimeError("栈溢出");
        return;
    }
    stack_.push_back(std::move(val));
}

Value VM::pop() {
    if (stack_.empty()) {
        runtimeError("栈下溢");
        hasError_ = true;  // 栈下溢视为不可恢复错误
        return Value::nullValue();
    }
    Value val = std::move(stack_.back());
    stack_.pop_back();
    return val;
}

const Value& VM::peek(size_t distance) const {
    // P0-12 fix: 越界时返回静态 null 哨兵但设置 hasError_ 标志，
    // 避免静默失败导致调用方在错误数据上继续执行
    // P0 fix: hasError_ 改为 mutable，在 const 方法中也可设置
    static const Value nullSentinel;  // 静态 null 哨兵，用于越界访问
    if (distance >= stack_.size()) {
        Logger::Error(ErrorFormat::format("VM 栈下溢: peek(distance=%zu) 但栈大小=%zu",
                                          distance, stack_.size()), "VM");
        hasError_ = true;  // P0 fix: 设置错误标志，防止调用方在 null 哨兵上继续执行
        return nullSentinel;
    }
    return stack_[stack_.size() - 1 - distance];
}

Value& VM::peekRef(size_t distance) {
    // PERF-12 fix: 非 const peek 重载，允许直接修改栈槽
    static Value nullSentinel;  // 静态哨兵（与 const 版本一致）
    if (distance >= stack_.size()) {
        Logger::Error(ErrorFormat::format("VM 栈下溢: peekRef(distance=%zu) 但栈大小=%zu",
                                          distance, stack_.size()), "VM");
        hasError_ = true;
        return nullSentinel;
    }
    return stack_[stack_.size() - 1 - distance];
}

void VM::popN(size_t n) {
    // PERF-12 fix: 批量 pop，一次 resize 替代多次 pop_back
    if (n > stack_.size()) {
        runtimeError("栈下溢: popN");
        hasError_ = true;
        stack_.clear();
        return;
    }
    stack_.resize(stack_.size() - n);
}

VMResult VM::runtimeError(const std::string& msg) {
    lastError_ = msg;
    lastErrorLine_ = getCurrentLine();
    hasError_ = true;
    diagnostics_.addError(msg, lastErrorLine_, 0, DiagSource::VM);
    // P1-9 fix: 使用 ErrorFormat::formatWithLine 替代 std::to_string + operator+
    Logger::Error(ErrorFormat::formatWithLine(msg, lastErrorLine_), "VM");
    return VMResult::VM_RUNTIME_ERROR;
}

VMResult VM::throwException(Value thrownValue) {
    // F11: 异常处理 — 搜索 try 处理器或跨帧传播
    while (true) {
        size_t currentFrameIdx = frames_.size() - 1;

        // 从 tryStack_ 顶部搜索当前帧的处理器
        // tryStack_ 是栈结构，顶部可能是当前帧或更内层帧的处理器
        while (!tryStack_.empty()) {
            const auto& handler = tryStack_.back();
            if (handler.frameIndex == currentFrameIdx) {
                // 找到 catch 处理器：截断栈、推入异常值、跳转到 catch
                // P0-5 fix: 截断栈前关闭指向被截断栈槽的 open upvalues，防止悬垂指针
                if (handler.stackBase <= stack_.size()) {
                    closeUpvaluesFrom(handler.stackBase);
                    stack_.resize(handler.stackBase);
                }
                push(std::move(thrownValue));
                currentFrame().ip = handler.catchIp;
                tryStack_.pop_back();
                return VMResult::VM_OK;
            }
            if (handler.frameIndex < currentFrameIdx) {
                // 处理器在外层帧 — 当前帧无处理器，需弹出当前帧
                break;
            }
            // handler.frameIndex > currentFrameIdx: 内层帧残留处理器，弹出
            tryStack_.pop_back();
        }

        // 当前帧无处理器 — 弹出帧，传播到调用者
        if (frames_.size() <= 1) {
            // 无更多帧 — 未捕获的异常
            // P2-5 fix: 限制异常值 toString 长度，防止循环引用对象导致超长输出
            std::string str = thrownValue.toString();
            if (str.size() > 200) str = str.substr(0, 200) + "...";
            return runtimeError("未捕获的异常: " + str);
        }

        size_t savedReturnIp = frames_.back().returnIp;
        size_t savedBp = frames_.back().basePointer;
        size_t poppedFrameIdx = frames_.size() - 1;
        // P0-5 fix: 弹帧前关闭该帧关联的 open upvalues
        closeUpvaluesFrom(savedBp);
        frames_.pop_back();
        // Bug3 fix: 清理属于被弹出帧的 tryStack_ handler（与 OP_RETURN 保持一致）
        while (!tryStack_.empty() && tryStack_.back().frameIndex >= poppedFrameIdx) {
            tryStack_.pop_back();
        }

        // 截断栈到调用者的 basePointer
        if (savedBp <= stack_.size()) {
            stack_.resize(savedBp);
        }

        // 设置调用者的 ip，继续在调用者帧中搜索
        currentFrame().ip = savedReturnIp;
        // 循环回到顶部，在调用者帧中搜索 tryStack_
    }
}

void VM::closeUpvaluesFrom(size_t fromSlot) {
    // F11-fix: 关闭指向 [fromSlot, stack_.size()) 范围内栈槽的 open upvalues
    // B5 fix: openUpvalues_ 现为按 stackSlot 排序的 multimap，lower_bound(fromSlot) 定位起始点，
    // 一次性关闭并擦除 [fromSlot, ∞) 全部条目，复杂度从 O(n) 降为 O(log n + k)。
    auto it = openUpvalues_.lower_bound(fromSlot);
    while (it != openUpvalues_.end()) {
        if (auto uv = it->second.lock()) {  // closure 仍持有强引用则有效
            if (!uv->isClosed) {
                if (uv->stackSlot < stack_.size()) {
                    uv->value = stack_[uv->stackSlot];
                } else {
                    // BUGFIX-P2 fix: 防御性警告 — 正常情况下不应到达此分支
                    // 若触发说明 close/truncate 顺序有误，闭包将静默捕获 null
                    Logger::Warning("closeUpvaluesFrom: slot " + std::to_string(uv->stackSlot) +
                                 " >= stack size " + std::to_string(stack_.size()), "VM");
                }
                uv->isClosed = true;
            }
        }
        it = openUpvalues_.erase(it);
    }
}

bool VM::fillDefaultArgs(const BytecodeChunk& chunk, uint8_t& argCount,
                         const std::string& funName, std::vector<Value>& defaults) {
    // F10-fix: 为 init 方法填充缺失的默认参数
    if (argCount < static_cast<uint8_t>(chunk.requiredArity) ||
        argCount > static_cast<uint8_t>(chunk.arity)) {
        return false;
    }
    if (argCount < static_cast<uint8_t>(chunk.arity)) {
        int missingCount = chunk.arity - argCount;
        int defaultStartIdx = static_cast<int>(chunk.defaultConstIndices.size()) - missingCount;
        if (defaultStartIdx < 0 ||
            static_cast<size_t>(defaultStartIdx + missingCount) > chunk.defaultConstIndices.size()) {
            return false;
        }
        for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
            uint16_t constIdx = chunk.defaultConstIndices[i];
            if (constIdx == 0xFFFF) return false;
            if (constIdx >= chunk.constants.size()) return false;
            defaults.push_back(chunk.constants[constIdx]);
        }
        argCount = static_cast<uint8_t>(chunk.arity);
    }
    return true;
}

std::string VM::getLastError() const {
    return lastError_;
}

int VM::getLastErrorLine() const {
    return lastErrorLine_;
}

bool VM::hasError() const {
    return hasError_;
}

std::vector<Value> VM::getStack() const {
    return stack_.toVector();
}

size_t VM::getCurrentIP() const {
    if (frames_.empty()) return 0;
    return frames_.back().ip;
}

OpCode VM::getCurrentOpCode() const {
    if (frames_.empty()) return OpCode::OP_NULL;
    const BytecodeChunk& chunk = *frames_.back().chunk;
    size_t ip = frames_.back().ip;
    if (ip >= chunk.code.size()) return OpCode::OP_NULL;
    return static_cast<OpCode>(chunk.code[ip]);
}

int VM::getCurrentLine() const {
    if (frames_.empty()) return 0;
    const BytecodeChunk& chunk = *frames_.back().chunk;
    size_t ip = frames_.back().ip;
    if (ip >= chunk.lines.size()) return 0;
    return chunk.lines[ip];
}

std::string VM::getCurrentChunkName() const {
    if (frames_.empty()) return "";
    return frames_.back().functionName;
}

std::vector<VM::VMCallStackEntry> VM::getCallStack() const {
    std::vector<VMCallStackEntry> result;
    result.reserve(frames_.size());
    for (const auto& frame : frames_) {
        VMCallStackEntry entry;
        entry.functionName = frame.functionName;
        entry.ip = frame.ip;
        // 获取该帧当前行号
        if (frame.chunk && frame.ip < frame.chunk->lines.size()) {
            entry.line = frame.chunk->lines[frame.ip];
        } else {
            entry.line = 0;
        }
        result.push_back(entry);
    }
    return result;
}

void VM::setOutputCallback(std::function<void(const std::string&)> callback) {
    outputCallback_ = callback;
}

void VM::setInputCallback(std::function<std::string(const std::string&)> callback) {
    inputCallback_ = callback;
}

void VM::setStepCallback(std::function<void(const VMStepInfo&)> callback) {
    stepCallback_ = callback;
}

void VM::setStepCallbackEnabled(bool enabled) {
    stepCallbackEnabled_ = enabled;
}


VMCallFrame& VM::currentFrame() {
    // 调用方契约：execute/stepOnce 在调用前已检查 frames_.empty()
    // B3 fix: 越界时调用 runtimeError（设置 hasError_ + 诊断）后抛 std::runtime_error，
    // 替代原 std::abort()。调用方（VmStepper::stepByMode / runBatch）已用 try/catch 包裹，
    // 抛出会被捕获并转化为 ERROR 状态，避免 IDE 整个进程崩溃。
    // 与 RegisterVM::currentFrame() 保持对称处理。
    assert(!frames_.empty() && "currentFrame() on empty frames");
    if (frames_.empty()) {
        runtimeError("currentFrame() on empty frames");
        throw std::runtime_error("VM: currentFrame() on empty frames");
    }
    return frames_.back();
}

const BytecodeChunk& VM::currentChunk() {
    return *currentFrame().chunk;  // chunk 由 initExecution 设置，始终有效
}

const BytecodeChunk* VM::findMethodChunk(const std::string& className,
                                         const std::string& methodName) const {
    // P4 fix: 先查缓存
    auto clsIt = classInfo_.find(className);
    if (clsIt != classInfo_.end()) {
        auto& cache = clsIt->second.methodCache;
        auto cacheIt = cache.find(methodName);
        if (cacheIt != cache.end()) {
            return cacheIt->second;  // 缓存命中（含 nullptr 表示方法不存在）
        }
    }

    // #12 fix: 冷路径走两级 unordered_map 索引（methodsByClass_），消除每层
    // 继承链 "Class.method" 字符串拼接。语义等价：自底向上沿继承链查找首个命中。
    std::string cur = className;
    for (int guard = 0; guard < MAX_INHERITANCE_DEPTH && !cur.empty(); ++guard) {
        auto clsIdxIt = methodsByClass_.find(cur);
        if (clsIdxIt != methodsByClass_.end()) {
            auto mIt = clsIdxIt->second.find(methodName);
            if (mIt != clsIdxIt->second.end()) {
                // P4: 写入缓存
                if (clsIt != classInfo_.end()) {
                    clsIt->second.methodCache[methodName] = mIt->second;
                }
                return mIt->second;
            }
        }
        auto nextIt = classInfo_.find(cur);
        if (nextIt == classInfo_.end()) break;
        cur = nextIt->second.superClassName;
    }
    // P4: 缓存 nullptr（方法不存在），避免重复查找
    if (clsIt != classInfo_.end()) {
        clsIt->second.methodCache[methodName] = nullptr;
    }
    return nullptr;
}

// 数值运算类型枚举（避免字符串比较）
enum { OP_ADD_INT = 0, OP_SUB_INT, OP_MUL_INT, OP_DIV_INT, OP_MOD_INT };

VMResult VM::numericOp(int opType) {
    // 使用 peek 访问栈顶避免深拷贝，然后调整栈指针
    if (stack_.size() < 2) return runtimeError("栈下溢：二元运算需要两个操作数");

    const Value& rightRef = stack_[stack_.size() - 1];
    const Value& leftRef = stack_[stack_.size() - 2];

    // 字符串拼接（仅加法）
    if (opType == OP_ADD_INT) {
        if (leftRef.isString() && rightRef.isString()) {
            const auto& ls = leftRef.stringVal();
            const auto& rs = rightRef.stringVal();
            std::string concat;
            concat.reserve(ls.size() + rs.size());
            concat.append(ls).append(rs);
            stack_[stack_.size() - 2] = Value(std::move(concat));
            stack_.pop_back();
            return VMResult::VM_OK;
        }
        if (leftRef.isString() || rightRef.isString()) {
            // S5 fix: 字符串侧用 stringVal() 引用，非字符串侧才 toString()，消除冗余拷贝
            std::string result;
            if (leftRef.isString()) {
                const auto& ls = leftRef.stringVal();
                auto rs = rightRef.toString();
                result.reserve(ls.size() + rs.size());
                result.append(ls).append(rs);
            } else {
                auto ls = leftRef.toString();
                const auto& rs = rightRef.stringVal();
                result.reserve(ls.size() + rs.size());
                result.append(ls).append(rs);
            }
            stack_[stack_.size() - 2] = Value(std::move(result));
            stack_.pop_back();
            return VMResult::VM_OK;
        }
    }

    // 非数值类型检查（字符串拼接已在上面处理）
    if (!leftRef.isNumber() || !rightRef.isNumber()) {
        return runtimeError("算术运算需要数值类型");
    }

    // C12 fix: 调用共享算术运算逻辑，消除与 Interpreter 的重复实现
    using namespace NumericOps;
    ArithOp op;
    switch (opType) {
    case OP_ADD_INT: op = ArithOp::Add; break;
    case OP_SUB_INT: op = ArithOp::Sub;  break;
    case OP_MUL_INT: op = ArithOp::Mul;  break;
    case OP_DIV_INT: op = ArithOp::Div;  break;
    case OP_MOD_INT: op = ArithOp::Mod;  break;
    default:
        return runtimeError("未知算术运算类型");
    }

    ArithResult r = computeArith(op, leftRef.isInt(), leftRef.isInt() ? leftRef.intVal() : 0, leftRef.toDouble(),
                                 rightRef.isInt(), rightRef.isInt() ? rightRef.intVal() : 0, rightRef.toDouble());
    switch (r.status) {
    case ArithStatus::DivByZero:
        return runtimeError("除零错误");
    case ArithStatus::IntOverflow:
        return runtimeError("整数运算溢出");
    case ArithStatus::NotNumeric:
        return runtimeError("算术运算需要数值类型");
    case ArithStatus::OK:
        stack_[stack_.size() - 2] = r.isIntResult ? Value(r.intVal) : Value(r.floatVal);
        break;
    default:
        // ArithStatus 是封闭枚举，落空表示内部错误；不 break 以避免
        // stack_[size-2] 未被写入却在下方 pop_back 后成为脏结果。
        return runtimeError("内部错误: 未知算术状态");
    }

    stack_.pop_back();  // 弹出 right，保留结果在 left 原位
    return VMResult::VM_OK;
}

// ---- C10: 内建方法名枚举分发（#20 fix: 已提取为共享自由函数 classifyBuiltinMethod
//          于 BuiltinMethods.h/.cpp，VM 与 RegisterVM 共用）----

// ---- C11: 比较运算辅助函数 ----
VMResult VM::pushCompareResult(bool result, size_t& ip, OpCode opcode) {
    stack_[stack_.size() - 2] = Value(result);
    stack_.pop_back();
    notifyStep(ip, opcode);
    ip += 1;
    return VMResult::VM_OK;
}

// ============================================================
// writeBack 接收者写回机制（ARCH-24 设计说明）
// ------------------------------------------------------------
// 背景：MiniLang 方法调用语义要求变异方法（如 arr.push()、dict.remove()、
// 实例字段写入）的修改回写到原始变量/字段。由于 VM 栈式执行，方法调用时
// 接收者被拷贝到新栈帧，方法内的修改不会自动反映到调用者栈帧。
//
// 编译期编码（Compiler::visitMethodCall）：
//   编译器检测接收者是否为简单 VarRef：
//     - 全局变量：编码 receiverVarIdx（常量池索引，0xFFFF 表示无）
//     - 局部变量：编码 receiverLocalSlot（栈槽偏移，0xFF 表示无）
//     - 复杂表达式（如 obj.x.y.f()）：不写回，结果存入 lastMutatedReceiver_
//   这些编码附加在 OP_METHOD_CALL 指令后，VM 执行时读取。
//
// 运行期写回（VM::writeBackReceiver）：
//   方法返回时（OP_RETURN），VM 根据 receiverVarIdx/receiverLocalSlot 判断
//   写回目标，执行三分支写回：
//     1. receiverVarIdx != 0xFFFF → 写入 globalSlots_（槽位寻址）或 globals_（名称寻址）
//     2. receiverLocalSlotByte != 0xFF → 写入栈帧（含字段同步到实例）
//     3. 否则 → 存入 lastMutatedReceiver_（供调用方通过 OP_GET_LAST_MUTATED 读取）
//
// 字段同步优化（PERF-13 fix）：
//   fieldsModified 脏标记由 OP_SET_LOCAL/OP_MEMBER_SET_LOCAL 在写入字段槽时置位。
//   OP_RETURN 仅在 fieldsModified=true 时执行字段同步循环，跳过只读方法
//   和无字段写入的 init，避免冗余拷贝。
//
// 链式调用（dispatchArrayBuiltin/dispatchDictBuiltin）：
//   内建方法分发器在变异路径（ARR_PUSH/POP/REMOVE、DICT_REMOVE）后调用
//   writeBackReceiver(..., true) 强制写回。非变异路径（ARR_LEN/CONTAINS、
//   DICT_LEN/KEYS/VALUES/HAS/GET）不调用写回。
//
// P7 fix：字段同步先拷贝后 move 的顺序
//   receiverLocalSlotByte == 0 时（this 槽位），先拷贝 mutatedObj 到字段，
//   再 move 到栈槽。若先 move 则拷贝时 mutatedObj 已被掏空。
// ============================================================

// 写回变异方法调用后的接收者（统一数组/字典/实例三处写回逻辑）
// 判断链：
//   1. receiverVarIdx != 0xFFFF → 写入 globalSlots_ 或 globals_
//   2. receiverLocalSlotByte != 0xFF → 写入栈帧（含字段同步）
//   3. 否则 → 存入 lastMutatedReceiver_
VMResult VM::writeBackReceiver(uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte,
                               Value& mutatedObj, bool fieldsModified) {
    const BytecodeChunk& chunk = currentChunk();
    if (receiverVarIdx != 0xFFFF && receiverVarIdx < chunk.constants.size()) {
        // 防御性检查：常量条目应为字符串（编译期由 IR visitMethodCall 写入）。
        // 若字节码被恶意构造或 lowering 存在 bug 写入了非字符串常量，
        // stringVal() 行为未定义。显式报错优于静默 UB。
        if (!chunk.constants[receiverVarIdx].isString()) {
            return runtimeError("writeBackReceiver: 接收者常量非字符串类型");
        }
        const std::string& recvName = chunk.constants[receiverVarIdx].stringVal();
        auto gsIt = globalNameToSlot_.find(recvName);
        if (gsIt != globalNameToSlot_.end()) {
            globalSlots_[gsIt->second] = std::move(mutatedObj);
        } else {
            globals_[recvName] = std::move(mutatedObj);
        }
    } else if (receiverLocalSlotByte != 0xFF) {
        size_t bp = currentFrame().basePointer;
        // P7 fix: 先拷贝到字段（需要完整副本），再 move 到栈槽
        if (fieldsModified && receiverLocalSlotByte > 0 && bp < stack_.size() && stack_[bp].isInstance()) {
            VMCallFrame& curFrame = currentFrame();
            if (curFrame.chunk && receiverLocalSlotByte <= curFrame.chunk->fieldOrder.size()) {
                const std::string& fn = curFrame.chunk->fieldOrder[receiverLocalSlotByte - 1];
                stack_[bp].fields()[fn] = mutatedObj;  // 拷贝（move 前）
            }
        }
        if (bp + receiverLocalSlotByte < stack_.size()) {
            stack_[bp + receiverLocalSlotByte] = std::move(mutatedObj);  // move 在后
        }
    } else {
        lastMutatedReceiver_ = std::move(mutatedObj);
    }
    return VMResult::VM_OK;
}

// ============================================================
// B7 fix: 内建方法分发（从 executeCallOps 提取）
// ============================================================

// ---- P0-3 fix: 共享内置方法分派样板提取 ----
VMResult VM::finishSharedBuiltin(Result<Value>&& sr, size_t& ip, OpCode op, int instrLen) {
    if (sr.is_err()) return runtimeError(sr.error().message);
    pop();  // 弹出接收者
    push(std::move(sr.value()));
    notifyStep(ip, op);
    ip += instrLen;
    return VMResult::VM_OK;
}

bool VM::extractSharedBuiltin(Result<Value>&& sr, Value& out) {
    if (sr.is_err()) {
        runtimeError(sr.error().message);
        return false;
    }
    out = std::move(sr.value());
    return true;
}

// ---- 数组内建方法 ----
VMResult VM::dispatchArrayBuiltin(const Value& obj, BuiltinMethod method,
                                   const std::string& methodName, uint8_t argCount,
                                   uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte,
                                   size_t& ip, OpCode op, int instrLen) {
    // 先弹出参数（接收者仍在栈上，const ref 有效）
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) args[i] = pop();
    // V-P1-4 fix: pop 循环后检查 hasError_，避免在 null 上继续执行
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;

    // 非变异路径：从 const 引用计算结果，无需拷贝接收者
    Value result = Value::nullValue();
    if (method == BuiltinMethod::ARR_LEN) {
        return finishSharedBuiltin(executeSharedLen(obj, args.begin(), args.size()), ip, op, instrLen);
    }
    if (method == BuiltinMethod::ARR_CONTAINS) {
        return finishSharedBuiltin(executeSharedArrayContains(obj, args.begin(), args.size()), ip, op, instrLen);
    }
    if (method == BuiltinMethod::ARR_JOIN) {
        return finishSharedBuiltin(executeSharedArrayJoin(obj, args.begin(), args.size()), ip, op, instrLen);
    }

    // A2 变异路径：先 pop 接收者(move, refcount 不变)，再通过 getMutableArrayRef 原地修改
    Value mutableObj = std::move(stack_.back());
    stack_.pop_back();

    if (method == BuiltinMethod::ARR_PUSH) {
        if (args.size() != 1) return runtimeError("push 期望 1 个参数");
        // P2-9 fix: 使用 getMutableArrayRef 统一 COW 变异模式
        getMutableArrayRef(mutableObj).push_back(std::move(args[0]));
    } else if (method == BuiltinMethod::ARR_POP) {
        // P2-9 fix: 使用 getMutableArrayRef 统一 COW 变异模式
        auto& arr = getMutableArrayRef(mutableObj);
        if (arr.empty()) return runtimeError("对空数组调用 pop");
        result = std::move(arr.back());
        arr.pop_back();
    } else if (method == BuiltinMethod::ARR_REMOVE) {
        if (args.size() != 1) return runtimeError("remove 期望 1 个参数(索引)");
        if (!args[0].isInt()) return runtimeError("remove 参数必须是整数索引");
        int64_t ri = args[0].intVal();
        // P2-9 fix: 使用 getMutableArrayRef 统一 COW 变异模式
        auto& arr = getMutableArrayRef(mutableObj);
        if (ri < 0 || static_cast<size_t>(ri) >= arr.size())
            return runtimeError(ErrorFormat::format("数组索引越界: %lld", static_cast<long long>(ri)));
        arr.erase(arr.begin() + static_cast<size_t>(ri));
    } else {
        return runtimeError("数组没有方法 " + methodName);
    }

    // 写回变异后的对象（P7 fix: 使用 std::move 避免二次深拷贝）
    writeBackReceiver(receiverVarIdx, receiverLocalSlotByte, mutableObj, true);
    // V-P2-20 fix: result 后续不再使用，std::move 入栈
    push(std::move(result));
    notifyStep(ip, op);
    ip += instrLen;
    return VMResult::VM_OK;
}

// ---- 字典内建方法 ----
VMResult VM::dispatchDictBuiltin(const Value& obj, BuiltinMethod method,
                                  const std::string& methodName, uint8_t argCount,
                                  uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte,
                                  size_t& ip, OpCode op, int instrLen) {
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) args[i] = pop();
    // V-P1-4 fix: pop 循环后检查 hasError_
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;

    Value result = Value::nullValue();

    // 非变异路径：直接从 const 引用读取
    if (method == BuiltinMethod::DICT_LEN || method == BuiltinMethod::ARR_LEN) {
        return finishSharedBuiltin(executeSharedLen(obj, args.begin(), args.size()), ip, op, instrLen);
    }
    if (method == BuiltinMethod::DICT_KEYS) {
        return finishSharedBuiltin(executeSharedDictKeys(obj, args.begin(), args.size()), ip, op, instrLen);
    }
    if (method == BuiltinMethod::DICT_VALUES) {
        return finishSharedBuiltin(executeSharedDictValues(obj, args.begin(), args.size()), ip, op, instrLen);
    }
    if (method == BuiltinMethod::DICT_HAS || method == BuiltinMethod::ARR_CONTAINS) {
        return finishSharedBuiltin(executeSharedDictHas(obj, methodName, args.begin(), args.size()), ip, op, instrLen);
    }
    if (method == BuiltinMethod::DICT_GET) {
        return finishSharedBuiltin(executeSharedDictGet(obj, args.begin(), args.size()), ip, op, instrLen);
    }

    // A2 变异路径（remove）：先 pop 再原地修改
    Value mutableObj = std::move(stack_.back());
    stack_.pop_back();

    if (method == BuiltinMethod::DICT_REMOVE || method == BuiltinMethod::ARR_REMOVE) {
        if (args.size() != 1) return runtimeError("remove 期望 1 个参数(键)");
        // P2-9 fix: 使用 getMutableDictRef 统一 COW 变异模式
        getMutableDictRef(mutableObj).erase(args[0].toString());
    } else if (method == BuiltinMethod::DICT_SET) {
        if (args.size() != 2) return runtimeError("set 期望 2 个参数(键, 值)");
        getMutableDictRef(mutableObj)[args[0].toString()] = args[1];
    } else {
        return runtimeError("字典没有方法 " + methodName);
    }

    // 写回（P7 fix: 使用 std::move 避免二次深拷贝）
    writeBackReceiver(receiverVarIdx, receiverLocalSlotByte, mutableObj, true);
    // V-P2-20 fix: result 后续不再使用，std::move 入栈
    push(std::move(result));
    notifyStep(ip, op);
    ip += instrLen;
    return VMResult::VM_OK;
}

// ---- 字符串内建方法（全部非变异，使用 const 引用）----
VMResult VM::dispatchStringBuiltin(const Value& obj, BuiltinMethod method,
                                    const std::string& methodName, uint8_t argCount,
                                    size_t& ip, OpCode op, int instrLen) {
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) args[i] = pop();
    // V-P1-4 fix: pop 循环后检查 hasError_
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;

    Value result = Value::nullValue();

    if (method == BuiltinMethod::STR_LEN || method == BuiltinMethod::ARR_LEN || method == BuiltinMethod::DICT_LEN) {
        if (!extractSharedBuiltin(executeSharedLen(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_UPPER) {
        if (!extractSharedBuiltin(executeSharedStrUpper(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_LOWER) {
        if (!extractSharedBuiltin(executeSharedStrLower(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_SPLIT) {
        if (!extractSharedBuiltin(executeSharedStrSplit(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_TRIM) {
        if (!extractSharedBuiltin(executeSharedStrTrim(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_CONTAINS || method == BuiltinMethod::ARR_CONTAINS) {
        // ARCH-15 fix: 改用 executeSharedStrContains 共享函数，消除与 Interpreter 的内联重复
        if (!extractSharedBuiltin(executeSharedStrContains(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_STARTS_WITH) {
        if (!extractSharedBuiltin(executeSharedStrStartsWith(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_ENDS_WITH) {
        if (!extractSharedBuiltin(executeSharedStrEndsWith(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_REPLACE) {
        if (!extractSharedBuiltin(executeSharedStrReplace(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_SUBSTR) {
        if (!extractSharedBuiltin(executeSharedStrSubstr(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else if (method == BuiltinMethod::STR_INDEX_OF) {
        if (!extractSharedBuiltin(executeSharedStrIndexOf(obj, args.begin(), args.size()), result))
            return VMResult::VM_RUNTIME_ERROR;
    } else {
        return runtimeError("字符串没有方法 " + methodName);
    }

    pop();  // 移除接收者（在计算完成后）
    // V-P2-20 fix: result 后续不再使用，std::move 入栈
    push(std::move(result));
    notifyStep(ip, op);
    ip += instrLen;
    return VMResult::VM_OK;
}

// ============================================================
// 初始化 / 单步 / 状态查询
// ============================================================

void VM::initExecution(const CompileResult& result) {
    stack_.clear();
    // PERF-13: VMStack 使用定长数组，无需 reserve
    globals_.clear();
    lastError_.clear();
    lastErrorLine_ = 0;
    hasError_ = false;
    diagnostics_.clear();
    frames_.clear();
    // 预分配到 MAX_FRAMES 上限：push_back 前已有 frames_.size() < MAX_FRAMES 检查，
    // reserve 到上限可保证 push_back 永不 realloc / 抛 bad_alloc，从而消除
    // OP_CALL 路径中"extraSlots 已推入栈但 frames_.push_back 抛异常"的状态不一致。
    frames_.reserve(MAX_FRAMES);
    functionChunks_ = result.functionChunks;
    // #12 fix: 构建 类→方法名→chunk 两级索引（仅收录 "Class.method" 条目）。
    // 用首个 '.' 拆分；普通函数名（无 '.'）不入索引。functionChunks_ 是 std::map
    // 节点稳定，存储的 BytecodeChunk* 在 functionChunks_ 生命周期内有效。
    methodsByClass_.clear();
    methodsByClass_.reserve(functionChunks_.size());
    for (auto& kv : functionChunks_) {
        const std::string& key = kv.first;
        auto dotPos = key.find('.');
        if (dotPos != std::string::npos && dotPos > 0 && dotPos + 1 < key.size()) {
            methodsByClass_[key.substr(0, dotPos)][key.substr(dotPos + 1)] = &kv.second;
        }
    }
    // P3: 清除函数调用缓存（functionChunks_ 地址已变）
    // PERF-14 fix: unordered_map 替代数组，clear() 即可
    callCache_.clear();
    // P2: 清除全局变量缓存
    globalCache_.clear();
    classInfo_.clear();
    // M-新1 fix: 清理残留闭包/upvalue 状态，避免多次执行时悬空指针
    openUpvalues_.clear();
    functionClosures_.clear();
    lastMutatedReceiver_ = Value::nullValue();
    pendingFieldOrder_.clear();
    tryStack_.clear();  // F11: 清理异常处理栈
    // P1 fix: 重置 stepOnce 指令计数器和 ASCII 缓存
    stepInstructionCount_ = 0;
    lastAsciiStrPtr_ = nullptr;
    lastAsciiStrIsAscii_ = false;
    // A2/B4: 初始化全局变量槽位
    globalSlots_.clear();
    globalNameToSlot_.clear();
    globalSlots_.resize(result.globalSlotCount);
    // B4: 不再存储 globalSlotNames_，直接构建 globalNameToSlot_（调试时从逆映射重建）
    for (int i = 0; i < result.globalSlotCount; ++i) {
        globalNameToSlot_[result.globalSlotNames[i]] = i;
    }
    mainChunk_ = result.mainChunk;  // 持有主 chunk 副本，避免悬垂指针

    // 设置主帧
    VMCallFrame mainFrame;
    mainFrame.chunk = &mainChunk_;  // 指向 VM 自持的副本
    mainFrame.ip = 0;
    mainFrame.basePointer = 0;
    mainFrame.functionName = "main";
    frames_.push_back(std::move(mainFrame));

    // IR 路径主帧局部槽预留：IR 的 AND/OR 短路等在顶层代码用 STORE_LOCAL/LOAD_LOCAL
    // 访问临时局部槽（nextLocalSlot_ 分配），但 StackVM 主帧 basePointer=0 且栈初始为空，
    // OP_SET_LOCAL/OP_GET_LOCAL 的 bp+slot 越界检查会失败（"局部变量槽越界"）。
    // 正常 Compiler 路径顶层代码用 OP_DEFINE_VAR（globals_ 哈希），localCount=0，此处为 no-op。
    if (mainChunk_.localCount > 0) {
        for (int i = 0; i < mainChunk_.localCount; ++i) {
            stack_.push_back(Value::nullValue());
        }
    }

    initialized_ = true;
}

bool VM::isFinished() const {
    return frames_.empty();
}

bool VM::isInitialized() const {
    return initialized_;
}

void VM::resetState() {
    stack_.clear();
    globals_.clear();
    lastError_.clear();
    lastErrorLine_ = 0;
    hasError_ = false;
    diagnostics_.clear();
    frames_.clear();
    functionChunks_.clear();
    classInfo_.clear();
    methodsByClass_.clear();  // #12 fix: 同步清理类方法索引
    globalSlots_.clear();
    // B4: globalSlotNames_ 已删除
    globalNameToSlot_.clear();
    mainChunk_ = BytecodeChunk();  // 清空主 chunk 副本
    lastMutatedReceiver_ = Value::nullValue();
    pendingFieldOrder_.clear();
    tryStack_.clear();  // F11: 清理异常处理栈
    openUpvalues_.clear();       // VM-05/06
    functionClosures_.clear();   // VM-05/06
    // P1 fix: 重置 stepOnce 指令计数器和 ASCII 缓存
    stepInstructionCount_ = 0;
    lastAsciiStrPtr_ = nullptr;
    lastAsciiStrIsAscii_ = false;
    // V-P1-1 fix: 清理内联缓存，避免悬垂指针（callCache_/globalCache_ 指向已清空的容器）
    // PERF-14 fix: unordered_map clear()
    callCache_.clear();
    globalCache_.clear();
    initialized_ = false;
}

VMResult VM::stepOnce() {
    // 帧已空 → 执行完毕
    if (frames_.empty()) return VMResult::VM_OK;

    // 已有错误 → 不再执行
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;

    // P1 fix: stepOnce 累计指令预算检查，防止通过循环调用 stepOnce 绕过 DoS 防护
    if (++stepInstructionCount_ > MAX_INSTRUCTIONS) {
        lastError_ = ErrorFormat::format("指令执行数超过上限 %lld，疑似无限循环",
                                          static_cast<long long>(MAX_INSTRUCTIONS));
        lastErrorLine_ = 0;
        hasError_ = true;
        return VMResult::VM_RUNTIME_ERROR;
    }

    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    size_t& ip = frame.ip;

    // 当前 chunk 执行完毕 → 弹帧（防御性路径，正常情况由 OP_RETURN 处理）
    if (ip >= chunk.code.size()) {
        size_t returnIp = frame.returnIp;
        size_t savedBp = frame.basePointer;
        bool wasInit = frame.isInitCall;
        // P1-5 fix: 记录帧索引并清理 tryStack_ handler
        size_t returningFrameIdx = frames_.size() - 1;
        closeUpvaluesFrom(savedBp);  // P1-5 fix: 关闭 open upvalues
        frames_.pop_back();
        while (!tryStack_.empty() && tryStack_.back().frameIndex >= returningFrameIdx) {
            tryStack_.pop_back();  // P1-5 fix: 清理残留 handler
        }
        if (!frames_.empty()) {
            currentFrame().ip = returnIp;  // 恢复调用者 ip，避免重复执行调用指令
            // V-P1-5 fix: 先截断栈清理 callee 残留局部变量，再 push 返回值
            // 原代码直接 push 导致栈布局为 [调用者数据][callee 残留局部变量][返回值]
            // AUDIT-BUG-V1 fix: wasInit 分支访问 stack_[savedBp] 需 savedBp < stack_.size()，
            // 原条件 savedBp <= stack_.size() 允许 savedBp == top_ 触发 VMStack::operator[] abort。
            if (savedBp <= stack_.size()) {
                if (wasInit && savedBp < stack_.size()) {
                    Value thisVal = stack_[savedBp];  // 先拷出 this
                    stack_.resize(savedBp);            // 清理 callee 残留
                    push(std::move(thisVal));
                } else {
                    stack_.resize(savedBp);
                    push(Value::nullValue());
                }
            } else {
                push(Value::nullValue());
            }
        }
        return VMResult::VM_OK;
    }

    return executeOneInstruction();
}

// ============================================================
// execute() — 全速执行
// ============================================================

VMResult VM::execute(const CompileResult& result) {
    initExecution(result);

    // S-02 fix: 指令执行预算，防止恶意字节码导致 DoS
    int64_t instructionCount = 0;
    while (!frames_.empty()) {
        VMCallFrame& frame = currentFrame();
        const BytecodeChunk& chunk = *frame.chunk;
        size_t& ip = frame.ip;

        if (ip >= chunk.code.size()) {
            // 当前 chunk 执行完毕 → 弹帧（防御性路径，正常情况由 OP_RETURN 处理）
            size_t returnIp = frame.returnIp;  // 保存调用者返回地址
            size_t savedBp = frame.basePointer;
            bool wasInit = frame.isInitCall;
            // P1-5 fix: 记录帧索引并清理 tryStack_ handler
            size_t returningFrameIdx = frames_.size() - 1;
            closeUpvaluesFrom(savedBp);  // P1-5 fix: 关闭 open upvalues
            frames_.pop_back();
            while (!tryStack_.empty() && tryStack_.back().frameIndex >= returningFrameIdx) {
                tryStack_.pop_back();  // P1-5 fix: 清理残留 handler
            }
            if (!frames_.empty()) {
                currentFrame().ip = returnIp;   // 恢复调用者 ip，避免重复执行调用指令
                // V-P1-5 fix: 先截断栈清理 callee 残留局部变量，再 push 返回值
                // AUDIT-BUG-V1 fix: wasInit 分支访问 stack_[savedBp] 需 savedBp < stack_.size()
                if (savedBp <= stack_.size()) {
                    if (wasInit && savedBp < stack_.size()) {
                        Value thisVal = stack_[savedBp];
                        stack_.resize(savedBp);
                        push(std::move(thisVal));
                    } else {
                        stack_.resize(savedBp);
                        push(Value::nullValue());
                    }
                } else {
                    push(Value::nullValue());
                }
            }
            continue;
        }

        if (hasError_) return VMResult::VM_RUNTIME_ERROR;
        // S-02 fix: 检查指令预算
        if (++instructionCount > MAX_INSTRUCTIONS) {
            lastError_ = ErrorFormat::format("指令执行数超过上限 %lld，疑似无限循环",
                                          static_cast<long long>(MAX_INSTRUCTIONS));
            lastErrorLine_ = 0;
            hasError_ = true;
            return VMResult::VM_RUNTIME_ERROR;
        }
        VMResult r = executeOneInstruction();
        if (r != VMResult::VM_OK || hasError_) return VMResult::VM_RUNTIME_ERROR;
    }

    if (hasError_) return VMResult::VM_RUNTIME_ERROR;
    return VMResult::VM_OK;
}

// ============================================================
// executeOneInstruction() — 单条指令执行（核心逻辑）
// ============================================================
// A3 注记：MSVC 对密集 switch 已自动生成跳转表（jump table），
// 函数指针表的额外重构（1600+ 行拆分为 50+ 方法）收益极小。
// 保留 switch 形式，确保代码可维护性。


// ============================================================
// executeOneInstruction() — 单条指令执行（按指令类别转发到私有方法）
// ============================================================

VMResult VM::executeOneInstruction() {
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    size_t& ip = frame.ip;

    OpCode op = static_cast<OpCode>(chunk.code[ip]);

    // M-新2 fix: OP_CLOSURE 是变长指令，需要计算完整长度再做边界检查
    size_t instrSize = BytecodeChunk::instructionSize(op);
    if (op == OpCode::OP_CLOSURE && ip + 3 < chunk.code.size()) {
        uint8_t uvCount = chunk.code[ip + 3];
        instrSize = 4 + static_cast<size_t>(uvCount) * 2;
    }
    if (ip + instrSize > chunk.code.size()) {
        return runtimeError("字节码截断: 指令不完整");
    }

    switch (op) {
    // 常量加载类
    case OpCode::OP_CONSTANT:
    case OpCode::OP_INT:
    case OpCode::OP_FLOAT:
    case OpCode::OP_STRING:
    case OpCode::OP_NULL:
    case OpCode::OP_TRUE:
    case OpCode::OP_FALSE:
        return executeConstantOps(op, ip);

    // 算术运算类
    case OpCode::OP_ADD:
    case OpCode::OP_SUBTRACT:
    case OpCode::OP_MULTIPLY:
    case OpCode::OP_DIVIDE:
    case OpCode::OP_MODULO:
    case OpCode::OP_NEGATE:
        return executeArithOps(op, ip);

    // 比较与逻辑运算类
    case OpCode::OP_EQUAL:
    case OpCode::OP_NOT_EQUAL:
    case OpCode::OP_LESS:
    case OpCode::OP_GREATER:
    case OpCode::OP_LESS_EQUAL:
    case OpCode::OP_GREATER_EQUAL:
    case OpCode::OP_NOT:
        return executeCompareOps(op, ip);

    // 变量操作类
    case OpCode::OP_DEFINE_VAR:
    case OpCode::OP_GET_VAR:
    case OpCode::OP_SET_VAR:
    case OpCode::OP_DELETE_VAR:
    case OpCode::OP_GET_GLOBAL:
    case OpCode::OP_SET_GLOBAL:
    case OpCode::OP_DEFINE_GLOBAL:
    case OpCode::OP_DELETE_GLOBAL:
    case OpCode::OP_GET_LOCAL:
    case OpCode::OP_SET_LOCAL:
    case OpCode::OP_GET_UPVALUE:
    case OpCode::OP_SET_UPVALUE:
    case OpCode::OP_CLOSE_UPVALUE:
        return executeVarOps(op, ip);

    // 调用相关类
    case OpCode::OP_CALL:
    case OpCode::OP_CALL_EXPR:
    case OpCode::OP_CLOSURE:
    case OpCode::OP_RETURN:
    case OpCode::OP_SUPER_CALL:
    case OpCode::OP_METHOD_CALL:
    case OpCode::OP_CLASS_NEW:
    case OpCode::OP_DEFINE_CLASS:
        return executeCallOps(op, ip);

    // 容器与成员操作类
    case OpCode::OP_BUILD_ARRAY:
    case OpCode::OP_BUILD_DICT:
    case OpCode::OP_INDEX_GET:
    case OpCode::OP_INDEX_SET:
    case OpCode::OP_INDEX_SET_VAR:
    case OpCode::OP_INDEX_SET_LOCAL:
    case OpCode::OP_MEMBER_GET:
    case OpCode::OP_MEMBER_SET:
    case OpCode::OP_MEMBER_SET_VAR:
    case OpCode::OP_MEMBER_SET_LOCAL:
    case OpCode::OP_SUPER_MEMBER_GET:
        return executeContainerOps(op, ip);

    // 嵌套访问写回类
    case OpCode::OP_WRITEBACK_MEMBER_VAR:
    case OpCode::OP_WRITEBACK_MEMBER_LOCAL:
    case OpCode::OP_WRITEBACK_INDEX_VAR:
    case OpCode::OP_WRITEBACK_INDEX_LOCAL:
    case OpCode::OP_WRITEBACK_MEMBER_UPVALUE:
    case OpCode::OP_WRITEBACK_INDEX_UPVALUE:
        return executeWritebackOps(op, ip);

    // 其他指令
    case OpCode::OP_PRINT:
    case OpCode::OP_POP:
    case OpCode::OP_DUP:
    case OpCode::OP_DUP_N:
    case OpCode::OP_JUMP:
    case OpCode::OP_JUMP_IF_FALSE:
    case OpCode::OP_LOOP:
    case OpCode::OP_INIT_FIELD:
    case OpCode::OP_TRY_BEGIN:
    case OpCode::OP_TRY_END:
    case OpCode::OP_THROW:
    case OpCode::OP_LOAD_MUTATED:
    case OpCode::OP_TYPE_CHECK:
        return executeMiscOps(op, ip);

    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 常量加载类指令
// ============================================================

VMResult VM::executeConstantOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case OpCode::OP_CONSTANT:
    case OpCode::OP_INT:
    case OpCode::OP_FLOAT:
    case OpCode::OP_STRING: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        // PERF-12 fix: 直接 emplace const Value& 到栈槽，避免临时 Value 的额外原子操作
        // push(const Value&) 内部 push_back 是拷贝（shared_ptr 原子递增），
        // emplace(const Value&) 同样是拷贝但显式语义，便于未来替换为更激进的优化。
        push(chunk.constants[idx]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_NULL:
        // PERF-12 fix: 直接 emplace 构造 null Value，避免临时对象
        emplace<>();
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_TRUE:
        // PERF-12 fix: 直接 emplace 构造 bool Value
        emplace<bool>(true);
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_FALSE:
        // PERF-12 fix: 直接 emplace 构造 bool Value
        emplace<bool>(false);
        notifyStep(ip, op);
        ip += 1;
        break;

    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 算术运算类指令
// ============================================================

VMResult VM::executeArithOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    (void)frame;
    (void)chunk;

    switch (op) {
    case OpCode::OP_ADD: {
        VMResult r = numericOp(OP_ADD_INT);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_SUBTRACT: {
        VMResult r = numericOp(OP_SUB_INT);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_MULTIPLY: {
        VMResult r = numericOp(OP_MUL_INT);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_DIVIDE: {
        VMResult r = numericOp(OP_DIV_INT);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_MODULO: {
        VMResult r = numericOp(OP_MOD_INT);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_NEGATE: {
        // P17 fix: 原地修改栈顶，避免 pop+push 开销
        if (stack_.empty()) return runtimeError("栈下溢：NEGATE 运算需要一个操作数");
        auto& top = stack_.back();
        if (top.isInt()) {
            if (OverflowCheck::negateOverflow(top.intVal())) return runtimeError("整数溢出：无法对最小值取负");
            top = Value(-top.intVal());
        }
        else if (top.isFloat()) top = Value(-top.floatVal());
        else return runtimeError("一元减运算需要数值类型");
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 比较与逻辑运算类指令
// ============================================================

VMResult VM::executeCompareOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    (void)frame;
    (void)chunk;

    switch (op) {
    case OpCode::OP_NOT: {
        if (stack_.empty()) return runtimeError("栈下溢：NOT 运算需要一个操作数");
        // P17 fix: 原地修改栈顶
        stack_.back() = Value(!stack_.back().isTruthy());
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_EQUAL: {
        if (stack_.size() < 2) return runtimeError("栈下溢：比较运算需要两个操作数");
        bool result = stack_[stack_.size() - 2].equals(stack_.back());
        return pushCompareResult(result, ip, op);
    }

    case OpCode::OP_NOT_EQUAL: {
        if (stack_.size() < 2) return runtimeError("栈下溢：比较运算需要两个操作数");
        bool result = !stack_[stack_.size() - 2].equals(stack_.back());
        return pushCompareResult(result, ip, op);
    }

    case OpCode::OP_LESS:
        return orderedCompare([](const Value& l, const Value& r) {
            if (l.isString() && r.isString()) return l.stringVal() < r.stringVal();
            return l.toDouble() < r.toDouble();
        }, ip, op);

    case OpCode::OP_GREATER:
        return orderedCompare([](const Value& l, const Value& r) {
            if (l.isString() && r.isString()) return l.stringVal() > r.stringVal();
            return l.toDouble() > r.toDouble();
        }, ip, op);

    case OpCode::OP_LESS_EQUAL:
        return orderedCompare([](const Value& l, const Value& r) {
            if (l.isString() && r.isString()) return l.stringVal() <= r.stringVal();
            return l.toDouble() <= r.toDouble();
        }, ip, op);

    case OpCode::OP_GREATER_EQUAL:
        return orderedCompare([](const Value& l, const Value& r) {
            if (l.isString() && r.isString()) return l.stringVal() >= r.stringVal();
            return l.toDouble() >= r.toDouble();
        }, ip, op);

    // D6 fix: OP_AND/OP_OR 为死操作码（编译器从不生成），删除显式 case，
    // 落入下方 default 返回错误。保留枚举值仅因指令长度表按位置索引。

    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 变量操作类指令
// ============================================================

VMResult VM::executeVarOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case OpCode::OP_DEFINE_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size() || !chunk.constants[idx].isString())
            return runtimeError("常量池索引越界或类型错误");
        const std::string& name = chunk.constants[idx].stringVal();
        Value val = pop();
        auto gsIt = globalNameToSlot_.find(name);
        if (gsIt != globalNameToSlot_.end()) {
            globalSlots_[gsIt->second] = std::move(val);
        } else {
            globals_[name] = std::move(val);
            // P0-8 fix: 插入新元素可能触发 unordered_map rehash，使已缓存的
            // &it->second 指针失效。清除 globalCache_ 避免悬垂指针访问
            // PERF-14 fix: unordered_map clear()
            globalCache_.clear();
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_GET_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size() || !chunk.constants[idx].isString())
            return runtimeError("常量池索引越界或类型错误");
        const std::string& name = chunk.constants[idx].stringVal();

        // A2: slot-based global fast path
        auto gsIt = globalNameToSlot_.find(name);
        if (gsIt != globalNameToSlot_.end()) {
            push(globalSlots_[gsIt->second]);
            notifyStep(ip, op);
            ip += 3;
            break;
        }

        // P2 fix: 内联缓存快速路径（仅 fallback globals_）
        // PERF-14 fix: unordered_map find O(1) 替代数组线性扫描
        // MEM-05 fix: generation = (bucket_count() << 16) ^ size()，erase 后 size 变化触发失效
        const std::string* namePtr = &name;
        size_t curGen = (globals_.bucket_count() << 16) ^ globals_.size();
        Value* cachedVal = nullptr;
        auto ccIt = globalCache_.find(namePtr);
        if (ccIt != globalCache_.end() && ccIt->second.generation == curGen) {
            cachedVal = ccIt->second.valuePtr;
        }

        if (cachedVal) {
            push(*cachedVal);
        } else {
            auto it = globals_.find(name);
            if (it != globals_.end()) {
                // 写入缓存
                globalCache_[namePtr] = { &it->second, curGen };
                push(it->second);
            } else {
                return runtimeError("未定义的变量: " + name);
            }
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_SET_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size() || !chunk.constants[idx].isString())
            return runtimeError("常量池索引越界或类型错误");
        const std::string& name = chunk.constants[idx].stringVal();
        Value val = pop();

        // A2: slot-based global fast path
        auto gsIt = globalNameToSlot_.find(name);
        if (gsIt != globalNameToSlot_.end()) {
            globalSlots_[gsIt->second] = std::move(val);
            notifyStep(ip, op);
            ip += 3;
            break;
        }

        // P2 fix: 内联缓存快速路径（仅 fallback globals_）
        // PERF-14 fix: unordered_map find O(1) 替代数组线性扫描
        // MEM-05 fix: generation = (bucket_count() << 16) ^ size()
        const std::string* namePtr = &name;
        size_t curGen = (globals_.bucket_count() << 16) ^ globals_.size();
        Value* cachedVal = nullptr;
        auto ccIt = globalCache_.find(namePtr);
        if (ccIt != globalCache_.end() && ccIt->second.generation == curGen) {
            cachedVal = ccIt->second.valuePtr;
        }

        if (cachedVal) {
            *cachedVal = std::move(val);
        } else {
            auto it = globals_.find(name);
            if (it == globals_.end()) {
                return runtimeError("未定义的变量: " + name);
            }
            // 写入缓存
            globalCache_[namePtr] = { &it->second, curGen };
            it->second = std::move(val);
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_DELETE_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size() || !chunk.constants[idx].isString())
            return runtimeError("常量池索引越界或类型错误");
        const std::string& name = chunk.constants[idx].stringVal();
        auto gsIt = globalNameToSlot_.find(name);
        if (gsIt != globalNameToSlot_.end()) {
            globalSlots_[gsIt->second] = Value::nullValue();
        } else {
            globals_.erase(name);
            // Bug fix: erase 可能不改变 bucket_count()，但会使已缓存指针悬垂
            // 清除所有缓存条目以确保安全
            // PERF-14 fix: unordered_map clear()
            // MEM-05 fix: 即使不清空，generation 检测也会因 size() 变化而失效
            globalCache_.clear();
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    // ---- A2: 全局变量整数槽位指令 ----

    case OpCode::OP_GET_GLOBAL: {
        uint16_t slot = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (slot >= globalSlots_.size()) return runtimeError("全局变量槽越界");
        push(globalSlots_[slot]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_SET_GLOBAL: {
        uint16_t slot = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (slot >= globalSlots_.size()) return runtimeError("全局变量槽越界");
        Value val = pop();
        globalSlots_[slot] = std::move(val);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_DEFINE_GLOBAL: {
        uint16_t slot = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (slot >= globalSlots_.size()) return runtimeError("全局变量槽越界");
        Value val = pop();
        globalSlots_[slot] = std::move(val);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_DELETE_GLOBAL: {
        uint16_t slot = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        // AUDIT-BUG-V3 fix: 越界时报错而非静默无操作，与其他 OP_*_GLOBAL 一致
        if (slot >= globalSlots_.size()) return runtimeError("全局变量槽越界");
        globalSlots_[slot] = Value::nullValue();
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_GET_LOCAL: {
        uint8_t slot = chunk.code[ip + 1];
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            return runtimeError(ErrorFormat::format("内部错误: 局部变量槽越界 (slot %d)", slot));
        }
        push(stack_[bp + slot]);
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_SET_LOCAL: {
        uint8_t slot = chunk.code[ip + 1];
        size_t bp = currentFrame().basePointer;
        const Value& val = peek(0);
        if (bp + slot >= stack_.size()) {
            return runtimeError(ErrorFormat::format("内部错误: 局部变量槽越界 (slot %d)", slot));
        }
        stack_[bp + slot] = val;
        // P0-7 fix: 若 slot 在字段范围内（1..fieldOrder.size()），标记字段已修改，
        // 确保 OP_RETURN 时字段同步回实例，避免方法内字段赋值丢失
        if (slot > 0 && currentFrame().chunk &&
            slot <= currentFrame().chunk->fieldOrder.size()) {
            currentFrame().fieldsModified = true;
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    // VM-05/06: upvalue 读写操作码
    case OpCode::OP_GET_UPVALUE: {
        uint8_t uvIdx = chunk.code[ip + 1];
        if (static_cast<size_t>(uvIdx) >= frame.upvalues.size()) {
            return runtimeError(ErrorFormat::format("内部错误: upvalue 索引越界 (%d)", static_cast<int>(uvIdx)));
        }
        auto& uv = frame.upvalues[uvIdx];
        if (uv->isClosed) {
            push(uv->value);
        } else {
            if (uv->stackSlot < stack_.size()) {
                push(stack_[uv->stackSlot]);
            } else {
                return runtimeError("内部错误: upvalue 栈槽越界");
            }
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_SET_UPVALUE: {
        uint8_t uvIdx = chunk.code[ip + 1];
        if (static_cast<size_t>(uvIdx) >= frame.upvalues.size()) {
            return runtimeError(ErrorFormat::format("内部错误: upvalue 索引越界 (%d)", static_cast<int>(uvIdx)));
        }
        auto& uv = frame.upvalues[uvIdx];
        const Value& val = peek(0); // peek 不消费（与 OP_SET_LOCAL 一致）
        if (uv->isClosed) {
            uv->value = val;
        } else {
            if (uv->stackSlot < stack_.size()) {
                stack_[uv->stackSlot] = val;
                // V-P1-6 fix: 若修改的是某外层帧的字段槽，标记该帧 fieldsModified，
                // 确保 OP_RETURN 时字段同步回实例（闭包内修改捕获的字段）
                // #18 fix: 用 owningFrameIdx O(1) 定位所属帧，替代 O(frames_) 线性扫描。
                // upvalue 为 open 状态时所属帧必在 frames_ 中（帧返回前会关闭其 upvalue）。
                if (uv->owningFrameIdx < frames_.size()) {
                    VMCallFrame& of = frames_[uv->owningFrameIdx];
                    if (of.chunk && !of.chunk->fieldOrder.empty()) {
                        size_t fieldStart = of.basePointer + 1;
                        size_t fieldEnd = fieldStart + of.chunk->fieldOrder.size();
                        if (uv->stackSlot >= fieldStart && uv->stackSlot < fieldEnd) {
                            of.fieldsModified = true;
                        }
                    }
                }
            } else {
                return runtimeError("内部错误: upvalue 栈槽越界");
            }
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_CLOSE_UPVALUE: {
        // B1 fix: 关闭所有指向 slot >= basePointer+slotBase 的 open upvalues，
        // 使块作用域退出时闭包捕获退出时刻的值快照（对齐 Interpreter per-block env）。
        uint8_t slotBase = chunk.code[ip + 1];
        closeUpvaluesFrom(frame.basePointer + slotBase);
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    default:
        return runtimeError(ErrorFormat::format("未知操作码: %d", static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

