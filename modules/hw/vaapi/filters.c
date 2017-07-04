/*****************************************************************************
 * filters.c: VAAPI filters
 *****************************************************************************
 * Copyright (C) 2017 VLC authors, VideoLAN and VideoLabs
 *
 * Author: Victorien Le Couviour--Tuffet <victorien.lecouviour.tuffet@gmail.com>
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

#include <assert.h>

#include <vlc_common.h>
#include <vlc_atomic.h>
#include <vlc_filter.h>
#include <vlc_plugin.h>
#include "filters.h"

/********************************
 * Common structures and macros *
 ********************************/

struct  va_filter_desc
{
    struct vlc_vaapi_instance *inst;
    VADisplay           dpy;
    VAConfigID          conf;
    VAContextID         ctx;
    VABufferID          buf;
    VASurfaceID *       surface_ids;
};

struct  filter_sys_t
{
    struct va_filter_desc       va;
    picture_pool_t *            dest_pics;
    bool                        b_pipeline_fast;
    void *                      p_data;
};

#define DEST_PICS_POOL_SZ       3

struct  range
{
    float       min_value;
    float       max_value;
};

#define GET_DRV_SIGMA(vlc_sigma, vlc_range, drv_range)                  \
    ((vlc_sigma - (vlc_range).min_value) *                              \
     ((drv_range).max_value - (drv_range).min_value) /                  \
     ((vlc_range).max_value - (vlc_range).min_value) + (drv_range).min_value)

/***********************************
 * Adjust structures and constants *
 ***********************************/

enum { ADJUST_CONT, ADJUST_LUM, ADJUST_HUE, ADJUST_SAT, NUM_ADJUST_MODES };

static VAProcColorBalanceType const     va_adjust_modes[NUM_ADJUST_MODES] =
{
    [ADJUST_CONT] = VAProcColorBalanceContrast,
    [ADJUST_LUM] = VAProcColorBalanceBrightness,
    [ADJUST_HUE] = VAProcColorBalanceHue,
    [ADJUST_SAT] = VAProcColorBalanceSaturation
};

static char const       adjust_params_names[NUM_ADJUST_MODES][11] =
{
    [ADJUST_CONT] = "contrast",
    [ADJUST_LUM] = "brightness",
    [ADJUST_HUE] = "hue",
    [ADJUST_SAT] = "saturation"
};

static struct range const       vlc_adjust_sigma_ranges[NUM_ADJUST_MODES] =
{
    [ADJUST_CONT] = { .0f,    2.f    },
    [ADJUST_LUM]  = { .0f,    2.f    },
    [ADJUST_HUE]  = { -180.f, +180.f },
    [ADJUST_SAT]  = { .0f,    3.f    }
};

struct  adjust_params
{
    struct
    {
        vlc_atomic_float        drv_value;
        VAProcFilterValueRange  drv_range;
        bool                    is_available;
    } sigma[NUM_ADJUST_MODES];
};

struct  adjust_data
{
    struct adjust_params  params;
    unsigned int          num_available_modes;
};

/* Adaptation of VAAPI adjust filter so it looks more like the CPU one */
static float
adapt_adjust_sigma(char const * psz_var, float const sigma,
                   struct range const * p_range)
{
    if (!strcmp(psz_var, "contrast"))
    {
        struct range const      adapt_range = { .0f, .35f };
        return GET_DRV_SIGMA(sigma, *p_range, adapt_range);
    }
    else if (!strcmp(psz_var, "saturation"))
    {
        struct range const      adapt_range = { .0f, 1.f };
        return GET_DRV_SIGMA(sigma, *p_range, adapt_range);
    }
    return sigma;
}

/*****************************************
 * Basic filter structures and constants *
 *****************************************/

static struct range const       vlc_denoise_sigma_range = { .0f, 2.f };
static struct range const       vlc_sharpen_sigma_range = { .0f, 2.f };

struct  basic_filter_data
{
    struct
    {
        vlc_atomic_float        drv_value;
        VAProcFilterValueRange  drv_range;
        struct range const *    p_vlc_range;
        char *                  psz_name;
    } sigma;

    VAProcFilterType    filter_type;
};

/****************************************
 * Deinterlace structures and constants *
 ****************************************/

struct  deint_mode
{
    char const                  name[5];
    VAProcDeinterlacingType     type;
};

static struct deint_mode const  deint_modes[] =
{
    { "x",     VAProcDeinterlacingMotionAdaptive },
    { "x",     VAProcDeinterlacingMotionCompensated },
    { "bob",   VAProcDeinterlacingBob },
    { "mean",  VAProcDeinterlacingWeave }
};

