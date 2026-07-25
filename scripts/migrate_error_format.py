#!/usr/bin/env python3
"""
L25: Migrate ErrorFormat::format() (printf-style) -> ErrorFormat::formatStd() (std::format-style).

Strategy:
  1. Find each `ErrorFormat::format(...)` call by scanning with paren-depth tracking.
  2. Split args at top-level commas (depth 0 within the call).
  3. The first arg is the format string (literal "..." or ErrorMessages::kXFmt constant).
  4. Replace %d / %s / %zu / %lld in string literals with {}.
  5. Replace ErrorMessages::kXFmt with ErrorMessages::kXFmtStd (only for known pairs).
  6. Strip trailing `.c_str()` from each remaining arg.
  7. Rename `format` -> `formatStd`.

Safety:
  - Skip if format spec is malformed or contains unsupported specifiers (%f, %x, %5d, etc).
  - Skip if the first arg is not a string literal AND not a known ErrorMessages constant.
  - CrashHandler.cpp is excluded (snprintf kept for async-signal-safety).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

# Known ErrorMessages::k*Fmt -> k*FmtStd mappings
FMT_TO_FMTSTD = {
    "ErrorMessages::kRecursionDepthExceededFmt": "ErrorMessages::kRecursionDepthExceededFmtStd",
    "ErrorMessages::kUndefinedFunctionFmt": "ErrorMessages::kUndefinedFunctionFmtStd",
    "ErrorMessages::kTypeAnnotationViolationFmt": "ErrorMessages::kTypeAnnotationViolationFmtStd",
}

# Only these printf specifiers are migrated; any other (e.g. %f, %x, %5.2f) -> skip the call
SUPPORTED_SPECS = {r"%d", r"%s", r"%zu", r"%lld"}
SPEC_PATTERN = re.compile(r"%(-?\+?#?0?)(\d+)?(\.\d+)?(lld|zu|[diouxXeEfgGsc])")


def find_format_calls(text: str):
    """Yield (start, end) byte offsets for each `ErrorFormat::format(...)` call, end exclusive of trailing RPAREN."""
    needle = "ErrorFormat::format("
    i = 0
    while True:
        idx = text.find(needle, i)
        if idx == -1:
            return
        # Walk from idx + len(needle) - 1 (the '(' position) tracking depth
        depth = 1
        j = idx + len(needle)
        in_string = False
        in_char = False
        escape = False
        while j < len(text) and depth > 0:
            ch = text[j]
            if in_string:
                if escape:
                    escape = False
                elif ch == "\\":
                    escape = True
                elif ch == '"':
                    in_string = False
            elif in_char:
                if escape:
                    escape = False
                elif ch == "\\":
                    escape = True
                elif ch == "'":
                    in_char = False
            else:
                if ch == '"':
                    in_string = True
                elif ch == "'":
                    in_char = True
                elif ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        break
            j += 1
        if depth != 0:
            # Unbalanced; skip
            return
        # Call spans [idx, j+1] (inclusive of trailing RPAREN)
        yield idx, j + 1
        i = j + 1


def split_top_level_args(body: str):
    """Split a function call body (between outer parens) into top-level args, respecting strings/parens."""
    args = []
    cur = []
    depth = 0
    in_string = False
    in_char = False
    escape = False
    for ch in body:
        if in_string:
            cur.append(ch)
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
        elif in_char:
            cur.append(ch)
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == "'":
                in_char = False
        else:
            if ch == '"':
                in_string = True
                cur.append(ch)
            elif ch == "'":
                in_char = True
                cur.append(ch)
            elif ch == "(":
                depth += 1
                cur.append(ch)
            elif ch == ")":
                depth -= 1
                cur.append(ch)
            elif ch == "," and depth == 0:
                args.append("".join(cur))
                cur = []
            else:
                cur.append(ch)
    if cur:
        args.append("".join(cur))
    return args


def migrate_format_string(s: str):
    """Return (migrated, ok). If ok=False, the format string contains an unsupported specifier."""
    out = []
    i = 0
    while i < len(s):
        m = SPEC_PATTERN.match(s, i)
        if m:
            spec = m.group(0)
            # Build a normalized key to check support
            normalized = spec
            # %lld, %d, %s, %zu are supported
            supported = False
            if spec == "%d" or spec == "%s" or spec == "%zu" or spec == "%lld":
                supported = True
            elif spec.startswith("%") and (m.group(4) == "lld" or m.group(4) == "zu" or m.group(4) in "ds"):
                # e.g. %5d, %-s, %3lld etc. We'll support plain (no width) only.
                supported = False
            if not supported:
                return None, False
            out.append("{}")
            i = m.end()
        else:
            out.append(s[i])
            i += 1
    return "".join(out), True


def extract_first_string_literal(arg_text: str):
    """If arg_text is a string literal (possibly wrapped in parens), return (start, literal, end). Else None."""
    s = arg_text.strip()
    # Strip surrounding parens that wrap the whole arg
    while s.startswith("(") and s.endswith(")"):
        # Verify balanced
        depth = 0
        ok = True
        for k, ch in enumerate(s):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and k != len(s) - 1:
                    ok = False
                    break
        if ok:
            s = s[1:-1].strip()
        else:
            break
    if s.startswith('"'):
        # Find the end of the string literal (handle escapes)
        i = 1
        while i < len(s):
            if s[i] == "\\":
                i += 2
            elif s[i] == '"':
                return s[: i + 1]
            else:
                i += 1
    return None


def migrate_call(call_text: str):
    """Migrate a single `ErrorFormat::format(...)` call. Returns migrated text, or None to skip."""
    # call_text = "ErrorFormat::format(" + body + ")"
    body_start = len("ErrorFormat::format(")
    body_end = len(call_text) - 1  # exclude trailing ')'
    body = call_text[body_start:body_end]
    args = split_top_level_args(body)
    if not args:
        return None
    first_arg = args[0].strip()

    # Determine if first arg is a string literal or an ErrorMessages constant
    is_string_literal = first_arg.startswith('"')
    is_em_const = first_arg in FMT_TO_FMTSTD
    if not (is_string_literal or is_em_const):
        return None  # Unknown first arg shape; skip

    # Migrate format string
    if is_string_literal:
        lit = extract_first_string_literal(first_arg)
        if lit is None:
            return None
        # lit is the literal including quotes
        # Strip the surrounding quotes
        inner = lit[1:-1]
        migrated_inner, ok = migrate_format_string(inner)
        if not ok:
            return None
        # Re-quote
        new_first = '"' + migrated_inner + '"'
        # Preserve any trailing whitespace/text after the literal in first_arg
        # (typically nothing; but handle it just in case)
        suffix = first_arg[len(lit) :]
        new_first_arg_text = new_first + suffix
    else:
        # ErrorMessages constant - just remap
        new_first_arg_text = FMT_TO_FMTSTD[first_arg]

    # Strip trailing .c_str() from each subsequent arg
    new_args = [new_first_arg_text]
    spec_count = (migrated_inner.count("{}") if is_string_literal else None)
    for a in args[1:]:
        s = a
        # Find trailing `.c_str()`
        # Match `.c_str()` at end, possibly followed by whitespace
        m = re.search(r"\.c_str\(\)\s*$", s.rstrip())
        if m:
            # Preserve trailing whitespace
            stripped = s[: m.start()]
            new_args.append(stripped + s[len(s.rstrip()):])
        else:
            new_args.append(s)

    # If the format string came from an ErrorMessages constant, we don't know
    # spec_count a priori; assume args align (they should).
    new_body = ", ".join(a if i == 0 else a for i, a in enumerate(new_args))
    # Preserve original arg separator style? Use ", " for simplicity; this changes
    # whitespace, but clang-format will normalize later.
    # Actually preserve original whitespace pattern by joining with "," (no space)
    # and let clang-format fix. But multi-line args would lose formatting.
    # Safer: preserve original separators. Let's reconstruct more carefully.

    # Reconstruct preserving original separators (commas + whitespace between args).
    # Find comma positions in original body at depth 0.
    sep_positions = []
    depth = 0
    in_string = False
    in_char = False
    escape = False
    for k, ch in enumerate(body):
        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
        elif in_char:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == "'":
                in_char = False
        else:
            if ch == '"':
                in_string = True
            elif ch == "'":
                in_char = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == "," and depth == 0:
                sep_positions.append(k)
    if len(sep_positions) != len(args) - 1:
        # Fallback to simple join
        new_body = ", ".join(new_args)
    else:
        # Reconstruct: arg0 + sep0 + arg1 + sep1 + arg2 ...
        new_body = new_args[0]
        for k, sp in enumerate(sep_positions):
            # sep is body[sp] = ",", and we need body[sp+1:next_arg_start] whitespace
            # The original separator is from sp to the start of the next arg.
            # We already have the next arg text (new_args[k+1]), so just use the
            # whitespace between "," and the next arg in the original.
            # Find the original next arg start (skip whitespace after comma).
            j = sp + 1
            while j < len(body) and body[j] in " \t\r\n":
                j += 1
            # The whitespace in original was body[sp+1:j]
            ws = body[sp + 1 : j]
            new_body += "," + ws + new_args[k + 1]

    return "ErrorFormat::formatStd(" + new_body + ")"


def migrate_file(path: Path, dry_run: bool = False):
    text = path.read_text(encoding="utf-8")
    # Find all calls (process from end to start so offsets stay valid)
    calls = list(find_format_calls(text))
    if not calls:
        return 0
    new_text = text
    delta = 0
    migrated = 0
    skipped = 0
    for start, end in calls:
        call_text = text[start:end]
        new_call = migrate_call(call_text)
        if new_call is None:
            skipped += 1
            continue
        # Apply on new_text with offset
        s = start + delta
        e = end + delta
        new_text = new_text[:s] + new_call + new_text[e:]
        delta += len(new_call) - (end - start)
        migrated += 1
    if migrated == 0:
        return 0
    if not dry_run:
        path.write_text(new_text, encoding="utf-8")
    return migrated


def main():
    files = [
        "compiler/RegisterVM.cpp",
        "compiler/VM.cpp",
        "interpreter/BuiltinMethods.cpp",
        "interpreter/InterpreterCalls.cpp",
        "interpreter/Interpreter.cpp",
        "compiler/VMCalls.cpp",
        "compiler/VMContainers.cpp",
        "compiler/JIT.cpp",
    ]
    root = Path(__file__).parent.parent
    total = 0
    for f in files:
        p = root / f
        if not p.exists():
            print(f"[skip] not found: {f}")
            continue
        n = migrate_file(p, dry_run=False)
        print(f"[ok] {f}: migrated {n} calls")
        total += n
    print(f"Total migrated: {total}")


if __name__ == "__main__":
    main()
