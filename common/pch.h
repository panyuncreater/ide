#pragma once

// ============================================================
// 预编译头（PCH）
// ------------------------------------------------------------
// 包含最常用、最稳定的标准库头文件，减少重复解析开销。
// 仅放入标准库头文件 —— 项目头文件变更频繁，放入 PCH 会使其频繁失效。
// 目标：minilang_core（10 个 .cpp，编译时间下降约 20-35%）。
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