struct  deint_data
{
    struct
    {
        picture_t **        pp_pics;
        picture_t **        pp_cur_pic;
        unsigned int        num_pics;
        unsigned int        sz;
    } history;

    struct
    {
        VASurfaceID *   surfaces;
        unsigned int    sz;
    } backward_refs, forward_refs;
};

/********************
 * Common functions *
 ********************/

static picture_t *
Filter(filter_t * filter, picture_t * src,
       void (*pf_update_va_filter_params)(void *, void *),
       void (*pf_prepare_render_surface)(void *),
       void (*pf_update_pipeline_params)(void *,
                                         VAProcPipelineParameterBuffer *))
{
    filter_sys_t *const filter_sys = filter->p_sys;
    picture_t *const    dest = picture_pool_Wait(filter_sys->dest_pics);
    if (!dest)
        return NULL;

    vlc_vaapi_PicAttachContext(dest);
    picture_CopyProperties(dest, src);

    void *      p_va_params;

    if (vlc_vaapi_MapBuffer(VLC_OBJECT(filter), filter_sys->va.dpy,
                            filter_sys->va.buf, &p_va_params))
        goto error;

    if (pf_update_va_filter_params)
        pf_update_va_filter_params(filter_sys->p_data, p_va_params);

    if (vlc_vaapi_UnmapBuffer(VLC_OBJECT(filter),
                              filter_sys->va.dpy, filter_sys->va.buf))
        goto error;

    if (vlc_vaapi_BeginPicture(VLC_OBJECT(filter),
                               filter_sys->va.dpy, filter_sys->va.ctx,
                               vlc_vaapi_PicGetSurface(dest)))
        goto error;

    if (pf_prepare_render_surface)
        pf_prepare_render_surface(filter_sys->p_data);

    VABufferID                          pipeline_buf = VA_INVALID_ID;
    VAProcPipelineParameterBuffer *     pipeline_params;

    pipeline_buf =
        vlc_vaapi_CreateBuffer(VLC_OBJECT(filter),
                               filter_sys->va.dpy, filter_sys->va.ctx,
                               VAProcPipelineParameterBufferType,
                               sizeof(*pipeline_params), 1, NULL);
    if (pipeline_buf == VA_INVALID_ID)
        goto error;

    if (vlc_vaapi_MapBuffer(VLC_OBJECT(filter), filter_sys->va.dpy,
                            pipeline_buf, (void **)&pipeline_params))
        goto error;

    *pipeline_params = (typeof(*pipeline_params)){0};
    pipeline_params->surface = vlc_vaapi_PicGetSurface(src);
    pipeline_params->filters = &filter_sys->va.buf;
    pipeline_params->num_filters = 1;
    if (filter_sys->b_pipeline_fast)
        pipeline_params->pipeline_flags = VA_PROC_PIPELINE_FAST;
    if (pf_update_pipeline_params)
        pf_update_pipeline_params(filter_sys->p_data, pipeline_params);

    if (vlc_vaapi_UnmapBuffer(VLC_OBJECT(filter),
                              filter_sys->va.dpy, pipeline_buf))
        goto error;

    if (vlc_vaapi_RenderPicture(VLC_OBJECT(filter),
                                filter_sys->va.dpy, filter_sys->va.ctx,
                                &pipeline_buf, 1))
        goto error;

    if (vlc_vaapi_EndPicture(VLC_OBJECT(filter),
                             filter_sys->va.dpy, filter_sys->va.ctx))
        goto error;

    return dest;

error:
    if (pipeline_buf != VA_INVALID_ID)
        vlc_vaapi_DestroyBuffer(VLC_OBJECT(filter),
                                filter_sys->va.dpy, pipeline_buf);
    if (dest)
        picture_Release(dest);
    return NULL;
}

