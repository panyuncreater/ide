#pragma once

// ============================================================
// DebugTypes 调试公共类型（ARCH-16 fix）
// ============================================================
// 提取 debug/DebugController.h 与 test_harness/debug/DebugController.h
// 中重复定义的公共类型到本共享头文件，确保两套实现的类型契约一致。
// 本头文件不依赖 Qt（仅依赖 STL + interpreter/Value.h），
// 可被生产实现（debug/DebugController.h，QObject 派生）和
// 测试桩实现（test_harness/debug/DebugController.h，无 Qt）共同包含。
//
// R104 调试器拓展：新增 BreakpointKind 枚举（区分 Line/Logpoint）、
// FunctionBreakpointInfo（函数断点）、ExceptionBreakpointState（异常断点）。
// Logpoint 复用 BreakpointInfo（同为行锚点），新增 kind/logMessage 字段；
// 函数断点与异常断点不依赖行号，使用独立结构。
// ============================================================

#include "interpreter/Value.h"
#include <atomic>
#include <string>
#include <utility>
#include <vector>

// ============================================================
// 调试公共类型
// ============================================================

/// 调试步进模式
/// 两套实现必须保持枚举值一致（test_harness 注释明确要求与生产版匹配）
enum class StepMode {
    MODE_RUN,       // 正常运行（仅检查断点）
    MODE_STEP_IN,   // 单步进入（每个节点暂停）
    MODE_STEP_OVER, // 单步跳过（同调用深度暂停）
    MODE_STEP_OUT   // 单步跳出（浅于当前深度时暂停）
};

/// R104 调试器拓展：断点类型
/// - Line：普通行断点（命中即暂停，可附加条件）
/// - Logpoint：日志断点（命中不暂停，求值日志模板并输出到日志通道，递增 hitCount）
///   Logpoint 仍可附加 condition 字段（condition 非空时仅在条件为真时输出日志）
enum class BreakpointKind {
    Line,     // 普通行断点（默认）
    Logpoint, // 日志断点（不暂停，仅输出日志）
};

/// 变量快照条目
struct VariableSnapshot {
    std::string name;
    Value value;
    std::string scope; // 作用域描述
};

/// 调用栈条目
struct CallStackEntry {
    std::string functionName;
    int line = 0;  // R121 fix: 添加默认初始化避免未初始化内存（MSVC Debug 为 0xCDCDCDCD）
    int depth = 0; // R121 fix: 添加默认初始化避免未初始化内存
    std::vector<std::pair<std::string, Value>> locals; // 该帧的局部变量快照
};

/// 断点信息（支持条件断点 + R104 Logpoint）
struct BreakpointInfo {
    int line;
    std::string condition; // 条件表达式（空字符串 = 无条件断点）
    int hitCount = 0;      // 命中次数
    // R104 Logpoint：断点类型 + 日志模板
    // kind == Logpoint 时，logMessage 非空，支持 {expr} 插值（在沙箱中求值后输出）。
    // kind == Line 时，logMessage 字段被忽略。
    BreakpointKind kind = BreakpointKind::Line;
    std::string logMessage; // Logpoint 日志模板（如 "i={i}, sum={sum}"）

    BreakpointInfo() : line(0) {}
    BreakpointInfo(int ln, const std::string& cond = "") : line(ln), condition(cond) {}

    bool isConditional() const { return !condition.empty(); }
    bool isLogpoint() const { return kind == BreakpointKind::Logpoint; }
};

/// R104 调试器拓展：函数断点信息
/// 按函数名设置断点（不依赖行号）。函数被调用时（callNamedFunction / OP_CALL / REG_CALL 入口），
/// 若函数名匹配则暂停。适合无法看到源码的标准库函数或跨文件函数调用。
struct FunctionBreakpointInfo {
    std::string functionName; // 函数名（与 funRegistry_ / functionChunks 键匹配）
    std::string condition;    // 条件表达式（空 = 无条件）
    int hitCount = 0;         // 命中次数

    FunctionBreakpointInfo() = default;
    explicit FunctionBreakpointInfo(std::string name) : functionName(std::move(name)) {}

    bool isConditional() const { return !condition.empty(); }
};

/// R104 调试器拓展：异常断点状态
/// 单一全局开关，启用后在 throw 语句执行前暂停（对齐 GDB `catch throw`）。
/// 不支持 catch 入口暂停（设计决策：catch 入口需侵入 try/catch 调度链，复杂度高且
/// 与三后端 try/catch/finally 续跳链布局耦合，故仅支持 throw 暂停）。
struct ExceptionBreakpointState {
    bool enabled = false; // 是否启用异常断点
    int hitCount = 0;     // 命中次数
    // 注：当前不支持条件过滤（异常值匹配）。throw 抛出的值类型多样（string/int/object），
    // 在沙箱中匹配需要额外的值序列化机制。后续可作为拓展项。

    ExceptionBreakpointState() = default;
};

// ============================================================
// R161 调试器拓展：数据断点（Watchpoint）
// ------------------------------------------------------------
// 监视变量/字段被修改时暂停（类似 GDB `watch` 命令）。
// VM 路径通过 pre-execution peek（peekWriteTarget）在写入指令执行前检查；
// Interpreter 路径在 visitAssignment/visitMemberAssign/visitIndexAssign 入口检查。
// ============================================================

/// R161: 数据断点目标类型
enum class WatchpointTargetKind {
    Variable, // 监视变量（如 x）
    Field,    // 监视实例字段（如 obj.field）
};

/// R161: 数据断点信息
struct WatchpointInfo {
    WatchpointTargetKind kind = WatchpointTargetKind::Variable;
    std::string varName;   // 变量名（Variable/Field 共用，Field 时为接收者变量名）
    std::string fieldName; // 字段名（仅 Field 有效）
    std::string condition; // 条件表达式（可选，空=无条件）
    int hitCount = 0;      // 命中次数

    WatchpointInfo() = default;
    explicit WatchpointInfo(std::string name) : varName(std::move(name)) {}

    bool isConditional() const { return !condition.empty(); }
};

/// R161: pre-execution peek 写入目标描述（VM/RegisterVM 共享，peekWriteTarget 返回）
/// 用于数据断点（Watchpoint）在写入指令执行前检查。
struct WriteTarget {
    bool isWrite = false;      // 当前指令是否为写入指令
    std::string varName;       // 写入的变量名
    int localSlot = -1;        // 写入的局部槽号
    std::string fieldName;     // 字段名（仅字段写入有效）
    bool isFieldWrite = false; // 是否字段写入
    bool isIndexWrite = false; // 是否索引写入
};
