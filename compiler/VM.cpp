#include "compiler/VM.h"
#include "interpreter/BuiltinMethods.h"  // 共享纯函数层（len/contains/has）
#include "interpreter/NumericUtils.h"    // 共享溢出检查（B6 fix）
#include "Logger.h"
#include <sstream>
#include <climits>
#include <cstdint>
#include <cmath>  // BUG 8.1 fix: std::fmod
#include <algorithm>

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
        Logger::Error("VM 栈下溢: peek(distance=" + std::to_string(distance) +
                      ") 但栈大小=" + std::to_string(stack_.size()), "VM");
        hasError_ = true;  // P0 fix: 设置错误标志，防止调用方在 null 哨兵上继续执行
        return nullSentinel;
    }
    return stack_[stack_.size() - 1 - distance];
}

VMResult VM::runtimeError(const std::string& msg) {
    lastError_ = msg;
    lastErrorLine_ = getCurrentLine();
    hasError_ = true;
    diagnostics_.addError(msg, lastErrorLine_, 0, DiagSource::VM);
    Logger::Error(msg + " (行 " + std::to_string(lastErrorLine_) + ")", "VM");
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
    auto it = openUpvalues_.begin();
    while (it != openUpvalues_.end()) {
        auto& uv = *it;
        if (!uv->isClosed && uv->stackSlot >= fromSlot) {
            if (uv->stackSlot < stack_.size()) {
                uv->value = stack_[uv->stackSlot];
            }
            uv->isClosed = true;
            it = openUpvalues_.erase(it);
        } else {
            ++it;
        }
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
    return stack_;
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
    return frames_.back();  // 调用方应确保 frames_ 非空（execute/stepOnce 中已检查）
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

    std::string cur = className;
    std::string methodKey;
    methodKey.reserve(cur.size() + 1 + methodName.size());
    for (int guard = 0; guard < MAX_INHERITANCE_DEPTH && !cur.empty(); ++guard) {
        methodKey.clear();
        methodKey.append(cur).append(1, '.').append(methodName);
        auto it = functionChunks_.find(methodKey);
        if (it != functionChunks_.end()) {
            // P4: 写入缓存
            if (clsIt != classInfo_.end()) {
                clsIt->second.methodCache[methodName] = &it->second;
            }
            return &it->second;
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

    // 数值运算
    switch (opType) {
    case OP_ADD_INT:
        if (leftRef.isInt() && rightRef.isInt()) {
            int64_t a = leftRef.intVal(), b = rightRef.intVal();
            if (OverflowCheck::addOverflow(a, b))
                return runtimeError("整数加法溢出");
            stack_[stack_.size() - 2] = Value(a + b);
        }
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() + rightRef.toDouble());
        break;
    case OP_SUB_INT:
        if (leftRef.isInt() && rightRef.isInt()) {
            int64_t a = leftRef.intVal(), b = rightRef.intVal();
            if (OverflowCheck::subOverflow(a, b))
                return runtimeError("整数减法溢出");
            stack_[stack_.size() - 2] = Value(a - b);
        }
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() - rightRef.toDouble());
        break;
    case OP_MUL_INT:
        if (leftRef.isInt() && rightRef.isInt()) {
            int64_t a = leftRef.intVal(), b = rightRef.intVal();
            if (OverflowCheck::mulOverflow(a, b))
                return runtimeError("整数乘法溢出");
            stack_[stack_.size() - 2] = Value(a * b);
        }
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() * rightRef.toDouble());
        break;
    case OP_DIV_INT:
        if (rightRef.isInt() && leftRef.isInt()) {
            int64_t b = rightRef.intVal();
            if (b == 0) return runtimeError("除零错误");
            if (OverflowCheck::divOverflow(leftRef.intVal(), b)) return runtimeError("整数除法溢出");
            stack_[stack_.size() - 2] = Value(leftRef.intVal() / b);  // int/int → int
            break;
        }
        if (rightRef.toDouble() == 0.0) return runtimeError("除零错误");
        stack_[stack_.size() - 2] = Value(leftRef.toDouble() / rightRef.toDouble());
        break;
    case OP_MOD_INT:
        // BUG 8.1 fix: 浮点操作数应使用 fmod，而非截断为整数后取模
        {
            // 两个 int → 整数取模
            if (leftRef.isInt() && rightRef.isInt()) {
                int64_t a = leftRef.intVal();
                int64_t b = rightRef.intVal();
                if (b == 0) return runtimeError("除零错误");
                if (OverflowCheck::modOverflow(a, b)) return runtimeError("整数取模溢出");
                stack_[stack_.size() - 2] = Value(a % b);
                break;
            }
            // 至少一个 float → fmod 浮点取模
            if (!leftRef.isNumber() || !rightRef.isNumber())
                return runtimeError("取模运算需要数值类型");
            double a = leftRef.toDouble();
            double b = rightRef.toDouble();
            if (b == 0.0) return runtimeError("除零错误");
            stack_[stack_.size() - 2] = Value(std::fmod(a, b));
            break;
        }
    }

    stack_.pop_back();  // 弹出 right，保留结果在 left 原位
    return VMResult::VM_OK;
}

// ---- C10: 内建方法名枚举分发 ----
VM::BuiltinMethod VM::classifyBuiltinMethod(const std::string& name) {
    // 按长度快速筛选，减少不必要的字符串比较
    switch (name.size()) {
    case 3:
        if (name == "pop") return BuiltinMethod::ARR_POP;
        if (name == "len") return BuiltinMethod::ARR_LEN; // 数组/字典/字符串共用
        if (name == "has") return BuiltinMethod::DICT_HAS;
        if (name == "get") return BuiltinMethod::DICT_GET;
        break;
    case 4:
        if (name == "push") return BuiltinMethod::ARR_PUSH;
        if (name == "keys") return BuiltinMethod::DICT_KEYS;
        if (name == "trim") return BuiltinMethod::STR_TRIM;
        if (name == "join") return BuiltinMethod::ARR_JOIN;
        break;
    case 5:
        if (name == "upper") return BuiltinMethod::STR_UPPER;
        if (name == "lower") return BuiltinMethod::STR_LOWER;
        if (name == "split") return BuiltinMethod::STR_SPLIT;
        break;
    case 6:
        if (name == "values") return BuiltinMethod::DICT_VALUES;
        if (name == "remove") return BuiltinMethod::ARR_REMOVE; // 数组/字典共用
        if (name == "substr") return BuiltinMethod::STR_SUBSTR;
        break;
    case 7:
        if (name == "replace") return BuiltinMethod::STR_REPLACE;
        if (name == "indexOf") return BuiltinMethod::STR_INDEX_OF;
        break;
    case 8:
        if (name == "contains") return BuiltinMethod::ARR_CONTAINS; // 数组/字典共用
        if (name == "endsWith") return BuiltinMethod::STR_ENDS_WITH;
        break;
    case 9:
        break;
    case 10:
        if (name == "startsWith") return BuiltinMethod::STR_STARTS_WITH;
        break;
    }
    return BuiltinMethod::UNKNOWN;
}

// ---- C11: 比较运算辅助函数 ----
VMResult VM::pushCompareResult(bool result, size_t& ip, OpCode opcode) {
    stack_[stack_.size() - 2] = Value(result);
    stack_.pop_back();
    notifyStep(ip, opcode);
    ip += 1;
    return VMResult::VM_OK;
}

