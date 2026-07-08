# ADR-001: NaN-boxing 值表示

## Status

Accepted. 自项目初始采用，持续维护中。

## Background

MiniLang 需要一个紧凑的值表示方案来高效存储和传递各种类型的值（int、float、bool、null、string、array、dict、instance、closure）。传统的 tagged union 方案（如 `std::variant`）需要 24 字节（8 字节 tag + 8 字节对齐 + 8 字节最大 payload），在操作数栈（1024 槽）和数组存储中造成显著的内存开销。

## Decision

采用 NaN-boxing 编码方案，将所有值压缩到 8 字节（一个 `uint64_t`）：

- **利用 IEEE 754 NaN 空间**：双精度浮点数的 NaN 表示有大量的未使用位模式，利用这些位模式编码非 float 类型
- **Tag 分配**（高 16 位）：
  - `0x7FF8`-`0x7FFB`：堆指针（ptr）
  - `0x7FFC`：int 或 float（通过 payload 区分）
  - `0x7FFD`：bool
  - `0x7FFE`：null
- **Payload**（低 48 位）：
  - int：48 位有符号整数（范围 ±2^47 = ±140,737,488,355,328）
  - float：IEEE 754 double 的 NaN payload（规范化为固定 marker）
  - ptr：48 位堆地址

### 关键设计约束

1. **NaN 完全规范化**：所有 NaN 统一为 `NAN_BOXED_FLOAT_MARKER`（0x7FFC...），不保留 payload。这是为了避免 NaN payload 中的位模式意外匹配 int/bool/null/ptr 的 tag
2. **类型访问前置校验**：访问 Value 前必须用 `isXxx()` 校验类型，防止 NaN-box 位模式误判
3. **value-initialize**：容器强制 value-initialize 防止未初始化内存被误判为合法值
4. **int48 边界**：超出 ±2^47 范围时自动提升为 float

## Impact

- **内存**：`sizeof(Value)` 从 24 字节降至 8 字节，操作数栈从 24KB 降至 8KB
- **性能**：标量（int/float/bool/null）内联存储，零堆分配、零原子操作
- **复杂度**：需要严格的类型校验纪律，所有类型访问路径必须前置 `isXxx()` 检查
- **限制**：int 范围限制在 48 位（±2^47），超出时自动提升为 float

## Alternatives

- **Tagged union（`std::variant`）**：更安全但内存开销 3x，栈操作性能下降
- **指针装箱（pointer tagging）**：所有值堆分配，标量性能差
- **双字表示（16 字节）**：保留完整 64 位 int 和 double，但内存翻倍
