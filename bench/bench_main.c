/* bench/bench_main.c — timing harness for kernels/kernels.c.
 *
 * Usage: bench <kernel|all> <n> [samples] [fixed_reps]
 *   fixed_reps: skip calibration and use this many calls per sample, so two
 *   builds do exactly the same work (used for perf stat comparisons).
 * Output (CSV on stdout):
 *   kernel,n,reps_per_sample,median_ns_per_elem,min_ns_per_elem,checksum
 *
 * Method, per kernel:
 *   1. initialise arrays (fixed values; identical for every build),
 *   2. calibrate: pick `reps` so one sample (reps back-to-back calls) takes
 *      >= 5 ms, which amortises timer overhead for tiny n,
 *   3. 3 warm-up samples (caches, page faults, frequency ramp),
 *   4. `samples` timed samples (default 21) with CLOCK_MONOTONIC; arrays are
 *      re-initialised (untimed) before each sample so values stay bounded,
 *   5. report the median (robust to the odd interrupted sample) and min.
 * The checksum is computed after one extra call on fresh data. It must match
 * between the clang and clang+vecpass builds (same arithmetic, same order).
 *
 * This file is always compiled by plain clang; only kernels.c changes
 * between builds, so harness overhead is identical.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include "../kernels/kernels.h"

typedef void (*kfn)(int);
static void k_off1(int n) { k_off(n, 1); } /* runtime offset, k = 1 */

static const struct { const char *name; kfn fn; } K[] = {
    {"s212", k_s212},   {"s1213", k_s1213},     {"s211", k_s211},
    {"s241", k_s241},   {"s1244", k_s1244},     {"s261", k_s261},
    {"s244", k_s244},   {"s116", k_s116},       {"fwd1213", k_fwd1213},
    {"s000", k_s000},   {"s321", k_s321},       {"off_k1", k_off1},
};
#define NK ((int)(sizeof K / sizeof K[0]))

static void init(int n) {
  /* Values near 1 so repeated a*=c etc. don't overflow quickly. */
  for (int i = 0; i < n + 8; i++) {
    a[i] = 1.0f + (i % 7) * 0.001f;
    b[i] = 1.0f - (i % 5) * 0.001f;
    c[i] = 0.999f + (i % 3) * 0.001f;
    d[i] = 0.5f + (i % 11) * 0.01f;
    e[i] = 0.25f + (i % 13) * 0.01f;
  }
}

static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}

static int cmp_d(const void *x, const void *y) {
  double p = *(const double *)x, q = *(const double *)y;
  return (p > q) - (p < q);
}

static double checksum(int n) {
  double s = 0;
  for (int i = 0; i < n + 8; i++)
    s += a[i] + 2.0 * b[i] + 3.0 * c[i] + 4.0 * d[i] + 5.0 * e[i];
  return s;
}

static void run(int k, int n, int samples, long fixed_reps) {
  init(n);
  /* calibrate */
  long reps = fixed_reps > 0 ? fixed_reps : 1;
  while (fixed_reps <= 0) {
    double t0 = now_ns();
    for (long r = 0; r < reps; r++) K[k].fn(n);
    if (now_ns() - t0 >= 5e6 || reps >= (1L << 30)) break;
    reps *= 2;
  }
  double *t = malloc(sizeof(double) * samples);
  for (int s = -3; s < samples; s++) {
    init(n);
    double t0 = now_ns();
    for (long r = 0; r < reps; r++) K[k].fn(n);
    double dt = now_ns() - t0;
    if (s >= 0) t[s] = dt / ((double)reps * n);
  }
  qsort(t, samples, sizeof(double), cmp_d);
  init(n);
  K[k].fn(n);
  printf("%s,%d,%ld,%.4f,%.4f,%.10e\n", K[k].name, n, reps, t[samples / 2],
         t[0], checksum(n));
  fflush(stdout);
  free(t);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <kernel|all> <n> [samples]\n", argv[0]);
    return 2;
  }
  /* Flush denormals to zero in BOTH builds, so slow denormal arithmetic
   * can't distort the comparison. */
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
  int n = atoi(argv[2]);
  int samples = argc > 3 ? atoi(argv[3]) : 21;
  long fixed_reps = argc > 4 ? atol(argv[4]) : 0;
  if (samples < 1) samples = 1;
  if (n < 8 || n > N_MAX) { fprintf(stderr, "n out of range\n"); return 2; }
  int found = 0;
  for (int k = 0; k < NK; k++)
    if (!strcmp(argv[1], "all") || !strcmp(argv[1], K[k].name)) {
      run(k, n, samples, fixed_reps);
      found = 1;
    }
  if (!found) { fprintf(stderr, "unknown kernel %s\n", argv[1]); return 2; }
  return 0;
}
