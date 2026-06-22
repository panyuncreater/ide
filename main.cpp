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
    try {
        Ide w;
        w.show();
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
