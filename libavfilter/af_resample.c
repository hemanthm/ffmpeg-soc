/*
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

/**
 * @file
 * resample audio filter
 */

#include "avfilter.h"
#include "libavutil/avutil.h"
#include "libavutil/audiodesc.h"

typedef struct {

    short reconfig_channel_layout;     ///< set when channel layout of incoming buffer changes
    short reconfig_sample_fmt;         ///< set when sample format of incoming buffer changes

    enum SampleFormat in_sample_fmt;   ///< default incoming sample format expected
    enum SampleFormat out_sample_fmt;  ///< output sample format
    int64_t in_channel_layout;         ///< default incoming channel layout expected
    int64_t out_channel_layout;        ///< output channel layout
    
     AVFilterSamplesRef *temp_samples; ///< Stores temporary audio data between sample format and channel layout conversions

} ResampleContext;

static void stereo_to_mono(short *output, short *input, int samples_nb)
{
    short *p, *q;
    int n = samples_nb;

    p = input;
    q = output;
    while (n >= 4) {
        q[0] = (p[0] + p[1]) >> 1;
        q[1] = (p[2] + p[3]) >> 1;
        q[2] = (p[4] + p[5]) >> 1;
        q[3] = (p[6] + p[7]) >> 1;
        q += 4;
        p += 8;
        n -= 4;
    }
    while (n > 0) {
        q[0] = (p[0] + p[1]) >> 1;
        q++;
        p += 2;
        n--;
    }
}

static void mono_to_stereo(short *output, short *input, int samples_nb)
{
    short *p, *q;
    int n = samples_nb;
    int v;

    p = input;
    q = output;
    while (n >= 4) {
        v = p[0]; q[0] = v; q[1] = v;
        v = p[1]; q[2] = v; q[3] = v;
        v = p[2]; q[4] = v; q[5] = v;
        v = p[3]; q[6] = v; q[7] = v;
        q += 8;
        p += 4;
        n -= 4;
    }
    while (n > 0) {
        v = p[0]; q[0] = v; q[1] = v;
        q += 2;
        p += 1;
        n--;
    }
}

/* FIXME: should use more abstract 'N' channels system */
static void stereo_split(short *output1, short *output2, short *input, int n)
{
    int i;

    for(i=0;i<n;i++) {
        *output1++ = *input++;
        *output2++ = *input++;
    }
}

static void stereo_mux(short *output, short *input1, short *input2, int n)
{
    int i;

    for(i=0;i<n;i++) {
        *output++ = *input1++;
        *output++ = *input2++;
    }
}

static void ac3_5p1_mux(short *output, short *input1, short *input2, int n)
{
    int i;
    short l,r;

    for(i=0;i<n;i++) {
      l=*input1++;
      r=*input2++;
      *output++ = l;           /* left */
      *output++ = (l/2)+(r/2); /* center */
      *output++ = r;           /* right */
      *output++ = 0;           /* left surround */
      *output++ = 0;           /* right surroud */
      *output++ = 0;           /* low freq */
    }
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    ResampleContext *resample = ctx->priv;

    if (args){
        sscanf(args, "%d:%ld", &resample->out_sample_fmt, &resample->out_channel_layout);
    }

    /**
     * sanity check params
     * SAMPLE_FMT_NONE is a valid value for out_sample_fmt and indicates no change in sample format
     * -1 is a valid value for out_channel_layout and indicates no change in channel layout
     */
    if (resample->out_sample_fmt >= SAMPLE_FMT_NB || resample->out_sample_fmt < SAMPLE_FMT_NONE) {
        av_log(ctx, AV_LOG_ERROR, "Invalid sample format %d, cannot resample.\n", resample->out_sample_fmt);
        return AVERROR(EINVAL);
    }
    if ((resample->out_channel_layout > CH_LAYOUT_STEREO_DOWNMIX ||
         resample->out_channel_layout < CH_LAYOUT_STEREO) && (resample->out_channel_layout != -1)) {
        av_log(ctx, AV_LOG_ERROR, "Invalid channel layout %ld, cannot resample.\n", resample->out_channel_layout);
        return AVERROR(EINVAL);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ResampleContext *resample = ctx->priv;
    if (resample->temp_samples)
        avfilter_unref_samples(resample->temp_samples);
}

static int query_formats(AVFilterContext *ctx)
{
    enum SampleFormat sample_fmts[] = {
        SAMPLE_FMT_U8, SAMPLE_FMT_S16, SAMPLE_FMT_S32,
        SAMPLE_FMT_FLT, SAMPLE_FMT_DBL, SAMPLE_FMT_NONE };
    avfilter_set_common_formats(ctx, avfilter_make_aformat_list(sample_fmts));

    return 0;
}

static void convert_channel_layout(AVFilterLink *link)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFilterSamplesRef *outsamples = outlink->out_samples;
    AVFilterSamplesRef *insamples = resample->temp_samples;

    if (resample->reconfig_channel_layout) {
    /* Initialize input/output strides, intermediate buffers etc. */
    }

    if (outsamples && insamples)
        memcpy(outsamples->data[0], insamples->data[0], outsamples->size);

    /* TODO: Convert to required channel layout using functions above and populate output audio buffer */
}

