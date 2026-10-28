/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bilinear BGR0 fit-and-pad kernel, written in baseline PTX (no CUDA toolkit
 * or runtime compiler dependency). One thread writes one complete output
 * pixel, including black padding; reads only encoder-owned pitched storage.
 * Sampling: clamp((destination + .5) * source / fitted_size - .5).
 */
#pragma once

namespace TrophyBackend {
inline constexpr char cudaScalePtx[] = R"ptx(
.version 6.0
.target sm_30
.address_size 64

.visible .entry trophyScaleBgr0(
    .param .u64 src, .param .u64 srcPitch,
    .param .u32 sw, .param .u32 sh,
    .param .u64 dst, .param .u64 dstPitch,
    .param .u32 dw, .param .u32 dh,
    .param .u32 fx, .param .u32 fy,
    .param .u32 fw, .param .u32 fh)
{
    .reg .pred %p<8>;
    .reg .b32 %r<40>;
    .reg .b64 %rd<24>;
    .reg .f32 %f<32>;

    ld.param.u64 %rd0, [src];
    ld.param.u64 %rd1, [srcPitch];
    ld.param.u64 %rd2, [dst];
    ld.param.u64 %rd3, [dstPitch];
    ld.param.u32 %r0, [sw];
    ld.param.u32 %r1, [sh];
    ld.param.u32 %r2, [dw];
    ld.param.u32 %r3, [dh];
    ld.param.u32 %r4, [fx];
    ld.param.u32 %r5, [fy];
    ld.param.u32 %r6, [fw];
    ld.param.u32 %r7, [fh];
    mov.u32 %r8, %ctaid.x;
    mov.u32 %r9, %ntid.x;
    mov.u32 %r10, %tid.x;
    mad.lo.u32 %r8, %r8, %r9, %r10;
    mov.u32 %r9, %ctaid.y;
    mov.u32 %r10, %ntid.y;
    mov.u32 %r11, %tid.y;
    mad.lo.u32 %r9, %r9, %r10, %r11;
    setp.ge.u32 %p0, %r8, %r2;
    setp.ge.u32 %p1, %r9, %r3;
    or.pred %p0, %p0, %p1;
    @%p0 ret;

    cvt.u64.u32 %rd4, %r9;
    mul.lo.u64 %rd4, %rd4, %rd3;
    mul.wide.u32 %rd5, %r8, 4;
    add.u64 %rd4, %rd4, %rd5;
    add.u64 %rd4, %rd4, %rd2;
    mov.u32 %r10, 0;
    st.global.u32 [%rd4], %r10;
    sub.s32 %r10, %r8, %r4;
    sub.s32 %r11, %r9, %r5;
    // Unsigned comparisons reject negative coordinates as well.
    setp.ge.u32 %p0, %r10, %r6;
    setp.ge.u32 %p1, %r11, %r7;
    or.pred %p0, %p0, %p1;
    @%p0 ret;

    cvt.rn.f32.u32 %f0, %r0;
    cvt.rn.f32.u32 %f1, %r1;
    cvt.rn.f32.u32 %f2, %r6;
    cvt.rn.f32.u32 %f3, %r7;
    div.rn.f32 %f0, %f0, %f2;
    div.rn.f32 %f1, %f1, %f3;
    cvt.rn.f32.u32 %f2, %r10;
    cvt.rn.f32.u32 %f3, %r11;
    add.f32 %f2, %f2, 0f3f000000;
    add.f32 %f3, %f3, 0f3f000000;
    mul.f32 %f2, %f2, %f0;
    mul.f32 %f3, %f3, %f1;
    sub.f32 %f2, %f2, 0f3f000000;
    sub.f32 %f3, %f3, 0f3f000000;
    sub.u32 %r12, %r0, 1;
    sub.u32 %r13, %r1, 1;
    cvt.rn.f32.u32 %f4, %r12;
    cvt.rn.f32.u32 %f5, %r13;
    max.f32 %f2, %f2, 0f00000000;
    max.f32 %f3, %f3, 0f00000000;
    min.f32 %f2, %f2, %f4;
    min.f32 %f3, %f3, %f5;
    cvt.rzi.u32.f32 %r14, %f2;
    cvt.rzi.u32.f32 %r15, %f3;
    add.u32 %r16, %r14, 1;
    add.u32 %r17, %r15, 1;
    min.u32 %r16, %r16, %r12;
    min.u32 %r17, %r17, %r13;
    cvt.rn.f32.u32 %f4, %r14;
    cvt.rn.f32.u32 %f5, %r15;
    sub.f32 %f6, %f2, %f4;
    sub.f32 %f7, %f3, %f5;

    cvt.u64.u32 %rd6, %r15;
    cvt.u64.u32 %rd7, %r17;
    mul.lo.u64 %rd6, %rd6, %rd1;
    mul.lo.u64 %rd7, %rd7, %rd1;
    add.u64 %rd6, %rd6, %rd0;
    add.u64 %rd7, %rd7, %rd0;
    mul.wide.u32 %rd8, %r14, 4;
    mul.wide.u32 %rd9, %r16, 4;
    add.u64 %rd10, %rd6, %rd8;
    add.u64 %rd11, %rd6, %rd9;
    add.u64 %rd12, %rd7, %rd8;
    add.u64 %rd13, %rd7, %rd9;
    mov.u32 %r18, 0;
channel:
    ld.global.u8 %r19, [%rd10];
    ld.global.u8 %r20, [%rd11];
    ld.global.u8 %r21, [%rd12];
    ld.global.u8 %r22, [%rd13];
    cvt.rn.f32.u32 %f8, %r19;
    cvt.rn.f32.u32 %f9, %r20;
    cvt.rn.f32.u32 %f10, %r21;
    cvt.rn.f32.u32 %f11, %r22;
    sub.f32 %f9, %f9, %f8;
    sub.f32 %f11, %f11, %f10;
    fma.rn.f32 %f8, %f9, %f6, %f8;
    fma.rn.f32 %f10, %f11, %f6, %f10;
    sub.f32 %f10, %f10, %f8;
    fma.rn.f32 %f8, %f10, %f7, %f8;
    cvt.rni.u32.f32 %r19, %f8;
    st.global.u8 [%rd4], %r19;
    add.u64 %rd10, %rd10, 1;
    add.u64 %rd11, %rd11, 1;
    add.u64 %rd12, %rd12, 1;
    add.u64 %rd13, %rd13, 1;
    add.u64 %rd4, %rd4, 1;
    add.u32 %r18, %r18, 1;
    setp.lt.u32 %p0, %r18, 3;
    @%p0 bra channel;
    ret;
}
)ptx";
} // namespace TrophyBackend
