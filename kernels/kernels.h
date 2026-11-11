/* kernels/kernels.h — declarations shared by kernels.c, the differential test
 * driver (tests/diff) and the benchmark harness (bench). */
#ifndef VECPASS_KERNELS_H
#define VECPASS_KERNELS_H

typedef float real_t;

/* Largest n any driver may pass. 16M floats = 64 MiB per array, 5 arrays =
 * 320 MiB: big enough that the "memory" size class exceeds the L3 cache. */
#define N_MAX (16 * 1024 * 1024)

/* Extra elements after each array. The arrays are separate globals, so the
 * linker places them back to back. With ARRAY_PAD=8 each is 64 MiB + 32 B,
 * and the measured layout put all five arrays within 128 bytes of each other
 * modulo 4096, so a[i], b[i], ... share their low 12 address bits ("4K
 * aliasing", see RESULTS.md). ARRAY_PAD=200 spreads them over the 4 KiB
 * window. The benchmark harness prints the layout it got. */
#ifndef ARRAY_PAD
#define ARRAY_PAD 8
#endif
#define ARRAY_LEN (N_MAX + ARRAY_PAD)

extern real_t a[ARRAY_LEN], b[ARRAY_LEN], c[ARRAY_LEN], d[ARRAY_LEN],
    e[ARRAY_LEN];

void k_s212(int n);
void k_s1213(int n);
void k_s211(int n);
void k_s241(int n);
void k_s1244(int n);
void k_s261(int n);
void k_s244(int n);
void k_s116(int n);
void k_fwd1213(int n);
void k_s000(int n);
void k_s321(int n);
void k_off(int n, int k);

#endif