static void convert_sample_format(AVFilterLink *link, AVFilterSamplesRef *insamples)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterSamplesRef *out = NULL;

    int ch = 0;
    /* Here, out_channels is same as input channels, we are only changing sample format */
    int out_channels  = av_channel_layout_num_channels(insamples->channel_layout);
    int out_sample_sz = (av_get_bits_per_sample_fmt(resample->out_sample_fmt) >> 3);
    int in_sample_sz  = (av_get_bits_per_sample_fmt(insamples->sample_fmt) >> 3);

    int planar   = insamples->planar;
    int len      = (planar) ? insamples->samples_nb : insamples->samples_nb*out_channels;
    int fmt_pair = insamples->sample_fmt*SAMPLE_FMT_NB+resample->out_sample_fmt;

    if (resample->reconfig_sample_fmt || !out || !out->size) {

        int size = out_channels*out_sample_sz*insamples->samples_nb;

        if (out)
            avfilter_unref_samples(out);
        out = avfilter_get_samples_ref(link, AV_PERM_WRITE, size,
                                   insamples->channel_layout, resample->out_sample_fmt, 0);

    }

    /* Timestamp and sample rate can change even while sample format/channel layout remain the same */
    out->pts          = insamples->pts;
    out->sample_rate  = insamples->sample_rate;

    do {
        const uint8_t *pi =  insamples->data[ch];
        uint8_t *po   = out->data[ch];
        int instride  = (planar) ? insamples->linesize[ch] : in_sample_sz;
        int outstride = (planar) ? out->linesize[ch] : out_sample_sz;
        uint8_t *end  = po + outstride*len;

        if(!out->data[ch])
            continue;

#define CONV(ofmt, otype, ifmt, expr)\
if (fmt_pair == ofmt + SAMPLE_FMT_NB*ifmt) {\
    do {\
        *(otype*)po = expr; pi += instride; po += outstride;\
    } while(po < end);\
}

        CONV(SAMPLE_FMT_U8 , uint8_t, SAMPLE_FMT_U8 ,  *(const uint8_t*)pi)
        else CONV(SAMPLE_FMT_S16, int16_t, SAMPLE_FMT_U8 , (*(const uint8_t*)pi - 0x80)<<8)
        else CONV(SAMPLE_FMT_S32, int32_t, SAMPLE_FMT_U8 , (*(const uint8_t*)pi - 0x80)<<24)
        else CONV(SAMPLE_FMT_FLT, float  , SAMPLE_FMT_U8 , (*(const uint8_t*)pi - 0x80)*(1.0 / (1<<7)))
        else CONV(SAMPLE_FMT_DBL, double , SAMPLE_FMT_U8 , (*(const uint8_t*)pi - 0x80)*(1.0 / (1<<7)))
        else CONV(SAMPLE_FMT_U8 , uint8_t, SAMPLE_FMT_S16, (*(const int16_t*)pi>>8) + 0x80)
        else CONV(SAMPLE_FMT_S16, int16_t, SAMPLE_FMT_S16,  *(const int16_t*)pi)
        else CONV(SAMPLE_FMT_S32, int32_t, SAMPLE_FMT_S16,  *(const int16_t*)pi<<16)
        else CONV(SAMPLE_FMT_FLT, float  , SAMPLE_FMT_S16,  *(const int16_t*)pi*(1.0 / (1<<15)))
        else CONV(SAMPLE_FMT_DBL, double , SAMPLE_FMT_S16,  *(const int16_t*)pi*(1.0 / (1<<15)))
        else CONV(SAMPLE_FMT_U8 , uint8_t, SAMPLE_FMT_S32, (*(const int32_t*)pi>>24) + 0x80)
        else CONV(SAMPLE_FMT_S16, int16_t, SAMPLE_FMT_S32,  *(const int32_t*)pi>>16)
        else CONV(SAMPLE_FMT_S32, int32_t, SAMPLE_FMT_S32,  *(const int32_t*)pi)
        else CONV(SAMPLE_FMT_FLT, float  , SAMPLE_FMT_S32,  *(const int32_t*)pi*(1.0 / (1<<31)))
        else CONV(SAMPLE_FMT_DBL, double , SAMPLE_FMT_S32,  *(const int32_t*)pi*(1.0 / (1<<31)))
        else CONV(SAMPLE_FMT_U8 , uint8_t, SAMPLE_FMT_FLT, av_clip_uint8(  lrintf(*(const float*)pi * (1<<7)) + 0x80))
        else CONV(SAMPLE_FMT_S16, int16_t, SAMPLE_FMT_FLT, av_clip_int16(  lrintf(*(const float*)pi * (1<<15))))
        else CONV(SAMPLE_FMT_S32, int32_t, SAMPLE_FMT_FLT, av_clipl_int32(llrintf(*(const float*)pi * (1U<<31))))
        else CONV(SAMPLE_FMT_FLT, float  , SAMPLE_FMT_FLT, *(const float*)pi)
        else CONV(SAMPLE_FMT_DBL, double , SAMPLE_FMT_FLT, *(const float*)pi)
        else CONV(SAMPLE_FMT_U8 , uint8_t, SAMPLE_FMT_DBL, av_clip_uint8(  lrint(*(const double*)pi * (1<<7)) + 0x80))
        else CONV(SAMPLE_FMT_S16, int16_t, SAMPLE_FMT_DBL, av_clip_int16(  lrint(*(const double*)pi * (1<<15))))
        else CONV(SAMPLE_FMT_S32, int32_t, SAMPLE_FMT_DBL, av_clipl_int32(llrint(*(const double*)pi * (1U<<31))))
        else CONV(SAMPLE_FMT_FLT, float  , SAMPLE_FMT_DBL, *(const double*)pi)
        else CONV(SAMPLE_FMT_DBL, double , SAMPLE_FMT_DBL, *(const double*)pi)

    } while (ch < insamples->planar*out_channels);

    resample->temp_samples = out;
    return;
}

