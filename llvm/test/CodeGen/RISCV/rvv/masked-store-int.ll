; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=riscv32 -mattr=+v -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -mtriple=riscv64 -mattr=+v -verify-machineinstrs < %s | FileCheck %s

define void @masked_store_nxv1i8(<vscale x 1 x i8> %val, ptr %a, <vscale x 1 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv1i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e8, mf8, ta, ma
; CHECK-NEXT:    vse8.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v1i8.p0(<vscale x 1 x i8> %val, ptr %a, i32 1, <vscale x 1 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v1i8.p0(<vscale x 1 x i8>, ptr, i32, <vscale x 1 x i1>)

define void @masked_store_nxv1i16(<vscale x 1 x i16> %val, ptr %a, <vscale x 1 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv1i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e16, mf4, ta, ma
; CHECK-NEXT:    vse16.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v1i16.p0(<vscale x 1 x i16> %val, ptr %a, i32 2, <vscale x 1 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v1i16.p0(<vscale x 1 x i16>, ptr, i32, <vscale x 1 x i1>)

define void @masked_store_nxv1i32(<vscale x 1 x i32> %val, ptr %a, <vscale x 1 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv1i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e32, mf2, ta, ma
; CHECK-NEXT:    vse32.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v1i32.p0(<vscale x 1 x i32> %val, ptr %a, i32 4, <vscale x 1 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v1i32.p0(<vscale x 1 x i32>, ptr, i32, <vscale x 1 x i1>)

define void @masked_store_nxv1i64(<vscale x 1 x i64> %val, ptr %a, <vscale x 1 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv1i64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e64, m1, ta, ma
; CHECK-NEXT:    vse64.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v1i64.p0(<vscale x 1 x i64> %val, ptr %a, i32 8, <vscale x 1 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v1i64.p0(<vscale x 1 x i64>, ptr, i32, <vscale x 1 x i1>)

define void @masked_store_nxv2i8(<vscale x 2 x i8> %val, ptr %a, <vscale x 2 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv2i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e8, mf4, ta, ma
; CHECK-NEXT:    vse8.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v2i8.p0(<vscale x 2 x i8> %val, ptr %a, i32 1, <vscale x 2 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v2i8.p0(<vscale x 2 x i8>, ptr, i32, <vscale x 2 x i1>)

define void @masked_store_nxv2i16(<vscale x 2 x i16> %val, ptr %a, <vscale x 2 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv2i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e16, mf2, ta, ma
; CHECK-NEXT:    vse16.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v2i16.p0(<vscale x 2 x i16> %val, ptr %a, i32 2, <vscale x 2 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v2i16.p0(<vscale x 2 x i16>, ptr, i32, <vscale x 2 x i1>)

define void @masked_store_nxv2i32(<vscale x 2 x i32> %val, ptr %a, <vscale x 2 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv2i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e32, m1, ta, ma
; CHECK-NEXT:    vse32.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v2i32.p0(<vscale x 2 x i32> %val, ptr %a, i32 4, <vscale x 2 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v2i32.p0(<vscale x 2 x i32>, ptr, i32, <vscale x 2 x i1>)

define void @masked_store_nxv2i64(<vscale x 2 x i64> %val, ptr %a, <vscale x 2 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv2i64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e64, m2, ta, ma
; CHECK-NEXT:    vse64.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v2i64.p0(<vscale x 2 x i64> %val, ptr %a, i32 8, <vscale x 2 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v2i64.p0(<vscale x 2 x i64>, ptr, i32, <vscale x 2 x i1>)

define void @masked_store_nxv4i8(<vscale x 4 x i8> %val, ptr %a, <vscale x 4 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv4i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e8, mf2, ta, ma
; CHECK-NEXT:    vse8.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v4i8.p0(<vscale x 4 x i8> %val, ptr %a, i32 1, <vscale x 4 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v4i8.p0(<vscale x 4 x i8>, ptr, i32, <vscale x 4 x i1>)

define void @masked_store_nxv4i16(<vscale x 4 x i16> %val, ptr %a, <vscale x 4 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv4i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e16, m1, ta, ma
; CHECK-NEXT:    vse16.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v4i16.p0(<vscale x 4 x i16> %val, ptr %a, i32 2, <vscale x 4 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v4i16.p0(<vscale x 4 x i16>, ptr, i32, <vscale x 4 x i1>)

define void @masked_store_nxv4i32(<vscale x 4 x i32> %val, ptr %a, <vscale x 4 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv4i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e32, m2, ta, ma
; CHECK-NEXT:    vse32.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v4i32.p0(<vscale x 4 x i32> %val, ptr %a, i32 4, <vscale x 4 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v4i32.p0(<vscale x 4 x i32>, ptr, i32, <vscale x 4 x i1>)

define void @masked_store_nxv4i64(<vscale x 4 x i64> %val, ptr %a, <vscale x 4 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv4i64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e64, m4, ta, ma
; CHECK-NEXT:    vse64.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v4i64.p0(<vscale x 4 x i64> %val, ptr %a, i32 8, <vscale x 4 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v4i64.p0(<vscale x 4 x i64>, ptr, i32, <vscale x 4 x i1>)

define void @masked_store_nxv8i8(<vscale x 8 x i8> %val, ptr %a, <vscale x 8 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv8i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e8, m1, ta, ma
; CHECK-NEXT:    vse8.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v8i8.p0(<vscale x 8 x i8> %val, ptr %a, i32 1, <vscale x 8 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v8i8.p0(<vscale x 8 x i8>, ptr, i32, <vscale x 8 x i1>)

define void @masked_store_nxv8i16(<vscale x 8 x i16> %val, ptr %a, <vscale x 8 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv8i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e16, m2, ta, ma
; CHECK-NEXT:    vse16.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v8i16.p0(<vscale x 8 x i16> %val, ptr %a, i32 2, <vscale x 8 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v8i16.p0(<vscale x 8 x i16>, ptr, i32, <vscale x 8 x i1>)

define void @masked_store_nxv8i32(<vscale x 8 x i32> %val, ptr %a, <vscale x 8 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv8i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e32, m4, ta, ma
; CHECK-NEXT:    vse32.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v8i32.p0(<vscale x 8 x i32> %val, ptr %a, i32 4, <vscale x 8 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v8i32.p0(<vscale x 8 x i32>, ptr, i32, <vscale x 8 x i1>)

define void @masked_store_nxv8i64(<vscale x 8 x i64> %val, ptr %a, <vscale x 8 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv8i64:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e64, m8, ta, ma
; CHECK-NEXT:    vse64.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v8i64.p0(<vscale x 8 x i64> %val, ptr %a, i32 8, <vscale x 8 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v8i64.p0(<vscale x 8 x i64>, ptr, i32, <vscale x 8 x i1>)

define void @masked_store_nxv16i8(<vscale x 16 x i8> %val, ptr %a, <vscale x 16 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv16i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e8, m2, ta, ma
; CHECK-NEXT:    vse8.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v16i8.p0(<vscale x 16 x i8> %val, ptr %a, i32 1, <vscale x 16 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v16i8.p0(<vscale x 16 x i8>, ptr, i32, <vscale x 16 x i1>)

define void @masked_store_nxv16i16(<vscale x 16 x i16> %val, ptr %a, <vscale x 16 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv16i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e16, m4, ta, ma
; CHECK-NEXT:    vse16.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v16i16.p0(<vscale x 16 x i16> %val, ptr %a, i32 2, <vscale x 16 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v16i16.p0(<vscale x 16 x i16>, ptr, i32, <vscale x 16 x i1>)

define void @masked_store_nxv16i32(<vscale x 16 x i32> %val, ptr %a, <vscale x 16 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv16i32:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e32, m8, ta, ma
; CHECK-NEXT:    vse32.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v16i32.p0(<vscale x 16 x i32> %val, ptr %a, i32 4, <vscale x 16 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v16i32.p0(<vscale x 16 x i32>, ptr, i32, <vscale x 16 x i1>)

define void @masked_store_nxv32i8(<vscale x 32 x i8> %val, ptr %a, <vscale x 32 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv32i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e8, m4, ta, ma
; CHECK-NEXT:    vse8.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v32i8.p0(<vscale x 32 x i8> %val, ptr %a, i32 1, <vscale x 32 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v32i8.p0(<vscale x 32 x i8>, ptr, i32, <vscale x 32 x i1>)

define void @masked_store_nxv32i16(<vscale x 32 x i16> %val, ptr %a, <vscale x 32 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv32i16:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e16, m8, ta, ma
; CHECK-NEXT:    vse16.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v32i16.p0(<vscale x 32 x i16> %val, ptr %a, i32 2, <vscale x 32 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v32i16.p0(<vscale x 32 x i16>, ptr, i32, <vscale x 32 x i1>)

define void @masked_store_nxv64i8(<vscale x 64 x i8> %val, ptr %a, <vscale x 64 x i1> %mask) nounwind {
; CHECK-LABEL: masked_store_nxv64i8:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e8, m8, ta, ma
; CHECK-NEXT:    vse8.v v8, (a0), v0.t
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v64i8.p0(<vscale x 64 x i8> %val, ptr %a, i32 4, <vscale x 64 x i1> %mask)
  ret void
}
declare void @llvm.masked.store.v64i8.p0(<vscale x 64 x i8>, ptr, i32, <vscale x 64 x i1>)

define void @masked_store_zero_mask(<vscale x 2 x i8> %val, ptr %a) nounwind {
; CHECK-LABEL: masked_store_zero_mask:
; CHECK:       # %bb.0:
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v2i8.p0(<vscale x 2 x i8> %val, ptr %a, i32 1, <vscale x 2 x i1> zeroinitializer)
  ret void
}

define void @masked_store_allones_mask(<vscale x 2 x i8> %val, ptr %a) nounwind {
; CHECK-LABEL: masked_store_allones_mask:
; CHECK:       # %bb.0:
; CHECK-NEXT:    vsetvli a1, zero, e8, mf4, ta, ma
; CHECK-NEXT:    vse8.v v8, (a0)
; CHECK-NEXT:    ret
  call void @llvm.masked.store.v2i8.p0(<vscale x 2 x i8> %val, ptr %a, i32 1, <vscale x 2 x i1> splat (i1 1))
  ret void
}
