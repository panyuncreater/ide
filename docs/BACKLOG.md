# BACKLOG（唯一活跃工作清单）

> 规则：本文件是**唯一**开放项清单；审计报告只引用 ID，不再维护“剩余未修”列表。
> 状态：`open`（待做）/ `blocked`（外部阻塞）/ `verifying`（待证实）/ `candidate`（优化候选，未立项）。
> 关闭时删除本行并在 CHANGELOG 记录；已否决方案见 `docs/specs/rejected-decisions.md`。
> 轮末检查：超过 5 轮未更新的 `open/verifying` 行必须重新定级或关闭。

## 1. 缺陷与阻塞

- **B-009** `open` / P1：macOS 重放平台 RegisterVM 在挂起窗口闭包 upvalue 场景报
  `closeUpvaluesFrom: upvalue 指向已弹出的帧`（CI `CoroutineSuspension.SuspendedWindowClosureUpvalueSemantics`）。
  测试已按 `MINILANG_CORO_FIBER` 门控（该语义仅真挂起平台成立）；下一步在 macOS 重放路径定位
  `owningFrameIdx` 与回放帧生命周期。
- 2026-10-24 缺陷轮的 P0–P2 已清偿（见 `docs/audit/2026-10-24-defects.md`）；B-009 为跨平台 CI
  新暴露的存量缺陷。

## 2. 优化候选

当前无开放优化候选（2026-10-23 清偿全部 17 项，见 CHANGELOG 与 `docs/audit/2026-10-24.md`；性能路径不变量见 `docs/specs/backend-consistency.md` §10 与 `docs/specs/memory-model.md` §4；未实施的 OPT-18 B4/B5 见 `docs/specs/rejected-decisions.md` R-19/R-20）。
