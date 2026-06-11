; ModuleID = '/Users/theunknownthing/RTComp/bench/out/audio_dsp.ll'
source_filename = "/Users/theunknownthing/RTComp/bench/audio_dsp.cpp"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

@_ZL4gBuf = internal unnamed_addr global [256 x float] zeroinitializer, align 4
@.str = private unnamed_addr constant [44 x i8] c"rtsan_enter=%llu rtsan_exit=%llu iters=%zu\0A\00", align 1
@.str.1 = private unnamed_addr constant [15 x i8] c"rt_nonblocking\00", section "llvm.metadata"
@.str.2 = private unnamed_addr constant [50 x i8] c"/Users/theunknownthing/RTComp/bench/audio_dsp.cpp\00", section "llvm.metadata"
@.str.3 = private unnamed_addr constant [17 x i8] c"rt_nonallocating\00", section "llvm.metadata"
@llvm.global.annotations = appending global [2 x { ptr, ptr, ptr, i32, ptr }] [{ ptr, ptr, ptr, i32, ptr } { ptr @audio_callback, ptr @.str.1, ptr @.str.2, i32 78, ptr null }, { ptr, ptr, ptr, i32, ptr } { ptr @audio_callback, ptr @.str.3, ptr @.str.2, i32 78, ptr null }], section "llvm.metadata"
@llvm.used = appending global [1 x ptr] [ptr @audio_callback], section "llvm.metadata"

; Function Attrs: mustprogress noinline nounwind ssp uwtable(sync)
define void @audio_callback(i32 noundef %0) #0 !rt.effect !10 {
  call void @__rtsan_realtime_enter()
  tail call fastcc void @_ZL10apply_gainPfmf()
  tail call fastcc void @_ZL10map_bufferPfPFvS_E()
  %2 = icmp eq i32 %0, 0
  br i1 %2, label %4, label %3

3:                                                ; preds = %1
  tail call fastcc void @_ZL11heavy_allocv(), !rt.witness !11
  br label %4

4:                                                ; preds = %3, %1
  call void @__rtsan_realtime_exit()
  ret void
}

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind ssp memory(readwrite, argmem: none, inaccessiblemem: none, target_mem0: none, target_mem1: none) uwtable(sync)
define internal fastcc void @_ZL10apply_gainPfmf() unnamed_addr #1 !rt.effect !12 {
  br label %2

1:                                                ; preds = %2
  ret void

2:                                                ; preds = %2, %0
  %3 = phi i64 [ 0, %0 ], [ %8, %2 ]
  %4 = getelementptr inbounds nuw float, ptr @_ZL4gBuf, i64 %3
  %5 = load float, ptr %4, align 4, !tbaa !13
  %6 = fmul float %5, 0x3FE6666660000000
  %7 = tail call fastcc noundef float @_ZL3mixfff(float noundef %5, float noundef %6)
  store float %7, ptr %4, align 4, !tbaa !13
  %8 = add nuw nsw i64 %3, 1
  %9 = icmp eq i64 %8, 256
  br i1 %9, label %1, label %2, !llvm.loop !15
}

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind ssp memory(readwrite, argmem: none, inaccessiblemem: none, target_mem0: none, target_mem1: none) uwtable(sync)
define internal fastcc void @_ZL10map_bufferPfPFvS_E() unnamed_addr #1 !rt.effect !17 {
  br label %2

1:                                                ; preds = %11
  ret void

2:                                                ; preds = %11, %0
  %3 = phi i64 [ 0, %0 ], [ %12, %11 ]
  %4 = getelementptr inbounds nuw float, ptr @_ZL4gBuf, i64 %3
  %5 = load float, ptr %4, align 4, !tbaa !13
  %6 = fcmp ogt float %5, 1.000000e+00
  br i1 %6, label %9, label %7

7:                                                ; preds = %2
  %8 = fcmp olt float %5, -1.000000e+00
  br i1 %8, label %9, label %11

9:                                                ; preds = %7, %2
  %10 = phi float [ 1.000000e+00, %2 ], [ -1.000000e+00, %7 ]
  store float %10, ptr %4, align 4, !tbaa !13
  br label %11

11:                                               ; preds = %9, %7
  %12 = add nuw nsw i64 %3, 1
  %13 = icmp eq i64 %12, 256
  br i1 %13, label %1, label %2, !llvm.loop !18
}

; Function Attrs: mustprogress noinline nounwind ssp uwtable(sync)
define internal fastcc void @_ZL11heavy_allocv() unnamed_addr #0 !rt.effect !19 {
  %1 = tail call dereferenceable_or_null(64) ptr @malloc(i64 noundef 64) #9, !rt.witness !20
  tail call void asm sideeffect "", "r,~{memory}"(ptr %1) #10, !srcloc !21
  tail call void @free(ptr noundef %1), !rt.witness !22
  ret void
}

