#include <linux/list.h>
#include <linux/types.h>
#include "../drivers/neoprof/neoprof.h"
#include "../mm/damon/ops-common.h"
#include <linux/mm.h>
#include "../mm/internal.h"
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/page-flags.h>
#include <linux/swap.h>

// #define DEBUG

#define PAGES_PER_MB (256)


static DECLARE_WAIT_QUEUE_HEAD(kneomemd_scanning_wait);
LIST_HEAD(hotpage_list);
static DEFINE_SPINLOCK(hotpage_list_lock);

// neomem_scanning, peridically get hot pages from neoprof
static bool neomem_scanning_enabled = false;                    // enable the neomem scanning thread
static unsigned long neomem_scanning_interval_us = 100000;    // 100ms
static u32 neomem_scanning_reset_period = 10;          // reset the neoprof sketch every 10 scanning iterations
static u32 neomem_scanning_hotness_threshold = 2;
static u32 neomem_scanning_sample_period = 0;          // sampling the memory access, it will also affect content in neomem_hist

// neomem_state, showing bandwidth information (read & write)
static u32 neomem_states_sample_period = 100;           // sampling the memory access
static u32 neomem_states_sample_total_cycle = 0;        // total access being sampled
static u32 neomem_states_sample_write_cnt = 0;
static u32 neomem_states_sample_read_cnt = 0;

// neomem_hist, showing the distribution of memory access in a period
static u32 neomem_hist_scan_period = 50;              // get the hist every certain period
static u32 *neomem_hist;                               // store the information
static u32 neomem_hist_nr_bins = 64;                   // number of bins in the histogram
static u32 neomem_error_bound = 0;                     // error bound for the estimated hotness
static u32 neomem_percentile = 50;                     // the p-percentile value for error bound estimation

// neomem_quota, showing the limitation of page migration in a period
static u32 neomem_quota_migrate_page_size_MB = 256;              // limit the migration size in a period

// sync update
bool neomem_update_state = false;

static u32 hist_bin[64] = {0,     1,     2,     3,     4,     5,     6,     7,
                           8,    10,    12,    14,    16,     18,    20,    22,
                           24,   28,    32,    36,    40,     44,    48,    52,
                           56,   64,    72,    80,    88,     96,   104,   112,
                           120,  136,   152,   168,   184,    200,   216,   232,
                           248,  280,   312,   344,   376,    408,   440,   472,
                           504,  568,   632,   696,   760,    824,   888,   952,
                           1016, 1144,  1272,  1400,  1528,   1656,  1784,  1912};

static void kneomem_usleep(unsigned long usecs)
{
    /* See Documentation/timers/timers-howto.rst for the thresholds */
    if (usecs > 20 * USEC_PER_MSEC)
        schedule_timeout_idle(usecs_to_jiffies(usecs));
    else
        usleep_idle_range(usecs, usecs + 1);
}


static void neomem_get_states(void){
    neomem_states_sample_total_cycle = get_total_state_sample_cnt();
    neomem_states_sample_write_cnt = get_wr_state_sample_cnt();
    neomem_states_sample_read_cnt = get_rd_state_sample_cnt();
}


// static int hotpage_add(u64 paddr)
// {
//     struct page *page;
//     struct folio *folio;
//     u64 pfn = PHYS_PFN(paddr);

//     if (!pfn_valid(pfn))
//     {
//         count_vm_event(NEOMEM_PFN_INVALID);
//         return 0;
//     }

//     page = pfn_to_page(pfn);
//     if (!page){
//         count_vm_event(NEOMEM_PFN_TO_PAGE_FAILED);
//         return 0;
//     }

//     page = compound_head(page);
//     pfn = page_to_pfn(page);

//     page = pfn_to_online_page(pfn);

// 	if (!page || PageTail(page)){
//         count_vm_event(NEOMEM_PAGE_TAIL_OR_INVALID);
// 		return 0;
// 	}

