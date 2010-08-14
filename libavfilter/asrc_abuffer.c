/*
 * Memory buffer source filter
 * Copyright (c) 2010 S.N. Hemanth Meenakshisundaram
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

#include "libavutil/fifo.h"
#include "avfilter.h"
#include "asrc_abuffer.h"

#define FIFO_SIZE 8

typedef struct {
    int           init_sample_fmt; ///< initial sample format indicated by client
    AVFifoBuffer *fifo;            ///< fifo buffer of audio frame pointers
} ABufferSourceContext;


int av_asrc_buffer_add_frame(AVFilterContext *filter, uint8_t *frame, int sample_fmt,
                             int size, int64_t ch_layout, int planar, int64_t pts)
{
    AVFilterLink *link = filter->outputs[0];
    ABufferSourceContext *ctx = filter->priv;
    AVFilterBufferRef *samplesref;

    if (av_fifo_space(ctx->fifo) < sizeof(samplesref)) {
        av_log(filter, AV_LOG_ERROR,
               "Buffering limit reached. Please consume some available frames before adding new ones.\n");
        return AVERROR(ENOMEM);
    }

    samplesref = avfilter_get_audio_buffer(link, AV_PERM_WRITE | AV_PERM_PRESERVE |
                                           AV_PERM_REUSE2, sample_fmt, size, ch_layout, planar);

    memcpy(samplesref->data[0], frame, samplesref->audio->size);
    samplesref->pts = pts;

    av_fifo_generic_write(ctx->fifo, &samplesref, sizeof(samplesref), NULL);

    return 0;
}

static av_cold int init(AVFilterContext *filter, const char *args, void *opaque)
{
    ABufferSourceContext *ctx = filter->priv;
    if (args && sscanf(args, "%d", &ctx->init_sample_fmt) != 1)
    {
        av_log(filter, AV_LOG_ERROR, "init() expected 1 parameter:'%s'\n", args);
        return AVERROR(EINVAL);
    }
    ctx->fifo = av_fifo_alloc(FIFO_SIZE*sizeof(AVFilterBufferRef*));
    if (!ctx->fifo) {
        av_log(filter, AV_LOG_ERROR, "Failed to allocate fifo, filter init failed.\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static av_cold void uninit(AVFilterContext *filter)
{
    ABufferSourceContext *ctx = filter->priv;
    av_fifo_free(ctx->fifo);
}

static int query_formats(AVFilterContext *filter)
{
    ABufferSourceContext *ctx = filter->priv;
    enum SampleFormat sample_fmts[] = { ctx->init_sample_fmt, SAMPLE_FMT_NONE };

    avfilter_set_common_formats(filter, avfilter_make_format_list(sample_fmts));
    return 0;
}

static int config_props(AVFilterLink *link)
{
    return 0;
}

static int request_frame(AVFilterLink *link)
{
    ABufferSourceContext *ctx = link->src->priv;
    AVFilterBufferRef *samplesref;

    if (!av_fifo_size(ctx->fifo)) {
        av_log(link->src, AV_LOG_ERROR,
               "request_frame() called with no available frames!\n");
    }

    av_fifo_generic_read(ctx->fifo, &samplesref, sizeof(samplesref), NULL);
    avfilter_filter_samples(link, avfilter_ref_buffer(samplesref, ~0));
    avfilter_unref_buffer(samplesref);

    return 0;
}

static int poll_frame(AVFilterLink *link)
{
    ABufferSourceContext *ctx = link->src->priv;
    return av_fifo_size(ctx->fifo)/sizeof(AVFilterBufferRef*);
}

AVFilter avfilter_asrc_abuffer =
{
    .name      = "abuffer",
    .description = NULL_IF_CONFIG_SMALL("Buffer audio frames, and make them accessible to the filterchain."),
    .priv_size = sizeof(ABufferSourceContext),
    .query_formats = query_formats,

    .init      = init,
    .uninit    = uninit,

    .inputs    = (AVFilterPad[]) {{ .name = NULL }},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_AUDIO,
                                    .request_frame   = request_frame,
                                    .poll_frame      = poll_frame,
                                    .config_props    = config_props, },
                                  { .name = NULL}},
};

