/*****************************************************************************
 * display.c: Android video output module
 *****************************************************************************
 * Copyright (C) 2014 VLC authors and VideoLAN
 * Modified for XRVLC by XRVLC contributors on 2026-08-16.
 *
 * Authors: Thomas Guillem <thomas@gllm.fr>
 *          Felix Abecassis <felix.abecassis@gmail.com>
 *          Ming Hu <tewilove@gmail.com>
 *          Ludovic Fauvet <etix@l0cal.com>
 *          Sébastien Toque <xilasz@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_picture_pool.h>
#include <vlc_filter.h>
#include <vlc_codec.h>
#include <vlc_image.h>

#include <vlc_opengl.h> /* for ClearSurface */
#include <GLES2/gl2.h>  /* for ClearSurface */

#include <dlfcn.h>
#include <limits.h>
#include <math.h>

#include "display.h"
#include "utils.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define USE_ANWP
#define CHROMA_TEXT "Chroma used"
#define CHROMA_LONGTEXT \
    "Force use of a specific chroma for output. Default is RGB32."

#define CFG_PREFIX "android-display-"
static int  Open (vlc_object_t *);
static int  OpenOpaque (vlc_object_t *);
static void Close(vlc_object_t *);
static void SubpicturePrepare(vout_display_t *vd, subpicture_t *subpicture);

static inline long long XrAndroidDiagMs(vlc_tick_t i_date)
{
    return i_date > VLC_TICK_INVALID ? (long long)(i_date / 1000) : -1;
}

static inline long long XrAndroidDiagDeltaMs(vlc_tick_t i_date,
                                             vlc_tick_t i_now)
{
    return i_date > VLC_TICK_INVALID ? (long long)((i_date - i_now) / 1000) : -1;
}

static int XrAndroidDiagOpaqueIndex(picture_t *p_pic, bool *pb_locked)
{
    if (!p_pic || !p_pic->p_sys)
    {
        if (pb_locked)
            *pb_locked = false;
        return -2;
    }

    picture_sys_t *p_picsys = p_pic->p_sys;
    vlc_mutex_lock(&p_picsys->hw.lock);
    int i_index = p_picsys->hw.i_index;
    if (pb_locked)
        *pb_locked = p_picsys->b_locked;
    vlc_mutex_unlock(&p_picsys->hw.lock);
    return i_index;
}

vlc_module_begin()
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_description("Android video output")
    set_capability("vout display", 260)
    add_shortcut("android-display")
    add_string(CFG_PREFIX "chroma", NULL, CHROMA_TEXT, CHROMA_LONGTEXT, true)
    set_callbacks(Open, Close)
    add_submodule ()
        set_description("Android opaque video output")
        set_capability("vout display", 280)
        add_shortcut("android-opaque")
        set_callbacks(OpenOpaque, Close)
vlc_module_end()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

#define THREAD_NAME "android-display"

static const vlc_fourcc_t subpicture_chromas[] =
{
    VLC_CODEC_RGBA,
    0
};

static picture_pool_t   *Pool  (vout_display_t *, unsigned);
static void             Prepare(vout_display_t *, picture_t *, subpicture_t *);
static void             Display(vout_display_t *, picture_t *, subpicture_t *);
static int              Control(vout_display_t *, int, va_list);

typedef struct android_window android_window;
struct android_window
{
    video_format_t fmt;
    int i_android_hal;
    unsigned int i_angle;
    unsigned int i_pic_count;
    unsigned int i_min_undequeued;
    bool b_use_priv;
    bool b_opaque;

    enum AWindow_ID id;
    ANativeWindow *p_surface;
    jobject       *p_jsurface;
    native_window_priv *p_surface_priv;
};

typedef struct buffer_bounds buffer_bounds;
struct buffer_bounds
{
    uint8_t *p_pixels;
    ARect bounds;
};

typedef struct
{
    subpicture_region_t *region;
    double order;
    unsigned index;
} xr_subtitle_region_ref_t;

#define XR_SUBTITLE_STACK_VAR "xr-subtitle-stack-outside"
#define XR_SUBTITLE_SURFACE_ENABLED_VAR "xr-subtitle-surface-enabled"
#define XR_SUBTITLE_MAX_EDGE_PIXELS 1920

struct vout_display_sys_t
{
    vout_window_t *embed;
    picture_pool_t *pool;

    int i_display_width;
    int i_display_height;

    AWindowHandler *p_awh;
    native_window_api_t *anw;
    native_window_priv_api_t anwp;
    bool b_has_anwp;

    android_window *p_window;
    android_window *p_sub_window;

    bool b_displayed;
    bool b_sub_invalid;
    filter_t *p_spu_blend;
    picture_t *p_sub_pic;
    buffer_bounds *p_sub_buffer_bounds;
    int64_t i_sub_last_order;
    ARect sub_last_region;

    bool b_has_subpictures;
    bool b_subpicture_updated;
    unsigned int i_xr_sub_prepare_entry_logs;
    unsigned int i_xr_sub_prepare_logs;
    unsigned int i_xr_sub_subprepare_entry_logs;
    unsigned int i_xr_sub_lock_logs;
    unsigned int i_xr_sub_display_entry_logs;
    unsigned int i_xr_sub_display_logs;

    uint8_t hash[16];
};

#define PRIV_WINDOW_FORMAT_YV12 0x32315659

static const char *
AndroidWindowIdName(enum AWindow_ID id)
{
    switch (id)
    {
        case AWindow_Video:
            return "AWindow_Video";
        case AWindow_Subtitles:
            return "AWindow_Subtitles";
        case AWindow_SurfaceTexture:
            return "AWindow_SurfaceTexture";
        default:
            return "AWindow_Unknown";
    }
}

static unsigned XrSubtitleRegionWidth(const subpicture_region_t *region)
{
    if (region->fmt.i_visible_width > 0)
        return region->fmt.i_visible_width;
    return region->fmt.i_width;
}

static unsigned XrSubtitleRegionHeight(const subpicture_region_t *region)
{
    if (region->fmt.i_visible_height > 0)
        return region->fmt.i_visible_height;
    return region->fmt.i_height;
}

static void XrSubtitleRegionTopLeft(const subpicture_region_t *region,
                                    unsigned target_width,
                                    unsigned target_height,
                                    int *x,
                                    int *y)
{
    const int width = (int) XrSubtitleRegionWidth(region);
    const int height = (int) XrSubtitleRegionHeight(region);

    if (region->i_align & SUBPICTURE_ALIGN_RIGHT)
        *x = (int) target_width - region->i_x - width;
    else if (region->i_align & SUBPICTURE_ALIGN_LEFT)
        *x = region->i_x;
    else
        *x = ((int) target_width - width) / 2 + region->i_x;

    if (region->i_align & SUBPICTURE_ALIGN_BOTTOM)
        *y = (int) target_height - region->i_y - height;
    else if (region->i_align & SUBPICTURE_ALIGN_TOP)
        *y = region->i_y;
    else
        *y = ((int) target_height - height) / 2 + region->i_y;
}

static double XrSubtitleRegionOrder(const subpicture_region_t *region,
                                    unsigned target_width,
                                    unsigned target_height)
{
    int x, y;
    XrSubtitleRegionTopLeft(region, target_width, target_height, &x, &y);

    const double center_x = x + XrSubtitleRegionWidth(region) * 0.5;
    const double center_y = y + XrSubtitleRegionHeight(region) * 0.5;
    const double dx = center_x - target_width * 0.5;
    const double dy = center_y - target_height * 0.5;
    double angle = atan2(-dx, dy);
    if (angle < 0.0)
        angle += 6.28318530717958647692;
    return angle;
}

static int XrSubtitleRegionCompare(const void *left, const void *right)
{
    const xr_subtitle_region_ref_t *a = left;
    const xr_subtitle_region_ref_t *b = right;
    if (a->order < b->order)
        return -1;
    if (a->order > b->order)
        return 1;
    if (a->index < b->index)
        return -1;
    if (a->index > b->index)
        return 1;
    return 0;
}

