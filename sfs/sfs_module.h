#ifndef SFS_H
#define SFS_H

#include <linux/types.h>
#include <asm/byteorder.h>


#define SFS_MAX_FILE_NAME 128
#define SFS_BLOCK_SIZE 4096


#define SFS_INLINE_EXTENTS 6
#define SFS_EXTENTS_PER_OVERFLOW_BLOCK (SFS_BLOCK_SIZE / sizeof(struct sfs_extent)) /* 256 */


/* Last slot of every overflow block is reserved as a chain pointer:
 * its start_block holds the next overflow block number (0 = end);
 * its length must be 0. Real extents live in slots [0, N-2]. */

/* indexed directories */
#define SFS_DIR_INDEX_BUCKETS (SFS_BLOCK_SIZE / sizeof(__le64)) /* 512 */

#define SFS_DIRENTS_PER_LEAF ((SFS_BLOCK_SIZE - sizeof(struct sfs_dirblock_header)) / sizeof(struct sfs_dirent))


/* extents */
struct sfs_extent {
	__le64 start_block;    /* absolute block number */
	__le32 length;         /* blocks; 0 = unused slot */
	__le32 file_block;     /* logical block offset within the file */
} __attribute__((packed)); /* 16 bytes */



struct sfs_dirblock_header {
	__le64 next; /* next leaf block in this bucket's chain, 0 = end */
} __attribute__((packed));


struct sfs_super {
	char magic[8];
	__le64 total_blocks;
	__le64 total_inodes;
	__le64 inode_table_block;
	__le64 inode_table_blocks;
	__le64 data_bitmap_block;
	__le64 data_bitmap_blocks;
	__le64 data_start_block;
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
	__le32 alloc_hint;      /* relative block hint for this inode's first extent allocation */
	struct sfs_extent extents[SFS_INLINE_EXTENTS];
	__le64 extent_overflow; /* first overflow block, 0 = none. Files/symlinks only. */
	__le64 index_block;     /* hashed directory index block, 0 = none. Directories only. */
	
	__le64 access_time;
	__le64 modified_time;
	__le64 metadata_modified_time;
	__le64 birth_time;
} __attribute__((packed));

#endif