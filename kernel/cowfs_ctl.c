#include "cowfs.h"
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/string.h>

static int path_to_lower_ino(const char *path, unsigned long *ino_out,
                               struct super_block **sb_out)
{
    struct path p;
    int err;

    err = kern_path(path, LOOKUP_FOLLOW, &p);
    if (err)
        return err;

    if (p.dentry->d_sb->s_magic != COWFS_MAGIC) {
        path_put(&p);
        return -EINVAL;
    }

    *ino_out = d_inode(cowfs_lower_dentry(p.dentry))->i_ino;
    *sb_out  = p.dentry->d_sb;
    path_put(&p);
    return 0;
}

/*
 * Откат удаления (COW_OP_UNLINK), когда путь уже не существует —
 * path_to_lower_ino() не может его разрешить, поэтому ino-ключ версии
 * неизвестен. Ищем версию по исходному имени файла и пересоздаём его
 * в родительском каталоге.
 */
static int rollback_deleted(const char *path, u64 timestamp)
{
    struct cow_version *v;
    struct path parent_path;
    struct dentry *lower_parent, *new_dentry;
    struct inode *lower_dir;
    struct cowfs_sb_info *sbi;
    char dirbuf[512];
    char *slash;
    const char *base;
    int err;

    strscpy(dirbuf, path, sizeof(dirbuf));
    slash = strrchr(dirbuf, '/');
    if (!slash)
        return -EINVAL;
    base = path + (slash - dirbuf) + 1;
    if (!*base)
        return -EINVAL;
    if (slash == dirbuf)
        dirbuf[1] = '\0';
    else
        *slash = '\0';

    v = cowfs_version_find_deleted(base, timestamp);
    if (!v)
        return -ENOENT;

    err = kern_path(dirbuf, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent_path);
    if (err)
        return err;

    if (parent_path.dentry->d_sb->s_magic != COWFS_MAGIC) {
        path_put(&parent_path);
        return -EINVAL;
    }

    sbi = COWFS_SB(parent_path.dentry->d_sb);
    lower_parent = cowfs_lower_dentry(parent_path.dentry);
    lower_dir = d_inode(lower_parent);

    inode_lock(lower_dir);
    new_dentry = lookup_one_len(base, lower_parent, strlen(base));
    if (!IS_ERR(new_dentry)) {
        err = vfs_create(&nop_mnt_idmap, lower_dir, new_dentry,
                          v->saved_stat.mode, false);
        if (!err && v->has_data)
            cowfs_shadow_restore_file(v->shadow_path, new_dentry, sbi->lower_mnt);
        dput(new_dentry);
    } else {
        err = PTR_ERR(new_dentry);
    }
    inode_unlock(lower_dir);

    path_put(&parent_path);
    return err;
}

