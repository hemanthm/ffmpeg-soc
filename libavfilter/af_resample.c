/*
 * copyright (c) 2010 S.N. Hemanth Meenakshisundaram <smeenaks@ucsd.edu>
 * based on code in libavcodec/resample.c by Fabrice Bellard
 * and libavcodec/audioconvert.c by Michael Neidermayer
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

typedef struct {

    short reconfig_channel_layout;     ///< set when channel layout of incoming buffer changes
    short reconfig_sample_fmt;         ///< set when sample format of incoming buffer changes

    enum SampleFormat in_sample_fmt;   ///< default incoming sample format expected
    enum SampleFormat out_sample_fmt;  ///< output sample format
    int64_t in_channel_layout;         ///< default incoming channel layout expected
    int64_t out_channel_layout;        ///< output channel layout

    int in_samples_nb;                 ///< stores number of samples in previous incoming buffer
    AVFilterSamplesRef *s16_samples;   ///< stores temporary audio data in s16 sample format for channel layout conversions
    AVFilterSamplesRef *temp_samples;  ///< stores temporary audio data in s16 sample format after channel layout conversions
    AVFilterSamplesRef *out_samples;   ///< stores audio data after required sample format and channel layout conversions

    void (*channel_conversion) (uint8_t *out[], uint8_t *in[], int , int); ///< channel conversion routine that will
                                                                       ///< point to one of the routines below
} ResampleContext;

/**
 * All of the routines below are for packed audio data. SDL accepts packed data only and current ffplay also assumes
 * packed data only at all times.
 */

/* Optimized stereo to mono and mono to stereo routines - common case */
static void stereo_to_mono(uint8_t *out[], uint8_t *in[], int samples_nb, int in_channels)
{
    short *input  = (short *) in[0];
    short *output = (short *) out[0];

    while (samples_nb >= 4) {
        output[0] = (input[0] + input[1]) >> 1;
        output[1] = (input[2] + input[3]) >> 1;
        output[2] = (input[4] + input[5]) >> 1;
        output[3] = (input[6] + input[7]) >> 1;
        output += 4;
        input += 8;
        samples_nb -= 4;
    }
    while (samples_nb > 0) {
        output[0] = (input[0] + input[1]) >> 1;
        output++;
        input += 2;
        samples_nb--;
    }
}

static void mono_to_stereo(uint8_t *out[], uint8_t *in[], int samples_nb, int in_channels)
{
    int v;
    short *input  = (short *) in[0];
    short *output = (short *) out[0];


    while (samples_nb >= 4) {
        v = input[0]; output[0] = v; output[1] = v;
        v = input[1]; output[2] = v; output[3] = v;
        v = input[2]; output[4] = v; output[5] = v;
        v = input[3]; output[6] = v; output[7] = v;
        output += 8;
        input += 4;
        samples_nb -= 4;
    }
    while (samples_nb > 0) {
        v = input[0]; output[0] = v; output[1] = v;
        output += 2;
        input += 1;
        samples_nb--;
    }
}

/**
 * This is for when we have more than 2 input channels, need to downmix to stereo and do not have a 
 * conversion formula available.  We just use first two input channels - left and right. This is
 * a placeholder until more conversion functions are written.
 */
static void stereo_downmix(uint8_t *out[], uint8_t *in[], int samples_nb, int in_channels)
{
    int i;
    short *output = (short *) out[0];
    short *input = (short *) out[0];

    for (i = 0; i < samples_nb; i++) {
        *output++ = *input++;
        *output++ = *input++;
        input+=(in_channels-2);       
    }
}

/**
 * This is for when we have more than 2 input channels, need to downmix to mono and do not have a 
 * conversion formula available.  We just use first two input channels - left and right. This is
 * a placeholder until more conversion functions are written.
 */
static void mono_downmix(uint8_t *out[], uint8_t *in[], int samples_nb, int in_channels)
{
    int i;
    short *input = (short *) in[0];
    short *output = (short *) out[0];
    short left, right;

    for (i = 0; i < samples_nb; i++) {
        left = *input++;
        right = *input++;
        *output++ = (left>>1)+(right>>1);
        input+=(in_channels-2);
    }
}

