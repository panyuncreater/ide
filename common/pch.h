#pragma once

// ============================================================
// 预编译头（PCH）
// ------------------------------------------------------------
// 包含最常用、最稳定的标准库头文件，减少重复解析开销。
// 仅放入标准库头文件 —— 项目头文件变更频繁，放入 PCH 会使其频繁失效。
// 目标：minilang_core（10 个 .cpp，编译时间下降约 20-35%）。
// ============================================================

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <cmath>
#include <climits>
#include <utility>
#include <optional>
#include <variant>
#include <charconv>
#include <stdexcept>
