/*
 * copyright (c) 2007 Bobby Bingham
 * copyright (c) 2010 Baptiste Coudurier
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
 * @file
 * filter to overlay one video on top of another
 */

#include "avfilter.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"

static const char *var_names[] = {
    "main_w",    ///< width of the main video
    "main_h",    ///< height of the main video
    "overlay_w", ///< width of the overlay video
    "overlay_h", ///< height of the overlay video
    NULL
};

enum var_name {
    MAIN_W,
    MAIN_H,
    OVERLAY_W,
    OVERLAY_H,
    VARS_NB
};

typedef struct {
    unsigned x, y;              //< position of subpicture

    AVFilterPicRef *overlay;

    int bpp;                    //< bytes per pixel
    int hsub, vsub;             //< chroma subsampling

    char x_expr[256], y_expr[256];
} OverlayContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    OverlayContext *over = ctx->priv;

    av_strlcpy(over->x_expr, "0", sizeof(over->x_expr));
    av_strlcpy(over->y_expr, "0", sizeof(over->y_expr));

    if (args)
        sscanf(args, "%255[^:]:%255[^:]", over->x_expr, over->y_expr);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    OverlayContext *over = ctx->priv;

    if (over->overlay)
        avfilter_unref_pic(over->overlay);
}

static int query_formats(AVFilterContext *ctx)
{
    //const enum PixelFormat inout_pix_fmts[] = { PIX_FMT_BGR24, PIX_FMT_RGB24, PIX_FMT_NONE };
    //const enum PixelFormat blend_pix_fmts[] = { PIX_FMT_BGRA, PIX_FMT_NONE };
    const enum PixelFormat inout_pix_fmts[] = { PIX_FMT_YUV420P, PIX_FMT_NONE };
    const enum PixelFormat blend_pix_fmts[] = { PIX_FMT_YUVA420P, PIX_FMT_NONE };
    AVFilterFormats *inout_formats = avfilter_make_format_list(inout_pix_fmts);
    AVFilterFormats *blend_formats = avfilter_make_format_list(blend_pix_fmts);

    avfilter_formats_ref(inout_formats, &ctx->inputs [0]->out_formats);
    avfilter_formats_ref(blend_formats, &ctx->inputs [1]->out_formats);
    avfilter_formats_ref(inout_formats, &ctx->outputs[0]->in_formats );

    return 0;
}

static int config_input_main(AVFilterLink *link)
{
    OverlayContext *over = link->dst->priv;

    switch(link->format) {
    case PIX_FMT_RGB32:
    case PIX_FMT_BGR32:
        over->bpp = 4;
        break;
    case PIX_FMT_RGB24:
    case PIX_FMT_BGR24:
        over->bpp = 3;
        break;
    case PIX_FMT_RGB565:
    case PIX_FMT_RGB555:
    case PIX_FMT_BGR565:
    case PIX_FMT_BGR555:
    case PIX_FMT_GRAY16BE:
    case PIX_FMT_GRAY16LE:
        over->bpp = 2;
        break;
    default:
        over->bpp = 1;
    }

    over->hsub = av_pix_fmt_descriptors[link->format].log2_chroma_w;
    over->vsub = av_pix_fmt_descriptors[link->format].log2_chroma_h;

    return 0;
}

