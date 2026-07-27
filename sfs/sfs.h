#ifndef SFS_H
#define SFS_H

/* linux kernel headers for endian aware types */
#include <linux/types.h>
#include <asm/byteorder.h>

struct sfs_super {
	char magic[8];      /* SIMPLEFS */
	__le64 max_dirs;    /* DIRECTORIES USE A CUTOFF NUMBER */
	__le64 max_files;
	__le64 entry_size;
};

struct sfs_inode {
	char name[16];      /* null padded when less than 16 bytes, length of 0 means unused */
	__le64 parent_dir;  /* inode number of parent directory */
	__le64 file_size;   /* size of file (0 for directories) */
};

#endif  /* SFS_H */

