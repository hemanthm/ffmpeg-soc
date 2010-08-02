/*
 * copyright (c) 2010 S.N. Hemanth Meenakshisundaram
 * Original vhook author: Gustavo Sverzut Barbieri <gsbarbieri@yahoo.com.br>
 * Libavfilter version  : S.N. Hemanth Meenakshisundaram <smeenaks@ucsd.edu>
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
 * Drawtext Filter
 */

#include "avfilter.h"
#include "parseutils.h"
#include "libavutil/colorspace.h"
#include "libavutil/pixdesc.h"

#undef time
#include <sys/time.h>
#include <time.h>

#include <ft2build.h>
#include <freetype/config/ftheader.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

typedef struct {
    const AVClass *class;
    unsigned char *fontfile;        ///< font to be used
    unsigned char *text;            ///< text to be drawn
    char *textfile;                 ///< file with text to be drawn
    unsigned int x;                 ///< x position to start drawing text
    unsigned int y;                 ///< y position to start drawing text
    unsigned int fontsize;          ///< font size to use
    char *fgcolor_string;           ///< foreground color as string
    char *bgcolor_string;           ///< background color as string
    unsigned char fgcolor[4];       ///< foreground color in YUV
    unsigned char bgcolor[4];       ///< background/Box color in YUV
    short int draw_box;             ///< draw box around text - true or false
    short int outline;              ///< draw outline in bg color around text
    int text_height;                ///< height of a font symbol
    int baseline;                   ///< baseline to draw fonts from
    int use_kerning;                ///< font kerning is used - true/false
    FT_Library library;             ///< freetype font library handle
    FT_Face face;                   ///< freetype font face handle
    FT_Glyph glyphs[256];           ///< array holding glyphs of font
    FT_Bitmap bitmaps[256];         ///< array holding bitmaps of font
    int advance[256];
    int bitmap_left[256];
    int bitmap_top[256];
    unsigned int glyphs_index[256];
    int hsub, vsub;                 ///< chroma subsampling values
} DrawTextContext;

#define OFFSET(x) offsetof(DrawTextContext, x)

static const AVOption drawtext_options[]= {
{"fontfile", "set font file",        OFFSET(fontfile),       FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"text",     "set text",             OFFSET(text),           FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"textfile", "set text file",        OFFSET(textfile),       FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"fgcolor",  "set foreground color", OFFSET(fgcolor_string), FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"bgcolor",  "set background color", OFFSET(bgcolor_string), FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"box",      "set box",              OFFSET(draw_box),       FF_OPT_TYPE_INT,    0,         0,        1 },
{"outline",  "set outline",          OFFSET(outline),        FF_OPT_TYPE_INT,    0,         0,        1 },
{"fontsize", "set font size",        OFFSET(fontsize),       FF_OPT_TYPE_INT,   16,         1,       72 },
{"x",        "set x",                OFFSET(x),              FF_OPT_TYPE_INT,    0,         0,  INT_MAX },
{"y",        "set y",                OFFSET(y),              FF_OPT_TYPE_INT,    0,         0,  INT_MAX },
{NULL},
};

static const char *drawtext_get_name(void *ctx)
{
    return "drawtext";
}

static const AVClass drawtext_class = {
    "DrawTextContext",
    drawtext_get_name,
    drawtext_options
};

static int query_formats(AVFilterContext *ctx)
{
    /* FIXME: Add support for other formats */
    enum PixelFormat pix_fmts[] = {
        PIX_FMT_YUV420P, PIX_FMT_YUV444P, PIX_FMT_YUV422P,
        PIX_FMT_YUV411P, PIX_FMT_YUV410P,
        PIX_FMT_YUV440P, PIX_FMT_NONE
    };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(pix_fmts));
    return 0;
}

#undef __FTERRORS_H__
#define FT_ERROR_START_LIST {
#define FT_ERRORDEF(e, v, s) { (e), (s) },
#define FT_ERROR_END_LIST { 0, NULL } };

