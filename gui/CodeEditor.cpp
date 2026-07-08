#include "gui/CodeEditor.h"
#include "gui/GuiTextUtils.h" // Dedup-4A: monospaceFont()
#include "gui/I18n.h"         // 功能 13：mlTr() 国际化
#include <QAbstractItemView>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QVBoxLayout>

// ============================================================
// CodeEditor 代码编辑器实现
// 负责行号区、断点（含条件）标记、错误下划线与精确范围、当前执行行高亮、
// 代码折叠、自动补全、括号匹配、注释切换/块注释，以及代码模板（snippet）
// 占位符导航等编辑器增强功能。
// ============================================================

// ============================================================
// LineNumberArea 行号区域
// ============================================================

LineNumberArea::LineNumberArea(QPlainTextEdit* editor, QWidget* parent) : QWidget(parent), editor_(editor) {
    setAutoFillBackground(true);
}

QSize LineNumberArea::sizeHint() const {
    return QSize(editor_->viewport()->width(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent* event) {
    CodeEditor* codeEditor = qobject_cast<CodeEditor*>(editor_);
    if (!codeEditor)
        return;

    QPainter painter(this);
    // F9: 主题感知背景色
    QColor bgColor = codeEditor->isDarkTheme_ ? QColor(37, 37, 37) : QColor(0xee, 0xe8, 0xd5);
    painter.fillRect(event->rect(), bgColor);

    // 字体只需设置一次（移出循环避免每行重建）
    // P2 fix: 使用 static const 避免 paintEvent 每次重绘都构造 QFont
    // Dedup-4A: 通过 GuiTextUtils::monospaceFont 共享全局 QFont 缓存
    painter.setFont(GuiTextUtils::monospaceFont(10));

    QTextBlock block = codeEditor->firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(codeEditor->blockBoundingGeometry(block).translated(codeEditor->contentOffset()).top());
    int bottom = top + static_cast<int>(codeEditor->blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            int lineNumber = blockNumber + 1;

            // 绘制断点标记
            if (codeEditor->breakpoints_.contains(lineNumber)) {
                // P-IDE-5 fix: 重新设计断点圆点样式——抗锯齿 + 径向渐变 + 外环描边，
                // 条件断点附加金色外环以增强视觉区分度。原实现在 paintEvent 全局
                // 无抗锯齿且用纯色 drawEllipse，圆点边缘锯齿明显，视觉粗糙。
                bool hasCondition = codeEditor->breakpointConditions_.contains(lineNumber) &&
                                    !codeEditor->breakpointConditions_[lineNumber].empty();
                // 抗锯齿：仅在此圆点绘制阶段开启，避免影响行号文字渲染
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing, true);

                int radius = 7;
                int cx = 14;
                int cy = (top + bottom) / 2;

                // 主题感知核心色：无条件=红色，条件=橙色（与原语义一致，但采用更柔和的色值）
                QColor coreColor = hasCondition ? QColor(0xF5, 0xA6, 0x23) : QColor(0xE4, 0x3B, 0x44);
                QColor ringColor = hasCondition ? QColor(0xC2, 0x7A, 0x0E) : QColor(0xA8, 0x24, 0x2C);
                QColor glowColor = hasCondition ? QColor(0xF5, 0xA6, 0x23, 60) : QColor(0xE4, 0x3B, 0x44, 60);

                // 1) 外发光（半透明大圆，营造柔和光晕）
                painter.setPen(Qt::NoPen);
                painter.setBrush(glowColor);
                painter.drawEllipse(QPointF(cx, cy), radius + 3.0, radius + 3.0);

                // 2) 主圆点——径向渐变（中心高光 → 边缘深色），产生立体感
                QRadialGradient gradient(QPointF(cx - radius * 0.3, cy - radius * 0.3), radius * 1.4);
                QColor highlight = coreColor.lighter(140);
                gradient.setColorAt(0.0, highlight);
                gradient.setColorAt(1.0, coreColor);
                painter.setBrush(QBrush(gradient));
                painter.setPen(QPen(ringColor, 1.2));
                painter.drawEllipse(QPointF(cx, cy), radius, radius);

                // 3) 条件断点：外环金色装饰（带间隙的双层圆环）
                if (hasCondition) {
                    painter.setBrush(Qt::NoBrush);
                    painter.setPen(QPen(QColor(0xC2, 0x7A, 0x0E, 180), 1.0));
                    painter.drawEllipse(QPointF(cx, cy), radius + 2.0, radius + 2.0);
                }
                painter.restore();
            }

            // 绘制行号
            // F9: 主题感知文字颜色
            QColor numColor = codeEditor->isDarkTheme_ ? QColor(115, 115, 115) : QColor(0x65, 0x7b, 0x83);
            painter.setPen(numColor);
            painter.drawText(0, top, width() - 20, bottom - top, Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(lineNumber));

            // F8: 绘制折叠标记（右侧）—— VS Code 风格雪佛龙箭头
            // 展开态：向下箭头 ▽（提示可折叠）；折叠态：向右箭头 ▷（提示可展开）
            // 采用抗锯齿绘制 + 主题感知配色，无方框背景，视觉更轻盈
            if (codeEditor->isFoldable(block)) {
                bool folded = codeEditor->isFolded(blockNumber);
                const int cx = width() - 9;
                const int cy = (top + bottom) / 2;
                // F9: 主题感知折叠标记颜色——折叠态用主题蓝强调，展开态用次要灰
                QColor arrowColor =
                    folded ? (codeEditor->isDarkTheme_ ? QColor(0x6c, 0x71, 0xc4) : QColor(0x26, 0x8b, 0xd2))
                           : (codeEditor->isDarkTheme_ ? QColor(0x93, 0xa1, 0xa1) : QColor(0x58, 0x6e, 0x75));
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(QPen(arrowColor, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                const int s = 4; // 雪佛龙半臂长度
                if (folded) {
                    // 向右箭头 ▷：从左上到右尖再到左下
                    painter.drawLine(cx - s, cy - s, cx + s, cy);
                    painter.drawLine(cx + s, cy, cx - s, cy + s);
                } else {
                    // 向下箭头 ▽：从左上到下尖再到右上
                    painter.drawLine(cx - s, cy - s, cx, cy + s);
                    painter.drawLine(cx, cy + s, cx + s, cy - s);
                }
                painter.restore();
            }
        }

        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(codeEditor->blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void LineNumberArea::mousePressEvent(QMouseEvent* event) {
    CodeEditor* codeEditor = qobject_cast<CodeEditor*>(editor_);
    if (!codeEditor)
        return;

    // F8: 检查是否点击了折叠区域（右侧 14px）
    int clickX = static_cast<int>(event->position().x());
    // 与 paintEvent 绘制区域一致：雪佛龙箭头中心 cx = width()-9，臂长 4，
    // 覆盖 [width()-13, width()-5)。命中区统一为 [width()-14, width()-4)。
    if (clickX >= width() - 14 && clickX < width() - 4) {
        // 映射 Y 坐标到块号
        QTextCursor cursor = codeEditor->cursorForPosition(QPoint(0, static_cast<int>(event->position().y())));
        int blockNumber = cursor.blockNumber();
        QTextBlock block = codeEditor->document()->findBlockByNumber(blockNumber);
        if (block.isValid() && codeEditor->isFoldable(block)) {
            codeEditor->toggleFold(blockNumber);
            return;
        }
    }

    // 将 Y 坐标映射到行号
    QTextCursor cursor = codeEditor->cursorForPosition(QPoint(0, static_cast<int>(event->position().y())));
    int lineNumber = cursor.blockNumber() + 1;

    // AUDIT-BUG-F13 fix: 检查点击是否在文档内容区域内。
    // cursorForPosition 对最后一行下方的空白区域返回文档末尾光标（最后一个块号），
    // 导致空白区域点击在最后一行设置断点。检查 Y 坐标是否超出最后一个块的下边界。
    QTextBlock lastBlock = codeEditor->document()->lastBlock();
    qreal lastBlockBottom = static_cast<qreal>(
        codeEditor->blockBoundingGeometry(lastBlock).translated(codeEditor->contentOffset()).bottom());
    if (static_cast<qreal>(event->position().y()) >= lastBlockBottom) {
        return; // 点击在最后一行下方的空白区域，不设置断点
    }

    // DB-2 fix: 不允许在空行/纯注释行设置断点
    QTextBlock block = codeEditor->document()->findBlockByNumber(lineNumber - 1);
    if (block.isValid()) {
        QString text = block.text().trimmed();
        // BUG-CE-7 fix: 不仅识别行注释 //，也识别块注释行（userState >= 100
        // 或向后兼容编码 2 表示块注释跨行上下文）
        int s = block.userState();
        bool isCommentLine = text.startsWith("//") || s == 2 || s >= 100;
        if (text.isEmpty() || isCommentLine) {
            return; // 跳过不可执行行
        }
    }

    if (codeEditor->breakpoints_.contains(lineNumber)) {
        codeEditor->breakpoints_.remove(lineNumber);
        codeEditor->breakpointConditions_.remove(lineNumber);
    } else {
        codeEditor->breakpoints_.insert(lineNumber);
    }
    update();

    // 更新断点显示
    codeEditor->viewport()->update();

    // AUDIT-P2-CORRECT fix: 发射 breakpointsChanged 信号，通知上层（Ide）
    // 在调试暂停期间同步断点到 DebugController，避免新断点不生效/已删除断点仍触发。
    emit codeEditor->breakpointsChanged();
}

void LineNumberArea::contextMenuEvent(QContextMenuEvent* event) {
    CodeEditor* codeEditor = qobject_cast<CodeEditor*>(editor_);
    if (!codeEditor)
        return;

    // 将 Y 坐标映射到行号
    QTextCursor cursor = codeEditor->cursorForPosition(QPoint(0, static_cast<int>(event->pos().y())));
    int lineNumber = cursor.blockNumber() + 1;

    // BUG-CE-5 fix: 与 mousePressEvent 一致，检查点击是否在文档内容区域内。
    // cursorForPosition 对最后一行下方的空白区域返回文档末尾光标（最后一个块号），
    // 导致空白区域右键在最后一行误触发条件菜单。
    QTextBlock lastBlock = codeEditor->document()->lastBlock();
    qreal lastBlockBottom = static_cast<qreal>(
        codeEditor->blockBoundingGeometry(lastBlock).translated(codeEditor->contentOffset()).bottom());
    if (static_cast<qreal>(event->pos().y()) >= lastBlockBottom) {
        return; // 点击在最后一行下方的空白区域
    }

    if (!codeEditor->breakpoints_.contains(lineNumber))
        return;

    QMenu menu(this);
    QString currentCond = QString::fromStdString(codeEditor->getBreakpointCondition(lineNumber));

    QAction* setCondAction = menu.addAction(currentCond.isEmpty() ? QString("设置条件... (行 %1)").arg(lineNumber)
                                                                  : QString("修改条件: \"%1\"").arg(currentCond));

    QAction* removeCondAction = nullptr;
    if (!currentCond.isEmpty()) {
        removeCondAction = menu.addAction("移除条件");
    }

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == setCondAction) {
        // P-IDE-5 fix: 用自定义 Fluent 风格对话框替代默认 QInputDialog，
        // 统一 IDE 视觉语言（圆角/主色按钮/提示文案色板）。原 QInputDialog 在
        // Windows 原生主题下显得突兀，与 Fluent Design 风格不一致。
        QDialog dlg(this);
        dlg.setWindowTitle(QString("设置断点条件"));
        dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
        dlg.setFixedSize(420, 180);

        // Fluent 色板（亮色主题，与 IDE 整体风格一致）
        const QString bgSurf = "#FDF6E3";
        const QString fgPrim = "#002B36";
        const QString fgSec = "#657B83";
        const QString accent = "#268BD2";
        const QString border = "#93A1A1";
        const QString warnFg = "#B58900";
        const QString btnBg = "#268BD2";
        const QString btnHov = "#1E6FA3";

        dlg.setStyleSheet(
            QString("QDialog { background: %1; border-radius: 8px; }"
                    "QLabel#condTitle { font-size: 14px; font-weight: 600; color: %2; }"
                    "QLabel#condHint  { font-size: 11px; color: %3; }"
                    "QLabel#condWarn  { font-size: 11px; color: %4; }"
                    "QLineEdit#condEdit { "
                    "  background: #FFFFFF; color: %2; "
                    "  border: 1px solid %5; border-radius: 4px; "
                    "  padding: 6px 8px; font-family: 'Consolas','Cascadia Mono','Courier New',monospace; "
                    "  font-size: 13px; "
                    "} "
                    "QLineEdit#condEdit:focus { border: 1px solid %6; }"
                    "QPushButton#condOk { "
                    "  background: %7; color: #FFFFFF; border: none; border-radius: 4px; "
                    "  padding: 6px 18px; font-size: 13px; font-weight: 500; "
                    "} "
                    "QPushButton#condOk:hover { background: %8; }"
                    "QPushButton#condCancel { "
                    "  background: transparent; color: %3; border: 1px solid %5; border-radius: 4px; "
                    "  padding: 6px 14px; font-size: 13px; "
                    "} "
                    "QPushButton#condCancel:hover { background: rgba(0,0,0,0.05); }")
                .arg(bgSurf, fgPrim, fgSec, warnFg, border, accent, btnBg, btnHov));

        auto* layout = new QVBoxLayout(&dlg);
        layout->setContentsMargins(20, 16, 20, 16);
        layout->setSpacing(8);

        auto* titleLabel = new QLabel(QString("断点条件 · 行 %1").arg(lineNumber), &dlg);
        titleLabel->setObjectName("condTitle");
        layout->addWidget(titleLabel);

        auto* hintLabel = new QLabel(QString("输入条件表达式（例如 i == 5 或 x > 10）"), &dlg);
        hintLabel->setObjectName("condHint");
        hintLabel->setWordWrap(true);
        layout->addWidget(hintLabel);

        auto* edit = new QLineEdit(currentCond, &dlg);
        edit->setObjectName("condEdit");
        edit->setPlaceholderText("留空则变为无条件断点");
        layout->addWidget(edit);

        auto* warnLabel = new QLabel(QString("提示：条件为假时断点不会暂停；语法错误会导致断点失效。"), &dlg);
        warnLabel->setObjectName("condWarn");
        warnLabel->setWordWrap(true);
        layout->addWidget(warnLabel);

        auto* btnRow = new QHBoxLayout();
        btnRow->addStretch();
        auto* cancelBtn = new QPushButton("取消", &dlg);
        cancelBtn->setObjectName("condCancel");
        auto* okBtn = new QPushButton("确定", &dlg);
        okBtn->setObjectName("condOk");
        btnRow->addWidget(cancelBtn);
        btnRow->addSpacing(8);
        btnRow->addWidget(okBtn);
        layout->addLayout(btnRow);

        connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
        edit->setFocus();
        edit->selectAll();

        if (dlg.exec() == QDialog::Accepted) {
            QString cond = edit->text();
            QString condTrimmed = cond.trimmed();
            std::string condStr = condTrimmed.toStdString();
            if (condStr.empty()) {
                codeEditor->breakpointConditions_.remove(lineNumber);
            } else {
                codeEditor->breakpointConditions_[lineNumber] = condStr;
            }
            emit codeEditor->breakpointConditionRequested(lineNumber, condTrimmed);
            update(); // 刷新颜色（红/橙）
        }
    } else if (chosen == removeCondAction) {
        codeEditor->breakpointConditions_.remove(lineNumber);
        emit codeEditor->breakpointConditionRequested(lineNumber, QString());
        update();
    }
}

// ============================================================
// CodeEditor 代码编辑器
// ============================================================

CodeEditor::CodeEditor(QWidget* parent) : QPlainTextEdit(parent) {
    lineNumberArea_ = new LineNumberArea(this, this);

    connect(this, &CodeEditor::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &CodeEditor::updateRequest, this, &CodeEditor::updateLineNumberArea);

    // QT-R-08 fix: 光标行高亮用 QTimer 防抖（16ms ≈ 60fps），避免快速移动光标时
    // 频繁 setExtraSelections 触发重绘导致卡顿/闪烁
    lineHighlightTimer_ = new QTimer(this);
    lineHighlightTimer_->setSingleShot(true);
    lineHighlightTimer_->setInterval(16);
    connect(lineHighlightTimer_, &QTimer::timeout, this, &CodeEditor::highlightCurrentLine);
    connect(this, &CodeEditor::cursorPositionChanged, this, [this]() {
        if (lineHighlightTimer_)
            lineHighlightTimer_->start();
        // 功能 13：光标移出当前占位符范围时清理 snippet 导航状态。
        // isSnippetNavigating_ 用于排除程序化光标移动（Tab 跳转 / 初始展开选中）。
        if (currentPlaceholderIdx_ >= 0 && !isSnippetNavigating_) {
            const auto& range = currentPlaceholders_[currentPlaceholderIdx_];
            const int pos = textCursor().position();
            // 占位符范围 [start, end)，光标在 [start, end] 内视为有效（含 end
            // 便于紧贴占位符末尾编辑）。超出此区间则退出导航模式。
            if (pos < range.first || pos > range.second) {
                clearSnippetState();
            }
        }
    });

    // BUG-CE-2/CE-3 fix: 监听文档内容变化，文档修改后调整断点/折叠块号偏移。
    // 原实现未监听 contentsChange，文档修改后断点和折叠停留在原行号，导致行号错位。
    contentsChangeConn_ = connect(document(), &QTextDocument::contentsChange, this, &CodeEditor::onContentsChange);
    lastBlockCount_ = document()->blockCount();

    // 功能 13：监听文档内容变化，同步更新 snippet 占位符范围。
    // 与 onContentsChange 独立连接，互不干扰——breakpoint 偏移按行号 delta，
    // 占位符偏移按字符 delta，两者维度不同。
    connect(document(), &QTextDocument::contentsChange, this, [this](int position, int charsRemoved, int charsAdded) {
        updatePlaceholderRanges(position, charsRemoved, charsAdded);
    });

    // H2: 光标移动时高亮匹配的括号
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightBracketMatch);

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();

    // 设置字体
    // Dedup-4A: monospaceFont 共享缓存
    setFont(GuiTextUtils::monospaceFont(11));
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);

    // F13: 初始化自动补全器
    completionModel_ = new QStringListModel(this);
    completer_ = new QCompleter(this);
    completer_->setModel(completionModel_);
    completer_->setWidget(this);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setFilterMode(Qt::MatchStartsWith);
    connect(completer_, QOverload<const QString&>::of(&QCompleter::activated), this, &CodeEditor::insertCompletion);

    // P-IDE-5 fix: 为补全弹窗应用 Fluent 风格 QSS，与 IDE 整体视觉语言统一。
    // 默认 QListView popup 在 Windows 原生主题下边框生硬、选中色为蓝色块状，
    // 与 IDE 的 Solarized 亮色 + Fluent 圆角风格不协调。
    if (completer_->popup()) {
        completer_->popup()->setStyleSheet("QListView { "
                                           "  background: #FDF6E3; "
                                           "  color: #002B36; "
                                           "  border: 1px solid #93A1A1; "
                                           "  border-radius: 6px; "
                                           "  padding: 4px; "
                                           "  font-family: 'Consolas','Cascadia Mono','Courier New',monospace; "
                                           "  font-size: 12px; "
                                           "  outline: none; "
                                           "} "
                                           "QListView::item { "
                                           "  padding: 4px 10px; "
                                           "  border-radius: 4px; "
                                           "} "
                                           "QListView::item:selected { "
                                           "  background: #268BD2; "
                                           "  color: #FFFFFF; "
                                           "} "
                                           "QListView::item:hover:!selected { "
                                           "  background: rgba(38, 139, 210, 0.12); "
                                           "  color: #002B36; "
                                           "} "
                                           "QScrollBar:vertical { "
                                           "  background: transparent; width: 8px; margin: 2px; "
                                           "} "
                                           "QScrollBar::handle:vertical { "
                                           "  background: #93A1A1; border-radius: 4px; min-height: 20px; "
                                           "} "
                                           "QScrollBar::handle:vertical:hover { background: #657B83; } "
                                           "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }");
    }
}

int CodeEditor::lineNumberAreaWidth() {
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    // 行号区域宽度 = 数字宽度 * 位数 + 断点空间 + 边距
    int space = 30 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits + 6;
    return space;
}

void CodeEditor::setErrorLines(const QSet<int>& lines) {
    errorLines_ = lines;

    // 构建并缓存错误行选择（仅在 errorLines_ 变化时重建）
    // 视觉优化：波浪线改用 Solarized 红 #DC322F（比 Qt::red 更柔和、与主题一致），
    // 并叠加极淡红色背景（alpha=18）增强错误行可见性，对齐 VS Code 错误行高亮风格。
    cachedErrorSelections_.clear();
    const QColor squiggleColor(0xDC, 0x32, 0x2F);
    const QColor errLineBg(0xDC, 0x32, 0x2F, 18);
    for (int line : errorLines_) {
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(block);
            sel.cursor.select(QTextCursor::LineUnderCursor);
            sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            sel.format.setUnderlineColor(squiggleColor);
            sel.format.setBackground(errLineBg);
            cachedErrorSelections_.append(sel);
        }
    }
    highlightCurrentLine();
}

void CodeEditor::setErrorRanges(const std::vector<ErrorRange>& ranges) {
    errorLines_.clear();
    cachedErrorSelections_.clear();
    // 精确 token 标记：仅波浪线（无背景），避免短 token 背景闪烁。
    const QColor squiggleColor(0xDC, 0x32, 0x2F);
    for (const auto& r : ranges) {
        errorLines_.insert(r.line);
        QTextBlock block = document()->findBlockByNumber(r.line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            int startPos = block.position();
            // EU-1 fix: 从列位置开始，精确标记错误 token
            int col = (r.column > 0) ? r.column - 1 : 0;
            int blockTextLen = block.length() - 1; // block.length() includes the newline
            // BUG-CE-4 fix: 空行跳过，避免 len=1 选中换行符产生无效选区
            if (blockTextLen <= 0)
                continue;
            if (col >= blockTextLen)
                col = (blockTextLen > 0) ? blockTextLen - 1 : 0;
            int len = (r.length > 0) ? r.length : blockTextLen - col;
            if (len <= 0)
                len = blockTextLen - col;
            if (len <= 0)
                len = 1;
            if (col + len > blockTextLen)
                len = blockTextLen - col;
            sel.cursor = QTextCursor(document());
            sel.cursor.setPosition(startPos + col);
            sel.cursor.setPosition(startPos + col + len, QTextCursor::KeepAnchor);
            sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            sel.format.setUnderlineColor(squiggleColor);
            cachedErrorSelections_.append(sel);
        }
    }
    highlightCurrentLine();
}

void CodeEditor::clearErrorLines() {
    errorLines_.clear();
    cachedErrorSelections_.clear();
    highlightCurrentLine();
}

void CodeEditor::setCurrentLine(int line) {
    currentLine_ = line;
    highlightCurrentLine();
}

void CodeEditor::clearCurrentLine() {
    currentLine_ = -1;
    highlightCurrentLine();
}

void CodeEditor::gotoLine(int line) {
    if (line <= 0)
        return;
    QTextBlock block = document()->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;
    QTextCursor cursor(block);
    setTextCursor(cursor);
    centerCursor();
    setFocus();
}

void CodeEditor::highlightSourceLine(int line) {
    sourceHighlightLine_ = line;
    // 同时跳转到该行并居中显示
    if (line > 0) {
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextCursor cursor(block);
            setTextCursor(cursor);
            centerCursor();
        }
    }
    highlightCurrentLine();
}

