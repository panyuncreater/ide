// ============================================================
// RegisterAllocatorPanel.cpp — 寄存器分配可视化教学面板实现
// ------------------------------------------------------------
// 纯教学/模拟面板，不运行任何执行引擎。包含：
//   1. 寄存器分配原理静态文档 + 经典算法对比 +
//      MiniLang RegisterVM 寄存器模型
//   2. 寄存器分配模拟器：输入 MiniLang 代码，运行简化版
//      线性扫描算法，可视化变量-寄存器分配 + 溢出决策
// ============================================================

#include "gui/RegisterAllocatorPanel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <sstream>

// ============================================================
// RegisterAllocatorLibrary — 静态教学数据
// ============================================================

const std::vector<RegAllocAlgoInfo>& RegisterAllocatorLibrary::algoInfos() {
    static const std::vector<RegAllocAlgoInfo> kAlgos = {
        {"图着色（Graph Coloring）", "O(n^2) 启发式（最优为 NP 完备）", "分配质量高，接近最优；干涉图精确建模变量冲突",
         "编译速度慢，不适合 JIT；实现复杂，干涉图构建开销大"},
        {"线性扫描（Linear Scan）", "O(n log n)", "速度快，适合 JIT 编译；实现简单，工程实用性强",
         "分配质量次优；无回退，可能产生不必要的溢出"},
        {"SSA-based（基于 SSA 形式）", "O(n) 近线性", "SSA 形式下干涉图可简化为弦图，多项式时间最优分配",
         "需先构建 SSA 形式，实现复杂；phi 节点处理增加难度"},
        {"MiniLang 编译期分配", "O(n)", "实现极简，零运行时开销；编译期静态映射，无运行时分析",
         "无溢出支持，变量数受限于 32；无活跃区间分析，无寄存器复用优化"},
    };
    return kAlgos;
}

const std::vector<MiniLangRegModel>& RegisterAllocatorLibrary::miniLangModels() {
    static const std::vector<MiniLangRegModel> kModels = {
        {"虚拟寄存器数", "32（R0-R31）", "定长 std::array<Value, 32>，零堆分配。与 RegCallFrame::MAX_REGISTERS 对齐。"},
        {"寄存器编号", "R0-R31（零索引）", "编译期由 RegisterBytecodeBackend 确定寄存器号，运行时直接索引。"},
        {"寄存器分配", "编译期静态映射", "每个局部变量直接映射到一个固定寄存器号，无活跃区间分析。简单但无复用。"},
        {"溢出处理", "不支持（超限报错）", "若变量数超过 32，编译期直接报错。简化实现，强制单函数变量数 ≤ 32。"},
        {"参数传递", "R0..R(arity-1)", "调用者将参数写入被调用帧的参数寄存器。R0 为第一个参数，依此类推。"},
        {"返回值", "returnReg（调用者帧的指定寄存器）",
         "返回值直接写入调用者帧的 returnReg 寄存器，-1 表示丢弃返回值。"},
    };
    return kModels;
}

