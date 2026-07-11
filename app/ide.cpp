#include "ide.h"
#include "Logger.h"
#include "common/RuntimeLimits.h"
#include "common/SpellChecker.h"
#include "gui/ErrorHintEngine.h" // P0-3 fix (F10): 主编译/运行路径接入错误增强
#include "gui/GuiTextUtils.h"
#include "gui/I18n.h" // D2: i18n 翻译宏 mlTr
#include "gui/PanelAnimator.h"
#include "gui/TeachingTheme.h" // P2 视觉一致性：info/success/warning/error/hint 语义色集中管理

#include <QAbstractAnimation>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <algorithm>
#include <functional>
#include <sstream>

#ifdef Q_OS_WIN
// WIN32_LEAN_AND_MEAN + NOMINMAX 减少 windows.h 宏污染
// （避免 ERROR/WARNING/min/max 宏与 QFluentKit 枚举及 std::min/max 冲突）
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
// windows.h 仍可能定义 ERROR/WARNING，与 InfoBar::Type 枚举冲突，必须取消
#ifdef ERROR
#undef ERROR
#endif
#ifdef WARNING
#undef WARNING
#endif
#endif

// ADS headers
#include "DockAreaWidget.h"
#include "DockManager.h"
#include "DockSplitter.h"
#include "DockWidget.h"
#include "DockWidgetTab.h"

// QFluentKit Theme
#include "Theme.h"

// QFluentKit components (sixth-round UI refactor → twelfth-round unified title bar)
#include "FluentIcon.h"
#include "QFluent/ComboBox.h"
#include "QFluent/Flyout.h"
#include "QFluent/InfoBar.h"
#include "QFluent/Label.h" // P2 视觉一致性：TitleLabel / CaptionLabel（Welcome 欢迎页）
#include "QFluent/Menu/RoundMenu.h"
#include "QFluent/Navigation/Pivot.h"
#include "QFluent/Progress/IndeterminateProgressBar.h"
#include "QFluent/PushButton.h"
#include "QFluent/ScrollBar.h"
#include "QFluent/TabBar.h"
#include "QFluent/TableView.h"
#include "QFluent/ToolButton.h"
#include "StyleSheet.h"

// GUI: ActivityBar (sixth-round)
#include "gui/ActivityBar.h"
// 功能 1：首次启动欢迎向导
#include "gui/WelcomeWizard.h"
// 功能 1b：3 分钟 Hello World 引导（GuidedTour）
#include "gui/GuidedTour.h"

// ============================================================
// ide.cpp — MiniLang IDE 主窗口实现
// ------------------------------------------------------------
// 实现 Ide 类（QMainWindow 子类），是整个 IDE 的 GUI 视图层：
//   - 菜单 / 工具栏 / 统一标题栏 / 状态栏的构建与主题应用
//   - 编辑器标签页、文件树、欢迎页的创建、切换与持久化
//   - 运行/调试/VM 单步等按钮槽：转发到 IdeController（Facade）协调
//     PipelineRunner / WorkerManager / DebugCoordinator / VmStepper 完成
//     编译-运行-调试流水线
//   - 输出面板、错误列表、字节码/IR/Token 可视化的刷新
//   - 19 个教学增强面板的懒加载与 centerStack_ 路由
// 本文件只负责界面与交互编排，所有业务逻辑委托给 IdeController 及其协作类，
// 不持有解释器/编译器核心状态。
// ============================================================

// ============================================================
// Static helpers
// ============================================================

/// 递归填充文件树子节点：仅列出 .mini/.ml 文件与子目录，设置 Fluent 图标与完整路径 tooltip。
static void populateDirChildren(QTreeWidget* tree, QTreeWidgetItem* parentItem, const QString& dirPath, int depth) {
    if (depth > 8)
        return;
    QDir dir(dirPath);
    if (!dir.exists())
        return;

    QStringList filters;
    filters << "*.mini" << "*.ml";
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        auto* item = new QTreeWidgetItem(parentItem);
        item->setText(0, fi.fileName());
        // 第九轮：文件名显示不全时悬浮显示完整路径 tooltip
        item->setToolTip(0, fi.absoluteFilePath());
        // P2 视觉一致性：文件树图标走 Fluent 图标（FOLDER / DOCUMENT / CODE）
        // .ml 文件用 CODE 图标以区分 MiniLang 源码文件，其余用 DOCUMENT
        Fluent::IconType iconType = (fi.suffix() == "ml") ? Fluent::IconType::CODE : Fluent::IconType::DOCUMENT;
        item->setIcon(0, Fluent::icon(iconType));
        item->setData(0, Qt::UserRole, fi.absoluteFilePath());
        item->setData(0, Qt::UserRole + 1, false);
    }

    QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fi : dirs) {
        auto* item = new QTreeWidgetItem(parentItem);
        item->setText(0, fi.fileName());
        item->setToolTip(0, fi.absoluteFilePath());
        item->setIcon(0, Fluent::icon(Fluent::IconType::FOLDER));
        item->setData(0, Qt::UserRole, fi.absoluteFilePath());
        item->setData(0, Qt::UserRole + 1, true);
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
}

/// 解析文件树右键菜单的目标目录：目录自身取其路径，文件则取其父目录路径。
static QString resolveTreeContextMenuTargetDir(QTreeWidget* tree, QTreeWidgetItem* item) {
    if (!item)
        return QString();
    bool isDir = item->data(0, Qt::UserRole + 1).toBool();
    if (isDir)
        return item->data(0, Qt::UserRole).toString();
    QTreeWidgetItem* parent = item->parent();
    if (parent)
        return parent->data(0, Qt::UserRole).toString();
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
    explicit CleanToolButton(Fluent::IconType type, QWidget* parent = nullptr) : TransparentToolButton(type, parent) {
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
        if (!isEnabled())
            p.setOpacity(0.4);
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

    explicit RichTextItemDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();

        // 背景：选中 / 交替行
        // R74: 使用 TeachingTheme 的中性色板，不再硬编码。
        // 选中态浅蓝、交替行浅灰、常态纯白，跨机器渲染一致。
        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, TeachingTheme::ideSelectedBg());
        } else if (option.features & QStyleOptionViewItem::Alternate) {
            painter->fillRect(option.rect, TeachingTheme::ideBgPanel());
        } else {
            painter->fillRect(option.rect, TeachingTheme::ideBgMain());
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

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QString html = index.data(kHtmlRole).toString();
        if (html.isEmpty()) {
            return QStyledItemDelegate::sizeHint(option, index);
        }
        QTextDocument doc;
        doc.setDefaultFont(option.font);
        doc.setDocumentMargin(2);
        doc.setHtml(html);
        doc.setTextWidth(option.rect.width() > 0 ? option.rect.width() : 400);
        return QSize(static_cast<int>(doc.idealWidth()) + 12, static_cast<int>(doc.size().height()));
    }
};

/// 将字节码指令文本转为带语法高亮的 HTML
/// opcode（OP_*）蓝色，常量/字符串/数字橙色，注释灰色，函数头紫色
static QString formatBytecodeHtml(const std::string& text) {
    QString qtext = QString::fromStdString(text);
    // 函数头分隔线 ---- xxx ---- → 紫色加粗
    if (qtext.startsWith("----") && qtext.endsWith("----")) {
        return QString("<span style='color:#6C71C4;font-weight:bold;'>%1</span>").arg(qtext.toHtmlEscaped());
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
        if (!first)
            html += "&nbsp;";
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
            html += "<span style='color:#268BD2;font-weight:bold;'>" + esc + "</span>";
        }
        // 数字常量
        else if (!core.isEmpty() && core[0].isDigit()) {
            html += "<span style='color:#CB4B16;'>" + esc + "</span>";
        }
        // 字符串字面量
        else if (core.startsWith('"') && core.endsWith('"')) {
            html += "<span style='color:#d83b01;'>" + esc + "</span>";
        }
        // 标签 Lxx / BBxx → 绿色
        else if ((core.startsWith('L') || core.startsWith('B')) && core.size() > 1 && core.mid(1).toInt() > 0) {
            html += "<span style='color:#859900;'>" + esc + "</span>";
        }
        // = / -> / | 等符号
        else if (core == "=" || core == "->" || core == "|" || core == "&" || core == ":") {
            html += "<span style='color:#8C8C8C;'>" + esc + "</span>";
        }
        // 标识符默认色
        else {
            html += "<span style='color:#1E1E1E;'>" + esc + "</span>";
        }
        if (!suffix.isEmpty()) {
            html += "<span style='color:#6e6e6e;'>" + suffix.toHtmlEscaped() + "</span>";
        }
    }

    if (!comment.isEmpty()) {
        if (!html.isEmpty())
            html += "&nbsp;";
        html += "<span style='color:#6e6e6e;font-style:italic;'>" + comment.toHtmlEscaped() + "</span>";
    }

    return html;
}

// ============================================================
// Constructor / Destructor
// ============================================================

/// PERF: 延迟合并样式应用请求
/// 启动期间多个路径（Theme::setThemeMode、dockWidgetAdded、showEvent、restoreLayout）
/// 都会请求应用样式，直接调用会导致 applyFluentStyle 被执行 5-6 次。
/// 通过 0ms 定时器防抖，同一事件循环内的多次请求合并为一次实际执行。
void Ide::scheduleApplyStyle() {
    if (closing_ || !applyStyleTimer_)
        return;
    if (!applyStyleTimer_->isActive()) {
        applyStyleTimer_->start();
    }
}

/// 构造函数：构建标题栏/主界面/信号连接/文件树/状态栏，初始化主题、拖放与文件监视，
/// 首次启动弹出欢迎向导；最后恢复布局并刷新标题与状态栏。
Ide::Ide(QWidget* parent) : QMainWindow(parent) {

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

    // ---- 主题系统：仅使用亮色主题（深色主题已移除） ----
    // 强制 LIGHT 模式，不再读取 QSettings 持久化（删除深色主题后无切换需求）。
    // onThemeModeChanged 回调保留用于响应 ThemeColor（主题色）变更，但不再切换亮/暗。
    // PERF: 使用 scheduleApplyStyle 合并多个样式请求，避免启动时重复调用
    Theme::onThemeModeChanged(this, [this](Fluent::ThemeMode) {
        scheduleApplyStyle(); // 主题色变更时重算 QSS（合并防抖）
    });
    // setThemeMode 触发 onThemeModeChanged → scheduleApplyStyle（通过0ms定时器延迟执行）
    Theme::setThemeMode(Fluent::ThemeMode::LIGHT);
    applyTeachingFontSize(); // 首次应用教学面板字号（codeFontSize_ + 2）

    setupCompletion();

    // 文件拖放支持：主窗口接受从资源管理器拖入的 .mini/.ml 文件
    setAcceptDrops(true);
    // 文件外部修改监听：QFileSystemWatcher 监视当前打开的文件
    fileWatcher_ = new QFileSystemWatcher(this);
    connect(fileWatcher_, &QFileSystemWatcher::fileChanged, this, &Ide::onFileChangedExternally);

    // Layout save timer (debounced)
    splitterSaveTimer_ = new QTimer(this);
    splitterSaveTimer_->setSingleShot(true);
    splitterSaveTimer_->setInterval(500);
    connect(splitterSaveTimer_, &QTimer::timeout, this, &Ide::saveLayout);

    // PERF: 样式应用防抖定时器 - 合并多次 applyFluentStyle 请求为一次
    applyStyleTimer_ = new QTimer(this);
    applyStyleTimer_->setSingleShot(true);
    applyStyleTimer_->setInterval(0);
    connect(applyStyleTimer_, &QTimer::timeout, this, [this]() { applyFluentStyle(); });

    // Restore AST window geometry (independent top-level window)
    restoreAstWindowGeometry();

    // Startup: show welcome page, hide non-essential panels
    centerStack_->setCurrentWidget(welcomePage_);

    restoreLayout();
    // applyFluentStyle 样式化初始 UI（首次显示前）。
    // showEvent 中 restoreState 后会再次调用 applyFluentStyle，确保 restoreState
    // 创建的新 dock 容器也被样式化。
    applyFluentStyle();
    updateWindowTitle();
    updateStatusBar();

    // 功能 1：首次启动检测 —— 若未完成欢迎向导则模态弹出。
    // 在所有 UI 初始化完成后显示，避免 parent 关系问题。
    // 用 QSettings 持久化 welcome_completed 标记，完成/跳过后不再显示。
    QSettings welcomeSettings;
    if (!welcomeSettings.value(kWelcomeCompletedKey, false).toBool()) {
        auto* wizard = new WelcomeWizard(this);
        // Step4「开始学习 ✓」按钮 emit learningPathRequested() → 打开学习路径地图面板。
        // 与「再次显示欢迎向导」(ide.cpp onReshowWelcome) 和 onActivityRequested("welcome")
        // 两处 WelcomeWizard 创建点保持信号连接一致。
        // 历史：第五十七轮 P3 修复误删此连接（同时删除 exec() 后的无条件调用），
        // 导致首次启动点 Step4 按钮无响应——用户感知「学习中心面板打不开」。
        // 本次恢复连接：点 Step4 才打开面板，点跳过/Esc 则停留在欢迎页（符合 P3 初衷）。
        connect(wizard, &WelcomeWizard::learningPathRequested, this,
                [this]() { showTeachingPanel(QStringLiteral("learning-path")); });
        wizard->exec();
        welcomeSettings.setValue(kWelcomeCompletedKey, true);
        wizard->deleteLater();
    }
}

/// 析构：保存当前窗口布局后释放资源（worker/解释器由协作类通过智能指针管理）。
Ide::~Ide() {
    // ROUND-60 fix: 安全网。若 closeEvent 被绕过（如 QCoreApplication::quit()），
    // 此处确保 controller_ → Ide 的信号连接在成员析构前被切断，避免析构链中
    // 残留信号访问已析构的 UI 成员。
    if (controller_)
        disconnect(controller_, nullptr, this, nullptr);
    // ROUND-67 P0/P1 fix: 停止所有活跃动画 + 清理 GuidedTour（安全网）。
    // QVariantAnimation 不受 removePostedEvents 控制，必须显式 stop()。
    if (splitterAnim_) {
        splitterAnim_->stop();
        splitterAnim_ = nullptr;
    }
    if (bottomPanelAnim_) {
        bottomPanelAnim_->stop();
        bottomPanelAnim_ = nullptr;
    }
    if (!activePanelTours_.isEmpty()) {
        for (GuidedTour* tour : activePanelTours_) {
            if (tour) {
                QCoreApplication::removePostedEvents(tour);
                delete tour;
            }
        }
        activePanelTours_.clear();
    }
    // ROUND-73 P0 fix: 停止所有子对象的 QTimer 和 QAbstractAnimation（安全网）。
    // 对齐 closeEvent 中的 stopChildAnimations，但用 findChildren(this) 递归覆盖全部子对象。
    // 关键认知：closeEvent 被绕过时（如 QCoreApplication::quit()、系统强制关闭），
    // 教学面板的 autoTimer（2s 间隔）和 QPropertyAnimation 仍然活跃。
    // ~QObject 删除子 QTimer 时 QTimer 析构会 stop，但析构链中若有事件派发
    // （如 ADS QSS 重算 → repaint），活跃的定时器会触发回调访问正在析构的成员 → UAF。
    // QVariantAnimation/QPropertyAnimation 由 QUnifiedTimer 驱动，不受 removePostedEvents 控制。
    {
        const auto timers = findChildren<QTimer*>();
        for (auto* t : timers) {
            if (t && t->isActive())
                t->stop();
        }
        const auto anims = findChildren<QAbstractAnimation*>();
        for (auto* a : anims) {
            if (a && a->state() == QAbstractAnimation::Running)
                a->stop();
        }
    }
    // ROUND-67 P2 fix: 清空 vmStateChangedListener（安全网）。
    if (controller_)
        controller_->clearVmStateChangedListeners();
    // ROUND-66 P0 fix: 清空所有待处理事件，防止子对象析构期间 Qt 派发残留的
    // QMetaCallEvent（queued slot lambda）。Qt6 disconnect 不移除已投递的
    // QMetaCallEvent，若析构链中任何子对象析构触发了事件派发（如 ADS 内部
    // QSS 重算触发 repaint → processEvents），残留 lambda 会访问已析构的
    // WorkerManager/VmStepper/面板 → UAF（读取访问权限冲突）。
    QCoreApplication::removePostedEvents(this);
    if (controller_)
        controller_->clearPendingEvents();
    saveLayout();
}

// ============================================================
// Close event
// ============================================================

/// 显示事件：首次显示时应用样式。
/// R65-3 fix: restoreState 已移到 restoreLayout 的 QTimer::singleShot(0) 中，
/// 在事件循环开始后执行（此时窗口已完全布局，几何尺寸有效）。
/// showEvent 中仅保留 applyFluentStyle 确保初始 dock 容器被样式化。
/// 注意：firstShow_ 不在此处重置，而是在 QTimer restoreState 完成后重置，
/// 以防止 restoreState 触发的 Resize 事件导致误保存（覆盖用户保存的布局）。
void Ide::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (firstShow_) {
        // PERF: 使用 scheduleApplyStyle 合并，避免与构造函数/restoreLayout 中的重复调用
        scheduleApplyStyle();
    }
}

