#pragma once

// ============================================================
// 预编译头（PCH）
// ------------------------------------------------------------
// 包含最常用、最稳定的标准库头文件，减少重复解析开销。
// 仅放入标准库头文件 —— 项目头文件变更频繁，放入 PCH 会使其频繁失效。
// 目标：minilang_core（10 个 .cpp，编译时间下降约 20-35%）。
//
// L10 fix（2026-07-19）：确认 PCH 仅含标准库头，不含 Bytecode.h 等项目头。
// docs/testing.md 提到的"修改 Bytecode.h 触发全量重编译"是 C++ 依赖管理
// 固有行为（Bytecode.h 被约 30+ .cpp 直接 include），非 PCH 配置问题。
// 彻底解决需对 BytecodeChunk 等核心类采用 PIMPL 模式，但重构成本高、
// 收益有限（核心头变更不频繁），当前维持现状。
// ============================================================

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