static int
Open(filter_t * filter,
     VAProcFilterType const filter_type,
     VAProcPipelineCaps * p_pipeline_caps,
     void * p_data,
     int (*pf_init_filter_params)(filter_t *, void *, void **,
                                  unsigned int *, unsigned int *),
     int (*pf_use_pipeline_caps)(void *, VAProcPipelineCaps const *))
{
    filter_sys_t *      filter_sys;

    if (filter->fmt_out.video.i_chroma != VLC_CODEC_VAAPI_420 ||
        !video_format_IsSimilar(&filter->fmt_out.video, &filter->fmt_in.video))
        return VLC_EGENERIC;

    filter_sys = calloc(1, sizeof(*filter_sys));
    if (!filter_sys)
        return VLC_ENOMEM;
    filter->p_sys = filter_sys;

    filter_sys->p_data = p_data;

    filter_sys->va.conf = VA_INVALID_ID;
    filter_sys->va.ctx = VA_INVALID_ID;
    filter_sys->va.buf = VA_INVALID_ID;
    filter_sys->va.inst =
        vlc_vaapi_FilterHoldInstance(filter, &filter_sys->va.dpy);
    if (!filter_sys->va.inst)
        goto error;

    filter_sys->dest_pics =
        vlc_vaapi_PoolNew(VLC_OBJECT(filter), filter_sys->va.inst,
                          filter_sys->va.dpy, DEST_PICS_POOL_SZ,
                          &filter_sys->va.surface_ids, &filter->fmt_out.video,
                          VA_RT_FORMAT_YUV420, VA_FOURCC_NV12);
    if (!filter_sys->dest_pics)
        goto error;

    filter_sys->va.conf =
        vlc_vaapi_CreateConfigChecked(VLC_OBJECT(filter), filter_sys->va.dpy,
                                      VAProfileNone, VAEntrypointVideoProc,
                                      VA_FOURCC_NV12);
    if (filter_sys->va.conf == VA_INVALID_ID)
        goto error;

    filter_sys->va.ctx =
        vlc_vaapi_CreateContext(VLC_OBJECT(filter),
                                filter_sys->va.dpy, filter_sys->va.conf,
                                filter->fmt_out.video.i_width,
                                filter->fmt_out.video.i_height,
                                0, filter_sys->va.surface_ids,
                                DEST_PICS_POOL_SZ);
    if (filter_sys->va.ctx == VA_INVALID_ID)
        goto error;

    if (vlc_vaapi_IsVideoProcFilterAvailable(VLC_OBJECT(filter),
                                             filter_sys->va.dpy,
                                             filter_sys->va.ctx,
                                             filter_type))
        goto error;

    void *      p_va_params;
    uint32_t    i_sz_param;
    uint32_t    i_num_params;

    if (pf_init_filter_params(filter, p_data,
                              &p_va_params, &i_sz_param, &i_num_params))
        goto error;

    filter_sys->va.buf =
        vlc_vaapi_CreateBuffer(VLC_OBJECT(filter),
                               filter_sys->va.dpy, filter_sys->va.ctx,
                               VAProcFilterParameterBufferType,
                               i_sz_param, i_num_params, p_va_params);
    free(p_va_params);
    if (filter_sys->va.buf == VA_INVALID_ID)
        goto error;

    if (vlc_vaapi_QueryVideoProcPipelineCaps(VLC_OBJECT(filter),
                                             filter_sys->va.dpy,
                                             filter_sys->va.ctx,
                                             &filter_sys->va.buf,
                                             1, p_pipeline_caps))
        goto error;

    filter_sys->b_pipeline_fast =
        p_pipeline_caps->pipeline_flags & VA_PROC_PIPELINE_FAST;

    if (pf_use_pipeline_caps &&
        pf_use_pipeline_caps(p_data, p_pipeline_caps))
        goto error;

    return VLC_SUCCESS;

error:
    if (filter_sys->va.buf != VA_INVALID_ID)
        vlc_vaapi_DestroyBuffer(VLC_OBJECT(filter),
                                filter_sys->va.dpy, filter_sys->va.buf);
    if (filter_sys->va.ctx != VA_INVALID_ID)
        vlc_vaapi_DestroyContext(VLC_OBJECT(filter),
                                 filter_sys->va.dpy, filter_sys->va.ctx);
    if (filter_sys->va.conf != VA_INVALID_ID)
        vlc_vaapi_DestroyConfig(VLC_OBJECT(filter),
                                filter_sys->va.dpy, filter_sys->va.conf);
    if (filter_sys->va.inst)
        vlc_vaapi_ReleaseInstance(filter_sys->va.inst);
    free(filter_sys);
    return VLC_EGENERIC;
}

static void
Close(vlc_object_t * obj, filter_sys_t * filter_sys)
{
    picture_pool_Release(filter_sys->dest_pics);
    vlc_vaapi_DestroyBuffer(obj, filter_sys->va.dpy, filter_sys->va.buf);
    vlc_vaapi_DestroyContext(obj, filter_sys->va.dpy, filter_sys->va.ctx);
    vlc_vaapi_DestroyConfig(obj, filter_sys->va.dpy, filter_sys->va.conf);
    vlc_vaapi_ReleaseInstance(filter_sys->va.inst);
    free(filter_sys);
}