// 	folio = page_folio(page);
// 	if (!folio_test_lru(folio) || !folio_try_get(folio)){
//         count_vm_event(NEOMEM_FOLIO_NOT_LRU_OR_GET_FOLIO_FAILED);
// 		return 0;
// 	}

// 	if (unlikely(page_folio(page) != folio || !folio_test_lru(folio))) {
//         count_vm_event(NEOMEM_FOLIO_LATE_FAILED);
// 		folio_put(folio);
//         return 0;
// 	}

//     if (!folio)
//     {
//         count_vm_event(NEOMEM_FOLIO_INVALID);
//         return 0;
//     }

//     if (!folio_isolate_lru(folio)) {
//         count_vm_event(NEOMEM_FOLIO_ISOLATE_LRU_FAILED);
//         folio_put(folio);
//         return 0;
//     }

//     if (folio_test_unevictable(folio)){
//         count_vm_event(NEOMEM_FOLIO_UNEVICTABLE);
//         folio_putback_lru(folio);
//         folio_put(folio);
//         return 0;
//     }
//     else{
//         count_vm_event(NEOMEM_ADD_HOT_PAGE);

//         if (folio_test_demoted(folio)){
//             count_vm_event(NEOMEM_HOT_AFTER_DEMOTED);
//             folio_clear_demoted(folio);
//         }

//         spin_lock(&hotpage_list_lock);
//         list_add(&folio->lru, &hotpage_list);
//         spin_unlock(&hotpage_list_lock);
//     }

//     folio_put(folio);

//     return 1;
// }


/*
 * Read hotpages from neoprof device
 */
// static int get_hotpages_from_neoprof(void)
// {
//     u32 nr_hotpages = get_nr_hotpages();

//     count_vm_events(NEOMEM_HOT_PAGE_CANDIDATE, nr_hotpages);
//     for(int i = 0; i < nr_hotpages; i++) {
//         u64 paddr = get_hotpage();
//         hotpage_add(paddr);
//     }

//     return 0;   
// }


void get_error_bound(void){
    u32 total_count = 0;
    for(int i = 0; i < neomem_hist_nr_bins; i++){
        total_count += neomem_hist[i];
    }
    u32 p_percentile = total_count * neomem_percentile / 100;
    u32 new_error_bound = 0;
    u32 accumulated_count = 0;
    for(int i = 0; i < neomem_hist_nr_bins; i++){
        accumulated_count += neomem_hist[i];
        if (accumulated_count >= p_percentile){
            new_error_bound = i;
            break;
        }
    }
    neomem_error_bound = new_error_bound;
}

void get_hist_from_neoprof(void){
    u32 nr_hist = 0;
    int retry_times = 0;
    start_rd_hist();
    while ((nr_hist != neomem_hist_nr_bins) && (retry_times < 10)){
        kneomem_usleep(500);
        nr_hist = get_nr_hist();
        retry_times++;
    }
    get_hist(nr_hist, neomem_hist);
    if (retry_times >= 10){
        printk("Error: cannot get hist from neoprof\n");
        return;
    }
    get_error_bound();
}

// static int kneomemd_migration(void *data){
//     LIST_HEAD(hotpage_list_local);
//     int nr_remaining, nr_succeeded;
//     struct folio *_folio;

//     spin_lock(&hotpage_list_lock);
//     list_splice(&hotpage_list, &hotpage_list_local);
//     INIT_LIST_HEAD(&hotpage_list);
//     spin_unlock(&hotpage_list_lock);

//     nr_remaining = _neomem_migrate_pages(&hotpage_list_local, alloc_misplaced_dst_page,
//                      NULL, 0, MIGRATE_ASYNC,
//                      MR_NUMA_MISPLACED, &nr_succeeded);


//     if (nr_remaining) {
//         while (!list_empty(&hotpage_list_local)){
//             count_vm_event(NEOMEM_MIGRATE_PAGES_REMAINED);
//             _folio = list_entry(hotpage_list_local.next, struct folio, lru);
//             list_del(&_folio->lru);
//             folio_putback_lru(_folio);
//         }
//     }