void CodeEditor::clearSourceHighlight() {
    sourceHighlightLine_ = -1;
    highlightCurrentLine();
}

QSet<int> CodeEditor::getBreakpoints() const {
    return breakpoints_;
}

void CodeEditor::setBreakpoints(const QSet<int>& breakpoints) {
    breakpoints_.clear();
    // DB-2 fix: 过滤空行和纯注释行，与 mousePressEvent 行为一致
    for (int line : breakpoints) {
        // G-P1-2 fix: 校验行号合法性，过滤 <= 0 的无效行号避免 findBlockByNumber 越界
        if (line <= 0)
            continue;
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QString text = block.text().trimmed();
            // BUG-CE-7 fix: 不仅识别行注释 //，也识别块注释行（userState >= 100
            // 或向后兼容编码 2 表示块注释跨行上下文）
            int s = block.userState();
            bool isCommentLine = text.startsWith("//") || s == 2 || s >= 100;
            if (text.isEmpty() || isCommentLine)
                continue;
        }
        breakpoints_.insert(line);
    }
    // 清理不再有效的断点条件
    for (auto it = breakpointConditions_.begin(); it != breakpointConditions_.end();) {
        if (!breakpoints_.contains(it.key())) {
            it = breakpointConditions_.erase(it);
        } else {
            ++it;
        }
    }
    lineNumberArea_->update();
}