const std::vector<RegAllocScenario>& RegisterAllocatorLibrary::scenarios() {
    // 使用 lambda 初始化静态变量，便于生成 40 变量场景
    static const std::vector<RegAllocScenario> kScenarios = [] {
        std::vector<RegAllocScenario> v;

        // 场景 1：少量变量（3 个，不溢出）
        v.push_back({
            "少量变量（3 个变量，不溢出）",
            "var a = 1;\nvar b = 2;\nvar c = a + b;\nprint(c);\n",
            "3 个变量，活跃峰值 3，远低于 32 寄存器上限。所有变量均分配寄存器。",
            {},
            0,
            0,
        });

        // 场景 2：中等变量（8 个，不溢出）
        {
            std::string code = "";
            for (int i = 0; i < 8; ++i) {
                code += "var v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
            }
            code += "print(v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7);\n";
            v.push_back({
                "中等变量（8 个变量，不溢出）",
                code,
                "8 个变量顺序定义，全部在 print 行使用。活跃峰值 8，无需溢出。",
                {},
                0,
                0,
            });
        }

        // 场景 3：大量变量（40 个，部分溢出）
        {
            std::string code = "";
            for (int i = 0; i < 40; ++i) {
                code += "var a" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
            }
            code += "print(a0";
            for (int i = 1; i < 40; ++i) {
                code += " + a" + std::to_string(i);
            }
            code += ");\n";
            v.push_back({
                "大量变量（40 个变量，部分溢出）",
                code,
                "40 个变量同时活跃，超过 32 寄存器上限。线性扫描将后 8 个变量溢出到栈。",
                {},
                0,
                0,
            });
        }

        // 场景 4：循环变量（5 个变量，循环内复用）
        v.push_back({
            "循环变量（5 个变量，循环内复用）",
            "for i in 0..10 {\n  var x = i * 2;\n  var y = x + 1;\n  var z = y + x;\n  var w = z + y;\n  "
            "print(w);\n}\n",
            "循环体内 5 个变量，变量早期死亡后寄存器可复用。展示线性扫描的寄存器复用能力。",
            {},
            0,
            0,
        });

        return v;
    }();
    return kScenarios;
}

