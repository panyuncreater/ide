// ============================================================
// MemoryLayoutPanel.cpp — 内存布局可视化教学面板实现
// ------------------------------------------------------------
// 纯教学/模拟面板，不运行任何执行引擎。包含：
//   1. 内存布局原理静态文档（NaN-boxing + COW + upvalue 三节）
//   2. 交互式内存布局模拟器：输入值，展示对应的 NaN-boxing 位模式
//      + 引用计数 + 堆分配状态 + COW 状态，并可视化 8 字节位布局
// ============================================================

#include "gui/MemoryLayoutPanel.h"

#include "gui/TeachingSubPageBar.h" // UX-R2 fix: 统一子页切换组件
#include "gui/TeachingTheme.h"      // UX-R2 fix: 硬编码颜色迁移到语义色

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cstdint>
#include <cstdio>
#include <cstring>

// ============================================================
// 匿名命名空间 — 辅助函数
// ============================================================

namespace {

/// 将 64 位整数格式化为 0x%016llX 字符串
std::string formatHex64(uint64_t bits) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(bits));
    return buf;
}

/// 生成 8 字节位模式可视化 HTML
/// isPointer 为 true 时高 2 字节（tag）用红色，低 6 字节（payload）用蓝色；
/// isPointer 为 false 时全 8 字节均为 payload（蓝色）。
QString formatBitPatternHtml(const std::string& bitPatternHex, bool isPointer) {
    std::string hex = bitPatternHex;
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex = hex.substr(2);
    }
    while (hex.size() < 16) {
        hex = "0" + hex;
    }

    QString html = QStringLiteral(
        "<h3>内存布局可视化（8 字节 / 64 位）</h3>"
        "<table border='1' cellpadding='8' cellspacing='0' style='font-family:monospace;font-size:14px;'>");

    // 字节序号行
    html += QStringLiteral("<tr>");
    for (int i = 7; i >= 0; --i) {
        QString label = (i == 7)   ? QStringLiteral("byte 7 (MSB)")
                        : (i == 0) ? QStringLiteral("byte 0 (LSB)")
                                   : QStringLiteral("byte %1").arg(i);
        html +=
            QStringLiteral("<th style='background:%1;'>%2</th>").arg(TeachingTheme::surfaceHover().name()).arg(label);
    }
    html += QStringLiteral("</tr>");

    // 字节值行
    html += QStringLiteral("<tr>");
    for (int i = 0; i < 8; ++i) {
        std::string byteStr = hex.substr(i * 2, 2);
        QString color;
        if (isPointer && i < 2) {
            color = QStringLiteral("#FFB3BA"); // tag — 粉红
        } else {
            color = QStringLiteral("#BAE1FF"); // payload — 浅蓝
        }
        html += QStringLiteral("<td bgcolor='%1' style='font-weight:bold;text-align:center;'>%2</td>")
                    .arg(color)
                    .arg(QString::fromUtf8(byteStr.c_str()).toUpper());
    }
    html += QStringLiteral("</tr></table>");

    // 图例
    if (isPointer) {
        html += QStringLiteral("<p style='margin-top:8px;'>"
                               "<span style='background:#FFB3BA;padding:2px 8px;'>&nbsp;tag&nbsp;</span> "
                               "高 16 位类型标签 &nbsp;&nbsp;"
                               "<span style='background:#BAE1FF;padding:2px 8px;'>&nbsp;payload&nbsp;</span> "
                               "低 48 位数据（指针 / 整数 / 布尔值）"
                               "</p>");
    } else {
        html += QStringLiteral("<p style='margin-top:8px;'>"
                               "<span style='background:#BAE1FF;padding:2px 8px;'>&nbsp;payload&nbsp;</span> "
                               "全 64 位为 IEEE 754 double 编码（无 tag）"
                               "</p>");
    }

    return html;
}

} // namespace

// ============================================================
// MemoryLayoutLibrary — 静态教学数据
// ============================================================

