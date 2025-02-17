/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/mutex.h>

#include "mdss_mdp.h"

#define SMP_MB_SIZE		(mdss_res->smp_mb_size)
#define SMP_MB_CNT		(mdss_res->smp_mb_cnt)
#define SMP_ENTRIES_PER_MB	(SMP_MB_SIZE / 16)
#define SMP_MB_ENTRY_SIZE	16
#define MAX_BPP 4

static DEFINE_MUTEX(mdss_mdp_sspp_lock);
static DEFINE_MUTEX(mdss_mdp_smp_lock);

static int mdss_mdp_pipe_free(struct mdss_mdp_pipe *pipe);
static struct mdss_mdp_pipe *mdss_mdp_pipe_search_by_client_id(
	struct mdss_data_type *mdata, int client_id);

static inline void mdss_mdp_pipe_write(struct mdss_mdp_pipe *pipe,
				       u32 reg, u32 val)
{
	writel_relaxed(val, pipe->base + reg);
}

static inline u32 mdss_mdp_pipe_read(struct mdss_mdp_pipe *pipe, u32 reg)
{
	return readl_relaxed(pipe->base + reg);
}
static void mdss_mdp_smp_mmb_free(unsigned long *smp, bool write);
static u32 mdss_mdp_smp_mmb_reserve(struct mdss_mdp_pipe_smp_map *smp_map,
	size_t n)
{
	u32 i, mmb;
	u32 fixed_cnt = bitmap_weight(smp_map->fixed, SMP_MB_CNT);
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();

	if (n <= fixed_cnt)
		return fixed_cnt;
	else
		n -= fixed_cnt;

	i = bitmap_weight(smp_map->allocated, SMP_MB_CNT);

	/*
	 * SMP programming is not double buffered. Fail the request,
	 * that calls for change in smp configuration (addition/removal
	 * of smp blocks), so that fallback solution happens.
	 */
	if (i != 0 && n != i) {
		pr_debug("Can't change mmb config, num_blks: %d alloc: %d\n",
			n, i);
		return 0;
	}

	/*
	 * Clear previous SMP reservations and reserve according to the
	 * latest configuration
	 */
	mdss_mdp_smp_mmb_free(smp_map->reserved, false);

	/* Reserve mmb blocks*/
	for (; i < n; i++) {
		if (bitmap_full(mdata->mmb_alloc_map, SMP_MB_CNT))
			break;

		mmb = find_first_zero_bit(mdata->mmb_alloc_map, SMP_MB_CNT);
		set_bit(mmb, smp_map->reserved);
		set_bit(mmb, mdata->mmb_alloc_map);
	}

	return i + fixed_cnt;
}

static int mdss_mdp_smp_mmb_set(int client_id, unsigned long *smp)
{
	u32 mmb, off, data, s;
	int cnt = 0;

	for_each_set_bit(mmb, smp, SMP_MB_CNT) {
		off = (mmb / 3) * 4;
		s = (mmb % 3) * 8;
		data = MDSS_MDP_REG_READ(MDSS_MDP_REG_SMP_ALLOC_W0 + off);
		data &= ~(0xFF << s);
		data |= client_id << s;
		MDSS_MDP_REG_WRITE(MDSS_MDP_REG_SMP_ALLOC_W0 + off, data);
		MDSS_MDP_REG_WRITE(MDSS_MDP_REG_SMP_ALLOC_R0 + off, data);
		cnt++;
	}
	return cnt;
}

static void mdss_mdp_smp_mmb_amend(unsigned long *smp, unsigned long *extra)
{
	bitmap_or(smp, smp, extra, SMP_MB_CNT);
	bitmap_zero(extra, SMP_MB_CNT);
}

static void mdss_mdp_smp_mmb_free(unsigned long *smp, bool write)
{
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();

	if (!bitmap_empty(smp, SMP_MB_CNT)) {
		if (write)
			mdss_mdp_smp_mmb_set(0, smp);
		bitmap_andnot(mdata->mmb_alloc_map, mdata->mmb_alloc_map,
			      smp, SMP_MB_CNT);
		bitmap_zero(smp, SMP_MB_CNT);
	}
}