// ============================================================
// 匿名命名空间 — 简化版线性扫描分析工具
// ============================================================
namespace {

/// 判断字符是否为标识符字符（字母/数字/下划线）
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// 从字符串的指定位置提取标识符
std::string extractIdent(const std::string& s, size_t start) {
    size_t i = start;
    // 跳过前导空白
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    // 读取标识符
    std::string name;
    while (i < s.size() && isIdentChar(s[i])) {
        name += s[i];
        ++i;
    }
    return name;
}

/// 去除行首空白
std::string trimLeft(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

/// 检查行中是否包含某个标识符（词边界匹配）
bool containsIdent(const std::string& line, const std::string& word) {
    if (word.empty()) {
        return false;
    }
    size_t pos = 0;
    while ((pos = line.find(word, pos)) != std::string::npos) {
        bool leftOk = (pos == 0) || !isIdentChar(line[pos - 1]);
        bool rightOk = (pos + word.size() >= line.size()) || !isIdentChar(line[pos + word.size()]);
        if (leftOk && rightOk) {
            return true;
        }
        pos += word.size();
    }
    return false;
}

/// 变量活跃区间（分析内部用）
struct VarLiveRange {
    std::string name;
    int defLine;     // 定义行（1-based）
    int lastUseLine; // 最后使用行
};

/// 按行分割代码（支持 \n 和 \r\n）
void splitLines(const std::string& code, std::vector<std::string>& out) {
    out.clear();
    std::string current;
    for (char c : code) {
        if (c == '\n') {
            out.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current += c;
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
}

/// 从代码中提取变量定义和活跃区间
std::vector<VarLiveRange> extractLiveRanges(const std::string& code) {
    std::vector<std::string> lines;
    splitLines(code, lines);

    std::vector<VarLiveRange> ranges;

    // 第一步：识别变量定义（var 声明 + for 循环变量）
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string trimmed = trimLeft(lines[i]);
        if (trimmed.rfind("var ", 0) == 0) {
            std::string name = extractIdent(trimmed, 4);
            if (!name.empty()) {
                ranges.push_back({name, i + 1, i + 1});
            }
        } else if (trimmed.rfind("for ", 0) == 0) {
            // for <var> in <range> {
            std::string name = extractIdent(trimmed, 4);
            if (!name.empty()) {
                ranges.push_back({name, i + 1, i + 1});
            }
        }
    }

    // 第二步：计算每个变量的最后使用行
    for (auto& r : ranges) {
        for (int i = r.defLine - 1; i < static_cast<int>(lines.size()); ++i) {
            if (containsIdent(lines[i], r.name)) {
                r.lastUseLine = i + 1;
            }
        }
    }

    return ranges;
}

/// 计算活跃峰值（同时活跃的最大变量数）
int computeActivePeak(const std::vector<VarLiveRange>& ranges) {
    if (ranges.empty()) {
        return 0;
    }
    int maxLine = 0;
    for (const auto& r : ranges) {
        maxLine = std::max(maxLine, r.lastUseLine);
    }
    int peak = 0;
    for (int line = 1; line <= maxLine; ++line) {
        int count = 0;
        for (const auto& r : ranges) {
            if (r.defLine <= line && line <= r.lastUseLine) {
                ++count;
            }
        }
        peak = std::max(peak, count);
    }
    return peak;
}

/// 活跃区间项（线性扫描内部用）
struct ActiveItem {
    VarLiveRange range;
    int reg; // 0-31 已分配，-1 已溢出，-2 已释放
};

/// 运行简化版线性扫描算法
/// - 按 defLine 排序
/// - 贪心分配最低空闲寄存器
/// - 无空闲寄存器时溢出结束最远的变量（经典启发式）
std::vector<VarAllocInfo> runLinearScan(const std::vector<VarLiveRange>& ranges, int& spillCount) {
    spillCount = 0;
    std::vector<VarAllocInfo> result;

    if (ranges.empty()) {
        return result;
    }

    // 按定义行排序
    std::vector<VarLiveRange> sorted = ranges;
    std::sort(sorted.begin(), sorted.end(),
              [](const VarLiveRange& a, const VarLiveRange& b) { return a.defLine < b.defLine; });

    std::vector<ActiveItem> active;
    std::vector<bool> regFree(RegisterAllocatorLibrary::kRegisterCount, true);

    for (const auto& r : sorted) {
        // 释放已结束的活跃区间
        for (auto& a : active) {
            if (a.reg >= 0 && a.range.lastUseLine < r.defLine) {
                regFree[a.reg] = true;
                a.reg = -2; // 标记已释放
            }
        }
        active.erase(std::remove_if(active.begin(), active.end(), [](const ActiveItem& a) { return a.reg == -2; }),
                     active.end());

        // 查找最低空闲寄存器
        int freeReg = -1;
        for (int i = 0; i < RegisterAllocatorLibrary::kRegisterCount; ++i) {
            if (regFree[i]) {
                freeReg = i;
                break;
            }
        }

        VarAllocInfo info;
        info.varName = r.name;
        info.liveRange = "行" + std::to_string(r.defLine) + "-行" + std::to_string(r.lastUseLine);
        info.spilled = false;

        if (freeReg >= 0) {
            // 有空闲寄存器，直接分配
            regFree[freeReg] = false;
            info.regName = "R" + std::to_string(freeReg);
            active.push_back({r, freeReg});
        } else {
            // 无空闲寄存器，需要溢出决策
            // 找到活跃集中结束最远的变量
            int furthestIdx = -1;
            int furthestEnd = r.lastUseLine;
            for (int i = 0; i < static_cast<int>(active.size()); ++i) {
                if (active[i].range.lastUseLine > furthestEnd) {
                    furthestEnd = active[i].range.lastUseLine;
                    furthestIdx = i;
                }
            }

            if (furthestIdx >= 0 && active[furthestIdx].range.lastUseLine > r.lastUseLine) {
                // 溢出最远的活跃变量，将其寄存器分配给当前变量
                int reg = active[furthestIdx].reg;
                // 在结果中找到被溢出的变量并更新
                for (auto& prev : result) {
                    if (prev.varName == active[furthestIdx].range.name) {
                        prev.regName = "溢出";
                        prev.spilled = true;
                        ++spillCount;
                        break;
                    }
                }
                active[furthestIdx] = {r, reg};
                info.regName = "R" + std::to_string(reg);
            } else {
                // 当前变量被溢出（结束时间不比最远的早）
                active.push_back({r, -1});
                info.regName = "溢出";
                info.spilled = true;
                ++spillCount;
            }
        }

        result.push_back(info);
    }

    return result;
}

} // namespace

// ============================================================
// RegisterAllocatorPanel 构造
// ============================================================

RegisterAllocatorPanel::RegisterAllocatorPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    // 顶部：子页切换按钮
    auto* pageBar = new QHBoxLayout;
    pageTheoryBtn_ = new QPushButton(tr("① 寄存器分配原理"));
    pageSimulatorBtn_ = new QPushButton(tr("② 寄存器分配模拟器"));
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
    connect(loadSampleBtn, &QPushButton::clicked, this, &RegisterAllocatorPanel::onLoadSample);

    // 初始数据填充
    populateTheory();
    populatePresets();

    // 默认加载第一个场景
    codeEdit_->setText(QString::fromUtf8(RegisterAllocatorLibrary::scenarios()[0].code.c_str()));
}

// ============================================================
// 子页 1：寄存器分配原理
// ============================================================

void RegisterAllocatorPanel::buildTheoryPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 理论概览
    theoryBrowser_ = new QTextBrowser;
    layout->addWidget(theoryBrowser_, 1);

