# clazy 静态分析报告（阶段六 · 只报告不修改）

- 生成时间：2026-07-31 23:44:26
- 工具：(未找到 clazy-standalone / clazy)  版本：-
- 配置：CLAZY_CHECKS=level1
- 分析范围：Qt 专用检查（clazy）——本机未安装，见下前置条件
- 翻译单元：0 个（成功 0 / 超时 0 / 解析失败 0）
- 总耗时：0.0s

> clazy 未在本机找到，因此**未产生真实 findings**（阶段六不伪造结果）。
> clazy 依赖 Clang 工具链，推荐在 Linux/WSL 安装后重跑：
> - Ubuntu/Debian：`sudo apt-get install -y clazy`（提供 clazy-standalone）
> - 然后：在 Linux 侧用 clang 生成 compile_commands.json 的构建目录上运行
>   `CLAZY_CHECKS=level1 python testing/static-analysis/run_clazy.py --build-dir <linux-build>`
> - 或设 `MINI_CLAZY` 指向 clazy-standalone 可执行文件。
> 详见 `testing/static-analysis/README.md`（安装方式 + CLAZY_CHECKS 推荐级别）。

## 分级统计

| 严重度 | 数量 |
|--------|-----:|
| 高（high） | 0 |
| 中（medium） | 0 |
| 低（low） | 0 |

合计 **0** 条独立告警（去重后），涉及 0 类检查项、0 个文件。

## Top 20 问题清单（文件:行号 + 检查项 + 一句话解释）

_本轮无命中（findings 为空）。_

## 按检查项汇总（命中数降序）

| 检查项 | 严重度 | 命中 | 说明 |
|--------|--------|-----:|------|

## 命中最多的文件（Top 20）

| 文件 | 命中 |
|------|-----:|