std::string CodeEditor::getBreakpointCondition(int line) const {
    auto it = breakpointConditions_.find(line);
    if (it != breakpointConditions_.end()) {
        return it.value();
    }
    return "";
}

void CodeEditor::onContentsChange(int position, int charsRemoved, int charsAdded) {
    Q_UNUSED(charsRemoved);
    // AUDIT-P1-CORRECT fix: charsAdded 用于多行删除场景推导实际删除行数，
    // 不再 Q_UNUSED 丢弃。

    // BUG-CE-2/CE-3 fix: 文档内容变化后，断点（1-based 行号）、断点条件、
    // 折叠块（0-based blockNumber）以行号为 key，文档修改导致行号变化时需同步偏移。
    //
    // 实现说明：contentsChange 在变更已应用后发射，被删除文本不可访问，无法直接
    // 统计其换行符数量。改为通过文档块数变化计算 delta（lastBlockCount_ 在上次
    // 变更后/构造时更新），保证纯插入/纯删除/替换三类场景均正确。
    int newBlockCount = document()->blockCount();
    int delta = newBlockCount - lastBlockCount_;
    lastBlockCount_ = newBlockCount;
    if (delta == 0)
        return;

    // 计算 position 所在行（1-based），仅调整 >= startLine 的行号
    QTextBlock block = document()->findBlock(position);
    if (!block.isValid())
        return;
    int startLine = block.blockNumber() + 1; // 1-based，变更起始行

    // BUG-GUI-AUDIT-2 fix: 区分"行末插入换行符"与"行首插入换行符"两种场景。
    //   - 行首插入换行符（光标在行首按 Enter）：新行创建在当前行之前，原行内容下移，
    //     断点应 +1（当前实现已正确）。
    //   - 行末插入换行符（光标在行末按 Enter）：新行创建在当前行之后，原行内容不变，
    //     断点应保持原行号（不应 +1）。原实现错误地对原行断点也 +1。
    //   - 删除整行（含换行符）：原行内容消失，下移内容上移。VS Code 语义是断点保持
    //     原行号（指向新移入的内容），原实现错误地 -1。
    // 通过检查 position 是否位于块末尾换行符位置来区分插入场景。
    // block.length() 含换行符，position == block.position() + block.length() - 1
    // 表示光标在块末尾换行符之前（即行末）。
    if (delta > 0) {
        // 插入场景：若插入发生在块末尾换行符位置，原行内容不变，断点不应偏移。
        // 将 startLine +1 使原行 line < startLine，不参与偏移。
        // AUDIT-P1-CORRECT fix: 添加 block.length() > 1 条件区分"行末插入"与"行首插入"。
        //   - 行末插入：原块有内容（length > 1），position 在块末尾换行符位置 → 行末语义
        //   - 行首插入：新块为空（length == 1），position 在块开头 → 行首语义（原行内容下移，断点应 +1）
        // 原实现遗漏 length > 1 条件，导致行首插入被误判为行末插入，startLine 错误 +1，
        // 原行断点未随内容下移（停留在新空行而非跟随原内容）。
        if (block.length() > 1 && position >= block.position() + block.length() - 1) {
            ++startLine;
        }
    } else if (delta < 0) {
        // AUDIT-P1-CORRECT fix: 多行删除需正确处理被删除行区间内的断点。
        // 原实现用 delta（NET 行数变化）统一偏移所有 >= startLine 的断点，
        // 仅通过 startLine +1 保护一行。对于多行删除（delta = -N），中间被删除行
        // 的断点被错误平移而非删除，导致断点指向错误行号。
        //
        // 利用 charsAdded 推导实际删除的行数：linesRemoved = linesAdded - delta
        // （delta 为负，linesAdded 通常为 0 表示纯删除；替换场景 linesAdded > 0）
        int linesAdded = 0;
        if (charsAdded > 0) {
            for (int i = position; i < position + charsAdded && i < document()->characterCount(); ++i) {
                if (document()->characterAt(i) == '\n')
                    ++linesAdded;
            }
        }
        int linesRemoved = linesAdded - delta; // 实际删除的行数（delta 为负）

        if (linesRemoved > 0) {
            // 多行删除：需要删除被删除行区间内的断点
            int firstDeletedLine = startLine;                   // 删除起始行
            int lastDeletedLine = startLine + linesRemoved - 1; // 最后被删除行
            // 断点 line <= firstDeletedLine：保持（VS Code 语义，指向合并后内容）
            // 断点 firstDeletedLine < line <= lastDeletedLine：删除（不插入）
            // 断点 line > lastDeletedLine：平移 delta
            QSet<int> newBreakpoints;
            QSet<int> breakpointsToRemove;
            for (int line : breakpoints_) {
                if (line <= firstDeletedLine) {
                    newBreakpoints.insert(line);
                } else if (line > lastDeletedLine) {
                    int newLine = line + delta;
                    if (newLine > 0)
                        newBreakpoints.insert(newLine);
                } else {
                    breakpointsToRemove.insert(line);
                }
            }
            breakpoints_ = newBreakpoints;
            // 同理处理 breakpointConditions_：移除被删除行的条件
            for (int line : breakpointsToRemove) {
                breakpointConditions_.remove(line);
            }
            // 同理处理 foldedBlocks_（0-based blockNumber，与 1-based line 同步偏移）
            QSet<int> newFolded;
            for (int line : foldedBlocks_) {
                if (line <= firstDeletedLine) {
                    newFolded.insert(line);
                } else if (line > lastDeletedLine) {
                    int newLine = line + delta;
                    if (newLine > 0)
                        newFolded.insert(newLine);
                }
                // else: 在删除区间内，丢弃
            }
            foldedBlocks_ = newFolded;
            update();
            emit breakpointsChanged();
            return;
        }
        // linesRemoved == 0：单行内删除（不改变行数，但 delta < 0 不应到达此处）
        // 回退到原有单行删除逻辑
        if (position == block.position()) {
            ++startLine;
        }
    }

    // 调整断点行号（1-based）
    QSet<int> newBreakpoints;
    newBreakpoints.reserve(breakpoints_.size());
    for (int line : breakpoints_) {
        if (line < startLine) {
            newBreakpoints.insert(line);
        } else {
            int newLine = line + delta;
            if (newLine > 0)
                newBreakpoints.insert(newLine);
        }
    }
    breakpoints_ = newBreakpoints;

    // 调整断点条件（1-based）
    QMap<int, std::string> newConditions;
    for (auto it = breakpointConditions_.begin(); it != breakpointConditions_.end(); ++it) {
        int line = it.key();
        if (line < startLine) {
            newConditions[line] = it.value();
        } else {
            int newLine = line + delta;
            if (newLine > 0)
                newConditions[newLine] = it.value();
        }
    }
    breakpointConditions_ = std::move(newConditions);

    // BUG-CE-3 fix: 调整 foldedBlocks_（0-based blockNumber）
    QSet<int> newFolded;
    newFolded.reserve(foldedBlocks_.size());
    int startBlockNumber = startLine - 1; // 0-based
    for (int bn : foldedBlocks_) {
        if (bn < startBlockNumber) {
            newFolded.insert(bn);
        } else {
            int newBn = bn + delta;
            if (newBn >= 0)
                newFolded.insert(newBn);
        }
    }
    foldedBlocks_ = newFolded;

    update();
}

void CodeEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    QRect cr = contentsRect();
    lineNumberArea_->setGeometry(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height());
}

void CodeEditor::updateLineNumberAreaWidth(int newBlockCount) {
    Q_UNUSED(newBlockCount);
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::highlightCurrentLine() {
    QList<QTextEdit::ExtraSelection> selections;

    // GUI-12 fix: 光标行先添加（蓝色），执行行后添加（黄色）
    // Qt ExtraSelection 后添加的覆盖先添加的，黄色要在蓝色之上
    // F9: 主题感知高亮颜色
    QColor cursorLineColor = isDarkTheme_ ? QColor(40, 44, 48) : QColor(0xee, 0xe8, 0xd5);
    QColor execLineColor = isDarkTheme_ ? QColor(86, 90, 46) : QColor(0xee, 0xe8, 0xd5);

    // BUG-CE-8 fix: 查找高亮先添加（底层），错误下划线次之（不冲突），
    // 光标行再次（蓝色覆盖查找高亮），执行行最后（黄色覆盖光标行和查找高亮）。
    // 原顺序将 findSelections_ 放在最后，导致查找高亮覆盖执行行高亮。
    // BUG 4.2 fix: 合并查找高亮，不覆盖编辑器自身 selections
    selections.append(findSelections_);

    // 错误下划线（使用预构建的缓存，避免每次光标移动都遍历）
    selections.append(cachedErrorSelections_);

    // H2: 括号匹配高亮（在错误下划线之后、光标行之前，避免覆盖）
    selections.append(bracketSelections_);

    QTextEdit::ExtraSelection cursorSel;
    cursorSel.cursor = textCursor();
    cursorSel.cursor.select(QTextCursor::LineUnderCursor);
    cursorSel.format.setBackground(cursorLineColor);
    cursorSel.format.setProperty(QTextCharFormat::FullWidthSelection, true);
    selections.append(cursorSel);

    // 当前执行行高亮（黄色背景，后添加以覆盖蓝色和查找高亮）
    if (currentLine_ > 0) {
        QTextBlock block = document()->findBlockByNumber(currentLine_ - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(block);
            sel.cursor.select(QTextCursor::LineUnderCursor);
            sel.format.setBackground(execLineColor);
            sel.format.setProperty(QTextCharFormat::FullWidthSelection, true);
            selections.append(sel);
        }
    }

    // IR/字节码面板点击高亮（蓝色背景，最后添加以覆盖所有其他高亮）
    if (sourceHighlightLine_ > 0) {
        QColor srcHighlightColor = isDarkTheme_ ? QColor(30, 58, 95) : QColor(0xbf, 0xdb, 0xfe);
        QTextBlock block = document()->findBlockByNumber(sourceHighlightLine_ - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(block);
            sel.cursor.select(QTextCursor::LineUnderCursor);
            sel.format.setBackground(srcHighlightColor);
            sel.format.setProperty(QTextCharFormat::FullWidthSelection, true);
            selections.append(sel);
        }
    }

    setExtraSelections(selections);
}

void CodeEditor::setFindSelections(const QList<QTextEdit::ExtraSelection>& selections) {
    findSelections_ = selections;
    highlightCurrentLine(); // 触发重绘，合并所有 selections
}

void CodeEditor::clearFindSelections() {
    findSelections_.clear();
    highlightCurrentLine();
}

void CodeEditor::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy) {
        lineNumberArea_->scroll(0, dy);
    } else {
        lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);
    }
}

// ============================================================
// F8: 代码折叠
// ============================================================

namespace {
// 统计单行中的大括号深度变化，跳过字符串和注释
// inBlockComment 跨行追踪块注释状态
int countBracesInLine(const QString& text, bool& inBlockComment) {
    int depth = 0;
    bool inString = false;
    for (int i = 0; i < text.size(); ++i) {
        QChar c = text[i];
        if (inBlockComment) {
            if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
                inBlockComment = false;
                i++;
            }
            continue;
        }
        if (inString) {
            if (c == '\\' && i + 1 < text.size()) {
                i++; // 跳过转义字符
                continue;
            }
            if (c == '"')
                inString = false;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            break; // 行注释，忽略后续内容
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            inBlockComment = true;
            i++;
            continue;
        }
        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == '{')
            depth++;
        else if (c == '}')
            depth--;
    }
    return depth;
}
} // namespace