static void mdss_mdp_smp_set_wm_levels(struct mdss_mdp_pipe *pipe, int mb_cnt)
{
	u32 fetch_size, val, wm[3];

	fetch_size = mb_cnt * SMP_MB_SIZE;

	/*
	 * when doing hflip, one line is reserved to be consumed down the
	 * pipeline. This line will always be marked as full even if it doesn't
	 * have any data. In order to generate proper priority levels ignore
	 * this region while setting up watermark levels
	 */
	if (pipe->flags & MDP_FLIP_LR) {
		u8 bpp = pipe->src_fmt->is_yuv ? 1 :
			pipe->src_fmt->bpp;
		fetch_size -= (pipe->src.w * bpp);
	}

	/* 1/4 of SMP pool that is being fetched */
	val = (fetch_size / SMP_MB_ENTRY_SIZE) >> 2;

	wm[0] = val;
	wm[1] = wm[0] + val;
	wm[2] = wm[1] + val;

	pr_debug("pnum=%d fetch_size=%u watermarks %u,%u,%u\n", pipe->num,
			fetch_size, wm[0], wm[1], wm[2]);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_REQPRIO_FIFO_WM_0, wm[0]);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_REQPRIO_FIFO_WM_1, wm[1]);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_REQPRIO_FIFO_WM_2, wm[2]);
}

static void mdss_mdp_smp_free(struct mdss_mdp_pipe *pipe)
{
	int i;

	mutex_lock(&mdss_mdp_smp_lock);
	for (i = 0; i < MAX_PLANES; i++) {
		mdss_mdp_smp_mmb_free(pipe->smp_map[i].reserved, false);
		mdss_mdp_smp_mmb_free(pipe->smp_map[i].allocated, true);
	}
	mutex_unlock(&mdss_mdp_smp_lock);
}

void mdss_mdp_smp_unreserve(struct mdss_mdp_pipe *pipe)
{
	int i;

	mutex_lock(&mdss_mdp_smp_lock);
	for (i = 0; i < MAX_PLANES; i++)
		mdss_mdp_smp_mmb_free(pipe->smp_map[i].reserved, false);
	mutex_unlock(&mdss_mdp_smp_lock);
}

int mdss_mdp_smp_reserve(struct mdss_mdp_pipe *pipe)
{
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	u32 num_blks = 0, reserved = 0;
	struct mdss_mdp_plane_sizes ps;
	int i;
	int rc = 0, rot_mode = 0;
	u32 nlines;
	u16 width;

	width = pipe->src.w >> pipe->horz_deci;

	if (pipe->bwc_mode) {
		rc = mdss_mdp_get_rau_strides(pipe->src.w, pipe->src.h,
			pipe->src_fmt, &ps);
		if (rc)
			return rc;
		pr_debug("BWC SMP strides ystride0=%x ystride1=%x\n",
			ps.ystride[0], ps.ystride[1]);
	} else {
		rc = mdss_mdp_get_plane_sizes(pipe->src_fmt->format,
			width, pipe->src.h, &ps, 0);
		if (rc)
			return rc;

		if (pipe->mixer && pipe->mixer->rotator_mode) {
			rot_mode = 1;
		} else if ((ps.num_planes == 1) && pipe->mixer) {
			ps.ystride[0] = MAX_BPP *
				max(pipe->mixer->width, width);
		} else if (mdata->has_decimation) {
			/*
			 * when decimation block is used, all chroma planes
			 * are fetched on a single SMP plane for chroma pixels
			 */
			if (ps.num_planes == 3) {
				ps.num_planes = 2;
				ps.ystride[1] += ps.ystride[2];
			}

			/*
			 * To avoid quailty loss, MDP does one less decimation
			 * on chroma components if they are subsampled.
			 * Account for this to have enough SMPs for latency
			 */
			switch (pipe->src_fmt->chroma_sample) {
			case MDSS_MDP_CHROMA_H2V1:
			case MDSS_MDP_CHROMA_420:
				ps.ystride[1] <<= 1;
				break;
			}
		}
	}

	nlines = pipe->bwc_mode ? 1 : 2;

	mutex_lock(&mdss_mdp_smp_lock);
	for (i = (MAX_PLANES - 1); i >= ps.num_planes; i--) {
		if (bitmap_weight(pipe->smp_map[i].allocated, SMP_MB_CNT)) {
			pr_debug("Extra mmb identified for pnum=%d plane=%d\n",
				pipe->num, i);
			mutex_unlock(&mdss_mdp_smp_lock);
			return -EAGAIN;
		}
	}

	for (i = 0; i < ps.num_planes; i++) {
		if (rot_mode) {
			num_blks = 1;
		} else {
			num_blks = DIV_ROUND_UP(ps.ystride[i] * nlines,
					SMP_MB_SIZE);

			if (mdata->mdp_rev == MDSS_MDP_HW_REV_100)
				num_blks = roundup_pow_of_two(num_blks);

			if (mdata->smp_mb_per_pipe &&
				(num_blks > mdata->smp_mb_per_pipe) &&
				!(pipe->flags & MDP_FLIP_LR))
				num_blks = mdata->smp_mb_per_pipe;
		}

		pr_debug("reserving %d mmb for pnum=%d plane=%d\n",
				num_blks, pipe->num, i);
		reserved = mdss_mdp_smp_mmb_reserve(&pipe->smp_map[i],
			num_blks);
		if (reserved < num_blks)
			break;
	}

	if (reserved < num_blks) {
		pr_debug("insufficient MMB blocks\n");
		for (; i >= 0; i--)
			mdss_mdp_smp_mmb_free(pipe->smp_map[i].reserved,
				false);
		rc = -ENOMEM;
	}
	mutex_unlock(&mdss_mdp_smp_lock);

	return rc;
}
/*
 * mdss_mdp_smp_alloc() -- set smp mmb and and wm levels for a staged pipe
 * @pipe: pointer to a pipe
 *
 * Function amends reserved smp mmbs to allocated bitmap and ties respective
 * mmbs to their pipe fetch_ids. Based on the number of total allocated mmbs
 * for a staged pipe, it also sets the watermark levels (wm).
 *
 * This function will be called on every commit where pipe params might not
 * have changed. In such cases, we need to ensure that wm levels are not
 * wiped out. Also in some rare situations hw might have reset and wiped out
 * smp mmb programming but new smp reservation is not done. In such cases we
 * need to ensure that for a staged pipes, mmbs are set properly based on
 * allocated bitmap.
 */