const std::vector<NaNBoxingTypeInfo>& MemoryLayoutLibrary::typeInfos() {
    static const std::vector<NaNBoxingTypeInfo> kTypes = {
        {"double", "无（全 64 位 IEEE 754）", "0x4045000000000000 (42.0)",
         "原样存储。若为 NaN 则整个 64 位替换为 NAN_BOXED_FLOAT_MARKER（0x7FFC...）"},
        {"bool", "0x7FF9", "0x7FF9000000000001 (true) / 0x7FF9000000000000 (false)",
         "tag=0x7FF9，最低位 0/1 表示 false/true"},
        {"null", "0x7FFA", "0x7FFA000000000000", "tag=0x7FFA，无 payload，全 48 位为 0"},
        {"pointer", "0x7FFB", "0x7FFB00000040A230", "tag=0x7FFB，低 48 位为 RefCounted 堆对象指针"},
        {"array", "0x7FFB (pointer)", "0x7FFB00000040A230", "pointer tag 指向堆上的 ArrayObject，使用 COW 写时复制"},
        {"dict", "0x7FFB (pointer)", "0x7FFB00000040A2A0", "pointer tag 指向堆上的 DictObject，使用 COW 写时复制"},
        {"closure", "0x7FFB (pointer)", "0x7FFB00000040A3B0",
         "pointer tag 指向堆上的 ClosureObject，含 upvalue 捕获链"},
        {"string", "0x7FFB (pointer)", "0x7FFB00000040A4D0", "pointer tag 指向堆上的 StringObject，引用计数管理"},
    };
    return kTypes;
}

const std::vector<CowProcessInfo>& MemoryLayoutLibrary::cowProcesses() {
    static const std::vector<CowProcessInfo> kProcesses = {
        {"创建 arr1 = [1, 2, 3]", "1", "否", "新建数组，独占所有权，refcount=1"},
        {"赋值 arr2 = arr1", "2", "否", "浅拷贝指针，共享底层数据，refcount=2，无需复制"},
        {"读取 arr2[0]", "2", "否", "只读访问，无需 detach，共享继续"},
        {"修改 arr2[0] = 10", "1", "是",
         "写前 detach：refcount>1 时复制底层数据，arr2 独占副本（refcount=1），arr1 保持原数据（refcount=1）"},
        {"arr1 离开作用域", "0 → 释放", "否", "refcount 减至 0，触发析构，释放堆内存"},
    };
    return kProcesses;
}

const std::vector<UpvalueStateInfo>& MemoryLayoutLibrary::upvalueStates() {
    static const std::vector<UpvalueStateInfo> kStates = {
        {"open upvalue", "指向栈槽（栈地址 + 偏移）",
         "栈未弹出，闭包通过 upvalue 间接读取栈上的变量。多个闭包可共享同一 open upvalue。"},
        {"closed upvalue", "值已拷贝到 upvalue 对象",
         "栈已弹出（变量离开作用域），upvalue 将值独立保存在堆上，闭包仍可访问。"},
    };
    return kStates;
}

const std::vector<MemoryScenario>& MemoryLayoutLibrary::scenarios() {
    static const std::vector<MemoryScenario> kScenarios = {
        {"场景 1：整数 42",
         "42",
         "展示 double 编码的整数 42，8 字节全为 IEEE 754 payload",
         {{"类型", "double（整数按 double 编码）"},
          {"位模式", "0x4045000000000000"},
          {"引用计数", "N/A（非堆分配）"},
          {"是否堆分配", "否"},
          {"COW 状态", "N/A"},
          {"说明", "整数 42 按 IEEE 754 double 编码，8 字节全为 payload，无 tag"}},
         "0x4045000000000000",
         false},
        {"场景 2：浮点 3.14",
         "3.14",
         "展示 double 编码的浮点 3.14，8 字节全为 IEEE 754 payload",
         {{"类型", "double"},
          {"位模式", "0x40091EB851EB851F"},
          {"引用计数", "N/A（非堆分配）"},
          {"是否堆分配", "否"},
          {"COW 状态", "N/A"},
          {"说明", "浮点 3.14 按 IEEE 754 double 编码，8 字节全为 payload，无 tag"}},
         "0x40091EB851EB851F",
         false},
        {"场景 3：数组 COW",
         "array",
         "展示 array 的 pointer tag + 引用计数变化过程（COW 写时复制）",
         {{"类型", "array（堆分配，pointer tag）"},
          {"位模式", "0x7FFB00000040A230"},
          {"引用计数", "1（独占）→ 2（共享）→ 1（detach 后）"},
          {"是否堆分配", "是"},
          {"COW 状态", "写前 detach：refcount>1 时复制底层数据"},
          {"说明", "数组通过 pointer tag（0x7FFB）引用堆上的 ArrayObject，COW 共享时 refcount>1，写前 detach"}},
         "0x7FFB00000040A230",
         true},
        {"场景 4：闭包捕获",
         "closure",
         "展示 closure 的 pointer tag + upvalue 捕获链",
         {{"类型", "closure（堆分配，pointer tag）"},
          {"位模式", "0x7FFB00000040A3B0"},
          {"引用计数", "1"},
          {"是否堆分配", "是"},
          {"COW 状态", "N/A（闭包不参与 COW）"},
          {"说明", "闭包通过 pointer tag（0x7FFB）引用堆上的 ClosureObject，含 upvalue 捕获链（open/closed）"}},
         "0x7FFB00000040A3B0",
         true},
    };
    return kScenarios;
}

