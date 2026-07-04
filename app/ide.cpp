#include "ide.h"
#include "Logger.h"
#include "common/RuntimeLimits.h"
#include "common/SpellChecker.h"
#include "gui/GuiTextUtils.h"
#include "gui/PanelAnimator.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QAbstractItemView>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QHeaderView>
#include <QApplication>
#include <QColor>
#include <QScrollBar>
#include <QFile>
#include <QTextStream>
#include <QMenuBar>
#include <QFileInfo>
#include <QInputDialog>
#include <QDialog>
#include <QMouseEvent>
#include <QShortcut>
#include <QSettings>
#include <QRegularExpression>
#include <QSet>
#include <QHash>
#include <QDir>
#include <QToolButton>
#include <QMenu>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QSizePolicy>
#include <QToolTip>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QAction>
#include <QToolBar>
#include <QTableWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QTextDocument>
#include <QTextEdit>
#include <QStatusBar>
#include <QTimer>
#include <QStyledItemDelegate>
#include <QMenuBar>
#include <QGraphicsDropShadowEffect>
#include <sstream>
#include <algorithm>

#ifdef Q_OS_WIN
// WIN32_LEAN_AND_MEAN + NOMINMAX 减少 windows.h 宏污染
// （避免 ERROR/WARNING/min/max 宏与 QFluentKit 枚举及 std::min/max 冲突）
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <windowsx.h>
// windows.h 仍可能定义 ERROR/WARNING，与 InfoBar::Type 枚举冲突，必须取消
#  ifdef ERROR
#    undef ERROR
#  endif
#  ifdef WARNING
#    undef WARNING
#  endif
#endif

// ADS headers
#include "DockManager.h"
#include "DockWidget.h"
#include "DockAreaWidget.h"

// QFluentKit Theme
#include "Theme.h"

// QFluentKit components (sixth-round UI refactor → twelfth-round unified title bar)
#include "QFluent/Navigation/Pivot.h"
#include "QFluent/Menu/RoundMenu.h"
#include "QFluent/Flyout.h"
#include "QFluent/InfoBar.h"
#include "QFluent/PushButton.h"
#include "QFluent/ToolButton.h"
#include "QFluent/ComboBox.h"
#include "QFluent/TableView.h"
#include "QFluent/ScrollBar.h"
#include "QFluent/Progress/IndeterminateProgressBar.h"
#include "QFluent/TabBar.h"
#include "StyleSheet.h"
#include "FluentIcon.h"

// GUI: ActivityBar (sixth-round)
#include "gui/ActivityBar.h"

// ============================================================
// Static helpers
// ============================================================

static void populateDirChildren(QTreeWidget* tree, QTreeWidgetItem* parentItem,
                                const QString& dirPath, int depth) {
    if (depth > 8) return;
    QDir dir(dirPath);
    if (!dir.exists()) return;

    QStringList filters;
    filters << "*.mini" << "*.ml";
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        auto* item = new QTreeWidgetItem(parentItem);
        item->setText(0, fi.fileName());
        // 第九轮：文件名显示不全时悬浮显示完整路径 tooltip
        item->setToolTip(0, fi.absoluteFilePath());
        QStyle::StandardPixmap icon = QStyle::SP_FileIcon;
        if (fi.suffix() == "ml") icon = QStyle::SP_FileDialogContentsView;
        item->setIcon(0, tree->style()->standardIcon(icon));
        item->setData(0, Qt::UserRole, fi.absoluteFilePath());
        item->setData(0, Qt::UserRole + 1, false);
    }

    QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fi : dirs) {
        auto* item = new QTreeWidgetItem(parentItem);
        item->setText(0, fi.fileName());
        item->setToolTip(0, fi.absoluteFilePath());
        item->setIcon(0, tree->style()->standardIcon(QStyle::SP_DirIcon));
        item->setData(0, Qt::UserRole, fi.absoluteFilePath());
        item->setData(0, Qt::UserRole + 1, true);
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
}

static QString resolveTreeContextMenuTargetDir(QTreeWidget* tree, QTreeWidgetItem* item) {
    if (!item) return QString();
    bool isDir = item->data(0, Qt::UserRole + 1).toBool();
    if (isDir) return item->data(0, Qt::UserRole).toString();
    QTreeWidgetItem* parent = item->parent();
    if (parent) return parent->data(0, Qt::UserRole).toString();
    return item->data(0, Qt::UserRole).toString();
}

/// Extract a quoted identifier from an error message (e.g., "未定义的变量 'it'" -> "it")
static std::string extractQuotedIdentifier(const std::string& msg) {
    // Try single quotes first
    size_t sq1 = msg.find('\'');
    if (sq1 != std::string::npos) {
        size_t sq2 = msg.find('\'', sq1 + 1);
        if (sq2 != std::string::npos && sq2 > sq1 + 1) {
            return msg.substr(sq1 + 1, sq2 - sq1 - 1);
        }
    }
    // Try double quotes
    size_t dq1 = msg.find('"');
    if (dq1 != std::string::npos) {
        size_t dq2 = msg.find('"', dq1 + 1);
        if (dq2 != std::string::npos && dq2 > dq1 + 1) {
            return msg.substr(dq1 + 1, dq2 - dq1 - 1);
        }
    }
    return {};
}

// ============================================================
// CleanToolButton — 第八轮：完全自绘工具栏图标按钮
// 不调用 QToolButton::paintEvent，杜绝文字/菜单指示器/焦点框/QSS 边框残留
// 28px 固定高度，16x16 图标居中，基线对齐
// ============================================================
class CleanToolButton : public TransparentToolButton {
public:
    explicit CleanToolButton(Fluent::IconType type, QWidget* parent = nullptr)
        : TransparentToolButton(type, parent) {
        setFixedSize(28, 28);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setFocusPolicy(Qt::NoFocus);
        setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // 背景：checked > pressed > hover
        if (isChecked()) {
            p.fillRect(rect(), QColor(0, 102, 184, 30));
        } else if (isDown()) {
            p.fillRect(rect(), QColor(0, 0, 0, 30));
        } else if (underMouse()) {
            p.fillRect(rect(), QColor(0, 0, 0, 12));
        }
        // 图标 16x16 居中（与文字垂直居中、基线对齐）
        QRect ir((width() - 16) / 2, (height() - 16) / 2, 16, 16);
        if (!isEnabled()) p.setOpacity(0.4);
        fluentIcon().paint(&p, ir);
    }
};

// ============================================================
// RichTextItemDelegate — 第八轮：QListWidget HTML 富文本渲染代理
// 用于字节码列表的语法高亮（opcode/常量/注释多色渲染）
// ============================================================
class RichTextItemDelegate : public QStyledItemDelegate {
public:
    static constexpr int kHtmlRole = Qt::UserRole + 2;

    explicit RichTextItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();

        // 背景：选中 / 交替行
        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, QColor("#cfe4f5"));
        } else if (option.features & QStyleOptionViewItem::Alternate) {
            painter->fillRect(option.rect, QColor("#ffffff"));
        } else {
            painter->fillRect(option.rect, QColor("#f8f8f8"));
        }

        QString html = index.data(kHtmlRole).toString();
        if (html.isEmpty()) {
            // 回退到普通文本
            QStyledItemDelegate::paint(painter, option, index);
            painter->restore();
            return;
        }

        QTextDocument doc;
        doc.setDefaultFont(option.font);
        doc.setDocumentMargin(2);
        doc.setHtml(html);

        painter->translate(option.rect.left() + 6, option.rect.top());
        QRect clip(0, 0, option.rect.width() - 8, option.rect.height());
        doc.setTextWidth(clip.width());
        painter->setClipRect(clip);
        doc.drawContents(painter);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
        QString html = index.data(kHtmlRole).toString();
        if (html.isEmpty()) {
            return QStyledItemDelegate::sizeHint(option, index);
        }
        QTextDocument doc;
        doc.setDefaultFont(option.font);
        doc.setDocumentMargin(2);
        doc.setHtml(html);
        doc.setTextWidth(option.rect.width() > 0 ? option.rect.width() : 400);
        return QSize(static_cast<int>(doc.idealWidth()) + 12,
                     static_cast<int>(doc.size().height()));
    }
};

/// 将字节码指令文本转为带语法高亮的 HTML
/// opcode（OP_*）蓝色，常量/字符串/数字橙色，注释灰色，函数头紫色
static QString formatBytecodeHtml(const std::string& text) {
    QString qtext = QString::fromStdString(text);
    // 函数头分隔线 ---- xxx ---- → 紫色加粗
    if (qtext.startsWith("----") && qtext.endsWith("----")) {
        return QString("<span style='color:#8764b8;font-weight:bold;'>%1</span>")
                    .arg(qtext.toHtmlEscaped());
    }
    // 拆分注释（# 开头到行尾，或行内 # 注释）
    QString code = qtext;
    QString comment;
    int hashPos = qtext.indexOf('#');
    if (hashPos >= 0) {
        code = qtext.left(hashPos);
        comment = qtext.mid(hashPos);
    }

    // 按空白拆分
    QStringList tokens = code.split(' ', Qt::SkipEmptyParts);
    QString html;
    bool first = true;
    for (const QString& t : tokens) {
        if (!first) html += "&nbsp;";
        first = false;

        QString core = t;
        QString suffix;
        if (core.endsWith(',')) {
            suffix = ",";
            core.chop(1);
        }

        QString esc = core.toHtmlEscaped();
        // OP_ 开头 → opcode 蓝色加粗
        if (core.startsWith("OP_") || core.startsWith("REG_") || core.startsWith("TAG_")) {
            html += "<span style='color:#0078d4;font-weight:bold;'>" + esc + "</span>";
        }
        // 数字常量
        else if (!core.isEmpty() && core[0].isDigit()) {
            html += "<span style='color:#d83b01;'>" + esc + "</span>";
        }
        // 字符串字面量
        else if (core.startsWith('"') && core.endsWith('"')) {
            html += "<span style='color:#d83b01;'>" + esc + "</span>";
        }
        // 标签 Lxx / BBxx → 绿色
        else if ((core.startsWith('L') || core.startsWith('B')) && core.size() > 1 &&
                 core.mid(1).toInt() > 0) {
            html += "<span style='color:#107c10;'>" + esc + "</span>";
        }
        // = / -> / | 等符号
        else if (core == "=" || core == "->" || core == "|" || core == "&" || core == ":") {
            html += "<span style='color:#6e6e6e;'>" + esc + "</span>";
        }
        // 标识符默认色
        else {
            html += "<span style='color:#1e1e1e;'>" + esc + "</span>";
        }
        if (!suffix.isEmpty()) {
            html += "<span style='color:#6e6e6e;'>" + suffix.toHtmlEscaped() + "</span>";
        }
    }

    if (!comment.isEmpty()) {
        if (!html.isEmpty()) html += "&nbsp;";
        html += "<span style='color:#6e6e6e;font-style:italic;'>" + comment.toHtmlEscaped() + "</span>";
    }

    return html;
}

// ============================================================
// Constructor / Destructor
// ============================================================

Ide::Ide(QWidget* parent)
    : QMainWindow(parent) {

    // 第九轮：无边框窗口 + 自定义标题栏
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);

    controller_ = new IdeController(this);

    // 第十二轮：统一标题栏包含菜单+工具栏+窗口控制，initUI 仅需 titleBar_
    initTitleBar();
    initUI();
    initConnections();
    initFileTree();
    initStatusBar();

    replPanel_->setController(controller_);

    // 第十二轮：主题跟随 QSettings（main.cpp 已预设），支持亮/暗切换
    // 注册主题变更回调：切换主题时自动刷新所有 QSS
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        applyFluentStyle();
    });
    applyFluentStyle();

    setupCompletion();

    // Layout save timer (debounced)
    splitterSaveTimer_ = new QTimer(this);
    splitterSaveTimer_->setSingleShot(true);
    splitterSaveTimer_->setInterval(500);
    connect(splitterSaveTimer_, &QTimer::timeout, this, &Ide::saveLayout);

    // Restore AST window geometry (independent top-level window)
    restoreAstWindowGeometry();

    // Startup: show welcome page, hide non-essential panels
    centerStack_->setCurrentWidget(welcomePage_);

    restoreLayout();
    updateWindowTitle();
    updateStatusBar();
}

Ide::~Ide() {
    saveLayout();
}

// ============================================================
// Close event
// ============================================================

