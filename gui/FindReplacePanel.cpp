#include "gui/FindReplacePanel.h"
#include "gui/CodeEditor.h"
#include "common/RuntimeLimits.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextCharFormat>
#include <QShortcut>
#include <QApplication>

// ============================================================
// FindReplacePanel 查找替换面板实现
// ============================================================

FindReplacePanel::FindReplacePanel(CodeEditor* editor, QWidget* parent)
    : QWidget(parent), editor_(editor) {
    // 构建面板 UI
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 2, 4, 2);
    mainLayout->setSpacing(2);

    // 查找行
    auto* findLayout = new QHBoxLayout();
    findLayout->setSpacing(4);
    findEdit_ = new QLineEdit(this);
    findEdit_->setPlaceholderText("查找...");
    findEdit_->setClearButtonEnabled(true);
    findLayout->addWidget(findEdit_, 1);

    findNextBtn_ = new QPushButton("下一个", this);
    findPrevBtn_ = new QPushButton("上一个", this);
    findLayout->addWidget(findNextBtn_);
    findLayout->addWidget(findPrevBtn_);

    closeBtn_ = new QPushButton("✕", this);
    closeBtn_->setFixedWidth(24);
    closeBtn_->setToolTip("关闭 (Esc)");
    findLayout->addWidget(closeBtn_);

    mainLayout->addLayout(findLayout);

    // 替换行
    auto* replaceLayout = new QHBoxLayout();
    replaceLayout->setSpacing(4);
    replaceEdit_ = new QLineEdit(this);
    replaceEdit_->setPlaceholderText("替换为...");
    replaceEdit_->setClearButtonEnabled(true);
    replaceLayout->addWidget(replaceEdit_, 1);

    replaceBtn_ = new QPushButton("替换", this);
    replaceAllBtn_ = new QPushButton("全部替换", this);
    replaceLayout->addWidget(replaceBtn_);
    replaceLayout->addWidget(replaceAllBtn_);

    mainLayout->addLayout(replaceLayout);

    // 选项行
    auto* optLayout = new QHBoxLayout();
    optLayout->setSpacing(8);
    caseSensitiveCheck_ = new QCheckBox("区分大小写", this);
    wholeWordCheck_ = new QCheckBox("全字匹配", this);
    statusLabel_ = new QLabel("", this);
    statusLabel_->setStyleSheet("color: gray;");

    optLayout->addWidget(caseSensitiveCheck_);
    optLayout->addWidget(wholeWordCheck_);
    optLayout->addStretch();
    optLayout->addWidget(statusLabel_);
    mainLayout->addLayout(optLayout);

    // 信号连接
    connect(findNextBtn_, &QPushButton::clicked, this, &FindReplacePanel::onFindNext);
    connect(findPrevBtn_, &QPushButton::clicked, this, &FindReplacePanel::onFindPrev);
    connect(replaceBtn_, &QPushButton::clicked, this, &FindReplacePanel::onReplace);
    connect(replaceAllBtn_, &QPushButton::clicked, this, &FindReplacePanel::onReplaceAll);
    connect(closeBtn_, &QPushButton::clicked, this, &FindReplacePanel::closePanel);
    connect(findEdit_, &QLineEdit::textChanged, this, &FindReplacePanel::onFindTextChanged);
    connect(findEdit_, &QLineEdit::returnPressed, this, &FindReplacePanel::onFindNext);
    connect(replaceEdit_, &QLineEdit::returnPressed, this, &FindReplacePanel::onReplace);

    // 默认隐藏替换行（仅查找模式）
    replaceEdit_->setVisible(false);
    replaceBtn_->setVisible(false);
    replaceAllBtn_->setVisible(false);

    setFixedHeight(90);
    setVisible(false);
}

void FindReplacePanel::showPanel(bool showReplace) {
    // P1-13 fix: showFind/showReplace 共用逻辑提取
    replaceVisible_ = showReplace;
    replaceEdit_->setVisible(showReplace);
    replaceBtn_->setVisible(showReplace);
    replaceAllBtn_->setVisible(showReplace);
    setFixedHeight(showReplace ? 90 : 60);
    setVisible(true);
    findEdit_->setFocus();
    findEdit_->selectAll();
    // 如果编辑器有选中文本，填充到查找框
    QString selected = editor_->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains('\n')) {
        findEdit_->setText(selected);
    }
    if (!findEdit_->text().isEmpty()) {
        highlightMatches(findEdit_->text());
    }
}

void FindReplacePanel::showFind() {
    showPanel(false);
}

void FindReplacePanel::showReplace() {
    showPanel(true);
}

void FindReplacePanel::closePanel() {
    setVisible(false);
    clearHighlights();
    statusLabel_->setText("");
    editor_->setFocus();
}

void FindReplacePanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closePanel();
        return;
    }
    // F3 查找下一个, Shift+F3 查找上一个
    if (event->key() == Qt::Key_F3) {
        if (event->modifiers() & Qt::ShiftModifier) {
            onFindPrev();
        } else {
            onFindNext();
        }
        return;
    }
    QWidget::keyPressEvent(event);
}

void FindReplacePanel::onFindTextChanged(const QString& text) {
    if (text.isEmpty()) {
        clearHighlights();
        statusLabel_->setText("");
        return;
    }
    highlightMatches(text);
    // 文本变化时从头查找
    QTextCursor cursor = editor_->textCursor();
    cursor.movePosition(QTextCursor::Start);
    editor_->setTextCursor(cursor);
    findText(true);
}

