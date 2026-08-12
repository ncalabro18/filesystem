#ifndef SFS_DIR_INDEX_H
#define SFS_DIR_INDEX_H

#include <linux/types.h>
#include "sfs_internal.h"


__u32 sfs_name_hash(const char *name, size_t len);

int sfs_dir_ensure_index(struct super_block *sb, struct sfs_mount_config *config,
                          struct sfs_inode *dir_disk);

int sfs_dir_find(struct super_block *sb, struct sfs_mount_config *config,
                  struct sfs_inode *dir_disk, const char *name, size_t namelen,
                  unsigned int *out_ino);
int sfs_dir_add(struct super_block *sb, struct sfs_mount_config *config,
                 struct sfs_inode *dir_disk,
                 const char *name, size_t namelen, unsigned int ino);
int sfs_dir_remove(struct super_block *sb, struct sfs_mount_config *config,
                    struct sfs_inode *dir_disk, const char *name, size_t namelen);
bool sfs_dir_is_empty(struct super_block *sb, struct sfs_mount_config *config,
                       struct sfs_inode *dir_disk);
void sfs_free_dir_index(struct super_block *sb, struct sfs_mount_config *config,
                         __u64 index_block);

#endif
