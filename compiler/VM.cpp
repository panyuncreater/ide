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

VMResult VM::execute(const BytecodeChunk& chunk) {
    stack_.clear();
    globals_.clear();
    lastError_.clear();

    size_t ip = 0;  // 指令指针

    while (ip < chunk.code.size()) {
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
                // 继续评估右操作数（已编译为短路逻辑）
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
                // 继续评估右操作数
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

        case OpCode::OP_RETURN:
            notifyStep(ip, op);
            return VMResult::VM_OK;

        case OpCode::OP_CALL: {
            // 简化：函数调用在 VM 中暂不支持完整实现
            // 清理栈上的参数
            uint8_t argCount = chunk.code[ip + 3];
            for (uint8_t i = 0; i < argCount; ++i) {
                pop();
            }
            pop(); // 弹出函数名
            push(Value::nullValue());
            notifyStep(ip, op);
            ip += 4;
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
            // 简化处理：由于 Value 是值类型，索引赋值需要修改变量
            // VM 中暂不支持完整实现
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
            // 简化处理
            push(val);
            notifyStep(ip, op);
            ip += 3;
            break;
        }

        case OpCode::OP_METHOD_CALL: {
            uint16_t idx = chunk.code[ip + 1] | (chunk.code[ip + 2] << 8);
            uint8_t argCount = chunk.code[ip + 3];
            // 清理栈
            for (uint8_t i = 0; i < argCount; ++i) pop();
            pop(); // 弹出对象
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

        default:
            return runtimeError("未知操作码: " + std::to_string(static_cast<int>(op)));
        }
    }

    return VMResult::VM_OK;
}