void Ide::closeEvent(QCloseEvent* event) {
    if (!maybeSave()) {
        event->ignore();
        return;
    }

    if (replPanel_->isReplRunning()) {
        auto ret = QMessageBox::warning(this,
            QString::fromUtf8("REPL 仍在执行"),
            QString::fromUtf8("REPL 有异步任务正在执行。\n关闭窗口将发送中止请求并等待最多 5 秒。\n\n是否继续关闭？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    if (controller_->isRunning()) {
        if (!controller_->stopForClose(3000)) {
            if (codeEditor_ && codeEditor_->document()->isModified()) {
                auto ret = QMessageBox::warning(this,
                    QString::fromUtf8("程序无响应，即将强制终止"),
                    QString::fromUtf8("解释器线程未在 3 秒内响应停止请求，将强制终止进程。\n"
                                      "强制终止会跳过正常析构，未保存的代码将丢失。\n\n"
                                      "是否现在保存？"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                    QMessageBox::Save);
                if (ret == QMessageBox::Save) {
                    onSave();
                    if (codeEditor_->document()->isModified()) {
                        event->ignore();
                        return;
                    }
                } else if (ret == QMessageBox::Cancel) {
                    event->ignore();
                    return;
                }
            }
            controller_->forceStop();
        }
    }
    if (controller_->isVmRunning()) {
        onVmStop();
    }
    replPanel_->waitReplFuture();
    // Save AST independent window geometry before closing
    if (astWindow_ && !astWindow_->isHidden()) saveAstWindowGeometry();
    saveLayout();
    event->accept();
}

// ============================================================
// Native event: frameless window edge resize (Windows WM_NCHITTEST)
// 第九轮：无边框窗口保留边缘拖拽调整大小能力
// ============================================================

bool Ide::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        MSG* msg = reinterpret_cast<MSG*>(message);
        if (msg->message == WM_NCHITTEST) {
            const LONG borderWidth = 5;
            LONG x = GET_X_LPARAM(msg->lParam);
            LONG y = GET_Y_LPARAM(msg->lParam);
            RECT winRect;
            GetWindowRect(msg->hwnd, &winRect);

            bool left   = x >= winRect.left && x < winRect.left + borderWidth;
            bool right  = x < winRect.right && x >= winRect.right - borderWidth;
            bool top    = y >= winRect.top && y < winRect.top + borderWidth;
            bool bottom = y < winRect.bottom && y >= winRect.bottom - borderWidth;

            if (top && left)     { *result = HTTOPLEFT;     return true; }
            if (top && right)    { *result = HTTOPRIGHT;    return true; }
            if (bottom && left)  { *result = HTBOTTOMLEFT;  return true; }
            if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
            if (left)            { *result = HTLEFT;        return true; }
            if (right)           { *result = HTRIGHT;       return true; }
            if (top)             { *result = HTTOP;         return true; }
            if (bottom)          { *result = HTBOTTOM;      return true; }
        }
        if (msg->message == WM_NCLBUTTONDBLCLK) {
            // 双击标题栏区域 → 最大化/还原（由标题栏 mouseDoubleClickEvent 处理，此处忽略系统默认）
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

// ============================================================
// VM breakpoint sync
// ============================================================

void Ide::syncVmBreakpoints() {
    if (!codeEditor_) return;
    QSet<int> bps = codeEditor_->getBreakpoints();
    QMap<int, std::string> conds;
    for (int line : bps) {
        std::string cond = codeEditor_->getBreakpointCondition(line);
        if (!cond.empty()) conds[line] = cond;
    }
    controller_->setVmBreakpoints(bps);
    controller_->setVmBreakpointConditions(conds);
}

// ============================================================
// Event filter: editor tab middle-click close + hover close button
// ============================================================

bool Ide::eventFilter(QObject* watched, QEvent* event) {
    // 第十二轮：统一标题栏拖拽（仅在非交互控件区域触发窗口拖动）
    if (watched == titleBar_) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                // 排除所有可交互控件：按钮（含 Fluent ComboBox/PushButton/SplitButton）等
                auto* child = titleBar_->childAt(me->pos());
                bool isInteractive = false;
                while (child && child != titleBar_) {
                    if (qobject_cast<QAbstractButton*>(child)) {
                        isInteractive = true;
                        break;
                    }
                    child = child->parentWidget();
                }
                if (!isInteractive) {
#ifdef Q_OS_WIN
                    HWND hwnd = reinterpret_cast<HWND>(winId());
                    ReleaseCapture();
                    SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
#endif
                }
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                auto* child = titleBar_->childAt(me->pos());
                bool isInteractive = false;
                while (child && child != titleBar_) {
                    if (qobject_cast<QAbstractButton*>(child)) {
                        isInteractive = true;
                        break;
                    }
                    child = child->parentWidget();
                }
                if (!isInteractive) {
                    if (isMaximized()) showNormal();
                    else showMaximized();
                }
            }
        }
        return false;  // 不拦截，按钮仍可正常点击
    }
    if (watched == editorTabWidget_->tabBar()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::MiddleButton) {
                int idx = editorTabWidget_->tabBar()->tabAt(me->pos());
                if (idx >= 0) {
                    onEditorTabCloseRequested(idx);
                    return true;
                }
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            int hovered = editorTabWidget_->tabBar()->tabAt(me->pos());
            updateTabCloseButtons(hovered);
        } else if (event->type() == QEvent::Leave) {
            updateTabCloseButtons(-1);
        } else if (event->type() == QEvent::ContextMenu) {
            auto* ce = static_cast<QContextMenuEvent*>(event);
            onEditorTabContextMenu(ce->globalPos());
            return true;
        }
    }
    // 第十一轮：欢迎页支持拖拽文件夹打开
    if (watched == welcomePage_ || watched == welcomePage_->findChild<QWidget*>("welcomeCenter")) {
        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                for (const QUrl& url : de->mimeData()->urls()) {
                    QString path = url.toLocalFile();
                    QFileInfo fi(path);
                    if (fi.isDir()) {
                        openWorkspace(path);
                        return true;
                    }
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void Ide::updateTabCloseButtons(int hoveredIndex) {
    int current = editorTabWidget_->currentIndex();
    for (int i = 0; i < editorTabWidget_->count(); ++i) {
        bool show = (i == current) || (i == hoveredIndex);
        QWidget* btn = editorTabWidget_->tabBar()->tabButton(i, QTabBar::RightSide);
        if (btn) btn->setVisible(show);
    }
}

// ============================================================
// Multi-tab editor management
// ============================================================

int Ide::createNewEditorTab(const QString& filePath, const QString& content) {
    EditorTabData data;
    data.container = new QWidget;
    data.isUntitled = filePath.isEmpty();
    data.filePath = filePath;

    auto* layout = new QVBoxLayout(data.container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    data.editor = new CodeEditor(data.container);
    data.editor->setMinimumWidth(200);
    data.highlighter = new SyntaxHighlighter(data.editor->document());

    data.findPanel = new FindReplacePanel(data.editor, data.container);
    data.findPanel->hide();

    layout->addWidget(data.findPanel);
    layout->addWidget(data.editor, 1);

    if (!content.isEmpty()) {
        data.editor->setPlainText(content);
        data.editor->document()->setModified(false);
    }

    QString tabTitle;
    if (data.isUntitled) {
        untitledCount_++;
        tabTitle = QString::fromUtf8("未命名-%1").arg(untitledCount_);
    } else {
        QFileInfo fi(filePath);
        tabTitle = fi.fileName();
    }

    int idx = editorTabWidget_->addTab(data.container, tabTitle);
    data.container->setProperty("tabIndex", idx);

    editorTabs_.push_back(data);

    CodeEditor* editorPtr = data.editor;

    // Unsaved dot mark: "● filename.min" means unsaved
    auto updateTabDirtyMark = [this](int tabIdx, bool changed) {
        if (tabIdx < 0 || tabIdx >= editorTabWidget_->count()) return;
        QString text = editorTabWidget_->tabText(tabIdx);
        const QString dot = QString(QChar(0x25CF)) + " ";
        bool hasDot = text.startsWith(dot);
        if (text.endsWith("*")) text.chop(1);
        if (changed && !hasDot) {
            editorTabWidget_->setTabText(tabIdx, dot + text);
        } else if (!changed && hasDot) {
            text.remove(0, dot.length());
            editorTabWidget_->setTabText(tabIdx, text);
        }
    };

    connect(data.editor->document(), &QTextDocument::modificationChanged,
            this, [this, editorPtr, updateTabDirtyMark](bool changed) {
        int tabIdx = -1;
        for (int i = 0; i < static_cast<int>(editorTabs_.size()); ++i) {
            if (editorTabs_[i].editor == editorPtr) { tabIdx = i; break; }
        }
        if (tabIdx < 0) return;
        updateTabDirtyMark(tabIdx, changed);
        if (tabIdx == editorTabWidget_->currentIndex()) {
            isDirty_ = changed;
            updateWindowTitle();
        }
    });

    connect(data.editor, &CodeEditor::breakpointConditionRequested,
            this, [this](int line, const QString& condition) {
        controller_->setBreakpointCondition(line, condition.toStdString());
    });

    data.editor->setCompletionWords(staticCompletionWords_);

    // Wire textChanged for debounce timers
    connect(data.editor, &QPlainTextEdit::textChanged, this, [this]() {
        if (completionTimer_) completionTimer_->start();
        if (syntaxCheckTimer_) syntaxCheckTimer_->start();
    });

    // Update status bar (line/column) when cursor moves or text changes
    connect(data.editor, &QPlainTextEdit::cursorPositionChanged,
            this, [this]() { if (codeEditor_ == sender()) updateStatusBar(); });

    return idx;
}

void Ide::switchToTab(int index) {
    if (index < 0 || index >= static_cast<int>(editorTabs_.size())) return;
    auto& data = editorTabs_[index];
    codeEditor_ = data.editor;
    highlighter_ = data.highlighter;
    findReplacePanel_ = data.findPanel;
    currentFilePath_ = data.filePath;
    isDirty_ = codeEditor_->document()->isModified();
    editorTabWidget_->setCurrentIndex(index);
    updateWindowTitle();
    updateStatusBar();
}

int Ide::findTabForFile(const QString& path) {
    QFileInfo targetFi(path);
    for (int i = 0; i < static_cast<int>(editorTabs_.size()); ++i) {
        if (editorTabs_[i].filePath.isEmpty()) continue;
        QFileInfo tabFi(editorTabs_[i].filePath);
        if (tabFi.absoluteFilePath() == targetFi.absoluteFilePath()) return i;
    }
    return -1;
}

void Ide::loadFileIntoTab(int tabIndex, const QString& path) {
    if (tabIndex < 0 || tabIndex >= static_cast<int>(editorTabs_.size())) return;
    auto& data = editorTabs_[tabIndex];

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        InfoBar::warning(QString::fromUtf8("错误"),
            QString::fromUtf8("无法打开文件: ") + file.errorString(),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    qint64 fileSize = file.size();
    if (fileSize > static_cast<qint64>(RuntimeLimits::MAX_SOURCE_SIZE)) {
        InfoBar::warning(QString::fromUtf8("错误"),
            QString::fromUtf8("文件过大 (") + QString::number(fileSize) +
            QString::fromUtf8(" 字节)，超过上限 (") +
            QString::number(RuntimeLimits::MAX_SOURCE_SIZE) + QString::fromUtf8(" 字节)"),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        file.close();
        return;
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    QString content = in.readAll();
    file.close();
    if (!content.isEmpty() && content[0] == QChar(0xFEFF)) content.remove(0, 1);

    data.editor->setPlainText(content);
    data.editor->document()->setModified(false);
    data.filePath = path;
    data.isUntitled = false;

    QFileInfo fi(path);
    editorTabWidget_->setTabText(tabIndex, fi.fileName());

    if (tabIndex == editorTabWidget_->currentIndex()) {
        currentFilePath_ = path;
        isDirty_ = false;
        updateWindowTitle();
    }
}

void Ide::onCurrentTabChanged(int index) {
    if (index < 0 || index >= static_cast<int>(editorTabs_.size())) return;
    switchToTab(index);
    if (codeEditor_) codeEditor_->setFocus();
    updateTabCloseButtons(index);
}

void Ide::onEditorTabCloseRequested(int index) {
    if (index < 0 || index >= static_cast<int>(editorTabs_.size())) return;
    auto& data = editorTabs_[index];
    if (data.editor->document()->isModified()) {
        codeEditor_ = data.editor;
        currentFilePath_ = data.filePath;
        if (!maybeSave()) return;
    }

    // Close last tab: clear editor area, return to welcome page
    if (static_cast<int>(editorTabs_.size()) <= 1) {
        editorTabWidget_->removeTab(0);
        data.container->deleteLater();
        editorTabs_.clear();
        codeEditor_ = nullptr;
        highlighter_ = nullptr;
        findReplacePanel_ = nullptr;
        currentFilePath_.clear();
        isDirty_ = false;
        hideBottomPanel();
        hideRightPanel();
        centerStack_->setCurrentWidget(welcomePage_);
        updateWindowTitle();
        return;
    }

    int currIdx = editorTabWidget_->currentIndex();
    int newCurrent = currIdx;
    if (index == currIdx) {
        newCurrent = (index > 0) ? index - 1 : 0;
    } else if (index < currIdx) {
        newCurrent = currIdx - 1;
    }
    editorTabWidget_->removeTab(index);
    data.container->deleteLater();
    editorTabs_.erase(editorTabs_.begin() + index);
    for (int i = index; i < static_cast<int>(editorTabs_.size()); ++i) {
        editorTabs_[i].container->setProperty("tabIndex", i);
    }
    switchToTab(newCurrent);
}

void Ide::ensureEditorVisible() {
    if (centerStack_ && centerStack_->currentWidget() != editorTabWidget_) {
        centerStack_->setCurrentWidget(editorTabWidget_);
    }
}

void Ide::onEditorTabContextMenu(const QPoint& pos) {
    int idx = editorTabWidget_->tabBar()->tabAt(editorTabWidget_->tabBar()->mapFromGlobal(pos));
    if (idx < 0) return;

    QMenu menu(this);
    auto* closeAct = menu.addAction(QString::fromUtf8("关闭"));
    auto* closeOthersAct = menu.addAction(QString::fromUtf8("关闭其他"));
    auto* closeAllAct = menu.addAction(QString::fromUtf8("关闭全部"));

    // Store the clicked index for the action handlers
    int clickedIdx = idx;

    auto* chosen = menu.exec(pos);
    if (!chosen) return;

    if (chosen == closeAct) {
        onEditorTabCloseRequested(clickedIdx);
    } else if (chosen == closeOthersAct) {
        // Close all tabs except clickedIdx
        // Close from right to left to preserve indices
        for (int i = static_cast<int>(editorTabs_.size()) - 1; i >= 0; --i) {
            if (i == clickedIdx) continue;
            onEditorTabCloseRequested(i);
            if (i < clickedIdx) clickedIdx--;
        }
    } else if (chosen == closeAllAct) {
        onCloseAllTabs();
    }
}

void Ide::onCloseOtherTabs() {
    int current = editorTabWidget_->currentIndex();
    for (int i = static_cast<int>(editorTabs_.size()) - 1; i >= 0; --i) {
        if (i == current) continue;
        onEditorTabCloseRequested(i);
        if (i < current) current--;
    }
}

void Ide::onCloseAllTabs() {
    while (!editorTabs_.empty()) {
        onEditorTabCloseRequested(static_cast<int>(editorTabs_.size()) - 1);
    }
}

// ============================================================
// Welcome page
// ============================================================

void Ide::initWelcomePage() {
    welcomePage_ = new QWidget;
    welcomePage_->setObjectName("welcomePage");
    // 第十一轮：支持拖拽文件夹到欢迎页打开
    welcomePage_->setAcceptDrops(true);
    auto* outerLayout = new QHBoxLayout(welcomePage_);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // Left: recent workspaces (第十一轮：220px 宽)
    auto* recentPanel = new QWidget;
    recentPanel->setObjectName("welcomeRecentPanel");
    recentPanel->setFixedWidth(220);
    auto* recentLayout = new QVBoxLayout(recentPanel);
    recentLayout->setContentsMargins(0, 16, 0, 0);
    recentLayout->setSpacing(0);

    auto* recentHeader = new QLabel(QString::fromUtf8("最近打开"));
    recentHeader->setObjectName("welcomeRecentHeader");
    recentLayout->addWidget(recentHeader);

    recentListWidget_ = new QListWidget;
    recentListWidget_->setObjectName("welcomeRecentList");
    recentListWidget_->setFrameStyle(QFrame::NoFrame);
    recentListWidget_->setSpacing(0);
    recentListWidget_->setCursor(Qt::PointingHandCursor);
    connect(recentListWidget_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (!item) return;
        QString dir = item->data(Qt::UserRole).toString();
        if (!dir.isEmpty() && QDir(dir).exists()) openWorkspace(dir);
    });
    connect(recentListWidget_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        QString dir = item->data(Qt::UserRole).toString();
        if (!dir.isEmpty() && QDir(dir).exists()) openWorkspace(dir);
    });
    recentLayout->addWidget(recentListWidget_, 1);
    outerLayout->addWidget(recentPanel);

    // Center area
    auto* centerArea = new QWidget;
    centerArea->setObjectName("welcomeCenter");
    centerArea->setAcceptDrops(true);
    auto* centerLayout = new QVBoxLayout(centerArea);
    centerLayout->setAlignment(Qt::AlignCenter);
    centerLayout->setSpacing(12);

    // 第十一轮：48px 蓝色代码花括号图标 { }
    auto* iconLabel = new QLabel(QString::fromUtf8("{ }"));
    iconLabel->setObjectName("welcomeIcon");
    iconLabel->setAlignment(Qt::AlignCenter);
    QFont iconFont = iconLabel->font();
    iconFont.setPointSize(48);
    iconFont.setBold(true);
    iconFont.setStyleHint(QFont::Monospace);
    iconLabel->setFont(iconFont);

    // 第十一轮：标题 24px Medium，副标题 14px 灰色
    auto* titleLabel = new QLabel(QString::fromUtf8("MiniLang IDE"));
    titleLabel->setObjectName("welcomeTitle");
    titleLabel->setAlignment(Qt::AlignCenter);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(24);
    titleFont.setWeight(QFont::Medium);
    titleLabel->setFont(titleFont);

    auto* subtitleLabel = new QLabel(QString::fromUtf8("现代化 MiniLang 编程语言开发环境"));
    subtitleLabel->setObjectName("welcomeSubtitle");
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setWordWrap(true);
    QFont subFont = subtitleLabel->font();
    subFont.setPointSize(14);
    subtitleLabel->setFont(subFont);

    centerLayout->addStretch(3);
    centerLayout->addWidget(iconLabel);
    centerLayout->addSpacing(8);
    centerLayout->addWidget(titleLabel);
    centerLayout->addSpacing(2);
    centerLayout->addWidget(subtitleLabel);
    centerLayout->addSpacing(24);

    // 第十一轮：按钮区域，最大宽度 400px，8px 圆角
    auto* btnContainer = new QWidget;
    btnContainer->setObjectName("welcomeBtnContainer");
    auto* btnLayout = new QVBoxLayout(btnContainer);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(10);

    // Primary button: 打开文件夹 (Claude DS 风格 — terra-cotta 填充)
    // 使用原生 QPushButton + QSS 而非 QFluentKit PrimaryPushButton，
    // 因为 QFluentKit 自绘不读 QSS，无法应用 Claude DS 配色
    auto* primaryBtn = new QPushButton(QString::fromUtf8("  打开文件夹"), btnContainer);
    primaryBtn->setObjectName("welcomePrimaryBtn");
    primaryBtn->setMinimumHeight(38);
    primaryBtn->setMinimumWidth(220);
    primaryBtn->setCursor(Qt::PointingHandCursor);
    primaryBtn->setIcon(QIcon(QString::fromUtf8(":/icons/openfolder_black.svg")));
    connect(primaryBtn, &QPushButton::clicked, this, &Ide::onOpenFolder);
    btnLayout->addWidget(primaryBtn);

    // Secondary button: 新建文件 (白底 terra-cotta 边框)
    auto* secondaryBtn = new QPushButton(QString::fromUtf8("  新建文件"), btnContainer);
    secondaryBtn->setObjectName("welcomeSecondaryBtn");
    secondaryBtn->setMinimumHeight(38);
    secondaryBtn->setMinimumWidth(220);
    secondaryBtn->setCursor(Qt::PointingHandCursor);
    secondaryBtn->setIcon(QIcon(QString::fromUtf8(":/icons/newfile_black.svg")));
    connect(secondaryBtn, &QPushButton::clicked, this, [this]() {
        ensureEditorVisible();
        onNew();
    });
    btnLayout->addWidget(secondaryBtn);

    centerLayout->addWidget(btnContainer, 0, Qt::AlignCenter);

    centerLayout->addSpacing(16);

    // 第十一轮：辅助链接（语法示例、帮助文档）12px 蓝色
    auto* shortcutLayout = new QHBoxLayout;
    shortcutLayout->setAlignment(Qt::AlignCenter);
    shortcutLayout->setSpacing(24);
    auto* sampleLabel = new QLabel(QString::fromUtf8(
        "<a href=\"sample\" style=\"color:#C96442;text-decoration:none;font-size:12px;\">语法示例</a>"));
    sampleLabel->setObjectName("welcomeShortcut");
    sampleLabel->setCursor(Qt::PointingHandCursor);
    connect(sampleLabel, &QLabel::linkActivated, this, [this]() {
        QString sampleDir = QApplication::applicationDirPath() + "/../../samples/mini";
        if (!QDir(sampleDir).exists())
            sampleDir = QApplication::applicationDirPath() + "/samples/mini";
        if (QDir(sampleDir).exists()) openWorkspace(sampleDir);
        else onOpenFolder();
    });
    auto* helpLabel = new QLabel(QString::fromUtf8(
        "<a href=\"help\" style=\"color:#C96442;text-decoration:none;font-size:12px;\">帮助文档</a>"));
    helpLabel->setObjectName("welcomeShortcut");
    helpLabel->setCursor(Qt::PointingHandCursor);
    connect(helpLabel, &QLabel::linkActivated, this, [this]() { showHelpDialog(); });
    shortcutLayout->addWidget(sampleLabel);
    shortcutLayout->addWidget(helpLabel);
    centerLayout->addLayout(shortcutLayout);

    centerLayout->addStretch(4);
    outerLayout->addWidget(centerArea, 1);

    // 第十一轮：安装拖拽 eventFilter
    welcomePage_->installEventFilter(this);
    centerArea->installEventFilter(this);

    loadRecentWorkspaces();
}

void Ide::loadRecentWorkspaces() {
    QSettings settings("MiniLang", "MiniLang IDE");
    recentWorkspaces_ = settings.value("recent/workspaces").toStringList();
    refreshRecentList();
}

void Ide::addRecentWorkspace(const QString& dir) {
    if (dir.isEmpty()) return;
    QString normalized = QDir(dir).absolutePath();
    recentWorkspaces_.removeAll(normalized);
    recentWorkspaces_.prepend(normalized);
    while (recentWorkspaces_.size() > 10) recentWorkspaces_.removeLast();
    QSettings settings("MiniLang", "MiniLang IDE");
    settings.setValue("recent/workspaces", recentWorkspaces_);
    refreshRecentList();
}

void Ide::refreshRecentList() {
    if (!recentListWidget_) return;
    recentListWidget_->clear();
    bool hasItems = false;
    for (const QString& ws : recentWorkspaces_) {
        if (!QDir(ws).exists()) continue;
        QFileInfo fi(ws);
        auto* item = new QListWidgetItem(fi.fileName());
        item->setToolTip(ws);
        item->setData(Qt::UserRole, ws);
        recentListWidget_->addItem(item);
        hasItems = true;
    }
    // 第十一轮：列表为空时显示浅灰色提示
    if (!hasItems) {
        auto* emptyItem = new QListWidgetItem(QString::fromUtf8("暂无最近打开的工作区"));
        emptyItem->setFlags(Qt::NoItemFlags);
        emptyItem->setData(Qt::ForegroundRole, QColor(180, 180, 180));
        recentListWidget_->addItem(emptyItem);
    }
}

// ============================================================
// Custom title bar (第九轮：VS Code 风格无边框标题栏)
// 32px 高，白色背景，左侧图标+标题+路径，右侧最小化/最大化/关闭
// ============================================================

void Ide::initTitleBar() {
    // ============================================================
    // 第十二轮：统一 36px 标题栏（融合菜单 + 工具栏 + 窗口控制）
    // 取代旧三层结构（标题栏32px + 菜单栏28px + 工具栏34px = 94px）
    // ============================================================
    titleBar_ = new QWidget(this);
    titleBar_->setObjectName("titleBar");
    titleBar_->setFixedHeight(36);

    auto* layout = new QHBoxLayout(titleBar_);
    layout->setContentsMargins(12, 0, 0, 0);
    layout->setSpacing(8);

    // ---- 左侧：应用图标 + 名称 ----
    titleIconLabel_ = new QLabel(titleBar_);
    titleIconLabel_->setFixedSize(16, 16);
    titleIconLabel_->setPixmap(Fluent::icon(Fluent::IconType::CODE).pixmap(QSize(16, 16)));
    titleIconLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(titleIconLabel_);

    titleTextLabel_ = new QLabel(QString::fromUtf8("MiniLang IDE"), titleBar_);
    titleTextLabel_->setObjectName("titleText");
    QFont titleFont = titleTextLabel_->font();
    titleFont.setPointSize(9);
    titleFont.setWeight(QFont::DemiBold);
    titleTextLabel_->setFont(titleFont);
    layout->addWidget(titleTextLabel_);

    titlePathLabel_ = new QLabel(titleBar_);
    titlePathLabel_->setObjectName("titlePath");
    QFont pathFont = titlePathLabel_->font();
    pathFont.setPointSize(9);
    titlePathLabel_->setFont(pathFont);
    layout->addWidget(titlePathLabel_);

    // ---- 系统菜单按钮（Fluent DropDownToolButton + RoundMenu 替代 QMenuBar）----
    auto* menuBtn = new DropDownToolButton(Fluent::IconType::MENU, titleBar_);
    menuBtn->setFixedSize(32, 32);
    menuBtn->setToolTip(QString::fromUtf8("菜单"));

    // 创建主菜单（File / Edit / View / Run / Help 子菜单）
    auto* mainMenu = new RoundMenu(QString(), titleBar_);

    // -- File --
    auto* fileMenu = new RoundMenu(QString::fromUtf8("文件"), titleBar_);
    newAction_ = new QAction(QString::fromUtf8("新建(&N)"), this);
    newAction_->setShortcut(QKeySequence::New);
    connect(newAction_, &QAction::triggered, this, [this]() { ensureEditorVisible(); onNew(); });
    fileMenu->addAction(newAction_);

    openAction_ = new QAction(QString::fromUtf8("打开文件(&O)..."), this);
    openAction_->setShortcut(QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &Ide::onOpen);
    fileMenu->addAction(openAction_);

    openFolderAction_ = new QAction(QString::fromUtf8("打开文件夹(&D)..."), this);
    connect(openFolderAction_, &QAction::triggered, this, &Ide::onOpenFolder);
    fileMenu->addAction(openFolderAction_);
    fileMenu->addSeparator();

    saveAction_ = new QAction(QString::fromUtf8("保存(&S)"), this);
    saveAction_->setShortcut(QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &Ide::onSave);
    fileMenu->addAction(saveAction_);

    saveAsAction_ = new QAction(QString::fromUtf8("另存为(&A)..."), this);
    saveAsAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_S);
    connect(saveAsAction_, &QAction::triggered, this, &Ide::onSaveAs);
    fileMenu->addAction(saveAsAction_);
    mainMenu->addMenu(fileMenu);

    // -- Edit --
    auto* editMenu = new RoundMenu(QString::fromUtf8("编辑"), titleBar_);
    auto* formatMenuAct = new QAction(QString::fromUtf8("格式化"), this);
    formatMenuAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_F);
    connect(formatMenuAct, &QAction::triggered, this, [this]() { ensureEditorVisible(); onFormat(); });
    editMenu->addAction(formatMenuAct);
    auto* findMenuAct = new QAction(QString::fromUtf8("查找"), this);
    findMenuAct->setShortcut(QKeySequence::Find);
    connect(findMenuAct, &QAction::triggered, this, [this]() { ensureEditorVisible(); onFind(); });
    editMenu->addAction(findMenuAct);
    auto* replaceMenuAct = new QAction(QString::fromUtf8("替换"), this);
    replaceMenuAct->setShortcut(QKeySequence::Replace);
    connect(replaceMenuAct, &QAction::triggered, this, [this]() { ensureEditorVisible(); onReplace(); });
    editMenu->addAction(replaceMenuAct);
    mainMenu->addMenu(editMenu);

    // -- View --
    auto* viewMenu = new RoundMenu(QString::fromUtf8("视图"), titleBar_);
    viewExplorerAction_ = new QAction(QString::fromUtf8("资源管理器"), this);
    viewExplorerAction_->setCheckable(true);
    connect(viewExplorerAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_) return;
        if (fileTreeDock_) fileTreeDock_->toggleView(on);
    });
    viewMenu->addAction(viewExplorerAction_);

    viewDebugAction_ = new QAction(QString::fromUtf8("调试面板"), this);
    viewDebugAction_->setCheckable(true);
    connect(viewDebugAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_) return;
        if (debugPanelDock_) debugPanelDock_->toggleView(on);
    });
    viewMenu->addAction(viewDebugAction_);

    viewOutputAction_ = new QAction(QString::fromUtf8("输出面板"), this);
    viewOutputAction_->setCheckable(true);
    connect(viewOutputAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_) return;
        if (bottomDock_) bottomDock_->toggleView(on);
    });
    viewMenu->addAction(viewOutputAction_);

    viewCompileAnalysisAction_ = new QAction(QString::fromUtf8("编译分析面板"), this);
    viewCompileAnalysisAction_->setCheckable(true);
    viewCompileAnalysisAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_V);
    connect(viewCompileAnalysisAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_) return;
        if (!rightDock_) return;
        if (on) {
            rightDock_->toggleView(true);
            rightDock_->setAsCurrentTab();
            if (rightPivot_) rightPivot_->setCurrentItem("token");
            if (codeEditor_) onCompileAnalysis();
        } else {
            rightDock_->toggleView(false);
        }
    });
    viewMenu->addAction(viewCompileAnalysisAction_);
    mainMenu->addMenu(viewMenu);

    // -- Run --
    auto* runMenu = new RoundMenu(QString::fromUtf8("运行"), titleBar_);
    auto* runMenuAct = new QAction(QString::fromUtf8("运行"), this);
    runMenuAct->setShortcut(Qt::Key_F5);
    connect(runMenuAct, &QAction::triggered, this, [this]() { ensureEditorVisible(); onRun(); });
    runMenu->addAction(runMenuAct);
    auto* debugMenuAct = new QAction(QString::fromUtf8("调试"), this);
    debugMenuAct->setShortcut(Qt::Key_F6);
    connect(debugMenuAct, &QAction::triggered, this, [this]() { ensureEditorVisible(); onDebug(); });
    runMenu->addAction(debugMenuAct);
    runMenu->addSeparator();
    auto* stepInMenuAct = new QAction(QString::fromUtf8("单步进入"), this);
    stepInMenuAct->setShortcut(Qt::Key_F11);
    connect(stepInMenuAct, &QAction::triggered, this, &Ide::onStepIn);
    runMenu->addAction(stepInMenuAct);
    auto* stepOverMenuAct = new QAction(QString::fromUtf8("单步跳过"), this);
    stepOverMenuAct->setShortcut(Qt::Key_F10);
    connect(stepOverMenuAct, &QAction::triggered, this, &Ide::onStepOver);
    runMenu->addAction(stepOverMenuAct);
    auto* stepOutMenuAct = new QAction(QString::fromUtf8("单步跳出"), this);
    stepOutMenuAct->setShortcut(Qt::SHIFT | Qt::Key_F11);
    connect(stepOutMenuAct, &QAction::triggered, this, &Ide::onStepOut);
    runMenu->addAction(stepOutMenuAct);
    auto* resumeMenuAct = new QAction(QString::fromUtf8("继续"), this);
    resumeMenuAct->setShortcut(Qt::Key_F9);
    connect(resumeMenuAct, &QAction::triggered, this, &Ide::onResume);
    runMenu->addAction(resumeMenuAct);
    auto* stopMenuAct = new QAction(QString::fromUtf8("停止"), this);
    stopMenuAct->setShortcut(Qt::SHIFT | Qt::Key_F5);
    connect(stopMenuAct, &QAction::triggered, this, &Ide::onStop);
    runMenu->addAction(stopMenuAct);
    mainMenu->addMenu(runMenu);

    // -- Help --
    auto* helpMenu = new RoundMenu(QString::fromUtf8("帮助"), titleBar_);
    auto* samplesAct = new QAction(QString::fromUtf8("语法示例"), this);
    connect(samplesAct, &QAction::triggered, this, [this]() {
        QString sampleDir = QApplication::applicationDirPath() + "/../../samples/mini";
        if (!QDir(sampleDir).exists())
            sampleDir = QApplication::applicationDirPath() + "/samples/mini";
        if (QDir(sampleDir).exists()) openWorkspace(sampleDir);
        else onOpenFolder();
    });
    helpMenu->addAction(samplesAct);
    auto* helpAct = new QAction(QString::fromUtf8("帮助"), this);
    connect(helpAct, &QAction::triggered, this, &Ide::showHelpDialog);
    helpMenu->addAction(helpAct);
    mainMenu->addMenu(helpMenu);

    menuBtn->setMenu(mainMenu);
    layout->addWidget(menuBtn);

    // 分隔线：菜单 ↔ 工具栏
    auto addSeparator = [&layout, this]() {
        auto* sep = new QFrame(titleBar_);
        sep->setFrameShape(QFrame::VLine);
        sep->setFixedWidth(1);
        sep->setStyleSheet("background: transparent;");
        sep->setObjectName("titleBarSep");
        layout->addWidget(sep);
    };
    addSeparator();

    // ---- 中间区域：工具栏按钮 ----

    // Run split button (PrimarySplitPushButton with dropdown for run/debug)
    runAction_ = new QAction(QString::fromUtf8("运行"), this);
    runAction_->setShortcut(Qt::Key_F5);
    connect(runAction_, &QAction::triggered, this, [this]() { ensureEditorVisible(); onRun(); });

    debugAction_ = new QAction(QString::fromUtf8("调试"), this);
    debugAction_->setShortcut(Qt::Key_F6);
    connect(debugAction_, &QAction::triggered, this, [this]() { ensureEditorVisible(); onDebug(); });

    auto* runSplit = new PrimarySplitPushButton(QString::fromUtf8("运行"), Fluent::IconType::PLAY_SOLID, titleBar_);
    runSplit->setToolTip(QString::fromUtf8("运行程序 (F5)"));
    connect(runSplit, &PrimarySplitPushButton::clicked, runAction_, &QAction::trigger);
    auto* runFlyout = new RoundMenu(QString(), titleBar_);
    runFlyout->addAction(runAction_);
    runFlyout->addAction(debugAction_);
    runSplit->setFlyout(runFlyout);
    layout->addWidget(runSplit);

    // 编译按钮
    compileAnalysisAction_ = new QAction(QString::fromUtf8("编译分析"), this);
    compileAnalysisAction_->setToolTip(QString::fromUtf8("查看 Token / IR / 字节码"));

    // 格式化按钮
    formatAction_ = new QAction(QString::fromUtf8("格式化"), this);
    formatAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_F);
    auto* formatBtn = new CleanToolButton(Fluent::IconType::BROOM, titleBar_);
    formatBtn->setToolTip(QString::fromUtf8("格式化代码 (Ctrl+Shift+F)"));
    formatBtn->setDefaultAction(formatAction_);
    layout->addWidget(formatBtn);

    // AST 按钮
    astAction_ = new QAction(QString::fromUtf8("AST"), this);
    astAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_A);
    auto* astBtn = new CleanToolButton(Fluent::IconType::DICTIONARY, titleBar_);
    astBtn->setToolTip(QString::fromUtf8("查看 AST 树形图 (Ctrl+Shift+A)"));
    astBtn->setDefaultAction(astAction_);
    layout->addWidget(astBtn);

    // ---- 调试按钮组（动态显隐）----
    debugSepAction_ = nullptr;  // 无独立分隔线，用布局间距控制
    debugButtonContainer_ = new QWidget(titleBar_);
    debugButtonContainer_->setObjectName("debugButtonContainer");
    auto* dbgLayout = new QHBoxLayout(debugButtonContainer_);
    dbgLayout->setContentsMargins(0, 0, 0, 0);
    dbgLayout->setSpacing(2);

    auto makeDebugBtn = [this](QAction*& action, const QString& text,
                               const QString& tip, const QKeySequence& shortcut,
                               Fluent::IconType icon) {
        action = new QAction(text, this);
        action->setToolTip(tip);
        action->setShortcut(shortcut);
        auto* btn = new CleanToolButton(icon, titleBar_);
        btn->setToolTip(tip);
        btn->setDefaultAction(action);
        return btn;
    };

    dbgLayout->addWidget(makeDebugBtn(stepInAction_, QString::fromUtf8("步入"),
        QString::fromUtf8("单步进入 (F11)"), Qt::Key_F11, Fluent::IconType::CHEVRON_RIGHT_MED));
    dbgLayout->addWidget(makeDebugBtn(stepOverAction_, QString::fromUtf8("跨过"),
        QString::fromUtf8("单步跳过 (F10)"), Qt::Key_F10, Fluent::IconType::CHEVRON_DOWN_MED));
    dbgLayout->addWidget(makeDebugBtn(stepOutAction_, QString::fromUtf8("跨出"),
        QString::fromUtf8("单步跳出 (Shift+F11)"), Qt::SHIFT | Qt::Key_F11, Fluent::IconType::CHEVRON_RIGHT));
    dbgLayout->addWidget(makeDebugBtn(resumeAction_, QString::fromUtf8("继续"),
        QString::fromUtf8("继续运行到下一个断点 (F9)"), Qt::Key_F9, Fluent::IconType::PLAY));
    dbgLayout->addWidget(makeDebugBtn(stopAction_, QString::fromUtf8("停止"),
        QString::fromUtf8("停止运行 (Shift+F5)"), Qt::SHIFT | Qt::Key_F5, Fluent::IconType::CANCEL));

    layout->addWidget(debugButtonContainer_);
    debugButtonContainer_->hide();
    debugButtonsVisible_ = false;

    // ---- VM 按钮组（动态显隐）----
    vmSepAction_ = nullptr;
    vmButtonContainer_ = new QWidget(titleBar_);
    vmButtonContainer_->setObjectName("vmButtonContainer");
    auto* vmLayout = new QHBoxLayout(vmButtonContainer_);
    vmLayout->setContentsMargins(0, 0, 0, 0);
    vmLayout->setSpacing(2);

    auto makeVmBtn = [this](QAction*& action, const QString& text,
                             const QString& tip, const QKeySequence& shortcut,
                             Fluent::IconType icon) {
        action = new QAction(text, this);
        action->setToolTip(tip);
        action->setShortcut(shortcut);
        action->setEnabled(false);
        auto* btn = new CleanToolButton(icon, titleBar_);
        btn->setToolTip(tip);
        btn->setDefaultAction(action);
        return btn;
    };

    vmLayout->addWidget(makeVmBtn(vmStepAction_, QString::fromUtf8("VM单步"),
        QString::fromUtf8("VM 单步 (Ctrl+Shift+N)"), Qt::CTRL | Qt::SHIFT | Qt::Key_N, Fluent::IconType::CHEVRON_RIGHT_MED));
    vmLayout->addWidget(makeVmBtn(vmStepOverAction_, QString::fromUtf8("VM跨过"),
        QString::fromUtf8("VM 跨过 (Ctrl+Shift+O)"), Qt::CTRL | Qt::SHIFT | Qt::Key_O, Fluent::IconType::CHEVRON_DOWN_MED));
    vmLayout->addWidget(makeVmBtn(vmStepOutAction_, QString::fromUtf8("VM跨出"),
        QString::fromUtf8("VM 跨出 (Ctrl+Shift+U)"), Qt::CTRL | Qt::SHIFT | Qt::Key_U, Fluent::IconType::CHEVRON_RIGHT));
    vmLayout->addWidget(makeVmBtn(vmRunAction_, QString::fromUtf8("VM运行"),
        QString::fromUtf8("VM 运行 (Ctrl+Shift+R)"), Qt::CTRL | Qt::SHIFT | Qt::Key_R, Fluent::IconType::PLAY));
    vmLayout->addWidget(makeVmBtn(vmStopAction_, QString::fromUtf8("VM停止"),
        QString::fromUtf8("停止 VM"), QKeySequence(), Fluent::IconType::CANCEL));

    layout->addWidget(vmButtonContainer_);
    vmButtonContainer_->hide();

    clearAction_ = new QAction(QString::fromUtf8("清空"), this);
    clearAction_->setToolTip(QString::fromUtf8("清空输出面板"));

    // 弹性空间
    layout->addStretch(1);

    // ---- 执行引擎切换 ComboBox ----
    engineCombo_ = new ComboBox(titleBar_);
    engineCombo_->setObjectName("engineCombo");
    engineCombo_->addItem(QString::fromUtf8("树遍历解释器"));
    engineCombo_->addItem(QString::fromUtf8("栈式 VM"));
    engineCombo_->addItem(QString::fromUtf8("寄存器式 VM"));
    engineCombo_->setCurrentIndex(0);
    engineCombo_->setToolTip(QString::fromUtf8("切换执行引擎"));
    layout->addWidget(engineCombo_);

    // ---- 主题切换按钮 ----
    themeToggleBtn_ = new QToolButton(titleBar_);
    themeToggleBtn_->setObjectName("themeToggleBtn");
    themeToggleBtn_->setFixedSize(32, 32);
    themeToggleBtn_->setAutoRaise(true);
    themeToggleBtn_->setCursor(Qt::PointingHandCursor);
    themeToggleBtn_->setFocusPolicy(Qt::NoFocus);
    themeToggleBtn_->setToolTip(QString::fromUtf8("切换亮/暗主题"));
    themeToggleBtn_->setIcon(Fluent::icon(Fluent::IconType::BRIGHTNESS));
    themeToggleBtn_->setIconSize(QSize(16, 16));
    connect(themeToggleBtn_, &QToolButton::clicked, this, [this]() {
        Theme::toggleTheme();
        bool dark = Theme::isDark();
        QSettings settings("MiniLang", "MiniLang IDE");
        settings.setValue("theme/dark", dark);
        // 同步编辑器主题
        if (codeEditor_) codeEditor_->setDarkTheme(dark);
        // 同步 ADS 配色
        if (dockManager_) {
            dockManager_->setColorSchemeMode(
                ads::CDockManager::ColorSchemeMode::FollowPalette);
        }
        // 刷新所有 QSS
        StyleSheet::updateStyleSheet();
        applyFluentStyle();
    });
    layout->addWidget(themeToggleBtn_);

    // 分隔线：工具栏 ↔ 窗口按钮
    addSeparator();

    // ---- 右侧：窗口控制按钮 ----
    auto makeWinBtn = [this](Fluent::IconType icon, const QString& tip) {
        auto* btn = new QToolButton(titleBar_);
        btn->setFixedSize(40, 36);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setCursor(Qt::ArrowCursor);
        btn->setToolTip(tip);
        btn->setIcon(Fluent::icon(icon));
        btn->setIconSize(QSize(12, 12));
        return btn;
    };

    titleMinBtn_ = makeWinBtn(Fluent::IconType::MINIMIZE, QString::fromUtf8("最小化"));
    titleMinBtn_->setObjectName("titleMinBtn");
    titleMaxBtn_ = makeWinBtn(Fluent::IconType::FULL_SCREEN, QString::fromUtf8("最大化"));
    titleMaxBtn_->setObjectName("titleMaxBtn");
    titleCloseBtn_ = makeWinBtn(Fluent::IconType::CLOSE, QString::fromUtf8("关闭"));
    titleCloseBtn_->setObjectName("titleCloseBtn");

    connect(titleMinBtn_, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(titleMaxBtn_, &QToolButton::clicked, this, [this]() {
        if (isMaximized()) showNormal();
        else showMaximized();
    });
    connect(titleCloseBtn_, &QToolButton::clicked, this, &QWidget::close);

    layout->addWidget(titleMinBtn_);
    layout->addWidget(titleMaxBtn_);
    layout->addWidget(titleCloseBtn_);

    // 拖拽移动窗口 + 双击最大化/还原
    titleBar_->installEventFilter(this);
}