//     if (nr_succeeded) {
//         count_vm_events(NEOMEM_MIGRATE_PAGES, nr_succeeded);
//     }

//     BUG_ON(!list_empty(&hotpage_list_local));

//     return 0;
// }


static void neomem_update_state_all(void){
    set_hotness_threshold(neomem_scanning_hotness_threshold);
    set_access_sample_interval(neomem_scanning_sample_period);
    set_state_sample_interval(neomem_states_sample_period);
    neomem_update_state = false;
}


/*
 * The monitoring daemon that runs as a kernel thread
 */
static int kneomemd_scanning(void * data)
{
    u64 scan_cnt = 0;
    void *ctx;
    printk("kneomemd_scanning starts\n");
    while (!kthread_should_stop()) {
        
        kneomem_usleep(neomem_scanning_interval_us);

        if (neomem_update_state){
            neomem_update_state_all();
        }

        if(!neomem_scanning_enabled){
            wait_event_interruptible(kneomemd_scanning_wait, neomem_scanning_enabled);
        }

        // int err = get_hotpages_from_neoprof();
        // if(err)
        // {
        //     printk("get_hotpages_from_neoprof failed\n");
        //     return err;
        // }

        // kthread_run(kneomemd_migration, ctx, "kneomemd.migration");
        // kneomemd_migration(ctx);
        if(scan_cnt % neomem_hist_scan_period == 0){
            get_hist_from_neoprof();
            kneomem_usleep(5000);
        }

        neomem_get_states();

        if(scan_cnt % neomem_scanning_reset_period == 0){

            count_vm_event(NEOMEM_RESET_NEOPROF);

            // clear the states in neoprof
            reset_neoprof();
        }
        scan_cnt++;
    }
    printk("kneomemd_scanning exit\n");
    return 0;
}

/*
 * __damon_start() - Starts monitoring.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int neomem_start(void)
{
    int err = -EBUSY;
    void *ctx;
    struct task_struct * kneomemd;
    set_hotness_threshold(neomem_scanning_hotness_threshold);

    neomem_hist = kmalloc(sizeof(u32) * HIST_SIZE, GFP_KERNEL);
    memset(neomem_hist, 0, sizeof(u32) * HIST_SIZE);
    
    kneomemd = kthread_run(kneomemd_scanning, ctx, "kneomemd.scanning");

    if (IS_ERR(kneomemd)) {
        err = PTR_ERR(kneomemd);
        kneomemd = NULL;
    }

    return err;
}



/***********************************************************
 ****************** sysfs interface ************************
 ***********************************************************/

/*
 * neomem_scanning_enabled
 */

static ssize_t neomem_scanning_enabled_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", neomem_scanning_enabled ? "true" : "false");
}

static ssize_t neomem_scanning_enabled_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtobool(buf, &neomem_scanning_enabled);
    if (neomem_scanning_enabled)
        wake_up_interruptible(&kneomemd_scanning_wait);
	if (ret)
		return ret;

	return count;
}

static struct kobj_attribute neomem_scanning_enabled_attr =
	__ATTR(neomem_scanning_enabled, 0644, neomem_scanning_enabled_show,
	       neomem_scanning_enabled_store);


/*
 * neomem_scanning_interval_us
 */

static ssize_t neomem_scanning_interval_us_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_scanning_interval_us);
}

static ssize_t neomem_scanning_interval_us_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf, size_t count)
{
    ssize_t ret;
    u32 new_neomem_scanning_interval_us;

    ret = kstrtou32(buf, 0, &new_neomem_scanning_interval_us);
    if (ret)
        return ret;

    neomem_scanning_interval_us = new_neomem_scanning_interval_us;
    return count;
}

