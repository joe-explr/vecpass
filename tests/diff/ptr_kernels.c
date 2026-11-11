/* tests/diff/ptr_kernels.c — pointer-argument kernels for the differential
 * test. Unlike kernels/kernels.c (global arrays, which the compiler knows are
 * distinct), these take pointers that MAY alias. The driver calls them with
 * overlapping arguments on purpose, so we test that vecpass never reorders
 * accesses that could touch the same memory in the same iteration.
 *
 *   p_*  plain pointers   -> vecpass may only move accesses it proves apart
 *   r_*  restrict         -> the programmer promises no aliasing
 *   d_*  double, i_* int  -> other element sizes (8 and 4 bytes)
 */
#define RESTRICT __restrict

/* s212 shape: hoisting a[i+1] above a[i] is safe even with plain pointers
 * (the two addresses always differ by 4 bytes). */
void p_s212(float *a, float *b, const float *c, const float *d, int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] *= c[i];
    b[i] += a[i + 1] * d[i];
  }
}
void r_s212(float *RESTRICT a, float *RESTRICT b, const float *RESTRICT c,
            const float *RESTRICT d, int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] *= c[i];
    b[i] += a[i + 1] * d[i];
  }
}
void d_s212(double *RESTRICT a, double *RESTRICT b, const double *RESTRICT c,
            const double *RESTRICT d, int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] *= c[i];
    b[i] += a[i + 1] * d[i];
  }
}
void i_s212(int *RESTRICT a, int *RESTRICT b, const int *RESTRICT c,
            const int *RESTRICT d, int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] *= c[i];
    b[i] += a[i + 1] * d[i];
  }
}

/* s1213 shape: moving S2 above S1 needs a and b to be distinct. With plain
 * pointers vecpass must refuse (dependence cycle); with restrict it may. */
void p_s1213(float *a, float *b, const float *c, const float *d, int n) {
  for (int i = 1; i < n - 1; i++) {
    a[i] = b[i - 1] + c[i];
    b[i] = a[i + 1] * d[i];
  }
}
void r_s1213(float *RESTRICT a, float *RESTRICT b, const float *RESTRICT c,
             const float *RESTRICT d, int n) {
  for (int i = 1; i < n - 1; i++) {
    a[i] = b[i - 1] + c[i];
    b[i] = a[i + 1] * d[i];
  }
}
void d_s1213(double *RESTRICT a, double *RESTRICT b, const double *RESTRICT c,
             const double *RESTRICT d, int n) {
  for (int i = 1; i < n - 1; i++) {
    a[i] = b[i - 1] + c[i];
    b[i] = a[i + 1] * d[i];
  }
}

/* s261 shape with restrict. */
void r_s261(float *RESTRICT a, const float *RESTRICT b, float *RESTRICT c,
            const float *RESTRICT d, int n) {
  for (int i = 1; i < n; ++i) {
    float t = a[i] + b[i];
    a[i] = t + c[i - 1];
    c[i] = c[i] * d[i];
  }
}

/* Larger forward offsets: b[i] uses a[i+2] after a[i] is written. */
void r_off2(float *RESTRICT a, float *RESTRICT b, const float *RESTRICT c,
            int n) {
  for (int i = 0; i < n - 2; i++) {
    a[i] = c[i] * 2.0f;
    b[i] = a[i + 2] + a[i];
  }
}

/* Runtime offset k on one array (the brief's original candidate). LLVM
 * already versions this on the distance; vecpass should leave it alone.
 * The driver runs it with k = 0, +-1, +-3, +-7, +-8, +-9, +-16, +-33. */
void p_offk(float *a, const float *b, int n, int k) {
  for (int i = 0; i < n; i++)
    a[i] = a[i + k] + b[i];
}

/* True recurrence: must stay scalar and untouched. */
void p_rec(float *a, const float *b, int n) {
  for (int i = 1; i < n; i++)
    a[i] = a[i - 1] * 0.5f + b[i];
}