/* Stereo to 5.1 output */
static void ac3_5p1_mux(uint8_t *out[], uint8_t *in[], int samples_nb, int in_channels)
{
    int i;
    short *output = (short *) out[0];
    short *input = (short *) in[0];
    short left, right;

    for (i = 0; i < samples_nb; i++) {
      left  = *input++;                 /* Grab next left sample */
      right = *input++;                 /* Grab next right sample */
      *output++ = left;                 /* left */
      *output++ = right;                /* right */
      *output++ = (left>>1)+(right>>1); /* center */
      *output++ = 0;                    /* low freq */
      *output++ = 0;                    /* FIXME: left surround is either -3dB, -6dB or -9dB of stereo left */
      *output++ = 0;                    /* FIXME: right surroud is either -3dB, -6dB or -9dB of stereo right */
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

    /* Set default values for expected incoming sample format and channel layout */
    resample->in_channel_layout = CH_LAYOUT_STEREO;
    resample->in_sample_fmt     = SAMPLE_FMT_S16;
    resample->in_samples_nb     = 0;
    /* We do not yet know the channel conversion function to be used */
    resample->channel_conversion = NULL;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ResampleContext *resample = ctx->priv;
    if (resample->s16_samples)
        avfilter_unref_samples(resample->s16_samples);
    if (resample->temp_samples)
        avfilter_unref_samples(resample->temp_samples);
    if (resample->out_samples)
        avfilter_unref_samples(resample->out_samples);
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
    AVFilterSamplesRef *outsamples = resample->temp_samples;
    AVFilterSamplesRef *insamples = resample->s16_samples;
    unsigned int num_ip_channels = avcodec_channel_layout_num_channels(resample->in_channel_layout);

    if (insamples)
        resample->in_channel_layout = insamples->channel_layout;

    /* Init stage or input channels changed, so reconfigure conversion function pointer */
    if (resample->reconfig_channel_layout || !resample->channel_conversion) {

        int64_t in_channel = resample->in_channel_layout;
        int64_t out_channel = resample->out_channel_layout;

        /*
         * Pick appropriate channel conversion function based on input-output channel layouts.
         * If no suitable conversion function is available, downmix to stereo and set buffer
         * channel layout to stereo.
         *
         * FIXME: Add error handling if channel conversion is unsupported, more channel conversion
         * routines and finally the ability to handle various stride lengths (sample formats).
         *
         */

        if ((in_channel == CH_LAYOUT_STEREO) && (out_channel == CH_LAYOUT_MONO))
            resample->channel_conversion = stereo_to_mono;
        else if ((in_channel == CH_LAYOUT_MONO) && (out_channel == CH_LAYOUT_STEREO))
            resample->channel_conversion = mono_to_stereo;
        else if ((in_channel == CH_LAYOUT_STEREO) && (out_channel == CH_LAYOUT_5POINT1))
            resample->channel_conversion = ac3_5p1_mux;
        else if (out_channel == CH_LAYOUT_MONO)
            resample->channel_conversion = mono_downmix;
        else {
            resample->channel_conversion = stereo_downmix;
            outsamples->channel_layout = CH_LAYOUT_STEREO;
        }

    }

    if (outsamples && insamples) {
        resample->channel_conversion(outsamples->data, insamples->data,
                                     outsamples->samples_nb, num_ip_channels);
    }

}

static void convert_sample_fmt(AVFilterLink *link, AVFilterSamplesRef *insamples, AVFilterSamplesRef *out,
                               int len, int out_sample_size, int out_channels, int fmt_pair, int planar)
{
    int in_sample_size = av_get_bits_per_sample_format(insamples->sample_fmt) >> 3;
    int ch = 0;

    do {
        const uint8_t *pi =  insamples->data[ch];
        uint8_t *po   = out->data[ch];
        int instride  = (planar) ? insamples->linesize[ch] : in_sample_size;
        int outstride = (planar) ? out->linesize[ch] : out_sample_size;
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

    return;
}

static void convert_sample_format_wrapper(AVFilterLink *link)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterSamplesRef *insamples = resample->temp_samples;
    AVFilterSamplesRef *out = resample->out_samples;

    /* Here, out_channels is same as input channels, we are only changing sample format */
    /* FIXME: Need to use hamming weight counting function instead once it is added to libavutil */
    int out_channels  = avcodec_channel_layout_num_channels(insamples->channel_layout);
    int out_sample_size = av_get_bits_per_sample_format(resample->out_sample_fmt) >> 3;

    int planar   = insamples->planar;
    int len      = (planar) ? insamples->samples_nb : insamples->samples_nb*out_channels;
    int fmt_pair = insamples->sample_fmt*SAMPLE_FMT_NB+resample->out_sample_fmt;

    if (resample->reconfig_sample_fmt || !out || !out->size) {

        int size = out_channels*out_sample_size*insamples->samples_nb;

        if (out)
            avfilter_unref_samples(out);
        out = avfilter_get_samples_ref(link, AV_PERM_WRITE, size,
                                       insamples->channel_layout, resample->out_sample_fmt, 0);

    }

    /* Timestamp and sample rate can change even while sample format/channel layout remain the same */
    out->pts          = insamples->pts;
    out->sample_rate  = insamples->sample_rate;

    convert_sample_fmt(link, insamples, out, len, out_sample_size, out_channels, fmt_pair, planar);
    resample->out_samples = out;

}

static void convert_s16_format_wrapper(AVFilterLink *link, AVFilterSamplesRef *insamples)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterSamplesRef *out = resample->s16_samples;

    /* Here, out_channels is same as input channels, we are only changing sample format */
    int out_channels  = avcodec_channel_layout_num_channels(insamples->channel_layout);

    int planar   = insamples->planar;
    int len      = (planar) ? insamples->samples_nb : insamples->samples_nb*out_channels;
    int fmt_pair = insamples->sample_fmt*SAMPLE_FMT_NB+SAMPLE_FMT_S16;

    if (resample->reconfig_sample_fmt || !out || !out->size) {

        int size = out_channels*2*insamples->samples_nb;

        if (out)
            avfilter_unref_samples(out);
        out = avfilter_get_samples_ref(link, AV_PERM_WRITE, size,
                                       insamples->channel_layout, SAMPLE_FMT_S16, 0);

    }

    /* Timestamp and sample rate can change even while sample format/channel layout remain the same */
    out->pts          = insamples->pts;
    out->sample_rate  = insamples->sample_rate;

    convert_sample_fmt(link, insamples, out, len, 2, out_channels, fmt_pair, planar);
    resample->s16_samples = out;

}

