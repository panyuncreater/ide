#pragma once

// ============================================================
// DebugTypes 调试公共类型（ARCH-16 fix）
// ============================================================
// 提取 debug/DebugController.h 与 test_harness/debug/DebugController.h
// 中重复定义的公共类型到本共享头文件，确保两套实现的类型契约一致。
// 本头文件不依赖 Qt（仅依赖 STL + interpreter/Value.h），
// 可被生产实现（debug/DebugController.h，QObject 派生）和
// 测试桩实现（test_harness/debug/DebugController.h，无 Qt）共同包含。

#include <string>
#include <vector>
#include <utility>
#include "interpreter/Value.h"

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

/// 变量快照条目
struct VariableSnapshot {
    std::string name;
    Value value;
    std::string scope;  // 作用域描述
};

/// 调用栈条目
struct CallStackEntry {
    std::string functionName;
    int line;
    int depth;
    std::vector<std::pair<std::string, Value>> locals;  // 该帧的局部变量快照
};

/// 断点信息（支持条件断点）
struct BreakpointInfo {
    int line;
    std::string condition;  // 条件表达式（空字符串 = 无条件断点）
    int hitCount = 0;       // 命中次数

    BreakpointInfo() : line(0) {}
    BreakpointInfo(int ln, const std::string& cond = "")
        : line(ln), condition(cond) {}

    bool isConditional() const { return !condition.empty(); }
};
