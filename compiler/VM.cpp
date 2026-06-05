#include "compiler/VM.h"
#include <sstream>
#include <cmath>

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
        return;
    }
    stack_.push_back(val);
}

Value VM::pop() {
    if (stack_.empty()) {
        runtimeError("栈下溢");
        return Value::nullValue();
    }
    Value val = stack_.back();
    stack_.pop_back();
    return val;
}

Value& VM::peek(size_t distance) {
    return stack_[stack_.size() - 1 - distance];
}

VMResult VM::runtimeError(const std::string& msg) {
    lastError_ = msg;
    return VMResult::VM_RUNTIME_ERROR;
}

std::string VM::getLastError() const {
    return lastError_;
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
    info.stackSnapshot = stack_;
    info.globalsSnapshot = globals_;
    stepCallback_(info);
}

VMCallFrame& VM::currentFrame() {
    return frames_.back();
}

const BytecodeChunk& VM::currentChunk() {
    return *currentFrame().chunk;
}

VMResult VM::numericOp(const std::string& op, int line) {
    Value right = pop();
    Value left = pop();

    // 字符串拼接
    if (op == "+" && left.isString() && right.isString()) {
        push(Value(left.stringVal + right.stringVal));
        return VMResult::VM_OK;
    }
    if (op == "+" && (left.isString() || right.isString())) {
        push(Value(left.toString() + right.toString()));
        return VMResult::VM_OK;
    }

    // 数值运算
    if (op == "+") {
        if (left.isInt() && right.isInt()) push(Value(left.intVal + right.intVal));
        else push(Value(left.toDouble() + right.toDouble()));
    } else if (op == "-") {
        if (left.isInt() && right.isInt()) push(Value(left.intVal - right.intVal));
        else push(Value(left.toDouble() - right.toDouble()));
    } else if (op == "*") {
        if (left.isInt() && right.isInt()) push(Value(left.intVal * right.intVal));
        else push(Value(left.toDouble() * right.toDouble()));
    } else if (op == "/") {
        if (right.toDouble() == 0.0) return runtimeError("除零错误");
        if (left.isInt() && right.isInt()) push(Value(left.intVal / right.intVal));
        else push(Value(left.toDouble() / right.toDouble()));
    } else if (op == "%") {
        if (!left.isInt() || !right.isInt()) return runtimeError("取模运算仅支持整数");
        if (right.intVal == 0) return runtimeError("除零错误");
        push(Value(left.intVal % right.intVal));
    }

    return VMResult::VM_OK;
}

// ============================================================
// 初始化 / 单步 / 状态查询
// ============================================================

void VM::initExecution(const CompileResult& result) {
    stack_.clear();
    globals_.clear();
    lastError_.clear();
    frames_.clear();
    functionChunks_ = result.functionChunks;

    // 设置主帧
    VMCallFrame mainFrame;
    mainFrame.chunk = &result.mainChunk;
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
    frames_.clear();
    functionChunks_.clear();
    initialized_ = false;
}