static int mdss_mdp_smp_alloc(struct mdss_mdp_pipe *pipe)
{
	int i;
	int cnt = 0;

	mutex_lock(&mdss_mdp_smp_lock);
	for (i = 0; i < MAX_PLANES; i++) {
		cnt += bitmap_weight(pipe->smp_map[i].fixed, SMP_MB_CNT);

		if (bitmap_empty(pipe->smp_map[i].reserved, SMP_MB_CNT)) {
			cnt += mdss_mdp_smp_mmb_set(pipe->ftch_id + i,
				pipe->smp_map[i].allocated);
			continue;
		}

		mdss_mdp_smp_mmb_amend(pipe->smp_map[i].allocated,
			pipe->smp_map[i].reserved);
		cnt += mdss_mdp_smp_mmb_set(pipe->ftch_id + i,
			pipe->smp_map[i].allocated);
	}
	mdss_mdp_smp_set_wm_levels(pipe, cnt);
	mutex_unlock(&mdss_mdp_smp_lock);
	return 0;
}

void mdss_mdp_smp_release(struct mdss_mdp_pipe *pipe)
{
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
	mdss_mdp_smp_free(pipe);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
}

int mdss_mdp_smp_setup(struct mdss_data_type *mdata, u32 cnt, u32 size)
{
	if (!mdata)
		return -EINVAL;

	mdata->smp_mb_cnt = cnt;
	mdata->smp_mb_size = size;

	return 0;
}

/**
 * mdss_mdp_smp_handoff() - Handoff SMP MMBs in use by staged pipes
 * @mdata: pointer to the global mdss data structure.
 *
 * Iterate through the list of all SMP MMBs and check to see if any
 * of them are assigned to a pipe being marked as being handed-off.
 * If so, update the corresponding software allocation map to reflect
 * this.
 *
 * This function would typically be called during MDP probe for the case
 * when certain pipes might be programmed in the bootloader to display
 * the splash screen.
 */
int mdss_mdp_smp_handoff(struct mdss_data_type *mdata)
{
	int rc = 0;
	int i, client_id, prev_id = 0;
	u32 off, s, data;
	struct mdss_mdp_pipe *pipe = NULL;

	/*
	 * figure out what SMP MMBs are allocated for each of the pipes
	 * that need to be handed off.
	 */
	for (i = 0; i < SMP_MB_CNT; i++) {
		off = (i / 3) * 4;
		s = (i % 3) * 8;
		data = MDSS_MDP_REG_READ(MDSS_MDP_REG_SMP_ALLOC_W0 + off);
		client_id = (data >> s) & 0xFF;
		if (test_bit(i, mdata->mmb_alloc_map)) {
			/*
			 * Certain pipes may have a dedicated set of
			 * SMP MMBs statically allocated to them. In
			 * such cases, we do not need to do anything
			 * here.
			 */
			pr_debug("smp mmb %d already assigned to pipe %d (client_id %d)"
				, i, pipe->num, client_id);
			continue;
		}

		if (client_id) {
			if (client_id != prev_id) {
				pipe = mdss_mdp_pipe_search_by_client_id(mdata,
					client_id);
				prev_id = client_id;
			}

			if (!pipe) {
				pr_warn("Invalid client id %d for SMP MMB %d\n",
					client_id, i);
				continue;
			}

			if (!pipe->is_handed_off) {
				pr_warn("SMP MMB %d assigned to a pipe not marked for handoff (client id %d)"
					, i, client_id);
				continue;
			}

			/*
			 * Assume that the source format only has
			 * one plane
			 */
			pr_debug("Assigning smp mmb %d to pipe %d (client_id %d)\n"
				, i, pipe->num, client_id);
			set_bit(i, pipe->smp_map[0].allocated);
			set_bit(i, mdata->mmb_alloc_map);
		}
	}

	return rc;
}

