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
#include "libavutil/avutil.h"
#include "libavutil/audiodesc.h"

typedef struct {

    short reconfig_channel_layout;     ///< set when channel layout of incoming buffer changes
    short reconfig_sample_fmt;         ///< set when sample format of incoming buffer changes

    enum SampleFormat in_sample_fmt;   ///< default incoming sample format expected
    enum SampleFormat out_sample_fmt;  ///< output sample format
    int64_t in_channel_layout;         ///< default incoming channel layout expected
    int64_t out_channel_layout;        ///< output channel layout

    AVFilterSamplesRef *temp_samples;  ///< Stores temporary audio data between sample format and channel layout conversions

} ResampleContext;

/* channel_conversion will point to one of the required channel conversion routines below */
static void (*channel_conversion) (short *output1, short *output2, short *input1, short *input2, int samples_nb, int dummy);

/* All of the routines below may have some dummy arguments that are never used. This is to present a uniform interface */

static void stereo_to_mono(short *output, short *dummy_out, short *input, short *dummy_in, int samples_nb, int dummy)
{
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

static void mono_to_stereo(short *output, short *dummy_out, short *input, short *dummy_in, int samples_nb, int dummy)
{
    int v;

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

/* FIXME: should use more abstract 'N' channels system */
static void stereo_demux(short *output1, short *output2, short *input, short *dummy_in, int samples_nb, int dummy)
{
    int i;

    for (i = 0; i < samples_nb; i++) {
        *output1++ = *input++;
        *output2++ = *input++;
    }
}

static void stereo_mux(short *output, short *dummy_out, short *input1, short *input2, int samples_nb, int dummy)
{
    int i;

    for (i = 0; i < samples_nb; i++) {
        *output++ = *input1++;
        *output++ = *input2++;
    }
}

static void ac3_5p1_mux(short *output, short *dummy_out, short *input1, short *input2, int samples_nb, int dummy)
{
    int i;
    short left, right;

    for (i = 0; i < samples_nb; i++) {
      left  = *input1++;
      right = *input2++;
      *output++ = left;               /* left */
      *output++ = (left/2)+(right/2); /* center */
      *output++ = right;              /* right */
      *output++ = 0;                  /* left surround */
      *output++ = 0;                  /* right surroud */
      *output++ = 0;                  /* low freq */
    }
}

static void channel_copy(short *output, short *dummy_out, short *input, short *dummy_in, int dummy, int size)
{
    memcpy(output, input, size);
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
    /* We do not yet know the channel conversion function to be used */
    channel_conversion = NULL;

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

    if (insamples)
        resample->in_channel_layout = insamples->channel_layout;

    /* Init stage or input channels changed, so reconfigure conversion function pointer */
    if (resample->reconfig_channel_layout || !channel_conversion) {

        int planar   = insamples->planar;
        int64_t in_channel = resample->in_channel_layout;
        int64_t out_channel = resample->out_channel_layout;

        /*
         * Pick appropriate channel conversion function based on input-output channel layouts
         * and on whether buffer is planar or packed. If no suitable conversion function is
         * available, blindly copy the buffer and hope for the best.
         *
         * FIXME: Add error handling if channel conversion is unsupported, more channel conversion
         * routines and finally the ability to handle various stride lengths (sample formats).
         *
         */
        if ((in_channel == CH_LAYOUT_STEREO) && (out_channel == CH_LAYOUT_MONO))
            channel_conversion = (planar) ? stereo_mux : stereo_to_mono;
        else if ((in_channel == CH_LAYOUT_MONO) && (out_channel == CH_LAYOUT_STEREO))
            channel_conversion = (planar) ? stereo_demux : mono_to_stereo;
        else if ((in_channel == CH_LAYOUT_STEREO) && (out_channel == CH_LAYOUT_5POINT1))
            channel_conversion = (planar) ? channel_copy : ac3_5p1_mux;
        else
            channel_conversion = channel_copy;

    }

    if (outsamples && insamples) {
        channel_conversion ((short *)outsamples->data[0], (short *)outsamples->data[1],
                            (short *)insamples->data[0], (short *)insamples->data[1],
                            outsamples->samples_nb, outsamples->size);
    }

}

static void convert_sample_format(AVFilterLink *link, AVFilterSamplesRef *insamples)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterSamplesRef *out = NULL;
    const SampleFmtInfo *in_fmt_info = &sample_fmt_info[insamples->sample_fmt];
    const SampleFmtInfo *out_fmt_info = &sample_fmt_info[resample->out_sample_fmt];

    int ch = 0;
    /* Here, out_channels is same as input channels, we are only changing sample format */
    int out_channels  = av_get_hamming_weight(insamples->channel_layout);
    int out_sample_sz = (out_fmt_info->bits) >> 3;
    int in_sample_sz  = (in_fmt_info->bits) >> 3;

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

    return 0;
}

static void filter_samples(AVFilterLink *link, AVFilterSamplesRef *samplesref)
{
    ResampleContext *resample = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFilterSamplesRef *outsamples;
    const SampleFmtInfo *reqd_sample_info = &sample_fmt_info[resample->out_sample_fmt];
    int size = 0;

#define CALC_BUFFER_SIZE(nsamp, ch, sample_info) {\
    int n_chan = av_get_hamming_weight(ch);\
    int n_stride = (sample_info->bits) >> 3;\
    size = nsamp*n_chan*n_stride;\
}
    CALC_BUFFER_SIZE(samplesref->samples_nb, resample->out_channel_layout, reqd_sample_info);
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

   resample->reconfig_sample_fmt = (samplesref->sample_fmt != resample->in_sample_fmt);
   resample->reconfig_channel_layout = (samplesref->channel_layout != resample->in_channel_layout);

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
