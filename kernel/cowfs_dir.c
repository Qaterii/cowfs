#include "cowfs.h"

struct cowfs_getdents_ctx {
    struct dir_context  ctx;
    struct dir_context *caller_ctx;
};

static bool cowfs_filldir(struct dir_context *ctx, const char *name,
                           int namlen, loff_t offset, u64 ino,
                           unsigned int d_type)
{
    struct cowfs_getdents_ctx *cctx =
        container_of(ctx, struct cowfs_getdents_ctx, ctx);

    /* Скрываем теневую директорию от пользователя */
    if (namlen == strlen(COWFS_SHADOW_DIR) &&
        !strncmp(name, COWFS_SHADOW_DIR, namlen))
        return true;

    return dir_emit(cctx->caller_ctx, name, namlen, ino, d_type);
}

static int cowfs_iterate_shared(struct file *file,
                                  struct dir_context *caller_ctx)
{
    struct file *lower_file = cowfs_lower_file(file);
    struct cowfs_getdents_ctx cctx = {
        .ctx        = { .actor = cowfs_filldir,
                        .pos   = caller_ctx->pos },
        .caller_ctx = caller_ctx,
    };
    int err;

    err = iterate_dir(lower_file, &cctx.ctx);
    caller_ctx->pos = cctx.ctx.pos;
    file->f_pos     = lower_file->f_pos;
    return err;
}

static int cowfs_dir_open(struct inode *inode, struct file *file)
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

    fi->lower_file     = lower_file;
    file->private_data = fi;
    return 0;
}

static int cowfs_dir_release(struct inode *inode, struct file *file)
{
    struct cowfs_file_info *fi = COWFS_F(file);
    if (fi) {
        filp_close(fi->lower_file, NULL);
        kfree(fi);
    }
    return 0;
}

static void cowfs_d_release(struct dentry *dentry)
{
    struct cowfs_dentry_info *di = COWFS_D(dentry);
    if (di) {
        dput(di->lower_dentry);
        mntput(di->lower_mnt);
        kfree(di);
        dentry->d_fsdata = NULL;
    }
}

const struct file_operations cowfs_dir_fops = {
    .open           = cowfs_dir_open,
    .release        = cowfs_dir_release,
    .iterate_shared = cowfs_iterate_shared,
    .llseek         = generic_file_llseek,
};

const struct dentry_operations cowfs_dops = {
    .d_release = cowfs_d_release,
};
