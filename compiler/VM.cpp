#include "compiler/VM.h"
#include <sstream>
#include <cmath>
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

Value VM::pop() {
    if (stack_.empty()) {
        runtimeError("栈下溢");
        hasError_ = true;  // 栈下溢视为不可恢复错误
        return Value::nullValue();
    }
    Value val = stack_.back();
    stack_.pop_back();
    return val;
}

Value VM::peek(size_t distance) const {
    if (distance >= stack_.size()) {
        // peek 是 const 方法，无法调用 runtimeError，但调用方会检查 hasError_
        return Value::nullValue();
    }
    return stack_[stack_.size() - 1 - distance];
}

VMResult VM::runtimeError(const std::string& msg) {
    lastError_ = msg;
    hasError_ = true;
    return VMResult::VM_RUNTIME_ERROR;
}

std::string VM::getLastError() const {
    return lastError_;
}

bool VM::hasError() const {
    return hasError_;
}

std::vector<Value> VM::getStack() const {
    return stack_;
}

std::unordered_map<std::string, Value> VM::getGlobals() const {
    return globals_;
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

void VM::notifyStep(size_t ip, OpCode opcode) {
    if (!stepCallbackEnabled_) return;
    VMStepInfo info;
    info.ip = ip;
    info.opcode = opcode;
    stepCallback_(info);
}

VMCallFrame& VM::currentFrame() {
    return frames_.back();  // 调用方应确保 frames_ 非空（execute/stepOnce 中已检查）
}

const BytecodeChunk& VM::currentChunk() {
    return *currentFrame().chunk;  // chunk 由 initExecution 设置，始终有效
}

const BytecodeChunk* VM::findMethodChunk(const std::string& className,
                                         const std::string& methodName) const {
    std::string cur = className;
    // 沿继承链向上查找，防止循环继承（超过 256 层视为异常）
    for (int guard = 0; guard < 256 && !cur.empty(); ++guard) {
        std::string methodKey = cur + "." + methodName;
        auto it = functionChunks_.find(methodKey);
        if (it != functionChunks_.end()) {
            return &it->second;
        }
        // 查父类
        auto clsIt = classInfo_.find(cur);
        if (clsIt == classInfo_.end()) break;
        cur = clsIt->second.superClassName;
    }
    return nullptr;
}

// 数值运算类型枚举（避免字符串比较）
enum { OP_ADD_INT = 0, OP_SUB_INT, OP_MUL_INT, OP_DIV_INT, OP_MOD_INT };

VMResult VM::numericOp(int opType, int line) {
    // 使用 peek 访问栈顶避免深拷贝，然后调整栈指针
    if (stack_.size() < 2) return runtimeError("栈下溢：二元运算需要两个操作数");

    const Value& rightRef = stack_[stack_.size() - 1];
    const Value& leftRef = stack_[stack_.size() - 2];

    // 字符串拼接（仅加法）
    if (opType == OP_ADD_INT) {
        if (leftRef.isString() && rightRef.isString()) {
            stack_[stack_.size() - 2] = Value(leftRef.stringVal() + rightRef.stringVal());
            stack_.pop_back();
            return VMResult::VM_OK;
        }
        if (leftRef.isString() || rightRef.isString()) {
            stack_[stack_.size() - 2] = Value(leftRef.toString() + rightRef.toString());
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
        if (leftRef.isInt() && rightRef.isInt()) stack_[stack_.size() - 2] = Value(leftRef.intVal() + rightRef.intVal());
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() + rightRef.toDouble());
        break;
    case OP_SUB_INT:
        if (leftRef.isInt() && rightRef.isInt()) stack_[stack_.size() - 2] = Value(leftRef.intVal() - rightRef.intVal());
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() - rightRef.toDouble());
        break;
    case OP_MUL_INT:
        if (leftRef.isInt() && rightRef.isInt()) stack_[stack_.size() - 2] = Value(leftRef.intVal() * rightRef.intVal());
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() * rightRef.toDouble());
        break;
    case OP_DIV_INT:
        if (rightRef.toDouble() == 0.0) return runtimeError("除零错误");
        if (leftRef.isInt() && rightRef.isInt()) {
            if (leftRef.intVal() == INT64_MIN && rightRef.intVal() == -1) return runtimeError("整数除法溢出");
            stack_[stack_.size() - 2] = Value(leftRef.intVal() / rightRef.intVal());
        } else stack_[stack_.size() - 2] = Value(leftRef.toDouble() / rightRef.toDouble());
        break;
    case OP_MOD_INT:
        if (!leftRef.isInt() || !rightRef.isInt()) return runtimeError("取模运算仅支持整数");
        if (rightRef.intVal() == 0) return runtimeError("除零错误");
        if (leftRef.intVal() == INT64_MIN && rightRef.intVal() == -1) { stack_[stack_.size() - 2] = Value(0); break; }
        stack_[stack_.size() - 2] = Value(leftRef.intVal() % rightRef.intVal());
        break;
    }

    stack_.pop_back();  // 弹出 right，保留结果在 left 原位
    return VMResult::VM_OK;
}

