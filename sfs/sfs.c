#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/kernel.h>
#include <linux/iversion.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/mpage.h>
#include <linux/types.h>

#include "sfs.h"


#define SFS_SECTOR_SIZE 512
#define SFS_BLOCK_SIZE 4096
#define SFS_MAX_FILE_SIZE 4096

#define SFS_DEBUG 1
#ifdef SFS_DEBUG
#define sfs_debug_printk(fmt, ...) printk(KERN_DEBUG "sfs(error): " fmt, ##__VA_ARGS__)
#else
#define sfs_debug_printk(fmt, ...) do {} while (0)
#endif

#define sfs_error_printk(fmt, ...) printk(KERN_DEBUG "sfs(debug): " fmt, ##__VA_ARGS__)


struct sfs_mount_config {
	struct rw_semaphore lock;
	struct sfs_super super;
};

/*** function declarations ***/

/* helpers */
static struct inode* sfs_lookup_inode(struct super_block *sb, unsigned int inode_number);
struct sfs_super* sfs_get_super (struct super_block *sb);
static int sfs_read_disk_inode(struct super_block *sb, unsigned int ino, struct sfs_inode *out);
static int sfs_write_disk_inode(struct super_block *sb, unsigned int ino, struct sfs_inode *in);
static sector_t sfs_data_block_for_inode(struct super_block *sb, unsigned int ino);

/* inode_operation callbacks */
static struct dentry* sfs_lookup (struct inode *, struct dentry *, unsigned int);
static int sfs_create (struct mnt_idmap *, struct inode *,struct dentry *, umode_t, bool);
static int sfs_unlink (struct inode *,struct dentry *);
static struct dentry* sfs_mkdir (struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
static int sfs_rmdir (struct inode *,struct dentry *);

static int sfs_init_fs_context(struct fs_context *fc);
static int sfs_get_tree(struct fs_context *fc);

/* callback to mount_bdev */
int sfs_fill_super(struct super_block *sb, struct fs_context *fc);

/* file_operations callbacks */
static int     sfs_dop_iterate_shared (struct file *, struct dir_context *);

/* super_operation callbacks */
static void sfs_put_super (struct super_block *);
static int  sfs_write_inode   (struct inode *, struct writeback_control *wbc);

/* address_space_operation callbacks */
static int sfs_read_folio  (struct file *, struct folio *);
static int sfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
                     loff_t pos, unsigned int len,
                     struct folio **foliop, void **fsdata);
static int sfs_write_pages (struct address_space *, struct writeback_control *);

static int sfs_get_block   (struct inode *inode, sector_t iblock,
				struct buffer_head *bh_result, int create);


/*** table structures to hold sfs callback functions ***/

static struct super_operations sfs_super_operations = {
	.put_super = sfs_put_super,
	.write_inode = sfs_write_inode
};

/* FILE operations */
static struct inode_operations sfs_file_inode_operations = {
	/* nothing needed yet */
};

/* directory operations*/
static struct inode_operations sfs_dir_inode_operations = {
	.lookup = sfs_lookup,
	.create = sfs_create,
	.mkdir  = sfs_mkdir,
	.unlink = sfs_unlink,
	.rmdir  = sfs_rmdir
};

static struct address_space_operations sfs_address_space_operations = {
	.dirty_folio = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.write_end = generic_write_end,
	.read_folio = sfs_read_folio,
	.writepages = sfs_write_pages,
	.write_begin = sfs_write_begin
};

static struct file_operations sfs_file_operations = {
	.read_iter      = generic_file_read_iter,
	.write_iter     = generic_file_write_iter,
	.llseek         = generic_file_llseek,
	.mmap           = generic_file_mmap,
	.fsync          = generic_file_fsync
};

static struct file_operations sfs_directory_operations = {
	.read           = generic_read_dir,
	.fsync          = noop_fsync,
	.release        = simple_transaction_release,
	.iterate_shared = sfs_dop_iterate_shared
};

static const struct fs_context_operations sfs_context_ops = {
	.get_tree = sfs_get_tree,
};