    // 经典算法对比表
    layout->addWidget(new QLabel(tr("<b>经典寄存器分配算法对比</b>")));
    algoTable_ = new QTableWidget(0, 4);
    algoTable_->setHorizontalHeaderLabels({tr("算法"), tr("复杂度"), tr("优点"), tr("缺点")});
    algoTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    algoTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    algoTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    algoTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    algoTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(algoTable_, 1);

    // MiniLang RegisterVM 寄存器模型表
    layout->addWidget(new QLabel(tr("<b>MiniLang RegisterVM 寄存器模型</b>")));
    regModelTable_ = new QTableWidget(0, 3);
    regModelTable_->setHorizontalHeaderLabels({tr("概念"), tr("MiniLang 实现"), tr("说明")});
    regModelTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    regModelTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    regModelTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    regModelTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(regModelTable_, 1);
}

void RegisterAllocatorPanel::populateTheory() {
    // 理论概览 HTML
    QString html = QStringLiteral("<h2>寄存器分配（Register Allocation）原理</h2>"
                                  "<p>寄存器分配是编译器的核心优化阶段，其目标是：<b>将程序中的变量映射到有限的 CPU "
                                  "寄存器，最大化寄存器使用率，最小化内存访问</b>。</p>"
                                  "<h3>为什么需要寄存器分配</h3>"
                                  "<ul>"
                                  "<li>CPU 寄存器数量有限（x86-64 约 16 个通用寄存器，ARM64 约 31 个）</li>"
                                  "<li>程序中的变量数可能远超寄存器数（一个函数可能有几十个局部变量）</li>"
                                  "<li>寄存器访问比内存访问快 1-2 个数量级</li>"
                                  "<li>合理分配寄存器直接影响程序运行性能</li>"
                                  "</ul>"
                                  "<h3>核心概念</h3>"
                                  "<ul>"
                                  "<li><b>活跃区间（Live Range）</b>：变量从定义点到最后一次使用的代码区间。"
                                  "区间不重叠的变量可以共享同一寄存器。</li>"
                                  "<li><b>寄存器溢出（Spilling）</b>：当活跃变量数超过可用寄存器数时，"
                                  "部分变量被溢出到栈内存，需要时再加载回寄存器。</li>"
                                  "<li><b>干涉图（Interference Graph）</b>：若两个变量的活跃区间重叠，"
                                  "则它们互相干涉，不能共享同一寄存器。</li>"
                                  "</ul>"
                                  "<h3>经典算法</h3>"
                                  "<ul>"
                                  "<li><b>图着色（Graph Coloring）</b>：构建干涉图，用 k 种颜色着色"
                                  "（k = 寄存器数）。NP 完备问题，实际使用启发式求解。</li>"
                                  "<li><b>线性扫描（Linear Scan）</b>：按活跃区间起始排序，一次扫描完成分配。"
                                  "O(n log n)，适合 JIT 编译。</li>"
                                  "<li><b>SSA-based</b>：在 SSA 形式下，干涉图可简化为弦图（Chordal Graph），"
                                  "多项式时间内最优分配。</li>"
                                  "</ul>"
                                  "<h3>MiniLang RegisterVM 的寄存器模型</h3>"
                                  "<p>MiniLang 的寄存器式 VM 使用 <b>32 个虚拟寄存器（R0-R31）</b>，"
                                  "编译期静态分配：</p>"
                                  "<ul>"
                                  "<li>每个调用帧最多 32 个寄存器（std::array&lt;Value, 32&gt;，零堆分配）</li>"
                                  "<li>R0..R(arity-1)：参数寄存器（调用时由调用者填充）</li>"
                                  "<li>R(arity)..R(localCount-1)：局部变量寄存器</li>"
                                  "<li>R(localCount)..R(registerCount-1)：临时寄存器</li>"
                                  "<li>超过 32 个寄存器需求时，编译期报错（不支持运行时溢出）</li>"
                                  "</ul>"
                                  "<p><b>与工业级编译器的差异</b>：MiniLang 采用最简单的编译期分配——"
                                  "每个变量直接映射到一个固定寄存器号，无活跃区间分析、无溢出处理。"
                                  "这简化了实现但限制了单函数可处理的变量数（≤ 32）。</p>"
                                  "<p style='color:#666;font-size:small;'>"
                                  "提示：切换到「② 寄存器分配模拟器」子页，选择预设场景体验线性扫描算法。"
                                  "</p>");
    theoryBrowser_->setHtml(html);

    // 经典算法对比表
    const auto& algos = RegisterAllocatorLibrary::algoInfos();
    algoTable_->setRowCount(static_cast<int>(algos.size()));
    for (int i = 0; i < static_cast<int>(algos.size()); ++i) {
        const auto& a = algos[i];
        algoTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(a.name.c_str())));
        algoTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(a.complexity.c_str())));
        algoTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(a.pros.c_str())));
        algoTable_->setItem(i, 3, new QTableWidgetItem(QString::fromUtf8(a.cons.c_str())));
        algoTable_->item(i, 2)->setToolTip(QString::fromUtf8(a.pros.c_str()));
        algoTable_->item(i, 3)->setToolTip(QString::fromUtf8(a.cons.c_str()));
    }

    // MiniLang 寄存器模型表
    const auto& models = RegisterAllocatorLibrary::miniLangModels();
    regModelTable_->setRowCount(static_cast<int>(models.size()));
    for (int i = 0; i < static_cast<int>(models.size()); ++i) {
        const auto& m = models[i];
        regModelTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(m.conceptName.c_str())));
        regModelTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(m.miniLangImpl.c_str())));
        regModelTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(m.explanation.c_str())));
        regModelTable_->item(i, 2)->setToolTip(QString::fromUtf8(m.explanation.c_str()));
    }
}

