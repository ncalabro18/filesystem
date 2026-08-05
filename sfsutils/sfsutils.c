/* This file was provided with instructions describing implementation details of a simple filesystem
	from Introduction to Linux Development offered by UMass Lowell, designed by Red Hat */
#define _DEFAULT_SOURCE

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../sfs/sfs.h"


static void *disk_image_mapping;
static size_t mapping_size;
static unsigned long total_inodes;
static __u64 total_blocks;
static __u64 inode_table_block, inode_table_blocks;
static __u64 data_bitmap_block, data_bitmap_blocks;
static __u64 data_start_block, data_block_count;

static struct sfs_inode *inode_table_ptr(void)
{
	return (struct sfs_inode *)((char *)disk_image_mapping + inode_table_block * SFS_BLOCK_SIZE);
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

/* Returns the absolute block number holding file_block_idx of node's data,
 * allocating direct/indirect blocks on demand if alloc is true. Mirrors
 * sfs_get_block()'s logic in the kernel module. */
static __u64 file_block_ptr(struct sfs_inode *node, __u64 file_block_idx, bool alloc)
{
	if (file_block_idx < SFS_DIRECT_BLOCKS) {
		__u64 blk = __le64_to_cpu(node->direct[file_block_idx]);
		if (!blk && alloc) {
			long rel = bitmap_alloc();
			if (rel < 0) { fputs("No space left on device\n", stderr); exit(1); }
			blk = data_start_block + rel;
			node->direct[file_block_idx] = __cpu_to_le64(blk);
		}
		return blk;
	}

	__u64 idx = file_block_idx - SFS_DIRECT_BLOCKS;
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


static _Noreturn void usage(void)
{
	fputs("Usage: sfsutils init|list|creat|unlink|mkdir|rmdir|dump|alter\n"
		"\tinit: storage [entry size [max directories [max entries]]]\n"
		"\t - Initialize filesystem using all available space with 4k blocks or otherwise as specified\n"
		"\tlist: storage\n"
		"\t - Dump tree of files and directories with associated inode numbers\n"
		"\tcreat: storage path\n"
		"\t - create a new file at the specified path\n"
		"\tunlink: storage path\n"
		"\t - remove a file at the specified path\n"
		"\tmkdir: storage path\n"
		"\t - create a new directory at the specified path\n"
		"\trmdir: storage path\n"
		"\t - remove a directory at the specified path\n"
		"\tdump: storage path\n"
		"\t - dump contents of file at path to stdout\n"
		"\talter: storage path\n"
		"\t - read new contents for file from stdin\n", stderr);
	exit(1);
}

static void init(char *storage, char *size, char *dirs, char *entries);
static void list(char *storage);
static void creat_(char *storage, char *path);
static void unlink_(char *storage, char *path);
static void mkdir_(char *storage, char *path);
static void rmdir_(char *storage, char *path);
static void dump(char *storage, char *path);
static void alter(char *storage, char *path);


int main(int argc, char **argv)
{
	char *error_message = NULL;
	void (*fp)(char *, char *) = NULL;

	if (argc < 2)
		usage();

	switch (argv[1][0]) {
	case 'i': {
		char *size = NULL, *dirs = NULL, *entries = NULL;
		switch (argc) {
		case 6:
			entries = argv[5];
			[[fallthrough]];
		case 5:
			dirs = argv[4];
			[[fallthrough]];
		case 4:
			size = argv[3];
			[[fallthrough]];
		case 3:
			init(argv[2], size, dirs, entries);
			break;
		case 2:
			error_message = "Missing storage\n";
			break;
		default:
			error_message = "Too many arguments\n";
			break;
		}
		break;
	}
	case 'l':
		if (argc != 3)
			error_message = argc < 3 ? "Missing storage\n" : "Too many arguments\n";
		else
			list(argv[2]);
		break;
	case 'c':
		fp = creat_;
		break;
	case 'u':
		fp = unlink_;
		break;
	case 'm':
		fp = mkdir_;
		break;
	case 'r':
		fp = rmdir_;
		break;
	case 'd':
		fp = dump;
		break;
	case 'a':
		fp = alter;
		break;
	default:
		error_message = "Unknown command\n";
		break;
	}
	if (fp) {
		switch (argc) {
		case 2:
			error_message = "Missing storage and path\n";
			break;
		case 3:
			error_message = "Missing path\n";
			break;
		case 4:
			fp(argv[2], argv[3]);
			break;
		default:
			error_message = "Too many arguments\n";
			break;
		}
	}
	if (error_message) {
		fputs(error_message, stderr);
		usage();
	}
}

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

struct buf {
	void *ptr;
	size_t len, capacity;
};

static void resize_buf(struct buf *buf, size_t required_capacity)
{
	while (buf->capacity < required_capacity) {
		buf->capacity = 2 * buf->capacity + 1;
		if (!(buf->ptr = realloc(buf->ptr, buf->capacity))) {
			fputs("failed to reize buf\n", stderr);
			exit(1);
		}
	}
}

static void reverse_append(struct buf *buf, size_t len, const char *data)
{
	char *ptr;
	resize_buf(buf, buf->len + len);
	ptr = buf->ptr;
	while (len--) {
		char c = data[len];
		/* skip null padding */
		if (!c)
			continue;
		ptr[buf->len++] = c;
	}
}

static bool buf_eq(const struct buf *buf1, const struct buf *buf2)
{
	size_t len = buf1->len;
	if (len != buf2->len)
		return false;
	return !memcmp(buf1->ptr, buf2->ptr, len);
}

static void walk_inode_table(bool (*cb)(unsigned long ino, const struct buf *path, void *cookie), void *cookie)
{
	static const char slash = '/';
	struct sfs_inode *inode_tbl = inode_table_ptr();
	bool done = false;
	for (unsigned long i = 1; !done && i < total_inodes; ++i) {
		struct buf buf = {};
		unsigned long parent;
		if (!inode_tbl[i].name[0])
			continue;
		reverse_append(&buf, sizeof(inode_tbl->name), inode_tbl[i].name);
		parent = __le64_to_cpu(inode_tbl[i].parent_dir);
		while (parent != 0) {
			reverse_append(&buf, 1, &slash);
			reverse_append(&buf, sizeof(inode_tbl->name), inode_tbl[parent].name);
			parent = __le64_to_cpu(inode_tbl[parent].parent_dir);
		}
		done |= cb(i, &buf, cookie);
		free(buf.ptr);
	}
}

static __u64 strtou64(char *str)
{
	__u64 result = 0, addend;
	if (!str || !*str) {
		fputs("Empty value\n", stderr);
		exit(1);
	}

	for (char *ptr = str, c; (c = *ptr); ++ptr) {
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

void init(char *storage, char *size, char *dirs, char *entries)
{
	(void)size; /* block size is now fixed at SFS_BLOCK_SIZE - no longer configurable */
	(void)dirs; /* no longer meaningful - single inode pool, type is per-inode via mode */

	struct sfs_super super = {
		.magic = {'S', 'I', 'M', 'P', 'L', 'E', 'F', 'S'},
	};

	set_up_mapping(storage, true);

	__u64 total_blocks = mapping_size / SFS_BLOCK_SIZE;
	if (total_blocks < 4) {
		fputs("Image too small\n", stderr);
		exit(1);
	}

	__u64 sb_inodes = entries ? strtou64(entries) : (total_blocks / 4);
	if (sb_inodes < 1)
		sb_inodes = 1;

	__u64 inode_table_bytes = sb_inodes * sizeof(struct sfs_inode);
	__u64 inode_table_blocks = (inode_table_bytes + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
	__u64 reserved = 1 /* superblock */ + inode_table_blocks;

	/* bitmap size depends on data block count, which depends on bitmap
	 * size - solve iteratively (converges in a couple of steps) */
	__u64 data_bitmap_blocks = 1, data_start, data_blocks;
	for (;;) {
		data_start = reserved + data_bitmap_blocks;
		if (data_start >= total_blocks) {
			fputs("Image too small for requested inode count\n", stderr);
			exit(1);
		}
		data_blocks = total_blocks - data_start;
		__u64 needed = (data_blocks + 8 * SFS_BLOCK_SIZE - 1) / (8 * SFS_BLOCK_SIZE);
		if (needed == 0)
			needed = 1;
		if (needed == data_bitmap_blocks)
			break;
		data_bitmap_blocks = needed;
	}

	super.total_blocks = __cpu_to_le64(total_blocks);
	super.total_inodes = __cpu_to_le64(sb_inodes);
	super.inode_table_block = __cpu_to_le64(1);
	super.inode_table_blocks = __cpu_to_le64(inode_table_blocks);
	super.data_bitmap_block = __cpu_to_le64(1 + inode_table_blocks);
	super.data_bitmap_blocks = __cpu_to_le64(data_bitmap_blocks);
	super.data_start_block = __cpu_to_le64(data_start);

	memcpy(disk_image_mapping, &super, sizeof(super));

	memset((char *)disk_image_mapping + SFS_BLOCK_SIZE, 0,
	       (size_t)inode_table_blocks * SFS_BLOCK_SIZE);
	memset((char *)disk_image_mapping + (size_t)(1 + inode_table_blocks) * SFS_BLOCK_SIZE, 0,
	       (size_t)data_bitmap_blocks * SFS_BLOCK_SIZE);

	struct sfs_inode *inode_table = (struct sfs_inode *)((char *)disk_image_mapping + SFS_BLOCK_SIZE);
	inode_table[0].mode = __cpu_to_le32(S_IFDIR | 0755);
	inode_table[0].num_links = __cpu_to_le32(2);
	inode_table[0].file_size = __cpu_to_le64(0);
}

struct tree_entry {
	char *name;
	unsigned long ino;
};

static bool list_callback(unsigned long ino, const struct buf *path, void *cookie)
{
	struct tree_entry *next;
	struct buf *table = cookie;
	size_t len = path->len;
	char *name = malloc(len + 1), *ptr = name;
	while (len--)
		*ptr++ = len[(char *)path->ptr];
	*ptr = '\0';
	resize_buf(table, table->len += sizeof(struct tree_entry));
	next = table->ptr + table->len;
	next[-1] = (struct tree_entry){
		.name = name,
		.ino = ino,
	};
	return false;
}

void list(char *storage)
{
	struct buf table = {};
	set_up_mapping(storage, false);
	verify_and_load_sb();
	walk_inode_table(list_callback, &table);
	qsort(table.ptr, table.len / sizeof(struct tree_entry), sizeof(struct tree_entry), (void *)strcmp);
	puts("(0) (root)");
	for (struct tree_entry *ptr = table.ptr, *end = table.ptr + table.len; ptr < end; ++ptr) {
		printf("(%lu) %s\n", ptr->ino, ptr->name);
		free(ptr->name);
	}
	free(table.ptr);
}

struct lookup_cookie {
	struct buf target_path;
	unsigned long ino;
	bool found;
};

static bool lookup_callback(unsigned long ino, const struct buf *path, void *cookie)
{
	struct lookup_cookie *c = cookie;
	if (!buf_eq(&c->target_path, path))
		return false;
	c->ino = ino;
	c->found = true;
	return true;
}

static bool lookup(char *path, unsigned long *p_ino)
{
	struct lookup_cookie cookie = {};
	reverse_append(&cookie.target_path, strlen(path), path);
	walk_inode_table(lookup_callback, &cookie);
	free(cookie.target_path.ptr);
	if (!cookie.found)
		return false;
	*p_ino = cookie.ino;
	return true;
}

static char *find_parent_for_new(char *path, unsigned long *ino)
{
	char *slash;
	if (lookup(path, ino)) {
		fprintf(stderr, "Path %s already exists (inode %lu)\n", path, *ino);
		exit(1);
	}
	slash = strrchr(path, '/');
	if (slash == NULL) {
		*ino = 0;
		return path;
	}
	*slash = '\0';
	if (!lookup(path, ino)) {
		fprintf(stderr, "Parent directory %s does not exist\n", path);
		exit(1);
	}
	if (!S_ISDIR(__le32_to_cpu(inode_table_ptr()[*ino].mode))) {
		fprintf(stderr, "Parent %s is not a directory\n", path);
		exit(1);
	}
	return slash + 1;
}

static void new_file_impl(char *path, bool dir)
{
	unsigned long parent_ino;
	struct sfs_inode *inode_table = inode_table_ptr();
	char *basename = find_parent_for_new(path, &parent_ino);
	size_t len = strlen(basename);

	if (len > sizeof(inode_table->name)) {
		fprintf(stderr, "Filename %s is too long\n", basename);
		exit(1);
	}

	for (unsigned long i = 1; i < total_inodes; ++i) {
		if (!inode_table[i].name[0]) {
			memcpy(inode_table[i].name, basename, len);
			memset(inode_table[i].name + len, 0, sizeof(inode_table->name) - len);
			inode_table[i].parent_dir = __cpu_to_le64(parent_ino);
			inode_table[i].file_size = 0;
			inode_table[i].mode = __cpu_to_le32((dir ? S_IFDIR : S_IFREG) | 0755);
			memset(inode_table[i].direct, 0, sizeof(inode_table[i].direct));
			inode_table[i].indirect = 0;
			return;
		}
	}
	fputs("No free inode for new file\n", stderr);
	exit(1);
}

void creat_(char *storage, char *path)
{
	set_up_mapping(storage, true);
	verify_and_load_sb();
	new_file_impl(path, false);
}

void mkdir_(char *storage, char *path)
{
	set_up_mapping(storage, true);
	verify_and_load_sb();
	new_file_impl(path, true);
}

static void remove_file_impl(unsigned long ino)
{
	struct sfs_inode *node = &inode_table_ptr()[ino];

	if (S_ISREG(__le32_to_cpu(node->mode))) {
		for (int i = 0; i < SFS_DIRECT_BLOCKS; i++) {
			__u64 blk = __le64_to_cpu(node->direct[i]);
			if (blk)
				bitmap_set(blk - data_start_block, false);
		}
		__u64 indirect = __le64_to_cpu(node->indirect);
		if (indirect) {
			__le64 *ptrs = (__le64 *)((char *)disk_image_mapping + indirect * SFS_BLOCK_SIZE);
			for (int p = 0; p < (int)SFS_PTRS_PER_INDIRECT; p++) {
				__u64 blk = __le64_to_cpu(ptrs[p]);
				if (blk)
					bitmap_set(blk - data_start_block, false);
			}
			bitmap_set(indirect - data_start_block, false);
		}
	}

	memset(node, 0, sizeof(*node));
}

void unlink_(char *storage, char *path)
{
	unsigned long ino;
	set_up_mapping(storage, true);
	verify_and_load_sb();
	if (!lookup(path, &ino)) {
		fprintf(stderr, "No such file %s\n", path);
		exit(1);
	}
	if (S_ISDIR(__le32_to_cpu(inode_table_ptr()[ino].mode))) {
		fprintf(stderr, "Path %s is a directory\n", path);
		exit(1);
	}
	remove_file_impl(ino);
}

void rmdir_(char *storage, char *path)
{
	unsigned long ino;
	set_up_mapping(storage, true);
	verify_and_load_sb();
	struct sfs_inode *inode_table = inode_table_ptr();
	if (!lookup(path, &ino)) {
		fprintf(stderr, "No such file %s\n", path);
		exit(1);
	}
	if (!S_ISDIR(__le32_to_cpu(inode_table[ino].mode))) {
		fprintf(stderr, "Path %s is a file\n", path);
		exit(1);
	}
	for (unsigned long i = 1; i < total_inodes; ++i) {
		if (inode_table[i].name[0] && __le64_to_cpu(inode_table[i].parent_dir) == ino) {
			fprintf(stderr, "Directory %s still contains children\n", path);
			exit(1);
		}
	}
	remove_file_impl(ino);
}

void dump(char *storage, char *path)
{
	unsigned long ino;
	set_up_mapping(storage, false);
	verify_and_load_sb();
	if (!lookup(path, &ino)) {
		fprintf(stderr, "No such file %s\n", path);
		exit(1);
	}
	struct sfs_inode *node = &inode_table_ptr()[ino];
	if (S_ISDIR(__le32_to_cpu(node->mode))) {
		fprintf(stderr, "Path %s is a directory\n", path);
		exit(1);
	}

	__u64 remaining = __le64_to_cpu(node->file_size);
	__u64 blk_idx = 0;

	while (remaining) {
		__u64 blk = file_block_ptr(node, blk_idx, false);
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

void alter(char *storage, char *path)
{
	unsigned long ino;
	set_up_mapping(storage, true);
	verify_and_load_sb();
	if (!lookup(path, &ino)) {
		fprintf(stderr, "No such file %s\n", path);
		exit(1);
	}
	struct sfs_inode *node = &inode_table_ptr()[ino];
	if (S_ISDIR(__le32_to_cpu(node->mode))) {
		fprintf(stderr, "Path %s is a directory\n", path);
		exit(1);
	}

	__u64 total = 0;
	__u64 blk_idx = 0;
	char buf[SFS_BLOCK_SIZE];
	size_t n;

	while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
		__u64 blk = file_block_ptr(node, blk_idx, true);
		memcpy((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE, buf, n);
		if (n < sizeof(buf))
			memset((char *)disk_image_mapping + blk * SFS_BLOCK_SIZE + n, 0, sizeof(buf) - n);
		total += n;
		blk_idx++;
		if (n < sizeof(buf))
			break;
	}

	node->file_size = __cpu_to_le64(total);
}