static int
FilterCallback(vlc_object_t * obj, char const * psz_var,
               vlc_value_t oldval, vlc_value_t newval,
               void * p_data)
{ VLC_UNUSED(obj); VLC_UNUSED(oldval);
    struct range const *                p_vlc_range;
    VAProcFilterValueRange const *      p_drv_range;
    vlc_atomic_float *                  p_drv_value;
    bool                                b_found = false;
    bool                                b_adjust = false;

    for (unsigned int i = 0; i < NUM_ADJUST_MODES; ++i)
        if (!strcmp(psz_var, adjust_params_names[i]))
        {
            struct adjust_data *const   p_adjust_data = p_data;

            if (!p_adjust_data->params.sigma[i].is_available)
                return VLC_EGENERIC;

            p_vlc_range = vlc_adjust_sigma_ranges + i;
            p_drv_range = &p_adjust_data->params.sigma[i].drv_range;
            p_drv_value = &p_adjust_data->params.sigma[i].drv_value;

            b_adjust = true;
            b_found = true;
        }
    if (!b_found)
        if (!strcmp(psz_var, "denoise-sigma") ||
            !strcmp(psz_var, "sharpen-sigma"))
        {
            struct basic_filter_data *const     p_basic_filter_data = p_data;

            p_vlc_range = p_basic_filter_data->sigma.p_vlc_range;
            p_drv_range = &p_basic_filter_data->sigma.drv_range;
            p_drv_value = &p_basic_filter_data->sigma.drv_value;

            b_found = true;
        }

    if (!b_found)
        return VLC_EGENERIC;

    float       vlc_sigma = VLC_CLIP(newval.f_float,
                                     p_vlc_range->min_value,
                                     p_vlc_range->max_value);

    if (b_adjust)
        vlc_sigma = adapt_adjust_sigma(psz_var, vlc_sigma, p_vlc_range);

    float const drv_sigma = GET_DRV_SIGMA(vlc_sigma,
                                          *p_vlc_range, *p_drv_range);

    vlc_atomic_store_float(p_drv_value, drv_sigma);

    return VLC_SUCCESS;
}

/********************
 * Adjust functions *
 ********************/

static void
Adjust_UpdateVAFilterParams(void * p_data, void * va_params)
{
    struct adjust_data *const   p_adjust_data = p_data;
    struct adjust_params *const p_adjust_params = &p_adjust_data->params;
    VAProcFilterParameterBufferColorBalance *const      p_va_params = va_params;

    unsigned int i = 0;
    for (unsigned int j = 0; j < NUM_ADJUST_MODES; ++j)
        if (p_adjust_params->sigma[j].is_available)
            p_va_params[i++].value =
                vlc_atomic_load_float(&p_adjust_params->sigma[j].drv_value);
}

static picture_t *
Adjust(filter_t * filter, picture_t * src)
{
    picture_t *const    dest =
        Filter(filter, src, Adjust_UpdateVAFilterParams, NULL, NULL);
    picture_Release(src);
    return dest;
}

static int
OpenAdjust_InitFilterParams(filter_t * filter, void * p_data,
                            void ** pp_va_params,
                            uint32_t * p_va_param_sz,
                            uint32_t * p_num_va_params)
{
    struct adjust_data *const   p_adjust_data = p_data;
    struct adjust_params *const p_adjust_params = &p_adjust_data->params;
    filter_sys_t *const         filter_sys = filter->p_sys;
    VAProcFilterCapColorBalance caps[VAProcColorBalanceCount];
    unsigned int                num_caps = VAProcColorBalanceCount;

    if (vlc_vaapi_QueryVideoProcFilterCaps(VLC_OBJECT(filter),
                                           filter_sys->va.dpy,
                                           filter_sys->va.ctx,
                                           VAProcFilterColorBalance,
                                           caps, &num_caps))
        return VLC_EGENERIC;

    for (unsigned int i = 0; i < num_caps; ++i)
    {
        unsigned int    j;

        for (j = 0; j < num_caps; ++j)
            if (caps[j].type == va_adjust_modes[i])
            {
                float   vlc_sigma =
                    VLC_CLIP(var_InheritFloat(filter, adjust_params_names[i]),
                             vlc_adjust_sigma_ranges[i].min_value,
                             vlc_adjust_sigma_ranges[i].max_value);
                vlc_sigma =
                    adapt_adjust_sigma(adjust_params_names[i],
                                       vlc_sigma, vlc_adjust_sigma_ranges + i);

                p_adjust_params->sigma[i].drv_range = caps[j].range;
                p_adjust_params->sigma[i].is_available = true;
                ++p_adjust_data->num_available_modes;

                float const     drv_sigma =
                    GET_DRV_SIGMA(vlc_sigma, vlc_adjust_sigma_ranges[i],
                                  p_adjust_params->sigma[i].drv_range);

                vlc_atomic_init_float(&p_adjust_params->sigma[i].drv_value,
                                      drv_sigma);
                break;
            }
    }

    VAProcFilterParameterBufferColorBalance *   p_va_params;

    *p_va_param_sz = sizeof(typeof(*p_va_params));
    *p_num_va_params = p_adjust_data->num_available_modes;

    p_va_params = calloc(*p_num_va_params, *p_va_param_sz);
    if (!p_va_params)
        return VLC_ENOMEM;

    unsigned int i = 0;
    for (unsigned int j = 0; j < NUM_ADJUST_MODES; ++j)
        if (p_adjust_params->sigma[j].is_available)
        {
            p_va_params[i].type = VAProcFilterColorBalance;
            p_va_params[i++].attrib = va_adjust_modes[j];
        }

    *pp_va_params = p_va_params;

    return VLC_SUCCESS;
}

