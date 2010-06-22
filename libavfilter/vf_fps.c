/*
 * video framerate modification filter
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

/* TODO: improve handling of non-continuous timestamps (mpeg, seeking, etc) */

#include "avfilter.h"

typedef struct {
    uint64_t timebase;
    uint64_t pts;
    AVFilterPicRef *pic;
    int videoend;
    int has_frame;
} FPSContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    AVRational default_rate = (AVRational) {25, 1};
    FPSContext *fps = ctx->priv;
    AVRational rate;

    rate = default_rate;

    if (args && (av_parse_video_frame_rate(&rate, args) < 0)) {
        av_log(ctx, AV_LOG_ERROR, "Invalid frame rate: \"%s\"\n", args);
        rate = default_rate;
    }

    fps->timebase = ((int64_t)AV_TIME_BASE * rate.den) / rate.num;
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    FPSContext *fps = ctx->priv;
    if(fps->pic) avfilter_unref_pic(fps->pic);
}

static void start_frame(AVFilterLink *link, AVFilterPicRef *picref)
{
    FPSContext *fps = link->dst->priv;
    if(fps->pic) avfilter_unref_pic(fps->pic);
    fps->pic = picref;
}

static int poll_frame(AVFilterLink *link)
{
    FPSContext *fps = link->src->priv;

    if (fps->has_frame)
        return 1;

    if(avfilter_poll_frame(link->src->inputs[0]) &&
       avfilter_request_frame(link->src->inputs[0])) {
        fps->videoend = 1;
        return 1;
    }

    fps->has_frame = !!(fps->pic && fps->pic->pts >= fps->pts);
    return fps->has_frame;
}

static void end_frame(AVFilterLink *link)
{
}

static int request_frame(AVFilterLink *link)
{
    FPSContext *fps = link->src->priv;

    if (fps->videoend)
        return -1;

    if (!fps->has_frame) // support for filtering without poll_frame usage
        while(!fps->pic || fps->pic->pts < fps->pts)
            if(avfilter_request_frame(link->src->inputs[0]))
                return -1;

    fps->has_frame=0;
    avfilter_start_frame(link, avfilter_ref_pic(fps->pic, ~AV_PERM_WRITE));
    avfilter_draw_slice (link, 0, fps->pic->h, 1);
    avfilter_end_frame  (link);

    avfilter_unref_pic(fps->pic);
    fps->pic = NULL;

    fps->pts += fps->timebase;

    return 0;
}

AVFilter avfilter_vf_fps =
{
    .name      = "fps",

    .init      = init,
    .uninit    = uninit,

    .priv_size = sizeof(FPSContext),

    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .get_video_buffer= avfilter_null_get_video_buffer,
                                    .start_frame     = start_frame,
                                    .end_frame       = end_frame, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .poll_frame      = poll_frame,
                                    .request_frame   = request_frame, },
                                  { .name = NULL}},
};

