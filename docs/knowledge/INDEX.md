# 知识归属表（INDEX）

> 本表回答“某个事实应该写在哪”。新增/修改文档前先查此表；同一事实只允许一个权威源，其他位置只放链接。
> 维护规则见 `AGENTS.md` §7 与 `docs/knowledge/ROUND-END.md`。

## 1. 事实归属

| 事实类型 | 唯一权威 | 允许的消费者（只放链接） | 禁止复制到 |
|----------|----------|--------------------------|-----------|
| 执行管线、四后端架构、模块系统 | `docs/architecture.md` | `AGENTS.md` §3、README | spec、skill |
| 多后端语义细则与新增特性 checklist | `docs/specs/backend-consistency.md` §1-§3 | `AGENTS.md` §3、`docs/testing.md`、skill | CHANGELOG、BACKLOG |
| 协程挂起语义与实现 checklist | `docs/adr/ADR-007-*.md` + `backend-consistency.md` §9 | `AGENTS.md` §3、taxonomy K13 | 其他 spec |
| JIT per-chunk 与缓存协议 | `docs/adr/ADR-008-*.md` + `backend-consistency.md` §8 | `AGENTS.md` §3、taxonomy K14 | CHANGELOG |
| 值与内存/COW/GC/宿主隔离 | `docs/specs/memory-model.md` + ADR-001/004 | `AGENTS.md` §3、taxonomy K15 | development |
| 缺陷模式与审计入口 | `docs/specs/defect-taxonomy.md` | `AGENTS.md` §1/§4、bughunt skill | CHANGELOG |
| 已否决方案与重启条件 | `docs/specs/rejected-decisions.md` | `docs/BACKLOG.md`（只引用 ID） | CHANGELOG |
| 开放缺陷/阻塞/优化候选 | `docs/BACKLOG.md` | 审计报告（只引用 ID） | 任何其他“剩余清单” |
| 构建命令与验证标准 | `AGENTS.md` §6 | `minilang-build` skill | development、FAQ |
| 构建排错与环境差异 | `docs/getting-started.md`、`docs/faq.md` | `AGENTS.md` §6（链接） | skill（只写步骤） |
| 测试策略与矩阵 | `docs/testing.md` | `minilang-diffcheck` skill | README |
| 测试编写约定 | `docs/specs/testing-conventions.md` | `docs/testing.md` | — |
| 教学面板开发指南 | `docs/development.md` | `AGENTS.md` §1 | skill |
| 测试计数 | 实时命令（`ctest -N` / `--gtest_list_tests`）；快照见 `docs/generated/test-inventory.md` | README/testing（只放链接） | AGENTS、CHANGELOG、spec |
| 轮次历史、提交号、事故复盘 | `CHANGELOG.md`、`docs/audit/` | 无（默认不加载） | AGENTS、spec、skill |
| 用户偏好与协作方式 | Qoder auto-memory（用户级） | 无 | 仓库文档 |

## 2. 分层与预算

| 层 | 文件 | 加载时机 | 预算 |
|----|------|----------|------|
| L0 契约 | `AGENTS.md` | 每轮自动 | ≤ 8 KB / 120 行 |
| L1 权威 | `docs/adr/`、`docs/specs/` | 任务命中 | 每篇 ≤ 160 行 |
| L2 流程 | `.qoder/skills/*/SKILL.md` | 显式调用 | ≤ 150 行/篇 |
| L3 待办 | `docs/BACKLOG.md` | 开工/收尾 | ≤ 200 行 |
| L4 档案 | `CHANGELOG.md`、`docs/audit/` | 默认不读 | 不设限 |

## 3. 判定与升级

1. **违反会导致 bug** → 不变量：写 L1 spec，L0 留一行规则 + 指针。
2. **解释为什么这样工作** → 机制：写 ADR/spec；不写进 AGENTS。
3. **必须按顺序执行** → 流程：写 skill；不复制 L0/L1 的事实。
4. **尚未完成** → `BACKLOG.md`；必须有证据与下一步。
5. **只发生一次** → `CHANGELOG.md`/审计报告；不升级。
6. **重复 ≥2 次 / 曾致 P0/P1 / 改变不变量** → 升级为 spec 规则或 taxonomy 条目，并配可执行检查。
7. **被取代/证伪/评估后不实施** → 删除（git 保留）或进 `rejected-decisions.md`。

## 4. 退役规则

- 被取代的规则直接删除，禁止保留“已废弃但供参考”段落；git 与 CHANGELOG 保留历史。
- 同一事实第三次出现在不同文件时，必须合并到权威源并删除副本。
- 指针必须可解析；失效路径由 `docs/knowledge/tools/knowledge_lint.py` 检出。
