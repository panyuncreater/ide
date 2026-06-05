#include "compiler/Compiler.h"
#include <sstream>

// ============================================================
// Compiler 字节码编译器实现
// ============================================================

Compiler::Compiler() {}

CompileResult Compiler::compile(Block& program) {
    chunk_ = BytecodeChunk();
    chunk_.name = "main";
    chunk_.arity = 0;
    varIndex_.clear();
    lastError_.clear();
    functionChunks_.clear();
    currentLocals_.clear();
    inFunction_ = false;

    // 编译所有顶层语句
    compileBlock(program);

    // 末尾添加 RETURN
    chunk_.writeOp(OpCode::OP_RETURN, 0);

    CompileResult result;
    result.mainChunk = std::move(chunk_);
    result.functionChunks = std::move(functionChunks_);
    return result;
}

std::string Compiler::getLastError() const {
    return lastError_;
}

uint16_t Compiler::identifierIndex(const std::string& name) {
    auto it = varIndex_.find(name);
    if (it != varIndex_.end()) return it->second;
    uint16_t idx = chunk_.addConstant(Value(name));
    varIndex_[name] = idx;
    return idx;
}

void Compiler::compileNode(ASTNode* node) {
    if (!node) return;

    // 根据节点类型分发
    if (auto* n = dynamic_cast<BinaryOp*>(node)) { compileBinaryOp(*n); return; }
    if (auto* n = dynamic_cast<UnaryOp*>(node)) { compileUnaryOp(*n); return; }
    if (auto* n = dynamic_cast<NumberLiteral*>(node)) { compileNumberLiteral(*n); return; }
    if (auto* n = dynamic_cast<StringLiteral*>(node)) { compileStringLiteral(*n); return; }
    if (auto* n = dynamic_cast<BoolLiteral*>(node)) { compileBoolLiteral(*n); return; }
    if (auto* n = dynamic_cast<VarDecl*>(node)) { compileVarDecl(*n); return; }
    if (auto* n = dynamic_cast<Assignment*>(node)) { compileAssignment(*n); return; }
    if (auto* n = dynamic_cast<VarRef*>(node)) { compileVarRef(*n); return; }
    if (auto* n = dynamic_cast<IfStmt*>(node)) { compileIfStmt(*n); return; }
    if (auto* n = dynamic_cast<WhileStmt*>(node)) { compileWhileStmt(*n); return; }
    if (auto* n = dynamic_cast<ForStmt*>(node)) { compileForStmt(*n); return; }
    if (auto* n = dynamic_cast<FunDecl*>(node)) { compileFunDecl(*n); return; }
    if (auto* n = dynamic_cast<FunCall*>(node)) { compileFunCall(*n); return; }
    if (auto* n = dynamic_cast<ReturnStmt*>(node)) { compileReturnStmt(*n); return; }
    if (auto* n = dynamic_cast<PrintStmt*>(node)) { compilePrintStmt(*n); return; }
    if (auto* n = dynamic_cast<Block*>(node)) { compileBlock(*n); return; }
    if (auto* n = dynamic_cast<ArrayLiteral*>(node)) { compileArrayLiteral(*n); return; }
    if (auto* n = dynamic_cast<DictLiteral*>(node)) { compileDictLiteral(*n); return; }
    if (auto* n = dynamic_cast<IndexAccess*>(node)) { compileIndexAccess(*n); return; }
    if (auto* n = dynamic_cast<IndexAssign*>(node)) { compileIndexAssign(*n); return; }
    if (auto* n = dynamic_cast<ClassDecl*>(node)) { compileClassDecl(*n); return; }
    if (auto* n = dynamic_cast<MemberAccess*>(node)) { compileMemberAccess(*n); return; }
    if (auto* n = dynamic_cast<MemberAssign*>(node)) { compileMemberAssign(*n); return; }
    if (auto* n = dynamic_cast<MethodCall*>(node)) { compileMethodCall(*n); return; }
    if (auto* n = dynamic_cast<NullLiteral*>(node)) { compileNullLiteral(*n); return; }
}

