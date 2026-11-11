; Hand-written IR test: TSVC s1213, in the shape LoopVectorize sees it.
;   a[i] = b[i-1] + c[i];     (S1)
;   b[i] = a[i+1] * d[i];     (S2)
; Two backward dependences: a[i+1] is loaded after a[i] is stored, and b[i-1]
; is loaded before b[i] is stored (one iteration later). vecpass moves all of
; S2 above S1; LoopLoadElimination (run by vecpass) then replaces the b[i-1]
; load with a PHI of the value stored to b[i] in the previous iteration.
; RUN: %opt -passes=vecpass -S %s | FileCheck %s
; RUN: %opt -passes=vecpass -vecpass-run-lle=false -pass-remarks-missed=vecpass -S %s 2>&1 | FileCheck %s --check-prefix=NOLLE
; RUN: %opt -mtriple=x86_64-unknown-linux-gnu -mattr=+avx2 -passes=loop-vectorize -S %s | FileCheck %s --check-prefix=BASE
; RUN: %opt -mtriple=x86_64-unknown-linux-gnu -mattr=+avx2 -passes='vecpass,loop-vectorize' -S %s | FileCheck %s --check-prefix=VEC

@a = global [1000 x float] zeroinitializer
@b = global [1000 x float] zeroinitializer
@c = global [1000 x float] zeroinitializer
@d = global [1000 x float] zeroinitializer

define void @s1213() {
entry:
  br label %body
body:
  %i = phi i64 [ 1, %entry ], [ %i.next, %body ]
  %im1 = add nsw i64 %i, -1
  %pbm1 = getelementptr inbounds float, ptr @b, i64 %im1
  %vb = load float, ptr %pbm1                     ; b[i-1]
  %pc = getelementptr inbounds float, ptr @c, i64 %i
  %vc = load float, ptr %pc
  %s1 = fadd float %vb, %vc
  %pa = getelementptr inbounds float, ptr @a, i64 %i
  store float %s1, ptr %pa                        ; a[i] = ...
  %i.next = add nuw nsw i64 %i, 1
  %pa1 = getelementptr inbounds float, ptr @a, i64 %i.next
  %va1 = load float, ptr %pa1                     ; a[i+1]
  %pd = getelementptr inbounds float, ptr @d, i64 %i
  %vd = load float, ptr %pd
  %s2 = fmul float %va1, %vd
  %pb = getelementptr inbounds float, ptr @b, i64 %i
  store float %s2, ptr %pb                        ; b[i] = ...
  %done = icmp eq i64 %i.next, 999
  br i1 %done, label %exit, label %body
exit:
  ret void
}

; S2 now comes first, and b[i-1] is a PHI, not a load:
; CHECK-LABEL: define void @s1213
; CHECK:       body:
; CHECK:         %store_forwarded = phi float
; CHECK:         %va1 = load float, ptr %pa1
; CHECK:         store float %s2, ptr %pb
; CHECK:         %s1 = fadd float %store_forwarded, %vc
; CHECK:         store float %s1, ptr %pa
; CHECK-NOT:     load float, ptr %pbm1
; Without LLE the reordering alone isn't enough, so vecpass reverts it:
; NOLLE: loop not reordered: reordering did not make the loop legal (only store-to-load-forwarding dependences remain, but -vecpass-run-lle=false); original order restored
; NOLLE: store float %s1, ptr %pa
; NOLLE: %va1 = load float, ptr %pa1
; BASE-NOT: <8 x float>
; VEC:      <8 x float>