struct ft_error
{
    int err;
    const char *err_msg;
} ft_errors[] =
#include FT_ERRORS_H

#define FT_ERRMSG(e) ft_errors[e].err_msg

#define MAX_TEXT_SIZE 1024

static inline int extract_color(AVFilterContext *ctx, char *color_str, unsigned char *color)
{
    uint8_t rgba[4];
    uint8_t err;
    if ((err = av_parse_color(rgba, color_str, ctx))) {
        return err;
    }
    color[0] = RGB_TO_Y(rgba[0], rgba[1], rgba[2]);
    color[1] = RGB_TO_U(rgba[0], rgba[1], rgba[2], 0);
    color[2] = RGB_TO_V(rgba[0], rgba[1], rgba[2], 0);
    color[3] = rgba[3];

    return 0;
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    unsigned short int c;
    int err;
    int y_max, y_min;
    FT_BBox bbox;
    DrawTextContext *dtext = ctx->priv;

    dtext->class = &drawtext_class;
    av_opt_set_defaults2(dtext, 0, 0);
    dtext->fgcolor_string = av_strdup("black");
    dtext->bgcolor_string = av_strdup("white");

    if ((err = (av_set_options_string(dtext, args, "=", ":"))) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error parsing options string: '%s'\n", args);
        return err;
    }

    if (!dtext->fontfile) {
        av_log(ctx, AV_LOG_ERROR, "No font file provided! (=fontfile:<filename>)\n");
        return AVERROR(EINVAL);
    }

    if (dtext->textfile) {
        FILE *fp;
        if (dtext->text) {
            av_log(ctx, AV_LOG_ERROR, "Both text and file provided. Please provide only one.\n");
            return AVERROR(EINVAL);
        }
        if (!(fp = fopen(dtext->textfile, "r"))) {
            av_log(ctx, AV_LOG_ERROR, "The textfile %s could not be opened.\n", dtext->textfile);
            return AVERROR(EINVAL);
        } else {
            uint16_t read_bytes;
            char *tbuff = av_malloc(MAX_TEXT_SIZE);
            if (!tbuff) {
                av_log(ctx, AV_LOG_ERROR, "Could not allocate read buffer.\n");
                return AVERROR(ENOMEM);
            }
            read_bytes = fread(tbuff, sizeof(char), MAX_TEXT_SIZE-1, fp);
            if (read_bytes > 0) {
                tbuff[read_bytes] = 0;
                av_free(dtext->text);
                dtext->text = tbuff;
            } else {
                av_log(ctx, AV_LOG_ERROR, "The textfile %s could not be read or is empty.\n", dtext->textfile);
                av_free(tbuff);
                return AVERROR(EINVAL);
            }
            fclose(fp);
        }
    }

    if (!dtext->text) {
        av_log(ctx, AV_LOG_ERROR, "Either text or a valid file must be provided (=text:<text> or =textfile:<filename>)\n");
        return AVERROR(EINVAL);
    }

    if ((err = extract_color(ctx, dtext->fgcolor_string, dtext->fgcolor))) {
        av_log(ctx, AV_LOG_ERROR, "Invalid foreground color: '%s'.\n", dtext->fgcolor_string);
        return err;
    }

    if ((err = extract_color(ctx, dtext->bgcolor_string, dtext->bgcolor))) {
        av_log(ctx, AV_LOG_ERROR, "Invalid background color: '%s'.\n", dtext->fgcolor_string);
        return err;
    }

    if ((err = FT_Init_FreeType(&(dtext->library)))) {
        av_log(ctx, AV_LOG_ERROR, "Could not load FreeType: %s\n", FT_ERRMSG(err));
        return AVERROR(EINVAL);
    }

    if ((err = FT_New_Face(dtext->library, dtext->fontfile, 0, &(dtext->face)))) {
        av_log(ctx, AV_LOG_ERROR, "Could not load fontface %s: %s\n", dtext->fontfile, FT_ERRMSG(err));
        return AVERROR(EINVAL);
    }
    if ((err = FT_Set_Pixel_Sizes(dtext->face, 0, dtext->fontsize))) {
        av_log(ctx, AV_LOG_ERROR, "Could not set font size to %d pixels: %s\n", dtext->fontsize, FT_ERRMSG(err));
        return AVERROR(EINVAL);
    }

    dtext->use_kerning = FT_HAS_KERNING(dtext->face);

    /* load and cache glyphs */
    y_max = -32000;
    y_min =  32000;
    /* FIXME: Supports only ASCII text now. Add Unicode support */
    for (c=0; c <= 255; c++) {
        /* Load char */
        err = FT_Load_Char(dtext->face, (unsigned char)c, FT_LOAD_RENDER | FT_LOAD_MONOCHROME);
        if (err)
            continue;  /* ignore errors */

        dtext->bitmaps    [c] = dtext->face->glyph->bitmap;
        dtext->bitmap_left[c] = dtext->face->glyph->bitmap_left;
        dtext->bitmap_top [c] = dtext->face->glyph->bitmap_top;
        dtext->advance    [c] = dtext->face->glyph->advance.x >> 6;

        err = FT_Get_Glyph(dtext->face->glyph, &(dtext->glyphs[c]));
        if (err)
            continue;  /* ignore errors */

        dtext->glyphs_index[c] = FT_Get_Char_Index(dtext->face, (unsigned char)c);

        /* Measure text height to calculate text_height (or the maximum text height) */
        FT_Glyph_Get_CBox(dtext->glyphs[c], ft_glyph_bbox_pixels, &bbox);
        if (bbox.yMax > y_max)
          y_max = bbox.yMax;
        if (bbox.yMin < y_min)
          y_min = bbox.yMin;
    }

    dtext->text_height = y_max - y_min;
    dtext->baseline = y_max;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DrawTextContext *dtext = ctx->priv;
    av_free(dtext->fontfile);
    av_free(dtext->text);
    av_free(dtext->textfile);
    av_free(dtext->fgcolor_string);
    av_free(dtext->bgcolor_string);
    FT_Done_Face(dtext->face);
    FT_Done_FreeType(dtext->library);
}

