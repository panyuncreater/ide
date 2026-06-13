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

    // 预计算 ip→指令索引映射（用于调试高亮 O(1) 查找）
    result.mainChunk.buildIpMap();
    for (auto& kv : result.functionChunks) {
        kv.second.buildIpMap();
    }

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

    // 根据 NodeType 枚举快速分发（替代 24 次 dynamic_cast）
    switch (node->nodeType) {
    case NodeType::NODE_BINARY_OP:     compileBinaryOp(*static_cast<BinaryOp*>(node)); return;
    case NodeType::NODE_UNARY_OP:      compileUnaryOp(*static_cast<UnaryOp*>(node)); return;
    case NodeType::NODE_NUMBER_LITERAL: compileNumberLiteral(*static_cast<NumberLiteral*>(node)); return;
    case NodeType::NODE_STRING_LITERAL: compileStringLiteral(*static_cast<StringLiteral*>(node)); return;
    case NodeType::NODE_BOOL_LITERAL:   compileBoolLiteral(*static_cast<BoolLiteral*>(node)); return;
    case NodeType::NODE_VAR_DECL:       compileVarDecl(*static_cast<VarDecl*>(node)); return;
    case NodeType::NODE_ASSIGNMENT:     compileAssignment(*static_cast<Assignment*>(node)); return;
    case NodeType::NODE_VAR_REF:       compileVarRef(*static_cast<VarRef*>(node)); return;
    case NodeType::NODE_IF_STMT:        compileIfStmt(*static_cast<IfStmt*>(node)); return;
    case NodeType::NODE_WHILE_STMT:     compileWhileStmt(*static_cast<WhileStmt*>(node)); return;
    case NodeType::NODE_FOR_STMT:       compileForStmt(*static_cast<ForStmt*>(node)); return;
    case NodeType::NODE_FUN_DECL:       compileFunDecl(*static_cast<FunDecl*>(node)); return;
    case NodeType::NODE_FUN_CALL:       compileFunCall(*static_cast<FunCall*>(node)); return;
    case NodeType::NODE_RETURN_STMT:    compileReturnStmt(*static_cast<ReturnStmt*>(node)); return;
    case NodeType::NODE_PRINT_STMT:     compilePrintStmt(*static_cast<PrintStmt*>(node)); return;
    case NodeType::NODE_BLOCK:          compileBlock(*static_cast<Block*>(node)); return;
    case NodeType::NODE_ARRAY_LITERAL:  compileArrayLiteral(*static_cast<ArrayLiteral*>(node)); return;
    case NodeType::NODE_DICT_LITERAL:   compileDictLiteral(*static_cast<DictLiteral*>(node)); return;
    case NodeType::NODE_INDEX_ACCESS:   compileIndexAccess(*static_cast<IndexAccess*>(node)); return;
    case NodeType::NODE_INDEX_ASSIGN:   compileIndexAssign(*static_cast<IndexAssign*>(node)); return;
    case NodeType::NODE_CLASS_DECL:     compileClassDecl(*static_cast<ClassDecl*>(node)); return;
    case NodeType::NODE_MEMBER_ACCESS: compileMemberAccess(*static_cast<MemberAccess*>(node)); return;
    case NodeType::NODE_MEMBER_ASSIGN: compileMemberAssign(*static_cast<MemberAssign*>(node)); return;
    case NodeType::NODE_METHOD_CALL:    compileMethodCall(*static_cast<MethodCall*>(node)); return;
    case NodeType::NODE_NULL_LITERAL:   compileNullLiteral(*static_cast<NullLiteral*>(node)); return;
    }
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
        // 如果左操作数为真，跳过右操作数，保留左值
        size_t jumpPatch = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP_IF_FALSE, node.line);
        chunk_.writeShort(0, node.line);  // 占位（先跳到 or 右侧）
        // 如果到这里，左操作数为真 — 保留左值，跳到末尾
        size_t jumpEnd = chunk_.code.size();
        chunk_.writeOp(OpCode::OP_JUMP, node.line);
        chunk_.writeShort(0, node.line);  // 占位
        // 修补第一个跳转：左操作数为假，求值右操作数
        uint16_t rightStart = static_cast<uint16_t>(chunk_.code.size());
        chunk_.code[jumpPatch + 1] = static_cast<uint8_t>(rightStart & 0xFF);
        chunk_.code[jumpPatch + 2] = static_cast<uint8_t>((rightStart >> 8) & 0xFF);
        // 弹出左操作数，求值右操作数
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
    // 检查元素数量上限（uint8_t 编码限制）
    if (node.elements.size() > 255) {
        error("数组元素数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有元素
    for (auto& elem : node.elements) {
        compileNode(elem.get());
    }
    // 构建数组指令
    chunk_.writeOp(OpCode::OP_BUILD_ARRAY, node.line);
    chunk_.write(static_cast<uint8_t>(node.elements.size()), node.line);
}

void Compiler::compileDictLiteral(DictLiteral& node) {
    // 检查键值对数量上限（uint8_t 编码限制）
    if (node.pairs.size() > 255) {
        error("字典键值对数量超过 255 个上限", node.line, node.column);
        return;
    }
    // 编译所有键值对（先键后值，与 OP_BUILD_DICT 消费顺序一致）
    for (auto& pair : node.pairs) {
        compileNode(pair.first.get());
        compileNode(pair.second.get());
    }
    // 构建字典指令：弹出 2*count 个值，构建字典，push 到栈
    chunk_.writeOp(OpCode::OP_BUILD_DICT, node.line);
    chunk_.write(static_cast<uint8_t>(node.pairs.size()), node.line);
}

void Compiler::compileIndexAccess(IndexAccess& node) {
    compileNode(node.object.get());
    compileNode(node.index.get());
    chunk_.writeOp(OpCode::OP_INDEX_GET, node.line);
}

void Compiler::compileIndexAssign(IndexAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                     ? static_cast<VarRef*>(node.object.get()) : nullptr;

    if (objVar) {
        // 先查局部变量（如方法内的 this）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            // 局部变量索引赋值：this.arr[i] = val → OP_INDEX_SET_LOCAL
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            chunk_.writeOp(OpCode::OP_INDEX_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
        } else {
            // 全局变量索引赋值：arr[i] = val → OP_INDEX_SET_VAR
            compileNode(node.index.get());
            compileNode(node.value.get());
            uint16_t nameIdx = identifierIndex(objVar->name);
            chunk_.writeOp(OpCode::OP_INDEX_SET_VAR, node.line);
            chunk_.writeShort(nameIdx, node.line);
        }
    } else {
        // 通用路径（嵌套访问等）：推对象+索引+值，OP_INDEX_SET 不做写回（限制但安全）
        compileNode(node.object.get());
        compileNode(node.index.get());
        compileNode(node.value.get());
        chunk_.writeOp(OpCode::OP_INDEX_SET, node.line);
    }
}

void Compiler::compileClassDecl(ClassDecl& node) {
    // 类声明：发射 OP_CLASS_NEW + OP_INIT_FIELD 初始化字段 + 编译方法
    uint16_t nameIdx = identifierIndex(node.name);

    // 先编译所有方法为独立 chunk（方法不依赖字段初始化顺序）
    // 同时收集字段名列表，用于方法的局部变量映射
    std::vector<std::string> fieldNames;
    for (auto& member : node.members) {
        if (member->nodeType == NodeType::NODE_VAR_DECL) {
            fieldNames.push_back(static_cast<VarDecl*>(member.get())->name);
        }
    }

    for (auto& member : node.members) {
        if (member->nodeType != NodeType::NODE_FUN_DECL) continue;
        FunDecl* funDecl = static_cast<FunDecl*>(member.get());
        std::string methodKey = node.name + "." + funDecl->name;
        BytecodeChunk savedChunk = std::move(chunk_);
        std::unordered_map<std::string, uint16_t> savedVarIndex = varIndex_;
        std::unordered_map<std::string, int> savedLocals = currentLocals_;
        bool savedInFunction = inFunction_;

        chunk_ = BytecodeChunk(methodKey, static_cast<int>(funDecl->params.size()));
        varIndex_.clear();
        currentLocals_.clear();
        inFunction_ = true;

        // 局部变量映射：slot 0 = this，slot 1..N = 字段，slot N+1.. = 参数
        int slot = 0;
        currentLocals_["this"] = slot++;  // slot 0: this
        for (const auto& fieldName : fieldNames) {
            currentLocals_[fieldName] = slot++;  // slot 1..N: 实例字段
        }
        for (int i = 0; i < static_cast<int>(funDecl->params.size()); ++i) {
            currentLocals_[funDecl->params[i]] = slot++;  // slot N+1..: 参数
        }

        // 记录字段声明顺序，供 VM OP_METHOD_CALL 按序推入
        chunk_.fieldOrder = fieldNames;

        if (funDecl->body) {
            compileNode(funDecl->body.get());
        }

        chunk_.writeOp(OpCode::OP_NULL, funDecl->line);
        chunk_.writeOp(OpCode::OP_RETURN, funDecl->line);

        functionChunks_[methodKey] = std::move(chunk_);

        chunk_ = std::move(savedChunk);
        varIndex_ = savedVarIndex;
        currentLocals_ = savedLocals;
        inFunction_ = savedInFunction;
    }

    // 主 chunk 中：发射 OP_CLASS_NEW（0 参数构造，字段由 OP_INIT_FIELD 设置）
    chunk_.writeOp(OpCode::OP_CLASS_NEW, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(0), node.line);  // argCount = 0（字段由 OP_INIT_FIELD 初始化）

    // 此时栈顶是刚创建的空实例，发射 OP_INIT_FIELD 设置每个字段的默认值
    for (auto& member : node.members) {
        if (member->nodeType != NodeType::NODE_VAR_DECL) continue;
        VarDecl* varDecl = static_cast<VarDecl*>(member.get());
        // 编译字段默认值表达式
        if (varDecl->initializer) {
            compileNode(varDecl->initializer.get());
        } else {
            chunk_.writeOp(OpCode::OP_NULL, varDecl->line);
        }
        // 发射 OP_INIT_FIELD：pop 默认值，设置到栈顶实例的字段中
        uint16_t fieldIdx = identifierIndex(varDecl->name);
        chunk_.writeOp(OpCode::OP_INIT_FIELD, varDecl->line);
        chunk_.writeShort(fieldIdx, varDecl->line);
    }

    // 将实例注册为全局变量（OP_DEFINE_VAR 从栈上 pop 值并注册到 globals_）
    chunk_.writeOp(OpCode::OP_DEFINE_VAR, node.line);
    chunk_.writeShort(nameIdx, node.line);

    // 栈上的实例已被 OP_DEFINE_VAR 消费，无需额外 OP_POP
}

void Compiler::compileMemberAccess(MemberAccess& node) {
    compileNode(node.object.get());
    uint16_t nameIdx = identifierIndex(node.fieldName);
    chunk_.writeOp(OpCode::OP_MEMBER_GET, node.line);
    chunk_.writeShort(nameIdx, node.line);
}

void Compiler::compileMemberAssign(MemberAssign& node) {
    // 根据左值对象类型选择赋值策略
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                     ? static_cast<VarRef*>(node.object.get()) : nullptr;

    if (objVar) {
        // 先查局部变量（如方法内的 this）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt != currentLocals_.end()) {
            // 局部变量成员赋值：this.field = val → OP_MEMBER_SET_LOCAL
            compileNode(node.value.get());
            uint8_t slot = static_cast<uint8_t>(localIt->second);
            uint16_t fieldIdx = identifierIndex(node.fieldName);
            chunk_.writeOp(OpCode::OP_MEMBER_SET_LOCAL, node.line);
            chunk_.write(slot, node.line);
            chunk_.writeShort(fieldIdx, node.line);
            return;
        }
        // 全局变量成员赋值：obj.field = val → OP_MEMBER_SET_VAR
        compileNode(node.value.get());
        uint16_t varIdx = identifierIndex(objVar->name);
        uint16_t fieldIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET_VAR, node.line);
        chunk_.writeShort(varIdx, node.line);
        chunk_.writeShort(fieldIdx, node.line);
    } else {
        // 通用路径：推对象+值，OP_MEMBER_SET 不做写回
        compileNode(node.object.get());
        compileNode(node.value.get());
        uint16_t nameIdx = identifierIndex(node.fieldName);
        chunk_.writeOp(OpCode::OP_MEMBER_SET, node.line);
        chunk_.writeShort(nameIdx, node.line);
    }
}