// ============================================================
// UI initialization (ADS-based + ActivityBar + Pivot panels)
// ============================================================

void Ide::initUI() {
    setMinimumSize(800, 600);
    resize(1280, 800);

    // ---- Create content widgets before ADS docks ----

    // File tree
    fileTree_ = new QTreeWidget;
    fileTree_->setObjectName("fileTree");
    fileTree_->setHeaderHidden(true);
    fileTree_->setAnimated(true);
    fileTree_->setIndentation(12);
    fileTree_->setExpandsOnDoubleClick(true);
    fileTree_->setContextMenuPolicy(Qt::CustomContextMenu);

    // Debug panel
    debugPanel_ = new DebugPanel;

    // Output text edit
    outputTextEdit_ = new QTextEdit;
    outputTextEdit_->setReadOnly(true);
    QFont outputFont("Consolas", 10);
    outputFont.setStyleHint(QFont::Monospace);
    outputTextEdit_->setFont(outputFont);
    outputTextEdit_->document()->setMaximumBlockCount(10000);
    outputTextEdit_->setObjectName("outputEdit");

    // Error list
    errorListWidget_ = new QListWidget;
    errorListWidget_->setObjectName("errorList");
    errorListWidget_->setFont(GuiTextUtils::monospaceFont(10));
    errorListWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    errorListWidget_->setSpacing(0);
    connect(errorListWidget_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        bool ok = false;
        int line = item->data(Qt::UserRole).toInt(&ok);
        if (ok && line > 0 && codeEditor_) codeEditor_->gotoLine(line);
    });

    // REPL panel
    replPanel_ = new ReplPanel;

    // Token table (第八轮：列顺序 行号、列号、类型、词素、字面量)
    tokenTable_ = new QTableWidget;
    tokenTable_->setColumnCount(5);
    tokenTable_->setHorizontalHeaderLabels(
        {QString::fromUtf8("行号"), QString::fromUtf8("列号"),
         QString::fromUtf8("类型"), QString::fromUtf8("词素"),
         QString::fromUtf8("字面量")});
    tokenTable_->horizontalHeader()->setStretchLastSection(true);
    tokenTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tokenTable_->setAlternatingRowColors(true);
    tokenTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tokenTable_->setFont(GuiTextUtils::monospaceFont(10));
    tokenTable_->setVerticalScrollBar(new ScrollBar(tokenTable_));
    tokenTable_->setHorizontalScrollBar(new ScrollBar(tokenTable_));
    tokenTable_->verticalHeader()->setVisible(false);

    // IR viewer
    irViewer_ = new IrViewer;

    // Bytecode list + VM stack panel (will be placed in a splitter on the bytecode page)
    // 第八轮：使用 RichTextItemDelegate 支持语法高亮
    bytecodeList_ = new QListWidget;
    bytecodeList_->setObjectName("bytecodeList");
    bytecodeList_->setFont(GuiTextUtils::monospaceFont(10));
    bytecodeList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    bytecodeList_->setAlternatingRowColors(true);
    bytecodeList_->setItemDelegate(new RichTextItemDelegate(bytecodeList_));
    vmStackPanel_ = new VmStackPanel;
    vmStackPanel_->setMinimumWidth(180);

    // AST viewer (lives in an independent top-level window)
    astViewer_ = new AstViewer;

    // Welcome page
    initWelcomePage();

    // Editor tab widget
    editorTabWidget_ = new QTabWidget;
    editorTabWidget_->setObjectName("editorTabWidget");
    editorTabWidget_->setTabsClosable(true);
    editorTabWidget_->setMovable(true);
    editorTabWidget_->setDocumentMode(true);
    editorTabWidget_->tabBar()->installEventFilter(this);
    editorTabWidget_->tabBar()->setAutoHide(false);
    editorTabWidget_->tabBar()->setMouseTracking(true);
    editorTabWidget_->setMouseTracking(true);

    // Central stack: welcome page <-> editor tabs
    centerStack_ = new QStackedWidget;
    centerStack_->addWidget(welcomePage_);
    centerStack_->addWidget(editorTabWidget_);

    // ---- Activity bar (left-most 48px) ----
    activityBar_ = new ActivityBar;
    activityBar_->addItem(QString::fromUtf8("资源管理器"), Fluent::IconType::FOLDER);
    activityBar_->addItem(QString::fromUtf8("调试"), Fluent::IconType::DEVELOPER_TOOLS);
    connect(activityBar_, &ActivityBar::currentChanged, this, &Ide::onActivityChanged);

    // ---- Bottom panel container: Pivot + QStackedWidget ----
    // 第十一轮：默认高度 600px，最小 200px；标签栏压缩到 28px，紧凑左对齐
    auto* bottomContainer = new QWidget;
    bottomContainer->setObjectName("bottomPanelContainer");
    bottomContainer->setMinimumHeight(200);
    auto* bottomLayout = new QVBoxLayout(bottomContainer);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(0);

    // 第十一轮：Pivot 标签栏行（标签左对齐 + 右侧关闭按钮）
    auto* pivotRow = new QHBoxLayout;
    pivotRow->setContentsMargins(8, 0, 4, 0);
    pivotRow->setSpacing(0);

    bottomPivot_ = new Pivot(bottomContainer);
    bottomPivot_->setObjectName("bottomPivot");
    // 第十一轮：标签紧凑化 — 12px 字号，28px 行高，主题蓝下划线
    bottomPivot_->setItemFontSize(12);
    bottomPivot_->setIndicatorColor(QColor("#0078d4"), QColor("#0078d4"));
    bottomPivot_->setFixedHeight(28);
    bottomPivot_->addItem("output", QString::fromUtf8("输出"));
    bottomPivot_->addItem("errors", QString::fromUtf8("问题"));
    bottomPivot_->addItem("repl", QString::fromUtf8("REPL"));
    pivotRow->addWidget(bottomPivot_);

    pivotRow->addStretch();

    // 第十一轮：右侧关闭按钮（紧凑圆形，与 VS Code 一致）
    auto* bottomCloseBtn = new QToolButton(bottomContainer);
    bottomCloseBtn->setObjectName("panelCloseBtn");
    bottomCloseBtn->setText(QString::fromUtf8("×"));
    bottomCloseBtn->setFixedSize(20, 20);
    bottomCloseBtn->setToolTip(QString::fromUtf8("关闭面板"));
    bottomCloseBtn->setCursor(Qt::PointingHandCursor);
    connect(bottomCloseBtn, &QToolButton::clicked, this, [this]() {
        hideBottomPanel();
    });
    pivotRow->addWidget(bottomCloseBtn);

    auto* pivotRowWidget = new QWidget;
    pivotRowWidget->setObjectName("bottomPivotRow");
    auto* pivotRowLayout = new QVBoxLayout(pivotRowWidget);
    pivotRowLayout->setContentsMargins(0, 0, 0, 0);
    pivotRowLayout->setSpacing(0);
    pivotRowLayout->addLayout(pivotRow);
    bottomLayout->addWidget(pivotRowWidget);

    bottomStack_ = new QStackedWidget(bottomContainer);
    bottomStack_->addWidget(outputTextEdit_);
    bottomStack_->addWidget(errorListWidget_);
    bottomStack_->addWidget(replPanel_);
    bottomLayout->addWidget(bottomStack_, 1);
    connect(bottomPivot_, &Pivot::currentItemChanged, this, &Ide::onBottomPivotChanged);

    // ---- Right panel container: Pivot + close button + QStackedWidget ----
    // 第十一轮：默认宽度 800px，最小 500px；标签栏 28px 紧凑
    auto* rightContainer = new QWidget;
    rightContainer->setObjectName("rightPanelContainer");
    rightContainer->setMinimumWidth(500);
    rightContainer->setMaximumWidth(1200);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    // 第十一轮：Pivot 标签栏行（标签左对齐 + 右侧关闭按钮）
    auto* rightPivotRow = new QHBoxLayout;
    rightPivotRow->setContentsMargins(8, 0, 4, 0);
    rightPivotRow->setSpacing(0);

    rightPivot_ = new Pivot(rightContainer);
    rightPivot_->setObjectName("rightPivot");
    // 第十一轮：12px 字号，标签栏 28px，2px 主题蓝下划线
    rightPivot_->setItemFontSize(12);
    rightPivot_->setIndicatorColor(QColor("#0078d4"), QColor("#0078d4"));
    rightPivot_->setFixedHeight(28);
    rightPivot_->addItem("token", QString::fromUtf8("词法Token"));
    rightPivot_->addItem("ir", QString::fromUtf8("中间IR"));
    rightPivot_->addItem("bytecode", QString::fromUtf8("字节码"));
    rightPivotRow->addWidget(rightPivot_);

    rightPivotRow->addStretch();

    // 第十一轮：右侧关闭按钮
    auto* rightCloseBtn = new QToolButton(rightContainer);
    rightCloseBtn->setObjectName("panelCloseBtn");
    rightCloseBtn->setText(QString::fromUtf8("×"));
    rightCloseBtn->setFixedSize(20, 20);
    rightCloseBtn->setToolTip(QString::fromUtf8("关闭面板"));
    rightCloseBtn->setCursor(Qt::PointingHandCursor);
    connect(rightCloseBtn, &QToolButton::clicked, this, [this]() {
        hideRightPanel();
    });
    rightPivotRow->addWidget(rightCloseBtn);

    auto* rightPivotRowWidget = new QWidget;
    rightPivotRowWidget->setObjectName("rightPivotRow");
    auto* rightPivotRowLayout = new QVBoxLayout(rightPivotRowWidget);
    rightPivotRowLayout->setContentsMargins(0, 0, 0, 0);
    rightPivotRowLayout->setSpacing(0);
    rightPivotRowLayout->addLayout(rightPivotRow);
    rightLayout->addWidget(rightPivotRowWidget);

    rightStack_ = new QStackedWidget(rightContainer);
    rightStack_->addWidget(tokenTable_);    // index 0 → token
    rightStack_->addWidget(irViewer_);      // index 1 → IR

    // Bytecode page: bytecode list + VM stack panel in a splitter
    // 第八轮：普通查看模式仅展示纯字节码，VM 调试状态才追加操作数栈/全局变量
    auto* bytecodePage = new QWidget;
    auto* bcLayout = new QHBoxLayout(bytecodePage);
    bcLayout->setContentsMargins(0, 0, 0, 0);
    bcLayout->setSpacing(2);
    auto* bcSplitter = new QSplitter(Qt::Horizontal, bytecodePage);
    bcSplitter->addWidget(bytecodeList_);
    bcSplitter->addWidget(vmStackPanel_);
    bcSplitter->setStretchFactor(0, 3);
    bcSplitter->setStretchFactor(1, 2);
    bcSplitter->setHandleWidth(3);
    bcLayout->addWidget(bcSplitter);
    rightStack_->addWidget(bytecodePage);   // index 2 → bytecode
    // 普通模式隐藏 VM 调试面板（仅 VM 单步调试时显示）
    vmStackPanel_->hide();

    rightLayout->addWidget(rightStack_, 1);
    connect(rightPivot_, &Pivot::currentItemChanged, this, &Ide::onRightPivotChanged);

    // ---- AST independent window ----
    astWindow_ = new QWidget(nullptr);
    astWindow_->setWindowTitle(QString::fromUtf8("AST 树形图"));
    astWindow_->setWindowFlags(Qt::Window);
    astWindow_->resize(800, 600);
    auto* astLayout = new QVBoxLayout(astWindow_);
    astLayout->setContentsMargins(0, 0, 0, 0);
    astLayout->setSpacing(0);
    astLayout->addWidget(astViewer_);
    // Re-parent the astViewer now that it's in the window
    astViewer_->setParent(astWindow_);

    // ---- Main container: TitleBar + MenuBar + Toolbar + (ActivityBar | DockManager) ----
    // 第九轮：自定义顶部三层整合（标题栏 + 菜单栏 + 工具栏），扁平白色风格
    auto* mainContainer = new QWidget;
    mainContainer->setObjectName("mainContainer");
    auto* mainLayout = new QVBoxLayout(mainContainer);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 第十二轮：顶部仅统一标题栏（36px，已融合菜单+工具栏+窗口控制）
    mainLayout->addWidget(titleBar_);

    // 中间区域：活动栏 + ADS 停靠管理器
    auto* middleArea = new QWidget;
    middleArea->setObjectName("middleArea");
    auto* middleLayout = new QHBoxLayout(middleArea);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->setSpacing(0);
    middleLayout->addWidget(activityBar_);

    // ---- ADS Dock Manager setup ----
    // Config flags must be set BEFORE creating the manager
    ads::CDockManager::setConfigFlags(
        ads::CDockManager::DefaultOpaqueConfig
        | ads::CDockManager::MiddleMouseButtonClosesTab
        | ads::CDockManager::FocusHighlighting);
    ads::CDockManager::setAutoHideConfigFlags(
        ads::CDockManager::DefaultAutoHideConfig);

    dockManager_ = new ads::CDockManager(middleArea);
    dockManager_->setColorSchemeMode(
        ads::CDockManager::ColorSchemeMode::FollowPalette);
    middleLayout->addWidget(dockManager_, 1);

    mainLayout->addWidget(middleArea, 1);
    setCentralWidget(mainContainer);

    // Central dock widget (editor area) — must be set FIRST
    auto* centralDock = dockManager_->createDockWidget("Editor");
    centralDock->setWidget(centerStack_, ads::CDockWidget::ForceNoScrollArea);
    centralDock->setFeature(ads::CDockWidget::NoTab, true);
    dockManager_->setCentralWidget(centralDock);

    // Left panel: file tree
    fileTreeDock_ = dockManager_->createDockWidget(QString::fromUtf8("资源管理器"));
    fileTreeDock_->setWidget(fileTree_, ads::CDockWidget::ForceNoScrollArea);
    dockManager_->addDockWidget(ads::LeftDockWidgetArea, fileTreeDock_);

    // Left panel: debug (tabbed with file tree)
    debugPanelDock_ = dockManager_->createDockWidget(QString::fromUtf8("调试"));
    debugPanelDock_->setWidget(debugPanel_, ads::CDockWidget::ForceNoScrollArea);
    dockManager_->addDockWidgetTabToArea(
        debugPanelDock_, fileTreeDock_->dockAreaWidget());

    // Bottom panel: single dock containing Pivot + stack (output/errors/REPL)
    bottomDock_ = dockManager_->createDockWidget(QString::fromUtf8("面板"));
    bottomDock_->setWidget(bottomContainer, ads::CDockWidget::ForceNoScrollArea);
    dockManager_->addDockWidget(ads::BottomDockWidgetArea, bottomDock_);

    // Right panel: single dock containing Pivot + stack (token/IR/bytecode)
    rightDock_ = dockManager_->createDockWidget(QString::fromUtf8("编译分析"));
    rightDock_->setWidget(rightContainer, ads::CDockWidget::ForceNoScrollArea);
    dockManager_->addDockWidget(ads::RightDockWidgetArea, rightDock_);
    // 第十二轮：所有面板支持完整拖拽重组、浮动、标签分组（移除旧的浮动/移动锁定）

    // 第十二轮：面板尺寸对齐规范（左260px、底220px、右320px）
    // 所有面板支持拖拽重组、浮动、标签分组（布局持久化由 saveLayout/restoreLayout 处理）
    fileTree_->setMinimumWidth(200);
    debugPanel_->setMinimumWidth(200);
    bottomContainer->setMinimumHeight(160);
    rightContainer->setMinimumWidth(280);
    rightContainer->setMaximumWidth(1200);

    // 延迟调整 dock 区域尺寸 + 隐藏单 widget 标题栏
    QTimer::singleShot(0, this, [this]() {
        if (fileTreeDock_ && !fileTreeDock_->isClosed()) {
            if (auto* area = fileTreeDock_->dockAreaWidget()) {
                area->resize(260, area->height());
            }
        }
        if (rightDock_ && !rightDock_->isClosed()) {
            if (auto* area = rightDock_->dockAreaWidget()) {
                area->resize(320, area->height());
            }
        }
        // 隐藏底部 dock 的 ADS 标题栏（Pivot 作为唯一标题行）
        if (bottomDock_ && bottomDock_->dockAreaWidget()) {
            bottomDock_->dockAreaWidget()->setDockAreaFlag(
                ads::CDockAreaWidget::HideSingleWidgetTitleBar, true);
        }
        // 隐藏右侧 dock 的 ADS 标题栏（Pivot 作为标签栏）
        if (rightDock_ && rightDock_->dockAreaWidget()) {
            rightDock_->dockAreaWidget()->setDockAreaFlag(
                ads::CDockAreaWidget::HideSingleWidgetTitleBar, true);
        }
    });

    // 第九轮：启动时隐藏所有停靠面板（仅保留活动栏 + 顶部 + 欢迎页）
    fileTreeDock_->toggleView(false);
    debugPanelDock_->toggleView(false);
    bottomDock_->toggleView(false);
    rightDock_->toggleView(false);

    // Connect file tree context menu
    connect(fileTree_, &QWidget::customContextMenuRequested,
            this, &Ide::onFileTreeContextMenu);

    // 第九轮：连接 dock viewToggled → 防抖保存 + 视图菜单勾选同步
    auto connectDockSave = [this](ads::CDockWidget* dock) {
        if (dock) {
            connect(dock, &ads::CDockWidget::viewToggled,
                    this, [this]() {
                        if (splitterSaveTimer_) splitterSaveTimer_->start();
                        syncViewMenuChecks();
                    });
        }
    };
    connectDockSave(fileTreeDock_);
    connectDockSave(debugPanelDock_);
    connectDockSave(bottomDock_);
    connectDockSave(rightDock_);
    // focusedDockWidgetChanged fires when user interacts with dock widgets (drag/dock)
    connect(dockManager_, &ads::CDockManager::focusedDockWidgetChanged,
            this, [this]() { if (splitterSaveTimer_) splitterSaveTimer_->start(); });
}



