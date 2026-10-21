#pragma once

// ============================================================
// CoroutineFiber.h — 协程真挂起的平台支持门控（第四轮架构优化）
// ------------------------------------------------------------
// 四后端协程原为"重放模式"（每次 .next() 从头重执行函数体，用运行时
// yield 计数器跳过已消费 yield）：前缀副作用逐次重复执行，总代价
// O(n²)。本头文件定义真挂起的平台支持宏：
//
//   MINILANG_CORO_FIBER
//     =1：真挂起可用（Windows fiber API；Linux POSIX ucontext）。
//         yield 保存执行位置切回调用方，.next() 恢复执行——副作用只执行
//         一次，代价 O(n)。Interpreter 走 fiber 切换（递归树遍历解释器的
//         C++ 栈即延续）；StackVM/RegisterVM 走帧链快照/恢复（纯堆容器，
//         平台无关实现，但与 Interpreter 同门控以保持四后端一致）。
//     =0：其他平台（macOS/其他 POSIX）。四后端统一保留重放路径（ADR-002
//         核心不变量：同一平台上要么全部挂起、要么全部重放，不允许单
//         后端分叉）。
//
// 平台 API 包装不放在本头文件（windows.h 的 min/max 等宏会经 unity
// build 污染同批次编译单元；ucontext.h 同理隔离），仅
// InterpreterCoroutine.cpp 在其实现内包含并封装。Linux 路径按 ADR-007
// 平台覆盖方向落地（2026-10-07）：makecontext/swapcontext 切换协议 +
// ASan fiber 标注（__sanitizer_start/finish_switch_fiber）——Linux ASan
// 门禁（sanitize-asan job）持续验证该路径。macOS 保留重放：ucontext
// 例程标记 deprecated，且发布矩阵的 mac job 仅编译不跑测试，无行为
// 验证通道；启用需先补 macOS 测试门禁。bionic（Android）无
// makecontext，不在支持范围。
//
// 与 JIT 平台降级（ADR-006）同模式：不支持平台上行为回退而非报错，
// 跨平台行为差异属已记录的既定限制（重放与挂起的值序列逐点一致，
// 唯一差异是前缀副作用执行次数）。
// ============================================================

#if defined(_WIN32) || defined(__linux__)
#define MINILANG_CORO_FIBER 1
#else
#define MINILANG_CORO_FIBER 0
#endif // _WIN32 / __linux__
