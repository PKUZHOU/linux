#include <linux/module.h> 
#include <linux/numa.h>
#include <linux/err.h>
#include <linux/memory-tiers.h>
#include <linux/init.h>

#define MEMTIER_DEFAULT_EXT_MEM_ADISTANCE	(MEMTIER_ADISTANCE_DRAM * 5)

static struct memory_dev_type *ext_mem_type;
static int __init ext_mem_init(void) {
    int ext_node_id = 1;
    int rc;
    struct memory_tier *memtier;
	
    ext_mem_type = alloc_memory_type(MEMTIER_DEFAULT_EXT_MEM_ADISTANCE);
    if (IS_ERR(ext_mem_type)) {
        printk(KERN_INFO "alloc_memory_type failed\n");
		rc = PTR_ERR(ext_mem_type);
		goto err_ext_mem_type;
	}
    init_node_memory_type(ext_node_id, ext_mem_type);
    printk("Ext memory node init succss");
    // // set the memory tier for the numa node 1
    // memtier = set_node_memory_tier(ext_node_id);
    // if (!IS_ERR(memtier)) 
	// 	establish_demotion_targets();
    // else{
    //     printk(KERN_INFO "set_node_memory_tier failed\n");
    // }
    return 0;

err_ext_mem_type:
    return rc;
}
// static void __exit extmem_exit(void) { printk(KERN_INFO "Ext Mem driver exit\n"); }

arch_initcall(ext_mem_init); 
// module_exit(extmem_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("PKU Zhe Zhou");
MODULE_DESCRIPTION("A simple Ext-memory driver");
