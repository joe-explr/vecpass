// Already forward, but clang gives up because of store-to-load forwarding.
// vecpass schedules LoopLoadElimination; then it vectorizes.
// RUN: %cir %s -o %t.ll
// RUN: %opt -passes='default<O3>' -pass-remarks='vecpass|loop-vectorize' -disable-output %t.ll 2>&1 | FileCheck %s
float a[1000], b[1000], c[1000], d[1000];
void fwd(void) {
  for (int i = 1; i < 999; i++) {
    b[i] = a[i + 1] * d[i];
    a[i] = b[i - 1] + c[i];
  }
}
// CHECK: no reordering needed; only store-to-load-forwarding dependences
// CHECK: vectorized loop
// Baseline -O3 (no plugin) does not vectorize it:
// RUN: opt -passes='default<O3>' -pass-remarks-missed=loop-vectorize -disable-output %t.ll 2>&1 | FileCheck %s --check-prefix=BASE
// BASE: fwd_only.c:7:3: loop not vectorized