void Compiler::compileBinaryOp(BinaryOp& node) {
    // 短路运算特殊处理
    if (node.op == "and") {
        compileNode(node.left.get());
        // 如果左操作数为假，跳过右操作数
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);  // 占位
        chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出左操作数
        compileNode(node.right.get());
        // 修补跳转地址
        uint16_t jumpTarget = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(jumpTarget & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((jumpTarget >> 8) & 0xFF);
        return;
    }

    if (node.op == "or") {
        compileNode(node.left.get());
        // 如果左操作数为真，跳过右操作数
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);  // 占位（先跳到 or 右侧）
        // 如果到这里，左操作数为真
        chunk_.writeOp(OpCode::OP_POP, node.line);
        chunk_.writeOp(OpCode::OP_TRUE, node.line);
        size_t jumpEnd = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);  // 占位
        // 修补第一个跳转
        uint16_t rightStart = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(rightStart & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((rightStart >> 8) & 0xFF);
        // 编译右操作数
        chunk_.writeOp(OpCode::OP_POP, node.line);
        compileNode(node.right.get());
        // 修补第二个跳转
        uint16_t endTarget = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[jumpEnd + 1] = static_cast<uint8_t>(endTarget & 0xFF);
        chunk_.code[jumpEnd + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
        return;
    }

    // 普通二元运算
    compileNode(node.left.get());
    compileNode(node.right.get());

    OpCode op = OpCode::OP_ADD;
    if (node.op == "+") op = OpCode::OP_ADD;
    else if (node.op == "-") op = OpCode::OP_SUBTRACT;
    else if (node.op == "*") op = OpCode::OP_MULTIPLY;
    else if (node.op == "/") op = OpCode::OP_DIVIDE;
    else if (node.op == "%") op = OpCode::OP_MODULO;
    else if (node.op == "==") op = OpCode::OP_EQUAL;
    else if (node.op == "!=") op = OpCode::OP_NOT_EQUAL;
    else if (node.op == "<") op = OpCode::OP_LESS;
    else if (node.op == ">") op = OpCode::OP_GREATER;
    else if (node.op == "<=") op = OpCode::OP_LESS_EQUAL;
    else if (node.op == ">=") op = OpCode::OP_GREATER_EQUAL;
    else {
        error("不支持的运算符: " + node.op, node.line, node.column);
        return;
    }

    chunk_.writeOp(op, node.line);
}

void Compiler::compileUnaryOp(UnaryOp& node) {
    compileNode(node.operand.get());
    if (node.op == "-") {
        chunk_.writeOp(OpCode::OP_NEGATE, node.line);
    } else if (node.op == "not") {
        chunk_.writeOp(OpCode::OP_NOT, node.line);
    }
}

void Compiler::compileNumberLiteral(NumberLiteral& node) {
    uint16_t idx = chunk_.addConstant(node.value);
    if (node.value.isInt()) {
        chunk_.writeOp(OpCode::OP_INT, node.line);
    } else {
        chunk_.writeOp(OpCode::OP_FLOAT, node.line);
    }
    chunk_.writeShort(idx, node.line);
}

void Compiler::compileStringLiteral(StringLiteral& node) {
    uint16_t idx = chunk_.addConstant(Value(node.value));
    chunk_.writeOp(OpCode::OP_STRING, node.line);
    chunk_.writeShort(idx, node.line);
}

void Compiler::compileBoolLiteral(BoolLiteral& node) {
    chunk_.writeOp(node.value ? OpCode::OP_TRUE : OpCode::OP_FALSE, node.line);
}

void Compiler::compileVarDecl(VarDecl& node) {
    // 编译初始化表达式
    if (node.initializer) {
        compileNode(node.initializer.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }

    // 在函数体内使用局部变量
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it == currentLocals_.end()) {
            // 新局部变量，分配槽位
            int slot = static_cast<int>(currentLocals_.size());
            currentLocals_[node.name] = slot;
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(slot), node.line);
        } else {
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
        }
    } else {
        uint16_t nameIdx = identifierIndex(node.name);
        chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
    }
}

