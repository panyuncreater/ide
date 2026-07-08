#include "gui/IrViewer.h"
#include "QFluent/ScrollBar.h"
#include "gui/GuiTextUtils.h" // monospaceFont()
#include "gui/TeachingTheme.h"
#include <QColor>
#include <QTextBlock>
#include <QTextBlockFormat> // BUG-IRV-3 fix: 使用 block 格式设置整行背景，避免覆盖 span char 格式
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>
#include <regex>
#include <sstream>

// ============================================================
// IrViewer 实现（第八轮：QTextBrowser + HTML 语法高亮）
// ============================================================

/// 构造 IR 查看器：初始化只读浏览器并应用主题背景。
IrViewer::IrViewer(QWidget* parent) : QWidget(parent) {

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
    // 第八轮：背景色跟随 TeachingTheme 主题（亮色 #ffffff / 暗色 #2d2d2d）
    browser_->setStyleSheet(QString("QTextBrowser { background: #FDF6E3; border: none; padding: 8px; }")
                                .arg(TeachingTheme::surface().name()));
    mainLayout->addWidget(browser_, 1);
}

/// 对字符串做 HTML 转义，避免 IR 文本破坏标签。
QString IrViewer::htmlEscape(const std::string& s) {
    QString q = QString::fromStdString(s);
    return q.toHtmlEscaped();
}

// 判断是否为 IR opcode（大写字母+下划线，至少 2 字符）
static bool isIROpcode(const std::string& tok) {
    if (tok.size() < 2)
        return false;
    for (char c : tok) {
        if (!((c >= 'A' && c <= 'Z') || c == '_'))
            return false;
    }
    return true;
}

// 判断是否为寄存器/局部变量（v 数字 或 t 数字）
static bool isRegister(const std::string& tok) {
    if (tok.size() < 2)
        return false;
    if (tok[0] != 'v' && tok[0] != 't')
        return false;
    for (size_t i = 1; i < tok.size(); ++i) {
        if (tok[i] < '0' || tok[i] > '9')
            return false;
    }
    return true;
}

// 判断是否为数字常量
// BUG-IRV-5 fix: 收紧规则——仅允许首字符为 '-'，避免 "1-2" 等非法字面量被误识别。
static bool isNumericConst(const std::string& tok) {
    if (tok.empty())
        return false;
    bool hasDigit = false;
    for (size_t i = 0; i < tok.size(); ++i) {
        char c = tok[i];
        if (c >= '0' && c <= '9') {
            hasDigit = true;
            continue;
        }
        // 仅允许首字符为 '-'（负号），其余位置的 '-' 视为非法
        if (i == 0 && c == '-')
            continue;
        if (c == '.')
            continue;
        return false;
    }
    return hasDigit;
}

// 判断是否为字符串字面量（含引号）
static bool isStringLiteral(const std::string& tok) {
    return !tok.empty() && tok.front() == '"' && tok.back() == '"';
}

/// 将单行 IR 指令按 token 类型着色为 HTML 片段。
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
        if (!firstToken)
            html += "&nbsp;";
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
            html += "<span style=\"color:#268BD2;font-weight:bold;\">" + esc + "</span>";
        } else if (isRegister(core)) {
            html += "<span style=\"color:#2AA198;\">" + esc + "</span>";
        } else if (isStringLiteral(core)) {
            html += "<span style=\"color:#CB4B16;\">" + esc + "</span>";
        } else if (isNumericConst(core)) {
            html += "<span style=\"color:#CB4B16;\">" + esc + "</span>";
        } else if (core == "=" || core == "->" || core == "|" || core == "&") {
            html += "<span style=\"color:#657B83;\">" + esc + "</span>";
        } else {
            // 标识符（函数名、标签等）默认色
            html += "<span style=\"color:#002B36;\">" + esc + "</span>";
        }
        if (!suffix.empty()) {
            html += "<span style=\"color:#657B83;\">" + QString::fromStdString(suffix).toHtmlEscaped() + "</span>";
        }
    }

    if (!comment.empty()) {
        if (!html.isEmpty())
            html += "&nbsp;";
        html += "<span style=\"color:#586E75;font-style:italic;\">" + htmlEscape(comment) + "</span>";
    }

    return html;
}

