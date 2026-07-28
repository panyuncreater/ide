// ============================================================
// tests/TestShareCodec.cpp - 代码片段分享链接编解码测试
// ------------------------------------------------------------
// 拓展二期·平台：验证 ShareCodec::encode/decode 的
//   - 往返等价性（UTF-8 中文/换行/长代码）
//   - 链接格式（前缀 + base64url 字符集）
//   - 错误路径（前缀不匹配/空负载/截断/篡改）
// ============================================================
#include "gui/ShareCodec.h"

#include <gtest/gtest.h>

#include <QString>

// ---- 往返等价性 ----

TEST(ShareCodecTest, RoundTripSimpleCode) {
    QString code = QStringLiteral("var x = 42;\nprint(x);\n");
    QString url = ShareCodec::encode(code);
    ASSERT_FALSE(url.isEmpty());

    QString decoded, err;
    ASSERT_TRUE(ShareCodec::decode(url, decoded, err)) << err.toStdString();
    EXPECT_EQ(decoded, code);
}

TEST(ShareCodecTest, RoundTripUtf8ChineseAndEmoji) {
    QString code = QString::fromUtf8("// 中文注释：斐波那契 🚀\nfun fib(n) {\n"
                                     "    if (n < 2) { return n; }\n"
                                     "    return fib(n - 1) + fib(n - 2);\n}\nprint(fib(10));\n");
    QString url = ShareCodec::encode(code);
    QString decoded, err;
    ASSERT_TRUE(ShareCodec::decode(url, decoded, err)) << err.toStdString();
    EXPECT_EQ(decoded, code);
}

TEST(ShareCodecTest, RoundTripLongRepetitiveCodeCompresses) {
    // 长重复代码：验证压缩生效（负载显著短于源码）且往返无损
    QString code;
    for (int i = 0; i < 200; ++i) {
        code += QStringLiteral("print(\"line %1 of repetitive teaching sample\");\n").arg(i);
    }
    QString url = ShareCodec::encode(code);
    QString decoded, err;
    ASSERT_TRUE(ShareCodec::decode(url, decoded, err)) << err.toStdString();
    EXPECT_EQ(decoded, code);
    // zlib 对高重复文本压缩率极高，负载应远短于原文（保守断言 < 1/2）
    EXPECT_LT(url.size(), code.size() / 2);
}

// ---- 链接格式 ----

TEST(ShareCodecTest, UrlHasPrefixAndUrlSafePayload) {
    QString url = ShareCodec::encode(QStringLiteral("print(1);"));
    ASSERT_TRUE(url.startsWith(QString::fromLatin1(ShareCodec::kSharePrefix)));
    // 负载必须 URL 安全：不含 + / = 字符
    QString payload = url.mid(QString::fromLatin1(ShareCodec::kSharePrefix).size());
    EXPECT_FALSE(payload.isEmpty());
    EXPECT_EQ(payload.indexOf(QLatin1Char('+')), -1);
    EXPECT_EQ(payload.indexOf(QLatin1Char('/')), -1);
    EXPECT_EQ(payload.indexOf(QLatin1Char('=')), -1);
}

TEST(ShareCodecTest, EncodeEmptyCodeReturnsEmpty) {
    EXPECT_TRUE(ShareCodec::encode(QString()).isEmpty());
}

TEST(ShareCodecTest, DecodeToleratesSurroundingWhitespace) {
    QString code = QStringLiteral("print(\"hello\");");
    QString url = QStringLiteral("  \n") + ShareCodec::encode(code) + QStringLiteral("\r\n ");
    QString decoded, err;
    ASSERT_TRUE(ShareCodec::decode(url, decoded, err)) << err.toStdString();
    EXPECT_EQ(decoded, code);
}

// ---- 错误路径 ----

TEST(ShareCodecTest, DecodeRejectsWrongPrefix) {
    QString decoded, err;
    EXPECT_FALSE(ShareCodec::decode(QStringLiteral("https://example.com/abc"), decoded, err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(decoded.isEmpty());
}

TEST(ShareCodecTest, DecodeRejectsEmptyPayload) {
    QString decoded, err;
    EXPECT_FALSE(ShareCodec::decode(QString::fromLatin1(ShareCodec::kSharePrefix), decoded, err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(ShareCodecTest, DecodeRejectsCorruptedPayload) {
    QString url = ShareCodec::encode(QStringLiteral("var a = 1;\nprint(a);\n"));
    // 篡改负载中段字符（保持 base64url 字符集合法，破坏 zlib 流）
    QString corrupted = url;
    int mid = url.size() - 8;
    corrupted[mid] = (corrupted[mid] == QLatin1Char('A')) ? QLatin1Char('B') : QLatin1Char('A');
    QString decoded, err;
    // 篡改后要么 base64 仍合法但解压失败，要么直接解码失败——两者都必须报错
    EXPECT_FALSE(ShareCodec::decode(corrupted, decoded, err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(ShareCodecTest, DecodeRejectsTruncatedPayload) {
    QString url = ShareCodec::encode(QStringLiteral("fun f() { return 1; }\nprint(f());\n"));
    QString truncated = url.left(url.size() / 2);
    QString decoded, err;
    EXPECT_FALSE(ShareCodec::decode(truncated, decoded, err));
    EXPECT_FALSE(err.isEmpty());
}
