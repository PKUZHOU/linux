// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON-based page reclamation
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-migrate: " fmt

#include <linux/damon.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/memory-tiers.h>

#include "modules-common.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_migrate."

/*
 * Enable or disable DAMON_RECLAIM.
 *
 * You can enable DAMON_RCLAIM by setting the value of this parameter as ``Y``.
 * Setting it as ``N`` disables DAMON_RECLAIM.  Note that DAMON_RECLAIM could
 * do no real monitoring and reclamation due to the watermarks-based activation
 * condition.  Refer to below descriptions for the watermarks parameter for
 * this.
 */
static bool enabled __read_mostly;
static bool node1_is_toptier;

/*
 * Make DAMON_RECLAIM reads the input parameters again, except ``enabled``.
 *
 * Input parameters that updated while DAMON_RECLAIM is running are not applied
 * by default.  Once this parameter is set as ``Y``, DAMON_RECLAIM reads values
 * of parametrs except ``enabled`` again.  Once the re-reading is done, this
 * parameter is set as ``N``.  If invalid parameters are found while the
 * re-reading, DAMON_RECLAIM will be disabled.
 */
static bool commit_inputs __read_mostly;
module_param(commit_inputs, bool, 0600);

/*
 * Time threshold for cold memory regions identification in microseconds.
 *
 * If a memory region is not accessed for this or longer time, DAMON_RECLAIM
 * identifies the region as cold, and reclaims.  120 seconds by default.
 */
static unsigned long min_access __read_mostly = 1;
module_param(min_access, ulong, 0600);

static struct damos_quota damon_reclaim_quota = {
	/* use up to 10 ms time, reclaim up to 128 MiB per 1 sec by default */
	.ms = 0,
	.sz = 0,
	.reset_interval = 1000,
	/* Within the quota, page out older regions first. */
	.weight_sz = 0,
	.weight_nr_accesses = 0,
	.weight_age = 1
};
DEFINE_DAMON_MODULES_DAMOS_QUOTAS(damon_reclaim_quota);

static struct damos_watermarks damon_reclaim_wmarks = {
	.metric = DAMOS_WMARK_NONE,
	.interval = 0,	
	.high = 0,		
	.mid = 0,		
	.low = 0,		
};
DEFINE_DAMON_MODULES_WMARKS_PARAMS(damon_reclaim_wmarks);

static struct damon_attrs damon_reclaim_mon_attrs = {
	.sample_interval = 1000000,	
	.aggr_interval = 5000000,	
	.ops_update_interval = 0,
	.min_nr_regions = 10,
	.max_nr_regions = 10,
};
DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(damon_reclaim_mon_attrs);

/*
 * Start of the target memory region in physical address.
 *
 * The start physical address of memory region that DAMON_RECLAIM will do work
 * against.  By default, biggest System RAM is used as the region.
 */
static unsigned long monitor_region_start __read_mostly;
module_param(monitor_region_start, ulong, 0600);

/*
 * End of the target memory region in physical address.
 *
 * The end physical address of memory region that DAMON_RECLAIM will do work
 * against.  By default, biggest System RAM is used as the region.
 */
static unsigned long monitor_region_end __read_mostly;
module_param(monitor_region_end, ulong, 0600);

/*
 * Skip anonymous pages reclamation.
 *
 * If this parameter is set as ``Y``, DAMON_RECLAIM does not reclaim anonymous
 * pages.  By default, ``N``.
 */
static bool skip_anon __read_mostly;
module_param(skip_anon, bool, 0600);

/*
 * PID of the DAMON thread
 *
 * If DAMON_RECLAIM is enabled, this becomes the PID of the worker thread.
 * Else, -1.
 */
static int kdamond_pid __read_mostly = -1;
module_param(kdamond_pid, int, 0400);

static struct damos_stat damon_reclaim_stat;
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(damon_reclaim_stat,
		reclaim_tried_regions, reclaimed_regions, quota_exceeds);

static struct damon_ctx *ctx;
static struct damon_target *target;

static struct damos *damon_reclaim_new_scheme(void)
{
	struct damos_access_pattern pattern = {
		/* Find regions having PAGE_SIZE or larger size */
		.min_sz_region = 0,
		.max_sz_region = UINT_MAX,
		/* and not accessed at all */
		.min_nr_accesses = min_access,
		.max_nr_accesses = UINT_MAX,
		/* for min_age or more micro-seconds */
		.min_age_region = 0,
		.max_age_region = UINT_MAX,
	};

#ifdef PRINT_DEBUG_INFO
	printk("pattern.min_nr_accesses is set to %d\n", pattern.min_nr_accesses);
#endif

	return damon_new_scheme(
			&pattern,
			/* page out those, as soon as found */
			DAMOS_MIGRATE,
			/* under the quota. */
			&damon_reclaim_quota,
			/* (De)activate this according to the watermarks. */
			&damon_reclaim_wmarks);
}

