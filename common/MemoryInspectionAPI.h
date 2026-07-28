/**
 * @file common/MemoryInspectionAPI.h
 * @brief 内存检视只读快照 API（ARCH-10）。
 *
 * 为 GUI 教学面板（MemoryModelPanel / GcVisualizerPanel / VariableInspectorPanel
 * 等）提供对 NaN-boxing / RefCounted / GcManager 内部状态的只读快照访问，
 * 消除面板对 interpreter/NaNBox.h / interpreter/RefCounted.h /
 * interpreter/GcManager.h 内部类型的直接依赖。
 *
 * 设计原则：
 *   - 只暴露教学面板需要的只读快照（纯数据结构，无内部类型透出）
 *   - GcMode / GcPhase 枚举定义在此处（单一真相源），GcManager.h 通过
 *     #include 此头文件复用，避免 common/ → interpreter/ 反向依赖
 *   - 静态方法形式，无实例状态（GcManager 单例访问封装在 .cpp 内）
 *   - 返回值均为值拷贝或 std::string，调用方无需持有锁或担心生命周期
 *
 * 与 IBackend 的关系：
 *   - IBackend 抽象执行后端（Compiler/VM/RegisterVM）的回调与诊断接口
 *   - MemoryInspectionAPI 抽象内存子系统（NaNBox/RefCounted/GcManager）的只读检视
 *   - 两者互补，共同构成 GUI 层与引擎层之间的服务接口
 *
 * @since ARCH-10
 */
#pragma once

#include <cstdint>
#include <functional> // std::function（setGcTriggerCallback 参数）
#include <string>
#include <vector>

// 前向声明 Value，避免 #include "interpreter/Value.h" 造成循环依赖
// （Value.h → GcManager.h → MemoryInspectionAPI.h → Value.h）。
// 调用方（GUI 面板）必然已 include Value.h，传参时拥有完整定义。
struct Value;

// ============================================================
// GC 枚举类型（单一真相源，GcManager.h 复用）
// ============================================================
// 这两个枚举原定义在 interpreter/GcManager.h，现迁移到 common/ 公共头文件，
// 使 GUI 面板可通过 common/MemoryInspectionAPI.h 获取枚举类型而无需直接
// include interpreter/GcManager.h（后者会传递引入 mutex/unordered_set 等
// 内部实现细节）。
//
// GcManager.h 通过 #include "common/MemoryInspectionAPI.h" 复用枚举定义，
// 保持单一真相源，避免定义重复导致的漂移风险。

/// R113 C 项：GC 阶段状态枚举——用于 MemoryModelPanel 实时展示当前 mark-sweep 进度。
/// collectCycle 是同步阻塞操作，currentPhase_ 在阶段切换时短暂变更，
/// collectCycle 结束后回归 Idle。UI 在 animTimer_ 周期内能观察到上次 GC 经历的阶段。
enum class GcPhase : uint8_t {
    Idle,       // 未在 GC 中（或上次 GC 已结束）
    Marking,    // Phase 1: 从 roots 出发 mark 所有可达容器节点
    Sweeping,   // Phase 2: 清扫 marked 中不存在但 aliveSet_ 中存在的循环孤岛
    Finalizing, // Phase 3: 重建 tracked_ / aliveSet_，重置 marked 标志
};

/// R135 GC 模式枚举——三种内存管理策略可切换，用于教学对比与性能基准。
/// 模式语义：
///   RefCountOnly        - 纯引用计数，不注册 GcManager，循环引用会泄漏（基线对比用）
///   RefCountWithCycleGc - 引用计数主导 + GC 仅回收循环孤岛（默认，项目历史模式）
///   GcOnly              - GC 主导：sweep 阶段直接 delete 不可达对象（实验性）
/// 切换时机：仅在 Interpreter/VM 完全重置后切换（避免运行中切换导致状态不一致）
enum class GcMode : uint8_t {
    RefCountOnly = 0,
    RefCountWithCycleGc = 1,
    GcOnly = 2,
};

// ============================================================
// 只读快照数据结构
// ============================================================

/// NaN-box 位模式快照（替代面板直接调用 NaNBox::fromInt(...).rawBits()）
/// 所有字段为值类型，调用方无需依赖 NaNBox 类。
struct NaNBoxSnapshot {
    uint64_t bits = 0;        // 原始 64 位位模式
    std::string bitsHex;      // "0x" + 16 位十六进制（带前导零）
    std::string bitsBinary;   // 64 字符 "0/1" 字符串（最高位在索引 0）
    std::string tagName;      // "FLOAT" / "INT" / "BOOL" / "NUL" / "POINTER"
    int tagValue = -1;        // NaNBox::Tag 枚举值（0-4）；-1 表示未知
};

/// 堆对象元信息快照（替代面板直接访问 RefCounted* p = v.asPointer(); p->type）
struct HeapObjectSnapshot {
    bool isPointer = false;             // 是否为堆指针类型
    std::string structName;             // C++ 结构名（"ArrayData" / "DictData" / "InstanceData" 等）
    std::string addressHex;             // "0x" + 16 位十六进制指针
    size_t fieldCount = 0;              // 元素数/字段数（数组=元素数，字典/实例=字段数，闭包=capturedVars）
    std::string valueTypeName;          // "VAL_INT" / "VAL_STRING" / "VAL_ARRAY" / ...
    int valueType = -1;                 // ValueType 枚举值（与 Value::getType() 一致）
    size_t refCount = 0;                // 引用计数（RefCounted::useCount()），仅 isPointer 时有效
};

