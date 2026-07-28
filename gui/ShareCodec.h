#pragma once

// ============================================================
// ShareCodec — 代码片段分享链接编解码（拓展二期·平台）
// ------------------------------------------------------------
// 把 MiniLang 源码编码为可粘贴分享的自包含链接：
//   minilang://share/v1/<base64url(qCompress(utf8(code)))>
//
// 设计要点：
//   - qCompress(zlib) + Base64UrlEncoding|OmitTrailingEquals：
//     负载 URL 安全（无 + / = 字符），教学片段典型压缩率 40-60%
//   - v1 版本段：未来负载格式演进时可向后兼容解析
//   - 纯 Qt6::Core 依赖（QString/QByteArray），不依赖 Widgets /
//     IdeController / 引擎层——可安全加入测试目标（同 SandboxLevels 模式）
//
// 使用方：app/ide.cpp「复制分享链接 / 从分享链接导入」菜单动作。
// ============================================================

#include <QString>

namespace ShareCodec {

/// 分享链接前缀（scheme + 版本段）
inline constexpr const char* kSharePrefix = "minilang://share/v1/";

/// 源码 → 分享链接。
/// 空源码返回空字符串（无意义分享）。
QString encode(const QString& code);

/// 分享链接 → 源码。
/// 成功返回 true 并填充 codeOut；失败返回 false 并填充 errorOut
/// （前缀不匹配 / base64 解码失败 / 解压失败）。
/// 输入允许首尾空白（自动 trim，容忍聊天工具粘贴产生的换行）。
bool decode(const QString& url, QString& codeOut, QString& errorOut);

} // namespace ShareCodec
