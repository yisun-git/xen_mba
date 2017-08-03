/*
 * Copyright (C) 2014      Intel Corporation
 * Author Dongxiao Xu <dongxiao.xu@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h" /* must come before any other headers */
#include "libxl_internal.h"

#include <xen-tools/libs.h>

#define IA32_QM_CTR_ERROR_MASK         (0x3ul << 62)

static void libxl__psr_log_err_msg(libxl__gc *gc, int err)
{
    char *msg;

    switch (err) {
    case ENOSYS:
    case EOPNOTSUPP:
        msg = "unsupported operation";
        break;
    case ESRCH:
        msg = "invalid domain ID";
        break;
    case ENOTSOCK:
        msg = "socket is not supported";
        break;
    case EFAULT:
        msg = "failed to exchange data with Xen";
        break;
    default:
        msg = "unknown error";
        break;
    }

    LOGE(ERROR, "%s", msg);
}

static void libxl__psr_cmt_log_err_msg(libxl__gc *gc, int err)
{
    char *msg;

    switch (err) {
    case ENODEV:
        msg = "CMT is not supported in this system";
        break;
    case EEXIST:
        msg = "CMT is already attached to this domain";
        break;
    case ENOENT:
        msg = "CMT is not attached to this domain";
        break;
    case EOVERFLOW:
        msg = "no free RMID available";
        break;
    default:
        libxl__psr_log_err_msg(gc, err);
        return;
    }

    LOGE(ERROR, "%s", msg);
}

static void libxl__psr_alloc_log_err_msg(libxl__gc *gc,
                                         int err,
                                         libxl_psr_cbm_type type)
{
    /*
     * Index is 'libxl_psr_cbm_type' so we set two 'CDP' to correspond to
     * DATA and CODE.
     */
    const char * const feat_name[6] = {
        "UNKNOWN",
        "L3 CAT",
        "CDP",
        "CDP",
        "L2 CAT",
        "MBA",
    };
    char *msg;

    switch (err) {
    case ENODEV:
        msg = "is not supported in this system";
        break;
    case ENOENT:
        msg = "is not enabled on the socket";
        break;
    case EOVERFLOW:
        msg = "no free COS available";
        break;
    case EEXIST:
        msg = "The same CBM is already set to this domain";
        break;
    case ENXIO:
        msg = "Unable to set code or data CBM when CDP is disabled";
        break;
    case EINVAL:
        msg = "Invalid input or some internal values are not expected";
        break;
    case ERANGE:
        msg = "Socket number is wrong";
        break;
    case ENOSPC:
        msg = "Value array exceeds the range";
        break;

    default:
        libxl__psr_log_err_msg(gc, err);
        return;
    }

    LOGE(ERROR, "%s: %s", feat_name[type], msg);
}

static int libxl__pick_socket_cpu(libxl__gc *gc, uint32_t socketid)
{
    int i, nr_cpus;
    libxl_cputopology *topology;
    int cpu = ERROR_FAIL;

    topology = libxl_get_cpu_topology(CTX, &nr_cpus);
    if (!topology)
        return ERROR_FAIL;

    for (i = 0; i < nr_cpus; i++)
        if (topology[i].socket == socketid) {
            cpu = i;
            break;
        }

    libxl_cputopology_list_free(topology, nr_cpus);
    return cpu;
}

int libxl_psr_cmt_attach(libxl_ctx *ctx, uint32_t domid)
{
    GC_INIT(ctx);
    int rc;

    rc = xc_psr_cmt_attach(ctx->xch, domid);
    if (rc < 0) {
        libxl__psr_cmt_log_err_msg(gc, errno);
        rc = ERROR_FAIL;
    }

    GC_FREE;
    return rc;
}

int libxl_psr_cmt_detach(libxl_ctx *ctx, uint32_t domid)
{
    GC_INIT(ctx);
    int rc;

    rc = xc_psr_cmt_detach(ctx->xch, domid);
    if (rc < 0) {
        libxl__psr_cmt_log_err_msg(gc, errno);
        rc = ERROR_FAIL;
    }

    GC_FREE;
    return rc;
}

int libxl_psr_cmt_domain_attached(libxl_ctx *ctx, uint32_t domid)
{
    int rc;
    uint32_t rmid;

    rc = xc_psr_cmt_get_domain_rmid(ctx->xch, domid, &rmid);
    if (rc < 0)
        return 0;

    return !!rmid;
}

