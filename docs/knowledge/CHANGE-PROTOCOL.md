# 每次改动的微蒸馏协议（CHANGE-PROTOCOL）

> 目标：经验在产生它的那次提交内归位，禁止先堆着、轮末再考古。
> 适用：任何改动批次（代码/测试/文档）。`ROUND-END.md` 是轮末聚合检查，本协议是每次提交的入口检查。

## 1. 三步

1. **判定**：这次改动产生了哪类经验？

   | 判定 | 归宿 |
   |------|------|
   | 违反会致 bug | L1 spec/ADR（L0 只留一行规则） |
   | 解释机制 | ADR/spec |
   | 必须按序执行 | skill |
   | 尚未完成 | `docs/BACKLOG.md` |
   | 只发生一次 | `CHANGELOG.md` / `docs/audit/` |
   | 复发 ≥2 / 曾致 P0/P1 / 改变不变量 | spec 或 taxonomy，且必须配检查 |
   | 被取代/证伪/不实施 | 删除，或 `rejected-decisions.md` |
   | 无知识影响 | 显式 `none` |

2. **归位**：写到唯一权威（见 `INDEX.md`），其他位置只放链接。新增条目必须回答：取代/合并了哪条旧条目，或为何预算仍有余量。

3. **门禁**：运行 `python docs/knowledge/tools/knowledge_lint.py`；提交信息末尾加尾注：

   ```
   Knowledge-Impact: spec|adr|taxonomy|rejected|backlog|changelog|knowledge|none
   ```

   `commit-msg` hook 校验尾注与暂存文件匹配；`pre-commit` hook 运行 lint。

## 2. 反叠加规则

- L0–L3 是改写/退役，不是追加；只有 CHANGELOG/audit 允许追加。
- **硬上限**：AGENTS 10 条不变量、taxonomy 20 类、rejected 25 条、BACKLOG 40 行。到顶必须先退役/合并；lint 超限阻断。
- 每条 taxonomy 必须有“锁定”（回归测试/编译期断言/脚本）；没有检查的教训不得入表。
- 每条 rejected 必须有可观测的重启条件；否则应删除而不是记录。
- AGENTS/spec/skill 达到预算 95% 时 lint 报警；达到 100% 阻断。
- 单次事件不升级；只有复发 ≥2、曾致 P0/P1 或改变不变量才升级为规则。

## 3. 尾注映射

| 值 | 暂存文件中必须包含 |
|----|------------------|
| `spec` | `docs/specs/` 或 `docs/adr/` |
| `taxonomy` | `docs/specs/defect-taxonomy.md` |
| `rejected` | `docs/specs/rejected-decisions.md` |
| `backlog` | `docs/BACKLOG.md` |
| `changelog` | `CHANGELOG.md` |
| `knowledge` | `AGENTS.md` 或 `docs/knowledge/` |
| `none` | 无（显式声明本次改动无知识影响） |

## 4. 安装

```bash
cp docs/knowledge/tools/pre-commit .git/hooks/pre-commit && chmod +x .git/hooks/pre-commit
cp docs/knowledge/tools/commit-msg .git/hooks/commit-msg && chmod +x .git/hooks/commit-msg
```

## 5. 反例

- 把当轮修复细节写进 AGENTS → 应进 CHANGELOG，AGENTS 只留规则。
- 新教训只写一段散文、没有测试/断言 → 不入 taxonomy。
- 同一事实在 spec、README、skill 各写一遍 → 只留一个权威，其余放链接。
- 为了“以后可能有用”新增条目 → 预算不允许；当下没有归宿就删除。
