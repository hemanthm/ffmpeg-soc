/*
 * Misc image conversion routines
 * most functionality is exported to the public API, see avcodec.h
 *
 * Copyright (c) 2008 Vitor Sessak
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

#ifndef AVCODEC_IMGCONVERT_H
#define AVCODEC_IMGCONVERT_H

#include <stdint.h>
#include "avcodec.h"

/* calculate image plane linesizes based on colorspace and width */
int ff_fill_image_linesize(int linesize[4], enum PixelFormat pix_fmt, int width);

/* wrapper for ff_fill_image_linesize that takes an AVPicture pointer */
int ff_fill_linesize(AVPicture *picture, enum PixelFormat pix_fmt, int width);

/* calculate offsets of image planes into a frame buffer */
int ff_fill_image_data_ptr(uint8_t *data[4], uint8_t *ptr, enum PixelFormat pix_fmt,
                           int height, const int linesize[4]);

/* wrapper for ff_fill_image_data_ptr that takes an AVPicture pointer */
int ff_fill_pointer(AVPicture *picture, uint8_t *ptr, enum PixelFormat pix_fmt, int height);

int ff_get_plane_bytewidth(enum PixelFormat pix_fmt, int width, int plane);

int ff_set_systematic_pal(uint32_t pal[256], enum PixelFormat pix_fmt);

#endif /* AVCODEC_IMGCONVERT_H */
