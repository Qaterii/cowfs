#include "cowfs.h"
#include <linux/namei.h>
#include <linux/xattr.h>

struct inode *cowfs_get_inode(struct super_block *sb,
                               struct inode *lower_inode)
{
    struct inode *inode;

    if (!lower_inode)
        return ERR_PTR(-EACCES);

    inode = iget_locked(sb, lower_inode->i_ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    if (!(inode->i_state & I_NEW))
        return inode;

    inode->i_mode  = lower_inode->i_mode;
    inode->i_uid   = lower_inode->i_uid;
    inode->i_gid   = lower_inode->i_gid;
    inode->i_atime = lower_inode->i_atime;
    inode->i_mtime = lower_inode->i_mtime;
    inode_set_ctime_to_ts(inode, inode_get_ctime(lower_inode));
    set_nlink(inode, lower_inode->i_nlink);
    inode->i_size   = lower_inode->i_size;
    inode->i_blocks = lower_inode->i_blocks;

    if (S_ISDIR(lower_inode->i_mode)) {
        inode->i_op  = &cowfs_dir_iops;
        inode->i_fop = &cowfs_dir_fops;
    } else if (S_ISLNK(lower_inode->i_mode)) {
        inode->i_op  = &cowfs_symlink_iops;
    } else {
        inode->i_op  = &cowfs_file_iops;
        inode->i_fop = &cowfs_file_fops;
    }

    unlock_new_inode(inode);
    return inode;
}

static struct dentry *cowfs_lookup(struct inode *dir,
                                    struct dentry *dentry,
                                    unsigned int flags)
{
    struct dentry *lower_dentry, *lower_dir_dentry;
    struct inode  *lower_dir_inode;
    struct inode  *lower_inode;
    struct inode  *inode = NULL;
    struct cowfs_dentry_info *di;
    int err = 0;

    lower_dir_dentry = cowfs_lower_dentry(dentry->d_parent);
    lower_dir_inode  = d_inode(lower_dir_dentry);

    /* lookup_one_len() требует залоченный родительский inode (WARN_ON_ONCE) */
    inode_lock_shared(lower_dir_inode);
    lower_dentry = lookup_one_len(dentry->d_name.name,
                                   lower_dir_dentry,
                                   dentry->d_name.len);
    inode_unlock_shared(lower_dir_inode);
    if (IS_ERR(lower_dentry))
        return ERR_CAST(lower_dentry);

    di = kzalloc(sizeof(*di), GFP_KERNEL);
    if (!di) {
        dput(lower_dentry);
        return ERR_PTR(-ENOMEM);
    }
    di->lower_dentry = lower_dentry;
    di->lower_mnt    = COWFS_SB(dentry->d_sb)->lower_mnt;
    dentry->d_fsdata = di;
    d_set_d_op(dentry, &cowfs_dops);

    lower_inode = d_inode(lower_dentry);
    if (lower_inode) {
        inode = cowfs_get_inode(dentry->d_sb, lower_inode);
        if (IS_ERR(inode)) {
            err = PTR_ERR(inode);
            dput(lower_dentry);
            kfree(di);
            dentry->d_fsdata = NULL;
            return ERR_PTR(err);
        }
    }

    d_add(dentry, inode);
    return NULL;
}

static int cowfs_create(struct mnt_idmap *idmap,
                         struct inode *dir, struct dentry *dentry,
                         umode_t mode, bool excl)
{
    struct dentry *lower_dentry = cowfs_lower_dentry(dentry);
    struct inode  *lower_dir    = d_inode(cowfs_lower_dentry(dentry->d_parent));
    int err;

    err = vfs_create(&nop_mnt_idmap, lower_dir, lower_dentry, mode, excl);
    if (!err) {
        struct inode *inode = cowfs_get_inode(dentry->d_sb,
                                               d_inode(lower_dentry));
        if (!IS_ERR(inode))
            d_instantiate(dentry, inode);
        else
            err = PTR_ERR(inode);
    }
    return err;
}

static int cowfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct dentry *lower_dentry     = cowfs_lower_dentry(dentry);
    struct dentry *lower_dir_dentry = cowfs_lower_dentry(dentry->d_parent);
    struct inode  *lower_dir_inode  = d_inode(lower_dir_dentry);
    struct super_block *sb          = dentry->d_sb;
    struct cow_version *v;
    char shadow_path[256];
    int err;