VMResult VM::stepOnce() {
    // 帧已空 → 执行完毕
    if (frames_.empty()) return VMResult::VM_OK;

    VMCallFrame& frame = currentFrame();
    const BytecodeChunk& chunk = *frame.chunk;
    size_t& ip = frame.ip;

    // 当前 chunk 执行完毕 → 弹帧
    if (ip >= chunk.code.size()) {
        frames_.pop_back();
        if (!frames_.empty()) {
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
            frames_.pop_back();
            if (!frames_.empty()) {
                push(Value::nullValue());
            }
            continue;
        }

        VMResult r = executeOneInstruction();
        if (r != VMResult::VM_OK) return r;
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
        push(chunk.constants[idx]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_INT: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        push(chunk.constants[idx]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_FLOAT: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        push(chunk.constants[idx]);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_STRING: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
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
        VMResult r = numericOp("+", line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_SUBTRACT: {
        VMResult r = numericOp("-", line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_MULTIPLY: {
        VMResult r = numericOp("*", line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_DIVIDE: {
        VMResult r = numericOp("/", line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_MODULO: {
        VMResult r = numericOp("%", line);
        if (r != VMResult::VM_OK) return r;
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_NEGATE: {
        Value val = pop();
        if (val.isInt()) push(Value(-val.intVal));
        else if (val.isFloat()) push(Value(-val.floatVal));
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
        push(Value(left.toDouble() < right.toDouble()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_GREATER: {
        Value right = pop();
        Value left = pop();
        push(Value(left.toDouble() > right.toDouble()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_LESS_EQUAL: {
        Value right = pop();
        Value left = pop();
        push(Value(left.toDouble() <= right.toDouble()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_GREATER_EQUAL: {
        Value right = pop();
        Value left = pop();
        push(Value(left.toDouble() >= right.toDouble()));
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_AND: {
        Value left = pop();
        if (!left.isTruthy()) {
            push(Value(false));
        } else {
            // 左操作数为真，结果取决于右操作数（已在栈上由编译器短路机制处理）
            // 注：Compiler 当前不生成 OP_AND，使用 OP_JUMP_IF_FALSE + OP_POP 实现
        }
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_OR: {
        Value left = pop();
        if (left.isTruthy()) {
            push(Value(true));
        } else {
            // 左操作数为假，结果取决于右操作数（已在栈上由编译器短路机制处理）
            // 注：Compiler 当前不生成 OP_OR，使用 OP_JUMP_IF_FALSE + OP_POP 实现
        }
        notifyStep(ip, op);
        ip += 1;
        break;
    }

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
        std::string name = chunk.constants[idx].stringVal;
        Value val = pop();
        globals_[name] = val;
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_GET_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        std::string name = chunk.constants[idx].stringVal;
        auto it = globals_.find(name);
        if (it != globals_.end()) {
            push(it->second);
        } else {
            push(Value::nullValue());
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_SET_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        std::string name = chunk.constants[idx].stringVal;
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
        Value cond = pop();
        notifyStep(ip, op);
        if (!cond.isTruthy()) {
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
        frames_.pop_back();

        // init 方法返回 this 实例而非 null
        if (retFrame.functionName.size() >= 5 &&
            retFrame.functionName.substr(retFrame.functionName.size() - 5) == ".init") {
            if (retFrame.basePointer < stack_.size()) {
                result = stack_[retFrame.basePointer];
            }
        }

        if (frames_.empty()) {
            push(result);
            notifyStep(ip, op);
            return VMResult::VM_OK;
        }
        // 恢复栈：清理当前帧的局部变量和参数
        stack_.resize(retFrame.basePointer);
        push(result);
        // 恢复 ip
        currentFrame().ip = retFrame.returnIp;
        notifyStep(ip, op);
        break;
    }

    case OpCode::OP_CALL: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        std::string funName = chunk.constants[idx].stringVal;

        auto it = functionChunks_.find(funName);
        if (it == functionChunks_.end()) {
            for (uint8_t i = 0; i < argCount; ++i) {
                pop();
            }
            push(Value::nullValue());
            notifyStep(ip, op);
            ip += 4;
            break;
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

        VMCallFrame newFrame;
        newFrame.chunk = &targetChunk;
        newFrame.returnIp = ip + 4;
        newFrame.basePointer = stack_.size() - argCount;
        newFrame.functionName = funName;
        newFrame.ip = 0;
        frames_.push_back(newFrame);

        notifyStep(ip, op);
        break;
    }

    case OpCode::OP_BUILD_ARRAY: {
        uint8_t count = chunk.code[ip + 1];
        std::vector<Value> elements;
        for (uint8_t i = 0; i < count; ++i) {
            elements.insert(elements.begin(), pop());
        }
        push(Value(elements));
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_INDEX_GET: {
        Value idx = pop();
        Value obj = pop();
        if (obj.isArray() && idx.isInt()) {
            int i = idx.intVal;
            if (i >= 0 && i < static_cast<int>(obj.arrayVal.size())) {
                push(obj.arrayVal[i]);
            } else {
                push(Value::nullValue());
            }
        } else if (obj.isDict() && idx.isString()) {
            auto it = obj.dictVal.find(idx.stringVal);
            if (it != obj.dictVal.end()) {
                push(it->second);
            } else {
                push(Value::nullValue());
            }
        } else {
            push(Value::nullValue());
        }
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_INDEX_SET: {
        Value val = pop();
        Value idx = pop();
        Value obj = pop();
        push(val);
        notifyStep(ip, op);
        ip += 1;
        break;
    }

    case OpCode::OP_MEMBER_GET: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        std::string fieldName = chunk.constants[idx].stringVal;
        Value obj = pop();
        if (obj.isInstance()) {
            auto it = obj.fields.find(fieldName);
            if (it != obj.fields.end()) {
                push(it->second);
            } else {
                push(Value::nullValue());
            }
        } else if (obj.isDict()) {
            auto it = obj.dictVal.find(fieldName);
            if (it != obj.dictVal.end()) {
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
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        std::string fieldName = chunk.constants[idx].stringVal;
        Value val = pop();
        Value obj = pop();
        push(val);
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_METHOD_CALL: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        std::string methodName = chunk.constants[idx].stringVal;

        Value obj = peek(argCount);

        if (obj.isInstance()) {
            std::string methodKey = obj.className + "." + methodName;
            auto it = functionChunks_.find(methodKey);
            if (it != functionChunks_.end()) {
                const BytecodeChunk& targetChunk = it->second;

                if (frames_.size() >= MAX_FRAMES) {
                    return runtimeError("调用栈溢出");
                }

                VMCallFrame newFrame;
                newFrame.chunk = &targetChunk;
                newFrame.returnIp = ip + 4;
                newFrame.basePointer = stack_.size() - argCount - 1;
                newFrame.functionName = methodKey;
                newFrame.ip = 0;
                frames_.push_back(newFrame);

                notifyStep(ip, op);
                break;
            }
        }

        for (uint8_t i = 0; i < argCount; ++i) pop();
        pop();
        push(Value::nullValue());
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    case OpCode::OP_DUP:
        push(peek(0));
        notifyStep(ip, op);
        ip += 1;
        break;

    case OpCode::OP_CLOSURE: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint8_t argCount = chunk.code[ip + 3];
        std::string funName = chunk.constants[idx].stringVal;
        Value closure = Value::makeClosure(funName, nullptr, {});
        auto it = functionChunks_.find(funName);
        if (it != functionChunks_.end()) {
            for (int i = 0; i < it->second.arity; ++i) {
                closure.closureParams.push_back("param" + std::to_string(i));
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
        std::string className = chunk.constants[idx].stringVal;

        Value instance = Value::makeInstance(className);
        push(instance);

        std::string initKey = className + ".init";
        auto it = functionChunks_.find(initKey);
        if (it != functionChunks_.end()) {
            const BytecodeChunk& initChunk = it->second;

            if (frames_.size() >= MAX_FRAMES) {
                return runtimeError("调用栈溢出");
            }

            VMCallFrame newFrame;
            newFrame.chunk = &initChunk;
            newFrame.returnIp = ip + 4;
            newFrame.basePointer = stack_.size() - argCount - 1;
            newFrame.functionName = initKey;
            newFrame.ip = 0;
            frames_.push_back(newFrame);

            notifyStep(ip, op);
            break;
        }

        for (uint8_t i = 0; i < argCount; ++i) pop();
        notifyStep(ip, op);
        ip += 4;
        break;
    }

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}
