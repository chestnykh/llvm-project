; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -S -passes=newgvn %s | FileCheck %s

define void @tinkywinky(ptr %b, i1 %arg) {
; CHECK-LABEL: @tinkywinky(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[BODY:%.*]]
; CHECK:       body:
; CHECK-NEXT:    store i64 undef, ptr [[B:%.*]], align 4
; CHECK-NEXT:    br i1 %arg, label [[BODY]], label [[END:%.*]]
; CHECK:       end:
; CHECK-NEXT:    br label [[BODY]]
;
entry:
  br label %body
body:
  %d.1 = phi ptr [ undef, %entry ], [ %d.1, %body ], [ %b, %end ]
  store i64 undef, ptr %d.1
  %b2 = load i64, ptr %b
  %or = or i64 %b2, 0
  store i64 %or, ptr %b
  br i1 %arg, label %body, label %end
end:
  br label %body
}