// ============================================================
// 子页 2：寄存器分配模拟器
// ============================================================

void RegisterAllocatorPanel::buildSimulatorPage(QWidget* host) {
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // 顶部：代码输入 + 分析按钮
    auto* inputBar = new QHBoxLayout;
    inputBar->addWidget(new QLabel(tr("代码：")));
    codeEdit_ = new QLineEdit;
    codeEdit_->setPlaceholderText(tr("输入 MiniLang 代码（var 声明 + for 循环）"));
    inputBar->addWidget(codeEdit_, 1);
    analyzeBtn_ = new QPushButton(tr("分析"));
    inputBar->addWidget(analyzeBtn_);
    layout->addLayout(inputBar);

    // 预设场景列表
    layout->addWidget(new QLabel(tr("预设场景：")));
    presetList_ = new QListWidget;
    presetList_->setMaximumHeight(100);
    layout->addWidget(presetList_);

    // 中间：垂直分割器
    auto* vSplitter = new QSplitter(Qt::Vertical);

    // 上方：变量-寄存器分配表
    auto* varAllocWidget = new QWidget;
    auto* varAllocLayout = new QVBoxLayout(varAllocWidget);
    varAllocLayout->setContentsMargins(0, 0, 0, 0);
    varAllocLayout->setSpacing(2);
    varAllocLayout->addWidget(new QLabel(tr("<b>变量-寄存器分配表</b>")));
    varAllocTable_ = new QTableWidget(0, 4);
    varAllocTable_->setHorizontalHeaderLabels({tr("变量名"), tr("分配寄存器"), tr("活跃区间"), tr("状态")});
    varAllocTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    varAllocTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    varAllocTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    varAllocTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    varAllocTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    varAllocLayout->addWidget(varAllocTable_);
    vSplitter->addWidget(varAllocWidget);

    // 下方：水平分割器（寄存器使用情况 + 分配详情）
    auto* hSplitter = new QSplitter(Qt::Horizontal);

    // 左侧：寄存器使用情况表（32 行 R0-R31）
    auto* regUsageWidget = new QWidget;
    auto* regUsageLayout = new QVBoxLayout(regUsageWidget);
    regUsageLayout->setContentsMargins(0, 0, 0, 0);
    regUsageLayout->setSpacing(2);
    regUsageLayout->addWidget(new QLabel(tr("<b>寄存器使用情况（R0-R31）</b>")));
    regUsageTable_ = new QTableWidget(RegisterAllocatorLibrary::kRegisterCount, 2);
    regUsageTable_->setHorizontalHeaderLabels({tr("寄存器"), tr("分配的变量")});
    regUsageTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    regUsageTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    regUsageTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // 预填充寄存器行
    for (int i = 0; i < RegisterAllocatorLibrary::kRegisterCount; ++i) {
        regUsageTable_->setItem(i, 0, new QTableWidgetItem(QStringLiteral("R%1").arg(i)));
        regUsageTable_->setItem(i, 1, new QTableWidgetItem(tr("（空闲）")));
    }
    regUsageLayout->addWidget(regUsageTable_);
    hSplitter->addWidget(regUsageWidget);

    // 右侧：分配详情
    auto* detailWidget = new QWidget;
    auto* detailLayout = new QVBoxLayout(detailWidget);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(2);
    detailLayout->addWidget(new QLabel(tr("<b>分配详情</b>")));
    detailBrowser_ = new QTextBrowser;
    detailLayout->addWidget(detailBrowser_);
    hSplitter->addWidget(detailWidget);

    hSplitter->setStretchFactor(0, 2);
    hSplitter->setStretchFactor(1, 3);
    vSplitter->addWidget(hSplitter);

    vSplitter->setStretchFactor(0, 2);
    vSplitter->setStretchFactor(1, 3);
    layout->addWidget(vSplitter, 1);

    // 信号连接
    connect(analyzeBtn_, &QPushButton::clicked, this, &RegisterAllocatorPanel::onAnalyze);
    connect(presetList_, &QListWidget::currentRowChanged, this, &RegisterAllocatorPanel::onSelectPreset);
}

