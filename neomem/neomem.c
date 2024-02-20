#include "neomem.h"

static int neomem_hotpage_migration_turn(void)
{
	int err;
	err = neomem_start();
	return 0;
}


static int __init neomem_hotpage_migration_init(void)
{
    int err;
    pr_debug("kneomemd neomem_hotpage_migration_init\n");

    err = neomem_hotpage_migration_turn();
    return err;
}
late_initcall(neomem_hotpage_migration_init);