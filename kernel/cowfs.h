#ifndef _COWFS_H
#define _COWFS_H

#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/rhashtable.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/stat.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>

#define COWFS_MAGIC         0xC0WBEE15
#define COWFS_SHADOW_DIR    ".cowfs_shadow"
#define COWFS_MAX_VERSIONS  64
#define COWFS_DEV_NAME      "cowfs_ctl"

enum cow_op {
    COW_OP_WRITE   = 1,
    COW_OP_UNLINK  = 2,
    COW_OP_RENAME  = 3,
    COW_OP_SETATTR = 4,
};

struct cow_version {
    u64              timestamp;
    enum cow_op      op_type;
    char             shadow_path[256];
    char             orig_name[256];
    struct kstat     saved_stat;
    bool             has_data;
    struct list_head node;
};

struct cow_inode_info {
    unsigned long    lower_ino;
    struct list_head versions;
    int              version_count;
    spinlock_t       lock;
    struct hlist_node hash_node;
    struct rcu_head  rcu;
};

struct cowfs_sb_info {
    struct vfsmount *lower_mnt;
    struct dentry   *lower_root;
    char             shadow_root[512];
};

struct cowfs_dentry_info {
    struct dentry   *lower_dentry;
    struct vfsmount *lower_mnt;
};

struct cowfs_file_info {
    struct file *lower_file;
    bool         snapshot_taken;
};

static inline struct cowfs_sb_info *COWFS_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static inline struct cowfs_dentry_info *COWFS_D(struct dentry *d)
{
    return d->d_fsdata;
}

static inline struct cowfs_file_info *COWFS_F(struct file *f)
{
    return f->private_data;
}

static inline struct dentry *cowfs_lower_dentry(struct dentry *d)
{
    return COWFS_D(d)->lower_dentry;
}

static inline struct file *cowfs_lower_file(struct file *f)
{
    return COWFS_F(f)->lower_file;
}

extern ulong cowfs_window_seconds;
extern ulong cowfs_gc_interval;

/* cowfs_super.c */
int cowfs_fill_super(struct super_block *sb, void *data, int silent);
struct dentry *cowfs_mount(struct file_system_type *fs_type,
                            int flags, const char *dev_name, void *data);
void cowfs_put_super(struct super_block *sb);
int  cowfs_statfs(struct dentry *dentry, struct kstatfs *buf);
int  cowfs_show_options(struct seq_file *m, struct dentry *root);

/* cowfs_inode.c */
extern const struct inode_operations cowfs_file_iops;
extern const struct inode_operations cowfs_dir_iops;
extern const struct inode_operations cowfs_symlink_iops;
struct inode *cowfs_get_inode(struct super_block *sb, struct inode *lower_inode);

/* cowfs_file.c */
extern const struct file_operations cowfs_file_fops;

/* cowfs_dir.c */
extern const struct file_operations cowfs_dir_fops;
extern const struct dentry_operations cowfs_dops;

/* cowfs_shadow.c */
int  cowfs_shadow_init(struct super_block *sb);
void cowfs_shadow_cleanup(struct super_block *sb);
int  cowfs_shadow_copy_file(struct dentry *lower_dentry,
                             struct vfsmount *lower_mnt,
                             const char *shadow_path);
int  cowfs_shadow_restore_file(const char *shadow_path,
                                struct dentry *lower_dentry,
                                struct vfsmount *lower_mnt);
void cowfs_shadow_remove(const char *shadow_path);
int  cowfs_shadow_make_path(struct super_block *sb,
                             unsigned long ino, u64 ts,
                             char *buf, size_t bufsz);

/* cowfs_versions.c */
int  cowfs_versions_init(void);
void cowfs_versions_destroy(void);
struct cow_version *cowfs_version_alloc(enum cow_op op);
void cowfs_version_free(struct cow_version *v);
int  cowfs_version_add(unsigned long ino, struct cow_version *v);
int  cowfs_version_list(unsigned long ino,
                         struct cow_version **out, int max_count);
struct cow_version *cowfs_version_find(unsigned long ino, u64 timestamp);
void cowfs_version_gc(void);
void cowfs_versions_gc_all(void);

/* cowfs_ctl.c */
int  cowfs_ctl_init(void);
void cowfs_ctl_exit(void);

/* ioctl structures (shared with userspace) */
#define COWFS_IOC_MAGIC 'C'

struct cowfs_version_info {
    u64  timestamp;
    u32  op_type;
    char shadow_path[256];
};

struct cowfs_list_req {
    char path[512];
    u32  max_count;
    u32  found_count;
    struct cowfs_version_info versions[64];
};

struct cowfs_rollback_req {
    char path[512];
    u64  timestamp;
};

#define COWFS_IOC_LIST     _IOWR(COWFS_IOC_MAGIC, 1, struct cowfs_list_req)
#define COWFS_IOC_ROLLBACK _IOW(COWFS_IOC_MAGIC,  2, struct cowfs_rollback_req)

#endif /* _COWFS_H */