/// 关闭事件：存在未保存修改时弹出保存确认，用户取消则忽略关闭。
void Ide::closeEvent(QCloseEvent* event) {
    // ISSUE-7 fix + ROUND-60: 关闭流程开始，置 closing_ 标志。后续所有信号处理回调
    // （handleVmStepResult/onWorkerFinished/onPausedAt/displayDiagnostics 以及
    // outputReady/runOk/stoppedByUser/runtimeError/genericError 五个 lambda）
    // 入口检查此标志直接 return，避免 processEvents 或析构期间残留的
    // QueuedConnection 信号访问已部分析构的成员导致 UAF（读取访问权限冲突）。
    closing_ = true;

    // ROUND-76 fix: 通知 ProfileDashboardPanel 正在关闭。
    // runProfile 中的 processEvents(ExcludeUserInputEvents) 不排除 QCloseEvent，
    // closeEvent 会在 runProfile 调用栈内同步执行。设置 closing_ 后，
    // runProfile 在每次 processEvents 返回后检查并立即退出，
    // 避免继续访问正在关闭的 widget（renderResults/update 等）。
    if (profileDashboardPanel_)
        profileDashboardPanel_->setClosing(true);

    // ROUND-66 fix: 立即停止所有内部定时器，避免 maybeSave 模态对话框的事件循环
    // 期间定时器触发（completionTimer_/syntaxCheckTimer_/fileTreeFilterTimer_ 的
    // 回调未受 closing_ 守卫保护，可能访问正在清理的状态）。splitterSaveTimer_ 也停止。
    if (splitterSaveTimer_)
        splitterSaveTimer_->stop();
    if (completionTimer_)
        completionTimer_->stop();
    if (syntaxCheckTimer_)
        syntaxCheckTimer_->stop();
    if (fileTreeFilterTimer_)
        fileTreeFilterTimer_->stop();

    // ROUND-67 P0/P1 fix: 立即停止所有活跃的 QVariantAnimation。
    // 关键认知：QVariantAnimation 由 QUnifiedTimer（全局动画定时器）驱动，
    // 不经过 Qt 对象事件队列，removePostedEvents 完全无法清理它！
    // 只有显式 stop() 才能停止。closeEvent 入口必须停止所有活跃动画，
    // 否则 maybeSave 模态对话框期间动画继续运行，valueChanged lambda 访问
    // 正在清理的 centerSplitter_/editorSplitter_ → UAF（读取访问权限冲突）。
    // 这是学习中心面板关闭崩溃的核心根因：animateCenterSplitter 在打开/切换
    // 教学面板时频繁触发（7 处调用点），动画持续约 300ms。
    if (splitterAnim_) {
        splitterAnim_->stop();
        splitterAnim_ = nullptr;
    }
    if (bottomPanelAnim_) {
        bottomPanelAnim_->stop();
        bottomPanelAnim_ = nullptr;
    }
    // ROUND-75 fix (根因 B1): 停止 slideInWidget 创建的 QPropertyAnimation。
    // 这些动画以各 panel widget 为 parent（非 Ide 成员变量），但 QPropertyAnimation
    // 是 QAbstractAnimation 子类，由 QUnifiedTimer 驱动，不经过 Qt 事件队列，
    // removePostedEvents 完全无法清理它。maybeSave 模态对话框期间动画继续运行，
    // valueChanged 访问正在 reparent/析构的 widget → UAF（读取访问权限冲突）。
    // slideInWidget 在 showTeachingPanel/showBottomPanel/showRightPanel/
    // onActivityChangedById 中频繁调用，是打开/切换面板时必然触发的动画。
    // 遍历 centerStack_ 各页 + 底部/右侧栈的子动画并 stop() + deleteLater()。
    const auto stopChildAnimations = [](QWidget* w) {
        if (!w)
            return;
        // 停止 QPropertyAnimation（QUnifiedTimer 驱动，removePostedEvents 无法清理）
        const auto anims = w->findChildren<QPropertyAnimation*>();
        for (auto* a : anims) {
            if (a->state() == QAbstractAnimation::Running) {
                a->stop();
                a->deleteLater();
            }
        }
        // ROUND-76 fix: 同时停止 QTimer（Qt 定时器事件驱动）。
        // ProfileDashboardPanel 的 statusAnimTimer_（400ms）就是 QTimer，
        // maybeSave 模态对话框期间其 timeout 信号会被派发，lambda 访问
        // 正在清理的 statusLabel_ → UAF。findChildren 递归覆盖所有子 QTimer。
        const auto timers = w->findChildren<QTimer*>();
        for (auto* t : timers) {
            if (t->isActive())
                t->stop();
        }
        // ROUND-73 P1 fix: 清除面板已排队的 QMetaCallEvent（QueuedConnection 槽调用）。
        // stop() 停止 QTimer 但不取消已投递的 timeout 事件和 queued slot 调用。
        // closeEvent 后续的 processEvents(L700) 会派发这些残留事件，访问正在清理的 UI → UAF。
        QCoreApplication::removePostedEvents(w);
    };
    stopChildAnimations(centerStack_);
    if (centerStack_) {
        for (int i = 0; i < centerStack_->count(); ++i)
            stopChildAnimations(qobject_cast<QWidget*>(centerStack_->widget(i)));
    }
    stopChildAnimations(bottomStack_);
    stopChildAnimations(rightStack_);
    // 侧栏 dock 内容（fileTreeDock_/debugPanelDock_/teachingTreeDock_ 的 widget）
    // 也会被 slideInWidget（onActivityChangedById）。findChildren 递归遍历，
    // 覆盖 dockManager_ 下所有 dock 的子动画。
    stopChildAnimations(dockManager_);
    // ROUND-75 fix (根因 B2): 停止 pendingHide 定时器，避免 maybeSave 模态对话框
    // 期间 timeout 触发访问正在清理的 centerStack_/editorTabWidget_。
    if (pendingHideCenterTimer_)
        pendingHideCenterTimer_->stop();
    if (pendingHideEditorTimer_)
        pendingHideEditorTimer_->stop();

    // ROUND-89 P0 fix: 捕获所有 Ide 直接子 QTimer/QPropertyAnimation。
    // stopChildAnimations 只扫描 centerStack_/bottomStack_/rightStack_/dockManager_
    // 的子对象，但 QTimer::singleShot(0, this, ...) 创建的临时 QTimer 是 Ide 的
    // 直接子对象（如 showTeachingPanel 的自动引导延迟启动、restoreLayout 的
    // restoreState 延迟、dock resize 延迟）。这些定时器注册在事件调度器中，
    // removePostedEvents(this) 无法取消。maybeSave/processEvents 的事件循环
    // 会派发它们，访问正在清理的成员 → UAF。此处对 this 递归 findChildren
    // 停止所有活跃定时器和动画，作为 closeEvent 入口的安全网。
    {
        const auto allTimers = findChildren<QTimer*>();
        for (auto* t : allTimers) {
            if (t && t->isActive())
                t->stop();
        }
        const auto allAnims = findChildren<QPropertyAnimation*>();
        for (auto* a : allAnims) {
            if (a && a->state() == QAbstractAnimation::Running) {
                a->stop();
                a->deleteLater();
            }
        }
    }

    // ROUND-67 P1 fix: 清理所有活跃的面板特定 GuidedTour。
    // 5 个教学面板首次访问自动触发 GuidedTour，tour 的 showStep 中排队
    // QTimer::singleShot(0, tour, ...) 持有裸 targetWidget 指针。
    // removePostedEvents(this) 只清空 Ide 队列，不清空 tour 队列。
    // maybeSave 模态对话框期间 tour 的 singleShot 被派发，访问可能已被
    // reparent/清理的 targetWidget → UAF。显式 disconnect + removePostedEvents(tour) + delete。
    // （hideOverlay 是 private，~GuidedTour 析构会调用它清理气泡）
    if (!activePanelTours_.isEmpty()) {
        for (GuidedTour* tour : activePanelTours_) {
            if (tour) {
                disconnect(tour, nullptr, this, nullptr);
                QCoreApplication::removePostedEvents(tour);
                delete tour;
            }
        }
        activePanelTours_.clear();
    }

    // ROUND-60 fix: 尽早切断 controller_ → Ide 的所有信号-槽连接。
    // 原实现将 disconnect 放在 closeEvent 末尾（maybeSave/processEvents 之后），
    // 但 maybeSave() 的模态对话框与 processEvents(521) 会派发主线程事件队列中
    // 挂起的 QueuedConnection 事件（outputReady 等），此时 5 个 lambda 无 closing_
    // 守卫会访问已部分析构的 UI 成员导致 UAF。尽早 disconnect + removePostedEvents
    // 双重保险：disconnect 阻止未来投递，removePostedEvents 清空已投递未派发的事件。
    disconnect(controller_, nullptr, this, nullptr);
    // ROUND-67 P2 fix: 清空所有 vmStateChangedListener 回调，防止 closeEvent 后续
    // 操作（vmStop/stopForClose 等）触发 notifyVmStateChanged 时，7 个教学面板的
    // onVmStateChanged 回调访问正在清理的 UI。
    if (controller_)
        controller_->clearVmStateChangedListeners();
    // ROUND-66 P0 fix: removePostedEvents 第二参数 0 表示 QEvent::None（几乎不存在
    // 的事件类型），不是"所有事件"。原代码注释声称"清空已投递未派发的事件"但实际
    // 只移除了 type==0 的事件，QMetaCallEvent（queued slot, type=43）和 QEvent::Timer
    // （type=1）均未被移除。改为无参版本（默认 -1 = 所有类型），真正清空 Ide 的
    // 待处理事件队列。同时通过 controller_->clearPendingEvents() 清空 IdeController
    // 及其 4 个协作成员（WorkerManager/VmStepper/DebugCoordinator/PipelineRunner）
    // 的事件队列，防止内部 lambda（如 cleanupWorker、notifyVmStateChanged）在
    // 析构链中被 dispatch 访问已析构成员 → UAF。
    QCoreApplication::removePostedEvents(this);
    if (controller_)
        controller_->clearPendingEvents();

    if (!maybeSave()) {
        closing_ = false;
        event->ignore();
        return;
    }

    if (replPanel_->isReplRunning()) {
        auto ret = QMessageBox::warning(
            this, mlTr("REPL 仍在执行"),
            mlTr("REPL 有异步任务正在执行。\n关闭窗口将发送中止请求并等待最多 5 秒。\n\n是否继续关闭？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    if (controller_->isRunning()) {
        if (!controller_->stopForClose(3000)) {
            if (codeEditor_ && codeEditor_->document()->isModified()) {
                auto ret = QMessageBox::warning(
                    this, mlTr("程序无响应，即将强制终止"),
                    mlTr(
                        "解释器线程未在 3 "
                        "秒内响应停止请求\n强制终止会跳过正常析构\uff0c未保存的代码将丢失\u3002\n\n是否现在保存\uff1f"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
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
            // IDE-CLOSE-03 fix: forceStop 是异常路径，不经过 onWorkerFinished，
            // 需手动 setRunningState(false) 清理 UI 按钮状态（运行/停止/调试）。
            setRunningState(false);
        }
    }
    if (controller_->isVmRunning()) {
        // BUG-IDE-10 fix: 直接调用 controller_->vmStop() 仅停止 VM，避免调用 onVmStop()
        // 触发 UI 更新（setVmStepActionsEnabled/codeEditor->setReadOnly 等），这些 UI 操作
        // 在 closeEvent 路径下既无必要也可能与正在进行的清理产生竞态。
        controller_->vmStop();
        // IDE-CLOSE-02 fix: QTimer::stop() 不取消已排队的 timeout 信号，
        // 调用 processEvents 清空挂起的 vmRunTimer_ 信号，避免 close 后仍触发 runBatch。
        // runBatch 入口有 isVmRunning_ 守卫（vmStop 后为 false），实际安全，
        // 但显式清空避免依赖守卫的脆弱模式。
        qApp->processEvents(QEventLoop::ExcludeUserInputEvents | QEventLoop::ExcludeSocketNotifiers);
    }
    // BUG-IDE-08 fix: 对话框承诺"发送中止请求并等待最多 5 秒"，但原实现仅调用
    // waitReplFuture() 等待 future 完成而未先发送中止请求，REPL 中的死循环会等到默认
    // 超时才结束（或永远不结束）。先 requestReplStop() 设置 interpreter 的 stop 标志，
    // 让 worker 在下次 checkBreak 时抛 DebugStopException 主动退出。
    if (replPanel_->isReplRunning()) {
        controller_->requestReplStop();
    }
    replPanel_->waitReplFuture();
    // Save AST independent window geometry before closing
    if (astWindow_ && !astWindow_->isHidden())
        saveAstWindowGeometry();
    // ISSUE-7 fix: 清理 GuidedTour。若引导气泡仍活跃，bubble_ 是 host_(Ide) 的子 widget，
    // 析构链会销毁它，但 showStep 中排队的 QTimer::singleShot(0, this, ...) 可能持有
    // GuidedTour 裸 this。先调用 hideOverlay 取消气泡并 deleteLater，再 delete guidedTour_
    // 确保所有延迟 lambda 的 this 在析构前失效（GuidedTour 析构会 hideOverlay）。
    if (guidedTour_) {
        delete guidedTour_;
        guidedTour_ = nullptr;
    }
    // ISSUE-7 fix: disconnect 已在 closeEvent 入口执行（ROUND-60 提前到 maybeSave 之前），
    // 此处保留 removePostedEvents 兜底，清空 stopForClose/processEvents 期间可能重新投递的事件。
    // ROUND-66 P0 fix: 原参数 0 只移除 QEvent::None，改为无参（-1=所有类型）。
    // 同时清空 controller_ 及其协作成员的队列。
    QCoreApplication::removePostedEvents(this);
    if (controller_)
        controller_->clearPendingEvents();
    saveLayout();
    event->accept();
}

// ============================================================
// Native event: frameless window edge resize (Windows WM_NCHITTEST)
// 第九轮：无边框窗口保留边缘拖拽调整大小能力
// ============================================================

/// Windows 原生事件：捕获 WM_NCHITTEST 实现无边框窗口的边缘拖拽与缩放。
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

            bool left = x >= winRect.left && x < winRect.left + borderWidth;
            bool right = x < winRect.right && x >= winRect.right - borderWidth;
            bool top = y >= winRect.top && y < winRect.top + borderWidth;
            bool bottom = y < winRect.bottom && y >= winRect.bottom - borderWidth;

            if (top && left) {
                *result = HTTOPLEFT;
                return true;
            }
            if (top && right) {
                *result = HTTOPRIGHT;
                return true;
            }
            if (bottom && left) {
                *result = HTBOTTOMLEFT;
                return true;
            }
            if (bottom && right) {
                *result = HTBOTTOMRIGHT;
                return true;
            }
            if (left) {
                *result = HTLEFT;
                return true;
            }
            if (right) {
                *result = HTRIGHT;
                return true;
            }
            if (top) {
                *result = HTTOP;
                return true;
            }
            if (bottom) {
                *result = HTBOTTOM;
                return true;
            }
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

/// 将编辑器中的断点集合同步到 VmStepper（VM 调试模式下使用）。
void Ide::syncVmBreakpoints() {
    if (!codeEditor_)
        return;
    QSet<int> bps = codeEditor_->getBreakpoints();
    QMap<int, std::string> conds;
    for (int line : bps) {
        std::string cond = codeEditor_->getBreakpointCondition(line);
        if (!cond.empty())
            conds[line] = cond;
    }
    controller_->setVmBreakpoints(bps);
    controller_->setVmBreakpointConditions(conds);
}

// ============================================================
// Event filter: editor tab middle-click close + hover close button
// ============================================================

/// 事件过滤器：处理编辑器区文件拖放、标题栏双击最大化等全局交互。
bool Ide::eventFilter(QObject* watched, QEvent* event) {
    // R61-3 fix: dockManager_ 的 Resize 事件 → 防抖保存布局
    // 用户拖拽 ADS splitter 调整面板大小时，dockManager_ 收到 Resize 事件
    if (watched == dockManager_ && event->type() == QEvent::Resize) {
        if (splitterSaveTimer_ && !firstShow_) {
            splitterSaveTimer_->start();
        }
    }
    // 第十二轮：统一标题栏拖拽（仅在非交互控件区域触发窗口拖动）
    if (watched == titleBar_) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                // 排除所有可交互控件：按钮（含 Fluent ComboBox/PushButton/SplitButton）等
                // BUG-IDE-13 fix: 扩展交互控件检测范围。QFluentKit 的 ComboBox 继承自
                // QPushButton（已能被 QAbstractButton 匹配），此处额外加 QComboBox 与
                // QLineEdit 是为防御性覆盖未来可能改用其他基类或新增输入控件的场景。
                auto* child = titleBar_->childAt(me->pos());
                bool isInteractive = false;
                while (child && child != titleBar_) {
                    if (qobject_cast<QAbstractButton*>(child) || qobject_cast<QComboBox*>(child) ||
                        qobject_cast<QLineEdit*>(child)) {
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
                    if (qobject_cast<QAbstractButton*>(child) || qobject_cast<QComboBox*>(child) ||
                        qobject_cast<QLineEdit*>(child)) {
                        isInteractive = true;
                        break;
                    }
                    child = child->parentWidget();
                }
                if (!isInteractive) {
                    if (isMaximized())
                        showNormal();
                    else
                        showMaximized();
                }
            }
        }
        return false; // 不拦截，按钮仍可正常点击
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

/// 更新标签页关闭按钮显隐（仅悬停或激活标签显示）。
void Ide::updateTabCloseButtons(int hoveredIndex) {
    int current = editorTabWidget_->currentIndex();
    for (int i = 0; i < editorTabWidget_->count(); ++i) {
        bool show = (i == current) || (i == hoveredIndex);
        QWidget* btn = editorTabWidget_->tabBar()->tabButton(i, QTabBar::RightSide);
        if (btn)
            btn->setVisible(show);
    }
}

// ============================================================
// Multi-tab editor management
// ============================================================

/// 新建编辑器标签页：构建 CodeEditor/语法高亮/查找面板并连接信号，返回标签索引。
int Ide::createNewEditorTab(const QString& filePath, const QString& content) {
    EditorTabData data;
    // IDE-LIFE-01 fix: container 传入 editorTabWidget_ 作为 parent，防止 insertTab
    // 之前的异常路径（如 new CodeEditor 抛 bad_alloc）导致 container 孤儿泄漏。
    // Qt parent 机制会在 editorTabWidget_ 析构时自动删除 container。
    data.container = new QWidget(editorTabWidget_);
    data.isUntitled = filePath.isEmpty();
    data.filePath = filePath;

    auto* layout = new QVBoxLayout(data.container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    data.editor = new CodeEditor(data.container);
    data.editor->setMinimumWidth(200);
    // 应用全局字号（与其他编辑器标签页保持一致）
    if (codeFontSize_ != 11) {
        data.editor->changeFontSize(codeFontSize_ - 11);
    }
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
        tabTitle = mlTr("未命名-%1").arg(untitledCount_);
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
        if (tabIdx < 0 || tabIdx >= editorTabWidget_->count())
            return;
        QString text = editorTabWidget_->tabText(tabIdx);
        const QString dot = QString(QChar(0x25CF)) + " ";
        bool hasDot = text.startsWith(dot);
        if (text.endsWith("*"))
            text.chop(1);
        if (changed && !hasDot) {
            editorTabWidget_->setTabText(tabIdx, dot + text);
        } else if (!changed && hasDot) {
            text.remove(0, dot.length());
            editorTabWidget_->setTabText(tabIdx, text);
        }
    };

    connect(data.editor->document(), &QTextDocument::modificationChanged, this,
            [this, editorPtr, updateTabDirtyMark](bool changed) {
                int tabIdx = -1;
                for (int i = 0; i < static_cast<int>(editorTabs_.size()); ++i) {
                    if (editorTabs_[i].editor == editorPtr) {
                        tabIdx = i;
                        break;
                    }
                }
                if (tabIdx < 0)
                    return;
                updateTabDirtyMark(tabIdx, changed);
                if (tabIdx == editorTabWidget_->currentIndex()) {
                    isDirty_ = changed;
                    updateWindowTitle();
                }
            });

    connect(data.editor, &CodeEditor::breakpointConditionRequested, this, [this](int line, const QString& condition) {
        controller_->setBreakpointCondition(line, condition.toStdString());
    });

    // AUDIT-P2-CORRECT fix: 行号区点击切换断点时，若处于 Interpreter 调试暂停状态，
    // 同步断点到 DebugController，避免新断点不生效/已删除断点仍触发。
    // VM 模式无需处理（每次步进前 syncVmBreakpoints 会同步）。
    connect(data.editor, &CodeEditor::breakpointsChanged, this, [this]() {
        if (controller_->isDebugPaused()) {
            controller_->setBreakpoints(codeEditor_->getBreakpoints());
        }
        // AUDIT-P2-ROUND49 fix: VM RUN 模式下断点变更不同步到 VmStepper 的 vmBreakpoints_。
        // VmStepper.runBatch() 使用内部 vmBreakpoints_ 副本，不直接读取编辑器断点，
        // 导致 VM 运行期间新增/删除的断点不生效。isDebugPaused() 仅检查 Interpreter 调试
        // 状态，VM RUN 模式下始终为 false。补充 VM 模式同步分支。
        if (controller_->isVmInitialized() || controller_->isVmRunning()) {
            syncVmBreakpoints();
        }
    });

    data.editor->setCompletionWords(staticCompletionWords_);

    // Wire textChanged for debounce timers
    connect(data.editor, &QPlainTextEdit::textChanged, this, [this]() {
        if (completionTimer_)
            completionTimer_->start();
        if (syntaxCheckTimer_)
            syntaxCheckTimer_->start();
    });

    // Update status bar (line/column) when cursor moves or text changes
    connect(data.editor, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        if (codeEditor_ == sender())
            updateStatusBar();
    });

    // 第十三轮：CodeEditor 右键菜单 contextActionRequested 信号路由
    connect(data.editor, &CodeEditor::contextActionRequested, this, [this](const QString& action) {
        if (action == "toggleComment") {
            // 复用 CodeEditor 自带的 Ctrl+/ 逻辑
            QKeyEvent keyPress(QEvent::KeyPress, Qt::Key_Slash, Qt::ControlModifier, "/");
            QApplication::sendEvent(codeEditor_, &keyPress);
        } else if (action == "toggleBlockComment") {
            QKeyEvent keyPress(QEvent::KeyPress, Qt::Key_Slash, Qt::ControlModifier | Qt::ShiftModifier, "/");
            QApplication::sendEvent(codeEditor_, &keyPress);
        } else if (action == "format") {
            onFormat();
        } else if (action == "gotoLine") {
            QKeyEvent keyPress(QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier, "g");
            QApplication::sendEvent(codeEditor_, &keyPress);
        } else if (action == "find") {
            onFind();
        } else if (action == "replace") {
            onReplace();
        } else if (action == "toggleBreakpoint") {
            // 切换当前行断点
            int line = codeEditor_->textCursor().blockNumber() + 1;
            auto bps = codeEditor_->getBreakpoints();
            if (bps.contains(line))
                bps.remove(line);
            else
                bps.insert(line);
            codeEditor_->setBreakpoints(bps);
            syncVmBreakpoints();
        } else if (action == "editBreakpointCondition") {
            // 复用 CodeEditor 的断点条件编辑
            int line = codeEditor_->textCursor().blockNumber() + 1;
            // 通过 LineNumberArea 的右键菜单触发——直接发射 breakpointConditionRequested
            // CodeEditor 没有公共 API，这里简化为提示用户使用行号区右键
            QToolTip::showText(QCursor::pos(), mlTr("请在行号左侧右键点击断点设置条件"), codeEditor_);
        } else if (action == "runToCursor") {
            // 运行到当前行：通过断点临时切换实现（简化版）
            int line = codeEditor_->textCursor().blockNumber() + 1;
            // TODO: 实现真正的 runToCursor，当前提示用户该功能开发中
            QToolTip::showText(QCursor::pos(), mlTr("运行到当前行功能开发中"), codeEditor_);
        }
    });

    return idx;
}

/// 切换到指定标签：同步活动编辑器、高亮器、文件路径与窗口标题。
void Ide::switchToTab(int index) {
    if (index < 0 || index >= static_cast<int>(editorTabs_.size()))
        return;
    auto& data = editorTabs_[index];
    codeEditor_ = data.editor;
    highlighter_ = data.highlighter;
    findReplacePanel_ = data.findPanel;
    currentFilePath_ = data.filePath;
    // BUG-REPL-AUDIT-9 fix: 同步活动文件路径到 controller，供 REPL 模块加载器解析相对 import 路径
    controller_->setActiveFilePath(data.filePath.toStdString());
    isDirty_ = codeEditor_->document()->isModified();
    editorTabWidget_->setCurrentIndex(index);
    updateWindowTitle();
    updateStatusBar();
}

/// 按绝对路径查找已打开的对应标签索引，未找到返回 -1。
int Ide::findTabForFile(const QString& path) {
    QFileInfo targetFi(path);
    for (int i = 0; i < static_cast<int>(editorTabs_.size()); ++i) {
        if (editorTabs_[i].filePath.isEmpty())
            continue;
        QFileInfo tabFi(editorTabs_[i].filePath);
        if (tabFi.absoluteFilePath() == targetFi.absoluteFilePath())
            return i;
    }
    return -1;
}

/// 将磁盘文件读入指定标签页：校验大小上限、去除 BOM、设置内容并启动文件监视。
void Ide::loadFileIntoTab(int tabIndex, const QString& path) {
    if (tabIndex < 0 || tabIndex >= static_cast<int>(editorTabs_.size()))
        return;
    auto& data = editorTabs_[tabIndex];

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        InfoBar::warning(mlTr("错误"), mlTr("无法打开文件: ") + file.errorString(), Qt::Horizontal, true, 2500,
                         InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    qint64 fileSize = file.size();
    if (fileSize > static_cast<qint64>(RuntimeLimits::MAX_SOURCE_SIZE)) {
        InfoBar::warning(mlTr("错误"),
                         mlTr("文件过大 (") + QString::number(fileSize) + mlTr(" 字节)，超过上限 (") +
                             QString::number(RuntimeLimits::MAX_SOURCE_SIZE) + mlTr(" 字节)"),
                         Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT, this);
        file.close();
        return;
    }
    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    QString content = in.readAll();
    file.close();
    if (!content.isEmpty() && content[0] == QChar(0xFEFF))
        content.remove(0, 1);

    data.editor->setPlainText(content);
    data.editor->document()->setModified(false);
    data.filePath = path;
    data.isUntitled = false;

    QFileInfo fi(path);
    editorTabWidget_->setTabText(tabIndex, fi.fileName());

    if (tabIndex == editorTabWidget_->currentIndex()) {
        currentFilePath_ = path;
        // BUG-REPL-AUDIT-9 fix: 同步活动文件路径到 controller，供 REPL 模块加载器解析相对 import 路径
        controller_->setActiveFilePath(path.toStdString());
        isDirty_ = false;
        updateWindowTitle();
        // 文件加载到当前标签后更新监视（onCurrentTabChanged 在 loadFileIntoTab 前触发，
        // 此时 currentFilePath_ 才是新路径，需要在此补一次 setupFileWatcher）
        setupFileWatcher(currentFilePath_);
    }
}

/// 当前标签切换响应：切换编辑器焦点、刷新关闭按钮与文件监视。
void Ide::onCurrentTabChanged(int index) {
    if (index < 0 || index >= static_cast<int>(editorTabs_.size()))
        return;
    switchToTab(index);
    if (codeEditor_)
        codeEditor_->setFocus();
    updateTabCloseButtons(index);
    // 切换标签时更新文件监视（指向当前标签的文件路径）
    setupFileWatcher(currentFilePath_);
}

/// 标签页关闭请求：提示保存后移除标签并清理关联状态。
void Ide::onEditorTabCloseRequested(int index) {
    if (index < 0 || index >= static_cast<int>(editorTabs_.size()))
        return;

    // R53-UX7 fix: 运行/调试中关闭标签会破坏调试上下文（currentFilePath_ 切换、
    // 断点清空、editorTabs_ 重组），导致 worker 线程引用的 source/filePath 失配。
    // 原实现无任何守卫。修复：运行中拒绝关闭并提示用户先停止。
    if (controller_->isRunning() || controller_->isVmRunning() || controller_->isDebugPaused()) {
        InfoBar::warning(mlTr("关闭标签"), mlTr("当前有运行或调试在进行，请先停止运行再关闭标签。"), Qt::Horizontal,
                         true, 4000, InfoBar::Position::TOP_RIGHT, this);
        return;
    }

    auto& data = editorTabs_[index];
    if (data.editor->document()->isModified()) {
        codeEditor_ = data.editor;
        currentFilePath_ = data.filePath;
        if (!maybeSave())
            return;
    }

    // Close last tab: clear editor area
    if (static_cast<int>(editorTabs_.size()) <= 1) {
        editorTabWidget_->removeTab(0);
        data.container->deleteLater();
        editorTabs_.clear();
        codeEditor_ = nullptr;
        highlighter_ = nullptr;
        findReplacePanel_ = nullptr;
        currentFilePath_.clear();
        // BUG-REPL-AUDIT-9 fix: 清空 controller 的活动文件路径（无活动文件时 REPL loader 回退到字面路径解析）
        controller_->setActiveFilePath("");
        isDirty_ = false;
        // BUG-IDE-11 fix: 关闭最后一个编辑器标签时清空 Interpreter/VM 断点。
        // 原实现仅清空本地 editor 引用，DebugCoordinator/VmStepper 仍持有旧断点行号。
        // 下次新建/打开文件时若行号重叠会在新文件的对应行意外暂停（断点行号是全局的，
        // 不与文件绑定）。
        controller_->setBreakpoints(QSet<int>());
        controller_->setVmBreakpoints(QSet<int>());
        controller_->setVmBreakpointConditions(QMap<int, std::string>());
        hideBottomPanel();
        hideRightPanel();
        updateWindowTitle();
        // 关闭最后一个标签时清空文件监视
        setupFileWatcher(QString());

        // issue 3：教学模式下关闭编辑器，教学区平滑延展恢复（不回欢迎页）。
        // 编辑器模式下回欢迎页（原逻辑）。
        if (!centerInEditorMode_ && centerSplitter_ && centerStack_) {
            // 教学模式：动画延展教学区到全宽
            // ROUND56 fix: 原实现先 editorTabWidget_->hide() 再 animateCenterSplitter，
            // QSplitter 在子控件 hide 时会自动重分配空间（centerStack_ 立即获得全宽），
            // 导致 animateCenterSplitter 的 startSizes 与实际不符，动画无视觉效果，
            // 用户感知"全部被关闭"（教学区瞬时跳到全宽而非平滑延展）。
            // 修复：先动画到 [total, 0]，动画结束后再 hide editorTabWidget_。
            QList<int> savedSizes = centerSplitter_->sizes();
            centerStack_->show();
            if (savedSizes.size() == 2 && savedSizes[1] > 5) {
                int total = savedSizes[0] + savedSizes[1];
                if (total > 100) {
                    animateCenterSplitter(savedSizes, {total, 0});
                    // 动画结束后再隐藏编辑器栏（避免动画期间 QSplitter 自动重分配）
                    // ROUND-75 fix: 成员 QTimer + closing_ 守卫，closeEvent 可取消
                    if (!pendingHideEditorTimer_) {
                        pendingHideEditorTimer_ = new QTimer(this);
                        pendingHideEditorTimer_->setSingleShot(true);
                        connect(pendingHideEditorTimer_, &QTimer::timeout, this, [this]() {
                            if (closing_)
                                return;
                            if (editorTabWidget_ && editorTabWidget_->count() == 0)
                                editorTabWidget_->hide();
                        });
                    }
                    pendingHideEditorTimer_->start(360);
                } else {
                    editorTabWidget_->hide();
                }
            } else {
                // 编辑器宽度已为 0 或极小，直接隐藏
                editorTabWidget_->hide();
                if (savedSizes.size() == 2) {
                    int total = savedSizes[0] + savedSizes[1];
                    centerSplitter_->setSizes({total, 0});
                }
            }
        } else {
            // 编辑器模式：回欢迎页
            centerStack_->setCurrentWidget(welcomePage_);
            centerStack_->show();
            editorTabWidget_->hide();
            // ROUND-76 fix: 关闭最后一个标签后必须重分配 centerSplitter_ 让 centerStack_
            // 占满，否则 splitter 保持编辑器独占态 [0, total]（centerStack_ 宽 0）或
            // 三栏态 [420, 780]（editorSplitter_ 内已空却仍占大半宽度），表现为
            // 「欢迎页空白」+「之后切学习面板加载不出来」。
            // 此处与 showTeachingPanel 的无标签分支互为兜底：这里治本（欢迎页可见），
            // showTeachingPanel 兜底保证教学面板占满。
            if (centerSplitter_) {
                QList<int> curSizes = centerSplitter_->sizes();
                if (curSizes.size() == 2) {
                    int total = curSizes[0] + curSizes[1];
                    if (total > 200 && curSizes[0] < total - 5) {
                        // editorSplitter_ 内已无标签 + 底部面板已 hide，
                        // 直接收为 {total, 0}，无需动画（关闭动画已由 hideBottomPanel 完成）
                        centerSplitter_->setSizes({total, 0});
                    }
                }
            }
        }
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

/// 中心分栏尺寸动画：在 startSizes 与 endSizes 间用 QVariantAnimation 平滑过渡（教学面板滑入/滑出用）。
void Ide::animateCenterSplitter(const QList<int>& startSizes, const QList<int>& targetSizes, int durationMs) {
    if (!centerSplitter_)
        return;
    if (splitterAnim_) {
        splitterAnim_->stop();
        splitterAnim_ = nullptr;
    }
    if (startSizes.size() != 2 || targetSizes.size() != 2)
        return;
    if (qAbs(startSizes[0] - targetSizes[0]) < 5)
        return;

    auto* anim = new QVariantAnimation(this);
    anim->setDuration(durationMs);
    anim->setEasingCurve(QEasingCurve::InOutCubic);
    anim->setStartValue(startSizes[0]);
    anim->setEndValue(targetSizes[0]);
    connect(anim, &QVariantAnimation::valueChanged, this, [this, startSizes, targetSizes](const QVariant& val) {
        if (!centerSplitter_)
            return;
        int s0 = val.toInt();
        int total = startSizes[0] + startSizes[1];
        int s1 = total - s0;
        if (s0 < 0)
            s0 = 0;
        if (s1 < 0)
            s1 = 0;
        centerSplitter_->setSizes({s0, s1});
    });
    connect(anim, &QVariantAnimation::finished, this, [this, targetSizes]() {
        if (centerSplitter_)
            centerSplitter_->setSizes(targetSizes);
        splitterAnim_ = nullptr;
    });
    splitterAnim_ = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

/// 确保主编辑器区域可见（切回编辑器视图，隐藏其他中心面板）。
void Ide::ensureEditorVisible() {
    // 任务2：三栏布局 — editorTabWidget_ 在 centerSplitter_ 中独立显示
    // 记录切换前的状态，用于判断是否需要重新分配 splitter 空间
    const bool wasHidden = editorTabWidget_ && !editorTabWidget_->isVisible();

    if (editorTabWidget_) {
        editorTabWidget_->show();
    }

    // ROUND-60 fix (Issue 3+4): 当教学面板可见时，保留教学模式（三栏共存），
    // 不切换到编辑器独占模式。原实现无条件将 centerInEditorMode_ 置 true 并
    // 隐藏 centerStack_，导致：(1) 关闭编辑器时教学区丢失（Issue 3——关闭后
    // centerInEditorMode_ 已为 true，onEditorTabCloseRequested 走欢迎页分支）；
    // (2) 之后无法打开学习模块面板（Issue 4——centerStack_ 被 hide 后
    // showTeachingPanel 的动画条件 !editorWasVisible 不满足，splitter 不重分配，
    // 面板 0 宽不可见）。修复：教学模式下仅确保编辑器栏可见并设置三栏布局。
    if (centerInEditorMode_ == false) {
        // 教学模式：保留 centerInEditorMode_ = false，仅设置三栏布局
        if (wasHidden && centerSplitter_ && centerStack_) {
            QList<int> savedSizes = centerSplitter_->sizes();
            if (savedSizes.size() == 2) {
                int total = savedSizes[0] + savedSizes[1];
                if (total > 200) {
                    // 教学区 45%，编辑器 55%（与 loadCodeIntoMainEditor 一致）
                    int editorW = static_cast<int>(total * 0.55);
                    int teachingW = total - editorW;
                    animateCenterSplitter(savedSizes, {teachingW, editorW});
                }
            }
        }
        return; // 不执行后续的编辑器独占模式逻辑
    }

    // 编辑器模式：切回编辑器视图（隐藏教学面板栏）
    // 注意：此处不立即 hide centerStack_，留到动画末尾再 hide，保证过渡平滑

    // 修复（issue 3）：从欢迎页或教学面板打开文件时，编辑器需完全展开。
    // 原逻辑仅在 centerStack_->isHidden() 时触发动画，但欢迎页场景下 centerStack_
    // 仍可见（显示欢迎页），导致动画被跳过、编辑器未占满空间。
    // 新逻辑：只要 editorTabWidget_ 之前是隐藏状态（wasHidden），就动画折叠
    // centerStack_ 让编辑器获得全部宽度。
    if (wasHidden && centerSplitter_ && centerStack_) {
        QList<int> savedSizes = centerSplitter_->sizes();
        if (savedSizes.size() == 2 && savedSizes[0] > 10) {
            int total = savedSizes[0] + savedSizes[1];
            if (total > 100) {
                animateCenterSplitter(savedSizes, {0, total});
                // ROUND-75 fix: 用成员 QTimer 替代 QTimer::singleShot。
                // 原因：singleShot 创建的临时 QTimer 事件队列独立，
                // showTeachingPanel 无法取消挂起的 hide 意图，导致用户在 350ms 内
                // 切换到教学面板后，回调仍执行 centerStack_->hide() 覆盖 show()。
                // 改为成员 QTimer 后，showTeachingPanel 入口 stop() 即可撤销折叠意图。
                // 同时加 closing_ 守卫，避免 closeEvent 期间回调触发 UAF。
                if (!pendingHideCenterTimer_) {
                    pendingHideCenterTimer_ = new QTimer(this);
                    pendingHideCenterTimer_->setSingleShot(true);
                    connect(pendingHideCenterTimer_, &QTimer::timeout, this, [this]() {
                        if (closing_)
                            return;
                        if (centerStack_ && editorTabWidget_ && editorTabWidget_->isVisible()) {
                            centerStack_->hide();
                            int tw = centerSplitter_->width();
                            if (tw > 100)
                                centerSplitter_->setSizes({0, tw});
                        }
                    });
                }
                pendingHideCenterTimer_->start(350);
            }
        } else if (savedSizes.size() == 2) {
            // centerStack_ 已折叠（savedSizes[0] <= 10）：直接收尾
            if (centerStack_)
                centerStack_->hide();
            int tw = centerSplitter_->width();
            if (tw > 100)
                centerSplitter_->setSizes({0, tw});
        }
    }
}

// ============================================================
// 教学面板树形导航：centerStack_ 切换（第十四轮重构）
// ============================================================

/// 懒加载指定教学面板（首次访问时构建并缓存）。
void Ide::ensureTeachingPanelCreated(const QString& panelId) {
    // 懒加载：若面板已构造（在 panelToStackIndex_ 中）则直接返回
    if (panelToStackIndex_.contains(panelId))
        return;
    auto factoryIt = teachingPanelFactories_.constFind(panelId);
    if (factoryIt != teachingPanelFactories_.constEnd()) {
        factoryIt.value()(); // 调用工厂构造面板
    }
}

/// 显示指定教学面板：经 centerStack_ 路由并以滑入动画呈现。
void Ide::showTeachingPanel(const QString& panelId) {
    // 懒加载：首次访问时构造面板
    ensureTeachingPanelCreated(panelId);

    // ROUND-75 fix (根因 A1+A2): 取消"折叠教学区"的挂起意图。
    // ensureEditorVisible/showEditorArea 在动画启动后排队了 350ms 延迟回调，
    // 回调会执行 centerStack_->hide() + setSizes({0, total})。
    // 若用户在 350ms 内切换到教学面板，该回调仍会执行，覆盖 showTeachingPanel
    // 的 centerStack_->show()，并锁死教学区宽度为 0——表现为"教学面板打不开"。
    // 停止成员 QTimer 即可撤销挂起的 hide 意图。
    if (pendingHideCenterTimer_)
        pendingHideCenterTimer_->stop();
    // 同时停止正在运行的折叠教学区动画（splitterAnim_ 目标 {0, total}）。
    // 否则旧动画继续把教学区宽度压缩到 0，且 showTeachingPanel 的动画分支
    // 条件在"教学区尚可见但正被压缩"时不满足，splitter 不重分配。
    if (splitterAnim_) {
        splitterAnim_->stop();
        splitterAnim_ = nullptr;
    }

    auto it = panelToStackIndex_.constFind(panelId);
    if (it == panelToStackIndex_.end() || !centerStack_) {
        // panelId 不在映射中，回退到编辑器模式
        showEditorArea();
        return;
    }
    int idx = it.value();

    // 任务2：三栏布局 — 先保存 splitter 尺寸（布局引擎还未重算）
    const bool editorHasTabs = editorTabWidget_ && editorTabWidget_->count() > 0;
    const bool editorWasVisible = editorTabWidget_ && editorTabWidget_->isVisible();
    QList<int> savedSplitterSizes;
    if (centerSplitter_)
        savedSplitterSizes = centerSplitter_->sizes();

    // BUG-R14-2 fix: 从编辑器模式进入教学面板时，记录 bottomContainer_/rightDock_ 的可见状态，
    // 供 showEditorArea 恢复。必须在 dock 显隐调整之前记录，否则会捕获到隐藏后的错误状态。
    // 教学面板间切换时不记录（已隐藏，避免覆盖记录）。
    if (centerInEditorMode_) {
        bottomDockWasVisibleBeforeTeaching_ = bottomVisible_;
        rightDockWasVisibleBeforeTeaching_ = (rightDock_ && !rightDock_->isClosed());
    }
    centerInEditorMode_ = false;

    // 动画流畅度优化：dock 显隐在 setCurrentIndex 之前完成，让布局重算
    // 在动画启动前结束，避免动画进行中 dock 隐藏触发 layout 重绘导致卡顿。
    // P0-1 fix (F5/F13): 教学面板模式下对 bottomDock_（输出/错误/REPL）采用白名单策略。
    // 需要运行反馈的面板（实验手册/Bug狩猎/语法浏览器/后端对比）保留 bottomDock_，
    // 让学员能在面板旁看到运行输出与报错，打通"看教学 + 写代码 + 看运行结果"同屏闭环。
    // P1-E fix: rightDock_（编译分析）白名单——对需要对照真实编译产物/字节码/内存
    // 动画的面板（pipeline/ir-transform/bytecode-trace/memory-model/backend-compare）保留。
    static const QSet<QString> kKeepBottomDockPanels = {
        QStringLiteral("lab-manual"),      QStringLiteral("bug-hunt"),     QStringLiteral("syntax-explorer"),
        QStringLiteral("backend-compare"), QStringLiteral("ir-transform"), QStringLiteral("profile-dashboard"),
    };
    const bool keepBottom = kKeepBottomDockPanels.contains(panelId);
    if (!keepBottom && bottomVisible_) {
        hideBottomPanel();
    }
    static const QSet<QString> kKeepRightDockPanels = {
        QStringLiteral("pipeline"),     QStringLiteral("ir-transform"),    QStringLiteral("bytecode-trace"),
        QStringLiteral("memory-model"), QStringLiteral("backend-compare"),
    };
    const bool keepRight = kKeepRightDockPanels.contains(panelId);
    if (!keepRight && rightDock_ && !rightDock_->isClosed()) {
        rightDock_->toggleView(false);
    }

    centerStack_->setCurrentIndex(idx);
    centerStack_->show(); // 确保教学面板栏可见（可能被 showEditorArea 隐藏）

    // 教学面板刷新移到 setCurrentIndex 之后：先切换页面让用户看到响应，
    // 再执行可能耗时的 refresh（learning-path 重建 28 活动 × 多标签）。
    // pipeline 的 reloadCurrentStep 也同理延后。
    if (panelId == QStringLiteral("pipeline") && pipelineViewer_) {
        pipelineViewer_->reloadCurrentStep();
    } else if (panelId == QStringLiteral("learning-path") && learningPathPanel_) {
        learningPathPanel_->refresh();
    }

    // 轻量过渡动画：新面板从右侧 24px 滑入，替代原 LearningPathPanel 内
    // fadeInWidget 的 QGraphicsOpacityEffect 方案（后者对 100+ 子 widget 做
    // 离屏合成，是章节切换卡顿与偶发崩溃的根因）。slideInWidget 仅驱动 pos
    // 属性，O(1) 复杂度，无 pixmap 合成，适合任意复杂度的教学面板。
    // 动画时长优化：从 220ms (FADE_DURATION_MS) 降到 150ms (DURATION_MS)，
    // 对齐 VS Code 面板切换节奏，减少视觉等待。
    if (QWidget* newPanel = centerStack_->widget(idx)) {
        PanelAnimator::slideInWidget(newPanel, PanelAnimator::DURATION_MS);
    }

    // 编辑器栏：若有标签则保持可见（形成三栏），无标签则隐藏
    if (editorTabWidget_ && editorTabWidget_->count() == 0) {
        editorTabWidget_->hide();
    } else if (editorTabWidget_) {
        editorTabWidget_->show();
    }

    // 动画：编辑器从右侧滑入，教学区压缩
    // ROUND-60 fix (Issue 4): 原条件仅 !editorWasVisible 时触发动画，
    // 但编辑器已可见（编辑器独占模式 centerStack_ 被 hide 且宽度 0）时，
    // 需要重分配 splitter 让教学区获得可见宽度。检测 centerStack_ 是否被
    // 隐藏或宽度为 0，若是则触发重分配动画。
    const bool centerStackWasHiddenOrZero =
        !centerStack_->isVisible() || (savedSplitterSizes.size() == 2 && savedSplitterSizes[0] <= 10);
    if (editorHasTabs && savedSplitterSizes.size() == 2 && (!editorWasVisible || centerStackWasHiddenOrZero)) {
        int total = savedSplitterSizes[0] + savedSplitterSizes[1];
        if (total > 200) {
            int teachingW = static_cast<int>(total * 0.6);
            int editorW = total - teachingW;
            animateCenterSplitter(savedSplitterSizes, {teachingW, editorW});
        }
    } else if (!editorHasTabs && savedSplitterSizes.size() == 2) {
        // ROUND-76 fix: 无编辑器标签时教学区必须占满 splitter。
        // 原 ROUND-60 fix 仅在 centerStackWasHiddenOrZero 时动画，漏掉
        // 「centerStack_ 可见但 editorSplitter_ 仍占据大半宽度」的场景：
        //   - 关闭所有标签后 splitter 保持 [420, 780]（centerStack_ 有宽度但
        //     editorSplitter_ 内已空），教学面板被挤在 420px 窄区域、右侧大片空白，
        //     用户感知「学习面板加载不出来」；
        //   - onEditorTabCloseRequested 已治本性收为 {total, 0}，此处为兜底：
        //     只要 editorSplitter_ 还占空间（savedSplitterSizes[1] > 5）就动画压缩到 0，
        //     保证教学面板在任何 splitter 残留状态下都能占满。
        int total = savedSplitterSizes[0] + savedSplitterSizes[1];
        if (total > 200 && savedSplitterSizes[1] > 5) {
            animateCenterSplitter(savedSplitterSizes, {total, 0});
        }
    }

    // 同步教学树高亮（跨面板跳转时让树节点选中对应项）
    if (teachingTreePanel_) {
        teachingTreePanel_->setCurrentPanel(panelId);
    }

    // 确保教学树 dock 可见（用户可能从视图菜单快捷键触发，此时树可能隐藏）
    if (teachingTreeDock_ && teachingTreeDock_->isClosed()) {
        teachingTreeDock_->toggleView(true);
    }

    // P1-F2/F fix: 浏览型面板进入即标记 visited-* 活动完成。
    // 这些面板无"通关"概念（纯参考资料展示），学员进入浏览即视为"已查阅"。
    // 让 LearningPathData 中的 visited-* 活动有完成态，避免"永远未完成"误导。
    if (learningPathPanel_) {
        static const QHash<QString, QString> kBrowsePanelToActivity = {
            {QStringLiteral("glossary"), QStringLiteral("visited-glossary")},
            {QStringLiteral("pipeline"), QStringLiteral("visited-pipeline")},
            {QStringLiteral("memory-model"), QStringLiteral("visited-memory-model")},
            {QStringLiteral("bytecode-trace"), QStringLiteral("visited-bytecode-trace")},
            {QStringLiteral("exception-flow"), QStringLiteral("visited-exception-flow")},
            {QStringLiteral("closure-inspector"), QStringLiteral("visited-closure-inspector")},
            // AUDIT-P1 fix: 以下 4 个面板无"通关"概念（工具/参考资料型），
            // 进入浏览即视为"已查阅"，标记对应活动完成。否则这些活动永远不可完成，
            // 导致阶段 1/2/3 的 stageProgress 无法达到 100%，通关状态不可达。
            {QStringLiteral("syntax-explorer"), QStringLiteral("syntax-explorer")},
            {QStringLiteral("backend-compare"), QStringLiteral("backend-compare")},
            {QStringLiteral("ir-transform"), QStringLiteral("ir-transform")},
            {QStringLiteral("profile-dashboard"), QStringLiteral("profile-dashboard")},
        };
        auto actIt = kBrowsePanelToActivity.constFind(panelId);
        if (actIt != kBrowsePanelToActivity.constEnd()) {
            learningPathPanel_->markActivityCompleted(actIt.value());
        }
    }

    // 首次访问 5 个目标面板时自动触发新手引导（QSettings 持久化「已显示」标记）。
    // 仅对带 createGuidedTour 的面板生效；用户跳过或走完后不再自动弹出。
    static const QSet<QString> kAutoTourPanels = {
        QStringLiteral("bytecode-trace"),       QStringLiteral("call-stack"), QStringLiteral("variable-inspector"),
        QStringLiteral("breakpoint-condition"), QStringLiteral("bug-hunt"),
    };
    if (kAutoTourPanels.contains(panelId)) {
        QSettings s;
        const QString key = QStringLiteral("guidedTour/shown_%1").arg(panelId);
        if (!s.value(key, false).toBool()) {
            s.setValue(key, true);
            // 延迟一帧启动，确保面板已完成布局（widget 几何就绪后高亮定位才准确）
            QTimer::singleShot(0, this, [this, panelId]() { onPanelGuidedTourRequested(panelId); });
        }
    }

    syncViewMenuChecks();
}

/// 将中心区域切回主编辑器（隐藏欢迎页/教学面板）。
void Ide::showEditorArea() {
    if (!centerStack_)
        return;

    // BUG-R14-2 fix: 从教学面板模式切回编辑器时，恢复 bottomContainer_/rightDock_ 的可见状态
    // 到进入教学面板前的状态。尊重用户上次的布局选择（若用户原本就隐藏了输出面板，不强制弹出）。
    if (!centerInEditorMode_) {
        if (!bottomVisible_ && bottomDockWasVisibleBeforeTeaching_) {
            showBottomPanel();
        }
        if (rightDock_ && rightDock_->isClosed() && rightDockWasVisibleBeforeTeaching_) {
            rightDock_->toggleView(true);
        }
    }

    // 任务2：三栏布局 — 先保存 splitter 尺寸，再做可见性变化
    if (editorTabWidget_ && editorTabWidget_->count() > 0) {
        QList<int> savedSizes;
        if (centerSplitter_)
            savedSizes = centerSplitter_->sizes();
        editorTabWidget_->show();
        // 流畅动画：从保存的初始尺寸动画到编辑器独占
        if (savedSizes.size() == 2 && savedSizes[0] > 10) {
            int total = savedSizes[0] + savedSizes[1];
            animateCenterSplitter(savedSizes, {0, total});
            // ROUND-75 fix: 成员 QTimer（同 ensureEditorVisible），可被 showTeachingPanel 取消
            if (!pendingHideCenterTimer_) {
                pendingHideCenterTimer_ = new QTimer(this);
                pendingHideCenterTimer_->setSingleShot(true);
                connect(pendingHideCenterTimer_, &QTimer::timeout, this, [this]() {
                    if (closing_)
                        return;
                    if (centerStack_)
                        centerStack_->hide();
                    if (centerSplitter_) {
                        int tw = centerSplitter_->width();
                        if (tw > 100)
                            centerSplitter_->setSizes({0, tw});
                    }
                });
            }
            pendingHideCenterTimer_->start(350);
        } else {
            if (centerStack_)
                centerStack_->hide();
            if (centerSplitter_) {
                int tw = centerSplitter_->width();
                if (tw > 100)
                    centerSplitter_->setSizes({0, tw});
            }
        }
    } else {
        // 无标签：显示欢迎页
        centerStack_->setCurrentWidget(welcomePage_);
        centerStack_->show();
        if (editorTabWidget_)
            editorTabWidget_->hide();
    }
    centerInEditorMode_ = true;

    // 同步教学树高亮到「代码编辑器」项
    if (teachingTreePanel_) {
        teachingTreePanel_->setCurrentPanel(QStringLiteral("editor"));
    }

    syncViewMenuChecks();
}

/// 教学面板请求信号：按需创建并显示对应面板。
void Ide::onTeachingPanelRequested(const QString& panelId) {
    // 教学树点击 → panelId 路由
    if (panelId == QStringLiteral("editor")) {
        showEditorArea();
    } else if (panelId == QStringLiteral("welcome")) {
        // BUG-R14-1 fix: welcome 不是教学面板，路由到 onActivityRequested 创建 WelcomeWizard
        onActivityRequested(QStringLiteral("welcome"));
    } else {
        showTeachingPanel(panelId);
    }
}

/// 将全局代码字号应用到所有已打开的编辑器。
void Ide::applyCodeFontSizeToAllEditors() {
    // 遍历所有已打开的编辑器标签页，应用全局 codeFontSize_
    for (auto& tab : editorTabs_) {
        if (tab.editor) {
            int cur = tab.editor->fontSize();
            int delta = codeFontSize_ - cur;
            if (delta != 0) {
                tab.editor->changeFontSize(delta);
            }
        }
    }
    // 同步教学面板字号（教学阅读字号 = codeFontSize_ + 2）
    applyTeachingFontSize();
    // 更新当前活跃编辑器的状态栏显示
    updateStatusBar();
}

/// 应用教学面板字号（代码字号 +2）到所有教学面板。
void Ide::applyTeachingFontSize() {
    if (!centerStack_)
        return;
    // 教学阅读字号 = codeFontSize_ + 2（范围限制 [9, 34]）
    int teachingPt = qBound(9, codeFontSize_ + 2, 34);

    // 递归遍历 widget 树，找到所有 QTextBrowser / QListWidget / QTextEdit 并设置字号
    std::function<void(QWidget*)> applyToWidget = [&](QWidget* w) {
        if (!w)
            return;
        // QTextBrowser 是 QTextEdit 子类，先检查 QTextBrowser 再检查 QTextEdit
        if (auto* browser = qobject_cast<QTextBrowser*>(w)) {
            QFont f = browser->font();
            if (f.pointSize() != teachingPt) {
                f.setPointSize(teachingPt);
                browser->setFont(f);
            }
            return;
        }
        if (auto* list = qobject_cast<QListWidget*>(w)) {
            QFont f = list->font();
            if (f.pointSize() != teachingPt) {
                f.setPointSize(teachingPt);
                list->setFont(f);
            }
            return;
        }
        if (auto* edit = qobject_cast<QTextEdit*>(w)) {
            QFont f = edit->font();
            if (f.pointSize() != teachingPt) {
                f.setPointSize(teachingPt);
                edit->setFont(f);
            }
            return;
        }
        // 递归子 widget
        const auto children = w->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget* child : children) {
            applyToWidget(child);
        }
    };

    // 遍历 centerStack_ 的所有页面（教学面板）
    for (int i = 0; i < centerStack_->count(); ++i) {
        QWidget* page = centerStack_->widget(i);
        if (page)
            applyToWidget(page);
    }
}

/// 编辑器标签页右键菜单：关闭/关闭其他/关闭全部等动作。
void Ide::onEditorTabContextMenu(const QPoint& pos) {
    int idx = editorTabWidget_->tabBar()->tabAt(editorTabWidget_->tabBar()->mapFromGlobal(pos));
    if (idx < 0)
        return;

    QMenu menu(this);
    auto* closeAct = menu.addAction(mlTr("关闭"));
    auto* closeOthersAct = menu.addAction(mlTr("关闭其他"));
    auto* closeAllAct = menu.addAction(mlTr("关闭全部"));

    // Store the clicked index for the action handlers
    int clickedIdx = idx;

    auto* chosen = menu.exec(pos);
    if (!chosen)
        return;

    if (chosen == closeAct) {
        onEditorTabCloseRequested(clickedIdx);
    } else if (chosen == closeOthersAct) {
        // Close all tabs except clickedIdx
        // Close from right to left to preserve indices
        for (int i = static_cast<int>(editorTabs_.size()) - 1; i >= 0; --i) {
            if (i == clickedIdx)
                continue;
            onEditorTabCloseRequested(i);
            if (i < clickedIdx)
                clickedIdx--;
        }
    } else if (chosen == closeAllAct) {
        onCloseAllTabs();
    }
}

/// 关闭除当前标签外的所有标签页。
void Ide::onCloseOtherTabs() {
    int current = editorTabWidget_->currentIndex();
    for (int i = static_cast<int>(editorTabs_.size()) - 1; i >= 0; --i) {
        if (i == current)
            continue;
        onEditorTabCloseRequested(i);
        if (i < current)
            current--;
    }
}

/// 关闭所有标签页。
void Ide::onCloseAllTabs() {
    while (!editorTabs_.empty()) {
        onEditorTabCloseRequested(static_cast<int>(editorTabs_.size()) - 1);
    }
}

// ============================================================
// Welcome page
// ============================================================

/// 初始化欢迎页：构建最近工作区列表与快速开始入口。
void Ide::initWelcomePage() {
    welcomePage_ = new QWidget;
    welcomePage_->setObjectName("welcomePage");
    // 第十一轮：支持拖拽文件夹到欢迎页打开
    welcomePage_->setAcceptDrops(true);
    // ISSUE-3 fix: 确保 QSS background 在普通 QWidget 上生效
    welcomePage_->setAttribute(Qt::WA_StyledBackground, true);
    auto* outerLayout = new QHBoxLayout(welcomePage_);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // Left: recent workspaces (第十一轮：220px 宽)
    auto* recentPanel = new QWidget;
    recentPanel->setObjectName("welcomeRecentPanel");
    recentPanel->setFixedWidth(220);
    // ISSUE-3 fix: 确保 QSS background 在普通 QWidget 上生效
    recentPanel->setAttribute(Qt::WA_StyledBackground, true);
    auto* recentLayout = new QVBoxLayout(recentPanel);
    recentLayout->setContentsMargins(0, 16, 0, 0);
    recentLayout->setSpacing(0);

    auto* recentHeader = new QLabel(mlTr("最近打开"));
    recentHeader->setObjectName("welcomeRecentHeader");
    recentLayout->addWidget(recentHeader);

    recentListWidget_ = new QListWidget;
    recentListWidget_->setObjectName("welcomeRecentList");
    recentListWidget_->setFrameStyle(QFrame::NoFrame);
    recentListWidget_->setSpacing(0);
    recentListWidget_->setCursor(Qt::PointingHandCursor);
    connect(recentListWidget_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (!item)
            return;
        QString dir = item->data(Qt::UserRole).toString();
        if (!dir.isEmpty() && QDir(dir).exists())
            openWorkspace(dir);
    });
    connect(recentListWidget_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item)
            return;
        QString dir = item->data(Qt::UserRole).toString();
        if (!dir.isEmpty() && QDir(dir).exists())
            openWorkspace(dir);
    });
    recentLayout->addWidget(recentListWidget_, 1);
    outerLayout->addWidget(recentPanel);

    // Center area
    auto* centerArea = new QWidget;
    centerArea->setObjectName("welcomeCenter");
    centerArea->setAcceptDrops(true);
    // ISSUE-3 fix: 确保 QSS background 在普通 QWidget 上生效（applyFluentStyle 设置背景色）
    centerArea->setAttribute(Qt::WA_StyledBackground, true);
    auto* centerLayout = new QVBoxLayout(centerArea);
    centerLayout->setAlignment(Qt::AlignCenter);
    centerLayout->setSpacing(12);

    // 第十三轮：ML 几何品牌 Logo（精致连笔版，替代原 { } 文字）
    // 修复：QPixmap(":/icons/minilang_logo.svg") 在某些 Qt6 安装下因 SVG plugin
    // 加载时机问题返回空 pixmap。改用 QIcon 自动调用 QSvgRenderer 渲染，更可靠。
    auto* iconLabel = new QLabel;
    iconLabel->setObjectName("welcomeIcon");
    iconLabel->setAlignment(Qt::AlignCenter);
    {
        // 2026-07：新 Logo（宝石图标）替代旧 SVG
        // ISSUE-2 fix: 替换为新生成的宝石 Logo PNG，消除旧 {} 设计残留。
        // 尺寸提升至 128x128 增强首屏视觉冲击力。
        QIcon logoIcon(":/icons/minilang_icon_warm_256.png");
        QPixmap logoPixmap;
        if (!logoIcon.isNull()) {
            logoPixmap = logoIcon.pixmap(QSize(128, 128));
        }
        if (logoPixmap.isNull()) {
            // 回退到 QFluentKit 内置 CODE 图标
            logoPixmap = Fluent::icon(Fluent::IconType::CODE).pixmap(128, 128);
        }
        iconLabel->setPixmap(logoPixmap.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        iconLabel->setFixedSize(128, 128);
    }

    // 第十一轮：标题 24px Medium，副标题 14px 灰色
    // P2 视觉一致性：标题/副标题改用 QFluentKit TitleLabel / CaptionLabel，
    // 保留 setObjectName 以便 styles.qss / styles_dark.qss 的 QLabel#welcomeTitle
    // / QLabel#welcomeSubtitle 选择器仍生效（TitleLabel/CaptionLabel 继承自 QLabel）。
    auto* titleLabel = new TitleLabel(mlTr("MiniLang IDE"));
    titleLabel->setObjectName("welcomeTitle");
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(24);
    titleFont.setWeight(QFont::Medium);
    titleLabel->setFont(titleFont);
    // 字体设置后强制重置居中对齐（TitleLabel 可能因 font change 触发重布局覆盖 alignment）
    titleLabel->setAlignment(Qt::AlignCenter);
    // 兜底：通过 QSS qproperty-alignment 确保居中
    titleLabel->setStyleSheet(QStringLiteral("QLabel#welcomeTitle { qproperty-alignment: 'AlignCenter'; }"));

    auto* subtitleLabel = new CaptionLabel(mlTr("现代化 MiniLang 编程语言开发环境"));
    subtitleLabel->setObjectName("welcomeSubtitle");
    QFont subFont = subtitleLabel->font();
    subFont.setPointSize(14);
    subtitleLabel->setFont(subFont);
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setWordWrap(true);
    subtitleLabel->setStyleSheet(QStringLiteral("QLabel#welcomeSubtitle { qproperty-alignment: 'AlignCenter'; }"));

    centerLayout->addStretch(3);
    centerLayout->addWidget(iconLabel, 0, Qt::AlignCenter);
    centerLayout->addSpacing(8);
    centerLayout->addWidget(titleLabel, 0, Qt::AlignCenter);
    centerLayout->addSpacing(2);
    centerLayout->addWidget(subtitleLabel, 0, Qt::AlignCenter);
    centerLayout->addSpacing(24);

    // 第十一轮：按钮区域，最大宽度 400px，8px 圆角
    auto* btnContainer = new QWidget;
    btnContainer->setObjectName("welcomeBtnContainer");
    // ISSUE-3 fix: 确保 QSS background 在普通 QWidget 上生效
    btnContainer->setAttribute(Qt::WA_StyledBackground, true);
    auto* btnLayout = new QVBoxLayout(btnContainer);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(10);

    // Primary button: 打开文件夹 (Claude DS 风格 — terra-cotta 填充)
    // 使用原生 QPushButton + QSS 而非 QFluentKit PrimaryPushButton，
    // 因为 QFluentKit 自绘不读 QSS，无法应用 Claude DS 配色
    auto* primaryBtn = new QPushButton(mlTr("打开文件夹"), btnContainer);
    primaryBtn->setObjectName("welcomePrimaryBtn");
    primaryBtn->setMinimumHeight(38);
    primaryBtn->setMinimumWidth(220);
    primaryBtn->setCursor(Qt::PointingHandCursor);
    primaryBtn->setIcon(Fluent::icon(Fluent::IconType::FOLDER));
    primaryBtn->setIconSize(QSize(18, 18));
    connect(primaryBtn, &QPushButton::clicked, this, &Ide::onOpenFolder);
    btnLayout->addWidget(primaryBtn);

    // Secondary button: 新建文件 (白底 terra-cotta 边框)
    auto* secondaryBtn = new QPushButton(mlTr("新建文件"), btnContainer);
    secondaryBtn->setObjectName("welcomeSecondaryBtn");
    secondaryBtn->setMinimumHeight(38);
    secondaryBtn->setMinimumWidth(220);
    secondaryBtn->setCursor(Qt::PointingHandCursor);
    secondaryBtn->setIcon(Fluent::icon(Fluent::IconType::DOCUMENT));
    secondaryBtn->setIconSize(QSize(18, 18));
    connect(secondaryBtn, &QPushButton::clicked, this, [this]() {
        ensureEditorVisible();
        onNew();
    });
    btnLayout->addWidget(secondaryBtn);

    // 第十三轮：3 分钟 Hello World 引导按钮（新手入门入口）
    auto* tourBtn = new QPushButton(mlTr("🎬 3 分钟 Hello World"), btnContainer);
    tourBtn->setObjectName("welcomeTourBtn");
    tourBtn->setMinimumHeight(34);
    tourBtn->setMinimumWidth(220);
    tourBtn->setCursor(Qt::PointingHandCursor);
    tourBtn->setStyleSheet(QString("QPushButton { background: transparent; color: %1; border: 1px solid %1;"
                                   "  border-radius: 6px; padding: 6px 16px; font-size: 12px; }"
                                   "QPushButton:hover { background: %1; color: white; }")
                               .arg(TeachingTheme::primary().name()));
    connect(tourBtn, &QPushButton::clicked, this, [this]() {
        ensureEditorVisible();
        onNew();
        // 创建一个 Hello World 示例代码
        if (codeEditor_) {
            codeEditor_->setPlainText(QStringLiteral("print(\"Hello, World!\");\n"));
        }
        startGuidedTour();
    });
    btnLayout->addWidget(tourBtn);

    centerLayout->addWidget(btnContainer, 0, Qt::AlignCenter);

    centerLayout->addSpacing(16);

    // 第十三轮：辅助链接带 Fluent 图标（BOOK_SHELF + HELP）
    auto* shortcutLayout = new QHBoxLayout;
    shortcutLayout->setAlignment(Qt::AlignCenter);
    shortcutLayout->setSpacing(32);

    // 语法示例 — BOOK_SHELF 图标 + Fluent Blue
    auto* sampleContainer = new QWidget;
    sampleContainer->setStyleSheet("background:transparent;");
    auto* sampleHLayout = new QHBoxLayout(sampleContainer);
    sampleHLayout->setContentsMargins(0, 0, 0, 0);
    sampleHLayout->setSpacing(6);
    auto* sampleIcon = new QLabel;
    sampleIcon->setPixmap(Fluent::icon(Fluent::IconType::BOOK_SHELF).pixmap(16, 16));
    auto* sampleLink = new QLabel(
        QString::fromUtf8("<a href=\"sample\" "
                          "style=\"color:#0078D4;text-decoration:none;font-size:12px;\">\u8bed\u6cd5\u793a\u4f8b</a>"));
    sampleLink->setCursor(Qt::PointingHandCursor);
    connect(sampleLink, &QLabel::linkActivated, this, [this]() {
        QString sampleDir = QApplication::applicationDirPath() + "/../../samples/mini";
        if (!QDir(sampleDir).exists())
            sampleDir = QApplication::applicationDirPath() + "/samples/mini";
        if (QDir(sampleDir).exists())
            openWorkspace(sampleDir);
        else
            onOpenFolder();
    });
    sampleHLayout->addWidget(sampleIcon);
    sampleHLayout->addWidget(sampleLink);

    // 帮助文档 — HELP 图标 + Fluent Blue
    auto* helpContainer = new QWidget;
    helpContainer->setStyleSheet("background:transparent;");
    auto* helpHLayout = new QHBoxLayout(helpContainer);
    helpHLayout->setContentsMargins(0, 0, 0, 0);
    helpHLayout->setSpacing(6);
    auto* helpIcon = new QLabel;
    helpIcon->setPixmap(Fluent::icon(Fluent::IconType::HELP).pixmap(16, 16));
    auto* helpLink = new QLabel(QString::fromUtf8(
        "<a href=\"help\" style=\"color:#0078D4;text-decoration:none;font-size:12px;\">\u5e2e\u52a9\u6587\u6863</a>"));
    helpLink->setCursor(Qt::PointingHandCursor);
    connect(helpLink, &QLabel::linkActivated, this, [this]() { showHelpDialog(); });
    helpHLayout->addWidget(helpIcon);
    helpHLayout->addWidget(helpLink);

    shortcutLayout->addWidget(sampleContainer);
    shortcutLayout->addWidget(helpContainer);
    centerLayout->addLayout(shortcutLayout);

    centerLayout->addStretch(4);
    outerLayout->addWidget(centerArea, 1);

    // 第十一轮：安装拖拽 eventFilter
    welcomePage_->installEventFilter(this);
    centerArea->installEventFilter(this);

    loadRecentWorkspaces();
}

/// 从持久化设置加载最近工作区列表。
void Ide::loadRecentWorkspaces() {
    QSettings settings("MiniLang", "MiniLang IDE");
    recentWorkspaces_ = settings.value("recent/workspaces").toStringList();
    refreshRecentList();
}

/// 将目录加入最近工作区（去重并置顶）并持久化。
void Ide::addRecentWorkspace(const QString& dir) {
    if (dir.isEmpty())
        return;
    QString normalized = QDir(dir).absolutePath();
    recentWorkspaces_.removeAll(normalized);
    recentWorkspaces_.prepend(normalized);
    while (recentWorkspaces_.size() > 10)
        recentWorkspaces_.removeLast();
    QSettings settings("MiniLang", "MiniLang IDE");
    settings.setValue("recent/workspaces", recentWorkspaces_);
    refreshRecentList();
}

/// 刷新欢迎页/菜单中的最近工作区列表显示。
void Ide::refreshRecentList() {
    if (!recentListWidget_)
        return;
    recentListWidget_->clear();
    bool hasItems = false;
    for (const QString& ws : recentWorkspaces_) {
        if (!QDir(ws).exists())
            continue;
        QFileInfo fi(ws);
        auto* item = new QListWidgetItem(fi.fileName());
        item->setIcon(Fluent::icon(Fluent::IconType::FOLDER).pixmap(16, 16));
        item->setToolTip(ws);
        item->setData(Qt::UserRole, ws);
        recentListWidget_->addItem(item);
        hasItems = true;
    }
    // 第十一轮：列表为空时显示浅灰色提示
    if (!hasItems) {
        auto* emptyItem = new QListWidgetItem(mlTr("暂无最近打开的工作区"));
        emptyItem->setFlags(Qt::NoItemFlags);
        emptyItem->setData(Qt::ForegroundRole, QColor(180, 180, 180));
        recentListWidget_->addItem(emptyItem);
    }
}

// ============================================================
// Custom title bar (第九轮：VS Code 风格无边框标题栏)
// 32px 高，白色背景，左侧图标+标题+路径，右侧最小化/最大化/关闭
// ============================================================

/// 构建统一标题栏：菜单、工具栏按钮与窗口控制（最小/最大/关闭）。
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
    titleIconLabel_->setPixmap(QIcon(":/icons/minilang_icon_16.png").pixmap(QSize(16, 16)));
    titleIconLabel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(titleIconLabel_);

    titleTextLabel_ = new QLabel(mlTr("MiniLang IDE"), titleBar_);
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
    menuBtn->setToolTip(mlTr("菜单"));

    // 创建主菜单（File / Edit / View / Run / Help 子菜单）
    auto* mainMenu = new RoundMenu(QString(), titleBar_);

    // -- File --
    auto* fileMenu = new RoundMenu(mlTr("文件"), titleBar_);
    newAction_ = new QAction(mlTr("新建(&N)"), this);
    newAction_->setShortcut(QKeySequence::New);
    connect(newAction_, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        onNew();
    });
    fileMenu->addAction(newAction_);

    openAction_ = new QAction(mlTr("打开文件(&O)..."), this);
    openAction_->setShortcut(QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &Ide::onOpen);
    fileMenu->addAction(openAction_);

    openFolderAction_ = new QAction(mlTr("打开文件夹(&D)..."), this);
    connect(openFolderAction_, &QAction::triggered, this, &Ide::onOpenFolder);
    fileMenu->addAction(openFolderAction_);
    fileMenu->addSeparator();

    saveAction_ = new QAction(mlTr("保存(&S)"), this);
    saveAction_->setShortcut(QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &Ide::onSave);
    fileMenu->addAction(saveAction_);

    saveAsAction_ = new QAction(mlTr("另存为(&A)..."), this);
    saveAsAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_S);
    connect(saveAsAction_, &QAction::triggered, this, &Ide::onSaveAs);
    fileMenu->addAction(saveAsAction_);
    mainMenu->addMenu(fileMenu);

    // -- Edit --
    auto* editMenu = new RoundMenu(mlTr("编辑"), titleBar_);
    auto* formatMenuAct = new QAction(mlTr("格式化"), this);
    formatMenuAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_F);
    connect(formatMenuAct, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        onFormat();
    });
    editMenu->addAction(formatMenuAct);
    auto* findMenuAct = new QAction(mlTr("查找"), this);
    findMenuAct->setShortcut(QKeySequence::Find);
    connect(findMenuAct, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        onFind();
    });
    editMenu->addAction(findMenuAct);
    auto* replaceMenuAct = new QAction(mlTr("替换"), this);
    replaceMenuAct->setShortcut(QKeySequence::Replace);
    connect(replaceMenuAct, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        onReplace();
    });
    editMenu->addAction(replaceMenuAct);
    // H3: 跳转到行（Ctrl+G）
    auto* gotoLineAct = new QAction(mlTr("跳转到行..."), this);
    gotoLineAct->setShortcut(Qt::CTRL | Qt::Key_G);
    connect(gotoLineAct, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        if (!codeEditor_)
            return;
        bool ok = false;
        int maxLine = codeEditor_->blockCount();
        int line = QInputDialog::getInt(this, mlTr("跳转到行"), mlTr("输入行号 (1 - %1):").arg(maxLine),
                                        codeEditor_->textCursor().blockNumber() + 1, 1, maxLine, 1, &ok);
        if (ok && line > 0) {
            codeEditor_->gotoLine(line);
        }
    });
    editMenu->addAction(gotoLineAct);
    mainMenu->addMenu(editMenu);

    // -- View --
    auto* viewMenu = new RoundMenu(mlTr("视图"), titleBar_);
    viewExplorerAction_ = new QAction(mlTr("资源管理器"), this);
    viewExplorerAction_->setCheckable(true);
    connect(viewExplorerAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_)
            return;
        if (fileTreeDock_)
            fileTreeDock_->toggleView(on);
    });
    viewMenu->addAction(viewExplorerAction_);

    viewDebugAction_ = new QAction(mlTr("调试面板"), this);
    viewDebugAction_->setCheckable(true);
    connect(viewDebugAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_)
            return;
        if (debugPanelDock_)
            debugPanelDock_->toggleView(on);
    });
    viewMenu->addAction(viewDebugAction_);

    viewOutputAction_ = new QAction(mlTr("输出面板"), this);
    viewOutputAction_->setCheckable(true);
    connect(viewOutputAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_)
            return;
        if (on)
            showBottomPanel();
        else
            hideBottomPanel();
    });
    viewMenu->addAction(viewOutputAction_);

    viewCompileAnalysisAction_ = new QAction(mlTr("编译分析面板"), this);
    viewCompileAnalysisAction_->setCheckable(true);
    viewCompileAnalysisAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_V);
    connect(viewCompileAnalysisAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_)
            return;
        if (!rightDock_)
            return;
        if (on) {
            rightDock_->toggleView(true);
            rightDock_->setAsCurrentTab();
            if (rightPivot_)
                rightPivot_->setCurrentItem("token");
            if (codeEditor_)
                onCompileAnalysis();
        } else {
            rightDock_->toggleView(false);
        }
    });
    viewMenu->addAction(viewCompileAnalysisAction_);

    // ---- 学习中心入口（ActivityBar 「学习」对应的菜单项）----
    viewMenu->addSeparator();
    viewLearningHubAction_ = new QAction(mlTr("学习中心"), this);
    viewLearningHubAction_->setCheckable(true);
    viewLearningHubAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_L);
    connect(viewLearningHubAction_, &QAction::toggled, this, [this](bool on) {
        if (syncingViewAction_)
            return;
        if (on) {
            // 显示教学树导航 dock，并切到该 tab
            if (teachingTreeDock_ && teachingTreeDock_->isClosed())
                teachingTreeDock_->toggleView(true);
            if (teachingTreeDock_)
                teachingTreeDock_->setAsCurrentTab();
            // 同时切换 ActivityBar 到「学习」项
            if (activityBar_)
                activityBar_->setCurrentId("learn");
            // 若中央区当前在欢迎页或编辑器（非教学面板），同时打开「学习路径地图」面板，
            // 与 ActivityBar「学习」入口行为一致，避免用户只看到左侧树而误以为面板打不开。
            if (centerStack_ && (centerInEditorMode_ || centerStack_->currentWidget() == welcomePage_)) {
                showTeachingPanel(QStringLiteral("learning-path"));
            }
        } else {
            if (teachingTreeDock_ && !teachingTreeDock_->isClosed())
                teachingTreeDock_->toggleView(false);
        }
    });
    viewMenu->addAction(viewLearningHubAction_);
    viewMenu->addSeparator();

    // 教学面板的统一入口已收敛到「学习中心」（左侧 TeachingTreePanel 树形导航）。
    // 原 4 个教学面板子菜单（入门导览/编译前端/执行引擎/深入实战）已移除，
    // 避免视图菜单冗余选项造成新手认知负担。教学面板通过 TeachingTreePanel 切换。

    // 保留教学面板快捷键（不在菜单中显示，通过 QShortcut 注册）
    auto registerTeachingShortcut = [this](const QString& panelId, const QKeySequence& shortcut) {
        auto* sc = new QShortcut(shortcut, this);
        connect(sc, &QShortcut::activated, this, [this, panelId]() {
            // 切换逻辑：若当前已显示该面板则切回编辑器，否则显示该面板
            auto it = panelToStackIndex_.constFind(panelId);
            if (it != panelToStackIndex_.end() && centerStack_ && !centerInEditorMode_ &&
                centerStack_->currentIndex() == it.value()) {
                showEditorArea();
            } else {
                showTeachingPanel(panelId);
            }
        });
    };
    registerTeachingShortcut(QStringLiteral("pipeline"), Qt::CTRL | Qt::SHIFT | Qt::Key_P);
    registerTeachingShortcut(QStringLiteral("backend-compare"), Qt::CTRL | Qt::SHIFT | Qt::Key_B);
    registerTeachingShortcut(QStringLiteral("bug-hunt"), Qt::CTRL | Qt::SHIFT | Qt::Key_H);
    registerTeachingShortcut(QStringLiteral("syntax-explorer"), Qt::CTRL | Qt::SHIFT | Qt::Key_1);
    registerTeachingShortcut(QStringLiteral("lab-manual"), Qt::ALT | Qt::Key_5);
    registerTeachingShortcut(QStringLiteral("memory-model"), Qt::CTRL | Qt::SHIFT | Qt::Key_7);
    registerTeachingShortcut(QStringLiteral("ir-transform"), Qt::CTRL | Qt::SHIFT | Qt::Key_8);
    registerTeachingShortcut(QStringLiteral("profile-dashboard"), Qt::ALT | Qt::Key_4);
    registerTeachingShortcut(QStringLiteral("call-stack"), Qt::CTRL | Qt::SHIFT | Qt::Key_5);
    registerTeachingShortcut(QStringLiteral("variable-inspector"), Qt::CTRL | Qt::SHIFT | Qt::Key_6);
    registerTeachingShortcut(QStringLiteral("bytecode-trace"), Qt::CTRL | Qt::SHIFT | Qt::Key_4);
    registerTeachingShortcut(QStringLiteral("breakpoint-condition"), Qt::ALT | Qt::Key_1);
    registerTeachingShortcut(QStringLiteral("exception-flow"), Qt::ALT | Qt::Key_2);
    registerTeachingShortcut(QStringLiteral("closure-inspector"), Qt::ALT | Qt::Key_3);
    registerTeachingShortcut(QStringLiteral("token-puzzle"), Qt::CTRL | Qt::SHIFT | Qt::Key_2);
    registerTeachingShortcut(QStringLiteral("ast-toy"), Qt::CTRL | Qt::SHIFT | Qt::Key_3);
    registerTeachingShortcut(QStringLiteral("vm-sandbox"), Qt::CTRL | Qt::SHIFT | Qt::Key_9);
    registerTeachingShortcut(QStringLiteral("code-journey"), Qt::CTRL | Qt::SHIFT | Qt::Key_J);
    registerTeachingShortcut(QStringLiteral("glossary"), Qt::CTRL | Qt::SHIFT | Qt::Key_G);

    // ---- 字号调节（代码编辑器，全局应用于所有编辑器标签页） ----
    // 注意：viewMenu 是 QFluentKit RoundMenu（非 QMenu），QAction 的 setShortcut
    // 不会注册为窗口级全局快捷键。改用 QShortcut（与 registerTeachingShortcut 一致）
    // 确保快捷键在任何焦点上下文下生效，QAction 仅作菜单显示项。
    viewMenu->addSeparator();
    auto* fontIncreaseAction = new QAction(mlTr("放大字号"), this);
    connect(fontIncreaseAction, &QAction::triggered, this, [this]() {
        codeFontSize_ = qBound(8, codeFontSize_ + 1, 32);
        applyCodeFontSizeToAllEditors();
    });
    viewMenu->addAction(fontIncreaseAction);
    auto* fontIncSc = new QShortcut(Qt::CTRL | Qt::Key_Equal, this);
    QObject::connect(fontIncSc, &QShortcut::activated, this, [this]() {
        codeFontSize_ = qBound(8, codeFontSize_ + 1, 32);
        applyCodeFontSizeToAllEditors();
    });

    auto* fontDecreaseAction = new QAction(mlTr("缩小字号"), this);
    connect(fontDecreaseAction, &QAction::triggered, this, [this]() {
        codeFontSize_ = qBound(8, codeFontSize_ - 1, 32);
        applyCodeFontSizeToAllEditors();
    });
    viewMenu->addAction(fontDecreaseAction);
    auto* fontDecSc = new QShortcut(Qt::CTRL | Qt::Key_Minus, this);
    QObject::connect(fontDecSc, &QShortcut::activated, this, [this]() {
        codeFontSize_ = qBound(8, codeFontSize_ - 1, 32);
        applyCodeFontSizeToAllEditors();
    });

    auto* fontResetAction = new QAction(mlTr("重置字号"), this);
    connect(fontResetAction, &QAction::triggered, this, [this]() {
        codeFontSize_ = 11; // 重置到默认 11pt
        applyCodeFontSizeToAllEditors();
    });
    viewMenu->addAction(fontResetAction);
    auto* fontResetSc = new QShortcut(Qt::CTRL | Qt::Key_0, this);
    QObject::connect(fontResetSc, &QShortcut::activated, this, [this]() {
        codeFontSize_ = 11; // 重置到默认 11pt
        applyCodeFontSizeToAllEditors();
    });

    mainMenu->addMenu(viewMenu);

    // -- Run --
    auto* runMenu = new RoundMenu(mlTr("运行"), titleBar_);
    auto* runMenuAct = new QAction(mlTr("运行"), this);
    runMenuAct->setShortcut(Qt::Key_F5);
    connect(runMenuAct, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        onRun();
    });
    runMenu->addAction(runMenuAct);
    auto* debugMenuAct = new QAction(mlTr("调试"), this);
    debugMenuAct->setShortcut(Qt::Key_F6);
    connect(debugMenuAct, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        onDebug();
    });
    runMenu->addAction(debugMenuAct);
    runMenu->addSeparator();
    auto* stepInMenuAct = new QAction(mlTr("单步进入"), this);
    stepInMenuAct->setShortcut(Qt::Key_F11);
    connect(stepInMenuAct, &QAction::triggered, this, &Ide::onStepIn);
    runMenu->addAction(stepInMenuAct);
    auto* stepOverMenuAct = new QAction(mlTr("单步跳过"), this);
    stepOverMenuAct->setShortcut(Qt::Key_F10);
    connect(stepOverMenuAct, &QAction::triggered, this, &Ide::onStepOver);
    runMenu->addAction(stepOverMenuAct);
    auto* stepOutMenuAct = new QAction(mlTr("单步跳出"), this);
    stepOutMenuAct->setShortcut(Qt::SHIFT | Qt::Key_F11);
    connect(stepOutMenuAct, &QAction::triggered, this, &Ide::onStepOut);
    runMenu->addAction(stepOutMenuAct);
    auto* resumeMenuAct = new QAction(mlTr("继续"), this);
    resumeMenuAct->setShortcut(Qt::Key_F9);
    connect(resumeMenuAct, &QAction::triggered, this, &Ide::onResume);
    runMenu->addAction(resumeMenuAct);
    auto* stopMenuAct = new QAction(mlTr("停止"), this);
    stopMenuAct->setShortcut(Qt::SHIFT | Qt::Key_F5);
    connect(stopMenuAct, &QAction::triggered, this, &Ide::onStop);
    runMenu->addAction(stopMenuAct);
    mainMenu->addMenu(runMenu);

    // -- Help --
    auto* helpMenu = new RoundMenu(mlTr("帮助"), titleBar_);
    auto* samplesAct = new QAction(mlTr("语法示例"), this);
    connect(samplesAct, &QAction::triggered, this, [this]() {
        QString sampleDir = QApplication::applicationDirPath() + "/../../samples/mini";
        if (!QDir(sampleDir).exists())
            sampleDir = QApplication::applicationDirPath() + "/samples/mini";
        if (QDir(sampleDir).exists())
            openWorkspace(sampleDir);
        else
            onOpenFolder();
    });
    helpMenu->addAction(samplesAct);
    // 功能 1：再次显示欢迎向导（重置首次启动标记并弹出）
    auto* reshowWelcomeAct = new QAction(mlTr("再次显示欢迎向导"), this);
    connect(reshowWelcomeAct, &QAction::triggered, this, [this]() {
        auto* wizard = new WelcomeWizard(this);
        // Step 4 完成后自动展开 LearningPathPanel（与首次启动逻辑一致）
        connect(wizard, &WelcomeWizard::learningPathRequested, this,
                [this]() { showTeachingPanel(QStringLiteral("learning-path")); });
        wizard->exec();
        QSettings s;
        s.setValue(kWelcomeCompletedKey, true);
        wizard->deleteLater();
    });
    helpMenu->addAction(reshowWelcomeAct);
    auto* helpAct = new QAction(mlTr("帮助"), this);
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
    runAction_ = new QAction(mlTr("运行"), this);
    runAction_->setShortcut(Qt::Key_F5);
    connect(runAction_, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        onRun();
    });

    debugAction_ = new QAction(mlTr("调试"), this);
    debugAction_->setShortcut(Qt::Key_F6);
    connect(debugAction_, &QAction::triggered, this, [this]() {
        ensureEditorVisible();
        onDebug();
    });

    auto* runSplit = new PrimarySplitPushButton(mlTr("运行"), Fluent::IconType::PLAY_SOLID, titleBar_);
    runSplit->setToolTip(mlTr("运行程序 (F5)"));
    connect(runSplit, &PrimarySplitPushButton::clicked, runAction_, &QAction::trigger);
    auto* runFlyout = new RoundMenu(QString(), titleBar_);
    runFlyout->addAction(runAction_);
    runFlyout->addAction(debugAction_);
    runSplit->setFlyout(runFlyout);
    layout->addWidget(runSplit);

    // 编译按钮
    compileAnalysisAction_ = new QAction(mlTr("编译分析"), this);
    compileAnalysisAction_->setToolTip(mlTr("查看 Token / IR / 字节码"));

    // 格式化按钮
    formatAction_ = new QAction(mlTr("格式化"), this);
    formatAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_F);
    auto* formatBtn = new CleanToolButton(Fluent::IconType::BROOM, titleBar_);
    formatBtn->setToolTip(mlTr("格式化代码 (Ctrl+Shift+F)"));
    formatBtn->setDefaultAction(formatAction_);
    layout->addWidget(formatBtn);

    // AST 按钮
    astAction_ = new QAction(mlTr("AST"), this);
    astAction_->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_A);
    auto* astBtn = new CleanToolButton(Fluent::IconType::DICTIONARY, titleBar_);
    astBtn->setToolTip(mlTr("查看 AST 树形图 (Ctrl+Shift+A)"));
    astBtn->setDefaultAction(astAction_);
    layout->addWidget(astBtn);

    // ---- 调试按钮组（动态显隐）----
    debugSepAction_ = nullptr; // 无独立分隔线，用布局间距控制
    debugButtonContainer_ = new QWidget(titleBar_);
    debugButtonContainer_->setObjectName("debugButtonContainer");
    auto* dbgLayout = new QHBoxLayout(debugButtonContainer_);
    dbgLayout->setContentsMargins(0, 0, 0, 0);
    dbgLayout->setSpacing(2);

    auto makeDebugBtn = [this](QAction*& action, const QString& text, const QString& tip, const QKeySequence& shortcut,
                               Fluent::IconType icon) {
        action = new QAction(text, this);
        action->setToolTip(tip);
        action->setShortcut(shortcut);
        auto* btn = new CleanToolButton(icon, titleBar_);
        btn->setToolTip(tip);
        btn->setDefaultAction(action);
        return btn;
    };

    dbgLayout->addWidget(makeDebugBtn(stepInAction_, mlTr("步入"), mlTr("单步进入 (F11)"), Qt::Key_F11,
                                      Fluent::IconType::CHEVRON_RIGHT_MED));
    dbgLayout->addWidget(makeDebugBtn(stepOverAction_, mlTr("跨过"), mlTr("单步跳过 (F10)"), Qt::Key_F10,
                                      Fluent::IconType::CHEVRON_DOWN_MED));
    dbgLayout->addWidget(makeDebugBtn(stepOutAction_, mlTr("跨出"), mlTr("单步跳出 (Shift+F11)"),
                                      Qt::SHIFT | Qt::Key_F11, Fluent::IconType::CHEVRON_RIGHT));
    dbgLayout->addWidget(makeDebugBtn(resumeAction_, mlTr("继续"), mlTr("继续运行到下一个断点 (F9)"), Qt::Key_F9,
                                      Fluent::IconType::PLAY));
    dbgLayout->addWidget(makeDebugBtn(stopAction_, mlTr("停止"), mlTr("停止运行 (Shift+F5)"), Qt::SHIFT | Qt::Key_F5,
                                      Fluent::IconType::CANCEL));

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

    auto makeVmBtn = [this](QAction*& action, const QString& text, const QString& tip, const QKeySequence& shortcut,
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

    vmLayout->addWidget(makeVmBtn(vmStepAction_, mlTr("VM单步"), mlTr("VM 单步 (Ctrl+Shift+N)"),
                                  Qt::CTRL | Qt::SHIFT | Qt::Key_N, Fluent::IconType::CHEVRON_RIGHT_MED));
    vmLayout->addWidget(makeVmBtn(vmStepOverAction_, mlTr("VM跨过"), mlTr("VM 跨过 (Ctrl+Shift+O)"),
                                  Qt::CTRL | Qt::SHIFT | Qt::Key_O, Fluent::IconType::CHEVRON_DOWN_MED));
    vmLayout->addWidget(makeVmBtn(vmStepOutAction_, mlTr("VM跨出"), mlTr("VM 跨出 (Ctrl+Shift+U)"),
                                  Qt::CTRL | Qt::SHIFT | Qt::Key_U, Fluent::IconType::CHEVRON_RIGHT));
    vmLayout->addWidget(makeVmBtn(vmRunAction_, mlTr("VM运行"), mlTr("VM 运行 (Ctrl+Shift+R)"),
                                  Qt::CTRL | Qt::SHIFT | Qt::Key_R, Fluent::IconType::PLAY));
    vmLayout->addWidget(
        makeVmBtn(vmStopAction_, mlTr("VM停止"), mlTr("停止 VM"), QKeySequence(), Fluent::IconType::CANCEL));

    layout->addWidget(vmButtonContainer_);
    vmButtonContainer_->hide();

    clearAction_ = new QAction(mlTr("清空"), this);
    clearAction_->setToolTip(mlTr("清空输出面板"));

    // 弹性空间
    layout->addStretch(1);

    // ---- 执行引擎切换 ComboBox ----
    engineCombo_ = new ComboBox(titleBar_);
    engineCombo_->setObjectName("engineCombo");
    engineCombo_->addItem(mlTr("树遍历解释器"));
    engineCombo_->addItem(mlTr("栈式 VM"));
    engineCombo_->addItem(mlTr("寄存器式 VM"));
    engineCombo_->setCurrentIndex(0);
    engineCombo_->setToolTip(mlTr("切换执行引擎"));
    layout->addWidget(engineCombo_);

    // 深色主题已移除：不再创建 themeToggleBtn_ 切换按钮
    // 用户如需自定义主题色，仍可在 QFluentKit 设置中切换 ThemeColor

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

    titleMinBtn_ = makeWinBtn(Fluent::IconType::MINIMIZE, mlTr("最小化"));
    titleMinBtn_->setObjectName("titleMinBtn");
    titleMaxBtn_ = makeWinBtn(Fluent::IconType::FULL_SCREEN, mlTr("最大化"));
    titleMaxBtn_->setObjectName("titleMaxBtn");
    titleCloseBtn_ = makeWinBtn(Fluent::IconType::CLOSE, mlTr("关闭"));
    titleCloseBtn_->setObjectName("titleCloseBtn");

    connect(titleMinBtn_, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(titleMaxBtn_, &QToolButton::clicked, this, [this]() {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
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

/// 构建主界面骨架：菜单栏、活动栏、编辑器/底部/右侧面板与 dock 布局。
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

    // 文件树搜索过滤框
    fileTreeFilterEdit_ = new QLineEdit;
    fileTreeFilterEdit_->setObjectName("fileTreeFilter");
    fileTreeFilterEdit_->setPlaceholderText(mlTr("搜索文件..."));
    fileTreeFilterEdit_->setClearButtonEnabled(true);

    // Debug panel
    debugPanel_ = new DebugPanel;
    // M5: 选中调用栈帧时跳转到对应源码行
    connect(debugPanel_, &DebugPanel::gotoLineRequested, this, [this](int line) {
        if (codeEditor_)
            codeEditor_->gotoLine(line);
    });

    // Output text edit
    // R15-3: VSCode 风格输出面板——等宽字体 + 白色背景（R74: 回退中性白）
    outputTextEdit_ = new QTextEdit;
    outputTextEdit_->setReadOnly(true);
    outputTextEdit_->setFont(GuiTextUtils::monospaceFont(10));
    outputTextEdit_->document()->setMaximumBlockCount(10000);
    outputTextEdit_->setObjectName("outputEdit");
    // R15-7: 统一背景色为白色，避免与周边面板色差割裂（R74: 回退中性白）
    QPalette outPal = outputTextEdit_->palette();
    outPal.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF)); // 白色背景
    outPal.setColor(QPalette::Text, QColor(0x1E, 0x1E, 0x1E)); // 深灰文本
    outputTextEdit_->setPalette(outPal);
    outputTextEdit_->setAutoFillBackground(true);

    // Error list
    errorListWidget_ = new QListWidget;
    errorListWidget_->setObjectName("errorList");
    errorListWidget_->setFont(GuiTextUtils::monospaceFont(10));
    errorListWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    errorListWidget_->setSpacing(0);
    errorListWidget_->setItemDelegate(new RichTextItemDelegate(errorListWidget_));
    connect(errorListWidget_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item)
            return;
        bool ok = false;
        int line = item->data(Qt::UserRole).toInt(&ok);
        if (ok && line > 0 && codeEditor_)
            codeEditor_->gotoLine(line);
    });

    // 错误列表类型过滤栏（错误/警告/信息/提示 4 个 toggle 按钮）
    auto makeFilterBtn = [this](const QString& text, const QColor& color) {
        auto* btn = new QToolButton(this);
        btn->setText(text);
        btn->setCheckable(true);
        btn->setChecked(true);
        btn->setAutoRaise(true);
        btn->setStyleSheet(QString("QToolButton { padding: 2px 6px; font-size: 11px; border: none; }"
                                   "QToolButton:checked { background: %1; color: white; }")
                               .arg(color.name()));
        return btn;
    };
    errFilterErrorBtn_ = makeFilterBtn(mlTr("\u25CF 错误"), TeachingTheme::error());
    errFilterWarningBtn_ = makeFilterBtn(mlTr("\u25D0 警告"), TeachingTheme::warning());
    errFilterInfoBtn_ = makeFilterBtn(mlTr("\u25CB 信息"), TeachingTheme::info());
    errFilterHintBtn_ = makeFilterBtn(mlTr("\u25C7 提示"), TeachingTheme::hint());

    // REPL panel
    replPanel_ = new ReplPanel;
    replPanel_->setObjectName("replPanel");

    // Token table (第八轮：列顺序 行号、列号、类型、词素、字面量)
    tokenTable_ = new QTableWidget;
    tokenTable_->setColumnCount(5);
    tokenTable_->setHorizontalHeaderLabels({mlTr("行号"), mlTr("列号"), mlTr("类型"), mlTr("词素"), mlTr("字面量")});
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
    connect(bytecodeList_, &QListWidget::currentRowChanged, this, &Ide::onBytecodeRowClicked);
    vmStackPanel_ = new VmStackPanel;
    vmStackPanel_->setMinimumWidth(180);

    // AST viewer (lives in an independent top-level window)
    // IDE-AST-01 fix: 传入 this 作为初始 parent，防止 astWindow_ 创建之前的异常路径
    // 导致 astViewer_ 孤儿泄漏。后续 setParent(astWindow_) 会自动 reparent。
    astViewer_ = new AstViewer(this);

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

    // Central stack: welcome page + teaching panels（editorTabWidget_ 移至 centerSplitter_ 独立显示）
    centerStack_ = new QStackedWidget;
    centerStack_->setObjectName("centerStack");
    centerStack_->addWidget(welcomePage_); // index 0: 欢迎页

    // 任务2：三栏布局 splitter — [centerStack_ (教学/欢迎) | editorSplitter_ (编辑器+底部面板)]
    // 教学面板打开时与编辑器并排显示，形成「学习树 | 教学面板 | 代码编辑区」三栏
    // 底部面板（输出/问题/REPL）位于 editorSplitter_ 内部，仅覆盖代码编辑区（类似 VS Code 集成终端）
    centerSplitter_ = new QSplitter(Qt::Horizontal);
    centerSplitter_->setObjectName("centerSplitter");
    centerSplitter_->addWidget(centerStack_);

    // ---- Bottom panel container: Pivot + QStackedWidget ----
    // 第十一轮：默认高度 600px，最小 200px；标签栏压缩到 28px，紧凑左对齐
    bottomContainer_ = new QWidget;
    bottomContainer_->setObjectName("bottomPanelContainer");
    bottomContainer_->setMinimumHeight(200);
    auto* bottomLayout = new QVBoxLayout(bottomContainer_);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(0);

    // 第十一轮：Pivot 标签栏行（标签左对齐 + 右侧关闭按钮）
    auto* pivotRow = new QHBoxLayout;
    pivotRow->setContentsMargins(8, 0, 4, 0);
    pivotRow->setSpacing(0);

    bottomPivot_ = new Pivot(bottomContainer_);
    bottomPivot_->setObjectName("bottomPivot");
    // 第十一轮：标签紧凑化 — 12px 字号，28px 行高，主题蓝下划线
    bottomPivot_->setItemFontSize(12);
    bottomPivot_->setIndicatorColor(QColor("#0078d4"), QColor("#0078d4"));
    bottomPivot_->setFixedHeight(28);
    bottomPivot_->addItem("output", mlTr("输出"));
    bottomPivot_->addItem("errors", mlTr("问题"));
    bottomPivot_->addItem("repl", mlTr("REPL"));
    pivotRow->addWidget(bottomPivot_);

    pivotRow->addStretch();

    // 第十一轮：右侧关闭按钮（紧凑圆形，与 VS Code 一致）
    auto* bottomCloseBtn = new QToolButton(bottomContainer_);
    bottomCloseBtn->setObjectName("panelCloseBtn");
    bottomCloseBtn->setText(mlTr("×"));
    bottomCloseBtn->setFixedSize(20, 20);
    bottomCloseBtn->setToolTip(mlTr("关闭面板"));
    bottomCloseBtn->setCursor(Qt::PointingHandCursor);
    connect(bottomCloseBtn, &QToolButton::clicked, this, [this]() { hideBottomPanel(); });
    pivotRow->addWidget(bottomCloseBtn);

    auto* pivotRowWidget = new QWidget;
    pivotRowWidget->setObjectName("bottomPivotRow");
    auto* pivotRowLayout = new QVBoxLayout(pivotRowWidget);
    pivotRowLayout->setContentsMargins(0, 0, 0, 0);
    pivotRowLayout->setSpacing(0);
    pivotRowLayout->addLayout(pivotRow);
    bottomLayout->addWidget(pivotRowWidget);

    bottomStack_ = new QStackedWidget(bottomContainer_);
    bottomStack_->addWidget(outputTextEdit_);
    // 错误页：过滤栏 + 错误列表（包裹在容器内）
    auto* errorPageContainer = new QWidget;
    errorPageContainer->setObjectName("errorPageContainer");
    auto* errorPageLayout = new QVBoxLayout(errorPageContainer);
    errorPageLayout->setContentsMargins(0, 0, 0, 0);
    errorPageLayout->setSpacing(0);
    auto* errFilterLayout = new QHBoxLayout;
    errFilterLayout->setContentsMargins(4, 2, 4, 2);
    errFilterLayout->setSpacing(4);
    if (errFilterErrorBtn_)
        errFilterLayout->addWidget(errFilterErrorBtn_);
    if (errFilterWarningBtn_)
        errFilterLayout->addWidget(errFilterWarningBtn_);
    if (errFilterInfoBtn_)
        errFilterLayout->addWidget(errFilterInfoBtn_);
    if (errFilterHintBtn_)
        errFilterLayout->addWidget(errFilterHintBtn_);
    errFilterLayout->addStretch();
    errorPageLayout->addLayout(errFilterLayout);
    errorPageLayout->addWidget(errorListWidget_, 1);
    bottomStack_->addWidget(errorPageContainer);
    bottomStack_->addWidget(replPanel_);
    bottomLayout->addWidget(bottomStack_, 1);
    connect(bottomPivot_, &Pivot::currentItemChanged, this, &Ide::onBottomPivotChanged);

    // editorSplitter_：纵向分割 [editorTabWidget_ | bottomContainer_]
    editorSplitter_ = new QSplitter(Qt::Vertical);
    editorSplitter_->setObjectName("editorSplitter");
    editorSplitter_->addWidget(editorTabWidget_);
    editorSplitter_->addWidget(bottomContainer_);
    editorSplitter_->setStretchFactor(0, 3); // 编辑器占更多空间
    editorSplitter_->setStretchFactor(1, 1); // 底部面板默认高度

    centerSplitter_->addWidget(editorSplitter_);
    centerSplitter_->setStretchFactor(0, 1); // 教学面板/欢迎页占比
    centerSplitter_->setStretchFactor(1, 2); // 编辑器区域占比（更大）
    centerSplitter_->setSizes({420, 780});
    // 启动时无编辑器标签，隐藏编辑器栏（仅显示欢迎页）
    editorTabWidget_->hide();

    // ---- Activity bar (left-most 48px) ----
    // P0.5 注册制重构：每个活动项有唯一字符串 ID，新增面板只需追加一行
    activityBar_ = new ActivityBar;
    activityBar_->addItem("explorer", mlTr("资源管理器"), Fluent::IconType::FOLDER);
    activityBar_->addItem("debug", mlTr("调试"), Fluent::IconType::DEVELOPER_TOOLS);
    // 学习中心入口：点击展开左侧教学树面板（P2-1 fix: 替代原 LearningHubDialog 弹窗）
    activityBar_->addItem("learn", mlTr("学习"), Fluent::IconType::EDUCATION);
    // 仅连接 id-based 信号，避免 index+id 双重分发
    connect(activityBar_, &ActivityBar::currentChangedById, this, &Ide::onActivityChangedById);

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
    rightPivot_->addItem("token", mlTr("词法Token"));
    rightPivot_->addItem("ir", mlTr("中间IR"));
    rightPivot_->addItem("bytecode", mlTr("字节码"));
    rightPivotRow->addWidget(rightPivot_);

    rightPivotRow->addStretch();

    // 第十一轮：右侧关闭按钮
    auto* rightCloseBtn = new QToolButton(rightContainer);
    rightCloseBtn->setObjectName("panelCloseBtn");
    rightCloseBtn->setText(mlTr("×"));
    rightCloseBtn->setFixedSize(20, 20);
    rightCloseBtn->setToolTip(mlTr("关闭面板"));
    rightCloseBtn->setCursor(Qt::PointingHandCursor);
    connect(rightCloseBtn, &QToolButton::clicked, this, [this]() { hideRightPanel(); });
    rightPivotRow->addWidget(rightCloseBtn);

    auto* rightPivotRowWidget = new QWidget;
    rightPivotRowWidget->setObjectName("rightPivotRow");
    auto* rightPivotRowLayout = new QVBoxLayout(rightPivotRowWidget);
    rightPivotRowLayout->setContentsMargins(0, 0, 0, 0);
    rightPivotRowLayout->setSpacing(0);
    rightPivotRowLayout->addLayout(rightPivotRow);
    rightLayout->addWidget(rightPivotRowWidget);

    rightStack_ = new QStackedWidget(rightContainer);
    rightStack_->setObjectName("rightStack");
    rightStack_->addWidget(tokenTable_); // index 0 → token
    rightStack_->addWidget(irViewer_);   // index 1 → IR

    // Bytecode page: bytecode list + VM stack panel in a splitter
    // 第八轮：普通查看模式仅展示纯字节码，VM 调试状态才追加操作数栈/全局变量
    auto* bytecodePage = new QWidget;
    bytecodePage->setObjectName("bytecodePage");
    auto* bcLayout = new QHBoxLayout(bytecodePage);
    bcLayout->setContentsMargins(0, 0, 0, 0);
    bcLayout->setSpacing(2);
    auto* bcSplitter = new QSplitter(Qt::Horizontal, bytecodePage);
    bcSplitter->setObjectName("bytecodeSplitter");
    bcSplitter->addWidget(bytecodeList_);
    bcSplitter->addWidget(vmStackPanel_);
    bcSplitter->setStretchFactor(0, 3);
    bcSplitter->setStretchFactor(1, 2);
    bcSplitter->setHandleWidth(3);
    bcLayout->addWidget(bcSplitter);
    rightStack_->addWidget(bytecodePage); // index 2 → bytecode
    // 普通模式隐藏 VM 调试面板（仅 VM 单步调试时显示）
    vmStackPanel_->hide();

    rightLayout->addWidget(rightStack_, 1);
    connect(rightPivot_, &Pivot::currentItemChanged, this, &Ide::onRightPivotChanged);

    // ---- AST independent window ----
    // BUG-LEAK-01 fix: 原 new QWidget(nullptr) 无 Qt parent 所有权，析构时泄漏。
    // 传入 this 作为 parent，Qt 会在 Ide 析构时自动 delete astWindow_。
    // Qt::Window flag 仍保持其为独立顶层窗口（非嵌入子控件）。
    astWindow_ = new QWidget(this);
    astWindow_->setWindowTitle(mlTr("AST 树形图"));
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
    // R68 根因修复（修正版）：移除 DisableStylesheet，改用 setColorSchemeMode(Light)。
    //
    // 真正根因：applyFluentStyle() 中 setPalette(pal) 向 dockManager_ 传播
    // QEvent::ApplicationPaletteChange 事件，ADS eventFilter 检测到
    // ColorSchemeMode==FollowPalette（默认值）→ 调用 loadStylesheet() →
    // setStyleSheet(default.css) 完全覆盖自定义 adsQss。
    //
    // 初版修复用 DisableStylesheet 太激进——它连构造时的初始默认样式都不加载，
    // 导致 ADS 标题栏按钮图标（关闭/浮动/标签菜单）丢失、QToolTip 回退到 Windows 11
    // 黑色样式。正确方案：保留默认配置（初始 default.css 正常加载提供基础图标/按钮
    // 样式），创建后立即调用 setColorSchemeMode(Light) 将 ColorSchemeMode 从默认的
    // FollowPalette 改为 Light。这样 eventFilter 检测到 ColorSchemeMode!=FollowPalette
    // 时跳过 loadStylesheet，自定义 adsQss 通过 setStyleSheet 覆盖后不会被重载。
    ads::CDockManager::setConfigFlags(ads::CDockManager::DefaultOpaqueConfig |
                                      ads::CDockManager::MiddleMouseButtonClosesTab);
    ads::CDockManager::setAutoHideConfigFlags(ads::CDockManager::DefaultAutoHideConfig);

    dockManager_ = new ads::CDockManager(middleArea);
    middleLayout->addWidget(dockManager_, 1);

    // R73 fix: 连接 dockWidgetAdded 信号，对运行时（含 restoreState）创建的
    // 新 dock widget 自动应用样式。restoreState 创建的新 tab 不会被此前的
    // applyFluentStyle 样式化，导致蓝底问题。通过此信号在 applyFluentStyle
    // 中对 findChildren<CDockWidgetTab*>() 设置 palette。
    // PERF: 使用 scheduleApplyStyle 防抖合并，避免 restoreState 期间每个 dock
    // 添加都触发一次全量样式重算。
    connect(dockManager_, &ads::CDockManager::dockWidgetAdded, this,
            [this](ads::CDockWidget*) { scheduleApplyStyle(); });

    // R68 关键修复：锁定 ColorSchemeMode 为 Light，阻止 palette 变化触发 loadStylesheet。
    // 默认 FollowPalette 模式下，setPalette 触发 ApplicationPaletteChange 事件 →
    // eventFilter 调用 loadStylesheet() 覆盖自定义 QSS。设为 Light 后，eventFilter
    // 条件 (ColorSchemeMode == FollowPalette) 不满足，不会重载样式。
    dockManager_->setColorSchemeMode(ads::CDockManager::ColorSchemeMode::Light);

    mainLayout->addWidget(middleArea, 1);
    // 底部面板已移至 editorSplitter_ 内部（仅覆盖代码编辑区，类似 VS Code 集成终端）
    // 不再放在 mainLayout 中，避免横跨全宽覆盖教学面板
    setCentralWidget(mainContainer);

    // Central dock widget (editor area) — must be set FIRST
    auto* centralDock = dockManager_->createDockWidget("Editor");
    centralDock->setWidget(centerSplitter_, ads::CDockWidget::ForceNoScrollArea);
    centralDock->setFeature(ads::CDockWidget::NoTab, true);
    dockManager_->setCentralWidget(centralDock);

    // Left panel: file tree (包裹过滤框 + 树)
    // R73 fix: createDockWidget 的 title 同时用作 objectName 和 windowTitle。
    // objectName 必须固定（ADS restoreState 通过 objectName 匹配），所以传固定ID。
    // 然后 setWindowTitle 改回 i18n 标题用于显示（CDockWidget 的 WindowTitleChange
    // 事件会自动更新 tab 文本）。此前用 mlTr() 作为 title 导致 locale 变化时
    // restoreState 静默失败；而创建后 setObjectName 会导致 DockWidgetsMap 的
    // key 与 restoreState 保存的旧名称不匹配，dock 变成浮动窗口。
    fileTreeDock_ = dockManager_->createDockWidget("fileTreeDock");
    fileTreeDock_->setWindowTitle(mlTr("资源管理器"));
    auto* fileTreeContainer = new QWidget;
    fileTreeContainer->setObjectName("fileTreeContainer");
    auto* fileTreeLayout = new QVBoxLayout(fileTreeContainer);
    fileTreeLayout->setContentsMargins(0, 0, 0, 0);
    fileTreeLayout->setSpacing(0);
    if (fileTreeFilterEdit_)
        fileTreeLayout->addWidget(fileTreeFilterEdit_);
    fileTreeLayout->addWidget(fileTree_, 1);
    fileTreeDock_->setWidget(fileTreeContainer, ads::CDockWidget::ForceNoScrollArea);
    dockManager_->addDockWidget(ads::LeftDockWidgetArea, fileTreeDock_);

    // Left panel: debug (tabbed with file tree)
    debugPanelDock_ = dockManager_->createDockWidget("debugPanelDock");
    debugPanelDock_->setWindowTitle(mlTr("调试"));
    debugPanelDock_->setWidget(debugPanel_, ads::CDockWidget::ForceNoScrollArea);
    dockManager_->addDockWidgetTabToArea(debugPanelDock_, fileTreeDock_->dockAreaWidget());

    // Bottom panel: 已移至 editorSplitter_ 内部（仅覆盖代码编辑区），不再需要 ADS dock

    // Right panel: single dock containing Pivot + stack (token/IR/bytecode)
    rightDock_ = dockManager_->createDockWidget("rightDock");
    rightDock_->setWindowTitle(mlTr("编译分析"));
    rightDock_->setWidget(rightContainer, ads::CDockWidget::ForceNoScrollArea);
    dockManager_->addDockWidget(ads::RightDockWidgetArea, rightDock_);
    // 第十二轮：所有面板支持完整拖拽重组、浮动、标签分组（移除旧的浮动/移动锁定）

    // ---- 教学增强面板（迁移至 centerStack_，由左侧 TeachingTreePanel 切换）----
    // 第十四轮重构：原独立 dock 改为 centerStack_ 子页，点击教学树叶子节点切换。
    // 启动性能优化：教学面板采用懒加载——启动时仅注册工厂函数，首次访问时才构造。
    // 避免启动时全量构造 20 个面板（含子 widget 树、信号连接、SyntaxHighlighter 等）。
    auto registerLazyPanel = [this](const QString& panelId, const QString& title, std::function<QWidget*()> factory) {
        teachingPanelFactories_[panelId] = [this, panelId, title, factory]() {
            if (panelToStackIndex_.contains(panelId))
                return; // 已构造
            QWidget* panel = factory();
            QWidget* wrapped = wrapTeachingPanel(panelId, title, panel);
            int idx = centerStack_->addWidget(wrapped);
            panelToStackIndex_[panelId] = idx;
        };
    };

    // IR/字节码面板点击 → 高亮编辑器对应源码行（共享 lambda）
    auto highlightSourceLine = [this](int line) {
        if (codeEditor_)
            codeEditor_->highlightSourceLine(line);
    };

    // 第一波 + 第三波
    registerLazyPanel(QStringLiteral("pipeline"), mlTr("编译管线可视化"), [this, highlightSourceLine]() {
        pipelineViewer_ = new PipelineViewer(this);
        pipelineViewer_->setController(controller_);
        connect(pipelineViewer_, &PipelineViewer::sourceLineRequested, this, highlightSourceLine);
        return pipelineViewer_;
    });
    registerLazyPanel(QStringLiteral("backend-compare"), mlTr("三后端对比"), [this]() {
        backendComparePanel_ = new BackendComparePanel(this);
        backendComparePanel_->setController(controller_);
        return backendComparePanel_;
    });
    registerLazyPanel(QStringLiteral("bug-hunt"), mlTr("Bug 狩猎"), [this]() {
        bugHuntPanel_ = new BugHuntPanel(this);
        bugHuntPanel_->setController(controller_);
        connect(bugHuntPanel_, &BugHuntPanel::loadSampleRequested, this, &Ide::loadCodeIntoMainEditor);
        connect(bugHuntPanel_, &BugHuntPanel::challengeSolved, this, [this](int difficulty) {
            if (!learningPathPanel_)
                return;
            QString activityId;
            switch (difficulty) {
            case 0:
                activityId = QStringLiteral("bug-hunt-beginner");
                break;
            case 1:
                activityId = QStringLiteral("bug-hunt-intermediate");
                break;
            default:
                activityId = QStringLiteral("bug-hunt-expert");
                break;
            }
            learningPathPanel_->markActivityCompleted(activityId);
        });
        return bugHuntPanel_;
    });
    registerLazyPanel(QStringLiteral("syntax-explorer"), mlTr("语法探索器"), [this]() {
        syntaxExplorerPanel_ = new SyntaxExplorerPanel(this);
        syntaxExplorerPanel_->setController(controller_);
        connect(syntaxExplorerPanel_, &SyntaxExplorerPanel::loadSampleRequested, this, &Ide::loadCodeIntoMainEditor);
        return syntaxExplorerPanel_;
    });
    registerLazyPanel(QStringLiteral("lab-manual"), mlTr("实验手册"), [this]() {
        labManualPanel_ = new LabManualPanel(this);
        labManualPanel_->setController(controller_);
        connect(labManualPanel_, &LabManualPanel::loadSampleRequested, this, &Ide::loadCodeIntoMainEditor);
        connect(labManualPanel_, &LabManualPanel::runSampleRequested, this, [this](const QString& code) {
            loadCodeIntoMainEditor(code);
            onRun();
        });
        connect(labManualPanel_, &LabManualPanel::jumpToPanelRequested, this, &Ide::onJumpToPanel);
        connect(labManualPanel_, &LabManualPanel::exerciseCompleted, this, [this](const QString& chapterId) {
            if (learningPathPanel_) {
                learningPathPanel_->markActivityCompleted(chapterId);
            }
        });
        return labManualPanel_;
    });

    // 第二波
    registerLazyPanel(QStringLiteral("memory-model"), mlTr("内存模型"), [this]() {
        memoryModelPanel_ = new MemoryModelPanel(this);
        memoryModelPanel_->setController(controller_);
        return memoryModelPanel_;
    });
    registerLazyPanel(QStringLiteral("ir-transform"), mlTr("IR 变换"), [this, highlightSourceLine]() {
        irTransformPanel_ = new IRTransformPanel(this);
        irTransformPanel_->setController(controller_);
        connect(irTransformPanel_, &IRTransformPanel::sourceLineRequested, this, highlightSourceLine);
        return irTransformPanel_;
    });
    registerLazyPanel(QStringLiteral("profile-dashboard"), mlTr("性能剖析"), [this]() {
        profileDashboardPanel_ = new ProfileDashboardPanel(this);
        profileDashboardPanel_->setController(controller_);
        return profileDashboardPanel_;
    });

    // 第三波
    registerLazyPanel(QStringLiteral("call-stack"), mlTr("调用栈"), [this]() {
        callStackPanel_ = new CallStackPanel(this);
        callStackPanel_->setController(controller_);
        connect(callStackPanel_, &CallStackPanel::loadSampleRequested, this, &Ide::loadCodeIntoMainEditor);
        return callStackPanel_;
    });
    registerLazyPanel(QStringLiteral("variable-inspector"), mlTr("变量检查器"), [this]() {
        variableInspectorPanel_ = new VariableInspectorPanel(this);
        variableInspectorPanel_->setController(controller_);
        connect(variableInspectorPanel_, &VariableInspectorPanel::loadSampleRequested, this,
                &Ide::loadCodeIntoMainEditor);
        return variableInspectorPanel_;
    });
    registerLazyPanel(QStringLiteral("bytecode-trace"), mlTr("字节码轨迹"), [this, highlightSourceLine]() {
        bytecodeTracePanel_ = new BytecodeTracePanel(this);
        bytecodeTracePanel_->setController(controller_);
        connect(bytecodeTracePanel_, &BytecodeTracePanel::loadSampleRequested, this, &Ide::loadCodeIntoMainEditor);
        connect(bytecodeTracePanel_, &BytecodeTracePanel::sourceLineRequested, this, highlightSourceLine);
        return bytecodeTracePanel_;
    });
    registerLazyPanel(QStringLiteral("breakpoint-condition"), mlTr("条件断点"), [this]() {
        breakpointConditionPanel_ = new BreakpointConditionPanel(this);
        breakpointConditionPanel_->setController(controller_);
        connect(breakpointConditionPanel_, &BreakpointConditionPanel::loadSampleRequested, this,
                &Ide::loadCodeIntoMainEditor);
        return breakpointConditionPanel_;
    });
    registerLazyPanel(QStringLiteral("exception-flow"), mlTr("异常流"), [this]() {
        exceptionFlowPanel_ = new ExceptionFlowPanel(this);
        connect(exceptionFlowPanel_, &ExceptionFlowPanel::loadSampleRequested, this, &Ide::loadCodeIntoMainEditor);
        return exceptionFlowPanel_;
    });
    registerLazyPanel(QStringLiteral("closure-inspector"), mlTr("闭包检查器"), [this]() {
        closureInspectorPanel_ = new ClosureInspectorPanel(this);
        connect(closureInspectorPanel_, &ClosureInspectorPanel::loadSampleRequested, this,
                &Ide::loadCodeIntoMainEditor);
        return closureInspectorPanel_;
    });

    // 第四档（功能 1-6）
    registerLazyPanel(QStringLiteral("learning-path"), mlTr("学习路径地图"), [this]() {
        learningPathPanel_ = new LearningPathPanel(this);
        connect(learningPathPanel_, &LearningPathPanel::activityRequested, this, &Ide::onActivityRequested);
        return learningPathPanel_;
    });
    registerLazyPanel(QStringLiteral("token-puzzle"), mlTr("Token 拼图"), [this]() {
        tokenPuzzlePanel_ = new TokenPuzzlePanel(this);
        connect(tokenPuzzlePanel_, &TokenPuzzlePanel::activityCompleted, this, [this](const QString& /*levelId*/) {
            if (!learningPathPanel_)
                return;
            if (LearnerProgressStore::instance().areAllLevelsCompleted(
                    {"token-puzzle-1", "token-puzzle-2", "token-puzzle-3", "token-puzzle-4", "token-puzzle-5"})) {
                learningPathPanel_->markActivityCompleted(QStringLiteral("token-puzzle"));
            }
        });
        return tokenPuzzlePanel_;
    });
    registerLazyPanel(QStringLiteral("ast-toy"), mlTr("AST 构建器"), [this]() {
        astBuilderToyPanel_ = new AstBuilderToyPanel(this);
        connect(astBuilderToyPanel_, &AstBuilderToyPanel::activityCompleted, this, [this](const QString& /*levelId*/) {
            if (!learningPathPanel_)
                return;
            if (LearnerProgressStore::instance().areAllLevelsCompleted({"ast-toy-level-1", "ast-toy-level-2",
                                                                        "ast-toy-level-3", "ast-toy-level-4",
                                                                        "ast-toy-level-5", "ast-toy-level-6"})) {
                learningPathPanel_->markActivityCompleted(QStringLiteral("ast-toy"));
                // AUDIT-P1 fix: op-priority-challenge 路由到 ast-toy 面板，
                // ast-toy 全部完成时同时标记 op-priority-challenge 完成。
                learningPathPanel_->markActivityCompleted(QStringLiteral("op-priority-challenge"));
            }
        });
        return astBuilderToyPanel_;
    });
    registerLazyPanel(QStringLiteral("vm-sandbox"), mlTr("VM 栈沙盒"), [this]() {
        vmStackSandboxPanel_ = new VmStackSandboxPanel(this);
        // 第二十七轮：绑定 IdeController，启用「真实字节码追踪」子页的真实 VM 单步功能
        vmStackSandboxPanel_->setController(controller_);
        connect(vmStackSandboxPanel_, &VmStackSandboxPanel::activityCompleted, this,
                [this](const QString& /*levelId*/) {
                    if (!learningPathPanel_)
                        return;
                    if (LearnerProgressStore::instance().areAllLevelsCompleted(
                            {"level-1", "level-2", "level-3", "level-4", "level-5"})) {
                        learningPathPanel_->markActivityCompleted(QStringLiteral("vm-sandbox"));
                    }
                });
        return vmStackSandboxPanel_;
    });
    registerLazyPanel(QStringLiteral("code-journey"), mlTr("代码生命旅程"), [this]() {
        codeJourneyPanel_ = new CodeJourneyInfoPanel(this);
        // ROUND-60 fix (Issue 1): 移除 jumpToPanelRequested 连接（底部按钮已删除）
        // 点进面板观看即标记 code-journey 活动完成
        connect(codeJourneyPanel_, &CodeJourneyInfoPanel::journeyCompleted, this, [this]() {
            if (learningPathPanel_) {
                learningPathPanel_->markActivityCompleted(QStringLiteral("code-journey"));
            }
        });
        return codeJourneyPanel_;
    });
    registerLazyPanel(QStringLiteral("glossary"), mlTr("术语表"), [this]() {
        glossaryPanel_ = new GlossaryPanel(this);
        connect(glossaryPanel_, &GlossaryPanel::termActivated, this, [this](const QString& termId) {
            const QString pid = GlossaryPanel::relatedPanelFor(termId);
            if (!pid.isEmpty()) {
                showTeachingPanel(pid);
            }
        });
        return glossaryPanel_;
    });

    // ---- 左侧教学树导航 dock（与文件树/调试面板同区域 tab）----
    teachingTreePanel_ = new TeachingTreePanel(this);
    teachingTreeDock_ = dockManager_->createDockWidget("teachingTreeDock");
    teachingTreeDock_->setWindowTitle(mlTr("学习"));
    teachingTreeDock_->setWidget(teachingTreePanel_, ads::CDockWidget::ForceNoScrollArea);
    dockManager_->addDockWidgetTabToArea(teachingTreeDock_, fileTreeDock_->dockAreaWidget());
    // 教学树默认隐藏（点击 ActivityBar 「学习」时显示）
    teachingTreeDock_->toggleView(false);
    // 教学树点击 → 路由
    connect(teachingTreePanel_, &TeachingTreePanel::panelRequested, this, &Ide::onTeachingPanelRequested);

    // 第十二轮：面板尺寸对齐规范（左260px、底220px、右320px）
    // 所有面板支持拖拽重组、浮动、标签分组（布局持久化由 saveLayout/restoreLayout 处理）
    // ISSUE-4 fix: 编译分析（右侧）与调试面板打开时尺寸过小，提升最小尺寸与初始尺寸，
    // 确保字节码追踪/AST/IR 可视化等内容有足够展示空间。
    fileTree_->setMinimumWidth(240);
    debugPanel_->setMinimumWidth(260);
    bottomContainer_->setMinimumHeight(220);
    rightContainer->setMinimumWidth(420);
    rightContainer->setMaximumWidth(1200);

    // 延迟隐藏单 widget 标题栏（R60-2 fix: 移除无条件 resize，改由 restoreLayout
    // 在无保存状态时才应用默认尺寸，避免覆盖用户上次保存的面板大小）
    QTimer::singleShot(0, this, [this]() {
        // 隐藏右侧 dock 的 ADS 标题栏（Pivot 作为标签栏）
        if (rightDock_ && rightDock_->dockAreaWidget()) {
            rightDock_->dockAreaWidget()->setDockAreaFlag(ads::CDockAreaWidget::HideSingleWidgetTitleBar, true);
        }
    });

    // 第九轮：启动时隐藏所有停靠面板（仅保留活动栏 + 顶部 + 欢迎页）
    fileTreeDock_->toggleView(false);
    debugPanelDock_->toggleView(false);
    rightDock_->toggleView(false);
    // R58-1 fix: 显式隐藏 bottomContainer_，保持与 bottomVisible_ (默认 false) 状态一致。
    // 此前未显式 hide，QWidget 创建后默认 visible，启动时 bottomContainer_ 占据
    // editorSplitter_ 全部空间（editorTabWidget_ 已 hide），表现为「欢迎页下方出现输出面板」。
    // 更严重的是：bottomVisible_ 为 false 时 hideBottomPanel() 的 `if (bottomVisible_)`
    // 守卫直接 return，导致关闭按钮失效——面板关不掉。
    if (bottomContainer_) {
        bottomContainer_->hide();
        bottomVisible_ = false;
    }
    // 第十四轮：教学面板已迁移到 centerStack_，默认显示欢迎页（index 0），
    // 教学面板按需通过教学树切换显示，无需 toggleView(false)
    // teachingTreeDock_ 已在创建时 toggleView(false)

    // Connect file tree context menu
    connect(fileTree_, &QWidget::customContextMenuRequested, this, &Ide::onFileTreeContextMenu);

    // 第九轮：连接 dock viewToggled → 防抖保存 + 视图菜单勾选同步
    auto connectDockSave = [this](ads::CDockWidget* dock) {
        if (dock) {
            connect(dock, &ads::CDockWidget::viewToggled, this, [this]() {
                if (splitterSaveTimer_)
                    splitterSaveTimer_->start();
                syncViewMenuChecks();
            });
        }
    };
    connectDockSave(fileTreeDock_);
    connectDockSave(debugPanelDock_);
    connectDockSave(rightDock_);
    // 第十四轮：教学面板已迁移到 centerStack_，无独立 dock；仅 teachingTreeDock_ 需连接
    connectDockSave(teachingTreeDock_);
    // focusedDockWidgetChanged fires when user interacts with dock widgets (drag/dock)
    connect(dockManager_, &ads::CDockManager::focusedDockWidgetChanged, this, [this]() {
        if (splitterSaveTimer_)
            splitterSaveTimer_->start();
    });
    // R61-3 fix: 安装事件过滤器捕获 dock 区域 resize 事件，用户拖拽 ADS splitter
    // 调整面板大小时触发防抖保存。ADS 无 splitterMoved 信号，用 Resize 事件替代。
    if (dockManager_) {
        dockManager_->installEventFilter(this);
    }
}

