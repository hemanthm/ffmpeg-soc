/*
 * audio utilities
 * Copyright (c) 2010 S.N. Hemanth Meenakshisundaram <smeenaks@ucsd.edu>
 * based on audioconvert.c in libavcodec by Michael Niedermayer
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
 * audio sample format and channel layout utilities
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include "avstring.h"
#include "libm.h"
#include "avutil.h"
#include "audiodesc.h"

/** this table gives more information about formats */
const SampleFmtInfo sample_fmt_info[SAMPLE_FMT_NB] = {
    [SAMPLE_FMT_U8]  = { .name = "u8",  .bits = 8 },
    [SAMPLE_FMT_S16] = { .name = "s16", .bits = 16 },
    [SAMPLE_FMT_S32] = { .name = "s32", .bits = 32 },
    [SAMPLE_FMT_FLT] = { .name = "flt", .bits = 32 },
    [SAMPLE_FMT_DBL] = { .name = "dbl", .bits = 64 },
};

enum SampleFormat av_get_sample_fmt(const char* name)
{
    int i;

    for (i=0; i < SAMPLE_FMT_NB; i++)
        if (!strcmp(sample_fmt_info[i].name, name))
            return i;
    return SAMPLE_FMT_NONE;
}

void av_sample_fmt_string (char *buf, int buf_size, int sample_fmt)
{
    /* print header */
    if (sample_fmt < 0)
        snprintf (buf, buf_size, "name  " " depth");
    else if (sample_fmt < SAMPLE_FMT_NB) {
        SampleFmtInfo info = sample_fmt_info[sample_fmt];
        snprintf (buf, buf_size, "%-6s" "   %2d ", info.name, info.bits);
    }
}

static const char* const channel_names[]={
    "FL", "FR", "FC", "LFE", "BL",  "BR",  "FLC", "FRC",
    "BC", "SL", "SR", "TC",  "TFL", "TFC", "TFR", "TBL",
    "TBC", "TBR",
    [29] = "DL",
    [30] = "DR",
};

static const struct {
    const char *name;
    int         nb_channels;
    int64_t     layout;
} channel_layout_map[] = {
    { "mono",        1,  CH_LAYOUT_MONO },
    { "stereo",      2,  CH_LAYOUT_STEREO },
    { "2.1",         3,  CH_LAYOUT_2_1 },
    { "surround",    3,  CH_LAYOUT_SURROUND },
    { "4.0",         4,  CH_LAYOUT_4POINT0 },
    { "2.2",         4,  CH_LAYOUT_2_2 },
    { "quad",        4,  CH_LAYOUT_QUAD },
    { "5.0",         5,  CH_LAYOUT_5POINT0 },
    { "5.0",         5,  CH_LAYOUT_5POINT0_BACK },
    { "5.1",         6,  CH_LAYOUT_5POINT1 },
    { "5.1",         6,  CH_LAYOUT_5POINT1_BACK },
    { "7.0",         7,  CH_LAYOUT_7POINT0 },
    { "5.1+downmix", 8,  CH_LAYOUT_5POINT1|CH_LAYOUT_STEREO_DOWNMIX, },
    { "7.1",         8,  CH_LAYOUT_7POINT1 },
    { "7.1(wide)",   8,  CH_LAYOUT_7POINT1_WIDE },
    { "7.1+downmix", 10, CH_LAYOUT_7POINT1|CH_LAYOUT_STEREO_DOWNMIX, },
    { 0 }
};

int64_t av_guess_channel_layout(int nb_channels)
{
    int i;

    for (i=0; channel_layout_map[i].name; i++)
        if (nb_channels == channel_layout_map[i].nb_channels)
            return channel_layout_map[i].nb_channels;

    return 0;
}

void av_get_channel_layout_string(char *buf, int buf_size, int nb_channels, int64_t channel_layout)
{
    int i;

    for (i=0; channel_layout_map[i].name; i++)
        if (nb_channels    == channel_layout_map[i].nb_channels &&
            channel_layout == channel_layout_map[i].layout) {
            av_strlcpy(buf, channel_layout_map[i].name, buf_size);
            return;
        }

    snprintf(buf, buf_size, "%d channels", nb_channels);
    if (channel_layout) {
        int i,ch;
        av_strlcat(buf, " (", buf_size);
        for(i=0,ch=0; i<64; i++) {
            if ((channel_layout & (1L<<i))) {
                const char *name = channel_names[i];
                if (name) {
                    if (ch>0) av_strlcat(buf, "|", buf_size);
                    av_strlcat(buf, name, buf_size);
                }
                ch++;
            }
        }
        av_strlcat(buf, ")", buf_size);
    }
}

int av_get_hamming_weight(int64_t bstring)
{
    int count;
    uint64_t x = bstring;
    for (count = 0; x; count++)
        x &= x-1; // unset lowest set bit
    return count;
}

