// ============================================================
// ShareCodec — 代码片段分享链接编解码实现（拓展二期·平台）
// ------------------------------------------------------------
// 负载格式：minilang://share/v1/<base64url(qCompress(utf8) || CRC16)>
//
// CRC-16 尾部校验（2 字节，big-endian，Qt qChecksum/ISO 3309）：
// 实测发现 qUncompress 对"被截断的 zlib 流"会进入缓冲区反复加倍的
// 近似无限循环（Z_BUF_ERROR 重试直到内存耗尽，表现为挂死）。
// 因此解压前必须先做完整性校验：截断/篡改的负载在 CRC 阶段
// 即被 O(n) 拒绝，qUncompress 只处理校验通过的数据。
// 另对负载声明的解压尺寸设 16MB 上限，拒绝恶意构造的 zip bomb。
// ============================================================

#include "gui/ShareCodec.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QtGlobal>

namespace ShareCodec {

namespace {
/// 解压后源码尺寸上限（16MB）：教学片段远小于此，超限视为恶意负载
constexpr quint32 kMaxDecompressedSize = 16u * 1024u * 1024u;
} // namespace

QString encode(const QString& code) {
    if (code.isEmpty())
        return {};
    // UTF-8 → zlib 压缩（级别 9：分享链接一次编码多次粘贴，压得越小越好）
    QByteArray blob = qCompress(code.toUtf8(), 9);
    // 追加 CRC-16 尾部校验（防截断/篡改，见文件头说明）
    quint16 crc = qChecksum(QByteArrayView(blob));
    blob.append(static_cast<char>((crc >> 8) & 0xFF));
    blob.append(static_cast<char>(crc & 0xFF));
    // base64url（无 + / 字符）+ 省略尾部 = 填充，负载可直接嵌入 URL/聊天消息
    QByteArray payload = blob.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QString::fromLatin1(kSharePrefix) + QString::fromLatin1(payload);
}

bool decode(const QString& url, QString& codeOut, QString& errorOut) {
    codeOut.clear();
    errorOut.clear();

    // 容忍聊天工具粘贴引入的首尾空白/换行
    QString trimmed = url.trimmed();
    const QString prefix = QString::fromLatin1(kSharePrefix);
    if (!trimmed.startsWith(prefix)) {
        errorOut = QStringLiteral("链接格式无效：应以 %1 开头").arg(prefix);
        return false;
    }

    QString payloadStr = trimmed.mid(prefix.size());
    if (payloadStr.isEmpty()) {
        errorOut = QStringLiteral("链接负载为空");
        return false;
    }

    // AbortOnBase64DecodingErrors：负载被截断/篡改时立即失败而非静默产出垃圾
    auto decoded = QByteArray::fromBase64Encoding(
        payloadStr.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        errorOut = QStringLiteral("base64 解码失败：链接可能被截断或损坏");
        return false;
    }

    // 结构下限：qCompress 4 字节长度头 + 至少 1 字节 zlib 流 + 2 字节 CRC
    QByteArray blob = *decoded;
    if (blob.size() < 7) {
        errorOut = QStringLiteral("链接负载过短：不是有效的分享负载");
        return false;
    }

    // CRC-16 完整性校验——必须先于 qUncompress（截断流会使其挂死，见文件头）
    const auto n = blob.size();
    quint16 storedCrc = static_cast<quint16>((static_cast<quint8>(blob[n - 2]) << 8) |
                                             static_cast<quint8>(blob[n - 1]));
    QByteArray compressed = blob.left(n - 2);
    if (qChecksum(QByteArrayView(compressed)) != storedCrc) {
        errorOut = QStringLiteral("校验和不匹配：链接被截断或篡改");
        return false;
    }

    // qCompress 前 4 字节为解压后尺寸（big-endian），超限拒绝（zip bomb 防护）
    quint32 expectedSize = (static_cast<quint32>(static_cast<quint8>(compressed[0])) << 24) |
                           (static_cast<quint32>(static_cast<quint8>(compressed[1])) << 16) |
                           (static_cast<quint32>(static_cast<quint8>(compressed[2])) << 8) |
                           static_cast<quint32>(static_cast<quint8>(compressed[3]));
    if (expectedSize > kMaxDecompressedSize) {
        errorOut = QStringLiteral("负载声明尺寸异常（%1 字节）：拒绝解压").arg(expectedSize);
        return false;
    }

    QByteArray decompressed = qUncompress(compressed);
    if (decompressed.isEmpty()) {
        // 合法空源码在 encode 端已被拒绝，空结果只可能是解压失败
        errorOut = QStringLiteral("解压失败：链接负载损坏");
        return false;
    }

    codeOut = QString::fromUtf8(decompressed);
    return true;
}

} // namespace ShareCodec