void RegisterAllocatorPanel::populatePresets() {
    presetList_->clear();
    const auto& scenarios = RegisterAllocatorLibrary::scenarios();
    for (const auto& s : scenarios) {
        presetList_->addItem(QString::fromUtf8(s.title.c_str()));
    }
}

// ============================================================
// 模拟器逻辑
// ============================================================

void RegisterAllocatorPanel::analyzeCode(const std::string& code) {
    // 提取变量活跃区间
    std::vector<VarLiveRange> ranges = extractLiveRanges(code);

    // 计算活跃峰值
    currentActivePeak_ = computeActivePeak(ranges);

    // 运行线性扫描分配
    currentAllocations_ = runLinearScan(ranges, currentSpillCount_);

    // 渲染结果
    renderAllocations();
    renderRegisterUsage();
    renderDetails();
}

void RegisterAllocatorPanel::renderAllocations() {
    varAllocTable_->setRowCount(static_cast<int>(currentAllocations_.size()));
    for (int i = 0; i < static_cast<int>(currentAllocations_.size()); ++i) {
        const auto& a = currentAllocations_[i];
        varAllocTable_->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(a.varName.c_str())));
        varAllocTable_->setItem(i, 1, new QTableWidgetItem(QString::fromUtf8(a.regName.c_str())));
        varAllocTable_->setItem(i, 2, new QTableWidgetItem(QString::fromUtf8(a.liveRange.c_str())));

        auto* statusItem = new QTableWidgetItem(a.spilled ? tr("溢出到栈") : tr("已分配寄存器"));
        if (a.spilled) {
            statusItem->setForeground(QColor("#C62828")); // 红
        } else {
            statusItem->setForeground(QColor("#2E7D32")); // 绿
        }
        varAllocTable_->setItem(i, 3, statusItem);
    }
}

