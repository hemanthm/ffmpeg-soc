/*
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
 * null audio source
 */

#include "avfilter.h"

typedef struct {
    enum SampleFormat sample_fmt;
    int64_t channel_layout;
} NullAudioContext;

static int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    NullAudioContext *priv = ctx->priv;

    priv->sample_fmt = SAMPLE_FMT_S16;
    priv->channel_layout = CH_LAYOUT_STEREO;

    if (args)
        sscanf(args, "%d:%ld", &priv->sample_fmt, &priv->channel_layout);

    if (priv->sample_fmt < 0 || priv->channel_layout < 3) {
        av_log(ctx, AV_LOG_ERROR, "Invalid values for sample format and/or channel layout.\n");
        return -1;
    }

    return 0;
}

static int config_props(AVFilterLink *outlink)
{
    NullAudioContext *priv = outlink->src->priv;

    outlink->sample_fmt = priv->sample_fmt;
    outlink->channel_layout = priv->channel_layout;

    av_log(outlink->src, AV_LOG_INFO, "sample format:%d channel_layout:%ld\n",
           priv->sample_fmt, priv->channel_layout);

    return 0;
}

static int request_frame(AVFilterLink *link)
{
    return -1;
}

AVFilter avfilter_vsrc_nullasrc = {
    .name        = "nullasrc",
    .description = NULL_IF_CONFIG_SMALL("Null audio source, never return audio frames."),

    .init       = init,
    .priv_size = sizeof(NullAudioContext),

    .inputs    = (AVFilterPad[]) {{ .name = NULL}},

    .outputs   = (AVFilterPad[]) {
        {
            .name            = "default",
            .type            = AVMEDIA_TYPE_AUDIO,
            .config_props    = config_props,
            .request_frame   = request_frame,
        },
        { .name = NULL}
    },
};
