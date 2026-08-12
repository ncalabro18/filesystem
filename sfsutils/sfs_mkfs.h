#ifndef SFS_MKFS_H
#define SFS_MKFS_H


/* Formats path at storage as a fresh SFS image. entries_arg is the optional
 * inode-count string from argv (NULL to use the default sizing). */
void sfs_mkfs_init(const char *storage, const char *entries_arg);



#endif