// 写回变异方法调用后的接收者（统一数组/字典/实例三处写回逻辑）
// 判断链：
//   1. receiverVarIdx != 0xFFFF → 写入 globalSlots_ 或 globals_
//   2. receiverLocalSlotByte != 0xFF → 写入栈帧（含字段同步）
//   3. 否则 → 存入 lastMutatedReceiver_
VMResult VM::writeBackReceiver(uint16_t receiverVarIdx, uint8_t receiverLocalSlotByte,
                               Value& mutatedObj, bool fieldsModified) {
    const BytecodeChunk& chunk = currentChunk();
    if (receiverVarIdx != 0xFFFF && receiverVarIdx < chunk.constants.size()) {
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
        auto sr = executeSharedLen(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        pop(); push(std::move(sr.value())); notifyStep(ip, op); ip += instrLen;
        return VMResult::VM_OK;
    }
    if (method == BuiltinMethod::ARR_CONTAINS) {
        auto sr = executeSharedArrayContains(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        pop(); push(std::move(sr.value())); notifyStep(ip, op); ip += instrLen;
        return VMResult::VM_OK;
    }
    if (method == BuiltinMethod::ARR_JOIN) {
        auto sr = executeSharedArrayJoin(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
        pop(); push(std::move(result)); notifyStep(ip, op); ip += instrLen;
        return VMResult::VM_OK;
    }

    // A2 变异路径：先 pop 接收者(move, refcount 不变)，再用 tryGetMutable 原地修改
    Value mutableObj = std::move(stack_.back());
    stack_.pop_back();

    if (method == BuiltinMethod::ARR_PUSH) {
        if (args.size() != 1) return runtimeError("push 期望 1 个参数");
        auto* arr = mutableObj.tryGetMutableArray();
        if (arr) arr->push_back(std::move(args[0]));
        // V-P2-19 fix: 缓存引用 + std::move，避免重复 arrayVal() 调用和不必要拷贝
        else mutableObj.arrayVal().push_back(std::move(args[0]));
    } else if (method == BuiltinMethod::ARR_POP) {
        auto* arr = mutableObj.tryGetMutableArray();
        if (arr) {
            if (arr->empty()) return runtimeError("对空数组调用 pop");
            result = std::move(arr->back());
            arr->pop_back();
        } else {
            // V-P2-19 fix: 缓存引用，避免 3 次独立 arrayVal() 调用 + std::move result
            auto& arrRef = mutableObj.arrayVal();
            if (arrRef.empty()) return runtimeError("对空数组调用 pop");
            result = std::move(arrRef.back());
            arrRef.pop_back();
        }
    } else if (method == BuiltinMethod::ARR_REMOVE) {
        if (args.size() != 1) return runtimeError("remove 期望 1 个参数(索引)");
        if (!args[0].isInt()) return runtimeError("remove 参数必须是整数索引");
        int64_t ri = args[0].intVal();
        auto* arr = mutableObj.tryGetMutableArray();
        if (arr) {
            if (ri < 0 || static_cast<size_t>(ri) >= arr->size())
                return runtimeError("数组索引越界: " + std::to_string(ri));
            arr->erase(arr->begin() + static_cast<size_t>(ri));
        } else {
            // V-P2-19 fix: 缓存引用，避免 2 次独立 arrayVal() 调用
            auto& arrRef = mutableObj.arrayVal();
            if (ri < 0 || static_cast<size_t>(ri) >= arrRef.size())
                return runtimeError("数组索引越界: " + std::to_string(ri));
            arrRef.erase(arrRef.begin() + static_cast<size_t>(ri));
        }
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
        auto sr = executeSharedLen(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        pop(); push(std::move(sr.value())); notifyStep(ip, op); ip += instrLen;
        return VMResult::VM_OK;
    }
    if (method == BuiltinMethod::DICT_KEYS) {
        auto sr = executeSharedDictKeys(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        pop(); push(std::move(sr.value())); notifyStep(ip, op); ip += instrLen;
        return VMResult::VM_OK;
    }
    if (method == BuiltinMethod::DICT_VALUES) {
        auto sr = executeSharedDictValues(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        pop(); push(std::move(sr.value())); notifyStep(ip, op); ip += instrLen;
        return VMResult::VM_OK;
    }
    if (method == BuiltinMethod::DICT_HAS || method == BuiltinMethod::ARR_CONTAINS) {
        auto sr = executeSharedDictHas(obj, methodName, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        pop(); push(std::move(sr.value())); notifyStep(ip, op); ip += instrLen;
        return VMResult::VM_OK;
    }
    if (method == BuiltinMethod::DICT_GET) {
        auto sr = executeSharedDictGet(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        pop(); push(std::move(sr.value())); notifyStep(ip, op); ip += instrLen;
        return VMResult::VM_OK;
    }

    // A2 变异路径（remove）：先 pop 再原地修改
    Value mutableObj = std::move(stack_.back());
    stack_.pop_back();

    if (method == BuiltinMethod::DICT_REMOVE || method == BuiltinMethod::ARR_REMOVE) {
        if (args.size() != 1) return runtimeError("remove 期望 1 个参数(键)");
        auto* dict = mutableObj.tryGetMutableDict();
        if (dict) dict->erase(args[0].toString());
        else mutableObj.dictVal().erase(args[0].toString());
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
        auto sr = executeSharedLen(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_UPPER) {
        // S3 fix: 改用共享纯函数实现，消除 Interpreter/VM 双重实现
        auto sr = executeSharedStrUpper(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_LOWER) {
        // S3 fix: 改用共享纯函数实现，消除 Interpreter/VM 双重实现
        auto sr = executeSharedStrLower(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_SPLIT) {
        // S3 fix: 改用共享纯函数实现，消除 Interpreter/VM 双重实现
        auto sr = executeSharedStrSplit(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_TRIM) {
        // S3 fix: 改用共享纯函数实现，消除 Interpreter/VM 双重实现
        auto sr = executeSharedStrTrim(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_CONTAINS || method == BuiltinMethod::ARR_CONTAINS) {
        if (args.size() != 1) return runtimeError("contains 期望 1 个参数");
        result = Value(obj.stringVal().find(args[0].toString()) != std::string::npos);
    } else if (method == BuiltinMethod::STR_STARTS_WITH) {
        auto sr = executeSharedStrStartsWith(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_ENDS_WITH) {
        auto sr = executeSharedStrEndsWith(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_REPLACE) {
        // S1 fix: 改用共享纯函数实现，消除 O(N²) Bug
        auto sr = executeSharedStrReplace(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_SUBSTR) {
        auto sr = executeSharedStrSubstr(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
    } else if (method == BuiltinMethod::STR_INDEX_OF) {
        auto sr = executeSharedStrIndexOf(obj, args.begin(), args.size());
        if (sr.is_err()) return runtimeError(sr.error().message);
        result = std::move(sr.value());
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
    // S1 fix: 预分配到 MAX_STACK_SIZE，消除运行中 realloc（栈操作提升 20-30%）
    stack_.reserve(MAX_STACK_SIZE);
    globals_.clear();
    lastError_.clear();
    lastErrorLine_ = 0;
    hasError_ = false;
    diagnostics_.clear();
    frames_.clear();
    frames_.reserve(64);  // 预分配调用帧空间，避免频繁 realloc
    functionChunks_ = result.functionChunks;
    // P3: 清除函数调用缓存（functionChunks_ 地址已变）
    for (int ci = 0; ci < CALL_CACHE_SIZE; ++ci) callCache_[ci] = {};
    callCacheNextSlot_ = 0;
    // P2: 清除全局变量缓存
    for (int ci = 0; ci < GLOBAL_CACHE_SIZE; ++ci) globalCache_[ci] = {};
    globalCacheNextSlot_ = 0;
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
    // A2: 初始化全局变量槽位
    globalSlots_.clear();
    globalSlotNames_.clear();
    globalNameToSlot_.clear();
    globalSlots_.resize(result.globalSlotCount);
    globalSlotNames_ = result.globalSlotNames;
    for (int i = 0; i < result.globalSlotCount; ++i) {
        globalNameToSlot_[result.globalSlotNames[i]] = i;
    }
    mainChunk_ = result.mainChunk;  // 持有主 chunk 副本，避免悬空指针

    // 设置主帧
    VMCallFrame mainFrame;
    mainFrame.chunk = &mainChunk_;  // 指向 VM 自持的副本
    mainFrame.ip = 0;
    mainFrame.basePointer = 0;
    mainFrame.functionName = "main";
    frames_.push_back(std::move(mainFrame));

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
    globalSlots_.clear();
    globalSlotNames_.clear();
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
    for (int ci = 0; ci < CALL_CACHE_SIZE; ++ci) callCache_[ci] = {};
    callCacheNextSlot_ = 0;
    for (int ci = 0; ci < GLOBAL_CACHE_SIZE; ++ci) globalCache_[ci] = {};
    globalCacheNextSlot_ = 0;
    initialized_ = false;
}

VMResult VM::stepOnce() {
    // 帧已空 → 执行完毕
    if (frames_.empty()) return VMResult::VM_OK;

    // 已有错误 → 不再执行
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;

    // P1 fix: stepOnce 累计指令预算检查，防止通过循环调用 stepOnce 绕过 DoS 防护
    if (++stepInstructionCount_ > MAX_INSTRUCTIONS) {
        lastError_ = "指令执行数超过上限 " + std::to_string(MAX_INSTRUCTIONS) + "，疑似无限循环";
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
            if (savedBp <= stack_.size()) {
                if (wasInit) {
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
                if (savedBp <= stack_.size()) {
                    if (wasInit) {
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
            lastError_ = "指令执行数超过上限 " + std::to_string(MAX_INSTRUCTIONS) + "，疑似无限循环";
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
    case OpCode::OP_AND:
    case OpCode::OP_OR:
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
        return executeMiscOps(op, ip);

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
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
        push(chunk.constants[idx]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_NULL:
        push(Value::nullValue());
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_TRUE:
        push(Value(true));
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_FALSE:
        push(Value(false));
        notifyStep(ip, op);
        ip += 1;
        break;

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
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
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
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

    case OpCode::OP_AND:
    case OpCode::OP_OR:
        // 编译器不生成这些操作码（使用 JUMP_IF_FALSE 短路实现）
        // 保留 case 防止未知操作码错误，但执行到此说明字节码损坏
        return runtimeError("内部错误: 编译器不应生成 OP_AND/OP_OR");

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
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
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& name = chunk.constants[idx].stringVal();
        Value val = pop();
        auto gsIt = globalNameToSlot_.find(name);
        if (gsIt != globalNameToSlot_.end()) {
            globalSlots_[gsIt->second] = std::move(val);
        } else {
            globals_[name] = std::move(val);
            // P0-8 fix: 插入新元素可能触发 unordered_map rehash，使已缓存的
            // &it->second 指针失效。清除 globalCache_ 避免悬垂指针访问
            for (int ci = 0; ci < GLOBAL_CACHE_SIZE; ++ci) globalCache_[ci] = {};
            globalCacheNextSlot_ = 0;
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_GET_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
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
        const std::string* namePtr = &name;
        size_t curGen = globals_.bucket_count();
        Value* cachedVal = nullptr;
        for (int ci = 0; ci < GLOBAL_CACHE_SIZE; ++ci) {
            if (globalCache_[ci].namePtr == namePtr && globalCache_[ci].generation == curGen) {
                cachedVal = globalCache_[ci].valuePtr;
                break;
            }
        }

        if (cachedVal) {
            push(*cachedVal);
        } else {
            auto it = globals_.find(name);
            if (it != globals_.end()) {
                // 写入缓存
                globalCache_[globalCacheNextSlot_].namePtr = namePtr;
                globalCache_[globalCacheNextSlot_].valuePtr = &it->second;
                globalCache_[globalCacheNextSlot_].generation = curGen;
                globalCacheNextSlot_ = (globalCacheNextSlot_ + 1) % GLOBAL_CACHE_SIZE;
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
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
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
        const std::string* namePtr = &name;
        size_t curGen = globals_.bucket_count();
        Value* cachedVal = nullptr;
        for (int ci = 0; ci < GLOBAL_CACHE_SIZE; ++ci) {
            if (globalCache_[ci].namePtr == namePtr && globalCache_[ci].generation == curGen) {
                cachedVal = globalCache_[ci].valuePtr;
                break;
            }
        }

        if (cachedVal) {
            *cachedVal = std::move(val);
        } else {
            auto it = globals_.find(name);
            if (it == globals_.end()) {
                return runtimeError("未定义的变量: " + name);
            }
            // 写入缓存
            globalCache_[globalCacheNextSlot_].namePtr = namePtr;
            globalCache_[globalCacheNextSlot_].valuePtr = &it->second;
            globalCache_[globalCacheNextSlot_].generation = curGen;
            globalCacheNextSlot_ = (globalCacheNextSlot_ + 1) % GLOBAL_CACHE_SIZE;
            it->second = std::move(val);
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_DELETE_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& name = chunk.constants[idx].stringVal();
        auto gsIt = globalNameToSlot_.find(name);
        if (gsIt != globalNameToSlot_.end()) {
            globalSlots_[gsIt->second] = Value::nullValue();
        } else {
            globals_.erase(name);
            // Bug fix: erase 可能不改变 bucket_count()，但会使已缓存指针悬垂
            // 清除所有缓存条目以确保安全
            for (int ci = 0; ci < GLOBAL_CACHE_SIZE; ++ci) globalCache_[ci] = {};
            globalCacheNextSlot_ = 0;
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
        if (slot < globalSlots_.size()) {
            globalSlots_[slot] = Value::nullValue();
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_GET_LOCAL: {
        uint8_t slot = chunk.code[ip + 1];
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            return runtimeError("内部错误: 局部变量槽越界 (slot " + std::to_string(slot) + ")");
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
            return runtimeError("内部错误: 局部变量槽越界 (slot " + std::to_string(slot) + ")");
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
            return runtimeError("内部错误: upvalue 索引越界 (" + std::to_string(uvIdx) + ")");
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
            return runtimeError("内部错误: upvalue 索引越界 (" + std::to_string(uvIdx) + ")");
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
                for (size_t fi = 0; fi < frames_.size(); ++fi) {
                    VMCallFrame& of = frames_[fi];
                    if (of.chunk && !of.chunk->fieldOrder.empty()) {
                        size_t fieldStart = of.basePointer + 1;
                        size_t fieldEnd = fieldStart + of.chunk->fieldOrder.size();
                        if (uv->stackSlot >= fieldStart && uv->stackSlot < fieldEnd) {
                            of.fieldsModified = true;
                            break;
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
        uint8_t uvIdx = chunk.code[ip + 1];
        if (static_cast<size_t>(uvIdx) < frame.upvalues.size()) {
            auto& uv = frame.upvalues[uvIdx];
            if (!uv->isClosed && uv->stackSlot < stack_.size()) {
                uv->value = stack_[uv->stackSlot];
                uv->isClosed = true;
                // L-新1 fix: 从 openUpvalues_ 移除已关闭的 upvalue
                openUpvalues_.erase(
                    std::remove(openUpvalues_.begin(), openUpvalues_.end(), uv),
                    openUpvalues_.end());
            }
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 调用相关类指令
// ============================================================

VMResult VM::executeCallOps(OpCode op, size_t& ip) {
    // S1 fix: 拆分为 8 个独立方法，降低圈复杂度（原 978 行 → 调度器 + 各方法 < 200 行）
    switch (op) {
    case OpCode::OP_RETURN:     return executeReturn(ip);
    case OpCode::OP_CALL:       return executeCall(ip, false);
    case OpCode::OP_CALL_EXPR:  return executeCall(ip, true);
    case OpCode::OP_SUPER_CALL:
    case OpCode::OP_METHOD_CALL: return executeMethodCall(ip, op);
    case OpCode::OP_CLOSURE:    return executeClosure(ip, op);
    case OpCode::OP_CLASS_NEW:  return executeClassNew(ip, op);
    case OpCode::OP_DEFINE_CLASS: return executeDefineClass(ip, op);
    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
    }
}

// ============================================================
// S1 fix: executeCallOps 拆分实现
// ============================================================

VMResult VM::executeReturn(size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    (void)chunk;  // OP_RETURN 不直接使用 chunk

    Value result = pop();
    // 提取标量字段 + move 字符串，避免拷贝整个 VMCallFrame（含 2 个 std::string）
    const size_t savedBp = frames_.back().basePointer;
    const size_t savedReturnIp = frames_.back().returnIp;
    const bool wasMethodCall = frames_.back().isMethodCall;
    const bool wasInitCall = frames_.back().isInitCall;
    const int recvLocalSlot = frames_.back().receiverLocalSlot;
    const BytecodeChunk* retChunk = frames_.back().chunk;
    const bool fieldsModified = frames_.back().fieldsModified;  // VM fix: 脏标记
    std::string recvVarName = std::move(frames_.back().receiverVarName);
    // 在 pop_back 之前保存 ip 值，避免悬空引用
    size_t savedIp = ip;
    // P0-3 fix: 记录当前帧索引，pop 后清理该帧的 tryStack_ handler
    size_t returningFrameIdx = frames_.size() - 1;
    frames_.pop_back();

    // P0-3 fix: 清理属于返回帧的 tryStack_ handler（return 跳出 try 块时 handler 会残留）
    while (!tryStack_.empty() && tryStack_.back().frameIndex >= returningFrameIdx) {
        tryStack_.pop_back();
    }

    // 方法调用字段同步：将方法内修改的字段槽（bp+1..N）同步回 this（bp）
    // VM fix: 仅在 init 调用或字段被修改时执行同步，跳过只读方法
    if (wasMethodCall && savedBp < stack_.size() && (wasInitCall || fieldsModified)) {
        Value& modifiedThis = stack_[savedBp];

        // 先把方法内的字段槽（bp+1..N）同步回 this
        if (modifiedThis.isInstance() && retChunk && !retChunk->fieldOrder.empty()) {
            for (size_t fi = 0; fi < retChunk->fieldOrder.size(); ++fi) {
                size_t pos = savedBp + 1 + fi;
                if (pos < stack_.size()) {
                    modifiedThis.fields()[retChunk->fieldOrder[fi]] = stack_[pos];
                }
            }
        }

        // V-P2-18 fix: 后续均为只读访问，绑定 const 引用避免 COW 深拷贝
        const Value& modifiedThisC = modifiedThis;

        // 写回到接收者的原始位置
        if (!frames_.empty()) {
            VMCallFrame& callerFrame = currentFrame();
            size_t callerBp = callerFrame.basePointer;

            // 路径 A：接收者是全局变量 → 写回 globals_ 或 globalSlots_
            if (!recvVarName.empty() && modifiedThisC.isInstance()) {
                auto gsIt = globalNameToSlot_.find(recvVarName);
                if (gsIt != globalNameToSlot_.end() && globalSlots_[gsIt->second].isInstance()) {
                    for (const auto& field : modifiedThisC.fields()) {
                        globalSlots_[gsIt->second].fields()[field.first] = field.second;
                    }
                } else {
                    auto it = globals_.find(recvVarName);
                    if (it != globals_.end() && it->second.isInstance()) {
                        for (const auto& field : modifiedThisC.fields()) {
                            it->second.fields()[field.first] = field.second;
                        }
                    }
                }
            }

            // 路径 B：接收者是调用者帧中的局部变量 → 写回栈帧
            if (recvLocalSlot >= 0 &&
                callerBp + recvLocalSlot < stack_.size()) {
                size_t receiverPos = callerBp + recvLocalSlot;

                if (recvLocalSlot == 0) {
                    // 接收者是调用者的 this（slot 0）：同步所有字段和字段槽
                    if (stack_[callerBp].isInstance()) {
                        for (const auto& field : modifiedThisC.fields()) {
                            stack_[callerBp].fields()[field.first] = field.second;
                        }
                        // 同步字段到调用者的字段槽（bp+1..N）
                        if (callerFrame.chunk && !callerFrame.chunk->fieldOrder.empty()) {
                            for (size_t fi = 0; fi < callerFrame.chunk->fieldOrder.size(); ++fi) {
                                const std::string& fieldName = callerFrame.chunk->fieldOrder[fi];
                                auto fieldIt = modifiedThisC.fields().find(fieldName);
                                if (fieldIt != modifiedThisC.fields().end()) {
                                    size_t slotPos = callerBp + 1 + fi;
                                    if (slotPos < stack_.size()) {
                                        stack_[slotPos] = fieldIt->second;
                                    }
                                }
                            }
                        }
                    }
                } else {
                    // 接收者是调用者帧中的特定局部变量（字段、参数或局部变量）
                    stack_[receiverPos] = modifiedThis;

                    // 如果接收者是调用者 this 的字段（slot 1..N），也更新 this.fields()
                    if (stack_[callerBp].isInstance() && callerFrame.chunk &&
                        recvLocalSlot <= static_cast<int>(callerFrame.chunk->fieldOrder.size())) {
                        const std::string& fieldName = callerFrame.chunk->fieldOrder[recvLocalSlot - 1];
                        stack_[callerBp].fields()[fieldName] = modifiedThis;
                    }
                }
            }
        }
    }

    // init 方法返回 this 实例而非 null（在字段同步之后读取）
    if (wasInitCall) {
        if (savedBp < stack_.size()) {
            result = stack_[savedBp];
        }
    }

    // VM-05/06: 关闭当前帧关联的 open upvalues（在 stack resize 之前）
    // P1-5 fix: 使用统一辅助函数，避免代码重复
    if (retChunk || !frames_.empty()) {
        closeUpvaluesFrom(savedBp);
    }

    if (frames_.empty()) {
        // V-P2-21 fix: result 后续不再使用，std::move 入栈
        push(std::move(result));
        notifyStep(savedIp, OpCode::OP_RETURN);
        return VMResult::VM_OK;
    }
    // 恢复栈：清理当前帧的局部变量和参数
    stack_.resize(savedBp);
    // V-P2-21 fix: result 后续不再使用，std::move 入栈
    push(std::move(result));
    // 恢复 ip
    currentFrame().ip = savedReturnIp;
    notifyStep(savedIp, OpCode::OP_RETURN);
    return VMResult::VM_OK;
}

VMResult VM::executeCall(size_t& ip, bool isExpr) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    OpCode op = isExpr ? OpCode::OP_CALL_EXPR : OpCode::OP_CALL;

    if (!isExpr) {
        // ---- OP_CALL ----
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& funName = chunk.constants[idx].stringVal();

        // P3 fix: 内联缓存快速路径（按指针比较，避免 hash 查找）
        const BytecodeChunk* cachedChunk = nullptr;
        const std::string* namePtr = &funName;
        for (int ci = 0; ci < CALL_CACHE_SIZE; ++ci) {
            if (callCache_[ci].namePtr == namePtr) {
                cachedChunk = callCache_[ci].chunkPtr;
                break;
            }
        }

        auto it = cachedChunk
            ? functionChunks_.end()  // 缓存命中，跳过 hash 查找
            : functionChunks_.find(funName);

        if (!cachedChunk && it == functionChunks_.end()) {
            auto classIt = classInfo_.find(funName);
            if (classIt != classInfo_.end()) {
                VMClassInfo& cls = classIt->second;

                // 收集参数
                if (stack_.size() < static_cast<size_t>(argCount)) return runtimeError("栈下溢: OP_CALL ctor");
                SmallArgs<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; --i) {
                    args[i] = pop();
                }

                // 创建新实例
                Value instance = Value::makeInstance(cls.name);
                instance.fields() = cls.fieldDefaults;  // 已含继承字段（OP_DEFINE_CLASS 合并）

                // 检查是否有 init 方法（沿继承链查找）
                const BytecodeChunk* initChunkPtr = findMethodChunk(funName, "init");

                if (initChunkPtr != nullptr) {
                    const BytecodeChunk& initChunk = *initChunkPtr;
                    // P0-1 fix: 使用范围检查支持默认参数，并填充缺失的默认值
                    std::vector<Value> defaults;
                    if (!fillDefaultArgs(initChunk, argCount, funName, defaults)) {
                        return runtimeError("构造函数 init 期望 " +
                            std::to_string(initChunk.requiredArity) + "-" +
                            std::to_string(initChunk.arity) + " 个参数，但传入了 " +
                            std::to_string(argCount) + " 个");
                    }
                    // 将默认参数追加到 args 末尾
                    for (auto& d : defaults) {
                        args.push_back(std::move(d));
                    }

                    if (frames_.size() >= MAX_FRAMES) {
                        return runtimeError("调用栈溢出");
                    }

                    // 推入 this
                    push(instance);
                    // 按方法 chunk 声明的字段顺序（含继承字段）推入字段值
                    int fieldCount = 0;
                    if (initChunk.fieldOrder.empty()) {
                        for (const auto& field : instance.fields()) {
                            push(field.second);
                        }
                        fieldCount = static_cast<int>(instance.fields().size());
                    } else {
                        for (const auto& fieldName : initChunk.fieldOrder) {
                            auto fieldIt = instance.fields().find(fieldName);
                            if (fieldIt != instance.fields().end()) {
                                push(fieldIt->second);
                            } else {
                                push(Value::nullValue());
                            }
                        }
                        fieldCount = static_cast<int>(initChunk.fieldOrder.size());
                    }
                    // 推入参数
                    for (const auto& arg : args) {
                        push(arg);
                    }

                    // 预分配局部变量栈空间：方法体内 var 声明的局部变量需要栈槽
                    int preAllocated = 1 + fieldCount + argCount;  // this + 字段 + 参数
                    int extraSlots = initChunk.localCount - preAllocated;
                    // V-P2-1 fix: extraSlots 为负表示帧布局损坏（fieldCount 与编译期不一致）
                    if (extraSlots < 0) {
                        return runtimeError("类 " + funName + " 的 init 方法帧布局损坏: localCount=" +
                            std::to_string(initChunk.localCount) + " < preAllocated=" +
                            std::to_string(preAllocated));
                    }
                    for (int i = 0; i < extraSlots; ++i) {
                        push(Value::nullValue());
                    }

                    VMCallFrame newFrame;
                    newFrame.chunk = initChunkPtr;
                    newFrame.returnIp = ip + 4;
                    newFrame.basePointer = stack_.size() - initChunk.localCount;
                    newFrame.functionName = initChunkPtr->name;
                    newFrame.ip = 0;
                    newFrame.isMethodCall = true;  // 使 OP_RETURN 同步字段到 this
                    newFrame.isInitCall = true;     // init 返回 this 而非 null
                    size_t savedIp = ip;
                    frames_.push_back(std::move(newFrame));

                    notifyStep(savedIp, op);
                    return VMResult::VM_OK;
                }

                // 无 init 方法：检查是否有多余参数（与解释器行为保持一致）
                if (argCount > 0) {
                    return runtimeError("类 " + cls.name + " 没有 init 方法，但传入了 " +
                                        std::to_string(argCount) + " 个参数");
                }
                push(instance);
                notifyStep(ip, op);
                ip += 4;
                return VMResult::VM_OK;
            }

            // 既不是函数也不是类：检查是否为 input() 函数
            if (funName == "input") {
                if (argCount > 1) {
                    for (uint8_t i = 0; i < argCount; ++i) pop();
                    return runtimeError("input 期望 0 或 1 个参数，但传入了 " + std::to_string(argCount) + " 个");
                }
                std::string prompt;
                if (argCount == 1) {
                    if (stack_.empty()) return runtimeError("栈下溢: OP_CALL input");
                    Value promptVal = pop();
                    prompt = promptVal.toString();
                }
                std::string userInput;
                if (inputCallback_) {
                    userInput = inputCallback_(prompt);
                }
                push(Value(std::move(userInput)));
                notifyStep(ip, op);
                ip += 4;
                return VMResult::VM_OK;
            }

            // 既不是函数也不是类：检查是否为顶层内置函数
            if (isBuiltinFunction(funName)) {
                // 收集参数（栈上顺序: [arg0, arg1, ..., argN-1]，栈顶是最后一个参数）
                if (stack_.size() < static_cast<size_t>(argCount)) {
                    return runtimeError("栈下溢: OP_CALL builtin");
                }
                SmallArgs<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; --i) {
                    args[i] = pop();
                }

                int line = 0;
                if (!chunk.lines.empty() && ip < chunk.lines.size()) {
                    line = chunk.lines[ip];
                }

                auto r = executeSharedBuiltinFunction(
                    funName, args.begin(), argCount, line, 0);

                if (r.is_err()) {
                    return runtimeError(r.error().message);
                }
                push(std::move(r.value()));
                notifyStep(ip, op);
                ip += 4;
                return VMResult::VM_OK;
            }

            for (uint8_t i = 0; i < argCount; ++i) {
                pop();
            }
            return runtimeError("未定义的函数: " + funName);
        }

        // P3: 缓存未命中时写入缓存（round-robin 替换）
        if (!cachedChunk) {
            callCache_[callCacheNextSlot_].namePtr = namePtr;
            callCache_[callCacheNextSlot_].chunkPtr = &it->second;
            callCacheNextSlot_ = (callCacheNextSlot_ + 1) % CALL_CACHE_SIZE;
        }

        const BytecodeChunk& targetChunk = cachedChunk ? *cachedChunk : it->second;
        // F10: 支持默认参数，参数数量可在 [requiredArity, arity] 范围内
        if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
            argCount > static_cast<uint8_t>(targetChunk.arity)) {
            return runtimeError("函数 " + funName + " 期望 " +
                std::to_string(targetChunk.requiredArity) + "-" +
                std::to_string(targetChunk.arity) + " 个参数，但传入了 " +
                std::to_string(argCount) + " 个");
        }

        // F10: 为缺失的尾部参数填充默认值
        if (argCount < static_cast<uint8_t>(targetChunk.arity)) {
            int missingCount = targetChunk.arity - argCount;
            int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
            if (defaultStartIdx < 0 || 
                static_cast<size_t>(defaultStartIdx + missingCount) > targetChunk.defaultConstIndices.size()) {
                return runtimeError("函数 " + funName + " 默认参数索引越界");
            }
            for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                uint16_t constIdx = targetChunk.defaultConstIndices[i];
                if (constIdx == 0xFFFF) {
                    return runtimeError("函数 " + funName + " 的默认参数包含非字面量表达式，VM 不支持");
                }
                if (constIdx >= targetChunk.constants.size()) {
                    return runtimeError("函数 " + funName + " 默认参数常量索引越界");
                }
                push(targetChunk.constants[constIdx]);
            }
            argCount = static_cast<uint8_t>(targetChunk.arity);
        }

        if (frames_.size() >= MAX_FRAMES) {
            return runtimeError("调用栈溢出");
        }

        // 预分配局部变量栈空间：函数体内 var 声明的局部变量需要栈槽，
        // 但帧创建时栈上只有参数，需补推 null 填充额外槽位
        int extraSlots = targetChunk.localCount - argCount;
        // V-P2-1 fix: extraSlots 为负表示帧布局损坏
        if (extraSlots < 0) {
            return runtimeError("闭包调用帧布局损坏: localCount=" +
                std::to_string(targetChunk.localCount) + " < argCount=" +
                std::to_string(argCount));
        }
        for (int i = 0; i < extraSlots; ++i) {
            push(Value::nullValue());
        }

        VMCallFrame newFrame;
        newFrame.chunk = &targetChunk;
        newFrame.returnIp = ip + 4;
        newFrame.basePointer = stack_.size() - targetChunk.localCount;
        newFrame.functionName = funName;
        newFrame.ip = 0;
        // VM-05/06: 从闭包注册表附加 upvalues（如果该函数有闭包绑定）
        auto closureIt = functionClosures_.find(funName);
        if (closureIt != functionClosures_.end() && closureIt->second.vmClosure()) {
            newFrame.upvalues = closureIt->second.vmClosure()->upvalues;
        }
        size_t savedIp = ip;
        frames_.push_back(std::move(newFrame));

        notifyStep(savedIp, op);
        return VMResult::VM_OK;
    }

    // ---- OP_CALL_EXPR ----
    {
        uint8_t argCount = chunk.code[ip + 1];
        // VM-05/06: 从栈上获取闭包值和参数，执行调用
        if (stack_.size() < static_cast<size_t>(argCount) + 1) {
            return runtimeError("栈下溢: OP_CALL_EXPR");
        }

        // R3-1 fix: 只弹出闭包值（在栈顶），参数保留在栈上供新帧使用
        // 栈布局: [..., arg0, arg1, ..., argN-1, closure]
        // 弹出 closure 后: [..., arg0, arg1, ..., argN-1] — 参数就位
        Value callee = pop();

        if (!callee.isClosure()) {
            // R3-1 fix: 弹出参数以保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("表达式调用需要函数值");
        }

        // M3 fix: 优先使用闭包值中的 chunkPtr（直接指针，无哈希查找）
        // 回退到名称查找兼容旧闭包（chunkPtr 为 null 时）
        const BytecodeChunk* targetChunkPtr = nullptr;
        if (callee.vmClosure() && callee.vmClosure()->chunkPtr) {
            targetChunkPtr = callee.vmClosure()->chunkPtr;
        } else {
            auto chunkIt = functionChunks_.find(callee.closureName());
            if (chunkIt != functionChunks_.end()) {
                targetChunkPtr = &chunkIt->second;
            }
        }
        if (!targetChunkPtr) {
            // R3-1 fix: 弹出参数保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("未找到函数: " + callee.closureName());
        }

        const BytecodeChunk& targetChunk = *targetChunkPtr;
        // F10: 支持默认参数
        if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
            argCount > static_cast<uint8_t>(targetChunk.arity)) {
            // R3-1 fix: 弹出参数保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("函数 " + callee.closureName() + " 期望 " +
                std::to_string(targetChunk.requiredArity) + "-" +
                std::to_string(targetChunk.arity) + " 个参数，但传入了 " +
                std::to_string(argCount) + " 个");
        }

        // F10: 为缺失的尾部参数填充默认值
        if (argCount < static_cast<uint8_t>(targetChunk.arity)) {
            int missingCount = targetChunk.arity - argCount;
            int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
            if (defaultStartIdx < 0 ||
                static_cast<size_t>(defaultStartIdx + missingCount) > targetChunk.defaultConstIndices.size()) {
                for (int i = 0; i < argCount; ++i) pop();
                return runtimeError("函数 " + callee.closureName() + " 默认参数索引越界");
            }
            for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                uint16_t constIdx = targetChunk.defaultConstIndices[i];
                if (constIdx == 0xFFFF) {
                    for (int j = 0; j < argCount; ++j) pop();
                    return runtimeError("函数 " + callee.closureName() + " 的默认参数包含非字面量表达式，VM 不支持");
                }
                if (constIdx >= targetChunk.constants.size()) {
                    for (int j = 0; j < argCount; ++j) pop();
                    return runtimeError("函数 " + callee.closureName() + " 默认参数常量索引越界");
                }
                push(targetChunk.constants[constIdx]);
            }
            argCount = static_cast<uint8_t>(targetChunk.arity);
        }

        if (frames_.size() >= MAX_FRAMES) {
            // R3-1 fix: 弹出参数保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("调用栈溢出");
        }

        // 预分配局部变量栈空间
        int extraSlots = targetChunk.localCount - argCount;
        // V-P2-1 fix: extraSlots 为负表示帧布局损坏
        if (extraSlots < 0) {
            return runtimeError("函数调用帧布局损坏: localCount=" +
                std::to_string(targetChunk.localCount) + " < argCount=" +
                std::to_string(argCount));
        }
        for (int i = 0; i < extraSlots; ++i) {
            push(Value::nullValue());
        }

        VMCallFrame newFrame;
        newFrame.chunk = &targetChunk;
        newFrame.returnIp = ip + 2; // OP_CALL_EXPR 是 2 字节指令
        newFrame.basePointer = stack_.size() - targetChunk.localCount;
        newFrame.functionName = callee.closureName();
        newFrame.ip = 0;

        // VM-05/06: 绑定闭包 upvalues 到新帧
        if (callee.vmClosure()) {
            newFrame.upvalues = callee.vmClosure()->upvalues;
        }

        size_t savedIp = ip;
        frames_.push_back(std::move(newFrame));
        notifyStep(savedIp, op);
        return VMResult::VM_OK;
    }
}

VMResult VM::executeMethodCall(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    bool isSuperCall = (op == OpCode::OP_SUPER_CALL);
    const int instrLen = isSuperCall ? 9 : 7;  // B1 fix: SUPER_CALL 多了 2 字节 classIdx
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        uint16_t receiverVarIdx = chunk.code[ip + 4] | (chunk.code[ip + 5] << 8);
        uint8_t receiverLocalSlotByte = chunk.code[ip + 6];
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& methodName = chunk.constants[idx].stringVal();

        // C8: 用 const 引用访问接收者，避免非变异方法的深拷贝
        const Value& obj = peek(argCount);
        // V-P2-25 fix: peek 栈下溢时已设置 hasError_ 并返回 null 哨兵，立即退出避免后续误报
        if (hasError_) return VMResult::VM_RUNTIME_ERROR;
        // C10: 方法名一次性分类为枚举
        BuiltinMethod method = classifyBuiltinMethod(methodName);

        // ---- 数组内置方法 ----
        if (obj.isArray()) {
            // B7 fix: 提取到 dispatchArrayBuiltin，降低 executeCallOps 圈复杂度
            VMResult r = dispatchArrayBuiltin(obj, method, methodName, argCount,
                                               receiverVarIdx, receiverLocalSlotByte,
                                               ip, op, instrLen);
            if (r != VMResult::VM_OK) return r;
            return VMResult::VM_OK;
        }

        // ---- 字典内置方法 ----
        if (obj.isDict()) {
            VMResult r = dispatchDictBuiltin(obj, method, methodName, argCount,
                                              receiverVarIdx, receiverLocalSlotByte,
                                              ip, op, instrLen);
            if (r != VMResult::VM_OK) return r;
            return VMResult::VM_OK;
        }

        // ---- 字符串内置方法（全部非变异，使用 const 引用）----
        if (obj.isString()) {
            VMResult r = dispatchStringBuiltin(obj, method, methodName, argCount,
                                                ip, op, instrLen);
            if (r != VMResult::VM_OK) return r;
            return VMResult::VM_OK;
        }

        // ---- 类实例方法调用 ----
        if (obj.isInstance()) {
            // 拷贝接收者，因为后续 pop() 会使 peek 引用失效
            // V-P2-12 fix: 使用 const 避免后续 fields()/className() 触发 COW 深拷贝
            const Value objCopy = obj;
            // B1 fix: super 调用使用编译时编码的类名（而非运行时实例类名）
            // 避免 3+ 级继承时 super 查找回到子类导致死循环
            std::string searchClassName = objCopy.className();
            if (isSuperCall) {
                uint16_t classIdx = chunk.code[ip + 7] | (chunk.code[ip + 8] << 8);
                // V-P1-7 fix: classIdx 越界应报错而非静默降级到运行时类名（可能导致错误的 super 查找）
                if (classIdx >= chunk.constants.size()) {
                    return runtimeError("内部错误: super 调用的类名常量索引越界 (" + std::to_string(classIdx) + ")");
                }
                searchClassName = chunk.constants[classIdx].stringVal();
                auto clsIt = classInfo_.find(searchClassName);
                if (clsIt == classInfo_.end() || clsIt->second.superClassName.empty()) {
                    return runtimeError("类 " + searchClassName + " 没有父类，不能使用 super");
                }
                searchClassName = clsIt->second.superClassName;
            }
            // 沿继承链查找方法（父类方法也可调用）
            const BytecodeChunk* targetChunkPtr = findMethodChunk(searchClassName, methodName);
            if (targetChunkPtr != nullptr) {
                const BytecodeChunk& targetChunk = *targetChunkPtr;

                // F10: 支持默认参数
                if (argCount < static_cast<uint8_t>(targetChunk.requiredArity) ||
                    argCount > static_cast<uint8_t>(targetChunk.arity)) {
                    for (uint8_t i = 0; i < argCount; ++i) pop();
                    pop();
                    return runtimeError("方法 " + methodName + " 期望 " +
                        std::to_string(targetChunk.requiredArity) + "-" +
                        std::to_string(targetChunk.arity) + " 个参数，但传入了 " +
                        std::to_string(argCount) + " 个");
                }

                if (frames_.size() >= MAX_FRAMES) {
                    return runtimeError("调用栈溢出");
                }

                // 收集参数（反向填充，省去 reverse）
                if (stack_.size() < static_cast<size_t>(argCount) + 1) return runtimeError("栈下溢: OP_METHOD_CALL");
                SmallArgs<Value> args(argCount);
                for (int i = argCount - 1; i >= 0; --i) {
                    args[i] = pop();
                }
                pop();  // 移除栈上的原始实例

                // 推入 this（拷贝，方法内修改会被 writeBack 写回）
                push(objCopy);
                // 按方法 chunk 声明的字段顺序推入实例字段值
                int fieldCount = 0;
                if (targetChunk.fieldOrder.empty()) {
                    // 回退：按 unordered_map 顺序（不保证正确，但兼容旧字节码）
                    for (const auto& field : objCopy.fields()) {
                        push(field.second);
                    }
                    fieldCount = static_cast<int>(objCopy.fields().size());
                } else {
                    for (const auto& fieldName : targetChunk.fieldOrder) {
                        auto fieldIt = objCopy.fields().find(fieldName);
                        if (fieldIt != objCopy.fields().end()) {
                            push(fieldIt->second);
                        } else {
                            push(Value::nullValue());
                        }
                    }
                    fieldCount = static_cast<int>(targetChunk.fieldOrder.size());
                }
                // 推入参数
                for (const auto& arg : args) {
                    push(arg);
                }

                // F10: 为缺失的尾部参数填充默认值
                if (argCount < static_cast<uint8_t>(targetChunk.arity)) {
                    int missingCount = targetChunk.arity - argCount;
                    int defaultStartIdx = static_cast<int>(targetChunk.defaultConstIndices.size()) - missingCount;
                    if (defaultStartIdx < 0 ||
                        static_cast<size_t>(defaultStartIdx + missingCount) > targetChunk.defaultConstIndices.size()) {
                        return runtimeError("方法 " + methodName + " 默认参数索引越界");
                    }
                    for (int i = defaultStartIdx; i < defaultStartIdx + missingCount; ++i) {
                        uint16_t constIdx = targetChunk.defaultConstIndices[i];
                        if (constIdx == 0xFFFF) {
                            return runtimeError("方法 " + methodName + " 的默认参数包含非字面量表达式，VM 不支持");
                        }
                        if (constIdx >= targetChunk.constants.size()) {
                            return runtimeError("方法 " + methodName + " 默认参数常量索引越界");
                        }
                        push(targetChunk.constants[constIdx]);
                    }
                    argCount = static_cast<uint8_t>(targetChunk.arity);
                }

                // 预分配局部变量栈空间：方法体内 var 声明的局部变量需要栈槽
                int preAllocated = 1 + fieldCount + argCount;  // this + 字段 + 参数
                int extraSlots = targetChunk.localCount - preAllocated;
                // V-P2-1 fix: extraSlots 为负表示帧布局损坏
                if (extraSlots < 0) {
                    return runtimeError("方法 " + methodName + " 帧布局损坏: localCount=" +
                        std::to_string(targetChunk.localCount) + " < preAllocated=" +
                        std::to_string(preAllocated));
                }
                for (int i = 0; i < extraSlots; ++i) {
                    push(Value::nullValue());
                }

                VMCallFrame newFrame;
                newFrame.chunk = targetChunkPtr;
                newFrame.returnIp = ip + instrLen;  // B1 fix: SUPER_CALL 是 9 字节
                newFrame.basePointer = stack_.size() - targetChunk.localCount;
                newFrame.functionName = targetChunk.name;
                newFrame.ip = 0;
                newFrame.isMethodCall = true;
                newFrame.isInitCall = (methodName == "init");  // init 返回 this 而非 null
                // 记录接收者变量名（用于 writeBack 到 globals_）
                if (receiverVarIdx != 0xFFFF && receiverVarIdx < chunk.constants.size()) {
                    newFrame.receiverVarName = chunk.constants[receiverVarIdx].stringVal();
                }
                // 记录接收者局部变量 slot（用于 writeBack 到调用者栈帧）
                newFrame.receiverLocalSlot = (receiverLocalSlotByte == 0xFF) ? -1 : receiverLocalSlotByte;
                size_t savedIp = ip;
                frames_.push_back(std::move(newFrame));

                notifyStep(savedIp, op);
                return VMResult::VM_OK;
            }
        }

        // 方法未找到或对象非实例
        // 先保存类型信息，因为 pop 会使 obj 引用失效
        bool wasInstance = obj.isInstance();
        std::string clsName = wasInstance ? obj.className() : "";
        for (uint8_t i = 0; i < argCount; ++i) pop();
        pop();
        if (wasInstance) {
            return runtimeError("类 " + clsName + " 没有方法 " + methodName);
        } else {
            return runtimeError("方法调用需要类实例");
        }
}

VMResult VM::executeClosure(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t upvalueCount = chunk.code[ip + 3]; // VM-05/06: 改为 upvalue 数量
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& funName = chunk.constants[idx].stringVal();

        // 创建闭包值（参数名由 OP_CALL 按 arity 绑定，此处不填充假名）
        Value closure = Value::makeClosure(funName, nullptr, {});

        // VM-05/06: 创建 VM 闭包数据并绑定 upvalue
        auto vmClosureData = std::make_shared<VMClosureData>();
        vmClosureData->functionName = funName;
        vmClosureData->upvalues.resize(upvalueCount);
        // M3 fix: 闭包值直接持有函数 chunk 指针（OP_CALL_EXPR 使用，避免名称查找）
        auto chunkIt = functionChunks_.find(funName);
        if (chunkIt != functionChunks_.end()) {
            vmClosureData->chunkPtr = &chunkIt->second;
        }

        size_t instrBase = ip + 4; // OP_CLOSURE(1) + nameIdx(2) + upvalueCount(1)

        for (uint8_t i = 0; i < upvalueCount; ++i) {
            uint8_t isLocal = chunk.code[instrBase + i * 2];
            uint8_t uvIndex = chunk.code[instrBase + i * 2 + 1];

            if (isLocal) {
                // 直接捕获：创建新 upvalue 指向调用者帧的栈槽
                auto uv = std::make_shared<VMUpvalue>();
                uv->stackSlot = frame.basePointer + uvIndex;
                uv->isClosed = false;
                vmClosureData->upvalues[i] = uv;
                openUpvalues_.push_back(uv);
            } else {
                // 透传：复用调用者帧的 upvalue
                if (static_cast<size_t>(uvIndex) < frame.upvalues.size()) {
                    vmClosureData->upvalues[i] = frame.upvalues[uvIndex];
                } else {
                    // 降级：创建空 upvalue
                    vmClosureData->upvalues[i] = std::make_shared<VMUpvalue>();
                    vmClosureData->upvalues[i]->value = Value::nullValue();
                    vmClosureData->upvalues[i]->isClosed = true;
                }
            }
        }
        closure.vmClosure() = vmClosureData;

        // 注册到函数闭包表（OP_CALL 按名称查找时使用）
        functionClosures_[funName] = closure;

        // V-P2-22 fix: closure 后续不再使用，std::move 入栈（map 已持有拷贝）
        push(std::move(closure));
        notifyStep(ip, op);
        ip = instrBase + upvalueCount * 2; // 跳过 upvalue 描述符
        return VMResult::VM_OK;
}

VMResult VM::executeClassNew(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    pendingFieldOrder_.clear();  // M3: 开始新的类定义，清空字段顺序记录
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint8_t argCount = chunk.code[ip + 3];
    if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
    const std::string& className = chunk.constants[idx].stringVal();

    // 收集参数（反向填充，省去 reverse）
    if (stack_.size() < argCount) return runtimeError("栈下溢: OP_CLASS_NEW");
    SmallArgs<Value> args(argCount);
    for (int i = argCount - 1; i >= 0; --i) {
        args[i] = pop();
    }

    // 查找类信息
    auto classIt = classInfo_.find(className);
    if (classIt == classInfo_.end()) {
        // 类定义阶段：类尚未注册（OP_DEFINE_CLASS 会随后注册）。
        // 创建空模板实例供 OP_INIT_FIELD 填充字段，不支持带参数构造。
        if (argCount > 0) {
            return runtimeError("类 " + className + " 尚未定义，不能带参数构造");
        }
        Value instance = Value::makeInstance(className);
        push(std::move(instance));
        notifyStep(ip, op);
        ip += 4;
        return VMResult::VM_OK;
    }
        VMClassInfo& cls = classIt->second;

        // 创建新实例
        Value instance = Value::makeInstance(cls.name);
        instance.fields() = cls.fieldDefaults;  // 已含继承字段

        // 检查是否有 init 方法（沿继承链查找）
        const BytecodeChunk* initChunkPtr = findMethodChunk(className, "init");

        // S2 fix: 也处理 argCount==0 且 init.arity==0 的自动构造场景
        // （与解释器 visitVarDecl 一致：Point p; 自动调用 0 参数 init）
        // P1-1 fix: 使用 requiredArity==0 判断自动构造，支持全默认参数 init
        bool shouldCallInit = false;
        if (initChunkPtr != nullptr) {
            if (argCount > 0) {
                shouldCallInit = true;
            } else if (initChunkPtr->requiredArity == 0) {
                shouldCallInit = true;  // 0 必需参数（含全默认参数 init），自动构造时调用
            }
        }

        if (shouldCallInit) {
            // 创建 init 帧执行初始化
            const BytecodeChunk& initChunk = *initChunkPtr;
            // P0-1 fix: 使用范围检查支持默认参数，并填充缺失的默认值
            std::vector<Value> defaults;
            if (!fillDefaultArgs(initChunk, argCount, className, defaults)) {
                return runtimeError("构造函数 init 期望 " +
                    std::to_string(initChunk.requiredArity) + "-" +
                    std::to_string(initChunk.arity) + " 个参数，但传入了 " +
                    std::to_string(argCount) + " 个");
            }
            // 将默认参数追加到 args 末尾
            for (auto& d : defaults) {
                args.push_back(std::move(d));
            }

            if (frames_.size() >= MAX_FRAMES) {
                return runtimeError("调用栈溢出");
            }

            // 推入 this
            push(instance);
            // 按方法 chunk 声明的字段顺序（含继承字段）推入字段值
            int fieldCount = 0;
            if (initChunk.fieldOrder.empty()) {
                for (const auto& field : instance.fields()) {
                    push(field.second);
                }
                fieldCount = static_cast<int>(instance.fields().size());
            } else {
                for (const auto& fieldName : initChunk.fieldOrder) {
                    auto fieldIt = instance.fields().find(fieldName);
                    if (fieldIt != instance.fields().end()) {
                        push(fieldIt->second);
                    } else {
                        push(Value::nullValue());
                    }
                }
                fieldCount = static_cast<int>(initChunk.fieldOrder.size());
            }
            // 推入参数
            // V-P2-24 fix: args 后续不再使用，std::move 入栈
            for (auto& arg : args) {
                push(std::move(arg));
            }

            // 预分配局部变量栈空间：方法体内 var 声明的局部变量需要栈槽
            int preAllocated = 1 + fieldCount + argCount;  // this + 字段 + 参数
            int extraSlots = initChunk.localCount - preAllocated;
            // V-P2-1 fix: extraSlots 为负表示帧布局损坏
            if (extraSlots < 0) {
                return runtimeError("类 " + className + " 的 init 方法帧布局损坏: localCount=" +
                    std::to_string(initChunk.localCount) + " < preAllocated=" +
                    std::to_string(preAllocated));
            }
            for (int i = 0; i < extraSlots; ++i) {
                push(Value::nullValue());
            }

            VMCallFrame newFrame;
            newFrame.chunk = initChunkPtr;
            newFrame.returnIp = ip + 4;
            newFrame.basePointer = stack_.size() - initChunk.localCount;
            newFrame.functionName = initChunkPtr->name;
            newFrame.ip = 0;
            newFrame.isMethodCall = true;  // 使 OP_RETURN 同步字段到 this
            newFrame.isInitCall = true;     // init 返回 this 而非 null
            size_t savedIp = ip;
            frames_.push_back(std::move(newFrame));

            notifyStep(savedIp, op);
            return VMResult::VM_OK;
        }

        // 无 init 或 init.arity 不匹配 argCount：推入实例（OP_INIT_FIELD 或手动 init 后续处理）
        // V-P1-8 fix: init 存在但参数不匹配（argCount==0 且 init.requiredArity>0）时报错，而非静默跳过
        if (initChunkPtr != nullptr && !shouldCallInit) {
            return runtimeError("类 " + cls.name + " 的 init 期望至少 " +
                std::to_string(initChunkPtr->requiredArity) + " 个参数，但传入了 " +
                std::to_string(argCount) + " 个");
        }
        push(instance);

        // 无 init 但有参数：报错（与解释器一致）
        if (initChunkPtr == nullptr && argCount > 0) {
            return runtimeError("类 " + cls.name + " 没有 init 方法，但传入了 " +
                         std::to_string(argCount) + " 个参数");
        }

        notifyStep(ip, op);
        ip += 4;
        return VMResult::VM_OK;
}

VMResult VM::executeDefineClass(size_t& ip, OpCode op) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    // 操作数: nameIdx(2B) + superNameIdx(2B)
    // superNameIdx == 0xFFFF 表示无父类
    uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
    uint16_t superIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
    if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
    const std::string& className = chunk.constants[idx].stringVal();
    std::string superClassName;
    if (superIdx != 0xFFFF && superIdx < chunk.constants.size()) {
        superClassName = chunk.constants[superIdx].stringVal();
    }
    // V-P2-17 fix: templateInstance 只读不写，改为 const 避免 fields() 触发 COW 深拷贝
    const Value templateInstance = pop();

    // 从模板实例提取当前类字段（M3 fix: 使用 pendingFieldOrder_ 保持声明顺序）
    std::vector<std::string> ownFieldOrder = std::move(pendingFieldOrder_);
    std::unordered_map<std::string, Value> ownFieldDefaults;
    if (templateInstance.isInstance()) {
        for (const auto& fieldName : ownFieldOrder) {
            auto it = templateInstance.fields().find(fieldName);
            if (it != templateInstance.fields().end()) {
                ownFieldDefaults[fieldName] = it->second;
            }
        }
    } else {
        return runtimeError("OP_DEFINE_CLASS: 模板值不是实例");
    }

    // 注册类信息（先注册以支持循环引用安全查找）
    VMClassInfo info;
    info.name = className;
    info.superClassName = superClassName;
    classInfo_[className] = info;

    // 沿继承链合并父类字段（父类字段在前，子类覆盖同名字段）
    // 先按"父→子"顺序收集字段名，子类已存在的字段不重复添加
    std::vector<std::string> mergedOrder;
    std::unordered_map<std::string, Value> mergedDefaults;

    std::vector<std::string> chain;
    std::string cur = superClassName;
    for (int guard = 0; guard < MAX_INHERITANCE_DEPTH && !cur.empty(); ++guard) {
        auto clsIt = classInfo_.find(cur);
        if (clsIt == classInfo_.end()) {
            return runtimeError("未定义的父类: " + cur);
        }
        chain.push_back(cur);
        cur = clsIt->second.superClassName;
    }
    // V-P2 fix: 循环继承检测 — guard 耗尽但 cur 仍非空说明存在继承环
    if (!cur.empty()) {
        return runtimeError("类继承链过深或存在循环继承: " + className + " -> " + cur);
    }
    // 倒序遍历链（最远的祖先在前），保证子类字段覆盖父类字段
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        auto clsIt = classInfo_.find(*it);
        if (clsIt == classInfo_.end()) continue;
        for (const auto& fieldName : clsIt->second.fieldOrder) {
            if (mergedDefaults.find(fieldName) == mergedDefaults.end()) {
                mergedOrder.push_back(fieldName);
                mergedDefaults[fieldName] = clsIt->second.fieldDefaults[fieldName];
            }
        }
    }
    // 当前类字段最后处理（覆盖父类同名字段）
    for (const auto& fieldName : ownFieldOrder) {
        if (mergedDefaults.find(fieldName) == mergedDefaults.end()) {
            mergedOrder.push_back(fieldName);
        }
        mergedDefaults[fieldName] = ownFieldDefaults[fieldName];
    }

    VMClassInfo& registered = classInfo_[className];
    registered.fieldOrder = std::move(mergedOrder);
    registered.fieldDefaults = std::move(mergedDefaults);

    // 在全局变量中存储类标记（与解释器语义一致：类名是类型标识，不是实例）
    Value classVal(std::string("class:") + className);
    auto classGsIt = globalNameToSlot_.find(className);
    // V-P2-23 fix: classVal 后续不再使用，std::move 写入（两分支互斥）
    if (classGsIt != globalNameToSlot_.end()) {
        globalSlots_[classGsIt->second] = std::move(classVal);
    } else {
        globals_[className] = std::move(classVal);
    }

    notifyStep(ip, op);
    ip += 5;  // nameIdx(2B) + superNameIdx(2B) + opcode(1B)
    return VMResult::VM_OK;
}

// ============================================================
// 容器与成员操作类指令
// ============================================================

VMResult VM::executeContainerOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case OpCode::OP_BUILD_ARRAY: {
        uint8_t count = chunk.code[ip + 1];
        if (stack_.size() < count) return runtimeError("栈下溢: OP_BUILD_ARRAY");
        std::vector<Value> elements(count);
        // 逆序弹出直接填入预分配槽位，无需 reverse
        for (int i = count - 1; i >= 0; --i) {
            elements[i] = pop();
        }
        push(Value(std::move(elements)));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_BUILD_DICT: {
        uint8_t pairCount = chunk.code[ip + 1];
        if (stack_.size() < static_cast<size_t>(pairCount) * 2) return runtimeError("栈下溢: OP_BUILD_DICT");
        std::unordered_map<std::string, Value> dict;
        dict.reserve(pairCount);
        // 先入后出：倒序弹出键值对
        for (uint8_t i = 0; i < pairCount; ++i) {
            Value val = pop();
            Value key = pop();
            // S5 fix: 字符串键直接用 stringVal() 引用，避免 toString() 中间临时对象
            if (key.isString()) {
                dict.emplace(key.stringVal(), std::move(val));
            } else {
                dict.emplace(key.toString(), std::move(val));
            }
        }
        push(Value(std::move(dict)));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_INDEX_GET: {
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_INDEX_GET");
        Value idx = pop();
        // V-P2-15 fix: obj 只读不写，改为 const 避免 arrayVal/dictVal/stringVal 触发 COW 深拷贝
        const Value obj = pop();
        if (obj.isArray() && idx.isInt()) {
            int64_t i = idx.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                push(obj.arrayVal()[static_cast<size_t>(i)]);
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i) + ", 有效范围 [0, " + std::to_string(obj.arrayVal().size()) + ")");
            }
        } else if (obj.isDict() && idx.isString()) {
            auto it = obj.dictVal().find(idx.stringVal());
            if (it != obj.dictVal().end()) {
                push(it->second);
            } else {
                push(Value::nullValue());  // 字典访问不存在的键返回 null（与解释器一致）
            }
        } else if (obj.isArray()) {
            return runtimeError("数组索引需要整数类型");
        } else if (obj.isString() && idx.isInt()) {
            // V3 fix: 基于 UTF-8 码位索引，与解释器 M6 fix 一致
            int64_t i = idx.intVal();
            const std::string& s = obj.stringVal();

            // P7 fix: ASCII 快速路径 — 纯 ASCII 字符串直接按字节索引 O(1)
            // S5 fix: 缓存 StringData* 指针，O(1) 指针比较替代 O(n) 字符串内容比较
            //         安全性：StringData 由 shared_ptr 持有，Value 在栈上时指针有效
            if (i >= 0 && static_cast<size_t>(i) < s.size()) {
                const void* strPtr = static_cast<const void*>(&s);
                bool isAscii;
                if (lastAsciiStrPtr_ == strPtr) {
                    isAscii = lastAsciiStrIsAscii_;
                } else {
                    isAscii = true;
                    for (size_t b = 0; b < s.size(); ++b) {
                        if (static_cast<unsigned char>(s[b]) >= 0x80) { isAscii = false; break; }
                    }
                    lastAsciiStrPtr_ = strPtr;
                    lastAsciiStrIsAscii_ = isAscii;
                }
                if (isAscii) {
                    push(Value(s.substr(static_cast<size_t>(i), 1)));
                    notifyStep(ip, op);
                    ip += 1;
                    break;  // 跳过慢路径，直接完成 OP_INDEX_GET
                }
            }

            size_t charCount = 0;
            size_t bytePos = 0;
            size_t targetBytePos = 0;
            size_t targetByteLen = 0;
            bool found = false;
            while (bytePos < s.size()) {
                unsigned char c = static_cast<unsigned char>(s[bytePos]);
                size_t charLen = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                                 ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
                if (static_cast<size_t>(i) == charCount) {
                    targetBytePos = bytePos;
                    targetByteLen = charLen;
                    found = true;
                }
                bytePos += charLen;
                charCount++;
            }
            if (i < 0 || !found) {
                return runtimeError("字符串索引越界: " + std::to_string(i)
                           + ", 有效范围 [0, " + std::to_string(charCount) + ")");
            }
            push(Value(s.substr(targetBytePos, targetByteLen)));
        } else if (obj.isString()) {
            return runtimeError("字符串索引需要整数类型");
        } else {
            return runtimeError("该类型不支持索引访问");
        }
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_INDEX_SET: {
        // M1 fix: 嵌套索引赋值（如 arr[i][j] = val）
        // 栈序: [..., obj, outerIdx, innerIdx, val]
        // 弹出 val, innerIdx, obj → 修改 obj[innerIdx] → 存入 lastMutatedReceiver_
        if (stack_.size() < 3) return runtimeError("栈下溢: OP_INDEX_SET");
        Value val = pop();
        Value innerIdx = pop();
        Value obj = pop();
        if (obj.isArray() && innerIdx.isInt()) {
            int64_t i = innerIdx.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i));
            }
        } else if (obj.isDict() && innerIdx.isString()) {
            obj.dictVal()[innerIdx.stringVal()] = val;
        } else if (obj.isArray()) {
            return runtimeError("数组索引需要整数类型");
        } else {
            return runtimeError("该类型不支持索引赋值");
        }
        lastMutatedReceiver_ = std::move(obj);
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_INDEX_SET_VAR: {
        // 直接修改全局变量中的数组/字典元素
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[idx].stringVal();
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_INDEX_SET_VAR");
        Value val = pop();
        Value index = pop();
        // A2: 先查 globalSlots_，再查 globals_
        Value* objPtr = nullptr;
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end()) {
            objPtr = &globalSlots_[gsIt->second];
        } else {
            auto it = globals_.find(varName);
            if (it == globals_.end()) {
                return runtimeError("未定义的变量: " + varName);
            }
            objPtr = &it->second;
        }
        Value& obj = *objPtr;  // 引用，直接修改
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i) +
                             ", 有效范围 [0, " + std::to_string(obj.arrayVal().size()) + ")");
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = val;
        } else if (obj.isArray()) {
            return runtimeError("数组索引需要整数类型");
        } else {
            return runtimeError("该类型不支持索引赋值");
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_INDEX_SET_LOCAL: {
        // 直接修改 stack_[bp+slot] 中的数组/字典元素（用于方法内 this.arr[i] = val）
        uint8_t slot = chunk.code[ip + 1];
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_INDEX_SET_LOCAL");
        Value val = pop();
        Value index = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            return runtimeError("内部错误: 局部变量槽越界");
        }
        Value& obj = stack_[bp + slot];  // 栈引用，直接修改
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = val;
                if (slot == 0) currentFrame().fieldsModified = true;  // VM fix
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i) +
                             ", 有效范围 [0, " + std::to_string(obj.arrayVal().size()) + ")");
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = val;
            if (slot == 0) currentFrame().fieldsModified = true;  // VM fix
        } else if (obj.isArray()) {
            return runtimeError("数组索引需要整数类型");
        } else {
            return runtimeError("该类型不支持索引赋值");
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_SUPER_MEMBER_GET:
    case OpCode::OP_MEMBER_GET: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[idx].stringVal();
        // V-P2-16 fix: obj 只读不写，改为 const 避免 fields/dictVal 触发 COW 深拷贝
        const Value obj = pop();
        if (obj.isInstance()) {
            auto it = obj.fields().find(fieldName);
            if (it != obj.fields().end()) {
                push(it->second);
            } else {
                // V-P2 fix: 字段未找到时查找方法（与 Interpreter visitMemberAccess 一致），
                // 返回 "method:ClassName.methodName" 标记字符串，而非直接报错。
                // 对于 super.field，字段在构造时已合并到实例，方法查找也从当前类开始即可
                // （super.method() 调用走 OP_SUPER_CALL 直接指定起始类，不经过此路径）
                const BytecodeChunk* methodChunk = findMethodChunk(obj.className(), fieldName);
                if (methodChunk != nullptr) {
                    push(Value(std::string("method:") + obj.className() + "." + fieldName));
                } else {
                    return runtimeError("类 " + obj.className() + " 没有字段或方法 '" + fieldName + "'");
                }
            }
        } else if (obj.isDict()) {
            auto it = obj.dictVal().find(fieldName);
            if (it != obj.dictVal().end()) {
                push(it->second);
            } else {
                push(Value::nullValue());
            }
        } else {
            return runtimeError("该类型不支持成员访问");
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_MEMBER_SET: {
        // M1 fix: 嵌套成员赋值（如 obj.field.subfield = val 或 arr[i].field = val）
        // 栈序: [..., obj, val]
        // 弹出 val, obj → 修改 obj.field → 存入 lastMutatedReceiver_
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_MEMBER_SET");
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[idx].stringVal();
        Value val = pop();
        Value obj = pop();
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = val;
        } else {
            return runtimeError("该类型不支持成员赋值");
        }
        lastMutatedReceiver_ = std::move(obj);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_MEMBER_SET_VAR: {
        // 直接修改全局变量.fields()[fieldName]
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[varIdx].stringVal();
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        Value val = pop();
        // A2: 先查 globalSlots_，再查 globals_
        Value* objPtr = nullptr;
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end()) {
            objPtr = &globalSlots_[gsIt->second];
        } else {
            auto it = globals_.find(varName);
            if (it == globals_.end()) {
                return runtimeError("未定义的变量: " + varName);
            }
            objPtr = &it->second;
        }
        Value& obj = *objPtr;  // 引用，直接修改
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = val;
        } else {
            return runtimeError("该类型不支持成员赋值");
        }
        notifyStep(ip, op);
        ip += 5;
        break;
    }

    case OpCode::OP_MEMBER_SET_LOCAL: {
        // 直接修改 stack_[bp+slot].fields()[fieldName]（用于方法内 this.field = val）
        uint8_t slot = chunk.code[ip + 1];
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        Value val = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            return runtimeError("内部错误: 局部变量槽越界");
        }
        Value& obj = stack_[bp + slot];  // 栈引用，直接修改
        if (obj.isInstance()) {
            obj.fields()[fieldName] = val;
            // VM fix: 标记字段已修改，OP_RETURN 可跳过只读方法的字段同步
            if (slot == 0) currentFrame().fieldsModified = true;
            // 当 slot==0（写 this.field）时，也需同步更新对应的字段槽
            // 否则 OP_RETURN 会用字段槽的旧值覆盖 this.fields()，导致 this.field 赋值丢失
            if (slot == 0 && currentFrame().chunk && !currentFrame().chunk->fieldOrder.empty()) {
                const auto& fieldOrder = currentFrame().chunk->fieldOrder;
                for (size_t fi = 0; fi < fieldOrder.size(); ++fi) {
                    if (fieldOrder[fi] == fieldName) {
                        size_t slotPos = bp + 1 + fi;
                        if (slotPos < stack_.size()) {
                            stack_[slotPos] = val;
                        }
                        break;
                    }
                }
            }
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = val;
        } else {
            return runtimeError("该类型不支持成员赋值");
        }
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 嵌套访问写回类指令
// ============================================================

VMResult VM::executeWritebackOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    // ---- 嵌套访问变异方法写回指令 ----
    // 从 lastMutatedReceiver_ 取值，写回基对象的字段或索引位置
    switch (op) {
    case OpCode::OP_WRITEBACK_MEMBER_VAR: {
        // 操作数: varIdx(2B) + fieldIdx(2B)
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[varIdx].stringVal();
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        // A2: 先查 globalSlots_，再查 globals_
        Value* objPtr = nullptr;
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end()) {
            objPtr = &globalSlots_[gsIt->second];
        } else {
            auto it = globals_.find(varName);
            if (it == globals_.end()) {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("未定义的变量: " + varName);
            }
            objPtr = &it->second;
        }
        Value& obj = *objPtr;
        if (obj.isInstance()) {
            obj.fields()[fieldName] = std::move(lastMutatedReceiver_);
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = std::move(lastMutatedReceiver_);
        } else {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("类型 " + obj.typeName() + " 不支持成员赋值");
        }
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 5;
        break;
    }

    case OpCode::OP_WRITEBACK_MEMBER_LOCAL: {
        // 操作数: slot(1B) + fieldIdx(2B)
        uint8_t slot = chunk.code[ip + 1];
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        if (fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("OP_WRITEBACK_MEMBER_LOCAL: 栈槽越界");
        }
        Value& obj = stack_[bp + slot];
        if (obj.isInstance()) {
            obj.fields()[fieldName] = std::move(lastMutatedReceiver_);
            // 如果 slot==0（this），也同步更新对应字段槽
            if (slot == 0) {
                VMCallFrame& curFrame = currentFrame();
                if (curFrame.chunk) {
                    for (size_t i = 0; i < curFrame.chunk->fieldOrder.size(); ++i) {
                        if (curFrame.chunk->fieldOrder[i] == fieldName) {
                            size_t fieldSlot = bp + 1 + i;
                            if (fieldSlot < stack_.size()) {
                                stack_[fieldSlot] = std::move(lastMutatedReceiver_);
                            }
                            break;
                        }
                    }
                }
            }
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = std::move(lastMutatedReceiver_);
        } else {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("类型 " + obj.typeName() + " 不支持成员赋值");
        }
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    case OpCode::OP_WRITEBACK_INDEX_VAR: {
        // 操作数: varIdx(2B)，索引从栈顶 pop
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        Value index = pop();
        if (varIdx >= chunk.constants.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("常量池索引越界");
        }
        const std::string& varName = chunk.constants[varIdx].stringVal();
        // A2: 先查 globalSlots_，再查 globals_
        Value* objPtr = nullptr;
        auto gsIt = globalNameToSlot_.find(varName);
        if (gsIt != globalNameToSlot_.end()) {
            objPtr = &globalSlots_[gsIt->second];
        } else {
            auto it = globals_.find(varName);
            if (it == globals_.end()) {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("未定义的变量: " + varName);
            }
            objPtr = &it->second;
        }
        Value& obj = *objPtr;
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = std::move(lastMutatedReceiver_);
            } else {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("数组索引越界: " + std::to_string(i));
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = std::move(lastMutatedReceiver_);
        } else {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("该类型不支持索引赋值");
        }
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_WRITEBACK_INDEX_LOCAL: {
        // 操作数: slot(1B)，索引从栈顶 pop
        uint8_t slot = chunk.code[ip + 1];
        Value index = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot >= stack_.size()) {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("OP_WRITEBACK_INDEX_LOCAL: 栈槽越界");
        }
        Value& obj = stack_[bp + slot];
        if (obj.isArray() && index.isInt()) {
            int64_t i = index.intVal();
            if (i >= 0 && static_cast<size_t>(i) < obj.arrayVal().size()) {
                obj.arrayVal()[static_cast<size_t>(i)] = std::move(lastMutatedReceiver_);
            } else {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("数组索引越界: " + std::to_string(i));
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = std::move(lastMutatedReceiver_);
        } else {
            lastMutatedReceiver_ = Value::nullValue();
            return runtimeError("该类型不支持索引赋值");
        }
        lastMutatedReceiver_ = Value::nullValue();
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 其他指令（输出、栈操作、跳转、字段初始化）
// ============================================================

VMResult VM::executeMiscOps(OpCode op, size_t& ip) {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;

    switch (op) {
    case OpCode::OP_PRINT: {
        Value val = pop();
        // S5 fix: 字符串类型直接用 stringVal() 引用，避免 toString() 拷贝
        outputCallback_(val.isString() ? val.stringVal() : val.toString());
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_POP:
        pop();
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_DUP:
        push(peek(0));
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_DUP_N: {
        uint8_t depth = chunk.code[ip + 1];
        if (depth >= stack_.size()) {
            return runtimeError("OP_DUP_N: 栈深度不足");
        }
        push(peek(depth));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_JUMP: {
        uint16_t jump = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (jump >= chunk.code.size()) return runtimeError("跳转目标越界: OP_JUMP");
        notifyStep(ip, op);
        ip = jump;
        break;
    }

    case OpCode::OP_JUMP_IF_FALSE: {
        uint16_t jump = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (jump >= chunk.code.size()) return runtimeError("跳转目标越界: OP_JUMP_IF_FALSE");
        // 注意：不弹出条件值——编译器在 OP_JUMP_IF_FALSE 后显式生成 OP_POP
        // 如果这里也 pop，会导致所有条件/短路表达式的栈操作双重弹出
        notifyStep(ip, op);
        if (!peek(0).isTruthy()) {
            ip = jump;
        } else {
            ip += 3;
        }
        break;
    }

    case OpCode::OP_LOOP: {
        uint16_t loop = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (loop >= chunk.code.size()) return runtimeError("跳转目标越界: OP_LOOP");
        notifyStep(ip, op);
        ip = loop;
        break;
    }

    case OpCode::OP_INIT_FIELD: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[idx].stringVal();
        Value val = pop();
        // 栈顶是实例（OP_CLASS_NEW 推入的），直接修改
        if (!stack_.empty() && stack_.back().isInstance()) {
            stack_.back().fields()[fieldName] = val;
        } else {
            return runtimeError("OP_INIT_FIELD: 栈顶不是实例");
        }
        // M3 fix: 记录字段声明顺序（OP_INIT_FIELD 按 AST 声明顺序执行）
        pendingFieldOrder_.push_back(fieldName);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_TRY_BEGIN: {
        // F11: push try handler，记录 catch 目标和当前栈深度
        uint16_t catchOffset = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        size_t catchIp = ip + 3 + catchOffset;
        if (catchIp >= chunk.code.size()) return runtimeError("OP_TRY_BEGIN: catch 目标越界");
        tryStack_.push_back({catchIp, stack_.size(), frames_.size() - 1});
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_TRY_END: {
        // F11: try 块正常结束，弹出 try 处理器
        if (!tryStack_.empty() && tryStack_.back().frameIndex == frames_.size() - 1) {
            tryStack_.pop_back();
        }
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_THROW: {
        // F11: 弹出栈顶值作为异常，搜索 try 处理器
        // Bug1 fix: 空栈检查 — pop() 在空栈时设置 hasError_ 并返回 null，
        // 若继续 throwException(null) 会使 catch handler 以错误数据执行
        if (stack_.empty()) {
            return runtimeError("throw 语句在空栈上执行");
        }
        Value thrownValue = pop();
        // P2-2 fix: 调试模式下通知步进（throwException 可能改变 ip）
        notifyStep(ip, op);
        return throwException(std::move(thrownValue));
    }

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}
