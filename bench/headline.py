#!/usr/bin/env python3
"""bench/headline.py <results/raw/tag> — compact speedup tables for RESULTS.md:
vecpass speedup over clang -O3 (base median / vecpass median), and icx vs base."""
import csv, sys
d = sys.argv[1]
med = {}
for r in csv.DictReader(open(f"{d}/bench.csv")):
    med[(r["layout"], r["isa"], r["kernel"], int(r["n"]), r["compiler"])] = float(r["median_ns_per_elem"])
K = ["s212", "s1213", "s211", "s241", "s1244", "s261", "s244", "s116", "fwd1213", "s000", "s321", "off_k1"]
for lay, desc in [("p200", "arrays spread over the 4 KiB window"), ("p8", "arrays within 128 B mod 4096")]:
    print(f"\n#### Layout {lay} ({desc}): speedup of clang+vecpass over clang -O3\n")
    cols = [(n, i) for n in (1024, 65536, 16777216) for i in ("avx2", "spr", "avx512")]
    nm = {1024: "L1", 65536: "L2", 16777216: "mem"}
    print("| kernel | " + " | ".join(f"{nm[n]} {i}" for n, i in cols) + " |")
    print("|---|" + "---:|" * len(cols))
    for k in K:
        cells = []
        for n, i in cols:
            b, v = med[(lay, i, k, n, "base")], med[(lay, i, k, n, "vecpass")]
            s = b / v
            cells.append(f"**{s:.2f}x**" if s >= 1.05 else (f"_{s:.2f}x_" if s <= 0.95 else f"{s:.2f}x"))
        print(f"| {k} | " + " | ".join(cells) + " |")
