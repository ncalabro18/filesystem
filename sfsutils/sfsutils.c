/* sfsutils - userspace tool for the SFS on-disk format.
 * Rewritten for the block-addressed, dirent-based format (v3):
 *   - single inode pool, type stored in mode (S_IFDIR/S_IFREG/S_IFLNK)
 *   - directories store their children as struct sfs_dirent arrays in
 *     their own data blocks, addressed via the same direct/indirect
 *     scheme as regular files
 *   - inodes carry num_links, uid/gid, and four timestamps
 */
#define _DEFAULT_SOURCE

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../sfs/sfs.h"

#define SFS_DIRENTS_PER_BLOCK (SFS_BLOCK_SIZE / sizeof(struct sfs_dirent))
#define SFS_NAME_MAX (sizeof(((struct sfs_dirent *)0)->name))


#ifdef __KERNEL__
typedef umode_t sfs_mode_t;
#else
#include <sys/types.h>
typedef mode_t sfs_mode_t;
#endif


static void *disk_image_mapping;
static size_t mapping_size;
static unsigned long total_inodes;
static __u64 total_blocks;
static __u64 inode_table_block, inode_table_blocks;
static __u64 data_bitmap_block, data_bitmap_blocks;
static __u64 data_start_block, data_block_count;

/*** low-level accessors ***/

static struct sfs_inode *inode_table_ptr(void)
{
	return (struct sfs_inode *)((char *)disk_image_mapping + inode_table_block * SFS_BLOCK_SIZE);
}

static struct sfs_inode *inode_at(unsigned long ino)
{
	return &inode_table_ptr()[ino];
}

static unsigned char *bitmap_ptr(void)
{
	return (unsigned char *)disk_image_mapping + data_bitmap_block * SFS_BLOCK_SIZE;
}

static bool bitmap_test(__u64 rel_block)
{
	return (bitmap_ptr()[rel_block / 8] >> (rel_block % 8)) & 1;
}

static void bitmap_set(__u64 rel_block, bool val)
{
	unsigned char *b = bitmap_ptr();
	if (val)
		b[rel_block / 8] |= (1 << (rel_block % 8));
	else
		b[rel_block / 8] &= ~(1 << (rel_block % 8));
}

static long bitmap_alloc(void)
{
	__u64 i;
	for (i = 0; i < data_block_count; i++) {
		if (!bitmap_test(i)) {
			bitmap_set(i, true);
			return (long)i;
		}
	}
	return -1;
}

/* Returns the absolute block number holding block_idx of node's data,
 * allocating direct/indirect blocks on demand if alloc is true. Applies
 * equally to regular files, symlinks, and directories - all three use
 * the same direct+indirect addressing, mirroring sfs_resolve_block() in
 * the kernel module. */
static __u64 node_block_ptr(struct sfs_inode *node, __u64 block_idx, bool alloc)
{
	if (block_idx < SFS_DIRECT_BLOCKS) {
		__u64 blk = __le64_to_cpu(node->direct[block_idx]);
		if (!blk && alloc) {
			long rel = bitmap_alloc();
			if (rel < 0) { fputs("No space left on device\n", stderr); exit(1); }
			blk = data_start_block + rel;
			node->direct[block_idx] = __cpu_to_le64(blk);
		}
		return blk;
	}

	__u64 idx = block_idx - SFS_DIRECT_BLOCKS;
	if (idx >= SFS_PTRS_PER_INDIRECT) {
		fputs("File too large\n", stderr);
		exit(1);
	}

	__u64 indirect = __le64_to_cpu(node->indirect);
	if (!indirect) {
		if (!alloc)
			return 0;
		long rel = bitmap_alloc();
		if (rel < 0) { fputs("No space left on device\n", stderr); exit(1); }
		indirect = data_start_block + rel;
		node->indirect = __cpu_to_le64(indirect);
		memset((char *)disk_image_mapping + indirect * SFS_BLOCK_SIZE, 0, SFS_BLOCK_SIZE);
	}

	__le64 *ptrs = (__le64 *)((char *)disk_image_mapping + indirect * SFS_BLOCK_SIZE);
	__u64 blk = __le64_to_cpu(ptrs[idx]);
	if (!blk && alloc) {
		long rel = bitmap_alloc();
		if (rel < 0) { fputs("No space left on device\n", stderr); exit(1); }
		blk = data_start_block + rel;
		ptrs[idx] = __cpu_to_le64(blk);
	}
	return blk;
}