void mdss_mdp_pipe_unmap(struct mdss_mdp_pipe *pipe)
{
	int tmp;

	tmp = atomic_dec_return(&pipe->ref_cnt);

	WARN(tmp < 0, "Invalid unmap with ref_cnt=%d", tmp);
	if (tmp == 0)
		mdss_mdp_pipe_free(pipe);
}

int mdss_mdp_pipe_map(struct mdss_mdp_pipe *pipe)
{
	if (!atomic_inc_not_zero(&pipe->ref_cnt)) {
		pr_err("attempting to map unallocated pipe (%d)", pipe->num);
		return -EINVAL;
	}
	return 0;
}

static struct mdss_mdp_pipe *mdss_mdp_pipe_init(struct mdss_mdp_mixer *mixer,
						u32 type, u32 off)
{
	struct mdss_mdp_pipe *pipe = NULL;
	struct mdss_data_type *mdata;
	struct mdss_mdp_pipe *pipe_pool = NULL;
	u32 npipes;
	bool pipe_share = false;
	u32 i;

	if (!mixer || !mixer->ctl || !mixer->ctl->mdata)
		return NULL;

	mdata = mixer->ctl->mdata;

	switch (type) {
	case MDSS_MDP_PIPE_TYPE_VIG:
		pipe_pool = mdata->vig_pipes;
		npipes = mdata->nvig_pipes;
		break;

	case MDSS_MDP_PIPE_TYPE_RGB:
		pipe_pool = mdata->rgb_pipes;
		npipes = mdata->nrgb_pipes;
		break;

	case MDSS_MDP_PIPE_TYPE_DMA:
		pipe_pool = mdata->dma_pipes;
		npipes = mdata->ndma_pipes;
		if (!mdata->has_wfd_blk &&
		   (mixer->type == MDSS_MDP_MIXER_TYPE_WRITEBACK))
			pipe_share = true;
		break;

	default:
		npipes = 0;
		pr_err("invalid pipe type %d\n", type);
		break;
	}

	for (i = off; i < npipes; i++) {
		pipe = pipe_pool + i;
		if (atomic_cmpxchg(&pipe->ref_cnt, 0, 1) == 0) {
			pipe->mixer = mixer;
			break;
		}
		pipe = NULL;
	}

	if (pipe) {
		pr_debug("type=%x   pnum=%d\n", pipe->type, pipe->num);
		mutex_init(&pipe->pp_res.hist.hist_mutex);
		spin_lock_init(&pipe->pp_res.hist.hist_lock);
	} else if (pipe_share) {
		/*
		 * when there is no dedicated wfd blk, DMA pipe can be
		 * shared as long as its attached to a writeback mixer
		 */
		pipe = mdata->dma_pipes + mixer->num;
		mdss_mdp_pipe_map(pipe);
		pr_debug("pipe sharing for pipe=%d\n", pipe->num);
	} else {
		pr_err("no %d type pipes available\n", type);
	}

	return pipe;
}

struct mdss_mdp_pipe *mdss_mdp_pipe_alloc_dma(struct mdss_mdp_mixer *mixer)
{
	struct mdss_mdp_pipe *pipe = NULL;
	struct mdss_data_type *mdata;

	mutex_lock(&mdss_mdp_sspp_lock);
	mdata = mixer->ctl->mdata;
	pipe = mdss_mdp_pipe_init(mixer, MDSS_MDP_PIPE_TYPE_DMA, mixer->num);
	if (!pipe) {
		pr_err("DMA pipes not available for mixer=%d\n", mixer->num);
	} else if (pipe != &mdata->dma_pipes[mixer->num]) {
		pr_err("Requested DMA pnum=%d not available\n",
			mdata->dma_pipes[mixer->num].num);
		mdss_mdp_pipe_unmap(pipe);
		pipe = NULL;
	} else {
		pipe->mixer = mixer;
	}
	mutex_unlock(&mdss_mdp_sspp_lock);
	return pipe;
}

