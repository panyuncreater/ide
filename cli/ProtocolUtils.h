// ============================================================
// cli/ProtocolUtils.h - LSP/DAP 共享协议边界工具
// ------------------------------------------------------------
// Content-Length 来自不可信输入：必须拒绝非数字、空值和超大值，
// 避免 std::stoul 抛异常终止服务器，或分配数 GB 的 body。
// ============================================================
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace minilang_cli {

inline std::optional<size_t> parseContentLength(std::string_view value) {
    constexpr size_t kMaxContentLength = 32u * 1024u * 1024u;

    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' ||
                           value[end - 1] == '\n')) {
        --end;
    }
    if (begin == end)
        return std::nullopt;

    size_t length = 0;
    for (size_t i = begin; i < end; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < '0' || c > '9')
            return std::nullopt;
        const size_t digit = static_cast<size_t>(c - '0');
        if (length > (kMaxContentLength - digit) / 10)
            return std::nullopt;
        length = length * 10 + digit;
    }
    if (length == 0)
        return std::nullopt;
    return length;
}

} // namespace minilang_cli