static subpicture_t *XrSubtitleStackSubpicture(const subpicture_t *source,
                                               unsigned target_width,
                                               unsigned target_height)
{
    if (!source || !source->p_region || target_width == 0 || target_height == 0)
        return NULL;

    unsigned count = 0;
    for (const subpicture_region_t *region = source->p_region; region;
         region = region->p_next)
        count++;
    if (count == 0)
        return NULL;

    xr_subtitle_region_ref_t *regions = calloc(count, sizeof(*regions));
    if (!regions)
        return NULL;

    unsigned index = 0;
    for (const subpicture_region_t *region = source->p_region; region;
         region = region->p_next)
    {
        if (!region->p_picture)
        {
            for (unsigned i = 0; i < index; i++)
                subpicture_region_Delete(regions[i].region);
            free(regions);
            return NULL;
        }
        regions[index].region = subpicture_region_Copy((subpicture_region_t *) region);
        if (!regions[index].region)
        {
            for (unsigned i = 0; i < index; i++)
                subpicture_region_Delete(regions[i].region);
            free(regions);
            return NULL;
        }
        regions[index].order = XrSubtitleRegionOrder(region, target_width,
                                                     target_height);
        regions[index].index = index;
        index++;
    }

    qsort(regions, count, sizeof(*regions), XrSubtitleRegionCompare);

    subpicture_t *stacked = subpicture_New(NULL);
    if (!stacked)
    {
        for (unsigned i = 0; i < count; i++)
            subpicture_region_Delete(regions[i].region);
        free(regions);
        return NULL;
    }

    stacked->i_channel = source->i_channel;
    stacked->i_order = source->i_order;
    stacked->i_start = source->i_start;
    stacked->i_stop = source->i_stop;
    stacked->b_ephemer = source->b_ephemer;
    stacked->b_fade = source->b_fade;
    stacked->b_subtitle = source->b_subtitle;
    stacked->b_absolute = true;
    stacked->i_original_picture_width = target_width;
    stacked->i_original_picture_height = target_height;
    stacked->i_alpha = source->i_alpha;

    int y = 0;
    subpicture_region_t **next = &stacked->p_region;
    for (unsigned i = 0; i < count; i++)
    {
        subpicture_region_t *region = regions[i].region;
        const int width = (int) XrSubtitleRegionWidth(region);
        const int height = (int) XrSubtitleRegionHeight(region);
        region->i_align = 0;
        region->i_x = ((int) target_width - width) / 2;
        region->i_y = y;
        region->p_next = NULL;
        *next = region;
        next = &region->p_next;
        y += height;
    }

    free(regions);
    return stacked;
}

static unsigned XrSubtitleScaleDimension(unsigned value,
                                         unsigned target_size,
                                         unsigned source_size)
{
    if (value == 0 || target_size == 0 || source_size == 0)
        return value;

    uint64_t scaled = ((uint64_t) value * target_size + source_size / 2)
                    / source_size;
    return scaled > 0 ? (unsigned) scaled : 1;
}

static int XrSubtitleScaleCoordinate(int value,
                                     unsigned target_size,
                                     unsigned source_size)
{
    if (target_size == 0 || source_size == 0)
        return value;

    int64_t scaled = (int64_t) value * target_size;
    if (scaled >= 0)
        scaled += source_size / 2;
    else
        scaled -= source_size / 2;
    return (int) (scaled / source_size);
}

static void XrSubtitleScaleRegionFormat(video_format_t *fmt,
                                         unsigned target_width,
                                         unsigned target_height,
                                         unsigned source_width,
                                         unsigned source_height)
{
    fmt->i_width = XrSubtitleScaleDimension(fmt->i_width, target_width,
                                            source_width);
    fmt->i_height = XrSubtitleScaleDimension(fmt->i_height, target_height,
                                             source_height);
    fmt->i_x_offset = XrSubtitleScaleDimension(fmt->i_x_offset, target_width,
                                               source_width);
    fmt->i_y_offset = XrSubtitleScaleDimension(fmt->i_y_offset, target_height,
                                               source_height);
    fmt->i_visible_width = XrSubtitleScaleDimension(fmt->i_visible_width,
                                                    target_width,
                                                    source_width);
    fmt->i_visible_height = XrSubtitleScaleDimension(fmt->i_visible_height,
                                                     target_height,
                                                     source_height);

    if (fmt->i_x_offset >= fmt->i_width)
        fmt->i_x_offset = 0;
    if (fmt->i_y_offset >= fmt->i_height)
        fmt->i_y_offset = 0;
    fmt->i_visible_width = __MIN(fmt->i_visible_width,
                                 fmt->i_width - fmt->i_x_offset);
    fmt->i_visible_height = __MIN(fmt->i_visible_height,
                                  fmt->i_height - fmt->i_y_offset);
}

static subpicture_t *XrSubtitleScaleSubpicture(vout_display_t *vd,
                                               const subpicture_t *source,
                                               unsigned target_width,
                                               unsigned target_height)
{
    if (!source || !source->p_region || target_width == 0 || target_height == 0
     || source->i_original_picture_width <= 0
     || source->i_original_picture_height <= 0)
        return NULL;

    const unsigned source_width = source->i_original_picture_width;
    const unsigned source_height = source->i_original_picture_height;
    if (source_width == target_width && source_height == target_height)
        return NULL;

    image_handler_t *image = image_HandlerCreate(vd);
    if (!image)
        return NULL;

    subpicture_t *scaled = subpicture_New(NULL);
    if (!scaled)
    {
        image_HandlerDelete(image);
        return NULL;
    }

    scaled->i_channel = source->i_channel;
    scaled->i_order = source->i_order;
    scaled->i_start = source->i_start;
    scaled->i_stop = source->i_stop;
    scaled->b_ephemer = source->b_ephemer;
    scaled->b_fade = source->b_fade;
    scaled->b_subtitle = source->b_subtitle;
    scaled->b_absolute = true;
    scaled->i_original_picture_width = target_width;
    scaled->i_original_picture_height = target_height;
    scaled->i_alpha = source->i_alpha;

    subpicture_region_t **next = &scaled->p_region;
    for (const subpicture_region_t *region = source->p_region; region;
         region = region->p_next)
    {
        if (!region->p_picture)
            goto error;

        video_format_t fmt_in = region->fmt;
        video_format_t fmt_out = fmt_in;
        XrSubtitleScaleRegionFormat(&fmt_out, target_width, target_height,
                                    source_width, source_height);

        picture_t *scaled_picture = image_Convert(image, region->p_picture,
                                                   &fmt_in, &fmt_out);
        if (!scaled_picture)
            goto error;

        subpicture_region_t *scaled_region = subpicture_region_New(&fmt_out);
        if (!scaled_region)
        {
            picture_Release(scaled_picture);
            goto error;
        }

        if (scaled_region->p_picture)
            picture_Release(scaled_region->p_picture);
        scaled_region->p_picture = scaled_picture;
        scaled_region->i_x = XrSubtitleScaleCoordinate(region->i_x,
                                                       target_width,
                                                       source_width);
        scaled_region->i_y = XrSubtitleScaleCoordinate(region->i_y,
                                                       target_height,
                                                       source_height);
        scaled_region->i_align = region->i_align;
        scaled_region->i_alpha = region->i_alpha;
        scaled_region->i_text_align = region->i_text_align;
        scaled_region->b_noregionbg = region->b_noregionbg;
        scaled_region->b_gridmode = region->b_gridmode;
        scaled_region->b_balanced_text = region->b_balanced_text;
        scaled_region->i_max_width = XrSubtitleScaleCoordinate(
            region->i_max_width, target_width, source_width);
        scaled_region->i_max_height = XrSubtitleScaleCoordinate(
            region->i_max_height, target_height, source_height);
        scaled_region->p_next = NULL;
        *next = scaled_region;
        next = &scaled_region->p_next;
    }

    image_HandlerDelete(image);
    return scaled;

error:
    image_HandlerDelete(image);
    subpicture_Delete(scaled);
    return NULL;
}

static inline int ChromaToAndroidHal(vlc_fourcc_t i_chroma)
{
    switch (i_chroma) {
        case VLC_CODEC_YV12:
        case VLC_CODEC_I420:
            return PRIV_WINDOW_FORMAT_YV12;
        case VLC_CODEC_RGB16:
            return WINDOW_FORMAT_RGB_565;
        case VLC_CODEC_RGB32:
            return WINDOW_FORMAT_RGBX_8888;
        case VLC_CODEC_RGBA:
            return WINDOW_FORMAT_RGBA_8888;
        default:
            return -1;
    }
}

static int UpdateVideoSize(vout_display_sys_t *sys, video_format_t *p_fmt,
                           bool b_cropped)
{
    unsigned int i_width, i_height;
    unsigned int i_sar_num = 1, i_sar_den = 1;
    video_format_t rot_fmt;

    video_format_ApplyRotation(&rot_fmt, p_fmt);

    if (rot_fmt.i_sar_num != 0 && rot_fmt.i_sar_den != 0) {
        i_sar_num = rot_fmt.i_sar_num;
        i_sar_den = rot_fmt.i_sar_den;
    }
    if (b_cropped) {
        i_width = rot_fmt.i_visible_width;
        i_height = rot_fmt.i_visible_height;
    } else {
        i_width = rot_fmt.i_width;
        i_height = rot_fmt.i_height;
    }

    AWindowHandler_setVideoLayout(sys->p_awh, i_width, i_height,
                                  rot_fmt.i_visible_width,
                                  rot_fmt.i_visible_height,
                                  i_sar_num, i_sar_den);
    return 0;
}