struct mdss_mdp_pipe *mdss_mdp_pipe_alloc(struct mdss_mdp_mixer *mixer,
						 u32 type)
{
	struct mdss_mdp_pipe *pipe;
	mutex_lock(&mdss_mdp_sspp_lock);
	pipe = mdss_mdp_pipe_init(mixer, type, 0);
	mutex_unlock(&mdss_mdp_sspp_lock);
	return pipe;
}

struct mdss_mdp_pipe *mdss_mdp_pipe_get(struct mdss_data_type *mdata, u32 ndx)
{
	struct mdss_mdp_pipe *pipe = NULL;

	if (!ndx)
		return ERR_PTR(-EINVAL);

	mutex_lock(&mdss_mdp_sspp_lock);

	pipe = mdss_mdp_pipe_search(mdata, ndx);
	if (!pipe) {
		pipe = ERR_PTR(-EINVAL);
		goto error;
	}

	if (mdss_mdp_pipe_map(pipe))
		pipe = ERR_PTR(-EACCES);

error:
	mutex_unlock(&mdss_mdp_sspp_lock);
	return pipe;
}

static struct mdss_mdp_pipe *mdss_mdp_pipe_search_by_client_id(
	struct mdss_data_type *mdata, int client_id)
{
	u32 i;

	for (i = 0; i < mdata->nrgb_pipes; i++) {
		if (mdata->rgb_pipes[i].ftch_id == client_id)
			return &mdata->rgb_pipes[i];
	}

	for (i = 0; i < mdata->nvig_pipes; i++) {
		if (mdata->vig_pipes[i].ftch_id == client_id)
			return &mdata->vig_pipes[i];
	}

	for (i = 0; i < mdata->ndma_pipes; i++) {
		if (mdata->dma_pipes[i].ftch_id == client_id)
			return &mdata->dma_pipes[i];
	}

	return NULL;
}

struct mdss_mdp_pipe *mdss_mdp_pipe_search(struct mdss_data_type *mdata,
						  u32 ndx)
{
	u32 i;
	for (i = 0; i < mdata->nvig_pipes; i++) {
		if (mdata->vig_pipes[i].ndx == ndx)
			return &mdata->vig_pipes[i];
	}

	for (i = 0; i < mdata->nrgb_pipes; i++) {
		if (mdata->rgb_pipes[i].ndx == ndx)
			return &mdata->rgb_pipes[i];
	}

	for (i = 0; i < mdata->ndma_pipes; i++) {
		if (mdata->dma_pipes[i].ndx == ndx)
			return &mdata->dma_pipes[i];
	}

	return NULL;
}

static int mdss_mdp_pipe_free(struct mdss_mdp_pipe *pipe)
{
	pr_debug("ndx=%x pnum=%d ref_cnt=%d\n", pipe->ndx, pipe->num,
			atomic_read(&pipe->ref_cnt));

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
	mdss_mdp_pipe_sspp_term(pipe);
	mdss_mdp_smp_free(pipe);
	pipe->flags = 0;
	pipe->bwc_mode = 0;
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	return 0;
}

int mdss_mdp_pipe_destroy(struct mdss_mdp_pipe *pipe)
{
	int tmp;

	tmp = atomic_dec_return(&pipe->ref_cnt);

	if (tmp != 0) {
		pr_err("unable to free pipe %d while still in use (%d)\n",
				pipe->num, tmp);
		return -EBUSY;
	}
	mdss_mdp_pipe_free(pipe);

	return 0;

}

/**
 * mdss_mdp_pipe_handoff() - Handoff staged pipes during bootup
 * @pipe: pointer to the pipe to be handed-off
 *
 * Populate the software structures for the pipe based on the current
 * configuration of the hardware pipe by the reading the appropriate MDP
 * registers.
 *
 * This function would typically be called during MDP probe for the case
 * when certain pipes might be programmed in the bootloader to display
 * the splash screen.
 */