static int
OpenAdjust(vlc_object_t * obj)
{
    VAProcPipelineCaps          pipeline_caps;
    filter_t *const             filter = (filter_t *)obj;
    struct adjust_data *const   p_data = calloc(1, sizeof(*p_data));
    if (!p_data)
        return VLC_ENOMEM;

    for (unsigned int i = 0; i < NUM_ADJUST_MODES; ++i)
        var_Create(obj, adjust_params_names[i],
                   VLC_VAR_FLOAT | VLC_VAR_DOINHERIT | VLC_VAR_ISCOMMAND);

    if (Open(filter, VAProcFilterColorBalance, &pipeline_caps, p_data,
             OpenAdjust_InitFilterParams, NULL))
        goto error;

    for (unsigned int i = 0; i < NUM_ADJUST_MODES; ++i)
        var_AddCallback(obj, adjust_params_names[i], FilterCallback, p_data);

    filter->pf_video_filter = Adjust;

    return VLC_SUCCESS;

error:
    for (unsigned int i = 0; i < NUM_ADJUST_MODES; ++i)
        var_Destroy(obj, adjust_params_names[i]);
    free(p_data);
    return VLC_EGENERIC;
}

static void
CloseAdjust(vlc_object_t * obj)
{
    filter_t *const     filter = (filter_t *)obj;
    filter_sys_t *const filter_sys = filter->p_sys;

    for (unsigned int i = 0; i < NUM_ADJUST_MODES; ++i)
    {
        var_DelCallback(obj, adjust_params_names[i],
                        FilterCallback, filter_sys->p_data);
        var_Destroy(obj, adjust_params_names[i]);
    }
    free(filter_sys->p_data);
    Close(obj, filter_sys);
}

/***************************
 * Basic filters functions *
 ***************************/

static void
BasicFilter_UpdateVAFilterParams(void * p_data, void * va_params)
{
    struct basic_filter_data *const     p_basic_filter_data = p_data;
    VAProcFilterParameterBuffer *const  p_va_param = va_params;

    p_va_param->value =
        vlc_atomic_load_float(&p_basic_filter_data->sigma.drv_value);
}

static picture_t *
BasicFilter(filter_t * filter, picture_t * src)
{
    picture_t *const    dest =
        Filter(filter, src, BasicFilter_UpdateVAFilterParams, NULL, NULL);
    picture_Release(src);
    return dest;
}

static int
OpenBasicFilter_InitFilterParams(filter_t * filter, void * p_data,
                                 void ** pp_va_params,
                                 uint32_t * p_va_param_sz,
                                 uint32_t * p_num_va_params)
{
    struct basic_filter_data *const     p_basic_filter_data = p_data;
    filter_sys_t *const                 filter_sys = filter->p_sys;
    VAProcFilterCap                     caps;
    unsigned int                        num_caps = 1;

    if (vlc_vaapi_QueryVideoProcFilterCaps(VLC_OBJECT(filter),
                                           filter_sys->va.dpy,
                                           filter_sys->va.buf,
                                           p_basic_filter_data->filter_type,
                                           &caps, &num_caps)
        || !num_caps)
        return VLC_EGENERIC;

    float const vlc_sigma =
        VLC_CLIP(var_InheritFloat(filter, p_basic_filter_data->sigma.psz_name),
                 p_basic_filter_data->sigma.p_vlc_range->min_value,
                 p_basic_filter_data->sigma.p_vlc_range->max_value);

    p_basic_filter_data->sigma.drv_range = caps.range;

    float const drv_sigma =
        GET_DRV_SIGMA(vlc_sigma, *p_basic_filter_data->sigma.p_vlc_range,
                      p_basic_filter_data->sigma.drv_range);

    vlc_atomic_init_float(&p_basic_filter_data->sigma.drv_value, drv_sigma);

    VAProcFilterParameterBuffer *       p_va_param;

    *p_va_param_sz = sizeof(*p_va_param);
    *p_num_va_params = 1;

    p_va_param = calloc(1, sizeof(*p_va_param));
    if (!p_va_param)
        return VLC_ENOMEM;

    p_va_param->type = p_basic_filter_data->filter_type;
    *pp_va_params = p_va_param;

    return VLC_SUCCESS;
}

