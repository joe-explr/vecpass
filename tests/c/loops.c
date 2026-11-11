/* tests/c/loops.c — small loops with known shapes for the vecpass-print test.
 * f_const: constant trip count (SCEV knows 1000)
 * f_n:     runtime trip count n (SCEV gives a symbolic count)
 * f_nest:  a 2-deep nest (depth 1 and depth 2)
 * f_while: data-dependent exit (SCEV cannot compute a trip count)
 * f_none:  no loops at all */
void f_const(float *a) { for (int i = 0; i < 1000; i++) a[i] += 1.0f; }
void f_n(float *a, int n) { for (int i = 0; i < n; i++) a[i] *= 2.0f; }
void f_nest(float *a, int n, int m) {
  for (int i = 0; i < n; i++)
    for (int j = 0; j < m; j++) a[i * m + j] += (float)j; /* not a memset */
}
int f_while(const int *p) { int i = 0; while (p[i] > 3) i++; return i; } /* != 0 would become wcslen */
int f_none(int x) { return x + 1; }
