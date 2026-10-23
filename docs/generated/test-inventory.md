# 测试清单（生成文件，勿手改）

> 本文件是仓库内唯一允许出现测试计数的常规文档。计数随时间变化，**以命令输出为准**；本页只是最近一次快照。
> 刷新时机：每轮末或测试目标变更后。其他文档只链接本页，不复制数字。

## 刷新命令

```bash
# 全量 ctest 计数（需先构建 4 个测试目标）
ctest --test-dir out/build/debug -N

# minilang_tests 用例/套件计数
out/build/debug/tests/minilang_tests.exe --gtest_list_tests
```

## 最近快照

| 项目 | 数值 | 口径 |
|------|------|------|
| 全量 ctest | 4191 | 最近一次全量通过（工作区在途轮次；以命令输出为准） |
| 通过率 | 4191/4191 | `/W4+/WX` 零警告 |

## 说明

- 计数包含 `minilang_tests` / `minilang_app_tests` / `minilang_gui_smoke` / `minilang_perf_test`。
- `minilang_perf_test` 受 `MINILANG_BUILD_PERF_TESTS` 门控，全新 configure 需显式开启，否则全量口径缺失。
- 历史计数见 `CHANGELOG.md` 与 `docs/audit/`（档案层，默认不读）。