int libxl_psr_cmt_enabled(libxl_ctx *ctx)
{
    return xc_psr_cmt_enabled(ctx->xch);
}

int libxl_psr_cmt_get_total_rmid(libxl_ctx *ctx, uint32_t *total_rmid)
{
    GC_INIT(ctx);
    int rc;

    rc = xc_psr_cmt_get_total_rmid(ctx->xch, total_rmid);
    if (rc < 0) {
        libxl__psr_cmt_log_err_msg(gc, errno);
        rc = ERROR_FAIL;
    }

    GC_FREE;
    return rc;
}

int libxl_psr_cmt_get_l3_cache_size(libxl_ctx *ctx,
                                    uint32_t socketid,
                                    uint32_t *l3_cache_size)
{
    GC_INIT(ctx);

    int rc;
    int cpu = libxl__pick_socket_cpu(gc, socketid);

    if (cpu < 0) {
        LOGE(ERROR, "failed to get socket cpu");
        rc = ERROR_FAIL;
        goto out;
    }

    rc = xc_psr_cmt_get_l3_cache_size(ctx->xch, cpu, l3_cache_size);
    if (rc < 0) {
        libxl__psr_cmt_log_err_msg(gc, errno);
        rc = ERROR_FAIL;
    }

out:
    GC_FREE;
    return rc;
}

int libxl_psr_cmt_type_supported(libxl_ctx *ctx, libxl_psr_cmt_type type)
{
    GC_INIT(ctx);
    uint32_t event_mask;
    int rc;

    rc = xc_psr_cmt_get_l3_event_mask(ctx->xch, &event_mask);
    if (rc < 0) {
        libxl__psr_cmt_log_err_msg(gc, errno);
        rc = 0;
    } else {
        rc = event_mask & (1 << (type - 1));
    }

    GC_FREE;
    return rc;
}

int libxl_psr_cmt_get_sample(libxl_ctx *ctx,
                             uint32_t domid,
                             libxl_psr_cmt_type type,
                             uint64_t scope,
                             uint64_t *sample_r,
                             uint64_t *tsc_r)
{
    GC_INIT(ctx);
    unsigned int rmid;
    uint32_t upscaling_factor;
    uint64_t monitor_data;
    int cpu, rc;

    rc = xc_psr_cmt_get_domain_rmid(ctx->xch, domid, &rmid);
    if (rc < 0 || rmid == 0) {
        LOGED(ERROR, domid, "fail to get the domain rmid, "
              "or domain is not attached with platform QoS monitoring service");
        rc = ERROR_FAIL;
        goto out;
    }

    cpu = libxl__pick_socket_cpu(gc, scope);
    if (cpu < 0) {
        LOGED(ERROR, domid, "failed to get socket cpu");
        rc = ERROR_FAIL;
        goto out;
    }

    rc = xc_psr_cmt_get_data(ctx->xch, rmid, cpu, type - 1,
                             &monitor_data, tsc_r);
    if (rc < 0) {
        LOGED(ERROR, domid, "failed to get monitoring data");
        rc = ERROR_FAIL;
        goto out;
    }

    rc = xc_psr_cmt_get_l3_upscaling_factor(ctx->xch, &upscaling_factor);
    if (rc < 0) {
        LOGED(ERROR, domid, "failed to get L3 upscaling factor");
        rc = ERROR_FAIL;
        goto out;
    }

    *sample_r = monitor_data * upscaling_factor;
out:
    GC_FREE;
    return rc;
}

int libxl_psr_cmt_get_cache_occupancy(libxl_ctx *ctx,
                                      uint32_t domid,
                                      uint32_t socketid,
                                      uint32_t *l3_cache_occupancy)
{
    uint64_t data;
    int rc;

    rc = libxl_psr_cmt_get_sample(ctx, domid,
                                  LIBXL_PSR_CMT_TYPE_CACHE_OCCUPANCY,
                                  socketid, &data, NULL);
    if (rc < 0)
        goto out;

    *l3_cache_occupancy = data / 1024;
out:
    return rc;
}

static inline xc_psr_val_type libxl__psr_cbm_type_to_libxc_psr_val_type(
    libxl_psr_cbm_type type)
{
    BUILD_BUG_ON(sizeof(libxl_psr_cbm_type) != sizeof(xc_psr_val_type));
    return (xc_psr_val_type)type;
}

