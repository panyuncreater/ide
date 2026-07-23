#include "ide.h"

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTextStream>

#include "FluentGlobal.h"
#include "Theme.h"
#include "common/CrashHandler.h" // R128: 崩溃报告系统
#include "gui/CrashReportDialog.h" // R128: 启动时检测上次崩溃
#include "gui/GuiTextUtils.h" // R75: uiFont() 跨机器字体回退链
#include "gui/I18n.h"         // D2: i18n 翻译辅助层

// ============================================================
// MiniLang IDE 程序入口
// 第五轮重构：接入 QFluentKit 主题系统 + DPI 自适应
// D2: 接入 i18n 翻译加载
// R128: 安装 CrashHandler + 启动时检测上次崩溃
// ============================================================

namespace {
// 持久化"忽略此崩溃报告"的设置键
// 仅当用户在 CrashReportDialog 勾选"不再提示"时写入
constexpr const char* kCrashIgnoreKey = "crash/ignoreLastReport";
} // anonymous namespace

int main(int argc, char* argv[]) {
    // R128: 安装崩溃处理器（在 QApplication 之前，捕获启动早期崩溃）
    // 写入路径：<temp>/minilang-crashdumps/crash-<signal>-<timestamp>.{dmp|txt}
    minilang::CrashHandler::instance().install();
    // 启动时清理 30 天以上的旧报告
    minilang::CrashHandler::cleanupOldReports(30);

    // DPI 自适应：PassThrough 保留分数缩放（1.25x/1.5x），高分辨率屏幕清晰不模糊
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

    QApplication a(argc, argv);

    // 设置应用程序信息
    a.setApplicationName("MiniLang IDE");
    a.setApplicationVersion("1.0");
    a.setOrganizationName("MiniLang");

    // 设置应用程序图标（Windows 任务栏/标题栏）
    // 使用 QIcon 加载 SVG，Qt6 内置 QSvgRenderer 自动渲染
    a.setWindowIcon(QIcon(":/icons/minilang_icon.png"));

    // D2: 加载翻译（MINILANG_ENABLE_I18N=ON 时查找 minilang_<locale>.qm；
    //     未启用时为 no-op，下方 mlTr 调用退化为 QString::fromUtf8）
    loadMiniLangTranslations();

    // R128: 检测上次会话是否崩溃
    // - 读取 QSettings 判断用户是否选择"忽略此报告"
    // - 未忽略则弹出 CrashReportDialog
    // - 用户可查看/复制/删除/忽略报告
    {
        QSettings settings;
        bool ignoreCrash = settings.value(kCrashIgnoreKey, false).toBool();
        if (!ignoreCrash) {
            auto report = minilang::CrashHandler::lastCrashReport();
            if (report.valid) {
                minilang::gui::CrashReportDialog dlg(report);
                dlg.exec();
                if (dlg.ignoreRequested()) {
                    settings.setValue(kCrashIgnoreKey, true);
                } else if (dlg.reportDeleted()) {
                    // 用户已删除报告，清除忽略标志
                    settings.remove(kCrashIgnoreKey);
                }
            }
        } else {
            // 用户选择忽略，消费掉报告但不弹窗
            minilang::CrashHandler::consumeLastCrashReport();
            settings.remove(kCrashIgnoreKey);
        }
    }

    // 使用 Fusion 作为基础样式（QFluentKit QSS 覆盖其调色板驱动的背景）
    // R76 fix: Fusion 插件可能未部署，检查返回值防止空指针回退到 windowsvista
    if (auto* fusion = QStyleFactory::create("Fusion")) {
        a.setStyle(fusion);
    } else {
        qWarning("Fusion style plugin not found, falling back to default style");
    }

    // R76 fix: 强制浅色方案，防止目标机器系统深色模式污染调色板
    // 根因：Qt6.5+ 在系统深色模式下会将默认调色板 Text/WindowText 设为浅色，
    // 配合本项目的白色背景导致文字不可见。此处锁定浅色方案 + 显式设置
    // 全局调色板文本角色，确保跨机器渲染一致。
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    a.styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
    {
        QPalette lightPal = a.palette();
        lightPal.setColor(QPalette::Window, QColor(0xFF, 0xFF, 0xFF));
        lightPal.setColor(QPalette::WindowText, QColor(0x1E, 0x1E, 0x1E));
        lightPal.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
        lightPal.setColor(QPalette::AlternateBase, QColor(0xF5, 0xF5, 0xF5));
        lightPal.setColor(QPalette::Text, QColor(0x1E, 0x1E, 0x1E));
        lightPal.setColor(QPalette::Button, QColor(0xFF, 0xFF, 0xFF));
        lightPal.setColor(QPalette::ButtonText, QColor(0x1E, 0x1E, 0x1E));
        lightPal.setColor(QPalette::Highlight, QColor(0xF5, 0xF5, 0xF5));
        lightPal.setColor(QPalette::HighlightedText, QColor(0x1E, 0x1E, 0x1E));
        lightPal.setColor(QPalette::ToolTipBase, QColor(0xFF, 0xFF, 0xFF));
        lightPal.setColor(QPalette::ToolTipText, QColor(0x1E, 0x1E, 0x1E));
        a.setPalette(lightPal);
    }

    // 全局字体（R75: 跨机器字体一致性）
    // 原 setPixelSize(14) + 短回退链在缺中文字体的机器上中文 UI 显示不清。
    // 改用 GuiTextUtils::uiFont()：完整中英文回退链 + pointSize 物理单位，
    // 随屏幕 DPI 自适应，保证不同机器物理大小一致。
    a.setFont(GuiTextUtils::uiFont(10)); // 10pt ≈ 13.3px @ 96dpi

    // 主题模式：由 Ide 构造函数从 QSettings 读取并应用（默认 light，用户可切换并持久化）。
    // 此处不再硬编码 LIGHT，避免覆盖用户上次选择的主题。

    // MAIN-01 fix: 顶层异常捕获保护
    // 崩溃修复: Ide 改为堆分配。QMainWindow 是大型对象，栈分配会在 MSVC /RTC1 Debug
    // 构建中放置栈 cookie，若析构期间发生任何越界写（如 worker 线程通过 QueuedConnection
    // 回写已析构成员）会触发 "Stack around the variable 'w' was corrupted"。
    // 堆分配消除该栈检查，且符合 Qt 窗口组件的惯用模式。
    try {
        auto w = std::make_unique<Ide>();
        w->show();
        int ret = a.exec();
        // 应用正常退出时卸载崩溃处理器（避免析构期异常触发 minidump）
        minilang::CrashHandler::instance().uninstall();
        return ret;
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, mlTr("MiniLang IDE - 启动错误"), QString::fromStdString(e.what()));
        return 1;
    } catch (...) {
        QMessageBox::critical(nullptr, mlTr("MiniLang IDE - 启动错误"), mlTr("未知的启动异常"));
        return 1;
    }
}