; Function Attrs: mustprogress norecurse ssp uwtable(sync)
define noundef i32 @main(i32 noundef %0, ptr noundef readnone captures(none) %1) local_unnamed_addr #2 !rt.effect !23 {
  %3 = icmp eq ptr @__rtsan_realtime_reset_counts, null
  br i1 %3, label %5, label %4

4:                                                ; preds = %2
  tail call void @__rtsan_realtime_reset_counts(), !rt.witness !24
  br label %5

5:                                                ; preds = %4, %2
  br label %6

6:                                                ; preds = %6, %5
  %7 = phi i64 [ %13, %6 ], [ 0, %5 ]
  %8 = phi <4 x i64> [ %14, %6 ], [ <i64 0, i64 1, i64 2, i64 3>, %5 ]
  %9 = and <4 x i64> %8, splat (i64 31)
  %10 = uitofp nneg <4 x i64> %9 to <4 x float>
  %11 = fmul <4 x float> %10, splat (float 3.125000e-02)
  %12 = getelementptr inbounds nuw float, ptr @_ZL4gBuf, i64 %7
  store <4 x float> %11, ptr %12, align 4, !tbaa !13
  %13 = add nuw i64 %7, 4
  %14 = add nuw nsw <4 x i64> %8, splat (i64 4)
  %15 = icmp eq i64 %13, 256
  br i1 %15, label %16, label %6, !llvm.loop !25

16:                                               ; preds = %6
  %17 = icmp sgt i32 %0, 1
  br label %20

18:                                               ; preds = %27
  %19 = icmp eq ptr @__rtsan_realtime_enter_count, null
  br i1 %19, label %33, label %31

20:                                               ; preds = %27, %16
  %21 = phi i64 [ 0, %16 ], [ %29, %27 ]
  br i1 %17, label %22, label %27

22:                                               ; preds = %20
  %23 = trunc nuw nsw i64 %21 to i32
  %24 = urem i32 %23, 1000
  %25 = icmp eq i32 %24, 0
  %26 = zext i1 %25 to i32
  br label %27

27:                                               ; preds = %22, %20
  %28 = phi i32 [ 0, %20 ], [ %26, %22 ]
  tail call void @audio_callback(i32 noundef %28), !rt.witness !28
  %29 = add nuw nsw i64 %21, 1
  %30 = icmp eq i64 %29, 200000
  br i1 %30, label %18, label %20, !llvm.loop !29

31:                                               ; preds = %18
  %32 = tail call i64 @__rtsan_realtime_enter_count(), !rt.witness !24
  br label %33

33:                                               ; preds = %31, %18
  %34 = phi i64 [ %32, %31 ], [ 0, %18 ]
  %35 = icmp eq ptr @__rtsan_realtime_exit_count, null
  br i1 %35, label %38, label %36

36:                                               ; preds = %33
  %37 = tail call i64 @__rtsan_realtime_exit_count(), !rt.witness !24
  br label %38

38:                                               ; preds = %36, %33
  %39 = phi i64 [ %37, %36 ], [ 0, %33 ]
  %40 = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, i64 noundef %34, i64 noundef %39, i64 noundef 200000), !rt.witness !30
  ret i32 0
}

declare extern_weak void @__rtsan_realtime_reset_counts() #3

declare extern_weak i64 @__rtsan_realtime_enter_count() #3

declare extern_weak i64 @__rtsan_realtime_exit_count() #3

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr noundef readonly captures(none), ...) local_unnamed_addr #4

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync)
define internal fastcc noundef float @_ZL3mixfff(float noundef %0, float noundef %1) unnamed_addr #5 !rt.effect !17 {
  %3 = fmul float %1, 5.000000e-01
  %4 = tail call float @llvm.fmuladd.f32(float %0, float 5.000000e-01, float %3)
  ret float %4
}

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.fmuladd.f32(float, float, float) #6

; Function Attrs: mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite)
declare noalias noundef ptr @malloc(i64 noundef) local_unnamed_addr #7

; Function Attrs: mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite)
declare void @free(ptr allocptr noundef captures(none)) local_unnamed_addr #8

declare void @__rtsan_realtime_enter()

declare void @__rtsan_realtime_exit()