static int
OpenBasicFilter(vlc_object_t * obj)
{
    VAProcPipelineCaps                  pipeline_caps;
    filter_t *const                     filter = (filter_t *)obj;
    assert(filter->psz_name);
    struct basic_filter_data *const     p_data = calloc(1, sizeof(*p_data));
    if (!p_data)
        return VLC_ENOMEM;

    p_data->sigma.psz_name =
        calloc(strlen(filter->psz_name) + strlen("-sigma") + 1, sizeof(char));
    if (!p_data->sigma.psz_name)
        goto error;

    strcpy(p_data->sigma.psz_name, filter->psz_name);
    strcat(p_data->sigma.psz_name, "-sigma");

    if (!strcmp(filter->psz_name, "denoise"))
    {
        p_data->filter_type = VAProcFilterNoiseReduction;
        p_data->sigma.p_vlc_range = &vlc_denoise_sigma_range;
    }
    else if (!strcmp(filter->psz_name, "sharpen"))
    {
        p_data->filter_type = VAProcFilterSharpening;
        p_data->sigma.p_vlc_range = &vlc_sharpen_sigma_range;
    }

    var_Create(obj, p_data->sigma.psz_name,
               VLC_VAR_FLOAT | VLC_VAR_DOINHERIT | VLC_VAR_ISCOMMAND);

    if (Open(filter, p_data->filter_type, &pipeline_caps, p_data,
             OpenBasicFilter_InitFilterParams, NULL))
        goto error;

    var_AddCallback(obj, p_data->sigma.psz_name, FilterCallback, p_data);

    filter->pf_video_filter = BasicFilter;

    return VLC_SUCCESS;

error:
    var_Destroy(obj, p_data->sigma.psz_name);
    if (p_data->sigma.psz_name)
        free(p_data->sigma.psz_name);
    free(p_data);
    return VLC_EGENERIC;
}

static void
CloseBasicFilter(vlc_object_t * obj)
{
    filter_t *const                     filter = (filter_t *)obj;
    filter_sys_t *const                 filter_sys = filter->p_sys;
    struct basic_filter_data *const     p_data = filter_sys->p_data;

    var_DelCallback(obj, p_data->sigma.psz_name, FilterCallback, p_data);
    var_Destroy(obj, p_data->sigma.psz_name);
    free(p_data->sigma.psz_name);
    free(p_data);
    Close(obj, filter_sys);
}

/*************************
 * Deinterlace functions *
 *************************/

static picture_t *
Deinterlace_UpdateHistory(struct deint_data * p_deint_data, picture_t * src)
{
    if (p_deint_data->history.num_pics == p_deint_data->history.sz)
    {
        picture_Release(*p_deint_data->history.pp_pics);
        memmove(p_deint_data->history.pp_pics, p_deint_data->history.pp_pics + 1,
                --p_deint_data->history.num_pics * sizeof(picture_t *));
    }
    p_deint_data->history.pp_pics[p_deint_data->history.num_pics++] = src;

    return *p_deint_data->history.pp_cur_pic;
}

static void
Deinterlace_UpdateReferenceFrames(void * p_data)
{
    struct deint_data *const    p_deint_data = p_data;

    if (p_deint_data->backward_refs.sz)
        for (unsigned int i = 0; i < p_deint_data->backward_refs.sz; ++i)
        {
            unsigned int const  idx = p_deint_data->forward_refs.sz + 1 + i;

            p_deint_data->backward_refs.surfaces[i] =
                vlc_vaapi_PicGetSurface(p_deint_data->history.pp_pics[idx]);
        }

    if (p_deint_data->forward_refs.sz)
        for (unsigned int i = 0; i < p_deint_data->forward_refs.sz; ++i)
        {
            unsigned int const  idx = p_deint_data->forward_refs.sz - 1 - i;

            p_deint_data->forward_refs.surfaces[i] =
                vlc_vaapi_PicGetSurface(p_deint_data->history.pp_pics[idx]);
        }
}