void Compiler::compileAssignment(Assignment& node) {
    compileNode(node.value.get());

    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            chunk_.writeOp(OpCode::OP_SET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
        } else {
            uint16_t nameIdx = identifierIndex(node.name);
            chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    } else {
        uint16_t nameIdx = identifierIndex(node.name);
        chunk_.writeOp(OpCode::OP_SET_VAR, node.line);
        chunk_.writeShort(nameIdx, node.line);
    }
}

void Compiler::compileVarRef(VarRef& node) {
    if (inFunction_) {
        auto it = currentLocals_.find(node.name);
        if (it != currentLocals_.end()) {
            chunk_.writeOp(OpCode::OP_GET_LOCAL, node.line);
            chunk_.write(static_cast<uint8_t>(it->second), node.line);
            return;
        }
    }
    uint16_t nameIdx = identifierIndex(node.name);
    chunk_.writeOp(OpCode::OP_GET_VAR, node.line);
    chunk_.writeShort(nameIdx, node.line);
}

void Compiler::compileIfStmt(IfStmt& node) {
    compileNode(node.condition.get());

    // 条件为假跳转到 else 分支
    size_t elseJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    // 编译 then 分支
    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值
    compileNode(node.thenBranch.get());

    // 跳过 else 分支
    size_t endJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP, node.line);
    chunk_.writeShort(0, node.line);

    // 修补 else 跳转
    uint16_t elseStart = static_cast<uint16_t>(chunk_.code.size());
    chunk_.code[elseJumpPatch + 1] = static_cast<uint8_t>(elseStart & 0xFF);
    chunk_.code[elseJumpPatch + 2] = static_cast<uint8_t>((elseStart >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值

    // 编译 else 分支
    if (node.elseBranch) {
        compileNode(node.elseBranch.get());
    }

    // 修补 end 跳转
    uint16_t endTarget = static_cast<uint16_t>(chunk_.code.size());
    chunk_.code[endJumpPatch + 1] = static_cast<uint8_t>(endTarget & 0xFF);
    chunk_.code[endJumpPatch + 2] = static_cast<uint8_t>((endTarget >> 8) & 0xFF);
}

void Compiler::compileWhileStmt(WhileStmt& node) {
    size_t loopStart = chunk_.code.size();

    // 编译条件
    compileNode(node.condition.get());

    // 条件为假跳出循环
    size_t exitJumpPatch = chunk_.code.size();
    chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
    chunk_.writeShort(0, node.line);

    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值

    // 编译循环体
    compileNode(node.body.get());

    // 回跳到条件检查
    uint16_t loopOffset = static_cast<uint16_t>(loopStart);
    chunk_.writeOp(OpCode::OP_LOOP, node.line);
    chunk_.writeShort(loopOffset, node.line);

    // 修补退出跳转
    uint16_t exitTarget = static_cast<uint16_t>(chunk_.code.size());
    chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
    chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);

    chunk_.writeOp(OpCode::OP_POP, node.line);  // 弹出条件值
}

