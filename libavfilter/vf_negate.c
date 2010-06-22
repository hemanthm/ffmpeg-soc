/*
 * video negative filter
 * copyright (c) 2007 Bobby Bingham
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
    int offY, offUV;
    int hsub, vsub;
} NegContext;

static int query_formats(AVFilterContext *ctx)
{
    enum PixelFormat pix_fmts[] = {
        PIX_FMT_YUV444P,  PIX_FMT_YUV422P,  PIX_FMT_YUV420P,
        PIX_FMT_YUV411P,  PIX_FMT_YUV410P,
        PIX_FMT_YUVJ444P, PIX_FMT_YUVJ422P, PIX_FMT_YUVJ420P,
        PIX_FMT_YUV440P,  PIX_FMT_YUVJ440P,
        PIX_FMT_MONOWHITE, PIX_FMT_MONOBLACK,
        PIX_FMT_NONE
    };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(pix_fmts));
    return 0;
}

static int config_props(AVFilterLink *link)
{
    NegContext *neg = link->dst->priv;

    avcodec_get_chroma_sub_sample(link->format, &neg->hsub, &neg->vsub);

    switch(link->format) {
    case PIX_FMT_YUVJ444P:
    case PIX_FMT_YUVJ422P:
    case PIX_FMT_YUVJ420P:
    case PIX_FMT_YUVJ440P:
        neg->offY  =
        neg->offUV = 0;
        break;
    default:
        neg->offY  = -4;
        neg->offUV = 1;
    }

    return 0;
}

static void draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
    NegContext *neg = link->dst->priv;
    AVFilterPicRef *in  = link->cur_pic;
    AVFilterPicRef *out = link->dst->outputs[0]->outpic;
    uint8_t *inrow, *outrow;
    int i, j, plane;

    if (link->format == PIX_FMT_MONOWHITE || link->format == PIX_FMT_MONOBLACK) {
        inrow  = in ->data[0] + y * in ->linesize[0];
        outrow = out->data[0] + y * out->linesize[0];
        for (i = 0; i < h; i++) {
            for (j = 0; j < link->w >> 3; j++)
                outrow[j] = ~inrow[j];
            inrow  += in-> linesize[0];
            outrow += out->linesize[0];
        }
    } else {
        /* luma plane */
        inrow  = in-> data[0] + y * in-> linesize[0];
        outrow = out->data[0] + y * out->linesize[0];
        for(i = 0; i < h; i ++) {
            for(j = 0; j < link->w; j ++)
                outrow[j] = 255 - inrow[j] + neg->offY;
            inrow  += in-> linesize[0];
            outrow += out->linesize[0];
        }

        /* chroma planes */
        for(plane = 1; plane < 3; plane ++) {
            inrow  = in-> data[plane] + (y >> neg->vsub) * in-> linesize[plane];
            outrow = out->data[plane] + (y >> neg->vsub) * out->linesize[plane];

            for(i = 0; i < h >> neg->vsub; i ++) {
                for(j = 0; j < link->w >> neg->hsub; j ++)
                    outrow[j] = 255 - inrow[j] + neg->offUV;
                inrow  += in-> linesize[plane];
                outrow += out->linesize[plane];
            }
        }
    }

    avfilter_draw_slice(link->dst->outputs[0], y, h, slice_dir);
}

AVFilter avfilter_vf_negate =
{
    .name      = "negate",

    .priv_size = sizeof(NegContext),

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