bool CodeEditor::isFoldable(const QTextBlock& block) const {
    if (!block.isValid())
        return false;
    // P2 fix: 使用语法高亮器的块状态 (userState) 判断是否处于块注释中，O(1) 而非 O(N)
    // BUG-CE-1 fix: 兼容两种块注释状态编码——向后兼容编码 2（depth=1）
    // 与嵌套编码 100+depth（depth>=1）。原代码仅检查 == 2，导致 SyntaxHighlighter
    // 设置的 100+depth 状态不被识别，折叠判定在块注释行失效。
    bool inBlockComment = false;
    QTextBlock prev = block.previous();
    if (prev.isValid()) {
        int s = prev.userState();
        if (s == 2 || s >= 100) {
            inBlockComment = true;
        }
    }
    int depth = countBracesInLine(block.text(), inBlockComment);
    return depth > 0;
}

bool CodeEditor::isFolded(int blockNumber) const {
    return foldedBlocks_.contains(blockNumber);
}

int CodeEditor::foldEndBlock(const QTextBlock& startBlock) const {
    if (!startBlock.isValid())
        return -1;

    // P2 fix: 使用语法高亮器的块状态 (userState) 判断是否处于块注释中，O(1) 而非 O(N)
    // BUG-CE-1 fix: 同 isFoldable，兼容状态编码 2 与 100+depth
    bool inBlockComment = false;
    QTextBlock prev = startBlock.previous();
    if (prev.isValid()) {
        int s = prev.userState();
        if (s == 2 || s >= 100) {
            inBlockComment = true;
        }
    }
    int depth = countBracesInLine(startBlock.text(), inBlockComment);
    if (depth <= 0)
        return -1;

    QTextBlock block = startBlock.next();
    while (block.isValid()) {
        depth += countBracesInLine(block.text(), inBlockComment);
        if (depth <= 0)
            return block.blockNumber();
        block = block.next();
    }
    return -1;
}

void CodeEditor::toggleFold(int blockNumber) {
    QTextBlock block = document()->findBlockByNumber(blockNumber);
    if (!block.isValid() || !isFoldable(block))
        return;

    int endNum = foldEndBlock(block);
    if (endNum < 0 || endNum <= blockNumber)
        return;

    if (foldedBlocks_.contains(blockNumber)) {
        // 展开：显示范围内的块，但跳过仍折叠的嵌套块的子块
        foldedBlocks_.remove(blockNumber);
        int i = blockNumber + 1;
        while (i <= endNum) {
            QTextBlock b = document()->findBlockByNumber(i);
            if (!b.isValid())
                break;
            b.setVisible(true);
            // 如果此块自身也处于折叠状态，跳过其子块
            if (foldedBlocks_.contains(i)) {
                int childEnd = foldEndBlock(b);
                if (childEnd > i) {
                    i = childEnd + 1;
                    continue;
                }
            }
            i++;
        }
    } else {
        // 折叠：隐藏范围内的所有块
        foldedBlocks_.insert(blockNumber);
        for (int i = blockNumber + 1; i <= endNum; ++i) {
            QTextBlock b = document()->findBlockByNumber(i);
            if (b.isValid())
                b.setVisible(false);
        }
    }

    viewport()->update();
    lineNumberArea_->update();
}

void CodeEditor::changeFontSize(int delta) {
    QFont f = font();
    int newSize = f.pointSize() + delta;
    // 范围限制 [8, 32]：太小看不清，太大占用过多编辑区
    newSize = qBound(8, newSize, 32);
    if (newSize == f.pointSize())
        return; // 无变化
    f.setPointSize(newSize);
    setFont(f);
    // 行号区域宽度依赖 fontMetrics，需重新计算
    updateLineNumberAreaWidth(0);
}

int CodeEditor::fontSize() const {
    return font().pointSize();
}

void CodeEditor::setDarkTheme(bool dark) {
    isDarkTheme_ = dark;

    // 第四轮迭代：设置 QPalette 实现编辑器基底色 / 选中区域 / 光标颜色主题化
    // VS Code 规范：
    //   浅色：base #ffffff, text #1f1f1f, selection #add6ff (半透明)
    //   深色：base #1e1e1e, text #d4d4d4, selection #264f78 (半透明)
    QPalette pal = this->palette();
    if (dark) {
        pal.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
        pal.setColor(QPalette::AlternateBase, QColor(0x25, 0x25, 0x26));
        pal.setColor(QPalette::Text, QColor(0xd4, 0xd4, 0xd4));
        pal.setColor(QPalette::Highlight, QColor(0x26, 0x4f, 0x78));
        pal.setColor(QPalette::HighlightedText, QColor(0xfd, 0xf6, 0xe3));
        pal.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    } else {
        // R15-7: 浅色模式 Base 从 #ffffff 改为 Solarized base3 (#FDF6E3)，
        // 与行号区背景 (#EEE8D5 base2) 和 IDE 整体米黄主题统一，
        // 消除「编辑区白色 vs 行号区米黄」的割裂感。
        pal.setColor(QPalette::Base, QColor(0xFD, 0xF6, 0xE3));
        pal.setColor(QPalette::AlternateBase, QColor(0xee, 0xe8, 0xd5));
        pal.setColor(QPalette::Text, QColor(0x00, 0x2b, 0x36));
        pal.setColor(QPalette::Highlight, QColor(0x58, 0x6e, 0x75, 0x60));
        pal.setColor(QPalette::HighlightedText, QColor(0xfd, 0xf6, 0xe3));
        pal.setColor(QPalette::PlaceholderText, QColor(0x93, 0xa1, 0xa1));
    }
    setPalette(pal);
    // viewport 也需应用 palette（QPlainTextEdit 的实际绘制发生在 viewport）
    viewport()->setPalette(pal);

    highlightCurrentLine();    // 刷新当前行高亮配色
    lineNumberArea_->update(); // 刷新行号区域
    viewport()->update();      // 刷新编辑器视口
}

// ============================================================
// F13: 自动补全实现
// ============================================================

void CodeEditor::setCompletionWords(const QStringList& words) {
    if (completionModel_) {
        completionModel_->setStringList(words);
    }
}

QString CodeEditor::textUnderCursor() const {
    QTextCursor tc = textCursor();
    tc.select(QTextCursor::WordUnderCursor);
    return tc.selectedText();
}

void CodeEditor::insertCompletion(const QString& completion) {
    if (!completer_ || completion.isEmpty())
        return;

    // 计算需要替换的前缀长度
    QString prefix = completer_->completionPrefix();
    if (prefix.isEmpty())
        return;

    QTextCursor tc = textCursor();
    // 选中当前单词（前缀部分），替换为完整补全文本
    // P1-1 fix: insertText 后光标已位于插入文本末尾，无需额外右移
    tc.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, prefix.length());
    tc.insertText(completion);
    setTextCursor(tc);
}

void CodeEditor::triggerCompletion() {
    if (!completer_)
        return;

    QString prefix = textUnderCursor();
    // 仅在有有效前缀时显示补全（至少 1 个字符）
    if (prefix.length() < 1) {
        completer_->popup()->hide();
        return;
    }

    completer_->setCompletionPrefix(prefix);
    updateCompletionPopup();
}

void CodeEditor::updateCompletionPopup() {
    if (!completer_)
        return;

    // 如果没有匹配项，隐藏弹窗
    if (completer_->completionCount() == 0) {
        completer_->popup()->hide();
        return;
    }

    // 计算弹窗位置：光标所在行的下方
    QRect cr = cursorRect();
    // 弹窗宽度根据最长补全项调整
    int popupWidth =
        completer_->popup()->sizeHintForColumn(0) + completer_->popup()->verticalScrollBar()->sizeHint().width();
    cr.setWidth(popupWidth);
    // P2-1 fix: QCompleter::complete() 会自动在 cr 下方显示弹窗，无需手动 translate
    completer_->complete(cr);
}

