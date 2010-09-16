/*
 * Copyright (c) 2004-2005 Michael Niedermayer, Loren Merritt
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/cpu.h"
#include "libavutil/x86_cpu.h"
#include "libavcodec/h264dsp.h"
#include "dsputil_mmx.h"

DECLARE_ALIGNED(8, static const uint64_t, ff_pb_3_1  ) = 0x0103010301030103ULL;
DECLARE_ALIGNED(8, static const uint64_t, ff_pb_7_3  ) = 0x0307030703070307ULL;

/***********************************/
/* IDCT */

void ff_h264_idct_add_mmx     (uint8_t *dst, int16_t *block, int stride);
void ff_h264_idct8_add_mmx    (uint8_t *dst, int16_t *block, int stride);
void ff_h264_idct8_add_sse2   (uint8_t *dst, int16_t *block, int stride);
void ff_h264_idct_dc_add_mmx2 (uint8_t *dst, int16_t *block, int stride);
void ff_h264_idct8_dc_add_mmx2(uint8_t *dst, int16_t *block, int stride);

void ff_h264_idct_add16_mmx      (uint8_t *dst, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct8_add4_mmx      (uint8_t *dst, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct_add16_mmx2     (uint8_t *dst, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct_add16intra_mmx (uint8_t *dst, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct_add16intra_mmx2(uint8_t *dst, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct8_add4_mmx2     (uint8_t *dst, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct8_add4_sse2     (uint8_t *dst, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct_add8_mmx       (uint8_t **dest, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct_add8_mmx2      (uint8_t **dest, const int *block_offset,
                                  DCTELEM *block, int stride, const uint8_t nnzc[6*8]);

void ff_h264_idct_add16_sse2     (uint8_t *dst, const int *block_offset, DCTELEM *block,
                                  int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct_add16intra_sse2(uint8_t *dst, const int *block_offset, DCTELEM *block,
                                  int stride, const uint8_t nnzc[6*8]);
void ff_h264_idct_add8_sse2      (uint8_t **dest, const int *block_offset, DCTELEM *block,
                                  int stride, const uint8_t nnzc[6*8]);

/***********************************/
/* deblocking */

static void h264_loop_filter_strength_mmx2( int16_t bS[2][4][4], uint8_t nnz[40], int8_t ref[2][40], int16_t mv[2][40][2],
                                            int bidir, int edges, int step, int mask_mv0, int mask_mv1, int field ) {
    int dir;
    __asm__ volatile(
        "movq %0, %%mm7 \n"
        "movq %1, %%mm6 \n"
        ::"m"(ff_pb_1), "m"(ff_pb_3)
    );
    if(field)
        __asm__ volatile(
            "movq %0, %%mm6 \n"
            ::"m"(ff_pb_3_1)
        );
    __asm__ volatile(
        "movq  %%mm6, %%mm5 \n"
        "paddb %%mm5, %%mm5 \n"
    :);

    // could do a special case for dir==0 && edges==1, but it only reduces the
    // average filter time by 1.2%
    for( dir=1; dir>=0; dir-- ) {
        const x86_reg d_idx = dir ? -8 : -1;
        const int mask_mv = dir ? mask_mv1 : mask_mv0;
        DECLARE_ALIGNED(8, const uint64_t, mask_dir) = dir ? 0 : 0xffffffffffffffffULL;
        int b_idx, edge;
        for( b_idx=12, edge=0; edge<edges; edge+=step, b_idx+=8*step ) {
            __asm__ volatile(
                "pand %0, %%mm0 \n\t"
                ::"m"(mask_dir)
            );
            if(!(mask_mv & edge)) {
                if(bidir) {
                    __asm__ volatile(
                        "movd         (%1,%0), %%mm2 \n"
                        "punpckldq  40(%1,%0), %%mm2 \n" // { ref0[bn], ref1[bn] }
                        "pshufw $0x44,   (%1), %%mm0 \n" // { ref0[b], ref0[b] }
                        "pshufw $0x44, 40(%1), %%mm1 \n" // { ref1[b], ref1[b] }
                        "pshufw $0x4E, %%mm2, %%mm3 \n"
                        "psubb         %%mm2, %%mm0 \n" // { ref0[b]!=ref0[bn], ref0[b]!=ref1[bn] }
                        "psubb         %%mm3, %%mm1 \n" // { ref1[b]!=ref1[bn], ref1[b]!=ref0[bn] }
                        "1: \n"
                        "por           %%mm1, %%mm0 \n"
                        "movq      (%2,%0,4), %%mm1 \n"
                        "movq     8(%2,%0,4), %%mm2 \n"
                        "movq          %%mm1, %%mm3 \n"
                        "movq          %%mm2, %%mm4 \n"
                        "psubw          (%2), %%mm1 \n"
                        "psubw         8(%2), %%mm2 \n"
                        "psubw       160(%2), %%mm3 \n"
                        "psubw       168(%2), %%mm4 \n"
                        "packsswb      %%mm2, %%mm1 \n"
                        "packsswb      %%mm4, %%mm3 \n"
                        "paddb         %%mm6, %%mm1 \n"
                        "paddb         %%mm6, %%mm3 \n"
                        "psubusb       %%mm5, %%mm1 \n" // abs(mv[b] - mv[bn]) >= limit
                        "psubusb       %%mm5, %%mm3 \n"
                        "packsswb      %%mm3, %%mm1 \n"
                        "add $40, %0 \n"
                        "cmp $40, %0 \n"
                        "jl 1b \n"
                        "sub $80, %0 \n"
                        "pshufw $0x4E, %%mm1, %%mm1 \n"
                        "por           %%mm1, %%mm0 \n"
                        "pshufw $0x4E, %%mm0, %%mm1 \n"
                        "pminub        %%mm1, %%mm0 \n"
                        ::"r"(d_idx),
                          "r"(ref[0]+b_idx),
                          "r"(mv[0]+b_idx)
                    );
                } else {
                    __asm__ volatile(
                        "movd        (%1), %%mm0 \n"
                        "psubb    (%1,%0), %%mm0 \n" // ref[b] != ref[bn]
                        "movq        (%2), %%mm1 \n"
                        "movq       8(%2), %%mm2 \n"
                        "psubw  (%2,%0,4), %%mm1 \n"
                        "psubw 8(%2,%0,4), %%mm2 \n"
                        "packsswb   %%mm2, %%mm1 \n"
                        "paddb      %%mm6, %%mm1 \n"
                        "psubusb    %%mm5, %%mm1 \n" // abs(mv[b] - mv[bn]) >= limit
                        "packsswb   %%mm1, %%mm1 \n"
                        "por        %%mm1, %%mm0 \n"
                        ::"r"(d_idx),
                          "r"(ref[0]+b_idx),
                          "r"(mv[0]+b_idx)
                    );
                }
            }
            __asm__ volatile(
                "movd %0, %%mm1 \n"
                "por  %1, %%mm1 \n" // nnz[b] || nnz[bn]
                ::"m"(nnz[b_idx]),
                  "m"(nnz[b_idx+d_idx])
            );
            __asm__ volatile(
                "pminub    %%mm7, %%mm1 \n"
                "pminub    %%mm7, %%mm0 \n"
                "psllw        $1, %%mm1 \n"
                "pxor      %%mm2, %%mm2 \n"
                "pmaxub    %%mm0, %%mm1 \n"
                "punpcklbw %%mm2, %%mm1 \n"
                "movq      %%mm1, %0    \n"
                :"=m"(*bS[dir][edge])
                ::"memory"
            );
        }
        edges = 4;
        step = 1;
    }
    __asm__ volatile(
        "movq   (%0), %%mm0 \n\t"
        "movq  8(%0), %%mm1 \n\t"
        "movq 16(%0), %%mm2 \n\t"
        "movq 24(%0), %%mm3 \n\t"
        TRANSPOSE4(%%mm0, %%mm1, %%mm2, %%mm3, %%mm4)
        "movq %%mm0,   (%0) \n\t"
        "movq %%mm3,  8(%0) \n\t"
        "movq %%mm4, 16(%0) \n\t"
        "movq %%mm2, 24(%0) \n\t"
        ::"r"(bS[0])
        :"memory"
    );
}

#define LF_FUNC(DIR, TYPE, OPT) \
void ff_x264_deblock_ ## DIR ## _ ## TYPE ## _ ## OPT (uint8_t *pix, int stride, \
                                               int alpha, int beta, int8_t *tc0);
#define LF_IFUNC(DIR, TYPE, OPT) \
void ff_x264_deblock_ ## DIR ## _ ## TYPE ## _ ## OPT (uint8_t *pix, int stride, \
                                               int alpha, int beta);

LF_FUNC (h,  chroma,       mmxext)
LF_IFUNC(h,  chroma_intra, mmxext)
LF_FUNC (v,  chroma,       mmxext)
LF_IFUNC(v,  chroma_intra, mmxext)

LF_FUNC (h,  luma,         mmxext)
LF_IFUNC(h,  luma_intra,   mmxext)
#if HAVE_YASM && ARCH_X86_32
LF_FUNC (v8, luma,         mmxext)
static void ff_x264_deblock_v_luma_mmxext(uint8_t *pix, int stride, int alpha, int beta, int8_t *tc0)
{
    if((tc0[0] & tc0[1]) >= 0)
        ff_x264_deblock_v8_luma_mmxext(pix+0, stride, alpha, beta, tc0);
    if((tc0[2] & tc0[3]) >= 0)
        ff_x264_deblock_v8_luma_mmxext(pix+8, stride, alpha, beta, tc0+2);
}
LF_IFUNC(v8, luma_intra,   mmxext)
static void ff_x264_deblock_v_luma_intra_mmxext(uint8_t *pix, int stride, int alpha, int beta)
{
    ff_x264_deblock_v8_luma_intra_mmxext(pix+0, stride, alpha, beta);
    ff_x264_deblock_v8_luma_intra_mmxext(pix+8, stride, alpha, beta);
}
#endif

LF_FUNC (h,  luma,         sse2)
LF_IFUNC(h,  luma_intra,   sse2)
LF_FUNC (v,  luma,         sse2)
LF_IFUNC(v,  luma_intra,   sse2)

/***********************************/
/* weighted prediction */

#define H264_WEIGHT(W, H, OPT) \
void ff_h264_weight_ ## W ## x ## H ## _ ## OPT(uint8_t *dst, \
    int stride, int log2_denom, int weight, int offset);

#define H264_BIWEIGHT(W, H, OPT) \
void ff_h264_biweight_ ## W ## x ## H ## _ ## OPT(uint8_t *dst, \
    uint8_t *src, int stride, int log2_denom, int weightd, \
    int weights, int offset);

#define H264_BIWEIGHT_MMX(W,H) \
H264_WEIGHT  (W, H, mmx2) \
H264_BIWEIGHT(W, H, mmx2)

#define H264_BIWEIGHT_MMX_SSE(W,H) \
H264_BIWEIGHT_MMX(W, H) \
H264_WEIGHT      (W, H, sse2) \
H264_BIWEIGHT    (W, H, sse2) \
H264_BIWEIGHT    (W, H, ssse3)

H264_BIWEIGHT_MMX_SSE(16, 16)
H264_BIWEIGHT_MMX_SSE(16,  8)
H264_BIWEIGHT_MMX_SSE( 8, 16)
H264_BIWEIGHT_MMX_SSE( 8,  8)
H264_BIWEIGHT_MMX_SSE( 8,  4)
H264_BIWEIGHT_MMX    ( 4,  8)
H264_BIWEIGHT_MMX    ( 4,  4)
H264_BIWEIGHT_MMX    ( 4,  2)

void ff_h264dsp_init_x86(H264DSPContext *c)
{
    int mm_flags = av_get_cpu_flags();

    if (mm_flags & AV_CPU_FLAG_MMX2) {
        c->h264_loop_filter_strength= h264_loop_filter_strength_mmx2;
    }
#if HAVE_YASM
    if (mm_flags & AV_CPU_FLAG_MMX) {
        c->h264_idct_dc_add=
        c->h264_idct_add= ff_h264_idct_add_mmx;
        c->h264_idct8_dc_add=
        c->h264_idct8_add= ff_h264_idct8_add_mmx;

        c->h264_idct_add16     = ff_h264_idct_add16_mmx;
        c->h264_idct8_add4     = ff_h264_idct8_add4_mmx;
        c->h264_idct_add8      = ff_h264_idct_add8_mmx;
        c->h264_idct_add16intra= ff_h264_idct_add16intra_mmx;

        if (mm_flags & AV_CPU_FLAG_MMX2) {
            c->h264_idct_dc_add= ff_h264_idct_dc_add_mmx2;
            c->h264_idct8_dc_add= ff_h264_idct8_dc_add_mmx2;
            c->h264_idct_add16     = ff_h264_idct_add16_mmx2;
            c->h264_idct8_add4     = ff_h264_idct8_add4_mmx2;
            c->h264_idct_add8      = ff_h264_idct_add8_mmx2;
            c->h264_idct_add16intra= ff_h264_idct_add16intra_mmx2;

            c->h264_v_loop_filter_chroma= ff_x264_deblock_v_chroma_mmxext;
            c->h264_h_loop_filter_chroma= ff_x264_deblock_h_chroma_mmxext;
            c->h264_v_loop_filter_chroma_intra= ff_x264_deblock_v_chroma_intra_mmxext;
            c->h264_h_loop_filter_chroma_intra= ff_x264_deblock_h_chroma_intra_mmxext;
#if ARCH_X86_32
            c->h264_v_loop_filter_luma= ff_x264_deblock_v_luma_mmxext;
            c->h264_h_loop_filter_luma= ff_x264_deblock_h_luma_mmxext;
            c->h264_v_loop_filter_luma_intra = ff_x264_deblock_v_luma_intra_mmxext;
            c->h264_h_loop_filter_luma_intra = ff_x264_deblock_h_luma_intra_mmxext;
#endif
            c->weight_h264_pixels_tab[0]= ff_h264_weight_16x16_mmx2;
            c->weight_h264_pixels_tab[1]= ff_h264_weight_16x8_mmx2;
            c->weight_h264_pixels_tab[2]= ff_h264_weight_8x16_mmx2;
            c->weight_h264_pixels_tab[3]= ff_h264_weight_8x8_mmx2;
            c->weight_h264_pixels_tab[4]= ff_h264_weight_8x4_mmx2;
            c->weight_h264_pixels_tab[5]= ff_h264_weight_4x8_mmx2;
            c->weight_h264_pixels_tab[6]= ff_h264_weight_4x4_mmx2;
            c->weight_h264_pixels_tab[7]= ff_h264_weight_4x2_mmx2;

            c->biweight_h264_pixels_tab[0]= ff_h264_biweight_16x16_mmx2;
            c->biweight_h264_pixels_tab[1]= ff_h264_biweight_16x8_mmx2;
            c->biweight_h264_pixels_tab[2]= ff_h264_biweight_8x16_mmx2;
            c->biweight_h264_pixels_tab[3]= ff_h264_biweight_8x8_mmx2;
            c->biweight_h264_pixels_tab[4]= ff_h264_biweight_8x4_mmx2;
            c->biweight_h264_pixels_tab[5]= ff_h264_biweight_4x8_mmx2;
            c->biweight_h264_pixels_tab[6]= ff_h264_biweight_4x4_mmx2;
            c->biweight_h264_pixels_tab[7]= ff_h264_biweight_4x2_mmx2;

            if (mm_flags&AV_CPU_FLAG_SSE2) {
                c->h264_idct8_add = ff_h264_idct8_add_sse2;
                c->h264_idct8_add4= ff_h264_idct8_add4_sse2;

                c->weight_h264_pixels_tab[0]= ff_h264_weight_16x16_sse2;
                c->weight_h264_pixels_tab[1]= ff_h264_weight_16x8_sse2;
                c->weight_h264_pixels_tab[2]= ff_h264_weight_8x16_sse2;
                c->weight_h264_pixels_tab[3]= ff_h264_weight_8x8_sse2;
                c->weight_h264_pixels_tab[4]= ff_h264_weight_8x4_sse2;

                c->biweight_h264_pixels_tab[0]= ff_h264_biweight_16x16_sse2;
                c->biweight_h264_pixels_tab[1]= ff_h264_biweight_16x8_sse2;
                c->biweight_h264_pixels_tab[2]= ff_h264_biweight_8x16_sse2;
                c->biweight_h264_pixels_tab[3]= ff_h264_biweight_8x8_sse2;
                c->biweight_h264_pixels_tab[4]= ff_h264_biweight_8x4_sse2;

#if ARCH_X86_64 || !defined(__ICC) || __ICC > 1110
                c->h264_v_loop_filter_luma = ff_x264_deblock_v_luma_sse2;
                c->h264_h_loop_filter_luma = ff_x264_deblock_h_luma_sse2;
                c->h264_v_loop_filter_luma_intra = ff_x264_deblock_v_luma_intra_sse2;
                c->h264_h_loop_filter_luma_intra = ff_x264_deblock_h_luma_intra_sse2;
#endif
                c->h264_idct_add16 = ff_h264_idct_add16_sse2;
                c->h264_idct_add8  = ff_h264_idct_add8_sse2;
                c->h264_idct_add16intra = ff_h264_idct_add16intra_sse2;
            }
            if (mm_flags&AV_CPU_FLAG_SSSE3) {
                c->biweight_h264_pixels_tab[0]= ff_h264_biweight_16x16_ssse3;
                c->biweight_h264_pixels_tab[1]= ff_h264_biweight_16x8_ssse3;
                c->biweight_h264_pixels_tab[2]= ff_h264_biweight_8x16_ssse3;
                c->biweight_h264_pixels_tab[3]= ff_h264_biweight_8x8_ssse3;
                c->biweight_h264_pixels_tab[4]= ff_h264_biweight_8x4_ssse3;
            }
        }
    }
#endif
}
