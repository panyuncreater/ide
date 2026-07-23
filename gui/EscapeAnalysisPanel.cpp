// ============================================================
// EscapeAnalysisPanel.cpp — 逃逸分析可视化教学面板实现
// ------------------------------------------------------------
// 纯教学/模拟面板，不运行任何执行引擎。包含：
//   1. 逃逸分析原理静态文档（No/Arg/Global Escape 三态）
//      + 栈上分配 + 标量替换 + MiniLang 实现对照
//   2. 逃逸分析模拟器：输入代码，分析对象逃逸状态与分配策略
// ============================================================

#include "gui/EscapeAnalysisPanel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cctype>

// ============================================================
// EscapeAnalysisLibrary — 静态教学数据
// ============================================================

const std::vector<EscapeStateInfo>& EscapeAnalysisLibrary::stateInfos() {
    static const std::vector<EscapeStateInfo> kStates = {
        {"No Escape", "不逃逸", "对象仅在创建它的函数内使用，不作为返回值、不赋值给全局变量、不传递给其他函数",
         "栈上分配（假设优化）", "最快（无堆开销，函数返回时自动回收，无需引用计数或 GC）"},
        {"Arg Escape", "参数逃逸", "对象作为参数传递给其他函数，但不逃逸到全局作用域或作为返回值", "堆分配",
         "较慢（堆分配 + 引用计数开销，但生命周期可能较短）"},
        {"Global Escape", "全局逃逸", "对象赋值给全局变量，或作为函数返回值逃逸到调用方作用域", "堆分配",
         "最慢（堆分配 + 引用计数 + 长生命周期，可能触发 GC）"},
    };
    return kStates;
}

const std::vector<MiniLangEscapeMapping>& EscapeAnalysisLibrary::miniLangMappings() {
    static const std::vector<MiniLangEscapeMapping> kMappings = {
        {"逃逸分析（Escape Analysis）", "未实现",
         "MiniLang 当前无逃逸分析。所有对象统一堆分配，不分析对象是否逃逸。"
         "未来引入逃逸分析后，可识别不逃逸对象并优化分配策略。"},
        {"栈上分配（Stack Allocation）", "未实现",
         "MiniLang 所有对象（数组/字典/字符串/闭包/类实例）均分配在堆上，"
         "通过 RefCounted 基类管理生命周期。无栈上分配优化。"
         "未来可对不逃逸对象使用栈上分配，避免堆开销与引用计数。"},
        {"标量替换（Scalar Replacement）", "未实现",
         "MiniLang 未实现标量替换。未来可将不逃逸对象拆解为标量变量，"
         "完全消除对象分配，进一步优化性能。"},
        {"对象分配（Object Allocation）", "堆分配（RefCounted + COW）",
         "所有堆类型继承 RefCounted 基类，数组/字典使用 COW（Copy-On-Write）。"
         "Value 采用 NaN-boxing（8 字节），堆类型通过侵入式引用计数管理。"},
        {"JIT 支持（JIT Support）", "不支持（未来拓展）",
         "当前 JIT（R138-R156）仅做类型特化（INT/FLOAT），未做逃逸分析驱动的"
         "栈上分配或标量替换。这是未来 JIT 优化的重要方向。"},
        {"教学价值（Teaching Value）", "本面板",
         "展示 GC/JIT 经典优化技术（逃逸分析 + 栈上分配 + 标量替换），"
         "为 development.md「GC 替代引用计数」拓展项做前置教学。"},
    };
    return kMappings;
}

