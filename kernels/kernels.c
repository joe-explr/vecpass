/* kernels/kernels.c — TSVC-style loop kernels used by the benchmark and the
 * differential tests.
 *
 * Each kernel is the loop body of a TSVC_2 kernel (github.com/UoB-HPC/TSVC_2,
 * same name), operating on global arrays like TSVC does, but with a runtime
 * trip count `n` so one binary can run L1-, L2- and memory-sized problems.
 * Arrays have N_MAX+8 elements so the i+1 / i-1 accesses stay in bounds.
 *
 * Groups:
 *   TARGET  — loops clang -O3 rejects with a "backward loop carried data
 *             dependence" that vecpass fixes by reordering (or by LLE only).
 *   CONTROL — loops vecpass must leave alone: already vectorized, a true
 *             recurrence, or a runtime offset LLVM already handles.
 *
 * Compile this file twice (baseline / with vecpass) and link each with the
 * same driver: any output difference is a vecpass bug. */
#include "kernels.h"

real_t a[N_MAX + 8], b[N_MAX + 8], c[N_MAX + 8], d[N_MAX + 8], e[N_MAX + 8];

/* ---------------- TARGET: statement reordering needed ---------------- */

/* s212: load a[i+1] after store a[i] -> hoist the load. */
void k_s212(int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] *= c[i];
    b[i] += a[i + 1] * d[i];
  }
}

/* s1213: S2 must move above S1 (b[i] stored before b[i-1] is read),
 * then LLE turns the b[i-1] load into a register recurrence. */
void k_s1213(int n) {
  for (int i = 1; i < n - 1; i++) {
    a[i] = b[i - 1] + c[i];
    b[i] = a[i + 1] * d[i];
  }
}

/* s211: same shape as s1213 with more arithmetic. */
void k_s211(int n) {
  for (int i = 1; i < n - 1; i++) {
    a[i] = b[i - 1] + c[i] * d[i];
    b[i] = b[i + 1] - e[i] * d[i];
  }
}

/* s241: like s212, a[i+1] is read after a[i] is written. */
void k_s241(int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] = b[i] * c[i] * d[i];
    b[i] = a[i] * a[i + 1] * d[i];
  }
}

/* s1244: like s212. */
void k_s1244(int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] = b[i] + c[i] * c[i] + b[i] * b[i] + c[i];
    d[i] = a[i] + a[i + 1];
  }
}

/* s261: c[i] is written after c[i-1] is read -> move S2 first. */
void k_s261(int n) {
  real_t t;
  for (int i = 1; i < n; ++i) {
    t = a[i] + b[i];
    a[i] = t + c[i - 1];
    t = c[i] * d[i];
    c[i] = t;
  }
}

/* s244: a[i+1] stored at the end of iteration i, a[i] stored at the start
 * of iteration i+1 (output dependence) -> move the a[i] store later. */
void k_s244(int n) {
  for (int i = 0; i < n - 1; ++i) {
    a[i] = b[i] + c[i] * d[i];
    b[i] = c[i] + b[i];
    a[i + 1] = b[i] + a[i + 1] * d[i];
  }
}

/* s116: manually unrolled by 5, stride-5 accesses. */
void k_s116(int n) {
  for (int i = 0; i < n - 5; i += 5) {
    a[i] = a[i + 1] * a[i];
    a[i + 1] = a[i + 2] * a[i + 1];
    a[i + 2] = a[i + 3] * a[i + 2];
    a[i + 3] = a[i + 4] * a[i + 3];
    a[i + 4] = a[i + 5] * a[i + 4];
  }
}

/* ------------- TARGET: forward dependence only (LLE path) ------------- */

/* s1213 as a programmer might write it with S2 first: already "forward",
 * but LAA says it "prevents store-to-load forwarding"; clang -O3 gives up. */
void k_fwd1213(int n) {
  for (int i = 1; i < n - 1; i++) {
    b[i] = a[i + 1] * d[i];
    a[i] = b[i - 1] + c[i];
  }
}

/* ----------------------------- CONTROL -------------------------------- */

/* Already vectorized by clang. */
void k_s000(int n) {
  for (int i = 0; i < n; i++)
    a[i] = b[i] + 1;
}

/* True recurrence: no reordering can help. vecpass must skip it. */
void k_s321(int n) {
  for (int i = 1; i < n; i++)
    a[i] += a[i - 1] * b[i];
}

/* Runtime offset k (s162-like). LLVM 23 already vectorizes it with a
 * dependence-distance runtime check; vecpass leaves it alone. */
void k_off(int n, int k) {
  for (int i = 0; i < n; i++)
    a[i] = a[i + k] + b[i] * c[i];
}
