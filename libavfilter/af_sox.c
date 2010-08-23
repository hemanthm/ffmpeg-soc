/*
 * copyright (c) 2010 S.N. Hemanth Meenakshisundaram
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
 * Sox Filter
 */

#include "avfilter.h"
#include "parseutils.h"
#include "libavcodec/audioconvert.h"
#include "libavutil/fifo.h"
#include <assert.h>
#include <sox.h>
#include <string.h>

typedef struct {
    char *sox_args;              ///< filter arguments;
    sox_effects_chain_t * chain; ///< handle to sox effects chain.
    AVFifoBuffer *in_fifo;       ///< fifo buffer of input audio frame pointers
    AVFifoBuffer *out_fifo;      ///< fifo buffer of output audio data from sox
    int64_t ch_layout;           ///< channel layout of data handled
    int64_t sample_rate;         ///< sample rate of data handled
    int nb_channels;             ///< number of channels in our channel layout
    int out_size;                ///< desired size of each output audio buffer
} SoxContext;

static int query_formats(AVFilterContext *ctx)
{
    // Sox effects only operate on signed 32-bit integer audio data.
    enum SampleFormat sample_fmts[] = {
        SAMPLE_FMT_S32, SAMPLE_FMT_NONE
    };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(sample_fmts));
    return 0;
}

typedef struct {
    SoxContext *lavfi_ctx;
} sox_inout_ctx;

// configures the sox effect for sample input.
static int inout_config_opts(sox_effect_t *effect, int argc, char **argv)
{
    SoxContext *sox;
    if (argc < 2) {
        lsx_fail("lavfi context not supplied");
        return (SOX_EOF);
    }
    sscanf(argv[1], "%ld", (long int *)&sox);
    ((sox_inout_ctx *)effect->priv)->lavfi_ctx = sox;
    return SOX_SUCCESS;
}

/**
 * a sox effect handler to handle input of samples to the effects chain.
 * The function that will be called to input samples into the effects chain.
 */
static int input_drain(sox_effect_t *effect,
                       sox_sample_t *o_samples, size_t *o_samples_size)
{

    SoxContext *sox = ((sox_inout_ctx *)effect->priv)->lavfi_ctx;
    AVFilterBufferRef *samplesref;
    int input_nb_samples = 0;

    if (av_fifo_size(sox->in_fifo)) {

        // read first audio frame from queued input buffers and give it to sox.
        av_fifo_generic_read(sox->in_fifo, &samplesref, sizeof(samplesref), NULL);

        /**
         * inside lavfi, nb_samples is number of samples in each channel, while in sox
         * number of samples refers to the total number over all channels
         */
        input_nb_samples = samplesref->audio->samples_nb * sox->nb_channels;

        // ensure that *o_samples_size is a multiple of the number of channels.
        *o_samples_size -= *o_samples_size % sox->nb_channels;

        /**
         * FIXME: Right now, if sox chain accepts fewer samples than in one buffer, we drop
         * remaining data. We should be taking the required data and preserving the rest.
         * Luckily, this is highly unlikely.
         */
        if (*o_samples_size < input_nb_samples)
            input_nb_samples = *o_samples_size;

        memcpy(o_samples, samplesref->data[0], input_nb_samples*sizeof(int));
        *o_samples_size = input_nb_samples;

        avfilter_unref_buffer(samplesref);
    }
    return SOX_EOF;
}

/**
 * a sox effect handler to handle output of samples to the effects chain.
 * The function that will be called to output samples from the effects chain.
 */
static int output_flow(sox_effect_t *effect UNUSED,
                       sox_sample_t const *i_samples,
                       sox_sample_t *o_samples UNUSED, size_t *i_samples_size,
                       size_t *o_samples_size)
{
    SoxContext *sox = ((sox_inout_ctx *)effect->priv)->lavfi_ctx;

    // If our fifo runs out of space, we just drop this frame and keep going.
    if (*i_samples_size > 0) {
        if (av_fifo_space(sox->out_fifo) < *i_samples_size * sizeof(int)) {
            av_log(NULL, AV_LOG_ERROR,
                   "Buffering limit reached. Sox output data being dropped.\n");
            return SOX_SUCCESS;
        }

        av_fifo_generic_write(sox->out_fifo, (void *)i_samples, *i_samples_size, NULL);
    }

    // Set *o_samples_size to 0 since this is the last effect of the sox chain.
    *o_samples_size = 0;

    return SOX_SUCCESS; // All samples output successfully.
}

