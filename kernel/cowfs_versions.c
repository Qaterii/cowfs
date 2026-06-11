#include "cowfs.h"
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#define VERSIONS_HASH_BITS 8
static DEFINE_HASHTABLE(versions_htable, VERSIONS_HASH_BITS);
static DEFINE_SPINLOCK(versions_lock);

static struct kmem_cache *cow_version_cache;
static struct kmem_cache *cow_inode_info_cache;

static struct delayed_work gc_work;

static void gc_worker(struct work_struct *work);

int cowfs_versions_init(void)
{
    cow_version_cache = kmem_cache_create("cowfs_version",
                            sizeof(struct cow_version), 0,
                            SLAB_RECLAIM_ACCOUNT, NULL);
    if (!cow_version_cache)
        return -ENOMEM;

    cow_inode_info_cache = kmem_cache_create("cowfs_inode_info",
                            sizeof(struct cow_inode_info), 0,
                            SLAB_RECLAIM_ACCOUNT, NULL);
    if (!cow_inode_info_cache) {
        kmem_cache_destroy(cow_version_cache);
        return -ENOMEM;
    }

    hash_init(versions_htable);

    INIT_DELAYED_WORK(&gc_work, gc_worker);
    schedule_delayed_work(&gc_work,
                          msecs_to_jiffies(cowfs_gc_interval * 1000));
    return 0;
}

void cowfs_versions_destroy(void)
{
    cancel_delayed_work_sync(&gc_work);
    cowfs_versions_gc_all();
    kmem_cache_destroy(cow_version_cache);
    kmem_cache_destroy(cow_inode_info_cache);
}

struct cow_version *cowfs_version_alloc(enum cow_op op)
{
    struct cow_version *v = kmem_cache_zalloc(cow_version_cache, GFP_KERNEL);
    if (!v)
        return NULL;
    v->timestamp = ktime_get_real_seconds();
    v->op_type   = op;
    INIT_LIST_HEAD(&v->node);
    return v;
}

void cowfs_version_free(struct cow_version *v)
{
    if (v->has_data)
        cowfs_shadow_remove(v->shadow_path);
    kmem_cache_free(cow_version_cache, v);
}

static struct cow_inode_info *find_or_create_inode_info(unsigned long ino)
{
    struct cow_inode_info *info;

    hash_for_each_possible(versions_htable, info, hash_node, ino) {
        if (info->lower_ino == ino)
            return info;
    }

    info = kmem_cache_zalloc(cow_inode_info_cache, GFP_ATOMIC);
    if (!info)
        return NULL;

    info->lower_ino = ino;
    INIT_LIST_HEAD(&info->versions);
    spin_lock_init(&info->lock);
    hash_add(versions_htable, &info->hash_node, ino);
    return info;
}

int cowfs_version_add(unsigned long ino, struct cow_version *v)
{
    struct cow_inode_info *info;
    unsigned long flags;

    spin_lock_irqsave(&versions_lock, flags);
    info = find_or_create_inode_info(ino);
    if (!info) {
        spin_unlock_irqrestore(&versions_lock, flags);
        return -ENOMEM;
    }

    spin_lock(&info->lock);

    if (info->version_count >= COWFS_MAX_VERSIONS) {
        struct cow_version *oldest =
            list_last_entry(&info->versions, struct cow_version, node);
        list_del(&oldest->node);
        info->version_count--;
        cowfs_version_free(oldest);
    }

    list_add(&v->node, &info->versions);
    info->version_count++;

    spin_unlock(&info->lock);
    spin_unlock_irqrestore(&versions_lock, flags);
    return 0;
}

int cowfs_version_list(unsigned long ino,
                        struct cow_version **out, int max_count)
{
    struct cow_inode_info *info;
    struct cow_version *v;
    int count = 0;
    unsigned long flags;

    spin_lock_irqsave(&versions_lock, flags);
    hash_for_each_possible(versions_htable, info, hash_node, ino) {
        if (info->lower_ino != ino)
            continue;
        spin_lock(&info->lock);
        list_for_each_entry(v, &info->versions, node) {
            if (count >= max_count)
                break;
            out[count++] = v;
        }
        spin_unlock(&info->lock);
        break;
    }
    spin_unlock_irqrestore(&versions_lock, flags);
    return count;
}

struct cow_version *cowfs_version_find(unsigned long ino, u64 timestamp)
{
    struct cow_inode_info *info;
    struct cow_version *v, *found = NULL;
    unsigned long flags;

    spin_lock_irqsave(&versions_lock, flags);
    hash_for_each_possible(versions_htable, info, hash_node, ino) {
        if (info->lower_ino != ino)
            continue;
        spin_lock(&info->lock);
        list_for_each_entry(v, &info->versions, node) {
            if (timestamp == 0 || v->timestamp == timestamp) {
                found = v;
                break;
            }
        }
        spin_unlock(&info->lock);
        break;
    }
    spin_unlock_irqrestore(&versions_lock, flags);
    return found;
}

void cowfs_version_gc(void)
{
    struct cow_inode_info *info;
    struct cow_version *v, *tmp;
    u64 now    = ktime_get_real_seconds();
    u64 cutoff = now - cowfs_window_seconds;
    int bkt;
    unsigned long flags;

    spin_lock_irqsave(&versions_lock, flags);
    hash_for_each(versions_htable, bkt, info, hash_node) {
        spin_lock(&info->lock);
        list_for_each_entry_safe(v, tmp, &info->versions, node) {
            if (v->timestamp < cutoff) {
                list_del(&v->node);
                info->version_count--;
                cowfs_version_free(v);
            }
        }
        spin_unlock(&info->lock);
    }
    spin_unlock_irqrestore(&versions_lock, flags);
}

void cowfs_versions_gc_all(void)
{
    struct cow_inode_info *info;
    struct cow_version *v, *tmp;
    struct hlist_node *hnode_tmp;
    int bkt;
    unsigned long flags;

    spin_lock_irqsave(&versions_lock, flags);
    hash_for_each_safe(versions_htable, bkt, hnode_tmp, info, hash_node) {
        spin_lock(&info->lock);
        list_for_each_entry_safe(v, tmp, &info->versions, node) {
            list_del(&v->node);
            cowfs_version_free(v);
        }
        spin_unlock(&info->lock);
        hash_del(&info->hash_node);
        kmem_cache_free(cow_inode_info_cache, info);
    }
    spin_unlock_irqrestore(&versions_lock, flags);
}

static void gc_worker(struct work_struct *work)
{
    cowfs_version_gc();
    schedule_delayed_work(&gc_work,
                          msecs_to_jiffies(cowfs_gc_interval * 1000));
}
