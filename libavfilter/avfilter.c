/*
 * filter layer
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

/* #define DEBUG */

#include "libavcodec/imgconvert.h"
#include "libavcodec/audioconvert.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "internal.h"

unsigned avfilter_version(void) {
    return LIBAVFILTER_VERSION_INT;
}

const char *avfilter_configuration(void)
{
    return FFMPEG_CONFIGURATION;
}

const char *avfilter_license(void)
{
#define LICENSE_PREFIX "libavfilter license: "
    return LICENSE_PREFIX FFMPEG_LICENSE + sizeof(LICENSE_PREFIX) - 1;
}

/** helper macros to get the in/out pad on the dst/src filter */
#define link_dpad(link)     link->dst-> input_pads[link->dstpad]
#define link_spad(link)     link->src->output_pads[link->srcpad]

#define AVFILTER_COPY_AUDIO_PROPS(dst, src) {\
    dst->props = av_malloc(sizeof(AVFilterBufferRefAudioProps));\
    memcpy(dst->props, src->props, sizeof(AVFilterBufferRefAudioProps));\
}

#define AVFILTER_COPY_VIDEO_PROPS(dst, src) {\
    dst->props = av_malloc(sizeof(AVFilterBufferRefVideoProps));\
    memcpy(dst->props, src->props, sizeof(AVFilterBufferRefVideoProps));\
}

AVFilterBufferRef *avfilter_ref_buffer(AVFilterBufferRef *ref, int pmask)
{
    AVFilterBufferRef *ret = av_malloc(sizeof(AVFilterBufferRef));
    *ret = *ref;
    if (ref->type == AVMEDIA_TYPE_VIDEO) {
        AVFILTER_COPY_VIDEO_PROPS(ret, ref);
    } else if (ref->type == AVMEDIA_TYPE_AUDIO) {
        AVFILTER_COPY_AUDIO_PROPS(ret, ref);
    }
    ret->perms &= pmask;
    ret->buf->refcount ++;
    return ret;
}

void avfilter_unref_buffer(AVFilterBufferRef *ref)
{
    if(!(--ref->buf->refcount))
        ref->buf->free(ref->buf);
    av_free(ref->props);
    av_free(ref);
}

void avfilter_insert_pad(unsigned idx, unsigned *count, size_t padidx_off,
                         AVFilterPad **pads, AVFilterLink ***links,
                         AVFilterPad *newpad)
{
    unsigned i;

    idx = FFMIN(idx, *count);

    *pads  = av_realloc(*pads,  sizeof(AVFilterPad)   * (*count + 1));
    *links = av_realloc(*links, sizeof(AVFilterLink*) * (*count + 1));
    memmove(*pads +idx+1, *pads +idx, sizeof(AVFilterPad)   * (*count-idx));
    memmove(*links+idx+1, *links+idx, sizeof(AVFilterLink*) * (*count-idx));
    memcpy(*pads+idx, newpad, sizeof(AVFilterPad));
    (*links)[idx] = NULL;

    (*count) ++;
    for(i = idx+1; i < *count; i ++)
        if(*links[i])
            (*(unsigned *)((uint8_t *) *links[i] + padidx_off)) ++;
}

int avfilter_link(AVFilterContext *src, unsigned srcpad,
                  AVFilterContext *dst, unsigned dstpad)
{
    AVFilterLink *link;

    if(src->output_count <= srcpad || dst->input_count <= dstpad ||
       src->outputs[srcpad]        || dst->inputs[dstpad])
        return -1;

    src->outputs[srcpad] =
    dst-> inputs[dstpad] = link = av_mallocz(sizeof(AVFilterLink));

    link->src     = src;
    link->dst     = dst;
    link->srcpad  = srcpad;
    link->dstpad  = dstpad;
    link->type    = src->output_pads[srcpad].type;
    assert(PIX_FMT_NONE == -1 && SAMPLE_FMT_NONE == -1);
    link->format  = -1;

    return 0;
}

int avfilter_insert_filter(AVFilterLink *link, AVFilterContext *filt,
                           unsigned in, unsigned out)
{
    av_log(link->dst, AV_LOG_INFO, "auto-inserting filter '%s' "
           "between the filter '%s' and the filter '%s'\n",
           filt->name, link->src->name, link->dst->name);

    link->dst->inputs[link->dstpad] = NULL;
    if(avfilter_link(filt, out, link->dst, link->dstpad)) {
        /* failed to link output filter to new filter */
        link->dst->inputs[link->dstpad] = link;
        return -1;
    }

    /* re-hookup the link to the new destination filter we inserted */
    link->dst = filt;
    link->dstpad = in;
    filt->inputs[in] = link;

    /* if any information on supported media formats already exists on the
     * link, we need to preserve that */
    if(link->out_formats)
        avfilter_formats_changeref(&link->out_formats,
                                   &filt->outputs[out]->out_formats);

    return 0;
}