/// 设置并渲染 IR 内容到浏览器（含异常保护与超长截断）。
void IrViewer::setIR(const IRFunction* ir) {
    // BUG-IRV-1 fix: 用 try/catch 包裹整个函数体，避免 formatIRInstruction / HTML 构造
    // 抛出异常时导致面板进入未定义状态。捕获后显示错误占位文本。
    try {
        browser_->clear();
        rowToSourceLine_.clear();
        rowToInstrIndex_.clear();
        highlightedRow_ = -1;

        if (!ir) {
            browser_->setHtml(
                "<div style='color:#657B83;padding:8px;'>(未启用 IR 编译 — 在视图菜单勾选编译分析面板后查看)</div>");
            return;
        }

        if (ir->blocks.empty()) {
            browser_->setHtml("<div style='color:#657B83;padding:8px;'>(空 IR — 无基本块)</div>");
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
            html += QString("<div style='color:#6C71C4;font-weight:bold;padding:2px 0;'>%1</div>")
                        .arg(htmlEscape(oss.str()));
            rowToSourceLine_.push_back(0);
            rowToInstrIndex_.push_back(SIZE_MAX);
        }

        size_t instrIndex = 0;

        for (size_t bi = 0; bi < ir->blocks.size(); ++bi) {
            const auto& block = ir->blocks[bi];

            if (static_cast<size_t>(rowToSourceLine_.size()) > MAX_IR_ROWS) {
                html += QString("<div style='color:#657B83;padding:2px 0;'>... (IR 超过 %1 行，已截断显示)</div>")
                            .arg(MAX_IR_ROWS);
                rowToSourceLine_.push_back(0);
                rowToInstrIndex_.push_back(SIZE_MAX);
                break;
            }

            // ---- 基本块头 ----
            {
                std::ostringstream oss;
                oss << "  BB" << bi << " (label=" << block.labelIndex << "):";
                html += QString("<div style='color:#6C71C4;font-weight:bold;padding:2px 0;'>%1</div>")
                            .arg(htmlEscape(oss.str()));
                rowToSourceLine_.push_back(0);
                rowToInstrIndex_.push_back(SIZE_MAX);
            }

            // ---- 块内指令 ----
            for (const auto& instr : block.instructions) {
                std::string text = formatIRInstruction(instr);
                html += QString("<div style='padding:0 0 0 16px;'>%1</div>").arg(formatIRLineHtml(text));
                rowToSourceLine_.push_back(instr.line);
                rowToInstrIndex_.push_back(instrIndex);
                instrIndex++;
            }
        }

        html += "</div>";
        browser_->setHtml(html);
    } catch (const std::exception& e) {
        rowToSourceLine_.clear();
        rowToInstrIndex_.clear();
        highlightedRow_ = -1;
        browser_->setHtml(QString("<div style='color:#DC322F;padding:8px;'>(IR 渲染失败: %1)</div>")
                              .arg(QString::fromUtf8(e.what())));
    } catch (...) {
        rowToSourceLine_.clear();
        rowToInstrIndex_.clear();
        highlightedRow_ = -1;
        browser_->setHtml("<div style='color:#DC322F;padding:8px;'>(IR 渲染失败: 未知错误)</div>");
    }
}

/// 清空 IR 显示与行映射缓存。
void IrViewer::clearIR() {
    browser_->clear();
    rowToSourceLine_.clear();
    rowToInstrIndex_.clear();
    highlightedRow_ = -1;
}

