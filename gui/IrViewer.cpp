#include "gui/IrViewer.h"
#include "gui/GuiTextUtils.h"  // monospaceFont()
#include "QFluent/ScrollBar.h"
#include <QVBoxLayout>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QColor>
#include <sstream>
#include <regex>

// ============================================================
// IrViewer 实现（第八轮：QTextBrowser + HTML 语法高亮）
// ============================================================

IrViewer::IrViewer(QWidget* parent)
    : QWidget(parent) {

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    browser_ = new QTextBrowser(this);
    browser_->setObjectName("irViewerBrowser");
    browser_->setFont(GuiTextUtils::monospaceFont(10));
    browser_->setOpenExternalLinks(false);
    browser_->setLineWrapMode(QTextBrowser::NoWrap);
    browser_->setVerticalScrollBar(new ScrollBar(browser_));
    browser_->setHorizontalScrollBar(new ScrollBar(browser_));
    // 第八轮：#f8f8f8 背景，8px 内边距
    browser_->setStyleSheet(
        "QTextBrowser { background: #f8f8f8; border: none; padding: 8px; }");
    mainLayout->addWidget(browser_, 1);
}

QString IrViewer::htmlEscape(const std::string& s) {
    QString q = QString::fromStdString(s);
    return q.toHtmlEscaped();
}

// 判断是否为 IR opcode（大写字母+下划线，至少 2 字符）
static bool isIROpcode(const std::string& tok) {
    if (tok.size() < 2) return false;
    for (char c : tok) {
        if (!((c >= 'A' && c <= 'Z') || c == '_')) return false;
    }
    return true;
}

// 判断是否为寄存器/局部变量（v 数字 或 t 数字）
static bool isRegister(const std::string& tok) {
    if (tok.size() < 2) return false;
    if (tok[0] != 'v' && tok[0] != 't') return false;
    for (size_t i = 1; i < tok.size(); ++i) {
        if (tok[i] < '0' || tok[i] > '9') return false;
    }
    return true;
}

// 判断是否为数字常量
static bool isNumericConst(const std::string& tok) {
    if (tok.empty()) return false;
    bool hasDigit = false;
    for (char c : tok) {
        if (c >= '0' && c <= '9') { hasDigit = true; continue; }
        if (c == '.' || c == '-') continue;
        return false;
    }
    return hasDigit;
}

// 判断是否为字符串字面量（含引号）
static bool isStringLiteral(const std::string& tok) {
    return !tok.empty() && tok.front() == '"' && tok.back() == '"';
}

QString IrViewer::formatIRLineHtml(const std::string& text) const {
    // 分离注释（; 开头到行尾）
    std::string code = text;
    std::string comment;
    size_t semiPos = code.find(';');
    if (semiPos != std::string::npos) {
        comment = code.substr(semiPos);
        code = code.substr(0, semiPos);
    }

    // 按空白拆分 token
    std::vector<std::string> tokens;
    std::istringstream iss(code);
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }

    QString html;
    bool firstToken = true;
    for (const auto& t : tokens) {
        if (!firstToken) html += "&nbsp;";
        firstToken = false;

        // 去除尾随逗号
        std::string core = t;
        std::string suffix;
        if (!core.empty() && core.back() == ',') {
            suffix = ",";
            core.pop_back();
        }

        QString esc = htmlEscape(core);
        if (isIROpcode(core)) {
            html += "<span style=\"color:#0078d4;font-weight:bold;\">" + esc + "</span>";
        } else if (isRegister(core)) {
            html += "<span style=\"color:#107c10;\">" + esc + "</span>";
        } else if (isStringLiteral(core)) {
            html += "<span style=\"color:#d83b01;\">" + esc + "</span>";
        } else if (isNumericConst(core)) {
            html += "<span style=\"color:#d83b01;\">" + esc + "</span>";
        } else if (core == "=" || core == "->" || core == "|" || core == "&") {
            html += "<span style=\"color:#6e6e6e;\">" + esc + "</span>";
        } else {
            // 标识符（函数名、标签等）默认色
            html += "<span style=\"color:#1e1e1e;\">" + esc + "</span>";
        }
        if (!suffix.empty()) {
            html += "<span style=\"color:#6e6e6e;\">" + QString::fromStdString(suffix).toHtmlEscaped() + "</span>";
        }
    }

    if (!comment.empty()) {
        if (!html.isEmpty()) html += "&nbsp;";
        html += "<span style=\"color:#6e6e6e;font-style:italic;\">" + htmlEscape(comment) + "</span>";
    }

    return html;
}

