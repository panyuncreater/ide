#!/usr/bin/env python3
"""
VM opcode profiling 自动化脚本。

构建 MINILANG_VM_PROFILING=ON 版本，运行 samples/ 下基准程序，
解析 opcode 执行计数并输出 top-20 热路径报告。

用法:
    python scripts/vm_profile_report.py [--samples-dir samples/mini] [--top 20]

前置条件:
    - CMake 已配置（windows-msvc-debug preset 或手动配置）
    - 构建工具可用（Ninja / MSBuild）

输出:
    标准输出打印 opcode 热路径排名表。
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def find_project_root() -> Path:
    """查找项目根目录（包含 CMakeLists.txt 的最近祖先）。"""
    current = Path(__file__).resolve().parent
    while current != current.parent:
        if (current / "CMakeLists.txt").exists() and (current / "compiler").exists():
            return current
        current = current.parent
    print("ERROR: 无法找到项目根目录", file=sys.stderr)
    sys.exit(1)


def configure_and_build(root: Path, build_dir: Path) -> None:
    """配置并构建带 VM profiling 的版本。"""
    print(f"[1/3] 配置 CMake (MINILANG_VM_PROFILING=ON) -> {build_dir}")
    
    configure_cmd = [
        "cmake", "-S", str(root), "-B", str(build_dir),
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DMINILANG_VM_PROFILING=ON",
        "-DMINILANG_BUILD_TESTS=OFF",
        "-DMINILANG_BUILD_CLI=ON",
        "-DMINILANG_BUILD_TEST_HARNESS=OFF",
    ]
    
    # Windows: 需要 QTDIR
    if sys.platform == "win32":
        qt_dir = os.environ.get("QTDIR", "")
        if qt_dir:
            configure_cmd.append(f"-DCMAKE_PREFIX_PATH={qt_dir}")
    
    result = subprocess.run(configure_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"CMake 配置失败:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    
    print(f"[2/3] 构建项目...")
    build_cmd = ["cmake", "--build", str(build_dir), "--parallel"]
    result = subprocess.run(build_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"构建失败:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)


def find_cli_executable(build_dir: Path) -> Path:
    """查找 CLI 可执行文件。"""
    candidates = [
        build_dir / "cli" / "minilang.exe",
        build_dir / "cli" / "minilang",
        build_dir / "minilang.exe",
        build_dir / "minilang",
    ]
    for p in candidates:
        if p.exists():
            return p
    print("ERROR: 找不到 minilang CLI 可执行文件", file=sys.stderr)
    sys.exit(1)


def collect_samples(samples_dir: Path) -> list:
    """收集所有 .mini 样本文件。"""
    if not samples_dir.exists():
        print(f"WARNING: 样本目录不存在: {samples_dir}", file=sys.stderr)
        return []
    
    samples = []
    for f in sorted(samples_dir.rglob("*.mini")):
        samples.append(f)
    return samples


def run_and_collect_profile(cli_exe: Path, sample: Path) -> dict:
    """运行样本程序并收集 opcode profile 输出。
    
    VM profiling 模式下 CLI 会在 stderr 输出 JSON 格式的 opcode 计数。
    格式: {"opcode_profile": {"OP_ADD": 1234, ...}}
    """
    try:
        result = subprocess.run(
            [str(cli_exe), "compile", "--run", "--vm-profile", str(sample)],
            capture_output=True, text=True, timeout=30
        )
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT: {sample.name}", file=sys.stderr)
        return {}
    
    # 尝试从 stderr 解析 profile JSON
    for line in result.stderr.splitlines():
        line = line.strip()
        if line.startswith("{") and "opcode_profile" in line:
            try:
                data = json.loads(line)
                return data.get("opcode_profile", {})
            except json.JSONDecodeError:
                pass
    
    # 备选: 从 stdout 解析（某些模式可能输出到 stdout）
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("{") and "opcode_profile" in line:
            try:
                data = json.loads(line)
                return data.get("opcode_profile", {})
            except json.JSONDecodeError:
                pass
    
    return {}


def print_report(aggregated: dict, top_n: int) -> None:
    """打印 opcode 热路径报告。"""
    if not aggregated:
        print("\n[报告] 未收集到 opcode profile 数据。")
        print("  可能原因:")
        print("  1. CLI 未实现 --vm-profile 输出（需手动集成）")
        print("  2. 样本文件未触发 VM 执行路径")
        print("  3. 输出格式不匹配解析逻辑")
        return
    
    sorted_ops = sorted(aggregated.items(), key=lambda x: x[1], reverse=True)
    total = sum(aggregated.values())
    
    print(f"\n{'='*60}")
    print(f" VM Opcode 热路径报告 (Top-{top_n})")
    print(f" 总执行指令数: {total:,}")
    print(f"{'='*60}")
    print(f"{'排名':<4} {'Opcode':<28} {'执行次数':<15} {'占比':<8}")
    print(f"{'-'*60}")
    
    for i, (opcode, count) in enumerate(sorted_ops[:top_n], 1):
        pct = count / total * 100 if total > 0 else 0
        bar = "█" * int(pct / 2)
        print(f"{i:<4} {opcode:<28} {count:<15,} {pct:>5.1f}% {bar}")
    
    print(f"{'='*60}")
    
    # 冷路径分析
    cold_ops = [op for op, cnt in sorted_ops if cnt == 0]
    if cold_ops:
        print(f"\n未触发的 Opcode ({len(cold_ops)} 个):")
        for op in cold_ops[:10]:
            print(f"  - {op}")
        if len(cold_ops) > 10:
            print(f"  ... 及其他 {len(cold_ops) - 10} 个")


def main():
    parser = argparse.ArgumentParser(description="VM opcode profiling 自动化报告")
    parser.add_argument("--samples-dir", type=str, default=None,
                        help="样本文件目录 (默认: <project>/samples/mini)")
    parser.add_argument("--top", type=int, default=20,
                        help="显示 top N 热路径 (默认: 20)")
    parser.add_argument("--skip-build", action="store_true",
                        help="跳过构建步骤（使用已有构建产物）")
    parser.add_argument("--build-dir", type=str, default=None,
                        help="构建输出目录 (默认: <project>/out/build/vm-profile)")
    args = parser.parse_args()
    
    root = find_project_root()
    build_dir = Path(args.build_dir) if args.build_dir else root / "out" / "build" / "vm-profile"
    samples_dir = Path(args.samples_dir) if args.samples_dir else root / "samples" / "mini"
    
    # Step 1: 构建
    if not args.skip_build:
        configure_and_build(root, build_dir)
    else:
        print("[跳过构建] 使用已有构建产物")
    
    # Step 2: 收集样本
    samples = collect_samples(samples_dir)
    if not samples:
        print(f"未找到样本文件 (目录: {samples_dir})")
        sys.exit(1)
    print(f"[3/3] 运行 {len(samples)} 个样本文件...")
    
    # Step 3: 运行并聚合
    cli_exe = find_cli_executable(build_dir)
    aggregated = {}
    
    for sample in samples:
        print(f"  运行: {sample.relative_to(root)}", end="")
        profile = run_and_collect_profile(cli_exe, sample)
        if profile:
            print(f" -> {sum(profile.values()):,} ops")
            for op, count in profile.items():
                aggregated[op] = aggregated.get(op, 0) + count
        else:
            print(" -> (no profile)")
    
    # Step 4: 输出报告
    print_report(aggregated, args.top)


if __name__ == "__main__":
    main()
