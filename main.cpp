#include "ide.h"

#include <QApplication>

// ============================================================
// MiniLang IDE 程序入口
// ============================================================

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    // 设置应用程序信息
    a.setApplicationName("MiniLang IDE");
    a.setApplicationVersion("1.0");
    a.setOrganizationName("MiniLang");

    // 设置全局样式
    a.setStyleSheet(R"(
        QMainWindow {
            background-color: #F5F5F5;
        }
        QTabWidget::pane {
            border: 1px solid #CCC;
            background: white;
        }
        QTabBar::tab {
            padding: 6px 14px;
            border: 1px solid #CCC;
            border-bottom: none;
            background: #EEE;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: white;
            border-bottom: 2px solid #4A90D9;
            color: #333;
        }
        QTabBar::tab:hover {
            background: #E0E0E0;
        }
        QToolBar {
            background: #FAFAFA;
            border-bottom: 1px solid #DDD;
            spacing: 4px;
            padding: 2px;
        }
        QToolBar QToolButton {
            padding: 4px 10px;
            border: 1px solid transparent;
            border-radius: 3px;
            background: transparent;
            font-size: 13px;
        }
        QToolBar QToolButton:hover {
            background: #E8E8E8;
            border: 1px solid #CCC;
        }
        QToolBar QToolButton:pressed {
            background: #D0D0D0;
        }
        QToolBar QToolButton:disabled {
            color: #AAA;
        }
        QSplitter::handle {
            background: #DDD;
        }
        QSplitter::handle:horizontal {
            width: 3px;
        }
        QSplitter::handle:vertical {
            height: 3px;
        }
        QTableWidget {
            gridline-color: #DDD;
            selection-background-color: #4A90D9;
            selection-color: white;
        }
        QTableWidget::item {
            padding: 2px 6px;
        }
        QHeaderView::section {
            background: #F0F0F0;
            padding: 4px 8px;
            border: 1px solid #DDD;
            font-weight: bold;
        }
        QTreeWidget {
            border: 1px solid #DDD;
            alternate-background-color: #F8F8F8;
        }
        QListWidget {
            border: 1px solid #DDD;
            alternate-background-color: #F8F8F8;
        }
        QStatusBar {
            background: #F0F0F0;
            border-top: 1px solid #DDD;
        }
    )");

    Ide w;
    w.show();
    return a.exec();
}
