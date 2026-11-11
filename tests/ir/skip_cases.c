// Loops vecpass must leave alone, each with its reason, in the real -O3
// pipeline. Also: loading the plugin must not change this file's output at
// all, since every loop here is skipped.
// RUN: %cir %s -o %t.ll
// RUN: %opt -passes='default<O3>' -pass-remarks-missed=vecpass -disable-output %t.ll 2>&1 | FileCheck %s
// RUN: opt -passes='default<O3>' -S %t.ll -o %t.base.ll
// RUN: %opt -passes='default<O3>' -S %t.ll -o %t.vp.ll
// RUN: diff %t.base.ll %t.vp.ll
float a[1000], b[1000];
void ext(float);

// A true recurrence: a[i] needs a[i-1] from the previous iteration, so
// "store a[i]" must precede "load a[i]" of the next iteration AND follow it.
// CHECK: skip_cases.c:[[@LINE+2]]:{{.*}}no legal order makes all dependences forward
void recurrence(int n) {
  for (int i = 1; i < n; i++) a[i] = a[i - 1] * 0.5f + b[i];
}
// x and y may alias: S2 can't be proven independent of S1, so no legal order.
// CHECK: skip_cases.c:[[@LINE+2]]:{{.*}}no legal order makes all dependences forward
void mayalias(float *x, float *y, const float *z, int n) {
  for (int i = 1; i < n - 1; i++) { x[i] = y[i - 1] + z[i]; y[i] = x[i + 1] * z[i]; }
}
// CHECK: skip_cases.c:[[@LINE+2]]:{{.*}}instruction with side effects: call
void withcall(int n) {
  for (int i = 0; i < n - 1; i++) { a[i] = b[i]; ext(a[i + 1]); }
}
// CHECK: skip_cases.c:[[@LINE+2]]:{{.*}}volatile/atomic
void withvolatile(volatile float *v, int n) {
  for (int i = 0; i < n - 1; i++) { v[i] = b[i]; b[i] = v[i + 1]; }
}
// CHECK: skip_cases.c:[[@LINE+2]]:{{.*}}LAA already considers memory accesses vectorizable
void legal(int n) {
  for (int i = 0; i < n; i++) a[i] = b[i] + 1.0f;
}
// CHECK: skip_cases.c:[[@LINE+2]]:{{.*}}loop body has {{[0-9]+}} basic blocks
void multiblock(int n) {
  for (int i = 0; i < n - 1; i++) { if (b[i] > 0) a[i] = b[i]; b[i] = a[i + 1]; }
}
// CHECK: skip_cases.c:[[@LINE+2]]:{{.*}}not an innermost loop
void nest(int n) {
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n - 1; i++) { a[i] *= b[j]; b[i] += a[i + 1]; }
}
