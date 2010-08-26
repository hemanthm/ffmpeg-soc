/*
 * Memory buffer source filter for audio
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

#include "libavcodec/audioconvert.h"
#include "libavutil/fifo.h"
#include "avfilter.h"
#include "asrc_abuffer.h"

#define FIFO_SIZE 8

typedef struct {
    unsigned int init_sample_fmt;  ///< initial sample format indicated by client
    int64_t init_ch_layout;        ///< initial channel layout indicated by client
    AVFifoBuffer *fifo;            ///< fifo buffer of audio frame pointers
} ABufferSourceContext;


int av_asrc_buffer_add_frame(AVFilterContext *ctx, uint8_t *frame, int sample_fmt,
                             int size, int64_t ch_layout, int planar, int64_t pts)
{
    AVFilterLink *link = ctx->outputs[0];
    ABufferSourceContext *abuffer = ctx->priv;
    AVFilterBufferRef *samplesref;

    if (av_fifo_space(abuffer->fifo) < sizeof(samplesref)) {
        av_log(ctx, AV_LOG_ERROR,
               "Buffering limit reached. Please consume some available frames before adding new ones.\n");
        return AVERROR(ENOMEM);
    }

    samplesref = avfilter_get_audio_buffer(link, AV_PERM_WRITE | AV_PERM_PRESERVE |
                                           AV_PERM_REUSE2, sample_fmt, size, ch_layout, planar);

    memcpy(samplesref->data[0], frame, samplesref->audio->size);
    samplesref->pts = pts;

    av_fifo_generic_write(abuffer->fifo, &samplesref, sizeof(samplesref), NULL);

    return 0;
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    ABufferSourceContext *abuffer = ctx->priv;
    char sample_fmt_str[16], ch_layout_str[16];

    if (args && (sscanf(args, "%15[a-z0-9]:%15[a-z0-9]", sample_fmt_str, ch_layout_str) != 2))
    {
        av_log(ctx, AV_LOG_ERROR, "init() expected 2 parameters:'%s'\n", args);
        return AVERROR(EINVAL);
    }

    abuffer->init_sample_fmt = avcodec_get_sample_fmt(sample_fmt_str);
    if (abuffer->init_sample_fmt >= SAMPLE_FMT_NB) {
        char *tail;
        abuffer->init_sample_fmt = strtol(sample_fmt_str, &tail, 10);
        if (*tail || abuffer->init_sample_fmt >= SAMPLE_FMT_NB) {
            av_log(ctx, AV_LOG_ERROR, "Invalid sample format %s\n", sample_fmt_str);
            return AVERROR(EINVAL);
        }
    }

    abuffer->init_ch_layout = avcodec_get_channel_layout(ch_layout_str);
    if (abuffer->init_ch_layout < CH_LAYOUT_STEREO) {
        char *tail;
        abuffer->init_ch_layout = strtol(ch_layout_str, &tail, 10);
        if (*tail || abuffer->init_ch_layout < CH_LAYOUT_STEREO) {
            av_log(ctx, AV_LOG_ERROR, "Invalid channel layout %s\n", ch_layout_str);
            return AVERROR(EINVAL);
        }
    }

    abuffer->fifo = av_fifo_alloc(FIFO_SIZE*sizeof(AVFilterBufferRef*));
    if (!abuffer->fifo) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate fifo, filter init failed.\n");
        return AVERROR(ENOMEM);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ABufferSourceContext *abuffer = ctx->priv;
    av_fifo_free(abuffer->fifo);
}

static int query_formats(AVFilterContext *ctx)
{
    ABufferSourceContext *abuffer = ctx->priv;
    enum SampleFormat sample_fmts[] = { abuffer->init_sample_fmt, SAMPLE_FMT_NONE };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(sample_fmts));
    return 0;
}

static int config_props(AVFilterLink *link)
{
    ABufferSourceContext *abuffer = link->src->priv;
    link->format = abuffer->init_sample_fmt;
    link->channel_layout = abuffer->init_ch_layout;
    return 0;
}

static int request_frame(AVFilterLink *link)
{
    ABufferSourceContext *abuffer = link->src->priv;
    AVFilterBufferRef *samplesref;

    if (!av_fifo_size(abuffer->fifo)) {
        av_log(link->src, AV_LOG_ERROR,
               "request_frame() called with no available frames!\n");
    }

    av_fifo_generic_read(abuffer->fifo, &samplesref, sizeof(samplesref), NULL);
    avfilter_filter_samples(link, avfilter_ref_buffer(samplesref, ~0));
    avfilter_unref_buffer(samplesref);

    return 0;
}

static int poll_frame(AVFilterLink *link)
{
    ABufferSourceContext *abuffer = link->src->priv;
    return av_fifo_size(abuffer->fifo)/sizeof(AVFilterBufferRef*);
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