bool FindReplacePanel::findText(bool forward) {
    QString text = findEdit_->text();
    if (text.isEmpty()) return false;

    QTextDocument::FindFlags flags;
    if (caseSensitiveCheck_->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    if (wholeWordCheck_->isChecked()) flags |= QTextDocument::FindWholeWords;
    if (!forward) flags |= QTextDocument::FindBackward;

    QTextCursor cursor = editor_->textCursor();
    QTextCursor found = editor_->document()->find(text, cursor, flags);

    if (found.isNull()) {
        // 到达末尾/开头，回绕查找
        if (forward) {
            cursor.movePosition(QTextCursor::Start);
        } else {
            cursor.movePosition(QTextCursor::End);
        }
        found = editor_->document()->find(text, cursor, flags);
    }

    if (!found.isNull()) {
        editor_->setTextCursor(found);
        statusLabel_->setText("");
        return true;
    } else {
        statusLabel_->setText("未找到");
        statusLabel_->setStyleSheet("color: red;");
        return false;
    }
}

void FindReplacePanel::onFindNext() {
    findText(true);
}

void FindReplacePanel::onFindPrev() {
    findText(false);
}

void FindReplacePanel::onReplace() {
    QString findTextStr = findEdit_->text();
    QString replaceTextStr = replaceEdit_->text();
    if (findTextStr.isEmpty()) return;

    QTextCursor cursor = editor_->textCursor();
    // 如果当前有选中文本且匹配查找内容，替换它
    QString selected = cursor.selectedText();
    // P2 fix: 正确处理大小写敏感和全字匹配选项
    bool match = false;
    if (caseSensitiveCheck_->isChecked()) {
        match = (selected == findTextStr);
    } else {
        match = (selected.compare(findTextStr, Qt::CaseInsensitive) == 0);
    }
    // 全字匹配检查：选中文本前后字符不能是单词字符
    if (match && wholeWordCheck_ && wholeWordCheck_->isChecked()) {
        int selStart = cursor.selectionStart();
        int selEnd = cursor.selectionEnd();
        QTextDocument* doc = editor_->document();
        // 检查前一个字符
        if (selStart > 0) {
            QChar prevChar = doc->characterAt(selStart - 1);
            if (prevChar.isLetterOrNumber() || prevChar == '_') {
                match = false;
            }
        }
        // 检查后一个字符
        if (match && selEnd < doc->characterCount()) {
            QChar nextChar = doc->characterAt(selEnd);
            if (nextChar.isLetterOrNumber() || nextChar == '_') {
                match = false;
            }
        }
    }

    if (match) {
        cursor.insertText(replaceTextStr);
    }
    // 查找下一个
    findText(true);
}

void FindReplacePanel::onReplaceAll() {
    QString findTextStr = findEdit_->text();
    QString replaceTextStr = replaceEdit_->text();
    if (findTextStr.isEmpty()) return;

    QTextDocument::FindFlags flags;
    if (caseSensitiveCheck_->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    if (wholeWordCheck_->isChecked()) flags |= QTextDocument::FindWholeWords;

    QTextCursor cursor(editor_->document());
    cursor.beginEditBlock();

    int replaceCount = 0;
    cursor.movePosition(QTextCursor::Start);
    // P2 fix: 限制替换数量上限，防止超大文档阻塞 UI
    // D14 fix: 上限统一引用 RuntimeLimits::MAX_REPLACE_ALL
    while (replaceCount < RuntimeLimits::MAX_REPLACE_ALL) {
        QTextCursor found = editor_->document()->find(findTextStr, cursor, flags);
        if (found.isNull()) break;
        cursor = found;
        cursor.insertText(replaceTextStr);
        replaceCount++;
    }
    cursor.endEditBlock();

    statusLabel_->setText(QString("已替换 %1 处").arg(replaceCount));
    statusLabel_->setStyleSheet("color: green;");
    clearHighlights();
}

void FindReplacePanel::highlightMatches(const QString& text) {
    QList<QTextEdit::ExtraSelection> selections;

    QTextDocument::FindFlags flags;
    if (caseSensitiveCheck_->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    if (wholeWordCheck_->isChecked()) flags |= QTextDocument::FindWholeWords;

    QTextCursor cursor(editor_->document());
    cursor.movePosition(QTextCursor::Start);

    int count = 0;
    while (true) {
        QTextCursor found = editor_->document()->find(text, cursor, flags);
        if (found.isNull()) break;
        cursor = found;

        QTextEdit::ExtraSelection sel;
        sel.cursor = found;
        sel.format.setBackground(QColor(255, 255, 0, 100));  // 半透明黄色
        selections.append(sel);
        count++;
        // P2 fix: 限制高亮匹配数量，防止大文档卡顿
        // D14 fix: 上限统一引用 RuntimeLimits::MAX_FIND_HIGHLIGHTS
        if (count >= RuntimeLimits::MAX_FIND_HIGHLIGHTS) break;
    }

    editor_->setFindSelections(selections);

    if (count >= RuntimeLimits::MAX_FIND_HIGHLIGHTS) {
        statusLabel_->setText(QString("匹配 %1+ 处").arg(count));
        statusLabel_->setStyleSheet("color: gray;");
    } else if (count > 0) {
        statusLabel_->setText(QString("匹配 %1 处").arg(count));
        statusLabel_->setStyleSheet("color: gray;");
    } else {
        statusLabel_->setText("未找到");
        statusLabel_->setStyleSheet("color: red;");
    }
}

void FindReplacePanel::clearHighlights() {
    // P1 fix: 仅清除查找高亮，不要清除错误下划线和当前行高亮
    editor_->clearFindSelections();
}