static void ClampSubtitleSurfaceSize(int *width, int *height)
{
    const int max_edge = __MAX(*width, *height);
    if (max_edge <= XR_SUBTITLE_MAX_EDGE_PIXELS)
        return;

    if (*width >= *height)
    {
        *height = __MAX(1, (int) (((int64_t) *height
                    * XR_SUBTITLE_MAX_EDGE_PIXELS + *width / 2) / *width));
        *width = XR_SUBTITLE_MAX_EDGE_PIXELS;
    }
    else
    {
        *width = __MAX(1, (int) (((int64_t) *width
                    * XR_SUBTITLE_MAX_EDGE_PIXELS + *height / 2) / *height));
        *height = XR_SUBTITLE_MAX_EDGE_PIXELS;
    }
}

static picture_t *PictureAlloc(vout_display_sys_t *sys, video_format_t *fmt,
                               bool b_opaque)
{
    picture_t *p_pic;
    picture_resource_t rsc;
    picture_sys_t *p_picsys = calloc(1, sizeof(*p_picsys));

    if (unlikely(p_picsys == NULL))
        return NULL;


    memset(&rsc, 0, sizeof(picture_resource_t));
    rsc.p_sys = p_picsys;

    if (b_opaque)
    {
        p_picsys->hw.b_vd_ref = true;
        p_picsys->hw.p_surface = sys->p_window->p_surface;
        p_picsys->hw.p_jsurface =  sys->p_window->p_jsurface;
        p_picsys->hw.i_index = -1;
        vlc_mutex_init(&p_picsys->hw.lock);
        rsc.pf_destroy = AndroidOpaquePicture_DetachVout;
    }
    else
        p_picsys->sw.p_vd_sys = sys;

    p_pic = picture_NewFromResource(fmt, &rsc);
    if (!p_pic)
    {
        free(p_picsys);
        return NULL;
    }
    return p_pic;
}

static void FixSubtitleFormat(vout_display_sys_t *sys)
{
    video_format_t *p_subfmt;
    video_format_t fmt;
    int i_width, i_height;
    int i_video_width, i_video_height;
    int i_display_width, i_display_height;
    double aspect;

    if (!sys->p_sub_window)
        return;
    p_subfmt = &sys->p_sub_window->fmt;

    video_format_ApplyRotation(&fmt, &sys->p_window->fmt);

    if (fmt.i_visible_width == 0 || fmt.i_visible_height == 0) {
        i_video_width = fmt.i_width;
        i_video_height = fmt.i_height;
    } else {
        i_video_width = fmt.i_visible_width;
        i_video_height = fmt.i_visible_height;
    }

    if (fmt.i_sar_num > 0 && fmt.i_sar_den > 0) {
        if (fmt.i_sar_num >= fmt.i_sar_den)
            i_video_width = i_video_width * fmt.i_sar_num / fmt.i_sar_den;
        else
            i_video_height = i_video_height * fmt.i_sar_den / fmt.i_sar_num;
    }

    if (sys->p_window->i_angle == 90 || sys->p_window->i_angle == 180) {
        i_display_width = sys->i_display_height;
        i_display_height = sys->i_display_width;
        aspect = i_video_height / (double) i_video_width;
    } else {
        i_display_width = sys->i_display_width;
        i_display_height = sys->i_display_height;
        aspect = i_video_width / (double) i_video_height;
    }

    if (i_display_width / aspect < i_display_height) {
        i_width = i_display_width;
        i_height = i_display_width / aspect;
    } else {
        i_width = i_display_height * aspect;
        i_height = i_display_height;
    }

    // Use the biggest size available
    if (i_width * i_height < i_video_width * i_video_height) {
        i_width = i_video_width;
        i_height = i_video_height;
    }

    ClampSubtitleSurfaceSize(&i_width, &i_height);

    p_subfmt->i_width =
    p_subfmt->i_visible_width = i_width;
    p_subfmt->i_height =
    p_subfmt->i_visible_height = i_height;
    p_subfmt->i_x_offset = 0;
    p_subfmt->i_y_offset = 0;
    p_subfmt->i_sar_num = 1;
    p_subfmt->i_sar_den = 1;
    sys->b_sub_invalid = true;
}

#define ALIGN_16_PIXELS( x ) ( ( ( x ) + 15 ) / 16 * 16 )
static void SetupPictureYV12(picture_t *p_picture, uint32_t i_in_stride)
{
    /* according to document of android.graphics.ImageFormat.YV12 */
    int i_stride = ALIGN_16_PIXELS(i_in_stride);
    int i_c_stride = ALIGN_16_PIXELS(i_stride / 2);

    p_picture->p->i_pitch = i_stride;

    /* Fill chroma planes for planar YUV */
    for (int n = 1; n < p_picture->i_planes; n++)
    {
        const plane_t *o = &p_picture->p[n-1];
        plane_t *p = &p_picture->p[n];

        p->p_pixels = o->p_pixels + o->i_lines * o->i_pitch;
        p->i_pitch  = i_c_stride;
        p->i_lines  = p_picture->format.i_height / 2;
        /*
          Explicitly set the padding lines of the picture to black (127 for YUV)
          since they might be used by Android during rescaling.
        */
        int visible_lines = p_picture->format.i_visible_height / 2;
        if (visible_lines < p->i_lines)
            memset(&p->p_pixels[visible_lines * p->i_pitch], 127, (p->i_lines - visible_lines) * p->i_pitch);
    }

    if (vlc_fourcc_AreUVPlanesSwapped(p_picture->format.i_chroma,
                                      VLC_CODEC_YV12)) {
        uint8_t *p_tmp = p_picture->p[1].p_pixels;
        p_picture->p[1].p_pixels = p_picture->p[2].p_pixels;
        p_picture->p[2].p_pixels = p_tmp;
    }
}

static void AndroidWindow_DisconnectSurface(vout_display_sys_t *sys,
                                            android_window *p_window)
{
    if (p_window->p_surface_priv) {
        sys->anwp.disconnect(p_window->p_surface_priv);
        p_window->p_surface_priv = NULL;
    }
    if (p_window->p_surface) {
        AWindowHandler_releaseANativeWindow(sys->p_awh, p_window->id);
        p_window->p_surface = NULL;
    }
}

static int AndroidWindow_ConnectSurface(vout_display_sys_t *sys,
                                        android_window *p_window)
{
    if (!p_window->p_surface) {
        p_window->p_surface = AWindowHandler_getANativeWindow(sys->p_awh,
                                                              p_window->id);
        if (!p_window->p_surface)
        {
            if (p_window->id == AWindow_Subtitles)
                msg_Err(sys->embed, "XR_SUB_WINDOW AndroidWindow_ConnectSurface id=%s failed: surface=null window=%p",
                        AndroidWindowIdName(p_window->id), (void *) p_window);
            return -1;
        }
        if (p_window->id == AWindow_Subtitles)
            msg_Err(sys->embed, "XR_SUB_WINDOW AndroidWindow_ConnectSurface id=%s window=%p surface=%p opaque=%d use_priv=%d fmt=%ux%u visible=%ux%u chroma=0x%08x",
                    AndroidWindowIdName(p_window->id), (void *) p_window,
                    (void *) p_window->p_surface, p_window->b_opaque,
                    p_window->b_use_priv, p_window->fmt.i_width,
                    p_window->fmt.i_height, p_window->fmt.i_visible_width,
                    p_window->fmt.i_visible_height, p_window->fmt.i_chroma);
        if (p_window->b_opaque)
            p_window->p_jsurface = AWindowHandler_getSurface(sys->p_awh,
                                                             p_window->id);
    }

    return 0;
}

static android_window *AndroidWindow_New(vout_display_t *vd,
                                         video_format_t *p_fmt,
                                         enum AWindow_ID id,
                                         bool b_use_priv)
{
    vout_display_sys_t *sys = vd->sys;
    android_window *p_window = NULL;

    p_window = calloc(1, sizeof(android_window));
    if (!p_window)
        goto error;

    p_window->id = id;
    p_window->b_opaque = p_fmt->i_chroma == VLC_CODEC_ANDROID_OPAQUE;
    if (!p_window->b_opaque) {
        p_window->b_use_priv = sys->b_has_anwp && b_use_priv;

        p_window->i_android_hal = ChromaToAndroidHal(p_fmt->i_chroma);
        if (p_window->i_android_hal == -1)
            goto error;
    }

    switch (p_fmt->orientation)
    {
        case ORIENT_ROTATED_90:
            p_window->i_angle = 90;
            break;
        case ORIENT_ROTATED_180:
            p_window->i_angle = 180;
            break;
        case ORIENT_ROTATED_270:
            p_window->i_angle = 270;
            break;
        default:
            p_window->i_angle = 0;
    }
    if (p_window->b_use_priv)
        p_window->fmt = *p_fmt;
    else
        video_format_ApplyRotation(&p_window->fmt, p_fmt);
    p_window->i_pic_count = 1;

    if (AndroidWindow_ConnectSurface(sys, p_window) != 0)
    {
        if (id == AWindow_Video)
            msg_Err(vd, "can't get Video Surface");
        else if (id == AWindow_Subtitles)
            msg_Err(vd, "can't get Subtitles Surface");
        goto error;
    }

    if (id == AWindow_Subtitles)
        msg_Err(vd, "XR_SUB_WINDOW AndroidWindow_New id=%s window=%p surface=%p opaque=%d use_priv=%d hal=%d angle=%u fmt=%ux%u visible=%ux%u chroma=0x%08x",
                AndroidWindowIdName(id), (void *) p_window,
                (void *) p_window->p_surface, p_window->b_opaque,
                p_window->b_use_priv, p_window->i_android_hal,
                p_window->i_angle, p_window->fmt.i_width,
                p_window->fmt.i_height, p_window->fmt.i_visible_width,
                p_window->fmt.i_visible_height, p_window->fmt.i_chroma);

    return p_window;
error:
    free(p_window);
    return NULL;
}

