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
    // #18 fix: 捕获该 upvalue 的栈槽所属帧索引（在 OP_CLOSURE isLocal=true 时设置）。
    // upvalue 为 open 状态时，所属帧必定仍在 frames_ 中（帧返回前会 closeUpvaluesFrom
    // 关闭所有指向该帧的 upvalue），故 owningFrameIdx 始终有效。passthrough 复用时
    // shared_ptr 共享同一 VMUpvalue，owningFrameIdx 自动透传。OP_SET_UPVALUE 借此
    // O(1) 定位目标帧，替代原 O(frames_) 线性扫描。
    size_t owningFrameIdx = 0;
};
