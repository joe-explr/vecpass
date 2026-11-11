/* analysis/experiments/reorder.c — original TSVC kernels vs. hand-reordered
 * versions (suffix r). Shows that reordering alone lets clang vectorize s212/s241/s1244,
 * and that s1213/s211/s261 additionally need LoopLoadElimination before LV. */
#define N 32000
float a[N], b[N], c[N], d[N], e[N];
void s212(void)  { for (int i = 0; i < N-1; i++) { a[i] *= c[i]; b[i] += a[i + 1] * d[i]; } }
void s212r(void) { for (int i = 0; i < N-1; i++) { float t = a[i+1]; a[i] *= c[i]; b[i] += t * d[i]; } }
void s1213(void) { for (int i = 1; i < N-1; i++) { a[i] = b[i-1]+c[i]; b[i] = a[i+1]*d[i]; } }
void s1213r(void){ for (int i = 1; i < N-1; i++) { b[i] = a[i+1]*d[i]; a[i] = b[i-1]+c[i]; } }
void s211(void)  { for (int i = 1; i < N-1; i++) { a[i] = b[i-1] + c[i]*d[i]; b[i] = b[i+1] - e[i]*d[i]; } }
void s211r(void) { for (int i = 1; i < N-1; i++) { b[i] = b[i+1] - e[i]*d[i]; a[i] = b[i-1] + c[i]*d[i]; } }
void s241(void)  { for (int i = 0; i < N-1; i++) { a[i] = b[i]*c[i]*d[i]; b[i] = a[i]*a[i+1]*d[i]; } }
void s241r(void) { for (int i = 0; i < N-1; i++) { float t = a[i+1]; a[i] = b[i]*c[i]*d[i]; b[i] = a[i]*t*d[i]; } }
void s1244(void) { for (int i = 0; i < N-1; i++) { a[i] = b[i] + c[i]*c[i] + b[i]*b[i] + c[i]; d[i] = a[i] + a[i+1]; } }
void s1244r(void){ for (int i = 0; i < N-1; i++) { float t = a[i+1]; a[i] = b[i] + c[i]*c[i] + b[i]*b[i] + c[i]; d[i] = a[i] + t; } }
void s261(void)  { for (int i = 1; i < N; i++) { float t = a[i] + b[i]; a[i] = t + c[i-1]; t = c[i]*d[i]; c[i] = t; } }
void s261r(void) { for (int i = 1; i < N; i++) { float u = c[i]*d[i]; c[i] = u; float t = a[i] + b[i]; a[i] = t + c[i-1]; } }