static void AndroidWindow_Destroy(vout_display_t *vd,
                                  android_window *p_window)
{
    AndroidWindow_DisconnectSurface(vd->sys, p_window);
    free(p_window);
}

static int AndroidWindow_UpdateCrop(vout_display_sys_t *sys,
                                    android_window *p_window)
{
    if (!p_window->p_surface_priv)
        return -1;

    return sys->anwp.setCrop(p_window->p_surface_priv,
                             p_window->fmt.i_x_offset,
                             p_window->fmt.i_y_offset,
                             p_window->fmt.i_visible_width,
                             p_window->fmt.i_visible_height);
}

static int AndroidWindow_SetupANWP(vout_display_sys_t *sys,
                                   android_window *p_window)
{
    unsigned int i_max_buffer_count = 0;

    if (!p_window->p_surface_priv)
        p_window->p_surface_priv = sys->anwp.connect(p_window->p_surface);

    if (!p_window->p_surface_priv)
        goto error;

    if (sys->anwp.setUsage(p_window->p_surface_priv, false, 0) != 0)
        goto error;

    if (sys->anwp.setBuffersGeometry(p_window->p_surface_priv,
                                        p_window->fmt.i_width,
                                        p_window->fmt.i_height,
                                        p_window->i_android_hal) != 0)
        goto error;

    sys->anwp.getMinUndequeued(p_window->p_surface_priv,
                               &p_window->i_min_undequeued);

    sys->anwp.getMaxBufferCount(p_window->p_surface_priv, &i_max_buffer_count);

    if ((p_window->i_min_undequeued + p_window->i_pic_count) >
         i_max_buffer_count)
        p_window->i_pic_count = i_max_buffer_count - p_window->i_min_undequeued;

    if (sys->anwp.setBufferCount(p_window->p_surface_priv,
                                 p_window->i_pic_count +
                                 p_window->i_min_undequeued) != 0)
        goto error;

    if (sys->anwp.setOrientation(p_window->p_surface_priv,
                                 p_window->i_angle) != 0)
        goto error;

    AndroidWindow_UpdateCrop(sys, p_window);

    return 0;
error:
    if (p_window->p_surface_priv) {
        sys->anwp.disconnect(p_window->p_surface_priv);
        p_window->p_surface_priv = NULL;
    }
    p_window->b_use_priv = false;
    if (p_window->i_angle != 0)
        video_format_TransformTo(&p_window->fmt, ORIENT_NORMAL);
    return -1;
}

static int AndroidWindow_SetupANW(vout_display_sys_t *sys,
                                  android_window *p_window)
{
    p_window->i_pic_count = 1;
    p_window->i_min_undequeued = 0;

    if (sys->anw->setBuffersGeometry)
        return sys->anw->setBuffersGeometry(p_window->p_surface,
                                            p_window->fmt.i_width,
                                            p_window->fmt.i_height,
                                            p_window->i_android_hal);
    else
        return 0;
}

static int AndroidWindow_Setup(vout_display_sys_t *sys,
                               android_window *p_window,
                               unsigned int i_pic_count)
{
    if (i_pic_count != 0)
        p_window->i_pic_count = i_pic_count;

    if (!p_window->b_opaque) {
        int align_pixels;
        picture_t *p_pic = PictureAlloc(sys, &p_window->fmt, false);

        // For RGB (32 or 16) we need to align on 8 or 4 pixels, 16 pixels for YUV
        align_pixels = (16 / p_pic->p[0].i_pixel_pitch) - 1;
        p_window->fmt.i_height = p_pic->format.i_height;
        p_window->fmt.i_width = (p_pic->format.i_width + align_pixels) & ~align_pixels;
        picture_Release(p_pic);

        if (!p_window->b_use_priv
            || AndroidWindow_SetupANWP(sys, p_window) != 0) {
            if (AndroidWindow_SetupANW(sys, p_window) != 0)
                return -1;
        }
    } else {
        sys->p_window->i_pic_count = 31; // TODO
        sys->p_window->i_min_undequeued = 0;
    }

    return 0;
}

static void AndroidWindow_UnlockPicture(vout_display_sys_t *sys,
                                        android_window *p_window,
                                        picture_t *p_pic,
                                        bool b_render)
{
    picture_sys_t *p_picsys = p_pic->p_sys;

    if (!p_picsys->b_locked)
        return;

    if (p_window->b_use_priv) {
        void *p_handle = p_picsys->sw.p_handle;

        if (p_handle != NULL)
            sys->anwp.unlockData(p_window->p_surface_priv, p_handle, b_render);
    } else
        sys->anw->unlockAndPost(p_window->p_surface);

    p_picsys->b_locked = false;
}

static int AndroidWindow_LockPicture(vout_display_sys_t *sys,
                                     android_window *p_window,
                                     picture_t *p_pic)
{
    picture_sys_t *p_picsys = p_pic->p_sys;

    if (p_picsys->b_locked)
        return -1;

    if (p_window->b_use_priv) {
        void *p_handle;
        int err;

        err = sys->anwp.lockData(p_window->p_surface_priv,
                                 &p_handle, &p_picsys->sw.buf);
        if (err != 0)
            return -1;
        p_picsys->sw.p_handle = p_handle;
    } else {
        if (sys->anw->winLock(p_window->p_surface,
                              &p_picsys->sw.buf, NULL) != 0)
            return -1;
    }
    if (p_picsys->sw.buf.width < 0 ||
        p_picsys->sw.buf.height < 0 ||
        (unsigned)p_picsys->sw.buf.width < p_window->fmt.i_width ||
        (unsigned)p_picsys->sw.buf.height < p_window->fmt.i_height)
    {
        p_picsys->b_locked = true;
        AndroidWindow_UnlockPicture(sys, p_window, p_pic, false);
        return -1;
    }

    p_pic->p[0].p_pixels = p_picsys->sw.buf.bits;
    p_pic->p[0].i_lines = p_picsys->sw.buf.height;
    p_pic->p[0].i_pitch = p_pic->p[0].i_pixel_pitch * p_picsys->sw.buf.stride;

    if (p_picsys->sw.buf.format == PRIV_WINDOW_FORMAT_YV12)
        SetupPictureYV12(p_pic, p_picsys->sw.buf.stride);

    p_picsys->b_locked = true;
    return 0;
}

static void SetRGBMask(video_format_t *p_fmt)
{
    switch(p_fmt->i_chroma) {
        case VLC_CODEC_RGB16:
            p_fmt->i_bmask = 0x0000001f;
            p_fmt->i_gmask = 0x000007e0;
            p_fmt->i_rmask = 0x0000f800;
            break;

        case VLC_CODEC_RGB32:
        case VLC_CODEC_RGBA:
            p_fmt->i_rmask = 0x000000ff;
            p_fmt->i_gmask = 0x0000ff00;
            p_fmt->i_bmask = 0x00ff0000;
            break;
    }
}

static bool XrSubtitleSurfaceEnabled(vout_display_t *vd)
{
    return var_InheritBool(vd, XR_SUBTITLE_SURFACE_ENABLED_VAR);
}

static void XrSubtitleReleaseSubWindow(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (sys->p_sub_pic)
    {
        picture_Release(sys->p_sub_pic);
        sys->p_sub_pic = NULL;
    }
    if (sys->p_spu_blend)
    {
        filter_DeleteBlend(sys->p_spu_blend);
        sys->p_spu_blend = NULL;
    }
    free(sys->p_sub_buffer_bounds);
    sys->p_sub_buffer_bounds = NULL;
    if (sys->p_sub_window)
    {
        msg_Err(vd, "XR_SUB_RENDER releasing subtitle window p_sub_window=%p surface=%p",
                (void *) sys->p_sub_window,
                (void *) sys->p_sub_window->p_surface);
        AndroidWindow_Destroy(vd, sys->p_sub_window);
        sys->p_sub_window = NULL;
    }
    sys->b_has_subpictures = false;
    sys->b_subpicture_updated = false;
    sys->b_sub_invalid = true;
    sys->i_sub_last_order = -1;
}

