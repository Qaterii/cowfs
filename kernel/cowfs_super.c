#include "cowfs.h"
#include <linux/mount.h>
#include <linux/parser.h>
#include <linux/seq_file.h>

enum { OPT_LOWERDIR, OPT_ERR };

static const match_table_t cowfs_tokens = {
    { OPT_LOWERDIR, "lowerdir=%s" },
    { OPT_ERR,      NULL },
};

static const struct super_operations cowfs_sops = {
    .put_super    = cowfs_put_super,
    .statfs       = cowfs_statfs,
    .show_options = cowfs_show_options,
};

void cowfs_put_super(struct super_block *sb)
{
    struct cowfs_sb_info *sbi = COWFS_SB(sb);
    if (sbi) {
        cowfs_shadow_cleanup(sb);
        if (sbi->lower_mnt)
            mntput(sbi->lower_mnt);
        if (sbi->lower_root)
            dput(sbi->lower_root);
        kfree(sbi);
        sb->s_fs_info = NULL;
    }
}

int cowfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct cowfs_sb_info *sbi = COWFS_SB(dentry->d_sb);
    struct path lower_path = {
        .mnt    = sbi->lower_mnt,
        .dentry = sbi->lower_root,
    };
    return vfs_statfs(&lower_path, buf);
}

int cowfs_show_options(struct seq_file *m, struct dentry *root)
{
    struct cowfs_sb_info *sbi = COWFS_SB(root->d_sb);
    seq_printf(m, ",lowerdir=%s", sbi->shadow_root);
    return 0;
}

static char *parse_options(char *options)
{
    char *p, *lowerdir = NULL;
    substring_t args[MAX_OPT_ARGS];
    int token;

    while ((p = strsep(&options, ",")) != NULL) {
        if (!*p)
            continue;
        token = match_token(p, cowfs_tokens, args);
        if (token == OPT_LOWERDIR)
            lowerdir = match_strdup(&args[0]);
    }
    return lowerdir;
}

int cowfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct cowfs_sb_info *sbi;
    struct inode *root_inode;
    struct dentry *root_dentry;
    struct path lower_path;
    char *lowerdir;
    int err;

    lowerdir = parse_options((char *)data);
    if (!lowerdir) {
        pr_err("cowfs: missing lowerdir= option\n");
        return -EINVAL;
    }

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi) {
        kfree(lowerdir);
        return -ENOMEM;
    }
    sb->s_fs_info = sbi;

    err = kern_path(lowerdir, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &lower_path);
    if (err) {
        pr_err("cowfs: lowerdir '%s' not found: %d\n", lowerdir, err);
        goto err_sbi;
    }

    sbi->lower_mnt  = mntget(lower_path.mnt);
    sbi->lower_root = dget(lower_path.dentry);
    snprintf(sbi->shadow_root, sizeof(sbi->shadow_root),
             "%s/" COWFS_SHADOW_DIR, lowerdir);
    path_put(&lower_path);
    kfree(lowerdir);

    sb->s_magic    = COWFS_MAGIC;
    sb->s_op       = &cowfs_sops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;

    root_inode = cowfs_get_inode(sb, d_inode(sbi->lower_root));
    if (IS_ERR(root_inode)) {
        err = PTR_ERR(root_inode);
        goto err_mnt;
    }

    root_dentry = d_make_root(root_inode);
    if (!root_dentry) {
        err = -ENOMEM;
        goto err_mnt;
    }

    root_dentry->d_fsdata = kzalloc(sizeof(struct cowfs_dentry_info),
                                    GFP_KERNEL);
    if (!root_dentry->d_fsdata) {
        err = -ENOMEM;
        dput(root_dentry);
        goto err_mnt;
    }
    COWFS_D(root_dentry)->lower_dentry = dget(sbi->lower_root);
    COWFS_D(root_dentry)->lower_mnt    = mntget(sbi->lower_mnt);

    sb->s_root = root_dentry;

    err = cowfs_shadow_init(sb);
    if (err)
        pr_warn("cowfs: shadow init failed: %d (continuing)\n", err);

    pr_info("cowfs: mounted over %s\n", sbi->shadow_root);
    return 0;

err_mnt:
    mntput(sbi->lower_mnt);
    dput(sbi->lower_root);
err_sbi:
    kfree(sbi);
    sb->s_fs_info = NULL;
    return err;
}

struct dentry *cowfs_mount(struct file_system_type *fs_type,
                            int flags, const char *dev_name, void *data)
{
    return mount_nodev(fs_type, flags, data, cowfs_fill_super);
}
