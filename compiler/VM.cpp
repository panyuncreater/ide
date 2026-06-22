#include "compiler/VM.h"
#include <sstream>
#include <climits>
#include <cstdint>
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
    static const Value nullSentinel;  // 静态 null 哨兵，用于越界访问
    if (distance >= stack_.size()) {
        return nullSentinel;
    }
    return stack_[stack_.size() - 1 - distance];
}

VMResult VM::runtimeError(const std::string& msg) {
    lastError_ = msg;
    lastErrorLine_ = getCurrentLine();
    hasError_ = true;
    return VMResult::VM_RUNTIME_ERROR;
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
    for (int guard = 0; guard < 256 && !cur.empty(); ++guard) {
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
            // VM fix: 预估大小避免 realloc
            auto rs = rightRef.toString();
            std::string ls = leftRef.toString();
            ls.reserve(ls.size() + rs.size());
            ls.append(rs);
            stack_[stack_.size() - 2] = Value(std::move(ls));
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
            if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
                return runtimeError("整数加法溢出");
            stack_[stack_.size() - 2] = Value(a + b);
        }
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() + rightRef.toDouble());
        break;
    case OP_SUB_INT:
        if (leftRef.isInt() && rightRef.isInt()) {
            int64_t a = leftRef.intVal(), b = rightRef.intVal();
            if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
                return runtimeError("整数减法溢出");
            stack_[stack_.size() - 2] = Value(a - b);
        }
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() - rightRef.toDouble());
        break;
    case OP_MUL_INT:
        if (leftRef.isInt() && rightRef.isInt()) {
            int64_t a = leftRef.intVal(), b = rightRef.intVal();
            if (a != 0 && b != 0) {
                if (a == -1 && b == INT64_MIN) return runtimeError("整数乘法溢出");
                if (b == -1 && a == INT64_MIN) return runtimeError("整数乘法溢出");
                if ((a > 0 && b > 0 && a > INT64_MAX / b) ||
                    (a > 0 && b < 0 && b < INT64_MIN / a) ||
                    (a < 0 && b > 0 && a < INT64_MIN / b) ||
                    (a < 0 && b < 0 && a < INT64_MAX / b))
                    return runtimeError("整数乘法溢出");
            }
            stack_[stack_.size() - 2] = Value(a * b);
        }
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() * rightRef.toDouble());
        break;
    case OP_DIV_INT:
        if (rightRef.isInt() && leftRef.isInt()) {
            int64_t b = rightRef.intVal();
            if (b == 0) return runtimeError("除零错误");
            if (leftRef.intVal() == INT64_MIN && b == -1) return runtimeError("整数除法溢出");
            stack_[stack_.size() - 2] = Value(leftRef.intVal() / b);  // int/int → int
            break;
        }
        if (rightRef.toDouble() == 0.0) return runtimeError("除零错误");
        stack_[stack_.size() - 2] = Value(leftRef.toDouble() / rightRef.toDouble());
        break;
    case OP_MOD_INT:
        // Bug3 fix: 接受 float 操作数（截断为整数后取模）
        {
            int64_t a, b;
            if (leftRef.isInt()) a = leftRef.intVal();
            else if (leftRef.isFloat()) {
                double lf = leftRef.floatVal();
                // M-新3 fix: 用 >= 代替 >，因 double(INT64_MAX) 上取整为 2^63，该值无法表示为 int64_t
                if (lf >= -static_cast<double>(INT64_MIN) || lf < static_cast<double>(INT64_MIN))
                    return runtimeError("浮点数转整数溢出");
                a = static_cast<int64_t>(lf);
            }
            else return runtimeError("取模运算需要数值类型");
            if (rightRef.isInt()) b = rightRef.intVal();
            else if (rightRef.isFloat()) {
                double rf = rightRef.floatVal();
                if (rf >= -static_cast<double>(INT64_MIN) || rf < static_cast<double>(INT64_MIN))
                    return runtimeError("浮点数转整数溢出");
                b = static_cast<int64_t>(rf);
            }
            else return runtimeError("取模运算需要数值类型");
            if (b == 0) return runtimeError("除零错误");
            if (a == INT64_MIN && b == -1) { stack_[stack_.size() - 2] = Value(0); break; }
            stack_[stack_.size() - 2] = Value(a % b);
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
        break;
    case 7:
        if (name == "replace") return BuiltinMethod::STR_REPLACE;
        break;
    case 8:
        if (name == "contains") return BuiltinMethod::ARR_CONTAINS; // 数组/字典共用
        if (name == "substr") return BuiltinMethod::STR_SUBSTR;
        break;
    case 9:
        if (name == "indexOf") return BuiltinMethod::STR_INDEX_OF;
        break;
    case 10:
        if (name == "startsWith") return BuiltinMethod::STR_STARTS_WITH;
        if (name == "endsWith") return BuiltinMethod::STR_ENDS_WITH;
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

// ============================================================
// 初始化 / 单步 / 状态查询
// ============================================================

void VM::initExecution(const CompileResult& result) {
    stack_.clear();
    stack_.reserve(256);  // 预分配栈空间，避免频繁 realloc
    globals_.clear();
    lastError_.clear();
    lastErrorLine_ = 0;
    hasError_ = false;
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
    frames_.push_back(mainFrame);

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
    frames_.clear();
    functionChunks_.clear();
    classInfo_.clear();
    globalSlots_.clear();
    globalSlotNames_.clear();
    globalNameToSlot_.clear();
    mainChunk_ = BytecodeChunk();  // 清空主 chunk 副本
    lastMutatedReceiver_ = Value::nullValue();
    pendingFieldOrder_.clear();
    openUpvalues_.clear();       // VM-05/06
    functionClosures_.clear();   // VM-05/06
    initialized_ = false;
}

VMResult VM::stepOnce() {
    // 帧已空 → 执行完毕
    if (frames_.empty()) return VMResult::VM_OK;

    // 已有错误 → 不再执行
    if (hasError_) return VMResult::VM_RUNTIME_ERROR;

    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    size_t& ip = frame.ip;

    // 当前 chunk 执行完毕 → 弹帧（防御性路径，正常情况由 OP_RETURN 处理）
    if (ip >= chunk.code.size()) {
        size_t returnIp = frame.returnIp;
        size_t savedBp = frame.basePointer;
        bool wasInit = frame.isInitCall;
        frames_.pop_back();
        if (!frames_.empty()) {
            currentFrame().ip = returnIp;  // 恢复调用者 ip，避免重复执行调用指令
            // L2 fix: init 帧返回 this 实例而非 null（与 execute() 防御路径一致）
            if (wasInit && savedBp < stack_.size()) {
                push(stack_[savedBp]);
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

    while (!frames_.empty()) {
        VMCallFrame& frame = currentFrame();
        const BytecodeChunk& chunk = *frame.chunk;
        size_t& ip = frame.ip;

        if (ip >= chunk.code.size()) {
            // 当前 chunk 执行完毕 → 弹帧（防御性路径，正常情况由 OP_RETURN 处理）
            size_t returnIp = frame.returnIp;  // 保存调用者返回地址
            size_t savedBp = frame.basePointer;
            bool wasInit = frame.isInitCall;
            frames_.pop_back();
            if (!frames_.empty()) {
                currentFrame().ip = returnIp;   // 恢复调用者 ip，避免重复执行调用指令
                // init 帧返回 this 实例而非 null
                if (wasInit && savedBp < stack_.size()) {
                    push(stack_[savedBp]);
                } else {
                    push(Value::nullValue());
                }
            }
            continue;
        }

        if (hasError_) return VMResult::VM_RUNTIME_ERROR;
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
            if (top.intVal() == INT64_MIN) return runtimeError("整数溢出：无法对最小值取负");
            top = Value(-top.intVal());
        }
        else if (top.isFloat()) top = Value(-top.floatVal());
        else return runtimeError("一元减运算需要数值类型");
        notifyStep(ip, op);
        ip += 1;
        break;
    }

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

    case OpCode::OP_PRINT: {
        Value val = pop();
        outputCallback_(val.toString());
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_POP:
        pop();
        notifyStep(ip, op);
        ip += 1;
        break;

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

    case OpCode::OP_RETURN: {
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
        frames_.pop_back();

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

            // 写回到接收者的原始位置
            if (!frames_.empty()) {
                VMCallFrame& callerFrame = currentFrame();
                size_t callerBp = callerFrame.basePointer;

                // 路径 A：接收者是全局变量 → 写回 globals_ 或 globalSlots_
                if (!recvVarName.empty() && modifiedThis.isInstance()) {
                    auto gsIt = globalNameToSlot_.find(recvVarName);
                    if (gsIt != globalNameToSlot_.end() && globalSlots_[gsIt->second].isInstance()) {
                        for (auto& field : modifiedThis.fields()) {
                            globalSlots_[gsIt->second].fields()[field.first] = field.second;
                        }
                    } else {
                        auto it = globals_.find(recvVarName);
                        if (it != globals_.end() && it->second.isInstance()) {
                            for (auto& field : modifiedThis.fields()) {
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
                            for (auto& field : modifiedThis.fields()) {
                                stack_[callerBp].fields()[field.first] = field.second;
                            }
                            // 同步字段到调用者的字段槽（bp+1..N）
                            if (callerFrame.chunk && !callerFrame.chunk->fieldOrder.empty()) {
                                for (size_t fi = 0; fi < callerFrame.chunk->fieldOrder.size(); ++fi) {
                                    const std::string& fieldName = callerFrame.chunk->fieldOrder[fi];
                                    auto fieldIt = modifiedThis.fields().find(fieldName);
                                    if (fieldIt != modifiedThis.fields().end()) {
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
        if (retChunk || !frames_.empty()) {
            // 先关闭当前帧的 upvalues
            // frames_ 可能已被 pop_back，需通过 saved 数据来关闭
            // 遍历 openUpvalues_ 列表，关闭指向 savedBp 及以上栈槽的 upvalue
            auto it = openUpvalues_.begin();
            while (it != openUpvalues_.end()) {
                auto& uv = *it;
                if (!uv->isClosed && uv->stackSlot >= savedBp) {
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

        if (frames_.empty()) {
            push(result);
            notifyStep(savedIp, op);
            return VMResult::VM_OK;
        }
        // 恢复栈：清理当前帧的局部变量和参数
        stack_.resize(savedBp);
        push(result);
        // 恢复 ip
        currentFrame().ip = savedReturnIp;
        notifyStep(savedIp, op);
        break;
    }

    case OpCode::OP_CALL: {
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
                    if (initChunk.arity != static_cast<int>(argCount)) {
                        return runtimeError("构造函数 init 期望 " +
                            std::to_string(initChunk.arity) + " 个参数，但传入了 " +
                            std::to_string(argCount) + " 个");
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
                    frames_.push_back(newFrame);

                    notifyStep(savedIp, op);
                    break;
                }

                // 无 init 方法：检查是否有多余参数（与解释器行为保持一致）
                if (argCount > 0) {
                    return runtimeError("类 " + cls.name + " 没有 init 方法，但传入了 " +
                                        std::to_string(argCount) + " 个参数");
                }
                push(instance);
                notifyStep(ip, op);
                ip += 4;
                break;
            }

            // 既不是函数也不是类：报运行时错误
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
        if (targetChunk.arity != argCount) {
            return runtimeError("函数 " + funName + " 期望 " +
                std::to_string(targetChunk.arity) + " 个参数，但传入了 " +
                std::to_string(argCount) + " 个");
        }

        if (frames_.size() >= MAX_FRAMES) {
            return runtimeError("调用栈溢出");
        }

        // 预分配局部变量栈空间：函数体内 var 声明的局部变量需要栈槽，
        // 但帧创建时栈上只有参数，需补推 null 填充额外槽位
        int extraSlots = targetChunk.localCount - argCount;
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
        frames_.push_back(newFrame);

        notifyStep(savedIp, op);
        break;
    }

    case OpCode::OP_CALL_EXPR: {
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
        if (targetChunk.arity != argCount) {
            // R3-1 fix: 弹出参数保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("函数 " + callee.closureName() + " 期望 " +
                std::to_string(targetChunk.arity) + " 个参数，但传入了 " +
                std::to_string(argCount) + " 个");
        }

        if (frames_.size() >= MAX_FRAMES) {
            // R3-1 fix: 弹出参数保持栈平衡
            for (int i = 0; i < argCount; ++i) pop();
            return runtimeError("调用栈溢出");
        }

        // 预分配局部变量栈空间
        int extraSlots = targetChunk.localCount - argCount;
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
        frames_.push_back(newFrame);
        notifyStep(savedIp, op);
        break;
    }

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
            dict.emplace(key.toString(), std::move(val));  // 先入后出：最后入栈的键值对最先弹出，emplace 保留首次插入的值 = 源码中后者覆盖
        }
        push(Value(std::move(dict)));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_INDEX_GET: {
        if (stack_.size() < 2) return runtimeError("栈下溢: OP_INDEX_GET");
        Value idx = pop();
        Value obj = pop();
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
            // 缓存上次检查的字符串，循环中重复访问同一字符串时跳过 O(n) 扫描
            if (i >= 0 && static_cast<size_t>(i) < s.size()) {
                bool isAscii;
                if (lastAsciiStr_ == &s) {
                    isAscii = lastAsciiStrIsAscii_;
                } else {
                    isAscii = true;
                    for (size_t b = 0; b < s.size(); ++b) {
                        if (static_cast<unsigned char>(s[b]) >= 0x80) { isAscii = false; break; }
                    }
                    lastAsciiStr_ = &s;
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
        Value obj = pop();
        if (obj.isInstance()) {
            auto it = obj.fields().find(fieldName);
            if (it != obj.fields().end()) {
                push(it->second);
            } else {
                return runtimeError("类 " + obj.className() + " 没有字段或方法 '" + fieldName + "'");
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

    case OpCode::OP_SUPER_CALL:
    case OpCode::OP_METHOD_CALL: {
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
        // C10: 方法名一次性分类为枚举
        BuiltinMethod method = classifyBuiltinMethod(methodName);

        // ---- 数组内置方法 ----
        if (obj.isArray()) {
            // 先弹出参数（接收者仍在栈上，const ref 有效）
            SmallArgs<Value> args(argCount);
            for (int i = argCount - 1; i >= 0; --i) args[i] = pop();

            // 非变异路径：从 const 引用计算结果，无需拷贝接收者
            Value result = Value::nullValue();
            if (method == BuiltinMethod::ARR_LEN) {
                result = Value(static_cast<int64_t>(obj.arrayVal().size()));
                pop(); push(result); notifyStep(ip, op); ip += instrLen; break;
            }
            if (method == BuiltinMethod::ARR_CONTAINS) {
                if (args.size() != 1) return runtimeError("contains 期望 1 个参数");
                bool found = false;
                for (const auto& elem : obj.arrayVal()) {
                    if (elem.equals(args[0])) { found = true; break; }
                }
                result = Value(found);
                pop(); push(result); notifyStep(ip, op); ip += instrLen; break;
            }
            if (method == BuiltinMethod::ARR_JOIN) {
                std::string sep = args.empty() ? "" : args[0].toString();
                std::string joined;
                joined.reserve(obj.arrayVal().size() * (8 + sep.size()));
                for (size_t i = 0; i < obj.arrayVal().size(); ++i) {
                    if (i > 0) joined += sep;
                    joined += obj.arrayVal()[i].toString();
                }
                result = Value(std::move(joined));
                pop(); push(result); notifyStep(ip, op); ip += instrLen; break;
            }

            // A2 变异路径：先 pop 接收者(move, refcount 不变)，再用 tryGetMutable 原地修改
            Value mutableObj = std::move(stack_.back());
            stack_.pop_back();

            if (method == BuiltinMethod::ARR_PUSH) {
                if (args.size() != 1) return runtimeError("push 期望 1 个参数");
                auto* arr = mutableObj.tryGetMutableArray();
                if (arr) arr->push_back(std::move(args[0]));
                else mutableObj.arrayVal().push_back(args[0]);
            } else if (method == BuiltinMethod::ARR_POP) {
                auto* arr = mutableObj.tryGetMutableArray();
                if (arr) {
                    if (arr->empty()) return runtimeError("对空数组调用 pop");
                    result = std::move(arr->back());
                    arr->pop_back();
                } else {
                    if (mutableObj.arrayVal().empty()) return runtimeError("对空数组调用 pop");
                    result = mutableObj.arrayVal().back();
                    mutableObj.arrayVal().pop_back();
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
                    if (ri < 0 || static_cast<size_t>(ri) >= mutableObj.arrayVal().size())
                        return runtimeError("数组索引越界: " + std::to_string(ri));
                    mutableObj.arrayVal().erase(mutableObj.arrayVal().begin() + static_cast<size_t>(ri));
                }
            } else {
                return runtimeError("数组没有方法 " + methodName);
            }

            // 写回变异后的对象（P7 fix: 使用 std::move 避免二次深拷贝）
            if (receiverVarIdx != 0xFFFF && receiverVarIdx < chunk.constants.size()) {
                const std::string& recvName = chunk.constants[receiverVarIdx].stringVal();
                auto gsIt = globalNameToSlot_.find(recvName);
                if (gsIt != globalNameToSlot_.end()) {
                    globalSlots_[gsIt->second] = std::move(mutableObj);
                } else {
                    globals_[recvName] = std::move(mutableObj);
                }
            } else if (receiverLocalSlotByte != 0xFF) {
                size_t bp = currentFrame().basePointer;
                // P7 fix: 先拷贝到字段（需要完整副本），再 move 到栈槽
                if (receiverLocalSlotByte > 0 && bp < stack_.size() && stack_[bp].isInstance()) {
                    VMCallFrame& curFrame = currentFrame();
                    if (curFrame.chunk && receiverLocalSlotByte <= curFrame.chunk->fieldOrder.size()) {
                        const std::string& fn = curFrame.chunk->fieldOrder[receiverLocalSlotByte - 1];
                        stack_[bp].fields()[fn] = mutableObj;  // 拷贝（move 前）
                    }
                }
                if (bp + receiverLocalSlotByte < stack_.size()) {
                    stack_[bp + receiverLocalSlotByte] = std::move(mutableObj);  // move 在后
                }
            } else {
                lastMutatedReceiver_ = std::move(mutableObj);
            }
            push(result);
            notifyStep(ip, op);
            ip += instrLen;
            break;
        }

        // ---- 字典内置方法 ----
        if (obj.isDict()) {
            SmallArgs<Value> args(argCount);
            for (int i = argCount - 1; i >= 0; --i) args[i] = pop();

            Value result = Value::nullValue();

            // 非变异路径：直接从 const 引用读取
            if (method == BuiltinMethod::DICT_LEN || method == BuiltinMethod::ARR_LEN) {
                result = Value(static_cast<int64_t>(obj.dictVal().size()));
                pop(); push(result); notifyStep(ip, op); ip += instrLen; break;
            }
            if (method == BuiltinMethod::DICT_KEYS) {
                std::vector<Value> keys;
                keys.reserve(obj.dictVal().size());
                for (const auto& kv : obj.dictVal()) keys.emplace_back(Value(kv.first));
                result = Value(std::move(keys));
                pop(); push(result); notifyStep(ip, op); ip += instrLen; break;
            }
            if (method == BuiltinMethod::DICT_VALUES) {
                std::vector<Value> vals;
                vals.reserve(obj.dictVal().size());
                for (const auto& kv : obj.dictVal()) vals.push_back(kv.second);
                result = Value(std::move(vals));
                pop(); push(result); notifyStep(ip, op); ip += instrLen; break;
            }
            if (method == BuiltinMethod::DICT_HAS || method == BuiltinMethod::ARR_CONTAINS) {
                if (args.size() != 1) return runtimeError(methodName + " 期望 1 个参数(键)");
                result = Value(obj.dictVal().find(args[0].toString()) != obj.dictVal().end());
                pop(); push(result); notifyStep(ip, op); ip += instrLen; break;
            }
            if (method == BuiltinMethod::DICT_GET) {
                if (args.empty() || args.size() > 2) return runtimeError("get 期望 1-2 个参数(键[, 默认值])");
                std::string key = args[0].toString();
                auto it = obj.dictVal().find(key);
                if (it != obj.dictVal().end()) {
                    result = it->second;
                } else {
                    result = (args.size() == 2) ? args[1] : Value::nullValue();
                }
                pop(); push(result); notifyStep(ip, op); ip += instrLen; break;
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
            if (receiverVarIdx != 0xFFFF && receiverVarIdx < chunk.constants.size()) {
                const std::string& recvName = chunk.constants[receiverVarIdx].stringVal();
                auto gsIt = globalNameToSlot_.find(recvName);
                if (gsIt != globalNameToSlot_.end()) {
                    globalSlots_[gsIt->second] = std::move(mutableObj);
                } else {
                    globals_[recvName] = std::move(mutableObj);
                }
            } else if (receiverLocalSlotByte != 0xFF) {
                size_t bp = currentFrame().basePointer;
                // P7 fix: 先拷贝到字段（需要完整副本），再 move 到栈槽
                if (receiverLocalSlotByte > 0 && bp < stack_.size() && stack_[bp].isInstance()) {
                    VMCallFrame& curFrame = currentFrame();
                    if (curFrame.chunk && receiverLocalSlotByte <= curFrame.chunk->fieldOrder.size()) {
                        const std::string& fn = curFrame.chunk->fieldOrder[receiverLocalSlotByte - 1];
                        stack_[bp].fields()[fn] = mutableObj;  // 拷贝（move 前）
                    }
                }
                if (bp + receiverLocalSlotByte < stack_.size()) {
                    stack_[bp + receiverLocalSlotByte] = std::move(mutableObj);  // move 在后
                }
            } else {
                lastMutatedReceiver_ = std::move(mutableObj);
            }
            push(result);
            notifyStep(ip, op);
            ip += instrLen;
            break;
        }

        // ---- 字符串内置方法（全部非变异，使用 const 引用）----
        if (obj.isString()) {
            SmallArgs<Value> args(argCount);
            for (int i = argCount - 1; i >= 0; --i) args[i] = pop();

            Value result = Value::nullValue();

            if (method == BuiltinMethod::STR_LEN || method == BuiltinMethod::ARR_LEN || method == BuiltinMethod::DICT_LEN) {
                // V3 fix: 字符串按 UTF-8 码位计数，与解释器 M6 fix 一致
                const std::string& s = obj.stringVal();
                size_t count = 0;
                for (size_t i = 0; i < s.size(); ) {
                    unsigned char c = static_cast<unsigned char>(s[i]);
                    i += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                         ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
                    count++;
                }
                result = Value(static_cast<int64_t>(count));
            } else if (method == BuiltinMethod::STR_UPPER) {
                std::string s = obj.stringVal();
                for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                result = Value(std::move(s));
            } else if (method == BuiltinMethod::STR_LOWER) {
                std::string s = obj.stringVal();
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                result = Value(std::move(s));
            } else if (method == BuiltinMethod::STR_SPLIT) {
                std::string sep = args.empty() ? " " : args[0].toString();
                if (sep.empty()) return runtimeError("split 的分隔符不能为空字符串");
                std::vector<Value> parts;
                const std::string& src = obj.stringVal();
                size_t estCount = 1;
                for (size_t p = 0; (p = src.find(sep, p)) != std::string::npos; p += sep.size()) ++estCount;
                parts.reserve(estCount);
                size_t start = 0, pos;
                while ((pos = src.find(sep, start)) != std::string::npos) {
                    parts.emplace_back(Value(src.substr(start, pos - start)));
                    start = pos + sep.size();
                }
                parts.emplace_back(Value(src.substr(start)));
                result = Value(std::move(parts));
            } else if (method == BuiltinMethod::STR_TRIM) {
                const std::string& s = obj.stringVal();
                size_t l = s.find_first_not_of(" \t\r\n");
                size_t r = s.find_last_not_of(" \t\r\n");
                if (l == std::string::npos) result = Value(std::string(""));
                else result = Value(s.substr(l, r - l + 1));
            } else if (method == BuiltinMethod::STR_CONTAINS) {
                if (args.size() != 1) return runtimeError("contains 期望 1 个参数");
                result = Value(obj.stringVal().find(args[0].toString()) != std::string::npos);
            } else if (method == BuiltinMethod::STR_STARTS_WITH) {
                if (args.size() != 1) return runtimeError("startsWith 期望 1 个参数");
                const std::string& prefix = args[0].toString();
                const std::string& s = obj.stringVal();
                result = Value(s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0);
            } else if (method == BuiltinMethod::STR_ENDS_WITH) {
                if (args.size() != 1) return runtimeError("endsWith 期望 1 个参数");
                const std::string& suffix = args[0].toString();
                const std::string& s = obj.stringVal();
                result = Value(s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
            } else if (method == BuiltinMethod::STR_REPLACE) {
                if (args.size() != 2) return runtimeError("replace 期望 2 个参数(旧串, 新串)");
                std::string s = obj.stringVal();
                const std::string& from = args[0].toString();
                const std::string& to = args[1].toString();
                if (!from.empty()) {
                    size_t pos = 0;
                    while ((pos = s.find(from, pos)) != std::string::npos) {
                        s.replace(pos, from.size(), to);
                        pos += to.size();
                    }
                }
                result = Value(std::move(s));
            } else if (method == BuiltinMethod::STR_SUBSTR) {
                if (args.size() < 1 || args.size() > 2) return runtimeError("substr 期望 1-2 个参数(起始[, 长度])");
                int64_t start = args[0].intVal();
                const std::string& s = obj.stringVal();
                if (start < 0 || static_cast<size_t>(start) > s.size()) result = Value(std::string(""));
                else if (args.size() == 2) {
                    int64_t len = args[1].intVal();
                    result = Value(s.substr(static_cast<size_t>(start), static_cast<size_t>(len)));
                } else {
                    result = Value(s.substr(static_cast<size_t>(start)));
                }
            } else if (method == BuiltinMethod::STR_INDEX_OF) {
                if (args.size() != 1) return runtimeError("indexOf 期望 1 个参数");
                // M2 fix: 返回 UTF-8 字符位置而非字节位置
                const std::string& s = obj.stringVal();
                const std::string& needle = args[0].toString();
                size_t bytePos = s.find(needle);
                if (bytePos == std::string::npos) {
                    result = Value(static_cast<int64_t>(-1));
                } else {
                    // 将字节偏移转换为 UTF-8 字符索引
                    int64_t charIdx = 0;
                    for (size_t b = 0; b < bytePos; ) {
                        unsigned char c = static_cast<unsigned char>(s[b]);
                        b += (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                             ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
                        charIdx++;
                    }
                    result = Value(charIdx);
                }
            } else {
                return runtimeError("字符串没有方法 " + methodName);
            }

            pop();  // 移除接收者（在计算完成后）
            push(result);
            notifyStep(ip, op);
            ip += instrLen;
            break;
        }

        // ---- 类实例方法调用 ----
        if (obj.isInstance()) {
            // 拷贝接收者，因为后续 pop() 会使 peek 引用失效
            Value objCopy = obj;
            // B1 fix: super 调用使用编译时编码的类名（而非运行时实例类名）
            // 避免 3+ 级继承时 super 查找回到子类导致死循环
            std::string searchClassName = objCopy.className();
            if (isSuperCall) {
                uint16_t classIdx = chunk.code[ip + 7] | (chunk.code[ip + 8] << 8);
                if (classIdx < chunk.constants.size()) {
                    searchClassName = chunk.constants[classIdx].stringVal();
                }
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

                // 检查参数数量
                if (targetChunk.arity != argCount) {
                    for (uint8_t i = 0; i < argCount; ++i) pop();
                    pop();
                    return runtimeError("方法 " + methodName + " 期望 " +
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

                // 预分配局部变量栈空间：方法体内 var 声明的局部变量需要栈槽
                int preAllocated = 1 + fieldCount + argCount;  // this + 字段 + 参数
                int extraSlots = targetChunk.localCount - preAllocated;
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
                frames_.push_back(newFrame);

                notifyStep(savedIp, op);
                break;
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

    case OpCode::OP_CLOSURE: {
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
        VMCallFrame& frame = currentFrame();

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

        push(closure);
        notifyStep(ip, op);
        ip = instrBase + upvalueCount * 2; // 跳过 upvalue 描述符
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
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    // VM-05/06: upvalue 读写操作码
    case OpCode::OP_GET_UPVALUE: {
        uint8_t uvIdx = chunk.code[ip + 1];
        VMCallFrame& frame = currentFrame();
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
        VMCallFrame& frame = currentFrame();
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
        VMCallFrame& frame = currentFrame();
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

    case OpCode::OP_CLASS_NEW: {
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
            return runtimeError("未定义的类: " + className);
        }
        VMClassInfo& cls = classIt->second;

        // 创建新实例
        Value instance = Value::makeInstance(cls.name);
        instance.fields() = cls.fieldDefaults;  // 已含继承字段

        // 检查是否有 init 方法（沿继承链查找）
        const BytecodeChunk* initChunkPtr = findMethodChunk(className, "init");

        // S2 fix: 也处理 argCount==0 且 init.arity==0 的自动构造场景
        // （与解释器 visitVarDecl 一致：Point p; 自动调用 0 参数 init）
        bool shouldCallInit = false;
        if (initChunkPtr != nullptr) {
            if (argCount > 0) {
                shouldCallInit = true;
            } else if (initChunkPtr->arity == 0) {
                shouldCallInit = true;  // 0 参数 init，自动构造时调用
            }
        }

        if (shouldCallInit) {
            // 创建 init 帧执行初始化
            const BytecodeChunk& initChunk = *initChunkPtr;
            if (initChunk.arity != static_cast<int>(argCount)) {
                return runtimeError("构造函数 init 期望 " +
                    std::to_string(initChunk.arity) + " 个参数，但传入了 " +
                    std::to_string(argCount) + " 个");
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
            frames_.push_back(newFrame);

            notifyStep(savedIp, op);
            break;
        }

        // 无 init 或 init.arity 不匹配 argCount：推入实例（OP_INIT_FIELD 或手动 init 后续处理）
        push(instance);

        // 无 init 但有参数：报错（与解释器一致）
        if (initChunkPtr == nullptr && argCount > 0) {
            return runtimeError("类 " + cls.name + " 没有 init 方法，但传入了 " +
                         std::to_string(argCount) + " 个参数");
        }

        notifyStep(ip, op);
        ip += 4;
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

    case OpCode::OP_DEFINE_CLASS: {
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
        Value templateInstance = pop();

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
        for (int guard = 0; guard < 256 && !cur.empty(); ++guard) {
            auto clsIt = classInfo_.find(cur);
            if (clsIt == classInfo_.end()) {
                return runtimeError("未定义的父类: " + cur);
            }
            chain.push_back(cur);
            cur = clsIt->second.superClassName;
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
        if (classGsIt != globalNameToSlot_.end()) {
            globalSlots_[classGsIt->second] = classVal;
        } else {
            globals_[className] = classVal;
        }

        notifyStep(ip, op);
        ip += 5;  // nameIdx(2B) + superNameIdx(2B) + opcode(1B)
        break;
    }

    // ---- 嵌套访问变异方法写回指令 ----
    // 从 lastMutatedReceiver_ 取值，写回基对象的字段或索引位置
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
            obj.fields()[fieldName] = lastMutatedReceiver_;
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = lastMutatedReceiver_;
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
            obj.fields()[fieldName] = lastMutatedReceiver_;
            // 如果 slot==0（this），也同步更新对应字段槽
            if (slot == 0) {
                VMCallFrame& curFrame = currentFrame();
                if (curFrame.chunk) {
                    for (size_t i = 0; i < curFrame.chunk->fieldOrder.size(); ++i) {
                        if (curFrame.chunk->fieldOrder[i] == fieldName) {
                            size_t fieldSlot = bp + 1 + i;
                            if (fieldSlot < stack_.size()) {
                                stack_[fieldSlot] = lastMutatedReceiver_;
                            }
                            break;
                        }
                    }
                }
            }
        } else if (obj.isDict()) {
            obj.dictVal()[fieldName] = lastMutatedReceiver_;
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
                obj.arrayVal()[static_cast<size_t>(i)] = lastMutatedReceiver_;
            } else {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("数组索引越界: " + std::to_string(i));
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = lastMutatedReceiver_;
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
                obj.arrayVal()[static_cast<size_t>(i)] = lastMutatedReceiver_;
            } else {
                lastMutatedReceiver_ = Value::nullValue();
                return runtimeError("数组索引越界: " + std::to_string(i));
            }
        } else if (obj.isDict() && index.isString()) {
            obj.dictVal()[index.stringVal()] = lastMutatedReceiver_;
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
