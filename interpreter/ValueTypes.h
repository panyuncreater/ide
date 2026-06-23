#pragma once

// ============================================================
// ValueTypes.h — 类型定义模块
// ------------------------------------------------------------
// 从 Value.h 拆分出的类型定义与前置声明，供需要 ValueType 枚举
// 或 VMClosureData 但不需要完整 Value 定义的文件包含，
// 降低编译耦合度。
// ============================================================

#include <string>
#include <vector>
#include <memory>

// 前向声明（避免循环依赖）
class Environment;
class FunDecl;
class BytecodeChunk;  // M3 fix: 闭包值持有函数 chunk 指针

// ============================================================
// Value 运行时值类型 — std::variant 存储 + COW 语义
// ============================================================
// A1 架构优化：unique_ptr → shared_ptr + copy-on-write
//   拷贝操作 O(1)（仅递增引用计数），写入时通过 ensureUnique() 自动 detach
//   sizeof(Value) 仍为 ~16 字节
//   小类型（int/double/bool/null）内联存储，无堆分配
//   大类型（string/array/dict/instance/closure）通过 shared_ptr 堆分配

/// 值类型枚举（顺序必须与 std::variant Data 的类型顺序严格一致！
/// getType() 通过 static_cast<ValueType>(data_.index()) 将 variant 索引映射到此枚举）
enum class ValueType {
    VAL_NULL,      // 0 = std::monostate
    VAL_INT,       // 1 = int64_t
    VAL_FLOAT,     // 2 = double
    VAL_BOOL,      // 3 = bool
    VAL_STRING,    // 4 = std::shared_ptr<StringData>
    VAL_ARRAY,     // 5 = std::shared_ptr<ArrayData>
    VAL_DICT,      // 6 = std::shared_ptr<DictData>
    VAL_INSTANCE,  // 7 = std::shared_ptr<InstanceData>
    VAL_CLOSURE    // 8 = std::shared_ptr<ClosureData>
};

// ============================================================
// VM 闭包数据结构（定义在 Value 之前，因 ClosureData 持有其 shared_ptr）
// ============================================================

/// VM 闭包 upvalue 数据（VM-05/06）
/// 定义在此处以便 ClosureData 通过 shared_ptr<VMClosureData> 引用，
/// 完整的 VMUpvalue 定义在 ValueData.h（需要 Value 完整定义）
struct VMClosureData {
    std::string functionName;
    std::vector<std::shared_ptr<struct VMUpvalue>> upvalues;
    const BytecodeChunk* chunkPtr = nullptr;  // M3 fix: 直接持有函数 chunk 指针
};
