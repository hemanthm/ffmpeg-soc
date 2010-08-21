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
#include "libavcodec/audioconvert.h"

typedef struct {

    short reconfig_channel_layout;        ///< set when channel layout of incoming buffer changes
    short reconfig_sample_fmt;            ///< set when sample format of incoming buffer changes

    enum SampleFormat in_sample_fmt;      ///< default incoming sample format expected
    enum SampleFormat out_sample_fmt;     ///< output sample format
    int64_t in_channel_layout;            ///< default incoming channel layout expected
    int64_t out_channel_layout;           ///< output channel layout

    int in_samples_nb;                    ///< stores number of samples in previous incoming buffer
    AVFilterBufferRef *s16_samples;      ///< stores temporary audio data in s16 sample format for channel layout conversions
    AVFilterBufferRef *s16_samples_ptr;  ///< duplicate pointer to audio data in s16 sample format
    AVFilterBufferRef *temp_samples;     ///< stores temporary audio data in s16 sample format after channel layout conversions
    AVFilterBufferRef *temp_samples_ptr; ///< duplicate pointer to audio data after channel layout conversions
    AVFilterBufferRef *out_samples;      ///< stores audio data after required sample format and channel layout conversions
    AVFilterBufferRef *out_samples_ptr;  ///< duplicate pointer to audio data after required conversions

    AVAudioConvert *conv_handle_s16;     ///< audio convert handle for conversion to s16 sample format
    AVAudioConvert *conv_handle_out;     ///< audio convert handle for conversion to output sample format

    void (*channel_conversion) (uint8_t *out[], uint8_t *in[], int , int); ///< channel conversion routine that will
                                                                       ///< point to one of the routines below
} ResampleContext;

/**
 * All of the routines below are for packed audio data. SDL accepts packed data
 * only and current ffplay also assumes packed data only at all times.
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
 * This is for when we have more than 2 input channels, need to downmix to
 * stereo and do not have a conversion formula available.  We just use first
 * two input channels - left and right. This is a placeholder until more
 * conversion functions are written.
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
 * This is for when we have more than 2 input channels, need to downmix to mono
 * and do not have a conversion formula available.  We just use first two input
 * channels - left and right. This is a placeholder until more conversion
 * functions are written.
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
    char sample_fmt_str[16], ch_layout_str[16];

    if (args) sscanf(args, "%15[a-z0-9]:%15[a-z0-9]", sample_fmt_str, ch_layout_str);

    resample->out_sample_fmt = avcodec_get_sample_fmt(sample_fmt_str);

    if (*sample_fmt_str && (resample->out_sample_fmt >= SAMPLE_FMT_NB ||
                            resample->out_sample_fmt <= SAMPLE_FMT_NONE)) {
        /**
         * SAMPLE_FMT_NONE is a valid value for out_sample_fmt and indicates no
         * change in sample format.
         */
        char *tail;
        resample->out_sample_fmt = strtol(sample_fmt_str, &tail, 10);
        if (*tail || resample->out_sample_fmt >= SAMPLE_FMT_NB ||
                     resample->out_sample_fmt < SAMPLE_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Invalid sample format %s\n", sample_fmt_str);
            return AVERROR(EINVAL);
        }
    }

    resample->out_channel_layout = *ch_layout_str ?
                                    avcodec_get_channel_layout(ch_layout_str) : -1;

    if (*ch_layout_str && resample->out_channel_layout < CH_LAYOUT_STEREO) {
        /**
         * -1 is a valid value for out_channel_layout and indicates no change
         * in channel layout.
         */
        char *tail;
        resample->out_channel_layout = strtol(ch_layout_str, &tail, 10);
        if (*tail || (resample->out_channel_layout < CH_LAYOUT_STEREO &&
                      resample->out_channel_layout != -1)) {
            av_log(ctx, AV_LOG_ERROR, "Invalid channel layout %s\n", ch_layout_str);
            return AVERROR(EINVAL);
        }
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
        avfilter_unref_buffer(resample->s16_samples);
    if (resample->temp_samples)
        avfilter_unref_buffer(resample->temp_samples);
    if (resample->out_samples)
        avfilter_unref_buffer(resample->out_samples);
    if (resample->conv_handle_s16)
        av_audio_convert_free(resample->conv_handle_s16);
    if (resample->conv_handle_out)
        av_audio_convert_free(resample->conv_handle_out);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;

    if (ctx->inputs[0]) {
        formats = NULL;
        formats = avfilter_all_formats(AVMEDIA_TYPE_AUDIO);
        avfilter_formats_ref(formats, &ctx->inputs[0]->out_formats);
    }
    if (ctx->outputs[0]) {
        formats = NULL;
        formats = avfilter_all_formats(AVMEDIA_TYPE_AUDIO);
        avfilter_formats_ref(formats, &ctx->outputs[0]->in_formats);
    }

    return 0;
}

