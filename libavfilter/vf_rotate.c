/*
 * rotation filter
 * Copyright (c) 2008 Vitor Sessak
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

/**
 * @file libavfilter/vf_rotate.c
 * rotation filter
 *
 * @todo copy code from rotozoom.c to remove use of floating-point
 * @todo handle packed pixel formats
 * @todo make backcolor configurable
*/

#include <math.h>
#include "avfilter.h"

typedef struct
{
    int ang;
    int hsub, vsub;
    float transx, transy; ///< how much to translate (in pixels)
    float sinx, cosx;
    int output_h, output_w;
    int backcolor[3];
} RotContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    RotContext *rot = ctx->priv;

    /* default to 45 degrees */
    rot->ang = 45;

    if(args)
        sscanf(args, "%d", &rot->ang);

    return 0;
}

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

static int config_props_input(AVFilterLink *link)
{
    RotContext *rot = link->dst->priv;

    avcodec_get_chroma_sub_sample(link->format, &rot->hsub, &rot->vsub);

    rot->backcolor[0] = 16;
    rot->backcolor[1] = 128;
    rot->backcolor[2] = 128;
    return 0;
}

static int config_props_output(AVFilterLink *link)
{
    RotContext *rot = link->src->priv;

    rot->sinx = sin(rot->ang*M_PI/180.);
    rot->cosx = cos(rot->ang*M_PI/180.);

    rot->transx = FFMAX(0,  link->src->inputs[0]->h * rot->sinx) +
        FFMAX(0, -link->src->inputs[0]->w*rot->cosx);

    rot->transy = FFMAX(0, -link->src->inputs[0]->h * rot->cosx) +
        FFMAX(0, -link->src->inputs[0]->w*rot->sinx);

    rot->output_w = rot->transx + FFMAX(0, rot->cosx*link->src->inputs[0]->w) +
        FFMAX(0, -rot->sinx*link->src->inputs[0]->h);

    rot->output_h = rot->transy + FFMAX(0, rot->cosx*link->src->inputs[0]->h) +
        FFMAX(0,  rot->sinx*link->src->inputs[0]->w);

    link->w = rot->output_w;
    link->h = rot->output_h;

    return 0;
}

static void draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
}

static void end_frame(AVFilterLink *link)
{
    RotContext *rot = link->dst->priv;
    AVFilterPicRef *in  = link->cur_pic;
    AVFilterPicRef *out = link->dst->outputs[0]->outpic;
    int i, j, plane;

    /* luma plane */
    for(i = 0; i < rot->output_h; i++)
        for(j = 0; j < rot->output_w; j++) {
            int line   = (i - rot->transy)*rot->sinx +
                (j - rot->transx)*rot->cosx + 0.5;

            int column = (i - rot->transy)*rot->cosx -
                (j - rot->transx)*rot->sinx + 0.5;

            if (line < 0 || line >= in->w || column < 0 || column >= in->h)
                *(out->data[0] +   i*out->linesize[0] + j) = rot->backcolor[0];
            else
                *(out->data[0] +   i*out->linesize[0] + j) =
                    *(in->data[0] + column*in->linesize[0] + line);
        }

    /* chroma planes */
    for(plane = 1; plane < 3; plane ++)
        for(i = 0 >> rot->vsub; i < rot->output_h >> rot->vsub; i++)
            for(j = 0; j < rot->output_w >> rot->hsub; j++) {
                int i2 = (i + rot->vsub/2) << rot->vsub;
                int j2 = (j + rot->hsub/2) << rot->hsub;

                int line =   (i2 - rot->transy)*rot->sinx +
                    (j2 - rot->transx)*rot->cosx + 0.5;

                int column = (i2 - rot->transy)*rot->cosx -
                    (j2 - rot->transx)*rot->sinx + 0.5;

                if (line < 0 || line >= in->w || column < 0 || column >= in->h) {
                    *(out->data[plane] +   i*out->linesize[plane] + j) =
                        rot->backcolor[plane];
                } else {
                    line   = (line   + rot->hsub/2) >> rot->hsub;
                    column = (column + rot->vsub/2) >> rot->vsub;

                    *(out->data[plane] +   i*out->linesize[plane] + j) =
                        *(in->data[plane] + column*in->linesize[plane] + line);
                }
            }

    avfilter_unref_pic(in);
    avfilter_draw_slice(link->dst->outputs[0], 0, rot->output_h, 1);
    avfilter_end_frame(link->dst->outputs[0]);
    avfilter_unref_pic(out);
}

static void start_frame(AVFilterLink *link, AVFilterPicRef *picref)
{
    AVFilterLink *out = link->dst->outputs[0];

    out->outpic      = avfilter_get_video_buffer(out, AV_PERM_WRITE, out->w, out->h);
    out->outpic->pts = picref->pts;
    out->outpic->pos = picref->pos;

    if(picref->pixel_aspect.num == 0) {
        out->outpic->pixel_aspect = picref->pixel_aspect;
    } else {
        out->outpic->pixel_aspect.num = picref->pixel_aspect.den;
        out->outpic->pixel_aspect.den = picref->pixel_aspect.num;
    }

    avfilter_start_frame(out, avfilter_ref_pic(out->outpic, ~0));
}

AVFilter avfilter_vf_rotate =
{
    .name      = "rotate",

    .init      = init,

    .priv_size = sizeof(RotContext),

    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .start_frame     = start_frame,
                                    .draw_slice      = draw_slice,
                                    .end_frame       = end_frame,
                                    .config_props    = config_props_input,
                                    .min_perms       = AV_PERM_READ, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .config_props    = config_props_output,
                                    .type            = AVMEDIA_TYPE_VIDEO, },
                                  { .name = NULL}},
};