// ============================================================
// syncViewMenuChecks — 第九轮：视图菜单勾选状态与 dock 显隐双向同步
// ============================================================

void Ide::syncViewMenuChecks() {
    if (syncingViewAction_) return;
    syncingViewAction_ = true;
    if (viewExplorerAction_)
        viewExplorerAction_->setChecked(fileTreeDock_ && !fileTreeDock_->isClosed());
    if (viewDebugAction_)
        viewDebugAction_->setChecked(debugPanelDock_ && !debugPanelDock_->isClosed());
    if (viewOutputAction_)
        viewOutputAction_->setChecked(bottomDock_ && !bottomDock_->isClosed());
    if (viewCompileAnalysisAction_)
        viewCompileAnalysisAction_->setChecked(rightDock_ && !rightDock_->isClosed());
    syncingViewAction_ = false;
}

// ============================================================
// Status bar initialization
// ============================================================

void Ide::initStatusBar() {
    auto* sb = statusBar();
    sb->setFixedHeight(24);
    statusLineLabel_ = new QLabel(QString::fromUtf8("行 1"));
    statusColLabel_ = new QLabel(QString::fromUtf8("列 1"));
    statusSaveLabel_ = new QLabel(QString());
    statusRunLabel_ = new QLabel(QString());
    statusEncodingLabel_ = new QLabel(QString::fromUtf8("UTF-8"));

    // 第八轮：左侧显示行列，右侧显示编码/保存/运行状态
    sb->addWidget(statusLineLabel_);
    sb->addWidget(statusColLabel_);
    sb->addPermanentWidget(statusEncodingLabel_);
    sb->addPermanentWidget(statusSaveLabel_);
    sb->addPermanentWidget(statusRunLabel_);
}

// ============================================================
// Fluent styling
// ============================================================