static bool XrSubtitleEnsureSubWindow(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    video_format_t sub_fmt;

    if (!XrSubtitleSurfaceEnabled(vd))
        return false;
    if (sys->p_sub_window)
    {
        ANativeWindow *surface = AWindowHandler_getANativeWindow(sys->p_awh,
                                                                 AWindow_Subtitles);
        if (!surface)
        {
            sys->p_sub_window->p_surface = NULL;
            XrSubtitleReleaseSubWindow(vd);
            return false;
        }
        if (surface != sys->p_sub_window->p_surface)
        {
            if (sys->p_sub_pic)
            {
                picture_Release(sys->p_sub_pic);
                sys->p_sub_pic = NULL;
            }
            if (sys->p_spu_blend)
            {
                filter_DeleteBlend(sys->p_spu_blend);
                sys->p_spu_blend = NULL;
            }
            free(sys->p_sub_buffer_bounds);
            sys->p_sub_buffer_bounds = NULL;
            sys->p_sub_window->p_surface = surface;
            sys->b_sub_invalid = true;
            msg_Err(vd, "XR_SUB_WINDOW updated p_sub_window=%p subtitle_surface=%p",
                    (void *) sys->p_sub_window,
                    (void *) sys->p_sub_window->p_surface);
        }
        return true;
    }

    video_format_ApplyRotation(&sub_fmt, &vd->fmt);
    sub_fmt.i_chroma = subpicture_chromas[0];
    SetRGBMask(&sub_fmt);
    video_format_FixRgb(&sub_fmt);

    sys->p_sub_window = AndroidWindow_New(vd, &sub_fmt, AWindow_Subtitles, false);
    if (!sys->p_sub_window)
        return false;

    FixSubtitleFormat(sys);
    sys->i_sub_last_order = -1;
    sys->b_sub_invalid = true;
    vd->info.subpicture_chromas = subpicture_chromas;
    msg_Err(vd, "XR_SUB_WINDOW ensured p_sub_window=%p subtitle_surface=%p sub_chromas=%p chroma0=0x%08x fmt=%ux%u visible=%ux%u",
            (void *) sys->p_sub_window,
            (void *) sys->p_sub_window->p_surface,
            (void *) vd->info.subpicture_chromas,
            vd->info.subpicture_chromas ? vd->info.subpicture_chromas[0] : 0,
            sys->p_sub_window->fmt.i_width,
            sys->p_sub_window->fmt.i_height,
            sys->p_sub_window->fmt.i_visible_width,
            sys->p_sub_window->fmt.i_visible_height);
    return true;
}

static int OpenCommon(vout_display_t *vd)
{
    vout_display_sys_t *sys;

    /* Fallback to normal projection in case of soft decoding/display (the
     * openGL vout, with a higher priority, should be used when the projection
     * need to be handled). */
    if (vd->fmt.i_chroma == VLC_CODEC_ANDROID_OPAQUE
     && vd->fmt.projection_mode != PROJECTION_MODE_RECTANGULAR)
        return VLC_EGENERIC;
    vd->fmt.projection_mode = PROJECTION_MODE_RECTANGULAR;

    vout_window_t *embed =
        vout_display_NewWindow(vd, VOUT_WINDOW_TYPE_ANDROID_NATIVE);

    if (embed == NULL)
        return VLC_EGENERIC;
    assert(embed->handle.anativewindow);
    AWindowHandler *p_awh = embed->handle.anativewindow;

    if (!AWindowHandler_canSetVideoLayout(p_awh))
    {
        /* It's better to use gles2 if we are not able to change the video
         * layout */
        vout_display_DeleteWindow(vd, embed);
        return VLC_EGENERIC;
    }

    /* Allocate structure */
    vd->sys = sys = (struct vout_display_sys_t*)calloc(1, sizeof(*sys));
    if (!sys)
    {
        vout_display_DeleteWindow(vd, embed);
        return VLC_ENOMEM;
    }

    sys->embed = embed;
    sys->p_awh = p_awh;
    sys->anw = AWindowHandler_getANativeWindowAPI(sys->p_awh);

#ifdef USE_ANWP
    sys->b_has_anwp = android_loadNativeWindowPrivApi(&sys->anwp) == 0;
    if (!sys->b_has_anwp)
        msg_Warn(vd, "Could not initialize NativeWindow Priv API.");
#endif

    sys->i_display_width = vd->cfg->display.width;
    sys->i_display_height = vd->cfg->display.height;

    if (vd->fmt.i_chroma != VLC_CODEC_ANDROID_OPAQUE) {
        /* Setup chroma */
        char *psz_fcc = var_InheritString(vd, CFG_PREFIX "chroma");
        if (psz_fcc) {
            vd->fmt.i_chroma = vlc_fourcc_GetCodecFromString(VIDEO_ES, psz_fcc);
            free(psz_fcc);
        } else
            vd->fmt.i_chroma = VLC_CODEC_RGB32;

        switch(vd->fmt.i_chroma) {
            case VLC_CODEC_YV12:
                /* avoid swscale usage by asking for I420 instead since the
                 * vout already has code to swap the buffers */
                vd->fmt.i_chroma = VLC_CODEC_I420;
            case VLC_CODEC_I420:
                break;
            case VLC_CODEC_RGB16:
            case VLC_CODEC_RGB32:
            case VLC_CODEC_RGBA:
                SetRGBMask(&vd->fmt);
                video_format_FixRgb(&vd->fmt);
                break;
            default:
                goto error;
        }
    }

    sys->p_window = AndroidWindow_New(vd, &vd->fmt, AWindow_Video, true);
    if (!sys->p_window)
        goto error;

    if (AndroidWindow_Setup(sys, sys->p_window, 0) != 0)
        goto error;

    /* use software rotation if we don't use private anw */
    if (!sys->p_window->b_opaque && !sys->p_window->b_use_priv)
        video_format_TransformTo(&vd->fmt, ORIENT_NORMAL);

    msg_Dbg(vd, "using %s", sys->p_window->b_opaque ? "opaque" :
            (sys->p_window->b_use_priv ? "ANWP" : "ANW"));

    /* Export direct subpicture capability; xr-subtitle-surface-enabled decides
     * at render time whether the core should use it. */
    vd->info.subpicture_chromas = subpicture_chromas;

    if (!XrSubtitleEnsureSubWindow(vd) &&
        XrSubtitleSurfaceEnabled(vd) &&
        !vd->obj.force && sys->p_window->b_opaque)
    {
        msg_Warn(vd, "cannot blend subtitles with an opaque surface, "
                     "trying next vout");
        goto error;
    }

    /* Setup vout_display */
    vd->pool    = Pool;
    vd->prepare = Prepare;
    vd->display = Display;
    vd->control = Control;
    vd->info.is_slow = !sys->p_window->b_opaque;

    return VLC_SUCCESS;

error:
    Close(VLC_OBJECT(vd));
    return VLC_EGENERIC;
}

static int Open(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t*)p_this;

    if (vd->fmt.i_chroma == VLC_CODEC_ANDROID_OPAQUE)
        return VLC_EGENERIC;

    /* At this point, gles2 vout failed (old Android device) */
    vd->fmt.projection_mode = PROJECTION_MODE_RECTANGULAR;
    return OpenCommon(vd);
}

static int OpenOpaque(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t*)p_this;

    if (vd->fmt.i_chroma != VLC_CODEC_ANDROID_OPAQUE
     || vd->fmt.projection_mode != PROJECTION_MODE_RECTANGULAR
     || vd->fmt.orientation != ORIENT_NORMAL)
    {
        /* Let the gles2 vout handle orientation and projection */
        return VLC_EGENERIC;
    }

    return OpenCommon(vd);
}

static void ClearSurface(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (sys->p_window->b_opaque)
    {
        /* Clear the surface to black with OpenGL ES 2 */
        vlc_gl_t *gl = vlc_gl_Create(sys->embed, VLC_OPENGL_ES2, "$gles2");
        if (gl == NULL)
            return;

        if (vlc_gl_MakeCurrent(gl))
            goto end;

        vlc_gl_Resize(gl, 1, 1);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        vlc_gl_Swap(gl);

        vlc_gl_ReleaseCurrent(gl);

end:
        vlc_gl_Release(gl);
    }
    else
    {
        android_window *p_window = sys->p_window;
        ANativeWindow_Buffer buf;

        if (p_window->p_surface_priv) {
            sys->anwp.disconnect(p_window->p_surface_priv);
            p_window->p_surface_priv = NULL;
        }

        if (sys->anw->setBuffersGeometry(p_window->p_surface, 1, 1,
                                         WINDOW_FORMAT_RGB_565) == 0
          && sys->anw->winLock(p_window->p_surface, &buf, NULL) == 0)
        {
            uint16_t *p_bit = buf.bits;
            p_bit[0] = 0x0000;
            sys->anw->unlockAndPost(p_window->p_surface);
        }
    }
}