void RegisterAllocatorPanel::renderRegisterUsage() {
    // 清空所有寄存器的分配
    for (int i = 0; i < RegisterAllocatorLibrary::kRegisterCount; ++i) {
        regUsageTable_->setItem(i, 0, new QTableWidgetItem(QStringLiteral("R%1").arg(i)));
        regUsageTable_->setItem(i, 1, new QTableWidgetItem(tr("（空闲）")));
    }

    // 收集每个寄存器分配的变量（可能有多个变量复用同一寄存器）
    std::vector<std::vector<std::string>> regVars(RegisterAllocatorLibrary::kRegisterCount);
    for (const auto& a : currentAllocations_) {
        if (!a.spilled && a.regName.size() > 1 && a.regName[0] == 'R') {
            // 解析寄存器号
            int regNum = -1;
            try {
                regNum = std::stoi(a.regName.substr(1));
            } catch (...) {
                continue;
            }
            if (regNum >= 0 && regNum < RegisterAllocatorLibrary::kRegisterCount) {
                regVars[regNum].push_back(a.varName);
            }
        }
    }

    // 渲染
    for (int i = 0; i < RegisterAllocatorLibrary::kRegisterCount; ++i) {
        if (regVars[i].empty()) {
            regUsageTable_->setItem(i, 1, new QTableWidgetItem(tr("（空闲）")));
        } else {
            std::string combined;
            for (size_t j = 0; j < regVars[i].size(); ++j) {
                if (j > 0) {
                    combined += ", ";
                }
                combined += regVars[i][j];
            }
            auto* item = new QTableWidgetItem(QString::fromUtf8(combined.c_str()));
            item->setToolTip(QString::fromUtf8(combined.c_str()));
            regUsageTable_->setItem(i, 1, item);
        }
    }
}

