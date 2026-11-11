#!/usr/bin/env python3
"""bench/summarize.py — turn results/raw/<tag>/bench.csv (+ perf.csv) into
Markdown tables. Usage: summarize.py results/raw/<tag>

Speedup = baseline median / vecpass median (> 1 means vecpass is faster).
"chk" says whether the clang and clang+vecpass checksums are identical.
"""
import csv
import os
import sys
from collections import defaultdict

d = sys.argv[1]
rows = list(csv.DictReader(open(os.path.join(d, "bench.csv"))))
med = {}
chk = {}
for r in rows:
    key = (r["isa"], r["kernel"], int(r["n"]))
    med[key + (r["compiler"],)] = float(r["median_ns_per_elem"])
    chk[key + (r["compiler"],)] = r["checksum"]

isas = sorted({r["isa"] for r in rows}, key=["avx2", "spr", "avx512"].index)
sizes = sorted({int(r["n"]) for r in rows})
kernels = list(dict.fromkeys(r["kernel"] for r in rows))
comps = list(dict.fromkeys(r["compiler"] for r in rows))
label = {1024: "L1-resident", 65536: "L2-resident", 16777216: "memory (> L3)"}

mismatch = []
for n in sizes:
    print(f"\n### n = {n} ({label.get(n, '')}), median ns per element (lower is better)\n")
    hdr = "| kernel |"
    sep = "|---|"
    for isa in isas:
        for c in comps:
            hdr += f" {isa} {c} |"; sep += "---:|"
        hdr += f" {isa} speedup |"; sep += "---:|"
    print(hdr); print(sep)
    for k in kernels:
        line = f"| {k} |"
        for isa in isas:
            for c in comps:
                v = med.get((isa, k, n, c))
                line += f" {v:.3f} |" if v is not None else " – |"
            b, v = med.get((isa, k, n, "base")), med.get((isa, k, n, "vecpass"))
            line += f" **{b / v:.2f}x** |" if b and v else " – |"
            cb, cv = chk.get((isa, k, n, "base")), chk.get((isa, k, n, "vecpass"))
            if cb is not None and cv is not None and cb != cv:
                mismatch.append((isa, k, n, cb, cv))
        print(line)

print("\nChecksum check (clang vs clang+vecpass): " +
      ("all identical" if not mismatch else f"{len(mismatch)} MISMATCHES: {mismatch}"))

pf = os.path.join(d, "perf.csv")
if os.path.exists(pf):
    p = defaultdict(dict)
    for r in csv.DictReader(open(pf)):
        try:
            p[(r["isa"], r["kernel"])][(r["compiler"], r["event"].split(":")[0])] = float(r["value"])
        except ValueError:
            pass
    print("\n### perf stat, n = 65536, fixed work (8 x 2000 calls incl. warm-up), whole process\n")
    print("| isa | kernel | base cycles | vecpass cycles | base instr | vecpass instr | base IPC | vecpass IPC |")
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for (isa, k), ev in p.items():
        try:
            bc, vc = ev[("base", "cycles")], ev[("vecpass", "cycles")]
            bi, vi = ev[("base", "instructions")], ev[("vecpass", "instructions")]
        except KeyError:
            continue
        print(f"| {isa} | {k} | {bc:.3e} | {vc:.3e} | {bi:.3e} | {vi:.3e} | {bi/bc:.2f} | {vi/vc:.2f} |")
    print("\nBoth builds run identical work (fixed reps), so cycle and instruction"
          " counts compare directly; they include init() and harness overhead.")
