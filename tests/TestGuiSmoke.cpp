// ============================================================
// TestGuiSmoke.cpp — GUI 冒烟测试
// ------------------------------------------------------------
// 验证关键 GUI 部件可在测试进程中被安全实例化，不发生崩溃。
//
// 框架说明：
//   本项目使用 GoogleTest 作为测试运行器（见 tests/main.cpp），
//   而非 QTest 自带的 main()。因此本文件以 gtest TEST_F 编写，
//   并在 fixture 中自行确保 QApplication 存在——任何 QWidget
//   构造前必须先有 QApplication 实例。
//
// 链接依赖（见 tests/CMakeLists.txt）：
//   - Qt6::Widgets（CodeEditor / QWidget）
//   - Qt6::Test（如需扩展 QTest 键鼠模拟可用，当前冒烟用例未直接使用）
//   CodeEditor.cpp 依赖 Qt6::Widgets 与多个 GUI 组件，加入测试
//   目标时需一并编译 gui/CodeEditor.cpp 及其依赖（GuiTextUtils / I18n）。
// ============================================================

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QWidget>

#include "gui/CodeEditor.h"

namespace {

// ============================================================
// Qt 测试环境：懒初始化 QApplication
// ------------------------------------------------------------
// QApplication 必须在任何 QWidget 构造前创建。
// 使用静态局部变量实现线程安全的懒初始化，且永不销毁
// （测试进程退出时由 OS 回收）。若已有 QCoreApplication 实例
// （如其他测试创建），直接复用 qApp。
// ============================================================
QApplication* ensureQApp() {
    if (QCoreApplication::instance())
        return qApp;
    static int argc = 1;
    static char arg0[] = "minilang_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

/// GUI 冒烟测试 fixture：保证每个用例运行前 QApplication 已就绪
class GuiSmokeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
    }
};

} // namespace

// ------------------------------------------------------------
// 用例 1：基础 QWidget 健全性检查
// 验证 Qt Widgets 运行时（QApplication + 平台插件）可用。
// ------------------------------------------------------------
TEST_F(GuiSmokeTest, QWidgetSanity) {
    QWidget widget;
    EXPECT_NE(&widget, nullptr);
    widget.setWindowTitle(QStringLiteral("smoke"));
    EXPECT_EQ(widget.windowTitle(), QStringLiteral("smoke"));
}

// ------------------------------------------------------------
// 用例 2：CodeEditor 实例化
// 仅构造 + 验证非空，确认默认构造路径不崩溃。
// ------------------------------------------------------------
TEST_F(GuiSmokeTest, CodeEditorInstantiation) {
    CodeEditor editor;
    EXPECT_NE(&editor, nullptr);
}

// ------------------------------------------------------------
// 用例 3：CodeEditor 文本读写
// setPlainText / toPlainText 往返一致。
// ------------------------------------------------------------
TEST_F(GuiSmokeTest, CodeEditorSetGetText) {
    CodeEditor editor;
    editor.setPlainText(QStringLiteral("var x = 1;"));
    EXPECT_EQ(editor.toPlainText(), QStringLiteral("var x = 1;"));
}

// ------------------------------------------------------------
// 用例 4：CodeEditor 行号区域初始状态
// 验证初始（空文档）状态下查询行号区域宽度不崩溃，且宽度非负。
// ------------------------------------------------------------
TEST_F(GuiSmokeTest, CodeEditorLineNumberArea) {
    CodeEditor editor;
    // 空文档初始状态：行号区域宽度应为非负值且不触发崩溃
    const int width = editor.lineNumberAreaWidth();
    EXPECT_GE(width, 0);
}
