/* analysis/experiments/runtime_offset.c — does clang 23 already vectorize a[i] = a[i+k] + b[i]?
 * Run: analysis/experiments/run.sh   (look for diff.check in runtime_offset.ll) */
#define N 4096
float A[N+64], B[N+64];
// 1: global arrays, unknown k, no guard
void g_nog(int n, int k) { for (int i = 0; i < n; i++) A[i] = A[i + k] + B[i]; }
// 2: same with guard k >= 0
void g_guard(int n, int k) { if (k >= 0) for (int i = 0; i < n; i++) A[i] = A[i + k] + B[i]; }
// 3: pointer args
void p_nog(float *a, float *b, int n, int k) { for (int i = 0; i < n; i++) a[i] = a[i + k] + b[i]; }
void p_guard(float *a, float *b, int n, int k) { if (k >= 0) for (int i = 0; i < n; i++) a[i] = a[i + k] + b[i]; }
void p_restrict(float *restrict a, const float *restrict b, int n, int k) { for (int i = 0; i < n; i++) a[i] = a[i + k] + b[i]; }
// 4: backward with unknown k:  a[i+k] = a[i] + b[i]  (s174 style)
void p_s174(float *a, float *b, int n, int k) { for (int i = 0; i < n; i++) a[i + k] = a[i] + b[i]; }