static int config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    ResampleContext *resample = ctx->priv;

    if (resample->out_channel_layout == -1)
        resample->out_channel_layout = link->channel_layout;

    if (resample->out_sample_fmt == -1)
        resample->out_sample_fmt = link->aformat;

    /* Call the channel layout conversion routine to prepare for default conversion. */

    resample->reconfig_channel_layout = 1;
    convert_channel_layout(link);
    
    return 0;
}

static void filter_samples(AVFilterLink *link, AVFilterSamplesRef *samplesref)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFilterSamplesRef *outsamples;
    int size = 0;

#define CALC_BUFFER_SIZE(nsamp, ch, samples) {\
    int n_chan = av_channel_layout_num_channels(ch);\
    int n_stride = (av_get_bits_per_sample_format(samples) >> 3);\
    size = nsamp*n_chan*n_stride;\
}
    CALC_BUFFER_SIZE(samplesref->samples_nb, resample->out_channel_layout, resample->out_sample_fmt);
    outsamples = avfilter_get_samples_ref(outlink, AV_PERM_WRITE, size,
                                          resample->out_channel_layout, resample->out_sample_fmt, 0);

    outsamples->pts         = samplesref->pts;
    outsamples->planar      = samplesref->planar;
    outsamples->sample_rate = samplesref->sample_rate;

    outlink->out_samples    = outsamples;

   /**
    * If input data of this buffer differs from the earlier buffer/s, set flag
    * to reconfigure the channel and sample format conversions.
    */

   resample->reconfig_sample_fmt = (samplesref->sample_fmt != resample->out_sample_fmt);
   resample->reconfig_channel_layout = (samplesref->channel_layout != resample->out_channel_layout);

   /* Convert to desired output sample format first and then to desired channel layout */

   convert_sample_format(link, samplesref);
   convert_channel_layout(link);

   avfilter_filter_samples(outlink, avfilter_ref_samples(outsamples, ~0));
}

AVFilter avfilter_af_resample = {
    .name        = "resample",
    .description = "Reformat the input audio to sample_fmt:channel_layout.",

    .init      = init,
    .uninit    = uninit,

    .query_formats = query_formats,

    .priv_size = sizeof(ResampleContext),

    .inputs    = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO,
                                    .filter_samples   = filter_samples,
                                    .config_props     = config_props,
                                    .min_perms        = AV_PERM_READ, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO, },
                                  { .name = NULL}},
};