attributes #0 = { mustprogress noinline nounwind ssp uwtable(sync) "frame-pointer"="non-leaf-no-reserve" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #1 = { mustprogress nofree noinline norecurse nosync nounwind ssp memory(readwrite, argmem: none, inaccessiblemem: none, target_mem0: none, target_mem1: none) uwtable(sync) "frame-pointer"="non-leaf-no-reserve" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #2 = { mustprogress norecurse ssp uwtable(sync) "frame-pointer"="non-leaf-no-reserve" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #3 = { "frame-pointer"="non-leaf-no-reserve" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #4 = { nofree nounwind "frame-pointer"="non-leaf-no-reserve" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #5 = { mustprogress nofree noinline norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync) "frame-pointer"="non-leaf-no-reserve" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #6 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #7 = { mustprogress nofree nounwind willreturn allockind("alloc,uninitialized") allocsize(0) memory(inaccessiblemem: readwrite) "alloc-family"="malloc" "frame-pointer"="non-leaf-no-reserve" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #8 = { mustprogress nounwind willreturn allockind("free") memory(argmem: readwrite, inaccessiblemem: readwrite) "alloc-family"="malloc" "frame-pointer"="non-leaf-no-reserve" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }
attributes #9 = { allocsize(0) }
attributes #10 = { nounwind }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}
!llvm.errno.tbaa = !{!6}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 15, i32 5]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 8, !"PIC Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 1}
!4 = !{i32 7, !"frame-pointer", i32 4}
!5 = !{!"Homebrew clang version 22.1.1"}
!6 = !{!7, !7, i64 0}
!7 = !{!"int", !8, i64 0}
!8 = !{!"omnipotent char", !9, i64 0}
!9 = !{!"Simple C++ TBAA"}
!10 = !{!"rt.effect.v2", i32 1, i32 3, i1 false, !"", i1 true, !"_ZL11heavy_allocv -> free", !"audio_callback|_ZL11heavy_allocv|call||0|0|  tail call fastcc void @_ZL11heavy_allocv()\0A_ZL11heavy_allocv|free|external||0|0|  tail call void @free(ptr noundef %1)", i1 true, !"_ZL11heavy_allocv -> malloc", !"audio_callback|_ZL11heavy_allocv|call||0|0|  tail call fastcc void @_ZL11heavy_allocv()\0A_ZL11heavy_allocv|malloc|external||0|0|  %1 = tail call dereferenceable_or_null(64) ptr @malloc(i64 noundef 64) #9", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 true, !"_ZL11heavy_allocv -> malloc", !"audio_callback|_ZL11heavy_allocv|call||0|0|  tail call fastcc void @_ZL11heavy_allocv()\0A_ZL11heavy_allocv|malloc|external||0|0|  %1 = tail call dereferenceable_or_null(64) ptr @malloc(i64 noundef 64) #9", i1 false, !"", !""}
!11 = !{!"may_alloc", !"may_block", !"may_signal_unsafe"}
!12 = !{!"rt.effect.v2", i32 2, i32 2, i1 false, !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !""}
!13 = !{!14, !14, i64 0}
!14 = !{!"float", !8, i64 0}
!15 = distinct !{!15, !16}
!16 = !{!"llvm.loop.mustprogress"}
!17 = !{!"rt.effect.v2", i32 2, i32 1, i1 false, !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !""}
!18 = distinct !{!18, !16}
!19 = !{!"rt.effect.v2", i32 2, i32 1, i1 false, !"", i1 true, !"free", !"_ZL11heavy_allocv|free|external||0|0|  tail call void @free(ptr noundef %1)", i1 true, !"malloc", !"_ZL11heavy_allocv|malloc|external||0|0|  %1 = tail call dereferenceable_or_null(64) ptr @malloc(i64 noundef 64) #9", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 true, !"malloc", !"_ZL11heavy_allocv|malloc|external||0|0|  %1 = tail call dereferenceable_or_null(64) ptr @malloc(i64 noundef 64) #9", i1 false, !"", !""}
!20 = !{!"may_alloc", !"may_signal_unsafe"}
!21 = !{i64 2234}
!22 = !{!"may_block"}
!23 = !{!"rt.effect.v2", i32 2, i32 4, i1 false, !"", i1 true, !"printf", !"main|printf|external||0|0|  %40 = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, i64 noundef %34, i64 noundef %39, i64 noundef 200000)", i1 true, !"audio_callback -> _ZL11heavy_allocv -> malloc", !"main|audio_callback|call||0|0|  tail call void @audio_callback(i32 noundef %28)\0Aaudio_callback|_ZL11heavy_allocv|call||0|0|  tail call fastcc void @_ZL11heavy_allocv()\0A_ZL11heavy_allocv|malloc|external||0|0|  %1 = tail call dereferenceable_or_null(64) ptr @malloc(i64 noundef 64) #9", i1 false, !"", !"", i1 false, !"", !"", i1 false, !"", !"", i1 true, !"printf", !"main|printf|external||0|0|  %40 = tail call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str, i64 noundef %34, i64 noundef %39, i64 noundef 200000)", i1 true, !"__rtsan_realtime_reset_counts", !"main|__rtsan_realtime_reset_counts|unknown-external||0|0|  tail call void @__rtsan_realtime_reset_counts()"}
!24 = !{!"unknown"}
!25 = distinct !{!25, !16, !26, !27}
!26 = !{!"llvm.loop.isvectorized", i32 1}
!27 = !{!"llvm.loop.unroll.runtime.disable"}
!28 = !{!"may_alloc"}
!29 = distinct !{!29, !16}
!30 = !{!"may_block", !"may_signal_unsafe"}
