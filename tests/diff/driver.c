/* tests/diff/driver.c — differential test driver.
 *
 * Linked twice: once with kernels compiled by plain clang -O3, once with the
 * same kernels compiled with -O3 + vecpass. Both binaries print one line per
 * (kernel, trip count, aliasing setup) with a bitwise hash of ALL memory the
 * kernel could touch. The test passes iff the two outputs are identical.
 *
 * Why bitwise and not "close enough": vecpass only reorders independent
 * operations and LoopVectorize does not reassociate FP math without
 * -ffast-math, so results must match exactly. Any difference is a bug.
 *
 * Inputs are pseudo-random (fixed seed) so runs are reproducible.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../kernels/kernels.h"

/* pointer kernels (ptr_kernels.c) */
void p_s212(float *a, float *b, const float *c, const float *d, int n);
void r_s212(float *a, float *b, const float *c, const float *d, int n);
void d_s212(double *a, double *b, const double *c, const double *d, int n);
void i_s212(int *a, int *b, const int *c, const int *d, int n);
void p_s1213(float *a, float *b, const float *c, const float *d, int n);
void r_s1213(float *a, float *b, const float *c, const float *d, int n);
void d_s1213(double *a, double *b, const double *c, const double *d, int n);
void r_s261(float *a, const float *b, float *c, const float *d, int n);
void r_off2(float *a, float *b, const float *c, int n);
void p_offk(float *a, const float *b, int n, int k);
void p_rec(float *a, const float *b, int n);

/* Trip counts: tiny, around every VF/UF multiple (4, 8, 16, 32, 64 lanes),
 * and odd sizes so the scalar remainder / epilogue loops run. */
static const int Ns[] = {0,  1,  2,  3,  4,  5,  6,  7,   8,   9,   15,  16,
                         17, 31, 32, 33, 63, 64, 65, 100, 127, 128, 129, 255,
                         256, 257, 1000, 1023, 4097};
#define NUM_N ((int)(sizeof Ns / sizeof Ns[0]))
#define MAXN 4097
#define PAD 64 /* room for offsets up to +-PAD around each array */

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { /* xorshift64 */
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
}
static float rnd(void) { return (float)((next() >> 40) % 4001) / 1000.0f - 2.0f; }

/* FNV-1a over raw bytes: equal hash <=> (almost surely) equal bits. */
static uint64_t hash(const void *p, size_t bytes) {
  const unsigned char *s = p;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < bytes; i++) { h ^= s[i]; h *= 1099511628211ull; }
  return h;
}

/* ---- global-array kernels (kernels.c) ---- */
static void fill_globals(int n) {
  for (int i = 0; i < n + 8; i++) {
    a[i] = rnd(); b[i] = rnd(); c[i] = rnd(); d[i] = rnd(); e[i] = rnd();
  }
}
static uint64_t hash_globals(int n) {
  size_t s = (size_t)(n + 8) * sizeof(real_t);
  return hash(a, s) ^ 3 * hash(b, s) ^ 5 * hash(c, s) ^ 7 * hash(d, s) ^
         11 * hash(e, s);
}
typedef void (*gk_t)(int);
static const struct { const char *name; gk_t fn; } GK[] = {
    {"s212", k_s212},   {"s1213", k_s1213}, {"s211", k_s211},
    {"s241", k_s241},   {"s1244", k_s1244}, {"s261", k_s261},
    {"s244", k_s244},   {"s116", k_s116},   {"fwd1213", k_fwd1213},
    {"s000", k_s000},   {"s321", k_s321},
};

/* ---- pointer kernels: one big buffer so we can make arrays overlap ---- */
#define SLOT (MAXN + 2 * PAD)
static float fbuf[6 * SLOT];
static double dbuf[6 * SLOT];
static int ibuf[6 * SLOT];

static void fill_f(void) { for (int i = 0; i < 6 * SLOT; i++) fbuf[i] = rnd(); }
static float *slot(int s) { return fbuf + s * SLOT + PAD; } /* distinct arrays */