// ============================================================
// syncViewMenuChecks — 第九轮：视图菜单勾选状态与 dock 显隐双向同步
// ============================================================

/// 同步“视图”菜单中各面板的勾选状态与当前可见性。
void Ide::syncViewMenuChecks() {
    if (syncingViewAction_)
        return;
    syncingViewAction_ = true;
    if (viewExplorerAction_)
        viewExplorerAction_->setChecked(fileTreeDock_ && !fileTreeDock_->isClosed());
    if (viewDebugAction_)
        viewDebugAction_->setChecked(debugPanelDock_ && !debugPanelDock_->isClosed());
    if (viewOutputAction_)
        viewOutputAction_->setChecked(bottomVisible_);
    if (viewCompileAnalysisAction_)
        viewCompileAnalysisAction_->setChecked(rightDock_ && !rightDock_->isClosed());
    // 第十四轮：教学面板已迁移到 centerStack_，视图菜单的 checkable 状态
    // 由当前 centerStack_ 索引决定——哪个教学面板正在显示就勾选哪个 action
    if (viewLearningHubAction_)
        viewLearningHubAction_->setChecked(teachingTreeDock_ && !teachingTreeDock_->isClosed());
    // 辅助 lambda：按 panelId 同步对应 view action 的勾选状态
    auto syncTeachingAction = [this](QAction* action, const QString& panelId) {
        if (!action)
            return;
        auto it = panelToStackIndex_.constFind(panelId);
        // 任务2：三栏布局 — 编辑器模式下 centerStack_ 被隐藏，教学面板不活跃
        bool active = (it != panelToStackIndex_.end() && centerStack_ && !centerInEditorMode_ &&
                       centerStack_->currentIndex() == it.value());
        action->setChecked(active);
    };
    syncTeachingAction(viewPipelineAction_, QStringLiteral("pipeline"));
    syncTeachingAction(viewBackendCompareAction_, QStringLiteral("backend-compare"));
    syncTeachingAction(viewBugHuntAction_, QStringLiteral("bug-hunt"));
    syncTeachingAction(viewSyntaxExplorerAction_, QStringLiteral("syntax-explorer"));
    syncTeachingAction(viewLabManualAction_, QStringLiteral("lab-manual"));
    syncTeachingAction(viewMemoryModelAction_, QStringLiteral("memory-model"));
    syncTeachingAction(viewIRTransformAction_, QStringLiteral("ir-transform"));
    syncTeachingAction(viewProfileDashboardAction_, QStringLiteral("profile-dashboard"));
    syncTeachingAction(viewCallStackAction_, QStringLiteral("call-stack"));
    syncTeachingAction(viewVariableInspectorAction_, QStringLiteral("variable-inspector"));
    syncTeachingAction(viewBytecodeTraceAction_, QStringLiteral("bytecode-trace"));
    syncTeachingAction(viewBreakpointConditionAction_, QStringLiteral("breakpoint-condition"));
    syncTeachingAction(viewExceptionFlowAction_, QStringLiteral("exception-flow"));
    syncTeachingAction(viewClosureInspectorAction_, QStringLiteral("closure-inspector"));
    syncTeachingAction(viewLearningPathAction_, QStringLiteral("learning-path"));
    syncTeachingAction(viewTokenPuzzleAction_, QStringLiteral("token-puzzle"));
    syncTeachingAction(viewAstBuilderToyAction_, QStringLiteral("ast-toy"));
    syncTeachingAction(viewVmStackSandboxAction_, QStringLiteral("vm-sandbox"));
    syncTeachingAction(viewCodeJourneyAction_, QStringLiteral("code-journey"));
    syncTeachingAction(viewGlossaryAction_, QStringLiteral("glossary"));
    syncingViewAction_ = false;
}