// ============================================================
// 初始化 / 单步 / 状态查询
// ============================================================

void VM::initExecution(const CompileResult& result) {
    stack_.clear();
    globals_.clear();
    lastError_.clear();
    hasError_ = false;
    frames_.clear();
    functionChunks_ = result.functionChunks;
    classInfo_.clear();
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
    hasError_ = false;
    frames_.clear();
    functionChunks_.clear();
    classInfo_.clear();
    mainChunk_ = BytecodeChunk();  // 清空主 chunk 副本
    lastMutatedReceiver_ = Value::nullValue();
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
        frames_.pop_back();
        if (!frames_.empty()) {
            currentFrame().ip = returnIp;  // 恢复调用者 ip，避免重复执行调用指令
            push(Value::nullValue());
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
            frames_.pop_back();
            if (!frames_.empty()) {
                currentFrame().ip = returnIp;   // 恢复调用者 ip，避免重复执行调用指令
                push(Value::nullValue());
            }
            continue;
        }

        VMResult r = executeOneInstruction();
        if (r != VMResult::VM_OK || hasError_) return r;
    }

    return VMResult::VM_OK;
}

// ============================================================
// executeOneInstruction() — 单条指令执行（核心逻辑）
// ============================================================

VMResult VM::executeOneInstruction() {
    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    size_t& ip = frame.ip;

    OpCode op = static_cast<OpCode>(chunk.code[ip]);
    int line = chunk.getLine(ip);

    switch (op) {
    case OpCode::OP_CONSTANT: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界 (idx=" + std::to_string(idx) + ")");
        push(chunk.constants[idx]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_INT: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        push(chunk.constants[idx]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_FLOAT: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        push(chunk.constants[idx]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

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
        VMResult r = numericOp(OP_ADD_INT, line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_SUBTRACT: {
        VMResult r = numericOp(OP_SUB_INT, line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_MULTIPLY: {
        VMResult r = numericOp(OP_MUL_INT, line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_DIVIDE: {
        VMResult r = numericOp(OP_DIV_INT, line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_MODULO: {
        VMResult r = numericOp(OP_MOD_INT, line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_NEGATE: {
        Value val = pop();
        if (val.isInt()) push(Value(-val.intVal()));
        else if (val.isFloat()) push(Value(-val.floatVal()));
        else return runtimeError("一元减运算需要数值类型");
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_NOT: {
        Value val = pop();
        push(Value(!val.isTruthy()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_EQUAL: {
        Value right = pop();
        Value left = pop();
        push(Value(left.equals(right)));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_NOT_EQUAL: {
        Value right = pop();
        Value left = pop();
        push(Value(!left.equals(right)));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_LESS: {
        Value right = pop();
        Value left = pop();
        if (!left.isNumber() || !right.isNumber())
            return runtimeError("比较运算需要数值类型");
        push(Value(left.toDouble() < right.toDouble()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_GREATER: {
        Value right = pop();
        Value left = pop();
        if (!left.isNumber() || !right.isNumber())
            return runtimeError("比较运算需要数值类型");
        push(Value(left.toDouble() > right.toDouble()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_LESS_EQUAL: {
        Value right = pop();
        Value left = pop();
        if (!left.isNumber() || !right.isNumber())
            return runtimeError("比较运算需要数值类型");
        push(Value(left.toDouble() <= right.toDouble()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_GREATER_EQUAL: {
        Value right = pop();
        Value left = pop();
        if (!left.isNumber() || !right.isNumber())
            return runtimeError("比较运算需要数值类型");
        push(Value(left.toDouble() >= right.toDouble()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

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
        globals_[name] = val;
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_GET_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& name = chunk.constants[idx].stringVal();
        auto it = globals_.find(name);
        if (it != globals_.end()) {
            push(it->second);
        } else {
            return runtimeError("未定义的变量: " + name);
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
        globals_[name] = val;
        // 不推入值：赋值是语句而非表达式，不返回值
        // 与 OP_SET_LOCAL 语义一致（SET_LOCAL 用 peek 不 pop 也不 push）
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_JUMP: {
        uint16_t jump = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        notifyStep(ip, op);
        ip = jump;
        break;
    }

    case OpCode::OP_JUMP_IF_FALSE: {
        uint16_t jump = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
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
        notifyStep(ip, op);
        ip = loop;
        break;
    }

    case OpCode::OP_RETURN: {
        Value result = pop();
        VMCallFrame retFrame = frames_.back();
        // 在 pop_back 之前保存 ip 值，避免悬空引用
        size_t savedIp = ip;
        frames_.pop_back();

        // 方法调用字段同步：将方法内修改的字段槽（bp+1..N）同步回 this（bp）
        // 对所有 isMethodCall 都执行，不受 receiverVarName 限制
        if (retFrame.isMethodCall && retFrame.basePointer < stack_.size()) {
            Value& modifiedThis = stack_[retFrame.basePointer];

            // 先把方法内的字段槽（bp+1..N）同步回 this
            if (modifiedThis.isInstance() && retFrame.chunk && !retFrame.chunk->fieldOrder.empty()) {
                for (size_t fi = 0; fi < retFrame.chunk->fieldOrder.size(); ++fi) {
                    size_t pos = retFrame.basePointer + 1 + fi;
                    if (pos < stack_.size()) {
                        modifiedThis.fields()[retFrame.chunk->fieldOrder[fi]] = stack_[pos];
                    }
                }
            }

            // 写回到接收者的原始位置
            if (!frames_.empty()) {
                VMCallFrame& callerFrame = currentFrame();
                size_t callerBp = callerFrame.basePointer;

                // 路径 A：接收者是全局变量 → 写回 globals_
                if (!retFrame.receiverVarName.empty() && modifiedThis.isInstance()) {
                    auto it = globals_.find(retFrame.receiverVarName);
                    if (it != globals_.end() && it->second.isInstance()) {
                        for (auto& field : modifiedThis.fields()) {
                            it->second.fields()[field.first] = field.second;
                        }
                    }
                }

                // 路径 B：接收者是调用者帧中的局部变量 → 写回栈帧
                if (retFrame.receiverLocalSlot >= 0 &&
                    callerBp + retFrame.receiverLocalSlot < stack_.size()) {
                    size_t receiverPos = callerBp + retFrame.receiverLocalSlot;

                    if (retFrame.receiverLocalSlot == 0) {
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
                            retFrame.receiverLocalSlot <= static_cast<int>(callerFrame.chunk->fieldOrder.size())) {
                            const std::string& fieldName = callerFrame.chunk->fieldOrder[retFrame.receiverLocalSlot - 1];
                            stack_[callerBp].fields()[fieldName] = modifiedThis;
                        }
                    }
                }
            }
        }

        // init 方法返回 this 实例而非 null（在字段同步之后读取）
        if (retFrame.isInitCall) {
            if (retFrame.basePointer < stack_.size()) {
                result = stack_[retFrame.basePointer];
            }
        }

        if (frames_.empty()) {
            push(result);
            notifyStep(savedIp, op);
            return VMResult::VM_OK;
        }
        // 恢复栈：清理当前帧的局部变量和参数
        stack_.resize(retFrame.basePointer);
        push(result);
        // 恢复 ip
        currentFrame().ip = retFrame.returnIp;
        notifyStep(savedIp, op);
        break;
    }

    case OpCode::OP_CALL: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& funName = chunk.constants[idx].stringVal();

        auto it = functionChunks_.find(funName);
        if (it == functionChunks_.end()) {
            // 检查是否为类构造调用
            auto classIt = classInfo_.find(funName);
            if (classIt != classInfo_.end()) {
                VMClassInfo& cls = classIt->second;

                // 收集参数
                std::vector<Value> args;
                args.reserve(argCount);
                for (uint8_t i = 0; i < argCount; ++i) {
                    args.push_back(pop());
                }
                std::reverse(args.begin(), args.end());

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
                    frames_.push_back(newFrame);

                    notifyStep(ip, op);
                    break;
                }

                // 无 init 方法：直接返回新实例
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

        const BytecodeChunk& targetChunk = it->second;
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
        frames_.push_back(newFrame);

        notifyStep(ip, op);
        break;
    }

    case OpCode::OP_BUILD_ARRAY: {
        uint8_t count = chunk.code[ip + 1];
        std::vector<Value> elements;
        elements.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            elements.push_back(pop());
        }
        // 栈是后进先出，需要反转
        std::reverse(elements.begin(), elements.end());
        push(Value(elements));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_BUILD_DICT: {
        uint8_t pairCount = chunk.code[ip + 1];
        std::unordered_map<std::string, Value> dict;
        // 先入后出：倒序弹出键值对
        for (uint8_t i = 0; i < pairCount; ++i) {
            Value val = pop();
            Value key = pop();
            dict[key.toString()] = val;
        }
        push(Value(dict));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_INDEX_GET: {
        Value idx = pop();
        Value obj = pop();
        if (obj.isArray() && idx.isInt()) {
            int i = idx.intVal();
            if (i >= 0 && i < static_cast<int>(obj.arrayVal().size())) {
                push(obj.arrayVal()[i]);
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i) + " (长度: " + std::to_string(obj.arrayVal().size()) + ")");
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
        } else {
            return runtimeError("该类型不支持索引访问");
        }
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_INDEX_SET: {
        // fallback 路径：用于非简单变量的嵌套访问（如 arr[i][j] = val）
        // Value 是值语义，pop 出来的是副本，修改副本无法写回原位置
        // 报错提示，并清理栈上的操作数
        Value val = pop();
        Value idx = pop();
        Value obj = pop();
        (void)obj; (void)idx; (void)val;  // 消除未使用警告
        return runtimeError("VM 不支持嵌套索引赋值（如 arr[i][j] = val），请使用临时变量");
    }

    case OpCode::OP_INDEX_SET_VAR: {
        // 直接修改 globals_[varName] 中的数组/字典元素
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[idx].stringVal();
        Value val = pop();
        Value index = pop();
        auto it = globals_.find(varName);
        if (it != globals_.end()) {
            Value& obj = it->second;  // 引用，直接修改
            if (obj.isArray() && index.isInt()) {
                int i = index.intVal();
                if (i >= 0 && i < static_cast<int>(obj.arrayVal().size())) {
                    obj.arrayVal()[i] = val;
                }
            } else if (obj.isDict() && index.isString()) {
                obj.dictVal()[index.stringVal()] = val;
            }
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_INDEX_SET_LOCAL: {
        // 直接修改 stack_[bp+slot] 中的数组/字典元素（用于方法内 this.arr[i] = val）
        uint8_t slot = chunk.code[ip + 1];
        Value val = pop();
        Value index = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot < stack_.size()) {
            Value& obj = stack_[bp + slot];  // 栈引用，直接修改
            if (obj.isArray() && index.isInt()) {
                int i = index.intVal();
                if (i >= 0 && i < static_cast<int>(obj.arrayVal().size())) {
                    obj.arrayVal()[i] = val;
                }
            } else if (obj.isDict() && index.isString()) {
                obj.dictVal()[index.stringVal()] = val;
            }
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

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
                push(Value::nullValue());
            }
        } else if (obj.isDict()) {
            auto it = obj.dictVal().find(fieldName);
            if (it != obj.dictVal().end()) {
                push(it->second);
            } else {
                push(Value::nullValue());
            }
        } else {
            push(Value::nullValue());
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_MEMBER_SET: {
        // fallback 路径：用于非简单变量的嵌套成员赋值
        // Value 是值语义，pop 出来的是副本，修改副本无法写回原位置
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& fieldName = chunk.constants[idx].stringVal();
        Value val = pop();
        Value obj = pop();
        (void)obj; (void)fieldName; (void)val;  // 消除未使用警告
        return runtimeError("VM 不支持嵌套成员赋值（如 arr[i].field = val），请使用临时变量");
    }

    case OpCode::OP_MEMBER_SET_VAR: {
        // 直接修改 globals_[varName].fields()[fieldName]
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        if (varIdx >= chunk.constants.size() || fieldIdx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& varName = chunk.constants[varIdx].stringVal();
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
        Value val = pop();
        auto it = globals_.find(varName);
        if (it != globals_.end()) {
            Value& obj = it->second;  // 引用，直接修改
            if (obj.isInstance()) {
                obj.fields()[fieldName] = val;
            } else if (obj.isDict()) {
                obj.dictVal()[fieldName] = val;
            }
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
        if (bp + slot < stack_.size()) {
            Value& obj = stack_[bp + slot];  // 栈引用，直接修改
            if (obj.isInstance()) {
                obj.fields()[fieldName] = val;
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
            }
        }
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    case OpCode::OP_METHOD_CALL: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        uint16_t receiverVarIdx = chunk.code[ip + 4] | (chunk.code[ip + 5] << 8);
        uint8_t receiverLocalSlotByte = chunk.code[ip + 6];
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& methodName = chunk.constants[idx].stringVal();

        Value obj = peek(argCount);  // peek 返回 Value 拷贝

        // ---- 数组内置方法 ----
        if (obj.isArray()) {
            std::vector<Value> args;
            args.reserve(argCount);
            for (uint8_t i = 0; i < argCount; ++i) args.push_back(pop());
            std::reverse(args.begin(), args.end());
            pop();  // 移除接收者

            Value result = Value::nullValue();
            bool mutated = false;

            if (methodName == "push") {
                if (args.size() != 1) return runtimeError("push 期望 1 个参数");
                obj.arrayVal().push_back(args[0]);
                mutated = true;
            } else if (methodName == "pop") {
                if (obj.arrayVal().empty()) return runtimeError("对空数组调用 pop");
                result = obj.arrayVal().back();
                obj.arrayVal().pop_back();
                mutated = true;
            } else if (methodName == "len") {
                result = Value(static_cast<int>(obj.arrayVal().size()));
            } else if (methodName == "remove") {
                if (args.size() != 1) return runtimeError("remove 期望 1 个参数(索引)");
                if (!args[0].isInt()) return runtimeError("remove 参数必须是整数索引");
                int ri = args[0].intVal();
                if (ri < 0 || static_cast<size_t>(ri) >= obj.arrayVal().size())
                    return runtimeError("数组索引越界: " + std::to_string(ri));
                obj.arrayVal().erase(obj.arrayVal().begin() + ri);
                mutated = true;
            } else if (methodName == "contains") {
                if (args.size() != 1) return runtimeError("contains 期望 1 个参数");
                bool found = false;
                for (const auto& elem : obj.arrayVal()) {
                    if (elem.equals(args[0])) { found = true; break; }
                }
                result = Value(found);
            } else if (methodName == "join") {
                std::string sep = args.empty() ? "" : args[0].toString();
                std::string joined;
                for (size_t i = 0; i < obj.arrayVal().size(); ++i) {
                    if (i > 0) joined += sep;
                    joined += obj.arrayVal()[i].toString();
                }
                result = Value(joined);
            } else {
                return runtimeError("数组没有方法 " + methodName);
            }

            // 变异方法需要写回修改后的对象
            if (mutated) {
                if (receiverVarIdx != 0xFFFF && receiverVarIdx < chunk.constants.size()) {
                    globals_[chunk.constants[receiverVarIdx].stringVal()] = obj;
                } else if (receiverLocalSlotByte != 0xFF) {
                    size_t bp = currentFrame().basePointer;
                    if (bp + receiverLocalSlotByte < stack_.size()) {
                        stack_[bp + receiverLocalSlotByte] = obj;
                    }
                    // 如果局部变量是调用者 this 的字段，也更新 this.fields()
                    if (receiverLocalSlotByte > 0 && bp < stack_.size() && stack_[bp].isInstance()) {
                        VMCallFrame& curFrame = currentFrame();
                        if (curFrame.chunk && receiverLocalSlotByte <= curFrame.chunk->fieldOrder.size()) {
                            const std::string& fn = curFrame.chunk->fieldOrder[receiverLocalSlotByte - 1];
                            stack_[bp].fields()[fn] = obj;
                        }
                    }
                } else {
                    // 嵌套访问（如 this.arr.push(42)）：暂存修改后的对象，供后续写回指令使用
                    lastMutatedReceiver_ = obj;
                }
            }
            push(result);
            notifyStep(ip, op);
            ip += 7;
            break;
        }

        // ---- 字典内置方法 ----
        if (obj.isDict()) {
            std::vector<Value> args;
            args.reserve(argCount);
            for (uint8_t i = 0; i < argCount; ++i) args.push_back(pop());
            std::reverse(args.begin(), args.end());
            pop();

            Value result = Value::nullValue();
            bool mutated = false;

            if (methodName == "len") {
                result = Value(static_cast<int>(obj.dictVal().size()));
            } else if (methodName == "keys") {
                std::vector<Value> keys;
                for (const auto& kv : obj.dictVal()) keys.push_back(Value(kv.first));
                result = Value(keys);
            } else if (methodName == "values") {
                std::vector<Value> vals;
                for (const auto& kv : obj.dictVal()) vals.push_back(kv.second);
                result = Value(vals);
            } else if (methodName == "has" || methodName == "contains") {
                if (args.size() != 1) return runtimeError(methodName + " 期望 1 个参数(键)");
                result = Value(obj.dictVal().find(args[0].toString()) != obj.dictVal().end());
            } else if (methodName == "remove") {
                if (args.size() != 1) return runtimeError("remove 期望 1 个参数(键)");
                obj.dictVal().erase(args[0].toString());
                mutated = true;
            } else {
                return runtimeError("字典没有方法 " + methodName);
            }

            if (mutated) {
                if (receiverVarIdx != 0xFFFF && receiverVarIdx < chunk.constants.size()) {
                    globals_[chunk.constants[receiverVarIdx].stringVal()] = obj;
                } else if (receiverLocalSlotByte != 0xFF) {
                    size_t bp = currentFrame().basePointer;
                    if (bp + receiverLocalSlotByte < stack_.size()) {
                        stack_[bp + receiverLocalSlotByte] = obj;
                    }
                    if (receiverLocalSlotByte > 0 && bp < stack_.size() && stack_[bp].isInstance()) {
                        VMCallFrame& curFrame = currentFrame();
                        if (curFrame.chunk && receiverLocalSlotByte <= curFrame.chunk->fieldOrder.size()) {
                            const std::string& fn = curFrame.chunk->fieldOrder[receiverLocalSlotByte - 1];
                            stack_[bp].fields()[fn] = obj;
                        }
                    }
                } else {
                    // 嵌套访问（如 this.dict.remove("key")）：暂存修改后的对象，供后续写回指令使用
                    lastMutatedReceiver_ = obj;
                }
            }
            push(result);
            notifyStep(ip, op);
            ip += 7;
            break;
        }

        // ---- 字符串内置方法 ----
        if (obj.isString()) {
            std::vector<Value> args;
            args.reserve(argCount);
            for (uint8_t i = 0; i < argCount; ++i) args.push_back(pop());
            std::reverse(args.begin(), args.end());
            pop();

            Value result = Value::nullValue();

            if (methodName == "len") {
                result = Value(static_cast<int>(obj.stringVal().size()));
            } else if (methodName == "upper") {
                std::string s = obj.stringVal();
                for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                result = Value(s);
            } else if (methodName == "lower") {
                std::string s = obj.stringVal();
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                result = Value(s);
            } else if (methodName == "split") {
                std::string sep = args.empty() ? " " : args[0].toString();
                if (sep.empty()) return runtimeError("split 的分隔符不能为空字符串");
                std::vector<Value> parts;
                size_t start = 0, pos;
                while ((pos = obj.stringVal().find(sep, start)) != std::string::npos) {
                    parts.push_back(Value(obj.stringVal().substr(start, pos - start)));
                    start = pos + sep.size();
                }
                parts.push_back(Value(obj.stringVal().substr(start)));
                result = Value(parts);
            } else if (methodName == "trim") {
                std::string s = obj.stringVal();
                size_t l = s.find_first_not_of(" \t\r\n");
                size_t r = s.find_last_not_of(" \t\r\n");
                if (l == std::string::npos) result = Value(std::string(""));
                else result = Value(s.substr(l, r - l + 1));
            } else {
                return runtimeError("字符串没有方法 " + methodName);
            }
            push(result);
            notifyStep(ip, op);
            ip += 7;
            break;
        }

        // ---- 类实例方法调用 ----
        if (obj.isInstance()) {
            // 沿继承链查找方法（父类方法也可调用）
            const BytecodeChunk* targetChunkPtr = findMethodChunk(obj.className(), methodName);
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

                // 收集参数，重新排列栈
                std::vector<Value> args;
                args.reserve(argCount);
                for (uint8_t i = 0; i < argCount; ++i) {
                    args.push_back(pop());
                }
                // 栈是后进先出，需要反转参数顺序
                std::reverse(args.begin(), args.end());
                pop();  // 移除栈上的原始实例

                // 推入 this（拷贝，方法内修改会被 writeBack 写回）
                push(obj);
                // 按方法 chunk 声明的字段顺序推入实例字段值
                int fieldCount = 0;
                if (targetChunk.fieldOrder.empty()) {
                    // 回退：按 unordered_map 顺序（不保证正确，但兼容旧字节码）
                    for (const auto& field : obj.fields()) {
                        push(field.second);
                    }
                    fieldCount = static_cast<int>(obj.fields().size());
                } else {
                    for (const auto& fieldName : targetChunk.fieldOrder) {
                        auto fieldIt = obj.fields().find(fieldName);
                        if (fieldIt != obj.fields().end()) {
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
                newFrame.returnIp = ip + 7;  // OP_METHOD_CALL 是 7 字节
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
                frames_.push_back(newFrame);

                notifyStep(ip, op);
                break;
            }
        }

        // 方法未找到或对象非实例
        for (uint8_t i = 0; i < argCount; ++i) pop();
        pop();
        if (obj.isInstance()) {
            return runtimeError("类 " + obj.className() + " 没有方法 " + methodName);
        } else {
            return runtimeError("方法调用需要类实例");
        }
    }

    case OpCode::OP_DUP:
        push(peek(0));
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_CLOSURE: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& funName = chunk.constants[idx].stringVal();
        Value closure = Value::makeClosure(funName, nullptr, {});
        // 捕获当前全局变量环境到闭包中
        closure.capturedVars() = {};  // 不再深拷贝整个全局环境（capturedVars 未被有效使用）
        auto it = functionChunks_.find(funName);
        if (it != functionChunks_.end()) {
            for (int i = 0; i < it->second.arity; ++i) {
                closure.closureParams().push_back("param" + std::to_string(i));
            }
        }
        push(closure);
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    case OpCode::OP_GET_LOCAL: {
        uint8_t slot = chunk.code[ip + 1];
        size_t bp = currentFrame().basePointer;
        if (bp + slot < stack_.size()) {
            push(stack_[bp + slot]);
        } else {
            push(Value::nullValue());
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_SET_LOCAL: {
        uint8_t slot = chunk.code[ip + 1];
        size_t bp = currentFrame().basePointer;
        Value val = peek(0);
        if (bp + slot < stack_.size()) {
            stack_[bp + slot] = val;
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_CLASS_NEW: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& className = chunk.constants[idx].stringVal();

        // 收集参数
        std::vector<Value> args;
        args.reserve(argCount);
        for (uint8_t i = 0; i < argCount; ++i) {
            args.push_back(pop());
        }
        std::reverse(args.begin(), args.end());

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

        if (initChunkPtr != nullptr && argCount > 0) {
            // 有 init 方法且有参数：创建 init 帧执行初始化
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
            frames_.push_back(newFrame);

            notifyStep(ip, op);
            break;
        }

        // 无 init 或无参数：推入实例，由后续 OP_INIT_FIELD 设置字段
        push(instance);

        // 如果有 init 但 argCount==0，init 通过后续 OP_METHOD_CALL 调用
        // 如果无 init 且有参数，弹出多余参数（不应发生，但防御性处理）
        if (initChunkPtr == nullptr && argCount > 0) {
            // 无 init 方法但有参数：静默忽略参数（与解释器行为一致）
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
        }
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

        // 从模板实例提取当前类字段
        std::vector<std::string> ownFieldOrder;
        std::unordered_map<std::string, Value> ownFieldDefaults;
        if (templateInstance.isInstance()) {
            for (const auto& field : templateInstance.fields()) {
                ownFieldOrder.push_back(field.first);
                ownFieldDefaults[field.first] = field.second;
            }
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
        globals_[className] = classVal;

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
        if (varIdx < chunk.constants.size() && fieldIdx < chunk.constants.size()) {
            const std::string& varName = chunk.constants[varIdx].stringVal();
            const std::string& fieldName = chunk.constants[fieldIdx].stringVal();
            auto it = globals_.find(varName);
            if (it != globals_.end()) {
                Value& obj = it->second;
                if (obj.isInstance()) {
                    obj.fields()[fieldName] = lastMutatedReceiver_;
                } else if (obj.isDict()) {
                    obj.dictVal()[fieldName] = lastMutatedReceiver_;
                }
            }
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
        if (bp + slot < stack_.size()) {
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
            }
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
        if (varIdx < chunk.constants.size()) {
            const std::string& varName = chunk.constants[varIdx].stringVal();
            auto it = globals_.find(varName);
            if (it != globals_.end()) {
                Value& obj = it->second;
                if (obj.isArray() && index.isInt()) {
                    int i = index.intVal();
                    if (i >= 0 && i < static_cast<int>(obj.arrayVal().size())) {
                        obj.arrayVal()[i] = lastMutatedReceiver_;
                    }
                } else if (obj.isDict() && index.isString()) {
                    obj.dictVal()[index.stringVal()] = lastMutatedReceiver_;
                }
            }
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
        if (bp + slot < stack_.size()) {
            Value& obj = stack_[bp + slot];
            if (obj.isArray() && index.isInt()) {
                int i = index.intVal();
                if (i >= 0 && i < static_cast<int>(obj.arrayVal().size())) {
                    obj.arrayVal()[i] = lastMutatedReceiver_;
                }
            } else if (obj.isDict() && index.isString()) {
                obj.dictVal()[index.stringVal()] = lastMutatedReceiver_;
            }
            // 如果 slot==0（this），也同步更新对应字段槽
            if (slot == 0 && obj.isInstance()) {
                // 索引写回 this 不太常见，但为一致性处理
            }
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
