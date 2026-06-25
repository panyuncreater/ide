#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "ast/ASTNode.h"
#include "compiler/Bytecode.h"
#include "Diagnostic.h"
#include "interpreter/Visitor.h"  // 继承 DefaultVisitor，统一 AST 分派为 Visitor 模式
#include "common/RuntimeLimits.h"

// ============================================================
// Compiler 字节码编译器
// ============================================================

/// 将 AST 编译为字节码
class Compiler : public DefaultVisitor {
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
    DiagnosticBag diagnostics_;                      // 诊断收集器
    std::unordered_map<std::string, BytecodeChunk> functionChunks_;  // 函数字节码块
    std::unordered_map<std::string, int> currentLocals_;  // 当前函数的局部变量槽位映射
    bool inFunction_ = false;                       // 是否在函数体内
    std::unordered_map<std::string, std::vector<std::string>> classFieldNames_;  // 类名 → 字段名列表（含继承字段）
    std::unordered_map<std::string, int> outerLocals_;  // 外层函数的局部变量（用于检测闭包捕获）
    // VM-05/06: 闭包 upvalue 编译期追踪
    std::vector<UpvalueDesc> currentUpvalues_;       // 当前函数正在构建的 upvalue 描述符列表
    std::unordered_map<std::string, int> currentUpvalueNames_; // 变量名→upvalue索引（去重用）
    std::vector<UpvalueDesc> outerUpvalues_;         // 外层函数的 upvalue 描述符（用于透传检测）
    std::unordered_map<std::string, int> outerUpvalueNames_; // 外层函数的 upvalue 名称映射
    std::unordered_map<std::string, int> outerFunctions_; // 外层作用域的函数名→槽位号（内嵌函数捕获用）
    int writebackCounter_ = 0;  // B6: 写回计数器，生成唯一缓存变量名避免索引重复求值
    int peakLocals_ = 0;        // VMBUG-2: 函数编译期间局部变量槽位峰值（含被块作用域回收的变量）
    int blockDepth_ = 0;        // VMBUG-1: 顶层块作用域嵌套深度（仅在 inFunction_==false 时有效）
    int blockSaveCounter_ = 0;  // L11 fix: 块作用域保存计数器（成员变量，编译间重置）
    // P1 fix: 编译递归深度计数器，防止深度嵌套 AST 导致 C++ 栈溢出
    int compileDepth_ = 0;
    static constexpr int MAX_COMPILE_DEPTH = RuntimeLimits::MAX_COMPILE_DEPTH;
    std::string currentClassName_;  // B1 fix: 当前正在编译的类名（供 OP_SUPER_CALL 编码类上下文）
    std::unordered_set<std::string> topLevelGlobals_;  // VMBUG-1: 顶层（非块/非函数）var 声明的全局变量名集合

    // break/continue 循环上下文栈
    // 每层循环编译时压入，记录 break 跳转目标（循环出口）和 continue 跳转目标（循环起始/更新）
    // 的待回填跳转指令偏移列表，循环编译完成后统一回填
    struct LoopContext {
        size_t loopStart;              // 循环起始字节码偏移（continue 跳转目标）
        size_t loopEndPatch;           // 循环出口跳转指令偏移（break 跳转目标，循环结束时回填）
        std::vector<size_t> breakJumps;    // break 语句的 OP_JUMP 偏移列表（待回填到循环出口）
        std::vector<size_t> continueJumps; // continue 语句的 OP_JUMP/OP_LOOP 偏移列表（待回填到循环起始）
        bool hasUpdate;                // for 循环有 update 表达式，continue 应跳到 update 而非 loopStart
        size_t updateStart;            // for 循环 update 表达式起始偏移（hasUpdate=true 时有效）
        int tryDepthAtStart;           // BUG1 fix: 循环开始时的 tryDepth_，break/continue 只弹循环内 try handler
    };
    std::vector<LoopContext> loopStack_;

    // P0-4 fix: 跟踪当前 try 块嵌套深度，break/continue 跳出 try 块时需发射 OP_TRY_END
    int tryDepth_ = 0;

    // H5 fix: 内嵌函数闭包追踪 — 内嵌函数存储为局部变量，通过 OP_CALL_EXPR 调用
    std::unordered_set<std::string> innerFunctions_;           // 当前作用域中的内嵌函数名
    std::unordered_map<std::string, int> innerFunctionSlots_;  // 内嵌函数名 → 局部变量槽位号

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

    /// 编译 AST 节点（通过 Visitor 模式的 accept 分派）
    void compileNode(ASTNode* node);

    /// 编译节点作为语句（确保栈平衡：纯表达式语句会补发 OP_POP 弹出返回值）
    void compileStatement(ASTNode* node);

    /// 编译各个节点类型（Visitor 模式：由 accept 分派调用，返回 Value 统一接口）
    Value visitBinaryOp(BinaryOp& node) override;
    Value visitUnaryOp(UnaryOp& node) override;
    Value visitNumberLiteral(NumberLiteral& node) override;
    Value visitStringLiteral(StringLiteral& node) override;
    Value visitBoolLiteral(BoolLiteral& node) override;
    Value visitVarDecl(VarDecl& node) override;
    Value visitAssignment(Assignment& node) override;
    Value visitVarRef(VarRef& node) override;
    Value visitIfStmt(IfStmt& node) override;
    Value visitWhileStmt(WhileStmt& node) override;
    Value visitForStmt(ForStmt& node) override;
    Value visitFunDecl(FunDecl& node) override;
    Value visitFunCall(FunCall& node) override;
    Value visitReturnStmt(ReturnStmt& node) override;
    Value visitPrintStmt(PrintStmt& node) override;
    Value visitBlock(Block& node) override;

    // 新增节点编译
    Value visitArrayLiteral(ArrayLiteral& node) override;
    Value visitDictLiteral(DictLiteral& node) override;
    Value visitIndexAccess(IndexAccess& node) override;
    Value visitIndexAssign(IndexAssign& node) override;
    Value visitClassDecl(ClassDecl& node) override;
    Value visitMemberAccess(MemberAccess& node) override;
    Value visitMemberAssign(MemberAssign& node) override;
    Value visitMethodCall(MethodCall& node) override;
    Value visitNullLiteral(NullLiteral& node) override;
    Value visitSuperExpr(SuperExpr& node) override;
    Value visitBreakStmt(BreakStmt& node) override;
    Value visitContinueStmt(ContinueStmt& node) override;
    Value visitTryStmt(TryStmt& node) override;
    Value visitThrowStmt(ThrowStmt& node) override;
    Value visitImportStmt(ImportStmt& node) override;
    Value visitExportStmt(ExportStmt& node) override;

    /// 发出编译错误
    void error(const std::string& msg, int line, int col);

    /// VM-05/06: 解析闭包捕获变量为 upvalue 索引（返回 -1 表示未找到）
    int resolveUpvalue(const std::string& name, int line);

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