static void Close(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;
    vout_display_sys_t *sys = vd->sys;

    /* Check if SPU regions have been properly cleared, and clear them if they
     * were not. */
    if (sys->b_has_subpictures)
    {
        SubpicturePrepare(vd, NULL);
        AndroidWindow_UnlockPicture(sys, sys->p_sub_window, sys->p_sub_pic, true);
    }

    if (sys->pool)
        picture_pool_Release(sys->pool);

    if (sys->p_window)
    {
        if (sys->b_displayed)
            ClearSurface(vd);
        if (!sys->p_window->b_opaque && !sys->p_window->b_use_priv)
            sys->anw->setBuffersGeometry(sys->p_window->p_surface, 0, 0, 0);
        AndroidWindow_Destroy(vd, sys->p_window);
    }

    if (sys->p_sub_pic)
        picture_Release(sys->p_sub_pic);
    if (sys->p_spu_blend)
        filter_DeleteBlend(sys->p_spu_blend);
    free(sys->p_sub_buffer_bounds);
    if (sys->p_sub_window)
        AndroidWindow_Destroy(vd, sys->p_sub_window);

    if (sys->embed)
    {
        AWindowHandler_setVideoLayout(sys->p_awh, 0, 0, 0, 0, 0, 0);
        vout_display_DeleteWindow(vd, sys->embed);
    }

    free(sys);
}

static int PoolLockPicture(picture_t *p_pic)
{
    picture_sys_t *p_picsys = p_pic->p_sys;
    vout_display_sys_t *sys = p_picsys->sw.p_vd_sys;

    if (AndroidWindow_LockPicture(sys, sys->p_window, p_pic) != 0)
        return -1;

    return 0;
}

static void PoolUnlockPicture(picture_t *p_pic)
{
    picture_sys_t *p_picsys = p_pic->p_sys;
    vout_display_sys_t *sys = p_picsys->sw.p_vd_sys;

    AndroidWindow_UnlockPicture(sys, sys->p_window, p_pic, false);
}

static int PoolLockOpaquePicture(picture_t *p_pic)
{
    picture_sys_t *p_picsys = p_pic->p_sys;

    p_picsys->b_locked = true;
    return 0;
}

static void PoolUnlockOpaquePicture(picture_t *p_pic)
{
    picture_sys_t *p_picsys = p_pic->p_sys;

    AndroidOpaquePicture_Release(p_picsys, false);
}

static picture_pool_t *PoolAlloc(vout_display_t *vd, unsigned requested_count)
{
    vout_display_sys_t *sys = vd->sys;
    picture_pool_t *pool = NULL;
    picture_t **pp_pics = NULL;
    unsigned int i = 0;

    msg_Dbg(vd, "PoolAlloc: request %d frames", requested_count);
    if (AndroidWindow_Setup(sys, sys->p_window, requested_count) != 0)
        goto error;

    requested_count = sys->p_window->i_pic_count;
    msg_Dbg(vd, "PoolAlloc: got %d frames", requested_count);

    UpdateVideoSize(sys, &sys->p_window->fmt, sys->p_window->b_use_priv);

    pp_pics = calloc(requested_count, sizeof(picture_t));

    for (i = 0; i < requested_count; i++)
    {
        picture_t *p_pic = PictureAlloc(sys, &sys->p_window->fmt,
                                        sys->p_window->b_opaque);
        if (!p_pic)
            goto error;

        pp_pics[i] = p_pic;
    }

    picture_pool_configuration_t pool_cfg;
    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.picture_count = requested_count;
    pool_cfg.picture       = pp_pics;
    if (sys->p_window->b_opaque)
    {
        pool_cfg.lock      = PoolLockOpaquePicture;
        pool_cfg.unlock    = PoolUnlockOpaquePicture;
    }
    else
    {
        pool_cfg.lock      = PoolLockPicture;
        pool_cfg.unlock    = PoolUnlockPicture;
    }
    pool = picture_pool_NewExtended(&pool_cfg);

error:
    if (!pool && pp_pics) {
        for (unsigned j = 0; j < i; j++)
            picture_Release(pp_pics[j]);
    }
    free(pp_pics);
    return pool;
}

static void SubtitleRegionToBounds(subpicture_t *subpicture,
                                   ARect *p_out_bounds)
{
    if (subpicture) {
        for (subpicture_region_t *r = subpicture->p_region; r != NULL; r = r->p_next) {
            ARect new_bounds;

            new_bounds.left = r->i_x;
            new_bounds.top = r->i_y;
            if (new_bounds.left < 0)
                new_bounds.left = 0;
            if (new_bounds.top < 0)
                new_bounds.top = 0;
            new_bounds.right = r->fmt.i_visible_width + r->i_x;
            new_bounds.bottom = r->fmt.i_visible_height + r->i_y;
            if (r == &subpicture->p_region[0])
                *p_out_bounds = new_bounds;
            else {
                if (p_out_bounds->left > new_bounds.left)
                    p_out_bounds->left = new_bounds.left;
                if (p_out_bounds->right < new_bounds.right)
                    p_out_bounds->right = new_bounds.right;
                if (p_out_bounds->top > new_bounds.top)
                    p_out_bounds->top = new_bounds.top;
                if (p_out_bounds->bottom < new_bounds.bottom)
                    p_out_bounds->bottom = new_bounds.bottom;
            }
        }
    } else {
        p_out_bounds->left = p_out_bounds->top = 0;
        p_out_bounds->right = p_out_bounds->bottom = 0;
    }
}

static void SubtitleGetDirtyBounds(vout_display_t *vd,
                                   subpicture_t *subpicture,
                                   ARect *p_out_bounds)
{
    vout_display_sys_t *sys = vd->sys;
    int i = 0;
    bool b_found = false;

    /* Try to find last bounds set by current locked buffer.
     * Indeed, even if we can lock only one buffer at a time, differents
     * buffers can be locked. This functions will find the last bounds set by
     * the current buffer. */
    if (sys->p_sub_buffer_bounds) {
        for (; sys->p_sub_buffer_bounds[i].p_pixels != NULL; ++i) {
            buffer_bounds *p_bb = &sys->p_sub_buffer_bounds[i];
            if (p_bb->p_pixels == sys->p_sub_pic->p[0].p_pixels) {
                *p_out_bounds = p_bb->bounds;
                b_found = true;
                break;
            }
        }
    }

    if (!b_found
     || p_out_bounds->left < 0
     || p_out_bounds->right < 0
     || (unsigned int) p_out_bounds->right > sys->p_sub_pic->format.i_width
     || p_out_bounds->bottom < 0
     || p_out_bounds->top < 0
     || (unsigned int) p_out_bounds->top > sys->p_sub_pic->format.i_height)
    {
        /* default is full picture */
        p_out_bounds->left = 0;
        p_out_bounds->top = 0;
        p_out_bounds->right = sys->p_sub_pic->format.i_width;
        p_out_bounds->bottom = sys->p_sub_pic->format.i_height;
    }

    /* buffer not found, add it to the array */
    if (!sys->p_sub_buffer_bounds
     || sys->p_sub_buffer_bounds[i].p_pixels == NULL) {
        buffer_bounds *p_bb = realloc(sys->p_sub_buffer_bounds,
                                      (i + 2) * sizeof(buffer_bounds));
        if (p_bb) {
            sys->p_sub_buffer_bounds = p_bb;
            sys->p_sub_buffer_bounds[i].p_pixels = sys->p_sub_pic->p[0].p_pixels;
            sys->p_sub_buffer_bounds[i+1].p_pixels = NULL;
        }
    }

    /* set buffer bounds */
    if (sys->p_sub_buffer_bounds
     && sys->p_sub_buffer_bounds[i].p_pixels != NULL)
        SubtitleRegionToBounds(subpicture, &sys->p_sub_buffer_bounds[i].bounds);
}

