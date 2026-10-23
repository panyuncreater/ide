# 轮末知识维护仪式（ROUND-END）

> 每个工作轮结束前执行；5 分钟内完成。目的是让下一轮从“权威 + 待办”开工，而不是从历史叙事里考古。

## 1. 五步清单

1. **关闭/新增 BACKLOG**
   - 已修复项删除行，并在 CHANGELOG 写一条用户可见变更（含回归测试名）。
   - 新发现项进 `docs/BACKLOG.md`：ID、严重性、状态、证据、下一步；没有证据的猜想标 `verifying`。
   - 阻塞项在“下一步”写清解除条件，不写过程叙事。
2. **CHANGELOG 追加**
   - 只写用户可见变更与语义影响；内部轮次复盘、测试计数、提交号不进 AGENTS/spec。
3. **审计报告另起文件**
   - 新的一轮写 `docs/audit/YYYY-MM-DD.md`，不复用旧报告继续追加。
   - 报告内“剩余未修”只写 BACKLOG ID；不复制清单内容。
   - 旧报告冻结为历史，不再修改（勘误除外，需在文首注明）。
4. **权威同步**
   - 语义/不变量变化：更新 `docs/specs/backend-consistency.md`、对应 ADR，并删除被取代的旧规则。
   - 新增缺陷模式：进 `docs/specs/defect-taxonomy.md`，必须带审计入口与锁定测试。
   - 评估后不实施：进 `docs/specs/rejected-decisions.md`，必须带重启条件。
5. **跑门禁**
   - `python docs/knowledge/tools/knowledge_lint.py`
   - 可选硬门禁：`cp docs/knowledge/tools/pre-commit .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit`（仅本地，不改 CI）。
   - 代码有改动时按 `AGENTS.md` §6 构建 + 全量 ctest；未通过不进入下一轮。

## 2. 强制蒸馏触发条件（满足任一）

- `AGENTS.md` 超过 8 KB 或 120 行。
- 任一 L1/L2 文档超过预算，或同一事实在 3 个以上文件出现。
- 距上次蒸馏满 5 轮。
- 发生 P0/P1 事故（事故复盘必须转化为一条 spec 规则或 taxonomy 条目 + 检查）。

## 3. 写入纪律

- 中文文档改动使用 write/edit 工具或 Python UTF-8 字节级操作；禁止 PowerShell 裸 `Get-Content`/`Set-Content` 管道（无 BOM UTF-8 会被按 GBK 误解码）。
- 审计轮与修复轮尽量收敛到单一会话；并行会话必须按文件所有权分区，避免编辑互踩与验证口径漂移。
- 文档中的路径、符号、命令必须可解析；不确定的路径不写，改为引用目录或测试名。
- 测试计数只通过命令获取，不手写进任何文档（快照见 `docs/generated/test-inventory.md`）。

## 4. 完成判据

- [ ] BACKLOG 中每个 open/verifying 行都有证据与下一步
- [ ] CHANGELOG 有本轮用户可见变更（纯文档轮可省略）
- [ ] 新审计报告为独立文件，旧报告未被追加
- [ ] 语义变更已同步 spec/ADR，旧规则已删除
- [ ] `knowledge_lint.py` 通过
- [ ] 代码改动已通过构建 + 全量 ctest