const std::vector<EscapeScenario>& EscapeAnalysisLibrary::scenarios() {
    static const std::vector<EscapeScenario> kScenarios = {
        {"场景 1：不逃逸（No Escape）",
         "fun add(a, b) { var r = [a, b]; return r[0] + r[1]; }",
         "数组 r 仅在函数 add 内部使用，返回的是 r[0]+r[1] 标量而非 r 本身，未逃逸",
         {{"r", "数组", "No Escape", "栈上分配（假设优化）",
           "数组 r 仅在函数 add 内使用，未作为返回值/全局赋值/参数传递，可栈上分配"}}},
        {"场景 2：全局逃逸（Global Escape）",
         "var g = []; fun append(x) { g.append(x); }",
         "数组 g 赋值给全局变量 g，逃逸到全局作用域",
         {{"g", "数组", "Global Escape", "堆分配",
           "数组 g 赋值给全局变量 g，逃逸到全局作用域，生命周期延续到程序结束"}}},
        {"场景 3：参数逃逸（Arg Escape）",
         "fun process(arr) { return arr.length; }",
         "参数 arr 由调用方传入，被调用函数 process 使用，发生参数逃逸",
         {{"arr", "数组（参数）", "Arg Escape", "堆分配",
           "参数 arr 由调用方传入，作为参数跨越方法边界，从调用方视角发生参数逃逸"}}},
        {"场景 4：返回值逃逸（Global Escape）",
         "fun makePair(a, b) { return [a, b]; }",
         "数组 [a,b] 作为函数返回值，逃逸到调用方作用域",
         {{"[a, b]", "数组", "Global Escape", "堆分配",
           "数组 [a,b] 作为 makePair 的返回值，逃逸到调用方作用域，必须堆分配"}}},
    };
    return kScenarios;
}

std::string EscapeAnalysisLibrary::stateToChinese(const std::string& state) {
    if (state == "No Escape")
        return "不逃逸";
    if (state == "Arg Escape")
        return "参数逃逸";
    if (state == "Global Escape")
        return "全局逃逸";
    return "未知";
}

// ============================================================
// EscapeAnalysisPanel 构造
// ============================================================

EscapeAnalysisPanel::EscapeAnalysisPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：子页切换按钮
    auto* pageBar = new QHBoxLayout;
    pageTheoryBtn_ = new QPushButton(tr("① 逃逸分析原理"));
    pageSimulatorBtn_ = new QPushButton(tr("② 逃逸分析模拟器"));
    pageTheoryBtn_->setCheckable(true);
    pageSimulatorBtn_->setCheckable(true);
    pageTheoryBtn_->setChecked(true);
    pageBar->addWidget(pageTheoryBtn_);
    pageBar->addWidget(pageSimulatorBtn_);
    pageBar->addStretch();
    outer->addLayout(pageBar);

    // 子页堆栈
    stack_ = new QStackedWidget;
    auto* theoryPage = new QWidget;
    auto* simulatorPage = new QWidget;
    buildTheoryPage(theoryPage);
    buildSimulatorPage(simulatorPage);
    stack_->addWidget(theoryPage);
    stack_->addWidget(simulatorPage);
    outer->addWidget(stack_, 1);

    // 底部：加载样例按钮
    auto* bottomBar = new QHBoxLayout;
    auto* loadSampleBtn = new QPushButton(tr("加载样例到主编辑器"));
    bottomBar->addStretch();
    bottomBar->addWidget(loadSampleBtn);
    outer->addLayout(bottomBar);

    // 信号连接
    connect(pageTheoryBtn_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            stack_->setCurrentIndex(0);
            pageSimulatorBtn_->setChecked(false);
        }
    });
    connect(pageSimulatorBtn_, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            stack_->setCurrentIndex(1);
            pageTheoryBtn_->setChecked(false);
        }
    });
    connect(loadSampleBtn, &QPushButton::clicked, this, &EscapeAnalysisPanel::onLoadSample);

    // 初始数据填充
    populateTheory();
    populatePresets();
}

// ============================================================
// 子页 1：逃逸分析原理
// ============================================================

void EscapeAnalysisPanel::buildTheoryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 理论概览
    theoryBrowser_ = new QTextBrowser;
    layout->addWidget(theoryBrowser_, 1);

    // 三种逃逸状态对比表
    layout->addWidget(new QLabel(tr("<b>三种逃逸状态对比</b>")));
    stateTable_ = new QTableWidget(0, 5);
    stateTable_->setHorizontalHeaderLabels({tr("状态"), tr("中文名"), tr("判定条件"), tr("分配策略"), tr("性能影响")});
    stateTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    stateTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    stateTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    stateTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    stateTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    stateTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(stateTable_, 1);

    // MiniLang 逃逸分析实现对照表
    layout->addWidget(new QLabel(tr("<b>MiniLang 逃逸分析实现对照</b>")));
    mappingTable_ = new QTableWidget(0, 3);
    mappingTable_->setHorizontalHeaderLabels({tr("概念"), tr("MiniLang 状态"), tr("说明")});
    mappingTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    mappingTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    mappingTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    mappingTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(mappingTable_, 1);
}