int mdss_mdp_pipe_handoff(struct mdss_mdp_pipe *pipe)
{
	int rc = 0;
	u32 src_fmt, reg = 0, bpp = 0;

	/*
	 * todo: for now, only reading pipe src and dest size details
	 * from the registers. This is needed for appropriately
	 * calculating perf metrics for the handed off pipes.
	 * We may need to parse some more details at a later date.
	 */
	reg = mdss_mdp_pipe_read(pipe, MDSS_MDP_REG_SSPP_SRC_SIZE);
	pipe->src.h = reg >> 16;
	pipe->src.w = reg & 0xFFFF;
	reg = mdss_mdp_pipe_read(pipe, MDSS_MDP_REG_SSPP_OUT_SIZE);
	pipe->dst.h = reg >> 16;
	pipe->dst.w = reg & 0xFFFF;

	/* Assume that the source format is RGB */
	reg = mdss_mdp_pipe_read(pipe, MDSS_MDP_REG_SSPP_SRC_FORMAT);
	bpp = ((reg >> 9) & 0x3) + 1;
	switch (bpp) {
	case 4:
		src_fmt = MDP_RGBA_8888;
		break;
	case 3:
		src_fmt = MDP_RGB_888;
		break;
	case 2:
		src_fmt = MDP_RGB_565;
		break;
	default:
		pr_err("Invalid bpp=%d found\n", bpp);
		rc = -EINVAL;
		goto error;
	}
	pipe->src_fmt = mdss_mdp_get_format_params(src_fmt);

	pr_debug("Pipe settings: src.h=%d src.w=%d dst.h=%d dst.w=%d bpp=%d\n"
		, pipe->src.h, pipe->src.w, pipe->dst.h, pipe->dst.w,
		pipe->src_fmt->bpp);

	pipe->is_handed_off = true;
	atomic_inc(&pipe->ref_cnt);

error:
	return rc;
}

void mdss_mdp_crop_rect(struct mdss_mdp_img_rect *src_rect,
	struct mdss_mdp_img_rect *dst_rect,
	const struct mdss_mdp_img_rect *sci_rect)
{
	struct mdss_mdp_img_rect res;
	mdss_mdp_intersect_rect(&res, dst_rect, sci_rect);

	if (res.w && res.h) {
		if ((res.w != dst_rect->w) || (res.h != dst_rect->h)) {
			src_rect->x = src_rect->x + (res.x - dst_rect->x);
			src_rect->y = src_rect->y + (res.y - dst_rect->y);
			src_rect->w = res.w;
			src_rect->h = res.h;
		}
		*dst_rect = (struct mdss_mdp_img_rect)
			{(res.x - sci_rect->x), (res.y - sci_rect->y),
			res.w, res.h};
	}
}

static int mdss_mdp_image_setup(struct mdss_mdp_pipe *pipe,
					struct mdss_mdp_data *data)
{
	u32 img_size, src_size, src_xy, dst_size, dst_xy, ystride0, ystride1;
	u32 width, height;
	u32 decimation;
	struct mdss_mdp_img_rect sci, dst, src;
	int ret = 0;

	pr_debug("pnum=%d wh=%dx%d src={%d,%d,%d,%d} dst={%d,%d,%d,%d}\n",
			pipe->num, pipe->img_width, pipe->img_height,
			pipe->src.x, pipe->src.y, pipe->src.w, pipe->src.h,
			pipe->dst.x, pipe->dst.y, pipe->dst.w, pipe->dst.h);

	width = pipe->img_width;
	height = pipe->img_height;
	mdss_mdp_get_plane_sizes(pipe->src_fmt->format, width, height,
			&pipe->src_planes, pipe->bwc_mode);

	if (data != NULL) {
		ret = mdss_mdp_data_check(data, &pipe->src_planes);
		if (ret)
			return ret;
	}

	if ((pipe->flags & MDP_DEINTERLACE) &&
			!(pipe->flags & MDP_SOURCE_ROTATED_90)) {
		int i;
		for (i = 0; i < pipe->src_planes.num_planes; i++)
			pipe->src_planes.ystride[i] *= 2;
		width *= 2;
		height /= 2;
	}

	decimation = ((1 << pipe->horz_deci) - 1) << 8;
	decimation |= ((1 << pipe->vert_deci) - 1);
	if (decimation)
		pr_debug("Image decimation h=%d v=%d\n",
				pipe->horz_deci, pipe->vert_deci);

	sci = pipe->mixer->ctl->roi;
	dst = pipe->dst;
	src = pipe->src;

	if (pipe->mixer->type == MDSS_MDP_MIXER_TYPE_INTF)
		mdss_mdp_crop_rect(&src, &dst, &sci);

	src_size = (src.h << 16) | src.w;
	src_xy = (src.y << 16) | src.x;
	dst_size = (dst.h << 16) | dst.w;
	dst_xy = (dst.y << 16) | dst.x;

	ystride0 =  (pipe->src_planes.ystride[0]) |
			(pipe->src_planes.ystride[1] << 16);
	ystride1 =  (pipe->src_planes.ystride[2]) |
			(pipe->src_planes.ystride[3] << 16);