// ============================================================
// loadCodeIntoMainEditor — 教学增强面板：将面板内代码加载到主编辑器
// ============================================================

/// 将给定源码文本载入主编辑器（教学示例/外部注入用）。
void Ide::loadCodeIntoMainEditor(const QString& code) {
    if (code.isEmpty())
        return;
    // 修复（issue 3）：教学面板（BugHunt/LabManual/SyntaxExplorer 等）请求加载代码时，
    // 不切换到独占编辑器模式（ensureEditorVisible 会 hide 教学面板），而是让编辑器
    // 从右侧展开，配合流畅动画压缩教学区内容，形成「教学树 | 教学面板(压缩) | 编辑器」
    // 三栏并排布局。关闭编辑器标签时教学区再平滑延展恢复（见 onEditorTabCloseRequested）。
    const bool inTeachingMode = !centerInEditorMode_;
    if (!inTeachingMode) {
        // 编辑器/欢迎页模式：走原逻辑（编辑器完全展开）
        ensureEditorVisible();
    } else {
        // 教学模式：显示编辑器栏但保留教学面板可见
        if (editorTabWidget_)
            editorTabWidget_->show();
    }

    if (!codeEditor_) {
        // 创建新标签
        // BUG-R15-6 fix: createNewEditorTab 返回新标签索引但不设置 codeEditor_ 成员，
        // 导致后续 onRun() 第 4371 行 `if (!codeEditor_) return;` 直接返回，
        // 表现为「加载代码后点击运行无反应」和「LabManual 触发示例无法运行」。
        // 必须调用 switchToTab 同步 codeEditor_/highlighter_/currentFilePath_。
        int newIdx = createNewEditorTab(QString(), code);
        switchToTab(newIdx);
    } else {
        codeEditor_->setPlainText(code);
        // 标记为未保存
        if (!editorTabs_.empty()) {
            int idx = editorTabWidget_ ? editorTabWidget_->currentIndex() : 0;
            if (idx >= 0 && idx < (int)editorTabs_.size()) {
                editorTabs_[idx].isUntitled = true;
                editorTabs_[idx].filePath.clear();
            }
        }
        isDirty_ = true;
        updateWindowTitle();
    }

    // issue 3：教学模式下编辑器从右侧展开，动画压缩教学区（教学 45% / 编辑器 55%）
    if (inTeachingMode && centerSplitter_ && centerStack_ && editorTabWidget_) {
        QList<int> savedSizes = centerSplitter_->sizes();
        if (savedSizes.size() == 2) {
            int total = savedSizes[0] + savedSizes[1];
            if (total > 200) {
                int teachingW = static_cast<int>(total * 0.45);
                int editorW = total - teachingW;
                // 编辑器之前可能宽度为 0（隐藏），动画到目标占比
                animateCenterSplitter(savedSizes, {teachingW, editorW});
            }
        }
    }
}

// ============================================================
// Status bar initialization
// ============================================================

