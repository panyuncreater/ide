// ============================================================
// tests/TestCApi.cpp - libminilang C API 测试（拓展二期·平台）
// ------------------------------------------------------------
// 通过纯 C 接口（extern "C" ABI）验证：
//   - 上下文生命周期（create/destroy，NULL 安全）
//   - 三后端 eval 输出一致性
//   - 错误分类（编译错误 vs 运行时错误）与错误消息前缀
//   - 输出缓冲逐次覆盖语义
// ============================================================
#include "capi/minilang_capi.h"

#include <gtest/gtest.h>

#include <string>

TEST(CApiTest, CreateDestroyLifecycle) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(minilang_get_backend(ctx), MINILANG_BACKEND_INTERPRETER);
    minilang_destroy(ctx);
    // NULL 安全
    minilang_destroy(nullptr);
}

TEST(CApiTest, NullArgumentsAreSafe) {
    EXPECT_EQ(minilang_eval(nullptr, "print(1);"), MINILANG_ERR_INVALID);
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(minilang_eval(ctx, nullptr), MINILANG_ERR_INVALID);
    EXPECT_STREQ(minilang_get_output(nullptr), "");
    EXPECT_STREQ(minilang_get_error(nullptr), "");
    EXPECT_EQ(minilang_set_backend(nullptr, MINILANG_BACKEND_STACKVM), MINILANG_ERR_INVALID);
    minilang_destroy(ctx);
}

TEST(CApiTest, EvalOutputConsistentAcrossThreeBackends) {
    const char* src = "fun fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); }\n"
                      "print(fib(10));\nprint(\"done\");";
    const minilang_backend backends[] = {MINILANG_BACKEND_INTERPRETER, MINILANG_BACKEND_STACKVM,
                                         MINILANG_BACKEND_REGISTERVM};
    for (minilang_backend b : backends) {
        minilang_context* ctx = minilang_create();
        ASSERT_NE(ctx, nullptr);
        ASSERT_EQ(minilang_set_backend(ctx, b), MINILANG_OK);
        EXPECT_EQ(minilang_get_backend(ctx), b);
        ASSERT_EQ(minilang_eval(ctx, src), MINILANG_OK) << "backend=" << b << " err=" << minilang_get_error(ctx);
        std::string out = minilang_get_output(ctx);
        EXPECT_NE(out.find("55"), std::string::npos) << "backend=" << b;
        EXPECT_NE(out.find("done"), std::string::npos) << "backend=" << b;
        EXPECT_STREQ(minilang_get_error(ctx), "");
        minilang_destroy(ctx);
    }
}

TEST(CApiTest, CompileErrorClassified) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(minilang_eval(ctx, "var x = ;"), MINILANG_ERR_COMPILE);
    EXPECT_NE(std::string(minilang_get_error(ctx)).size(), 0u);
    minilang_destroy(ctx);
}

TEST(CApiTest, RuntimeErrorClassified) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(minilang_eval(ctx, "var a = [1]; print(a[10]);"), MINILANG_ERR_RUNTIME);
    std::string err = minilang_get_error(ctx);
    // 注：不用 u8 前缀——C++20 下 u8 字面量是 char8_t*，std::string::find 不接受；
    // 源文件 UTF-8 + /utf-8 编译选项保证普通字面量字节序即 UTF-8
    EXPECT_NE(err.find("运行时错误"), std::string::npos) << err;
    minilang_destroy(ctx);
}

TEST(CApiTest, OutputBufferOverwrittenPerEval) {
    minilang_context* ctx = minilang_create();
    ASSERT_NE(ctx, nullptr);
    ASSERT_EQ(minilang_eval(ctx, "print(\"first\");"), MINILANG_OK);
    EXPECT_NE(std::string(minilang_get_output(ctx)).find("first"), std::string::npos);
    ASSERT_EQ(minilang_eval(ctx, "print(\"second\");"), MINILANG_OK);
    std::string out2 = minilang_get_output(ctx);
    EXPECT_NE(out2.find("second"), std::string::npos);
    EXPECT_EQ(out2.find("first"), std::string::npos); // 覆盖而非累积
    // 出错后再成功：错误缓冲被清空
    EXPECT_EQ(minilang_eval(ctx, "var x = ;"), MINILANG_ERR_COMPILE);
    ASSERT_EQ(minilang_eval(ctx, "print(3);"), MINILANG_OK);
    EXPECT_STREQ(minilang_get_error(ctx), "");
    minilang_destroy(ctx);
}

TEST(CApiTest, VersionString) {
    std::string v = minilang_version();
    EXPECT_NE(v.find("libminilang"), std::string::npos);
}
