/*
 * horizontal flip filter
 * Copyright (c) 2007 Benoit Fouet
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

#include "avfilter.h"

typedef struct
{
    int hsub;   /**< chroma subsampling along width */
    int vsub;   /**< chroma subsampling along height */
} FlipContext;

static int query_formats(AVFilterContext *ctx)
{
    enum PixelFormat pix_fmts[] = {
        PIX_FMT_YUV444P,  PIX_FMT_YUV422P,  PIX_FMT_YUV420P,
        PIX_FMT_YUV411P,  PIX_FMT_YUV410P,
        PIX_FMT_YUVJ444P, PIX_FMT_YUVJ422P, PIX_FMT_YUVJ420P,
        PIX_FMT_YUV440P,  PIX_FMT_YUVJ440P,
        PIX_FMT_NONE
    };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(pix_fmts));
    return 0;
}

static int config_props(AVFilterLink *link)
{
    FlipContext *flip = link->dst->priv;

    avcodec_get_chroma_sub_sample(link->format, &flip->hsub, &flip->vsub);

    return 0;
}

static void draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
    FlipContext    *flip = link->dst->priv;
    AVFilterPicRef *in   = link->cur_pic;
    AVFilterPicRef *out  = link->dst->outputs[0]->outpic;
    uint8_t *inrow, *outrow;
    int i, j, plane;

    /* luma plane */
    outrow = out->data[0] + y * out->linesize[0];
    inrow  = in-> data[0] + y * in-> linesize[0] + in->w -1;
    for(i = 0; i < h; i++) {
        for(j = 0; j < link->w; j++)
            outrow[j] = inrow[-j];
        inrow  += in-> linesize[0];
        outrow += out->linesize[0];
    }

    /* chroma planes */
    for(plane = 1; plane < 4; plane++) {
        if (in->data[plane]) {
            outrow = out->data[plane] + (y>>flip->vsub) * out->linesize[plane];
            inrow  = in-> data[plane] + (y>>flip->vsub) * in-> linesize[plane] +
                     (link->w >> flip->hsub) -1;

            for(i = 0; i < h >> flip->vsub; i++) {
                for(j = 0; j < link->w >> flip->hsub; j++)
                    outrow[j] = inrow[-j];
                outrow += out->linesize[plane];
                inrow  += in-> linesize[plane];
            }
        }
    }

    avfilter_draw_slice(link->dst->outputs[0], y, h, slice_dir);
}

AVFilter avfilter_vf_hflip =
{
    .name      = "hflip",
    .priv_size = sizeof(FlipContext),
    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .draw_slice      = draw_slice,
                                    .config_props    = config_props,
                                    .min_perms       = AV_PERM_READ, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO, },
                                  { .name = NULL}},
};

