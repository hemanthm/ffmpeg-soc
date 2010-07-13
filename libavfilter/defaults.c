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

#include "libavcodec/imgconvert.h"
#include "avfilter.h"

/* TODO: buffer pool.  see comment for avfilter_default_get_video_buffer() */
static void avfilter_default_free_video_buffer(AVFilterBuffer *pic)
{
    av_free(pic->data[0]);
    av_free(pic);
}

/* TODO: set the buffer's priv member to a context structure for the whole
 * filter chain.  This will allow for a buffer pool instead of the constant
 * alloc & free cycle currently implemented. */
AVFilterPicRef *avfilter_default_get_video_buffer(AVFilterLink *link, int perms, int w, int h)
{
    AVFilterBuffer *pic = av_mallocz(sizeof(AVFilterBuffer));
    AVFilterPicRef *ref = av_mallocz(sizeof(AVFilterPicRef));
    int i, tempsize;
    char *buf;

    ref->pic   = pic;
    ref->w     = w;
    ref->h     = h;

    /* make sure the buffer gets read permission or it's useless for output */
    ref->perms = perms | AV_PERM_READ;

    pic->refcount = 1;
    pic->format   = link->format;
    pic->free     = avfilter_default_free_video_buffer;
    ff_fill_linesize((AVPicture *)pic, pic->format, ref->w);

    for (i=0; i<4;i++)
        pic->linesize[i] = FFALIGN(pic->linesize[i], 16);

    tempsize = ff_fill_pointer((AVPicture *)pic, NULL, pic->format, ref->h);
    buf = av_malloc(tempsize + 16); // +2 is needed for swscaler, +16 to be
                                    // SIMD-friendly
    ff_fill_pointer((AVPicture *)pic, buf, pic->format, ref->h);

    memcpy(ref->data,     pic->data,     sizeof(pic->data));
    memcpy(ref->linesize, pic->linesize, sizeof(pic->linesize));

    return ref;
}

AVFilterSamplesRef *avfilter_default_get_samples_ref(AVFilterLink *link, int perms,
                                                      int size, int64_t channel_layout,
                                                      enum SampleFormat sample_fmt, int planar)
{
    AVFilterBuffer *buffer = av_mallocz(sizeof(AVFilterBuffer));
    AVFilterSamplesRef *ref = av_mallocz(sizeof(AVFilterSamplesRef));
    int i, sample_size, num_chans, bufsize, per_channel_size, step_size = 0;
    char *buf;

    ref->samples         = buffer;
    ref->channel_layout = channel_layout;
    ref->sample_fmt     = sample_fmt;
    ref->size           = size;
    ref->planar         = planar;

    /* make sure the buffer gets read permission or it's useless for output */
    ref->perms = perms | AV_PERM_READ;

    buffer->refcount   = 1;
    buffer->free       = avfilter_default_free_buffer;

    sample_size = av_get_bits_per_sample_format(sample_fmt) >>3;
    num_chans = avcodec_channel_layout_num_channels(channel_layout);

    per_channel_size = size/num_chans;
    ref->samples_nb = per_channel_size/sample_size;

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

void avfilter_default_start_frame(AVFilterLink *link, AVFilterPicRef *picref)
{
    AVFilterLink *out = NULL;

    if(link->dst->output_count)
        out = link->dst->outputs[0];

    if(out) {
        out->outpic      = avfilter_get_video_buffer(out, AV_PERM_WRITE, out->w, out->h);
        out->outpic->pts = picref->pts;
        out->outpic->pos = picref->pos;
        out->outpic->pixel_aspect = picref->pixel_aspect;
        out->outpic->interlaced      = picref->interlaced;
        out->outpic->top_field_first = picref->top_field_first;
        avfilter_start_frame(out, avfilter_ref_pic(out->outpic, ~0));
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

    avfilter_unref_pic(link->cur_pic);
    link->cur_pic = NULL;

    if(out) {
        if(out->outpic) {
            avfilter_unref_pic(out->outpic);
            out->outpic = NULL;
        }
        avfilter_end_frame(out);
    }
}

/* FIXME: samplesref is same as link->cur_samples. Need to consider removing the redundant parameter. */
void avfilter_default_filter_samples(AVFilterLink *link, AVFilterSamplesRef *samplesref)
{
    AVFilterLink *out = NULL;

    if (link->dst->output_count)
        out = link->dst->outputs[0];

    if (out) {
        out->out_samples = avfilter_default_get_samples_ref(link, AV_PERM_WRITE, samplesref->size,
                                                            samplesref->channel_layout,
                                                            samplesref->sample_fmt, samplesref->planar);
        out->out_samples->pts            = samplesref->pts;
        out->out_samples->sample_rate    = samplesref->sample_rate;
        avfilter_filter_samples(out, avfilter_ref_samples(out->out_samples, ~0));
        avfilter_unref_samples(out->out_samples);
        out->out_samples = NULL;
    }
    avfilter_unref_samples(samplesref);
    link->cur_samples = NULL;
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
    avfilter_set_common_formats(ctx, avfilter_all_colorspaces());
    return 0;
}

void avfilter_null_start_frame(AVFilterLink *link, AVFilterPicRef *picref)
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

void avfilter_null_filter_samples(AVFilterLink *link, AVFilterSamplesRef *samplesref)
{
    avfilter_filter_samples(link->dst->outputs[0], samplesref);
}

AVFilterPicRef *avfilter_null_get_video_buffer(AVFilterLink *link, int perms, int w, int h)
{
    return avfilter_get_video_buffer(link->dst->outputs[0], perms, w, h);
}

AVFilterSamplesRef *avfilter_null_get_samples_ref(AVFilterLink *link, int perms, int size,
                                                   int64_t channel_layout,
                                                   enum SampleFormat sample_fmt, int packed)
{
    return avfilter_get_samples_ref(link->dst->outputs[0], perms, size,
                                     channel_layout, sample_fmt, packed);
}

