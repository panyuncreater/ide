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
- **B-012** `verifying` / P2：JIT 操作数栈仍是借用模型（push 不 addRef、pop 不 release），局部槽/容器
  元素替换等路径的借用悬垂风险未消除。2026-10-25 已将全局/局部槽覆盖、删除、`OP_WRITEBACK_*` 的旧堆值
  统一改为 `retiredValues_` 延迟释放（下次 execute 清空 PIC 后释放，并作为 GC 根），消除执行期全局槽
  泄漏与 PIC 地址复用假命中（B-010/B-013），并增同 backend 多次 execute 回归。重启条件：容器元素替换/
  局部槽借用逃逸复现 UAF，或 JIT 内存压测出现执行内堆单调增长。

## 2. 优化候选

2026-10-23 的 17 项已清偿（见 CHANGELOG 与 `docs/audit/2026-10-24.md`）；2026-10-25 性能轮
清偿 15 项（解释器 4 / VM-JIT 4 / 编译器 1 / 前端 3 / GUI-格式化 3）；同日第二轮清偿
OPT-19~22（见 CHANGELOG 与 `docs/audit/2026-10-25-optimizations.md`）。当前无未立项候选。

性能路径不变量见 `docs/specs/backend-consistency.md` §10 与 `docs/specs/memory-model.md` §4；
未实施的 OPT-18 B4/B5 见 `docs/specs/rejected-decisions.md` R-19/R-20。
JIT 基准对机器码布局敏感（同义发射序列变化可致数倍漂移），A/B 必须同轮交错测量并核对输出，
禁止单批次先后对比（2026-10-25 归因实验，详见审计报告）。