void EscapeAnalysisPanel::populateTheory() {
    // 理论概览 HTML
    QString html =
        QStringLiteral("<h2>逃逸分析（Escape Analysis）原理</h2>"
                       "<p>逃逸分析是 JIT/GC 经典优化技术，其核心思想是："
                       "<b>分析对象的作用域是否逃逸出创建它的方法</b>。"
                       "对于不逃逸的对象，编译器可以进行栈上分配或标量替换，避免堆分配开销。</p>"
                       "<h3>三种逃逸状态</h3>"
                       "<ul>"
                       "<li><b>No Escape（不逃逸）</b>：对象仅在创建它的方法内使用，不逃逸</li>"
                       "<li><b>Arg Escape（参数逃逸）</b>：对象作为参数传递给其他方法，但不逃逸到全局</li>"
                       "<li><b>Global Escape（全局逃逸）</b>：对象赋值给全局变量或作为返回值，逃逸到外部</li>"
                       "</ul>"
                       "<h3>栈上分配（Stack Allocation）</h3>"
                       "<p>不逃逸的对象可以分配在栈上，而非堆上。栈上分配的对象在方法返回时自动回收，"
                       "无需 GC 或引用计数开销。这显著减少了堆压力和 GC 频率。</p>"
                       "<h3>标量替换（Scalar Replacement）</h3>"
                       "<p>对于不逃逸的对象，编译器可以将其拆解为多个标量变量（字段），"
                       "完全消除对象分配。这是比栈上分配更激进的优化。</p>"
                       "<h3>教学价值</h3>"
                       "<p>逃逸分析是 GC 替代引用计数的关键技术之一。通过逃逸分析，"
                       "可以识别大部分不逃逸对象，使用栈上分配避免引用计数开销。"
                       "本面板为 development.md「GC 替代引用计数」拓展项做前置教学。</p>"
                       "<h3>MiniLang 现状</h3>"
                       "<p>MiniLang 当前<b>无逃逸分析</b>，所有对象（数组/字典/字符串/闭包/类实例）"
                       "均分配在堆上，通过 RefCounted 基类 + COW 管理生命周期。"
                       "未来引入逃逸分析后，可对不逃逸对象使用栈上分配，优化性能。</p>"
                       "<p style='color:#666;font-size:small;'>"
                       "提示：切换到「② 逃逸分析模拟器」子页，选择预设场景交互式体验逃逸分析。</p>");
    theoryBrowser_->setHtml(html);

    // 三种逃逸状态对比表
    const auto& states = EscapeAnalysisLibrary::stateInfos();
    stateTable_->setRowCount(static_cast<int>(states.size()));
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        const auto& s = states[i];
        QString displayName = QString::fromUtf8(s.name.c_str()) + QStringLiteral("（") +
                              QString::fromUtf8(s.chineseName.c_str()) + QStringLiteral("）");
        stateTable_->setItem(i, 0, new QTableWidgetItem(displayName));
        stateTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(s.chineseName.c_str())));
        stateTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(s.condition.c_str())));
        stateTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(s.allocStrategy.c_str())));
        stateTable_->setItem(i, 4, new QTableWidgetItem(QString::fromUtf8(s.performance.c_str())));
        stateTable_->item(i, 2)->setToolTip(QString::fromUtf8(s.condition.c_str()));
        stateTable_->item(i, 4)->setToolTip(QString::fromUtf8(s.performance.c_str()));
    }

    // MiniLang 逃逸分析实现对照表
    const auto& mappings = EscapeAnalysisLibrary::miniLangMappings();
    mappingTable_->setRowCount(static_cast<int>(mappings.size()));
    for (int i = 0; i < static_cast<int>(mappings.size()); ++i) {
        const auto& m = mappings[i];
        mappingTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(m.conceptName.c_str())));
        mappingTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(m.miniLangState.c_str())));
        mappingTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(m.explanation.c_str())));
        mappingTable_->item(i, 2)->setToolTip(QString::fromUtf8(m.explanation.c_str()));
    }
}

// ============================================================
// 子页 2：逃逸分析模拟器
// ============================================================