static int config_input(AVFilterLink *link)
{
    DrawTextContext *dtext = link->dst->priv;
    const AVPixFmtDescriptor *pix_desc = &av_pix_fmt_descriptors[link->format];
    dtext->hsub = pix_desc->log2_chroma_w;
    dtext->vsub = pix_desc->log2_chroma_h;
    return 0;
}

#define SET_PIXEL(pic_ref, yuv_color, x, y, hsub, vsub) { \
    luma_pos    = ((x)          ) + ((y)          ) * pic_ref->linesize[0]; \
    chroma_pos1 = ((x) >> (hsub)) + ((y) >> (vsub)) * pic_ref->linesize[1]; \
    chroma_pos2 = ((x) >> (hsub)) + ((y) >> (vsub)) * pic_ref->linesize[2]; \
    pic_ref->data[0][luma_pos   ] = (yuv_color[3] * yuv_color[0] + (255 - yuv_color[3]) * pic_ref->data[0][luma_pos   ]) >> 8; \
    pic_ref->data[1][chroma_pos1] = (yuv_color[3] * yuv_color[1] + (255 - yuv_color[3]) * pic_ref->data[1][chroma_pos1]) >> 8; \
    pic_ref->data[2][chroma_pos2] = (yuv_color[3] * yuv_color[2] + (255 - yuv_color[3]) * pic_ref->data[2][chroma_pos2]) >> 8; \
}

#define GET_PIXEL(pic_ref, yuv_color, x, y, hsub, vsub) { \
    yuv_color[0] = pic_ref->data[0][( x           ) + ( y           ) * pic_ref->linesize[0]]; \
    yuv_color[1] = pic_ref->data[1][((x) >> (hsub)) + ((y) >> (vsub)) * pic_ref->linesize[1]]; \
    yuv_color[2] = pic_ref->data[2][((x) >> (hsub)) + ((y) >> (vsub)) * pic_ref->linesize[2]]; \
}

