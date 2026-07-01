// ============================================================
// MiniLang 单元测试主入口
// ------------------------------------------------------------
// 注册全局 TestEnvironment，在每个测试用例前后清理全局单例
// 状态（GcManager / StringIntern），防止跨测试累积导致的
// 内存持续增长（T-4 fix）。
//
// 根因：
//   1. GcManager 是进程级单例，其 tracked_ vector 在非
//      Interpreter 后端（VM/RegVM）运行后不被自动清理
//   2. StringIntern 池永不释放
//   3. MSVC Debug CRT 默认保留已释放内存块用于泄漏检测
//      （_CRTDBG_DELAY_FREE_MEM_DF），导致 tasklist 报告的
//      内存只增不减
//
// 本文件三项修复：
//   - 全局 TestEventListener 在每个测试后 reset GcManager + StringIntern
//   - 禁用 Logger 控制台输出（减少 I/O 缓冲累积）
//   - 关闭 _CRTDBG_DELAY_FREE_MEM_DF，让 Debug 堆释放的内存归还 OS
// ============================================================

#include <gtest/gtest.h>
#include "interpreter/GcManager.h"
#include "interpreter/StringIntern.h"
#include "common/Logger.h"

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace {

class MiniLangTestEnv : public ::testing::Environment {
public:
    void SetUp() override {
        // 测试全局：禁用 Logger 控制台输出，减少 I/O 开销与缓冲累积
        Logger::instance().setLevel(LogLevel::NONE);
        Logger::instance().setConsoleOutput(false);

#ifdef _MSC_VER
        // T-4 fix: 关闭 Debug 堆的 "延迟释放内存" 标志。
        // 默认情况下 MSVC Debug CRT 保留已 free/delete 的内存块用于
        // 泄漏检测，导致 tasklist 报告的内存只增不减。
        // 测试场景下不需要泄漏检测（gtest 自身会报告失败），
        // 关闭此标志让释放的内存立即归还 OS。
        int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
        flags &= ~_CRTDBG_DELAY_FREE_MEM_DF;
        _CrtSetDbgFlag(flags);
#endif
    }

    void TearDown() override {
        // 全局测试结束：清理所有全局单例状态
        GcManager::instance().reset();
        StringIntern::clear();
    }
};

class MiniLangTestListener : public ::testing::EmptyTestEventListener {
public:
    void OnTestEnd(const ::testing::TestInfo&) override {
        // 每个测试用例结束后清理全局单例状态
        // GcManager::reset() 仅清空跟踪结构，不释放节点（节点由 refCount 管理）
        GcManager::instance().reset();
        // StringIntern::clear() 清空驻留池，释放字符串内存
        StringIntern::clear();
    }
};

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new MiniLangTestEnv);
    ::testing::TestEventListeners& listeners = ::testing::UnitTest::GetInstance()->listeners();
    listeners.Append(new MiniLangTestListener);
    return RUN_ALL_TESTS();
}
