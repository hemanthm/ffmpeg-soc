/*
 * audio utilities
 * Copyright (c) 2006 Michael Niedermayer <michaelni@gmx.at>
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

typedef struct SampleFmtInfo {
    const char *name;
    int bits;
} SampleFmtInfo;

/** this table gives more information about formats */
static const SampleFmtInfo sample_fmt_info[SAMPLE_FMT_NB] = {
    [SAMPLE_FMT_U8]  = { .name = "u8",  .bits = 8 },
    [SAMPLE_FMT_S16] = { .name = "s16", .bits = 16 },
    [SAMPLE_FMT_S32] = { .name = "s32", .bits = 32 },
    [SAMPLE_FMT_FLT] = { .name = "flt", .bits = 32 },
    [SAMPLE_FMT_DBL] = { .name = "dbl", .bits = 64 },
};

const char *av_get_sample_fmt_name(int sample_fmt)
{
    if (sample_fmt < 0 || sample_fmt >= SAMPLE_FMT_NB)
        return NULL;
    return sample_fmt_info[sample_fmt].name;
}

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
        SampleFmtInfo info= sample_fmt_info[sample_fmt];
        snprintf (buf, buf_size, "%-6s" "   %2d ", info.name, info.bits);
    }
}

int av_get_bits_per_sample_fmt(enum SampleFormat sample_fmt) {
    if (sample_fmt < 0 || sample_fmt >= SAMPLE_FMT_NB)
        return -1;
    return sample_fmt_info[sample_fmt].bits;
}

static const char* const channel_names[]={
    "FL", "FR", "FC", "LFE", "BL",  "BR",  "FLC", "FRC",
    "BC", "SL", "SR", "TC",  "TFL", "TFC", "TFR", "TBL",
    "TBC", "TBR",
    [29] = "DL",
    [30] = "DR",
};

static const char *get_channel_name(int channel_id)
{
    if (channel_id<0 || channel_id>=FF_ARRAY_ELEMS(channel_names))
        return NULL;
    return channel_names[channel_id];
}

int64_t av_guess_channel_layout(int nb_channels, enum CodecID codec_id, const char *fmt_name)
{
    switch(nb_channels) {
    case 1: return CH_LAYOUT_MONO;
    case 2: return CH_LAYOUT_STEREO;
    case 3: return CH_LAYOUT_SURROUND;
    case 4: return CH_LAYOUT_QUAD;
    case 5: return CH_LAYOUT_5POINT0;
    case 6: return CH_LAYOUT_5POINT1;
    case 8: return CH_LAYOUT_7POINT1;
    default: return 0;
    }
}

static const struct {
    const char *name;
    int         nb_channels;
    int64_t     layout;
} channel_layout_map[] = {
    { "mono",        1,  CH_LAYOUT_MONO },
    { "stereo",      2,  CH_LAYOUT_STEREO },
    { "4.0",         4,  CH_LAYOUT_4POINT0 },
    { "quad",        4,  CH_LAYOUT_QUAD },
    { "5.0",         5,  CH_LAYOUT_5POINT0 },
    { "5.0",         5,  CH_LAYOUT_5POINT0_BACK },
    { "5.1",         6,  CH_LAYOUT_5POINT1 },
    { "5.1",         6,  CH_LAYOUT_5POINT1_BACK },
    { "5.1+downmix", 8,  CH_LAYOUT_5POINT1|CH_LAYOUT_STEREO_DOWNMIX, },
    { "7.1",         8,  CH_LAYOUT_7POINT1 },
    { "7.1(wide)",   8,  CH_LAYOUT_7POINT1_WIDE },
    { "7.1+downmix", 10, CH_LAYOUT_7POINT1|CH_LAYOUT_STEREO_DOWNMIX, },
    { 0 }
};

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
                const char *name = get_channel_name(i);
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

int av_channel_layout_num_channels(int64_t channel_layout)
{
    int count;
    uint64_t x = channel_layout;
    for (count = 0; x; count++)
        x &= x-1; // unset lowest set bit
    return count;
}