static inline void draw_glyph(AVFilterPicRef *pic_ref, FT_Bitmap *bitmap, unsigned int x,
                              unsigned int y, unsigned int width, unsigned int height,
                              unsigned char yuv_fgcolor[4], unsigned char yuv_bgcolor[4],
                              short int outline, int hsub, int vsub)
{
    int r, c;
    unsigned int luma_pos, chroma_pos1, chroma_pos2;
    uint8_t spixel, dpixel[4], in_glyph=0;

    if (bitmap->pixel_mode == ft_pixel_mode_mono) {
        in_glyph = 0;
        for (r=0; (r < bitmap->rows) && (r+y < height); r++) {
            for (c=0; (c < bitmap->width) && (c+x < width); c++) {
                /* pixel in the pic_ref (destination) */
                GET_PIXEL(pic_ref, dpixel, c+x, y+r, hsub, vsub);

                /* pixel in the glyph bitmap (source) */
                spixel = bitmap->buffer[r*bitmap->pitch + c/8] & (0x80>>(c%8));

                if (spixel)
                    memcpy(dpixel, yuv_fgcolor, 4);

                if (outline) {
                    /* border detection: */
                    if (!in_glyph && spixel) {
                        /* left border detected */
                        in_glyph = 1;
                        /* draw left pixel border */
                        if (c-1 >= 0)
                            SET_PIXEL(pic_ref, yuv_bgcolor, c+x-1, y+r, hsub, vsub);
                    } else if (in_glyph && !spixel) {
                    /* right border detected */
                        in_glyph = 0;
                        /* 'draw' right pixel border */
                        memcpy(dpixel, yuv_bgcolor, 4);
                    }

                    if (in_glyph) {
                    /* see if we have a top/bottom border */
                        /* top */
                        if ((r-1 >= 0) && (!(bitmap->buffer[(r-1) * bitmap->pitch + c/8] & (0x80>>(c%8)))))
                            /* we have a top border */
                            SET_PIXEL(pic_ref, yuv_bgcolor, c+x, y+r-1, hsub, vsub);

                        /* bottom border detection */
                        if ((r+1 < height) && (!(bitmap->buffer[(r+1) * bitmap->pitch + c/8] & (0x80>>(c%8)))))
                            /* draw bottom border */
                            SET_PIXEL(pic_ref, yuv_bgcolor, c+x, y+r+1, hsub, vsub);
                    }
                }
                SET_PIXEL(pic_ref, dpixel, c+x, y+r, hsub, vsub);
            }
        }
    }
}

static inline void drawbox(AVFilterPicRef *pic_ref, unsigned int x, unsigned int y,
                           unsigned int width, unsigned int height,
                           unsigned char yuv_color[4], int hsub, int vsub)
{
    int i, plane;
    uint8_t *p;

    if (yuv_color[3] != 0xFF) {
        unsigned int j, luma_pos, chroma_pos1, chroma_pos2;

        for (j = 0; j < height; j++)
            for (i = 0; i < width; i++)
                SET_PIXEL(pic_ref, yuv_color, (i+x), (y+j), hsub, vsub);

    } else {
        for (plane = 0; plane < 3 && pic_ref->data[plane]; plane++) {
            int hsub1 = plane == 1 || plane == 2 ? hsub : 0;
            int vsub1 = plane == 1 || plane == 2 ? vsub : 0;

            p = pic_ref->data[plane] + (y >> vsub1) * pic_ref->linesize[plane] + (x >> hsub1);
            for (i = 0; i < (height >> vsub1); i++) {
                memset(p, yuv_color[plane], (width >> hsub1));
                p += pic_ref->linesize[plane];
            }
        }
    }
}

