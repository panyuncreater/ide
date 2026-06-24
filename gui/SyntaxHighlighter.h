#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <vector>

// ============================================================
// SyntaxHighlighter 语法高亮器
// ============================================================

/// 语法高亮规则
struct HighlightRule {
    QRegularExpression pattern;
    QTextCharFormat format;
};

/// MiniLang 语法高亮器
class SyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit SyntaxHighlighter(QTextDocument* parent = nullptr);

    /// F9: 切换深色/浅色主题
    void setDarkTheme(bool dark);

protected:
    void highlightBlock(const QString& text) override;

private:
    std::vector<HighlightRule> rules_;

    QTextCharFormat keywordFormat_;     // 关键字：蓝色粗体
    QTextCharFormat stringFormat_;      // 字符串：绿色
    QTextCharFormat numberFormat_;      // 数字：橙色
    QTextCharFormat commentFormat_;     // 注释：灰色
    QTextCharFormat operatorFormat_;    // 运算符：紫色

    bool isDarkTheme_ = false;          // F9: 当前是否深色主题

    static QRegularExpression commentRegex_;  // 注释正则（全局共享，避免每次 highlightBlock 重建）
    static QRegularExpression stringRegex_;   // 字符串正则（多行字符串状态传递用）

    /// 初始化高亮规则
    void initRules();
};