static struct file_system_type sfs_fs_type = {
	.owner		 = THIS_MODULE,
	.name		 = "sfs",
	.kill_sb	 = kill_block_super,
	.fs_flags	 = FS_REQUIRES_DEV,
	.init_fs_context = sfs_init_fs_context,
};

MODULE_ALIAS_FS("sfs");


/*** function definitions ***/
int sfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	sb->s_blocksize = sb_set_blocksize(sb, SFS_BLOCK_SIZE);
	if (sb->s_blocksize == 0) {
		printk("sfs_fill_super: sb_set_blocksize 0 return\n");
		return -EINVAL;
	}

	struct buffer_head *bh = sb_bread(sb, 0);
	if (bh == NULL) {
		printk("sfs_fill_super: sb_bread NULL return\n");
		return -EINVAL;
	}

	struct sfs_super *disk_super = (struct sfs_super*) bh->b_data;
	if (strncmp(disk_super->magic, "SIMPLEFS", 8) != 0) {
		printk("sfs_fill_super: bad magic\n");
		brelse(bh);
		return -EINVAL;
	}

	sb->s_fs_info = kzalloc(sizeof(struct sfs_mount_config), GFP_KERNEL);
	if (sb->s_fs_info == NULL) {
		brelse(bh);
		return -ENOMEM;
	}

	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	init_rwsem(&config->lock);

	memcpy(&config->super, disk_super, sizeof(struct sfs_super));
	brelse(bh);

	sb->s_maxbytes = SFS_MAX_FILE_SIZE;
	sb->s_op = &sfs_super_operations;

	struct inode *root = sfs_lookup_inode(sb, 0);
	if (IS_ERR(root)) {
		printk("sfs_fill_super: lookup_inode error %ld\n", PTR_ERR(root));
		kfree(sb->s_fs_info);
		sb->s_fs_info = NULL;
		return PTR_ERR(root);
	}

	sb->s_root = d_make_root(root);
	if (sb->s_root == NULL) {
		printk("sfs_fill_super: d_make_root NULL\n");
		kfree(sb->s_fs_info);
		sb->s_fs_info = NULL;
		return -ENOMEM;
	}

	return 0;
}


static int sfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &sfs_context_ops;
	return 0;
}
static int sfs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, sfs_fill_super);
}

/*** super_operation callback definitions ***/

static void sfs_put_super (struct super_block *sb) {
	kfree(sb->s_fs_info);
}

static int sfs_write_inode
	(struct inode *in, struct writeback_control *wbc) {

	struct sfs_mount_config *config = (struct sfs_mount_config*) in->i_sb->s_fs_info;
	struct sfs_inode disk_ino;

	down_write(&config->lock);

	if (sfs_read_disk_inode(in->i_sb, in->i_ino, &disk_ino)) {
		up_write(&config->lock);
		sfs_error_printk("sfs_write_inode: read_disk_inode failed\n");
		return -EIO;
	}

	disk_ino.file_size = cpu_to_le64(i_size_read(in));

	if (sfs_write_disk_inode(in->i_sb, in->i_ino, &disk_ino)) {
		up_write(&config->lock);
		sfs_error_printk("sfs_write_inode: write_disk_inode failed\n");
		return -EIO;
	}

	up_write(&config->lock);
	return 0;
}

/*
 * Reads/writes a single on-disk sfs_inode struct at the given inode index.
 * Handles the entry straddling a block boundary generically rather than
 * requiring the metadata table to be block-aligned.
 */
static int sfs_read_disk_inode(struct super_block *sb, unsigned int ino, struct sfs_inode *out) {
	unsigned int off = sizeof(struct sfs_super) + ino * sizeof(struct sfs_inode);
	unsigned int remaining = sizeof(struct sfs_inode);
	char *dst = (char *) out;

	while (remaining) {
		sector_t block = off / sb->s_blocksize;
		unsigned int block_off = off % sb->s_blocksize;
		unsigned int chunk = min_t(unsigned int, remaining, sb->s_blocksize - block_off);

		struct buffer_head *bh = sb_bread(sb, block);
		if (bh == NULL)
			return -EIO;

		memcpy(dst, bh->b_data + block_off, chunk);
		brelse(bh);

		dst += chunk;
		off += chunk;
		remaining -= chunk;
	}
	return 0;
}