void EscapeAnalysisPanel::buildSimulatorPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部：预设场景 + 代码输入 + 分析按钮
    auto* inputBar = new QHBoxLayout;
    inputBar->addWidget(new QLabel(tr("预设场景：")));
    presetCombo_ = new QComboBox;
    presetCombo_->setMinimumWidth(200);
    inputBar->addWidget(presetCombo_);
    inputBar->addWidget(new QLabel(tr("代码：")));
    codeEdit_ = new QLineEdit;
    codeEdit_->setPlaceholderText(tr("输入 MiniLang 代码或选择预设场景"));
    inputBar->addWidget(codeEdit_, 1);
    analyzeBtn_ = new QPushButton(tr("分析逃逸"));
    inputBar->addWidget(analyzeBtn_);
    layout->addLayout(inputBar);

    // 中间：垂直分割器（上方对象表 + 下方水平分割器）
    auto* vSplitter = new QSplitter(Qt::Vertical);

    // 上方：对象逃逸分析表
    objTable_ = new QTableWidget(0, 5);
    objTable_->setHorizontalHeaderLabels({tr("对象名"), tr("类型"), tr("逃逸状态"), tr("分配策略"), tr("原因")});
    objTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    objTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    objTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    objTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    objTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    objTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vSplitter->addWidget(objTable_);

    // 下方：水平分割器（逃逸分析详情 + 优化建议）
    auto* hSplitter = new QSplitter(Qt::Horizontal);
    detailBrowser_ = new QTextBrowser;
    detailBrowser_->setHtml(tr("<p style='color:#666;'>选择预设场景并点击「分析逃逸」查看逃逸路径分析</p>"));
    suggestionBrowser_ = new QTextBrowser;
    suggestionBrowser_->setHtml(tr("<p style='color:#666;'>选择预设场景并点击「分析逃逸」查看优化建议</p>"));
    hSplitter->addWidget(detailBrowser_);
    hSplitter->addWidget(suggestionBrowser_);
    vSplitter->addWidget(hSplitter);

    // 设置分割比例
    vSplitter->setStretchFactor(0, 2);
    vSplitter->setStretchFactor(1, 3);
    hSplitter->setStretchFactor(0, 1);
    hSplitter->setStretchFactor(1, 1);

    layout->addWidget(vSplitter, 1);

    // 信号连接
    connect(analyzeBtn_, &QPushButton::clicked, this, &EscapeAnalysisPanel::onAnalyze);
    connect(presetCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &EscapeAnalysisPanel::onSelectPreset);
}

void EscapeAnalysisPanel::populatePresets() {
    presetCombo_->clear();
    for (const auto& s : EscapeAnalysisLibrary::scenarios()) {
        presetCombo_->addItem(QString::fromUtf8(s.title.c_str()));
    }
}

// ============================================================
// 模拟器逻辑
// ============================================================

std::string EscapeAnalysisPanel::normalizeCode(const std::string& code) {
    // 折叠空白为单个空格并去除首尾空格（用于预设场景匹配）
    std::string result;
    bool lastSpace = false;
    for (char c : code) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!lastSpace) {
                result += ' ';
                lastSpace = true;
            }
        } else {
            result += c;
            lastSpace = false;
        }
    }
    size_t start = result.find_first_not_of(' ');
    if (start == std::string::npos)
        return "";
    size_t end = result.find_last_not_of(' ');
    return result.substr(start, end - start + 1);
}