/// GC 统计快照（替代面板直接调用 GcManager::instance().trackedCount() 等）
struct GcStatsSnapshot {
    size_t trackedCount = 0;             // 当前 tracked 节点数
    size_t totalGcCount = 0;             // 累计 GC 次数
    size_t lastMarkedCount = 0;          // 上次 GC 标记的可达节点数
    size_t lastCollectedCount = 0;       // 上次 GC 回收的孤岛数
    GcMode mode = GcMode::RefCountWithCycleGc;       // 当前 GcMode
    GcPhase phase = GcPhase::Idle;                   // 当前 GcPhase
    size_t allocationsSinceLastGc = 0;   // 自上次 GC 后累计分配数
    size_t allocationThreshold = 0;      // 增量触发阈值
};

// ============================================================
// MemoryInspectionAPI — 内存检视只读 API
// ============================================================
// 所有方法均为静态，无实例状态。GcManager 单例访问、NaNBox 构造、RefCounted
// 指针解引用等内部实现细节封装在 .cpp 中，调用方仅依赖此头文件的纯数据结构。
//
// 线程安全：
//   - 读接口（inspectValue / inspectHeap / getGcStats）通过 GcManager 内部锁
//     保护，与 worker 线程的 collectCycle 互斥。
//   - 写接口（resetGc / setGcMode / collectCycle）同上，调用方需确保 Interpreter/VM
//     已完全重置（不在执行中），避免运行中切换导致状态不一致。
class MemoryInspectionAPI {
public:
    // ---- NaN-box 检视（替代 NaNBox::fromInt(...).rawBits() 等直接调用）----

    /// 检视 Value 的 NaN-box 位模式。
    /// 对标量类型（int/float/bool/null）返回编码后的位模式；
    /// 对堆指针类型返回 PTR_TAG_BASE 占位符（指针值不固定，教学展示用占位符）。
    static NaNBoxSnapshot inspectValue(const Value& v);

    /// 检视原始 64 位位模式的 NaN-box 解码信息。
    /// 用于面板展示"假设位模式为 X，对应什么类型"的教学场景。
    static NaNBoxSnapshot inspectBits(uint64_t bits);

    // ---- NaN-box 编码（供 MemoryModelPanel 交互式编码器使用）----
    // 这些方法封装 NaNBox 的编码逻辑，使面板无需直接依赖 interpreter/NaNBox.h。
    // 返回原始 64 位位模式，调用方可通过 inspectBits() 获取 tag/解码信息。

    /// 检查 int64 值是否在 int48 编码范围内（|v| < 2^47）。
    /// 超范围的整数会装箱为 BoxedIntData 堆对象，无法用 NaN-box 标量编码。
    static bool canEncodeInt(int64_t v);

    /// 将 int 编码为 NaN-box 位模式（超范围会降级为 float 并 LOG_WARNING）。
    static uint64_t encodeInt(int64_t v);

    /// 将 double 编码为 NaN-box 位模式。
    static uint64_t encodeFloat(double v);

    /// 将 bool 编码为 NaN-box 位模式。
    static uint64_t encodeBool(bool v);

    /// 返回 null 的 NaN-box 位模式。
    static uint64_t encodeNull();

    // ---- NaN-box 解码（供 MemoryModelPanel 自定义编码器 demotion 路径使用）----

    /// 从 NaN-box 位模式解码 float 值（用于 int 超范围降级为 float 后取实际值）。
    static double decodeFloat(uint64_t bits);

    /// 从 NaN-box 位模式解码 int 值（仅对 INT tag 有效，其他 tag 行为未定义）。
    static int64_t decodeInt(uint64_t bits);

    // ---- 堆对象检视（替代 v.asPointer() + p->type 直接访问）----

    /// 检视 Value 引用的堆对象元信息。
    /// 对非指针类型返回 isPointer=false；
    /// 对指针类型返回结构名/地址/字段数/类型名。
    static HeapObjectSnapshot inspectHeap(const Value& v);

    // ---- GC 统计与控制（替代 GcManager::instance() 直接调用）----

    /// 获取 GC 当前统计快照（tracked/GcCount/marked/collected/mode/phase 等）。
    static GcStatsSnapshot getGcStats();

    /// 重置 GcManager（清空 tracked/aliveSet，不清除节点本身）。
    /// 调用方需确保 Interpreter/VM 已完全重置。
    static void resetGc();

    /// 切换 GC 模式（RefCountOnly / RefCountWithCycleGc / GcOnly）。
    /// 调用方需确保 Interpreter/VM 已完全重置。
    static void setGcMode(GcMode mode);

    /// 执行一次 mark-sweep 收集。
    /// @param roots 根集指针（容器 Value 的内部 RefCounted* 地址）
    static void collectCycle(const std::vector<const void*>& roots);

    /// 设置增量 GC 触发阈值（测试场景可调小以验证触发行为）。
    static void setGcAllocationThreshold(size_t threshold);

    /// 设置增量 GC 触发回调（由 Interpreter 在构造时注册）。
    /// 教学面板一般不设置此回调，仅查询统计。
    static void setGcTriggerCallback(std::function<void()> cb);
};

/// 将 GcPhase 枚举转为英文显示文本（"Idle" / "Marking" / "Sweeping" / "Finalizing"）
/// 公开工具函数，供面板渲染统计表时使用，避免每个面板自行实现转换。
std::string gcPhaseToString(GcPhase phase);

/// 将 GcMode 枚举转为英文显示文本（"RefCountOnly" / "RefCountWithCycleGc" / "GcOnly"）
std::string gcModeToString(GcMode mode);