	if (pipe->overfetch_disable) {
		if (pipe->overfetch_disable & OVERFETCH_DISABLE_BOTTOM) {
			height = pipe->src.h;
			if (!(pipe->overfetch_disable & OVERFETCH_DISABLE_TOP))
				height += pipe->src.y;
		}
		if (pipe->overfetch_disable & OVERFETCH_DISABLE_RIGHT) {
			width = pipe->src.w;
			if (!(pipe->overfetch_disable & OVERFETCH_DISABLE_LEFT))
				width += pipe->src.x;
		}
		if (pipe->overfetch_disable & OVERFETCH_DISABLE_LEFT)
			src_xy &= ~0xFFFF;
		if (pipe->overfetch_disable & OVERFETCH_DISABLE_TOP)
			src_xy &= ~(0xFFFF << 16);

		pr_debug("overfetch w=%d/%d h=%d/%d src_xy=0x%08x\n", width,
			pipe->img_width, height, pipe->img_height, src_xy);
	}
	img_size = (height << 16) | width;

	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_IMG_SIZE, img_size);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_SIZE, src_size);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_XY, src_xy);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_OUT_SIZE, dst_size);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_OUT_XY, dst_xy);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_YSTRIDE0, ystride0);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_YSTRIDE1, ystride1);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_DECIMATION_CONFIG,
		decimation);

	return 0;
}

static int mdss_mdp_format_setup(struct mdss_mdp_pipe *pipe)
{
	struct mdss_mdp_format_params *fmt;
	u32 chroma_samp, unpack, src_format;
	u32 secure = 0;
	u32 opmode;

	fmt = pipe->src_fmt;

	if (pipe->flags & MDP_SECURE_OVERLAY_SESSION)
		secure = 0xF;

	opmode = pipe->bwc_mode;
	if (pipe->flags & MDP_FLIP_LR)
		opmode |= MDSS_MDP_OP_FLIP_LR;
	if (pipe->flags & MDP_FLIP_UD)
		opmode |= MDSS_MDP_OP_FLIP_UD;

	pr_debug("pnum=%d format=%d opmode=%x\n", pipe->num, fmt->format,
			opmode);

	chroma_samp = fmt->chroma_sample;
	if (pipe->flags & MDP_SOURCE_ROTATED_90) {
		if (chroma_samp == MDSS_MDP_CHROMA_H2V1)
			chroma_samp = MDSS_MDP_CHROMA_H1V2;
		else if (chroma_samp == MDSS_MDP_CHROMA_H1V2)
			chroma_samp = MDSS_MDP_CHROMA_H2V1;
	}

	src_format = (chroma_samp << 23) |
		     (fmt->fetch_planes << 19) |
		     (fmt->bits[C3_ALPHA] << 6) |
		     (fmt->bits[C2_R_Cr] << 4) |
		     (fmt->bits[C1_B_Cb] << 2) |
		     (fmt->bits[C0_G_Y] << 0);

	if (pipe->flags & MDP_ROT_90)
		src_format |= BIT(11); /* ROT90 */

	if (fmt->alpha_enable &&
			fmt->fetch_planes != MDSS_MDP_PLANE_INTERLEAVED)
		src_format |= BIT(8); /* SRCC3_EN */

	unpack = (fmt->element[3] << 24) | (fmt->element[2] << 16) |
			(fmt->element[1] << 8) | (fmt->element[0] << 0);
	src_format |= ((fmt->unpack_count - 1) << 12) |
			(fmt->unpack_tight << 17) |
			(fmt->unpack_align_msb << 18) |
			((fmt->bpp - 1) << 9);

	mdss_mdp_pipe_sspp_setup(pipe, &opmode);

	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_FORMAT, src_format);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_UNPACK_PATTERN, unpack);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_OP_MODE, opmode);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_ADDR_SW_STATUS, secure);

	return 0;
}

int mdss_mdp_pipe_addr_setup(struct mdss_data_type *mdata,
	struct mdss_mdp_pipe *head, u32 *offsets, u32 *ftch_id, u32 type,
	u32 num_base, u32 len)
{
	u32 i;

	if (!head || !mdata) {
		pr_err("unable to setup pipe type=%d: invalid input\n", type);
		return -EINVAL;
	}

	for (i = 0; i < len; i++) {
		head[i].type = type;
		head[i].ftch_id  = ftch_id[i];
		head[i].num = i + num_base;
		head[i].ndx = BIT(i + num_base);
		head[i].base = mdata->mdp_base + offsets[i];
	}

	return 0;
}

static int mdss_mdp_src_addr_setup(struct mdss_mdp_pipe *pipe,
				   struct mdss_mdp_data *data)
{
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	int ret = 0;

	pr_debug("pnum=%d\n", pipe->num);

	data->bwc_enabled = pipe->bwc_mode;

