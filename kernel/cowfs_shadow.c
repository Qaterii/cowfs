#include "cowfs.h"
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/init_task.h>

#define COPY_BUF_SIZE (64 * 1024)

/*
 * .cowfs_shadow/ создаётся при монтировании с правами 0700 от root
 * (cowfs_shadow_init выполняется в контексте sudo mount). Операции с
 * теневым хранилищем — это внутренняя бухгалтерия cowfs и не должны
 * зависеть от прав пользователя, чья запись/удаление их вызвали,
 * иначе любой непривилегированный write/unlink проваливает COW
 * с -EACCES. Поэтому на время доступа к .cowfs_shadow подменяем
 * креды на root.
 */
static struct cred *cowfs_shadow_enter(const struct cred **old_cred)
{
    struct cred *new_cred = prepare_kernel_cred(&init_task);

    if (!new_cred)
        return NULL;
    *old_cred = override_creds(new_cred);
    return new_cred;
}

static void cowfs_shadow_exit(struct cred *new_cred,
                               const struct cred *old_cred)
{
    revert_creds(old_cred);
    put_cred(new_cred);
}

int cowfs_shadow_init(struct super_block *sb)
{
    struct cowfs_sb_info *sbi = COWFS_SB(sb);
    struct dentry *lower_root = sbi->lower_root;
    struct dentry *shadow_dentry;
    struct inode  *lower_dir_inode = d_inode(lower_root);
    int err = 0;

    inode_lock(lower_dir_inode);
    shadow_dentry = lookup_one_len(COWFS_SHADOW_DIR, lower_root,
                                   strlen(COWFS_SHADOW_DIR));
    if (IS_ERR(shadow_dentry)) {
        err = PTR_ERR(shadow_dentry);
        goto out_unlock;
    }

    if (d_is_negative(shadow_dentry)) {
        err = vfs_mkdir(&nop_mnt_idmap, lower_dir_inode,
                        shadow_dentry, 0700);
        if (err)
            pr_err("cowfs: failed to create shadow dir: %d\n", err);
        else
            pr_info("cowfs: created shadow dir\n");
    }
    dput(shadow_dentry);

out_unlock:
    inode_unlock(lower_dir_inode);
    return err;
}

void cowfs_shadow_cleanup(struct super_block *sb)
{
    pr_info("cowfs: shadow store preserved on unmount\n");
}

int cowfs_shadow_make_path(struct super_block *sb,
                            unsigned long ino, u64 ts,
                            char *buf, size_t bufsz)
{
    struct cowfs_sb_info *sbi = COWFS_SB(sb);
    return snprintf(buf, bufsz, "%s/%lu_%llu",
                    sbi->shadow_root, ino, ts);
}

int cowfs_shadow_copy_file(struct dentry *lower_dentry,
                            struct vfsmount *lower_mnt,
                            const char *shadow_path)
{
    struct file *src = NULL, *dst = NULL;
    char *buf;
    ssize_t bytes_read;
    loff_t src_pos = 0, dst_pos = 0;
    int err = 0;
    struct path lower_path = { .mnt = lower_mnt, .dentry = lower_dentry };
    const struct cred *old_cred;
    struct cred *new_cred = cowfs_shadow_enter(&old_cred);

    if (!new_cred)
        return -ENOMEM;

    src = dentry_open(&lower_path, O_RDONLY, current_cred());
    if (IS_ERR(src)) {
        err = PTR_ERR(src);
        goto out_cred;
    }

    dst = filp_open(shadow_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (IS_ERR(dst)) {
        err = PTR_ERR(dst);
        goto out_src;
    }

    buf = kmalloc(COPY_BUF_SIZE, GFP_KERNEL);
    if (!buf) {
        err = -ENOMEM;
        goto out_dst;
    }

    while ((bytes_read = kernel_read(src, buf, COPY_BUF_SIZE, &src_pos)) > 0) {
        ssize_t written = kernel_write(dst, buf, bytes_read, &dst_pos);
        if (written < 0) {
            err = written;
            goto out_buf;
        }
    }
    if (bytes_read < 0)
        err = bytes_read;

out_buf:
    kfree(buf);
out_dst:
    filp_close(dst, NULL);
out_src:
    filp_close(src, NULL);
out_cred:
    cowfs_shadow_exit(new_cred, old_cred);
    return err;
}

int cowfs_shadow_restore_file(const char *shadow_path,
                               struct dentry *lower_dentry,
                               struct vfsmount *lower_mnt)
{
    struct file *src = NULL, *dst = NULL;
    char *buf;
    ssize_t bytes_read;
    loff_t src_pos = 0, dst_pos = 0;
    int err = 0;
    struct path lower_path = { .mnt = lower_mnt, .dentry = lower_dentry };
    const struct cred *old_cred;
    struct cred *new_cred = cowfs_shadow_enter(&old_cred);

    if (!new_cred)
        return -ENOMEM;

    src = filp_open(shadow_path, O_RDONLY, 0);
    if (IS_ERR(src)) {
        err = PTR_ERR(src);
        goto out_cred;
    }

    dst = dentry_open(&lower_path, O_WRONLY | O_TRUNC, current_cred());
    if (IS_ERR(dst)) {
        err = PTR_ERR(dst);
        goto out_src;
    }

    err = vfs_truncate(&lower_path, 0);
    if (err)
        goto out_dst;

    buf = kmalloc(COPY_BUF_SIZE, GFP_KERNEL);
    if (!buf) {
        err = -ENOMEM;
        goto out_dst;
    }

    while ((bytes_read = kernel_read(src, buf, COPY_BUF_SIZE, &src_pos)) > 0) {
        ssize_t written = kernel_write(dst, buf, bytes_read, &dst_pos);
        if (written < 0) {
            err = written;
            goto out_buf;
        }
    }
    if (bytes_read < 0)
        err = bytes_read;

out_buf:
    kfree(buf);
out_dst:
    filp_close(dst, NULL);
out_src:
    filp_close(src, NULL);
out_cred:
    cowfs_shadow_exit(new_cred, old_cred);
    return err;
}

void cowfs_shadow_remove(const char *shadow_path)
{
    struct path path;
    const struct cred *old_cred;
    struct cred *new_cred = cowfs_shadow_enter(&old_cred);
    int err;

    if (!new_cred)
        return;

    err = kern_path(shadow_path, 0, &path);
    if (!err) {
        struct inode *dir = d_inode(path.dentry->d_parent);

        inode_lock(dir);
        err = vfs_unlink(&nop_mnt_idmap, dir, path.dentry, NULL);
        inode_unlock(dir);
        path_put(&path);
        if (err)
            pr_warn("cowfs: failed to remove shadow %s: %d\n",
                    shadow_path, err);
        else
            pr_info("cowfs: removed shadow %s\n", shadow_path);
    } else {
        pr_info("cowfs: shadow_remove: kern_path('%s') = %d\n", shadow_path, err);
    }

    cowfs_shadow_exit(new_cred, old_cred);
}
