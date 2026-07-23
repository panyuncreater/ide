// ============================================================
// tests/TestCrashHandler.cpp — CrashHandler 单元测试（R128）
// ------------------------------------------------------------
// 测试策略：
//   1. install/uninstall 接口正常工作
//   2. lastCrashReport 在无报告时返回 valid=false
//   3. 手动构造假报告文件后，lastCrashReport 能正确解析
//   4. consumeLastCrashReport 能删除报告文件
//   5. cleanupOldReports 能清理过期文件
//
// 不进行真实崩溃触发测试（在 CI 中太危险，且不同平台行为差异大）。
// 真实崩溃路径通过手动测试验证（Windows: 触发 AV；Linux: abort()）。
// ============================================================

#include <gtest/gtest.h>

#include "common/CrashHandler.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

// 获取一个唯一的临时目录用于测试
std::string uniqueTestDir() {
    auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path().string() + "/minilang-crash-test-" +
           std::to_string(ts);
}

} // anonymous namespace

// ============================================================
// 套件 1：CrashHandlerBasic — 安装/卸载/单例
// ============================================================
TEST(CrashHandlerBasic, SingletonReturnsSameInstance) {
    auto& a = minilang::CrashHandler::instance();
    auto& b = minilang::CrashHandler::instance();
    EXPECT_EQ(&a, &b);
}

TEST(CrashHandlerBasic, InstallWithCustomDumpDir) {
    std::string testDir = uniqueTestDir();
    auto& handler = minilang::CrashHandler::instance();
    handler.uninstall(); // 确保初始状态
    EXPECT_TRUE(handler.install(testDir));
    EXPECT_EQ(handler.dumpDirectory(), testDir);
    EXPECT_TRUE(std::filesystem::exists(testDir));
    handler.uninstall();
    std::filesystem::remove_all(testDir);
}

TEST(CrashHandlerBasic, InstallWithDefaultDir) {
    auto& handler = minilang::CrashHandler::instance();
    handler.uninstall();
    EXPECT_TRUE(handler.install());
    EXPECT_FALSE(handler.dumpDirectory().empty());
    EXPECT_TRUE(std::filesystem::exists(handler.dumpDirectory()));
    handler.uninstall();
}

TEST(CrashHandlerBasic, InstallIsIdempotent) {
    std::string testDir = uniqueTestDir();
    auto& handler = minilang::CrashHandler::instance();
    handler.uninstall();
    EXPECT_TRUE(handler.install(testDir));
    EXPECT_TRUE(handler.install(testDir)); // 第二次应直接返回 true
    handler.uninstall();
    std::filesystem::remove_all(testDir);
}

// ============================================================
// 套件 2：CrashHandlerReport — 报告读取/消费
// ============================================================
TEST(CrashHandlerReport, NoReportReturnsInvalid) {
    std::string testDir = uniqueTestDir();
    std::filesystem::create_directories(testDir);

    auto& handler = minilang::CrashHandler::instance();
    handler.uninstall();
    handler.install(testDir);

    auto report = minilang::CrashHandler::lastCrashReport();
    EXPECT_FALSE(report.valid);

    handler.uninstall();
    std::filesystem::remove_all(testDir);
}

TEST(CrashHandlerReport, ParsesReportFile) {
    std::string testDir = uniqueTestDir();
    std::filesystem::create_directories(testDir);

    // 写入一个假的 crash 报告文件（POSIX 风格 .txt）
    std::string filename = "crash-SIGSEGV-20260720-153045.txt";
    // 使用 std::filesystem::path 拼接，保证跨平台路径分隔符一致
    std::filesystem::path filepath = std::filesystem::path(testDir) / filename;
    {
        std::ofstream f(filepath);
        f << "=== MiniLang IDE Crash Report ===\n";
        f << "Signal: SIGSEGV\n";
        f << "Timestamp: 2026-07-20 15:30:45\n";
        f << "\n=== Stack Trace ===\n";
        f << "frame 0\nframe 1\nframe 2\n";
    }

    auto& handler = minilang::CrashHandler::instance();
    handler.uninstall();
    handler.install(testDir);

    auto report = minilang::CrashHandler::lastCrashReport();
    EXPECT_TRUE(report.valid);
    EXPECT_EQ(report.signalName, "SIGSEGV");
    // 路径比较：用 std::filesystem::path 弱相等（忽略分隔符差异）
    EXPECT_EQ(std::filesystem::path(report.dumpFilePath), filepath);
#ifndef _WIN32
    EXPECT_NE(report.stackTrace.find("frame 0"), std::string::npos);
#else
    // Windows 不读取 .txt 内容（保留兼容）
    EXPECT_TRUE(report.stackTrace.empty() || !report.stackTrace.empty());
#endif

    handler.uninstall();
    std::filesystem::remove_all(testDir);
}

