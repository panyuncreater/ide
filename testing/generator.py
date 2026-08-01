#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""testing/generator.py — MiniLang 确定性随机程序生成器（阶段二）

严格模仿 Csmith 思想：只生成「语义确定、无未定义行为、无运行时错误」的合法
MiniLang 程序，程序末尾输出一个全局状态校验和（print），作为四后端差分比对依据。

核心不变量（保证可复现 + 无运行时错误 + 无 float 提升）：
  - 所有整型变量恒被规范到 [0, MOD)（赋值处 ((expr) % MOD + MOD) % MOD）。
  - 乘法仅「变量 × 小常量(<1000)」，加减仅作用于 [0,MOD) 原子，
    故任一中间值 < MOD*1000 ≈ 1e9 < 2^48，绝不溢出提升为 float。
  - 除法/取模的除数恒为正的小常量（1..小上限），绝不除零。
  - 逻辑 and/or/not 只作用于比较结果（布尔），规避「返回操作数原值」的语义。
  - 只在顶层声明 fun（不在块/循环内），规避已知的块内命名函数差异（发现 1）。
  - 只用内建函数 len()/sum()（不用 .remove()/.contains() 方法），规避方法覆盖差异（发现 2）。
  - 字典（dict）不变量：字面量值恒为规范化 int，键为确定字符串键集合；索引读/values 索引
    仅用存活键（生成器跟踪 set/remove 后的键存活集合）；get 恒带默认值；has/contains 的键
    存在性由生成器确定。故 dict 操作绝不触发键缺失/类型错误。

可复现：同一 seed 必生成逐字节相同的程序（仅用 random.Random(seed)）。
可配置：程序规模、嵌套深度、特性开关（数组/字典/函数/循环）。

