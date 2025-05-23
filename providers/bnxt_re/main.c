/*
 * Broadcom NetXtreme-E User Space RoCE driver
 *
 * Copyright (c) 2015-2017, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Description: Device detection and initializatoin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "main.h"
#include "verbs.h"

static void bnxt_re_free_context(struct ibv_context *ibvctx);

#define PCI_VENDOR_ID_BROADCOM		0x14E4

#define CNA(v, d) VERBS_PCI_MATCH(PCI_VENDOR_ID_##v, d, NULL)
static const struct verbs_match_ent cna_table[] = {
	VERBS_DRIVER_ID(RDMA_DRIVER_BNXT_RE),
	CNA(BROADCOM, 0x1605),  /* BCM57454 NPAR */
	CNA(BROADCOM, 0x1606),  /* BCM57454 VF */
	CNA(BROADCOM, 0x1614),  /* BCM57454 */
	CNA(BROADCOM, 0x16C0),  /* BCM57417 NPAR */
	CNA(BROADCOM, 0x16C1),  /* BMC57414 VF */
	CNA(BROADCOM, 0x16CE),  /* BMC57311 */
	CNA(BROADCOM, 0x16CF),  /* BMC57312 */
	CNA(BROADCOM, 0x16D6),  /* BMC57412*/
	CNA(BROADCOM, 0x16D7),  /* BMC57414 */
	CNA(BROADCOM, 0x16D8),  /* BMC57416 Cu */
	CNA(BROADCOM, 0x16D9),  /* BMC57417 Cu */
	CNA(BROADCOM, 0x16DF),  /* BMC57314 */
	CNA(BROADCOM, 0x16E2),  /* BMC57417 */
	CNA(BROADCOM, 0x16E3),  /* BMC57416 */
	CNA(BROADCOM, 0x16E5),  /* BMC57314 VF */
	CNA(BROADCOM, 0x16ED),  /* BCM57414 NPAR */
	CNA(BROADCOM, 0x16EB),  /* BCM57412 NPAR */
	CNA(BROADCOM, 0x16EF),  /* BCM57416 NPAR */
	CNA(BROADCOM, 0x16F0),  /* BCM58730 */
	CNA(BROADCOM, 0x16F1),  /* BCM57452 */
	CNA(BROADCOM, 0x1750),	/* BCM57508 */
	CNA(BROADCOM, 0x1751),	/* BCM57504 */
	CNA(BROADCOM, 0x1752),	/* BCM57502 */
	CNA(BROADCOM, 0x1803),	/* BCM57508 NPAR */
	CNA(BROADCOM, 0x1804),	/* BCM57504 NPAR */
	CNA(BROADCOM, 0x1805),	/* BCM57502 NPAR */
	CNA(BROADCOM, 0x1807),	/* BCM5750x VF */
	CNA(BROADCOM, 0x1809),  /* BCM5750x Gen P5 VF HV */
	CNA(BROADCOM, 0xD800),  /* BCM880xx VF */
	CNA(BROADCOM, 0xD802),  /* BCM58802 */
	CNA(BROADCOM, 0xD804),  /* BCM8804 SR */
	{}
};

static const struct verbs_context_ops bnxt_re_cntx_ops = {
	.query_device_ex = bnxt_re_query_device,
	.query_port    = bnxt_re_query_port,
	.alloc_pd      = bnxt_re_alloc_pd,
	.dealloc_pd    = bnxt_re_free_pd,
	.reg_mr        = bnxt_re_reg_mr,
	.reg_dmabuf_mr = bnxt_re_reg_dmabuf_mr,
	.dereg_mr      = bnxt_re_dereg_mr,
	.create_cq     = bnxt_re_create_cq,
	.poll_cq       = bnxt_re_poll_cq,
	.req_notify_cq = bnxt_re_arm_cq,
	.resize_cq     = bnxt_re_resize_cq,
	.destroy_cq    = bnxt_re_destroy_cq,
	.create_srq    = bnxt_re_create_srq,
	.modify_srq    = bnxt_re_modify_srq,
	.query_srq     = bnxt_re_query_srq,
	.destroy_srq   = bnxt_re_destroy_srq,
	.post_srq_recv = bnxt_re_post_srq_recv,
	.create_qp     = bnxt_re_create_qp,
	.query_qp      = bnxt_re_query_qp,
	.modify_qp     = bnxt_re_modify_qp,
	.destroy_qp    = bnxt_re_destroy_qp,
	.post_send     = bnxt_re_post_send,
	.post_recv     = bnxt_re_post_recv,
	.async_event   = bnxt_re_async_event,
	.create_ah     = bnxt_re_create_ah,
	.destroy_ah    = bnxt_re_destroy_ah,
	.free_context  = bnxt_re_free_context,

	.create_qp_ex  = bnxt_re_create_qp_ex,
};

