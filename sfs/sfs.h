#ifndef SFS_H
#define SFS_H

#include <linux/types.h>
#include <asm/byteorder.h>

#define SFS_MAX_FILE_NAME 128
#define SFS_BLOCK_SIZE 4096
#define SFS_DIRECT_BLOCKS 8
#define SFS_PTRS_PER_INDIRECT (SFS_BLOCK_SIZE / sizeof(__le64))
#define SFS_DOUBLE_PTRS_PER_INDIRECT (SFS_PTRS_PER_INDIRECT * SFS_PTRS_PER_INDIRECT)
#define SFS_MAX_FILE_BLOCKS (SFS_DIRECT_BLOCKS + SFS_PTRS_PER_INDIRECT + SFS_DOUBLE_PTRS_PER_INDIRECT)


struct sfs_super {
	char magic[8];              /* SIMPLEFS */
	__le64 total_blocks;        /* total blocks on the device */
	__le64 total_inodes;        /* size of the single inode pool */
	__le64 inode_table_block;   /* first block of the inode metadata table */
	__le64 inode_table_blocks;
	__le64 data_bitmap_block;   /* first block of the data-block free bitmap */
	__le64 data_bitmap_blocks;
	__le64 data_start_block;    /* first block available for file data */
} __attribute__((packed));


struct sfs_dirent {
	char name[SFS_MAX_FILE_NAME];
	__le64 inode;
} __attribute__((packed));

struct sfs_inode {
	__le64 file_size;
	__le32 mode;
	__le32 uid;
	__le32 gid;
	__le32 num_links;
	__le64 direct[SFS_DIRECT_BLOCKS];
	__le64 indirect;            /* block number of an indirect block, or 0 */
	__le64 double_indirect;

	__le64 access_time;
	__le64 modified_time;
	__le64 metadata_modified_time;
	__le64 birth_time;
} __attribute__((packed));


#endif