void CodeEditor::keyPressEvent(QKeyEvent* event) {
    if (!completer_) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    // 补全弹窗打开时的导航处理
    if (completer_->popup()->isVisible()) {
        // 以下键由补全弹窗处理
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            event->ignore();
            return; // 让 completer 弹窗处理这些键
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            event->ignore();
            return; // 弹窗导航
        default:
            break;
        }
    }

    // ========================================================
    // 功能 13：代码模板 / Snippets 系统的键盘交互
    // ========================================================
    // Ctrl+T 打开模板列表对话框
    if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_T) {
        showSnippetListDialog();
        return;
    }

    // Tab 键：占位符导航 or 触发词展开 or 默认缩进
    if (event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier) {
        if (currentPlaceholderIdx_ >= 0) {
            // 已在占位符导航模式 → 跳到下一个占位符
            jumpToNextPlaceholder();
            return;
        }
        // 未在导航模式 → 检测触发词并尝试展开
        if (tryExpandSnippet()) {
            return; // 已展开，事件已消费
        }
        // H1: 无匹配触发词 → 多行缩进 or 插入 4 空格
        QTextCursor tc = textCursor();
        if (tc.hasSelection()) {
            indentSelection(tc, /*addIndent=*/true);
        } else {
            // 单行：插入 4 空格而非 \t（保持与 setTabStopDistance 一致）
            tc.insertText(QString(4, ' '));
        }
        return;
    }

    // Shift+Tab：占位符反向导航 or 默认反向缩进
    if (event->key() == Qt::Key_Backtab || (event->key() == Qt::Key_Tab && (event->modifiers() & Qt::ShiftModifier))) {
        if (currentPlaceholderIdx_ >= 0) {
            jumpToPrevPlaceholder();
            return;
        }
        // H1: 反向缩进
        QTextCursor tc = textCursor();
        if (tc.hasSelection()) {
            indentSelection(tc, /*addIndent=*/false);
        } else {
            unindentLine(tc);
        }
        return;
    }

    // Escape：退出占位符导航模式
    if (event->key() == Qt::Key_Escape && currentPlaceholderIdx_ >= 0) {
        clearSnippetState();
        return;
    }

    // P1-2 fix: Ctrl+Space 在中文 IME 下会被系统拦截切换输入法，增加 Ctrl+J 作为备选触发键
    if (event->modifiers() == Qt::ControlModifier && (event->key() == Qt::Key_Space || event->key() == Qt::Key_J)) {
        triggerCompletion();
        return;
    }

    // H4: Ctrl+/ 行注释切换（//）
    if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_Slash) {
        QTextCursor tc = textCursor();
        if (tc.hasSelection()) {
            toggleCommentSelection(tc);
        } else {
            // 当前行：选中整行后切换
            tc.select(QTextCursor::LineUnderCursor);
            toggleCommentSelection(tc);
        }
        return;
    }

    // H4: Ctrl+Shift+/ 块注释切换（/* */）
    if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) &&
        (event->key() == Qt::Key_Slash || event->key() == Qt::Key_Question)) {
        // 注意：Shift+/ 在某些键盘布局下产生 Key_Question
        QTextCursor tc = textCursor();
        toggleBlockComment(tc);
        return;
    }

    // M8: Ctrl+D 选中下一个相同单词（简化版：单光标，无多光标）
    if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_D) {
        QTextCursor tc = textCursor();
        if (tc.hasSelection()) {
            // 已有选择：查找下一个相同文本
            QString selectedText = tc.selectedText();
            QTextDocument::FindFlags flags;
            QTextCursor found = document()->find(selectedText, tc.position(), flags);
            if (!found.isNull()) {
                setTextCursor(found);
            }
        } else {
            // 无选择：选中当前单词
            tc.select(QTextCursor::WordUnderCursor);
            setTextCursor(tc);
        }
        return;
    }

    // M8: Ctrl+Shift+K 删除当前行
    if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && event->key() == Qt::Key_K) {
        QTextCursor tc = textCursor();
        tc.beginEditBlock();
        tc.select(QTextCursor::LineUnderCursor);
        if (tc.hasSelection()) {
            tc.removeSelectedText();
            // 同时删除行尾换行符（如果不是最后一行）
            tc.movePosition(QTextCursor::StartOfLine);
            if (tc.position() < document()->characterCount() - 1) {
                QTextCursor next = tc;
                next.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
                if (next.selectedText() == "\n") {
                    next.removeSelectedText();
                }
            }
        }
        tc.endEditBlock();
        return;
    }

    // H1: 回车自动缩进（无选择时，复制上一行缩进；上一行以 { 结尾则加一级）
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && event->modifiers() == Qt::NoModifier) {
        QTextCursor tc = textCursor();
        if (!tc.hasSelection()) {
            // 获取当前行完整文本
            QTextCursor lineCursor = tc;
            lineCursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
            QString lineText = lineCursor.selectedText();
            // 提取行首空白
            QString indent;
            for (QChar c : lineText) {
                if (c == ' ' || c == '\t')
                    indent += c;
                else
                    break;
            }
            // 光标前的部分：若以 { 结尾则加一级缩进
            int cursorCol = tc.position() - lineCursor.position();
            QString beforeCursor = lineText.left(cursorCol);
            if (beforeCursor.trimmed().endsWith('{')) {
                indent += QString(4, ' ');
            }
            // 插入换行 + 缩进
            tc.insertText('\n' + indent);
            setTextCursor(tc);
            return;
        }
    }

    // 先处理按键（插入字符等）
    QPlainTextEdit::keyPressEvent(event);

    // 自动触发补全：输入字母/下划线时自动弹出
    if (completer_->popup()->isVisible()) {
        // 弹窗已打开，更新前缀
        QString prefix = textUnderCursor();
        if (prefix.isEmpty()) {
            completer_->popup()->hide();
        } else {
            completer_->setCompletionPrefix(prefix);
            if (completer_->completionCount() == 0) {
                completer_->popup()->hide();
            } else {
                updateCompletionPopup();
            }
        }
    } else {
        // 输入字母或下划线时自动触发
        QChar lastChar = event->text().isEmpty() ? QChar() : event->text().back();
        if (lastChar.isLetter() || lastChar == '_') {
            QString prefix = textUnderCursor();
            if (prefix.length() >= 2) {
                completer_->setCompletionPrefix(prefix);
                if (completer_->completionCount() > 0) {
                    updateCompletionPopup();
                }
            }
        }
    }
}

void CodeEditor::focusInEvent(QFocusEvent* event) {
    if (completer_) {
        completer_->setWidget(this);
    }
    QPlainTextEdit::focusInEvent(event);
}

void CodeEditor::focusOutEvent(QFocusEvent* event) {
    // P2-4 fix: 失去焦点时隐藏补全弹窗，避免悬垂弹窗
    if (completer_ && completer_->popup()) {
        completer_->popup()->hide();
    }
    QPlainTextEdit::focusOutEvent(event);
}

void CodeEditor::contextMenuEvent(QContextMenuEvent* event) {
    // 获取 QPlainTextEdit 默认上下文菜单（含 Undo/Redo/Cut/Copy/Paste/Select All）
    // 返回的 QMenu* 由本函数负责释放（Qt 文档要求调用者 delete）
    QMenu* stdMenu = createStandardContextMenu();
    if (!stdMenu) {
        QPlainTextEdit::contextMenuEvent(event);
        return;
    }

    // —— 项目特化操作 ——
    stdMenu->addSeparator();

    QAction* toggleCommentAct = stdMenu->addAction(QString::fromUtf8("注释切换 (Ctrl+/)"));
    QAction* toggleBlockCommentAct = stdMenu->addAction(QString::fromUtf8("块注释 (Ctrl+Shift+/)"));
    QAction* formatAct = stdMenu->addAction(QString::fromUtf8("格式化代码"));
    QAction* gotoLineAct = stdMenu->addAction(QString::fromUtf8("跳转到行... (Ctrl+G)"));
    QAction* findAct = stdMenu->addAction(QString::fromUtf8("查找... (Ctrl+F)"));
    QAction* replaceAct = stdMenu->addAction(QString::fromUtf8("替换... (Ctrl+H)"));

    // —— 调试相关操作 ——
    // CodeEditor 始终持有 breakpoints_ 集合并暴露 setBreakpoints/getBreakpoints
    // 公共 API，断点切换能力始终可用，因此调试菜单无条件显示
    stdMenu->addSeparator();

    QAction* toggleBreakpointAct = stdMenu->addAction(QString::fromUtf8("添加/移除断点 (F9)"));
    QAction* editBreakpointCondAct = stdMenu->addAction(QString::fromUtf8("编辑断点条件..."));
    QAction* runToCursorAct = stdMenu->addAction(QString::fromUtf8("运行到当前行"));

    // 在事件位置弹出菜单（exec 模态阻塞，返回被点击的 QAction 或 nullptr）
    QAction* chosen = stdMenu->exec(event->globalPos());

    // 根据选中项发射信号交由上层（Ide）执行对应操作
    if (chosen) {
        if (chosen == toggleCommentAct) {
            emit contextActionRequested("toggleComment");
        } else if (chosen == toggleBlockCommentAct) {
            emit contextActionRequested("toggleBlockComment");
        } else if (chosen == formatAct) {
            emit contextActionRequested("format");
        } else if (chosen == gotoLineAct) {
            emit contextActionRequested("gotoLine");
        } else if (chosen == findAct) {
            emit contextActionRequested("find");
        } else if (chosen == replaceAct) {
            emit contextActionRequested("replace");
        } else if (chosen == toggleBreakpointAct) {
            emit contextActionRequested("toggleBreakpoint");
        } else if (chosen == editBreakpointCondAct) {
            emit contextActionRequested("editBreakpointCondition");
        } else if (chosen == runToCursorAct) {
            emit contextActionRequested("runToCursor");
        }
    }

    event->accept();
    delete stdMenu;
}

// ============================================================
// 功能 13：代码模板 / Snippets 系统实现
// ------------------------------------------------------------
// Tab 键触发流程：
//   1. 检测光标前的触发词（连续非空白字符，前方是空白或行首）
//   2. 删除触发词
//   3. 插入展开后的模板文本
//   4. 选中第一个占位符，进入导航模式
//   5. Tab 跳到下一个占位符，Shift+Tab 跳到上一个
//   6. Esc 或光标移出占位符 → 退出导航模式
// ============================================================