static void free_inode_blocks(struct sfs_inode *node)
{
	int i;
	for (i = 0; i < SFS_DIRECT_BLOCKS; i++) {
		__u64 blk = __le64_to_cpu(node->direct[i]);
		if (blk)
			bitmap_set(blk - data_start_block, false);
	}
	__u64 indirect = __le64_to_cpu(node->indirect);
	if (indirect) {
		__le64 *ptrs = (__le64 *)((char *)disk_image_mapping + indirect * SFS_BLOCK_SIZE);
		int p;
		for (p = 0; p < (int)SFS_PTRS_PER_INDIRECT; p++) {
			__u64 blk = __le64_to_cpu(ptrs[p]);
			if (blk)
				bitmap_set(blk - data_start_block, false);
		}
		bitmap_set(indirect - data_start_block, false);
	}
}

/*** directory entry helpers ***/

static bool dirent_find(unsigned long dir_ino, const char *name, size_t namelen, unsigned long *out_ino)
{
	struct sfs_inode *dir = inode_at(dir_ino);
	__u64 nblocks = (__le64_to_cpu(dir->file_size) + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
	__u64 b;

	for (b = 0; b < nblocks; b++) {
		__u64 blk = node_block_ptr(dir, b, false);
		struct sfs_dirent *ents;
		unsigned s;

		if (!blk)
			continue;
		ents = (struct sfs_dirent *)((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE);
		for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++) {
			size_t enl;
			if (__le64_to_cpu(ents[s].inode) == 0)
				continue;
			enl = strnlen(ents[s].name, sizeof(ents[s].name));
			if (enl == namelen && memcmp(ents[s].name, name, namelen) == 0) {
				*out_ino = (unsigned long)__le64_to_cpu(ents[s].inode);
				return true;
			}
		}
	}
	return false;
}

static void dirent_add(unsigned long dir_ino, const char *name, size_t namelen, unsigned long child_ino)
{
	struct sfs_inode *dir = inode_at(dir_ino);
	__u64 nblocks = (__le64_to_cpu(dir->file_size) + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
	__u64 b, blk;
	struct sfs_dirent *ents;
	unsigned s;

	for (b = 0; b < nblocks; b++) {
		blk = node_block_ptr(dir, b, false);
		if (!blk)
			continue;
		ents = (struct sfs_dirent *)((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE);
		for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++) {
			if (__le64_to_cpu(ents[s].inode) == 0) {
				memset(ents[s].name, 0, sizeof(ents[s].name));
				memcpy(ents[s].name, name, namelen);
				ents[s].inode = __cpu_to_le64(child_ino);
				return;
			}
		}
	}

	blk = node_block_ptr(dir, nblocks, true);
	memset((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE, 0, SFS_BLOCK_SIZE);
	ents = (struct sfs_dirent *)((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE);
	memcpy(ents[0].name, name, namelen);
	ents[0].inode = __cpu_to_le64(child_ino);
	dir->file_size = __cpu_to_le64((nblocks + 1) * SFS_BLOCK_SIZE);
}

static bool dirent_remove(unsigned long dir_ino, const char *name, size_t namelen)
{
	struct sfs_inode *dir = inode_at(dir_ino);
	__u64 nblocks = (__le64_to_cpu(dir->file_size) + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
	__u64 b;

	for (b = 0; b < nblocks; b++) {
		__u64 blk = node_block_ptr(dir, b, false);
		struct sfs_dirent *ents;
		unsigned s;

		if (!blk)
			continue;
		ents = (struct sfs_dirent *)((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE);
		for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++) {
			size_t enl;
			if (__le64_to_cpu(ents[s].inode) == 0)
				continue;
			enl = strnlen(ents[s].name, sizeof(ents[s].name));
			if (enl == namelen && memcmp(ents[s].name, name, namelen) == 0) {
				memset(&ents[s], 0, sizeof(ents[s]));
				return true;
			}
		}
	}
	return false;
}

static bool dirent_is_empty(unsigned long dir_ino)
{
	struct sfs_inode *dir = inode_at(dir_ino);
	__u64 nblocks = (__le64_to_cpu(dir->file_size) + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
	__u64 b;

	for (b = 0; b < nblocks; b++) {
		__u64 blk = node_block_ptr(dir, b, false);
		struct sfs_dirent *ents;
		unsigned s;

		if (!blk)
			continue;
		ents = (struct sfs_dirent *)((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE);
		for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++)
			if (__le64_to_cpu(ents[s].inode) != 0)
				return false;
	}
	return true;
}

/*** path resolution - walks the tree component by component ***/

static bool resolve(const char *path, unsigned long *out_ino)
{
	unsigned long cur = 0;
	const char *p = path;

	while (*p == '/')
		p++;
	if (*p == '\0') {
		*out_ino = 0; /* root */
		return true;
	}

	while (*p) {
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		unsigned long child;

		if (len == 0) {
			p = slash ? slash + 1 : p + strlen(p);
			continue;
		}
		if (len >= SFS_NAME_MAX) {
			fprintf(stderr, "Path component too long: %.*s\n", (int)len, p);
			exit(1);
		}
		if (!S_ISDIR(__le32_to_cpu(inode_at(cur)->mode)))
			return false;
		if (!dirent_find(cur, p, len, &child))
			return false;
		cur = child;
		p = slash ? slash + 1 : p + len;
	}

	*out_ino = cur;
	return true;
}

static unsigned long resolve_or_die(const char *path)
{
	unsigned long ino;
	if (!resolve(path, &ino)) {
		fprintf(stderr, "No such file or directory: %s\n", path);
		exit(1);
	}
	return ino;
}

/* Splits path into parent directory inode + final component name.
 * path is modified in place (a '/' is replaced with '\0'). */
static void resolve_parent(char *path, unsigned long *parent_ino, char **basename)
{
	char *slash = strrchr(path, '/');

	if (!slash) {
		*parent_ino = 0;
		*basename = path;
	} else {
		*slash = '\0';
		if (*path == '\0') {
			*parent_ino = 0; /* path was "/name" */
		} else if (!resolve(path, parent_ino)) {
			fprintf(stderr, "Parent directory does not exist: %s\n", path);
			exit(1);
		}
		*basename = slash + 1;
	}

	if (**basename == '\0') {
		fputs("Empty filename\n", stderr);
		exit(1);
	}
	if (strlen(*basename) >= SFS_NAME_MAX) {
		fprintf(stderr, "Filename too long: %s\n", *basename);
		exit(1);
	}
	if (!S_ISDIR(__le32_to_cpu(inode_at(*parent_ino)->mode))) {
		fputs("Parent is not a directory\n", stderr);
		exit(1);
	}
}

/*** inode allocation ***/

static unsigned long alloc_inode(void)
{
	struct sfs_inode *tbl = inode_table_ptr();
	unsigned long i;
	for (i = 1; i < total_inodes; i++)
		if (__le32_to_cpu(tbl[i].num_links) == 0)
			return i;
	fputs("No free inode\n", stderr);
	exit(1);
}

/*** superblock / mapping setup ***/

static void verify_and_load_sb(void)
{
	struct sfs_super *sb = disk_image_mapping;

	if (memcmp(sb->magic, "SIMPLEFS", sizeof(sb->magic))) {
		fputs("Invalid FS magic in super block\n", stderr);
		exit(1);
	}

	total_blocks = __le64_to_cpu(sb->total_blocks);
	total_inodes = (unsigned long)__le64_to_cpu(sb->total_inodes);
	inode_table_block = __le64_to_cpu(sb->inode_table_block);
	inode_table_blocks = __le64_to_cpu(sb->inode_table_blocks);
	data_bitmap_block = __le64_to_cpu(sb->data_bitmap_block);
	data_bitmap_blocks = __le64_to_cpu(sb->data_bitmap_blocks);
	data_start_block = __le64_to_cpu(sb->data_start_block);

	if (data_start_block > total_blocks) {
		fputs("Invalid metadata in super block (bad layout)\n", stderr);
		exit(1);
	}
	data_block_count = total_blocks - data_start_block;

	if ((__u64)mapping_size < total_blocks * SFS_BLOCK_SIZE) {
		fputs("Image smaller than superblock claims\n", stderr);
		exit(1);
	}
}

static void set_up_mapping(const char *path, bool writable)
{
	int flags = writable ? O_RDWR : O_RDONLY;
	int prot = PROT_READ | (writable ? PROT_WRITE : 0);
	int fd = open(path, flags);
	off_t ret;

	if (fd < 0) {
		fprintf(stderr, "Unable to open %s: %s\n", path, strerror(errno));
		exit(1);
	}
	ret = lseek(fd, 0, SEEK_END);
	if (ret < 0) {
		perror("Unable to seek");
		exit(1);
	}
	mapping_size = (size_t)ret;
	disk_image_mapping = mmap(NULL, mapping_size, prot, MAP_SHARED, fd, 0);
	if (disk_image_mapping == MAP_FAILED) {
		perror("Unable to mmap");
		exit(1);
	}
}

static __u64 strtou64_checked(const char *str)
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

/*** commands ***/

static void cmd_init(const char *storage, const char *entries)
{
	struct sfs_super super = {
		.magic = {'S', 'I', 'M', 'P', 'L', 'E', 'F', 'S'},
	};
	struct sfs_inode *root;
	__u64 blocks_total, sb_inodes, inode_table_bytes, itbl_blocks, reserved;
	__u64 bitmap_blocks_needed = 1, dstart, dblocks;

	set_up_mapping(storage, true);

	blocks_total = mapping_size / SFS_BLOCK_SIZE;
	if (blocks_total < 4) {
		fputs("Image too small\n", stderr);
		exit(1);
	}

	sb_inodes = entries ? strtou64_checked(entries) : (blocks_total / 4);
	if (sb_inodes < 1)
		sb_inodes = 1;

	inode_table_bytes = sb_inodes * sizeof(struct sfs_inode);
	itbl_blocks = (inode_table_bytes + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
	reserved = 1 + itbl_blocks; /* superblock + inode table */

	for (;;) {
		dstart = reserved + bitmap_blocks_needed;
		if (dstart >= blocks_total) {
			fputs("Image too small for requested inode count\n", stderr);
			exit(1);
		}
		dblocks = blocks_total - dstart;
		__u64 needed = (dblocks + 8 * SFS_BLOCK_SIZE - 1) / (8 * SFS_BLOCK_SIZE);
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

	root = (struct sfs_inode *)((char *)disk_image_mapping + SFS_BLOCK_SIZE);
	root->mode = __cpu_to_le32(S_IFDIR | 0755);
	root->num_links = __cpu_to_le32(2);
	root->file_size = __cpu_to_le64(0);
	root->access_time = root->modified_time =
		root->metadata_modified_time = root->birth_time = __cpu_to_le64(time(NULL));
}

static void cmd_list(void)
{
	void walk(unsigned long dir_ino, const char *path_prefix) {
		struct sfs_inode *dir = inode_at(dir_ino);
		__u64 nblocks = (__le64_to_cpu(dir->file_size) + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
		__u64 b;

		for (b = 0; b < nblocks; b++) {
			__u64 blk = node_block_ptr(dir, b, false);
			struct sfs_dirent *ents;
			unsigned s;

			if (!blk)
				continue;
			ents = (struct sfs_dirent *)((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE);
			for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++) {
				unsigned long cino;
				size_t enl;
				char childpath[PATH_MAX];
				struct sfs_inode *cnode;
				bool is_dir;

				if (__le64_to_cpu(ents[s].inode) == 0)
					continue;
				cino = (unsigned long)__le64_to_cpu(ents[s].inode);
				enl = strnlen(ents[s].name, sizeof(ents[s].name));
				snprintf(childpath, sizeof(childpath), "%s/%.*s", path_prefix, (int)enl, ents[s].name);

				cnode = inode_at(cino);
				is_dir = S_ISDIR(__le32_to_cpu(cnode->mode));
				printf("(%lu) %s%s\n", cino, childpath, is_dir ? "/" : "");

				if (is_dir)
					walk(cino, childpath);
			}
		}
	}

	puts("(0) /");
	walk(0, "");
}

static void cmd_creat(const char *storage, char *path, sfs_mode_t mode, bool is_dir)
{
	char *pathcopy = strdup(path);
	unsigned long parent_ino, existing, new_ino;
	char *basename;
	struct sfs_inode *node;
	__u64 now = time(NULL);

	if (!pathcopy) { fputs("Out of memory\n", stderr); exit(1); }

	set_up_mapping(storage, true);
	verify_and_load_sb();

	resolve_parent(pathcopy, &parent_ino, &basename);
	if (dirent_find(parent_ino, basename, strlen(basename), &existing)) {
		fprintf(stderr, "Path already exists: %s\n", path);
		exit(1);
	}

	new_ino = alloc_inode();
	node = inode_at(new_ino);
	memset(node, 0, sizeof(*node));
	node->mode = __cpu_to_le32((is_dir ? S_IFDIR : S_IFREG) | (mode & 07777));
	node->num_links = __cpu_to_le32(is_dir ? 2 : 1);
	node->access_time = node->modified_time =
		node->metadata_modified_time = node->birth_time = __cpu_to_le64(now);

	dirent_add(parent_ino, basename, strlen(basename), new_ino);
	free(pathcopy);
}

static void cmd_link(const char *storage, char *target_path, char *link_path)
{
	unsigned long target_ino, parent_ino, existing;
	char *link_copy = strdup(link_path);
	char *basename;
	struct sfs_inode *node;

	if (!link_copy) { fputs("Out of memory\n", stderr); exit(1); }

	set_up_mapping(storage, true);
	verify_and_load_sb();

	target_ino = resolve_or_die(target_path);
	if (S_ISDIR(__le32_to_cpu(inode_at(target_ino)->mode))) {
		fputs("Cannot hardlink a directory\n", stderr);
		exit(1);
	}

	resolve_parent(link_copy, &parent_ino, &basename);
	if (dirent_find(parent_ino, basename, strlen(basename), &existing)) {
		fprintf(stderr, "Path already exists: %s\n", link_path);
		exit(1);
	}

	dirent_add(parent_ino, basename, strlen(basename), target_ino);
	node = inode_at(target_ino);
	node->num_links = __cpu_to_le32(__le32_to_cpu(node->num_links) + 1);
	node->metadata_modified_time = __cpu_to_le64(time(NULL));

	free(link_copy);
}

static void cmd_symlink(const char *storage, const char *target, char *link_path)
{
	size_t target_len = strlen(target);
	char *link_copy = strdup(link_path);
	unsigned long parent_ino, existing, new_ino;
	char *basename;
	struct sfs_inode *node;
	__u64 now = time(NULL);
	size_t written = 0;
	__u64 blk_idx = 0;

	if (!link_copy) { fputs("Out of memory\n", stderr); exit(1); }
	if (target_len == 0 || target_len >= PATH_MAX) {
		fputs("Invalid symlink target length\n", stderr);
		exit(1);
	}

	set_up_mapping(storage, true);
	verify_and_load_sb();

	resolve_parent(link_copy, &parent_ino, &basename);
	if (dirent_find(parent_ino, basename, strlen(basename), &existing)) {
		fprintf(stderr, "Path already exists: %s\n", link_path);
		exit(1);
	}

	new_ino = alloc_inode();
	node = inode_at(new_ino);
	memset(node, 0, sizeof(*node));
	node->mode = __cpu_to_le32(S_IFLNK | 0777);
	node->num_links = __cpu_to_le32(1);
	node->access_time = node->modified_time =
		node->metadata_modified_time = node->birth_time = __cpu_to_le64(now);

	while (written < target_len) {
		__u64 blk = node_block_ptr(node, blk_idx, true);
		size_t chunk = target_len - written;
		if (chunk > SFS_BLOCK_SIZE)
			chunk = SFS_BLOCK_SIZE;
		memcpy((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE, target + written, chunk);
		written += chunk;
		blk_idx++;
	}
	node->file_size = __cpu_to_le64(target_len);

	dirent_add(parent_ino, basename, strlen(basename), new_ino);
	free(link_copy);
}

static void cmd_readlink(const char *storage, const char *path)
{
	unsigned long ino;
	struct sfs_inode *node;
	__u64 remaining, blk_idx = 0;

	set_up_mapping(storage, false);
	verify_and_load_sb();

	ino = resolve_or_die(path);
	node = inode_at(ino);
	if (!S_ISLNK(__le32_to_cpu(node->mode))) {
		fprintf(stderr, "Not a symlink: %s\n", path);
		exit(1);
	}

	remaining = __le64_to_cpu(node->file_size);
	while (remaining) {
		__u64 blk = node_block_ptr(node, blk_idx, false);
		size_t chunk = remaining < SFS_BLOCK_SIZE ? (size_t)remaining : SFS_BLOCK_SIZE;
		if (blk)
			fwrite((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE, 1, chunk, stdout);
		remaining -= chunk;
		blk_idx++;
	}
	putchar('\n');
}

static void cmd_unlink(const char *storage, char *path)
{
	char *pathcopy = strdup(path);
	unsigned long parent_ino, target_ino;
	char *basename;
	struct sfs_inode *node;
	__u32 links;

	if (!pathcopy) { fputs("Out of memory\n", stderr); exit(1); }

	set_up_mapping(storage, true);
	verify_and_load_sb();

	resolve_parent(pathcopy, &parent_ino, &basename);
	if (!dirent_find(parent_ino, basename, strlen(basename), &target_ino)) {
		fprintf(stderr, "No such file: %s\n", path);
		exit(1);
	}
	node = inode_at(target_ino);
	if (S_ISDIR(__le32_to_cpu(node->mode))) {
		fprintf(stderr, "Path is a directory (use rmdir): %s\n", path);
		exit(1);
	}

	dirent_remove(parent_ino, basename, strlen(basename));

	links = __le32_to_cpu(node->num_links);
	if (links > 0)
		links--;
	if (links == 0) {
		free_inode_blocks(node);
		memset(node, 0, sizeof(*node));
	} else {
		node->num_links = __cpu_to_le32(links);
		node->metadata_modified_time = __cpu_to_le64(time(NULL));
	}

	free(pathcopy);
}

static void cmd_rmdir(const char *storage, char *path)
{
	char *pathcopy = strdup(path);
	unsigned long parent_ino, target_ino;
	char *basename;
	struct sfs_inode *node;

	if (!pathcopy) { fputs("Out of memory\n", stderr); exit(1); }

	set_up_mapping(storage, true);
	verify_and_load_sb();

	resolve_parent(pathcopy, &parent_ino, &basename);
	if (!dirent_find(parent_ino, basename, strlen(basename), &target_ino)) {
		fprintf(stderr, "No such file: %s\n", path);
		exit(1);
	}
	node = inode_at(target_ino);
	if (!S_ISDIR(__le32_to_cpu(node->mode))) {
		fprintf(stderr, "Path is not a directory: %s\n", path);
		exit(1);
	}
	if (!dirent_is_empty(target_ino)) {
		fprintf(stderr, "Directory not empty: %s\n", path);
		exit(1);
	}

	dirent_remove(parent_ino, basename, strlen(basename));
	free_inode_blocks(node);
	memset(node, 0, sizeof(*node));

	free(pathcopy);
}

static void cmd_dump(const char *storage, const char *path)
{
	unsigned long ino;
	struct sfs_inode *node;
	__u64 remaining, blk_idx = 0;

	set_up_mapping(storage, false);
	verify_and_load_sb();

	ino = resolve_or_die(path);
	node = inode_at(ino);
	if (S_ISDIR(__le32_to_cpu(node->mode))) {
		fprintf(stderr, "Path is a directory: %s\n", path);
		exit(1);
	}

	remaining = __le64_to_cpu(node->file_size);
	while (remaining) {
		__u64 blk = node_block_ptr(node, blk_idx, false);
		size_t chunk = remaining < SFS_BLOCK_SIZE ? (size_t)remaining : SFS_BLOCK_SIZE;
		if (blk) {
			fwrite((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE, 1, chunk, stdout);
		} else {
			char zero[SFS_BLOCK_SIZE] = {0};
			fwrite(zero, 1, chunk, stdout);
		}
		remaining -= chunk;
		blk_idx++;
	}
}

static void cmd_alter(const char *storage, const char *path)
{
	unsigned long ino;
	struct sfs_inode *node;
	__u64 total = 0, blk_idx = 0;
	char buf[SFS_BLOCK_SIZE];
	size_t n;

	set_up_mapping(storage, true);
	verify_and_load_sb();

	ino = resolve_or_die(path);
	node = inode_at(ino);
	if (!S_ISREG(__le32_to_cpu(node->mode))) {
		fprintf(stderr, "Not a regular file: %s\n", path);
		exit(1);
	}

	while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
		__u64 blk = node_block_ptr(node, blk_idx, true);
		memcpy((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE, buf, n);
		if (n < sizeof(buf))
			memset((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE + n, 0, sizeof(buf) - n);
		total += n;
		blk_idx++;
		if (n < sizeof(buf))
			break;
	}

	node->file_size = __cpu_to_le64(total);
	node->modified_time = __cpu_to_le64(time(NULL));
}

static const char *mode_str(__u32 mode)
{
	static char buf[4];
	buf[0] = (mode & 0400) ? 'r' : '-';
	buf[1] = (mode & 0200) ? 'w' : '-';
	buf[2] = (mode & 0100) ? 'x' : '-';
	buf[3] = '\0';
	return buf;
}

static void cmd_stat(const char *storage, const char *path)
{
	unsigned long ino;
	struct sfs_inode *node;
	__u32 mode;
	char typechar;

	set_up_mapping(storage, false);
	verify_and_load_sb();

	ino = resolve_or_die(path);
	node = inode_at(ino);
	mode = __le32_to_cpu(node->mode);

	if (S_ISDIR(mode)) typechar = 'd';
	else if (S_ISLNK(mode)) typechar = 'l';
	else typechar = '-';

	printf("Inode:   %lu\n", ino);
	printf("Type:    %c\n", typechar);
	printf("Mode:    %s (owner: %s)\n", mode_str(mode & 0700), mode_str(mode & 0700));
	printf("Links:   %u\n", __le32_to_cpu(node->num_links));
	printf("UID/GID: %u/%u\n", __le32_to_cpu(node->uid), __le32_to_cpu(node->gid));
	printf("Size:    %" PRIu64 "\n", (uint64_t)__le64_to_cpu(node->file_size));
	printf("Atime:   %" PRIu64 "\n", (uint64_t)__le64_to_cpu(node->access_time));
	printf("Mtime:   %" PRIu64 "\n", (uint64_t)__le64_to_cpu(node->modified_time));
	printf("Ctime:   %" PRIu64 "\n", (uint64_t)__le64_to_cpu(node->metadata_modified_time));
	printf("Btime:   %" PRIu64 "\n", (uint64_t)__le64_to_cpu(node->birth_time));
}

/*** dispatch ***/

static _Noreturn void usage(void)
{
	fputs(
		"Usage: sfsutils <command> [args...]\n"
		"  init     <image> [inode_count]\n"
		"  list     <image>\n"
		"  stat     <image> <path>\n"
		"  creat    <image> <path> [octal_mode]\n"
		"  mkdir    <image> <path> [octal_mode]\n"
		"  unlink   <image> <path>\n"
		"  rmdir    <image> <path>\n"
		"  link     <image> <target_path> <link_path>   (hardlink)\n"
		"  symlink  <image> <target_string> <link_path>\n"
		"  readlink <image> <path>\n"
		"  dump     <image> <path>                       (writes file contents to stdout)\n"
		"  alter    <image> <path>                       (reads new contents from stdin)\n",
		stderr);
	exit(1);
}

int main(int argc, char **argv)
{
	if (argc < 3)
		usage();

	const char *cmd = argv[1];
	const char *storage = argv[2];

	if (!strcmp(cmd, "init")) {
		if (argc > 4) usage();
		cmd_init(storage, argc == 4 ? argv[3] : NULL);
	} else if (!strcmp(cmd, "list")) {
		if (argc != 3) usage();
		set_up_mapping(storage, false);
		verify_and_load_sb();
		cmd_list();
	} else if (!strcmp(cmd, "stat")) {
		if (argc != 4) usage();
		cmd_stat(storage, argv[3]);
	} else if (!strcmp(cmd, "creat")) {
		if (argc < 4 || argc > 5) usage();
		sfs_mode_t mode = argc == 5 ? (sfs_mode_t)strtoul(argv[4], NULL, 8) : 0644;
		cmd_creat(storage, argv[3], mode, false);
	} else if (!strcmp(cmd, "mkdir")) {
		if (argc < 4 || argc > 5) usage();
		sfs_mode_t mode = argc == 5 ? (sfs_mode_t)strtoul(argv[4], NULL, 8) : 0755;
		cmd_creat(storage, argv[3], mode, true);
	} else if (!strcmp(cmd, "unlink")) {
		if (argc != 4) usage();
		cmd_unlink(storage, argv[3]);
	} else if (!strcmp(cmd, "rmdir")) {
		if (argc != 4) usage();
		cmd_rmdir(storage, argv[3]);
	} else if (!strcmp(cmd, "link")) {
		if (argc != 5) usage();
		cmd_link(storage, argv[3], argv[4]);
	} else if (!strcmp(cmd, "symlink")) {
		if (argc != 5) usage();
		cmd_symlink(storage, argv[3], argv[4]);
	} else if (!strcmp(cmd, "readlink")) {
		if (argc != 4) usage();
		cmd_readlink(storage, argv[3]);
	} else if (!strcmp(cmd, "dump")) {
		if (argc != 4) usage();
		cmd_dump(storage, argv[3]);
	} else if (!strcmp(cmd, "alter")) {
		if (argc != 4) usage();
		cmd_alter(storage, argv[3]);
	} else {
		usage();
	}

	return 0;
}