无第三方依赖，兼容 Python 3.8+。可被 diff_test.py 作为库导入（generate/Config）。
"""

import argparse
import random
import sys
from dataclasses import dataclass, field

MOD = 1000003          # 大素数：变量值域上界（保证 < 2^48）
CS_MULT = 31           # 校验和滚动乘子
SMALL_MAX = 997        # 乘法/除法/取模用小常量上界（保证乘积 < 2^48）


@dataclass
class Config:
    """生成配置（特性开关 + 规模）。"""
    seed: int = 0
    num_globals: int = 6          # 全局整型变量数
    num_statements: int = 25      # 顶层语句数（不含变量声明与末尾 print）
    max_depth: int = 3            # 控制流最大嵌套深度
    enable_loops: bool = True     # if/while/for
    enable_functions: bool = True # 顶层函数定义与调用
    enable_arrays: bool = True    # 数组字面量 + 索引 + len/sum
    enable_dicts: bool = True     # 字典字面量 + 索引 + has/len/values/get
    max_loop_iters: int = 12      # 循环最大迭代次数（确定性上界）


class _Gen:
    """单次生成的可变状态机（一个实例对应一个 seed）。"""

    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.rng = random.Random(cfg.seed)
        self.lines = []
        self.indent = 0
        self.vars = []            # 当前可见的整型变量名
        self.arrays = []          # 当前可见的数组变量名
        self.dicts = []           # 当前可见的字典变量名
        self.dict_keys = {}       # 字典名 → 存活键集合（set/remove 后跟踪）
        self.funcs = []           # 已定义顶层函数 (name, arity)
        self._tmp = 0
        self._loopvar = 0
        self._in_if = 0           # if 块嵌套计数（if 块内不新增键/不移除键）

    # ---- 输出辅助 ----
    def emit(self, s):
        self.lines.append("    " * self.indent + s)

    def fresh_var(self, prefix="g"):
        name = f"{prefix}{len(self.vars) + len(self.arrays) + self._tmp}"
        self._tmp += 1
        return name

    # ---- 原子与表达式（恒定界、无副作用、无运行时错误）----
    def small_const(self):
        return self.rng.randint(0, SMALL_MAX)

    def nonzero_const(self):
        return self.rng.randint(1, SMALL_MAX)

    def atom(self):
        """一个 [0,MOD) 整型原子：变量或小常量。"""
        if self.vars and self.rng.random() < 0.65:
            return self.rng.choice(self.vars)
        return str(self.small_const())

    def int_expr(self, depth=0):
        """生成一个整型表达式；结果可能超出 [0,MOD)，调用方须规范化。

        为防溢出提升为 float：乘法仅『原子 × 小常量』，加减仅作用于原子/浅层，
        除法/取模除数恒为正小常量。
        """
        # 数组读取（len/sum/索引）作为整型来源
        if self.cfg.enable_arrays and self.arrays and self.rng.random() < 0.18:
            arr = self.rng.choice(self.arrays)
            choice = self.rng.random()
            if choice < 0.4:
                return f"len({arr})"
            if choice < 0.8:
                return f"sum({arr})"
            # 安全索引：len(arr) 恒 >= 1（生成时保证），用 (const % len) 防越界
            idx = self.small_const()
            return f"{arr}[{idx} % len({arr})]"
        # 函数调用作为整型来源
        if self.cfg.enable_functions and self.funcs and self.rng.random() < 0.18:
            name, arity = self.rng.choice(self.funcs)
            args = ", ".join(self.atom() for _ in range(arity))
            return f"{name}({args})"
        # 字典读取（存在键索引 / len / values 索引 / get 带默认值）作为整型来源
        if self.cfg.enable_dicts and self.dicts and self.rng.random() < 0.15:
            return self.dict_int()
        if depth >= 2 or self.rng.random() < 0.45:
            return self.atom()
        op = self.rng.choice(["+", "-", "*", "/", "%"])
        if op == "*":
            return f"({self.atom()} * {self.small_const()})"
        if op in ("/", "%"):
            return f"({self.atom()} {op} {self.nonzero_const()})"
        return f"({self.atom()} {op} {self.atom()})"

    def dict_int(self):
        """一个恒安全的 int 表达式：存在键索引读 / len / values 索引 / get 带默认值。

        不变量：dict 字面量值恒为 [0,MOD) 的 int，键集合由生成器跟踪；
        values() 恒非空（remove 保留至少 1 键）；get 恒带 int 默认值。
        """
        d = self.rng.choice(self.dicts)
        keys = sorted(self.dict_keys[d])
        if not keys:
            return f"len({d})"
        k = self.rng.choice(keys)
        choice = self.rng.random()
        if choice < 0.4:
            return f"{d}[{k!r}]"            # 存在键索引读
        if choice < 0.6:
            return f"len({d})"
        if choice < 0.8:
            return f"({d}.values()[{self.small_const()} % len({d}.values())])"
        return f"{d}.get({k!r}, {self.small_const()})"  # 带默认值恒安全

    def normalized(self, expr):
        """把任意整型表达式规范化回 [0, MOD)。"""
        return f"(({expr}) % {MOD} + {MOD}) % {MOD}"

    def bool_expr(self, depth=0):
        """布尔表达式：比较/字典 has 之上叠加 and/or/not（操作数恒为布尔）。"""
        cmp = self.rng.choice(["<", ">", "<=", ">=", "==", "!="])
        base = f"{self.atom()} {cmp} {self.atom()}"
        # 字典 has/contains：存在性由生成器确定（存活键 → true；absent 键 → false）
        if self.cfg.enable_dicts and self.dicts and self.rng.random() < 0.25:
            d = self.rng.choice(self.dicts)
            if self.rng.random() < 0.7 and self.dict_keys[d]:
                k = self.rng.choice(sorted(self.dict_keys[d]))
            else:
                k = self._absent_key(d)
            meth = self.rng.choice(["has", "contains"])
            base = f"{d}.{meth}({k!r})"
        if depth < self.cfg.max_depth and self.rng.random() < 0.35:
            conn = self.rng.choice(["and", "or"])
            rhs_cmp = self.rng.choice(["<", ">", "<=", ">=", "==", "!="])
            rhs = f"{self.atom()} {rhs_cmp} {self.atom()}"
            base = f"({base}) {conn} ({rhs})"
        if self.rng.random() < 0.15:
            base = f"not ({base})"
        return base

    def _absent_key(self, d):
        """生成一个确定不在 d 中的键名（试探随机键名直至不碰撞）。"""
        for _ in range(8):
            k = f"absent{self.rng.randint(0, 1 << 30)}"
            if k not in self.dict_keys[d]:
                return k
        return "zz_absent_key"

    # ---- 校验和折叠 ----
    def mix(self, expr):
        self.emit(f"cs = (cs * {CS_MULT} + {self.normalized(expr)}) % {MOD};")

    # ---- 语句 ----
    def stmt_assign(self):
        v = self.rng.choice(self.vars)
        self.emit(f"{v} = {self.normalized(self.int_expr())};")
        self.mix(v)

    def stmt_if(self, depth):
        self.emit(f"if ({self.bool_expr(depth)}) {{")
        self.indent += 1
        self._in_if += 1
        self.block_body(depth + 1)
        self._in_if -= 1
        self.indent -= 1
        if self.rng.random() < 0.5:
            self.emit("} else {")
            self.indent += 1
            self._in_if += 1
            self.block_body(depth + 1)
            self._in_if -= 1
            self.indent -= 1
        self.emit("}")

    def stmt_while(self, depth):
        self._loopvar += 1
        lv = f"w{self._loopvar}"
        iters = self.rng.randint(1, self.cfg.max_loop_iters)
        self.emit(f"var {lv} = 0;")
        self.emit(f"while ({lv} < {iters}) {{")
        self.indent += 1
        self.emit(f"{lv} = {lv} + 1;")
        self.block_body(depth + 1)
        self.indent -= 1
        self.emit("}")

    def stmt_for(self, depth):
        self._loopvar += 1
        lv = f"i{self._loopvar}"
        iters = self.rng.randint(1, self.cfg.max_loop_iters)
        self.emit(f"for (var {lv} = 0; {lv} < {iters}; {lv} = {lv} + 1) {{")
        self.indent += 1
        # 循环变量折入校验和，制造与循环计数相关的可观测状态
        self.mix(lv)
        # 偶发 break/continue（受布尔条件控制，仍确定性）
        if self.rng.random() < 0.25:
            self.emit(f"if ({lv} == {self.rng.randint(0, iters)}) {{ continue; }}")
        self.block_body(depth + 1)
        self.indent -= 1
        self.emit("}")

    # ---- 字典语句（set 新增键 / 索引写 / remove 移除键，均跟踪存活集合）----
    # 执行上下文约束（保证运行时键集合 == 生成器跟踪集合）：
    #   - remove 仅顶层直接语句（depth==0，无条件执行恰一次）——if/循环体内条件
    #     或重复执行会导致重复 remove / 键数跌破静态集合（values() 空索引越界）。
    #   - set 新增键仅顶层或循环体（必然执行至少一次）；if 块内仅允许覆盖存活键
    #     （if 可能不执行，新增键将悬空）。
    def stmt_dict_set(self):
        d = self.rng.choice(self.dicts)
        keys = sorted(self.dict_keys[d])
        # 50% 覆盖存活键（任意上下文安全），50% 新增键（仅必然执行上下文）
        if keys and self.rng.random() < 0.5:
            k = self.rng.choice(keys)
        else:
            if self._in_if > 0:
                return  # if 块内不新增键（可能不执行 → 后续索引悬空）
            k = f"k{len(keys)}"
            tries = 0
            while k in self.dict_keys[d] and tries < 8:
                tries += 1
                k = f"k{len(keys) + tries}"
            if k in self.dict_keys[d]:
                return  # 理论不可达：碰撞规避失败则放弃
        self.emit(f"{d}[{k!r}] = {self.normalized(self.int_expr())};")
        self.dict_keys[d].add(k)
        self.mix(f"{d}[{k!r}]")

    def stmt_dict_remove(self, depth):
        # 仅顶层直接语句（无条件执行恰一次）；循环/if 块内禁止
        if depth != 0:
            return
        d = self.rng.choice(self.dicts)
        keys = sorted(self.dict_keys[d])
        # 不变量：永远保留至少 1 键（values() 恒非空）
        if len(keys) <= 1:
            return
        k = self.rng.choice(keys)
        self.emit(f"{d}.remove({k!r});")
        self.dict_keys[d].discard(k)
        self.mix(f"len({d})")

    def block_body(self, depth):
        """块体：1-3 条语句。"""
        k = self.rng.randint(1, 3)
        for _ in range(k):
            self.gen_stmt(depth)

    def gen_stmt(self, depth):
        choices = ["assign", "assign", "mix"]
        if self.cfg.enable_loops and depth < self.cfg.max_depth:
            choices += ["if", "while", "for"]
        if self.cfg.enable_dicts and self.dicts:
            choices += ["dict_set", "dict_remove"]
        kind = self.rng.choice(choices)
        if kind == "assign":
            self.stmt_assign()
        elif kind == "mix":
            self.mix(self.int_expr())
        elif kind == "if":
            self.stmt_if(depth)
        elif kind == "while":
            self.stmt_while(depth)
        elif kind == "for":
            self.stmt_for(depth)
        elif kind == "dict_set":
            self.stmt_dict_set()
        elif kind == "dict_remove":
            self.stmt_dict_remove(depth)

    # ---- 顶层函数定义（纯整型、无副作用、必返回 [0,MOD)）----
    def gen_function(self):
        name = f"fn{len(self.funcs)}"
        arity = self.rng.randint(1, 3)
        params = [f"p{i}" for i in range(arity)]
        self.emit(f"fun {name}({', '.join(params)}) {{")
        self.indent += 1
        # 仅用参数与常量构造返回值（不引用全局，避免顺序依赖）
        saved_vars = self.vars
        self.vars = params[:]
        self.emit(f"var acc = {self.normalized(self.int_expr())};")
        self.vars = params + ["acc"]
        for _ in range(self.rng.randint(1, 3)):
            self.emit(f"acc = {self.normalized(self.int_expr())};")
        self.emit("return acc;")
        self.vars = saved_vars
        self.indent -= 1
        self.emit("}")
        self.funcs.append((name, arity))

    # ---- 顶层生成 ----
    def gen_dict(self):
        """生成一个全局字典：{'k0': <int>, 'k1': <int>, ...}（值规范化，键集合确定）。"""
        name = f"d{len(self.dicts)}"
        n = self.rng.randint(1, 3)
        keys = [f"k{i}" for i in range(n)]
        entries = ", ".join(f"{k!r}: {self.rng.randint(0, MOD - 1)}" for k in keys)
        self.emit(f"var {name} = {{{entries}}};")
        self.dicts.append(name)
        self.dict_keys[name] = set(keys)

    def generate(self):
        cfg = self.cfg
        self.emit(f"// auto-generated by generator.py  seed={cfg.seed}")
        self.emit("// deterministic; no runtime error by construction; ends with print(cs)")
        self.emit("var cs = 0;")

        # 顶层函数
        if cfg.enable_functions:
            for _ in range(self.rng.randint(1, 3)):
                self.gen_function()

        # 全局整型变量
        for _ in range(cfg.num_globals):
            v = f"g{len(self.vars)}"
            self.emit(f"var {v} = {self.rng.randint(0, MOD - 1)};")
            self.vars.append(v)

        # 数组（保证非空 → 索引/len 安全）
        if cfg.enable_arrays:
            for _ in range(self.rng.randint(1, 2)):
                a = f"arr{len(self.arrays)}"
                n = self.rng.randint(1, 5)
                elems = ", ".join(str(self.rng.randint(0, MOD - 1)) for _ in range(n))
                self.emit(f"var {a} = [{elems}];")
                self.arrays.append(a)

        # 字典（值恒为规范化 int，键集合确定 → 索引/has/values 安全）
        if cfg.enable_dicts:
            for _ in range(self.rng.randint(1, 2)):
                self.gen_dict()

        # 主体语句
        for _ in range(cfg.num_statements):
            self.gen_stmt(0)

        # 末尾校验和输出（差分 oracle）
        self.emit("print(cs);")
        return "\n".join(self.lines) + "\n"


def generate(seed, config=None):
    """生成一个 MiniLang 程序源码字符串（确定性）。"""
    cfg = config or Config()
    cfg.seed = seed
    return _Gen(cfg).generate()


# ============================================================
# CLI
# ============================================================
def build_parser():
    p = argparse.ArgumentParser(prog="generator.py",
                                description="MiniLang 确定性随机程序生成器（阶段二）")
    p.add_argument("--seed", type=int, required=True, help="随机种子（同种子生成相同程序）")
    p.add_argument("--statements", type=int, default=25, help="顶层语句数（默认 25）")
    p.add_argument("--globals", type=int, default=6, help="全局整型变量数（默认 6）")
    p.add_argument("--max-depth", type=int, default=3, help="控制流嵌套深度（默认 3）")
    p.add_argument("--max-loop-iters", type=int, default=12, help="循环最大迭代（默认 12）")
    p.add_argument("--no-loops", action="store_true", help="禁用 if/while/for")
    p.add_argument("--no-functions", action="store_true", help="禁用顶层函数")
    p.add_argument("--no-arrays", action="store_true", help="禁用数组")
    p.add_argument("--no-dicts", action="store_true", help="禁用字典")
    p.add_argument("--out", default=None, help="输出文件（默认 stdout）")
    return p


def config_from_args(args):
    return Config(
        seed=args.seed,
        num_globals=args.globals,
        num_statements=args.statements,
        max_depth=args.max_depth,
        max_loop_iters=args.max_loop_iters,
        enable_loops=not args.no_loops,
        enable_functions=not args.no_functions,
        enable_arrays=not args.no_arrays,
        enable_dicts=not args.no_dicts,
    )


def main(argv=None):
    args = build_parser().parse_args(argv)
    src = generate(args.seed, config_from_args(args))
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(src)
    else:
        sys.stdout.write(src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
