/**
 * @file common/VmTypes.h
 * @brief VM 公共类型定义（P1-5 fix: IBackend 子接口落地）。
 *
 * VMResult 从 compiler/VM.h 提取到此头文件，使 common/IBackend.h 的 IVmBackend
 * 子接口可引用该类型而不依赖 compiler/VM.h（避免 common → compiler 反向依赖）。
 *
 * VM.h 和 RegisterVM.h 通过 #include 此头文件保持 VMResult 定义单一真相源。
 *
 * @since P1-5
 */
#pragma once

/// 虚拟机执行结果
/// L14: VM_EXCEPTION_THROW — runtimeError 被 try/catch 捕获，ip 已设置为 catchIp，
/// 调用方应立即 return（不递增 ip），主循环应继续执行（不中断）。
enum class VMResult { VM_OK, VM_RUNTIME_ERROR, VM_STACK_OVERFLOW, VM_EXCEPTION_THROW };