void Compiler::compileForStmt(ForStmt& node) {
    // 编译初始化
    if (node.initializer) {
        compileNode(node.initializer.get());
    }

    size_t loopStart = chunk_.code.size();

    // 编译条件
    if (node.condition) {
        compileNode(node.condition.get());
        size_t exitJumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);

        // 编译循环体
        compileNode(node.body.get());

        // 编译更新（OP_SET_VAR 不再 push 值，无需额外 OP_POP）
        if (node.update) {
            compileNode(node.update.get());
        }

        // 回跳
        chunk_.writeOp(OpCode::OP_LOOP, node.line);
        chunk_.writeShort(static_cast<uint16_t>(loopStart), node.line);

        // 修补退出跳转
        uint16_t exitTarget = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
        chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);
        chunk_.writeOp(OpCode::OP_POP, node.line);
    } else {
        // 无条件循环（while true）
        chunk_.writeOp(OpCode::OP_TRUE, node.line);
        size_t exitJumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);
        chunk_.writeOp(OpCode::OP_POP, node.line);

        compileNode(node.body.get());

        // 编译更新（OP_SET_VAR 不再 push 值，无需额外 OP_POP）
        if (node.update) {
            compileNode(node.update.get());
        }

        chunk_.writeOp(OpCode::OP_LOOP, node.line);
        chunk_.writeShort(static_cast<uint16_t>(loopStart), node.line);

        uint16_t exitTarget = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[exitJumpPatch + 1] = static_cast<uint8_t>(exitTarget & 0xFF);
        chunk_.code[exitJumpPatch + 2] = static_cast<uint8_t>((exitTarget >> 8) & 0xFF);
        chunk_.writeOp(OpCode::OP_POP, node.line);
    }
}

void Compiler::compileFunDecl(FunDecl& node) {
    // 为函数体创建独立 BytecodeChunk
    BytecodeChunk savedChunk = std::move(chunk_);
    std::unordered_map<std::string, uint16_t> savedVarIndex = varIndex_;
    std::unordered_map<std::string, int> savedLocals = currentLocals_;
    bool savedInFunction = inFunction_;

    // 设置函数编译上下文
    chunk_ = BytecodeChunk(node.name, static_cast<int>(node.params.size()));
    varIndex_.clear();
    currentLocals_.clear();
    inFunction_ = true;

    // 编译参数到局部变量槽位
    for (int i = 0; i < static_cast<int>(node.params.size()); ++i) {
        currentLocals_[node.params[i]] = i;
    }

    // 编译函数体
    if (node.body) {
        compileNode(node.body.get());
    }

    // 末尾添加隐式返回 null
    chunk_.writeOp(OpCode::OP_NULL, node.line);
    chunk_.writeOp(OpCode::OP_RETURN, node.line);

    // 存储函数 chunk
    functionChunks_[node.name] = std::move(chunk_);

    // 恢复主 chunk
    chunk_ = std::move(savedChunk);
    varIndex_ = savedVarIndex;
    currentLocals_ = savedLocals;
    inFunction_ = savedInFunction;

    // 在主 chunk 中 emit OP_CLOSURE
    uint16_t nameIdx = identifierIndex(node.name);
    chunk_.writeOp(OpCode::OP_CLOSURE, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.params.size()), node.line);
    // OP_CALL 通过函数名查找，不需要栈上的闭包值，弹出
    chunk_.writeOp(OpCode::OP_POP, node.line);
}

void Compiler::compileFunCall(FunCall& node) {
    // 编译参数
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }

    // 函数名作为常量
    uint16_t nameIdx = identifierIndex(node.name);

    // 使用 OP_CALL 指令
    chunk_.writeOp(OpCode::OP_CALL, node.line);
    chunk_.write(static_cast<uint8_t>(nameIdx & 0xFF), node.line);
    chunk_.write(static_cast<uint8_t>((nameIdx >> 8) & 0xFF), node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
}

void Compiler::compileReturnStmt(ReturnStmt& node) {
    if (node.value) {
        compileNode(node.value.get());
    } else {
        chunk_.writeOp(OpCode::OP_NULL, node.line);
    }
    chunk_.writeOp(OpCode::OP_RETURN, node.line);
}

void Compiler::compilePrintStmt(PrintStmt& node) {
    for (auto& val : node.values) {
        compileNode(val.get());
        chunk_.writeOp(OpCode::OP_PRINT, node.line);
    }
}

void Compiler::compileBlock(Block& node) {
    for (auto& stmt : node.statements) {
        compileNode(stmt.get());
    }
}

// ---- 新增节点编译 ----