static void
Deinterlace_UpdatePipelineParams
(void * p_data, VAProcPipelineParameterBuffer * pipeline_param)
{
    struct deint_data *const    p_deint_data = p_data;

    pipeline_param->filter_flags =
        p_deint_data->history.pp_cur_pic[0]->b_top_field_first ?
        0 : VA_DEINTERLACING_BOTTOM_FIELD_FIRST;

    pipeline_param->backward_references = p_deint_data->backward_refs.surfaces;
    pipeline_param->forward_references = p_deint_data->forward_refs.surfaces;
    pipeline_param->num_backward_references = p_deint_data->backward_refs.sz;
    pipeline_param->num_forward_references = p_deint_data->forward_refs.sz;
}

static picture_t *
Deinterlace(filter_t * filter, picture_t * src)
{
    filter_sys_t *const         filter_sys = filter->p_sys;
    struct deint_data *const    p_deint_data = filter_sys->p_data;

    src = Deinterlace_UpdateHistory(p_deint_data, src);
    if (p_deint_data->history.num_pics < p_deint_data->history.sz)
        return NULL;

    picture_t *const    dest =
        Filter(filter, src, NULL,
               Deinterlace_UpdateReferenceFrames,
               Deinterlace_UpdatePipelineParams);

    if (dest)
        dest->b_progressive = true;

    return dest;
}

static inline bool
OpenDeinterlace_IsValidType(filter_t * filter,
                            VAProcDeinterlacingType const caps[],
                            unsigned int num_caps,
                            struct deint_mode const * deint_mode)
{
    (void) filter;
    for (unsigned int j = 0; j < num_caps; ++j)
        if (caps[j] == deint_mode->type)
            return true;
    return false;
}

static inline int
OpenDeinterlace_GetMode(filter_t * filter, char const * deint_mode,
                        VAProcDeinterlacingType * p_deint_mode,
                        VAProcDeinterlacingType const caps[],
                        unsigned int num_caps)
{
    bool fallback = false;
    if (deint_mode && strcmp(deint_mode, "auto"))
    {
        for (unsigned int i = 0; i < ARRAY_SIZE(deint_modes); ++i)
        {
            if (!strcmp(deint_mode, deint_modes[i].name))
            {
                if (OpenDeinterlace_IsValidType(filter, caps, num_caps,
                                                deint_modes + i))
                {
                    *p_deint_mode = deint_modes[i].type;
                    msg_Dbg(filter, "using %s deinterlace method",
                            deint_modes[i].name);
                    return VLC_SUCCESS;
                }
            }
        }
        fallback = true;
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(deint_modes); ++i)
        if (OpenDeinterlace_IsValidType(filter, caps, num_caps,
                                        deint_modes + i))
        {
            *p_deint_mode = deint_modes[i].type;
            if (fallback)
                msg_Info(filter, "%s algorithm not available, falling back to "
                         "%s algorithm", deint_mode, deint_modes[i].name);
            else
                msg_Dbg(filter, "using %s deinterlace method",
                        deint_modes[i].name);
            return VLC_SUCCESS;
        }

    /* We shouldn't be able to reach this, 'cause if there is no deinterlacing
       algorithm available, then the driver would have told us before the
       deinterlace filtering is not supported at all. */

    msg_Err(filter, "no algorithm available");
    return VLC_EGENERIC;
}

static int
OpenDeinterlace_InitFilterParams(filter_t * filter, void * p_data,
                                 void ** pp_va_params,
                                 uint32_t * p_va_param_sz,
                                 uint32_t * p_num_va_params)
{ VLC_UNUSED(p_data);
    filter_sys_t *const         filter_sys = filter->p_sys;
    VAProcDeinterlacingType     caps[VAProcDeinterlacingCount];
    unsigned int                num_caps = VAProcDeinterlacingCount;

    if (vlc_vaapi_QueryVideoProcFilterCaps(VLC_OBJECT(filter),
                                           filter_sys->va.dpy,
                                           filter_sys->va.buf,
                                           VAProcFilterDeinterlacing,
                                           &caps, &num_caps))
        return VLC_EGENERIC;

    VAProcDeinterlacingType     va_mode;
    char *const                 psz_deint_mode =
        var_InheritString(filter, "deinterlace-mode");

    int ret = OpenDeinterlace_GetMode(filter, psz_deint_mode,
                                      &va_mode, caps, num_caps);
    free(psz_deint_mode);
    if (ret)
        return VLC_EGENERIC;

    VAProcFilterParameterBufferDeinterlacing *  p_va_param;

    *p_va_param_sz = sizeof(*p_va_param);
    *p_num_va_params = 1;

    p_va_param = calloc(1, sizeof(*p_va_param));
    if (!p_va_param)
        return VLC_ENOMEM;

    p_va_param->type = VAProcFilterDeinterlacing;
    p_va_param->algorithm = va_mode;
    *pp_va_params = p_va_param;

    return VLC_SUCCESS;
}