// ============================================================
// MemoryLayoutPanel 构造
// ============================================================

MemoryLayoutPanel::MemoryLayoutPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // UX-R2 fix: 子页切换统一为 TeachingSubPageBar 组件（替代手写 2 按钮互斥逻辑）
    subPageBar_ = new TeachingSubPageBar(this);
    outer->addLayout(subPageBar_->buttonBar());

    // 子页堆栈（由 TeachingSubPageBar 持有）
    stack_ = subPageBar_->stack();
    auto* theoryPage = new QWidget;
    auto* simulatorPage = new QWidget;
    buildTheoryPage(theoryPage);
    buildSimulatorPage(simulatorPage);
    subPageBar_->addPage(tr("① 内存布局原理"), theoryPage);
    subPageBar_->addPage(tr("② 交互式内存布局模拟器"), simulatorPage);
    outer->addWidget(stack_, 1);

    // 底部：加载样例按钮
    auto* bottomBar = new QHBoxLayout;
    auto* loadSampleBtn = new QPushButton(tr("加载样例到主编辑器"));
    bottomBar->addStretch();
    bottomBar->addWidget(loadSampleBtn);
    outer->addLayout(bottomBar);

    // 信号连接
    connect(loadSampleBtn, &QPushButton::clicked, this, &MemoryLayoutPanel::onLoadSample);

    // 初始数据填充
    populateTheory();
    populatePresets();
}

// ============================================================
// 子页 1：内存布局原理
// ============================================================

void MemoryLayoutPanel::buildTheoryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 理论概览
    theoryBrowser_ = new QTextBrowser;
    layout->addWidget(theoryBrowser_, 2);

    // NaN-boxing 类型位模式表
    layout->addWidget(new QLabel(tr("<b>NaN-boxing 类型位模式</b>")));
    typeTable_ = new QTableWidget(0, 4);
    typeTable_->setHorizontalHeaderLabels({tr("类型名"), tr("高16位 tag"), tr("位模式示例"), tr("说明")});
    typeTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    typeTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    typeTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    typeTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    typeTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(typeTable_, 2);

    // COW 过程表
    layout->addWidget(new QLabel(tr("<b>COW（写时复制）过程</b>")));
    cowTable_ = new QTableWidget(0, 4);
    cowTable_->setHorizontalHeaderLabels({tr("操作"), tr("引用计数"), tr("是否 detach"), tr("说明")});
    cowTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    cowTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    cowTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    cowTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    cowTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(cowTable_, 2);

    // upvalue 状态表
    layout->addWidget(new QLabel(tr("<b>upvalue 捕获状态</b>")));
    upvalueTable_ = new QTableWidget(0, 3);
    upvalueTable_->setHorizontalHeaderLabels({tr("状态"), tr("内存结构"), tr("说明")});
    upvalueTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    upvalueTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    upvalueTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    upvalueTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(upvalueTable_, 1);
}