/// 初始化状态栏：行号/列号、运行状态、错误计数等指示控件。
void Ide::initStatusBar() {
    auto* sb = statusBar();
    sb->setFixedHeight(24);
    statusLineLabel_ = new QLabel(mlTr("行 1"));
    statusColLabel_ = new QLabel(mlTr("列 1"));
    statusSaveLabel_ = new QLabel(QString());
    statusRunLabel_ = new QLabel(QString());
    statusEncodingLabel_ = new QLabel(mlTr("UTF-8"));
    statusEngineLabel_ = new QLabel(QString());
    statusSelectionLabel_ = new QLabel(this);
    statusSelectionLabel_->setStyleSheet("color: #6e6e6e; padding: 0 6px;");
    statusSelectionLabel_->setVisible(false);

    // 第八轮：左侧显示行列，右侧显示编码/保存/运行状态/执行引擎
    // M6：选中范围作为第 6 个 permanent widget，仅在有选中时显示
    sb->addWidget(statusLineLabel_);
    sb->addWidget(statusColLabel_);
    sb->addPermanentWidget(statusEncodingLabel_);
    sb->addPermanentWidget(statusSaveLabel_);
    sb->addPermanentWidget(statusRunLabel_);
    sb->addPermanentWidget(statusEngineLabel_);
    sb->addPermanentWidget(statusSelectionLabel_);

    // 初始化引擎标签文本（engineCombo_ 已在 initTitleBar 中创建）
    if (engineCombo_) {
        statusEngineLabel_->setText(QString("\xE2\x9A\x99 %1").arg(engineCombo_->currentText()));
    }
}

// ============================================================
// Fluent styling
// ============================================================

/// 应用 QFluentKit 主题 QSS 与调色板，统一整体视觉风格。
void Ide::applyFluentStyle() {
    // PERF: 重入守卫 - 防止 setPalette/setStyleSheet 触发的事件递归调用
    if (applyingStyle_)
        return;
    applyingStyle_ = true;

    // ---- Register native widgets with QFluentKit style sheet manager ----
    // R66-2 fix: fileTree_/errorListWidget_/bytecodeList_/recentListWidget_/tokenTable_
    // 不再注册到 QFluentKit。原因：QFluentKit 的 list_view.qss/table_view.qss 设置
    // background: transparent，StyleSheetManager 在主题信号触发时调用 updateStyleSheet
    // 重新应用这些 QSS，覆盖下方 itemViewQss 中设置的米黄色背景。
    // 这些控件由 itemViewQss 统一样式化，滚动条仍替换为 Fluent ScrollBar。
    // editorTabWidget_ 保留注册（TAB_VIEW 样式无背景冲突）。
    if (editorTabWidget_)
        StyleSheet::registerWidget(editorTabWidget_, Fluent::ThemeStyle::TAB_VIEW);

    // ---- Replace native scrollbars with Fluent scrollbars ----
    // PERF: 仅首次替换，避免每次主题切换重复分配（旧 ScrollBar 由 parent 管理）
    // 通过检测现有 scrollbar 是否已是 Fluent::ScrollBar 实例来去重
    auto isFluentScrollBar = [](QScrollBar* sb) -> bool { return sb && sb->inherits("Fluent::ScrollBar"); };
    if (outputTextEdit_) {
        if (!isFluentScrollBar(outputTextEdit_->verticalScrollBar()))
            outputTextEdit_->setVerticalScrollBar(new ScrollBar(outputTextEdit_));
        if (!isFluentScrollBar(outputTextEdit_->horizontalScrollBar()))
            outputTextEdit_->setHorizontalScrollBar(new ScrollBar(Qt::Horizontal, outputTextEdit_));
    }
    if (errorListWidget_ && !isFluentScrollBar(errorListWidget_->verticalScrollBar()))
        errorListWidget_->setVerticalScrollBar(new ScrollBar(errorListWidget_));
    if (bytecodeList_ && !isFluentScrollBar(bytecodeList_->verticalScrollBar()))
        bytecodeList_->setVerticalScrollBar(new ScrollBar(bytecodeList_));
    if (fileTree_ && !isFluentScrollBar(fileTree_->verticalScrollBar()))
        fileTree_->setVerticalScrollBar(new ScrollBar(fileTree_));

    // ---- Theme-aware color palette ----
    // 14 色统一走 TeachingTheme::ide*()，亮/暗主题切换时 applyFluentStyle()
    // 会被重新调用（onThemeModeChanged 信号触发），无需手动刷新各 widget。
    bool dark = Theme::isDark();
    QString bgMain = TeachingTheme::ideBgMain().name();
    QString bgPanel = TeachingTheme::ideBgPanel().name();
    QString bgSidebar = TeachingTheme::ideBgSidebar().name();
    QString fgPrimary = TeachingTheme::ideFgPrimary().name();
    QString fgSecondary = TeachingTheme::ideFgSecondary().name();
    QString borderColor = TeachingTheme::ideBorder().name();
    QString accentColor = TeachingTheme::ideAccent().name();
    QString hoverBg = TeachingTheme::ideHoverBg().name();
    QString selectedBg = TeachingTheme::ideSelectedBg().name();
    QString statusBg = TeachingTheme::ideStatusBg().name();
    QString editorBg = TeachingTheme::ideEditorBg().name();
    QString lineNumBg = TeachingTheme::ideLineNumBg().name();
    QString lineNumFg = TeachingTheme::ideLineNumFg().name();

    // ============================================================
    // ADS (Qt Advanced Docking System) comprehensive QSS override
    // 完全覆盖 ADS 默认样式，对齐中性白主题（R74: 回退 Solarized 米黄）
    // R68 fix: adsQss 必须包含 default.css 中的关键 qproperty-icon 规则，
    // 因为 setStyleSheet() 会完全替换 dockManager_ 上的样式表（包括构造时
    // loadStylesheet 设置的图标属性）。缺失图标规则会导致关闭/浮动/菜单按钮无图标。
    // 同样补充 QToolTip 样式，防止回退到 Windows 11 默认黑色 tooltip。
    // ColorSchemeMode 已在构造后设为 Light，setPalette 不会触发 loadStylesheet 覆盖。
    // ============================================================
    QString adsQss =
        QString(R"(
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
        ads--CDockWidgetTab[focused="true"] {
            background: %1;
            border-bottom: 2px solid %3;
            color: %4;
        }
        ads--CDockWidgetTab:hover:!activeTab {
            background: %5;
        }
        ads--CDockWidgetTab QLabel {
            color: %2;
            background: transparent;
        }
        ads--CDockWidgetTab[activeTab="true"] QLabel {
            color: %4;
            background: transparent;
        }
        ads--CDockWidgetTab[focused="true"] QLabel {
            color: %4;
            background: transparent;
        }

        /* Tab close button: 图标 + hover 效果 */
        #tabCloseButton {
            margin-top: 2px;
            background: none;
            border: none;
            padding: 0px -2px;
            min-width: 16px;
            min-height: 16px;
            max-width: 16px;
            max-height: 16px;
            qproperty-icon: url(:/ads/images/close-button.svg),
                    url(:/ads/images/close-button-disabled.svg) disabled;
            qproperty-iconSize: 16px;
        }
        #tabCloseButton:hover {
            border: 1px solid rgba(0, 0, 0, 32);
            background: rgba(0, 0, 0, 16);
            border-radius: 3px;
        }
        #tabCloseButton:pressed {
            background: rgba(0, 0, 0, 32);
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

        /* Title bar button icons (from default.css) */
        #tabsMenuButton::menu-indicator {
            image: none;
        }
        #tabsMenuButton {
            qproperty-icon: url(:/ads/images/tabs-menu-button.svg);
            qproperty-iconSize: 16px;
        }
        #dockAreaCloseButton {
            qproperty-icon: url(:/ads/images/close-button.svg),
                    url(:/ads/images/close-button-disabled.svg) disabled;
            qproperty-iconSize: 16px;
        }
        #detachGroupButton {
            qproperty-icon: url(:/ads/images/detach-button.svg),
                    url(:/ads/images/detach-button-disabled.svg) disabled;
            qproperty-iconSize: 16px;
        }
        ads--CTitleBarButton {
            padding: 0px 0px;
        }

        /* Scroll area inside dock widgets */
        QScrollArea#dockWidgetScrollArea {
            padding: 0px;
            border: none;
        }

        /* ---- ADS Splitter (1px thin line, hover → 2px accent) ---- */
        ads--CDockContainerWidget > QSplitter {
            padding: 1 0 1 0;
        }
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

        /* ---- ADS Dock Container / Manager (顶层容器兜底) ---- */
        ads--CDockContainerWidget {
            background: %1;
            border: none;
        }
        ads--CDockManager {
            background: %1;
            border: none;
        }

        /* ---- ADS Dock Widget ---- */
        ads--CDockWidget {
            background: %1;
            border: none;
        }
        ads--CDockWidget[focused="true"] {
            border: none;
        }
        ads--CDockAreaWidget[focused="true"] {
            border: none;
        }
        ads--CDockAreaTitleBar[focused="true"] {
            background: transparent;
        }

        /* ---- ADS AutoHide Tabs ---- */
        ads--CAutoHideTab {
            background: %8;
            border: none;
            color: %2;
            padding: 4px 8px;
            min-height: 24px;
            qproperty-iconSize: 16px 16px;
        }
        ads--CAutoHideTab:hover {
            background: %5;
            color: %3;
        }
        ads--CAutoHideTab[activeTab="true"] {
            background: %1;
            border-left: 2px solid %3;
        }
        ads--CAutoHideSideBar {
            background: %8;
            border: none;
            qproperty-spacing: 12;
        }
        #sideTabsContainerWidget {
            background: transparent;
        }

        /* ---- QToolTip (防止回退到 Windows 11 黑色 tooltip) ---- */
        QToolTip {
            background: %1;
            color: %4;
            border: 1px solid %7;
            border-radius: 4px;
            padding: 4px 8px;
            font-size: 12px;
        }
    )")
            .arg(bgMain, fgPrimary, accentColor, fgPrimary, hoverBg, dark ? "#505050" : "#d0d0d0", /* close btn hover */
                 borderColor, bgSidebar);

    // ---- Apply ADS QSS ----
    // R68 修复方案：构造时已将 ColorSchemeMode 设为 Light（非 FollowPalette），
    // ADS eventFilter 检测到 ColorSchemeMode!=FollowPalette 时跳过 loadStylesheet，
    // 因此此处 setStyleSheet(adsQss) 设置的自定义样式不会被后续 setPalette 触发的
    // loadStylesheet 覆盖。adsQss 包含了 default.css 中的关键图标 qproperty 规则
    // 和 QToolTip 样式，确保完全替换后 ADS 按钮图标和 tooltip 仍正常显示。
    if (dockManager_) {
        dockManager_->setStyleSheet(adsQss);
        // R72 fix: 直接在代码层面设置 dockManager_ 及所有 CDockWidgetTab 的 palette，
        // 不依赖 QSS 类型选择器（ads--CDockWidgetTab）是否匹配。
        // 根因：QSS 类型选择器基于 metaObject()->className()，命名空间中的类返回
        // "ads::CDockWidgetTab"。虽然 Qt 文档说 :: 应替换为 --，但实际匹配可能
        // 因 Qt 版本/平台而异。直接 setPalette 确保背景色正确。
        QPalette dPal = dockManager_->palette();
        dPal.setColor(QPalette::Window, TeachingTheme::ideBgMain());
        dPal.setColor(QPalette::Base, TeachingTheme::ideBgMain());
        dPal.setColor(QPalette::AlternateBase, TeachingTheme::ideBgPanel());
        dPal.setColor(QPalette::Highlight, TeachingTheme::ideBgPanel());
        dPal.setColor(QPalette::HighlightedText, TeachingTheme::ideFgPrimary());
        dPal.setColor(QPalette::WindowText, TeachingTheme::ideFgPrimary());
        dPal.setColor(QPalette::Text, TeachingTheme::ideFgPrimary());
        dockManager_->setPalette(dPal);
        dockManager_->setAutoFillBackground(true);
        // 对每个 tab 直接设置 palette（不依赖 QSS 级联）
        // R73 fix: 补充 Light/Midlight 设置（default.css active tab 用 palette(light)），
        // 并对 tab 内的子控件（QLabel/CElidingLabel）也设置 palette。
        for (auto* tab : dockManager_->findChildren<ads::CDockWidgetTab*>()) {
            QPalette tPal = tab->palette();
            tPal.setColor(QPalette::Window, TeachingTheme::ideBgMain());
            tPal.setColor(QPalette::WindowText, TeachingTheme::ideFgPrimary());
            tPal.setColor(QPalette::Highlight, TeachingTheme::ideBgPanel());
            tPal.setColor(QPalette::HighlightedText, TeachingTheme::ideFgPrimary());
            // default.css activeTab 渐变使用 palette(window)→palette(light)
            tPal.setColor(QPalette::Light, TeachingTheme::ideBgPanel());
            tPal.setColor(QPalette::Midlight, TeachingTheme::ideBgMain());
            tPal.setColor(QPalette::Base, TeachingTheme::ideBgMain());
            tPal.setColor(QPalette::AlternateBase, TeachingTheme::ideBgPanel());
            tPal.setColor(QPalette::Text, TeachingTheme::ideFgPrimary());
            tab->setPalette(tPal);
            tab->setAutoFillBackground(true);
            // 对 tab 内的子控件（标题标签等）也设置 palette
            for (auto* child : tab->findChildren<QWidget*>()) {
                QPalette cPal = child->palette();
                cPal.setColor(QPalette::Window, TeachingTheme::ideBgMain());
                cPal.setColor(QPalette::WindowText, TeachingTheme::ideFgPrimary());
                cPal.setColor(QPalette::Text, TeachingTheme::ideFgPrimary());
                child->setPalette(cPal);
            }
        }
    }

    // ============================================================
    // Title bar styling (统一标题栏)
    // ============================================================
    if (titleBar_) {
        // 标题栏渐变背景：白色 → 浅灰（自上而下）（R74: 回退中性白）
        // 移除原 titleBg(%1) 占位，剩余参数重编号：%1=borderColor %2=fgPrimary %3=fgSecondary %4=hoverBg
        titleBar_->setStyleSheet(QString(R"(
            #titleBar {
                background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #FFFFFF, stop:1 #F5F5F5);
                border-bottom: 1px solid %1;
            }
            #titleText {
                color: %2;
                background: transparent;
                border: none;
            }
            #titlePath {
                color: %3;
                background: transparent;
                border: none;
                padding-left: 4px;
            }
            #titleBarSep {
                background: %1;
                max-width: 1px;
            }
            #titleMinBtn, #titleMaxBtn {
                background: transparent;
                border: none;
            }
            #titleMinBtn:hover, #titleMaxBtn:hover {
                background: %4;
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
                background: %4;
                border-radius: 4px;
            }
        )")
                                     .arg(borderColor, fgPrimary, fgSecondary, hoverBg));
    }

    // ============================================================
    // Status bar styling
    // ============================================================
    if (auto* sb = statusBar()) {
        // 状态栏：顶部 1px 分隔线保留，字号 11px 更紧凑，QLabel padding 0 8px（永久消息区右侧 8px padding）
        sb->setStyleSheet(QString(R"(
            QStatusBar {
                background: %1;
                border-top: 1px solid %2;
                color: %3;
                font-size: 11px;
                padding: 0 8px;
            }
            QStatusBar QLabel {
                color: %3;
                padding: 0 8px;
                background: transparent;
            }
        )")
                              .arg(statusBg, borderColor, fgSecondary));
    }

    // ============================================================
    // Panel container styling (bottom + right)
    // ============================================================
    // 教学面板卡片样式：4px 圆角 + 1px 柔和边框 + 白色背景（与 panel bg #f3f3f3 形成对比）
    // 内部 padding 8px 由 wrapTeachingPanel 的 layout contentsMargins 提供
    QString panelQss = QString(R"(
        #bottomPanelContainer, #rightPanelContainer {
            background: %1;
            border: none;
        }
        #bottomPanelContainer {
            border-top: 1px solid %2;
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
        #teachingPanelCard {
            background: %5;
            border: 1px solid %2;
            border-radius: 4px;
        }
    )")
                           .arg(bgPanel, borderColor, fgSecondary, hoverBg, bgMain);

    // PERF: 不再用 findChildren 全树遍历逐个 setStyleSheet。
    // panelQss 中的 #id 选择器会通过 Qt 样式表继承机制自动应用到匹配 objectName 的子控件。
    // panelQss 将和 QGroupBox 等样式一起合并设置到主窗口（见函数末尾）。

    // ============================================================
    // Editor tab widget styling
    // ============================================================
    if (editorTabWidget_) {
        editorTabWidget_->setStyleSheet(
            QString(R"(
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
        )")
                .arg(bgMain, bgPanel, fgSecondary, accentColor, fgPrimary, hoverBg, dark ? "#505050" : "#d0d0d0"));
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
    )")
                              .arg(bgMain, fgPrimary, selectedBg, hoverBg, bgPanel, borderColor);

    // Apply to all tree/table/list views
    if (fileTree_)
        fileTree_->setStyleSheet(itemViewQss);
    if (errorListWidget_)
        errorListWidget_->setStyleSheet(itemViewQss);
    if (bytecodeList_)
        bytecodeList_->setStyleSheet(itemViewQss);
    if (tokenTable_)
        tokenTable_->setStyleSheet(itemViewQss);
    if (recentListWidget_)
        recentListWidget_->setStyleSheet(itemViewQss);

    // ============================================================
    // Activity bar styling
    // ============================================================
    if (activityBar_) {
        activityBar_->setStyleSheet(QString(R"(
            ActivityBar {
                background: %1;
                border-right: 1px solid %2;
            }
        )")
                                        .arg(bgSidebar, borderColor));
    }

    // ============================================================
    // 全局兜底背景（R60-1 fix）
    // 历史问题：白名单枚举式 setStyleSheet 遗漏新容器即出现色块割裂。
    // 根因修复：在 QMainWindow 级别设置 palette + autoFillBackground，
    // 所有未显式设置背景的子 widget 自动继承此背景色，无需逐一枚举。
    // 下方白名单仅用于需要 border:none 等额外样式的容器。
    // ============================================================
    {
        QColor bg = TeachingTheme::ideBgMain();
        QColor bgPanel = TeachingTheme::ideBgPanel();
        QColor fg = TeachingTheme::ideFgPrimary();
        QPalette pal = palette();
        pal.setColor(QPalette::Window, bg);
        pal.setColor(QPalette::Base, bg);
        // R65-2 fix: 设置 AlternateBase 为面板色（比主背景稍深），
        // 使 alternatingRowColors 的列表（如字节码列表）交替行也保持浅灰色调。
        pal.setColor(QPalette::AlternateBase, bgPanel);
        // R68 fix: 设置 ToolTip 颜色，防止 QToolTip 回退到 Windows 11 默认黑色样式
        pal.setColor(QPalette::ToolTipBase, bg);
        pal.setColor(QPalette::ToolTipText, fg);
        // R71/R74 fix: 设置 QPalette::Highlight 为面板色 (#F5F5F5)，HighlightedText 为深色字。
        // 根因：ads--CDockWidgetTab 等 QSS 选择器不匹配 ads::CDockWidgetTab 类
        // (QSS 类型选择器基于 metaObject()->className()，命名空间中的类返回
        // "ads::CDockWidgetTab"，与 "ads--CDockWidgetTab" 不等)，QSS 规则不生效，
        // ADS tab 背景由 palette(Highlight) 控制。R65-1 移除 Highlight 设置后，
        // tab 使用系统默认 Highlight 色 (#258292 蓝绿色)，显示"蓝底"。
        // 设置 Highlight=#F5F5F5 (面板色) 后，tab 背景为浅灰色，与主背景 #FFFFFF
        // 有微妙区别，视觉上区分 active/inactive tab。列表选中项也使用此色，
        // 浅灰底+深色字，中性主题跨机器渲染一致。
        pal.setColor(QPalette::Highlight, bgPanel);
        pal.setColor(QPalette::HighlightedText, fg);
        // R76 fix: 补全文本角色，防止系统深色模式下 Text/WindowText 回退为浅色
        // 导致文字与白色背景不可见。
        pal.setColor(QPalette::WindowText, fg);
        pal.setColor(QPalette::Text, fg);
        pal.setColor(QPalette::ButtonText, fg);
        pal.setColor(QPalette::Button, bg);
        pal.setColor(QPalette::PlaceholderText, TeachingTheme::ideFgSecondary());
        setPalette(pal);
        setAutoFillBackground(true);
        // R68 fix: 全局设置 QToolTip 样式（qApp 级别），确保所有 widget 的 tooltip
        // 都使用主题色而非 Windows 11 默认黑色。用静态变量确保只设置一次（避免
        // applyFluentStyle 多次调用导致 QSS 重复累积）。
        static bool s_toolTipStyled = false;
        if (!s_toolTipStyled) {
            qApp->setStyleSheet(qApp->styleSheet() + QString(R"(
                QToolTip {
                    background: %1;
                    color: %2;
                    border: 1px solid %3;
                    border-radius: 4px;
                    padding: 4px 8px;
                    font-size: 12px;
                }
            )")
                                                         .arg(bg.name(), fg.name(), TeachingTheme::ideBorder().name()));
            s_toolTipStyled = true;
        }
    }

    // ============================================================
    // Main container / middle area / central stack / splitter
    // R15-7/R74: 补全覆盖 centerStack_、centerSplitter_、welcomePage_、replPanel_、
    // errorPageContainer、fileTreeContainer 等通用 widget 背景色，消除「部分区域
    // 未变白色」的割裂感。统一使用纯白 (#FFFFFF) 作为主背景，跨机器渲染一致。
    // PERF: 合并原来的 3 次 findChildren 全树遍历为 1 次，直接用成员指针处理已知控件
    // ============================================================
    const QString bgStyle = QString("background: %1; border: none;").arg(bgMain);

    // 直接通过成员指针设置已知控件（无需遍历）
    auto applyBgStyle = [&bgStyle](QWidget* w) {
        if (!w)
            return;
        w->setStyleSheet(bgStyle);
        w->setAttribute(Qt::WA_StyledBackground, true);
    };
    applyBgStyle(centerStack_);
    applyBgStyle(centerSplitter_);
    applyBgStyle(editorSplitter_);
    applyBgStyle(bottomContainer_);
    applyBgStyle(replPanel_);
    applyBgStyle(rightStack_);
    applyBgStyle(welcomePage_);

    // 一次性 findChildren 处理其余非成员容器（替代原来的 3 次全树遍历）
    // 注意：bottomContainer_/centerStack_/centerSplitter_/editorSplitter_/replPanel_/rightStack_/welcomePage_
    // 已通过 applyBgStyle 直接设置，不在此重复处理
    for (auto* w : findChildren<QWidget*>()) {
        QString name = w->objectName();
        if (name == "mainContainer" || name == "middleArea" || name == "welcomeRecentPanel" ||
            name == "welcomeCenter" || name == "welcomeBtnContainer" || name == "errorPageContainer" ||
            name == "fileTreeContainer" || name == "rightPanelContainer" || name == "bottomPivotRow" ||
            name == "rightPivotRow" || name == "bytecodePage" || name == "bytecodeSplitter" ||
            name == "debugButtonContainer" || name == "vmButtonContainer") {
            if (name == "mainContainer" || name == "middleArea") {
                QPalette p = w->palette();
                p.setColor(QPalette::Window, TeachingTheme::ideBgMain());
                w->setPalette(p);
                w->setAutoFillBackground(true);
            }
            if (name == "debugButtonContainer" || name == "vmButtonContainer") {
                w->setStyleSheet("background: transparent; border: none;");
            } else {
                w->setStyleSheet(bgStyle);
            }
            w->setAttribute(Qt::WA_StyledBackground, true);
        }
    }

    // ============================================================
    // VM Stack Panel 主题化样式（原由 styles.qss 集中管理，现迁移到 applyFluentStyle）
    // R74: 浅色路径回退为中性白/浅灰（原 Solarized base3/base2）
    // ============================================================
    QString vmOpBg = dark ? "#1e2a1e" : "#ffffff";
    QString vmOpFg = dark ? "#b5cea8" : "#859900";
    QString vmOpBorder = dark ? "#2d3a2d" : "#e0e0e0";
    QString vmItemBorder = dark ? "#2d2d30" : "#f0f0f0";
    QString vmSelectedBg = dark ? "#264f78" : "#cce4f7";
    QString vmSelectedFg = dark ? "#ffffff" : "#268BD2";
    QString vmHeaderBg = dark ? "#252526" : "#f5f5f5";

    QString vmQss = QString(R"(
        QLabel#vmOpLabel {
            background-color: %1;
            color: %2;
            font-family: "Cascadia Code", "Cascadia Mono", "Consolas", "JetBrains Mono", "Source Code Pro", "Menlo", "DejaVu Sans Mono", "Courier New", monospace;
            font-size: 12px;
            padding: 8px;
            border-radius: 5px;
            border: 1px solid %3;
        }
        QLabel#vmStackTitle,
        QLabel#vmGlobalsTitle {
            color: %4;
            font-size: 11px;
            font-weight: 600;
            padding: 4px 8px;
            letter-spacing: 0.8px;
        }
        QListWidget#vmStackList {
            background-color: %5;
            color: %6;
            border: 1px solid %7;
            border-radius: 5px;
            font-family: "Cascadia Code", "Cascadia Mono", "Consolas", "JetBrains Mono", "Source Code Pro", "Menlo", "DejaVu Sans Mono", "Courier New", monospace;
            font-size: 12px;
        }
        QListWidget#vmStackList::item {
            padding: 3px 6px;
            border-bottom: 1px solid %8;
        }
        QListWidget#vmStackList::item:selected {
            background-color: %9;
            color: %10;
        }
        QTableWidget#vmGlobalsTable {
            background-color: %5;
            color: %6;
            border: 1px solid %7;
            border-radius: 5px;
            font-family: "Cascadia Code", "Cascadia Mono", "Consolas", "JetBrains Mono", "Source Code Pro", "Menlo", "DejaVu Sans Mono", "Courier New", monospace;
            font-size: 12px;
        }
        QTableWidget#vmGlobalsTable QHeaderView::section {
            background-color: %11;
            color: %4;
            padding: 5px 8px;
            border: none;
            border-right: 1px solid %8;
            border-bottom: 1px solid %7;
            font-weight: 500;
            letter-spacing: 0;
        }
    )")
                        .arg(vmOpBg, vmOpFg, vmOpBorder, fgSecondary, bgMain, fgPrimary, borderColor, vmItemBorder,
                             vmSelectedBg, vmSelectedFg, vmHeaderBg);

    // 应用到 VM 栈面板（若已创建）
    if (vmStackPanel_)
        vmStackPanel_->setStyleSheet(vmQss);

    // ============================================================
    // P2-3/4: QGroupBox 统一 Fluent 外观 + 面板容器样式
    // WelcomeWizard / VmStackSandboxPanel 等面板使用 QGroupBox 作为分组容器，
    // 此处通过全局样式表统一着色（边框/背景/标题色跟随 TeachingTheme 主题色板），
    // 避免逐个 QGroupBox 替换为 SimpleCardWidget 的高风险改动。
    // PERF: 将 panelQss 合并到主窗口样式表，利用 Qt 样式表继承机制自动应用到
    // 匹配 objectName 的子控件，无需 findChildren 逐个 setStyleSheet。
    // 注意：setStyleSheet 会覆盖主窗口之前的样式，但主窗口无其他样式，故安全。
    // ============================================================
    QString mainQss = panelQss;
    mainQss += QString("QGroupBox { "
                       "  border: 1px solid %1; border-radius: 6px; "
                       "  margin-top: 12px; padding-top: 8px; "
                       "  background: %2; "
                       "} "
                       "QGroupBox::title { "
                       "  subcontrol-origin: margin; "
                       "  left: 8px; padding: 0 4px; "
                       "  color: %3; "
                       "}")
                   .arg(TeachingTheme::border().name())
                   .arg(TeachingTheme::surface().name())
                   .arg(TeachingTheme::textPrimary().name());
    setStyleSheet(mainQss);

    // ---- Sync editor theme（深色主题已移除，强制 light 配色）----
    if (codeEditor_)
        codeEditor_->setDarkTheme(false);
    for (auto& tab : editorTabs_) {
        if (tab.editor && tab.editor != codeEditor_)
            tab.editor->setDarkTheme(false);
    }

    applyingStyle_ = false;
}

// ============================================================
// Status bar update (cursor position + save/run state)
// ============================================================

/// 根据当前光标位置与运行状态刷新状态栏文本。
void Ide::updateStatusBar() {
    if (statusLineLabel_ && codeEditor_) {
        QTextCursor cur = codeEditor_->textCursor();
        int line = cur.blockNumber() + 1;
        int col = cur.columnNumber() + 1;
        statusLineLabel_->setText(mlTr("行 %1").arg(line));
        statusColLabel_->setText(mlTr("列 %1").arg(col));
    } else if (statusLineLabel_) {
        statusLineLabel_->setText(mlTr("行 1"));
        statusColLabel_->setText(mlTr("列 1"));
    }
    if (statusSaveLabel_) {
        statusSaveLabel_->setText(isDirty_ ? mlTr("● 未保存") : QString());
    }
    // M6：选中范围（行数 + 字符数）
    if (statusSelectionLabel_ && codeEditor_) {
        QTextCursor cur = codeEditor_->textCursor();
        if (cur.hasSelection()) {
            int start = cur.selectionStart();
            int end = cur.selectionEnd();
            int charCount = end - start;
            // 计算选中跨越的行数
            QTextCursor lineCounter = cur;
            lineCounter.setPosition(start);
            int startLine = lineCounter.blockNumber();
            lineCounter.setPosition(end);
            int endLine = lineCounter.blockNumber();
            int lineCount = endLine - startLine + 1;
            statusSelectionLabel_->setText(mlTr("选中 %1 行 %2 字符").arg(lineCount).arg(charCount));
            statusSelectionLabel_->setVisible(true);
        } else {
            statusSelectionLabel_->setVisible(false);
        }
    } else if (statusSelectionLabel_) {
        statusSelectionLabel_->setVisible(false);
    }
}

// ============================================================
// Connections
// ============================================================

/// 连接主界面信号槽：菜单/工具栏/编辑器交互路由到对应槽函数。
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
        if (viewCompileAnalysisAction_)
            viewCompileAnalysisAction_->toggle();
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
            // 同步状态栏引擎显示（不依赖 controller_，始终更新）
            if (statusEngineLabel_) {
                statusEngineLabel_->setText(QString("\xE2\x9A\x99 %1").arg(engineCombo_->currentText()));
            }
            if (!controller_)
                return;
            // 寄存器式 VM 使用 setUseRegisterVM(true)
            // 栈式 VM / 树遍历解释器 使用 setUseRegisterVM(false)
            controller_->setUseRegisterVM(index == 2);
            appendOutput(mlTr("--- 执行引擎切换: %1 ---").arg(engineCombo_->currentText()));
        });
    }

    connect(editorTabWidget_, &QTabWidget::currentChanged, this, &Ide::onCurrentTabChanged);
    connect(editorTabWidget_, &QTabWidget::tabCloseRequested, this, &Ide::onEditorTabCloseRequested);

    // Shortcuts
    auto* findSc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(findSc, &QShortcut::activated, this, [this]() {
        ensureEditorVisible();
        onFind();
    });
    auto* replaceSc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), this);
    connect(replaceSc, &QShortcut::activated, this, [this]() {
        ensureEditorVisible();
        onReplace();
    });
    auto* findNextSc = new QShortcut(QKeySequence(Qt::Key_F3), this);
    connect(findNextSc, &QShortcut::activated, this, [this]() {
        // 任务2：三栏布局 — editorTabWidget_ 已移至 centerSplitter_，用 centerInEditorMode_ 判断
        if (centerInEditorMode_ && editorTabWidget_ && editorTabWidget_->isVisible())
            onFindNext();
    });
    auto* findPrevSc = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3), this);
    connect(findPrevSc, &QShortcut::activated, this, [this]() {
        if (centerInEditorMode_ && editorTabWidget_ && editorTabWidget_->isVisible())
            onFindPrev();
    });
    auto* escSc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escSc, &QShortcut::activated, this, [this]() {
        if (findReplacePanel_ && findReplacePanel_->isVisible())
            findReplacePanel_->closePanel();
    });

    connect(fileTree_, &QTreeWidget::itemActivated, this, &Ide::onFileTreeItemActivated);

    // 文件树搜索过滤：200ms 防抖（避免大目录每次按键都全量遍历）
    if (fileTreeFilterEdit_) {
        fileTreeFilterTimer_ = new QTimer(this);
        fileTreeFilterTimer_->setSingleShot(true);
        fileTreeFilterTimer_->setInterval(200); // 200ms 防抖
        connect(fileTreeFilterTimer_, &QTimer::timeout, this, [this]() {
            if (!fileTree_ || !fileTreeFilterEdit_)
                return;
            QString filter = fileTreeFilterEdit_->text().toLower();
            std::function<void(QTreeWidgetItem*)> applyFilter;
            applyFilter = [&](QTreeWidgetItem* item) {
                if (item->childCount() == 0) {
                    // 叶子节点（文件）：按文件名匹配
                    bool match = filter.isEmpty() || item->text(0).toLower().contains(filter);
                    item->setHidden(!match);
                } else {
                    // 目录：递归处理子项
                    for (int i = 0; i < item->childCount(); ++i) {
                        applyFilter(item->child(i));
                    }
                    bool hasVisibleChild = false;
                    for (int i = 0; i < item->childCount(); ++i) {
                        if (!item->child(i)->isHidden()) {
                            hasVisibleChild = true;
                            break;
                        }
                    }
                    item->setHidden(!hasVisibleChild && !filter.isEmpty());
                }
            };
            for (int i = 0; i < fileTree_->topLevelItemCount(); ++i) {
                applyFilter(fileTree_->topLevelItem(i));
            }
        });
        connect(fileTreeFilterEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
            if (fileTreeFilterTimer_)
                fileTreeFilterTimer_->start(); // 重启定时器
        });
    }

    // 错误列表类型过滤按钮
    if (errFilterErrorBtn_)
        connect(errFilterErrorBtn_, &QToolButton::toggled, this, [this]() { applyErrorFilter(); });
    if (errFilterWarningBtn_)
        connect(errFilterWarningBtn_, &QToolButton::toggled, this, [this]() { applyErrorFilter(); });
    if (errFilterInfoBtn_)
        connect(errFilterInfoBtn_, &QToolButton::toggled, this, [this]() { applyErrorFilter(); });
    if (errFilterHintBtn_)
        connect(errFilterHintBtn_, &QToolButton::toggled, this, [this]() { applyErrorFilter(); });

    // Controller signals
    connect(controller_, &IdeController::outputReady, this, [this](const QString& msg) {
        if (closing_)
            return;
        appendOutput(msg);
        showBottomPanel(0);
    });
    connect(controller_, &IdeController::runOk, this, [this]() {
        if (closing_)
            return;
        appendOutput(mlTr("--- 程序执行结束 ---"));
        showBottomPanel(0);
    });
    connect(controller_, &IdeController::stoppedByUser, this, [this]() {
        if (closing_)
            return;
        appendOutput(mlTr("--- 调试终止 ---"));
        showBottomPanel(0);
    });
    connect(controller_, &IdeController::runtimeError, this, [this](const QString& msg, int line, int column) {
        // ISSUE-7 fix + ROUND-60: closing_ 标志防御 closeEvent 期间残留 QueuedConnection。
        // maybeSave() 的模态对话框与 processEvents 会派发挂起事件，此时成员可能已部分析构。
        if (closing_)
            return;
        // AUDIT-P2-CORRECT fix: stale 信号防御。
        // closeEvent 期间 stopForClose 唤醒 worker 退出，但 worker 内部已投递的
        // runtimeError QueuedConnection 仍在主线程队列。Ide 析构时处理 pending 事件
        // 会访问已析构成员（codeEditor_/controller_），try/catch 无法捕获 UAF。
        // 检查运行状态：worker 已停止且非调试暂停且非 VM 运行时说明是 stale 信号，丢弃。
        if (!controller_->isRunning() && !controller_->isDebugPaused() && !controller_->isVmRunning())
            return;
        // P0-3 fix (F10): 接入 ErrorHintEngine，为运行时错误附加教学性提示
        // 与拼写建议（基于当前 Interpreter 作用域变量名）
        std::vector<std::string> scopeVars;
        if (controller_) {
            scopeVars = controller_->getReplScopeVariableNames();
        }
        std::string enriched = ErrorHintEngine::enrichErrorMessage(msg.toStdString(), "runtime", scopeVars);
        Diagnostic diag(DiagLevel::Error, enriched, line, column, DiagSource::Interpreter);
        appendError(QString::fromStdString(diag.format()), line, column);
        showBottomPanel(1);
        if (line > 0 && codeEditor_) {
            QSet<int> errorLines;
            errorLines.insert(line);
            codeEditor_->setErrorLines(errorLines);
        }
    });
    connect(controller_, &IdeController::genericError, this, [this](const QString& msg) {
        // ISSUE-7 fix + ROUND-60: closing_ 标志防御 closeEvent 期间残留 QueuedConnection。
        if (closing_)
            return;
        // AUDIT-P2-CORRECT fix: stale 信号防御。
        // closeEvent 期间 stopForClose 唤醒 worker 退出，但 worker 内部已投递的
        // genericError QueuedConnection 仍在主线程队列。Ide 析构时处理 pending 事件
        // 会访问已析构成员（appendError/showBottomPanel 操作的 UI 控件），
        // try/catch 无法捕获 UAF。检查运行状态：worker 已停止且非调试暂停且非 VM
        // 运行时说明是 stale 信号，丢弃。
        if (!controller_->isRunning() && !controller_->isDebugPaused() && !controller_->isVmRunning())
            return;
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

/// 初始化文件树控件（根工作区与展开/折叠策略）。
void Ide::initFileTree() {
    connect(fileTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        bool isDir = item->data(0, Qt::UserRole + 1).toBool();
        if (!isDir)
            return;
        if (item->childCount() > 0)
            return;
        QString dirPath = item->data(0, Qt::UserRole).toString();
        populateDirChildren(fileTree_, item, dirPath, 0);
    });
}

/// 递归填充文件树：仅列出 .mini/.ml 文件与子目录。
void Ide::populateFileTree() {
    fileTree_->clear();
    if (workspaceDir_.isEmpty())
        return;

    QDir rootDir(workspaceDir_);
    if (!rootDir.exists())
        return;

    auto* rootItem = new QTreeWidgetItem(fileTree_);
    rootItem->setText(0, rootDir.dirName());
    rootItem->setIcon(0, Fluent::icon(Fluent::IconType::FOLDER));
    rootItem->setData(0, Qt::UserRole, workspaceDir_);
    rootItem->setData(0, Qt::UserRole + 1, true);
    rootItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);

    populateDirChildren(fileTree_, rootItem, workspaceDir_, 0);
    rootItem->setExpanded(true);
}