    /* COW: сохранить копию файла перед удалением */
    v = cowfs_version_alloc(COW_OP_UNLINK);
    if (v) {
        unsigned long ino = d_inode(lower_dentry)->i_ino;
        cowfs_shadow_make_path(sb, ino, v->timestamp,
                                shadow_path, sizeof(shadow_path));
        err = cowfs_shadow_copy_file(lower_dentry,
                                      COWFS_SB(sb)->lower_mnt,
                                      shadow_path);
        if (!err) {
            strscpy(v->shadow_path, shadow_path, sizeof(v->shadow_path));
            strscpy(v->orig_name, dentry->d_name.name, sizeof(v->orig_name));
            v->has_data = true;
            vfs_getattr(&(struct path){
                .mnt = COWFS_SB(sb)->lower_mnt,
                .dentry = lower_dentry }, &v->saved_stat, STATX_BASIC_STATS, 0);
            cowfs_version_add(ino, v);
        } else {
            pr_warn("cowfs: COW copy failed for unlink: %d\n", err);
            cowfs_version_free(v);
        }
    }

    inode_lock(lower_dir_inode);
    err = vfs_unlink(&nop_mnt_idmap, lower_dir_inode, lower_dentry, NULL);
    inode_unlock(lower_dir_inode);
    if (!err)
        d_drop(dentry);
    return err;
}

static int cowfs_rename(struct mnt_idmap *idmap,
                         struct inode *old_dir, struct dentry *old_dentry,
                         struct inode *new_dir, struct dentry *new_dentry,
                         unsigned int flags)
{
    struct dentry *lower_old_dentry = cowfs_lower_dentry(old_dentry);
    struct dentry *lower_new_dentry = cowfs_lower_dentry(new_dentry);
    struct dentry *lower_old_dir    = cowfs_lower_dentry(old_dentry->d_parent);
    struct dentry *lower_new_dir    = cowfs_lower_dentry(new_dentry->d_parent);
    struct super_block *sb          = old_dentry->d_sb;
    struct cow_version *v;
    int err;

    /* COW: сохранить старое имя и метаданные */
    v = cowfs_version_alloc(COW_OP_RENAME);
    if (v) {
        unsigned long ino = d_inode(lower_old_dentry)->i_ino;
        strscpy(v->orig_name, old_dentry->d_name.name, sizeof(v->orig_name));
        vfs_getattr(&(struct path){
            .mnt = COWFS_SB(sb)->lower_mnt,
            .dentry = lower_old_dentry }, &v->saved_stat, STATX_BASIC_STATS, 0);
        v->has_data = false;
        cowfs_version_add(ino, v);
    }

    {
        struct renamedata rd = {
            .old_mnt_idmap = &nop_mnt_idmap,
            .old_dir       = d_inode(lower_old_dir),
            .old_dentry    = lower_old_dentry,
            .new_mnt_idmap = &nop_mnt_idmap,
            .new_dir       = d_inode(lower_new_dir),
            .new_dentry    = lower_new_dentry,
            .flags         = flags,
        };
        err = vfs_rename(&rd);
    }
    return err;
}

static int cowfs_setattr(struct mnt_idmap *idmap,
                          struct dentry *dentry, struct iattr *attr)
{
    struct dentry *lower_dentry = cowfs_lower_dentry(dentry);
    struct inode  *lower_inode  = d_inode(lower_dentry);
    struct super_block *sb = dentry->d_sb;
    struct cow_version *v;
    struct iattr lower_attr;
    int err;

    /* COW: сохранить текущие атрибуты */
    v = cowfs_version_alloc(COW_OP_SETATTR);
    if (v) {
        unsigned long ino = lower_inode->i_ino;
        vfs_getattr(&(struct path){
            .mnt = COWFS_SB(sb)->lower_mnt,
            .dentry = lower_dentry }, &v->saved_stat, STATX_BASIC_STATS, 0);
        v->has_data = false;
        cowfs_version_add(ino, v);
    }

    /*
     * ATTR_FILE/ATTR_OPEN и attr->ia_file (если заданы, например при
     * O_TRUNC) указывают на верхний (cowfs) struct file и не имеют
     * смысла для нижней ФС. Кроме того, notify_change() требует, чтобы
     * inode был залочен (WARN_ON_ONCE -> panic при panic_on_warn=1
     * на WSL2), а нижний inode мы ещё не лочили.
     */
    lower_attr = *attr;
    lower_attr.ia_valid &= ~(ATTR_FILE | ATTR_OPEN);

    inode_lock(lower_inode);
    err = notify_change(&nop_mnt_idmap, lower_dentry, &lower_attr, NULL);
    inode_unlock(lower_inode);

    if (!err) {
        struct inode *inode = d_inode(dentry);
        inode->i_uid   = lower_inode->i_uid;
        inode->i_gid   = lower_inode->i_gid;
        inode->i_mode  = lower_inode->i_mode;
        inode->i_size  = lower_inode->i_size;
        inode->i_mtime = lower_inode->i_mtime;
        inode->i_atime = lower_inode->i_atime;
    }
    return err;
}

