/**
 * @file display.c
 * @brief OpenGL video output module
 */
/*****************************************************************************
 * Copyright © 2010-2011 Rémi Denis-Courmont
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
# include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>
#include <math.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_filter.h>
#include <vlc_opengl.h>
#include "vout_helper.h"

#if defined(__ANDROID__)
# include "../android/display.h"
# include "../android/utils.h"
#endif

/* Plugin callbacks */
static int Open (vlc_object_t *);
static void Close (vlc_object_t *);

#define GL_TEXT N_("OpenGL extension")
#define GLES2_TEXT N_("OpenGL ES 2 extension")
#define PROVIDER_LONGTEXT N_( \
    "Extension through which to use the Open Graphics Library (OpenGL).")

vlc_module_begin ()
#if defined (USE_OPENGL_ES2)
# define API VLC_OPENGL_ES2
# define MODULE_VARNAME "gles2"
    set_shortname (N_("OpenGL ES2"))
    set_description (N_("OpenGL for Embedded Systems 2 video output"))
    set_capability ("vout display", 265)
    set_callbacks (Open, Close)
    add_shortcut ("opengles2", "gles2")
    add_module ("gles2", "opengl es2", NULL,
                GLES2_TEXT, PROVIDER_LONGTEXT, true)

#else

# define API VLC_OPENGL
# define MODULE_VARNAME "gl"
    set_shortname (N_("OpenGL"))
    set_description (N_("OpenGL video output"))
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("vout display", 270)
    set_callbacks (Open, Close)
    add_shortcut ("opengl", "gl")
    add_module ("gl", "opengl", NULL,
                GL_TEXT, PROVIDER_LONGTEXT, true)
#endif
    add_glopts ()
vlc_module_end ()

struct vout_display_sys_t
{
    vout_display_opengl_t *vgl;
    vlc_gl_t *gl;
    picture_pool_t *pool;
#if defined(__ANDROID__)
    AWindowHandler *awh;
    native_window_api_t *anw;
    ANativeWindow *sub_surface;
    video_format_t sub_fmt;
    picture_t *sub_pic;
    filter_t *spu_blend;
    bool sub_invalid;
    bool has_subpictures;
    unsigned int sub_surface_logs;
    unsigned int sub_render_logs;
#endif
};

/* Display callbacks */
static picture_pool_t *Pool (vout_display_t *, unsigned);
static void PictureRender (vout_display_t *, picture_t *, subpicture_t *);
static void PictureDisplay (vout_display_t *, picture_t *, subpicture_t *);
static int Control (vout_display_t *, int, va_list);

#if defined(__ANDROID__)
typedef struct
{
    subpicture_region_t *region;
    double order;
    unsigned index;
} xr_subtitle_region_ref_t;

#define XR_SUBTITLE_STACK_VAR "xr-subtitle-stack-outside"
#define XR_SUBTITLE_SURFACE_ENABLED_VAR "xr-subtitle-surface-enabled"

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

static void XrSubtitleInvalidate(vout_display_sys_t *sys)
{
    sys->sub_invalid = true;
}

static void XrSubtitleSetRGBMask(video_format_t *fmt)
{
    fmt->i_rmask = 0x000000ff;
    fmt->i_gmask = 0x0000ff00;
    fmt->i_bmask = 0x00ff0000;
}

static void XrSubtitleFormat(vout_display_t *vd, video_format_t *fmt)
{
    vout_display_place_t place;
    video_format_t spu_fmt = vd->source;

    vout_display_PlacePicture(&place, &vd->source, vd->cfg, false);
    if (spu_fmt.i_width * spu_fmt.i_height < place.width * place.height)
    {
        spu_fmt.i_sar_num = vd->cfg->display.sar.num;
        spu_fmt.i_sar_den = vd->cfg->display.sar.den;
        spu_fmt.i_width =
        spu_fmt.i_visible_width = place.width;
        spu_fmt.i_height =
        spu_fmt.i_visible_height = place.height;
    }

    video_format_ApplyRotation(fmt, &spu_fmt);
    if (fmt->i_visible_width == 0)
        fmt->i_visible_width = fmt->i_width;
    if (fmt->i_visible_height == 0)
        fmt->i_visible_height = fmt->i_height;
    fmt->i_width = fmt->i_visible_width;
    fmt->i_height = fmt->i_visible_height;
    fmt->i_x_offset = 0;
    fmt->i_y_offset = 0;
    fmt->i_sar_num = 1;
    fmt->i_sar_den = 1;
    fmt->i_chroma = VLC_CODEC_RGBA;
    XrSubtitleSetRGBMask(fmt);
    video_format_FixRgb(fmt);
}

