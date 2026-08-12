#ifndef SFS_INTERNAL_H
#define SFS_INTERNAL_H

#include <linux/rwsem.h>
#include "sfs_module.h"

struct sfs_mount_config {
	struct rw_semaphore lock;
	struct sfs_super super;
	unsigned int *inode_free_stack;
	unsigned int inode_free_count;
	unsigned long *data_bitmap;
	__u64 data_block_count;
};

/* shared allocator/bitmap helpers - defined in sfs.c, no longer static
 * since sfs_extent.c and sfs_dir_index.c both need them */
long sfs_alloc_data_block(struct sfs_mount_config *config);
long sfs_alloc_data_block_near(struct sfs_mount_config *config, __u64 hint_rel);
void sfs_free_data_block(struct sfs_mount_config *config, __u64 rel_block);
bool sfs_block_free(struct sfs_mount_config *config, __u64 rel_block);
int sfs_sync_bitmap_bit(struct super_block *sb, struct sfs_mount_config *config,
                         __u64 rel_block, bool set);
struct sfs_super* sfs_get_super(struct super_block *sb);

#endif