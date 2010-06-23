/*
 * audio utilities
 * Copyright (c) 2006 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2008 Peter Ross
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

#ifndef AVUTIL_AUDIODESC_H
#define AVUTIL_AUDIODESC_H

/**
 * @file
 * audio format and channel layout utilities
 */


#include "avutil.h"


/**
 * Generate string corresponding to the sample format with
 * number sample_fmt, or a header if sample_fmt is negative.
 *
 * @param[in] buf the buffer where to write the string
 * @param[in] buf_size the size of buf
 * @param[in] sample_fmt the number of the sample format to print the corresponding info string, or
 * a negative value to print the corresponding header.
 * Meaningful values for obtaining a sample format info vary from 0 to SAMPLE_FMT_NB -1.
 */
void av_sample_fmt_string(char *buf, int buf_size, int sample_fmt);

/**
 * @return NULL on error
 */
const char *av_get_sample_fmt_name(int sample_fmt);

/**
 * @return SAMPLE_FMT_NONE on error
 */
enum SampleFormat av_get_sample_fmt(const char* name);

/**
 * @return -1 on error
 */
int av_get_bits_per_sample_fmt(enum SampleFormat sample_fmt);

/**
 * @return NULL on error
 */
const char *av_get_channel_name(int channel_id);

/**
 * @return description of channel layout
 */
void av_get_channel_layout_string(char *buf, int buf_size, int nb_channels, int64_t channel_layout);

/**
 * Guess the channel layout
 * @param nb_channels
 * @param codec_id Codec identifier, or CODEC_ID_NONE if unknown
 * @param fmt_name Format name, or NULL if unknown
 * @return Channel layout mask
 */
int64_t av_guess_channel_layout(int nb_channels, const char *fmt_name);

/**
 * @return the number of channels in the channel layout.
 */
int av_channel_layout_num_channels(int64_t channel_layout);

#endif /* AVUTIL_AUDIODESC_H */
