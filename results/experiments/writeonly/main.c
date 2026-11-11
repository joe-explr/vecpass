/* Times wo()/rw() from kern.c: 3 warm-ups, 11 samples, median ns/element. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>
#include <pmmintrin.h>
#define N (16 * 1024 * 1024)
extern float a[], b[], c[], d[];
void wo(int), rw(int);
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static int cmp(const void *x, const void *y) { double p = *(double *)x, q = *(double *)y; return (p > q) - (p < q); }
static void init(int n) { for (int i = 0; i < n + 8; i++) { a[i] = 1; b[i] = 0.999f; c[i] = 0.5f; d[i] = 0.25f; } }
int main(int argc, char **argv) {
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON); _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
  int n = atoi(argv[2]); void (*f)(int) = strcmp(argv[1], "wo") ? rw : wo;
  int reps = n <= 65536 ? 2000 : 1; double t[11];
  for (int s = -3; s < 11; s++) { init(n); double t0 = now(); for (int r = 0; r < reps; r++) f(n);
    double dt = (now() - t0) / ((double)reps * n); if (s >= 0) t[s] = dt; }
  qsort(t, 11, sizeof t[0], cmp); printf("%s,%d,%.4f\n", argv[1], n, t[5]); return 0;
}