static inline bool bnxt_re_is_chip_gen_p7(struct bnxt_re_chip_ctx *cctx)
{
	return (cctx->chip_num == CHIP_NUM_58818 ||
		cctx->chip_num == CHIP_NUM_57608);
}

static bool bnxt_re_is_chip_gen_p5(struct bnxt_re_chip_ctx *cctx)
{
	return (cctx->chip_num == CHIP_NUM_57508 ||
		cctx->chip_num == CHIP_NUM_57504 ||
		cctx->chip_num == CHIP_NUM_57502);
}

static inline bool bnxt_re_is_chip_gen_p5_p7(struct bnxt_re_chip_ctx *cctx)
{
	return bnxt_re_is_chip_gen_p5(cctx) || bnxt_re_is_chip_gen_p7(cctx);
}

static int bnxt_re_alloc_map_dbr_page(struct ibv_context *ibvctx)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_mmap_info minfo = {};
	int ret;

	minfo.type = BNXT_RE_ALLOC_DBR_PAGE;
	ret = bnxt_re_alloc_page(ibvctx, &minfo, NULL);
	if (ret)
		return ret;

	cntx->dbr_page = mmap(NULL, minfo.alloc_size, PROT_READ,
			      MAP_SHARED, ibvctx->cmd_fd, minfo.alloc_offset);
	if (cntx->dbr_page == MAP_FAILED)
		return -ENOMEM;

	return 0;
}

static int bnxt_re_alloc_map_dbr_bar_page(struct ibv_context *ibvctx)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_mmap_info minfo = {};
	int ret;

	minfo.type = BNXT_RE_ALLOC_DBR_BAR_PAGE;
	ret = bnxt_re_alloc_page(ibvctx, &minfo, NULL);
	if (ret)
		return ret;

	cntx->bar_map = mmap(NULL, minfo.alloc_size, PROT_WRITE,
			     MAP_SHARED, ibvctx->cmd_fd, minfo.alloc_offset);
	if (cntx->bar_map == MAP_FAILED)
		return -ENOMEM;

	return 0;
}

