/* This file was provided with instructions describing implementation details of a simple filesystem
	from Introduction to Linux Development offered by UMass Lowell, designed by Red Hat */
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../sfs/sfs.h"


static void *disk_image_mapping;
static size_t mapping_size;
static size_t max_file_size;
static unsigned long max_inodes;
static unsigned long max_dirs;

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

_Static_assert(sizeof(__u64) == sizeof(unsigned long) && sizeof(__u64) == sizeof(off_t), "Userspace types are too small");
static void verify_and_load_sb(void)
{
	struct sfs_super *sb = disk_image_mapping;
	__u64 sb_max_dirs = __le64_to_cpu(sb->max_dirs);
	__u64 sb_max_files = __le64_to_cpu(sb->max_files);
	__u64 sb_entry_size = __le64_to_cpu(sb->entry_size);
	__u64 sb_max_inodes, inodes_size, dir_space, total_size;
	if (memcmp(sb->magic, "SIMPLEFS", sizeof(sb->magic))) {
		fputs("Invalid FS magic in super block\n", stderr);
		exit(1);
	}
	if (__builtin_add_overflow(sb_max_dirs, sb_max_files, &sb_max_inodes)) {
		fputs("Invalid metadata in super block (overflow)\n", stderr);
		exit(1);
	}
	max_file_size = (size_t)sb_entry_size;
	if (max_file_size <= 0) {
		fputs("Invalid metadata in super block (underflow)\n", stderr);
		exit(1);
	}
	max_inodes = (unsigned long)sb_max_inodes;
	max_dirs = (unsigned long)sb_max_dirs;
	if (__builtin_mul_overflow(sb_max_inodes, sizeof(struct sfs_inode), &inodes_size) ||
	    __builtin_mul_overflow(sb_max_dirs, sb_entry_size, &dir_space) ||
	    __builtin_mul_overflow(sb_max_inodes, sb_entry_size, &total_size) ||
	    (__u64)mapping_size < total_size) {
		fputs("Invalid metadata in super block (not enough space)\n", stderr);
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
	struct sfs_inode *inode_tbl = disk_image_mapping;
	bool done = false;
	for (unsigned long i = 1; !done && i < max_inodes; ++i) {
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
	__u64 sb_size, sb_dirs, sb_inodes;
	struct sfs_super super = {
		.magic = {'S', 'I', 'M', 'P', 'L', 'E', 'F', 'S',},
	};
	set_up_mapping(storage, true);
	sb_size = size ? strtou64(size) : 4096;
	if (sb_size % 512) {
		fputs("Invalid block size, must be multiple of 512 bytes\n", stderr);
	}
	sb_inodes = entries ? strtou64(entries) : mapping_size / sb_size;
	sb_dirs = dirs ? strtou64(dirs) : 1 + ((sb_inodes - 1) / (sb_size / sizeof(struct sfs_inode)));
	if (sb_inodes < sb_dirs) {
		fputs("Number of directories exceeds overall limit on file count\n", stderr);
		exit(1);
	}
	super.entry_size = __cpu_to_le64(sb_size);
	super.max_dirs = __cpu_to_le64(sb_dirs);
	super.max_files = __cpu_to_le64(sb_inodes - sb_dirs);
	memcpy(disk_image_mapping, &super, sizeof(super));
	verify_and_load_sb();
	memset(disk_image_mapping + sizeof(super), 0, sizeof(struct sfs_inode) * sb_inodes);
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
	*ptr++ = '\0';
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
	/* child of root dir */
	if (slash == NULL) {
		*ino = 0;
		return path;
	}
	*slash = '\0'; /* split on final slash */
	if (!lookup(path, ino)) {
		fprintf(stderr, "Parent directory %s does not exist\n", path);
		exit(1);
	}
	return slash + 1;
}

static void new_file_impl(char *path, bool dir)
{
	unsigned long ino, start = dir ? 1 : max_dirs, end = dir ? max_dirs : max_inodes;
	struct sfs_inode *inode_table = disk_image_mapping;
	char *basename = find_parent_for_new(path, &ino);
	size_t len = strlen(basename);
	if (len > sizeof(inode_table->name)) {
		fprintf(stderr, "Filename %s is too long\n", basename);
		exit(1);
	}
	if (ino >= max_dirs) {
		fprintf(stderr, "Parent %s is not a directory\n", path);
		exit(1);
	}
	for (unsigned long i = start; i < end; ++i) {
		if (!inode_table[i].name[0]) {
			memcpy(inode_table[i].name, basename, len);
			memset(inode_table[i].name + len, 0, sizeof(inode_table->name) - len);
			inode_table[i].parent_dir = __cpu_to_le64(ino);
			inode_table[i].file_size = 0;
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
	struct sfs_inode *inode_table = disk_image_mapping;
	unsigned long parent = __le64_to_cpu(inode_table[ino].parent_dir);
	memset(&inode_table[ino], 0, sizeof(inode_table[ino]));
	inode_table[parent].file_size = __cpu_to_le64(__le64_to_cpu(inode_table[parent].file_size) - 1);
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
	if (ino < max_dirs) {
		fprintf(stderr, "Path %s is a directory\n", path);
		exit(1);
	}
	remove_file_impl(ino);
}

void rmdir_(char *storage, char *path)
{
	struct sfs_inode *inode_table;
	unsigned long ino;
	set_up_mapping(storage, true);
	verify_and_load_sb();
	inode_table = disk_image_mapping;
	if (!lookup(path, &ino)) {
		fprintf(stderr, "No such file %s\n", path);
		exit(1);
	}
	if (ino >= max_dirs) {
		fprintf(stderr, "Path %s is a file\n", path);
		exit(1);
	}
	for (unsigned long i = 1; i < max_inodes; ++i) {
		if (__cpu_to_le64(ino) == inode_table[i].parent_dir) {
			fprintf(stderr, "Directory %s still contains children\n", path);
			exit(1);
		}
	}
	remove_file_impl(ino);
}

void dump(char *storage, char *path)
{
	struct sfs_inode *inode_table;
	unsigned long ino;
	size_t size;
	set_up_mapping(storage, false);
	verify_and_load_sb();
	inode_table = disk_image_mapping;
	if (!lookup(path, &ino)) {
		fprintf(stderr, "No such file %s\n", path);
		exit(1);
	}
	if (ino < max_dirs) {
		fprintf(stderr, "Path %s is a directory\n", path);
		exit(1);
	}
	size = __le64_to_cpu(inode_table[ino].file_size);
	fwrite(disk_image_mapping + ino * max_file_size, sizeof(char), size, stdout);
}

void alter(char *storage, char *path)
{
	struct sfs_inode *inode_table;
	unsigned long ino;
	size_t size;
	set_up_mapping(storage, true);
	verify_and_load_sb();
	inode_table = disk_image_mapping;
	if (!lookup(path, &ino)) {
		fprintf(stderr, "No such file %s\n", path);
		exit(1);
	}
	if (ino < max_dirs) {
		fprintf(stderr, "Path %s is a directory\n", path);
		exit(1);
	}
	size = fread(disk_image_mapping + ino * max_file_size, sizeof(char), max_file_size, stdin);
	inode_table[ino].file_size = __cpu_to_le64(size);
}

