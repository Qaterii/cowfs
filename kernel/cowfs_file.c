#include "cowfs.h"
#include <linux/uio.h>
#include <linux/file.h>

static int cowfs_open(struct inode *inode, struct file *file)
{
    struct cowfs_file_info *fi;
    struct dentry *lower_dentry = cowfs_lower_dentry(file->f_path.dentry);
    struct cowfs_sb_info *sbi   = COWFS_SB(inode->i_sb);
    struct path lower_path      = { .mnt = sbi->lower_mnt,
                                    .dentry = lower_dentry };
    struct file *lower_file;

    fi = kzalloc(sizeof(*fi), GFP_KERNEL);
    if (!fi)
        return -ENOMEM;

    lower_file = dentry_open(&lower_path, file->f_flags, current_cred());
    if (IS_ERR(lower_file)) {
        kfree(fi);
        return PTR_ERR(lower_file);
    }

    fi->lower_file    = lower_file;
    file->private_data = fi;
    return 0;
}

static int cowfs_release(struct inode *inode, struct file *file)
{
    struct cowfs_file_info *fi = COWFS_F(file);
    if (fi) {
        filp_close(fi->lower_file, NULL);
        kfree(fi);
    }
    return 0;
}

static ssize_t cowfs_read(struct file *file, char __user *buf,
                           size_t count, loff_t *ppos)
{
    struct file *lower_file = cowfs_lower_file(file);
    return vfs_read(lower_file, buf, count, ppos);
}

static ssize_t cowfs_write(struct file *file, const char __user *buf,
                            size_t count, loff_t *ppos)
{
    struct file *lower_file      = cowfs_lower_file(file);
    struct dentry *lower_dentry  = lower_file->f_path.dentry;
    struct super_block *sb       = file->f_path.dentry->d_sb;
    struct cowfs_file_info *fi   = COWFS_F(file);
    char shadow_path[256];
    int err;

    /* COW: снять снимок один раз при первой записи в этот open */
    if (!fi->snapshot_taken && (file->f_flags & O_WRONLY ||
                                  file->f_flags & O_RDWR)) {
        fi->snapshot_taken = true;

        struct cow_version *v = cowfs_version_alloc(COW_OP_WRITE);
        if (v) {
            unsigned long ino = d_inode(lower_dentry)->i_ino;
            cowfs_shadow_make_path(sb, ino, v->timestamp,
                                    shadow_path, sizeof(shadow_path));
            err = cowfs_shadow_copy_file(lower_dentry,
                                          COWFS_SB(sb)->lower_mnt,
                                          shadow_path);
            if (!err) {
                strscpy(v->shadow_path, shadow_path, sizeof(v->shadow_path));
                v->has_data = true;
                vfs_getattr(&lower_file->f_path, &v->saved_stat,
                            STATX_BASIC_STATS, 0);
                cowfs_version_add(ino, v);
            } else {
                pr_warn("cowfs: COW copy failed for write: %d\n", err);
                cowfs_version_free(v);
            }
        }
    }

    return vfs_write(lower_file, buf, count, ppos);
}

static loff_t cowfs_llseek(struct file *file, loff_t offset, int whence)
{
    struct file *lower_file = cowfs_lower_file(file);
    loff_t ret = vfs_llseek(lower_file, offset, whence);
    file->f_pos = lower_file->f_pos;
    return ret;
}

static int cowfs_fsync(struct file *file, loff_t start,
                        loff_t end, int datasync)
{
    return vfs_fsync_range(cowfs_lower_file(file), start, end, datasync);
}

static int cowfs_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct file *lower_file = cowfs_lower_file(file);
    vma->vm_file = get_file(lower_file);
    fput(file);
    return lower_file->f_op->mmap(lower_file, vma);
}

const struct file_operations cowfs_file_fops = {
    .open    = cowfs_open,
    .release = cowfs_release,
    .read    = cowfs_read,
    .write   = cowfs_write,
    .llseek  = cowfs_llseek,
    .fsync   = cowfs_fsync,
    .mmap    = cowfs_mmap,
};
