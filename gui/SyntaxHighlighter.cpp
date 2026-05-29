#include "gui/SyntaxHighlighter.h"
#include <QStringList>

// ============================================================
// SyntaxHighlighter 语法高亮器实现
// ============================================================

SyntaxHighlighter::SyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {
    initRules();
}

void SyntaxHighlighter::initRules() {
    // 关键字：蓝色粗体
    keywordFormat_.setForeground(QColor(0, 0, 180));
    keywordFormat_.setFontWeight(QFont::Bold);

    // 字符串：绿色
    stringFormat_.setForeground(QColor(0, 128, 0));

    // 数字：橙色
    numberFormat_.setForeground(QColor(200, 100, 0));

    // 注释：灰色
    commentFormat_.setForeground(QColor(128, 128, 128));
    commentFormat_.setFontItalic(true);

    // 运算符：紫色
    operatorFormat_.setForeground(QColor(128, 0, 128));

    // 布尔值：深蓝色
    boolFormat_.setForeground(QColor(0, 0, 180));
    boolFormat_.setFontWeight(QFont::Bold);

    // ---- 添加高亮规则 ----

    // 单行注释
    HighlightRule commentRule;
    commentRule.pattern = QRegularExpression("//[^\n]*");
    commentRule.format = commentFormat_;
    rules_.push_back(commentRule);

    // 字符串字面量
    HighlightRule stringRule;
    stringRule.pattern = QRegularExpression("\"(?:[^\"\\\\]|\\\\.)*\"");
    stringRule.format = stringFormat_;
    rules_.push_back(stringRule);

    // 数字字面量（整数和浮点数）
    HighlightRule numberRule;
    numberRule.pattern = QRegularExpression("\\b\\d+\\.\\d+\\b|\\b\\d+\\b");
    numberRule.format = numberFormat_;
    rules_.push_back(numberRule);

    // 布尔值
    HighlightRule trueRule;
    trueRule.pattern = QRegularExpression("\\btrue\\b");
    trueRule.format = boolFormat_;
    rules_.push_back(trueRule);

    HighlightRule falseRule;
    falseRule.pattern = QRegularExpression("\\bfalse\\b");
    falseRule.format = boolFormat_;
    rules_.push_back(falseRule);

    // null 关键字
    HighlightRule nullRule;
    nullRule.pattern = QRegularExpression("\\bnull\\b");
    nullRule.format = keywordFormat_;
    rules_.push_back(nullRule);

    // 关键字（含新增关键字）
    QStringList keywordPatterns = {
        "\\bvar\\b", "\\bfun\\b", "\\bfunction\\b", "\\bfunc\\b", "\\bif\\b", "\\belse\\b",
        "\\bwhile\\b", "\\bfor\\b", "\\breturn\\b", "\\bprint\\b",
        "\\band\\b", "\\bor\\b", "\\bnot\\b",
        "\\bint\\b", "\\bfloat\\b", "\\bbool\\b", "\\bstring\\b",
        "\\bclass\\b", "\\bextends\\b", "\\bdict\\b", "\\barray\\b"
    };
    for (const auto& pattern : keywordPatterns) {
        HighlightRule rule;
        rule.pattern = QRegularExpression(pattern);
        rule.format = keywordFormat_;
        rules_.push_back(rule);
    }

    // 运算符（含新增分隔符）
    HighlightRule opRule;
    opRule.pattern = QRegularExpression("[+\\-*/%]|==|!=|<=|>=|<|>|=|\\(|\\)|\\{|\\}|\\[|\\]|;|,|:|\\.");
    opRule.format = operatorFormat_;
    rules_.push_back(opRule);
}

void SyntaxHighlighter::highlightBlock(const QString& text) {
    // 先处理注释（注释优先级最高，注释内的关键字不应高亮）
    QRegularExpression commentRegex("//[^\n]*");
    QRegularExpressionMatch commentMatch = commentRegex.match(text);
    if (commentMatch.hasMatch()) {
        // 注释前的部分正常高亮
        QString beforeComment = text.left(commentMatch.capturedStart());
        for (const auto& rule : rules_) {
            if (rule.format == commentFormat_) continue;  // 跳过注释规则
            QRegularExpressionMatchIterator it = rule.pattern.globalMatch(beforeComment);
            while (it.hasNext()) {
                QRegularExpressionMatch match = it.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.format);
            }
        }
        // 注释部分用注释格式
        setFormat(commentMatch.capturedStart(), commentMatch.capturedLength(), commentFormat_);
        return;
    }

    // 无注释时，正常应用所有规则
    for (const auto& rule : rules_) {
        if (rule.format == commentFormat_) continue;  // 跳过注释规则（已处理）
        QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }
}
