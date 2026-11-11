// vecpass-print smoke test (Phase 1).
// RUN: %cir %s -o %t.ll
// RUN: %opt -passes='function(sroa,early-cse,loop-simplify,loop(loop-rotate),vecpass-print)' -disable-output %t.ll 2>&1 | FileCheck %s
void f(float *a, int n) { for (int i = 0; i < n; i++) a[i] *= 2.0f; }
void g(float *a) { for (int i = 0; i < 64; i++) a[i] += 1.0f; }
// CHECK: function 'f': loop depth=1 header=for.body tripcount=({{.*}}%n{{.*}}) + 1
// CHECK: function 'g': loop depth=1 header=for.body tripcount=64