int libxl_psr_cat_set_cbm(libxl_ctx *ctx, uint32_t domid,
                          libxl_psr_cbm_type type, libxl_bitmap *target_map,
                          uint64_t cbm)
{
    GC_INIT(ctx);
    int rc;
    int socketid, nr_sockets;

    rc = libxl__count_physical_sockets(gc, &nr_sockets);
    if (rc) {
        LOGED(ERROR, domid, "failed to get system socket count");
        goto out;
    }

    libxl_for_each_set_bit(socketid, *target_map) {
        xc_psr_val_type xc_type;

        if (socketid >= nr_sockets)
            break;

        xc_type = libxl__psr_cbm_type_to_libxc_psr_val_type(type);
        if (xc_psr_cat_set_domain_data(ctx->xch, domid, xc_type,
                                       socketid, cbm)) {
            libxl__psr_alloc_log_err_msg(gc, errno, type);
            rc = ERROR_FAIL;
        }
    }

out:
    GC_FREE;
    return rc;
}

int libxl_psr_cat_get_cbm(libxl_ctx *ctx, uint32_t domid,
                          libxl_psr_cbm_type type, uint32_t target,
                          uint64_t *cbm_r)
{
    return libxl_psr_get_val(ctx, domid, type, target, cbm_r);
}

static inline int libxl__psr_hw_info_to_libxl_psr_cat_info(
                      libxl_psr_feat_type type, libxl_psr_hw_info *hw_info,
                      libxl_psr_cat_info *cat_info)
{
    if (type != LIBXL_PSR_FEAT_TYPE_CAT_INFO)
        return ERROR_INVAL;

    cat_info->id = hw_info->id;
    cat_info->cos_max = hw_info->u.cat_info.cos_max;
    cat_info->cbm_len = hw_info->u.cat_info.cbm_len;
    cat_info->cdp_enabled = hw_info->u.cat_info.cdp_enabled;

    return 0;
}

int libxl_psr_cat_get_info(libxl_ctx *ctx, libxl_psr_cat_info **info,
                           int *nr, unsigned int lvl)
{
    GC_INIT(ctx);
    int rc;
    unsigned int i;
    libxl_psr_hw_info *hw_info;
    libxl_psr_cat_info *ptr;

    rc = libxl_psr_get_hw_info(ctx, &hw_info, (unsigned int *)nr,
                               LIBXL_PSR_FEAT_TYPE_CAT_INFO, lvl);
    if (rc)
        goto out;

    ptr = libxl__malloc(NOGC, *nr * sizeof(libxl_psr_cat_info));

    for (i = 0; i < *nr; i++) {
        if (libxl__psr_hw_info_to_libxl_psr_cat_info(
                    LIBXL_PSR_FEAT_TYPE_CAT_INFO,
                    &hw_info[i], &ptr[i])) {
            rc = ERROR_FAIL;
            free(ptr);
            goto out;
        }
    }

    *info = ptr;
out:
    GC_FREE;
    return rc;
}

int libxl_psr_cat_get_l3_info(libxl_ctx *ctx, libxl_psr_cat_info **info,
                              int *nr)
{
    int rc;

    rc = libxl_psr_cat_get_info(ctx, info, nr, 3);

    return rc;
}

void libxl_psr_cat_info_list_free(libxl_psr_cat_info *list, int nr)
{
    int i;

    for (i = 0; i < nr; i++)
        libxl_psr_cat_info_dispose(&list[i]);
    free(list);
}

int libxl_psr_set_val(libxl_ctx *ctx, uint32_t domid,
                      libxl_psr_cbm_type type, libxl_bitmap *target_map,
                      uint64_t val)
{
    return ERROR_FAIL;
}

int libxl_psr_get_val(libxl_ctx *ctx, uint32_t domid,
                      libxl_psr_cbm_type type, unsigned int target,
                      uint64_t *val)
{
    GC_INIT(ctx);
    int rc = 0;

    xc_psr_val_type xc_type = libxl__psr_cbm_type_to_libxc_psr_val_type(type);

    if (xc_psr_get_domain_data(ctx->xch, domid, xc_type,
                               target, val)) {
        libxl__psr_alloc_log_err_msg(gc, errno, type);
        rc = ERROR_FAIL;
    }

    GC_FREE;
    return rc;
}

