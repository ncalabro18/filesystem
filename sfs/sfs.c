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

/* inode_operation callbacks */
static struct dentry* sfs_lookup (struct inode *, struct dentry *, unsigned int);
static int sfs_create (struct mnt_idmap *, struct inode *,struct dentry *, umode_t, bool);
static int sfs_unlink (struct inode *,struct dentry *);
static struct dentry* sfs_mkdir (struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
static int sfs_rmdir (struct inode *,struct dentry *);

static int sfs_init_fs_context(struct fs_context *fc);
static int sfs_get_tree(struct fs_context *fc);

/* file_system_type operations */
//static struct dentry* sfs_mount (struct file_system_type *fst, int, const char *dev_name, void *data);

// void sfs_free_fs(struct fs_context *fc);

/* callback to mount_bdev */
int sfs_fill_super(struct super_block *sb, struct fs_context *fc);

/* file_operations callbacks */
static int     sfs_fop_open   (struct inode *, struct file *);
//static int   sfs_fop_release(struct inode *, struct file *);
static ssize_t sfs_fop_read   (struct file *, char *, size_t, loff_t *);
static ssize_t sfs_fop_write  (struct file *, const char *, size_t, loff_t *);
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

/* helpers / libc */
int strnequal(const char *s1, const char *s2, size_t n);




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

	// copy superblock data into config so callers don't need to re-read block 0
	memcpy(&config->super, disk_super, sizeof(struct sfs_super));
	brelse(bh);

	sb->s_maxbytes = SFS_MAX_FILE_SIZE;
	sb->s_op = &sfs_super_operations;

	struct inode *root = sfs_lookup_inode(sb, 0);
	if (IS_ERR(root)) {
		printk("sfs_fill_super: lookup_inode error %ld\n", PTR_ERR(root));
		return PTR_ERR(root);
	}

	sb->s_root = d_make_root(root);
	if (sb->s_root == NULL) {
		printk("sfs_fill_super: d_make_root NULL\n");
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


// void sfs_free_fs(struct fs_context *fc) {
// return}

/*** super_operation callback definitions ***/

static void sfs_put_super (struct super_block *sb) {
	kfree(sb->s_fs_info);
}
static int sfs_write_inode
	(struct inode *in, struct writeback_control *wbc) {

	struct sfs_mount_config *config = (struct sfs_mount_config*) in->i_sb->s_fs_info;

	down_read(&config->lock);
	/*  being critical section  */

	sector_t block = 1 + in->i_ino;
	struct buffer_head *bhead = sb_bread(in->i_sb, block);
	if (bhead == NULL) {
		up_read(&config->lock);
		sfs_error_printk("sfs_write_inode: sb_bread NULL return\n");
		return -EINVAL;
	}

	struct sfs_inode *sfsnode = (struct sfs_inode*) bhead->b_data;

	sfsnode->file_size = cpu_to_le64(i_size_read(in));

	/* prevents reading old values from cache */
	mark_buffer_dirty(bhead);

	if (wbc->sync_mode == WB_SYNC_ALL) {
		int ret = sync_dirty_buffer(bhead);
		if (ret != 0) {
			up_read(&config->lock);
			brelse(bhead);
			sfs_error_printk("sfs_write_inode: sync_dirty_buffer error\n");
			return ret;
		}
	}

	brelse(bhead);

	/*   end critical section   */
	up_read(&config->lock);
	return 0;
}

static struct inode* sfs_lookup_inode
	(struct super_block *sb, unsigned int inode_number) {
	struct inode* node = iget_locked(sb, inode_number);
 	if (node == NULL) {
		sfs_error_printk("sfs_lookup_inode: iget_locked NULL return\n");
		return NULL;
	}

	if (node->i_state == I_NEW) {
		struct sfs_mount_config *config = (struct sfs_mount_config *) sb->s_fs_info;
		__u64 max_dirs = le64_to_cpu(config->super.max_dirs);

		node->i_ino = inode_number;

		if (inode_number < max_dirs) {
			node->i_mode = S_IFDIR | 0755;
			node->i_op = &sfs_dir_inode_operations;
			node->i_fop = &sfs_directory_operations;
		} else {
			node->i_mode = S_IFREG | 0644;
			node->i_op = &sfs_file_inode_operations;
			node->i_fop = &sfs_file_operations;
			node->i_mapping->a_ops = &sfs_address_space_operations;
		}

		/* unset the state */
		unlock_new_inode(node);
	}

	return node;
}

static struct inode* sfs_lookup_empty_inode
	(struct super_block *sb, unsigned int inode_number) {

	struct inode* node = iget_locked(sb, inode_number);
	if (node == NULL) {
		sfs_error_printk("sfs_lookup_empty_inode : iget_locked NULL return\n");
		return NULL;
	}

	struct buffer_head *bh = sb_bread(sb, 0);
	if (bh == NULL) {
		sfs_error_printk("sfs_lookup_empty_inode : sb_bread NULL return\n");
		return ERR_PTR(-EINVAL);
	}
	struct sfs_super *s = (struct sfs_super*) bh->b_data;
	
	if (node->i_state == I_NEW) {
		node->i_ino = inode_number;
		node->i_mapping->a_ops = &sfs_address_space_operations;
		node->i_fop = &sfs_file_operations;

		node->i_mode = 0777;
		// todo check if directory or not
		if (inode_number <= s->max_dirs) {
			/* directory */
			node->i_mode |= S_IFDIR;
			node->i_op = &sfs_dir_inode_operations;


		} else {
			/* regular file */
			node->i_mode |= S_IFREG;
			node->i_op = &sfs_file_inode_operations;

		}


		/* unset the new state */
		// node->i_state &= ~(I_NEW);
		unlock_new_inode(node);
	}
	return node;
}

static struct inode* sfs_lookup_sfs_inode(struct super_block *sb, unsigned int inode_number) {

	struct inode* node = iget_locked(sb, inode_number);
	if (node == NULL) {
		sfs_error_printk("sfs_lookup_sfs_inode: iget_locked NULL return\n");
		return NULL;
	}

	struct sfs_super *s = sfs_get_super(sb);
	if (s == NULL) {
		sfs_error_printk("sfs_lookup_sfs_inode: sfs_get_super returned NULL\n");
		return ERR_PTR(-EINVAL);
	}
	
	if (node->i_state == I_NEW) {
		node->i_ino = inode_number;
		node->i_op = &sfs_file_inode_operations;
		node->i_mapping->a_ops = &sfs_address_space_operations;
		node->i_fop = &sfs_file_operations;

		node->i_mode = 0777;
		// todo check if directory or not
		if (inode_number <= s->max_dirs) {
			/* directory */
			node->i_mode |= S_IFDIR;

		} else {
			/* regular file */
			node->i_mode |= S_IFREG;
		}
		

		/* unset the new state */
		// node->i_state &= ~(I_NEW);
		unlock_new_inode(node);
	}

	return node;
}

/* file_operations callbacks definitions */

static int     sfs_fop_open   (struct inode *, struct file *) {
	return 0;
}

//static int     sfs_fop_release(struct inode *, struct file *);
static ssize_t sfs_fop_read   (struct file *, char *, size_t, loff_t *);
static ssize_t sfs_fop_write  (struct file *, const char *, size_t, loff_t *);

static int     sfs_dop_iterate_shared (struct file *, struct dir_context *context) {
	//for (context.pos = 0; context.pos <
	sfs_error_printk("sfs_dop_iterate_shared unimp\n");
	return -1;
}

/* inode_operation callback definitions */

static struct dentry* sfs_lookup(struct inode *node, struct dentry *entry, unsigned int num) {
    struct sfs_super *s = sfs_get_super(node->i_sb);

    for (unsigned i = s->max_dirs; i < s->max_files; i++) {
        // TODO: read inode block, compare name to entry->d_name.name
    }

    return d_splice_alias(NULL, entry); // NULL until lookup is implemented
}

static int sfs_unlink (struct inode *node, struct dentry *de) {
	memset(node, '\0', sizeof(struct inode));
	return 0;
}

static struct dentry* sfs_mkdir (struct mnt_idmap *map, struct inode *node, struct dentry *d, umode_t mode) {

	struct sfs_super *s = sfs_get_super(node->i_sb);

	for (unsigned int i = 0; i < s->max_dirs; i++) {

	}
	return NULL;
}

static int sfs_rmdir(struct inode *node, struct dentry *de) {
    return 0;
}

static int sfs_rmdir (struct inode *,struct dentry *);

static int sfs_create(struct mnt_idmap *idmap, struct inode *node,
                      struct dentry *d, umode_t mode, bool excl) {
	struct sfs_super *s = sfs_get_super(node->i_sb);
	if (s == NULL) {
		sfs_error_printk("sfs_create: sfs_get_super returned NULL\n");
		return -EINVAL;
	}

	for (unsigned int i = s->max_dirs; i < s->max_files; i++) {
		struct inode *new_node = sfs_lookup_sfs_inode(node->i_sb, i);
		if (new_node == NULL)
			continue;
		// TODO: populate name, link dentry
		break;
	}
	return 0;
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

/* callback to sfs_read_folio */
static int sfs_get_block (struct inode *inode, sector_t iblock,
				struct buffer_head *bh_result, int create) {

	int sector_number = (inode->i_ino * SFS_MAX_FILE_SIZE)
			/ SFS_SECTOR_SIZE; // 512

	map_bh(bh_result, inode->i_sb, sector_number + iblock);

	if (create)
		set_buffer_new (bh_result);

	return 0;
}


/*** helper definitions ***/
struct sfs_super* sfs_get_super(struct super_block *sb) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	if (config == NULL)
		return NULL;
	return &config->super;
}

int strnequal(const char *s1, const char *s2, size_t n) {
	for (int i = 0; i < n; i++) {
		if (s1[i] == s2[i]) {
			continue;
		} else
			return 0;
	}
	return 1;
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