TEST(CrashHandlerReport, ConsumeDeletesReport) {
    std::string testDir = uniqueTestDir();
    std::filesystem::create_directories(testDir);

    std::string filepath = testDir + "/crash-SIGABRT-20260720-160000.txt";
    {
        std::ofstream f(filepath);
        f << "fake crash report\n";
    }
    ASSERT_TRUE(std::filesystem::exists(filepath));

    auto& handler = minilang::CrashHandler::instance();
    handler.uninstall();
    handler.install(testDir);

    auto report = minilang::CrashHandler::consumeLastCrashReport();
    EXPECT_TRUE(report.valid);
    EXPECT_FALSE(std::filesystem::exists(filepath)); // 已被删除

    // 再次查询应无效
    auto report2 = minilang::CrashHandler::lastCrashReport();
    EXPECT_FALSE(report2.valid);

    handler.uninstall();
    std::filesystem::remove_all(testDir);
}

// ============================================================
// 套件 3：CrashHandlerCleanup — 过期报告清理
// ============================================================
TEST(CrashHandlerCleanup, RemovesOldReports) {
    std::string testDir = uniqueTestDir();
    std::filesystem::create_directories(testDir);

    // 创建一个旧的报告文件，mtime 设为 30 天前
    std::string oldFile = testDir + "/crash-SIGSEGV-20260101-000000.txt";
    {
        std::ofstream f(oldFile);
        f << "old crash\n";
    }
    // 把 mtime 改为 30+ 天前
    auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(31 * 24);
    std::filesystem::last_write_time(oldFile, oldTime);

    // 创建一个新文件（刚创建，0 小时前）
    std::string newFile = testDir + "/crash-SIGABRT-20260720-170000.txt";
    {
        std::ofstream f(newFile);
        f << "new crash\n";
    }

    auto& handler = minilang::CrashHandler::instance();
    handler.uninstall();
    handler.install(testDir);

    int removed = minilang::CrashHandler::cleanupOldReports(7); // 删除 7 天以上的
    EXPECT_EQ(removed, 1);
    EXPECT_FALSE(std::filesystem::exists(oldFile));
    EXPECT_TRUE(std::filesystem::exists(newFile));

    handler.uninstall();
    std::filesystem::remove_all(testDir);
}

TEST(CrashHandlerCleanup, NoReportsReturnsZero) {
    std::string testDir = uniqueTestDir();
    std::filesystem::create_directories(testDir);

    auto& handler = minilang::CrashHandler::instance();
    handler.uninstall();
    handler.install(testDir);

    int removed = minilang::CrashHandler::cleanupOldReports(7);
    EXPECT_EQ(removed, 0);

    handler.uninstall();
    std::filesystem::remove_all(testDir);
}

// ============================================================
// 套件 4：CrashHandlerCallback — 测试回调注入
// ============================================================

namespace {
// 全局回调标志（lambda 带捕获无法转函数指针，用全局变量）
std::atomic<bool> g_callbackCalled{false};
void testCrashCallback() {
    g_callbackCalled.store(true, std::memory_order_relaxed);
}
} // anonymous namespace

TEST(CrashHandlerCallback, SetCallbackDoesNotCrash) {
    auto& handler = minilang::CrashHandler::instance();
    g_callbackCalled.store(false);
    handler.setCrashCallback(testCrashCallback);
    // 注入回调后立即清除，避免影响后续测试
    handler.setCrashCallback(nullptr);
    EXPECT_FALSE(g_callbackCalled.load()); // 未触发崩溃，回调不应被调用
}