static void draw_text(AVFilterContext *ctx, AVFilterPicRef *pic_ref, int width, int height)
{
    DrawTextContext *dtext = ctx->priv;
    FT_Face face = dtext->face;
    FT_GlyphSlot  slot = face->glyph;
    unsigned char *text = dtext->text;
    unsigned char c;
    unsigned char buff[MAX_TEXT_SIZE];
    int x = 0, y = 0, i = 0, size = 0;
    time_t now = time(0);
    int str_w, str_w_max;
    FT_Vector pos[MAX_TEXT_SIZE];
    FT_Vector delta;
    struct tm ltime;

#if HAVE_LOCALTIME_R
    strftime(buff, sizeof(buff), text, localtime_r(&now, &ltime));
    text = buff;
#else
    av_log(ctx, AV_LOG_WARNING, "strftime() expansion unavailable!");
#endif
    size = strlen(text);

    /* measure text size and save glyphs position*/
    str_w = str_w_max = 0;
    x = dtext->x;
    y = dtext->y;
    for (i=0; i < size; i++) {
        c = text[i];
        /* kerning */
        if (dtext->use_kerning && (i > 0) && dtext->glyphs_index[c]) {
            FT_Get_Kerning(dtext->face, dtext->glyphs_index[text[i-1]],
                           dtext->glyphs_index[c], ft_kerning_default, &delta);
            x += delta.x >> 6;
        }

        if (((x + dtext->advance[c]) >= width) || (c == '\n')) {
            if (c != '\n')
                str_w_max = width - dtext->x - 1;
            y += dtext->text_height;
            x = dtext->x;
        }

        /* save position */
        pos[i].x = x + dtext->bitmap_left[c];
        pos[i].y = y - dtext->bitmap_top [c] + dtext->baseline;
        x     += dtext->advance[c];
        str_w += dtext->advance[c];
    }
    y += dtext->text_height;
    if (str_w_max == 0)
        str_w_max = str_w;
    if (dtext->draw_box) {
        /* Check if it doesn't pass the limits */
        if (str_w_max + dtext->x >= width)
            str_w_max = width - dtext->x - 1;
        if (y >= height)
            y = height - 1;

        /* Draw Background */
        drawbox(pic_ref, dtext->x, dtext->y, str_w_max, y-dtext->y,
                dtext->bgcolor, dtext->hsub, dtext->vsub);
    }

    /* Draw Glyphs */
    for (i=0; i < size; i++) {
        c = text[i];

        /* skip new line char, just go to new line */
        if (c == '\n')
            continue;

        /* now, draw to our target surface */
        draw_glyph(pic_ref, &(dtext->bitmaps[c]), pos[i].x, pos[i].y, width, height,
                   dtext->fgcolor, dtext->bgcolor, dtext->outline, dtext->hsub, dtext->vsub);

        /* increment pen position */
        x += slot->advance.x >> 6;
    }
}

static void end_frame(AVFilterLink *link)
{
    AVFilterLink *output = link->dst->outputs[0];
    AVFilterPicRef *pic_ref = link->cur_pic;

    draw_text(link->dst, pic_ref, pic_ref->w, pic_ref->h);

    avfilter_draw_slice(output, 0, pic_ref->h, 1);
    avfilter_end_frame(output);
}

AVFilter avfilter_vf_drawtext = {
    .name      = "drawtext",
    .description = "Draw text on top of video frames using libfreetype library.",
    .priv_size = sizeof(DrawTextContext),
    .init      = init,
    .uninit      = uninit,

    .query_formats   = query_formats,
    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .get_video_buffer= avfilter_null_get_video_buffer,
                                    .start_frame     = avfilter_null_start_frame,
                                    .end_frame       = end_frame,
                                    .config_props    = config_input,
                                    .min_perms       = AV_PERM_WRITE |
                                                       AV_PERM_READ,
                                    .rej_perms       = AV_PERM_PRESERVE },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO, },
                                  { .name = NULL}},
};