void RegisterAllocatorPanel::renderDetails() {
    int varCount = static_cast<int>(currentAllocations_.size());
    int regAllocCount = varCount - currentSpillCount_;

    std::ostringstream html;
    html << "<h3>活跃区间分析</h3>";
    html << "<p>共分析 <b>" << varCount << "</b> 个变量，"
         << "活跃峰值 <b>" << currentActivePeak_ << "</b> 个变量同时活跃。</p>";

    html << "<h3>分配结果</h3>";
    html << "<p>分配寄存器：<b>" << regAllocCount << "</b> 个变量<br>"
         << "溢出到栈：<b>" << currentSpillCount_ << "</b> 个变量</p>";

    if (currentSpillCount_ == 0) {
        html << "<h3>溢出决策</h3>";
        html << "<p>所有变量均在寄存器中，<b>无需溢出</b>。";
        if (currentActivePeak_ <= RegisterAllocatorLibrary::kRegisterCount) {
            html << "活跃峰值 " << currentActivePeak_ << " ≤ 寄存器上限 " << RegisterAllocatorLibrary::kRegisterCount
                 << "。</p>";
        }
    } else {
        html << "<h3>溢出决策</h3>";
        html << "<p>活跃变量数超过寄存器上限 " << RegisterAllocatorLibrary::kRegisterCount
             << "，以下变量被溢出到栈：</p><ul>";
        for (const auto& a : currentAllocations_) {
            if (a.spilled) {
                html << "<li><b>" << a.varName << "</b>（活跃区间：" << a.liveRange << "）</li>";
            }
        }
        html << "</ul>";
        html << "<p>溢出策略：<b>选择结束最远的变量溢出</b>"
             << "（线性扫描经典启发式）。被溢出的变量在需要时从栈加载到寄存器，"
             << "使用后再写回栈。</p>";
    }

    html << "<h3>性能影响</h3>";
    if (currentSpillCount_ == 0) {
        html << "<p style='color:#2E7D32;'>所有变量访问均在寄存器中，"
             << "<b>性能最优</b>，无额外内存访问开销。</p>";
    } else {
        html << "<p style='color:#C62828;'>溢出变量每次访问需要额外的 <b>load/store</b> 操作，"
             << "预计增加约 <b>" << currentSpillCount_ * 2 << "</b> 次内存访问。"
             << "在热点循环中，溢出会显著降低性能。</p>";
    }

    html << "<h3>寄存器复用情况</h3>";
    // 统计寄存器复用
    int reusedRegs = 0;
    std::vector<int> regVarCount(RegisterAllocatorLibrary::kRegisterCount, 0);
    for (const auto& a : currentAllocations_) {
        if (!a.spilled && a.regName.size() > 1 && a.regName[0] == 'R') {
            int regNum = -1;
            try {
                regNum = std::stoi(a.regName.substr(1));
            } catch (...) {
                continue;
            }
            if (regNum >= 0 && regNum < RegisterAllocatorLibrary::kRegisterCount) {
                regVarCount[regNum]++;
                if (regVarCount[regNum] > 1) {
                    reusedRegs++;
                }
            }
        }
    }
    if (reusedRegs > 0) {
        html << "<p>有 <b>" << reusedRegs << "</b> 个寄存器被多个变量复用"
             << "（变量死亡后寄存器释放给新变量）。"
             << "这是线性扫描的寄存器复用优化，减少了对寄存器总数的需求。</p>";
    } else {
        html << "<p>无寄存器复用（每个变量独占一个寄存器）。</p>";
    }

    detailBrowser_->setHtml(QString::fromUtf8(html.str().c_str()));
}

// ============================================================
// 槽函数
// ============================================================

void RegisterAllocatorPanel::onAnalyze() {
    std::string code = codeEdit_->text().toStdString();
    if (code.empty()) {
        detailBrowser_->setHtml(tr("<p style='color:red;'>请输入代码或选择预设场景</p>"));
        return;
    }

    // 重置结果
    currentAllocations_.clear();
    currentActivePeak_ = 0;
    currentSpillCount_ = 0;

    analyzeCode(code);
}

void RegisterAllocatorPanel::onSelectPreset(int idx) {
    if (idx < 0) {
        return;
    }
    const auto& scenarios = RegisterAllocatorLibrary::scenarios();
    if (idx < static_cast<int>(scenarios.size())) {
        codeEdit_->setText(QString::fromUtf8(scenarios[idx].code.c_str()));
        // 自动分析
        onAnalyze();
    }
}

void RegisterAllocatorPanel::onLoadSample() {
    // 加载一个展示寄存器分配相关概念的 MiniLang 样例到主编辑器
    QString sample = QStringLiteral("// 寄存器分配样例：变量到寄存器的映射\n"
                                    "// MiniLang RegisterVM 使用 32 个虚拟寄存器（R0-R31）\n"
                                    "\n"
                                    "// 少量变量：a, b, c 分别分配 R0, R1, R2\n"
                                    "var a = 1;\n"
                                    "var b = 2;\n"
                                    "var c = a + b;\n"
                                    "print(c);\n"
                                    "\n"
                                    "// 变量复用：x 死亡后 R1 释放给 y\n"
                                    "var x = 10;\n"
                                    "var y = x * 2;\n"
                                    "print(y);\n");
    emit loadSampleRequested(sample);
}

void RegisterAllocatorPanel::onPageSwitch(int idx) {
    stack_->setCurrentIndex(idx);
}