static struct kobj_attribute neomem_scanning_interval_us_attr =
	__ATTR(neomem_scanning_interval_us, 0644, neomem_scanning_interval_us_show,
	       neomem_scanning_interval_us_store);


/* 
 *  neomem_scanning_reset_period
 */

static ssize_t neomem_scanning_reset_period_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_scanning_reset_period);
}

static ssize_t neomem_scanning_reset_period_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf, size_t count)
{
    ssize_t ret;
    u32 new_neomem_scanning_reset_period;

    ret = kstrtou32(buf, 0, &new_neomem_scanning_reset_period);
    if (ret)
        return ret;

    neomem_scanning_reset_period = new_neomem_scanning_reset_period;
    return count;
}

static struct kobj_attribute neomem_scanning_reset_period_attr =
	__ATTR(neomem_scanning_reset_period, 0644, neomem_scanning_reset_period_show,
	       neomem_scanning_reset_period_store);


/* 
 * neomem_scanning_hotness_threshold
 */

static ssize_t neomem_scanning_hotness_threshold_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_scanning_hotness_threshold);
}

static ssize_t neomem_scanning_hotness_threshold_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf, size_t count)
{
    ssize_t ret;
    u32 new_neomem_scanning_hotness_threshold;

    ret = kstrtou32(buf, 0, &new_neomem_scanning_hotness_threshold);
    if (ret)
        return ret;

    neomem_scanning_hotness_threshold = new_neomem_scanning_hotness_threshold;
    neomem_update_state = true;

    return count;
}

static struct kobj_attribute neomem_scanning_hotness_threshold_attr =
    __ATTR(neomem_scanning_hotness_threshold, 0644, neomem_scanning_hotness_threshold_show,
           neomem_scanning_hotness_threshold_store);


/* 
 * neomem_scanning_sample_period
 */

static ssize_t neomem_scanning_sample_period_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_scanning_sample_period);
}

static ssize_t neomem_scanning_sample_period_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf, size_t count)
{
    ssize_t ret;
    u32 new_neomem_scanning_sample_period;

    ret = kstrtou32(buf, 0, &new_neomem_scanning_sample_period);
    if (ret)
        return ret;

    neomem_scanning_sample_period = new_neomem_scanning_sample_period;
    neomem_update_state = true;

    return count;
}


static struct kobj_attribute neomem_scanning_sample_period_attr =
	__ATTR(neomem_scanning_sample_period, 0644, neomem_scanning_sample_period_show,
	       neomem_scanning_sample_period_store);


/*
 * neomem_states
 */

static ssize_t neomem_states_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "total cycles: %u\nwrite counts: %u\nread counts: %u\n", neomem_states_sample_total_cycle, neomem_states_sample_write_cnt, neomem_states_sample_read_cnt);
}

static struct kobj_attribute neomem_states_attr =
	__ATTR(neomem_states_show, 0644, neomem_states_show,
	       NULL);


/* 
 *  neomem_states_sample_period
 */

static ssize_t neomem_states_sample_period_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_states_sample_period);
}

static ssize_t neomem_states_sample_period_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf, size_t count)
{
    ssize_t ret;
    u32 new_neomem_states_sample_period;

    ret = kstrtou32(buf, 0, &new_neomem_states_sample_period);
    if (ret)
        return ret;

    neomem_states_sample_period = new_neomem_states_sample_period;
    neomem_update_state = true;
    return count;
}

static struct kobj_attribute neomem_states_sample_period_attr =
	__ATTR(neomem_states_sample_period, 0644, neomem_states_sample_period_show,
	       neomem_states_sample_period_store);


/* 
 *  neomem_hist
 */

static ssize_t neomem_hist_scan_period_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_hist_scan_period);
}

static ssize_t neomem_hist_scan_period_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf, size_t count)
{
    ssize_t ret;
    u32 new_neomem_hist_scan_period;

    ret = kstrtou32(buf, 0, &new_neomem_hist_scan_period);
    if (ret)
        return ret;

    neomem_hist_scan_period = new_neomem_hist_scan_period;
    return count;
}

