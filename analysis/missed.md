# Loops clang -O3 does not vectorize in TSVC_2

**What this is:** every TSVC_2 kernel loop that `clang -O3` leaves scalar,
grouped by the reason LoopVectorize gives, with a column for Intel icx and
one for clang + vecpass. Regenerate with `analysis/run_tsvc_analysis.sh`
(raw remark files are in `analysis/data/`).

**Setup (all measured, not assumed):**

| | |
|---|---|
| Source | TSVC_2 `src/tsvc.c`, commit `911fc639b933df38a7728477ce4b2e68e9c8eeff` |
| clang | 23.1.5 (conda-forge build; see `analysis/data/clang_version.txt`) |
| icx | Intel oneAPI DPC++/C++ 2025.0.0 (20241008), module `intel/oneAPI/2025.0` |
| clang flags | `-O3 -march=sapphirerapids -ffast-math -Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize` |
| icx flags | `-O3 -march=sapphirerapids -fp-model=fast -qopt-report=3 -qopt-report-phase=vec` |
| Excluded | the outer `for (int nl = ...)` timing loop around every kernel (it calls `dummy()`, so it is never vectorizable) |

`-ffast-math` / `-fp-model=fast` match TSVC's own "relaxed" build (its
Makefile builds `_default`, `_relaxed`, `_precise`); we analyzed the relaxed one.

## Summary

| reason class (clang 23 remark) | loops | icx vectorizes | vecpass fixes |
|---|---|---|---|
| Backward loop-carried dependence | 25 | 0 | **8** |
| Value not identified as a reduction (argmax, conditional scalars, packing) | 11 | 1 (s315) | 0 |
| Unknown data dependence | 3 | 0 | 0 |
| Unsafe indirect dependence (`a[idx[i]]`) | 2 | 1 (s1161) | 0 |
| Uncountable loop / early exit / array bounds | 3 | 0 | 0 |
| **Total** | **44** | **2** | **8** |

## What we learned from the table

1. **The brief's suggested candidate (runtime offset `a[i] = a[i+k] + b[i]`)
   is already handled by LLVM 23.** s162 (`a[i] = a[i+k] + b[i]*c[i]`, `k>0`) and
   s174 (`a[i+M] = a[i] + b[i]`) do not appear in the table at all: clang
   vectorizes them. Looking at the IR (`clang -O3 -S -emit-llvm` on a
   minimal `a[i] = a[i+k] + b[i]`), LoopAccessAnalysis emits a *dependence
   distance* check, `diff.check = icmp ugt i64 (src - sink), -128` (128 =
   VF 8 × UF 4 × 4 bytes), instead of a range-overlap check. That is exactly
   "loop versioning on a runtime check of k", done by LLVM itself, so
   writing that transform would duplicate existing work.
2. **The largest class, "Backward loop carried data dependence" (25 loops),
   splits into two kinds:**
   * *True recurrences* (s221, s222, s242, s321-s323, s2111, s231-s235,
     s256, s275, s126, ...): a value computed in iteration i is needed in
     iteration i+1. No legal reordering exists; these need other techniques
     (scan/prefix vectorization, interchange of 2-D loops) or are inherently
     serial.
   * *Order-only dependences* (s116, s211, s212, s1213, s241, s244, s1244,
     s261): the dependence is "backward" only because of the **order of the
     statements inside the loop body**. Example, s212:
     `a[i] *= c[i]; b[i] += a[i+1] * d[i];` reads `a[i+1]` after writing
     `a[i]`. In the same iteration those can never be the same address, so
     moving the load up is legal, and then the dependence is "forward".
     **Neither clang 23 nor icx 2025.0 vectorizes these** (icx reports
     "assumed ANTI dependence"). In an experiment with hand-reordered
     source (docs/DESIGN.md §2), icx vectorized all six reordered versions;
     clang vectorized three (s212, s241, s1244) and rejected the other three
     (s1213, s211, s261) with "Forward loop carried data dependence that
     prevents store-to-load forwarding". That second problem is why vecpass
     also runs LoopLoadElimination.
3. icx's only wins over clang are s315 (argmax with index, an idiom LLVM
   does not recognize as a reduction) and s1161 (indirect access). Neither is
   a "simple, safe IR transform".

**Chosen class:** order-only backward dependences, fixed by reordering the
loop body (vecpass). Result: **8 of the 44 missed loops** become vectorized
(TSVC total: 128 → 136 vectorized loop remarks, including remainder loops).
s116 is reordered and vectorized at `-march=sapphirerapids` (AVX-512) but
not at `-march=x86-64-v3` (AVX2), where the cost model rejects the stride-5
interleaved access groups.

## Full table (generated)

clang: 108 kernel loops vectorized, 44 kernel loops not vectorized (outer `nl` timing loops excluded)

### unsafe dependent memory operations in loop — Backward loop carried data dependence (25)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s116 | 274 | no | **yes** |
| s126 | 513 | no | no |
| s161 | 723 | no | no |
| s211 | 962 | no | **yes** |
| s212 | 985 | no | **yes** |
| s1213 | 1006 | no | **yes** |
| s221 | 1029 | no | no |
| s222 | 1071 | no | no |
| s231 | 1095 | no | no |
| s232 | 1119 | no | no |
| s233 | 1165 | no | no |
| s2233 | 1190 | no | no |
| s235 | 1217 | no | no |
| s241 | 1240 | no | **yes** |
| s242 | 1267 | no | no |
| s244 | 1313 | no | **yes** |
| s1244 | 1335 | no | **yes** |
| s256 | 1577 | no | no |
| s261 | 1653 | no | **yes** |
| s275 | 1782 | no | no |
| s277 | 1854 | no | no |
| s2111 | 2234 | no | no |
| s321 | 2687 | no | no |
| s322 | 2709 | no | no |
| s323 | 2731 | no | no |

### value that could not be identified as reduction is used outside the loop (11)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s2251 | 1425 | no | no |
| s258 | 1626 | no | no |
| s292 | 2140 | no | no |
| s315 | 2401 | **yes** | no |
| s318 | 2487 | no | no |
| s3110 | 2550 | no | no |
| s13110 | 2582 | no | no |
| s3112 | 2638 | no | no |
| s341 | 2820 | no | no |
| s342 | 2848 | no | no |
| s343 | 2877 | no | no |

### unsafe dependent memory operations in loop — Unknown data dependence (3)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s1113 | 182 | no | no |
| s114 | 206 | no | no |
| s281 | 2063 | no | no |

### unsafe dependent memory operations in loop — Unsafe indirect dependence (2)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s141 | 643 | no | no |
| s1161 | 752 | **yes** | no |

### Cannot vectorize uncountable loop (1)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s482 | 3395 | no | no |

### Early exit loop with store but no supported condition load (1)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s481 | 3369 | no | no |

### cannot identify array bounds (1)

| function | loop line | icx 2025.0 vectorized? | clang + vecpass vectorized? |
|---|---|---|---|
| s123 | 428 | no | no |

icx vectorizes 2 of the 44 loops clang misses: s1161, s315

clang + vecpass vectorizes 8 of the 44 loops clang misses: s116, s211, s212, s1213, s241, s244, s1244, s261
