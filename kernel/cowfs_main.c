#include "cowfs.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("COW filesystem with rollback support");
MODULE_VERSION("1.0");

ulong cowfs_window_seconds = 300;
ulong cowfs_gc_interval    = 60;

module_param_named(window_seconds, cowfs_window_seconds, ulong, 0644);
MODULE_PARM_DESC(window_seconds, "How long to keep snapshots (seconds)");

module_param_named(gc_interval, cowfs_gc_interval, ulong, 0644);
MODULE_PARM_DESC(gc_interval, "GC run interval (seconds)");

static struct file_system_type cowfs_type = {
    .name    = "cowfs",
    .mount   = cowfs_mount,
    .kill_sb = kill_anon_super,
    .owner   = THIS_MODULE,
};

static int __init cowfs_init(void)
{
    int err;

    pr_info("cowfs: initializing\n");

    err = cowfs_versions_init();
    if (err) {
        pr_err("cowfs: failed to init versions table: %d\n", err);
        return err;
    }

    err = cowfs_ctl_init();
    if (err) {
        pr_err("cowfs: failed to init ctl device: %d\n", err);
        cowfs_versions_destroy();
        return err;
    }

    err = register_filesystem(&cowfs_type);
    if (err) {
        pr_err("cowfs: failed to register filesystem: %d\n", err);
        cowfs_ctl_exit();
        cowfs_versions_destroy();
        return err;
    }

    pr_info("cowfs: ready. window=%lus gc_interval=%lus\n",
            cowfs_window_seconds, cowfs_gc_interval);
    return 0;
}

static void __exit cowfs_exit(void)
{
    pr_info("cowfs: unloading\n");
    unregister_filesystem(&cowfs_type);
    cowfs_ctl_exit();
    cowfs_versions_destroy();
    pr_info("cowfs: unloaded\n");
}

module_init(cowfs_init);
module_exit(cowfs_exit);