static int sfs_write_disk_inode(struct super_block *sb, unsigned int ino, struct sfs_inode *in) {
	unsigned int off = sizeof(struct sfs_super) + ino * sizeof(struct sfs_inode);
	unsigned int remaining = sizeof(struct sfs_inode);
	const char *src = (const char *) in;

	while (remaining) {
		sector_t block = off / sb->s_blocksize;
		unsigned int block_off = off % sb->s_blocksize;
		unsigned int chunk = min_t(unsigned int, remaining, sb->s_blocksize - block_off);

		struct buffer_head *bh = sb_bread(sb, block);
		if (bh == NULL)
			return -EIO;

		memcpy(bh->b_data + block_off, src, chunk);
		mark_buffer_dirty(bh);
		brelse(bh);

		src += chunk;
		off += chunk;
		remaining -= chunk;
	}
	return 0;
}

/*
 * Regular files get one fixed SFS_MAX_FILE_SIZE-byte slot each, located
 * after the (superblock + inode metadata table) region. Directories never
 * call this - their "contents" are derived by scanning parent_dir fields.
 */
static sector_t sfs_data_block_for_inode(struct super_block *sb, unsigned int ino) {
	struct sfs_super *s = sfs_get_super(sb);
	__u64 max_dirs = le64_to_cpu(s->max_dirs);
	__u64 max_files = le64_to_cpu(s->max_files);
	__u64 total_inodes = max_dirs + max_files;

	unsigned int metadata_bytes = sizeof(struct sfs_super) +
		total_inodes * sizeof(struct sfs_inode);
	sector_t data_start_block = DIV_ROUND_UP(metadata_bytes, sb->s_blocksize);

	unsigned int blocks_per_file = SFS_MAX_FILE_SIZE / sb->s_blocksize;
	if (blocks_per_file == 0)
		blocks_per_file = 1;

	unsigned int file_index = ino - max_dirs;
	return data_start_block + (sector_t) file_index * blocks_per_file;
}

static struct inode* sfs_lookup_inode
	(struct super_block *sb, unsigned int inode_number) {
	struct inode* node = iget_locked(sb, inode_number);
 	if (node == NULL) {
		sfs_error_printk("sfs_lookup_inode: iget_locked NULL return\n");
		return NULL;
	}

	if (node->i_state & I_NEW) {
		struct sfs_mount_config *config = (struct sfs_mount_config *) sb->s_fs_info;
		__u64 max_dirs = le64_to_cpu(config->super.max_dirs);
		struct sfs_inode disk_ino;

		if (sfs_read_disk_inode(sb, inode_number, &disk_ino)) {
			iget_failed(node);
			return ERR_PTR(-EIO);
		}

		node->i_ino = inode_number;

		if (inode_number < max_dirs) {
			node->i_mode = S_IFDIR | 0755;
			node->i_op = &sfs_dir_inode_operations;
			node->i_fop = &sfs_directory_operations;
			set_nlink(node, 2);
		} else {
			node->i_mode = S_IFREG | 0644;
			node->i_op = &sfs_file_inode_operations;
			node->i_fop = &sfs_file_operations;
			node->i_mapping->a_ops = &sfs_address_space_operations;
			i_size_write(node, le64_to_cpu(disk_ino.file_size));
			set_nlink(node, 1);
		}

		unlock_new_inode(node);
	}

	return node;
}

/* file_operations callbacks definitions */