void Compiler::compileArrayLiteral(ArrayLiteral& node) {
    // 编译所有元素
    for (auto& elem : node.elements) {
        compileNode(elem.get());
    }
    // 构建数组指令
    chunk_.writeOp(OpCode::OP_BUILD_ARRAY, node.line);
    chunk_.write(static_cast<uint8_t>(node.elements.size()), node.line);
}

void Compiler::compileDictLiteral(DictLiteral& node) {
    // 编译所有键值对
    for (auto& pair : node.pairs) {
        compileNode(pair.first.get());
        compileNode(pair.second.get());
    }
    // 字典构建暂用常量池
    // 简化：把字典字面量作为常量存入
    // 这里先输出空操作
    for (size_t i = 0; i < node.pairs.size(); ++i) {
        chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出值
        chunk_.writeOp(OpCode::OP_POP, node.line); // 弹出键
    }
    chunk_.writeOp(OpCode::OP_NULL, node.line);
}

void Compiler::compileIndexAccess(IndexAccess& node) {
    compileNode(node.object.get());
    compileNode(node.index.get());
    chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);
}

void Compiler::compileIndexAssign(IndexAssign& node) {
    compileNode(node.object.get());
    compileNode(node.index.get());
    compileNode(node.value.get());
    chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
}

void Compiler::compileClassDecl(ClassDecl& node) {
    // 类声明：在主 chunk 中 emit OP_CLASS_NEW 占位
    identifierIndex(node.name);
    // 简化：编译类成员方法为独立 chunk
    for (auto& member : node.members) {
        FunDecl* funDecl = dynamic_cast<FunDecl*>(member.get());
        if (funDecl) {
            // 编译方法为独立 chunk（方法名用 ClassName.methodName）
            BytecodeChunk savedChunk = std::move(chunk_);
            std::unordered_map<std::string, uint16_t> savedVarIndex = varIndex_;
            std::unordered_map<std::string, int> savedLocals = currentLocals_;
            bool savedInFunction = inFunction_;

            std::string methodKey = node.name + "." + funDecl->name;
            chunk_ = BytecodeChunk(methodKey, static_cast<int>(funDecl->params.size()));
            varIndex_.clear();
            currentLocals_.clear();
            inFunction_ = true;

            // 编译参数到局部变量槽位
            for (int i = 0; i < static_cast<int>(funDecl->params.size()); ++i) {
                currentLocals_[funDecl->params[i]] = i;
            }

            // 编译方法体
            if (funDecl->body) {
                compileNode(funDecl->body.get());
            }

            // 隐式返回 null
            chunk_.writeOp(OpCode::OP_NULL, funDecl->line);
            chunk_.writeOp(OpCode::OP_RETURN, funDecl->line);

            functionChunks_[methodKey] = std::move(chunk_);

            // 恢复
            chunk_ = std::move(savedChunk);
            varIndex_ = savedVarIndex;
            currentLocals_ = savedLocals;
            inFunction_ = savedInFunction;
        }
    }
}

void Compiler::compileMemberAccess(MemberAccess& node) {
    compileNode(node.object.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    chunk_.writeOp(OpCode::OP_MEMBER_GET, node.line);
    chunk_.writeShort(nameIdx, node.line);
}

void Compiler::compileMemberAssign(MemberAssign& node) {
    compileNode(node.object.get());
    compileNode(node.value.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
    chunk_.writeShort(nameIdx, node.line);
}

void Compiler::compileMethodCall(MethodCall& node) {
    compileNode(node.object.get());
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }
    uint16_t nameIdx = identifierIndex(node.methodName);
    chunk_.writeOp(OpCode::OP_METHOD_CALL, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
}

void Compiler::compileNullLiteral(NullLiteral& node) {
    chunk_.writeOp(OpCode::OP_NULL, node.line);
}

void Compiler::error(const std::string& msg, int line, int col) {
    std::ostringstream oss;
    oss << "编译错误 (行 " << line << ", 列 " << col << "): " << msg;
    lastError_ = oss.str();
}