static void convert_channel_layout(AVFilterLink *link)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterBufferRef *insamples = resample->s16_samples_ptr;
    AVFilterBufferRef *outsamples = resample->temp_samples;
    unsigned int num_ip_channels = avcodec_channel_layout_num_channels(resample->in_channel_layout);

    if (insamples)
        resample->in_channel_layout = insamples->audio->channel_layout;

    /* Init stage or input channels changed, so reconfigure conversion function pointer */
    if (resample->reconfig_channel_layout || !resample->channel_conversion) {

        int64_t in_channel = resample->in_channel_layout;
        int64_t out_channel = resample->out_channel_layout;

        int num_channels  = avcodec_channel_layout_num_channels(resample->out_channel_layout);
        int out_sample_size = av_get_bits_per_sample_format(insamples->format) >> 3;

        int size = num_channels*out_sample_size*insamples->audio->samples_nb;

        if (outsamples)
            avfilter_unref_buffer(outsamples);
        outsamples = avfilter_get_audio_buffer(link, AV_PERM_WRITE|AV_PERM_REUSE2,
                                               insamples->format, size,
                                               out_channel, 0);
        /*
         * Pick appropriate channel conversion function based on input-output channel layouts.
         * If no suitable conversion function is available, downmix to stereo and set buffer
         * channel layout to stereo.
         *
         * FIXME: Add error handling if channel conversion is unsupported, more channel conversion
         * routines and finally the ability to handle various stride lengths (sample formats).
         */

        if ((in_channel == CH_LAYOUT_STEREO) &&
            (out_channel == CH_LAYOUT_MONO))
            resample->channel_conversion = stereo_to_mono;
        else if ((in_channel == CH_LAYOUT_MONO) &&
                 (out_channel == CH_LAYOUT_STEREO))
            resample->channel_conversion = mono_to_stereo;
        else if ((in_channel == CH_LAYOUT_STEREO) &&
                 (out_channel == CH_LAYOUT_5POINT1))
            resample->channel_conversion = ac3_5p1_mux;
        else if (out_channel == CH_LAYOUT_MONO)
            resample->channel_conversion = mono_downmix;
        else {
            resample->channel_conversion = stereo_downmix;
            outsamples->audio->channel_layout = CH_LAYOUT_STEREO;
        }

    }

    if (outsamples && insamples) {
        resample->channel_conversion(outsamples->data, insamples->data,
                                     outsamples->audio->samples_nb,
                                     num_ip_channels);
    }
    resample->temp_samples     = outsamples;
    resample->temp_samples_ptr = outsamples;
}

static void convert_sample_format_wrapper(AVFilterLink *link)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterBufferRef *insamples = resample->temp_samples_ptr;
    AVFilterBufferRef *outsamples = resample->out_samples;
    int out_channels, out_sample_size, planar, len;

    /* Here, out_channels is same as input channels, we are only changing
     * sample format. */
    /* FIXME: Need to use hamming weight counting function instead once it is
     * added to libavutil. */
    out_channels  = avcodec_channel_layout_num_channels(insamples->audio->channel_layout);
    out_sample_size = av_get_bits_per_sample_format(resample->out_sample_fmt) >> 3;

    planar   = insamples->audio->planar;
    len      = insamples->audio->samples_nb;

    if (resample->reconfig_sample_fmt || !outsamples ||
        !outsamples->audio->size) {

        int size = out_channels*out_sample_size*insamples->audio->samples_nb;

        if (outsamples)
            avfilter_unref_buffer(outsamples);
        outsamples = avfilter_get_audio_buffer(link, AV_PERM_WRITE|AV_PERM_REUSE2,
                                               resample->out_sample_fmt, size,
                                               insamples->audio->channel_layout, 0);

        if (resample->conv_handle_out)
            av_audio_convert_free(resample->conv_handle_out);
        resample->conv_handle_out = av_audio_convert_alloc(resample->out_sample_fmt,
                                                           out_channels, insamples->format,
                                                           out_channels, NULL, 0);
    }

    /* Timestamp and sample rate can change even while sample format/channel layout remain the same */
    outsamples->pts                = insamples->pts;
    outsamples->audio->sample_rate = insamples->audio->sample_rate;

    av_audio_convert(resample->conv_handle_out, (void* const*)outsamples->data,
                     outsamples->linesize, (const void* const*)insamples->data,
                     insamples->linesize, len);

    resample->out_samples     = outsamples;
    resample->out_samples_ptr = outsamples;

}