static int
OpenDeinterlace_InitHistory(void * p_data, VAProcPipelineCaps const * pipeline_caps)
{
    struct deint_data *const    p_deint_data = p_data;
    unsigned int const          sz_backward_refs =
        pipeline_caps->num_backward_references;
    unsigned int const          sz_forward_refs =
        pipeline_caps->num_forward_references;
    unsigned int const          history_sz =
        sz_backward_refs + 1 + sz_forward_refs;

    p_deint_data->history.pp_pics = calloc(history_sz, sizeof(picture_t *));
    if (!p_deint_data->history.pp_pics)
        return VLC_ENOMEM;

    p_deint_data->history.pp_cur_pic =
        p_deint_data->history.pp_pics + sz_forward_refs;
    p_deint_data->history.num_pics = 0;
    p_deint_data->history.sz = history_sz;

    if (history_sz - 1)
    {
        p_deint_data->forward_refs.surfaces =
            malloc((history_sz - 1) * sizeof(VASurfaceID));
        if (!p_deint_data->forward_refs.surfaces)
            return VLC_ENOMEM;
    }

    p_deint_data->backward_refs.surfaces =
        p_deint_data->forward_refs.surfaces + sz_forward_refs;

    p_deint_data->backward_refs.sz = sz_backward_refs;
    p_deint_data->forward_refs.sz = sz_forward_refs;

    return VLC_SUCCESS;
}

static int
OpenDeinterlace(vlc_object_t * obj)
{
    VAProcPipelineCaps          pipeline_caps;
    filter_t *const             filter = (filter_t *)obj;
    struct deint_data *const    p_data = calloc(1, sizeof(*p_data));
    if (!p_data)
        return VLC_ENOMEM;

    if (Open(filter, VAProcFilterDeinterlacing, &pipeline_caps, p_data,
             OpenDeinterlace_InitFilterParams, OpenDeinterlace_InitHistory))
        goto error;

    filter->pf_video_filter = Deinterlace;

    return VLC_SUCCESS;

error:
    if (p_data->forward_refs.surfaces)
        free(p_data->forward_refs.surfaces);
    if (p_data->history.pp_pics)
        free(p_data->history.pp_pics);
    free(p_data);
    return VLC_EGENERIC;
}

static void
CloseDeinterlace(vlc_object_t * obj)
{
    filter_t *const             filter = (filter_t *)obj;
    filter_sys_t *const         filter_sys = filter->p_sys;
    struct deint_data *const    p_data = filter_sys->p_data;

    if (p_data->forward_refs.surfaces)
        free(p_data->forward_refs.surfaces);
    if (p_data->history.pp_pics)
    {
        while (p_data->history.num_pics)
            picture_Release(p_data->history.pp_pics[--p_data->history.num_pics]);
        free(p_data->history.pp_pics);
    }
    free(p_data);
    Close(obj, filter_sys);
}

/*********************
 * Module descriptor *
 *********************/

vlc_module_begin()
    set_shortname(N_("VAAPI filters"))
    set_description(N_("Video Accelerated API filters"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)

    add_submodule()
    set_callbacks(OpenAdjust, CloseAdjust)
    add_shortcut("adjust")

    add_submodule()
    set_callbacks(OpenDeinterlace, CloseDeinterlace)
    add_shortcut("deinterlace")

    add_submodule()
    set_callbacks(OpenBasicFilter, CloseBasicFilter)
    add_float_with_range("denoise-sigma", 1.f, .0f, .0f,
                         "Denoise strength (0-2)",
                         "Set the Denoise strength, between 0 and 2. "
                            "Defaults to 1.",
                         false)
    add_shortcut("denoise", "sharpen")

    add_submodule()
    set_capability("video converter", 10)
    set_callbacks(vlc_vaapi_OpenChroma, vlc_vaapi_CloseChroma)
vlc_module_end()