void Compiler::compileMethodCall(MethodCall& node) {
    // 检查接收者是否为简单变量（VarRef），用于 writeBack
    uint16_t receiverVarIdx = 0;  // 0 = 非简单变量
    VarRef* objVar = (node.object && node.object->nodeType == NodeType::NODE_VAR_REF)
                     ? static_cast<VarRef*>(node.object.get()) : nullptr;
    if (objVar) {
        // 仅全局变量需要 writeBack（局部变量的 writeBack 通过栈引用处理）
        auto localIt = currentLocals_.find(objVar->name);
        if (localIt == currentLocals_.end()) {
            // 全局变量：记录变量名索引供 VM writeBack 使用
            receiverVarIdx = identifierIndex(objVar->name);
        }
    }

    compileNode(node.object.get());
    for (auto& arg : node.arguments) {
        compileNode(arg.get());
    }
    uint16_t nameIdx = identifierIndex(node.methodName);
    chunk_.writeOp(OpCode::OP_METHOD_CALL, node.line);
    chunk_.writeShort(nameIdx, node.line);
    chunk_.write(static_cast<uint8_t>(node.arguments.size()), node.line);
    chunk_.writeShort(receiverVarIdx, node.line);  // 接收者变量名索引（0 = 无 writeBack）
}

void Compiler::compileNullLiteral(NullLiteral& node) {
    chunk_.writeOp(OpCode::OP_NULL, node.line);
}

void Compiler::error(const std::string& msg, int line, int col) {
    std::ostringstream oss;
    oss << "编译错误 (行 " << line << ", 列 " << col << "): " << msg;
    lastError_ = oss.str();
}