void EscapeAnalysisPanel::analyzeCode(const std::string& code, std::vector<EscapeSimObject>& out) {
    out.clear();
    std::string normalized = normalizeCode(code);

    // 1. 精确匹配预设场景（规范化后比较）
    for (const auto& scenario : EscapeAnalysisLibrary::scenarios()) {
        if (normalized == normalizeCode(scenario.code)) {
            out = scenario.objects;
            return;
        }
    }

    // 2. 简单规则匹配（非真实逃逸分析）
    // 规则优先级：
    //   A. var <name> = [ ... ] 且 <name> 出现在 return → Global Escape（返回值逃逸）
    //   B. var <name> = [ ... ] 在全局作用域（fun 之前）→ Global Escape（全局逃逸）
    //   C. var <name> = [ ... ] 函数内局部且未逃逸 → No Escape（不逃逸）
    //   D. return [ ... ] → Global Escape（匿名数组返回值逃逸）

    // 查找 fun 关键字位置（用于判断全局/局部作用域，需要词边界）
    size_t funPos = std::string::npos;
    {
        size_t searchPos = 0;
        while ((searchPos = normalized.find("fun ", searchPos)) != std::string::npos) {
            if (searchPos == 0 || (!std::isalnum(static_cast<unsigned char>(normalized[searchPos - 1])) &&
                                   normalized[searchPos - 1] != '_')) {
                funPos = searchPos;
                break;
            }
            searchPos += 4;
        }
    }

    // 查找所有 var <name> = [ 或 var <name> = { 模式
    size_t pos = 0;
    while ((pos = normalized.find("var ", pos)) != std::string::npos) {
        // 检查 var 前面是否是词边界
        if (pos > 0 && (std::isalnum(static_cast<unsigned char>(normalized[pos - 1])) || normalized[pos - 1] == '_')) {
            pos += 4;
            continue;
        }

        size_t nameStart = pos + 4;
        size_t nameEnd = nameStart;
        while (nameEnd < normalized.size() &&
               (std::isalnum(static_cast<unsigned char>(normalized[nameEnd])) || normalized[nameEnd] == '_')) {
            ++nameEnd;
        }
        if (nameEnd == nameStart) {
            pos = nameStart;
            continue;
        }
        std::string name = normalized.substr(nameStart, nameEnd - nameStart);

        // 跳过空格找 =
        size_t eqPos = nameEnd;
        while (eqPos < normalized.size() && normalized[eqPos] == ' ')
            ++eqPos;
        if (eqPos >= normalized.size() || normalized[eqPos] != '=') {
            pos = nameEnd;
            continue;
        }

        // 跳过空格找 [ 或 {
        size_t bracketPos = eqPos + 1;
        while (bracketPos < normalized.size() && normalized[bracketPos] == ' ')
            ++bracketPos;
        if (bracketPos >= normalized.size()) {
            pos = eqPos + 1;
            continue;
        }

        bool isArray = (normalized[bracketPos] == '[');
        bool isDict = (normalized[bracketPos] == '{');
        if (!isArray && !isDict) {
            pos = eqPos + 1;
            continue;
        }

        EscapeSimObject obj;
        obj.name = name;
        obj.type = isArray ? "数组" : "字典";

        // 检查 return <name>（需要词边界，且后面不是 [ 或 .，避免误匹配 return r[0]）
        bool isReturned = false;
        {
            std::string returnPattern = "return " + name;
            size_t rPos = 0;
            while ((rPos = normalized.find(returnPattern, rPos)) != std::string::npos) {
                // 检查 return 前面是否是词边界
                if (rPos > 0 &&
                    (std::isalnum(static_cast<unsigned char>(normalized[rPos - 1])) || normalized[rPos - 1] == '_')) {
                    rPos += returnPattern.size();
                    continue;
                }
                // 检查 name 后面是否是词边界（非字母/数字/下划线/[/.
                size_t afterName = rPos + returnPattern.size();
                if (afterName >= normalized.size()) {
                    isReturned = true;
                    break;
                }
                char nextChar = normalized[afterName];
                if (!std::isalnum(static_cast<unsigned char>(nextChar)) && nextChar != '_' && nextChar != '[' &&
                    nextChar != '.') {
                    isReturned = true;
                    break;
                }
                rPos += returnPattern.size();
            }
        }

        // 判断全局/局部作用域
        bool isGlobal;
        if (funPos == std::string::npos) {
            isGlobal = true; // 没有 fun，全是全局
        } else {
            isGlobal = (pos < funPos);
        }

        if (isReturned) {
            obj.state = "Global Escape";
            obj.allocStrategy = "堆分配";
            obj.reason = "对象 " + name + " 作为函数返回值，逃逸到调用方作用域";
        } else if (isGlobal) {
            obj.state = "Global Escape";
            obj.allocStrategy = "堆分配";
            obj.reason = "对象 " + name + " 赋值给全局变量，逃逸到全局作用域";
        } else {
            obj.state = "No Escape";
            obj.allocStrategy = "栈上分配（假设优化）";
            obj.reason = "对象 " + name + " 仅在函数内使用，未逃逸";
        }
        out.push_back(obj);
        pos = bracketPos + 1;
    }

    // 查找 return [ 模式（匿名数组返回值逃逸）
    {
        size_t rPos = 0;
        while ((rPos = normalized.find("return", rPos)) != std::string::npos) {
            // 检查 return 前面是否是词边界
            if (rPos > 0 &&
                (std::isalnum(static_cast<unsigned char>(normalized[rPos - 1])) || normalized[rPos - 1] == '_')) {
                rPos += 6;
                continue;
            }
            // 跳过空格找 [
            size_t bracketPos = rPos + 6;
            while (bracketPos < normalized.size() && normalized[bracketPos] == ' ')
                ++bracketPos;
            if (bracketPos < normalized.size() && normalized[bracketPos] == '[') {
                EscapeSimObject obj;
                obj.name = "[匿名数组]";
                obj.type = "数组";
                obj.state = "Global Escape";
                obj.allocStrategy = "堆分配";
                obj.reason = "匿名数组作为函数返回值，逃逸到调用方作用域";
                out.push_back(obj);
                rPos = bracketPos + 1;
            } else {
                rPos += 6;
            }
        }
    }

    // 如果未识别到对象
    if (out.empty()) {
        EscapeSimObject obj;
        obj.name = "(未识别)";
        obj.type = "未知";
        obj.state = "No Escape";
        obj.allocStrategy = "栈上分配（假设优化）";
        obj.reason = "未识别到可分析的对象，请尝试预设场景或包含数组/字典字面量的代码";
        out.push_back(obj);
    }
}

