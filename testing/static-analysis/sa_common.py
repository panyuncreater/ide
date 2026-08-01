#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""testing/static-analysis/sa_common.py — 静态分析公共库（阶段六）

被 run_clang_tidy.py / run_clazy.py 复用：clazy 是 clang-tidy 的插件，二者诊断
输出格式一致（`file:line:col: warning: msg [check]`），故解析/严重度分级/报告
生成/工具定位逻辑完全共享。

职责：
  - locate_tool         : 跨平台定位可执行文件（env > PATH > 常见目录）。
  - parse_diagnostics   : 从 clang-tidy/clazy stdout 解析诊断，去重。
  - severity_of         : 按检查项名归类严重度（high/medium/low）。
  - explain             : 给检查项一句话中文解释（未知项回退到原始消息）。
  - write_report        : 生成 Markdown 报告（含按严重度的 Top 20 清单）。

无第三方依赖，兼容 Python 3.8+。只新增工具代码，不改编译器实现。
"""

import os
import re
import shutil
from collections import Counter, defaultdict
from pathlib import Path

# clang-tidy / clazy 诊断行：路径:行:列: 级别: 消息 [check1,check2]
_DIAG_RE = re.compile(
    r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+):\s+"
    r"(?P<sev>warning|error|note):\s+(?P<msg>.*?)(?:\s+\[(?P<check>[A-Za-z0-9_.\-,]+)\])?$")


# ============================================================
# 工具定位
# ============================================================
def locate_tool(names, env_var=None, extra_dirs=None):
    """按 env > PATH > 额外目录 定位可执行文件；找不到返回 None。

    names: 候选可执行名（如 ['clang-tidy','clang-tidy-18']）。
    """
    if env_var:
        v = os.environ.get(env_var)
        if v and Path(v).is_file():
            return v
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    for d in (extra_dirs or []):
        for n in names:
            cand = Path(d) / (n + (".exe" if os.name == "nt" else ""))
            if cand.is_file():
                return str(cand)
    return None


def default_clang_tidy_dirs():
    """clang-tidy 常见安装目录（含 pip 的 clang-tidy wheel 的 Scripts 目录）。"""
    dirs = []
    # pip clang-tidy wheel 的入口点（Windows）
    for base in [os.environ.get("APPDATA", ""), os.environ.get("LOCALAPPDATA", "")]:
        if base:
            dirs.append(str(Path(base) / "Programs" / "Python"))
    # 常见 Python Scripts 目录
    la = os.environ.get("LOCALAPPDATA", "")
    if la:
        for v in ("Python313", "Python312", "Python311", "Python310"):
            dirs.append(str(Path(la) / "Programs" / "Python" / v / "Scripts"))
    # LLVM 标准安装 & VS 内置
    dirs += [
        r"C:\Program Files\LLVM\bin",
        "/usr/bin", "/usr/local/bin", "/usr/lib/llvm/bin",
    ]
    return [d for d in dirs if d]


# ============================================================
# 诊断解析与去重
# ============================================================
class Finding:
    __slots__ = ("file", "line", "col", "sev", "msg", "check")

    def __init__(self, file, line, col, sev, msg, check):
        self.file = file
        self.line = int(line)
        self.col = int(col)
        self.sev = sev
        self.msg = msg
        self.check = check or "clang-diagnostic"

    def key(self):
        return (self.file, self.line, self.col, self.check)

    def as_dict(self):
        return {"file": self.file, "line": self.line, "col": self.col,
                "sev": self.sev, "msg": self.msg, "check": self.check}


def parse_diagnostics(text, repo_root=None):
    """解析一段 clang-tidy/clazy 输出，返回 (findings, parse_error_count)。

    - 只收 warning/error（note 作为上一条的附注忽略）。
    - clang-diagnostic-error（多为编译/解析错误，如缺头文件）单独计数，不混入 findings。
    - 去重：同 (file,line,col,check) 只保留一条。
    """
    findings = {}
    parse_errors = 0
    for raw in text.splitlines():
        m = _DIAG_RE.match(raw.strip())
        if not m:
            continue
        sev = m.group("sev")
        if sev == "note":
            continue
        check = m.group("check") or ""
        if sev == "error" and (not check or check.startswith("clang-diagnostic")):
            parse_errors += 1
            continue
        f = m.group("file")
        if repo_root:
            try:
                f = str(Path(f).resolve().relative_to(repo_root)).replace("\\", "/")
            except (ValueError, OSError):
                f = f.replace("\\", "/")
        # 一行可能列出多个 check，用主 check（首个）
        primary = check.split(",")[0] if check else "clang-diagnostic"
        fd = Finding(f, m.group("line"), m.group("col"), sev, m.group("msg"), primary)
        findings[fd.key()] = fd
    return list(findings.values()), parse_errors


# ============================================================
# 严重度分级
# ============================================================
# 高危关键词（命中即 high）：多指向真实内存/生命周期/未定义行为。
_HIGH_KW = ("use-after-move", "dangling", "null-dereference", "uninitialized",
            "memory", "leak", "double-free", "invalid", "overflow",
            "use-after-free", "division", "sizeof", "bad-signal",
            "integer-division", "suspicious-memory")
# clang-analyzer 的核心/C++/内存组视为 high。
_HIGH_ANALYZER = ("analyzer-core", "analyzer-cplusplus", "analyzer-unix",
                  "analyzer-nullability", "analyzer-security")


def severity_of(check):
    """按检查项名归类严重度：high / medium / low。

    顺序关键：先判明确高危组（clang-analyzer 核心/内存）与低信号风格组，
    再按关键词兜底——否则 owning-memory 会先命中 "memory" 关键词而误升 high。
    """
    c = check.lower()
    # 明确高危：clang-analyzer 核心/内存/安全组（路径敏感分析的真 bug 信号）。
    if any(k in c for k in _HIGH_ANALYZER):
        return "high"
    # 低信号/风格类：先于关键词判断。
    if c.startswith("cppcoreguidelines-pro-") or \
       c.startswith("cppcoreguidelines-special-member") or \
       c.startswith("cppcoreguidelines-avoid-non-const-global") or \
       c.startswith("cppcoreguidelines-owning-memory") or \
       "analyzer-deadcode" in c or "readability" in c:
        return "low"
    # 关键词兜底（use-after-move / dangling / null / uninitialized / leak…）。
    if any(k in c for k in _HIGH_KW):
        return "high"
    # 其余 bugprone / cppcoreguidelines 正确性类归 medium
    return "medium"


SEV_ORDER = {"high": 0, "medium": 1, "low": 2}
SEV_ZH = {"high": "高", "medium": "中", "low": "低"}


# ============================================================
# 检查项一句话解释（常见项；未知回退原始消息）
# ============================================================
CHECK_EXPLANATIONS = {
    "clang-analyzer-core.NullDereference": "解引用可能为空的指针（路径敏感分析发现）。",
    "clang-analyzer-core.CallAndMessage": "以未初始化/空值作为调用或参数。",
    "clang-analyzer-core.UndefinedBinaryOperatorResult": "二元运算含未初始化操作数，结果未定义。",
    "clang-analyzer-core.uninitialized.Assign": "用未初始化的值赋值。",
    "clang-analyzer-core.uninitialized.Branch": "分支条件依赖未初始化的值。",
    "clang-analyzer-core.DivideZero": "可能的除零。",
    "clang-analyzer-cplusplus.NewDelete": "new/delete 使用错误（可能重复释放或误用）。",
    "clang-analyzer-cplusplus.NewDeleteLeaks": "new 分配的内存在某路径上泄漏。",
    "clang-analyzer-cplusplus.InnerPointer": "持有已失效容器的内部指针。",
    "clang-analyzer-deadcode.DeadStores": "赋值后从未被读取（死存储）。",
    "clang-analyzer-optin.cplusplus.VirtualCall": "构造/析构中调用虚函数。",
    "clang-analyzer-optin.cplusplus.UninitializedObject": "对象构造后仍有未初始化成员。",
    "clang-analyzer-unix.Malloc": "malloc/free 使用错误或泄漏。",
    "bugprone-use-after-move": "对象被 move 后又被使用，值处于未指定状态。",
    "bugprone-dangling-handle": "句柄/视图引用了已销毁的临时对象。",
    "bugprone-integer-division": "整型除法结果被隐式转 float，疑似精度丢失。",
    "bugprone-narrowing-conversions": "隐式收窄转换，可能丢位/溢出。",
    "bugprone-signed-char-misuse": "signed char 与整型混用导致符号扩展意外。",
    "bugprone-branch-clone": "if/else 或 switch 分支体完全相同（疑似复制粘贴错误）。",
    "bugprone-macro-parentheses": "宏参数未加括号，展开后优先级可能出错。",
    "bugprone-sizeof-expression": "可疑的 sizeof 用法（如 sizeof 指针/表达式）。",
    "bugprone-suspicious-memory-comparison": "对非平凡类型做 memcmp/memset。",
    "bugprone-unchecked-optional-access": "未检查即访问 optional，可能空解引用。",
    "bugprone-unhandled-self-assignment": "拷贝赋值未处理自赋值。",
    "bugprone-reserved-identifier": "使用了保留标识符（以 _ 开头等）。",
    "bugprone-implicit-widening-of-multiplication-result": "乘法在收窄类型上进行后再加宽，可能已溢出。",
    "bugprone-parent-virtual-call": "越过直接父类调用祖父类虚函数。",
    "bugprone-throw-keyword-missing": "疑似构造异常对象却忘记 throw。",
    "bugprone-string-integer-assignment": "把整数直接赋给 string（可能误当字符）。",
    "cppcoreguidelines-init-variables": "局部变量声明时未初始化。",
    "cppcoreguidelines-pro-type-member-init": "构造后存在未初始化的成员。",
    "cppcoreguidelines-pro-type-static-cast-downcast": "静态下转型，缺乏运行期类型检查。",
    "cppcoreguidelines-pro-type-reinterpret-cast": "使用 reinterpret_cast（类型不安全）。",
    "cppcoreguidelines-pro-type-const-cast": "使用 const_cast 去除常量性。",
    "cppcoreguidelines-pro-type-vararg": "使用 C 变参（类型不安全）。",
    "cppcoreguidelines-pro-bounds-pointer-arithmetic": "指针算术，越界风险。",
    "cppcoreguidelines-special-member-functions": "定义了部分特殊成员函数但未补全（三/五法则）。",
    "cppcoreguidelines-owning-memory": "裸指针持有所有权，宜用智能指针。",
    "cppcoreguidelines-slicing": "对象切片（按值传派生类给基类）。",
    "cppcoreguidelines-narrowing-conversions": "隐式收窄转换。",
    "cppcoreguidelines-avoid-const-or-ref-data-members": "类含 const/引用成员，影响可赋值性。",
    "cppcoreguidelines-prefer-member-initializer": "宜用成员初始化列表而非构造体内赋值。",
    "cppcoreguidelines-rvalue-reference-param-not-moved": "右值引用形参未被 move。",
    # ---- clazy（Qt 专用）常见检查 ----
    "clazy-qstring-allocations": "产生不必要的 QString 临时分配（宜用 QLatin1String/QStringLiteral）。",
    "clazy-qstring-arg": "QString::arg 可合并为一次调用以减少临时对象。",
    "clazy-range-loop-detach": "range-for 遭遇隐式 detach（容器未加 const/qAsConst）。",
    "clazy-range-loop-reference": "range-for 未用引用，发生不必要拷贝。",
    "clazy-container-anti-pattern": "容器反模式（如对临时容器取值导致拷贝）。",
    "clazy-qmap-with-pointer-key": "QMap 以指针为键，迭代顺序不确定。",
    "clazy-lambda-in-connect": "connect 的 lambda 未指定上下文对象，可能悬空调用。",
    "clazy-connect-non-signal": "connect 连接了非信号成员（连接会失败）。",
    "clazy-old-style-connect": "使用 SIGNAL/SLOT 字符串旧式 connect（无编译期校验）。",
    "clazy-wrong-qevent-cast": "QEvent 向错误子类强转，可能误读内存。",
    "clazy-writing-to-temporary": "写入一个立即丢弃的临时对象（写入无效）。",
    "clazy-detaching-temporary": "对临时容器调用 detach 类非 const 方法。",
    "clazy-qdeleteall": "可用 qDeleteAll 简化容器元素释放。",
    "clazy-function-args-by-ref": "大对象形参宜改为 const 引用传递。",
    "clazy-inefficient-qlist-soft": "QList 存储大于指针的类型，低效（宜用 QVector）。",
    "clazy-returning-void-expression": "return 一个 void 表达式（可读性差）。",
}


def explain(check, sample_msg=""):
    """检查项一句话解释；未收录则回退到样例消息（截断）。"""
    if check in CHECK_EXPLANATIONS:
        return CHECK_EXPLANATIONS[check]
    msg = (sample_msg or "").strip()
    if len(msg) > 70:
        msg = msg[:70] + "…"
    return msg or "（见检查项官方文档）"


# ============================================================
# Markdown 报告
# ============================================================
def write_report(out_path, tool_name, meta, findings, parse_errors, top_n=20):
    """生成 Markdown 报告：元信息 / 分级统计 / Top N 清单 / 按检查项汇总 / 按文件汇总。"""
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    by_check = Counter(f.check for f in findings)
    by_sev = Counter(severity_of(f.check) for f in findings)
    by_file = Counter(f.file for f in findings)
    sample_msg = {}
    for f in findings:
        sample_msg.setdefault(f.check, f.msg)

    # Top N：先按严重度（high>medium>low），同级按该检查项命中总数降序，再按文件行号。
    ranked = sorted(
        findings,
        key=lambda f: (SEV_ORDER[severity_of(f.check)], -by_check[f.check], f.file, f.line))

    # 多样化：Top N 中同一检查项最多占 cap 条，避免单个高噪声检查霸榜；
    # 若因检查项种类少而填不满，再放宽 cap 补齐。
    cap = max(2, top_n // 5)
    top = []
    picked = set()
    per_check = Counter()
    for f in ranked:
        if per_check[f.check] >= cap:
            continue
        top.append(f)
        picked.add(f.key())
        per_check[f.check] += 1
        if len(top) >= top_n:
            break
    if len(top) < top_n:
        for f in ranked:
            if f.key() in picked:
                continue
            top.append(f)
            if len(top) >= top_n:
                break

    L = []
    L.append(f"# {tool_name} 静态分析报告（阶段六 · 只报告不修改）\n")
    L.append(f"- 生成时间：{meta.get('time', '-')}")
    L.append(f"- 工具：{meta.get('tool_path', tool_name)}  版本：{meta.get('version', '-')}")
    L.append(f"- 配置：{meta.get('config', '-')}")
    L.append(f"- 分析范围：{meta.get('scope', '-')}")
    L.append(f"- 翻译单元：{meta.get('tu_total', '-')} 个"
             f"（成功 {meta.get('tu_ok', '-')} / 超时 {meta.get('tu_timeout', 0)} "
             f"/ 解析失败 {meta.get('tu_error', 0)}）")
    L.append(f"- 总耗时：{meta.get('elapsed', 0):.1f}s\n")

    if meta.get("note"):
        L.append(f"> {meta['note']}\n")

    L.append("## 分级统计\n")
    L.append("| 严重度 | 数量 |")
    L.append("|--------|-----:|")
    for s in ("high", "medium", "low"):
        L.append(f"| {SEV_ZH[s]}（{s}） | {by_sev.get(s, 0)} |")
    L.append(f"\n合计 **{len(findings)}** 条独立告警"
             f"（去重后），涉及 {len(by_check)} 类检查项、{len(by_file)} 个文件。")
    if parse_errors:
        L.append(f"\n> 另有 {parse_errors} 条 clang-diagnostic 解析级错误"
                 f"（多为 MSVC 特有构造/头文件在 clang 前端下的兼容告警，非目标缺陷；已单列不计入上表）。")
    L.append("")

    L.append(f"## Top {top_n} 问题清单（文件:行号 + 检查项 + 一句话解释）\n")
    if ranked:
        L.append("| # | 严重度 | 文件:行号 | 检查项 | 一句话解释 |")
        L.append("|--:|--------|-----------|--------|-----------|")
        for i, f in enumerate(top, 1):
            sev = severity_of(f.check)
            loc = f"{f.file}:{f.line}"
            L.append(f"| {i} | {SEV_ZH[sev]} | `{loc}` | `{f.check}` | "
                     f"{explain(f.check, f.msg)} |")
        L.append(f"> 说明：同一检查项在 Top {top_n} 中最多列 {cap} 条（多样化展示），"
                 f"完整分布见下「按检查项汇总」。\n")
    else:
        L.append("_本轮无命中（findings 为空）。_")
    L.append("")

    L.append("## 按检查项汇总（命中数降序）\n")
    L.append("| 检查项 | 严重度 | 命中 | 说明 |")
    L.append("|--------|--------|-----:|------|")
    for check, cnt in by_check.most_common(40):
        L.append(f"| `{check}` | {SEV_ZH[severity_of(check)]} | {cnt} | "
                 f"{explain(check, sample_msg.get(check, ''))} |")
    L.append("")

    L.append("## 命中最多的文件（Top 20）\n")
    L.append("| 文件 | 命中 |")
    L.append("|------|-----:|")
    for file, cnt in by_file.most_common(20):
        L.append(f"| `{file}` | {cnt} |")
    L.append("")

    out_path.write_text("\n".join(L) + "\n", encoding="utf-8", newline="\n")
    return out_path