bool CodeEditor::tryExpandSnippet() {
    // 获取光标前全部文本
    QTextCursor tc = textCursor();
    const int cursorPos = tc.position();
    QTextCursor scan = tc;
    scan.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
    const QString textBefore = scan.selectedText();

    const CodeSnippet* snip = CodeSnippetEngine::matchTrigger(textBefore);
    if (!snip)
        return false;

    // 计算触发词在文档中的范围并删除
    const QString trigger = QString::fromStdString(snip->trigger);
    const int triggerStart = cursorPos - trigger.length();
    if (triggerStart < 0)
        return false; // 防御性：触发词长度超过文档长度

    QTextCursor del = textCursor();
    del.setPosition(triggerStart);
    del.setPosition(cursorPos, QTextCursor::KeepAnchor);
    del.removeSelectedText();

    // 展开模板并插入
    const auto expansion = CodeSnippetEngine::expand(*snip);
    const int insertPos = triggerStart; // 插入起点（触发词已被删除）

    QTextCursor ins = textCursor();
    ins.setPosition(insertPos);
    ins.insertText(expansion.text);

    // 构造占位符的绝对位置列表（在文档中的字符偏移）
    currentPlaceholders_.clear();
    currentPlaceholders_.reserve(expansion.placeholderRanges.size());
    for (const auto& [relStart, relEnd] : expansion.placeholderRanges) {
        currentPlaceholders_.push_back({insertPos + relStart, insertPos + relEnd});
    }

    if (currentPlaceholders_.empty()) {
        // 无占位符：光标置于展开文本末尾，不进入导航模式
        currentPlaceholderIdx_ = -1;
        QTextCursor end = textCursor();
        end.setPosition(insertPos + expansion.text.length());
        isSnippetNavigating_ = true;
        setTextCursor(end);
        isSnippetNavigating_ = false;
    } else {
        // 有占位符：选中第一个占位符，进入导航模式
        currentPlaceholderIdx_ = 0;
        selectCurrentPlaceholder();
    }
    return true;
}

void CodeEditor::selectCurrentPlaceholder() {
    if (currentPlaceholderIdx_ < 0 || currentPlaceholderIdx_ >= static_cast<int>(currentPlaceholders_.size())) {
        return;
    }
    const auto& [start, end] = currentPlaceholders_[currentPlaceholderIdx_];
    QTextCursor tc = textCursor();
    tc.setPosition(start);
    tc.setPosition(end, QTextCursor::KeepAnchor);
    // 标记程序化移动，避免 cursorPositionChanged 触发清理
    isSnippetNavigating_ = true;
    setTextCursor(tc);
    isSnippetNavigating_ = false;
}

void CodeEditor::jumpToNextPlaceholder() {
    if (currentPlaceholderIdx_ < 0)
        return;
    if (currentPlaceholderIdx_ < static_cast<int>(currentPlaceholders_.size()) - 1) {
        ++currentPlaceholderIdx_;
        selectCurrentPlaceholder();
    } else {
        // 已是最后一个占位符：退出导航模式，光标置于当前占位符末尾
        const auto& range = currentPlaceholders_[currentPlaceholderIdx_];
        QTextCursor tc = textCursor();
        tc.setPosition(range.second);
        isSnippetNavigating_ = true;
        setTextCursor(tc);
        isSnippetNavigating_ = false;
        clearSnippetState();
    }
}

void CodeEditor::jumpToPrevPlaceholder() {
    if (currentPlaceholderIdx_ < 0)
        return;
    if (currentPlaceholderIdx_ > 0) {
        --currentPlaceholderIdx_;
        selectCurrentPlaceholder();
    }
    // 已是第一个占位符：保持不动（不退出导航模式，与 VS Code 行为一致）
}

void CodeEditor::clearSnippetState() {
    currentPlaceholders_.clear();
    currentPlaceholderIdx_ = -1;
}

void CodeEditor::updatePlaceholderRanges(int position, int charsRemoved, int charsAdded) {
    if (currentPlaceholderIdx_ < 0 || currentPlaceholders_.empty())
        return;

    const int delta = charsAdded - charsRemoved;
    if (delta == 0)
        return; // 长度未变，无需调整

    const int editEnd = position + charsRemoved; // 被删除文本的结束位置

    // 遍历所有占位符范围，按编辑位置相对关系调整
    for (auto& [start, end] : currentPlaceholders_) {
        if (position >= end) {
            // 编辑完全在本占位符之后 → 不变
            continue;
        }
        if (editEnd <= start) {
            // 编辑完全在本占位符之前 → 整体平移 delta
            start += delta;
            end += delta;
            continue;
        }
        // 编辑与占位符范围重叠
        if (position >= start) {
            // 编辑起点在占位符内 → 调整 end（用户在当前占位符内输入/删除）
            end += delta;
        } else {
            // 编辑起点在占位符之前但延伸到占位符内 → 整体平移
            start += delta;
            end += delta;
        }
    }
}

void CodeEditor::showSnippetListDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(mlTr("代码模板"));
    QVBoxLayout* layout = new QVBoxLayout(&dlg);

    QLabel* hint = new QLabel(mlTr("选择要插入的模板（双击或选中后点击确定）："), &dlg);
    layout->addWidget(hint);

    QListWidget* list = new QListWidget(&dlg);
    const auto& all = CodeSnippetEngine::snippets();
    for (const auto& snip : all) {
        const QString item =
            QString("%1\t— %2").arg(QString::fromStdString(snip.trigger)).arg(QString::fromStdString(snip.description));
        list->addItem(item);
    }
    list->setCurrentRow(0);
    layout->addWidget(list);

    QDialogButtonBox* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(btns);

    // 双击直接确定
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);

    if (dlg.exec() != QDialog::Accepted)
        return;
    const int row = list->currentRow();
    if (row < 0 || row >= static_cast<int>(all.size()))
        return;

    const CodeSnippet& snip = all[row];
    const auto expansion = CodeSnippetEngine::expand(snip);

    // 在当前光标位置插入展开文本
    QTextCursor ins = textCursor();
    const int insertPos = ins.position();
    ins.insertText(expansion.text);

    // 构造占位符的绝对位置列表
    currentPlaceholders_.clear();
    currentPlaceholders_.reserve(expansion.placeholderRanges.size());
    for (const auto& [relStart, relEnd] : expansion.placeholderRanges) {
        currentPlaceholders_.push_back({insertPos + relStart, insertPos + relEnd});
    }

    if (currentPlaceholders_.empty()) {
        currentPlaceholderIdx_ = -1;
        QTextCursor end = textCursor();
        end.setPosition(insertPos + expansion.text.length());
        isSnippetNavigating_ = true;
        setTextCursor(end);
        isSnippetNavigating_ = false;
    } else {
        currentPlaceholderIdx_ = 0;
        selectCurrentPlaceholder();
    }
}

// ============================================================
// H1/H2/H4: 编辑器增强（缩进 / 括号匹配 / 注释切换）
// -------------------------------------------------------------

/// H1: 对选中范围每行行首插入或移除 4 空格
/// 修复 P1 Bug：用 blockNumber() 跟踪行号，避免 lineEnd 在文档修改后过时
void CodeEditor::indentSelection(QTextCursor& tc, bool addIndent) {
    int start = tc.selectionStart();
    int end = tc.selectionEnd();
    QTextCursor cur = tc;
    cur.setPosition(start);
    int startBlock = cur.blockNumber();
    cur.setPosition(end);
    int endBlock = cur.blockNumber();

    cur.setPosition(start);
    cur.beginEditBlock(); // 合并为单次 undo

    for (int block = startBlock; block <= endBlock; ++block) {
        cur.movePosition(QTextCursor::StartOfLine);

        if (addIndent) {
            cur.insertText(QString(4, ' '));
        } else {
            // 移除行首最多 4 空格 或 1 Tab
            QTextCursor scan = cur;
            scan.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 4);
            QString head = scan.selectedText();
            int removeCount = 0;
            if (!head.isEmpty() && head[0] == '\t') {
                removeCount = 1;
            } else {
                for (QChar c : head) {
                    if (c == ' ')
                        removeCount++;
                    else
                        break;
                    if (removeCount >= 4)
                        break;
                }
            }
            if (removeCount > 0) {
                cur.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, removeCount);
                cur.removeSelectedText();
            }
        }

        // 移到下一行（NextBlock 跳到下一块行首，文档末尾时返回 false）
        if (!cur.movePosition(QTextCursor::NextBlock))
            break;
    }
    cur.endEditBlock();

    // 重新选中从 startBlock 行首到 endBlock 行尾
    // startBlock 行首 = 原 start 位置（若第一行缩进/反缩进，位置已偏移，
    // 但 StartOfLine 会回到行首，所以用 blockNumber 重新定位更稳妥）
    QTextCursor sel = textCursor();
    sel.movePosition(QTextCursor::Start);
    sel.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, startBlock);
    int newStart = sel.position();
    sel.movePosition(QTextCursor::Start);
    sel.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, endBlock);
    sel.movePosition(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
    setTextCursor(sel);
}

/// H1: 移除光标所在行行首最多 4 空格（或 1 Tab）
void CodeEditor::unindentLine(QTextCursor& tc) {
    tc.beginEditBlock();
    tc.movePosition(QTextCursor::StartOfLine);
    QTextCursor scan = tc;
    scan.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 4);
    QString head = scan.selectedText();
    int removeCount = 0;
    if (!head.isEmpty() && head[0] == '\t') {
        removeCount = 1;
    } else {
        for (QChar c : head) {
            if (c == ' ')
                removeCount++;
            else
                break;
            if (removeCount >= 4)
                break;
        }
    }
    if (removeCount > 0) {
        tc.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, removeCount);
        tc.removeSelectedText();
    }
    tc.endEditBlock();
}