int avfilter_config_links(AVFilterContext *filter)
{
    int (*config_link)(AVFilterLink *);
    unsigned i;

    for(i = 0; i < filter->input_count; i ++) {
        AVFilterLink *link = filter->inputs[i];

        if(!link) continue;

        switch(link->init_state) {
        case AVLINK_INIT:
            continue;
        case AVLINK_STARTINIT:
            av_log(filter, AV_LOG_INFO, "circular filter chain detected\n");
            return 0;
        case AVLINK_UNINIT:
            link->init_state = AVLINK_STARTINIT;

            if(avfilter_config_links(link->src))
                return -1;

            if(!(config_link = link_spad(link).config_props))
                config_link  = avfilter_default_config_output_link;
            if(config_link(link))
                return -1;

            if((config_link = link_dpad(link).config_props))
                if(config_link(link))
                    return -1;

            link->init_state = AVLINK_INIT;
        }
    }

    return 0;
}

void ff_dprintf_picref(void *ctx, AVFilterBufferRef *picref, int end)
{
    AVFilterBufferRefVideoProps *pic_props;
    AVFILTER_GET_REF_VIDEO_PROPS(pic_props, picref);
    dprintf(ctx,
            "picref[%p data[%p, %p, %p, %p] linesize[%d, %d, %d, %d] pts:%"PRId64" pos:%"PRId64" a:%d/%d s:%dx%d]%s",
            picref,
            picref->data    [0], picref->data    [1], picref->data    [2], picref->data    [3],
            picref->linesize[0], picref->linesize[1], picref->linesize[2], picref->linesize[3],
            picref->pts, picref->pos,
            pic_props->pixel_aspect.num, pic_props->pixel_aspect.den, pic_props->w, pic_props->h,
            end ? "\n" : "");
}

void ff_dprintf_link(void *ctx, AVFilterLink *link, int end)
{
    dprintf(ctx,
            "link[%p s:%dx%d fmt:%-16s %-16s->%-16s]%s",
            link, link->w, link->h,
            av_pix_fmt_descriptors[link->format].name,
            link->src ? link->src->filter->name : "",
            link->dst ? link->dst->filter->name : "",
            end ? "\n" : "");
}

AVFilterBufferRef *avfilter_get_video_buffer(AVFilterLink *link, int perms, int w, int h)
{
    AVFilterBufferRef *ret = NULL;

    FF_DPRINTF_START(NULL, get_video_buffer); ff_dprintf_link(NULL, link, 0); dprintf(NULL, " perms:%d w:%d h:%d\n", perms, w, h);

    if(link_dpad(link).get_video_buffer)
        ret = link_dpad(link).get_video_buffer(link, perms, w, h);

    if(!ret)
        ret = avfilter_default_get_video_buffer(link, perms, w, h);

    ret->type = AVMEDIA_TYPE_VIDEO;

    FF_DPRINTF_START(NULL, get_video_buffer); ff_dprintf_link(NULL, link, 0); dprintf(NULL, " returning "); ff_dprintf_picref(NULL, ret, 1);

    return ret;
}

AVFilterBufferRef *avfilter_get_audio_buffer(AVFilterLink *link, int perms, int size,
                                             int64_t channel_layout, enum SampleFormat sample_fmt, int planar)
{
    AVFilterBufferRef *ret = NULL;

    if(link_dpad(link).get_audio_buffer)
        ret = link_dpad(link).get_audio_buffer(link, perms, size, channel_layout, sample_fmt, planar);

    if(!ret)
        ret = avfilter_default_get_audio_buffer(link, perms, size, channel_layout, sample_fmt, planar);

    ret->type = AVMEDIA_TYPE_AUDIO;

    return ret;
}

int avfilter_request_frame(AVFilterLink *link)
{
    FF_DPRINTF_START(NULL, request_frame); ff_dprintf_link(NULL, link, 1);

    if(link_spad(link).request_frame)
        return link_spad(link).request_frame(link);
    else if(link->src->inputs[0])
        return avfilter_request_frame(link->src->inputs[0]);
    else return -1;
}

int avfilter_poll_frame(AVFilterLink *link)
{
    int i, min=INT_MAX;

    if(link_spad(link).poll_frame)
        return link_spad(link).poll_frame(link);

    for (i=0; i<link->src->input_count; i++) {
        int val;
        if(!link->src->inputs[i])
            return -1;
        val = avfilter_poll_frame(link->src->inputs[i]);
        min = FFMIN(min, val);
    }

    return min;
}