void MemoryLayoutPanel::populateTheory() {
    // 理论概览 HTML
    QString html =
        QStringLiteral(
            "<h2>MiniLang 内存布局原理</h2>"
            "<p>MiniLang 的 <b>Value</b> 类型采用 <b>NaN-boxing</b> 技术，将所有类型的值统一编码为 8 字节。"
            "堆类型（数组 / 字典 / 闭包 / 字符串）通过侵入式 <b>RefCounted</b> 基类管理生命周期，"
            "数组与字典使用 <b>Copy-On-Write (COW)</b> 优化，闭包通过 <b>upvalue</b> 捕获外层变量。</p>"

            "<h3>一、NaN-boxing（8 字节统一编码）</h3>"
            "<p>NaN-boxing 利用 IEEE 754 double 的 NaN（Not-a-Number）空间编码非浮点类型：</p>"
            "<ul>"
            "<li><b>double</b>：原样存储（全 64 位 IEEE 754）。若为 NaN，整个 64 位替换为 "
            "NAN_BOXED_FLOAT_MARKER（0x7FFC...）</li>"
            "<li><b>bool</b>：tag=0x7FF9，最低位 0/1 表示 false/true</li>"
            "<li><b>null</b>：tag=0x7FFA，无 payload</li>"
            "<li><b>pointer</b>：tag=0x7FFB，低 48 位为堆对象指针（array / dict / closure / string 均用此 tag）</li>"
            "</ul>"
            "<p>高 16 位为类型标签（tag），低 48 位为数据载荷（payload）。"
            "由于 IEEE 754 的 quiet NaN 高 16 位为 0x7FF8~0x7FFF，"
            "可安全复用此空间编码非浮点类型，实现 8 字节统一存储。</p>"

            "<h3>二、Copy-On-Write (COW)</h3>"
            "<p>数组（ArrayObject）和字典（DictObject）使用 COW 策略：</p>"
            "<ul>"
            "<li><b>共享</b>：赋值时浅拷贝指针，refcount 加 1，不复制底层数据</li>"
            "<li><b>写前 detach</b>：修改前检查 refcount，若 >1 则复制底层数据（detach），"
            "使修改方独占副本</li>"
            "<li><b>释放</b>：refcount 减至 0 时触发析构，释放堆内存</li>"
            "</ul>"
            "<p>COW 避免了不必要的深拷贝，同时保证语义正确性。"
            "MiniLang 的 COW 基于 RefCounted 引用计数，而非 GC。</p>"

            "<h3>三、upvalue 捕获链</h3>"
            "<p>闭包（ClosureObject）通过 upvalue 捕获外层作用域的变量：</p>"
            "<ul>"
            "<li><b>open upvalue</b>：指向栈槽（栈地址 + 偏移），栈未弹出。"
            "闭包通过 upvalue 间接读取栈上的变量。</li>"
            "<li><b>closed upvalue</b>：值已拷贝到 upvalue 对象，栈已弹出。"
            "upvalue 将值独立保存在堆上，闭包仍可访问。</li>"
            "</ul>"
            "<p>当变量离开作用域（栈弹出）时，open upvalue 转为 closed upvalue，"
            "确保闭包持有的引用仍然有效。多个闭包可共享同一 upvalue。</p>"

            "<h3>教学价值</h3>"
            "<p>本面板可视化 MiniLang 内存模型的核心概念，帮助理解：</p>"
            "<ul>"
            "<li>NaN-boxing 如何在 8 字节内统一编码所有类型</li>"
            "<li>COW 如何在语义正确性与性能之间取得平衡</li>"
            "<li>upvalue 如何实现闭包对外层变量的捕获与生命周期管理</li>"
            "</ul>"
            "<p style='color:%1;font-size:small;'>"
            "提示：切换到「② 交互式内存布局模拟器」子页，选择预设场景或输入自定义值体验。</p>")
            .arg(TeachingTheme::textHint().name());
    theoryBrowser_->setHtml(html);

    // NaN-boxing 类型位模式表
    const auto& types = MemoryLayoutLibrary::typeInfos();
    typeTable_->setRowCount(static_cast<int>(types.size()));
    for (int i = 0; i < static_cast<int>(types.size()); ++i) {
        const auto& t = types[i];
        typeTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(t.typeName.c_str())));
        typeTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(t.tag.c_str())));
        typeTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(t.bitPattern.c_str())));
        typeTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(t.description.c_str())));
        typeTable_->item(i, 3)->setToolTip(QString::fromUtf8(t.description.c_str()));
    }

    // COW 过程表
    const auto& procs = MemoryLayoutLibrary::cowProcesses();
    cowTable_->setRowCount(static_cast<int>(procs.size()));
    for (int i = 0; i < static_cast<int>(procs.size()); ++i) {
        const auto& p = procs[i];
        cowTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(p.operation.c_str())));
        cowTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(p.refcount.c_str())));
        cowTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(p.detach.c_str())));
        cowTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(p.description.c_str())));
        cowTable_->item(i, 3)->setToolTip(QString::fromUtf8(p.description.c_str()));
    }

    // upvalue 状态表
    const auto& states = MemoryLayoutLibrary::upvalueStates();
    upvalueTable_->setRowCount(static_cast<int>(states.size()));
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        const auto& u = states[i];
        upvalueTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(u.state.c_str())));
        upvalueTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(u.memoryStructure.c_str())));
        upvalueTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(u.description.c_str())));
        upvalueTable_->item(i, 2)->setToolTip(QString::fromUtf8(u.description.c_str()));
    }
}