static int damon_reclaim_apply_parameters(void)
{
	struct damos *scheme;
	struct damos_filter *filter;
	int err = 0;
	int page_granularity = true;

#ifdef PRINT_DEBUG_INFO
	int region_num = 0;
	int target_num = 0;
	struct damon_target *t;
	struct damon_region *r;
#endif

	err = damon_set_attrs(ctx, &damon_reclaim_mon_attrs);
	if (err)
		return err;

	/* Will be freed by next 'damon_set_schemes()' below */
	scheme = damon_reclaim_new_scheme();
	if (!scheme)
		return -ENOMEM;
	if (skip_anon) {
		filter = damos_new_filter(DAMOS_FILTER_TYPE_ANON, true);
		if (!filter) {
			/* Will be freed by next 'damon_set_schemes()' below */
			damon_destroy_scheme(scheme);
			return -ENOMEM;
		}
		damos_add_filter(scheme, filter);
	}
	damon_set_schemes(ctx, &scheme, 1);

	err = damon_set_region_numa_node1(target,
					&monitor_region_start,
					&monitor_region_end);
	if (err)
		return err;

	if (page_granularity){
		damon_reclaim_mon_attrs.max_nr_regions = (monitor_region_end - monitor_region_start) / PAGE_SIZE;
		damon_reclaim_mon_attrs.min_nr_regions = (monitor_region_end - monitor_region_start) / PAGE_SIZE;
		damon_set_attrs(ctx, &damon_reclaim_mon_attrs);
	}

#ifdef PRINT_DEBUG_INFO
	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t){
			region_num += 1;
		}
		target_num += 1;
	}
	printk("init: region_num: %d, target_num: %d\n", region_num, target_num);
#endif

	return 0;
}

static int damon_reclaim_turn(bool on)
{
	int err;

	if (!on) {
		err = damon_stop(&ctx, 1);
		if (!err)
			kdamond_pid = -1;
		return err;
	}

	err = damon_reclaim_apply_parameters();
	if (err)
		return err;

	err = damon_start(&ctx, 1, true);
	if (err)
		return err;
	kdamond_pid = ctx->kdamond->pid;
	return 0;
}

static int damon_reclaim_enabled_store(const char *val,
		const struct kernel_param *kp)
{
	bool is_enabled = enabled;
	bool enable;
	int err;

	err = kstrtobool(val, &enable);
	if (err)
		return err;

	if (is_enabled == enable)
		return 0;

	/* Called before init function.  The function will handle this. */
	if (!ctx)
		goto set_param_out;

	err = damon_reclaim_turn(enable);
	if (err)
		return err;

set_param_out:
	enabled = enable;
	return err;
}

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_reclaim_enabled_store,
	.get = param_get_bool,
};

module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled,
	"Enable or disable DAMON_RECLAIM (default: disabled)");

int node1_is_toptier_read(char *buffer, const struct kernel_param *kp)
{
	int t0, t1;
	t0 = (int)(node_is_toptier(0));
	t1 = (int)(node_is_toptier(1));
	/* Y and N chosen as being relatively non-coder friendly */
	return sprintf(buffer, "%c\nnode0 is toptier: %d\nnode1 is toptier: %d\n", *(bool *)kp->arg ? 'Y' : 'N', t0, t1);
}

static int node1_is_toptier_store(const char *val,
		const struct kernel_param *kp)
{
	bool enable;
	int err;
	struct memory_tier *mt;

	err = kstrtobool(val, &enable);
	if (err)
		return err;

	if (enable){
		mt = __node_get_memory_tier(1);
		top_tier_adistance = mt->adistance_start + MEMTIER_CHUNK_SIZE - 1;
	}
	else{
		mt = __node_get_memory_tier(0);
		top_tier_adistance = mt->adistance_start + MEMTIER_CHUNK_SIZE - 1;
	}
	return err;
}

static const struct kernel_param_ops node1_is_toptier_param_ops = {
	.set = node1_is_toptier_store,
	.get = node1_is_toptier_read,
};

module_param_cb(node1_is_toptier, &node1_is_toptier_param_ops, &node1_is_toptier, 0600);
MODULE_PARM_DESC(node1_is_toptier,
	"node1 is toptier (default: not)");


static int damon_reclaim_handle_commit_inputs(void)
{
	int err;

	if (!commit_inputs)
		return 0;

	err = damon_reclaim_apply_parameters();
	commit_inputs = false;
	return err;
}

extern unsigned long long migrated_pages_cnt;

static int damon_reclaim_after_aggregation(struct damon_ctx *c)
{
	struct damos *s;
#ifdef PRINT_DEBUG_INFO
	struct damon_target *t;
	struct damon_region *r;
	unsigned long nr_accesses = 0;
	int cnt = 0;
	int region_num = 512 * 1024;
	int heat_level = 0;
	unsigned long long bitmap = 0;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t){
			nr_accesses += r->nr_accesses;
			if (cnt == region_num - 1){
				heat_level = 0;
				bitmap *= 10;
				while (nr_accesses > 0){
					heat_level += 1;
					nr_accesses /= 10;
				}
				if (heat_level > 9)
					heat_level = 9;
				bitmap += heat_level;
			}
			cnt = (cnt + 1) % region_num;
		}
	}
	printk("%lld\n", bitmap);

	printk("%lld pages is migrated!\n", migrated_pages_cnt);
	migrated_pages_cnt = 0;

#endif

	/* update the stats parameter */
	damon_for_each_scheme(s, c)
		damon_reclaim_stat = s->stat;

	return damon_reclaim_handle_commit_inputs();
}

static int damon_reclaim_after_wmarks_check(struct damon_ctx *c)
{
	return damon_reclaim_handle_commit_inputs();
}

static int __init damon_migrate_init(void)
{
	int err = damon_modules_new_paddr_ctx_target(&ctx, &target);

	if (err)
		return err;

	ctx->callback.after_wmarks_check = damon_reclaim_after_wmarks_check;
	ctx->callback.after_aggregation = damon_reclaim_after_aggregation;

	/* 'enabled' has set before this function, probably via command line */
	if (enabled)
		err = damon_reclaim_turn(true);

	

	return err;
}

module_init(damon_migrate_init);
