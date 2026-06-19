#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "ast/ASTNode.h"
#include "compiler/Bytecode.h"
#include "Diagnostic.h"

// ============================================================
// Compiler 字节码编译器
// ============================================================

/// 将 AST 编译为字节码
class Compiler {
public:
    Compiler();

    /// 编译 AST 块到字节码
    CompileResult compile(Block& program);

    /// 获取编译错误信息
    std::string getLastError() const;

    /// 获取编译过程中的诊断信息
    const DiagnosticBag& getDiagnostics() const { return diagnostics_; }

private:
    BytecodeChunk chunk_;                           // 当前字节码块
    std::unordered_map<std::string, uint16_t> varIndex_;  // 变量名 → 常量池索引
    std::string lastError_;                         // 最近一次编译错误
    DiagnosticBag diagnostics_;                      // 诊断收集器
    std::unordered_map<std::string, BytecodeChunk> functionChunks_;  // 函数字节码块
    std::unordered_map<std::string, int> currentLocals_;  // 当前函数的局部变量槽位映射
    bool inFunction_ = false;                       // 是否在函数体内
    std::unordered_map<std::string, std::vector<std::string>> classFieldNames_;  // 类名 → 字段名列表（含继承字段）
    std::unordered_map<std::string, int> outerLocals_;  // 外层函数的局部变量（用于检测闭包捕获）
    int writebackCounter_ = 0;  // B6: 写回计数器，生成唯一缓存变量名避免索引重复求值
    int peakLocals_ = 0;        // VMBUG-2: 函数编译期间局部变量槽位峰值（含被块作用域回收的变量）
    int blockDepth_ = 0;        // VMBUG-1: 顶层块作用域嵌套深度（仅在 inFunction_==false 时有效）
    int blockSaveCounter_ = 0;  // L11 fix: 块作用域保存计数器（成员变量，编译间重置）
    std::string currentClassName_;  // B1 fix: 当前正在编译的类名（供 OP_SUPER_CALL 编码类上下文）
    std::unordered_set<std::string> topLevelGlobals_;  // VMBUG-1: 顶层（非块/非函数）var 声明的全局变量名集合

    // A2: 全局变量整数槽位管理
    std::unordered_map<std::string, int> globalSlots_;  // name -> slot index
    std::vector<std::string> slotNames_;                 // slot -> name (parallel array)
    std::vector<int> freeSlots_;                         // recycled slot indices

    /// 添加变量名到常量池，返回索引
    uint16_t identifierIndex(const std::string& name);

    /// A2: 分配全局槽位（已有则返回现有，否则从 freeSlots_ 或新分配）
    int allocateGlobalSlot(const std::string& name);

    /// A2: 释放全局槽位（从 globalSlots_ 移除，推入 freeSlots_）
    void releaseGlobalSlot(const std::string& name);

    /// A2: 查找全局槽位（未找到返回 -1）
    int lookupGlobalSlot(const std::string& name) const;

    /// 编译 AST 节点
    void compileNode(ASTNode* node);

    /// 编译节点作为语句（确保栈平衡：纯表达式语句会补发 OP_POP 弹出返回值）
    void compileStatement(ASTNode* node);

    /// 编译各个节点类型
    void compileBinaryOp(BinaryOp& node);
    void compileUnaryOp(UnaryOp& node);
    void compileNumberLiteral(NumberLiteral& node);
    void compileStringLiteral(StringLiteral& node);
    void compileBoolLiteral(BoolLiteral& node);
    void compileVarDecl(VarDecl& node);
    void compileAssignment(Assignment& node);
    void compileVarRef(VarRef& node);
    void compileIfStmt(IfStmt& node);
    void compileWhileStmt(WhileStmt& node);
    void compileForStmt(ForStmt& node);
    void compileFunDecl(FunDecl& node);
    void compileFunCall(FunCall& node);
    void compileReturnStmt(ReturnStmt& node);
    void compilePrintStmt(PrintStmt& node);
    void compileBlock(Block& node);

    // 新增节点编译
    void compileArrayLiteral(ArrayLiteral& node);
    void compileDictLiteral(DictLiteral& node);
    void compileIndexAccess(IndexAccess& node);
    void compileIndexAssign(IndexAssign& node);
    void compileClassDecl(ClassDecl& node);
    void compileMemberAccess(MemberAccess& node);
    void compileMemberAssign(MemberAssign& node);
    void compileMethodCall(MethodCall& node);
    void compileNullLiteral(NullLiteral& node);
    void compileSuperExpr(SuperExpr& node);

    /// 发出编译错误
    void error(const std::string& msg, int line, int col);

    /// 安全获取当前字节码偏移量（溢出检查）
    uint16_t safeCodeOffset() {
        return safeCodeOffset(chunk_.code.size());
    }

    /// 安全将 size_t 偏移量转为 uint16_t（溢出检查）
    uint16_t safeCodeOffset(size_t offset) {
        if (offset > 65535) {
            throw std::runtime_error("编译错误: 字节码超出 64KB 限制");
        }
        return static_cast<uint16_t>(offset);
    }

    /// 常量折叠：尝试在编译期求值二元运算，成功返回 true 并输出结果
    bool tryFoldBinary(BinOpType opType, ASTNode* left, ASTNode* right,
                       Value& result, int line);

    /// 常量折叠：尝试在编译期求值一元运算，成功返回 true 并输出结果
    bool tryFoldUnary(UnaryOp::UnaryOpType opType, ASTNode* operand,
                      Value& result, int line);

    /// 发射常量值指令（根据 Value 类型选择 OP_INT/OP_FLOAT/OP_STRING/OP_TRUE/OP_FALSE/OP_NULL）
    void emitConstant(const Value& val, int line);
};
