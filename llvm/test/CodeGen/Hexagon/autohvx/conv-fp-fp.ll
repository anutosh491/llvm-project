; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=hexagon < %s | FileCheck %s

define <64 x half> @f0(<64 x float> %a0) #0 {
; CHECK-LABEL: f0:
; CHECK:       // %bb.0: // %b0
; CHECK-NEXT:    {
; CHECK-NEXT:     v2 = vxor(v2,v2)
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v0.qf32 = vadd(v0.sf,v2.sf)
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v1.qf32 = vadd(v1.sf,v2.sf)
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v0.hf = v1:0.qf32
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v0.h = vdeal(v0.h)
; CHECK-NEXT:     jumpr r31
; CHECK-NEXT:    }
b0:
  %v0 = fptrunc <64 x float> %a0 to <64 x half>
  ret <64 x half> %v0
}

define <64 x float> @f1(<64 x half> %a0) #0 {
; CHECK-LABEL: f1:
; CHECK:       // %bb.0: // %b0
; CHECK-NEXT:    {
; CHECK-NEXT:     r0 = #15360
; CHECK-NEXT:     r7 = #-4
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v1.h = vsplat(r0)
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v1:0.qf32 = vmpy(v0.hf,v1.hf)
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v0.sf = v0.qf32
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v1.sf = v1.qf32
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v1:0 = vshuff(v1,v0,r7)
; CHECK-NEXT:     jumpr r31
; CHECK-NEXT:    }
b0:
  %v0 = fpext <64 x half> %a0 to <64 x float>
  ret <64 x float> %v0
}

define <64 x half> @f2(<64 x float> %a0) #1 {
; CHECK-LABEL: f2:
; CHECK:       // %bb.0: // %b0
; CHECK-NEXT:    {
; CHECK-NEXT:     v0.hf = vcvt(v1.sf,v0.sf)
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v0.h = vdeal(v0.h)
; CHECK-NEXT:     jumpr r31
; CHECK-NEXT:    }
b0:
  %v0 = fptrunc <64 x float> %a0 to <64 x half>
  ret <64 x half> %v0
}

define <64 x float> @f3(<64 x half> %a0) #1 {
; CHECK-LABEL: f3:
; CHECK:       // %bb.0: // %b0
; CHECK-NEXT:    {
; CHECK-NEXT:     v0.h = vshuff(v0.h)
; CHECK-NEXT:    }
; CHECK-NEXT:    {
; CHECK-NEXT:     v1:0.sf = vcvt(v0.hf)
; CHECK-NEXT:     jumpr r31
; CHECK-NEXT:    }
b0:
  %v0 = fpext <64 x half> %a0 to <64 x float>
  ret <64 x float> %v0
}

attributes #0 = { nounwind "target-features"="+hvxv69,+hvx-length128b,+hvx-qfloat" }
attributes #1 = { nounwind "target-features"="+hvxv69,+hvx-length128b,+hvx-ieee-fp,-hvx-qfloat" }