static int     sfs_dop_iterate_shared (struct file *file, struct dir_context *ctx) {
	struct inode *dir = file_inode(file);
	struct sfs_super *s = sfs_get_super(dir->i_sb);
	__u64 total = le64_to_cpu(s->max_dirs) + le64_to_cpu(s->max_files);
	__u64 i;

	if (!dir_emit_dots(file, ctx))
		return 0;

	/* ctx->pos starts at 2 after dir_emit_dots; map pos-2 to inode index */
	for (i = ctx->pos - 2; i < total; i++, ctx->pos++) {
		struct sfs_inode disk_ino;
		unsigned int namelen;
		unsigned char type;

		if (sfs_read_disk_inode(dir->i_sb, i, &disk_ino))
			continue;
		if (disk_ino.name[0] == '\0')
			continue;
		if (le64_to_cpu(disk_ino.parent_dir) != dir->i_ino)
			continue;

		namelen = strnlen(disk_ino.name, sizeof(disk_ino.name));
		type = (i < le64_to_cpu(s->max_dirs)) ? DT_DIR : DT_REG;

		if (!dir_emit(ctx, disk_ino.name, namelen, i, type))
			return 0;
	}
	return 0;
}

/* inode_operation callback definitions */

static struct dentry* sfs_lookup(struct inode *dir, struct dentry *entry, unsigned int flags) {
	struct sfs_super *s = sfs_get_super(dir->i_sb);
	__u64 total = le64_to_cpu(s->max_dirs) + le64_to_cpu(s->max_files);
	struct inode *found = NULL;
	__u64 i;

	for (i = 0; i < total; i++) {
		struct sfs_inode disk_ino;
		size_t namelen = entry->d_name.len;

		if (sfs_read_disk_inode(dir->i_sb, i, &disk_ino))
			continue;
		if (disk_ino.name[0] == '\0')
			continue;
		if (le64_to_cpu(disk_ino.parent_dir) != dir->i_ino)
			continue;

		if (namelen > sizeof(disk_ino.name))
			continue;
		if (memcmp(disk_ino.name, entry->d_name.name, namelen) != 0)
			continue;
		/* ensure no leftover bytes after the matched name */
		if (namelen < sizeof(disk_ino.name) && disk_ino.name[namelen] != '\0')
			continue;

		found = sfs_lookup_inode(dir->i_sb, i);
		break;
	}

	if (IS_ERR(found))
		return ERR_CAST(found);

	return d_splice_alias(found, entry);
}

static int sfs_unlink (struct inode *dir, struct dentry *de) {
	struct sfs_inode disk_ino;
	unsigned int ino = de->d_inode->i_ino;

	if (sfs_read_disk_inode(dir->i_sb, ino, &disk_ino))
		return -EIO;

	memset(&disk_ino, 0, sizeof(disk_ino));

	if (sfs_write_disk_inode(dir->i_sb, ino, &disk_ino))
		return -EIO;

	return 0;
}

static struct dentry* sfs_mkdir (struct mnt_idmap *map, struct inode *dir, struct dentry *d, umode_t mode) {
	struct sfs_super *s = sfs_get_super(dir->i_sb);
	__u64 max_dirs = le64_to_cpu(s->max_dirs);
	__u64 i;

	for (i = 0; i < max_dirs; i++) {
		struct sfs_inode disk_ino;
		struct inode *new_node;

		if (sfs_read_disk_inode(dir->i_sb, i, &disk_ino))
			continue;
		if (disk_ino.name[0] != '\0')
			continue; /* slot in use */

		memset(&disk_ino, 0, sizeof(disk_ino));
		strncpy(disk_ino.name, d->d_name.name, sizeof(disk_ino.name) - 1);
		disk_ino.parent_dir = cpu_to_le64(dir->i_ino);
		disk_ino.file_size = 0;

		if (sfs_write_disk_inode(dir->i_sb, i, &disk_ino))
			return ERR_PTR(-EIO);

		new_node = sfs_lookup_inode(dir->i_sb, i);
		if (IS_ERR(new_node))
			return ERR_CAST(new_node);

		d_instantiate(d, new_node);
		return NULL;
	}

	return ERR_PTR(-ENOSPC);
}