/* Context Init functions */
static struct verbs_context *bnxt_re_alloc_context(struct ibv_device *vdev,
						   int cmd_fd,
						   void *private_data)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(vdev);
	struct ubnxt_re_cntx_resp resp = {};
	struct ubnxt_re_cntx req = {};
	struct bnxt_re_context *cntx;
	int ret;

	cntx = verbs_init_and_alloc_context(vdev, cmd_fd, cntx, ibvctx,
					    RDMA_DRIVER_BNXT_RE);
	if (!cntx)
		return NULL;

	req.comp_mask |= BNXT_RE_COMP_MASK_REQ_UCNTX_POW2_SUPPORT;
	req.comp_mask |= BNXT_RE_COMP_MASK_REQ_UCNTX_VAR_WQE_SUPPORT;
	if (ibv_cmd_get_context(&cntx->ibvctx, &req.ibv_cmd, sizeof(req),
				NULL, &resp.ibv_resp, sizeof(resp)))
		goto failed;

	cntx->dev_id = resp.dev_id;
	cntx->max_qp = resp.max_qp;
	rdev->pg_size = resp.pg_size;
	rdev->cqe_size = resp.cqe_sz;
	rdev->max_cq_depth = resp.max_cqd;
	if (resp.comp_mask & BNXT_RE_UCNTX_CMASK_HAVE_CCTX) {
		cntx->cctx.chip_num = resp.chip_id0 & 0xFFFF;
		cntx->cctx.chip_rev = (resp.chip_id0 >>
				       BNXT_RE_CHIP_ID0_CHIP_REV_SFT) & 0xFF;
		cntx->cctx.chip_metal = (resp.chip_id0 >>
					 BNXT_RE_CHIP_ID0_CHIP_MET_SFT) &
					 0xFF;
		cntx->cctx.gen_p5_p7 = bnxt_re_is_chip_gen_p5_p7(&cntx->cctx);
	}

	if (resp.comp_mask & BNXT_RE_UCNTX_CMASK_HAVE_MODE)
		cntx->wqe_mode = resp.mode;
	if (resp.comp_mask & BNXT_RE_UCNTX_CMASK_WC_DPI_ENABLED)
		cntx->comp_mask |= BNXT_RE_COMP_MASK_UCNTX_WC_DPI_ENABLED;
	if (resp.comp_mask & BNXT_RE_UCNTX_CMASK_DBR_PACING_ENABLED)
		cntx->comp_mask |= BNXT_RE_COMP_MASK_UCNTX_DBR_PACING_ENABLED;
	if (resp.comp_mask & BNXT_RE_UCNTX_CMASK_POW2_DISABLED)
		cntx->comp_mask |= BNXT_RE_COMP_MASK_UCNTX_POW2_DISABLED;
	if (resp.comp_mask & BNXT_RE_UCNTX_CMASK_MSN_TABLE_ENABLED)
		cntx->comp_mask |= BNXT_RE_COMP_MASK_UCNTX_MSN_TABLE_ENABLED;

	/* mmap shared page. */
	cntx->shpg = mmap(NULL, rdev->pg_size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, cmd_fd, 0);
	if (cntx->shpg == MAP_FAILED) {
		cntx->shpg = NULL;
		goto failed;
	}

	if (cntx->comp_mask & BNXT_RE_COMP_MASK_UCNTX_DBR_PACING_ENABLED) {
		if (bnxt_re_alloc_map_dbr_page(&cntx->ibvctx.context)) {
			munmap(cntx->shpg, rdev->pg_size);
			cntx->shpg = NULL;
			goto failed;
		}

		if (bnxt_re_alloc_map_dbr_bar_page(&cntx->ibvctx.context)) {
			munmap(cntx->shpg, rdev->pg_size);
			cntx->shpg = NULL;
			munmap(cntx->dbr_page, rdev->pg_size);
			cntx->dbr_page = NULL;
			goto failed;
		}
	}

	pthread_mutex_init(&cntx->shlock, NULL);

	verbs_set_ops(&cntx->ibvctx, &bnxt_re_cntx_ops);
	cntx->rdev = rdev;
	ret = ibv_query_device(&cntx->ibvctx.context, &rdev->devattr);
	if (ret)
		goto failed;

	return &cntx->ibvctx;

failed:
	verbs_uninit_context(&cntx->ibvctx);
	free(cntx);
	return NULL;
}

static void bnxt_re_free_context(struct ibv_context *ibvctx)
{
	struct bnxt_re_context *cntx = to_bnxt_re_context(ibvctx);
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibvctx->device);

	/* Unmap if anything device specific was mapped in init_context. */
	pthread_mutex_destroy(&cntx->shlock);
	if (cntx->shpg)
		munmap(cntx->shpg, rdev->pg_size);

	/* Un-map DPI only for the first PD that was
	 * allocated in this context.
	 */
	if (cntx->udpi.wcdbpg && cntx->udpi.wcdbpg != MAP_FAILED) {
		munmap(cntx->udpi.wcdbpg, rdev->pg_size);
		cntx->udpi.wcdbpg = NULL;
	}
	if (cntx->udpi.dbpage && cntx->udpi.dbpage != MAP_FAILED) {
		munmap(cntx->udpi.dbpage, rdev->pg_size);
		cntx->udpi.dbpage = NULL;
	}
	if (cntx->comp_mask & BNXT_RE_COMP_MASK_UCNTX_DBR_PACING_ENABLED) {
		munmap(cntx->dbr_page, rdev->pg_size);
		cntx->dbr_page = NULL;
		munmap(cntx->bar_map, rdev->pg_size);
		cntx->bar_map = NULL;
	}

	verbs_uninit_context(&cntx->ibvctx);
	free(cntx);
}

static struct verbs_device *
bnxt_re_device_alloc(struct verbs_sysfs_dev *sysfs_dev)
{
	struct bnxt_re_dev *rdev;

	rdev = calloc(1, sizeof(*rdev));
	if (!rdev)
		return NULL;

	return &rdev->vdev;
}

static const struct verbs_device_ops bnxt_re_dev_ops = {
	.name = "bnxt_re",
	.match_min_abi_version = BNXT_RE_ABI_VERSION,
	.match_max_abi_version = BNXT_RE_ABI_VERSION,
	.match_table = cna_table,
	.alloc_device = bnxt_re_device_alloc,
	.alloc_context = bnxt_re_alloc_context,
};
PROVIDER_DRIVER(bnxt_re, bnxt_re_dev_ops);