/// 文件树项双击：打开对应文件到编辑器标签。
void Ide::onFileTreeItemActivated(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty())
        return;
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
        if (existingIdx >= 0)
            switchToTab(existingIdx);
        else
            loadFile(path);
    }
}

/// 文件树右键菜单：新建文件/文件夹、重命名、删除。
void Ide::onFileTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = fileTree_->itemAt(pos);
    QMenu menu(this);
    menu.setObjectName("fileTreeMenu");
    auto* newFileAct = menu.addAction(mlTr("新建文件"));
    auto* newFolderAct = menu.addAction(mlTr("新建文件夹"));
    menu.addSeparator();
    auto* renameAct = menu.addAction(mlTr("重命名"));
    auto* deleteAct = menu.addAction(mlTr("删除"));
    bool isRootOrEmpty = (!item || !item->parent());
    renameAct->setEnabled(!isRootOrEmpty);
    deleteAct->setEnabled(!isRootOrEmpty);

    // ---- 文件树右键菜单扩展：复制路径 / 复制相对路径 / 在资源管理器中显示 ----
    menu.addSeparator();
    QString itemPath = item ? item->data(0, Qt::UserRole).toString() : QString();
    auto* copyPathAct = menu.addAction(mlTr("复制路径"));
    auto* copyRelPathAct = menu.addAction(mlTr("复制相对路径"));
    auto* revealAct = menu.addAction(mlTr("在文件资源管理器中显示"));
    bool hasPath = !itemPath.isEmpty();
    copyPathAct->setEnabled(hasPath);
    copyRelPathAct->setEnabled(hasPath && !workspaceDir_.isEmpty());
    revealAct->setEnabled(hasPath);

    connect(copyPathAct, &QAction::triggered, this, [this, itemPath]() {
        if (itemPath.isEmpty())
            return;
        QApplication::clipboard()->setText(QDir::toNativeSeparators(itemPath));
    });
    connect(copyRelPathAct, &QAction::triggered, this, [this, itemPath]() {
        if (itemPath.isEmpty() || workspaceDir_.isEmpty())
            return;
        QDir baseDir(workspaceDir_);
        QString relPath = baseDir.relativeFilePath(itemPath);
        QApplication::clipboard()->setText(QDir::toNativeSeparators(relPath));
    });
    connect(revealAct, &QAction::triggered, this, [this, itemPath]() {
        if (itemPath.isEmpty())
            return;
#ifdef Q_OS_WIN
        QProcess::startDetached("explorer.exe", QStringList() << "/select," << QDir::toNativeSeparators(itemPath));
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(itemPath).absolutePath()));
#endif
    });

    auto* chosen = menu.exec(fileTree_->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == newFileAct)
        onNewFileInTree();
    else if (chosen == newFolderAct)
        onNewFolderInTree();
    else if (chosen == renameAct)
        onRenameInTree();
    else if (chosen == deleteAct)
        onDeleteInTree();
}

