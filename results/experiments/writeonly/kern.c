/* Write-only vs read-write output stream, s1244 shape (vecpass hoists a[i+1]).
 * wo: d[i]  = a[i] + a[i+1]   (d is write-only)
 * rw: d[i] += a[i] + a[i+1]   (d is read and written)
 * Compiled with and without vecpass; main.c times them at large n. */
#define N (16 * 1024 * 1024)
float a[N + 200], b[N + 200], c[N + 200], d[N + 200];
void wo(int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] = b[i] + c[i] * c[i] + b[i] * b[i] + c[i];
    d[i] = a[i] + a[i + 1];
  }
}
void rw(int n) {
  for (int i = 0; i < n - 1; i++) {
    a[i] = b[i] + c[i] * c[i] + b[i] * b[i] + c[i];
    d[i] += a[i] + a[i + 1];
  }
}
