#!/usr/bin/env python3
"""analysis/parse_remarks.py — turn clang -Rpass* output into a per-loop table.

Usage: parse_remarks.py <source.c> <clang-remarks.txt> [icx.optrpt] [--vecpass <remarks>]

clang prints, per loop, zero or more -Rpass-analysis lines (the reason, located
at the offending instruction) followed by one loop-level line located at the
loop's `for`:  "remark: vectorized loop ..." or "remark: loop not vectorized".
We key every loop by that `for` line, which is also what icx's opt-report uses
("LOOP BEGIN at file.c (LINE, COL)"), so the two compilers can be matched.

TSVC wraps every kernel in an outer timing loop `for (int nl = ...)` that
calls dummy(); those are never vectorizable and are dropped from the table.
Output is Markdown (pasted into analysis/missed.md).
"""
import re
import sys
from collections import defaultdict

args = sys.argv[1:]
vp_path = None
if "--vecpass" in args:
    i = args.index("--vecpass")
    vp_path = args[i + 1]
    del args[i:i + 2]
src_path, remarks_path = args[0], args[1]
icx_path = args[2] if len(args) > 2 else None

src = open(src_path, errors="replace").read().splitlines()

# Enclosing function for each line: TSVC kernels start with "real_t sNNN(".
func_at, current = {}, "?"
for n, line in enumerate(src, 1):
    m = re.match(r"^\w[\w \*]*?\b(\w+)\s*\(", line)
    if m and not line.rstrip().endswith(";"):
        current = m.group(1)
    func_at[n] = current

loc_re = re.compile(r"^\S+?:(\d+):\d+: remark: (.*?)(?: \[-Rpass[^\]]*\])?$")


def parse(path):
    """Return ({for_line: 'vec'|'novec'}, {for_line: [reasons]})."""
    status, reasons, pending = {}, {}, []
    lines = open(path, errors="replace").read().splitlines()
    for i, line in enumerate(lines):
        m = loc_re.match(line)
        if not m:
            continue
        ln, msg = int(m.group(1)), m.group(2)
        if msg.startswith("vectorized loop"):
            status[ln] = "vec"
            pending = []
        elif msg == "loop not vectorized":
            status.setdefault(ln, "novec")
            reasons[ln] = pending
            pending = []
        elif msg.startswith("loop not vectorized:"):
            r = msg[len("loop not vectorized: "):].split(". Use #pragma")[0]
            nxt = lines[i + 1] if i + 1 < len(lines) else ""
            if nxt and not loc_re.match(nxt) and not nxt.lstrip()[:1].isdigit():
                r += " — " + re.sub(r" \[-Rpass[^\]]*\]$", "", nxt).split(". Memory")[0]
            pending.append(r)
    return status, reasons


def is_timing_loop(ln):
    return "nl <" in src[ln - 1] or "nl=" in src[ln - 1].replace(" ", "")


status, reasons = parse(remarks_path)
vp_status = parse(vp_path)[0] if vp_path else {}

icx_vec = set()
if icx_path:
    cur = None
    for line in open(icx_path, errors="replace"):
        m = re.search(r"LOOP BEGIN at .*?\((\d+), \d+\)", line)
        if m:
            cur = int(m.group(1))
        elif cur and "LOOP WAS VECTORIZED" in line:
            icx_vec.add(cur)

missed = [ln for ln, s in status.items() if s == "novec" and not is_timing_loop(ln)]
nvec = sum(1 for ln, s in status.items() if s == "vec" and not is_timing_loop(ln))
print(f"clang: {nvec} kernel loops vectorized, {len(missed)} kernel loops not "
      f"vectorized (outer `nl` timing loops excluded)\n")

groups = defaultdict(list)
for ln in missed:
    rs = reasons.get(ln) or ["(no analysis remark)"]
    groups[rs[-1]].append(ln)

hdr = "| function | loop line |"
sep = "|---|---|"
if icx_path:
    hdr += " icx 2025.0 vectorized? |"; sep += "---|"
if vp_path:
    hdr += " clang + vecpass vectorized? |"; sep += "---|"
for reason, lns in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
    print(f"### {reason} ({len(lns)})\n")
    print(hdr); print(sep)
    for ln in sorted(lns):
        row = f"| {func_at.get(ln, '?')} | {ln} |"
        if icx_path:
            row += " **yes** |" if ln in icx_vec else " no |"
        if vp_path:
            row += " **yes** |" if vp_status.get(ln) == "vec" else " no |"
        print(row)
    print()

if icx_path:
    both = [ln for ln in missed if ln in icx_vec]
    print(f"icx vectorizes {len(both)} of the {len(missed)} loops clang misses: "
          + ", ".join(func_at[l] for l in sorted(both)))
if vp_path:
    fixed = [ln for ln in missed if vp_status.get(ln) == "vec"]
    print(f"\nclang + vecpass vectorizes {len(fixed)} of the {len(missed)} loops "
          "clang misses: " + ", ".join(func_at[l] for l in sorted(fixed)))