/// 在文件树目标目录新建 MiniLang 源文件。
void Ide::onNewFileInTree() {
    QTreeWidgetItem* cur = fileTree_->currentItem();
    QString targetDir = resolveTreeContextMenuTargetDir(fileTree_, cur);
    if (targetDir.isEmpty())
        targetDir = workspaceDir_;
    if (targetDir.isEmpty())
        return;
    bool ok = false;
    QString name = QInputDialog::getText(this, mlTr("新建文件"), mlTr("文件名 (将以 .mini 扩展名创建):"),
                                         QLineEdit::Normal, "untitled.mini", &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    name = name.trimmed();
    if (!name.endsWith(".mini") && !name.endsWith(".ml"))
        name += ".mini";
    if (name.contains("..") || name.contains('/') || name.contains('\\')) {
        InfoBar::warning(mlTr("非法文件名"), mlTr("文件名不得包含路径分隔符或父目录引用"), Qt::Horizontal, true, 2500,
                         InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    QString path = QDir(targetDir).filePath(name);
    QFile f(path);
    if (f.exists()) {
        InfoBar::warning(mlTr("已存在"), mlTr("文件已存在：") + name, Qt::Horizontal, true, 2500,
                         InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    if (!f.open(QIODevice::WriteOnly)) {
        InfoBar::warning(mlTr("错误"), mlTr("无法创建文件：") + f.errorString(), Qt::Horizontal, true, 2500,
                         InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    f.close();
    populateFileTree();
}

/// 在文件树目标目录新建文件夹。
void Ide::onNewFolderInTree() {
    QTreeWidgetItem* cur = fileTree_->currentItem();
    QString targetDir = resolveTreeContextMenuTargetDir(fileTree_, cur);
    if (targetDir.isEmpty())
        targetDir = workspaceDir_;
    if (targetDir.isEmpty())
        return;
    bool ok = false;
    QString name = QInputDialog::getText(this, mlTr("新建文件夹"), mlTr("文件夹名:"), QLineEdit::Normal, "", &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    name = name.trimmed();
    if (name.contains("..") || name.contains('/') || name.contains('\\')) {
        InfoBar::warning(mlTr("非法名称"), mlTr("名称不得包含路径分隔符或父目录引用"), Qt::Horizontal, true, 2500,
                         InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    QString path = QDir(targetDir).filePath(name);
    if (!QDir().mkdir(path)) {
        InfoBar::warning(mlTr("错误"), mlTr("无法创建文件夹（可能已存在）"), Qt::Horizontal, true, 2500,
                         InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    populateFileTree();
}

/// 重命名文件树中的文件或目录。
void Ide::onRenameInTree() {
    QTreeWidgetItem* cur = fileTree_->currentItem();
    if (!cur || !cur->parent())
        return;
    QString oldPath = cur->data(0, Qt::UserRole).toString();
    QString oldName = cur->text(0);
    bool ok = false;
    QString newName = QInputDialog::getText(this, mlTr("重命名"), mlTr("新名称:"), QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName == oldName)
        return;
    newName = newName.trimmed();
    if (newName.contains("..") || newName.contains('/') || newName.contains('\\')) {
        InfoBar::warning(mlTr("非法名称"), mlTr("名称不得包含路径分隔符或父目录引用"), Qt::Horizontal, true, 2500,
                         InfoBar::Position::TOP_RIGHT, this);
        return;
    }
    QString newPath = QFileInfo(oldPath).absolutePath() + "/" + newName;
    if (!QFile::rename(oldPath, newPath)) {
        InfoBar::warning(mlTr("错误"), mlTr("重命名失败"), Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT,
                         this);
        return;
    }
    populateFileTree();
}

/// 删除文件树选中的文件或目录（带确认）。
void Ide::onDeleteInTree() {
    QTreeWidgetItem* cur = fileTree_->currentItem();
    if (!cur || !cur->parent())
        return;
    QString path = cur->data(0, Qt::UserRole).toString();
    bool isDir = cur->data(0, Qt::UserRole + 1).toBool();
    auto ret = QMessageBox::question(this, mlTr("确认删除"), mlTr("确定删除 %1 ？").arg(cur->text(0)),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;
    bool ok = false;
    if (isDir)
        ok = QDir(path).removeRecursively();
    else
        ok = QFile::remove(path);
    if (!ok) {
        InfoBar::warning(mlTr("错误"), mlTr("删除失败"), Qt::Horizontal, true, 2500, InfoBar::Position::TOP_RIGHT,
                         this);
        return;
    }
    populateFileTree();
}

/// 打开工作区目录：设置根路径、刷新文件树并持久化。
void Ide::openWorkspace(const QString& dirPath) {
    QDir dir(dirPath);
    if (!dir.exists())
        return;
    workspaceDir_ = dir.absolutePath();
    hasWorkspace_ = true;
    QDir::setCurrent(workspaceDir_);

    QSettings settings("MiniLang", "MiniLang IDE");
    settings.setValue("workspace/path", workspaceDir_);

    addRecentWorkspace(workspaceDir_);
    populateFileTree();

    // 任务2：三栏布局 — 打开文件夹后显示编辑器（若无标签则显示欢迎页）
    if (editorTabWidget_ && editorTabWidget_->count() > 0) {
        ensureEditorVisible();
    } else {
        centerStack_->setCurrentWidget(welcomePage_);
        centerStack_->show();
    }
    // 第九轮：打开文件夹后自动展开左侧文件树面板
    switchLeftToFileTree();
    syncViewMenuChecks();
    updateWindowTitle();
}

// ============================================================
// Layout save/restore (ADS)
// ============================================================

/// 保存当前窗口布局（dock/分栏尺寸）到 QSettings（防抖）。
void Ide::saveLayout() {
    QSettings settings("MiniLang", "MiniLang IDE");
    if (dockManager_) {
        settings.setValue("layout/dockState", dockManager_->saveState());
        // R73 fix: 保存布局版本号，用于 objectName 变化时清除旧布局
        settings.setValue("layout/version", 2);
    }
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", QMainWindow::saveState());
    // 第九轮：记忆输出面板高度（最小 200px）
    if (bottomVisible_ && bottomContainer_) {
        int h = bottomContainer_->height();
        if (h >= 200 && h <= 1200) {
            bottomPanelHeight_ = h;
            settings.setValue("layout/bottomPanelHeight", h);
        }
    }
    // R60-2 fix: 持久化内部 splitter 尺寸（centerSplitter_ / editorSplitter_）
    // ADS saveState 只保存 dock 布局，不保存 dock 内部 widget 的 splitter 尺寸。
    // 不持久化则每次启动 centerSplitter_ 重置为 {420, 780}，用户调整丢失。
    if (centerSplitter_) {
        QList<int> sizes = centerSplitter_->sizes();
        if (sizes.size() >= 2) {
            settings.setValue("layout/centerSplitterSizes", QVariant::fromValue(sizes));
        }
    }
    if (editorSplitter_) {
        QList<int> sizes = editorSplitter_->sizes();
        if (sizes.size() >= 2) {
            settings.setValue("layout/editorSplitterSizes", QVariant::fromValue(sizes));
        }
    }
}

/// 从 QSettings 恢复上次保存的窗口布局。
void Ide::restoreLayout() {
    QSettings settings("MiniLang", "MiniLang IDE");
    // R60-2 fix: 跟踪是否有已保存的 dock 状态。无保存状态（首次启动）时
    // 才应用默认面板尺寸；有保存状态时由 restoreState 恢复用户上次布局，
    // 不再用硬编码尺寸覆盖——修复「每次都要自己调整面板大小」问题。
    // R65-3 fix: restoreState 延迟到 QTimer::singleShot(0) 中执行（事件循环
    // 开始后，窗口已完全布局，几何尺寸有效）。此前在 showEvent 中执行仍然
    // 太早（布局未完成），在构造函数中执行更早（窗口未 show）。
    // 保存到 pendingDockState_ 供 QTimer lambda 读取。
    bool hasSavedDockState = false;
    if (dockManager_) {
        // R73 fix: 布局版本号。dock widget objectName 从 i18n 改为固定ID时，
        // 旧版本保存的 dockState 无法匹配新 objectName，restoreState 会失败，
        // dock 变成浮动窗口。版本号变化时清除旧 dockState，应用默认布局。
        constexpr int kLayoutVersion = 2; // v1: i18n objectName; v2: 固定ID
        int savedVersion = settings.value("layout/version", 1).toInt();
        if (savedVersion < kLayoutVersion) {
            settings.remove("layout/dockState");
            settings.setValue("layout/version", kLayoutVersion);
            pendingDockState_.clear();
        } else {
            pendingDockState_ = settings.value("layout/dockState").toByteArray();
        }
        if (!pendingDockState_.isEmpty()) {
            hasSavedDockState = true;
        }
    }
    hasSavedLayout_ = hasSavedDockState;
    // 第十一轮：恢复输出面板记忆高度
    // BUG-R15-6 fix: 默认值从 600 改为 220，与 ide.h 声明一致。
    // 600px 在常见 800px 高度窗口下会占据 75% 垂直空间，导致编辑器区被挤压，
    // 表现为「输出面板全屏覆盖」。220px 是 VSCode 默认输出面板高度，合理且不遮挡编辑器。
    // 对历史已保存的过大值（>500）做一次性迁移，避免老用户继承 600px 配置。
    int savedH = settings.value("layout/bottomPanelHeight", 220).toInt();
    if (savedH > 500) {
        savedH = 220;
        settings.setValue("layout/bottomPanelHeight", 220);
    }
    if (savedH >= 200 && savedH <= 1200)
        bottomPanelHeight_ = savedH;
    QByteArray geometry = settings.value("window/geometry").toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    QByteArray winState = settings.value("window/state").toByteArray();
    if (!winState.isEmpty())
        QMainWindow::restoreState(winState);
    // R60-2 fix: 恢复内部 splitter 尺寸（centerSplitter_ / editorSplitter_）
    // ADS restoreState 只恢复 dock 布局，不恢复 dock 内部 widget 的 splitter 尺寸。
    if (centerSplitter_) {
        QVariant saved = settings.value("layout/centerSplitterSizes");
        if (saved.isValid() && saved.canConvert<QList<int>>()) {
            QList<int> sizes = saved.value<QList<int>>();
            if (sizes.size() >= 2 && sizes[0] >= 0 && sizes[1] >= 0)
                centerSplitter_->setSizes(sizes);
        }
    }
    if (editorSplitter_) {
        QVariant saved = settings.value("layout/editorSplitterSizes");
        if (saved.isValid() && saved.canConvert<QList<int>>()) {
            QList<int> sizes = saved.value<QList<int>>();
            if (sizes.size() >= 2 && sizes[0] >= 0 && sizes[1] >= 0)
                editorSplitter_->setSizes(sizes);
        }
    }
    // 第十一轮：恢复后重新隐藏右侧 dock 标题栏
    // R60-2 fix: 仅在无保存状态（首次启动）时应用默认面板尺寸。
    // 有保存状态时 restoreState 已恢复用户上次的布局，此处不得覆盖。
    // R65-3 fix: 将 restoreState 从 showEvent 移到此处（QTimer::singleShot(0)）。
    // showEvent 在窗口首次显示时触发，但此时布局尚未完成（几何尺寸虽已设置但
    // 未经过布局引擎处理），ADS restoreState 内部依赖容器几何尺寸计算 splitter
    // 比例，在无效几何下静默失败。QTimer::singleShot(0) 在事件循环开始后触发，
    // 所有 show/layout 事件已处理完毕，几何尺寸有效，restoreState 能正确恢复。
    QTimer::singleShot(0, this, [this, hasSavedDockState]() {
        // R65-3 fix: 在事件循环中执行 restoreState，此时窗口已完全布局
        bool restored = false;
        if (dockManager_ && !pendingDockState_.isEmpty()) {
            restored = dockManager_->restoreState(pendingDockState_);
        }
        if (rightDock_ && rightDock_->dockAreaWidget()) {
            rightDock_->dockAreaWidget()->setDockAreaFlag(ads::CDockAreaWidget::HideSingleWidgetTitleBar, true);
        }
        // R60-2/R65-3 fix: 首次启动或 restoreState 失败时应用默认面板尺寸
        // R72 fix: area->resize() 对 QSplitter 子控件无效——splitter 控制子控件
        // 尺寸，直接 resize 会被 splitter 在下次布局时覆盖。改用
        // parentSplitter()->setSizes() 设置比例尺寸。
        // R73 fix: 增大默认尺寸（左 280→320，右 560→600），避免用户觉得太小。
        if (!hasSavedDockState || !restored) {
            ads::CDockAreaWidget* leftArea =
                (fileTreeDock_ && !fileTreeDock_->isClosed()) ? fileTreeDock_->dockAreaWidget() : nullptr;
            ads::CDockAreaWidget* rightArea =
                (rightDock_ && !rightDock_->isClosed()) ? rightDock_->dockAreaWidget() : nullptr;
            // 若左右面板共享同一 splitter，需一次性设置所有尺寸
            if (leftArea && rightArea && leftArea->parentSplitter() == rightArea->parentSplitter()) {
                auto* sp = leftArea->parentSplitter();
                QList<int> sizes;
                for (int i = 0; i < sp->count(); ++i) {
                    if (sp->widget(i) == leftArea)
                        sizes.append(320);
                    else if (sp->widget(i) == rightArea)
                        sizes.append(600);
                    else
                        sizes.append(700);
                }
                sp->setSizes(sizes);
            } else {
                if (leftArea) {
                    if (auto* sp = leftArea->parentSplitter()) {
                        QList<int> sizes;
                        for (int i = 0; i < sp->count(); ++i)
                            sizes.append(sp->widget(i) == leftArea ? 320 : 700);
                        sp->setSizes(sizes);
                    }
                }
                if (rightArea) {
                    if (auto* sp = rightArea->parentSplitter()) {
                        QList<int> sizes;
                        for (int i = 0; i < sp->count(); ++i)
                            sizes.append(sp->widget(i) == rightArea ? 600 : 700);
                        sp->setSizes(sizes);
                    }
                }
            }
        }
        // PERF: 合并两次 applyFluentStyle 为一次，在 restoreState + 默认尺寸设置完成后调用。
        // 此前先调用一次再 restoreState 再调用一次，导致重复样式计算和全树遍历。
        // restoreState 可能创建新的 dock area 和 tab，在之后调用一次即可覆盖所有内容。
        applyFluentStyle();
        // R65-3 fix: restoreState 完成后再允许保存布局
        // 此前 firstShow_ 在 showEvent 中就设为 false，导致 restoreState 触发的
        // Resize 事件会启动 splitterSaveTimer_ 误保存，可能覆盖用户布局
        firstShow_ = false;

        // R58-1 fix: 仅在 bottomVisible_ 为 true 时恢复记忆高度。
        // 此前无条件 setFixedHeight 不会改变 visibility（QWidget::setFixedHeight
        // 不影响 visible 状态），但与 initUI 末尾的 hide() 配合时，必须保留
        // hide 状态。启动时 bottomVisible_ 为 false，无需操作；运行时若用户
        // 已展开底部面板（bottomVisible_ == true），才恢复其记忆高度。
        if (bottomContainer_ && bottomVisible_) {
            bottomContainer_->setFixedHeight(bottomPanelHeight_);
        }
    });
    // 第九轮：恢复后同步视图菜单勾选状态
    syncViewMenuChecks();
}

// ============================================================
// Panel control (consolidated docks + Pivot switching)
// ============================================================

/// 显示底部面板并切到指定标签页（输出/错误/字节码等）。
void Ide::showBottomPanel(int tabIndex) {
    if (!bottomContainer_)
        return;
    const bool wasHidden = !bottomVisible_;
    if (wasHidden) {
        // 在 editorSplitter_ 内部：通过动画 splitter 尺寸实现平滑展开
        const int targetH = bottomPanelHeight_;
        bottomContainer_->setFixedHeight(0); // 先设0高度，确保splitter有两项
        bottomContainer_->show();
        bottomVisible_ = true;

        if (editorSplitter_) {
            // 强制布局更新以获取正确的当前尺寸
            editorSplitter_->updateGeometry();
            QApplication::processEvents();

            QList<int> currentSizes = editorSplitter_->sizes();
            int editorH = currentSizes.size() > 0 ? currentSizes[0] : 600;
            QList<int> targetSizes = {editorH - targetH, targetH};
            if (targetSizes[0] < 100)
                targetSizes[0] = 100; // 保证编辑器最小高度

            // 动画插值：从全编辑器 → 编辑器+底部面板
            // ROUND-67 P1 fix: 改用成员变量 bottomPanelAnim_ 存储，closeEvent 中停止。
            // QVariantAnimation 不经过 Qt 事件队列（由 QUnifiedTimer 驱动），
            // removePostedEvents 无法清理，必须显式 stop() 才能停止。
            if (bottomPanelAnim_) {
                bottomPanelAnim_->stop();
                bottomPanelAnim_ = nullptr;
            }
            auto* anim = new QVariantAnimation(this);
            anim->setDuration(PanelAnimator::DURATION_MS);
            anim->setStartValue(0.0);
            anim->setEndValue(1.0);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            QObject::connect(
                anim, &QVariantAnimation::valueChanged, this, [this, currentSizes, targetSizes](const QVariant& value) {
                    if (!editorSplitter_)
                        return;
                    double t = value.toDouble();
                    QList<int> interpolated;
                    for (int i = 0; i < currentSizes.size() && i < targetSizes.size(); ++i) {
                        interpolated.append(static_cast<int>(currentSizes[i] * (1 - t) + targetSizes[i] * t));
                    }
                    editorSplitter_->setSizes(interpolated);
                });
            QObject::connect(anim, &QVariantAnimation::finished, this, [this, targetH]() {
                if (bottomContainer_)
                    bottomContainer_->setFixedHeight(targetH);
                bottomPanelAnim_ = nullptr;
            });
            bottomPanelAnim_ = anim;
            anim->start(QAbstractAnimation::DeleteWhenStopped);
        } else {
            bottomContainer_->setFixedHeight(targetH);
        }
    }
    static const char* keys[] = {"output", "errors", "repl"};
    if (tabIndex < 0 || tabIndex >= 3)
        tabIndex = 0;
    if (bottomPivot_)
        bottomPivot_->setCurrentItem(keys[tabIndex]);
    syncViewMenuChecks();
    if (wasHidden && bottomStack_ && bottomStack_->currentWidget()) {
        PanelAnimator::slideInWidget(bottomStack_->currentWidget(), PanelAnimator::DURATION_MS);
    }
}

/// 隐藏底部面板。
void Ide::hideBottomPanel() {
    if (!bottomContainer_)
        return;
    // R58-1 fix: 增加实际可见性兜底。若 bottomVisible_ 状态与 bottomContainer_
    // 实际可见性不一致（如历史 bug 残留），只要 bottomContainer_ 仍可见就强制隐藏，
    // 避免关闭按钮失效（用户点击无反应）。
    if (!bottomVisible_ && !bottomContainer_->isVisible()) {
        syncViewMenuChecks();
        return;
    }
    if (bottomVisible_ || bottomContainer_->isVisible()) {
        // 记忆当前高度
        QList<int> currentSizes = editorSplitter_ ? editorSplitter_->sizes() : QList<int>();
        int bottomH = currentSizes.size() > 1 ? currentSizes[1] : bottomContainer_->height();
        if (bottomH >= 100 && bottomH <= 1200)
            bottomPanelHeight_ = bottomH;

        bottomVisible_ = false;

        if (editorSplitter_ && currentSizes.size() >= 2) {
            // 动画插值：从当前分配 → 全编辑器（底部面板高度归零）
            int editorH = currentSizes[0];
            QList<int> targetSizes = {editorH + bottomH, 0};

            // ROUND-67 P1 fix: 改用成员变量 bottomPanelAnim_ 存储，closeEvent 中停止。
            if (bottomPanelAnim_) {
                bottomPanelAnim_->stop();
                bottomPanelAnim_ = nullptr;
            }
            auto* anim = new QVariantAnimation(this);
            anim->setDuration(PanelAnimator::DURATION_MS);
            anim->setStartValue(0.0);
            anim->setEndValue(1.0);
            anim->setEasingCurve(QEasingCurve::InCubic);
            QObject::connect(
                anim, &QVariantAnimation::valueChanged, this, [this, currentSizes, targetSizes](const QVariant& value) {
                    if (!editorSplitter_)
                        return;
                    double t = value.toDouble();
                    QList<int> interpolated;
                    for (int i = 0; i < currentSizes.size() && i < targetSizes.size(); ++i) {
                        interpolated.append(static_cast<int>(currentSizes[i] * (1 - t) + targetSizes[i] * t));
                    }
                    editorSplitter_->setSizes(interpolated);
                });
            QObject::connect(anim, &QVariantAnimation::finished, this, [this]() {
                if (bottomContainer_)
                    bottomContainer_->hide();
                bottomPanelAnim_ = nullptr;
            });
            bottomPanelAnim_ = anim;
            anim->start(QAbstractAnimation::DeleteWhenStopped);
        } else {
            // 兜底：无 editorSplitter_ 时直接隐藏
            bottomContainer_->hide();
        }
    }
    syncViewMenuChecks();
}

/// 显示右侧面板并切到指定标签页（调试/IR/Token 等）。
void Ide::showRightPanel(int tabIndex) {
    if (!rightDock_)
        return;
    const bool wasClosed = rightDock_->isClosed();
    if (wasClosed) {
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
    if (tabIndex < 0 || tabIndex >= 3)
        tabIndex = 0;
    if (rightPivot_)
        rightPivot_->setCurrentItem(keys[tabIndex]);
    syncViewMenuChecks();
    // issue 2: 用 slideInWidget 替代 fadeInWidget（O(1) pos 动画）
    if (wasClosed && rightStack_ && rightStack_->currentWidget()) {
        PanelAnimator::slideInWidget(rightStack_->currentWidget(), PanelAnimator::DURATION_MS);
    }
}

/// 隐藏右侧面板。
void Ide::hideRightPanel() {
    if (rightDock_)
        rightDock_->toggleView(false);
    syncViewMenuChecks();
}

/// 切换底部面板显隐。
void Ide::toggleBottomPanel() {
    if (bottomVisible_)
        hideBottomPanel();
    else
        showBottomPanel();
}

/// 切换右侧面板显隐。
void Ide::toggleRightPanel() {
    if (!rightDock_)
        return;
    rightDock_->toggleView(!rightDock_->isClosed() ? false : true);
}

/// 左侧区域切到文件树。
void Ide::switchLeftToFileTree() {
    if (fileTreeDock_ && fileTreeDock_->isClosed())
        fileTreeDock_->toggleView(true);
    if (fileTreeDock_)
        fileTreeDock_->setAsCurrentTab();
    if (activityBar_)
        activityBar_->setCurrentIndex(0);
    // R60-2 fix: 首次启动（无保存布局）时，左侧 dock 首次打开应用默认宽度 280px
    ensureLeftDockDefaultSize();
    syncViewMenuChecks();
}

/// 左侧区域切到调试面板。
void Ide::switchLeftToDebugPanel() {
    if (debugPanelDock_ && debugPanelDock_->isClosed())
        debugPanelDock_->toggleView(true);
    if (debugPanelDock_)
        debugPanelDock_->setAsCurrentTab();
    if (activityBar_)
        activityBar_->setCurrentIndex(1);
    // R60-2 fix: 首次启动（无保存布局）时，左侧 dock 首次打开应用默认宽度 280px
    ensureLeftDockDefaultSize();
    syncViewMenuChecks();
}

/// R60-2 fix: 首次启动（无保存布局）时，左侧 dock 首次打开应用默认宽度。
/// fileTreeDock_ 与 debugPanelDock_ 共享同一 dock area，只需对任一 resize 即可。
/// 仅在 hasSavedLayout_ == false 且 leftDockDefaultSized_ == false 时执行一次。
void Ide::ensureLeftDockDefaultSize() {
    if (hasSavedLayout_ || leftDockDefaultSized_)
        return;
    leftDockDefaultSized_ = true; // 标记已应用，避免每次切换 tab 都 resize
    QTimer::singleShot(0, this, [this]() {
        // fileTreeDock_ 与 debugPanelDock_ 在同一 dock area，取任一即可
        if (fileTreeDock_ && !fileTreeDock_->isClosed()) {
            if (auto* area = fileTreeDock_->dockAreaWidget()) {
                area->resize(280, area->height());
            }
        }
    });
}

/// 打开独立的 AST 查看窗口。
void Ide::showAstWindow() {
    if (!astWindow_)
        return;
    restoreAstWindowGeometry();
    if (astWindow_->isHidden()) {
        astWindow_->show();
    } else {
        astWindow_->raise();
        astWindow_->activateWindow();
    }
}

/// 持久化 AST 窗口的几何位置与尺寸。
void Ide::saveAstWindowGeometry() {
    if (!astWindow_)
        return;
    QSettings settings("MiniLang", "MiniLang IDE");
    settings.setValue("astWindow/geometry", astWindow_->saveGeometry());
}

/// 恢复 AST 窗口的几何位置与尺寸。
void Ide::restoreAstWindowGeometry() {
    if (!astWindow_)
        return;
    QSettings settings("MiniLang", "MiniLang IDE");
    QByteArray geo = settings.value("astWindow/geometry").toByteArray();
    if (!geo.isEmpty())
        astWindow_->restoreGeometry(geo);
}

/// 显示/隐藏调试控制按钮（运行/单步/停止）。
void Ide::showDebugButtons(bool show) {
    if (debugButtonsVisible_ == show)
        return;
    debugButtonsVisible_ = show;
    if (debugButtonContainer_)
        debugButtonContainer_->setVisible(show);
    // 第八轮：分隔线随容器显隐，禁止灰化占位
    if (debugSepAction_)
        debugSepAction_->setVisible(show);
    // P-IDE-4 fix: 调试按钮与 VM 按钮互斥——进入树遍历调试时隐藏 VM 按钮组，
    // 避免两套按钮同时残留导致工具栏重复拥挤。
    if (show && vmButtonContainer_ && vmButtonContainer_->isVisible()) {
        if (vmButtonContainer_)
            vmButtonContainer_->setVisible(false);
        if (vmSepAction_)
            vmSepAction_->setVisible(false);
    }
}

/// 显示/隐藏 VM 单步控制按钮。
void Ide::showVmButtons(bool show) {
    if (vmButtonContainer_)
        vmButtonContainer_->setVisible(show);
    if (vmSepAction_)
        vmSepAction_->setVisible(show);
    // 第八轮：VM 调试面板（操作数栈/全局变量）仅 VM 单步模式时显示
    if (vmStackPanel_)
        vmStackPanel_->setVisible(show);
    // P-IDE-4 fix: VM 按钮与调试按钮互斥——进入字节码调试时隐藏树遍历调试按钮组。
    if (show && debugButtonsVisible_) {
        debugButtonsVisible_ = false;
        if (debugButtonContainer_)
            debugButtonContainer_->setVisible(false);
        if (debugSepAction_)
            debugSepAction_->setVisible(false);
    }
}

// ============================================================
// Output helpers
// ============================================================

/// 向输出面板追加文本（按级别着色并处理自动滚动）。
void Ide::appendOutput(const QString& text, OutputLevel level) {
    // R15-3: VSCode 风格输出——文本级别前缀替代 Unicode 图标，更简洁的终端式排版。
    //   Plain（用户程序输出）：无前缀无时间戳，纯净正文
    //   Info/Success/Warning/Error：[HH:mm:ss] [Level] 正文，级别前缀着色
    // 颜色沿用中性白主题语义色（R74: 文本色对齐中性灰，语义强调色保留 Solarized）。
    QString escaped = text.toHtmlEscaped();

    // 亮色主题固定语义色（与 TeachingTheme::xxx() 亮色值一致）
    static const char* kTs = "color:#8C8C8C;";      // 次要时间戳（中性浅灰）
    static const char* kInfo = "color:#268BD2;";    // blue
    static const char* kSuccess = "color:#859900;"; // green
    static const char* kWarn = "color:#B58900;";    // yellow
    static const char* kError = "color:#DC322F;";   // red
    static const char* kBody = "color:#1E1E1E;";    // 主文本（中性深灰）

    QString html;
    if (level == OutputLevel::Plain) {
        // 用户程序输出：纯净正文，无前缀无时间戳（对齐 VSCode 终端行为）
        html = QString("<p style='margin:1px 0;'><span style='%1'>%2</span></p>").arg(kBody, escaped);
    } else {
        // 系统消息：时间戳 + 级别前缀 + 正文
        QString timestamp = QDateTime::currentDateTime().toString("[HH:mm:ss]");
        QString prefix;
        const char* prefixStyle = kBody;
        const char* bodyStyle = kBody;
        switch (level) {
        case OutputLevel::Info:
            prefix = QStringLiteral("[Info]");
            prefixStyle = kInfo;
            break;
        case OutputLevel::Success:
            prefix = QStringLiteral("[Done]");
            prefixStyle = kSuccess;
            break;
        case OutputLevel::Warning:
            prefix = QStringLiteral("[Warn]");
            prefixStyle = kWarn;
            bodyStyle = kWarn;
            break;
        case OutputLevel::ErrorMsg:
            prefix = QStringLiteral("[Error]");
            prefixStyle = kError;
            bodyStyle = kError;
            break;
        default:
            break;
        }
        html = QString("<p style='margin:1px 0;'>"
                       "<span style='%1'>%2</span> "
                       "<span style='%3'>%4</span> "
                       "<span style='%5'>%6</span>"
                       "</p>")
                   .arg(kTs, timestamp, prefixStyle, prefix, bodyStyle, escaped);
    }

    outputTextEdit_->append(html);
}

/// 向错误列表追加诊断项（带行/列与严重级别）。
void Ide::appendError(const QString& text, int line, int column, DiagLevel level) {
    if (!errorPanelHasErrors_) {
        errorListWidget_->clear();
        errorPanelHasErrors_ = true;
    }
    auto* item = new QListWidgetItem();

    // DiagLevel -> icon character + color
    // 注意：以下 4 色与 TeachingTheme::error()/warning()/info()/hint() 语义一致，
    // 此处保留 const char* 是因为 RichTextItemDelegate 需要 const char* 拼接到 HTML；
    // 未来主题感知时改为 QColor::name() 拼接。
    const char* iconChar;
    const char* iconColor;
    switch (level) {
    case DiagLevel::Error:
        iconChar = "\u25CF";
        iconColor = "#DC322F";
        break; // ●  = TeachingTheme::error()
    case DiagLevel::Warning:
        iconChar = "\u25D0";
        iconColor = "#B58900";
        break; // ◐  ≈ TeachingTheme::warning()
    case DiagLevel::Info:
        iconChar = "\u25CB";
        iconColor = "#268BD2";
        break; // ○  = TeachingTheme::info()
    case DiagLevel::Hint:
        iconChar = "\u25C7";
        iconColor = "#8C8C8C";
        break; // ◇  = TeachingTheme::hint()（中性浅灰）
    }

    const char* textColor = (level == DiagLevel::Error) ? "#DC322F" : // = TeachingTheme::error()
                                (level == DiagLevel::Warning) ? "#B58900"
                                                              : // ≈ TeachingTheme::warning()
                                (level == DiagLevel::Hint) ? "#8C8C8C"
                                                           : // = TeachingTheme::hint()（中性浅灰）
                                "#268BD2";                   // Solarized blue

    // Build rich-text HTML (all inline styles, no class selectors)
    QString html = QString("<table style='width:100%;border-collapse:collapse;border-spacing:0;'>"
                           "<tr>"
                           "  <td style='width:22px;color:%1;font-size:14px;'>%2</td>"
                           "  <td style='color:%3;'>%4</td>"
                           "</tr></table>")
                       .arg(iconColor, iconChar, textColor, text.toHtmlEscaped());

    item->setData(RichTextItemDelegate::kHtmlRole, html);
    item->setData(Qt::UserRole, line);                        // keep click-to-goto-line working
    item->setData(Qt::UserRole + 3, static_cast<int>(level)); // for badge count
    item->setToolTip(text);

    errorListWidget_->addItem(item);
    updateErrorBadge();
    applyErrorFilter(); // 新增项需遵循当前过滤状态
}

/// 清空输出面板与错误列表。
void Ide::clearOutput() {
    outputTextEdit_->clear();
    errorListWidget_->clear();
    errorPanelHasErrors_ = false;
    updateErrorBadge();
}

/// 更新错误计数徽标（仅在有错误时显示）。
void Ide::updateErrorBadge() {
    int errors = 0, warnings = 0;
    for (int i = 0; i < errorListWidget_->count(); ++i) {
        int lv = errorListWidget_->item(i)->data(Qt::UserRole + 3).toInt();
        switch (static_cast<DiagLevel>(lv)) {
        case DiagLevel::Error:
            errors++;
            break;
        case DiagLevel::Warning:
            warnings++;
            break;
        default:
            break;
        }
    }
    QString label = mlTr("\u95EE\u9898"); // 问题
    if (errors > 0 || warnings > 0)
        label += QString(" (%1/%2)").arg(errors).arg(warnings);
    bottomPivot_->setItemText("errors", label);
}

/// 按当前过滤器（错误/警告）刷新错误列表显示。
void Ide::applyErrorFilter() {
    if (!errorListWidget_)
        return;
    bool showErr = errFilterErrorBtn_ && errFilterErrorBtn_->isChecked();
    bool showWarn = errFilterWarningBtn_ && errFilterWarningBtn_->isChecked();
    bool showInfo = errFilterInfoBtn_ && errFilterInfoBtn_->isChecked();
    bool showHint = errFilterHintBtn_ && errFilterHintBtn_->isChecked();
    for (int i = 0; i < errorListWidget_->count(); ++i) {
        auto* item = errorListWidget_->item(i);
        if (!item)
            continue;
        int lv = item->data(Qt::UserRole + 3).toInt();
        bool show = false;
        switch (static_cast<DiagLevel>(lv)) {
        case DiagLevel::Error:
            show = showErr;
            break;
        case DiagLevel::Warning:
            show = showWarn;
            break;
        case DiagLevel::Info:
            show = showInfo;
            break;
        case DiagLevel::Hint:
            show = showHint;
            break;
        }
        item->setHidden(!show);
    }
}

/// “清空输出”按钮槽：清空输出与错误面板。
void Ide::onClearOutput() {
    clearOutput();
}

// 深色主题已移除：onThemeToggle slot 已删除

// ============================================================
// Run / Debug
// ============================================================

/// 运行/调试前拦截：执行前端管线并检查是否含编译错误，有错误则展示诊断并返回 true。
bool Ide::blockIfHasErrors() {
    // 第八轮：运行/调试前拦截编译错误，0 错误才允许执行
    if (!codeEditor_)
        return false;
    std::string source = codeEditor_->toPlainText().toStdString();
    auto result = controller_->runFrontendPipeline(source);
    if (result.diagnostics && result.diagnostics->hasErrors()) {
        displayDiagnostics(*result.diagnostics);
        showBottomPanel(1); // 错误标签
        return true;
    }
    // 清除残留的错误标记
    codeEditor_->clearErrorLines();
    return false;
}

/// 实时语法检查（300ms 防抖触发）：重跑前端管线，清空旧错误并刷新错误列表与波浪下划线。
void Ide::runRealTimeSyntaxCheck() {
    // 第八轮：实时语法检查（300ms 防抖触发）
    // 清空旧错误 → 全量扫描 → 按行号排序展示 → 更新波浪下划线（常驻不消失）
    if (!codeEditor_)
        return;

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
    std::stable_sort(sortedDiags.begin(), sortedDiags.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.line != b.line)
            return a.line < b.line;
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
                auto suggestion = SpellChecker::suggestSuffix(ident, spellCandidates_, 2);
                if (!suggestion.empty()) {
                    msg += suggestion;
                }
            }
        }

        // 构建展示文本：[级别] (行 X, 列 Y): 消息
        QString text = QString::fromStdString("[" + diag.sourceString() + "] " + diag.levelString());
        if (diag.line > 0) {
            text += mlTr(" (行 %1").arg(diag.line);
            if (diag.column > 0)
                text += mlTr(", 列 %1").arg(diag.column);
            text += ")";
        }
        text += ": " + QString::fromStdString(msg);

        if (diag.isError()) {
            appendError(text, diag.line, diag.column, diag.level);
            hasErrors = true;
            if (diag.line > 0) {
                ranges.push_back({diag.line, diag.column, 0});
            }
        } else if (diag.isWarning()) {
            appendOutput(text, OutputLevel::Warning);
        } else {
            appendOutput(text, OutputLevel::Info);
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

/// 运行按钮槽：校验 REPL 状态、清空旧输出/错误标记、拦截编译错误后，
/// 通过 controller_->prepareRun 启动 WorkerManager 执行当前源码。
void Ide::onRun() {
    if (!codeEditor_)
        return;
    std::string source = codeEditor_->toPlainText().toStdString();

    if (replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请等待其完成后再运行"));
        showBottomPanel(1);
        return;
    }

    // R53-2 fix: 运行守卫必须在破坏性清理（clearOutput/clearAll/clearErrorLines）之前。
    // 原实现依赖 prepareRun 内部的 isRunning 守卫，但该守卫在 clearOutput/clearAll
    // 之后才执行——若用户在运行/调试中误按 F5，会先清空调试上下文（断点高亮、
    // 调用栈、变量快照、当前行高亮），再被 prepareRun 拒绝，导致调试会话状态丢失。
    // F5/F6 工具栏 action 虽已禁用，但菜单 action 与键盘快捷键仍可能触发本槽。
    if (controller_->isRunning() || controller_->isVmRunning() || controller_->isDebugPaused()) {
        appendError(mlTr("已有运行或调试在进行，请先停止当前运行再启动新运行"));
        showBottomPanel(1);
        return;
    }

    clearOutput();
    debugPanel_->clearAll();
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();
    codeEditor_->clearSourceHighlight();

    // 第八轮：存在编译错误时拦截运行
    if (blockIfHasErrors())
        return;

    if (!controller_->prepareRun(false, source, currentFilePath_.toStdString()))
        return;

    updateTokenTable();
    updateAstViewer();
    setRunningState(true);
    replPanel_->setInputEnabled(false);

    try {
        controller_->startWorker();
    } catch (const std::exception& e) {
        // BUG-ORCH-1 fix: startWorker 异常后必须执行业务层清理（forceStop 重置
        // WorkerManager::isRunning_ / isDebugRun_、restoreReplState、reset debugger），
        // 否则 isRunning_ 永久卡死，IDE 无法再次运行。
        // 注意：workerThread_ 未真正启动（start 抛异常），forceStop 内 wait(5000)
        // 立即返回 true，不会阻塞 UI。
        try {
            controller_->forceStop();
        } catch (...) {
        }
        appendError(mlTr("启动失败: %1").arg(e.what()));
        showBottomPanel(1);
        setRunningState(false);
        replPanel_->setInputEnabled(true);
    } catch (...) {
        try {
            controller_->forceStop();
        } catch (...) {
        }
        appendError(mlTr("启动发生未知异常"));
        showBottomPanel(1);
        setRunningState(false);
        replPanel_->setInputEnabled(true);
    }
}

/// 调试按钮槽：与 onRun 类似，但以调试模式启动；收集编辑器断点及条件表达式，
/// 通过 controller_->setupDebug 配置调试器后启动 worker。
void Ide::onDebug() {
    if (!codeEditor_)
        return;
    std::string source = codeEditor_->toPlainText().toStdString();

    if (replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请等待其完成后再调试"));
        showBottomPanel(1);
        return;
    }

    // R53-2 fix: 运行守卫必须在破坏性清理之前（同 onRun）。
    if (controller_->isRunning() || controller_->isVmRunning() || controller_->isDebugPaused()) {
        appendError(mlTr("已有运行或调试在进行，请先停止当前运行再启动新调试"));
        showBottomPanel(1);
        return;
    }

    clearOutput();
    debugPanel_->clearAll();
    codeEditor_->clearErrorLines();
    codeEditor_->clearCurrentLine();
    codeEditor_->clearSourceHighlight();

    // 第八轮：存在编译错误时拦截调试
    if (blockIfHasErrors())
        return;

    if (!controller_->prepareRun(true, source, currentFilePath_.toStdString()))
        return;

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
        if (!cond.empty())
            conditions[line] = cond;
    }

    try {
        controller_->setupDebug(breakpoints, conditions);
        controller_->startWorker();
    } catch (const std::exception& e) {
        // BUG-ORCH-1 fix: startWorker 异常后必须执行业务层清理（对齐 onRun）
        try {
            controller_->forceStop();
        } catch (...) {
        }
        appendError(mlTr("启动调试失败: %1").arg(e.what()));
        showBottomPanel(1);
        setRunningState(false);
        replPanel_->setInputEnabled(true);
        switchLeftToFileTree();
        showDebugButtons(false);
    } catch (...) {
        try {
            controller_->forceStop();
        } catch (...) {
        }
        appendError(mlTr("启动调试发生未知异常"));
        showBottomPanel(1);
        setRunningState(false);
        replPanel_->setInputEnabled(true);
        switchLeftToFileTree();
        showDebugButtons(false);
    }
}

/// 调试单步进入：请求 DebugController 步入下一行。
void Ide::onStepIn() {
    if (!codeEditor_)
        return;
    // BUG-DBG-AUDIT-8 fix: Interpreter 单步与 REPL 异步执行并发会竞争 Interpreter 的
    // currentEnv_/callStack_ 等非线程安全字段。对齐 onVmStep 的 BUG-ORCH-8 fix 检查。
    if (replPanel_ && replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请先停止 REPL 再使用单步"));
        showBottomPanel(1);
        return;
    }
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepIn();
    } catch (const std::exception& e) {
        appendError(mlTr("单步进入失败: %1").arg(e.what()));
    } catch (...) {
        appendError(mlTr("单步进入发生未知异常"));
    }
}

/// 调试单步跳过：请求 DebugController 步过当前行。
void Ide::onStepOver() {
    if (!codeEditor_)
        return;
    // BUG-DBG-AUDIT-8 fix: 同 onStepIn，REPL 执行期间拒绝 Interpreter 单步
    if (replPanel_ && replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请先停止 REPL 再使用单步"));
        showBottomPanel(1);
        return;
    }
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepOver();
    } catch (const std::exception& e) {
        appendError(mlTr("单步跳过失败: %1").arg(e.what()));
    } catch (...) {
        appendError(mlTr("单步跳过发生未知异常"));
    }
}

/// 调试单步跳出：请求 DebugController 步出当前函数。
void Ide::onStepOut() {
    if (!codeEditor_)
        return;
    // BUG-DBG-AUDIT-8 fix: 同 onStepIn，REPL 执行期间拒绝 Interpreter 单步
    if (replPanel_ && replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请先停止 REPL 再使用单步"));
        showBottomPanel(1);
        return;
    }
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->stepOut();
    } catch (const std::exception& e) {
        appendError(mlTr("单步跳出失败: %1").arg(e.what()));
    } catch (...) {
        appendError(mlTr("单步跳出发生未知异常"));
    }
}

/// 调试继续：请求 DebugController 恢复运行直到下一断点。
void Ide::onResume() {
    if (!codeEditor_)
        return;
    // BUG-DBG-AUDIT-8 fix: 同 onStepIn，REPL 执行期间拒绝 Interpreter 继续
    if (replPanel_ && replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请先停止 REPL 再使用单步"));
        showBottomPanel(1);
        return;
    }
    try {
        controller_->setBreakpoints(codeEditor_->getBreakpoints());
        controller_->resume();
    } catch (const std::exception& e) {
        appendError(mlTr("继续执行失败: %1").arg(e.what()));
    } catch (...) {
        appendError(mlTr("继续执行发生未知异常"));
    }
}

/// 停止运行/调试：请求 WorkerManager 停止 worker 线程。
void Ide::onStop() {
    controller_->stop();
}

/// 调试暂停信号槽：解释器在断点暂停时触发，高亮当前行、刷新调试信息与面板。
void Ide::onPausedAt(int line) {
    // ISSUE-7 fix: 关闭流程中丢弃调试暂停信号，避免访问已部分析构的成员。
    if (closing_)
        return;
    // AUDIT-P1-CORRECT fix: stale 信号防御。
    // closeEvent 期间 stopForClose 唤醒 worker 退出，但 doPause 中已投递的 pausedAt
    // QueuedConnection 仍在主线程队列。Ide 析构时处理 pending 事件会访问已析构的成员
    // （codeEditor_/debugPanel_ 等），try/catch 无法捕获 UAF。
    // 检查 controller_ 运行/暂停状态：worker 已停止且非调试暂停时说明是 stale 信号，丢弃。
    if (!controller_->isRunning() && !controller_->isDebugPaused())
        return;
    try {
        if (codeEditor_)
            codeEditor_->setCurrentLine(line);
        updateDebugInfo();
        switchLeftToDebugPanel();
        showDebugButtons(true);
        showBottomPanel(0);
    } catch (const std::exception& e) {
        appendError(mlTr("调试信息更新失败: %1").arg(e.what()));
    } catch (...) {
        appendError(mlTr("调试信息更新发生未知异常"));
    }
}

/// Worker 结束信号槽：恢复运行态 UI、清理调试面板残留数据（避免陈旧调用栈/变量）。
void Ide::onWorkerFinished(bool wasDebug) {
    // ISSUE-7 fix: 关闭流程中丢弃 worker 完成信号，避免访问已部分析构的成员。
    if (closing_)
        return;
    setRunningState(false);
    if (codeEditor_) {
        codeEditor_->clearCurrentLine();
        codeEditor_->clearErrorLines();
    }
    replPanel_->setInputEnabled(true);
    if (wasDebug) {
        switchLeftToFileTree();
        showDebugButtons(false);
        // BUG-DBG-G2 fix (P2): worker 结束后清理调用栈与变量树，避免上次调试会话
        // 的陈旧数据残留显示。原 onRun/onDebug 入口虽已 clearAll()，但若 worker
        // 异常结束或用户停止，残留的调用栈/变量会误导用户以为仍在调试中。
        debugPanel_->clearAll();
    }
    // IDE-DBG-01 fix: 非 debug 异常结束也清理 debugPanel_，避免用户之前手动展开
    // debug 面板时残留的调用栈/变量数据误导用户以为仍在调试。
    // 注：wasDebug=false 路径原仅 setRunningState(false) 不清理 debugPanel_，
    // 若用户在 debug 面板可见时启动普通 Run 并异常结束，残留数据会显示。
    if (!wasDebug && debugPanel_) {
        debugPanel_->clearAll();
    }
}

// ============================================================
// Formatting
// ============================================================

/// “格式化”按钮槽：运行前端管线后调用 Formatter 重写当前编辑器内容。
void Ide::onFormat() {
    if (!codeEditor_)
        return;
    std::string source = codeEditor_->toPlainText().toStdString();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard {
        ~CursorGuard() { QApplication::restoreOverrideCursor(); }
    } guard;

    auto pipelineResult = controller_->runFrontendPipeline(source);
    updateTokenTable();

    if (pipelineResult.status != IdeController::PipelineStatus::OK) {
        if (!pipelineResult.errorMessage.empty()) {
            appendError(mlTr("[格式化] %1: %2")
                            .arg(pipelineResult.status == IdeController::PipelineStatus::LexerFailed ? mlTr("词法异常")
                                                                                                     : mlTr("解析异常"))
                            .arg(QString::fromStdString(pipelineResult.errorMessage)));
            showBottomPanel(1);
        } else if (pipelineResult.diagnostics) {
            for (const auto& diag : pipelineResult.diagnostics->all()) {
                appendError(mlTr("[格式化] ") + QString::fromStdString(diag.format()));
            }
            showBottomPanel(1);
        }
        updateAstViewer();
        return;
    }
    updateAstViewer();

    if (!controller_->astRoot())
        return;

    QSet<int> bps;
    if (codeEditor_)
        bps = codeEditor_->getBreakpoints();
    bool hadBreakpoints = !bps.isEmpty();
    QTextCursor savedCursor = codeEditor_->textCursor();
    int scrollPos = codeEditor_->verticalScrollBar()->value();

    std::string formatted;
    try {
        if (!controller_->formatCode(formatted))
            return;
        codeEditor_->setPlainText(QString::fromStdString(formatted));
    } catch (const std::exception& e) {
        appendError(mlTr("[格式化] 格式化异常: %1").arg(e.what()));
        showBottomPanel(1);
        return;
    }

    if (hadBreakpoints) {
        appendOutput(mlTr("[格式化] 断点已清除（行号变化，断点不再有效）"));
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

/// “编译分析”按钮槽：执行编译并呈现字节码/IR/诊断。
void Ide::onCompileAnalysis() {
    if (!codeEditor_) {
        InfoBar::warning(mlTr("编译分析"), mlTr("请先打开或新建一个文件再进行编译分析。"), Qt::Horizontal, true, 3000,
                         InfoBar::Position::TOP_RIGHT, this);
        return;
    }

    // R53-3 fix: 运行/调试中拒绝重跑前端管线。原实现先 clearErrorLines/clearCurrentLine
    // 再 runFrontendPipeline，会清掉调试暂停时的当前行高亮、并替换 pipeline_ 内部
    // lexer/parser/astRoot 状态——若 worker 线程正在使用同一 pipeline_ 解析模块
    // 源码（import 路径），将产生并发数据竞争（lexer/parser 非线程安全）。
    // 仅打开右侧可视化面板查看当前已编译结果，不重新执行管线。
    if (controller_->isRunning() || controller_->isVmRunning() || controller_->isDebugPaused()) {
        InfoBar::warning(mlTr("编译分析"), mlTr("已有运行或调试在进行，编译分析已跳过——请先停止当前运行再重试。"),
                         Qt::Horizontal, true, 3000, InfoBar::Position::TOP_RIGHT, this);
        showRightPanel(0);
        return;
    }

    std::string source = codeEditor_->toPlainText().toStdString();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    // R53-UX5 fix: 同步编译阶段状态栏进度反馈。原实现仅 WaitCursor 鼠标反馈，
    // 状态栏无文字提示——大文件编译时用户感知不到进度。RAII guard 统一管理
    // cursor 与状态栏消息，覆盖所有 return 路径。
    struct CursorStatusGuard {
        Ide* self;
        ~CursorStatusGuard() {
            QApplication::restoreOverrideCursor();
            if (auto* sb = self->statusBar())
                sb->clearMessage();
        }
    } guard{this};
    if (auto* sb = statusBar())
        sb->showMessage(mlTr("正在编译分析..."), 0);

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
                            .arg(pipelineResult.status == IdeController::PipelineStatus::LexerFailed ? mlTr("词法异常")
                                                                                                     : mlTr("解析异常"))
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

/// 右侧面板标签页切换：按需刷新对应可视化内容。
void Ide::onRightTabChanged(int index) {
    // Legacy slot retained for header compatibility.
    // Right-panel switching is now driven by onRightPivotChanged.
    Q_UNUSED(index);
}

/// 活动栏项切换（按索引路由到对应面板/动作）。
void Ide::onActivityChanged(int index) {
    // P0.5 注册制：保留为兼容存根；实际分发由 onActivityChangedById 处理
    Q_UNUSED(index);
}

/// 活动栏项切换（按面板 id 路由）。
void Ide::onActivityChangedById(const QString& id) {
    // P0.5 注册制：新增面板只需在此追加 else-if 分支
    // 三面板完全互斥：每个分支显式隐藏另外两个 dock，显示自己的 dock
    ads::CDockWidget* targetDock = nullptr;

    if (id == "explorer") {
        targetDock = fileTreeDock_;
        if (debugPanelDock_ && !debugPanelDock_->isClosed())
            debugPanelDock_->toggleView(false);
        if (teachingTreeDock_ && !teachingTreeDock_->isClosed())
            teachingTreeDock_->toggleView(false);
        if (fileTreeDock_ && fileTreeDock_->isClosed())
            fileTreeDock_->toggleView(true);
        if (fileTreeDock_)
            fileTreeDock_->setAsCurrentTab();
    } else if (id == "debug") {
        targetDock = debugPanelDock_;
        if (fileTreeDock_ && !fileTreeDock_->isClosed())
            fileTreeDock_->toggleView(false);
        if (teachingTreeDock_ && !teachingTreeDock_->isClosed())
            teachingTreeDock_->toggleView(false);
        if (debugPanelDock_ && debugPanelDock_->isClosed())
            debugPanelDock_->toggleView(true);
        if (debugPanelDock_)
            debugPanelDock_->setAsCurrentTab();
    } else if (id == "learn") {
        // 学习中心：显示左侧教学树导航 dock
        targetDock = teachingTreeDock_;
        if (fileTreeDock_ && !fileTreeDock_->isClosed())
            fileTreeDock_->toggleView(false);
        if (debugPanelDock_ && !debugPanelDock_->isClosed())
            debugPanelDock_->toggleView(false);
        if (teachingTreeDock_ && teachingTreeDock_->isClosed())
            teachingTreeDock_->toggleView(true);
        if (teachingTreeDock_)
            teachingTreeDock_->setAsCurrentTab();
        // 若中央区当前在欢迎页或编辑器（非教学面板），同时打开「学习路径地图」面板，
        // 避免用户只看到左侧树而误以为「学习中心面板打不开」。
        // 已在某个教学面板时则不抢占，保留树导航语义。
        if (centerStack_ && (centerInEditorMode_ || centerStack_->currentWidget() == welcomePage_)) {
            showTeachingPanel(QStringLiteral("learning-path"));
        }
    }

    // 平滑滑入：对新显示的左侧面板内容应用 150ms pos 滑入动画
    // issue 1：原使用 fadeInWidget（QGraphicsOpacityEffect）对 dock 内容做透明度渐变，
    // 但 GraphicsEffect 会触发离屏 pixmap 合成，在 dock 切换时产生卡顿/闪烁。
    // 改用 slideInWidget（仅驱动 pos 属性，O(1) 复杂度），过渡更流畅。
    if (targetDock) {
        QTimer::singleShot(30, this, [targetDock]() {
            if (auto* w = targetDock->widget()) {
                PanelAnimator::slideInWidget(w);
            }
        });
    }

    syncViewMenuChecks();
}

// ============================================================
// 第四档教学面板：跨面板跳转路由
// ============================================================

/// 跳转到指定教学面板并聚焦。
void Ide::onJumpToPanel(const QString& panelId) {
    // P1-4 fix (F16): 统一跳转目标——既支持编辑器底层视图 id
    // (editor/tokens/ast/ir/bytecode/output)，也支持教学面板 id
    // (pipeline/lab-manual/bug-hunt/...)。底层视图 id 优先匹配，未命中
    // 时 fallback 到 showTeachingPanel 路由，复用 onActivityRequested 的 id 集，
    // 消除「打开 XX 面板，切到 YY 步骤」需学员自己找的引导。
    if (panelId == "editor") {
        showEditorArea();
    } else if (panelId == "tokens") {
        showEditorArea();
        showRightPanel(0); // token tab
    } else if (panelId == "ast") {
        showEditorArea();
        showAstWindow();
    } else if (panelId == "ir") {
        showEditorArea();
        showRightPanel(1); // ir tab
    } else if (panelId == "bytecode") {
        showEditorArea();
        showRightPanel(2); // bytecode tab
    } else if (panelId == "output") {
        showEditorArea();
        showBottomPanel(0); // output tab
    } else if (!panelId.isEmpty()) {
        // P1-4 fix: 未匹配底层视图 id，尝试教学面板 id 路由
        // 复用 onActivityRequested 已建立的 id→panel 映射（pipeline/lab-manual/
        // bug-hunt/syntax-explorer/backend-compare/ir-transform/profile-dashboard/
        // memory-model/bytecode-trace/call-stack/variable-inspector/
        // breakpoint-condition/exception-flow/closure-inspector/glossary 等）
        onActivityRequested(panelId);
    }
}

/// 活动栏请求信号：按请求的动作切换视图。
void Ide::onActivityRequested(const QString& activityId) {
    // P2-1 fix: 通过 PanelCatalog 统一路由，消除原 22 项 if-else 链
    // 特殊活动（welcome / freeform-project）单独处理，其他走 canonicalPanelId 映射

    if (activityId == "welcome") {
        // 再次显示欢迎向导
        auto* wizard = new WelcomeWizard(this);
        // Step 4 完成后自动展开 LearningPathPanel（与首次启动逻辑一致）
        connect(wizard, &WelcomeWizard::learningPathRequested, this,
                [this]() { showTeachingPanel(QStringLiteral("learning-path")); });
        wizard->exec();
        QSettings s;
        s.setValue(kWelcomeCompletedKey, true);
        wizard->deleteLater();
        if (learningPathPanel_)
            learningPathPanel_->markActivityCompleted("welcome");
        return;
    }

    if (activityId == "freeform-project") {
        // 自由项目：无对应面板，切到编辑器让用户开始编码
        showEditorArea();
        // AUDIT-P1 fix: freeform-project 活动缺失 markActivityCompleted 调用，
        // 导致阶段 4 stageProgress 永远 < 100%（5 活动中缺 1）。
        // 采用"首次进入即完成"策略（与 code-journey/welcome 一致），
        // 因为 freeform-project 是开放式沙盒，无明确完成判定。
        if (learningPathPanel_)
            learningPathPanel_->markActivityCompleted("freeform-project");
        return;
    }

    // P2-1 fix: 别名 + 已注册面板 id 一并交给 PanelCatalog 处理
    std::string canonical = PanelCatalog::canonicalPanelId(activityId.toStdString());
    if (!canonical.empty()) {
        // showTeachingPanel 内部已对 "learning-path" 调用 refresh()，
        // 此处不再重复调用——原双重 refresh() 会触发两次 deleteLater 重建 +
        // 两次 fadeInWidget 动画堆积，是章节切换卡顿的诱因之一。
        showTeachingPanel(QString::fromStdString(canonical));
    }
    // 其他未知 id 静默忽略（原行为一致）
}

/// 将教学面板包裹到统一的标题/容器外壳中，供 centerStack_ 路由显示。
QWidget* Ide::wrapTeachingPanel(const QString& panelId, const QString& title, QWidget* panel) {
    // 教学面板包装器：顶部插入 TeachingPanelHeader（标题 + 帮助 + 学习路径跳转）
    // 容器卡片化：objectName + WA_StyledBackground 让 applyFluentStyle 的 QSS 生效
    auto* container = new QWidget;
    container->setObjectName("teachingPanelCard");
    container->setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(container);
    // 卡片内部 padding 8px
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(0);

    auto* header = new TeachingPanelHeader(panelId, title, container);
    layout->addWidget(header);

    // 「学习路径」按钮 → 切换到 LearningPathPanel 教学面板
    connect(header, &TeachingPanelHeader::learningPathRequested, this,
            [this]() { showTeachingPanel(QStringLiteral("learning-path")); });
    // 「新手引导」按钮 → 启动该面板的 GuidedTour
    connect(header, &TeachingPanelHeader::guidedTourRequested, this, &Ide::onPanelGuidedTourRequested);
    // 「返回编辑器」按钮 → 切回代码编辑区
    connect(header, &TeachingPanelHeader::returnToEditorRequested, this, &Ide::showEditorArea);

    layout->addWidget(panel, 1);

    // 教学面板滚动条 Fluent 化：遍历 panel 内所有 QAbstractScrollArea 子类
    // （QListWidget / QTableWidget / QTreeWidget / QTextBrowser / QScrollArea），
    // 替换原生 QScrollBar 为 QFluentKit ScrollBar，与全局 Fluent 主题一致。
    // 注：滚动条样式由 QFluentKit StyleSheet 全局驱动，此处仅替换实例。
    auto applyFluentScrollBars = [](QWidget* w) {
        for (auto* child : w->findChildren<QAbstractScrollArea*>()) {
            // 避免重复替换（已被设置为 ScrollBar 的实例其原生 scrollbar 已被接管）
            if (!child)
                continue;
            auto* oldV = child->verticalScrollBar();
            if (oldV && QString(oldV->metaObject()->className()) != QStringLiteral("ScrollBar")) {
                child->setVerticalScrollBar(new ScrollBar(child));
            }
            auto* oldH = child->horizontalScrollBar();
            if (oldH && QString(oldH->metaObject()->className()) != QStringLiteral("ScrollBar")) {
                child->setHorizontalScrollBar(new ScrollBar(Qt::Horizontal, child));
            }
        }
    };
    applyFluentScrollBars(panel);

    // 教学面板初始尺寸调大：设置最小尺寸（宽 500 / 高 400）
    // 配合 dock 的 setMinimumSizeHintMode(MinimumSizeHintFromDockWidgetMinimumSize)
    // 确保面板显示时有足够的可视空间，避免内容被挤压
    container->setMinimumSize(500, 400);

    return container;
}

// ============================================================
// onPanelGuidedTourRequested — 启动对应教学面板的新手引导
// ============================================================

/// 教学面板引导游请求：启动对应引导。
void Ide::onPanelGuidedTourRequested(const QString& panelId) {
    // ROUND-89 P0 fix: 关闭流程中禁止创建新 GuidedTour。showTeachingPanel 在
    // 首次访问 5 个自动引导面板时通过 QTimer::singleShot(0, this, ...) 延迟启动
    // 引导。该 singleShot 创建的临时 QTimer 是 Ide 的直接子对象，closeEvent 的
    // stopChildAnimations 只扫描 centerStack_/bottomStack_/rightStack_/dockManager_
    // 的子对象，不覆盖 Ide 直接子 QTimer；removePostedEvents(this) 不取消定时器
    // 事件。当 maybeSave 模态对话框或 processEvents 的事件循环派发该定时器时，
    // 会在关闭过程中创建"孤儿" GuidedTour，其 showStep 修改 widget 样式表、
    // 创建 bubble_ 子 widget、访问正在清理的目标 widget → UAF。
    if (closing_)
        return;
    // 面板可能尚未构造（懒加载），先确保创建
    ensureTeachingPanelCreated(panelId);

    GuidedTour* tour = nullptr;
    if (panelId == QStringLiteral("bytecode-trace") && bytecodeTracePanel_) {
        tour = bytecodeTracePanel_->createGuidedTour(this);
    } else if (panelId == QStringLiteral("call-stack") && callStackPanel_) {
        tour = callStackPanel_->createGuidedTour(this);
    } else if (panelId == QStringLiteral("variable-inspector") && variableInspectorPanel_) {
        tour = variableInspectorPanel_->createGuidedTour(this);
    } else if (panelId == QStringLiteral("breakpoint-condition") && breakpointConditionPanel_) {
        tour = breakpointConditionPanel_->createGuidedTour(this);
    } else if (panelId == QStringLiteral("bug-hunt") && bugHuntPanel_) {
        tour = bugHuntPanel_->createGuidedTour(this);
    }
    if (tour) {
        // AUDIT-P0 fix: 连接 finished → deleteLater，避免 tour 对象永不释放。
        // 原 onPanelGuidedTourRequested 中 tour 为局部变量不存储，完成/跳过后
        // 仅 hideOverlay 隐藏 bubble_，tour 对象作为 Ide 子对象一直存活至 IDE 析构。
        // 多次触发面板引导会累积多个 tour + bubble_ 对象，加剧 UAF 风险。
        // finished 信号在 GuidedTour::skip 或走完所有步骤时发射（参见 GuidedTour.cpp）。
        // ROUND-67 P1 fix: 记录到 activePanelTours_，closeEvent 中显式停止并清理，
        // 防止 tour 的 QTimer::singleShot(0, tour, ...) 在 maybeSave 模态对话框
        // 期间派发访问已被清理的 targetWidget → UAF。
        activePanelTours_.insert(tour);
        connect(tour, &GuidedTour::finished, this, [this, tour]() {
            activePanelTours_.remove(tour);
            tour->deleteLater();
        });
        tour->start();
    }
}

/// 右侧 Pivot 导航切换响应。
void Ide::onRightPivotChanged(const QString& routeKey) {
    if (!rightStack_ || !rightPivot_)
        return;
    // Only auto-load when the right panel is actually visible
    if (rightDock_ && rightDock_->isClosed())
        return;

    int idx = 0;
    if (routeKey == "token")
        idx = 0;
    else if (routeKey == "ir")
        idx = 1;
    else if (routeKey == "bytecode")
        idx = 2;
    rightStack_->setCurrentIndex(idx);
    loadVisualizationForTab(idx);
}

/// 底部 Pivot 导航切换响应。
void Ide::onBottomPivotChanged(const QString& routeKey) {
    if (!bottomStack_)
        return;
    if (routeKey == "output")
        bottomStack_->setCurrentIndex(0);
    else if (routeKey == "errors")
        bottomStack_->setCurrentIndex(1);
    else if (routeKey == "repl")
        bottomStack_->setCurrentIndex(2);
}

/// 根据当前标签类型加载对应可视化（AST/字节码/IR/Token）。
void Ide::loadVisualizationForTab(int tabIndex) {
    if (!controller_->astRoot())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    struct CursorGuard {
        ~CursorGuard() { QApplication::restoreOverrideCursor(); }
    } guard;

    switch (tabIndex) {
    case 0: // Token
        updateTokenTable();
        break;
    case 1: { // IR
        // BUG-ORCH-3 fix: RegisterVM 模式下 useRegisterVM_ 优先于 useIR_，
        // 需临时禁用 useRegisterVM_ 才能走 IR 路径，否则 IR 标签页静默失效。
        bool savedRegVM = controller_->compiler().getUseRegisterVM();
        controller_->compiler().setUseRegisterVM(false);
        controller_->compiler().setUseIR(true);
        // BUG-AUDIT-MOD-5 fix: 捕获所有异常（含非 std::exception），确保 setUseIR(false) 必定执行
        try {
            controller_->runCompiler();
            populateIRViewer();
        } catch (const std::exception& e) {
            irViewer_->clearIR();
            appendError(mlTr("IR 编译异常: %1").arg(e.what()));
            showBottomPanel(1);
        } catch (...) {
            irViewer_->clearIR();
            appendError(mlTr("IR 编译发生未知异常"));
            showBottomPanel(1);
        }
        controller_->compiler().setUseIR(false);
        controller_->compiler().setUseRegisterVM(savedRegVM); // 恢复原引擎状态
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
            bytecodeList_->addItem(mlTr("字节码编译异常: %1").arg(e.what()));
        }
        break;
    }
    }
}

/// 显示当前 AST 的树状视图。
void Ide::onShowAstTree() {
    if (!codeEditor_)
        return;
    std::string source = codeEditor_->toPlainText().toStdString();
    auto pipelineResult = controller_->runFrontendPipeline(source);
    updateAstViewer();
    showAstWindow();
}

// ============================================================
// VM debugging
// ============================================================

/// VM 单步按钮槽：先校验 VM 是否在运行 / REPL 是否占用，再按 STEP_IN 模式驱动 VmStepper。
void Ide::onVmStep() {
    if (controller_->isVmRunning())
        return;
    // BUG-ORCH-8 fix: REPL 异步执行期间 VM 步进会与 REPL 输出交错，且 Interpreter 被
    // REPL worker 持有，VM 步进操作 compiler/VM 状态可能与之冲突。拒绝并提示用户。
    if (replPanel_ && replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请先停止 REPL 再使用 VM 单步"));
        showBottomPanel(1);
        return;
    }
    bool hasCode = controller_->getUseRegisterVM()
                       ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
                       : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode)
        return;

    syncVmBreakpoints();
    setVmStepActionsEnabled(false);

    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_IN);
    } catch (const std::exception& e) {
        appendError(mlTr("VM 单步异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    } catch (...) {
        appendError(mlTr("VM 单步发生未知异常"));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    }
    handleVmStepResult(result);
}

/// VM 跨过按钮槽：按 STEP_OVER 模式驱动 VmStepper（同 onVmStep 的前置校验）。
void Ide::onVmStepOver() {
    if (controller_->isVmRunning())
        return;
    // BUG-ORCH-8 fix: 同 onVmStep，REPL 执行期间拒绝 VM 跨过操作
    if (replPanel_ && replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请先停止 REPL 再使用 VM 跨过"));
        showBottomPanel(1);
        return;
    }
    bool hasCode = controller_->getUseRegisterVM()
                       ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
                       : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode)
        return;

    syncVmBreakpoints();
    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_OVER);
    } catch (const std::exception& e) {
        appendError(mlTr("VM 跨过异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    } catch (...) {
        appendError(mlTr("VM 跨过发生未知异常"));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    }
    handleVmStepResult(result);
}

/// VM 跨出按钮槽：按 STEP_OUT 模式驱动 VmStepper（同 onVmStep 的前置校验）。
void Ide::onVmStepOut() {
    if (controller_->isVmRunning())
        return;
    // BUG-ORCH-8 fix: 同 onVmStep，REPL 执行期间拒绝 VM 跨出操作
    if (replPanel_ && replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请先停止 REPL 再使用 VM 跨出"));
        showBottomPanel(1);
        return;
    }
    bool hasCode = controller_->getUseRegisterVM()
                       ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
                       : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode)
        return;

    syncVmBreakpoints();
    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::STEP_OUT);
    } catch (const std::exception& e) {
        appendError(mlTr("VM 跨出异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    } catch (...) {
        appendError(mlTr("VM 跨出发生未知异常"));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    }
    handleVmStepResult(result);
}

/// VM 运行按钮槽：按 RUN 模式异步驱动 VmStepper（QTimer 分批执行），立即进入 RUNNING 态。
void Ide::onVmRun() {
    if (controller_->isVmRunning())
        return;
    // BUG-ORCH-8 fix: 同 onVmStep，REPL 执行期间拒绝 VM RUN 操作
    if (replPanel_ && replPanel_->isReplRunning()) {
        appendError(mlTr("REPL 正在执行，请先停止 REPL 再使用 VM 运行"));
        showBottomPanel(1);
        return;
    }
    bool hasCode = controller_->getUseRegisterVM()
                       ? !controller_->compiler().getLastRegisterResult().mainChunk.code.empty()
                       : !controller_->lastCompileResult().mainChunk.code.empty();
    if (!hasCode)
        return;

    syncVmBreakpoints();
    setVmStepActionsEnabled(false);
    IdeController::VmStepResult result;
    try {
        result = controller_->vmStepByMode(IdeController::VmStepMode::RUN);
    } catch (const std::exception& e) {
        appendError(mlTr("VM 运行异常: %1").arg(e.what()));
        setVmStepActionsEnabled(true, false);
        showBottomPanel(1);
        return;
    } catch (...) {
        appendError(mlTr("VM 运行发生未知异常"));
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
        formatAction_->setEnabled(false);
        if (replPanel_)
            replPanel_->setInputEnabled(false);
        if (codeEditor_)
            codeEditor_->setReadOnly(true);
        return;
    }
    handleVmStepResult(result);
}

/// 统一处理 VmStepper 的步进结果：根据 OK/FINISHED/ERROR/PAUSED 等状态刷新
/// 栈面板、全局变量、字节码高亮、源码行高亮并切换按钮可用性。
void Ide::handleVmStepResult(IdeController::VmStepResult result) {
    // ISSUE-7 fix: 关闭流程中丢弃所有 VM 步进信号。closeEvent 已 disconnect 信号，
    // 但 DirectConnection（同线程）的同步调用或在 disconnect 前已入队的信号仍可能进入。
    if (closing_)
        return;
    // AUDIT-P2-CORRECT fix: stale 信号防御。
    // closeEvent 期间 vmStop() 后，已投递的 vmRunPaused 信号仍在主线程队列。
    // Ide 析构时处理 pending 事件会访问已析构成员（vmStackPanel_/codeEditor_ 等），
    // try/catch 无法捕获 UAF。
    //
    // AUDIT-P1-ROUND50 fix: 原守卫 `!isVmInitialized() && !isVmRunning()` 会误杀
    // FINISHED/ERROR/NOT_READY 信号。VmStepper::runBatch/stepByMode 在发射这些信号
    // 前已合法地将 isVmInitialized_ 和 isVmRunning_ 都置为 false（正常结束/错误终态），
    // 导致守卫命中 return，FINISHED/ERROR 的 UI 更新分支被整体跳过——Step 模式下
    // 所有 VM 按钮禁用且无法通过 Stop 恢复。vmRunPaused 连接是同线程 DirectConnection，
    // closeEvent 的残留信号只可能是 PAUSED_AT_BREAKPOINT（断点暂停），FINISHED/ERROR
    // 是合法终态不应被丢弃。将守卫收窄到仅 PAUSED_AT_BREAKPOINT 路径。
    switch (result) {
    case IdeController::VmStepResult::NOT_READY:
        setVmStepActionsEnabled(true, false);
        runAction_->setEnabled(true);
        debugAction_->setEnabled(true);
        if (codeEditor_)
            codeEditor_->setReadOnly(false);
        return;
    case IdeController::VmStepResult::RUNNING:
        vmStepAction_->setEnabled(false);
        vmStepOverAction_->setEnabled(false);
        vmStepOutAction_->setEnabled(false);
        vmRunAction_->setEnabled(false);
        vmStopAction_->setEnabled(true);
        formatAction_->setEnabled(false);
        if (replPanel_)
            replPanel_->setInputEnabled(false);
        if (codeEditor_)
            codeEditor_->setReadOnly(true);
        return;
    case IdeController::VmStepResult::ERROR: {
        // P0-3 fix (F10): 接入 ErrorHintEngine，VM 错误也附加教学性提示
        // VM 路径无法直接访问 Interpreter 作用域，scopeVars 为空
        std::string enriched = ErrorHintEngine::enrichErrorMessage(controller_->getVmLastError(), "runtime", {});
        Diagnostic diag(DiagLevel::Error, enriched, controller_->getVmLastErrorLine(), 0, DiagSource::VM);
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
        if (codeEditor_)
            codeEditor_->setReadOnly(false);
        return;
    }
    case IdeController::VmStepResult::FINISHED:
        appendOutput(mlTr("--- VM 执行结束 ---"));
        showBottomPanel(0);
        vmStackPanel_->clearAll();
        setVmStepActionsEnabled(true, false);
        runAction_->setEnabled(true);
        debugAction_->setEnabled(true);
        if (codeEditor_)
            codeEditor_->setReadOnly(false);
        return;
    case IdeController::VmStepResult::OK:
    case IdeController::VmStepResult::PAUSED_AT_BREAKPOINT:
        // AUDIT-P1-ROUND50 fix: 守卫收窄到此路径——仅 PAUSED 信号需要 stale 防御。
        // OK 信号是单步成功的合法中间态，不应被丢弃。PAUSED 信号在 closeEvent 后
        // 可能成为 stale 信号（VM 已停止但排队中的断点暂停信号尚未处理）。
        if (result == IdeController::VmStepResult::PAUSED_AT_BREAKPOINT && !controller_->isVmInitialized() &&
            !controller_->isVmRunning())
            return;
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
                appendOutput(mlTr("🔴 VM 命中断点: 第 %1 行").arg(breakLine));
                showBottomPanel(0);
            }
        } else {
            // Sync source line highlight on every VM step
            int stepLine = controller_->getVmCurrentLine();
            if (stepLine > 0 && codeEditor_)
                codeEditor_->setCurrentLine(stepLine);
        }
        // BUG-ORCH-5 fix: VM 暂停期间禁止编辑代码，避免产生陈旧字节码
        if (codeEditor_)
            codeEditor_->setReadOnly(true);
        setVmStepActionsEnabled(true, true);
        return;
    }
}

/// 统一设置 VM 步进/运行/停止按钮的可用状态（running=true 时仅保留停止按钮可用）。
void Ide::setVmStepActionsEnabled(bool enabled, bool running) {
    vmStepAction_->setEnabled(enabled);
    vmStepOverAction_->setEnabled(enabled);
    vmStepOutAction_->setEnabled(enabled);
    vmRunAction_->setEnabled(enabled);
    vmStopAction_->setEnabled(enabled && running);
    compileAnalysisAction_->setEnabled(enabled && !running);
    // AUDIT-P2-CORRECT fix: VM 执行/调试期间禁用格式化和 REPL 输入，
    // 对齐 setRunningState 的禁用策略。格式化会修改 codeEditor 内容导致
    // VM 字节码陈旧；REPL 输入会并发访问 Interpreter/VM 状态。
    formatAction_->setEnabled(enabled && !running);
    if (replPanel_)
        replPanel_->setInputEnabled(enabled && !running);
}

/// VM 停止按钮槽：调用 controller_->vmStop() 中止 RUN 模式并复位 VM 状态。
void Ide::onVmStop() {
    controller_->vmStop();
    vmStackPanel_->clearAll();
    bytecodeList_->setCurrentRow(-1);
    if (codeEditor_)
        codeEditor_->setCurrentLine(-1);
    if (codeEditor_)
        codeEditor_->clearSourceHighlight();
    if (irViewer_)
        irViewer_->clearHighlight();
    setVmStepActionsEnabled(true, false);
    runAction_->setEnabled(true);
    debugAction_->setEnabled(true);
    if (codeEditor_)
        codeEditor_->setReadOnly(false);
    // P-IDE-4 fix: 退出字节码调试模式时隐藏 VM 按钮组，防止按钮残留重复。
    // 原 onVmStop 未调用 showVmButtons(false)，导致 VM 单步按钮和分隔线在
    // 调试结束后仍驻留工具栏；后续若再进入树遍历调试会出现两套按钮并存。
    showVmButtons(false);
}

// ============================================================
// Find / Replace
// ============================================================

/// 打开查找面板。
void Ide::onFind() {
    if (findReplacePanel_)
        findReplacePanel_->showFind();
}
/// 打开替换面板。
void Ide::onReplace() {
    if (findReplacePanel_)
        findReplacePanel_->showReplace();
}

/// 查找下一个匹配项。
void Ide::onFindNext() {
    if (findReplacePanel_ && findReplacePanel_->isVisible()) {
        findReplacePanel_->onFindNext();
        return;
    }
    if (findReplacePanel_)
        findReplacePanel_->showFind();
}

/// 查找上一个匹配项。
void Ide::onFindPrev() {
    if (findReplacePanel_ && findReplacePanel_->isVisible()) {
        findReplacePanel_->onFindPrev();
        return;
    }
    if (findReplacePanel_)
        findReplacePanel_->showFind();
}

// ============================================================
// Help dialog (Fluent-style, two-column shortcut layout)
// ============================================================

/// 显示帮助/关于对话框。
void Ide::showHelpDialog() {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(mlTr("帮助"));
    dlg->setWindowFlags(dlg->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    // 第十一轮：宽 520px 高 420px，8px 圆角，柔和阴影
    // 第十三轮：扩展快捷键到 32 项后调大为 720x520
    dlg->setFixedSize(720, 520);

    // P2 视觉一致性：硬编码色替换为 TeachingTheme 主题色板（亮/暗主题自适应）
    // 原先通过 palette().lightness() 检测暗色并分支取色，现统一走 TeachingTheme，
    // 与 applyFluentStyle / 教学面板 / WelcomeWizard 等保持一致。
    QString bg = TeachingTheme::surface().name();
    QString text = TeachingTheme::textPrimary().name();
    QString muted = TeachingTheme::textSecondary().name();
    QString keyColor = TeachingTheme::ideAccent().name();
    QString btnBg = TeachingTheme::primary().name();
    QString btnHover = TeachingTheme::primaryHover().name();
    QString btnPressed = TeachingTheme::primaryPressed().name();
    dlg->setStyleSheet(QString("QDialog { background: %1; border-radius: 8px; }"
                               "QLabel#helpTitle { font-size: 16px; font-weight: 600; color: %2; }"
                               "QLabel#helpKey { font-family: 'Cascadia Code','Cascadia Mono','Consolas','JetBrains "
                               "Mono','Source Code Pro','Menlo','DejaVu Sans Mono','Courier New',monospace;"
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

    auto* title = new QLabel(mlTr("MiniLang IDE 快捷键"), dlg);
    title->setObjectName("helpTitle");
    layout->addWidget(title);

    // 第十一轮：两列网格布局（左列快捷键、右列描述）
    // 第十三轮：扩展至完整 30+ 项快捷键列表，分组显示
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(6);
    grid->setColumnMinimumWidth(0, 120);
    grid->setColumnStretch(1, 1);

    // 分组结构：组名 + 该组快捷键列表
    struct Shortcut {
        const char* key;
        const char* desc;
    };
    struct Group {
        const char* name;
        Shortcut shortcuts[8];
        int count;
    };
    const Group groups[] = {
        {"📁 文件 / 编辑",
         {
             {"Ctrl+N", "新建文件"},
             {"Ctrl+S", "保存文件"},
             {"Ctrl+Shift+S", "另存为"},
             {"Ctrl+G", "跳转到行"},
             {"Ctrl+/", "切换行注释"},
             {"Ctrl+Shift+/", "切换块注释"},
             {"Ctrl+D", "选中下一个相同词"},
             {"Ctrl+Shift+K", "删除当前行"},
         },
         8},
        {"🔍 查找 / 替换 / 视图",
         {
             {"Ctrl+F", "查找"},
             {"Ctrl+H", "替换"},
             {"F3", "查找下一个"},
             {"Shift+F3", "查找上一个"},
             {"Esc", "关闭查找面板"},
             {"Ctrl+=", "放大字号"},
             {"Ctrl+-", "缩小字号"},
             {"Ctrl+0", "重置字号"},
         },
         8},
        {"▶ 运行 / 调试",
         {
             {"F5", "运行程序"},
             {"F6", "调试程序"},
             {"F10", "单步跳过"},
             {"F11", "单步进入"},
             {"Shift+F11", "单步跳出"},
             {"F9", "继续/恢复"},
             {"Shift+F5", "停止运行"},
             {"Ctrl+T", "插入代码模板"},
         },
         8},
        {"🎓 教学 / VM 面板",
         {
             {"Ctrl+Shift+L", "学习中心（含学习路径地图）"},
             {"Ctrl+Shift+P", "编译管线可视化"},
             {"Ctrl+Shift+J", "代码生命旅程"},
             {"Ctrl+Shift+G", "术语表"},
             {"Ctrl+Shift+1", "语法浏览器"},
             {"Ctrl+Shift+2", "Token 拼图"},
             {"Ctrl+Shift+3", "AST 搭建玩具"},
             {"Ctrl+Shift+4", "字节码轨迹"},
         },
         8},
    };
    int row = 0;
    int leftCol = 0, rightCol = 2;
    int leftRow = 0, rightRow = 0;
    int groupIdx = 0;
    for (int g = 0; g < 4; ++g) {
        const Group& grp = groups[g];
        int col = (groupIdx < 2) ? leftCol : rightCol;
        int r = (groupIdx < 2) ? leftRow : rightRow;
        // 组标题（跨两列）
        auto* groupLabel = new QLabel(QString::fromUtf8(grp.name), dlg);
        groupLabel->setObjectName("helpKey");
        groupLabel->setStyleSheet(
            QString("font-weight: 600; color: %1; padding: 4px 0 2px 0;").arg(TeachingTheme::primary().name()));
        grid->addWidget(groupLabel, r * 2, col, 1, 2);
        ++r;
        for (int i = 0; i < grp.count; ++i) {
            auto* k = new QLabel(QString::fromUtf8(grp.shortcuts[i].key), dlg);
            k->setObjectName("helpKey");
            k->setFixedWidth(110);
            auto* d = new QLabel(mlTr(grp.shortcuts[i].desc), dlg);
            d->setObjectName("helpDesc");
            grid->addWidget(k, r * 2, col);
            grid->addWidget(d, r * 2, col + 1);
            ++r;
        }
        // 更新行计数
        if (groupIdx < 2)
            leftRow = r;
        else
            rightRow = r;
        ++groupIdx;
    }
    // 设置行间距（每行之间稍大间距）
    grid->setVerticalSpacing(4);
    grid->setRowMinimumHeight(0, 28);
    layout->addLayout(grid);

    layout->addSpacing(4);
    auto* tipLabel = new QLabel(mlTr("在代码行号左侧点击可设置/取消断点，右键点击断点可设置条件。\n"
                                     "💡 更多教学面板快捷键：Ctrl+Shift+5~9（调用栈/变量/内存/IR变换/VM栈沙盒）、"
                                     "Alt+1~5（条件断点/异常流/闭包/性能/实验手册）。"),
                                dlg);
    tipLabel->setObjectName("helpTip");
    tipLabel->setWordWrap(true);
    layout->addWidget(tipLabel);

    layout->addStretch(1);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto* okBtn = new QPushButton(mlTr("确定"), dlg);
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
// 3 分钟 Hello World 引导（GuidedTour 接入）
// ============================================================

/// 启动新手指引游（GuidedTour）。
void Ide::startGuidedTour() {
    // 若已有引导在进行，先清理
    if (guidedTour_) {
        delete guidedTour_;
        guidedTour_ = nullptr;
    }
    guidedTour_ = new GuidedTour(this, this);

    // 步骤 1：高亮编辑器，引导用户输入代码
    guidedTour_->addStep(codeEditor_, mlTr("① 代码编辑器"),
                         mlTr("在这里输入 MiniLang 代码。我们已经为你准备好了 <b>print(\"Hello, World!\");</b><br><br>"
                              "下一步：点击下方「下一步」学习如何运行。"),
                         mlTr("下一步 →"), mlTr("跳过引导"));

    // 步骤 2：高亮运行按钮（F5）
    guidedTour_->addStep(runAction_ ? runAction_->associatedWidgets().value(0) : nullptr, mlTr("② 运行程序"),
                         mlTr("点击工具栏的 <b>▶ 运行</b> 按钮，或按 <b>F5</b> 运行你的代码。<br><br>"
                              "程序会被编译为字节码，由虚拟机执行。"),
                         mlTr("下一步 →"), mlTr("跳过引导"));

    // 步骤 3：高亮底部输出面板
    guidedTour_->addStep(bottomContainer_, mlTr("③ 输出面板"),
                         mlTr("程序运行结果会显示在底部的 <b>输出</b> 面板。<br><br>"
                              "你将看到 <b>Hello, World!</b> 出现在这里。"),
                         mlTr("下一步 →"), mlTr("跳过引导"));

    // 步骤 4：高亮 ActivityBar「学习」入口
    guidedTour_->addStep(activityBar_, mlTr("④ 学习中心"),
                         mlTr("想深入了解编译原理？点击左侧栏的 <b>「学习」</b> 图标（或按 <b>Ctrl+Shift+L</b>）<br>"
                              "打开 22 个教学面板的导航中心，包括 Token 拼图、AST 构建器、VM 沙盒等。"),
                         mlTr("下一步 →"), mlTr("跳过引导"));

    // 步骤 5：高亮 AST 语法树查看器
    guidedTour_->addStep(astViewer_, mlTr("⑤ AST 语法树"),
                         mlTr("查看语法树：代码被解析为树形结构。<br><br>"
                              "每个节点代表一种语法元素，帮助你理解编译器如何「看懂」你的代码。"),
                         mlTr("下一步 →"), mlTr("跳过引导"));

    // 步骤 6：高亮 REPL 交互面板
    guidedTour_->addStep(replPanel_, mlTr("⑥ REPL 交互面板"),
                         mlTr("交互式求值：输入表达式立即看到结果。<br><br>"
                              "无需编写完整程序，适合快速实验和调试语法。"),
                         mlTr("下一步 →"), mlTr("跳过引导"));

    // 步骤 7：高亮教学面板导航树
    guidedTour_->addStep(teachingTreePanel_, mlTr("⑦ 教学面板"),
                         mlTr("教学面板：22 个学习面板，从词法分析到 Bug 狩猎。<br><br>"
                              "点击左侧导航树中的任意面板，开始深入学习编译原理的各个环节。"),
                         mlTr("开始探索 ✓"), mlTr("跳过引导"));

    connect(guidedTour_, &GuidedTour::finished, this, [this](bool completed) {
        if (completed) {
            // 完成引导后自动展开学习中心
            showTeachingPanel(QStringLiteral("learning-path"));
        }
        guidedTour_->deleteLater();
        guidedTour_ = nullptr;
    });

    guidedTour_->start();
}

// ============================================================
// Completion + Spell check candidates
// ============================================================

/// 初始化代码补全：构建关键字/API 候选词并配置编辑器补全。
void Ide::setupCompletion() {
    staticCompletionWords_.clear();
    spellCandidates_.clear();

    const auto& keywords = controller_->getKeywords();
    for (const auto& kv : keywords) {
        staticCompletionWords_ << QString::fromStdString(kv.first);
        spellCandidates_.push_back(kv.first);
    }

    // Built-in functions and methods
    std::vector<std::string> builtins = {"print",    "input",   "len",        "type",     "str",    "int",  "abs",
                                         "min",      "max",     "range",      "sum",      "push",   "pop",  "split",
                                         "join",     "indexOf", "startsWith", "endsWith", "substr", "keys", "values",
                                         "contains", "true",    "false",      "null"};
    for (const auto& b : builtins) {
        staticCompletionWords_ << QString::fromStdString(b);
        spellCandidates_.push_back(b);
    }

    staticCompletionWords_.removeDuplicates();
    staticCompletionWords_.sort(Qt::CaseInsensitive);
    if (codeEditor_)
        codeEditor_->setCompletionWords(staticCompletionWords_);

    // 500ms completion word refresh timer
    completionTimer_ = new QTimer(this);
    completionTimer_->setSingleShot(true);
    completionTimer_->setInterval(500);
    connect(completionTimer_, &QTimer::timeout, this, &Ide::updateCompletionWords);

    // 300ms syntax check debounce timer
    syntaxCheckTimer_ = new QTimer(this);
    syntaxCheckTimer_->setSingleShot(true);
    syntaxCheckTimer_->setInterval(300);
    connect(syntaxCheckTimer_, &QTimer::timeout, this, [this]() { runRealTimeSyntaxCheck(); });

    // Wire textChanged for existing tabs
    for (auto& tab : editorTabs_) {
        if (!tab.editor)
            continue;
        connect(tab.editor, &QPlainTextEdit::textChanged, this, [this]() {
            if (completionTimer_)
                completionTimer_->start();
            if (syntaxCheckTimer_)
                syntaxCheckTimer_->start();
        });
    }
    updateCompletionWords();
}

/// 更新补全候选词（如导入模块带来的新符号）。
void Ide::updateCompletionWords() {
    if (!codeEditor_)
        return;
    QString text = codeEditor_->toPlainText();
    QStringList words = staticCompletionWords_;

    text.remove(QRegularExpression("\"(?:\\\\.|[^\"\\\\\\n])*\""));
    text.remove(QRegularExpression("/\\*.*?\\*/", QRegularExpression::DotMatchesEverythingOption));

    static const QRegularExpression pattern("\\b(?:var|fun|class)\\s+([A-Za-z_][A-Za-z0-9_]*)");
    auto matchIt = pattern.globalMatch(text);
    QSet<QString> userSymbols;
    while (matchIt.hasNext()) {
        QRegularExpressionMatch match = matchIt.next();
        QString name = match.captured(1);
        if (!name.isEmpty())
            userSymbols.insert(name);
    }

    QSet<QString> existingWords;
    for (const QString& w : words)
        existingWords.insert(w.toLower());
    for (const QString& sym : userSymbols) {
        if (!existingWords.contains(sym.toLower()))
            words << sym;
    }

    words.sort(Qt::CaseInsensitive);
    for (auto& tab : editorTabs_) {
        if (tab.editor)
            tab.editor->setCompletionWords(words);
    }
}

// ============================================================
// Diagnostics display (with smart spell correction)
// ============================================================

/// 将诊断包渲染到错误列表与编辑器行内标记。
void Ide::displayDiagnostics(const DiagnosticBag& bag) {
    // ISSUE-7 fix: 关闭流程中丢弃诊断信号，避免访问已部分析构的成员。
    if (closing_)
        return;
    const auto& allDiags = bag.all();
    for (const auto& diag : allDiags) {
        std::string msg = diag.message;

        // Smart spell correction: for error diagnostics, try to find
        // a close match among keywords/builtins
        if (diag.isError() && !spellCandidates_.empty()) {
            std::string ident = extractQuotedIdentifier(msg);
            if (!ident.empty() && ident.size() > 1) {
                auto suggestion = SpellChecker::suggestSuffix(ident, spellCandidates_, 2);
                if (!suggestion.empty()) {
                    msg += suggestion;
                }
            }
        }

        // P0-3 fix (F10): 接入 ErrorHintEngine 模式匹配（缺分号/括号不匹配/
        // 类型错误/除零/越界等），附加教学性提示。parse 阶段无 scopeVars。
        // P2 fix (错误码优先匹配): 若 Diagnostic 携带稳定 code（如 "missing-semicolon"），
        // 优先按 code 查 errorPatterns 表附加教学提示，避免消息文案变化时子串匹配失效。
        // code 为空时 4 参版本自动回退到 3 参子串匹配兜底（向后兼容）。
        if (diag.isError()) {
            msg = ErrorHintEngine::enrichErrorMessage(msg, diag.code, "parse", {});
        }

        // Build display text
        QString text = QString::fromStdString("[" + diag.sourceString() + "] " + diag.levelString());
        if (diag.line > 0) {
            text += mlTr(" (行 %1").arg(diag.line);
            if (diag.column > 0)
                text += mlTr(", 列 %1").arg(diag.column);
            text += ")";
        }
        text += ": " + QString::fromStdString(msg);

        if (diag.isError()) {
            appendError(text, diag.line, diag.column, diag.level);
        } else if (diag.isWarning()) {
            appendOutput(text, OutputLevel::Warning);
        } else {
            appendOutput(text, OutputLevel::Info);
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
        if (!ranges.empty() && codeEditor_)
            codeEditor_->setErrorRanges(ranges);
    }

    if (bag.size() > 1) {
        appendOutput(QString::fromStdString("--- " + bag.summary() + " ---"));
    }
}

// ============================================================
// Bytecode / IR / Token / AST helpers
// ============================================================

/// 在字节码列表中高亮当前执行到的指令行。
void Ide::highlightBytecodeLine(const std::string& chunkName, size_t ip) {
    const CompileResult& compileResult = controller_->lastCompileResult();
    const BytecodeChunk* targetChunk = nullptr;

    if (chunkName == "main" || chunkName.empty()) {
        targetChunk = &compileResult.mainChunk;
    } else {
        auto it = compileResult.functionChunks.find(chunkName);
        if (it != compileResult.functionChunks.end())
            targetChunk = &it->second;
    }
    if (!targetChunk || targetChunk->code.empty())
        return;

    int instrIndex = 0;
    if (ip < targetChunk->ipToInstrIndex.size() && targetChunk->ipToInstrIndex[ip] >= 0) {
        instrIndex = targetChunk->ipToInstrIndex[ip];
    } else {
        size_t offset = 0;
        while (offset < targetChunk->code.size()) {
            if (offset == ip)
                break;
            offset += targetChunk->instructionSizeAt(offset);
            instrIndex++;
        }
    }

    int startRow = 0;
    for (const auto& info : chunkRowMap_) {
        if (info.name == chunkName) {
            startRow = info.startRow;
            break;
        }
    }

    int targetRow = startRow + instrIndex;
    if (targetRow >= 0 && targetRow < bytecodeList_->count()) {
        bytecodeList_->setCurrentRow(targetRow);
        bytecodeList_->scrollToItem(bytecodeList_->item(targetRow));
    }
}

/// 字节码行点击：定位/选中对应源码行。
void Ide::onBytecodeRowClicked(int row) {
    if (row < 0 || !codeEditor_ || !controller_)
        return;
    // Find which chunk this row belongs to and get the source line
    const auto& compileResult = controller_->lastCompileResult();
    for (const auto& info : chunkRowMap_) {
        if (row >= info.startRow && row < info.startRow + info.rowCount) {
            int instrIndex = row - info.startRow;
            const BytecodeChunk* chunk = nullptr;
            if (info.name == "main") {
                chunk = &compileResult.mainChunk;
            } else {
                auto it = compileResult.functionChunks.find(info.name);
                if (it != compileResult.functionChunks.end())
                    chunk = &it->second;
            }
            if (!chunk)
                return;
            // Walk instructions to find the byte offset for this instruction index
            size_t byteOffset = 0;
            for (int i = 0; i < instrIndex; ++i) {
                byteOffset += chunk->instructionSizeAt(byteOffset);
            }
            int sourceLine = chunk->getLine(byteOffset);
            if (sourceLine > 0) {
                codeEditor_->highlightSourceLine(sourceLine);
            }
            return;
        }
    }
}

/// 用编译结果填充字节码列表（带语法高亮 HTML）。
void Ide::populateBytecodeList() {
    const CompileResult& compileResult = controller_->lastCompileResult();
    QString currentSource = codeEditor_ ? codeEditor_->toPlainText() : QString();
    // BUG-ORCH-2 fix: 缓存哈希需区分引擎模式，否则引擎切换后显示陈旧字节码
    int engineMode = engineCombo_ ? engineCombo_->currentIndex() : 0;
    size_t currentHash = qHash(currentSource) ^ (static_cast<size_t>(engineMode) << 32);
    if (currentHash == lastBytecodeSourceHash_ && bytecodeList_->count() > 0)
        return;
    lastBytecodeSourceHash_ = currentHash;

    bytecodeList_->clear();
    chunkRowMap_.clear();

    if (compileResult.mainChunk.code.empty()) {
        bytecodeList_->addItem(mlTr("(无字节码)"));
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
                item->setData(RichTextItemDelegate::kHtmlRole, formatBytecodeHtml(instr));
                bytecodeList_->addItem(item);
                currentRow++;
                offset += compileResult.mainChunk.instructionSizeAt(offset);
            }
            chunkRowMap_.push_back({"main", startRow, currentRow - startRow});
        }

        for (const auto& kv : compileResult.functionChunks) {
            std::string headerText = "---- " + kv.first + " (arity=" + std::to_string(kv.second.arity) + ") ----";
            auto* header = new QListWidgetItem;
            header->setText(QString::fromStdString(headerText));
            header->setFont(bytecodeFont);
            header->setData(RichTextItemDelegate::kHtmlRole, formatBytecodeHtml(headerText));
            bytecodeList_->addItem(header);
            currentRow++;

            int startRow = currentRow;
            size_t funcOffset = 0;
            while (funcOffset < kv.second.code.size()) {
                std::string instr = kv.second.disassembleInstruction(funcOffset);
                auto* item = new QListWidgetItem;
                item->setText(QString::fromStdString(instr));
                item->setFont(bytecodeFont);
                item->setData(RichTextItemDelegate::kHtmlRole, formatBytecodeHtml(instr));
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

/// 用 IR 中间表示填充 IR 查看器。
void Ide::populateIRViewer() {
    const IRFunction* ir = controller_->lastIR();
    irViewer_->setIR(ir);
    irToBytecodeOffset_ = controller_->lastIRToBytecodeOffset();
}

/// 在 IR 查看器中高亮当前行。
void Ide::highlightIRLine(size_t bytecodeOffset) {
    if (irToBytecodeOffset_.empty())
        return;
    irViewer_->highlightByBytecodeOffset(irToBytecodeOffset_, bytecodeOffset);
}

/// 刷新 Token 表格（词法分析产物可视化）。
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
            tokenTable_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(Token::typeToString(tok.type))));
            tokenTable_->setItem(i, 3, new QTableWidgetItem(QString::fromStdString(tok.lexeme)));
            tokenTable_->setItem(i, 4, new QTableWidgetItem(QString::fromStdString(tok.literalToString())));
            // Token type color coding
            QColor typeColor;
            switch (tok.type) {
            // Keywords: blue
            case TokenType::TK_VAR:
            case TokenType::TK_FUN:
            case TokenType::TK_IF:
            case TokenType::TK_ELSE:
            case TokenType::TK_WHILE:
            case TokenType::TK_FOR:
            case TokenType::TK_RETURN:
            case TokenType::TK_IMPORT:
            case TokenType::TK_FROM:
            case TokenType::TK_EXPORT:
            case TokenType::TK_CLASS:
            case TokenType::TK_EXTENDS:
            case TokenType::TK_SUPER:
            case TokenType::TK_PRINT:
            case TokenType::TK_BREAK:
            case TokenType::TK_CONTINUE:
            case TokenType::TK_TRY:
            case TokenType::TK_CATCH:
            case TokenType::TK_THROW:
            case TokenType::TK_AND:
            case TokenType::TK_OR:
            case TokenType::TK_NOT:
            case TokenType::TK_TRUE:
            case TokenType::TK_FALSE:
            case TokenType::TK_NULL:
            case TokenType::TK_DICT:
            case TokenType::TK_ARRAY:
            case TokenType::TK_INT:
            case TokenType::TK_FLOAT:
            case TokenType::TK_BOOL:
            case TokenType::TK_STRING_TYPE:
                typeColor = QColor("#268BD2");
                break;
            // Number literals: green
            case TokenType::TK_INT_LIT:
            case TokenType::TK_FLOAT_LIT:
                typeColor = QColor("#B58900");
                break;
            // String literals: red
            case TokenType::TK_STRING_LIT:
            case TokenType::TK_STRING_PART:
            case TokenType::TK_INTERP_START:
            case TokenType::TK_INTERP_END:
                typeColor = QColor("#2AA198");
                break;
            // Comments: neutral gray italic
            case TokenType::TK_LINE_COMMENT:
            case TokenType::TK_BLOCK_COMMENT:
                typeColor = QColor("#5A5A5A");
                break;
            // Operators: red
            case TokenType::TK_PLUS:
            case TokenType::TK_MINUS:
            case TokenType::TK_STAR:
            case TokenType::TK_SLASH:
            case TokenType::TK_PERCENT:
            case TokenType::TK_EQ:
            case TokenType::TK_NEQ:
            case TokenType::TK_LT:
            case TokenType::TK_GT:
            case TokenType::TK_LEQ:
            case TokenType::TK_GEQ:
            case TokenType::TK_ASSIGN:
                typeColor = QColor("#CB4B16");
                break;
            // Error: bright red
            case TokenType::TK_ERROR:
                typeColor = QColor("#DC322F");
                break;
            // Identifier / default: normal dark gray
            default:
                typeColor = QColor("#1E1E1E");
                break;
            }

            for (int col = 0; col < 5; ++col) {
                tokenTable_->item(i, col)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                if (col == 2) {
                    // Type column: use token type color
                    tokenTable_->item(i, col)->setForeground(typeColor);
                    if (tok.type == TokenType::TK_LINE_COMMENT || tok.type == TokenType::TK_BLOCK_COMMENT) {
                        QFont f = tokenTable_->item(i, col)->font();
                        f.setItalic(true);
                        tokenTable_->item(i, col)->setFont(f);
                    }
                } else if (col < 2) {
                    // Line/column: gray
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
        appendOutput(mlTr("[提示] Token 数量 %1 超过显示上限 %2，仅显示前 %2 条").arg(tokens.size()).arg(MAX_DISPLAY));
    }
}

/// 刷新 AST 查看器（树/文本）。
void Ide::updateAstViewer() {
    if (controller_->astRoot()) {
        astViewer_->setAst(controller_->astRoot());
    } else {
        astViewer_->clearAst();
    }
}

/// 刷新调试信息面板（变量快照/调用栈）。
void Ide::updateDebugInfo() {
    auto vars = controller_->getDebugVariableSnapshot();
    debugPanel_->updateVariables(vars);
    std::vector<CallStackEntry> stack;
    // BUG-DBG-6 fix: VM 模式下使用 VM 调用栈（VmStepper::getCallStack → IdeController::getVmCallStack），
    // 原 updateDebugInfo 始终使用 Interpreter 调用栈，VM 模式下显示空栈。
    // BUG-ORCH-6 fix: 用 isVmInitialized()/isVmRunning() 判断 VM 模式，
    // 不依赖 vmStackPanel_->isVisible()（dock 隐藏时走 Interpreter 路径不可靠）
    if (controller_->isVmInitialized() || controller_->isVmRunning()) {
        stack = controller_->getVmCallStack();
    } else {
        stack = controller_->getDebugCallStack();
    }
    debugPanel_->updateCallStack(stack);
}

/// 统一切换运行态：启用/禁用运行/调试/停止按钮与界面锁。
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
    // BUG-ORCH-4 fix: 运行/VM RUN 期间禁用引擎切换，避免中途切换导致状态不一致
    if (engineCombo_)
        engineCombo_->setEnabled(!running);
    if (codeEditor_)
        codeEditor_->setReadOnly(running);
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
            statusRunLabel_->setText(isDebug ? mlTr("调试中") : mlTr("运行中"));
        } else {
            statusRunLabel_->setText(QString());
        }
    }
}

// ============================================================
// File operations
// ============================================================

/// “新建”动作：创建空白标签页。
void Ide::onNew() {
    if (controller_->isRunning() || controller_->isVmRunning())
        return;
    int idx = createNewEditorTab();
    switchToTab(idx);
    ensureEditorVisible();
    if (codeEditor_)
        codeEditor_->setFocus();
}

/// “打开”动作：弹出文件对话框打开源文件。
void Ide::onOpen() {
    if (controller_->isRunning() || controller_->isVmRunning())
        return;
    QString startDir = workspaceDir_.isEmpty() ? QDir::homePath() : workspaceDir_;
    QString path =
        QFileDialog::getOpenFileName(this, mlTr("打开文件"), startDir, "MiniLang (*.mini *.ml);;All Files (*)");
    if (path.isEmpty())
        return;

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
    if (codeEditor_)
        codeEditor_->setFocus();
}

/// “打开文件夹”动作：打开工作区目录。
void Ide::onOpenFolder() {
    QString startDir = workspaceDir_.isEmpty() ? QDir::homePath() : workspaceDir_;
    QString dir = QFileDialog::getExistingDirectory(this, mlTr("打开文件夹"), startDir,
                                                    QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty())
        return;
    openWorkspace(dir);
}

/// “保存”动作：将当前编辑器内容写回文件。
void Ide::onSave() {
    if (!codeEditor_)
        return;
    if (currentFilePath_.isEmpty()) {
        onSaveAs();
        return;
    }
    // 标识 IDE 自身保存触发 fileChanged，避免弹出"外部修改"对话框。
    // 500ms 后自动复位，防止 fileChanged 未触发时标志残留误吞后续外部修改。
    selfSaving_ = true;
    QTimer::singleShot(500, this, [this]() {
        if (!closing_)
            selfSaving_ = false;
    });
    QFile file(currentFilePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        InfoBar::warning(mlTr("错误"), mlTr("无法保存文件: ") + file.errorString(), Qt::Horizontal, true, 2500,
                         InfoBar::Position::TOP_RIGHT, this);
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
    // 保存后更新文件监视（处理 Save As 后路径变更、文件被替换后旧 watch 失效）
    setupFileWatcher(currentFilePath_);
}

/// “另存为”动作：以新路径保存当前文件。
void Ide::onSaveAs() {
    QString startDir = workspaceDir_.isEmpty() ? QDir::homePath() : workspaceDir_;
    QString path =
        QFileDialog::getSaveFileName(this, mlTr("保存文件"), startDir, "MiniLang (*.mini *.ml);;All Files (*)");
    if (path.isEmpty())
        return;
    currentFilePath_ = path;
    // BUG-REPL-AUDIT-9 fix: 同步活动文件路径到 controller，供 REPL 模块加载器解析相对 import 路径
    controller_->setActiveFilePath(path.toStdString());
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

/// 保存前确认：存在修改时弹出保存对话框，返回是否可继续。
bool Ide::maybeSave() {
    if (!codeEditor_)
        return true;
    if (!codeEditor_->document()->isModified())
        return true;
    auto ret = QMessageBox::question(this, mlTr("MiniLang IDE"), mlTr("文件已修改，是否保存？"),
                                     QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (ret == QMessageBox::Save) {
        onSave();
        return !codeEditor_->document()->isModified();
    }
    if (ret == QMessageBox::Cancel)
        return false;
    return true;
}

/// 根据当前文件路径与脏标记刷新窗口标题。
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
    if (isDirty_)
        title += " *";
    setWindowTitle(title);

    // 第九轮：更新自定义标题栏路径标签（灰色小字号）
    if (titlePathLabel_) {
        QString path;
        if (!currentFilePath_.isEmpty()) {
            path = currentFilePath_;
            if (isDirty_)
                path += " *";
        } else if (!workspaceDir_.isEmpty()) {
            path = workspaceDir_;
        }
        titlePathLabel_->setText(path);
        titlePathLabel_->setToolTip(path);
    }
}

/// 加载文件到编辑器（含编码检测与 BOM 去除）。
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

// ============================================================
// 文件拖放支持（dragEnterEvent / dropEvent）
// ------------------------------------------------------------
// 主窗口接受从 Windows 资源管理器等拖入的 .mini/.ml 文件，
// 每个匹配文件调用 loadFile 加载到新标签。子控件（如欢迎页）
// 已自行 setAcceptDrops，事件优先派发给子控件，未接受时回退到主窗口。
// ============================================================

/// 拖拽进入事件：仅接受含 .mini/.ml 文件的拖放。
void Ide::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        const auto urls = event->mimeData()->urls();
        for (const QUrl& url : urls) {
            const QString path = url.toLocalFile();
            if (path.endsWith(".mini", Qt::CaseInsensitive) || path.endsWith(".ml", Qt::CaseInsensitive)) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

/// 拖拽放下事件：打开被拖入的源文件。
void Ide::dropEvent(QDropEvent* event) {
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    const auto urls = event->mimeData()->urls();
    bool loaded = false;
    for (const QUrl& url : urls) {
        const QString path = url.toLocalFile();
        if (path.isEmpty())
            continue;
        if (path.endsWith(".mini", Qt::CaseInsensitive) || path.endsWith(".ml", Qt::CaseInsensitive)) {
            loadFile(path);
            loaded = true;
        }
    }
    if (loaded) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

// ============================================================
// 文件外部修改监听（QFileSystemWatcher）
// ------------------------------------------------------------
// setupFileWatcher：切换/保存后更新被监视路径（一次只盯一个文件）
// onFileChangedExternally：外部编辑器修改/替换文件后弹框询问是否重载
//   - selfSaving_ 标志：IDE 自身 onSave 触发的 fileChanged 直接跳过弹框
//   - 文件被删除（Vim/某些编辑器先删再写）：100ms 后尝试重新 addPath
//   - 文件被替换：旧 watch 失效，需重新 addPath
// ============================================================

/// 为当前文件设置 QFileSystemWatcher 监视外部修改。
void Ide::setupFileWatcher(const QString& filePath) {
    if (!fileWatcher_)
        return;
    // 移除旧文件监视
    if (!watchedFilePath_.isEmpty() && fileWatcher_->files().contains(watchedFilePath_)) {
        fileWatcher_->removePath(watchedFilePath_);
    }
    // 添加新文件监视
    if (!filePath.isEmpty() && QFile::exists(filePath)) {
        fileWatcher_->addPath(filePath);
        watchedFilePath_ = filePath;
    } else {
        watchedFilePath_.clear();
    }
}

/// 文件被外部修改响应：提示用户重新加载。
void Ide::onFileChangedExternally(const QString& filePath) {
    // ROUND-60 fix: 关闭流程中忽略文件变更通知，避免 QTimer/singleShot 捕获 this
    // 在析构期间触发 UAF。
    if (closing_)
        return;
    if (filePath != watchedFilePath_)
        return;

    // 弹出对话框询问用户是否重新加载（异步避免阻塞 fileWatcher 信号）
    auto askReload = [this](const QString& path) {
        auto* msgBox = new QMessageBox(this);
        msgBox->setIcon(QMessageBox::Question);
        msgBox->setWindowTitle(mlTr("文件已修改"));
        msgBox->setText(mlTr("文件已被外部程序修改：\n%1\n\n是否重新加载？").arg(path));
        msgBox->setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox->setDefaultButton(QMessageBox::Yes);
        msgBox->setAttribute(Qt::WA_DeleteOnClose);
        connect(msgBox, &QMessageBox::finished, this, [this, path](int result) {
            if (result != QMessageBox::Yes)
                return;
            // 重新加载文件：已打开则从磁盘重读对应标签，否则新开标签
            int idx = findTabForFile(path);
            if (idx >= 0) {
                loadFileIntoTab(idx, path);
            } else {
                loadFile(path);
            }
        });
        msgBox->show();
    };

    // IDE 自身保存触发的 fileChanged，跳过弹框
    if (selfSaving_) {
        selfSaving_ = false;
        // 文件可能被替换后旧 watch 失效，重新添加
        if (!fileWatcher_->files().contains(filePath) && QFile::exists(filePath)) {
            fileWatcher_->addPath(filePath);
        }
        return;
    }

    // 文件可能已被删除（某些编辑器先删再写）
    if (!QFile::exists(filePath)) {
        QTimer::singleShot(100, this, [this, filePath, askReload]() {
            if (filePath != watchedFilePath_)
                return;
            if (QFile::exists(filePath)) {
                if (!fileWatcher_->files().contains(filePath)) {
                    fileWatcher_->addPath(filePath);
                }
                // 文件被重建，询问是否重新加载
                askReload(filePath);
            }
        });
        return;
    }

    // 重新添加监视（文件被替换后旧 watch 失效）
    if (!fileWatcher_->files().contains(filePath)) {
        fileWatcher_->addPath(filePath);
    }

    askReload(filePath);
}