static inline xc_psr_feat_type libxl__psr_feat_type_to_libxc_psr_feat_type(
                                   libxl_psr_feat_type type, unsigned int lvl)
{
    xc_psr_feat_type xc_type = XC_PSR_FEAT_UNKNOWN;

    switch (type) {
    case LIBXL_PSR_FEAT_TYPE_CAT_INFO:
        if (lvl == 3)
            xc_type = XC_PSR_FEAT_CAT_L3;
        if (lvl == 2)
            xc_type = XC_PSR_FEAT_CAT_L2;
        break;
    case LIBXL_PSR_FEAT_TYPE_MBA_INFO:
        xc_type = XC_PSR_FEAT_MBA;
    default:
        break;
    }

    return xc_type;
}

static inline int libxc__psr_hw_info_to_libxl_psr_hw_info(
                      libxl_psr_feat_type type, xc_psr_hw_info *xc_hw_info,
                      libxl_psr_hw_info *xl_hw_info)
{
    switch (type) {
    case LIBXL_PSR_FEAT_TYPE_CAT_INFO:
        xl_hw_info->u.cat_info.cos_max = xc_hw_info->u.xc_cat_info.cos_max;
        xl_hw_info->u.cat_info.cbm_len = xc_hw_info->u.xc_cat_info.cbm_len;
        xl_hw_info->u.cat_info.cdp_enabled =
                                        xc_hw_info->u.xc_cat_info.cdp_enabled;
        break;
    case LIBXL_PSR_FEAT_TYPE_MBA_INFO:
        xl_hw_info->u.mba_info.cos_max = xc_hw_info->u.xc_mba_info.cos_max;
        xl_hw_info->u.mba_info.thrtl_max = xc_hw_info->u.xc_mba_info.thrtl_max;
        xl_hw_info->u.mba_info.linear = xc_hw_info->u.xc_mba_info.linear;
        break;
    default:
        return ERROR_INVAL;
    }

    return 0;
}

int libxl_psr_get_hw_info(libxl_ctx *ctx, libxl_psr_hw_info **info,
                          unsigned int *nr, libxl_psr_feat_type type,
                          unsigned int lvl)
{
    GC_INIT(ctx);
    int rc, nr_sockets;
    unsigned int i = 0, socketid;
    libxl_bitmap socketmap;
    libxl_psr_hw_info *ptr;
    xc_psr_feat_type xc_type;
    xc_psr_hw_info hw_info;

    libxl_bitmap_init(&socketmap);

    if (type == LIBXL_PSR_FEAT_TYPE_CAT_INFO && lvl != 3 && lvl != 2) {
        LOGE(ERROR, "input lvl %d is wrong!\n", lvl);
        rc = ERROR_FAIL;
        goto out;
    }

    xc_type = libxl__psr_feat_type_to_libxc_psr_feat_type(type, lvl);

    rc = libxl__count_physical_sockets(gc, &nr_sockets);
    if (rc) {
        LOGE(ERROR, "failed to get system socket count");
        goto out;
    }

    libxl_socket_bitmap_alloc(ctx, &socketmap, nr_sockets);
    rc = libxl_get_online_socketmap(ctx, &socketmap);
    if (rc < 0) {
        LOGE(ERROR, "failed to get available sockets");
        goto out;
    }

    ptr = libxl__malloc(NOGC, nr_sockets * sizeof(libxl_psr_hw_info));

    libxl_for_each_set_bit(socketid, socketmap) {
        ptr[i].id = socketid;
        if (xc_psr_get_hw_info(ctx->xch, socketid, xc_type, &hw_info)) {
            rc = ERROR_FAIL;
            free(ptr);
            goto out;
        }

        if (libxc__psr_hw_info_to_libxl_psr_hw_info(type, &hw_info, &ptr[i])) {
            LOGE(ERROR, "Input type %d is wrong!\n", type);
            rc = ERROR_FAIL;
            free(ptr);
            goto out;
        }

        i++;
    }

    *info = ptr;
    *nr = i;
out:
    libxl_bitmap_dispose(&socketmap);
    GC_FREE;
    return rc;
}

void libxl_psr_hw_info_list_free(libxl_psr_hw_info *list, unsigned int nr)
{
    unsigned int i;

    for (i = 0; i < nr; i++)
        libxl_psr_hw_info_dispose(&list[i]);
    free(list);
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
