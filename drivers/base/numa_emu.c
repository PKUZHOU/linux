// SPDX-License-Identifier: GPL-2.0
/*
 * NUMA Emulation for non-NUMA platforms.
 */

#include <linux/numa.h>
#include <linux/nodemask.h>
#include <linux/pfn.h>
#include <linux/memblock.h>
#include <asm/numa.h>

static char *emu_cmdline __initdata;
#define MB2BYTE(x)	((u64)(x) << 6)


/*
 * arch_numa_emu_cmdline - parse early NUMA Emulation params.
 */
void __init arch_numa_emu_cmdline(char *str)
{
	emu_cmdline = str;
}

/*
 * arch_numa_emu_init - Initialize NUMA Emulation
 *
 * Used when NUMA Emulation is enabled on a platform without underlying
 * NUMA architecture.
 */
int __init arch_numa_emu_init(void)
{
	u64 node_size;
	int node_cnt = 0;
	int mblk_cnt = 0;
	int node = 0;
	struct memblock_region *mblk;
	bool split = false;
	int ret;
	u64 ratio[MAX_NUMNODES];
	u64 total_ratio = 0;
	u64 assigned_node_size[MAX_NUMNODES];

	pr_info("NUMA emulation init begin\n");

	if (!emu_cmdline)
		return -EINVAL;
	
	pr_info("PFN_PHYS(max_pfn) = %#018Lx\n", PFN_PHYS(max_pfn));
	pr_info("PFN_PHYS(min_low_pfb) = %#018Lx\n", PFN_PHYS(min_low_pfn)); 
	u64 phy_mem_size = PFN_PHYS(max_pfn) - PFN_PHYS(min_low_pfn) + 0x200000;  // TODO: Avoid hardcoding 0x200000

	char *ptr = strchr(emu_cmdline, ':');
	if (ptr != NULL) {
		int i = 0;
		int tmp_ratio;
		char *token, *rest;
		rest = emu_cmdline;
		while ((token = strsep(&rest, ":")) != NULL) {
        	ret = kstrtoint(token, 0, &tmp_ratio);
        	if (ret)
            	return -EINVAL;
			node_cnt ++;
			ratio[i++] = tmp_ratio;
			total_ratio += tmp_ratio;
    	}
		if (node_cnt > MAX_NUMNODES)
			pr_err("NUMA emu: Node count is too large\n");
		
		for (i = 0; i < node_cnt; i++) {
			assigned_node_size[i] = ratio[i] * phy_mem_size / total_ratio;
			if(i == 0) assigned_node_size[i] -= 0x200000;
			pr_info("NUMA emu: Node Size = %#018Lx Node_Idx = %d Ratio = %d\n",
				assigned_node_size[i], i, ratio[i]);
		}
	}
	else {
		/*
	 		* Split the system RAM into N equal chunks.
	 	*/
		ret = kstrtoint(emu_cmdline, 0, &node_cnt);
		if (ret || node_cnt <= 0)
			return -EINVAL;
		if (node_cnt > MAX_NUMNODES)
			node_cnt = MAX_NUMNODES;
		node_size = phy_mem_size / node_cnt;
		pr_info("NUMA emu: Node Size = %#018Lx Node = %d\n",
			node_size, node_cnt);
		for (int i = 0; i < node_cnt; i++)
			assigned_node_size[i] = node_size;
	}
	
	for_each_memblock(memory, mblk)
		mblk_cnt++;

	/*
	 * Size the node count to match the memory block count to avoid
	 * splitting memory blocks across nodes. If there is only one
	 * memory block split it.
	 */
	if (mblk_cnt <= node_cnt) {
		pr_info("NUMA emu: Nodes (%d) >= Memblocks (%d)\n",
			node_cnt, mblk_cnt);
		if (mblk_cnt == 1) {
			split = true;
			pr_info("NUMA emu: Splitting single Memory Block\n");
		} else {
			node_cnt = mblk_cnt;
			pr_info("NUMA emu: Adjust Nodes = Memory Blocks\n");
		}
	}

	for_each_memblock(memory, mblk) {

		if (split) {
			for (node = 0; node < node_cnt; node++) {
				u64 start, end;
				u64 offset = 0;
				for (int i = 0; i < node; i++)
					offset += assigned_node_size[i];
				start = mblk->base + offset;
				end = start + assigned_node_size[node];
				pr_info("Adding an emulation node %d for [mem %#018Lx-%#018Lx]\n",
					node, start, end);
				ret = numa_add_memblk(node, start, end);
				if (!ret)
					continue;
				pr_err("NUMA emulation init failed\n");
				return ret;
			}
			break;
		}
		pr_info("Adding a emulation node %d for [mem %#018Lx-%#018Lx]\n",
			node, mblk->base, mblk->base + mblk->size);
		ret = numa_add_memblk(node, mblk->base,
				      mblk->base + mblk->size);
		if (!ret)
			continue;
		pr_err("NUMA emulation init failed\n");
		return ret;
	}
	pr_info("NUMA: added %d emulation nodes of %#018Lx size each\n",
		node_cnt, node_size);

	return 0;
}