static void convert_s16_format_wrapper(AVFilterLink *link, AVFilterBufferRef *insamples)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterBufferRef *outsamples = resample->s16_samples;
    int out_channels, planar, len;

    /* Here, out_channels is same as input channels, we are only changing sample format */
    out_channels  = avcodec_channel_layout_num_channels(insamples->audio->channel_layout);

    planar   = insamples->audio->planar;
    len      = insamples->audio->samples_nb;

    if (resample->reconfig_sample_fmt || !outsamples || !outsamples->audio->size) {

        int size = out_channels*2*insamples->audio->samples_nb;

        if (outsamples)
            avfilter_unref_buffer(outsamples);
        outsamples = avfilter_get_audio_buffer(link, AV_PERM_WRITE|AV_PERM_REUSE2,
                                               SAMPLE_FMT_S16, size,
                                               insamples->audio->channel_layout, 0);

        if (resample->conv_handle_s16)
            av_audio_convert_free(resample->conv_handle_s16);
        resample->conv_handle_s16 = av_audio_convert_alloc(SAMPLE_FMT_S16, out_channels,
                                                           insamples->format, out_channels,
                                                           NULL, 0);
    }

    /* Timestamp and sample rate can change even while sample format/channel layout remain the same */
    outsamples->pts                 = insamples->pts;
    outsamples->audio->sample_rate  = insamples->audio->sample_rate;

    av_audio_convert(resample->conv_handle_s16, (void* const*)outsamples->data,
                     outsamples->linesize, (const void* const*)insamples->data,
                     insamples->linesize, len);

    resample->s16_samples     = outsamples;
    resample->s16_samples_ptr = outsamples;
}

static int config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    ResampleContext *resample = ctx->priv;

    if (resample->out_channel_layout == -1)
        resample->out_channel_layout = link->channel_layout;

    if (resample->out_sample_fmt == -1)
        resample->out_sample_fmt = link->format;

    return 0;
}

static void filter_samples(AVFilterLink *link, AVFilterBufferRef *samplesref)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    short samples_nb_changed = 0;

   /**
    * If input data of this buffer differs from the earlier buffer/s, set flag
    * to reconfigure the channel and sample format conversions.
    */

    samples_nb_changed = (samplesref->audio->samples_nb != resample->in_samples_nb);
    resample->in_samples_nb = samplesref->audio->samples_nb;
    resample->reconfig_sample_fmt = (samplesref->format != resample->in_sample_fmt) || samples_nb_changed;
    resample->in_sample_fmt = samplesref->format;
    resample->reconfig_channel_layout = (samplesref->audio->channel_layout != resample->in_channel_layout) || samples_nb_changed;
    resample->in_channel_layout = samplesref->audio->channel_layout;

    /* Convert to s16 sample format first, then to desired channel layout  and finally to desired sample format */

    if (samplesref->format == SAMPLE_FMT_S16)
        resample->s16_samples_ptr = samplesref;
    else
        convert_s16_format_wrapper(link, samplesref);

    if (samplesref->audio->channel_layout == resample->out_channel_layout)
        resample->temp_samples_ptr = resample->s16_samples_ptr;
    else
        convert_channel_layout(link);

    if (resample->out_sample_fmt == SAMPLE_FMT_S16)
        resample->out_samples_ptr = resample->temp_samples_ptr;
    else
        convert_sample_format_wrapper(link);

    avfilter_filter_samples(outlink, avfilter_ref_buffer(resample->out_samples_ptr, ~0));
    avfilter_unref_buffer(samplesref);
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