void IrViewer::setIR(const IRFunction* ir) {
    browser_->clear();
    rowToSourceLine_.clear();
    rowToInstrIndex_.clear();
    highlightedRow_ = -1;

    if (!ir) {
        browser_->setHtml("<div style='color:#6e6e6e;padding:8px;'>(未启用 IR 编译 — 在视图菜单勾选编译分析面板后查看)</div>");
        return;
    }

    if (ir->blocks.empty()) {
        browser_->setHtml("<div style='color:#6e6e6e;padding:8px;'>(空 IR — 无基本块)</div>");
        return;
    }

    // PERF: 大 IR 保护
    size_t totalInstrs = 0;
    for (const auto& block : ir->blocks) {
        totalInstrs += block.instructions.size();
    }
    static constexpr size_t MAX_IR_ROWS = 10000;

    QString html;
    html.reserve(64 * 1024);
    html += "<div style='font-family:Consolas,monospace;font-size:13px;line-height:1.6;'>";

    // ---- 函数元信息头 ----
    {
        std::ostringstream oss;
        oss << "IRFunction \"" << ir->name << "\"  "
            << "blocks=" << ir->blocks.size() << "  "
            << "constants=" << ir->constants.size() << "  "
            << "globals=" << ir->globalNames.size() << "  "
            << "vregs=" << ir->nextVReg;
        html += QString("<div style='color:#8764b8;font-weight:bold;padding:2px 0;'>%1</div>")
                    .arg(htmlEscape(oss.str()));
        rowToSourceLine_.push_back(0);
        rowToInstrIndex_.push_back(SIZE_MAX);
    }

    size_t instrIndex = 0;

    for (size_t bi = 0; bi < ir->blocks.size(); ++bi) {
        const auto& block = ir->blocks[bi];

        if (static_cast<size_t>(rowToSourceLine_.size()) > MAX_IR_ROWS) {
            html += QString("<div style='color:#808080;padding:2px 0;'>... (IR 超过 %1 行，已截断显示)</div>")
                        .arg(MAX_IR_ROWS);
            rowToSourceLine_.push_back(0);
            rowToInstrIndex_.push_back(SIZE_MAX);
            break;
        }

        // ---- 基本块头 ----
        {
            std::ostringstream oss;
            oss << "  BB" << bi << " (label=" << block.labelIndex << "):";
            html += QString("<div style='color:#8764b8;font-weight:bold;padding:2px 0;'>%1</div>")
                        .arg(htmlEscape(oss.str()));
            rowToSourceLine_.push_back(0);
            rowToInstrIndex_.push_back(SIZE_MAX);
        }

        // ---- 块内指令 ----
        for (const auto& instr : block.instructions) {
            std::string text = formatIRInstruction(instr);
            html += QString("<div style='padding:0 0 0 16px;'>%1</div>")
                        .arg(formatIRLineHtml(text));
            rowToSourceLine_.push_back(instr.line);
            rowToInstrIndex_.push_back(instrIndex);
            instrIndex++;
        }
    }

    html += "</div>";
    browser_->setHtml(html);
}

void IrViewer::clearIR() {
    browser_->clear();
    rowToSourceLine_.clear();
    rowToInstrIndex_.clear();
    highlightedRow_ = -1;
}

void IrViewer::highlightBySourceLine(int line) {
    // 清除旧高亮
    if (highlightedRow_ >= 0 && highlightedRow_ < static_cast<int>(rowToSourceLine_.size())) {
        QTextBlock block = browser_->document()->findBlockByNumber(highlightedRow_);
        if (block.isValid()) {
            QTextCursor c(block);
            QTextCharFormat fmt;
            fmt.setBackground(Qt::transparent);
            c.select(QTextCursor::LineUnderCursor);
            c.setCharFormat(fmt);
        }
    }
    highlightedRow_ = -1;

    if (line <= 0) return;

    for (int i = 0; i < static_cast<int>(rowToSourceLine_.size()); ++i) {
        if (rowToSourceLine_[i] == line) {
            QTextBlock block = browser_->document()->findBlockByNumber(i);
            if (block.isValid()) {
                QTextCursor c(block);
                QTextCharFormat fmt;
                fmt.setBackground(QColor("#FFF09B"));
                c.select(QTextCursor::LineUnderCursor);
                c.setCharFormat(fmt);
                browser_->setTextCursor(c);
                browser_->scrollToAnchor(QString::number(i));
            }
            highlightedRow_ = i;
            break;
        }
    }
}

void IrViewer::highlightByBytecodeOffset(const std::vector<std::pair<size_t, size_t>>& irToBytecodeOffset,
                                          size_t currentBytecodeOffset) {
    // 清除旧高亮
    if (highlightedRow_ >= 0 && highlightedRow_ < static_cast<int>(rowToSourceLine_.size())) {
        QTextBlock block = browser_->document()->findBlockByNumber(highlightedRow_);
        if (block.isValid()) {
            QTextCursor c(block);
            QTextCharFormat fmt;
            fmt.setBackground(Qt::transparent);
            c.select(QTextCursor::LineUnderCursor);
            c.setCharFormat(fmt);
        }
    }
    highlightedRow_ = -1;

    if (irToBytecodeOffset.empty()) return;

    size_t bestInstrIdx = SIZE_MAX;
    size_t lo = 0, hi = irToBytecodeOffset.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (irToBytecodeOffset[mid].second <= currentBytecodeOffset) {
            bestInstrIdx = irToBytecodeOffset[mid].first;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (bestInstrIdx == SIZE_MAX) return;

    for (int i = 0; i < static_cast<int>(rowToInstrIndex_.size()); ++i) {
        if (rowToInstrIndex_[i] == bestInstrIdx) {
            QTextBlock block = browser_->document()->findBlockByNumber(i);
            if (block.isValid()) {
                QTextCursor c(block);
                QTextCharFormat fmt;
                fmt.setBackground(QColor("#7CFC00"));
                c.select(QTextCursor::LineUnderCursor);
                c.setCharFormat(fmt);
                browser_->setTextCursor(c);
            }
            highlightedRow_ = i;
            break;
        }
    }
}

void IrViewer::clearHighlight() {
    if (highlightedRow_ >= 0 && highlightedRow_ < static_cast<int>(rowToSourceLine_.size())) {
        QTextBlock block = browser_->document()->findBlockByNumber(highlightedRow_);
        if (block.isValid()) {
            QTextCursor c(block);
            QTextCharFormat fmt;
            fmt.setBackground(Qt::transparent);
            c.select(QTextCursor::LineUnderCursor);
            c.setCharFormat(fmt);
        }
    }
    highlightedRow_ = -1;
}