static int cowfs_mkdir(struct mnt_idmap *idmap,
                        struct inode *dir, struct dentry *dentry, umode_t mode)
{
    struct dentry *lower_dentry = cowfs_lower_dentry(dentry);
    struct inode  *lower_dir    = d_inode(cowfs_lower_dentry(dentry->d_parent));
    int err;

    err = vfs_mkdir(&nop_mnt_idmap, lower_dir, lower_dentry, mode);
    if (!err) {
        struct inode *inode = cowfs_get_inode(dentry->d_sb,
                                               d_inode(lower_dentry));
        if (!IS_ERR(inode))
            d_instantiate(dentry, inode);
        else
            err = PTR_ERR(inode);
    }
    return err;
}

static int cowfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct dentry *lower_dentry     = cowfs_lower_dentry(dentry);
    struct dentry *lower_dir_dentry = cowfs_lower_dentry(dentry->d_parent);
    struct inode  *lower_dir_inode  = d_inode(lower_dir_dentry);
    int err;

    inode_lock(lower_dir_inode);
    err = vfs_rmdir(&nop_mnt_idmap, lower_dir_inode, lower_dentry);
    inode_unlock(lower_dir_inode);
    if (!err)
        d_drop(dentry);
    return err;
}

static int cowfs_symlink(struct mnt_idmap *idmap,
                          struct inode *dir, struct dentry *dentry,
                          const char *symname)
{
    struct dentry *lower_dentry = cowfs_lower_dentry(dentry);
    struct inode  *lower_dir    = d_inode(cowfs_lower_dentry(dentry->d_parent));
    return vfs_symlink(&nop_mnt_idmap, lower_dir, lower_dentry, symname);
}

static int cowfs_link(struct dentry *old_dentry, struct inode *dir,
                       struct dentry *new_dentry)
{
    struct dentry *lower_old = cowfs_lower_dentry(old_dentry);
    struct dentry *lower_new = cowfs_lower_dentry(new_dentry);
    struct inode  *lower_dir = d_inode(cowfs_lower_dentry(new_dentry->d_parent));
    return vfs_link(lower_old, &nop_mnt_idmap, lower_dir, lower_new, NULL);
}

static const char *cowfs_get_link(struct dentry *dentry,
                                   struct inode *inode,
                                   struct delayed_call *done)
{
    struct dentry *lower_dentry;
    if (!dentry)
        return ERR_PTR(-ECHILD);
    lower_dentry = cowfs_lower_dentry(dentry);
    return vfs_get_link(lower_dentry, done);
}

static int cowfs_getattr(struct mnt_idmap *idmap,
                          const struct path *path,
                          struct kstat *stat, u32 request_mask,
                          unsigned int query_flags)
{
    struct dentry *lower_dentry = cowfs_lower_dentry(path->dentry);
    struct cowfs_sb_info *sbi = COWFS_SB(path->dentry->d_sb);
    struct path lower_path = { .mnt = sbi->lower_mnt, .dentry = lower_dentry };

    /*
     * vfs_getattr_nosec() (вызывающая ->getattr для пути /mnt/cow)
     * передаёт сюда query_flags с битом AT_GETATTR_NOSEC. Обычный
     * vfs_getattr() начинается с WARN_ON_ONCE(query_flags &
     * AT_GETATTR_NOSEC) -> -EPERM, поэтому для нижнего пути нужно
     * использовать nosec-вариант (без повторной security-проверки).
     */
    return vfs_getattr_nosec(&lower_path, stat, request_mask, query_flags);
}

/*
 * Кастомный .permission не используется: VFS падает обратно на
 * generic_permission(), который проверяет права по inode->i_mode/
 * i_uid/i_gid — эти поля синхронизируются с нижним inode в
 * cowfs_get_inode() и cowfs_setattr().
 */

const struct inode_operations cowfs_file_iops = {
    .getattr    = cowfs_getattr,
    .setattr    = cowfs_setattr,
};

const struct inode_operations cowfs_dir_iops = {
    .lookup     = cowfs_lookup,
    .create     = cowfs_create,
    .unlink     = cowfs_unlink,
    .mkdir      = cowfs_mkdir,
    .rmdir      = cowfs_rmdir,
    .rename     = cowfs_rename,
    .symlink    = cowfs_symlink,
    .link       = cowfs_link,
    .setattr    = cowfs_setattr,
    .getattr    = cowfs_getattr,
};

const struct inode_operations cowfs_symlink_iops = {
    .get_link   = cowfs_get_link,
    .getattr    = cowfs_getattr,
};