static long cowfs_ctl_ioctl(struct file *file, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd) {

    case COWFS_IOC_LIST: {
        struct cowfs_list_req __user *ureq = (void __user *)arg;
        struct cowfs_list_req *req;
        struct cow_version *versions[COWFS_IOC_MAX_VERSIONS];
        unsigned long ino;
        struct super_block *sb;
        int i, count;
        long ret = 0;

        req = kzalloc(sizeof(*req), GFP_KERNEL);
        if (!req)
            return -ENOMEM;

        if (copy_from_user(req, ureq, sizeof(*req))) {
            ret = -EFAULT;
            goto list_out;
        }

        if (path_to_lower_ino(req->path, &ino, &sb)) {
            ret = -ENOENT;
            goto list_out;
        }

        count = cowfs_version_list(ino, versions,
                                    min_t(u32, req->max_count, COWFS_IOC_MAX_VERSIONS));
        req->found_count = count;

        for (i = 0; i < count; i++) {
            req->versions[i].timestamp = versions[i]->timestamp;
            req->versions[i].op_type   = versions[i]->op_type;
            strscpy(req->versions[i].shadow_path,
                    versions[i]->shadow_path,
                    sizeof(req->versions[i].shadow_path));
        }

        if (copy_to_user(ureq, req, sizeof(*req)))
            ret = -EFAULT;

list_out:
        kfree(req);
        return ret;
    }

    case COWFS_IOC_ROLLBACK: {
        struct cowfs_rollback_req __user *ureq = (void __user *)arg;
        struct cowfs_rollback_req req;
        struct cow_version *v;
        struct path target_path;
        unsigned long ino;
        struct super_block *sb;
        int err;

        if (copy_from_user(&req, ureq, sizeof(req)))
            return -EFAULT;

        err = path_to_lower_ino(req.path, &ino, &sb);
        if (err == -ENOENT)
            return rollback_deleted(req.path, req.timestamp);
        if (err)
            return err;

        v = cowfs_version_find(ino, req.timestamp);
        if (!v)
            return -ENOENT;

        err = kern_path(req.path, LOOKUP_FOLLOW, &target_path);
        if (err)
            return err;

        if (target_path.dentry->d_sb->s_magic != COWFS_MAGIC) {
            path_put(&target_path);
            return -EINVAL;
        }

        {
            struct dentry *lower  = cowfs_lower_dentry(target_path.dentry);
            struct cowfs_sb_info *sbi = COWFS_SB(sb);

            if (v->op_type == COW_OP_UNLINK) {
                /* Файл был удалён — воссоздать из теневой копии */
                struct inode *lower_dir = d_inode(lower->d_parent);
                struct dentry *new_dentry;

                inode_lock(lower_dir);
                new_dentry = lookup_one_len(v->orig_name,
                                             lower->d_parent,
                                             strlen(v->orig_name));
                if (!IS_ERR(new_dentry)) {
                    err = vfs_create(&nop_mnt_idmap, lower_dir,
                                     new_dentry, v->saved_stat.mode, false);
                    if (!err && v->has_data)
                        cowfs_shadow_restore_file(v->shadow_path,
                                                   new_dentry,
                                                   sbi->lower_mnt);
                    dput(new_dentry);
                }
                inode_unlock(lower_dir);

            } else if (v->has_data) {
                /* WRITE: восстановить содержимое */
                err = cowfs_shadow_restore_file(v->shadow_path,
                                                 lower, sbi->lower_mnt);

            } else if (v->op_type == COW_OP_RENAME) {
                /* Вернуть файлу исходное имя */
                struct inode *dir_inode = d_inode(lower->d_parent);
                struct dentry *old_name_dentry;

                inode_lock(dir_inode);
                old_name_dentry = lookup_one_len(v->orig_name,
                                                  lower->d_parent,
                                                  strlen(v->orig_name));
                if (!IS_ERR(old_name_dentry)) {
                    struct renamedata rd = {
                        .old_mnt_idmap = &nop_mnt_idmap,
                        .old_dir       = dir_inode,
                        .old_dentry    = lower,
                        .new_mnt_idmap = &nop_mnt_idmap,
                        .new_dir       = dir_inode,
                        .new_dentry    = old_name_dentry,
                    };
                    err = vfs_rename(&rd);
                    dput(old_name_dentry);
                } else {
                    err = PTR_ERR(old_name_dentry);
                }
                inode_unlock(dir_inode);
            }

            if (!err && v->op_type == COW_OP_SETATTR) {
                /* Восстановить атрибуты */
                struct iattr attr = {
                    .ia_valid = ATTR_UID | ATTR_GID | ATTR_MODE,
                    .ia_uid   = v->saved_stat.uid,
                    .ia_gid   = v->saved_stat.gid,
                    .ia_mode  = v->saved_stat.mode,
                };
                inode_lock(d_inode(lower));
                notify_change(&nop_mnt_idmap, lower, &attr, NULL);
                inode_unlock(d_inode(lower));
            }
        }
        path_put(&target_path);
        return err;
    }

    default:
        return -ENOTTY;
    }
}

static const struct file_operations cowfs_ctl_fops = {
    .unlocked_ioctl = cowfs_ctl_ioctl,
    .owner          = THIS_MODULE,
};

static struct miscdevice cowfs_ctl_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = COWFS_DEV_NAME,
    .fops  = &cowfs_ctl_fops,
};

int cowfs_ctl_init(void)
{
    return misc_register(&cowfs_ctl_dev);
}

void cowfs_ctl_exit(void)
{
    misc_deregister(&cowfs_ctl_dev);
}