static int config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    ResampleContext *resample = ctx->priv;

    if (resample->out_channel_layout == -1)
        resample->out_channel_layout = link->channel_layout;

    if (resample->out_sample_fmt == -1)
        resample->out_sample_fmt = link->aformat;

    return 0;
}

static void filter_samples(AVFilterLink *link, AVFilterSamplesRef *samplesref)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    short samples_nb_changed = 0;

   /**
    * If input data of this buffer differs from the earlier buffer/s, set flag
    * to reconfigure the channel and sample format conversions.
    */

    samples_nb_changed = (samplesref->samples_nb != resample->in_samples_nb);
    resample->in_samples_nb = samplesref->samples_nb;
    resample->reconfig_sample_fmt = (samplesref->sample_fmt != resample->in_sample_fmt) || samples_nb_changed;
    resample->in_sample_fmt = samplesref->sample_fmt;
    resample->reconfig_channel_layout = (samplesref->channel_layout != resample->in_channel_layout) || samples_nb_changed;
    resample->in_channel_layout = samplesref->channel_layout;

    /* Convert to s16 sample format first, then to desired channel layout  and finally to desired sample format */

    if (samplesref->sample_fmt == SAMPLE_FMT_S16)
        resample->s16_samples = samplesref;
    else
        convert_s16_format_wrapper(link, samplesref);

    if (samplesref->channel_layout == resample->out_channel_layout)
        resample->temp_samples = resample->s16_samples;
    else
        convert_channel_layout(link);

    if (resample->out_sample_fmt == SAMPLE_FMT_S16)
        resample->out_samples = resample->temp_samples;
    else
        convert_sample_format_wrapper(link);

    avfilter_filter_samples(outlink, avfilter_ref_samples(resample->out_samples, ~0));
    avfilter_unref_samples(samplesref);
}

AVFilter avfilter_af_resample = {
    .name        = "resample",
    .description = NULL_IF_CONFIG_SMALL("Reformat the input audio to sample_fmt:channel_layout."),

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
