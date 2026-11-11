/* kernels/kernels.h — declarations shared by kernels.c, the differential test
 * driver (tests/diff) and the benchmark harness (bench). */
#ifndef VECPASS_KERNELS_H
#define VECPASS_KERNELS_H

typedef float real_t;

/* Largest n any driver may pass. 16M floats = 64 MiB per array, 5 arrays =
 * 320 MiB: big enough that the "memory" size class exceeds the L3 cache. */
#define N_MAX (16 * 1024 * 1024)

extern real_t a[N_MAX + 8], b[N_MAX + 8], c[N_MAX + 8], d[N_MAX + 8],
    e[N_MAX + 8];

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