/* XXX: should we do the duplicating of the picture ref here, instead of
 * forcing the source filter to do it? */
void avfilter_start_frame(AVFilterLink *link, AVFilterBufferRef *picref)
{
    void (*start_frame)(AVFilterLink *, AVFilterBufferRef *);
    AVFilterPad *dst = &link_dpad(link);

    FF_DPRINTF_START(NULL, start_frame); ff_dprintf_link(NULL, link, 0); dprintf(NULL, " "); ff_dprintf_picref(NULL, picref, 1);

    if(!(start_frame = dst->start_frame))
        start_frame = avfilter_default_start_frame;

    /* prepare to copy the picture if it has insufficient permissions */
    if((dst->min_perms & picref->perms) != dst->min_perms ||
        dst->rej_perms & picref->perms) {
        /*
        av_log(link->dst, AV_LOG_INFO,
                "frame copy needed (have perms %x, need %x, reject %x)\n",
                picref->perms,
                link_dpad(link).min_perms, link_dpad(link).rej_perms);
        */

        link->cur_buf = avfilter_default_get_video_buffer(link, dst->min_perms, link->w, link->h);
        link->src_buf = picref;
        avfilter_copy_bufref_props(link->cur_buf, link->src_buf);
    }
    else
        link->cur_buf = picref;

    start_frame(link, link->cur_buf);
}

void avfilter_end_frame(AVFilterLink *link)
{
    void (*end_frame)(AVFilterLink *);

    if(!(end_frame = link_dpad(link).end_frame))
        end_frame = avfilter_default_end_frame;

    end_frame(link);

    /* unreference the source picture if we're feeding the destination filter
     * a copied version dues to permission issues */
    if(link->src_buf) {
        avfilter_unref_buffer(link->src_buf);
        link->src_buf = NULL;
    }

}

void avfilter_draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
    AVFilterBufferRefVideoProps *cur_buf_props;
    uint8_t *src[4], *dst[4];
    int i, j, vsub;
    void (*draw_slice)(AVFilterLink *, int, int, int);

    FF_DPRINTF_START(NULL, draw_slice); ff_dprintf_link(NULL, link, 0); dprintf(NULL, " y:%d h:%d dir:%d\n", y, h, slice_dir);

    /* copy the slice if needed for permission reasons */
    if(link->src_buf) {
        vsub = av_pix_fmt_descriptors[link->format].log2_chroma_h;

        for(i = 0; i < 4; i ++) {
            if(link->src_buf->data[i]) {
                src[i] = link->src_buf-> data[i] +
                    (y >> (i==0 ? 0 : vsub)) * link->src_buf-> linesize[i];
                dst[i] = link->cur_buf->data[i] +
                    (y >> (i==0 ? 0 : vsub)) * link->cur_buf->linesize[i];
            } else
                src[i] = dst[i] = NULL;
        }
        AVFILTER_GET_REF_VIDEO_PROPS(cur_buf_props, link->cur_buf);

        for(i = 0; i < 4; i ++) {
            int planew =
                ff_get_plane_bytewidth(link->format, cur_buf_props->w, i);

            if(!src[i]) continue;

            for(j = 0; j < h >> (i==0 ? 0 : vsub); j ++) {
                memcpy(dst[i], src[i], planew);
                src[i] += link->src_buf ->linesize[i];
                dst[i] += link->cur_buf->linesize[i];
            }
        }
    }

    if(!(draw_slice = link_dpad(link).draw_slice))
        draw_slice = avfilter_default_draw_slice;
    draw_slice(link, y, h, slice_dir);
}

void avfilter_filter_samples(AVFilterLink *link, AVFilterBufferRef *samplesref)
{
    AVFilterBufferRefAudioProps *sample_props, *cur_buf_props;
    void (*filter_samples)(AVFilterLink *, AVFilterBufferRef *);
    AVFilterPad *dst = &link_dpad(link);
    AVFILTER_GET_REF_AUDIO_PROPS(sample_props, samplesref);

    if (!(filter_samples = dst->filter_samples))
        filter_samples = avfilter_default_filter_samples;

    /* prepare to copy the samples if the buffer has insufficient permissions */
    if ((dst->min_perms & samplesref->perms) != dst->min_perms ||
        dst->rej_perms & samplesref->perms) {
        unsigned int i, num_channels, copy_size;

        av_log(link->dst, AV_LOG_INFO,
               "Copying audio data in avfilter (have perms %x, need %x, reject %x)\n",
               samplesref->perms, link_dpad(link).min_perms, link_dpad(link).rej_perms);

        link->cur_buf = avfilter_default_get_audio_buffer(link, dst->min_perms,
                                                         sample_props->size, sample_props->channel_layout,
                                                         samplesref->format, sample_props->planar);
        AVFILTER_GET_REF_AUDIO_PROPS(cur_buf_props, link->cur_buf);
        link->cur_buf->pts         = samplesref->pts;
        cur_buf_props->sample_rate = sample_props->sample_rate;

        /* Copy actual data into new samples buffer */

        /* FIXME: Need to use hamming weight count function instead once libavutil has the required function */
        num_channels = avcodec_channel_layout_num_channels(sample_props->channel_layout);
        copy_size = sample_props->size/num_channels;

        for (i = 0; i < num_channels; i++)
            memcpy(link->cur_buf->data[i], samplesref->data[i], copy_size);

        avfilter_unref_buffer(samplesref);
    }
    else
        link->cur_buf = samplesref;

    filter_samples(link, link->cur_buf);
}

