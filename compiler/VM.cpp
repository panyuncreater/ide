#include "compiler/VM.h"
#include <sstream>
#include <cmath>
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
            stack_[stack_.size() - 2] = Value(leftRef.stringVal + rightRef.stringVal);
            stack_.pop_back();
            return VMResult::VM_OK;
        }
        if (leftRef.isString() || rightRef.isString()) {
            stack_[stack_.size() - 2] = Value(leftRef.toString() + rightRef.toString());
            stack_.pop_back();
            return VMResult::VM_OK;
        }
    }

    // 数值运算
    switch (opType) {
    case OP_ADD_INT:
        if (leftRef.isInt() && rightRef.isInt()) stack_[stack_.size() - 2] = Value(leftRef.intVal + rightRef.intVal);
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() + rightRef.toDouble());
        break;
    case OP_SUB_INT:
        if (leftRef.isInt() && rightRef.isInt()) stack_[stack_.size() - 2] = Value(leftRef.intVal - rightRef.intVal);
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() - rightRef.toDouble());
        break;
    case OP_MUL_INT:
        if (leftRef.isInt() && rightRef.isInt()) stack_[stack_.size() - 2] = Value(leftRef.intVal * rightRef.intVal);
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() * rightRef.toDouble());
        break;
    case OP_DIV_INT:
        if (rightRef.toDouble() == 0.0) return runtimeError("除零错误");
        if (leftRef.isInt() && rightRef.isInt()) stack_[stack_.size() - 2] = Value(leftRef.intVal / rightRef.intVal);
        else stack_[stack_.size() - 2] = Value(leftRef.toDouble() / rightRef.toDouble());
        break;
    case OP_MOD_INT:
        if (!leftRef.isInt() || !rightRef.isInt()) return runtimeError("取模运算仅支持整数");
        if (rightRef.intVal == 0) return runtimeError("除零错误");
        stack_[stack_.size() - 2] = Value(leftRef.intVal % rightRef.intVal);
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
    mainChunk_ = BytecodeChunk();  // 清空主 chunk 副本
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
        const std::string& name = chunk.constants[idx].stringVal;
        Value val = pop();
        globals_[name] = val;
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    case OpCode::OP_GET_VAR: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        if (idx >= chunk.constants.size()) return runtimeError("常量池索引越界");
        const std::string& name = chunk.constants[idx].stringVal;
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
        const std::string& name = chunk.constants[idx].stringVal;
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

        // init 方法返回 this 实例而非 null
        if (retFrame.functionName.size() >= 5 &&
            retFrame.functionName.compare(retFrame.functionName.size() - 5, 5, ".init") == 0) {
            if (retFrame.basePointer < stack_.size()) {
                result = stack_[retFrame.basePointer];
            }
        }

        // 方法调用 writeBack：将方法内修改后的 this 字段写回全局变量
        if (retFrame.isMethodCall && !retFrame.receiverVarName.empty() &&
            retFrame.basePointer < stack_.size()) {
            Value& modifiedThis = stack_[retFrame.basePointer];

            // 先把方法内的字段槽（bp+1..N）同步回 this。
            // 栈布局：[this, field0, field1, ..., args]，
            // 方法内直接写 `field = x`（OP_SET_LOCAL）只改字段槽，this.fields 未变，
            // 必须先同步字段槽→this，否则 writeBack 读 this.fields 会丢失改动。
            if (modifiedThis.isInstance() && retFrame.chunk && !retFrame.chunk->fieldOrder.empty()) {
                for (size_t fi = 0; fi < retFrame.chunk->fieldOrder.size(); ++fi) {
                    size_t pos = retFrame.basePointer + 1 + fi;
                    if (pos < stack_.size()) {
                        modifiedThis.fields[retFrame.chunk->fieldOrder[fi]] = stack_[pos];
                    }
                }
            }

            if (modifiedThis.isInstance()) {
                auto it = globals_.find(retFrame.receiverVarName);
                if (it != globals_.end() && it->second.isInstance()) {
                    // 写回修改后的字段到 globals_ 中的原始实例
                    for (auto& field : modifiedThis.fields) {
                        it->second.fields[field.first] = field.second;
                    }
                }
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
        const std::string& funName = chunk.constants[idx].stringVal;

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

        // 检查是否有闭包捕获的变量需要注入到全局环境中
        // 在参数之后的栈位置查找闭包对象
        if (stack_.size() > argCount) {
            size_t closurePos = stack_.size() - argCount - 1;
            const Value& maybeClosure = stack_[closurePos];
            if (maybeClosure.isClosure() && maybeClosure.closureName == funName) {
                // 将闭包捕获的变量注入全局环境（仅注入当前不存在的变量）
                for (const auto& kv : maybeClosure.capturedVars) {
                    if (globals_.find(kv.first) == globals_.end()) {
                        globals_[kv.first] = kv.second;
                    }
                }
            }
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
            int i = idx.intVal;
            if (i >= 0 && i < static_cast<int>(obj.arrayVal.size())) {
                push(obj.arrayVal[i]);
            } else {
                return runtimeError("数组索引越界: " + std::to_string(i) + " (长度: " + std::to_string(obj.arrayVal.size()) + ")");
            }
        } else if (obj.isDict() && idx.isString()) {
            auto it = obj.dictVal.find(idx.stringVal);
            if (it != obj.dictVal.end()) {
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
        const std::string& varName = chunk.constants[idx].stringVal;
        Value val = pop();
        Value index = pop();
        auto it = globals_.find(varName);
        if (it != globals_.end()) {
            Value& obj = it->second;  // 引用，直接修改
            if (obj.isArray() && index.isInt()) {
                int i = index.intVal;
                if (i >= 0 && i < static_cast<int>(obj.arrayVal.size())) {
                    obj.arrayVal[i] = val;
                }
            } else if (obj.isDict() && index.isString()) {
                obj.dictVal[index.stringVal] = val;
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
                int i = index.intVal;
                if (i >= 0 && i < static_cast<int>(obj.arrayVal.size())) {
                    obj.arrayVal[i] = val;
                }
            } else if (obj.isDict() && index.isString()) {
                obj.dictVal[index.stringVal] = val;
            }
        }
        notifyStep(ip, op);
        ip += 2;
        break;
    }

    case OpCode::OP_MEMBER_GET: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        const std::string& fieldName = chunk.constants[idx].stringVal;
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
        // fallback 路径：用于非简单变量的嵌套成员赋值
        // Value 是值语义，pop 出来的是副本，修改副本无法写回原位置
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        const std::string& fieldName = chunk.constants[idx].stringVal;
        Value val = pop();
        Value obj = pop();
        (void)obj; (void)fieldName; (void)val;  // 消除未使用警告
        return runtimeError("VM 不支持嵌套成员赋值（如 arr[i].field = val），请使用临时变量");
    }

    case OpCode::OP_MEMBER_SET_VAR: {
        // 直接修改 globals_[varName].fields[fieldName]
        uint16_t varIdx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        uint16_t fieldIdx = chunk.code[ip + 3] | (chunk.code[ip + 4] << 8);
        const std::string& varName = chunk.constants[varIdx].stringVal;
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal;
        Value val = pop();
        auto it = globals_.find(varName);
        if (it != globals_.end()) {
            Value& obj = it->second;  // 引用，直接修改
            if (obj.isInstance()) {
                obj.fields[fieldName] = val;
            } else if (obj.isDict()) {
                obj.dictVal[fieldName] = val;
            }
        }
        notifyStep(ip, op);
        ip += 5;
        break;
    }

    case OpCode::OP_MEMBER_SET_LOCAL: {
        // 直接修改 stack_[bp+slot].fields[fieldName]（用于方法内 this.field = val）
        uint8_t slot = chunk.code[ip + 1];
        uint16_t fieldIdx = chunk.code[ip + 2] | (chunk.code[ip + 3] << 8);
        const std::string& fieldName = chunk.constants[fieldIdx].stringVal;
        Value val = pop();
        size_t bp = currentFrame().basePointer;
        if (bp + slot < stack_.size()) {
            Value& obj = stack_[bp + slot];  // 栈引用，直接修改
            if (obj.isInstance()) {
                obj.fields[fieldName] = val;
            } else if (obj.isDict()) {
                obj.dictVal[fieldName] = val;
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
        const std::string& methodName = chunk.constants[idx].stringVal;

        Value obj = peek(argCount);  // peek 返回 Value 拷贝

        if (obj.isInstance()) {
            std::string methodKey = obj.className + "." + methodName;
            auto it = functionChunks_.find(methodKey);
            if (it != functionChunks_.end()) {
                const BytecodeChunk& targetChunk = it->second;

                if (frames_.size() >= MAX_FRAMES) {
                    return runtimeError("调用栈溢出");
                }

                // 记录调用者栈上原始实例的位置（用于 writeBack）
                size_t callerPos = stack_.size() - argCount - 1;

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
                // 按编译器声明的字段顺序推入实例字段值
                int fieldCount = 0;
                if (targetChunk.fieldOrder.empty()) {
                    // 回退：按 unordered_map 顺序（不保证正确，但兼容旧字节码）
                    for (const auto& field : obj.fields) {
                        push(field.second);
                    }
                    fieldCount = static_cast<int>(obj.fields.size());
                } else {
                    for (const auto& fieldName : targetChunk.fieldOrder) {
                        auto fieldIt = obj.fields.find(fieldName);
                        if (fieldIt != obj.fields.end()) {
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

                VMCallFrame newFrame;
                newFrame.chunk = &targetChunk;
                newFrame.returnIp = ip + 6;  // OP_METHOD_CALL 现在是 6 字节
                newFrame.basePointer = stack_.size() - fieldCount - argCount - 1;
                newFrame.functionName = methodKey;
                newFrame.ip = 0;
                newFrame.isMethodCall = true;
                newFrame.callerInstancePos = callerPos;
                // 记录接收者变量名（用于 writeBack 到 globals_）
                if (receiverVarIdx > 0 && receiverVarIdx < chunk.constants.size()) {
                    newFrame.receiverVarName = chunk.constants[receiverVarIdx].stringVal;
                }
                frames_.push_back(newFrame);

                notifyStep(ip, op);
                break;
            }
        }

        for (uint8_t i = 0; i < argCount; ++i) pop();
        pop();
        push(Value::nullValue());
        notifyStep(ip, op);
        ip += 6;  // OP_METHOD_CALL 现在是 6 字节
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
        const std::string& funName = chunk.constants[idx].stringVal;
        Value closure = Value::makeClosure(funName, nullptr, {});
        // 捕获当前全局变量环境到闭包中
        closure.capturedVars = globals_;
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
        const std::string& className = chunk.constants[idx].stringVal;

        Value instance = Value::makeInstance(className);
        push(instance);

        // 检查是否有 init 方法
        std::string initKey = className + ".init";
        auto it = functionChunks_.find(initKey);

        if (it != functionChunks_.end()) {
            // 有 init 方法：创建新帧执行 init
            // init 的参数在栈上紧跟实例之后（如果有参数的话）
            // 但当前编译器总是 argCount=0，字段由 OP_INIT_FIELD 初始化
            // 这里处理两种情况：
            //   1. argCount > 0：有构造参数（未来扩展）
            //   2. argCount == 0：无参构造，init 可能做额外初始化逻辑

            if (frames_.size() >= MAX_FRAMES) {
                return runtimeError("调用栈溢出");
            }

            // 弹出多余参数（如果有）
            // 注意：当前编译路径 argCount 总是 0，但 OP_INIT_FIELD 会在
            // OP_CLASS_NEW 之后立即设置字段，所以 init 方法通过 OP_METHOD_CALL
            // 单独调用，而不是在 OP_CLASS_NEW 内部调用
            // 如果 argCount > 0，参数已在栈上，可以直接创建帧
            if (argCount > 0) {
                VMCallFrame newFrame;
                newFrame.chunk = &it->second;
                newFrame.returnIp = ip + 4;
                newFrame.basePointer = stack_.size() - argCount - 1;
                newFrame.functionName = initKey;
                newFrame.ip = 0;
                frames_.push_back(newFrame);
            } else {
                // 无参构造：弹出多余的参数（没有）
                // 不自动调用 init —— init 通过 OP_METHOD_CALL 调用
            }
        }

        // 弹出多余参数（如果 init 未被调用且有多余参数）
        if (argCount > 0 && (it == functionChunks_.end())) {
            for (uint8_t i = 0; i < argCount; ++i) pop();
        }

        notifyStep(ip, op);
        ip += 4;
        break;
    }

    case OpCode::OP_INIT_FIELD: {
        uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
        const std::string& fieldName = chunk.constants[idx].stringVal;
        Value val = pop();
        // 栈顶是实例（OP_CLASS_NEW 推入的），直接修改
        if (!stack_.empty() && stack_.back().isInstance()) {
            stack_.back().fields[fieldName] = val;
        }
        notifyStep(ip, op);
        ip += 3;
        break;
    }

    default:
        return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
    }

    return VMResult::VM_OK;
}