static int sfs_rmdir(struct inode *dir, struct dentry *de) {
	struct sfs_super *s = sfs_get_super(dir->i_sb);
	__u64 total = le64_to_cpu(s->max_dirs) + le64_to_cpu(s->max_files);
	unsigned int target_ino = de->d_inode->i_ino;
	__u64 i;

	/* refuse to remove a non-empty directory */
	for (i = 0; i < total; i++) {
		struct sfs_inode disk_ino;

		if (sfs_read_disk_inode(dir->i_sb, i, &disk_ino))
			continue;
		if (disk_ino.name[0] == '\0')
			continue;
		if (le64_to_cpu(disk_ino.parent_dir) == target_ino)
			return -ENOTEMPTY;
	}

	return sfs_unlink(dir, de);
}

static int sfs_create(struct mnt_idmap *idmap, struct inode *dir,
                      struct dentry *d, umode_t mode, bool excl) {
	struct sfs_super *s = sfs_get_super(dir->i_sb);
	__u64 max_dirs = le64_to_cpu(s->max_dirs);
	__u64 max_files = le64_to_cpu(s->max_files);
	__u64 i;

	if (s == NULL) {
		sfs_error_printk("sfs_create: sfs_get_super returned NULL\n");
		return -EINVAL;
	}

	for (i = max_dirs; i < max_dirs + max_files; i++) {
		struct sfs_inode disk_ino;
		struct inode *new_node;

		if (sfs_read_disk_inode(dir->i_sb, i, &disk_ino))
			continue;
		if (disk_ino.name[0] != '\0')
			continue; /* slot in use */

		memset(&disk_ino, 0, sizeof(disk_ino));
		strncpy(disk_ino.name, d->d_name.name, sizeof(disk_ino.name) - 1);
		disk_ino.parent_dir = cpu_to_le64(dir->i_ino);
		disk_ino.file_size = 0;

		if (sfs_write_disk_inode(dir->i_sb, i, &disk_ino))
			return -EIO;

		new_node = sfs_lookup_inode(dir->i_sb, i);
		if (IS_ERR(new_node))
			return PTR_ERR(new_node);

		d_instantiate(d, new_node);
		return 0;
	}

	return -ENOSPC;
}


/*** address_space_operation callbacks ***/

static int sfs_read_folio  (struct file *s, struct folio *fio) {
	int ret = block_read_full_folio(fio, sfs_get_block);
	if (ret != 0)
		sfs_error_printk("sfs_read_folio: block_read_full_folio non 0 return\n");
	return ret;
}

static int sfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
                     loff_t pos, unsigned int len,
                     struct folio **foliop, void **fsdata) {
	int ret = block_write_begin(mapping, pos, len, foliop, sfs_get_block);
	if (ret != 0)
		sfs_error_printk("sfs_write_begin: block_write_begin error\n");

	return ret;
}

static int sfs_write_pages (struct address_space *as, struct writeback_control *wc) {
	int ret = mpage_writepages (as, wc, sfs_get_block);
	if (ret != 0) {
		sfs_error_printk("sfs_write_pages: mpage_writepages non 0 return\n");
	}
	return ret;
}

/* callback to sfs_read_folio / sfs_write_begin */
static int sfs_get_block (struct inode *inode, sector_t iblock,
				struct buffer_head *bh_result, int create) {

	sector_t block = sfs_data_block_for_inode(inode->i_sb, inode->i_ino) + iblock;

	map_bh(bh_result, inode->i_sb, block);

	if (create)
		set_buffer_new(bh_result);

	return 0;
}


/*** helper definitions ***/
struct sfs_super* sfs_get_super(struct super_block *sb) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	if (config == NULL)
		return NULL;
	return &config->super;
}

/*** Module callbacks and setup ***/

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SFS filesystem support");
MODULE_AUTHOR("Nicholas Calabro");

static int __init init_sfs_fs(void) {
	return register_filesystem(&sfs_fs_type);
}

static void __exit exit_sfs_fs(void) {
	unregister_filesystem(&sfs_fs_type);
}

module_init(init_sfs_fs)
module_exit(exit_sfs_fs)