/// 按源码行号高亮对应 IR 指令行。
void IrViewer::highlightBySourceLine(int line) {
    // BUG-IRV-3 fix: 改用 QTextBlockFormat 设置整行背景，避免 setCharFormat 覆盖
    // HTML span 标签产生的字符级语法高亮（颜色/粗体等）。
    // 清除旧高亮
    if (highlightedRow_ >= 0 && highlightedRow_ < static_cast<int>(rowToSourceLine_.size())) {
        QTextBlock block = browser_->document()->findBlockByNumber(highlightedRow_);
        if (block.isValid()) {
            QTextCursor c(block);
            QTextBlockFormat fmt;
            fmt.clearBackground();
            c.setBlockFormat(fmt);
        }
    }
    highlightedRow_ = -1;

    if (line <= 0)
        return;

    // BUG-IRV-2 fix: 此处仅高亮第一个匹配的指令行（注释已同步修正于头文件）
    for (int i = 0; i < static_cast<int>(rowToSourceLine_.size()); ++i) {
        if (rowToSourceLine_[i] == line) {
            QTextBlock block = browser_->document()->findBlockByNumber(i);
            if (block.isValid()) {
                QTextCursor c(block);
                QTextBlockFormat fmt;
                fmt.setBackground(QColor("#EEE8D5"));
                c.setBlockFormat(fmt);
                browser_->setTextCursor(c);
                browser_->scrollToAnchor(QString::number(i));
            }
            highlightedRow_ = i;
            break;
        }
    }
}

/// 按字节码偏移量高亮对应的 IR 指令行。
void IrViewer::highlightByBytecodeOffset(const std::vector<std::pair<size_t, size_t>>& irToBytecodeOffset,
                                         size_t currentBytecodeOffset) {
    // BUG-IRV-3 fix: 改用 QTextBlockFormat 设置整行背景
    // 清除旧高亮
    if (highlightedRow_ >= 0 && highlightedRow_ < static_cast<int>(rowToSourceLine_.size())) {
        QTextBlock block = browser_->document()->findBlockByNumber(highlightedRow_);
        if (block.isValid()) {
            QTextCursor c(block);
            QTextBlockFormat fmt;
            fmt.clearBackground();
            c.setBlockFormat(fmt);
        }
    }
    highlightedRow_ = -1;

    if (irToBytecodeOffset.empty())
        return;

    // BUG-IRV-4 fix: 二分查找假设 irToBytecodeOffset 按 .second 严格升序排列，
    // 但实际语义未强制保证。改为线性查找最大的 <= currentBytecodeOffset 项，
    // 同时假设大致有序（遇到大于值即停止），既稳健又不损失常见路径效率。
    size_t bestInstrIdx = SIZE_MAX;
    for (size_t i = 0; i < irToBytecodeOffset.size(); ++i) {
        if (irToBytecodeOffset[i].second <= currentBytecodeOffset) {
            bestInstrIdx = irToBytecodeOffset[i].first;
        } else {
            break; // 假设大致有序，遇到大于的即停止
        }
    }

    if (bestInstrIdx == SIZE_MAX)
        return;

    for (int i = 0; i < static_cast<int>(rowToInstrIndex_.size()); ++i) {
        if (rowToInstrIndex_[i] == bestInstrIdx) {
            QTextBlock block = browser_->document()->findBlockByNumber(i);
            if (block.isValid()) {
                QTextCursor c(block);
                QTextBlockFormat fmt;
                fmt.setBackground(QColor("#dcd0b0"));
                c.setBlockFormat(fmt);
                browser_->setTextCursor(c);
            }
            highlightedRow_ = i;
            break;
        }
    }
}

/// 清除当前 IR 行高亮。
void IrViewer::clearHighlight() {
    // BUG-IRV-3 fix: 改用 QTextBlockFormat 清除背景
    if (highlightedRow_ >= 0 && highlightedRow_ < static_cast<int>(rowToSourceLine_.size())) {
        QTextBlock block = browser_->document()->findBlockByNumber(highlightedRow_);
        if (block.isValid()) {
            QTextCursor c(block);
            QTextBlockFormat fmt;
            fmt.clearBackground();
            c.setBlockFormat(fmt);
        }
    }
    highlightedRow_ = -1;
}