static sox_effect_handler_t const * input_handler(void)
{
    static sox_effect_handler_t handler = {
        "input", NULL, SOX_EFF_MCHAN, inout_config_opts, NULL, NULL,
        input_drain, NULL, NULL, sizeof(SoxContext *)
    };
    return &handler;
}

// a sox effect handler to handle output of samples from the effects chain.
static sox_effect_handler_t const *output_handler(void)
{
    static sox_effect_handler_t handler = {
        "output", NULL, SOX_EFF_MCHAN, inout_config_opts, NULL,
        output_flow, NULL, NULL, NULL, sizeof(SoxContext *)
    };
    return &handler;
}

#define INFIFO_SIZE 8
#define OUTFIFO_SIZE 8192
#define OUT_FRAME_SIZE 2048

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    SoxContext *sox = ctx->priv;

    sox->sox_args = av_strdup(args);
    sox->in_fifo = av_fifo_alloc(INFIFO_SIZE*sizeof(AVFilterBufferRef*));
    // the output data fifo stores samples in sox's native s32 integer format.
    sox->out_size = OUT_FRAME_SIZE; // FIXME: Make this configurable;
    sox->out_fifo = av_fifo_alloc(OUTFIFO_SIZE*sizeof(int));
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SoxContext *sox = ctx->priv;
    sox_delete_effects_chain(sox->chain);
    sox_quit();
    av_fifo_free(sox->in_fifo);
    av_fifo_free(sox->out_fifo);
    av_free(sox->sox_args);
}

#define MAX_EFFECT_ARGS 10

static inline int add_effect_and_setopts(AVFilterContext *ctx, char *effect_str, sox_signalinfo_t *signal)
{
    SoxContext *sox = ctx->priv;
    int nb_args = -1, err = 0;
    char *args[MAX_EFFECT_ARGS];
    sox_effect_t *effect = NULL;

    effect_str = strtok(effect_str, " ");
    effect = sox_create_effect(sox_find_effect(effect_str));
    if (!effect) {
        av_log(ctx, AV_LOG_ERROR, "No such sox effect: '%s'.\n", effect_str);
        return AVERROR(EINVAL);
    }

    while (args[++nb_args] = strtok(NULL, " ")) {
        if  (nb_args >= MAX_EFFECT_ARGS-1) {
            av_log(ctx, AV_LOG_ERROR, "Too many arguments for sox effect'%s'.\n", effect_str);
            return AVERROR(EINVAL);
        }
    }
    if ((err = sox_add_effect(sox->chain, effect, signal, signal)) != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Sox error: '%s'.\n", sox_strerror(err));
        return AVERROR(EINVAL);
    }
    if (nb_args >= 0)
        err = sox_effect_options(effect, nb_args, args);
    else
        err = sox_effect_options(effect, nb_args, NULL);
    if (err != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Sox error: '%s'.\n", sox_strerror(err));
        return AVERROR(EINVAL);
    }

    return 0;
}