static struct kobj_attribute neomem_hist_scan_period_attr =
    __ATTR(neomem_hist_scan_period, 0644, neomem_hist_scan_period_show,
           neomem_hist_scan_period_store);


static ssize_t neomem_hist_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
    int ret = 0;
    char msg[2000] = {0};
    
    for (int i = 0; i < HIST_SIZE; i++)
    {
        sprintf(msg, "%shist %d: %d\n", msg, hist_bin[i], neomem_hist[i]);
    }
    ret = sysfs_emit(buf, "%s", msg);
    return ret;
}

static ssize_t neomem_hist_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	return count;
}

static struct kobj_attribute neomem_hist_attr =
	__ATTR(neomem_hist, 0644, neomem_hist_show,
	       neomem_hist_store);



static ssize_t neomem_hist_percentile_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_percentile);
}

static ssize_t neomem_hist_percentile_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf, size_t count)
{
    ssize_t ret;
    u32 new_neomem_hist_percentile;

    ret = kstrtou32(buf, 0, &new_neomem_hist_percentile);
    if (ret)
        return ret;

    neomem_percentile = new_neomem_hist_percentile;
    return count;
}

static struct kobj_attribute neomem_hist_percentile_attr =
    __ATTR(neomem_hist_percentile, 0644, neomem_hist_percentile_show,
           neomem_hist_percentile_store);


static ssize_t neomem_error_bound_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_error_bound);
}

static struct kobj_attribute neomem_error_bound_attr =
    __ATTR(neomem_error_bound, 0644, neomem_error_bound_show,
           NULL);


/* 
 *  neomem_quota_migrate_page_size_MB
 */

static ssize_t neomem_quota_migrate_page_size_MB_show(struct kobject *kobj,
                      struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%u\n", neomem_quota_migrate_page_size_MB);
}

static ssize_t neomem_quota_migrate_page_size_MB_store(struct kobject *kobj,
                       struct kobj_attribute *attr,
                       const char *buf, size_t count)
{
    ssize_t ret;
    u32 new_neomem_quota_migrate_page_size_MB;

    ret = kstrtou32(buf, 0, &new_neomem_quota_migrate_page_size_MB);
    if (ret)
        return ret;

    neomem_quota_migrate_page_size_MB = new_neomem_quota_migrate_page_size_MB;
    return count;
}

static struct kobj_attribute neomem_quota_migrate_page_size_MB_attr =
    __ATTR(neomem_quota_migrate_page_size_MB, 0644, neomem_quota_migrate_page_size_MB_show,
           neomem_quota_migrate_page_size_MB_store);


static struct attribute *neomem_attrs[] = {
	&neomem_scanning_enabled_attr.attr,
    &neomem_scanning_interval_us_attr.attr,
    &neomem_scanning_reset_period_attr.attr,
    &neomem_scanning_hotness_threshold_attr.attr,
    &neomem_scanning_sample_period_attr.attr,
    
    &neomem_states_attr.attr, 
    &neomem_states_sample_period_attr.attr,
    
    &neomem_hist_attr.attr,
    &neomem_hist_percentile_attr.attr,
    &neomem_hist_scan_period_attr.attr,
    &neomem_error_bound_attr.attr,

    &neomem_quota_migrate_page_size_MB_attr.attr,
	NULL,
};

static const struct attribute_group neomem_attr_group = {
	.attrs = neomem_attrs,
};

static int __init neomem_init_sysfs(void)
{
	int err;
	struct kobject *neomem_kobj;

	neomem_kobj = kobject_create_and_add("neomem", mm_kobj);
	if (!neomem_kobj) {
		pr_err("failed to create neomem kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(neomem_kobj, &neomem_attr_group);
	if (err) {
		pr_err("failed to register neomem group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(neomem_kobj);
	return err;
}
subsys_initcall(neomem_init_sysfs);