void EscapeAnalysisPanel::renderAnalysis(const std::vector<EscapeSimObject>& objects) {
    // 对象逃逸分析表
    objTable_->setRowCount(static_cast<int>(objects.size()));
    for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
        const auto& o = objects[i];
        objTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(o.name.c_str())));
        objTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(o.type.c_str())));

        QString stateText = QString::fromUtf8(o.state.c_str()) + QStringLiteral("（") +
                            QString::fromUtf8(EscapeAnalysisLibrary::stateToChinese(o.state).c_str()) +
                            QStringLiteral("）");
        auto* stateItem = new QTableWidgetItem(stateText);
        if (o.state == "No Escape") {
            stateItem->setForeground(QColor("#2E7D32")); // 绿
        } else if (o.state == "Arg Escape") {
            stateItem->setForeground(QColor("#F57F17")); // 橙
        } else if (o.state == "Global Escape") {
            stateItem->setForeground(QColor("#C62828")); // 红
        }
        objTable_->setItem(i, 2, stateItem);

        objTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(o.allocStrategy.c_str())));
        objTable_->setItem(i, 4, new QTableWidgetItem(QString::fromUtf8(o.reason.c_str())));
        objTable_->item(i, 4)->setToolTip(QString::fromUtf8(o.reason.c_str()));
    }

    // 逃逸分析详情
    QString detailHtml = QStringLiteral("<h3>逃逸分析详情</h3>");
    for (const auto& o : objects) {
        QString color = (o.state == "No Escape")    ? QStringLiteral("#2E7D32")
                        : (o.state == "Arg Escape") ? QStringLiteral("#F57F17")
                                                    : QStringLiteral("#C62828");
        detailHtml += QStringLiteral("<p><b>%1</b>（%2）<br>"
                                     "状态：<span style='color:%3;font-weight:bold;'>%4（%5）</span><br>"
                                     "分配：%6<br>"
                                     "原因：%7</p>")
                          .arg(QString::fromUtf8(o.name.c_str()))
                          .arg(QString::fromUtf8(o.type.c_str()))
                          .arg(color)
                          .arg(QString::fromUtf8(o.state.c_str()))
                          .arg(QString::fromUtf8(EscapeAnalysisLibrary::stateToChinese(o.state).c_str()))
                          .arg(QString::fromUtf8(o.allocStrategy.c_str()))
                          .arg(QString::fromUtf8(o.reason.c_str()));
    }
    detailBrowser_->setHtml(detailHtml);

    // 优化建议
    QString suggestHtml = QStringLiteral("<h3>优化建议</h3>");
    bool hasNoEscape = false;
    bool hasEscape = false;
    for (const auto& o : objects) {
        if (o.state == "No Escape") {
            hasNoEscape = true;
            suggestHtml += QStringLiteral("<p style='color:#2E7D32;'><b>%1</b> 可栈上分配（Stack Allocation）：<br>"
                                          "该对象不逃逸，可分配在栈上，避免堆开销与引用计数。</p>")
                               .arg(QString::fromUtf8(o.name.c_str()));
            suggestHtml += QStringLiteral("<p style='color:#2E7D32;'><b>%1</b> 可标量替换（Scalar Replacement）：<br>"
                                          "该对象不逃逸，可拆解为标量变量，完全消除对象分配。</p>")
                               .arg(QString::fromUtf8(o.name.c_str()));
        } else {
            hasEscape = true;
            suggestHtml += QStringLiteral("<p style='color:#C62828;'><b>%1</b> 必须堆分配：<br>"
                                          "该对象%2，无法栈上分配。</p>")
                               .arg(QString::fromUtf8(o.name.c_str()))
                               .arg(QString::fromUtf8(EscapeAnalysisLibrary::stateToChinese(o.state).c_str()));
        }
    }
    if (!hasNoEscape && !hasEscape) {
        suggestHtml += QStringLiteral("<p>无优化建议。</p>");
    } else if (hasNoEscape && !hasEscape) {
        suggestHtml += QStringLiteral("<p style='color:#2E7D32;'><b>总结</b>：所有对象均不逃逸，"
                                      "理论上可全部栈上分配，性能最优。</p>");
    } else if (!hasNoEscape && hasEscape) {
        suggestHtml += QStringLiteral("<p style='color:#C62828;'><b>总结</b>：所有对象均逃逸，"
                                      "必须堆分配，无栈上分配优化空间。</p>");
    } else {
        suggestHtml += QStringLiteral("<p style='color:#F57F17;'><b>总结</b>：部分对象逃逸，"
                                      "部分不逃逸。不逃逸对象可栈上分配，逃逸对象必须堆分配。</p>");
    }
    suggestHtml += QStringLiteral("<hr><p style='color:#666;font-size:small;'>"
                                  "注意：MiniLang 当前未实现逃逸分析与栈上分配，"
                                  "以上为教学演示的假设优化。所有对象实际均堆分配（RefCounted + COW）。</p>");
    suggestionBrowser_->setHtml(suggestHtml);
}

