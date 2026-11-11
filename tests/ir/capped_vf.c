// A loop LLVM vectorizes only with VF=2 (backward dependence, distance 2).
// vecpass reorders it so the width cap disappears.
// RUN: %cir %s -o %t.ll
// RUN: %opt -passes='default<O3>' -pass-remarks=vecpass -disable-output %t.ll 2>&1 | FileCheck %s
// RUN: opt -passes='default<O3>' -pass-remarks=loop-vectorize -disable-output %t.ll 2>&1 | FileCheck %s --check-prefix=BASE
// RUN: %opt -passes='default<O3>' -pass-remarks=loop-vectorize -disable-output %t.ll 2>&1 | FileCheck %s --check-prefix=VEC
void off2(float *__restrict a, float *__restrict b, const float *__restrict c, int n) {
  for (int i = 0; i < n - 2; i++) {
    a[i] = c[i] * 2.0f;
    b[i] = a[i + 2] + a[i];
  }
}
// CHECK: max safe vector width was 64 bits, now unlimited
// BASE:  vectorized loop (vectorization width: 2,
// VEC:   vectorized loop (vectorization width: 8,