// ============================================================
// 子页 2：交互式内存布局模拟器
// ============================================================

void MemoryLayoutPanel::buildSimulatorPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部：预设场景 + 值输入 + 分析按钮
    auto* inputBar = new QHBoxLayout;
    inputBar->addWidget(new QLabel(tr("预设场景：")));
    presetCombo_ = new QComboBox;
    presetCombo_->setMinimumWidth(200);
    inputBar->addWidget(presetCombo_);
    inputBar->addWidget(new QLabel(tr("自定义值：")));
    valueEdit_ = new QLineEdit;
    valueEdit_->setPlaceholderText(tr("输入值（如 42, 3.14, array, closure, true, null）"));
    inputBar->addWidget(valueEdit_, 1);
    analyzeBtn_ = new QPushButton(tr("分析内存布局"));
    inputBar->addWidget(analyzeBtn_);
    layout->addLayout(inputBar);

    // 中间：垂直分割器（上方字段表 + 下方布局可视化）
    auto* vSplitter = new QSplitter(Qt::Vertical);

    // 上方：值类型分析表
    fieldTable_ = new QTableWidget(0, 2);
    fieldTable_->setHorizontalHeaderLabels({tr("字段"), tr("值")});
    fieldTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    fieldTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    fieldTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vSplitter->addWidget(fieldTable_);

    // 下方：内存布局可视化
    layoutBrowser_ = new QTextBrowser;
    layoutBrowser_->setHtml(
        tr("<p style='color:%1;'>选择预设场景或输入自定义值，点击「分析内存布局」查看 8 字节位模式可视化</p>")
            .arg(TeachingTheme::textHint().name()));
    vSplitter->addWidget(layoutBrowser_);

    // 设置分割比例
    vSplitter->setStretchFactor(0, 2);
    vSplitter->setStretchFactor(1, 3);

    layout->addWidget(vSplitter, 1);

    // 信号连接
    connect(analyzeBtn_, &QPushButton::clicked, this, &MemoryLayoutPanel::onAnalyze);
    connect(presetCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MemoryLayoutPanel::onSelectPreset);
}

void MemoryLayoutPanel::populatePresets() {
    presetCombo_->clear();
    for (const auto& s : MemoryLayoutLibrary::scenarios()) {
        presetCombo_->addItem(QString::fromUtf8(s.title.c_str()));
    }
}

// ============================================================
// 模拟器逻辑
// ============================================================

