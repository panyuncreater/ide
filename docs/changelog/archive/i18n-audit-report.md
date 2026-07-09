# 翻译文件审计报告 (i18n String Audit)

> 审计日期: 2026-07-05
> 审计范围: app/*.cpp, gui/*.cpp 中所有用户可见字符串

## 审计结论

发现约 **372 处**用户可见字符串使用 QString::fromUtf8 但未用 mlTr()/tr() 包裹。
app/main.cpp 中的字符串已正确包裹。

## 高优先级文件

### app/ide.cpp (~206 处)
- 菜单项 ~40 处: 新建/打开/保存/另存为/格式化/查找/替换/运行/调试/停止 等
- 工具提示 ~10 处: 菜单/运行程序/格式化代码/查看AST 等
- 标签/按钮 ~5 处: MiniLang IDE/行/列/UTF-8 等
- 右键菜单: 新建文件/新建文件夹/重命名/删除
- 状态栏 ~10 处: 行/列/未保存/调试中 等

### gui/ 目录 (~166 处)
- BackendComparePanel: 运行三后端对比/尚未运行/未绑定控制器
- BreakpointConditionPanel: 实时断点/教学场景库/行号/条件表达式
- BugHuntPanel: 运行验证/下一提示/查看答案/加载到主编辑器
- DebugPanel: 变量监视/调用栈/名称/值
- FindReplacePanel: 下一个/上一个/替换/全部替换/未找到
- IRTransformPanel: AST->IR lowering/优化pass对比
- VmStackPanel: 变量名/值
- CodeEditor: 移除条件

## 修复步骤

1. 批量包裹: QString::fromUtf8("...") -> mlTr("...")
2. 运行 lupdate 生成 .ts 条目
3. 填充翻译 (~200-250 条唯一翻译)

## 工作量估计: ~4-5 小时，建议作为独立 PR 提交
