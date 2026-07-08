/**
 * @file SyntaxHighlighter.h
 * @brief MiniLang 语法高亮器的类声明（QSyntaxHighlighter 子类）
 *
 * 供代码编辑器复用，统一 MiniLang 源码的语法着色表现。
 */
#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <QSet>
#include <QString>
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
/// 构造高亮器；parent 为关联文本文档。
    explicit SyntaxHighlighter(QTextDocument* parent = nullptr);

    /// F9: 切换深色/浅色主题
    void setDarkTheme(bool dark);

protected:
/// 对文本块应用高亮。
    void highlightBlock(const QString& text) override;

private:
    std::vector<HighlightRule> rules_;

    // PERF-22 fix: 关键字用 QSet 替代大正则 alternation，O(1) 查找替代 regex 回溯
    QSet<QString> keywordSet_;

    QTextCharFormat keywordFormat_;     // 关键字：蓝色粗体
    QTextCharFormat stringFormat_;      // 字符串：绿色
    QTextCharFormat numberFormat_;      // 数字：橙色
    QTextCharFormat commentFormat_;     // 注释：灰色
    QTextCharFormat operatorFormat_;    // 运算符：紫色

    bool isDarkTheme_ = false;          // F9: 当前是否深色主题

    /// 初始化高亮规则
    void initRules();
};