void MemoryLayoutPanel::analyzeValue(const std::string& input, std::vector<MemorySimField>& out,
                                     std::string& bitPattern, bool& isPointer) {
    out.clear();
    bitPattern.clear();
    isPointer = false;

    if (input.empty()) {
        out.push_back({"错误", "输入为空"});
        out.push_back({"提示", "请输入值或选择预设场景"});
        return;
    }

    // 1. 精确匹配预设场景
    for (const auto& scenario : MemoryLayoutLibrary::scenarios()) {
        if (input == scenario.input) {
            out = scenario.fields;
            bitPattern = scenario.bitPattern;
            isPointer = scenario.isPointer;
            return;
        }
    }

    // 2. 关键词匹配：bool / null
    if (input == "true" || input == "false") {
        bool val = (input == "true");
        uint64_t bits = 0x7FF9000000000000ULL | (val ? 1ULL : 0ULL);
        bitPattern = formatHex64(bits);
        isPointer = false;
        out.push_back({"类型", "bool"});
        out.push_back({"位模式", bitPattern});
        out.push_back({"引用计数", "N/A（非堆分配）"});
        out.push_back({"是否堆分配", "否"});
        out.push_back({"COW 状态", "N/A"});
        out.push_back({"说明", std::string("bool ") + input + "：tag=0x7FF9，最低位 " + (val ? "1" : "0")});
        return;
    }

    if (input == "null") {
        uint64_t bits = 0x7FFA000000000000ULL;
        bitPattern = formatHex64(bits);
        isPointer = false;
        out.push_back({"类型", "null"});
        out.push_back({"位模式", bitPattern});
        out.push_back({"引用计数", "N/A（非堆分配）"});
        out.push_back({"是否堆分配", "否"});
        out.push_back({"COW 状态", "N/A"});
        out.push_back({"说明", "null：tag=0x7FFA，无 payload"});
        return;
    }

    // 3. 关键词匹配：array / closure
    if (input.find("array") != std::string::npos || input.find('[') != std::string::npos ||
        input.find("数组") != std::string::npos) {
        bitPattern = "0x7FFB00000040A230";
        isPointer = true;
        out.push_back({"类型", "array（堆分配，pointer tag）"});
        out.push_back({"位模式", bitPattern});
        out.push_back({"引用计数", "1（独占）→ 2（共享）→ 1（detach 后）"});
        out.push_back({"是否堆分配", "是"});
        out.push_back({"COW 状态", "写前 detach：refcount>1 时复制底层数据"});
        out.push_back({"说明", "数组通过 pointer tag（0x7FFB）引用堆上的 ArrayObject，COW 共享时 refcount>1"});
        return;
    }

    if (input.find("closure") != std::string::npos || input.find("fun") != std::string::npos ||
        input.find("闭包") != std::string::npos) {
        bitPattern = "0x7FFB00000040A3B0";
        isPointer = true;
        out.push_back({"类型", "closure（堆分配，pointer tag）"});
        out.push_back({"位模式", bitPattern});
        out.push_back({"引用计数", "1"});
        out.push_back({"是否堆分配", "是"});
        out.push_back({"COW 状态", "N/A（闭包不参与 COW）"});
        out.push_back({"说明", "闭包通过 pointer tag（0x7FFB）引用堆上的 ClosureObject，含 upvalue 捕获链"});
        return;
    }

    // 4. 尝试解析为数字（double 编码）
    try {
        size_t pos = 0;
        long long intVal = std::stoll(input, &pos);
        if (pos == input.size()) {
            double dval = static_cast<double>(intVal);
            uint64_t bits;
            std::memcpy(&bits, &dval, sizeof(bits));
            bitPattern = formatHex64(bits);
            isPointer = false;
            out.push_back({"类型", "double（整数按 double 编码）"});
            out.push_back({"位模式", bitPattern});
            out.push_back({"引用计数", "N/A（非堆分配）"});
            out.push_back({"是否堆分配", "否"});
            out.push_back({"COW 状态", "N/A"});
            out.push_back({"说明", "整数 " + input + " 按 IEEE 754 double 编码，8 字节全为 payload，无 tag"});
            return;
        }
    } catch (...) {
        // 不是整数，继续尝试浮点
    }

    try {
        size_t pos = 0;
        double dval = std::stod(input, &pos);
        if (pos == input.size()) {
            uint64_t bits;
            std::memcpy(&bits, &dval, sizeof(bits));
            bitPattern = formatHex64(bits);
            isPointer = false;
            out.push_back({"类型", "double"});
            out.push_back({"位模式", bitPattern});
            out.push_back({"引用计数", "N/A（非堆分配）"});
            out.push_back({"是否堆分配", "否"});
            out.push_back({"COW 状态", "N/A"});
            out.push_back({"说明", "浮点 " + input + " 按 IEEE 754 double 编码，8 字节全为 payload，无 tag"});
            return;
        }
    } catch (...) {
        // 不是数字
    }

    // 5. 无法识别
    out.push_back({"错误", "无法识别的输入"});
    out.push_back({"提示", "请输入数字（如 42, 3.14）、true/false/null、array、closure，或选择预设场景"});
}