void Ide::applyFluentStyle() {
    // ---- Register native widgets with QFluentKit style sheet manager ----
    if (fileTree_)         StyleSheet::registerWidget(fileTree_, Fluent::ThemeStyle::LIST_VIEW);
    if (editorTabWidget_)  StyleSheet::registerWidget(editorTabWidget_, Fluent::ThemeStyle::TAB_VIEW);
    if (errorListWidget_)  StyleSheet::registerWidget(errorListWidget_, Fluent::ThemeStyle::LIST_VIEW);
    if (bytecodeList_)     StyleSheet::registerWidget(bytecodeList_, Fluent::ThemeStyle::LIST_VIEW);
    if (recentListWidget_) StyleSheet::registerWidget(recentListWidget_, Fluent::ThemeStyle::LIST_VIEW);
    if (tokenTable_)       StyleSheet::registerWidget(tokenTable_, Fluent::ThemeStyle::TABLE_VIEW);

    // ---- Replace native scrollbars with Fluent scrollbars ----
    if (outputTextEdit_) {
        outputTextEdit_->setVerticalScrollBar(new ScrollBar(outputTextEdit_));
        outputTextEdit_->setHorizontalScrollBar(new ScrollBar(Qt::Horizontal, outputTextEdit_));
    }
    if (errorListWidget_) errorListWidget_->setVerticalScrollBar(new ScrollBar(errorListWidget_));
    if (bytecodeList_)    bytecodeList_->setVerticalScrollBar(new ScrollBar(bytecodeList_));
    if (fileTree_)        fileTree_->setVerticalScrollBar(new ScrollBar(fileTree_));

    // ---- Theme-aware color palette ----
    bool dark = Theme::isDark();
    QString bgMain    = dark ? "#1e1e1e" : "#ffffff";
    QString bgPanel   = dark ? "#252526" : "#f3f3f3";
    QString bgSidebar = dark ? "#181818" : "#f8f8f8";
    QString fgPrimary = dark ? "#cccccc" : "#1e1e1e";
    QString fgSecondary = dark ? "#858585" : "#6e6e6e";
    QString borderColor = dark ? "#3c3c3c" : "#e5e5e5";
    QString accentColor = dark ? "#4cc2ff" : "#0078d4";
    QString hoverBg   = dark ? "#2a2d2e" : "#f0f0f0";
    QString selectedBg = dark ? "#37373d" : "#e8e8e8";
    QString titleBg   = dark ? "#1f1f1f" : "#ffffff";
    QString statusBg  = dark ? "#181818" : "#f3f3f3";
    QString editorBg  = dark ? "#1e1e1e" : "#ffffff";
    QString lineNumBg = dark ? "#1e1e1e" : "#f5f5f5";
    QString lineNumFg = dark ? "#858585" : "#999999";

    // ============================================================
    // ADS (Qt Advanced Docking System) comprehensive QSS override
    // 完全覆盖 ADS 默认样式，对齐 Fluent Design 规范
    // ============================================================
    QString adsQss = QString(R"(
        /* ---- ADS Tab Bar ---- */
        ads--CDockAreaWidget {
            background: %1;
        }
        ads--CDockAreaTabBar {
            background: transparent;
            border: none;
        }
        ads--CDockWidgetTab {
            background: transparent;
            border: none;
            border-bottom: 2px solid transparent;
            padding: 4px 12px;
            color: %2;
            font-size: 12px;
            min-height: 32px;
        }
        ads--CDockWidgetTab[activeTab="true"] {
            background: %1;
            border-bottom: 2px solid %3;
            color: %4;
        }
        ads--CDockWidgetTab:hover:!activeTab {
            background: %5;
        }

        /* Tab close button: only show on hover */
        ads--CDockWidgetTab QPushButton#tabCloseButton {
            background: transparent;
            border: none;
            padding: 2px;
            min-width: 16px;
            min-height: 16px;
            max-width: 16px;
            max-height: 16px;
        }
        ads--CDockWidgetTab QPushButton#tabCloseButton:hover {
            background: %6;
            border-radius: 3px;
        }

        /* ---- ADS Title Bar (dock area header) ---- */
        ads--CDockAreaTitleBar {
            background: transparent;
            border: none;
            min-height: 28px;
            padding: 0 4px;
        }
        ads--CDockAreaTitleBar QLabel {
            color: %2;
            font-size: 12px;
            padding-left: 4px;
        }
        ads--CDockAreaTitleBar QPushButton {
            background: transparent;
            border: none;
            min-width: 20px;
            min-height: 20px;
            max-width: 20px;
            max-height: 20px;
            padding: 2px;
        }
        ads--CDockAreaTitleBar QPushButton:hover {
            background: %5;
            border-radius: 3px;
        }

        /* ---- ADS Splitter (1px thin line, hover → 2px accent) ---- */
        ads--CDockSplitter {
            background: %1;
        }
        ads--CDockSplitter::handle {
            background: %7;
        }
        ads--CDockSplitter::handle:horizontal {
            width: 1px;
        }
        ads--CDockSplitter::handle:vertical {
            height: 1px;
        }
        ads--CDockSplitter::handle:hover {
            background: %3;
        }
        ads--CDockSplitter::handle:horizontal:hover {
            width: 2px;
        }
        ads--CDockSplitter::handle:vertical:hover {
            height: 2px;
        }

        /* ---- ADS Floating Window ---- */
        ads--CFloatingDockContainer {
            background: %1;
        }

        /* ---- ADS Dock Widget ---- */
        ads--CDockWidget {
            background: %1;
            border: none;
        }

        /* ---- ADS AutoHide Tabs ---- */
        ads--CAutoHideTab {
            background: %8;
            border: none;
            color: %2;
            padding: 4px 8px;
            min-height: 24px;
        }
        ads--CAutoHideTab:hover {
            background: %5;
        }
        ads--CAutoHideTab[activeTab="true"] {
            background: %1;
            border-left: 2px solid %3;
        }
        ads--CAutoHideSideBar {
            background: %8;
            border: none;
        }
    )").arg(bgMain, fgPrimary, accentColor, fgPrimary, hoverBg,
            dark ? "#505050" : "#d0d0d0",  /* close btn hover */
            borderColor, bgSidebar);

    // ---- Apply ADS QSS globally ----
    if (dockManager_) {
        qApp->setStyleSheet(qApp->styleSheet() + "\n" + adsQss);
        dockManager_->setColorSchemeMode(
            ads::CDockManager::ColorSchemeMode::FollowPalette);
    }

    // ============================================================
    // Title bar styling (统一标题栏)
    // ============================================================
    if (titleBar_) {
        titleBar_->setStyleSheet(QString(R"(
            #titleBar {
                background: %1;
                border-bottom: 1px solid %2;
            }
            #titleText {
                color: %3;
                background: transparent;
                border: none;
            }
            #titlePath {
                color: %4;
                background: transparent;
                border: none;
                padding-left: 4px;
            }
            #titleBarSep {
                background: %2;
                max-width: 1px;
            }
            #titleMinBtn, #titleMaxBtn {
                background: transparent;
                border: none;
            }
            #titleMinBtn:hover, #titleMaxBtn:hover {
                background: %5;
            }
            #titleCloseBtn {
                background: transparent;
                border: none;
            }
            #titleCloseBtn:hover {
                background: #e81123;
            }
            #themeToggleBtn {
                background: transparent;
                border: none;
            }
            #themeToggleBtn:hover {
                background: %5;
                border-radius: 4px;
            }
        )").arg(titleBg, borderColor, fgPrimary, fgSecondary, hoverBg));
    }

    // ============================================================
    // Status bar styling
    // ============================================================
    if (auto* sb = statusBar()) {
        sb->setStyleSheet(QString(R"(
            QStatusBar {
                background: %1;
                border-top: 1px solid %2;
                color: %3;
                font-size: 12px;
                padding: 0 8px;
            }
            QStatusBar QLabel {
                color: %3;
                padding: 0 6px;
                background: transparent;
            }
        )").arg(statusBg, borderColor, fgSecondary));
    }

    // ============================================================
    // Panel container styling (bottom + right)
    // ============================================================
    QString panelQss = QString(R"(
        #bottomPanelContainer, #rightPanelContainer {
            background: %1;
            border: none;
        }
        #bottomPivotRow, #rightPivotRow {
            background: %1;
            border-bottom: 1px solid %2;
        }
        #panelCloseBtn {
            background: transparent;
            border: none;
            color: %3;
            font-size: 14px;
            font-weight: bold;
        }
        #panelCloseBtn:hover {
            background: %4;
            border-radius: 3px;
        }
    )").arg(bgPanel, borderColor, fgSecondary, hoverBg);

    // Apply panel styling via findChildren
    for (auto* obj : findChildren<QWidget*>()) {
        QString name = obj->objectName();
        if (name == "bottomPanelContainer" || name == "rightPanelContainer" ||
            name == "bottomPivotRow" || name == "rightPivotRow" ||
            name == "panelCloseBtn") {
            obj->setStyleSheet(panelQss);
        }
    }

    // ============================================================
    // Editor tab widget styling
    // ============================================================
    if (editorTabWidget_) {
        editorTabWidget_->setStyleSheet(QString(R"(
            QTabWidget::pane {
                background: %1;
                border: none;
            }
            QTabBar::tab {
                background: %2;
                border: none;
                border-bottom: 2px solid transparent;
                padding: 6px 16px;
                color: %3;
                min-width: 80px;
                max-height: 32px;
            }
            QTabBar::tab:selected {
                background: %1;
                border-bottom: 2px solid %4;
                color: %5;
            }
            QTabBar::tab:hover:!selected {
                background: %6;
            }
            QTabBar::close-button {
                background: transparent;
                border: none;
                padding: 2px;
            }
            QTabBar::close-button:hover {
                background: %7;
                border-radius: 3px;
            }
        )").arg(bgMain, bgPanel, fgSecondary, accentColor, fgPrimary,
                hoverBg, dark ? "#505050" : "#d0d0d0"));
    }

    // ============================================================
    // Tree/Table/List view common styling
    // ============================================================
    QString itemViewQss = QString(R"(
        QTreeView, QTableView, QListView {
            background: %1;
            border: none;
            color: %2;
            font-size: 13px;
            outline: none;
        }
        QTreeView::item, QTableView::item, QListView::item {
            padding: 2px 4px;
            border: none;
        }
        QTreeView::item:selected, QTableView::item:selected, QListView::item:selected {
            background: %3;
            color: %2;
        }
        QTreeView::item:hover, QTableView::item:hover, QListView::item:hover {
            background: %4;
        }
        QHeaderView::section {
            background: %5;
            color: %2;
            border: none;
            border-right: 1px solid %6;
            border-bottom: 1px solid %6;
            padding: 4px 8px;
            font-size: 12px;
        }
        QTableWidget {
            gridline-color: %6;
        }
    )").arg(bgMain, fgPrimary, selectedBg, hoverBg, bgPanel, borderColor);

    // Apply to all tree/table/list views
    if (fileTree_)       fileTree_->setStyleSheet(itemViewQss);
    if (errorListWidget_) errorListWidget_->setStyleSheet(itemViewQss);
    if (bytecodeList_)    bytecodeList_->setStyleSheet(itemViewQss);
    if (tokenTable_)      tokenTable_->setStyleSheet(itemViewQss);
    if (recentListWidget_) recentListWidget_->setStyleSheet(itemViewQss);

    // ============================================================
    // Activity bar styling
    // ============================================================
    if (activityBar_) {
        activityBar_->setStyleSheet(QString(R"(
            ActivityBar {
                background: %1;
                border-right: 1px solid %2;
            }
        )").arg(bgSidebar, borderColor));
    }

    // ============================================================
    // Main container / middle area
    // ============================================================
    for (auto* w : findChildren<QWidget*>()) {
        if (w->objectName() == "mainContainer" || w->objectName() == "middleArea") {
            w->setStyleSheet(QString("background: %1; border: none;").arg(bgMain));
        }
    }

    // ============================================================
    // Debug/VM button containers
    // ============================================================
    for (auto* w : findChildren<QWidget*>()) {
        if (w->objectName() == "debugButtonContainer" ||
            w->objectName() == "vmButtonContainer") {
            w->setStyleSheet("background: transparent; border: none;");
        }
    }

    // ---- Sync editor theme ----
    if (codeEditor_) codeEditor_->setDarkTheme(dark);
}

// ============================================================
// Status bar update (cursor position + save/run state)
// ============================================================

void Ide::updateStatusBar() {
    if (statusLineLabel_ && codeEditor_) {
        QTextCursor cur = codeEditor_->textCursor();
        int line = cur.blockNumber() + 1;
        int col = cur.columnNumber() + 1;
        statusLineLabel_->setText(QString::fromUtf8("行 %1").arg(line));
        statusColLabel_->setText(QString::fromUtf8("列 %1").arg(col));
    } else if (statusLineLabel_) {
        statusLineLabel_->setText(QString::fromUtf8("行 1"));
        statusColLabel_->setText(QString::fromUtf8("列 1"));
    }
    if (statusSaveLabel_) {
        statusSaveLabel_->setText(isDirty_ ? QString::fromUtf8("● 未保存")
                                            : QString());
    }
}

// ============================================================
// Connections
// ============================================================

void Ide::initConnections() {
    connect(stepInAction_, &QAction::triggered, this, &Ide::onStepIn);
    connect(stepOverAction_, &QAction::triggered, this, &Ide::onStepOver);
    connect(stepOutAction_, &QAction::triggered, this, &Ide::onStepOut);
    connect(resumeAction_, &QAction::triggered, this, &Ide::onResume);
    connect(stopAction_, &QAction::triggered, this, &Ide::onStop);
    connect(clearAction_, &QAction::triggered, this, &Ide::onClearOutput);
    connect(formatAction_, &QAction::triggered, this, &Ide::onFormat);
    // 第八轮：compileAnalysisAction_ 改为切换视图菜单的勾选状态
    connect(compileAnalysisAction_, &QAction::triggered, this, [this]() {
        if (viewCompileAnalysisAction_) viewCompileAnalysisAction_->toggle();
    });
    connect(astAction_, &QAction::triggered, this, &Ide::onShowAstTree);

    connect(vmStepAction_, &QAction::triggered, this, &Ide::onVmStep);
    connect(vmStepOverAction_, &QAction::triggered, this, &Ide::onVmStepOver);
    connect(vmStepOutAction_, &QAction::triggered, this, &Ide::onVmStepOut);
    connect(vmRunAction_, &QAction::triggered, this, &Ide::onVmRun);
    connect(vmStopAction_, &QAction::triggered, this, &Ide::onVmStop);

    // 第十二轮：执行引擎切换（0=树遍历解释器，1=栈式VM，2=寄存器式VM）
    if (engineCombo_) {
        connect(engineCombo_, &ComboBox::currentIndexChanged, this, [this](int index) {
            if (!controller_) return;
            // 寄存器式 VM 使用 setUseRegisterVM(true)
            // 栈式 VM / 树遍历解释器 使用 setUseRegisterVM(false)
            controller_->setUseRegisterVM(index == 2);
            appendOutput(QString::fromUtf8("--- 执行引擎切换: %1 ---").arg(engineCombo_->currentText()));
        });
    }

    connect(editorTabWidget_, &QTabWidget::currentChanged, this, &Ide::onCurrentTabChanged);
    connect(editorTabWidget_, &QTabWidget::tabCloseRequested, this, &Ide::onEditorTabCloseRequested);

    // Shortcuts
    auto* findSc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(findSc, &QShortcut::activated, this, [this]() { ensureEditorVisible(); onFind(); });
    auto* replaceSc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), this);
    connect(replaceSc, &QShortcut::activated, this, [this]() { ensureEditorVisible(); onReplace(); });
    auto* findNextSc = new QShortcut(QKeySequence(Qt::Key_F3), this);
    connect(findNextSc, &QShortcut::activated, this, [this]() {
        if (centerStack_->currentWidget() == editorTabWidget_) onFindNext();
    });
    auto* findPrevSc = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), this);
    connect(findPrevSc, &QShortcut::activated, this, [this]() {
        if (centerStack_->currentWidget() == editorTabWidget_) onFindPrev();
    });
    auto* escSc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escSc, &QShortcut::activated, this, [this]() {
        if (findReplacePanel_ && findReplacePanel_->isVisible())
            findReplacePanel_->closePanel();
    });

    connect(fileTree_, &QTreeWidget::itemActivated, this, &Ide::onFileTreeItemActivated);

    // Controller signals
    connect(controller_, &IdeController::outputReady, this, [this](const QString& msg) {
        appendOutput(msg);
        showBottomPanel(0);
    });
    connect(controller_, &IdeController::runOk, this, [this]() {
        appendOutput(QString::fromUtf8("--- 程序执行结束 ---"));
        showBottomPanel(0);
    });
    connect(controller_, &IdeController::stoppedByUser, this, [this]() {
        appendOutput(QString::fromUtf8("--- 调试终止 ---"));
        showBottomPanel(0);
    });
    connect(controller_, &IdeController::runtimeError, this,
        [this](const QString& msg, int line, int column) {
        Diagnostic diag(DiagLevel::Error, msg.toStdString(), line, column, DiagSource::Interpreter);
        appendError(QString::fromStdString(diag.format()), line, column);
        showBottomPanel(1);
        if (line > 0 && codeEditor_) {
            QSet<int> errorLines;
            errorLines.insert(line);
            codeEditor_->setErrorLines(errorLines);
        }
    });
    connect(controller_, &IdeController::genericError, this, [this](const QString& msg) {
        appendError(msg);
        showBottomPanel(1);
    });
    connect(controller_, &IdeController::pausedAt, this, &Ide::onPausedAt);
    connect(controller_, &IdeController::workerFinished, this, &Ide::onWorkerFinished);
    connect(controller_, &IdeController::vmRunPaused, this, &Ide::handleVmStepResult);
    connect(controller_, &IdeController::diagnosticsReady, this, &Ide::displayDiagnostics);
}

// ============================================================
// File tree
// ============================================================

void Ide::initFileTree() {
    connect(fileTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        bool isDir = item->data(0, Qt::UserRole + 1).toBool();
        if (!isDir) return;
        if (item->childCount() > 0) return;
        QString dirPath = item->data(0, Qt::UserRole).toString();
        populateDirChildren(fileTree_, item, dirPath, 0);
    });
}

void Ide::populateFileTree() {
    fileTree_->clear();
    if (workspaceDir_.isEmpty()) return;

    QDir rootDir(workspaceDir_);
    if (!rootDir.exists()) return;

    auto* rootItem = new QTreeWidgetItem(fileTree_);
    rootItem->setText(0, rootDir.dirName());
    rootItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    rootItem->setData(0, Qt::UserRole, workspaceDir_);
    rootItem->setData(0, Qt::UserRole + 1, true);
    rootItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);

    populateDirChildren(fileTree_, rootItem, workspaceDir_, 0);
    rootItem->setExpanded(true);
}

void Ide::onFileTreeItemActivated(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;
    QFileInfo fi(path);
    if (fi.isDir()) {
        if (item->isExpanded()) {
            item->setExpanded(false);
        } else {
            if (item->childCount() == 0)
                populateDirChildren(fileTree_, item, path, 0);
            item->setExpanded(true);
        }
    } else if (fi.isFile()) {
        ensureEditorVisible();
        int existingIdx = findTabForFile(path);
        if (existingIdx >= 0) switchToTab(existingIdx);
        else loadFile(path);
    }
}