static void SubpicturePrepare(vout_display_t *vd, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    ARect memset_bounds;
    bool b_stack_outside = subpicture && var_InheritBool(vd, XR_SUBTITLE_STACK_VAR);

    SubtitleRegionToBounds(subpicture, &memset_bounds);
    if (sys->i_xr_sub_subprepare_entry_logs < 20)
    {
        msg_Err(vd, "XR_SUB_RENDER SubpicturePrepare entry subpicture=%p p_sub_window=%p surface=%p p_sub_pic=%p p_spu_blend=%p bounds=[%d,%d,%d,%d] order=%lld last_order=%lld",
                (void *) subpicture, (void *) sys->p_sub_window,
                sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                (void *) sys->p_sub_pic, (void *) sys->p_spu_blend,
                memset_bounds.left, memset_bounds.top,
                memset_bounds.right, memset_bounds.bottom,
                subpicture ? (long long) subpicture->i_order : -1,
                (long long) sys->i_sub_last_order);
        if (sys->i_xr_sub_subprepare_entry_logs < UINT_MAX)
            sys->i_xr_sub_subprepare_entry_logs++;
    }

    if( subpicture )
    {
        if( subpicture->i_order == sys->i_sub_last_order
         && memcmp( &memset_bounds, &sys->sub_last_region, sizeof(ARect) ) == 0 )
        {
            return;
        }

        sys->i_sub_last_order = subpicture->i_order;
        sys->sub_last_region = memset_bounds;
    }

    subpicture_t *scaled = NULL;
    subpicture_t *stacked = NULL;
    subpicture_t *blend_subpicture = subpicture;
    if (subpicture && sys->p_sub_pic
     && subpicture->i_original_picture_width > 0
     && subpicture->i_original_picture_height > 0
     && ((unsigned) subpicture->i_original_picture_width
            != sys->p_sub_pic->format.i_visible_width
      || (unsigned) subpicture->i_original_picture_height
            != sys->p_sub_pic->format.i_visible_height))
    {
        scaled = XrSubtitleScaleSubpicture(
            vd, subpicture,
            sys->p_sub_pic->format.i_visible_width,
            sys->p_sub_pic->format.i_visible_height);
        if (!scaled)
        {
            msg_Err(vd, "XR_SUB_RENDER failed to scale subpicture canvas=%dx%d target=%ux%u",
                    subpicture->i_original_picture_width,
                    subpicture->i_original_picture_height,
                    sys->p_sub_pic->format.i_visible_width,
                    sys->p_sub_pic->format.i_visible_height);
            return;
        }
        blend_subpicture = scaled;
    }
    if (b_stack_outside)
    {
        stacked = XrSubtitleStackSubpicture(blend_subpicture,
                                            sys->p_sub_pic->format.i_width,
                                            sys->p_sub_pic->format.i_height);
        if (stacked)
            blend_subpicture = stacked;
    }

    int i_lock_result = AndroidWindow_LockPicture(sys, sys->p_sub_window, sys->p_sub_pic);
    if (i_lock_result != 0)
    {
        msg_Err(vd, "XR_SUB_RENDER SubpicturePrepare lock failed result=%d subpicture=%p p_sub_window=%p surface=%p p_sub_pic=%p",
                i_lock_result, (void *) subpicture,
                (void *) sys->p_sub_window,
                sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                (void *) sys->p_sub_pic);
        if (stacked)
            subpicture_Delete(stacked);
        if (scaled)
            subpicture_Delete(scaled);
        return;
    }
    sys->b_subpicture_updated = true;

    /* Clear the subtitles surface. */
    SubtitleGetDirtyBounds(vd, blend_subpicture, &memset_bounds);
    const int x_pixels_offset = memset_bounds.left
                                * sys->p_sub_pic->p[0].i_pixel_pitch;
    const int i_line_size = (memset_bounds.right - memset_bounds.left)
                            * sys->p_sub_pic->p->i_pixel_pitch;
    for (int y = memset_bounds.top; y < memset_bounds.bottom; y++)
        memset(&sys->p_sub_pic->p[0].p_pixels[y * sys->p_sub_pic->p[0].i_pitch
                                              + x_pixels_offset], 0, i_line_size);

    int i_blend_result = 0;
    if (subpicture)
        i_blend_result = picture_BlendSubpicture(sys->p_sub_pic,
                                                 sys->p_spu_blend,
                                                 blend_subpicture);
    if (stacked)
        subpicture_Delete(stacked);
    if (scaled)
        subpicture_Delete(scaled);

    bool b_log_success = sys->i_xr_sub_lock_logs < 20;
    bool b_log_failure = subpicture && i_blend_result <= 0;
    if (b_log_failure)
    {
        msg_Err(vd, "XR_SUB_RENDER SubpicturePrepare lock/blend result lock=%d blend=%d subpicture=%p p_sub_window=%p surface=%p p_sub_pic=%p pixels=%p bounds=[%d,%d,%d,%d] order=%lld",
                i_lock_result, i_blend_result, (void *) subpicture,
                (void *) sys->p_sub_window,
                sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                (void *) sys->p_sub_pic,
                sys->p_sub_pic ? (void *) sys->p_sub_pic->p[0].p_pixels : NULL,
                memset_bounds.left, memset_bounds.top,
                memset_bounds.right, memset_bounds.bottom,
                subpicture ? (long long) subpicture->i_order : -1);
    }
    else if (b_log_success)
    {
        msg_Dbg(vd, "XR_SUB_RENDER SubpicturePrepare lock/blend result lock=%d blend=%d subpicture=%p p_sub_window=%p surface=%p p_sub_pic=%p pixels=%p bounds=[%d,%d,%d,%d] order=%lld",
                i_lock_result, i_blend_result, (void *) subpicture,
                (void *) sys->p_sub_window,
                sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                (void *) sys->p_sub_pic,
                sys->p_sub_pic ? (void *) sys->p_sub_pic->p[0].p_pixels : NULL,
                memset_bounds.left, memset_bounds.top,
                memset_bounds.right, memset_bounds.bottom,
                subpicture ? (long long) subpicture->i_order : -1);
    }
    if (sys->i_xr_sub_lock_logs < UINT_MAX)
        sys->i_xr_sub_lock_logs++;
}

static picture_pool_t *Pool(vout_display_t *vd, unsigned requested_count)
{
    vout_display_sys_t *sys = vd->sys;

    if (sys->pool == NULL)
        sys->pool = PoolAlloc(vd, requested_count);
    return sys->pool;
}