/// H4: 对选中范围切换 // 注释（行首有 // 则移除，否则插入）
/// 修复 P1 Bug：用 blockNumber() 跟踪行号，避免 lineEnd 在文档修改后过时
void CodeEditor::toggleCommentSelection(QTextCursor& tc) {
    int start = tc.selectionStart();
    int end = tc.selectionEnd();
    QTextCursor cur = tc;
    cur.setPosition(start);
    int startBlock = cur.blockNumber();
    cur.setPosition(end);
    int endBlock = cur.blockNumber();

    // 第一遍：检查所有行是否都已注释（不修改文档，安全）
    bool allCommented = true;
    {
        QTextCursor scan = cur;
        scan.setPosition(start);
        for (int block = startBlock; block <= endBlock; ++block) {
            scan.movePosition(QTextCursor::StartOfLine);
            QTextCursor lineScan = scan;
            lineScan.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
            if (lineScan.selectedText() != "//") {
                allCommented = false;
                break;
            }
            if (!scan.movePosition(QTextCursor::NextBlock))
                break;
        }
    }

    // 第二遍：添加或移除注释
    cur.setPosition(start);
    cur.beginEditBlock();
    for (int block = startBlock; block <= endBlock; ++block) {
        cur.movePosition(QTextCursor::StartOfLine);

        if (allCommented) {
            // 移除 //
            cur.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
            if (cur.selectedText() == "//") {
                cur.removeSelectedText();
            }
        } else {
            // 添加 //
            cur.insertText("//");
        }

        if (!cur.movePosition(QTextCursor::NextBlock))
            break;
    }
    cur.endEditBlock();

    // 重新选中从 startBlock 行首到 endBlock 行尾
    QTextCursor sel = textCursor();
    sel.movePosition(QTextCursor::Start);
    sel.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, startBlock);
    sel.movePosition(QTextCursor::Start);
    sel.movePosition(QTextCursor::NextBlock, QTextCursor::MoveAnchor, endBlock);
    sel.movePosition(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
    setTextCursor(sel);
}

/// H2 辅助：判断位置 pos 是否在字符串或注释内
/// 通过从文档开头扫描到 pos，统计字符串/注释状态实现
/// 支持：双引号字符串、// 行注释、/* */ 块注释
bool CodeEditor::isInsideStringOrComment(int pos) const {
    if (pos <= 0)
        return false;
    int charCount = static_cast<int>(document()->characterCount());
    // 性能优化：只扫描到 pos 位置
    int scanEnd = qMin(pos, charCount);

    bool inString = false;
    bool inLineComment = false;
    bool inBlockComment = false;

    for (int i = 0; i < scanEnd; ++i) {
        QChar c = document()->characterAt(i);
        QChar next = (i + 1 < charCount) ? document()->characterAt(i + 1) : QChar();

        if (inLineComment) {
            // 行注释遇到换行结束
            if (c == '\n')
                inLineComment = false;
            continue;
        }
        if (inBlockComment) {
            // 块注释遇到 */ 结束
            if (c == '*' && next == '/') {
                inBlockComment = false;
                ++i; // 跳过 /
            }
            continue;
        }
        if (inString) {
            // 字符串遇到非转义 " 结束
            if (c == '"')
                inString = false;
            // 简化：不处理转义（MiniLang 字符串转义 \" 较少见，且此判断用于括号匹配容错）
            continue;
        }

        // 不在任何上下文中，检测进入
        if (c == '/' && next == '/') {
            inLineComment = true;
            ++i;
        } else if (c == '/' && next == '*') {
            inBlockComment = true;
            ++i;
        } else if (c == '"') {
            inString = true;
        }
    }
    return inString || inLineComment || inBlockComment;
}

/// H2: 括号匹配高亮（光标停在 ([{ 时高亮对应 )]}）
/// 修复限制1：跳过字符串/注释内的括号
/// 修复限制3：大文件性能优化（限制扫描范围 + 跳过字符串/注释块）
void CodeEditor::highlightBracketMatch() {
    bracketSelections_.clear();

    // 仅在没有补全弹窗时处理
    if (completer_ && completer_->popup() && completer_->popup()->isVisible()) {
        highlightCurrentLine();
        return;
    }

    QTextCursor tc = textCursor();
    if (tc.hasSelection()) {
        highlightCurrentLine();
        return;
    }

    int pos = tc.position();
    if (pos <= 0 || pos >= document()->characterCount()) {
        highlightCurrentLine();
        return;
    }

    QChar leftChar = document()->characterAt(pos - 1);
    QChar rightChar = document()->characterAt(pos);

    QChar open, close;
    int curBracketPos = -1;
    bool forward = true;

    // 光标左侧是开括号 → 正向找闭括号
    if (leftChar == '(' || leftChar == '[' || leftChar == '{') {
        open = leftChar;
        close = (leftChar == '(') ? ')' : (leftChar == '[') ? ']' : '}';
        curBracketPos = pos - 1;
        forward = true;
    } else if (leftChar == ')' || leftChar == ']' || leftChar == '}') {
        close = leftChar;
        open = (leftChar == ')') ? '(' : (leftChar == ']') ? '[' : '{';
        curBracketPos = pos - 1;
        forward = false;
    } else if (rightChar == '(' || rightChar == '[' || rightChar == '{') {
        open = rightChar;
        close = (rightChar == '(') ? ')' : (rightChar == '[') ? ']' : '}';
        curBracketPos = pos;
        forward = true;
    } else if (rightChar == ')' || rightChar == ']' || rightChar == '}') {
        close = rightChar;
        open = (rightChar == ')') ? '(' : (rightChar == ']') ? '[' : '{';
        curBracketPos = pos;
        forward = false;
    } else {
        highlightCurrentLine();
        return;
    }

    // 限制1：当前括号在字符串/注释内 → 不高亮
    if (isInsideStringOrComment(curBracketPos)) {
        highlightCurrentLine();
        return;
    }

    // 搜索匹配的括号（跳过字符串/注释内的同名括号）
    // 限制3：大文件性能优化 — 限制扫描范围到 ±5000 字符
    int matchPos = -1;
    int depth = 0;
    int charCount = static_cast<int>(document()->characterCount());
    const int kMaxScanRange = 5000; // 性能阈值：单次扫描最多 5000 字符
    int scanStart = forward ? curBracketPos : curBracketPos;
    int scanEnd = forward ? qMin(charCount, curBracketPos + kMaxScanRange) : qMax(0, curBracketPos - kMaxScanRange);

    if (forward) {
        for (int i = scanStart; i < scanEnd; ++i) {
            QChar c = document()->characterAt(i);
            // 跳过字符串/注释内的字符
            if (i > curBracketPos && isInsideStringOrComment(i))
                continue;
            if (c == open)
                depth++;
            else if (c == close) {
                depth--;
                if (depth == 0) {
                    matchPos = i;
                    break;
                }
            }
        }
    } else {
        for (int i = scanStart; i >= scanEnd; --i) {
            QChar c = document()->characterAt(i);
            if (i < curBracketPos && isInsideStringOrComment(i))
                continue;
            if (c == close)
                depth++;
            else if (c == open) {
                depth--;
                if (depth == 0) {
                    matchPos = i;
                    break;
                }
            }
        }
    }

    if (matchPos < 0) {
        highlightCurrentLine();
        return;
    }

    // 构建 bracket selections（半透明黄色背景）
    QColor matchColor(0xb5, 0x89, 0x00, 120); // Solarized yellow

    QTextEdit::ExtraSelection curSel;
    curSel.cursor.setPosition(curBracketPos);
    curSel.cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
    curSel.format.setBackground(matchColor);
    bracketSelections_.append(curSel);

    QTextEdit::ExtraSelection matchSel;
    matchSel.cursor.setPosition(matchPos);
    matchSel.cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
    matchSel.format.setBackground(matchColor);
    bracketSelections_.append(matchSel);

    highlightCurrentLine();
}

/// H4: 对选中范围切换 /* */ 块注释
/// 选区首尾包裹 /* */ 或去除已有 /* */
void CodeEditor::toggleBlockComment(QTextCursor& tc) {
    if (!tc.hasSelection()) {
        // 无选择时，对当前行整行添加 /* */ 包裹
        tc.select(QTextCursor::LineUnderCursor);
        if (!tc.hasSelection())
            return;
    }

    int start = tc.selectionStart();
    int end = tc.selectionEnd();

    // 检查选区前 2 字符是否为 /* 且后 2 字符是否为 */
    QTextCursor checkStart = tc;
    checkStart.setPosition(start);
    checkStart.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
    bool hasBlockStart = (checkStart.selectedText() == "/*");

    QTextCursor checkEnd = tc;
    checkEnd.setPosition(end);
    if (checkEnd.position() >= 2) {
        checkEnd.setPosition(end - 2);
        checkEnd.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
    }
    bool hasBlockEnd = (checkEnd.selectedText() == "*/");

    tc.beginEditBlock();
    if (hasBlockStart && hasBlockEnd) {
        // 移除 /* 和 */
        QTextCursor rmEnd = tc;
        rmEnd.setPosition(end - 2);
        rmEnd.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
        rmEnd.removeSelectedText();

        QTextCursor rmStart = tc;
        rmStart.setPosition(start);
        rmStart.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 2);
        rmStart.removeSelectedText();
    } else {
        // 添加 /* 和 */
        QTextCursor addEnd = tc;
        addEnd.setPosition(end);
        addEnd.insertText("*/");

        QTextCursor addStart = tc;
        addStart.setPosition(start);
        addStart.insertText("/*");
    }
    tc.endEditBlock();
}
