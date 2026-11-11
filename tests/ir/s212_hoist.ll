; Hand-written IR test: TSVC s212 shape. The load of a[i+1] comes after the
; store to a[i]; vecpass must hoist it (and its GEP) above the store.
; RUN: %opt -passes=vecpass -S %s | FileCheck %s --check-prefix=AFTER
; RUN: %opt -passes=vecpass -pass-remarks=vecpass -disable-output %s 2>&1 | FileCheck %s --check-prefix=REMARK
; Without vecpass LoopVectorize refuses; with it, it vectorizes (AVX2: 8 x float).
; RUN: %opt -mtriple=x86_64-unknown-linux-gnu -mattr=+avx2 -passes=loop-vectorize -S %s | FileCheck %s --check-prefix=BASE
; RUN: %opt -mtriple=x86_64-unknown-linux-gnu -mattr=+avx2 -passes='vecpass,loop-vectorize' -S %s | FileCheck %s --check-prefix=VEC

@a = global [1000 x float] zeroinitializer
@b = global [1000 x float] zeroinitializer
@c = global [1000 x float] zeroinitializer

define void @s212(i64 %n) {
entry:
  br label %body
body:
  %i = phi i64 [ 0, %entry ], [ %i.next, %body ]
  %pc = getelementptr inbounds float, ptr @c, i64 %i
  %vc = load float, ptr %pc
  %pa = getelementptr inbounds float, ptr @a, i64 %i
  store float %vc, ptr %pa                         ; a[i] = c[i]
  %i.next = add nuw nsw i64 %i, 1
  %pa1 = getelementptr inbounds float, ptr @a, i64 %i.next
  %va1 = load float, ptr %pa1                      ; ... = a[i+1]
  %pb = getelementptr inbounds float, ptr @b, i64 %i
  store float %va1, ptr %pb                        ; b[i] = a[i+1]
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %body
exit:
  ret void
}

; AFTER-LABEL: define void @s212
; AFTER:       %va1 = load float, ptr %pa1
; AFTER:       store float %vc, ptr %pa
; AFTER:       store float %va1, ptr %pb
; REMARK: remark: <unknown>:0:0: reordered loop body ({{[0-9]+}} of {{[0-9]+}} instructions moved) so that 1 backward dependence(s) become forward
; BASE-NOT:    <8 x float>
; VEC:         load <8 x float>
; VEC:         store <8 x float>
