/*
 * video presentation timestamp (PTS) modification filter
 * copyright (c) 2008 Victor Paesa
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

/*
 # A few usage examples follow, usable too as test scenarios.
 #TODO Eventually move them into FFmpeg docs.

 # Start counting PTS from zero
 ffmpeg -i input.avi -vfilters setpts=PTS-STARTPTS output.avi

 # Fast motion
 ffmpeg -i input.avi -vfilters setpts=0.5*PTS output.avi

 # Fixed rate 25 fps
 ffmpeg -i input.avi -vfilters setpts=N*AVTB/25 output.avi

 # Fixed rate 25 fps with some jitter
 ffmpeg -i input.avi -vfilters 'setpts=AVTB/25*(N+0.05*sin(N*2*PI/25))' output.avi
*/

#include "libavutil/eval.h"

#include "avfilter.h"

static const char *const_names[]={
    "PI",
    "E",
    "AVTB",      ///< AV_TIME_BASE
    "N",         ///< frame number (starting at zero)
    "PTS",       ///< original pts
    "STARTPTS",  ///< pts at start of movie
    // TODO Add multiple inputs/outputs to filter
    // TODO Add functions of pin number: pts(), startpts()
    NULL
};

enum PosOfValue {
    POV_PI,
    POV_E,
    POV_AVTB,
    POV_N,
    POV_PTS,
    POV_STARTPTS,
    POV_NULL
};

typedef struct {
    AVExpr *expr;
    double const_values[POV_NULL+1];
} SetPTSContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    SetPTSContext *setpts = ctx->priv;
    int ret;

    ret = av_parse_expr(&setpts->expr, args ? args : "PTS",
                        const_names, NULL, NULL, NULL, NULL, 0, ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "Error while parsing expression '%s'\n", args);
        return ret;
    }

    setpts->const_values[POV_PI      ] = M_PI;
    setpts->const_values[POV_E       ] = M_E;
    setpts->const_values[POV_AVTB    ] = AV_TIME_BASE;
    setpts->const_values[POV_N       ] = 0.0;
    setpts->const_values[POV_STARTPTS] = AV_NOPTS_VALUE;
    setpts->const_values[POV_NULL    ] = 0.0;

    return !setpts->expr;
}

static AVFilterBufferRef *get_video_buffer(AVFilterLink *link, int perms,
                                        int w, int h)
{
    return avfilter_get_video_buffer(link->dst->outputs[0], perms, w, h);
}

static void start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    SetPTSContext *setpts = link->dst->priv;
    AVFilterBufferRef *ref2 = avfilter_ref_buffer(picref, ~0);


    if (setpts->const_values[POV_STARTPTS] == AV_NOPTS_VALUE)
        setpts->const_values[POV_STARTPTS] = ref2->pts;
    setpts->const_values[POV_PTS] = ref2->pts;

    //av_log(NULL, AV_LOG_INFO, "pts org:%lld final:%lld\n", ref2->pts,
    //    (uint64_t) ff_parse_eval(setpts->expr, setpts->const_values, setpts));

    ref2->pts =
        (uint64_t) av_eval_expr(setpts->expr, setpts->const_values, setpts);

    setpts->const_values[POV_N  ] += 1.0;

    avfilter_start_frame(link->dst->outputs[0], ref2);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SetPTSContext *setpts = ctx->priv;
    av_free_expr(setpts->expr);
}

AVFilter avfilter_vf_setpts =
{
    .name      = "setpts",

    .init      = init,
    .uninit    = uninit,

    .priv_size = sizeof(SetPTSContext),

    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .get_video_buffer= get_video_buffer,
                                    .start_frame     = start_frame,},
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,},
                                  { .name = NULL}},
};