static void Prepare(vout_display_t *vd, picture_t *picture,
                    subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    VLC_UNUSED(picture);

    sys->b_subpicture_updated = false;

    if (!XrSubtitleSurfaceEnabled(vd))
    {
        if (sys->p_sub_window || sys->p_sub_pic || sys->p_spu_blend ||
            sys->b_has_subpictures)
        {
            msg_Err(vd, "XR_SUB_RENDER disabled; releasing subtitle window p_sub_window=%p surface=%p subpicture=%p",
                    (void *) sys->p_sub_window,
                    sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                    (void *) subpicture);
            XrSubtitleReleaseSubWindow(vd);
        }
        return;
    }

    if ((subpicture || sys->b_has_subpictures) &&
        !XrSubtitleEnsureSubWindow(vd))
    {
        msg_Err(vd, "XR_SUB_RENDER Prepare skip reason=ensure_sub_window_failed subpicture=%p",
                (void *) subpicture);
        return;
    }

    if (!subpicture && !sys->b_has_subpictures)
        goto end;

    bool b_prepare_entry_success = sys->i_xr_sub_prepare_entry_logs < 20;
    bool b_prepare_entry_skip = !subpicture || !sys->p_sub_window;
    if (b_prepare_entry_success || b_prepare_entry_skip)
    {
        msg_Dbg(vd, "XR_SUB_RENDER Prepare entry subpicture=%p p_sub_window=%p surface=%p p_sub_pic=%p p_spu_blend=%p has_subpictures=%d b_sub_invalid=%d order=%lld skip_reason=%s",
                (void *) subpicture, (void *) sys->p_sub_window,
                sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                (void *) sys->p_sub_pic, (void *) sys->p_spu_blend,
                sys->b_has_subpictures, sys->b_sub_invalid,
                subpicture ? (long long) subpicture->i_order : -1,
                !subpicture ? "subpicture_null" :
                (!sys->p_sub_window ? "p_sub_window_null" : "none"));
    }
    if (sys->i_xr_sub_prepare_entry_logs < UINT_MAX)
        sys->i_xr_sub_prepare_entry_logs++;

    if (subpicture && sys->p_sub_window) {
        int i_setup_result = 0;
        bool b_setup_called = false;
        bool b_picture_alloc_attempted = false;
        bool b_blend_create_attempted = false;
        if (sys->b_sub_invalid) {
            sys->b_sub_invalid = false;
            if (sys->p_sub_pic) {
                picture_Release(sys->p_sub_pic);
                sys->p_sub_pic = NULL;
            }
            if (sys->p_spu_blend) {
                filter_DeleteBlend(sys->p_spu_blend);
                sys->p_spu_blend = NULL;
            }
            free(sys->p_sub_buffer_bounds);
            sys->p_sub_buffer_bounds = NULL;
        }

        if (!sys->p_sub_pic)
        {
            b_setup_called = true;
            i_setup_result = AndroidWindow_Setup(sys, sys->p_sub_window, 1);
            if (i_setup_result == 0)
            {
                b_picture_alloc_attempted = true;
                sys->p_sub_pic = PictureAlloc(sys, &sys->p_sub_window->fmt, false);
            }
        }
        if (!sys->p_spu_blend && sys->p_sub_pic)
        {
            b_blend_create_attempted = true;
            sys->p_spu_blend = filter_NewBlend(VLC_OBJECT(vd),
                                               &sys->p_sub_pic->format);
        }

        if (sys->p_sub_pic && sys->p_spu_blend)
            sys->b_has_subpictures = true;

        bool b_ready = sys->p_sub_pic && sys->p_spu_blend;
        bool b_log_success = b_ready && sys->i_xr_sub_prepare_logs < 20;
        bool b_log_failure = !b_ready;
        if (b_log_failure)
        {
            msg_Err(vd, "XR_SUB_RENDER Prepare subpicture=%p p_sub_window=%p surface=%p setup=%d setup_called=%d picture_alloc_attempted=%d blend_create_attempted=%d p_sub_pic=%p p_spu_blend=%p has_subpictures=%d ready=%d order=%lld",
                    (void *) subpicture, (void *) sys->p_sub_window,
                    (void *) sys->p_sub_window->p_surface, i_setup_result,
                    b_setup_called, b_picture_alloc_attempted,
                    b_blend_create_attempted,
                    (void *) sys->p_sub_pic, (void *) sys->p_spu_blend,
                    sys->b_has_subpictures, b_ready,
                    subpicture ? (long long) subpicture->i_order : -1);
        }
        else if (b_log_success)
        {
            msg_Dbg(vd, "XR_SUB_RENDER Prepare subpicture=%p p_sub_window=%p surface=%p setup=%d setup_called=%d picture_alloc_attempted=%d blend_create_attempted=%d p_sub_pic=%p p_spu_blend=%p has_subpictures=%d ready=%d order=%lld",
                    (void *) subpicture, (void *) sys->p_sub_window,
                    (void *) sys->p_sub_window->p_surface, i_setup_result,
                    b_setup_called, b_picture_alloc_attempted,
                    b_blend_create_attempted,
                    (void *) sys->p_sub_pic, (void *) sys->p_spu_blend,
                    sys->b_has_subpictures, b_ready,
                    subpicture ? (long long) subpicture->i_order : -1);
        }
        if (b_ready && sys->i_xr_sub_prepare_logs < UINT_MAX)
            sys->i_xr_sub_prepare_logs++;
    }
    else
    {
        if (subpicture)
            msg_Err(vd, "XR_SUB_RENDER Prepare skip reason=p_sub_window_null subpicture=%p p_sub_window=%p surface=%p has_subpictures=%d p_sub_pic=%p p_spu_blend=%p",
                    (void *) subpicture, (void *) sys->p_sub_window,
                    sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                    sys->b_has_subpictures, (void *) sys->p_sub_pic,
                    (void *) sys->p_spu_blend);
        else
            msg_Dbg(vd, "XR_SUB_RENDER Prepare skip reason=subpicture_null subpicture=%p p_sub_window=%p surface=%p has_subpictures=%d p_sub_pic=%p p_spu_blend=%p",
                    (void *) subpicture, (void *) sys->p_sub_window,
                    sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                    sys->b_has_subpictures, (void *) sys->p_sub_pic,
                    (void *) sys->p_spu_blend);
    }
    /* As long as no subpicture was received, do not call
       SubpictureDisplay since JNI calls and clearing the subtitles
       surface are expensive operations. */
    if (sys->b_has_subpictures)
    {
        msg_Dbg(vd, "XR_SUB_RENDER Prepare before_subprepare has_subpictures=%d subpicture=%p p_sub_window=%p surface=%p p_sub_pic=%p p_spu_blend=%p",
                sys->b_has_subpictures, (void *) subpicture,
                (void *) sys->p_sub_window,
                sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                (void *) sys->p_sub_pic, (void *) sys->p_spu_blend);
        SubpicturePrepare(vd, subpicture);
        if (!subpicture)
        {
            /* The surface has been cleared and there is no new
               subpicture to upload, do not clear again until a new
               subpicture is received. */
            sys->b_has_subpictures = false;
        }
    }
    else
    {
        msg_Dbg(vd, "XR_SUB_RENDER Prepare skip_subprepare reason=no_subpictures subpicture=%p p_sub_window=%p surface=%p p_sub_pic=%p p_spu_blend=%p",
                (void *) subpicture, (void *) sys->p_sub_window,
                sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                (void *) sys->p_sub_pic, (void *) sys->p_spu_blend);
    }

end:
    if (sys->p_window->b_opaque
     && AndroidOpaquePicture_CanReleaseAtTime(picture->p_sys))
    {
        vlc_tick_t now = mdate();
        if (picture->date > now)
        {
            if (picture->date - now <= INT64_C(1000000))
            {
                AndroidOpaquePicture_ReleaseAtTime(picture->p_sys, picture->date);
            }
            else /* The picture will be displayed from the Display callback */
            {
                bool b_locked;
                int i_index = XrAndroidDiagOpaqueIndex(picture, &b_locked);
                msg_Warn(vd, "picture way too early to release at time");
                msg_Warn(vd, "XR_ANDROID_DISPLAY_DIAG way_too_early "
                         "picture=%p index=%d locked=%d date_ms=%lld "
                         "now_ms=%lld delta_ms=%lld",
                         (void *) picture, i_index, b_locked,
                         XrAndroidDiagMs(picture->date),
                         XrAndroidDiagMs(now),
                         XrAndroidDiagDeltaMs(picture->date, now));
            }
        }
    }
}

static void Display(vout_display_t *vd, picture_t *picture,
                    subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    bool b_display_entry_success = sys->i_xr_sub_display_entry_logs < 20;
    bool b_display_entry_skip = false;
    if (b_display_entry_success || b_display_entry_skip)
    {
        msg_Dbg(vd, "XR_SUB_RENDER Display entry p_sub_pic=%p p_sub_window=%p surface=%p subpicture=%p will_post=%d",
                (void *) sys->p_sub_pic, (void *) sys->p_sub_window,
                sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                (void *) subpicture,
                sys->b_subpicture_updated && sys->p_sub_pic != NULL);
    }
    if (sys->i_xr_sub_display_entry_logs < UINT_MAX)
        sys->i_xr_sub_display_entry_logs++;

    if (sys->p_window->b_opaque)
    {
        AndroidOpaquePicture_Release(picture->p_sys, true);
    }
    else
        AndroidWindow_UnlockPicture(sys, sys->p_window, picture, true);

    picture_Release(picture);

    if (sys->b_subpicture_updated && sys->p_sub_pic)
    {
        if (sys->i_xr_sub_display_logs < 20)
            msg_Dbg(vd, "XR_SUB_RENDER Display posting subtitle surface p_sub_window=%p surface=%p p_sub_pic=%p subpicture=%p",
                    (void *) sys->p_sub_window,
                    sys->p_sub_window ? (void *) sys->p_sub_window->p_surface : NULL,
                    (void *) sys->p_sub_pic, (void *) subpicture);
        if (sys->i_xr_sub_display_logs < UINT_MAX)
            sys->i_xr_sub_display_logs++;
        AndroidWindow_UnlockPicture(sys, sys->p_sub_window, sys->p_sub_pic,
                                    true);
    }
    sys->b_subpicture_updated = false;

    if (subpicture)
        subpicture_Delete(subpicture);

    sys->b_displayed = true;
}

static void CopySourceAspect(video_format_t *p_dest,
                             const video_format_t *p_src)
{
    p_dest->i_sar_num = p_src->i_sar_num;
    p_dest->i_sar_den = p_src->i_sar_den;
}

static int Control(vout_display_t *vd, int query, va_list args)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query) {
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    {
        msg_Dbg(vd, "change source crop/aspect");

        if (query == VOUT_DISPLAY_CHANGE_SOURCE_CROP) {
            video_format_CopyCrop(&sys->p_window->fmt, &vd->source);
            AndroidWindow_UpdateCrop(sys, sys->p_window);
        } else
            CopySourceAspect(&sys->p_window->fmt, &vd->source);

        UpdateVideoSize(sys, &sys->p_window->fmt, sys->p_window->b_use_priv);
        FixSubtitleFormat(sys);
        return VLC_SUCCESS;
    }
    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
    {
        const vout_display_cfg_t *cfg = va_arg(args, const vout_display_cfg_t *);

        sys->i_display_width = cfg->display.width;
        sys->i_display_height = cfg->display.height;
        msg_Dbg(vd, "change display size: %dx%d", sys->i_display_width,
                                                  sys->i_display_height);
        FixSubtitleFormat(sys);
        return VLC_SUCCESS;
    }
    case VOUT_DISPLAY_RESET_PICTURES:
        vlc_assert_unreachable();
    default:
        msg_Warn(vd, "Unknown request in android-display: %d", query);
    case VOUT_DISPLAY_CHANGE_ZOOM:
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
        return VLC_EGENERIC;
    }
}
