#include "ide.h"

#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QStyleFactory>

#include "Theme.h"
#include "FluentGlobal.h"

// ============================================================
// MiniLang IDE 程序入口
// 第五轮重构：接入 QFluentKit 主题系统 + DPI 自适应
// ============================================================

int main(int argc, char *argv[]) {
    // DPI 自适应：PassThrough 保留分数缩放（1.25x/1.5x），高分辨率屏幕清晰不模糊
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    QApplication a(argc, argv);

    // 设置应用程序信息
    a.setApplicationName("MiniLang IDE");
    a.setApplicationVersion("1.0");
    a.setOrganizationName("MiniLang");

    // 使用 Fusion 作为基础样式（QFluentKit QSS 覆盖其调色板驱动的背景）
    a.setStyle(QStyleFactory::create("Fusion"));

    // 全局字体（与 QFluentKit Theme 内部字体族对齐）
    QFont font;
    font.setFamilies({"Microsoft YaHei", "PingFang SC", "Segoe UI"});
    font.setPixelSize(14);
    a.setFont(font);

    // 读取上次主题选择，在创建窗口前设置 QFluentKit 全局主题
    {
        QSettings settings("MiniLang", "MiniLang IDE");
        bool dark = settings.value("theme/dark", false).toBool();
        Theme::setThemeMode(dark ? Fluent::ThemeMode::DARK : Fluent::ThemeMode::LIGHT);
    }

    // MAIN-01 fix: 顶层异常捕获保护
    // 崩溃修复: Ide 改为堆分配。QMainWindow 是大型对象，栈分配会在 MSVC /RTC1 Debug
    // 构建中放置栈 cookie，若析构期间发生任何越界写（如 worker 线程通过 QueuedConnection
    // 回写已析构成员）会触发 "Stack around the variable 'w' was corrupted"。
    // 堆分配消除该栈检查，且符合 Qt 窗口组件的惯用模式。
    try {
        auto w = std::make_unique<Ide>();
        w->show();
        return a.exec();
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "MiniLang IDE - 启动错误",
                              QString::fromStdString(e.what()));
        return 1;
    } catch (...) {
        QMessageBox::critical(nullptr, "MiniLang IDE - 启动错误",
                              "未知的启动异常");
        return 1;
    }
}
