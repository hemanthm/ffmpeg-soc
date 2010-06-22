/*
 * frame FIFO
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

#include "avfilter.h"

typedef struct BufPic
{
    AVFilterPicRef *pic;
    struct BufPic  *next;
} BufPic;

typedef struct
{
    BufPic  root;
    BufPic *last;   ///< last buffered picture
} BufferContext;

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    BufferContext *buf = ctx->priv;
    buf->last = &buf->root;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    BufferContext *buf = ctx->priv;
    BufPic *pic, *tmp;

    for(pic = buf->root.next; pic; pic = tmp) {
        tmp = pic->next;
        avfilter_unref_pic(pic->pic);
        av_free(pic);
    }
}

static void start_frame(AVFilterLink *link, AVFilterPicRef *picref)
{
    BufferContext *buf = link->dst->priv;

    buf->last->next = av_mallocz(sizeof(BufPic));
    buf->last = buf->last->next;
    buf->last->pic = picref;
}

static void end_frame(AVFilterLink *link)
{
}

/* TODO: support forwarding slices as they come if the next filter has
 * requested a frame and we had none buffered */
static void draw_slice(AVFilterLink *link, int y, int h, int slice_dir)
{
}

static int request_frame(AVFilterLink *link)
{
    BufferContext *buf = link->src->priv;
    BufPic *tmp;

    if(!buf->root.next)
        if(avfilter_request_frame(link->src->inputs[0]))
            return -1;

    /* by doing this, we give ownership of the reference to the next filter,
     * so we don't have to worry about dereferencing it ourselves. */
    avfilter_start_frame(link, buf->root.next->pic);
    avfilter_draw_slice(link, 0, buf->root.next->pic->h, 1);
    avfilter_end_frame(link);

    if(buf->last == buf->root.next)
        buf->last = &buf->root;
    tmp = buf->root.next->next;
    av_free(buf->root.next);
    buf->root.next = tmp;

    return 0;
}

AVFilter avfilter_vf_fifo =
{
    .name      = "fifo",

    .init      = init,
    .uninit    = uninit,

    .priv_size = sizeof(BufferContext),

    .inputs    = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .get_video_buffer= avfilter_null_get_video_buffer,
                                    .start_frame     = start_frame,
                                    .draw_slice      = draw_slice,
                                    .end_frame       = end_frame,
                                    .rej_perms       = AV_PERM_REUSE2, },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name            = "default",
                                    .type            = AVMEDIA_TYPE_VIDEO,
                                    .request_frame   = request_frame, },
                                  { .name = NULL}},
};

