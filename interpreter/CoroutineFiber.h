#pragma once

// ============================================================
// CoroutineFiber.h — 协程真挂起的平台支持门控（第四轮架构优化）
// ------------------------------------------------------------
// 四后端协程原为"重放模式"（每次 .next() 从头重执行函数体，用运行时
// yield 计数器跳过已消费 yield）：前缀副作用逐次重复执行，总代价
// O(n²)。本头文件定义真挂起的平台支持宏：
//
//   MINILANG_CORO_FIBER
//     =1：真挂起可用（当前：Windows，fiber API）。yield 保存执行位置
//         切回调用方，.next() 恢复执行——副作用只执行一次，代价 O(n)。
//         Interpreter 走 fiber 切换（递归树遍历解释器的 C++ 栈即延续）；
//         StackVM/RegisterVM 走帧链快照/恢复（纯堆容器，平台无关实现，
//         但与 Interpreter 同门控以保持四后端一致）。
//     =0：其他平台。四后端统一保留重放路径（ADR-002 核心不变量：同一
//         平台上要么全部挂起、要么全部重放，不允许单后端分叉）。
//
// 平台 API 包装不放在本头文件（windows.h 的 min/max 等宏会经 unity
// build 污染同批次编译单元），仅 InterpreterCoroutine.cpp 在其实现内
// 以 NOMINMAX/WIN32_LEAN_AND_MEAN 防护后包含。POSIX ucontext 路径为
// 后续方向（需配套 ASan fiber 标注，见 docs/specs/backend-consistency.md）。
//
// 与 JIT 平台降级（ADR-006）同模式：不支持平台上行为回退而非报错，
// 跨平台行为差异属已记录的既定限制。
// ============================================================

#if defined(_WIN32)
#define MINILANG_CORO_FIBER 1
#else
#define MINILANG_CORO_FIBER 0
#endif // _WIN32