static int config_input_overlay(AVFilterLink *link)
{
    AVFilterContext *ctx  = link->dst;
    OverlayContext  *over = link->dst->priv;
    const char *expr;
    double var_values[VARS_NB], res;
    int ret;

    /* Finish the configuration by evaluating the expressions
       now when both inputs are configured. */
    var_values[MAIN_W]    = ctx->inputs[0]->w;
    var_values[MAIN_H]    = ctx->inputs[0]->h;
    var_values[OVERLAY_W] = ctx->inputs[1]->w;
    var_values[OVERLAY_H] = ctx->inputs[1]->h;

    av_log(ctx, AV_LOG_INFO, "main %dx%d overlay %dx%d\n", ctx->inputs[0]->w, ctx->inputs[0]->h,
           ctx->inputs[1]->w, ctx->inputs[1]->h);

    if ((ret = av_parse_and_eval_expr(&res, (expr = over->x_expr),
                                      var_names, var_values,
                                      NULL, NULL, NULL, NULL,
                                      NULL, 0, ctx)) < 0)
        goto fail;
    over->x = res;
    if ((ret = av_parse_and_eval_expr(&res, (expr = over->y_expr),
                                      var_names, var_values,
                                      NULL, NULL, NULL, NULL,
                                      NULL, 0, ctx)) < 0)
        goto fail;
    over->y = res;

    over->x &= ~((1 << over->hsub) - 1);
    over->y &= ~((1 << over->vsub) - 1);

    av_log(ctx, AV_LOG_INFO, "overlaying at %d,%d\n", over->x, over->y);

    return 0;

fail:
    av_log(NULL, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'\n", expr);
    return ret;
}

static AVFilterPicRef *get_video_buffer(AVFilterLink *link, int perms, int w, int h)
{
    AVFilterPicRef *picref = avfilter_get_video_buffer(link->dst->outputs[0],
                                                       perms, w, h);
    return picref;
}

static void start_frame(AVFilterLink *link, AVFilterPicRef *picref)
{
    AVFilterPicRef *outpicref = avfilter_ref_pic(picref, ~0);

    link->dst->outputs[0]->outpic = outpicref;

    avfilter_start_frame(link->dst->outputs[0], outpicref);
}

static void start_frame_overlay(AVFilterLink *link, AVFilterPicRef *picref)
{
    AVFilterContext *ctx = link->dst;
    OverlayContext *over = ctx->priv;

    over->overlay = picref;
}

static void blend_slice(AVFilterContext *ctx,
                        AVFilterPicRef *dst, AVFilterPicRef *src,
                        int x, int y, int w, int h,
                        int slice_y, int slice_w, int slice_h)
{
    OverlayContext *over = ctx->priv;
    int i, j, k;
    int width, height;
    int overlay_end_y = y+h;
    int slice_end_y = slice_y+slice_h;
    int end_y, start_y;

    width = FFMIN(slice_w - x, w);
    end_y = FFMIN(slice_end_y, overlay_end_y);
    start_y = FFMAX(y, slice_y);
    height = end_y - start_y;

    if (dst->pic->format == PIX_FMT_BGR24 || dst->pic->format == PIX_FMT_RGB24) {
        uint8_t *dp = dst->data[0] + x * 3 + start_y * dst->linesize[0];
        uint8_t *sp = src->data[0];
        int b = dst->pic->format == PIX_FMT_BGR24 ? 2 : 0;
        int r = dst->pic->format == PIX_FMT_BGR24 ? 0 : 2;
        if (slice_y > y)
            sp += (slice_y - y) * src->linesize[0];
        for (i = 0; i < height; i++) {
            uint8_t *d = dp, *s = sp;
            for (j = 0; j < width; j++) {
                d[r] = (d[r] * (0xff - s[3]) + s[0] * s[3] + 128) >> 8;
                d[1] = (d[1] * (0xff - s[3]) + s[1] * s[3] + 128) >> 8;
                d[b] = (d[b] * (0xff - s[3]) + s[2] * s[3] + 128) >> 8;
                d += 3;
                s += 4;
            }
            dp += dst->linesize[0];
            sp += src->linesize[0];
        }
    } else {
        for (i = 0; i < 3; i++) {
            int hsub = i ? over->hsub : 0;
            int vsub = i ? over->vsub : 0;
            uint8_t *dp = dst->data[i] + (x >> hsub) +
                (start_y >> vsub) * dst->linesize[i];
            uint8_t *sp = src->data[i];
            uint8_t *ap = src->data[3];
            int wp = FFALIGN(width, 1<<hsub) >> hsub;
            int hp = FFALIGN(height, 1<<vsub) >> vsub;
            if (slice_y > y) {
                sp += ((slice_y - y) >> vsub) * src->linesize[i];
                ap += (slice_y - y) * src->linesize[3];
            }
            for (j = 0; j < hp; j++) {
                uint8_t *d = dp, *s = sp, *a = ap;
                for (k = 0; k < wp; k++) {
                    // average alpha for color components, improve quality
                    int alpha_v, alpha_h, alpha;
                    if (hsub && vsub && j+1 < hp && k+1 < wp) {
                        alpha = (a[0] + a[src->linesize[3]] +
                                 a[1] + a[src->linesize[3]+1]) >> 2;
                    } else if (hsub || vsub) {
                        alpha_h = hsub && k+1 < wp ?
                            (a[0] + a[1]) >> 1 : a[0];
                        alpha_v = vsub && j+1 < hp ?
                            (a[0] + a[src->linesize[3]]) >> 1 : a[0];
                        alpha = (alpha_v + alpha_h) >> 1;
                    } else
                        alpha = a[0];
                    *d = (*d * (0xff - alpha) + *s++ * alpha + 128) >> 8;
                    d++;
                    a += 1 << hsub;
                }
                dp += dst->linesize[i];
                sp += src->linesize[i];
                ap += (1 << vsub) * src->linesize[3];
            }
        }
    }
}

static void draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
    AVFilterContext *ctx = link->dst;
    AVFilterPicRef *pic = ctx->outputs[0]->outpic;
    OverlayContext *over = ctx->priv;

    if (!over->overlay) {// || over->overlay->pts < pic->pts) {
        if (over->overlay) {
            avfilter_unref_pic(over->overlay);
            over->overlay = NULL;
        }
        if (avfilter_request_frame(ctx->inputs[1]))
            goto out;
        if (!over->overlay) {
            av_log(ctx, AV_LOG_ERROR, "error getting overlay frame\n");
            goto out;
        }
    }
    if (!(over->x >= pic->w || over->y >= pic->h ||
          y+h < over->y || y >= over->y+over->overlay->h)) {
        blend_slice(ctx, pic, over->overlay, over->x, over->y,
                    over->overlay->w, over->overlay->h, y, pic->w, h);
    }
 out:
    avfilter_draw_slice(ctx->outputs[0], y, h, slice_dir);
}

static void end_frame(AVFilterLink *link)
{
    avfilter_end_frame(link->dst->outputs[0]);
    avfilter_unref_pic(link->cur_pic);
}

static void null_draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
}

static void null_end_frame(AVFilterLink *link)
{
}

AVFilter avfilter_vf_overlay =
{
    .name      = "overlay",
    .description = "Overlay a video source on top of the input.",

    .init      = init,
    .uninit    = uninit,

    .priv_size = sizeof(OverlayContext),

    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .start_frame     = start_frame,
                                    .get_video_buffer= get_video_buffer,
                                    .config_props    = config_input_main,
                                    .draw_slice      = draw_slice,
                                    .end_frame       = end_frame },
                                  { .name            = "sub",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .start_frame     = start_frame_overlay,
                                    .config_props    = config_input_overlay,
                                    .draw_slice      = null_draw_slice,
                                    .end_frame       = null_end_frame },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO },
                                  { .name = NULL}},
};