static picture_t *XrSubtitlePictureAlloc(vout_display_sys_t *sys)
{
    picture_sys_t *picsys = calloc(1, sizeof(*picsys));
    if (unlikely(picsys == NULL))
        return NULL;

    picsys->sw.p_vd_sys = sys;

    picture_resource_t rsc;
    memset(&rsc, 0, sizeof(rsc));
    rsc.p_sys = picsys;

    picture_t *pic = picture_NewFromResource(&sys->sub_fmt, &rsc);
    if (!pic)
    {
        free(picsys);
        return NULL;
    }

    return pic;
}

static void XrSubtitleReleaseResources(vout_display_sys_t *sys)
{
    if (sys->sub_pic)
    {
        picture_Release(sys->sub_pic);
        sys->sub_pic = NULL;
    }
    if (sys->spu_blend)
    {
        filter_DeleteBlend(sys->spu_blend);
        sys->spu_blend = NULL;
    }
}

static void XrSubtitleReleaseSurface(vout_display_sys_t *sys)
{
    if (sys->sub_surface && sys->awh)
    {
        AWindowHandler_releaseANativeWindow(sys->awh, AWindow_Subtitles);
        sys->sub_surface = NULL;
    }
}

static int XrSubtitleEnsureSurface(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (!sys->awh)
        return VLC_EGENERIC;

    ANativeWindow *surface = AWindowHandler_getANativeWindow(sys->awh,
                                                             AWindow_Subtitles);
    if (!surface)
    {
        if (sys->sub_surface)
        {
            XrSubtitleReleaseResources(sys);
            sys->sub_surface = NULL;
            sys->sub_invalid = true;
        }
        if (sys->sub_surface_logs < 20)
        {
            msg_Err(vd, "XR_SUB_GL_SURFACE get AWindow_Subtitles failed awh=%p",
                    (void *) sys->awh);
            sys->sub_surface_logs++;
        }
        return VLC_EGENERIC;
    }

    if (sys->sub_surface != surface)
    {
        XrSubtitleReleaseResources(sys);
        sys->sub_invalid = true;
        sys->sub_surface = surface;
        msg_Err(vd, "XR_SUB_GL_SURFACE got AWindow_Subtitles surface=%p awh=%p",
                (void *) sys->sub_surface, (void *) sys->awh);
    }

    return VLC_SUCCESS;
}

