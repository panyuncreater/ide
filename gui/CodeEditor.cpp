#include "gui/CodeEditor.h"
#include <QPainter>
#include <QTextBlock>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QMenu>
#include <QInputDialog>
#include <QLineEdit>
#include <QCompleter>
#include <QStringListModel>
#include <QKeyEvent>
#include <QAbstractItemView>
#include <QScrollBar>
#include "gui/GuiTextUtils.h"  // Dedup-4A: monospaceFont()

// ============================================================
// LineNumberArea 行号区域
// ============================================================

LineNumberArea::LineNumberArea(QPlainTextEdit* editor, QWidget* parent)
    : QWidget(parent), editor_(editor) {
    setAutoFillBackground(true);
}

QSize LineNumberArea::sizeHint() const {
    return QSize(editor_->viewport()->width(), 0);
}

void LineNumberArea::paintEvent(QPaintEvent* event) {
    CodeEditor* codeEditor = qobject_cast<CodeEditor*>(editor_);
    if (!codeEditor) return;

    QPainter painter(this);
    // F9: 主题感知背景色
    QColor bgColor = codeEditor->isDarkTheme_ ? QColor(37, 37, 37) : QColor(245, 245, 245);
    painter.fillRect(event->rect(), bgColor);

    // 字体只需设置一次（移出循环避免每行重建）
    // P2 fix: 使用 static const 避免 paintEvent 每次重绘都构造 QFont
    // Dedup-4A: 通过 GuiTextUtils::monospaceFont 共享全局 QFont 缓存
    painter.setFont(GuiTextUtils::monospaceFont(10));

    QTextBlock block = codeEditor->firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(codeEditor->blockBoundingGeometry(block)
                                .translated(codeEditor->contentOffset()).top());
    int bottom = top + static_cast<int>(codeEditor->blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            int lineNumber = blockNumber + 1;

            // 绘制断点标记
            if (codeEditor->breakpoints_.contains(lineNumber)) {
                // 条件断点用橙色，无条件断点用红色
                bool hasCondition = codeEditor->breakpointConditions_.contains(lineNumber)
                                    && !codeEditor->breakpointConditions_[lineNumber].empty();
                painter.setBrush(hasCondition ? QColor(255, 165, 0) : Qt::red);
                painter.setPen(Qt::NoPen);
                int radius = 6;
                int cx = 14;
                int cy = (top + bottom) / 2;
                painter.drawEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
            }

            // 绘制行号
            // F9: 主题感知文字颜色
    QColor numColor = codeEditor->isDarkTheme_ ? QColor(115, 115, 115) : QColor(100, 100, 100);
            painter.setPen(numColor);
            painter.drawText(0, top, width() - 20, bottom - top,
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(lineNumber));

            // F8: 绘制折叠标记（右侧）
            if (codeEditor->isFoldable(block)) {
                bool folded = codeEditor->isFolded(blockNumber);
                int boxSize = 9;
                int bx = width() - 14;
                int by = (top + bottom) / 2 - boxSize / 2;
                // F9: 主题感知折叠标记颜色
                QColor foldBg = folded
                    ? (codeEditor->isDarkTheme_ ? QColor(100, 100, 220) : QColor(80, 80, 200))
                    : (codeEditor->isDarkTheme_ ? QColor(80, 80, 80) : QColor(200, 200, 200));
                QColor foldBorder = codeEditor->isDarkTheme_ ? QColor(140, 140, 140) : QColor(100, 100, 100);
                painter.setBrush(foldBg);
                painter.setPen(QPen(foldBorder, 1));
                painter.drawRect(bx, by, boxSize, boxSize);
                // 绘制 +/- 符号
                painter.setPen(folded ? Qt::white : (codeEditor->isDarkTheme_ ? Qt::white : Qt::black));
                int cx2 = bx + boxSize / 2;
                int cy2 = by + boxSize / 2;
                painter.drawLine(cx2 - 2, cy2, cx2 + 2, cy2);  // 横线
                if (!folded) {
                    painter.drawLine(cx2, cy2 - 2, cx2, cy2 + 2);  // 竖线（仅展开时）
                }
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
    if (!codeEditor) return;

    // F8: 检查是否点击了折叠区域（右侧 14px）
    int clickX = static_cast<int>(event->position().x());
    // BUG-CE-6 fix: 与 paintEvent 绘制区域一致。绘制位置 bx = width()-14，boxSize=9，
    // 覆盖 [width()-14, width()-5)。原判定 >= width()-16 比绘制区域宽，导致点击
    // 行号右侧空白也触发折叠。统一为 [width()-14, width()-4)。
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
        codeEditor->blockBoundingGeometry(lastBlock)
            .translated(codeEditor->contentOffset()).bottom());
    if (static_cast<qreal>(event->position().y()) >= lastBlockBottom) {
        return;  // 点击在最后一行下方的空白区域，不设置断点
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
            return;  // 跳过不可执行行
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
}

void LineNumberArea::contextMenuEvent(QContextMenuEvent* event) {
    CodeEditor* codeEditor = qobject_cast<CodeEditor*>(editor_);
    if (!codeEditor) return;

    // 将 Y 坐标映射到行号
    QTextCursor cursor = codeEditor->cursorForPosition(QPoint(0, static_cast<int>(event->pos().y())));
    int lineNumber = cursor.blockNumber() + 1;

    // BUG-CE-5 fix: 与 mousePressEvent 一致，检查点击是否在文档内容区域内。
    // cursorForPosition 对最后一行下方的空白区域返回文档末尾光标（最后一个块号），
    // 导致空白区域右键在最后一行误触发条件菜单。
    QTextBlock lastBlock = codeEditor->document()->lastBlock();
    qreal lastBlockBottom = static_cast<qreal>(
        codeEditor->blockBoundingGeometry(lastBlock)
            .translated(codeEditor->contentOffset()).bottom());
    if (static_cast<qreal>(event->pos().y()) >= lastBlockBottom) {
        return;  // 点击在最后一行下方的空白区域
    }

    if (!codeEditor->breakpoints_.contains(lineNumber)) return;

    QMenu menu(this);
    QString currentCond = QString::fromStdString(codeEditor->getBreakpointCondition(lineNumber));

    QAction* setCondAction = menu.addAction(
        currentCond.isEmpty()
            ? QString("设置条件... (行 %1)").arg(lineNumber)
            : QString("修改条件: \"%1\"").arg(currentCond));

    QAction* removeCondAction = nullptr;
    if (!currentCond.isEmpty()) {
        removeCondAction = menu.addAction("移除条件");
    }

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == setCondAction) {
        bool ok = false;
        QString cond = QInputDialog::getText(
            this, "设置断点条件",
            QString("行 %1 的条件表达式（为空则变为无条件断点）:").arg(lineNumber),
            QLineEdit::Normal, currentCond, &ok);
        if (ok) {
            QString condTrimmed = cond.trimmed();
            std::string condStr = condTrimmed.toStdString();
            if (condStr.empty()) {
                codeEditor->breakpointConditions_.remove(lineNumber);
            } else {
                codeEditor->breakpointConditions_[lineNumber] = condStr;
            }
            emit codeEditor->breakpointConditionRequested(lineNumber, condTrimmed);
            update();  // 刷新颜色（红/橙）
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

CodeEditor::CodeEditor(QWidget* parent)
    : QPlainTextEdit(parent) {
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
        if (lineHighlightTimer_) lineHighlightTimer_->start();
    });

    // BUG-CE-2/CE-3 fix: 监听文档内容变化，文档修改后调整断点/折叠块号偏移。
    // 原实现未监听 contentsChange，文档修改后断点和折叠停留在原行号，导致行号错位。
    contentsChangeConn_ = connect(document(), &QTextDocument::contentsChange,
        this, &CodeEditor::onContentsChange);
    lastBlockCount_ = document()->blockCount();

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
    connect(completer_, QOverload<const QString&>::of(&QCompleter::activated),
            this, &CodeEditor::insertCompletion);
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
    cachedErrorSelections_.clear();
    for (int line : errorLines_) {
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(block);
            sel.cursor.select(QTextCursor::LineUnderCursor);
            sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            sel.format.setUnderlineColor(Qt::red);
            cachedErrorSelections_.append(sel);
        }
    }
    highlightCurrentLine();
}

void CodeEditor::setErrorRanges(const std::vector<ErrorRange>& ranges) {
    errorLines_.clear();
    cachedErrorSelections_.clear();
    for (const auto& r : ranges) {
        errorLines_.insert(r.line);
        QTextBlock block = document()->findBlockByNumber(r.line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            int startPos = block.position();
            // EU-1 fix: 从列位置开始，精确标记错误 token
            int col = (r.column > 0) ? r.column - 1 : 0;
            int blockTextLen = block.length() - 1;  // block.length() includes the newline
            // BUG-CE-4 fix: 空行跳过，避免 len=1 选中换行符产生无效选区
            if (blockTextLen <= 0) continue;
            if (col >= blockTextLen) col = (blockTextLen > 0) ? blockTextLen - 1 : 0;
            int len = (r.length > 0) ? r.length : blockTextLen - col;
            if (len <= 0) len = blockTextLen - col;
            if (len <= 0) len = 1;
            if (col + len > blockTextLen) len = blockTextLen - col;
            sel.cursor = QTextCursor(document());
            sel.cursor.setPosition(startPos + col);
            sel.cursor.setPosition(startPos + col + len, QTextCursor::KeepAnchor);
            sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            sel.format.setUnderlineColor(Qt::red);
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
    if (line <= 0) return;
    QTextBlock block = document()->findBlockByNumber(line - 1);
    if (!block.isValid()) return;
    QTextCursor cursor(block);
    setTextCursor(cursor);
    centerCursor();
    setFocus();
}

QSet<int> CodeEditor::getBreakpoints() const {
    return breakpoints_;
}

void CodeEditor::setBreakpoints(const QSet<int>& breakpoints) {
    breakpoints_.clear();
    // DB-2 fix: 过滤空行和纯注释行，与 mousePressEvent 行为一致
    for (int line : breakpoints) {
        // G-P1-2 fix: 校验行号合法性，过滤 <= 0 的无效行号避免 findBlockByNumber 越界
        if (line <= 0) continue;
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QString text = block.text().trimmed();
            // BUG-CE-7 fix: 不仅识别行注释 //，也识别块注释行（userState >= 100
            // 或向后兼容编码 2 表示块注释跨行上下文）
            int s = block.userState();
            bool isCommentLine = text.startsWith("//") || s == 2 || s >= 100;
            if (text.isEmpty() || isCommentLine) continue;
        }
        breakpoints_.insert(line);
    }
    // 清理不再有效的断点条件
    for (auto it = breakpointConditions_.begin(); it != breakpointConditions_.end(); ) {
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
    Q_UNUSED(charsAdded);

    // BUG-CE-2/CE-3 fix: 文档内容变化后，断点（1-based 行号）、断点条件、
    // 折叠块（0-based blockNumber）以行号为 key，文档修改导致行号变化时需同步偏移。
    //
    // 实现说明：contentsChange 在变更已应用后发射，被删除文本不可访问，无法直接
    // 统计其换行符数量。改为通过文档块数变化计算 delta（lastBlockCount_ 在上次
    // 变更后/构造时更新），保证纯插入/纯删除/替换三类场景均正确。
    int newBlockCount = document()->blockCount();
    int delta = newBlockCount - lastBlockCount_;
    lastBlockCount_ = newBlockCount;
    if (delta == 0) return;

    // 计算 position 所在行（1-based），仅调整 >= startLine 的行号
    QTextBlock block = document()->findBlock(position);
    if (!block.isValid()) return;
    int startLine = block.blockNumber() + 1;  // 1-based，变更起始行

    // 调整断点行号（1-based）
    QSet<int> newBreakpoints;
    newBreakpoints.reserve(breakpoints_.size());
    for (int line : breakpoints_) {
        if (line < startLine) {
            newBreakpoints.insert(line);
        } else {
            int newLine = line + delta;
            if (newLine > 0) newBreakpoints.insert(newLine);
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
            if (newLine > 0) newConditions[newLine] = it.value();
        }
    }
    breakpointConditions_ = std::move(newConditions);

    // BUG-CE-3 fix: 调整 foldedBlocks_（0-based blockNumber）
    QSet<int> newFolded;
    newFolded.reserve(foldedBlocks_.size());
    int startBlockNumber = startLine - 1;  // 0-based
    for (int bn : foldedBlocks_) {
        if (bn < startBlockNumber) {
            newFolded.insert(bn);
        } else {
            int newBn = bn + delta;
            if (newBn >= 0) newFolded.insert(newBn);
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
    QColor cursorLineColor = isDarkTheme_ ? QColor(40, 44, 48) : QColor(232, 244, 255);
    QColor execLineColor = isDarkTheme_ ? QColor(86, 90, 46) : QColor(255, 255, 195);

    // BUG-CE-8 fix: 查找高亮先添加（底层），错误下划线次之（不冲突），
    // 光标行再次（蓝色覆盖查找高亮），执行行最后（黄色覆盖光标行和查找高亮）。
    // 原顺序将 findSelections_ 放在最后，导致查找高亮覆盖执行行高亮。
    // BUG 4.2 fix: 合并查找高亮，不覆盖编辑器自身 selections
    selections.append(findSelections_);

    // 错误下划线（使用预构建的缓存，避免每次光标移动都遍历）
    selections.append(cachedErrorSelections_);

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

    setExtraSelections(selections);
}

void CodeEditor::setFindSelections(const QList<QTextEdit::ExtraSelection>& selections) {
    findSelections_ = selections;
    highlightCurrentLine();  // 触发重绘，合并所有 selections
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
                i++;  // 跳过转义字符
                continue;
            }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            break;  // 行注释，忽略后续内容
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
        if (c == '{') depth++;
        else if (c == '}') depth--;
    }
    return depth;
}
}  // namespace

bool CodeEditor::isFoldable(const QTextBlock& block) const {
    if (!block.isValid()) return false;
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
    if (!startBlock.isValid()) return -1;

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
    if (depth <= 0) return -1;

    QTextBlock block = startBlock.next();
    while (block.isValid()) {
        depth += countBracesInLine(block.text(), inBlockComment);
        if (depth <= 0) return block.blockNumber();
        block = block.next();
    }
    return -1;
}

void CodeEditor::toggleFold(int blockNumber) {
    QTextBlock block = document()->findBlockByNumber(blockNumber);
    if (!block.isValid() || !isFoldable(block)) return;

    int endNum = foldEndBlock(block);
    if (endNum < 0 || endNum <= blockNumber) return;

    if (foldedBlocks_.contains(blockNumber)) {
        // 展开：显示范围内的块，但跳过仍折叠的嵌套块的子块
        foldedBlocks_.remove(blockNumber);
        int i = blockNumber + 1;
        while (i <= endNum) {
            QTextBlock b = document()->findBlockByNumber(i);
            if (!b.isValid()) break;
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
            if (b.isValid()) b.setVisible(false);
        }
    }

    viewport()->update();
    lineNumberArea_->update();
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
        pal.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
        pal.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    } else {
        pal.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
        pal.setColor(QPalette::AlternateBase, QColor(0xf8, 0xf8, 0xf8));
        pal.setColor(QPalette::Text, QColor(0x1f, 0x1f, 0x1f));
        pal.setColor(QPalette::Highlight, QColor(0xad, 0xd6, 0xff));
        pal.setColor(QPalette::HighlightedText, QColor(0x1f, 0x1f, 0x1f));
        pal.setColor(QPalette::PlaceholderText, QColor(0x9a, 0x9a, 0x9a));
    }
    setPalette(pal);
    // viewport 也需应用 palette（QPlainTextEdit 的实际绘制发生在 viewport）
    viewport()->setPalette(pal);

    highlightCurrentLine();  // 刷新当前行高亮配色
    lineNumberArea_->update();  // 刷新行号区域
    viewport()->update();  // 刷新编辑器视口
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
    if (!completer_ || completion.isEmpty()) return;

    // 计算需要替换的前缀长度
    QString prefix = completer_->completionPrefix();
    if (prefix.isEmpty()) return;

    QTextCursor tc = textCursor();
    // 选中当前单词（前缀部分），替换为完整补全文本
    // P1-1 fix: insertText 后光标已位于插入文本末尾，无需额外右移
    tc.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, prefix.length());
    tc.insertText(completion);
    setTextCursor(tc);
}

void CodeEditor::triggerCompletion() {
    if (!completer_) return;

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
    if (!completer_) return;

    // 如果没有匹配项，隐藏弹窗
    if (completer_->completionCount() == 0) {
        completer_->popup()->hide();
        return;
    }

    // 计算弹窗位置：光标所在行的下方
    QRect cr = cursorRect();
    // 弹窗宽度根据最长补全项调整
    int popupWidth = completer_->popup()->sizeHintForColumn(0)
                     + completer_->popup()->verticalScrollBar()->sizeHint().width();
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
            return;  // 让 completer 弹窗处理这些键
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            event->ignore();
            return;  // 弹窗导航
        default:
            break;
        }
    }

    // P1-2 fix: Ctrl+Space 在中文 IME 下会被系统拦截切换输入法，增加 Ctrl+J 作为备选触发键
    if (event->modifiers() == Qt::ControlModifier &&
        (event->key() == Qt::Key_Space || event->key() == Qt::Key_J)) {
        triggerCompletion();
        return;
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
