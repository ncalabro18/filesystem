#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../sfs/sfs_module.h"
#include "sfs_mkfs.h"


static __u64 sfs_strtou64_checked(const char *str)
{
	__u64 result = 0, addend;
	const char *ptr;
	char c;

	if (!str || !*str) {
		fputs("Empty value\n", stderr);
		exit(1);
	}
	for (ptr = str; (c = *ptr); ++ptr) {
		if (c < '0' || '9' < c) {
			fprintf(stderr, "Invalid character %c in %s\n", c, str);
			exit(1);
		}
		addend = (__u64)(c - '0');
		if (__builtin_mul_overflow(result, 10, &result) ||
		    __builtin_add_overflow(result, addend, &result)) {
			fprintf(stderr, "Value %s too large\n", str);
			exit(1);
		}
	}
	return result;
}

void sfs_mkfs_init(const char *storage, const char *entries_arg)
{
	int fd;
	off_t mapping_size_off;
	size_t mapping_size;
	void *disk_image_mapping;

	struct sfs_super super = {
		.magic = {'S', 'I', 'M', 'P', 'L', 'E', 'F', 'S'},
	};
	struct sfs_inode *root;
	__u64 blocks_total, sb_inodes, inode_table_bytes, itbl_blocks, reserved;
	__u64 bitmap_blocks_needed = 1, dstart, dblocks;

	fd = open(storage, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Unable to open %s: %s\n", storage, strerror(errno));
		exit(1);
	}
	mapping_size_off = lseek(fd, 0, SEEK_END);
	if (mapping_size_off < 0) {
		perror("Unable to seek");
		exit(1);
	}
	mapping_size = (size_t)mapping_size_off;

	disk_image_mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (disk_image_mapping == MAP_FAILED) {
		perror("Unable to mmap");
		exit(1);
	}
	close(fd); /* mapping remains valid after close */

	blocks_total = mapping_size / SFS_BLOCK_SIZE;
	if (blocks_total < 4) {
		fputs("Image too small\n", stderr);
		exit(1);
	}

	sb_inodes = entries_arg ? sfs_strtou64_checked(entries_arg) : (blocks_total / 4);
	if (sb_inodes < 1)
		sb_inodes = 1;

	inode_table_bytes = sb_inodes * sizeof(struct sfs_inode);
	itbl_blocks = (inode_table_bytes + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
	reserved = 1 + itbl_blocks; /* superblock + inode table */

	for (;;) {
		__u64 needed;
		dstart = reserved + bitmap_blocks_needed;
		if (dstart >= blocks_total) {
			fputs("Image too small for requested inode count\n", stderr);
			exit(1);
		}
		dblocks = blocks_total - dstart;
		needed = (dblocks + 8 * SFS_BLOCK_SIZE - 1) / (8 * SFS_BLOCK_SIZE);
		if (needed == 0)
			needed = 1;
		if (needed == bitmap_blocks_needed)
			break;
		bitmap_blocks_needed = needed;
	}

	super.total_blocks = __cpu_to_le64(blocks_total);
	super.total_inodes = __cpu_to_le64(sb_inodes);
	super.inode_table_block = __cpu_to_le64(1);
	super.inode_table_blocks = __cpu_to_le64(itbl_blocks);
	super.data_bitmap_block = __cpu_to_le64(1 + itbl_blocks);
	super.data_bitmap_blocks = __cpu_to_le64(bitmap_blocks_needed);
	super.data_start_block = __cpu_to_le64(dstart);

	memcpy(disk_image_mapping, &super, sizeof(super));

	memset((char *)disk_image_mapping + SFS_BLOCK_SIZE, 0, (size_t)itbl_blocks * SFS_BLOCK_SIZE);
	memset((char *)disk_image_mapping + (size_t)(1 + itbl_blocks) * SFS_BLOCK_SIZE, 0,
	       (size_t)bitmap_blocks_needed * SFS_BLOCK_SIZE);

	/* Root inode. index_block is left at 0 - the kernel's
	 * sfs_dir_ensure_index() allocates it lazily on the first entry
	 * added under root, so nothing needs pre-allocating here. */
	root = (struct sfs_inode *)((char *)disk_image_mapping + SFS_BLOCK_SIZE);
	root->mode = __cpu_to_le32(S_IFDIR | 0755);
	root->num_links = __cpu_to_le32(2);
	root->access_time = root->modified_time =
		root->metadata_modified_time = root->birth_time = __cpu_to_le64(time(NULL));

	msync(disk_image_mapping, mapping_size, MS_SYNC);
	munmap(disk_image_mapping, mapping_size);
}
