#pragma once

// ============================================================
// ValueData.h — 数据载体模块
// ------------------------------------------------------------
// 从 Value.h 拆分出的附加数据结构，这些结构依赖完整的 Value
// 类型定义，因此必须包含在 Value.h 之后。
// ============================================================

#include "interpreter/Value.h"

// VM-05/06: VM 闭包 upvalue 结构（定义在 Value 之后，以便使用完整 Value 类型）
struct VMUpvalue {
    Value value;           // 关闭后存储值（isClosed=true 时有效）
    bool isClosed = false; // 是否已关闭
    size_t stackSlot = 0;  // 未关闭时指向的栈绝对位置
};
