/*
 * Filter layer - default implementations
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

#include "libavcore/imgutils.h"
#include "libavcodec/audioconvert.h"
#include "avfilter.h"

/* TODO: buffer pool.  see comment for avfilter_default_get_video_buffer() */
static void avfilter_default_free_buffer(AVFilterBuffer *ptr)
{
    av_free(ptr->data[0]);
    av_free(ptr);
}

/* TODO: set the buffer's priv member to a context structure for the whole
 * filter chain.  This will allow for a buffer pool instead of the constant
 * alloc & free cycle currently implemented. */
AVFilterBufferRef *avfilter_default_get_video_buffer(AVFilterLink *link, int perms, int w, int h)
{
    AVFilterBuffer *pic = av_mallocz(sizeof(AVFilterBuffer));
    AVFilterBufferRef *ref = av_mallocz(sizeof(AVFilterBufferRef));
    AVFilterBufferRefVideoProps *ref_props = av_mallocz(sizeof(AVFilterBufferRefVideoProps));
    int i, tempsize;
    char *buf;

    ref->buf         = pic;
    ref_props->w     = w;
    ref_props->h     = h;
    ref->props       = ref_props;

    /* make sure the buffer gets read permission or it's useless for output */
    ref->perms = perms | AV_PERM_READ;

    pic->refcount = 1;
    ref->format   = link->format;
    pic->free     = avfilter_default_free_buffer;
    av_fill_image_linesizes(pic->linesize, ref->format, ref_props->w);

    for (i=0; i<4;i++)
        pic->linesize[i] = FFALIGN(pic->linesize[i], 16);

    tempsize = av_fill_image_pointers(pic->data, ref->format, ref_props->h, NULL, pic->linesize);
    buf = av_malloc(tempsize + 16); // +2 is needed for swscaler, +16 to be
                                    // SIMD-friendly
    av_fill_image_pointers(pic->data, ref->format, ref_props->h, buf, pic->linesize);

    memcpy(ref->data,     pic->data,     sizeof(pic->data)>>1);
    memcpy(ref->linesize, pic->linesize, sizeof(pic->linesize)>>1);

    return ref;
}

AVFilterBufferRef *avfilter_default_get_audio_buffer(AVFilterLink *link, int perms,
                                                     int size, int64_t channel_layout,
                                                     enum SampleFormat sample_fmt, int planar)
{
    AVFilterBuffer *buffer = av_mallocz(sizeof(AVFilterBuffer));
    AVFilterBufferRef *ref = av_mallocz(sizeof(AVFilterBufferRef));
    AVFilterBufferRefAudioProps *ref_props = av_mallocz(sizeof(AVFilterBufferRefAudioProps));
    int i, sample_size, num_chans, bufsize, per_channel_size, step_size = 0;
    char *buf;

    ref->buf                  = buffer;
    ref_props->channel_layout = channel_layout;
    ref->format               = sample_fmt;
    ref_props->size           = size;
    ref_props->planar         = planar;

    /* make sure the buffer gets read permission or it's useless for output */
    ref->perms = perms | AV_PERM_READ;

    buffer->refcount   = 1;
    buffer->free       = avfilter_default_free_buffer;

    sample_size = av_get_bits_per_sample_format(sample_fmt) >>3;
    num_chans = avcodec_channel_layout_num_channels(channel_layout);

    per_channel_size     = size/num_chans;
    ref_props->samples_nb = per_channel_size/sample_size;

    /* Set the number of bytes to traverse to reach next sample of a particular channel:
     * For planar, this is simply the sample size.
     * For packed, this is the number of samples * sample_size.
     */
    for (i = 0; i < num_chans; i++)
        buffer->linesize[i] = (planar > 0)?(per_channel_size):sample_size;
    memset(&buffer->linesize[num_chans], 0, (8-num_chans)*sizeof(buffer->linesize[0]));

    /* Calculate total buffer size, round to multiple of 16 to be SIMD friendly */
    bufsize = (size + 15)&~15;
    buf = av_malloc(bufsize);

    /* For planar, set the start point of each channel's data within the buffer
     * For packed, set the start point of the entire buffer only
     */
    buffer->data[0] = buf;
    if (planar > 0) {
        for (i = 1; i < num_chans; i++) {
            step_size += per_channel_size;
            buffer->data[i] = buf + step_size;
        }
    } else
        memset(&buffer->data[1], (long)buf, (num_chans-1)*sizeof(buffer->data[0]));

    memset(&buffer->data[num_chans], 0, (8-num_chans)*sizeof(buffer->data[0]));

    memcpy(ref->data,     buffer->data,     sizeof(buffer->data));
    memcpy(ref->linesize, buffer->linesize, sizeof(buffer->linesize));

    return ref;
}

void avfilter_default_start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    AVFilterLink *out = NULL;

    if(link->dst->output_count)
        out = link->dst->outputs[0];

    if(out) {
        out->out_buf      = avfilter_get_video_buffer(out, AV_PERM_WRITE, out->w, out->h);
        avfilter_copy_bufref_props(out->out_buf, picref);
        avfilter_start_frame(out, avfilter_ref_buffer(out->out_buf, ~0));
    }
}