void MemoryLayoutPanel::renderAnalysis(const std::vector<MemorySimField>& fields, const std::string& bitPattern,
                                       bool isPointer) {
    // 值类型分析表
    fieldTable_->setRowCount(static_cast<int>(fields.size()));
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        const auto& f = fields[i];
        fieldTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(f.field.c_str())));
        fieldTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(f.value.c_str())));
        fieldTable_->item(i, 1)->setToolTip(QString::fromUtf8(f.value.c_str()));
    }

    // 内存布局可视化
    if (bitPattern.empty()) {
        layoutBrowser_->setHtml(tr("<p style='color:red;'>无法生成位模式可视化，请检查输入</p>"));
        return;
    }

    QString html = formatBitPatternHtml(bitPattern, isPointer);
    html += QStringLiteral("<hr><p style='color:%1;font-size:small;'>"
                           "说明：byte 7 为最高位字节（MSB），byte 0 为最低位字节（LSB）。"
                           "NaN-boxing 利用 IEEE 754 quiet NaN 的高 16 位空间（0x7FF8~0x7FFF）编码非浮点类型。"
                           "</p>")
                .arg(TeachingTheme::textHint().name());
    layoutBrowser_->setHtml(html);
}

// ============================================================
// 槽函数
// ============================================================

void MemoryLayoutPanel::onAnalyze() {
    std::string input = valueEdit_->text().toStdString();
    if (input.empty()) {
        fieldTable_->setRowCount(0);
        layoutBrowser_->setHtml(tr("<p style='color:red;'>请输入值或选择预设场景</p>"));
        return;
    }
    std::vector<MemorySimField> fields;
    std::string bitPattern;
    bool isPointer = false;
    analyzeValue(input, fields, bitPattern, isPointer);
    renderAnalysis(fields, bitPattern, isPointer);
}

void MemoryLayoutPanel::onSelectPreset(int idx) {
    if (idx < 0) {
        return;
    }
    const auto& scenarios = MemoryLayoutLibrary::scenarios();
    if (idx < static_cast<int>(scenarios.size())) {
        valueEdit_->setText(QString::fromUtf8(scenarios[idx].input.c_str()));
    }
}

void MemoryLayoutPanel::onLoadSample() {
    // 加载一个展示内存布局相关概念的 MiniLang 样例到主编辑器
    QString sample = QStringLiteral("// 内存布局（Memory Layout）样例\n"
                                    "// 展示 NaN-boxing / COW / upvalue 的内存模型\n"
                                    "\n"
                                    "// 1. NaN-boxing：所有值统一 8 字节\n"
                                    "var n = 42;        // double 编码（0x4045000000000000）\n"
                                    "var f = 3.14;      // double 编码（0x40091EB851EB851F）\n"
                                    "var b = true;      // tag=0x7FF9（0x7FF9000000000001）\n"
                                    "var x = null;      // tag=0x7FFA（0x7FFA000000000000）\n"
                                    "\n"
                                    "// 2. COW：数组/字典写时复制\n"
                                    "var arr1 = [1, 2, 3];   // refcount=1\n"
                                    "var arr2 = arr1;        // 共享，refcount=2\n"
                                    "arr2[0] = 10;           // detach，复制底层数据，各自 refcount=1\n"
                                    "print(arr1[0]);         // 1（原数据未变）\n"
                                    "print(arr2[0]);         // 10（副本已修改）\n"
                                    "\n"
                                    "// 3. upvalue：闭包捕获外层变量\n"
                                    "fun makeCounter() {\n"
                                    "  var count = 0;        // 栈上变量\n"
                                    "  fun increment() {\n"
                                    "    count = count + 1;  // 通过 upvalue 捕获 count\n"
                                    "    return count;\n"
                                    "  }\n"
                                    "  return increment;     // 闭包逃逸，count 变为 closed upvalue\n"
                                    "}\n"
                                    "\n"
                                    "var counter = makeCounter();\n"
                                    "print(counter());       // 1\n"
                                    "print(counter());       // 2\n");
    emit loadSampleRequested(sample);
}

void MemoryLayoutPanel::onPageSwitch(int idx) {
    stack_->setCurrentIndex(idx);
}