	ret = mdss_mdp_data_check(data, &pipe->src_planes);
	if (ret)
		return ret;

	if (pipe->overfetch_disable) {
		u32 x = 0, y = 0;

		if (pipe->overfetch_disable & OVERFETCH_DISABLE_LEFT)
			x = pipe->src.x;
		if (pipe->overfetch_disable & OVERFETCH_DISABLE_TOP)
			y = pipe->src.y;

		mdss_mdp_data_calc_offset(data, x, y,
			&pipe->src_planes, pipe->src_fmt);
	}

	/* planar format expects YCbCr, swap chroma planes if YCrCb */
	if (mdata->mdp_rev < MDSS_MDP_HW_REV_102 &&
			(pipe->src_fmt->fetch_planes == MDSS_MDP_PLANE_PLANAR)
				&& (pipe->src_fmt->element[0] == C1_B_Cb))
		swap(data->p[1].addr, data->p[2].addr);

	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC0_ADDR, data->p[0].addr);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC1_ADDR, data->p[1].addr);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC2_ADDR, data->p[2].addr);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC3_ADDR, data->p[3].addr);

	return 0;
}

static int mdss_mdp_pipe_solidfill_setup(struct mdss_mdp_pipe *pipe)
{
	int ret;
	u32 secure, format;

	pr_debug("solid fill setup on pnum=%d\n", pipe->num);

	ret = mdss_mdp_image_setup(pipe, NULL);
	if (ret) {
		pr_err("image setup error for pnum=%d\n", pipe->num);
		return ret;
	}

	format = MDSS_MDP_FMT_SOLID_FILL;
	secure = (pipe->flags & MDP_SECURE_OVERLAY_SESSION ? 0xF : 0x0);

	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_FORMAT, format);
	mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_SSPP_SRC_ADDR_SW_STATUS, secure);

	return 0;
}

int mdss_mdp_pipe_queue_data(struct mdss_mdp_pipe *pipe,
			     struct mdss_mdp_data *src_data)
{
	int ret = 0;
	u32 params_changed, opmode;
	struct mdss_mdp_ctl *ctl;

	if (!pipe) {
		pr_err("pipe not setup properly for queue\n");
		return -ENODEV;
	}

	if (!pipe->mixer || !pipe->mixer->ctl) {
		pr_err("pipe mixer not setup properly for queue\n");
		return -ENODEV;
	}

	pr_debug("pnum=%x mixer=%d play_cnt=%u\n", pipe->num,
		 pipe->mixer->num, pipe->play_cnt);

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
	ctl = pipe->mixer->ctl;
	/*
	 * Reprogram the pipe when there is no dedicated wfd blk and
	 * virtual mixer is allocated for the DMA pipe during concurrent
	 * line and block mode operations
	 */
	params_changed = (pipe->params_changed) ||
			 ((pipe->type == MDSS_MDP_PIPE_TYPE_DMA) &&
			 (pipe->mixer->type == MDSS_MDP_MIXER_TYPE_WRITEBACK)
			 && (ctl->mdata->mixer_switched)) ||
			 ctl->roi_changed;
	if (src_data == NULL) {
		mdss_mdp_pipe_solidfill_setup(pipe);
		goto update_nobuf;
	}

	if (params_changed) {
		pipe->params_changed = 0;

		ret = mdss_mdp_pipe_pp_setup(pipe, &opmode);
		if (ret) {
			pr_err("pipe pp setup error for pnum=%d\n", pipe->num);
			goto done;
		}

		ret = mdss_mdp_image_setup(pipe, src_data);
		if (ret) {
			pr_err("image setup error for pnum=%d\n", pipe->num);
			goto done;
		}

		ret = mdss_mdp_format_setup(pipe);
		if (ret) {
			pr_err("format %d setup error pnum=%d\n",
			       pipe->src_fmt->format, pipe->num);
			goto done;
		}

		if (pipe->type == MDSS_MDP_PIPE_TYPE_VIG)
			mdss_mdp_pipe_write(pipe, MDSS_MDP_REG_VIG_OP_MODE,
			opmode);
	}

	mdss_mdp_smp_alloc(pipe);
	ret = mdss_mdp_src_addr_setup(pipe, src_data);
	if (ret) {
		pr_err("addr setup error for pnum=%d\n", pipe->num);
		goto done;
	}

update_nobuf:
	mdss_mdp_mixer_pipe_update(pipe, params_changed);

	pipe->play_cnt++;

done:
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	return ret;
}

int mdss_mdp_pipe_is_staged(struct mdss_mdp_pipe *pipe)
{
	return (pipe == pipe->mixer->stage_pipe[pipe->mixer_stage]);
}
