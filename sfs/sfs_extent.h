#ifndef SFS_EXTENT_H
#define SFS_EXTENT_H

#include <linux/types.h>
#include "sfs_internal.h"


int sfs_resolve_extent_block(struct super_block *sb, struct sfs_mount_config *config,
                              struct sfs_inode *disk_ino, __u64 logical_block, bool create,
                              __u64 *out_phys, bool *out_dirty);


#endif
