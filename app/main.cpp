#include "ide.h"

#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>

// ============================================================
// MiniLang IDE 程序入口
// ============================================================

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    // 设置应用程序信息
    a.setApplicationName("MiniLang IDE");
    a.setApplicationVersion("1.0");
    a.setOrganizationName("MiniLang");

    // 加载集中式样式表
    {
        QFile styleFile(":/styles.qss");
        if (styleFile.open(QFile::ReadOnly | QFile::Text)) {
            QTextStream ts(&styleFile);
            a.setStyleSheet(ts.readAll());
            styleFile.close();
        } else {
            // 回退：尝试从应用程序目录加载
            QFile fallback(QApplication::applicationDirPath() + "/styles.qss");
            if (fallback.open(QFile::ReadOnly | QFile::Text)) {
                QTextStream ts(&fallback);
                a.setStyleSheet(ts.readAll());
                fallback.close();
            }
        }
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