// ============================================================
// 槽函数
// ============================================================

void EscapeAnalysisPanel::onAnalyze() {
    std::string code = codeEdit_->text().toStdString();
    if (code.empty()) {
        objTable_->setRowCount(0);
        detailBrowser_->setHtml(tr("<p style='color:red;'>请输入代码或选择预设场景</p>"));
        suggestionBrowser_->setHtml(tr("<p style='color:red;'>请输入代码或选择预设场景</p>"));
        return;
    }
    std::vector<EscapeSimObject> objects;
    analyzeCode(code, objects);
    renderAnalysis(objects);
}

void EscapeAnalysisPanel::onSelectPreset(int idx) {
    if (idx < 0) {
        return;
    }
    const auto& scenarios = EscapeAnalysisLibrary::scenarios();
    if (idx < static_cast<int>(scenarios.size())) {
        codeEdit_->setText(QString::fromUtf8(scenarios[idx].code.c_str()));
    }
}

void EscapeAnalysisPanel::onLoadSample() {
    // 加载一个展示逃逸分析相关概念的 MiniLang 样例到主编辑器
    QString sample = QStringLiteral("// 逃逸分析（Escape Analysis）样例\n"
                                    "// 展示对象逃逸/不逃逸的判定与栈上分配优化\n"
                                    "\n"
                                    "// 场景 1：不逃逸 - 数组 r 仅在函数内使用\n"
                                    "// 理论上可栈上分配（MiniLang 当前仍堆分配）\n"
                                    "fun add(a, b) {\n"
                                    "  var r = [a, b];\n"
                                    "  return r[0] + r[1];\n"
                                    "}\n"
                                    "\n"
                                    "// 场景 2：全局逃逸 - 数组 g 赋值给全局变量\n"
                                    "// 必须堆分配，生命周期延续到程序结束\n"
                                    "var g = [];\n"
                                    "fun append(x) {\n"
                                    "  g.append(x);\n"
                                    "}\n"
                                    "\n"
                                    "// 场景 3：返回值逃逸 - 数组作为返回值\n"
                                    "// 必须堆分配，逃逸到调用方作用域\n"
                                    "fun makePair(a, b) {\n"
                                    "  return [a, b];\n"
                                    "}\n"
                                    "\n"
                                    "print(add(1, 2));\n"
                                    "append(3);\n"
                                    "print(g);\n"
                                    "print(makePair(4, 5));\n");
    emit loadSampleRequested(sample);
}

void EscapeAnalysisPanel::onPageSwitch(int idx) {
    stack_->setCurrentIndex(idx);
}