int main(void) {
  /* 1) global-array TSVC kernels */
  for (unsigned k = 0; k < sizeof GK / sizeof GK[0]; k++)
    for (int j = 0; j < NUM_N; j++) {
      int n = Ns[j];
      fill_globals(n);
      GK[k].fn(n);
      printf("%-8s n=%-5d %016llx\n", GK[k].name, n,
             (unsigned long long)hash_globals(n));
    }
  /* runtime offset kernel on globals, k from -8..8 (a has 8 spare slots at
   * the end; negative k reads before a[0] only if i+k<0, so start the
   * arrays' use at index 8 by restricting n). */
  for (int k = 0; k <= 8; k++)
    for (int j = 0; j < NUM_N; j++) {
      int n = Ns[j];
      fill_globals(n);
      k_off(n, k);
      printf("off      k=%-3d n=%-5d %016llx\n", k, n,
             (unsigned long long)hash_globals(n));
    }

  /* 2) pointer kernels. Aliasing setups: offset of the 2nd array relative to
   * the 1st, in elements. 9999 = separate (non-overlapping) arrays. */
  static const int Offs[] = {9999, 0, 1, -1, 2, -2, 3, 7, 8, 9, 16, -16, 33};
  const int NOFF = sizeof Offs / sizeof Offs[0];
  for (int o = 0; o < NOFF; o++)
    for (int j = 0; j < NUM_N; j++) {
      int n = Ns[j], off = Offs[o];
      float *A = slot(1), *B = off == 9999 ? slot(3) : slot(1) + off;
      fill_f(); p_s212(A, B, slot(4), slot(5), n);
      printf("p_s212   off=%-5d n=%-5d %016llx\n", off, n,
             (unsigned long long)hash(fbuf, sizeof fbuf));
      fill_f(); p_s1213(A, B, slot(4), slot(5), n);
      printf("p_s1213  off=%-5d n=%-5d %016llx\n", off, n,
             (unsigned long long)hash(fbuf, sizeof fbuf));
      /* c overlapping a, too */
      fill_f(); p_s212(slot(1), slot(3), off == 9999 ? slot(4) : slot(1) + off,
                       slot(5), n);
      printf("p_s212c  off=%-5d n=%-5d %016llx\n", off, n,
             (unsigned long long)hash(fbuf, sizeof fbuf));
    }
  for (int j = 0; j < NUM_N; j++) {
    int n = Ns[j];
    fill_f(); r_s212(slot(1), slot(2), slot(3), slot(4), n);
    printf("r_s212   n=%-5d %016llx\n", n, (unsigned long long)hash(fbuf, sizeof fbuf));
    fill_f(); r_s1213(slot(1), slot(2), slot(3), slot(4), n);
    printf("r_s1213  n=%-5d %016llx\n", n, (unsigned long long)hash(fbuf, sizeof fbuf));
    fill_f(); r_s261(slot(1), slot(2), slot(3), slot(4), n);
    printf("r_s261   n=%-5d %016llx\n", n, (unsigned long long)hash(fbuf, sizeof fbuf));
    fill_f(); r_off2(slot(1), slot(2), slot(3), n);
    printf("r_off2   n=%-5d %016llx\n", n, (unsigned long long)hash(fbuf, sizeof fbuf));
    fill_f(); p_rec(slot(1), slot(2), n);
    printf("p_rec    n=%-5d %016llx\n", n, (unsigned long long)hash(fbuf, sizeof fbuf));

    for (int i = 0; i < 6 * SLOT; i++) dbuf[i] = rnd();
    d_s212(dbuf + SLOT, dbuf + 2 * SLOT, dbuf + 3 * SLOT, dbuf + 4 * SLOT, n);
    printf("d_s212   n=%-5d %016llx\n", n, (unsigned long long)hash(dbuf, sizeof dbuf));
    for (int i = 0; i < 6 * SLOT; i++) dbuf[i] = rnd();
    d_s1213(dbuf + SLOT, dbuf + 2 * SLOT, dbuf + 3 * SLOT, dbuf + 4 * SLOT, n);
    printf("d_s1213  n=%-5d %016llx\n", n, (unsigned long long)hash(dbuf, sizeof dbuf));
    /* small ints so a[i]*=c[i] can't overflow (signed overflow is UB) */
    for (int i = 0; i < 6 * SLOT; i++) ibuf[i] = (int)(next() % 7) - 3;
    i_s212(ibuf + SLOT, ibuf + 2 * SLOT, ibuf + 3 * SLOT, ibuf + 4 * SLOT, n);
    printf("i_s212   n=%-5d %016llx\n", n, (unsigned long long)hash(ibuf, sizeof ibuf));
  }
  /* runtime offset k: the brief's edge cases k=0, negative k, |k| < VF,
   * |k| = VF, |k| > VF. */
  static const int Ks[] = {0, 1, -1, 2, -2, 3, -3, 7, -7, 8, -8, 9, -9,
                           15, -15, 16, -16, 17, -17, 31, -31, 32, -32, 33, -33};
  for (unsigned q = 0; q < sizeof Ks / sizeof Ks[0]; q++)
    for (int j = 0; j < NUM_N; j++) {
      int n = Ns[j], k = Ks[q];
      fill_f(); p_offk(slot(1), slot(3), n, k);
      printf("p_offk   k=%-3d n=%-5d %016llx\n", k, n,
             (unsigned long long)hash(fbuf, sizeof fbuf));
    }
  return 0;
}