void Ide::onFileTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = fileTree_->itemAt(pos);
    QMenu menu(this);
    menu.setObjectName("fileTreeMenu");
    auto* newFileAct = menu.addAction(QString::fromUtf8("新建文件"));
    auto* newFolderAct = menu.addAction(QString::fromUtf8("新建文件夹"));
    menu.addSeparator();
    auto* renameAct = menu.addAction(QString::fromUtf8("重命名"));
    auto* deleteAct = menu.addAction(QString::fromUtf8("删除"));
    bool isRootOrEmpty = (!item || !item->parent());
    renameAct->setEnabled(!isRootOrEmpty);
    deleteAct->setEnabled(!isRootOrEmpty);

    auto* chosen = menu.exec(fileTree_->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == newFileAct) onNewFileInTree();
    else if (chosen == newFolderAct) onNewFolderInTree();
    else if (chosen == renameAct) onRenameInTree();
    else if (chosen == deleteAct) onDeleteInTree();
}

void Ide::onNewFileInTree() {
    QTreeWidgetItem* cur = fileTree_->currentItem();
    QString targetDir = resolveTreeContextMenuTargetDir(fileTree_, cur);
    if (targetDir.isEmpty()) targetDir = workspaceDir_;
    if (targetDir.isEmpty()) return;
    bool ok = false;
    QString name = QInputDialog::getText(this, QString::fromUtf8("新建文件"),
        QString::fromUtf8("文件名 (将以 .mini 扩展名创建):"), QLineEdit::Normal,
        "untitled.mini", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();
    if (!name.endsWith(".mini") && !name.endsWith(".ml")) name += ".mini";
    if (name.contains("..") || name.contains('/') || name.contains('\\')) {
        InfoBar::warning(QString::fromUtf8("非法文件名"),
            QString::fromUtf8("文件名不得包含路径分隔符或父目录引用"),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    QString path = QDir(targetDir).filePath(name);
    QFile f(path);
    if (f.exists()) {
        InfoBar::warning(QString::fromUtf8("已存在"),
            QString::fromUtf8("文件已存在：") + name,
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    if (!f.open(QIODevice::WriteOnly)) {
        InfoBar::warning(QString::fromUtf8("错误"),
            QString::fromUtf8("无法创建文件：") + f.errorString(),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    f.close();
    populateFileTree();
}

void Ide::onNewFolderInTree() {
    QTreeWidgetItem* cur = fileTree_->currentItem();
    QString targetDir = resolveTreeContextMenuTargetDir(fileTree_, cur);
    if (targetDir.isEmpty()) targetDir = workspaceDir_;
    if (targetDir.isEmpty()) return;
    bool ok = false;
    QString name = QInputDialog::getText(this, QString::fromUtf8("新建文件夹"),
        QString::fromUtf8("文件夹名:"), QLineEdit::Normal, "", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();
    if (name.contains("..") || name.contains('/') || name.contains('\\')) {
        InfoBar::warning(QString::fromUtf8("非法名称"),
            QString::fromUtf8("名称不得包含路径分隔符或父目录引用"),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    QString path = QDir(targetDir).filePath(name);
    if (!QDir().mkdir(path)) {
        InfoBar::warning(QString::fromUtf8("错误"),
            QString::fromUtf8("无法创建文件夹（可能已存在）"),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    populateFileTree();
}

void Ide::onRenameInTree() {
    QTreeWidgetItem* cur = fileTree_->currentItem();
    if (!cur || !cur->parent()) return;
    QString oldPath = cur->data(0, Qt::UserRole).toString();
    QString oldName = cur->text(0);
    bool ok = false;
    QString newName = QInputDialog::getText(this, QString::fromUtf8("重命名"),
        QString::fromUtf8("新名称:"), QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName == oldName) return;
    newName = newName.trimmed();
    if (newName.contains("..") || newName.contains('/') || newName.contains('\\')) {
        InfoBar::warning(QString::fromUtf8("非法名称"),
            QString::fromUtf8("名称不得包含路径分隔符或父目录引用"),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    QString newPath = QFileInfo(oldPath).absolutePath() + "/" + newName;
    if (!QFile::rename(oldPath, newPath)) {
        InfoBar::warning(QString::fromUtf8("错误"),
            QString::fromUtf8("重命名失败"),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    populateFileTree();
}

void Ide::onDeleteInTree() {
    QTreeWidgetItem* cur = fileTree_->currentItem();
    if (!cur || !cur->parent()) return;
    QString path = cur->data(0, Qt::UserRole).toString();
    bool isDir = cur->data(0, Qt::UserRole + 1).toBool();
    auto ret = QMessageBox::question(this, QString::fromUtf8("确认删除"),
        QString::fromUtf8("确定删除 %1 ？").arg(cur->text(0)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;
    bool ok = false;
    if (isDir) ok = QDir(path).removeRecursively();
    else ok = QFile::remove(path);
    if (!ok) {
        InfoBar::warning(QString::fromUtf8("错误"),
            QString::fromUtf8("删除失败"),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    populateFileTree();
}

void Ide::openWorkspace(const QString& dirPath) {
    QDir dir(dirPath);
    if (!dir.exists()) return;
    workspaceDir_ = dir.absolutePath();
    hasWorkspace_ = true;
    QDir::setCurrent(workspaceDir_);

    QSettings settings("MiniLang", "MiniLang IDE");
    settings.setValue("workspace/path", workspaceDir_);

    addRecentWorkspace(workspaceDir_);
    populateFileTree();

    centerStack_->setCurrentWidget(editorTabWidget_);
    // 第九轮：打开文件夹后自动展开左侧文件树面板
    switchLeftToFileTree();
    syncViewMenuChecks();
    updateWindowTitle();
}

// ============================================================
// Layout save/restore (ADS)
// ============================================================

void Ide::saveLayout() {
    QSettings settings("MiniLang", "MiniLang IDE");
    if (dockManager_) {
        settings.setValue("layout/dockState", dockManager_->saveState());
    }
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", QMainWindow::saveState());
    // 第九轮：记忆输出面板高度（最小 200px）
    if (bottomDock_ && !bottomDock_->isClosed()) {
        if (auto* area = bottomDock_->dockAreaWidget()) {
            int h = area->height();
            if (h >= 200 && h <= 1200) {
                bottomPanelHeight_ = h;
                settings.setValue("layout/bottomPanelHeight", h);
            }
        }
    }
}

void Ide::restoreLayout() {
    QSettings settings("MiniLang", "MiniLang IDE");
    if (dockManager_) {
        QByteArray dockState = settings.value("layout/dockState").toByteArray();
        if (!dockState.isEmpty()) {
            dockManager_->restoreState(dockState);
        }
    }
    // 第十一轮：恢复输出面板记忆高度（默认 600px，最小 200px）
    int savedH = settings.value("layout/bottomPanelHeight", 600).toInt();
    if (savedH >= 200 && savedH <= 1200) bottomPanelHeight_ = savedH;
    QByteArray geometry = settings.value("window/geometry").toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    QByteArray winState = settings.value("window/state").toByteArray();
    if (!winState.isEmpty()) QMainWindow::restoreState(winState);
    // 第十一轮：恢复后重新隐藏底部/右侧 dock 标题栏
    QTimer::singleShot(0, this, [this]() {
        if (bottomDock_ && bottomDock_->dockAreaWidget()) {
            bottomDock_->dockAreaWidget()->setDockAreaFlag(
                ads::CDockAreaWidget::HideSingleWidgetTitleBar, true);
        }
        if (rightDock_ && rightDock_->dockAreaWidget()) {
            rightDock_->dockAreaWidget()->setDockAreaFlag(
                ads::CDockAreaWidget::HideSingleWidgetTitleBar, true);
        }
    });
    // 第九轮：恢复后同步视图菜单勾选状态
    syncViewMenuChecks();
}

// ============================================================
// Panel control (consolidated docks + Pivot switching)
// ============================================================

void Ide::showBottomPanel(int tabIndex) {
    if (!bottomDock_) return;
    if (bottomDock_->isClosed()) {
        bottomDock_->toggleView(true);
        // 第十一轮：弹出时恢复记忆高度（默认 600px）
        QTimer::singleShot(0, this, [this]() {
            if (bottomDock_ && !bottomDock_->isClosed()) {
                if (auto* area = bottomDock_->dockAreaWidget()) {
                    area->resize(area->width(), bottomPanelHeight_);
                }
            }
        });
    }
    bottomDock_->setAsCurrentTab();
    static const char* keys[] = {"output", "errors", "repl"};
    if (tabIndex < 0 || tabIndex >= 3) tabIndex = 0;
    if (bottomPivot_) bottomPivot_->setCurrentItem(keys[tabIndex]);
    syncViewMenuChecks();
}

void Ide::hideBottomPanel() {
    if (bottomDock_) bottomDock_->toggleView(false);
    syncViewMenuChecks();
}

void Ide::showRightPanel(int tabIndex) {
    if (!rightDock_) return;
    if (rightDock_->isClosed()) {
        rightDock_->toggleView(true);
        // 第十一轮：弹出时恢复默认宽度 800px
        QTimer::singleShot(0, this, [this]() {
            if (rightDock_ && !rightDock_->isClosed()) {
                if (auto* area = rightDock_->dockAreaWidget()) {
                    area->resize(800, area->height());
                }
            }
        });
    }
    rightDock_->setAsCurrentTab();
    static const char* keys[] = {"token", "ir", "bytecode"};
    if (tabIndex < 0 || tabIndex >= 3) tabIndex = 0;
    if (rightPivot_) rightPivot_->setCurrentItem(keys[tabIndex]);
    syncViewMenuChecks();
}

void Ide::hideRightPanel() {
    if (rightDock_) rightDock_->toggleView(false);
    syncViewMenuChecks();
}

void Ide::toggleBottomPanel() {
    if (!bottomDock_) return;
    bottomDock_->toggleView(!bottomDock_->isClosed() ? false : true);
}

void Ide::toggleRightPanel() {
    if (!rightDock_) return;
    rightDock_->toggleView(!rightDock_->isClosed() ? false : true);
}

void Ide::switchLeftToFileTree() {
    if (fileTreeDock_ && fileTreeDock_->isClosed())
        fileTreeDock_->toggleView(true);
    if (fileTreeDock_) fileTreeDock_->setAsCurrentTab();
    if (activityBar_) activityBar_->setCurrentIndex(0);
    syncViewMenuChecks();
}

void Ide::switchLeftToDebugPanel() {
    if (debugPanelDock_ && debugPanelDock_->isClosed())
        debugPanelDock_->toggleView(true);
    if (debugPanelDock_) debugPanelDock_->setAsCurrentTab();
    if (activityBar_) activityBar_->setCurrentIndex(1);
    syncViewMenuChecks();
}

void Ide::showAstWindow() {
    if (!astWindow_) return;
    restoreAstWindowGeometry();
    if (astWindow_->isHidden()) {
        astWindow_->show();
    } else {
        astWindow_->raise();
        astWindow_->activateWindow();
    }
}

void Ide::saveAstWindowGeometry() {
    if (!astWindow_) return;
    QSettings settings("MiniLang", "MiniLang IDE");
    settings.setValue("astWindow/geometry", astWindow_->saveGeometry());
}

void Ide::restoreAstWindowGeometry() {
    if (!astWindow_) return;
    QSettings settings("MiniLang", "MiniLang IDE");
    QByteArray geo = settings.value("astWindow/geometry").toByteArray();
    if (!geo.isEmpty()) astWindow_->restoreGeometry(geo);
}

void Ide::showDebugButtons(bool show) {
    if (debugButtonsVisible_ == show) return;
    debugButtonsVisible_ = show;
    if (debugButtonContainer_) debugButtonContainer_->setVisible(show);
    // 第八轮：分隔线随容器显隐，禁止灰化占位
    if (debugSepAction_) debugSepAction_->setVisible(show);
}

void Ide::showVmButtons(bool show) {
    if (vmButtonContainer_) vmButtonContainer_->setVisible(show);
    if (vmSepAction_) vmSepAction_->setVisible(show);
    // 第八轮：VM 调试面板（操作数栈/全局变量）仅 VM 单步模式时显示
    if (vmStackPanel_) vmStackPanel_->setVisible(show);
}

// ============================================================
// Output helpers
// ============================================================

void Ide::appendOutput(const QString& text) {
    GuiTextUtils::appendLine(outputTextEdit_, text);
}

void Ide::appendError(const QString& text, int line, int column) {
    if (!errorPanelHasErrors_) {
        errorListWidget_->clear();
        errorPanelHasErrors_ = true;
    }
    auto* item = new QListWidgetItem(text);
    item->setData(Qt::UserRole, line);
    errorListWidget_->addItem(item);
}

void Ide::clearOutput() {
    outputTextEdit_->clear();
    errorListWidget_->clear();
    errorPanelHasErrors_ = false;
}

void Ide::onClearOutput() {
    clearOutput();
}

// ============================================================
// Run / Debug
// ============================================================

bool Ide::blockIfHasErrors() {
    // 第八轮：运行/调试前拦截编译错误，0 错误才允许执行
    if (!codeEditor_) return false;
    std::string source = codeEditor_->toPlainText().toStdString();
    auto result = controller_->runFrontendPipeline(source);
    if (result.diagnostics && result.diagnostics->hasErrors()) {
        displayDiagnostics(*result.diagnostics);
        showBottomPanel(1);  // 错误标签
        return true;
    }
    // 清除残留的错误标记
    codeEditor_->clearErrorLines();
    return false;
}

void Ide::runRealTimeSyntaxCheck() {
    // 第八轮：实时语法检查（300ms 防抖触发）
    // 清空旧错误 → 全量扫描 → 按行号排序展示 → 更新波浪下划线（常驻不消失）
    if (!codeEditor_) return;

    std::string source = codeEditor_->toPlainText().toStdString();
    auto result = controller_->runFrontendPipeline(source);

    // 先清空旧的错误列表和波浪下划线，防止累积
    errorListWidget_->clear();
    errorPanelHasErrors_ = false;
    codeEditor_->clearErrorLines();

    if (!result.diagnostics || result.diagnostics->empty()) {
        // 无错误：确保标记清除，不弹出底部面板
        return;
    }

    // 按行号排序（同按列号），确保错误面板按行号顺序展示
    // diagnostics 是 const 指针，需拷贝后排序
    std::vector<Diagnostic> sortedDiags = result.diagnostics->all();
    std::stable_sort(sortedDiags.begin(), sortedDiags.end(),
        [](const Diagnostic& a, const Diagnostic& b) {
            if (a.line != b.line) return a.line < b.line;
            return a.column < b.column;
        });

    // 收集所有错误行用于波浪下划线
    std::vector<CodeEditor::ErrorRange> ranges;
    bool hasErrors = false;

    const auto& allDiags = sortedDiags;
    for (const auto& diag : allDiags) {
        std::string msg = diag.message;

        // 智能拼写纠错提示
        if (diag.isError() && !spellCandidates_.empty()) {
            std::string ident = extractQuotedIdentifier(msg);
            if (!ident.empty() && ident.size() > 1) {
                auto suggestion = SpellChecker::suggestSuffix(
                    ident, spellCandidates_, 2);
                if (!suggestion.empty()) {
                    msg += suggestion;
                }
            }
        }

        // 构建展示文本：[级别] (行 X, 列 Y): 消息
        QString text = QString::fromStdString(
            "[" + diag.sourceString() + "] " + diag.levelString());
        if (diag.line > 0) {
            text += QString(" (行 %1").arg(diag.line);
            if (diag.column > 0) text += QString(", 列 %1").arg(diag.column);
            text += ")";
        }
        text += ": " + QString::fromStdString(msg);

        if (diag.isError()) {
            appendError(text, diag.line, diag.column);
            hasErrors = true;
            if (diag.line > 0) {
                ranges.push_back({diag.line, diag.column, 0});
            }
        } else if (diag.isWarning()) {
            appendOutput(QString("[警告] ") + text);
        } else {
            appendOutput(text);
        }
    }

    // 更新波浪下划线（常驻，直到下次扫描清除）
    if (!ranges.empty()) {
        codeEditor_->setErrorRanges(ranges);
    }

    // 仅在有错误时弹出底部错误面板
    if (hasErrors) {
        showBottomPanel(1);
    }
}

void Ide::onRun() {
    if (!codeEditor_) return;
    std::string source = codeEditor_->toPlainText().toStdString();

    if (replPanel_->isReplRunning()) {
        appendError(QString::fromUtf8("REPL 正在执行，请等待其完成后再运行"));
        showBottomPanel(1);
        return;
    }

    clearOutput();
    debugPanel_->clearAll();
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    // 第八轮：存在编译错误时拦截运行
    if (blockIfHasErrors()) return;

    if (!controller_->prepareRun(false, source, currentFilePath_.toStdString())) return;

    updateTokenTable();
    updateAstViewer();
    setRunningState(true);
    replPanel_->setInputEnabled(false);

    try {
        controller_->startWorker();
    } catch (const std::exception& e) {
        appendError(QString::fromUtf8("启动失败: %1").arg(e.what()));
        showBottomPanel(1);
        setRunningState(false);
        replPanel_->setInputEnabled(true);
    } catch (...) {
        appendError(QString::fromUtf8("启动发生未知异常"));
        showBottomPanel(1);
        setRunningState(false);
        replPanel_->setInputEnabled(true);
    }
}

void Ide::onDebug() {
    if (!codeEditor_) return;
    std::string source = codeEditor_->toPlainText().toStdString();

    if (replPanel_->isReplRunning()) {
        appendError(QString::fromUtf8("REPL 正在执行，请等待其完成后再调试"));
        showBottomPanel(1);
        return;
    }

    clearOutput();
    debugPanel_->clearAll();
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    // 第八轮：存在编译错误时拦截调试
    if (blockIfHasErrors()) return;

    if (!controller_->prepareRun(true, source, currentFilePath_.toStdString())) return;

    updateTokenTable();
    updateAstViewer();
    setRunningState(true);
    replPanel_->setInputEnabled(false);
    switchLeftToDebugPanel();
    showDebugButtons(true);

    QSet<int> breakpoints = codeEditor_->getBreakpoints();
    QMap<int, std::string> conditions;
    for (int line : breakpoints) {
        std::string cond = codeEditor_->getBreakpointCondition(line);
        if (!cond.empty()) conditions[line] = cond;
    }

    try {
        controller_->setupDebug(breakpoints, conditions);
        controller_->startWorker();
    } catch (const std::exception& e) {
        appendError(QString::fromUtf8("启动调试失败: %1").arg(e.what()));
        showBottomPanel(1);
        setRunningState(false);
        replPanel_->setInputEnabled(true);
        switchLeftToFileTree();
        showDebugButtons(false);
    } catch (...) {
        appendError(QString::fromUtf8("启动调试发生未知异常"));
        showBottomPanel(1);
        setRunningState(false);
        replPanel_->setInputEnabled(true);
        switchLeftToFileTree();
        showDebugButtons(false);
    }
}

void Ide::onStepIn() {
    if (!codeEditor_) return;
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepIn();
    } catch (const std::exception& e) {
        appendError(QString::fromUtf8("单步进入失败: %1").arg(e.what()));
    } catch (...) {
        appendError(QString::fromUtf8("单步进入发生未知异常"));
    }
}

void Ide::onStepOver() {
    if (!codeEditor_) return;
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepOver();
    } catch (const std::exception& e) {
        appendError(QString::fromUtf8("单步跳过失败: %1").arg(e.what()));
    } catch (...) {
        appendError(QString::fromUtf8("单步跳过发生未知异常"));
    }
}

void Ide::onStepOut() {
    if (!codeEditor_) return;
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepOut();
    } catch (const std::exception& e) {
        appendError(QString::fromUtf8("单步跳出失败: %1").arg(e.what()));
    } catch (...) {
        appendError(QString::fromUtf8("单步跳出发生未知异常"));
    }
}

void Ide::onResume() {
    if (!codeEditor_) return;
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->resume();
    } catch (const std::exception& e) {
        appendError(QString::fromUtf8("继续执行失败: %1").arg(e.what()));
    } catch (...) {
        appendError(QString::fromUtf8("继续执行发生未知异常"));
    }
}

void Ide::onStop() {
    controller_->stop();
}

void Ide::onPausedAt(int line) {
    try {
        if (codeEditor_) codeEditor_->setCurrentLine(line);
        updateDebugInfo();
        switchLeftToDebugPanel();
        showDebugButtons(true);
        showBottomPanel(0);
    } catch (const std::exception& e) {
        appendError(QString::fromUtf8("调试信息更新失败: %1").arg(e.what()));
    } catch (...) {
        appendError(QString::fromUtf8("调试信息更新发生未知异常"));
    }
}

void Ide::onWorkerFinished(bool wasDebug) {
    setRunningState(false);
    if (codeEditor_) {
        codeEditor_->clearCurrentLine();
        codeEditor_->clearErrorLines();
    }
    replPanel_->setInputEnabled(true);
    if (wasDebug) {
        switchLeftToFileTree();
        showDebugButtons(false);
    }
}

// ============================================================
// Formatting
// ============================================================

void Ide::onFormat() {
    if (!codeEditor_) return;
    std::string source = codeEditor_->toPlainText().toStdString();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard { ~CursorGuard() { QApplication::restoreOverrideCursor(); } } guard;

    auto pipelineResult = controller_->runFrontendPipeline(source);
    updateTokenTable();

    if (pipelineResult.status != IdeController::PipelineStatus::OK) {
        if (!pipelineResult.errorMessage.empty()) {
            appendError(QString("[格式化] %1: %2")
                .arg(pipelineResult.status == IdeController::PipelineStatus::LexerFailed
                     ? QString::fromUtf8("词法异常") : QString::fromUtf8("解析异常"))
                .arg(QString::fromStdString(pipelineResult.errorMessage)));
            showBottomPanel(1);
        } else if (pipelineResult.diagnostics) {
            for (const auto& diag : pipelineResult.diagnostics->all()) {
                appendError(QString::fromStdString("[格式化] " + diag.format()));
            }
            showBottomPanel(1);
        }
        updateAstViewer();
        return;
    }
    updateAstViewer();

    if (!controller_->astRoot()) return;

    QSet<int> bps;
    if (codeEditor_) bps = codeEditor_->getBreakpoints();
    bool hadBreakpoints = !bps.isEmpty();
    QTextCursor savedCursor = codeEditor_->textCursor();
    int scrollPos = codeEditor_->verticalScrollBar()->value();

    std::string formatted;
    try {
        if (!controller_->formatCode(formatted)) return;
        codeEditor_->setPlainText(QString::fromStdString(formatted));
    } catch (const std::exception& e) {
        appendError(QString("[格式化] 格式化异常: %1").arg(e.what()));
        showBottomPanel(1);
        return;
    }

    if (hadBreakpoints) {
        appendOutput(QString::fromUtf8("[格式化] 断点已清除（行号变化，断点不再有效）"));
    }
    codeEditor_->setBreakpoints(QSet<int>());
    controller_->setBreakpoints(QSet<int>());
    updateAstViewer();

    if (savedCursor.position() <= codeEditor_->document()->characterCount()) {
        codeEditor_->setTextCursor(savedCursor);
    }
    codeEditor_->verticalScrollBar()->setValue(scrollPos);
}

// ============================================================
// Visualization (Token / IR / Bytecode) + AST
// ============================================================

void Ide::onCompileAnalysis() {
    if (!codeEditor_) {
        InfoBar::warning(QString::fromUtf8("编译分析"),
            QString::fromUtf8("请先打开或新建一个文件再进行编译分析。"),
            Qt::Horizontal, true, 3000, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    std::string source = codeEditor_->toPlainText().toStdString();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard { ~CursorGuard() { QApplication::restoreOverrideCursor(); } } guard;

    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();

    auto pipelineResult = controller_->runFrontendPipeline(source);
    updateTokenTable();
    updateAstViewer();

    if (pipelineResult.status != IdeController::PipelineStatus::OK) {
        if (pipelineResult.diagnostics) {
            displayDiagnostics(*pipelineResult.diagnostics);
        } else if (!pipelineResult.errorMessage.empty()) {
            appendError(QString("%1: %2")
                .arg(pipelineResult.status == IdeController::PipelineStatus::LexerFailed
                     ? QString::fromUtf8("词法异常") : QString::fromUtf8("解析异常"))
                .arg(QString::fromStdString(pipelineResult.errorMessage)));
            showBottomPanel(1);
        }
        bytecodeList_->clear();
        lastBytecodeSourceHash_ = 0;
        irViewer_->clearIR();
        showRightPanel(0);
        return;
    }

    showRightPanel(0);
    loadVisualizationForTab(0);
}

void Ide::onRightTabChanged(int index) {
    // Legacy slot retained for header compatibility.
    // Right-panel switching is now driven by onRightPivotChanged.
    Q_UNUSED(index);
}

void Ide::onActivityChanged(int index) {
    if (index == 0) {
        // Explorer: show file tree, hide debug panel
        if (debugPanelDock_ && !debugPanelDock_->isClosed())
            debugPanelDock_->toggleView(false);
        if (fileTreeDock_ && fileTreeDock_->isClosed())
            fileTreeDock_->toggleView(true);
        if (fileTreeDock_) fileTreeDock_->setAsCurrentTab();
    } else if (index == 1) {
        // Debug: show debug panel, hide file tree
        if (fileTreeDock_ && !fileTreeDock_->isClosed())
            fileTreeDock_->toggleView(false);
        if (debugPanelDock_ && debugPanelDock_->isClosed())
            debugPanelDock_->toggleView(true);
        if (debugPanelDock_) debugPanelDock_->setAsCurrentTab();
    }
    syncViewMenuChecks();
}

void Ide::onRightPivotChanged(const QString& routeKey) {
    if (!rightStack_ || !rightPivot_) return;
    // Only auto-load when the right panel is actually visible
    if (rightDock_ && rightDock_->isClosed()) return;

    int idx = 0;
    if (routeKey == "token") idx = 0;
    else if (routeKey == "ir") idx = 1;
    else if (routeKey == "bytecode") idx = 2;
    rightStack_->setCurrentIndex(idx);
    loadVisualizationForTab(idx);
}

void Ide::onBottomPivotChanged(const QString& routeKey) {
    if (!bottomStack_) return;
    if (routeKey == "output") bottomStack_->setCurrentIndex(0);
    else if (routeKey == "errors") bottomStack_->setCurrentIndex(1);
    else if (routeKey == "repl") bottomStack_->setCurrentIndex(2);
}

void Ide::loadVisualizationForTab(int tabIndex) {
    if (!controller_->astRoot()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard { ~CursorGuard() { QApplication::restoreOverrideCursor(); } } guard;

    switch (tabIndex) {
    case 0: // Token
        updateTokenTable();
        break;
    case 1: { // IR
        controller_->compiler().setUseIR(true);
        try {
            controller_->runCompiler();
            populateIRViewer();
        } catch (const std::exception& e) {
            irViewer_->clearIR();
            appendError(QString("IR 编译异常: %1").arg(e.what()));
            showBottomPanel(1);
        }
        controller_->compiler().setUseIR(false);
        break;
    }
    case 2: { // Bytecode
        try {
            controller_->runCompiler();
            populateBytecodeList();
            vmStackPanel_->clearAll();
            vmStepAction_->setEnabled(true);
            vmStopAction_->setEnabled(false);
            controller_->vmReset();
            showVmButtons(true);
        } catch (const std::exception& e) {
            bytecodeList_->clear();
            lastBytecodeSourceHash_ = 0;
            bytecodeList_->addItem(QString("字节码编译异常: %1").arg(e.what()));
        }
        break;
    }
    }
}

void Ide::onShowAstTree() {
    if (!codeEditor_) return;
    std::string source = codeEditor_->toPlainText().toStdString();
    auto pipelineResult = controller_->runFrontendPipeline(source);
    updateAstViewer();
    showAstWindow();
}

// ============================================================
// VM debugging
// ============================================================

void Ide::onVmStep() {
    if (controller_->isVmRunning()) return;
    bool hasCode = controller_->getUseRegisterVM()
        ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
        : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode) return;

    syncVmBreakpoints();
    setVmStepActionsEnabled(false);

    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_IN);
    } catch (const std::exception& e) {
        appendError(QString("VM 单步异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    } catch (...) {
        appendError(QString::fromUtf8("VM 单步发生未知异常"));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    }
    handleVmStepResult(result);
}

void Ide::onVmStepOver() {
    if (controller_->isVmRunning()) return;
    bool hasCode = controller_->getUseRegisterVM()
        ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
        : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode) return;

    syncVmBreakpoints();
    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_OVER);
    } catch (const std::exception& e) {
        appendError(QString("VM 跨过异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    } catch (...) {
        appendError(QString::fromUtf8("VM 跨过发生未知异常"));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    }
    handleVmStepResult(result);
}

void Ide::onVmStepOut() {
    if (controller_->isVmRunning()) return;
    bool hasCode = controller_->getUseRegisterVM()
        ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
        : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode) return;

    syncVmBreakpoints();
    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_OUT);
    } catch (const std::exception& e) {
        appendError(QString("VM 跨出异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    } catch (...) {
        appendError(QString::fromUtf8("VM 跨出发生未知异常"));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    }
    handleVmStepResult(result);
}

void Ide::onVmRun() {
    if (controller_->isVmRunning()) return;
    bool hasCode = controller_->getUseRegisterVM()
        ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
        : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode) return;

    syncVmBreakpoints();
    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::RUN);
    } catch (const std::exception& e) {
        appendError(QString("VM 运行异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    } catch (...) {
        appendError(QString::fromUtf8("VM 运行发生未知异常"));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    }
    if (result == IdeController::VmStepResult::RUNNING) {
        vmStepAction_->setEnabled(false);
        vmStepOverAction_->setEnabled(false);
        vmStepOutAction_->setEnabled(false);
        vmRunAction_->setEnabled(false);
        vmStopAction_->setEnabled(true);
        runAction_->setEnabled(false);
        debugAction_->setEnabled(false);
        if (codeEditor_) codeEditor_->setReadOnly(true);
        return;
    }
    handleVmStepResult(result);
}

void Ide::handleVmStepResult(IdeController::VmStepResult result) {
    switch (result) {
    case IdeController::VmStepResult::NOT_READY:
        setVmStepActionsEnabled(true, false);
        runAction_->setEnabled(true);
        debugAction_->setEnabled(true);
        if (codeEditor_) codeEditor_->setReadOnly(false);
        return;
    case IdeController::VmStepResult::RUNNING:
        vmStepAction_->setEnabled(false);
        vmStepOverAction_->setEnabled(false);
        vmStepOutAction_->setEnabled(false);
        vmRunAction_->setEnabled(false);
        vmStopAction_->setEnabled(true);
        return;
    case IdeController::VmStepResult::ERROR: {
        Diagnostic diag(DiagLevel::Error, controller_->getVmLastError(),
                        controller_->getVmLastErrorLine(), 0, DiagSource::VM);
        appendError(QString::fromStdString(diag.format()));
        showBottomPanel(1);
        if (controller_->getVmLastErrorLine() > 0 && codeEditor_) {
            QSet<int> errorLines;
            errorLines.insert(controller_->getVmLastErrorLine());
            codeEditor_->setErrorLines(errorLines);
        }
        vmStackPanel_->clearAll();
        setVmStepActionsEnabled(true, false);
        runAction_->setEnabled(true);
        debugAction_->setEnabled(true);
        if (codeEditor_) codeEditor_->setReadOnly(false);
        return;
    }
    case IdeController::VmStepResult::FINISHED:
        appendOutput(QString::fromUtf8("--- VM 执行结束 ---"));
        showBottomPanel(0);
        vmStackPanel_->clearAll();
        setVmStepActionsEnabled(true, false);
        runAction_->setEnabled(true);
        debugAction_->setEnabled(true);
        if (codeEditor_) codeEditor_->setReadOnly(false);
        return;
    case IdeController::VmStepResult::OK:
    case IdeController::VmStepResult::PAUSED_AT_BREAKPOINT:
        if (controller_->isVmRegisterMode()) {
            vmStackPanel_->updateRegisters(controller_->getVmStack());
        } else {
            vmStackPanel_->updateStack(controller_->getVmStack());
        }
        vmStackPanel_->updateGlobals(controller_->getVmGlobals());
        {
            size_t currentIP = controller_->getVmCurrentIP();
            std::string opName = controller_->getVmCurrentOpCodeName();
            int opLine = controller_->getVmCurrentLine();
            vmStackPanel_->updateCurrentOp(currentIP, opName, opLine);
            highlightBytecodeLine(controller_->getVmCurrentChunkName(), currentIP);
            if (controller_->getVmCurrentChunkName() == "main") {
                highlightIRLine(currentIP);
            }
        }
        if (result == IdeController::VmStepResult::PAUSED_AT_BREAKPOINT) {
            int breakLine = controller_->getVmCurrentLine();
            if (breakLine > 0 && codeEditor_) {
                codeEditor_->setCurrentLine(breakLine);
                appendOutput(QString("🔴 VM 命中断点: 第 %1 行").arg(breakLine));
                showBottomPanel(0);
            }
        }
        setVmStepActionsEnabled(true, true);
        return;
    }
}

void Ide::setVmStepActionsEnabled(bool enabled, bool running) {
    vmStepAction_->setEnabled(enabled);
    vmStepOverAction_->setEnabled(enabled);
    vmStepOutAction_->setEnabled(enabled);
    vmRunAction_->setEnabled(enabled);
    vmStopAction_->setEnabled(enabled && running);
    compileAnalysisAction_->setEnabled(enabled && !running);
}

void Ide::onVmStop() {
    controller_->vmStop();
    vmStackPanel_->clearAll();
    bytecodeList_->setCurrentRow(-1);
    if (codeEditor_) codeEditor_->setCurrentLine(-1);
    if (irViewer_) irViewer_->clearHighlight();
    setVmStepActionsEnabled(true, false);
    runAction_->setEnabled(true);
    debugAction_->setEnabled(true);
    if (codeEditor_) codeEditor_->setReadOnly(false);
}

// ============================================================
// Find / Replace
// ============================================================

void Ide::onFind() { if (findReplacePanel_) findReplacePanel_->showFind(); }
void Ide::onReplace() { if (findReplacePanel_) findReplacePanel_->showReplace(); }

void Ide::onFindNext() {
    if (findReplacePanel_ && findReplacePanel_->isVisible()) {
        findReplacePanel_->onFindNext();
        return;
    }
    if (findReplacePanel_) findReplacePanel_->showFind();
}

void Ide::onFindPrev() {
    if (findReplacePanel_ && findReplacePanel_->isVisible()) {
        findReplacePanel_->onFindPrev();
        return;
    }
    if (findReplacePanel_) findReplacePanel_->showFind();
}

// ============================================================
// Help dialog (Fluent-style, two-column shortcut layout)
// ============================================================

void Ide::showHelpDialog() {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(QString::fromUtf8("帮助"));
    dlg->setWindowFlags(dlg->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    // 第十一轮：宽 520px 高 420px，8px 圆角，柔和阴影
    dlg->setFixedSize(520, 420);

    // 第十一轮：主题感知样式（自动适配深色/浅色）
    bool isDark = (palette().color(QPalette::Window).lightness() < 128);
    QString bg = isDark ? QStringLiteral("#252526") : QStringLiteral("#ffffff");
    QString text = isDark ? QStringLiteral("#cccccc") : QStringLiteral("#1e1e1e");
    QString muted = isDark ? QStringLiteral("#737373") : QStringLiteral("#5a5a5a");
    QString keyColor = isDark ? QStringLiteral("#4fc1ff") : QStringLiteral("#0078d4");
    QString btnBg = isDark ? QStringLiteral("#0078d4") : QStringLiteral("#0078d4");
    QString btnHover = isDark ? QStringLiteral("#1f8cd6") : QStringLiteral("#106ebe");
    QString btnPressed = isDark ? QStringLiteral("#005a9e") : QStringLiteral("#005a9e");
    dlg->setStyleSheet(
        QString("QDialog { background: %1; border-radius: 8px; }"
                "QLabel#helpTitle { font-size: 16px; font-weight: 600; color: %2; }"
                "QLabel#helpKey { font-family: 'Consolas','Cascadia Mono','Courier New',monospace;"
                "  font-size: 12px; color: %4; }"
                "QLabel#helpDesc { font-size: 12px; color: %2; }"
                "QLabel#helpTip { color: %3; font-size: 11px; }"
                "QPushButton#helpOkBtn { background: %5; color: #ffffff; border: none;"
                "  border-radius: 5px; padding: 7px 28px; min-width: 80px; font-size: 13px; }"
                "QPushButton#helpOkBtn:hover { background: %6; }"
                "QPushButton#helpOkBtn:pressed { background: %7; }")
            .arg(bg, text, muted, keyColor, btnBg, btnHover, btnPressed));

    // 柔和阴影
    auto* shadow = new QGraphicsDropShadowEffect(dlg);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 50));
    dlg->setGraphicsEffect(shadow);

    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(24, 20, 24, 16);
    layout->setSpacing(12);

    auto* title = new QLabel(QString::fromUtf8("MiniLang IDE 快捷键"), dlg);
    title->setObjectName("helpTitle");
    layout->addWidget(title);

    // 第十一轮：两列网格布局（左列快捷键、右列描述）
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(6);
    grid->setColumnMinimumWidth(0, 120);
    grid->setColumnStretch(1, 1);

    struct Shortcut { const char* key; const char* desc; };
    const Shortcut rows[] = {
        {"F5",                "运行程序"},
        {"F6",                "调试程序"},
        {"Ctrl+F",            "查找"},
        {"Ctrl+H",            "替换"},
        {"F10",               "单步跳过"},
        {"F11",               "单步进入"},
        {"Shift+F11",         "单步跳出"},
        {"Shift+F5",          "停止运行"},
        {"Ctrl+Shift+F",      "格式化代码"},
        {"Esc",               "关闭查找面板"},
        {"Ctrl+Shift+V",      "编译分析面板"},
        {"Ctrl+Shift+A",      "AST 树形图"},
    };
    int row = 0;
    int leftCol = 0, rightCol = 2;
    int leftRow = 0, rightRow = 0;
    for (int i = 0; i < 12; ++i) {
        int col = (i < 6) ? leftCol : rightCol;
        int r = (i < 6) ? leftRow++ : rightRow++;

        auto* k = new QLabel(QString::fromUtf8(rows[i].key), dlg);
        k->setObjectName("helpKey");
        k->setFixedWidth(110);
        auto* d = new QLabel(QString::fromUtf8(rows[i].desc), dlg);
        d->setObjectName("helpDesc");
        grid->addWidget(k, r * 2, col);
        grid->addWidget(d, r * 2, col + 1);
    }
    // 设置行间距（每行之间稍大间距）
    grid->setVerticalSpacing(4);
    grid->setRowMinimumHeight(0, 28);
    layout->addLayout(grid);

    layout->addSpacing(4);
    auto* tipLabel = new QLabel(
        QString::fromUtf8("在代码行号左侧点击可设置/取消断点，右键点击断点可设置条件。"), dlg);
    tipLabel->setObjectName("helpTip");
    tipLabel->setWordWrap(true);
    layout->addWidget(tipLabel);

    layout->addStretch(1);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* okBtn = new QPushButton(QString::fromUtf8("确定"), dlg);
    okBtn->setObjectName("helpOkBtn");
    okBtn->setCursor(Qt::PointingHandCursor);
    connect(okBtn, &QPushButton::clicked, dlg, &QDialog::accept);
    btnRow->addWidget(okBtn);
    layout->addLayout(btnRow);

    // 居中显示在父窗口
    if (parentWidget()) {
        dlg->move(parentWidget()->geometry().center() - QPoint(dlg->width() / 2, dlg->height() / 2));
    }

    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

// ============================================================
// Completion + Spell check candidates
// ============================================================

void Ide::setupCompletion() {
    staticCompletionWords_.clear();
    spellCandidates_.clear();

    const auto& keywords = controller_->getKeywords();
    for (const auto& kv : keywords) {
        staticCompletionWords_ << QString::fromStdString(kv.first);
        spellCandidates_.push_back(kv.first);
    }

    // Built-in functions and methods
    std::vector<std::string> builtins = {
        "print", "input", "len", "type", "str", "int", "abs",
        "min", "max", "range", "sum", "push", "pop", "split", "join",
        "indexOf", "startsWith", "endsWith", "substr", "keys", "values",
        "contains", "true", "false", "null"
    };
    for (const auto& b : builtins) {
        staticCompletionWords_ << QString::fromStdString(b);
        spellCandidates_.push_back(b);
    }

    staticCompletionWords_.removeDuplicates();
    staticCompletionWords_.sort(Qt::CaseInsensitive);
    if (codeEditor_) codeEditor_->setCompletionWords(staticCompletionWords_);

    // 500ms completion word refresh timer
    completionTimer_ = new QTimer(this);
    completionTimer_->setSingleShot(true);
    completionTimer_->setInterval(500);
    connect(completionTimer_, &QTimer::timeout, this, &Ide::updateCompletionWords);

    // 300ms syntax check debounce timer
    syntaxCheckTimer_ = new QTimer(this);
    syntaxCheckTimer_->setSingleShot(true);
    syntaxCheckTimer_->setInterval(300);
    connect(syntaxCheckTimer_, &QTimer::timeout, this, [this]() {
        runRealTimeSyntaxCheck();
    });

    // Wire textChanged for existing tabs
    for (auto& tab : editorTabs_) {
        if (!tab.editor) continue;
        connect(tab.editor, &QPlainTextEdit::textChanged, this, [this]() {
            if (completionTimer_) completionTimer_->start();
            if (syntaxCheckTimer_) syntaxCheckTimer_->start();
        });
    }
    updateCompletionWords();
}

void Ide::updateCompletionWords() {
    if (!codeEditor_) return;
    QString text = codeEditor_->toPlainText();
    QStringList words = staticCompletionWords_;

    text.remove(QRegularExpression("\"(?:\\\\.|[^\"\\\\\\n])*\""));
    text.remove(QRegularExpression("/\\*.*?\\*/", QRegularExpression::DotMatchesEverythingOption));

    static const QRegularExpression pattern(
        "\\b(?:var|fun|class)\\s+([A-Za-z_][A-Za-z0-9_]*)");
    auto matchIt = pattern.globalMatch(text);
    QSet<QString> userSymbols;
    while (matchIt.hasNext()) {
        QRegularExpressionMatch match = matchIt.next();
        QString name = match.captured(1);
        if (!name.isEmpty()) userSymbols.insert(name);
    }

    QSet<QString> existingWords;
    for (const QString& w : words) existingWords.insert(w.toLower());
    for (const QString& sym : userSymbols) {
        if (!existingWords.contains(sym.toLower())) words << sym;
    }

    words.sort(Qt::CaseInsensitive);
    for (auto& tab : editorTabs_) {
        if (tab.editor) tab.editor->setCompletionWords(words);
    }
}

// ============================================================
// Diagnostics display (with smart spell correction)
// ============================================================

void Ide::displayDiagnostics(const DiagnosticBag& bag) {
    const auto& allDiags = bag.all();
    for (const auto& diag : allDiags) {
        std::string msg = diag.message;

        // Smart spell correction: for error diagnostics, try to find
        // a close match among keywords/builtins
        if (diag.isError() && !spellCandidates_.empty()) {
            std::string ident = extractQuotedIdentifier(msg);
            if (!ident.empty() && ident.size() > 1) {
                auto suggestion = SpellChecker::suggestSuffix(
                    ident, spellCandidates_, 2);
                if (!suggestion.empty()) {
                    msg += suggestion;
                }
            }
        }

        // Build display text
        QString text = QString::fromStdString(
            "[" + diag.sourceString() + "] " + diag.levelString());
        if (diag.line > 0) {
            text += QString(" (行 %1").arg(diag.line);
            if (diag.column > 0) text += QString(", 列 %1").arg(diag.column);
            text += ")";
        }
        text += ": " + QString::fromStdString(msg);

        if (diag.isError()) {
            appendError(text, diag.line, diag.column);
        } else if (diag.isWarning()) {
            appendOutput(QString("[警告] ") + text);
        } else {
            appendOutput(text);
        }
    }

    if (bag.hasErrors()) {
        showBottomPanel(1);
    }

    // Mark error ranges in editor with wavy underlines
    if (!bag.empty()) {
        std::vector<CodeEditor::ErrorRange> ranges;
        for (const auto& diag : allDiags) {
            if (diag.isError() && diag.line > 0) {
                ranges.push_back({diag.line, diag.column, 0});
            }
        }
        if (!ranges.empty() && codeEditor_) codeEditor_->setErrorRanges(ranges);
    }

    if (bag.size() > 1) {
        appendOutput(QString::fromStdString("--- " + bag.summary() + " ---"));
    }
}

// ============================================================
// Bytecode / IR / Token / AST helpers
// ============================================================

void Ide::highlightBytecodeLine(const std::string& chunkName, size_t ip) {
    const CompileResult& compileResult = controller_->lastCompileResult();
    const BytecodeChunk* targetChunk = nullptr;

    if (chunkName == "main" || chunkName.empty()) {
        targetChunk = &compileResult.mainChunk;
    } else {
        auto it = compileResult.functionChunks.find(chunkName);
        if (it != compileResult.functionChunks.end()) targetChunk = &it->second;
    }
    if (!targetChunk || targetChunk->code.empty()) return;

    int instrIndex = 0;
    if (ip < targetChunk->ipToInstrIndex.size() && targetChunk->ipToInstrIndex[ip] >= 0) {
        instrIndex = targetChunk->ipToInstrIndex[ip];
    } else {
        size_t offset = 0;
        while (offset < targetChunk->code.size()) {
            if (offset == ip) break;
            offset += targetChunk->instructionSizeAt(offset);
            instrIndex++;
        }
    }

    int startRow = 0;
    for (const auto& info : chunkRowMap_) {
        if (info.name == chunkName) { startRow = info.startRow; break; }
    }

    int targetRow = startRow + instrIndex;
    if (targetRow >= 0 && targetRow < bytecodeList_->count()) {
        bytecodeList_->setCurrentRow(targetRow);
        bytecodeList_->scrollToItem(bytecodeList_->item(targetRow));
    }
}

void Ide::populateBytecodeList() {
    const CompileResult& compileResult = controller_->lastCompileResult();
    QString currentSource = codeEditor_ ? codeEditor_->toPlainText() : QString();
    size_t currentHash = qHash(currentSource);
    if (currentHash == lastBytecodeSourceHash_ && bytecodeList_->count() > 0) return;
    lastBytecodeSourceHash_ = currentHash;

    bytecodeList_->clear();
    chunkRowMap_.clear();

    if (compileResult.mainChunk.code.empty()) {
        bytecodeList_->addItem("(无字节码)");
        return;
    }

    // Wrap setUpdatesEnabled in try/catch (project constraint)
    bytecodeList_->setUpdatesEnabled(false);
    try {
        int currentRow = 0;
        const QFont& bytecodeFont = GuiTextUtils::monospaceFont(10);

        {
            int startRow = currentRow;
            size_t offset = 0;
            while (offset < compileResult.mainChunk.code.size()) {
                std::string instr = compileResult.mainChunk.disassembleInstruction(offset);
                auto* item = new QListWidgetItem;
                item->setText(QString::fromStdString(instr));
                item->setFont(bytecodeFont);
                // 第八轮：设置 HTML 数据供 RichTextItemDelegate 渲染
                item->setData(RichTextItemDelegate::kHtmlRole,
                              formatBytecodeHtml(instr));
                bytecodeList_->addItem(item);
                currentRow++;
                offset += compileResult.mainChunk.instructionSizeAt(offset);
            }
            chunkRowMap_.push_back({"main", startRow, currentRow - startRow});
        }

        for (const auto& kv : compileResult.functionChunks) {
            std::string headerText = "---- " + kv.first + " (arity=" +
                                      std::to_string(kv.second.arity) + ") ----";
            auto* header = new QListWidgetItem;
            header->setText(QString::fromStdString(headerText));
            header->setFont(bytecodeFont);
            header->setData(RichTextItemDelegate::kHtmlRole,
                            formatBytecodeHtml(headerText));
            bytecodeList_->addItem(header);
            currentRow++;

            int startRow = currentRow;
            size_t funcOffset = 0;
            while (funcOffset < kv.second.code.size()) {
                std::string instr = kv.second.disassembleInstruction(funcOffset);
                auto* item = new QListWidgetItem;
                item->setText(QString::fromStdString(instr));
                item->setFont(bytecodeFont);
                item->setData(RichTextItemDelegate::kHtmlRole,
                              formatBytecodeHtml(instr));
                bytecodeList_->addItem(item);
                currentRow++;
                funcOffset += kv.second.instructionSizeAt(funcOffset);
            }
            chunkRowMap_.push_back({kv.first, startRow, currentRow - startRow});
        }
    } catch (...) {
        // Ensure updates are re-enabled even on exception
    }
    bytecodeList_->setUpdatesEnabled(true);
}

void Ide::populateIRViewer() {
    const IRFunction* ir = controller_->lastIR();
    irViewer_->setIR(ir);
    irToBytecodeOffset_ = controller_->lastIRToBytecodeOffset();
}

void Ide::highlightIRLine(size_t bytecodeOffset) {
    if (irToBytecodeOffset_.empty()) return;
    irViewer_->highlightByBytecodeOffset(irToBytecodeOffset_, bytecodeOffset);
}

void Ide::updateTokenTable() {
    const std::vector<Token>& tokens = controller_->lastTokens();
    static constexpr int MAX_DISPLAY = 10000;
    int displayCount = static_cast<int>(std::min(tokens.size(), static_cast<size_t>(MAX_DISPLAY)));
    tokenTable_->setRowCount(displayCount);
    tokenTable_->setUpdatesEnabled(false);
    try {
        for (int i = 0; i < displayCount; ++i) {
            const Token& tok = tokens[i];
            // 第八轮：列顺序 行号、列号、类型、词素、字面量
            tokenTable_->setItem(i, 0, new QTableWidgetItem(QString::number(tok.line)));
            tokenTable_->setItem(i, 1, new QTableWidgetItem(QString::number(tok.column)));
            tokenTable_->setItem(i, 2, new QTableWidgetItem(
                QString::fromStdString(Token::typeToString(tok.type))));
            tokenTable_->setItem(i, 3, new QTableWidgetItem(
                QString::fromStdString(tok.lexeme)));
            tokenTable_->setItem(i, 4, new QTableWidgetItem(
                QString::fromStdString(tok.literalToString())));
            // 错误 token 红色高亮；行号/列号列使用浅灰
            for (int col = 0; col < 5; ++col) {
                tokenTable_->item(i, col)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                if (tok.type == TokenType::TK_ERROR) {
                    tokenTable_->item(i, col)->setForeground(QColor("#d83b01"));
                } else if (col < 2) {
                    tokenTable_->item(i, col)->setForeground(QColor("#6e6e6e"));
                }
            }
        }
    } catch (...) {
        // Ensure updates are re-enabled even on exception
    }
    tokenTable_->setUpdatesEnabled(true);
    tokenTable_->resizeColumnsToContents();
    if (tokens.size() > static_cast<size_t>(MAX_DISPLAY)) {
        appendOutput(QString("[提示] Token 数量 %1 超过显示上限 %2，仅显示前 %2 条")
                       .arg(tokens.size()).arg(MAX_DISPLAY));
    }
}

void Ide::updateAstViewer() {
    if (controller_->astRoot()) {
        astViewer_->setAst(controller_->astRoot());
    } else {
        astViewer_->clearAst();
    }
}

void Ide::updateDebugInfo() {
    auto vars = controller_->getDebugVariableSnapshot();
    debugPanel_->updateVariables(vars);
    auto stack = controller_->getDebugCallStack();
    debugPanel_->updateCallStack(stack);
}

void Ide::setRunningState(bool running) {
    bool isDebug = controller_->isDebugRun() && running;
    runAction_->setEnabled(!running);
    debugAction_->setEnabled(!running);
    stepInAction_->setEnabled(isDebug);
    stepOverAction_->setEnabled(isDebug);
    stepOutAction_->setEnabled(isDebug);
    resumeAction_->setEnabled(isDebug);
    stopAction_->setEnabled(running);
    formatAction_->setEnabled(!running);
    compileAnalysisAction_->setEnabled(!running);
    if (codeEditor_) codeEditor_->setReadOnly(running);
    if (isDebug) {
        showDebugButtons(true);
    } else if (!running) {
        showDebugButtons(false);
    }
    if (running) {
        setVmStepActionsEnabled(false, false);
    } else if (!controller_->isVmRunning()) {
        bool vmInit = controller_->isVmInitialized();
        setVmStepActionsEnabled(vmInit, vmInit);
        showVmButtons(vmInit);
    }
    // 第八轮：状态栏运行状态提示
    if (statusRunLabel_) {
        if (running) {
            statusRunLabel_->setText(isDebug ? QString::fromUtf8("调试中")
                                             : QString::fromUtf8("运行中"));
        } else {
            statusRunLabel_->setText(QString());
        }
    }
}

// ============================================================
// File operations
// ============================================================

void Ide::onNew() {
    if (controller_->isRunning() || controller_->isVmRunning()) return;
    int idx = createNewEditorTab();
    switchToTab(idx);
    ensureEditorVisible();
    if (codeEditor_) codeEditor_->setFocus();
}

void Ide::onOpen() {
    if (controller_->isRunning() || controller_->isVmRunning()) return;
    QString startDir = workspaceDir_.isEmpty() ? QDir::homePath() : workspaceDir_;
    QString path = QFileDialog::getOpenFileName(this,
        QString::fromUtf8("打开文件"), startDir,
        "MiniLang (*.mini *.ml);;All Files (*)");
    if (path.isEmpty()) return;

    int existingIdx = findTabForFile(path);
    if (existingIdx >= 0) {
        ensureEditorVisible();
        switchToTab(existingIdx);
        return;
    }

    ensureEditorVisible();
    int idx = createNewEditorTab();
    switchToTab(idx);
    loadFileIntoTab(idx, path);
    if (codeEditor_) codeEditor_->setFocus();
}

void Ide::onOpenFolder() {
    QString startDir = workspaceDir_.isEmpty() ? QDir::homePath() : workspaceDir_;
    QString dir = QFileDialog::getExistingDirectory(this,
        QString::fromUtf8("打开文件夹"), startDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) return;
    openWorkspace(dir);
}

void Ide::onSave() {
    if (!codeEditor_) return;
    if (currentFilePath_.isEmpty()) { onSaveAs(); return; }
    QFile file(currentFilePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        InfoBar::warning(QString::fromUtf8("错误"),
            QString::fromUtf8("无法保存文件: ") + file.errorString(),
            Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << codeEditor_->toPlainText();
    file.close();
    isDirty_ = false;
    codeEditor_->document()->setModified(false);
    int currIdx = editorTabWidget_->currentIndex();
    if (currIdx >= 0 && currIdx < static_cast<int>(editorTabs_.size())) {
        editorTabs_[currIdx].filePath = currentFilePath_;
        editorTabs_[currIdx].isUntitled = false;
    }
    updateWindowTitle();
    populateFileTree();
}

void Ide::onSaveAs() {
    QString startDir = workspaceDir_.isEmpty() ? QDir::homePath() : workspaceDir_;
    QString path = QFileDialog::getSaveFileName(this,
        QString::fromUtf8("保存文件"), startDir,
        "MiniLang (*.mini *.ml);;All Files (*)");
    if (path.isEmpty()) return;
    currentFilePath_ = path;
    int currIdx = editorTabWidget_->currentIndex();
    if (currIdx >= 0 && currIdx < static_cast<int>(editorTabs_.size())) {
        editorTabs_[currIdx].filePath = path;
        editorTabs_[currIdx].isUntitled = false;
        QFileInfo fi(path);
        editorTabWidget_->setTabText(currIdx, fi.fileName());
    }
    onSave();
    populateFileTree();
}

bool Ide::maybeSave() {
    if (!codeEditor_) return true;
    if (!codeEditor_->document()->isModified()) return true;
    auto ret = QMessageBox::question(this,
        QString::fromUtf8("MiniLang IDE"),
        QString::fromUtf8("文件已修改，是否保存？"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (ret == QMessageBox::Save) { onSave(); return !codeEditor_->document()->isModified(); }
    if (ret == QMessageBox::Cancel) return false;
    return true;
}

void Ide::updateWindowTitle() {
    QString title = "MiniLang IDE";
    if (!workspaceDir_.isEmpty()) {
        QFileInfo fi(workspaceDir_);
        title += " - " + fi.fileName();
    }
    if (!currentFilePath_.isEmpty()) {
        QFileInfo fi(currentFilePath_);
        title += " / " + fi.fileName();
    }
    if (isDirty_) title += " *";
    setWindowTitle(title);

    // 第九轮：更新自定义标题栏路径标签（灰色小字号）
    if (titlePathLabel_) {
        QString path;
        if (!currentFilePath_.isEmpty()) {
            path = currentFilePath_;
            if (isDirty_) path += " *";
        } else if (!workspaceDir_.isEmpty()) {
            path = workspaceDir_;
        }
        titlePathLabel_->setText(path);
        titlePathLabel_->setToolTip(path);
    }
}

void Ide::loadFile(const QString& path) {
    int existingIdx = findTabForFile(path);
    if (existingIdx >= 0) {
        switchToTab(existingIdx);
        return;
    }
    int idx = createNewEditorTab();
    switchToTab(idx);
    loadFileIntoTab(idx, path);
}
