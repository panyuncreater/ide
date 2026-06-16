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
    QPainter painter(this);
    painter.fillRect(event->rect(), QColor(245, 245, 245));

    CodeEditor* codeEditor = qobject_cast<CodeEditor*>(editor_);
    if (!codeEditor) return;

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
            painter.setPen(QColor(120, 120, 120));
            QFont font = painter.font();
            font.setFamily("Consolas");
            font.setPointSize(10);
            painter.setFont(font);
            painter.drawText(0, top, width() - 8, bottom - top,
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(lineNumber));
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

    // 将 Y 坐标映射到行号
    QTextCursor cursor = codeEditor->cursorForPosition(QPoint(0, static_cast<int>(event->position().y())));
    int lineNumber = cursor.blockNumber() + 1;

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
    connect(this, &CodeEditor::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();

    // 设置字体
    QFont font("Consolas", 11);
    setFont(font);
    setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
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

    // 设置错误下划线
    QList<QTextEdit::ExtraSelection> selections;
    for (int line : errorLines_) {
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(block);
            sel.cursor.select(QTextCursor::LineUnderCursor);
            sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            sel.format.setUnderlineColor(Qt::red);
            selections.append(sel);
        }
    }
    // 保留当前行高亮
    highlightCurrentLine();
    setExtraSelections(selections);
}

void CodeEditor::clearErrorLines() {
    errorLines_.clear();
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

QSet<int> CodeEditor::getBreakpoints() const {
    return breakpoints_;
}

void CodeEditor::setBreakpoints(const QSet<int>& breakpoints) {
    breakpoints_ = breakpoints;
    lineNumberArea_->update();
}

std::string CodeEditor::getBreakpointCondition(int line) const {
    auto it = breakpointConditions_.find(line);
    if (it != breakpointConditions_.end()) {
        return it.value();
    }
    return "";
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

    // 当前执行行高亮（黄色背景）
    if (currentLine_ > 0) {
        QTextBlock block = document()->findBlockByNumber(currentLine_ - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(block);
            sel.cursor.select(QTextCursor::LineUnderCursor);
            sel.format.setBackground(QColor(255, 255, 180));
            sel.format.setProperty(QTextCharFormat::FullWidthSelection, true);
            selections.append(sel);
        }
    }

    // 光标行高亮（浅蓝色背景）
    QTextEdit::ExtraSelection cursorSel;
    cursorSel.cursor = textCursor();
    cursorSel.cursor.select(QTextCursor::LineUnderCursor);
    cursorSel.format.setBackground(QColor(235, 243, 255));
    cursorSel.format.setProperty(QTextCharFormat::FullWidthSelection, true);
    selections.append(cursorSel);

    // 错误下划线
    for (int line : errorLines_) {
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(block);
            sel.cursor.select(QTextCursor::LineUnderCursor);
            sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            sel.format.setUnderlineColor(Qt::red);
            selections.append(sel);
        }
    }

    setExtraSelections(selections);
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