static int XrSubtitleEnsureResources(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    video_format_t fmt;

    if (XrSubtitleEnsureSurface(vd) != VLC_SUCCESS)
        return VLC_EGENERIC;

    XrSubtitleFormat(vd, &fmt);
    if (sys->sub_invalid ||
        fmt.i_width != sys->sub_fmt.i_width ||
        fmt.i_height != sys->sub_fmt.i_height ||
        fmt.i_chroma != sys->sub_fmt.i_chroma)
    {
        sys->sub_fmt = fmt;
        XrSubtitleReleaseResources(sys);
        sys->sub_invalid = false;

        if (sys->anw->setBuffersGeometry &&
            sys->anw->setBuffersGeometry(sys->sub_surface,
                                         sys->sub_fmt.i_width,
                                         sys->sub_fmt.i_height,
                                         WINDOW_FORMAT_RGBA_8888) != 0)
        {
            msg_Err(vd, "XR_SUB_GL_SURFACE setBuffersGeometry failed surface=%p fmt=%ux%u",
                    (void *) sys->sub_surface, sys->sub_fmt.i_width,
                    sys->sub_fmt.i_height);
            return VLC_EGENERIC;
        }

        msg_Err(vd, "XR_SUB_GL_SURFACE configured surface=%p fmt=%ux%u visible=%ux%u chroma=0x%08x",
                (void *) sys->sub_surface, sys->sub_fmt.i_width,
                sys->sub_fmt.i_height, sys->sub_fmt.i_visible_width,
                sys->sub_fmt.i_visible_height, sys->sub_fmt.i_chroma);
    }

    if (!sys->sub_pic)
        sys->sub_pic = XrSubtitlePictureAlloc(sys);
    if (!sys->sub_pic)
    {
        msg_Err(vd, "XR_SUB_GL_SURFACE subtitle picture allocation failed fmt=%ux%u",
                sys->sub_fmt.i_width, sys->sub_fmt.i_height);
        return VLC_EGENERIC;
    }

    if (!sys->spu_blend)
        sys->spu_blend = filter_NewBlend(VLC_OBJECT(vd),
                                         &sys->sub_pic->format);
    if (!sys->spu_blend)
    {
        msg_Err(vd, "XR_SUB_GL_SURFACE blend creation failed fmt=%ux%u chroma=0x%08x",
                sys->sub_pic->format.i_width, sys->sub_pic->format.i_height,
                sys->sub_pic->format.i_chroma);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static int XrSubtitleLock(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    picture_sys_t *picsys = sys->sub_pic->p_sys;

    if (picsys->b_locked)
        return VLC_EGENERIC;

    if (sys->anw->winLock(sys->sub_surface, &picsys->sw.buf, NULL) != 0)
    {
        msg_Err(vd, "XR_SUB_GL_SURFACE lock failed surface=%p",
                (void *) sys->sub_surface);
        return VLC_EGENERIC;
    }

    if (picsys->sw.buf.width < 0 ||
        picsys->sw.buf.height < 0 ||
        (unsigned) picsys->sw.buf.width < sys->sub_fmt.i_width ||
        (unsigned) picsys->sw.buf.height < sys->sub_fmt.i_height)
    {
        sys->anw->unlockAndPost(sys->sub_surface);
        msg_Err(vd, "XR_SUB_GL_SURFACE lock buffer too small surface=%p buffer=%dx%d fmt=%ux%u",
                (void *) sys->sub_surface, picsys->sw.buf.width,
                picsys->sw.buf.height, sys->sub_fmt.i_width,
                sys->sub_fmt.i_height);
        return VLC_EGENERIC;
    }

    sys->sub_pic->p[0].p_pixels = picsys->sw.buf.bits;
    sys->sub_pic->p[0].i_lines = picsys->sw.buf.height;
    sys->sub_pic->p[0].i_pitch =
        sys->sub_pic->p[0].i_pixel_pitch * picsys->sw.buf.stride;
    picsys->b_locked = true;
    return VLC_SUCCESS;
}

static void XrSubtitleUnlockAndPost(vout_display_sys_t *sys)
{
    picture_sys_t *picsys = sys->sub_pic->p_sys;

    if (!picsys->b_locked)
        return;

    sys->anw->unlockAndPost(sys->sub_surface);
    picsys->b_locked = false;
}

static void XrSubtitleClearAndRelease(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    if (sys->sub_surface && sys->sub_pic && XrSubtitleLock(vd) == VLC_SUCCESS)
    {
        for (int y = 0; y < sys->sub_pic->p[0].i_lines; ++y)
            memset(&sys->sub_pic->p[0].p_pixels[y * sys->sub_pic->p[0].i_pitch],
                   0, sys->sub_pic->p[0].i_pitch);
        XrSubtitleUnlockAndPost(sys);
    }

    sys->has_subpictures = false;
    XrSubtitleReleaseResources(sys);
    XrSubtitleReleaseSurface(sys);
}

static bool XrSubtitleRender(vout_display_t *vd, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    if (!var_InheritBool(vd, XR_SUBTITLE_SURFACE_ENABLED_VAR))
    {
        if (sys->sub_surface || sys->sub_pic || sys->spu_blend ||
            sys->has_subpictures)
        {
            msg_Err(vd, "XR_SUB_GL_SURFACE disabled; releasing subtitle surface=%p subpicture=%p",
                    (void *) sys->sub_surface, (void *) subpicture);
            XrSubtitleClearAndRelease(vd);
        }
        return false;
    }

    if (!subpicture && !sys->has_subpictures)
        return false;
    if (XrSubtitleEnsureSurface(vd) != VLC_SUCCESS)
        return false;
    if (XrSubtitleEnsureResources(vd) != VLC_SUCCESS)
        return true;
    if (XrSubtitleLock(vd) != VLC_SUCCESS)
        return true;

    for (int y = 0; y < sys->sub_pic->p[0].i_lines; ++y)
        memset(&sys->sub_pic->p[0].p_pixels[y * sys->sub_pic->p[0].i_pitch],
               0, sys->sub_pic->p[0].i_pitch);

    int blend_result = 0;
    subpicture_t *stacked = NULL;
    subpicture_t *blend_subpicture = subpicture;
    if (subpicture && var_InheritBool(vd, XR_SUBTITLE_STACK_VAR))
    {
        stacked = XrSubtitleStackSubpicture(subpicture,
                                            sys->sub_pic->format.i_width,
                                            sys->sub_pic->format.i_height);
        if (stacked)
            blend_subpicture = stacked;
    }
    if (subpicture)
        blend_result = picture_BlendSubpicture(sys->sub_pic, sys->spu_blend,
                                               blend_subpicture);
    if (stacked)
        subpicture_Delete(stacked);

    XrSubtitleUnlockAndPost(sys);
    sys->has_subpictures = subpicture != NULL;

    bool log_success = sys->sub_render_logs < 20;
    bool log_failure = subpicture && blend_result <= 0;
    if (log_success || log_failure)
    {
        msg_Err(vd, "XR_SUB_GL_SURFACE post surface=%p subpicture=%p blend=%d fmt=%ux%u pitch=%d lines=%d order=%lld",
                (void *) sys->sub_surface, (void *) subpicture,
                blend_result, sys->sub_pic->format.i_width,
                sys->sub_pic->format.i_height,
                sys->sub_pic->p[0].i_pitch, sys->sub_pic->p[0].i_lines,
                subpicture ? (long long) subpicture->i_order : -1);
        if (sys->sub_render_logs < UINT_MAX)
            sys->sub_render_logs++;
    }
    return true;
}
#endif

/**
 * Allocates a surface and an OpenGL context for video output.
 */
static int Open (vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = malloc (sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    sys->gl = NULL;
    sys->pool = NULL;
#if defined(__ANDROID__)
    sys->awh = NULL;
    sys->anw = NULL;
    sys->sub_surface = NULL;
    sys->sub_pic = NULL;
    sys->spu_blend = NULL;
    sys->sub_invalid = true;
    sys->has_subpictures = false;
    sys->sub_surface_logs = 0;
    sys->sub_render_logs = 0;
#endif

    vout_window_t *surface = vout_display_NewWindow (vd, VOUT_WINDOW_TYPE_INVALID);
    if (surface == NULL)
    {
        msg_Err (vd, "parent window not available");
        goto error;
    }

    const char *gl_name = "$" MODULE_VARNAME;

    /* VDPAU GL interop works only with GLX. Override the "gl" option to force
     * it. */
#ifndef USE_OPENGL_ES2
    if (surface->type == VOUT_WINDOW_TYPE_XID)
    {
        switch (vd->fmt.i_chroma)
        {
            case VLC_CODEC_VDPAU_VIDEO_444:
            case VLC_CODEC_VDPAU_VIDEO_422:
            case VLC_CODEC_VDPAU_VIDEO_420:
            {
                /* Force the option only if it was not previously set */
                char *str = var_InheritString(surface, MODULE_VARNAME);
                if (str == NULL || str[0] == 0 || strcmp(str, "any") == 0)
                    gl_name = "glx";
                free(str);
                break;
            }
            default:
                break;
        }
    }
#endif

    sys->gl = vlc_gl_Create (surface, API, gl_name);
    if (sys->gl == NULL)
        goto error;

    vlc_gl_Resize (sys->gl, vd->cfg->display.width, vd->cfg->display.height);

    /* Initialize video display */
    const vlc_fourcc_t *spu_chromas;

    if (vlc_gl_MakeCurrent (sys->gl))
        goto error;

    sys->vgl = vout_display_opengl_New (&vd->fmt, &spu_chromas, sys->gl,
                                        &vd->cfg->viewpoint);
    vlc_gl_ReleaseCurrent (sys->gl);

    if (sys->vgl == NULL)
        goto error;

    vd->sys = sys;
#if defined(__ANDROID__)
    if (surface->handle.anativewindow)
    {
        sys->awh = surface->handle.anativewindow;
        sys->anw = AWindowHandler_getANativeWindowAPI(sys->awh);
        if (XrSubtitleEnsureSurface(vd) == VLC_SUCCESS)
            msg_Err(vd, "XR_SUB_GL_SURFACE Open subtitle surface ready");
    }
    else
        msg_Err(vd, "XR_SUB_GL_SURFACE Open missing Android AWindowHandler");
#endif
    vd->info.has_pictures_invalid = false;
    vd->info.subpicture_chromas = spu_chromas;
    vd->pool = Pool;
    vd->prepare = PictureRender;
    vd->display = PictureDisplay;
    vd->control = Control;
    return VLC_SUCCESS;

error:
    if (sys->gl != NULL)
        vlc_gl_Release (sys->gl);
    if (surface != NULL)
        vout_display_DeleteWindow (vd, surface);
    free (sys);
    return VLC_EGENERIC;
}

/**
 * Destroys the OpenGL context.
 */
static void Close (vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = vd->sys;
    vlc_gl_t *gl = sys->gl;
    vout_window_t *surface = gl->surface;

#if defined(__ANDROID__)
    if (sys->has_subpictures)
        XrSubtitleRender(vd, NULL);
    XrSubtitleReleaseResources(sys);
    XrSubtitleReleaseSurface(sys);
#endif

    vlc_gl_MakeCurrent (gl);
    vout_display_opengl_Delete (sys->vgl);
    vlc_gl_ReleaseCurrent (gl);

    vlc_gl_Release (gl);
    vout_display_DeleteWindow (vd, surface);
    free (sys);
}

/**
 * Returns picture buffers
 */
static picture_pool_t *Pool (vout_display_t *vd, unsigned count)
{
    vout_display_sys_t *sys = vd->sys;

    if (!sys->pool && vlc_gl_MakeCurrent (sys->gl) == VLC_SUCCESS)
    {
        sys->pool = vout_display_opengl_GetPool (sys->vgl, count);
        vlc_gl_ReleaseCurrent (sys->gl);
    }
    return sys->pool;
}

static void PictureRender (vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

#if defined(__ANDROID__)
    bool subtitle_surface_rendered = XrSubtitleRender(vd, subpicture);
#else
    bool subtitle_surface_rendered = false;
#endif

    if (vlc_gl_MakeCurrent (sys->gl) == VLC_SUCCESS)
    {
        vout_display_opengl_Prepare (sys->vgl, pic,
                                     subtitle_surface_rendered ? NULL : subpicture);
        vlc_gl_ReleaseCurrent (sys->gl);
    }
}

static void PictureDisplay (vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;

    if (vlc_gl_MakeCurrent (sys->gl) == VLC_SUCCESS)
    {
        vout_display_opengl_Display (sys->vgl, &vd->source);
        vlc_gl_ReleaseCurrent (sys->gl);
    }

    picture_Release (pic);
    if (subpicture != NULL)
        subpicture_Delete(subpicture);
}

static int Control (vout_display_t *vd, int query, va_list ap)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query)
    {
#ifndef NDEBUG
      case VOUT_DISPLAY_RESET_PICTURES: // not needed
        vlc_assert_unreachable();
#endif

      case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
      case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
      case VOUT_DISPLAY_CHANGE_ZOOM:
      {
        vout_display_cfg_t c = *va_arg (ap, const vout_display_cfg_t *);
        const video_format_t *src = &vd->source;
        vout_display_place_t place;

        /* Reverse vertical alignment as the GL tex are Y inverted */
        if (c.align.vertical == VOUT_DISPLAY_ALIGN_TOP)
            c.align.vertical = VOUT_DISPLAY_ALIGN_BOTTOM;
        else if (c.align.vertical == VOUT_DISPLAY_ALIGN_BOTTOM)
            c.align.vertical = VOUT_DISPLAY_ALIGN_TOP;

        vout_display_PlacePicture (&place, src, &c, false);
        vlc_gl_Resize (sys->gl, place.width, place.height);
        if (vlc_gl_MakeCurrent (sys->gl) != VLC_SUCCESS)
            return VLC_EGENERIC;
        vout_display_opengl_SetWindowAspectRatio(sys->vgl, (float)place.width / place.height);
        vout_display_opengl_Viewport(sys->vgl, place.x, place.y, place.width, place.height);
        vlc_gl_ReleaseCurrent (sys->gl);
#if defined(__ANDROID__)
        XrSubtitleInvalidate(sys);
#endif
        return VLC_SUCCESS;
      }

      case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
      case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
      {
        const vout_display_cfg_t *cfg = vd->cfg;
        vout_display_place_t place;

        vout_display_PlacePicture (&place, &vd->source, cfg, false);
        if (vlc_gl_MakeCurrent (sys->gl) != VLC_SUCCESS)
            return VLC_EGENERIC;
        vout_display_opengl_SetWindowAspectRatio(sys->vgl, (float)place.width / place.height);
        vout_display_opengl_Viewport(sys->vgl, place.x, place.y, place.width, place.height);
        vlc_gl_ReleaseCurrent (sys->gl);
#if defined(__ANDROID__)
        XrSubtitleInvalidate(sys);
#endif
        return VLC_SUCCESS;
      }
      case VOUT_DISPLAY_CHANGE_VIEWPOINT:
        return vout_display_opengl_SetViewpoint (sys->vgl,
            &va_arg (ap, const vout_display_cfg_t* )->viewpoint);
      default:
        msg_Err (vd, "Unknown request %d", query);
    }
    return VLC_EGENERIC;
}