void avfilter_default_draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
    AVFilterLink *out = NULL;

    if(link->dst->output_count)
        out = link->dst->outputs[0];

    if(out)
        avfilter_draw_slice(out, y, h, slice_dir);
}

void avfilter_default_end_frame(AVFilterLink *link)
{
    AVFilterLink *out = NULL;

    if(link->dst->output_count)
        out = link->dst->outputs[0];

    avfilter_unref_buffer(link->cur_buf);
    link->cur_buf = NULL;

    if(out) {
        if(out->out_buf) {
            avfilter_unref_buffer(out->out_buf);
            out->out_buf = NULL;
        }
        avfilter_end_frame(out);
    }
}

/* FIXME: samplesref is same as link->cur_buf. Need to consider removing the redundant parameter. */
void avfilter_default_filter_samples(AVFilterLink *link, AVFilterBufferRef *samplesref)
{
    AVFilterLink *out = NULL;
    AVFilterBufferRefAudioProps *sample_props, *out_buf_props;
    AVFILTER_GET_REF_AUDIO_PROPS(sample_props, samplesref);

    if (link->dst->output_count)
        out = link->dst->outputs[0];

    if (out) {
        AVFILTER_GET_REF_AUDIO_PROPS(out_buf_props, out->out_buf);
        out->out_buf = avfilter_default_get_audio_buffer(link, AV_PERM_WRITE, sample_props->size,
                                                         sample_props->channel_layout,
                                                         samplesref->format, sample_props->planar);
        out->out_buf->pts             = samplesref->pts;
        out_buf_props->sample_rate    = out_buf_props->sample_rate;
        avfilter_filter_samples(out, avfilter_ref_buffer(out->out_buf, ~0));
        avfilter_unref_buffer(out->out_buf);
        out->out_buf = NULL;
    }
    avfilter_unref_buffer(samplesref);
    link->cur_buf = NULL;
}

/**
 * default config_link() implementation for output video links to simplify
 * the implementation of one input one output video filters */
int avfilter_default_config_output_link(AVFilterLink *link)
{
    if(link->src->input_count && link->src->inputs[0]) {
        link->w = link->src->inputs[0]->w;
        link->h = link->src->inputs[0]->h;
    } else {
        /* XXX: any non-simple filter which would cause this branch to be taken
         * really should implement its own config_props() for this link. */
        return -1;
    }

    return 0;
}

/**
 * A helper for query_formats() which sets all links to the same list of
 * formats. If there are no links hooked to this filter, the list of formats is
 * freed.
 *
 * FIXME: this will need changed for filters with a mix of pad types
 * (video + audio, etc)
 */
void avfilter_set_common_formats(AVFilterContext *ctx, AVFilterFormats *formats)
{
    int count = 0, i;

    for(i = 0; i < ctx->input_count; i ++) {
        if(ctx->inputs[i]) {
            avfilter_formats_ref(formats, &ctx->inputs[i]->out_formats);
            count ++;
        }
    }
    for(i = 0; i < ctx->output_count; i ++) {
        if(ctx->outputs[i]) {
            avfilter_formats_ref(formats, &ctx->outputs[i]->in_formats);
            count ++;
        }
    }

    if(!count) {
        av_free(formats->formats);
        av_free(formats->refs);
        av_free(formats);
    }
}

int avfilter_default_query_formats(AVFilterContext *ctx)
{
    enum AVMediaType type = ctx->inputs [0] ? ctx->inputs [0]->type :
                            ctx->outputs[0] ? ctx->outputs[0]->type :
                            AVMEDIA_TYPE_VIDEO;

    avfilter_set_common_formats(ctx, avfilter_all_formats(type));
    return 0;
}

void avfilter_null_start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    avfilter_start_frame(link->dst->outputs[0], picref);
}

void avfilter_null_draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
    avfilter_draw_slice(link->dst->outputs[0], y, h, slice_dir);
}

void avfilter_null_end_frame(AVFilterLink *link)
{
    avfilter_end_frame(link->dst->outputs[0]);
}

void avfilter_null_filter_samples(AVFilterLink *link, AVFilterBufferRef *samplesref)
{
    avfilter_filter_samples(link->dst->outputs[0], samplesref);
}

AVFilterBufferRef *avfilter_null_get_video_buffer(AVFilterLink *link, int perms, int w, int h)
{
    return avfilter_get_video_buffer(link->dst->outputs[0], perms, w, h);
}

AVFilterBufferRef *avfilter_null_get_audio_buffer(AVFilterLink *link, int perms,
                                                 int size, int64_t channel_layout,
                                                 enum SampleFormat sample_fmt, int packed)
{
    return avfilter_get_audio_buffer(link->dst->outputs[0], perms, size,
                                     channel_layout, sample_fmt, packed);
}