static int config_input(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    SoxContext *sox = ctx->priv;
    sox_effects_chain_t * chain;
    sox_effect_t *e = NULL;
    sox_encodinginfo_t *enc = av_malloc (sizeof(sox_encodinginfo_t));
    sox_signalinfo_t *in_signal_info = av_malloc (sizeof(sox_signalinfo_t));

    char *token = NULL, param[32], *ioargs[] = {param};
    char *cpargs = av_strdup(sox->sox_args);
    int err = 0;

    memset(enc, 0, sizeof(sox_encodinginfo_t));
    memset(in_signal_info, 0, sizeof(sox_signalinfo_t));

    enc->encoding = SOX_DEFAULT_ENCODING;
    enc->bits_per_sample = 32;

    if (link->format != SAMPLE_FMT_S32) {
        av_log(link->dst, AV_LOG_ERROR,
               "Sox needs signed 32-bit input samples, insert resample filter.");
        return AVERROR(EINVAL);
    }
    // store channel layout and number of channels, insert resample filter to keep this constant.
    sox->ch_layout   = link->channel_layout;
    sox->sample_rate = link->sample_rate;
    sox->nb_channels = in_signal_info->channels =
                       avcodec_channel_layout_num_channels(link->channel_layout);
    in_signal_info->rate = (double) sox->sample_rate;
    in_signal_info->precision = 32;

    if ((err = sox_init()) != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Sox error: '%s'.\n", sox_strerror(err));
        return AVERROR(EINVAL);
    }

    chain = sox_create_effects_chain(enc, enc);
    sox->chain = chain;

    snprintf(param, sizeof(param), "%ld", (long int)sox);
    // set up the audio buffer source as first effect of the chain.
    e = sox_create_effect(input_handler());
    if ((err = sox_add_effect(sox->chain, e, in_signal_info, in_signal_info)) != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Sox error: '%s'.\n", sox_strerror(err));
        return AVERROR(EINVAL);
    }
    if ((err = sox_effect_options(e, 1, ioargs)) != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Sox error: '%s'.\n", sox_strerror(err));
        return AVERROR(EINVAL);
    }

    token = strtok (cpargs, ":");
    while (token) {
        if ((err = add_effect_and_setopts(ctx, token, in_signal_info))) {
            av_log(ctx, AV_LOG_ERROR, "Invalid sox argument: '%s'.\n", token);
            return err;
        }
        token = strtok (NULL, ":");
    }

    e = sox_create_effect(output_handler());
    if ((err = sox_add_effect(sox->chain, e, in_signal_info, in_signal_info)) != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Sox error: '%s'.\n", sox_strerror(err));
        return AVERROR(EINVAL);
    }
    if ((err = sox_effect_options(e, 1, ioargs)) != SOX_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Sox error: '%s'.\n", sox_strerror(err));
        return AVERROR(EINVAL);
    }
    av_free(cpargs);
    return 0;
}

static void filter_samples(AVFilterLink *link, AVFilterBufferRef *samplesref)
{
    SoxContext *sox = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[0];
    AVFilterBufferRef *outsamples;
    int outsize = 0;

    if (av_fifo_space(sox->in_fifo) < sizeof(samplesref)) {
        av_log(NULL, AV_LOG_ERROR,
               "Buffering limit reached. Please allow sox to consume some available frames before adding new ones.\n");
        return;
    }

    av_fifo_generic_write(sox->in_fifo, &samplesref, sizeof(samplesref), NULL);

    // start sox flow if this is the first audio frame we have got.
    sox_flow_effects(sox->chain, NULL, NULL);

    // libsox uses packed audio data internally.
    outsamples = avfilter_get_audio_buffer(link, AV_PERM_WRITE, SAMPLE_FMT_S32,
                                           sox->out_size, sox->ch_layout, 0);

    // send silence if no processed audio available from sox.
    outsize = av_fifo_size(sox->out_fifo);
    outsize = FFMIN(outsize, sox->out_size);
    if (outsize)
        av_fifo_generic_read(sox->out_fifo, outsamples->data[0],
                             sox->out_size, NULL);
    else
        memset(outsamples->data[0], 0, sox->out_size);

    avfilter_filter_samples(outlink, outsamples);
}

AVFilter avfilter_af_sox = {
    .name        = "sox",
    .description = NULL_IF_CONFIG_SMALL("SOund eXchange audio effects library wrapper."),
    .priv_size   = sizeof(SoxContext),
    .init        = init,
    .uninit      = uninit,

    .query_formats = query_formats,
    .inputs    = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO,
                                    .filter_samples   = filter_samples,
                                    .config_props     = config_input,
                                    .min_perms        = AV_PERM_READ },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_AUDIO, },
                                  { .name = NULL}},
};