#define MAX_REGISTERED_AVFILTERS_NB 64

static AVFilter *registered_avfilters[MAX_REGISTERED_AVFILTERS_NB + 1];

static int next_registered_avfilter_idx = 0;

AVFilter *avfilter_get_by_name(const char *name)
{
    int i;

    for (i = 0; registered_avfilters[i]; i++)
        if (!strcmp(registered_avfilters[i]->name, name))
            return registered_avfilters[i];

    return NULL;
}

int avfilter_register(AVFilter *filter)
{
    if (next_registered_avfilter_idx == MAX_REGISTERED_AVFILTERS_NB)
        return -1;

    registered_avfilters[next_registered_avfilter_idx++] = filter;
    return 0;
}

AVFilter **av_filter_next(AVFilter **filter)
{
    return filter ? ++filter : &registered_avfilters[0];
}

void avfilter_uninit(void)
{
    memset(registered_avfilters, 0, sizeof(registered_avfilters));
    next_registered_avfilter_idx = 0;
}

static int pad_count(const AVFilterPad *pads)
{
    int count;

    for(count = 0; pads->name; count ++) pads ++;
    return count;
}

static const char *filter_name(void *p)
{
    AVFilterContext *filter = p;
    return filter->filter->name;
}

static const AVClass avfilter_class = {
    "AVFilter",
    filter_name,
    NULL,
    LIBAVUTIL_VERSION_INT,
};

AVFilterContext *avfilter_open(AVFilter *filter, const char *inst_name)
{
    AVFilterContext *ret;

    if (!filter)
        return 0;

    ret = av_mallocz(sizeof(AVFilterContext));

    ret->av_class = &avfilter_class;
    ret->filter   = filter;
    ret->name     = inst_name ? av_strdup(inst_name) : NULL;
    ret->priv     = av_mallocz(filter->priv_size);

    ret->input_count  = pad_count(filter->inputs);
    if (ret->input_count) {
        ret->input_pads   = av_malloc(sizeof(AVFilterPad) * ret->input_count);
        memcpy(ret->input_pads, filter->inputs, sizeof(AVFilterPad) * ret->input_count);
        ret->inputs       = av_mallocz(sizeof(AVFilterLink*) * ret->input_count);
    }

    ret->output_count = pad_count(filter->outputs);
    if (ret->output_count) {
        ret->output_pads  = av_malloc(sizeof(AVFilterPad) * ret->output_count);
        memcpy(ret->output_pads, filter->outputs, sizeof(AVFilterPad) * ret->output_count);
        ret->outputs      = av_mallocz(sizeof(AVFilterLink*) * ret->output_count);
    }

    return ret;
}

void avfilter_destroy(AVFilterContext *filter)
{
    int i;

    if(filter->filter->uninit)
        filter->filter->uninit(filter);

    for(i = 0; i < filter->input_count; i ++) {
        if(filter->inputs[i])
            filter->inputs[i]->src->outputs[filter->inputs[i]->srcpad] = NULL;
        av_freep(&filter->inputs[i]);
    }
    for(i = 0; i < filter->output_count; i ++) {
        if(filter->outputs[i])
            filter->outputs[i]->dst->inputs[filter->outputs[i]->dstpad] = NULL;
        av_freep(&filter->outputs[i]);
    }

    av_freep(&filter->name);
    av_freep(&filter->input_pads);
    av_freep(&filter->output_pads);
    av_freep(&filter->inputs);
    av_freep(&filter->outputs);
    av_freep(&filter->priv);
    av_free(filter);
}

int avfilter_init_filter(AVFilterContext *filter, const char *args, void *opaque)
{
    int ret=0;

    if(filter->filter->init)
        ret = filter->filter->init(filter, args, opaque);
    return ret;
}

