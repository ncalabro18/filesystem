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
#include <linux/uidgid.h>
#include <linux/cred.h>
#include <linux/statfs.h>
#include <linux/bitmap.h>


#include "sfs.h"



#define SFS_SECTOR_SIZE 512
#define SFS_BLOCK_SIZE 4096

#define SFS_DIRENTS_PER_BLOCK (SFS_BLOCK_SIZE / sizeof(struct sfs_dirent))


#define SFS_DEBUG 1
#ifdef SFS_DEBUG
#define sfs_debug_printk(fmt, ...) printk(KERN_DEBUG "sfs(debug): " fmt, ##__VA_ARGS__)
#else
#define sfs_debug_printk(fmt, ...) do {} while (0)
#endif

#define sfs_error_printk(fmt, ...) printk(KERN_DEBUG "sfs(error): " fmt, ##__VA_ARGS__)


struct sfs_child_list {
	unsigned int *inos;
	unsigned int count;
	unsigned int capacity;
};

struct sfs_mount_config {
	struct rw_semaphore lock;
	struct sfs_super super;
	unsigned int *inode_free_stack;
	unsigned int inode_free_count;
	unsigned long *data_bitmap;
	__u64 data_block_count;

};


/*** function declarations ***/

/* helpers */
static int sfs_resolve_block(struct super_block *sb, struct sfs_mount_config *config,
                              struct sfs_inode *disk_ino, __u64 blk_idx, bool create,
                              __u64 *out_phys, bool *out_dirty);

static void sfs_free_data_block(struct sfs_mount_config *config, __u64 rel_block);

static void sfs_free_inode_blocks(struct super_block *sb, struct sfs_mount_config *config,
                                   struct sfs_inode *disk_ino);


static bool sfs_dir_is_empty(struct super_block *sb, struct sfs_mount_config *config,
							struct sfs_inode *dir_disk);
static int sfs_dir_add(struct super_block *sb, struct sfs_mount_config *config,
                        struct sfs_inode *dir_disk,
						const char *name, size_t namelen, unsigned int ino);
static int sfs_dir_remove(struct super_block *sb, struct sfs_mount_config *config,
                        	struct sfs_inode *dir_disk, const char *name, size_t namelen);

static int sfs_dir_find(struct super_block *sb, struct sfs_mount_config *config,
                         struct sfs_inode *dir_disk, const char *name, size_t namelen,
                         unsigned int *out_ino);

static struct inode* sfs_lookup_inode(struct super_block *sb, unsigned int inode_number);
struct sfs_super* sfs_get_super (struct super_block *sb);
static int sfs_read_disk_inode(struct super_block *sb, unsigned int ino, struct sfs_inode *out);
static int sfs_write_disk_inode(struct super_block *sb, unsigned int ino, struct sfs_inode *in);


static int sfs_build_free_lists(struct super_block *sb, struct sfs_mount_config *config);


static int sfs_build_data_bitmap(struct super_block *sb, struct sfs_mount_config *config);
static long sfs_alloc_inode_slot(struct sfs_mount_config *config);
static long sfs_alloc_data_block(struct sfs_mount_config *config);
static void sfs_free_data_block(struct sfs_mount_config *config, __u64 rel_block);
static int sfs_sync_bitmap_bit(struct super_block *sb, struct sfs_mount_config *config,
                                __u64 rel_block, bool set);



/* file inode_operation callbacks */
static int sfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr);


/* directory inode_operation callbacks */
static struct dentry* sfs_lookup (struct inode *, struct dentry *, unsigned int);
static int sfs_create (struct mnt_idmap *, struct inode *,struct dentry *, umode_t, bool);
static struct dentry* sfs_mkdir (struct mnt_idmap *, struct inode *, struct dentry *, umode_t);
static int sfs_rmdir (struct inode *,struct dentry *);
static int sfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry, unsigned int flags);
static int sfs_unlink (struct inode *,struct dentry *);
static int sfs_link(struct dentry *old_dentry, struct inode *dir,
            struct dentry *dentry);


/* symlink operation callbacks */
static int sfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *d, const char *target);
static const char* sfs_get_link(struct dentry *dentry, struct inode *inode,
			struct delayed_call *done);
	

static int sfs_init_fs_context(struct fs_context *fc);
static int sfs_get_tree(struct fs_context *fc);

/* callback to mount_bdev */
int sfs_fill_super(struct super_block *sb, struct fs_context *fc);

/* file_operations callbacks */
static int sfs_dop_iterate_shared (struct file *, struct dir_context *);

/* super_operation callbacks */
static void sfs_evict_inode(struct inode *inode);
static void sfs_put_super (struct super_block *);
static int  sfs_write_inode   (struct inode *, struct writeback_control *wbc);
static int  sfs_statfs(struct dentry *dentry, struct kstatfs *buf);

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
	.write_inode = sfs_write_inode,
	.statfs = sfs_statfs,
	.evict_inode = sfs_evict_inode
};

/* FILE operations */
static struct inode_operations sfs_file_inode_operations = {

	/* set/get metadata */
	.setattr = sfs_setattr,
	.getattr = simple_getattr
};

/* directory operations*/
static struct inode_operations sfs_dir_inode_operations = {
	.lookup = sfs_lookup,
	.create = sfs_create,
	.mkdir  = sfs_mkdir,
	.rmdir  = sfs_rmdir,
	.rename = sfs_rename,
	.unlink = sfs_unlink,
	.link   = sfs_link,
	.symlink = sfs_symlink
};

/* symlink operations */
static struct inode_operations sfs_symlink_inode_operations = {
	.get_link = sfs_get_link,
	.setattr  = sfs_setattr,
	.getattr  = simple_getattr
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

	/* iterates directory entries, dir_emit() is used for output */
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

	if (sfs_build_free_lists(sb, config)) {
		kfree(config->inode_free_stack);
		kfree(sb->s_fs_info);
		sb->s_fs_info = NULL;
		printk("sfs_fill_super: failed to build free lists\n");
		return -ENOMEM;
	}

	if (sfs_build_data_bitmap(sb, config)) {
		bitmap_free(config->data_bitmap);
		kfree(config->inode_free_stack);
		kfree(sb->s_fs_info);
		sb->s_fs_info = NULL;
		return -ENOMEM;
	}


	sb->s_maxbytes = (loff_t)SFS_MAX_FILE_BLOCKS * SFS_BLOCK_SIZE;
	sb->s_op = &sfs_super_operations;

	struct inode *root = sfs_lookup_inode(sb, 0);
	if (IS_ERR(root)) {
		printk("sfs_fill_super: lookup_inode error %ld\n", PTR_ERR(root));
		kfree(config->inode_free_stack);
		kfree(sb->s_fs_info);
		sb->s_fs_info = NULL;
		return PTR_ERR(root);
	}

	sb->s_root = d_make_root(root);
	if (sb->s_root == NULL) {
		printk("sfs_fill_super: d_make_root NULL\n");
		kfree(config->inode_free_stack);
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
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	if (config) {
		kfree(config->inode_free_stack);
	}
	kfree(sb->s_fs_info);
}

static int sfs_write_inode
	(struct inode *in, struct writeback_control *wbc) {

	struct sfs_mount_config *config = (struct sfs_mount_config*) in->i_sb->s_fs_info;
	struct sfs_inode disk_ino;
	struct timespec64 atime, mtime, ctime;

	down_write(&config->lock);

	if (sfs_read_disk_inode(in->i_sb, in->i_ino, &disk_ino)) {
		up_write(&config->lock);
		sfs_error_printk("sfs_write_inode: read_disk_inode failed\n");
		return -EIO;
	}

	disk_ino.file_size = cpu_to_le64(i_size_read(in));
	disk_ino.mode = cpu_to_le32(in->i_mode & 07777);
	disk_ino.uid = cpu_to_le32(from_kuid(&init_user_ns, in->i_uid));
	disk_ino.gid = cpu_to_le32(from_kgid(&init_user_ns, in->i_gid));
	disk_ino.num_links = cpu_to_le32(in->i_nlink);

	atime = inode_get_atime(in);
	mtime = inode_get_mtime(in);
	ctime = inode_get_ctime(in);
	disk_ino.access_time = cpu_to_le64(atime.tv_sec);
	disk_ino.modified_time = cpu_to_le64(mtime.tv_sec);
	disk_ino.metadata_modified_time = cpu_to_le64(ctime.tv_sec);

	if (sfs_write_disk_inode(in->i_sb, in->i_ino, &disk_ino)) {
		up_write(&config->lock);
		sfs_error_printk("sfs_write_inode: write_disk_inode failed\n");
		return -EIO;
	}

	up_write(&config->lock);
	return 0;
}

static int sfs_statfs(struct dentry *dentry, struct kstatfs *buf) {
	struct super_block *sb = dentry->d_sb;
	struct sfs_super *s = sfs_get_super(sb);
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;

	buf->f_type = 0x53465300;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = le64_to_cpu(s->total_blocks);
	buf->f_bfree = config->data_block_count - bitmap_weight(config->data_bitmap, config->data_block_count);
	buf->f_bavail = buf->f_bfree;
	buf->f_files = le64_to_cpu(s->total_inodes);
	buf->f_ffree = config->inode_free_count;
	buf->f_namelen = SFS_MAX_FILE_NAME - 1;

	return 0;
}

static void sfs_evict_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;

	truncate_inode_pages_final(&inode->i_data);

	if (!inode->i_nlink && config) {
		struct sfs_inode disk_ino;

		down_write(&config->lock);
		if (sfs_read_disk_inode(sb, inode->i_ino, &disk_ino) == 0) {
			__u32 mode = le32_to_cpu(disk_ino.mode);
			if (S_ISREG(mode) || S_ISLNK(mode) || S_ISDIR(mode))
				sfs_free_inode_blocks(sb, config, &disk_ino);
			memset(&disk_ino, 0, sizeof(disk_ino));
			sfs_write_disk_inode(sb, inode->i_ino, &disk_ino);
			config->inode_free_stack[config->inode_free_count++] = inode->i_ino;
		}
		up_write(&config->lock);
	}

	clear_inode(inode);
}

static int sfs_read_disk_inode(struct super_block *sb, unsigned int ino, struct sfs_inode *out) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	__u64 base = le64_to_cpu(config->super.inode_table_block) * sb->s_blocksize;
	unsigned int off = base + ino * sizeof(struct sfs_inode);
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
		dst += chunk; off += chunk; remaining -= chunk;
	}
	return 0;
}

static int sfs_write_disk_inode(struct super_block *sb, unsigned int ino, struct sfs_inode *in) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	__u64 base = le64_to_cpu(config->super.inode_table_block) * sb->s_blocksize;
	unsigned int off = base + ino * sizeof(struct sfs_inode);
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

static int sfs_truncate_blocks(struct inode *inode, loff_t new_size)
{
	struct super_block *sb = inode->i_sb;
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	struct sfs_inode disk_ino;
	__u64 new_blocks = DIV_ROUND_UP(new_size, sb->s_blocksize);
	int i, ret;

	down_write(&config->lock);
	if (sfs_read_disk_inode(sb, inode->i_ino, &disk_ino)) {
		up_write(&config->lock);
		return -EIO;
	}

	for (i = SFS_DIRECT_BLOCKS - 1; i >= (int)new_blocks; i--) {
		__u64 blk = le64_to_cpu(disk_ino.direct[i]);
		if (blk) {
			sfs_free_data_block(config, blk - data_start);
			sfs_sync_bitmap_bit(sb, config, blk - data_start, false);
			disk_ino.direct[i] = 0;
		}
	}

	__u64 indirect = le64_to_cpu(disk_ino.indirect);
	if (new_blocks <= SFS_DIRECT_BLOCKS) {
		if (indirect) {
			struct buffer_head *ibh = sb_bread(sb, indirect);
			if (ibh) {
				__le64 *ptrs = (__le64 *)ibh->b_data;
				int p;
				for (p = 0; p < SFS_PTRS_PER_INDIRECT; p++) {
					__u64 blk = le64_to_cpu(ptrs[p]);
					if (blk) {
						sfs_free_data_block(config, blk - data_start);
						sfs_sync_bitmap_bit(sb, config, blk - data_start, false);
					}
				}
				brelse(ibh);
			}
			sfs_free_data_block(config, indirect - data_start);
			sfs_sync_bitmap_bit(sb, config, indirect - data_start, false);
			disk_ino.indirect = 0;
		}
	} else if (indirect) {
		struct buffer_head *ibh = sb_bread(sb, indirect);
		if (ibh) {
			__le64 *ptrs = (__le64 *)ibh->b_data;
			__u64 keep = new_blocks - SFS_DIRECT_BLOCKS;
			int p;
			bool changed = false;
			for (p = SFS_PTRS_PER_INDIRECT - 1; p >= (int)keep; p--) {
				__u64 blk = le64_to_cpu(ptrs[p]);
				if (blk) {
					sfs_free_data_block(config, blk - data_start);
					sfs_sync_bitmap_bit(sb, config, blk - data_start, false);
					ptrs[p] = 0;
					changed = true;
				}
			}
			if (changed)
				mark_buffer_dirty(ibh);
			brelse(ibh);
		}
	}

	disk_ino.file_size = cpu_to_le64(new_size);
	ret = sfs_write_disk_inode(sb, inode->i_ino, &disk_ino) ? -EIO : 0;
	up_write(&config->lock);
	return ret;
}

static int sfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attr) {
    struct inode *inode = d_inode(dentry);
    int error;

    error = setattr_prepare(idmap, dentry, attr);
    if (error) {
        return error;
	}

	if (attr->ia_valid & ATTR_SIZE) {
		error = inode_newsize_ok(inode, attr->ia_size);
		if (error)
			return error;
		if (attr->ia_size < i_size_read(inode)) {
			error = sfs_truncate_blocks(inode, attr->ia_size);
			if (error)
				return error;
		}
		truncate_setsize(inode, attr->ia_size);
	}
    
    setattr_copy(idmap, inode, attr);
    mark_inode_dirty(inode);

    return 0;
}

static struct inode* sfs_lookup_inode
	(struct super_block *sb, unsigned int inode_number) {
	struct inode* node = iget_locked(sb, inode_number);
 	if (node == NULL) {
		sfs_error_printk("sfs_lookup_inode: iget_locked NULL return\n");
		return NULL;
	}

	if (node->i_state & I_NEW) {
		struct sfs_inode disk_ino;
		umode_t full_mode;
		struct timespec64 ts;

		if (sfs_read_disk_inode(sb, inode_number, &disk_ino)) {
			iget_failed(node);
			return ERR_PTR(-EIO);
		}

		full_mode = le32_to_cpu(disk_ino.mode);
		node->i_ino = inode_number;
		node->i_uid = make_kuid(&init_user_ns, le32_to_cpu(disk_ino.uid));
		node->i_gid = make_kgid(&init_user_ns, le32_to_cpu(disk_ino.gid));
		node->i_mode = full_mode;
		set_nlink(node, le32_to_cpu(disk_ino.num_links));

		ts.tv_nsec = 0;
		ts.tv_sec = le64_to_cpu(disk_ino.access_time);
		inode_set_atime_to_ts(node, ts);
		ts.tv_sec = le64_to_cpu(disk_ino.modified_time);
		inode_set_mtime_to_ts(node, ts);
		ts.tv_sec = le64_to_cpu(disk_ino.metadata_modified_time);
		inode_set_ctime_to_ts(node, ts);

		if (S_ISDIR(full_mode)) {
			node->i_op = &sfs_dir_inode_operations;
			node->i_fop = &sfs_directory_operations;
		} else if (S_ISLNK(full_mode)) {
			node->i_op = &sfs_symlink_inode_operations;
			node->i_mapping->a_ops = &sfs_address_space_operations;
			i_size_write(node, le64_to_cpu(disk_ino.file_size));
		} else {
			node->i_op = &sfs_file_inode_operations;
			node->i_fop = &sfs_file_operations;
			node->i_mapping->a_ops = &sfs_address_space_operations;
			i_size_write(node, le64_to_cpu(disk_ino.file_size));
		}

		unlock_new_inode(node);
	}

	return node;
}


/* file_operations callbacks definitions */

static int sfs_dop_iterate_shared (struct file *file, struct dir_context *ctx) {
	struct inode *dir = file_inode(file);
	struct super_block *sb = dir->i_sb;
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	struct sfs_inode dir_disk;
	__u64 nblocks, pos, b;

	if (!dir_emit_dots(file, ctx))
		return 0;

	down_read(&config->lock);
	if (sfs_read_disk_inode(sb, dir->i_ino, &dir_disk)) {
		up_read(&config->lock);
		return -EIO;
	}
	nblocks = DIV_ROUND_UP(le64_to_cpu(dir_disk.file_size), SFS_BLOCK_SIZE);
	pos = ctx->pos - 2;

	for (b = pos / SFS_DIRENTS_PER_BLOCK; b < nblocks; b++) {
		__u64 phys; bool dirty;
		struct buffer_head *bh;
		struct sfs_dirent *ents;
		unsigned int s, s_start;
		bool stop = false;

		if (sfs_resolve_block(sb, config, &dir_disk, b, false, &phys, &dirty) || !phys)
			continue;
		bh = sb_bread(sb, phys);
		if (!bh)
			continue;
		ents = (struct sfs_dirent *)bh->b_data;
		s_start = (b == pos / SFS_DIRENTS_PER_BLOCK) ? (unsigned int)(pos % SFS_DIRENTS_PER_BLOCK) : 0;

		for (s = s_start; s < SFS_DIRENTS_PER_BLOCK; s++) {
			unsigned int ino, namelen;
			struct sfs_inode child;
			unsigned char type;

			if (le64_to_cpu(ents[s].inode) == 0)
				continue;
			ino = (unsigned int)le64_to_cpu(ents[s].inode);
			if (sfs_read_disk_inode(sb, ino, &child))
				continue;

			namelen = strnlen(ents[s].name, sizeof(ents[s].name));
			type = S_ISDIR(le32_to_cpu(child.mode)) ? DT_DIR : DT_REG;
			ctx->pos = b * SFS_DIRENTS_PER_BLOCK + s + 2;

			if (!dir_emit(ctx, ents[s].name, namelen, ino, type)) {
				stop = true;
				break;
			}
		}
		brelse(bh);
		if (stop)
			break;
	}

	up_read(&config->lock);
	return 0;
}


/* dir inode_operation callback definitions */
static struct dentry* sfs_lookup(struct inode *dir, struct dentry *entry, unsigned int flags) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) dir->i_sb->s_fs_info;
	struct sfs_inode dir_disk;
	struct inode *found = NULL;
	unsigned int ino;
	int ret;

	down_read(&config->lock);
	if (sfs_read_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_read(&config->lock);
		return ERR_PTR(-EIO);
	}
	ret = sfs_dir_find(dir->i_sb, config, &dir_disk, entry->d_name.name, entry->d_name.len, &ino);
	up_read(&config->lock);

	if (ret == 0) {
		found = sfs_lookup_inode(dir->i_sb, ino);
		if (IS_ERR(found))
			return ERR_CAST(found);
	}

	return d_splice_alias(found, entry);
}

static int sfs_unlink (struct inode *dir, struct dentry *de) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) dir->i_sb->s_fs_info;
	struct inode *inode = de->d_inode;
	struct sfs_inode dir_disk, disk_ino;
	__u32 links;

	down_write(&config->lock);

	if (sfs_read_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}
	if (sfs_dir_remove(dir->i_sb, config, &dir_disk, de->d_name.name, de->d_name.len)) {
		up_write(&config->lock);
		return -ENOENT;
	}
	if (sfs_write_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}

	if (sfs_read_disk_inode(dir->i_sb, inode->i_ino, &disk_ino)) {
		up_write(&config->lock);
		return -EIO;
	}
	links = le32_to_cpu(disk_ino.num_links);
	if (links > 0)
		links--;
	disk_ino.num_links = cpu_to_le32(links);
	sfs_write_disk_inode(dir->i_sb, inode->i_ino, &disk_ino);

	up_write(&config->lock);

	/* Do NOT free blocks/slot here even if links reached 0 - the inode
	 * may still be open. sfs_evict_inode() reclaims it once the VFS is
	 * truly done, whether that's now or after the last close(). */
	drop_nlink(inode);
	mark_inode_dirty(inode);
	return 0;
}

static struct dentry* sfs_mkdir (struct mnt_idmap *map, struct inode *dir, struct dentry *d, umode_t mode) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) dir->i_sb->s_fs_info;
	struct sfs_inode dir_disk, disk_ino;
	struct inode *new_node;
	long slot;
	unsigned int existing;
	struct timespec64 now = current_time(dir);

	if (d->d_name.len >= SFS_MAX_FILE_NAME)
		return ERR_PTR(-ENAMETOOLONG);

	down_write(&config->lock);

	if (sfs_read_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return ERR_PTR(-EIO);
	}
	if (sfs_dir_find(dir->i_sb, config, &dir_disk, d->d_name.name, d->d_name.len, &existing) == 0) {
		up_write(&config->lock);
		return ERR_PTR(-EEXIST);
	}

	slot = sfs_alloc_inode_slot(config);
	if (slot < 0) {
		up_write(&config->lock);
		return ERR_PTR(-ENOSPC);
	}

	memset(&disk_ino, 0, sizeof(disk_ino));
	disk_ino.mode = cpu_to_le32(S_IFDIR | (mode & 07777));
	disk_ino.num_links = cpu_to_le32(2);
	disk_ino.access_time = disk_ino.modified_time =
		disk_ino.metadata_modified_time = disk_ino.birth_time = cpu_to_le64(now.tv_sec);

	if (sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino)) {
		config->inode_free_stack[config->inode_free_count++] = (unsigned int)slot;
		up_write(&config->lock);
		return ERR_PTR(-EIO);
	}

	if (sfs_dir_add(dir->i_sb, config, &dir_disk, d->d_name.name, d->d_name.len, (unsigned int)slot)) {
		memset(&disk_ino, 0, sizeof(disk_ino));
		sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino);
		config->inode_free_stack[config->inode_free_count++] = (unsigned int)slot;
		up_write(&config->lock);
		return ERR_PTR(-ENOSPC);
	}
	if (sfs_write_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return ERR_PTR(-EIO);
	}

	new_node = sfs_lookup_inode(dir->i_sb, (unsigned int)slot);
	if (IS_ERR(new_node)) {
		up_write(&config->lock);
		return ERR_CAST(new_node);
	}

	inode_init_owner(map, new_node, dir, mode);
	new_node->i_mode = S_IFDIR | (new_node->i_mode & 07777);

	disk_ino.uid = cpu_to_le32(from_kuid(&init_user_ns, new_node->i_uid));
	disk_ino.gid = cpu_to_le32(from_kgid(&init_user_ns, new_node->i_gid));
	if (sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino)) {
		up_write(&config->lock);
		return ERR_PTR(-EIO);
	}

	d_instantiate(d, new_node);
	up_write(&config->lock);
	return NULL;
}

static int sfs_rmdir(struct inode *dir, struct dentry *de) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) dir->i_sb->s_fs_info;
	struct inode *inode = de->d_inode;
	struct sfs_inode dir_disk, target_disk;

	down_write(&config->lock);

	if (sfs_read_disk_inode(dir->i_sb, inode->i_ino, &target_disk)) {
		up_write(&config->lock);
		return -EIO;
	}
	if (!sfs_dir_is_empty(dir->i_sb, config, &target_disk)) {
		up_write(&config->lock);
		return -ENOTEMPTY;
	}

	if (sfs_read_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}
	if (sfs_dir_remove(dir->i_sb, config, &dir_disk, de->d_name.name, de->d_name.len)) {
		up_write(&config->lock);
		return -ENOENT;
	}
	if (sfs_write_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}

	target_disk.num_links = cpu_to_le32(0);
	sfs_write_disk_inode(dir->i_sb, inode->i_ino, &target_disk);

	up_write(&config->lock);

	clear_nlink(inode);
	mark_inode_dirty(inode);
	return 0;
}

static int sfs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
                       struct inode *new_dir, struct dentry *new_dentry, unsigned int flags) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) old_dir->i_sb->s_fs_info;
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	struct sfs_inode old_dir_disk, new_dir_disk, target_disk;
	unsigned int old_ino = old_inode->i_ino;

	if (flags)
		return -EINVAL;
	if (new_dentry->d_name.len >= sizeof(((struct sfs_dirent *)0)->name))
		return -ENAMETOOLONG;

	down_write(&config->lock);

	/* NOTE: this format no longer stores a per-inode parent pointer, so
	 * the "moving a directory into its own descendant" cycle check from
	 * the previous version can't be reconstructed here without walking
	 * the tree top-down from root. Deliberately omitted - flag if this
	 * matters for your use case; it's a real, known gap versus before. */

	if (new_inode) {
		bool old_is_dir = S_ISDIR(old_inode->i_mode);
		bool new_is_dir = S_ISDIR(new_inode->i_mode);

		if (old_is_dir && !new_is_dir) { up_write(&config->lock); return -ENOTDIR; }
		if (!old_is_dir && new_is_dir) { up_write(&config->lock); return -EISDIR; }

		if (new_is_dir) {
			struct sfs_inode nd;
			if (sfs_read_disk_inode(old_dir->i_sb, new_inode->i_ino, &nd)) { up_write(&config->lock); return -EIO; }
			if (!sfs_dir_is_empty(old_dir->i_sb, config, &nd)) { up_write(&config->lock); return -ENOTEMPTY; }
		}
	}

	if (sfs_read_disk_inode(old_dir->i_sb, old_dir->i_ino, &old_dir_disk)) { up_write(&config->lock); return -EIO; }
	if (sfs_read_disk_inode(old_dir->i_sb, new_dir->i_ino, &new_dir_disk)) { up_write(&config->lock); return -EIO; }

	if (new_inode) {
		__u32 links;
		if (sfs_dir_remove(old_dir->i_sb, config, &new_dir_disk, new_dentry->d_name.name, new_dentry->d_name.len)) {
			up_write(&config->lock);
			return -EIO;
		}
		if (sfs_read_disk_inode(old_dir->i_sb, new_inode->i_ino, &target_disk)) { up_write(&config->lock); return -EIO; }
		links = le32_to_cpu(target_disk.num_links);
		if (links > 0) links--;
		target_disk.num_links = cpu_to_le32(links);
		sfs_write_disk_inode(old_dir->i_sb, new_inode->i_ino, &target_disk);
	}

	if (sfs_dir_remove(old_dir->i_sb, config, &old_dir_disk, old_dentry->d_name.name, old_dentry->d_name.len)) {
		up_write(&config->lock);
		return -EIO;
	}
	if (sfs_dir_add(old_dir->i_sb, config, &new_dir_disk, new_dentry->d_name.name, new_dentry->d_name.len, old_ino)) {
		up_write(&config->lock);
		return -ENOSPC;
	}

	sfs_write_disk_inode(old_dir->i_sb, old_dir->i_ino, &old_dir_disk);
	sfs_write_disk_inode(old_dir->i_sb, new_dir->i_ino, &new_dir_disk);

	up_write(&config->lock);

	if (new_inode) {
		if (S_ISDIR(new_inode->i_mode))
			clear_nlink(new_inode);
		else
			drop_nlink(new_inode);
		mark_inode_dirty(new_inode);
	}

	d_move(old_dentry, new_dentry);
	return 0;
}

static int sfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct sfs_mount_config *config = (struct sfs_mount_config*) dir->i_sb->s_fs_info;
	struct inode *inode = d_inode(old_dentry);
	struct sfs_inode dir_disk, disk_ino;
	unsigned int existing;

	if (S_ISDIR(inode->i_mode))
		return -EPERM;
	if (dentry->d_name.len >= sizeof(((struct sfs_dirent *)0)->name))
		return -ENAMETOOLONG;

	down_write(&config->lock);

	if (sfs_read_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}
	if (sfs_dir_find(dir->i_sb, config, &dir_disk, dentry->d_name.name,
			dentry->d_name.len, &existing) == 0) {
		up_write(&config->lock);
		return -EEXIST;
	}
	if (sfs_dir_add(dir->i_sb, config, &dir_disk, dentry->d_name.name,
			dentry->d_name.len, inode->i_ino)) {
		up_write(&config->lock);
		return -ENOSPC;
	}
	if (sfs_write_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}

	if (sfs_read_disk_inode(dir->i_sb, inode->i_ino, &disk_ino)) {
		up_write(&config->lock);
		return -EIO;
	}
	disk_ino.num_links = cpu_to_le32(le32_to_cpu(disk_ino.num_links) + 1);
	if (sfs_write_disk_inode(dir->i_sb, inode->i_ino, &disk_ino)) {
		up_write(&config->lock);
		return -EIO;
	}

	up_write(&config->lock);

	inc_nlink(inode);
	inode_set_ctime_current(inode);
	ihold(inode); /* d_instantiate for link doesn't take a reference itself */
	d_instantiate(dentry, inode);
	return 0;
}

/* directory entry helpers */

static int sfs_dir_find(struct super_block *sb, struct sfs_mount_config *config,
                         struct sfs_inode *dir_disk, const char *name, size_t namelen,
                         unsigned int *out_ino)
{
	__u64 nblocks = DIV_ROUND_UP(le64_to_cpu(dir_disk->file_size), SFS_BLOCK_SIZE);
	__u64 b;

	for (b = 0; b < nblocks; b++) {
		__u64 phys; bool dirty;
		struct buffer_head *bh;
		struct sfs_dirent *ents;
		unsigned int s;

		if (sfs_resolve_block(sb, config, dir_disk, b, false, &phys, &dirty) || !phys)
			continue;
		bh = sb_bread(sb, phys);
		if (!bh)
			continue;
		ents = (struct sfs_dirent *)bh->b_data;
		for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++) {
			size_t enl;
			if (le64_to_cpu(ents[s].inode) == 0)
				continue;
			enl = strnlen(ents[s].name, sizeof(ents[s].name));
			if (enl == namelen && memcmp(ents[s].name, name, namelen) == 0) {
				*out_ino = (unsigned int)le64_to_cpu(ents[s].inode);
				brelse(bh);
				return 0;
			}
		}
		brelse(bh);
	}
	return -ENOENT;
}

/* dir_disk may be mutated (file_size grows); caller must persist it after */
static int sfs_dir_add(struct super_block *sb, struct sfs_mount_config *config,
                        struct sfs_inode *dir_disk, const char *name, size_t namelen, unsigned int ino)
{
	__u64 nblocks = DIV_ROUND_UP(le64_to_cpu(dir_disk->file_size), SFS_BLOCK_SIZE);
	__u64 b, phys; bool dirty;
	struct buffer_head *bh;
	struct sfs_dirent *ents;
	unsigned int s;
	int ret;

	for (b = 0; b < nblocks; b++) {
		if (sfs_resolve_block(sb, config, dir_disk, b, false, &phys, &dirty) || !phys)
			continue;
		bh = sb_bread(sb, phys);
		if (!bh)
			continue;
		ents = (struct sfs_dirent *)bh->b_data;
		for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++) {
			if (le64_to_cpu(ents[s].inode) == 0) {
				memset(ents[s].name, 0, sizeof(ents[s].name));
				memcpy(ents[s].name, name, namelen);
				ents[s].inode = cpu_to_le64(ino);
				mark_buffer_dirty(bh);
				brelse(bh);
				return 0;
			}
		}
		brelse(bh);
	}

	ret = sfs_resolve_block(sb, config, dir_disk, nblocks, true, &phys, &dirty);
	if (ret)
		return ret;
	bh = sb_bread(sb, phys);
	if (!bh)
		return -EIO;
	memset(bh->b_data, 0, SFS_BLOCK_SIZE);
	ents = (struct sfs_dirent *)bh->b_data;
	memcpy(ents[0].name, name, namelen);
	ents[0].inode = cpu_to_le64(ino);
	mark_buffer_dirty(bh);
	brelse(bh);

	dir_disk->file_size = cpu_to_le64((nblocks + 1) * SFS_BLOCK_SIZE);
	return 0;
}

static int sfs_dir_remove(struct super_block *sb, struct sfs_mount_config *config,
                           struct sfs_inode *dir_disk, const char *name, size_t namelen)
{
	__u64 nblocks = DIV_ROUND_UP(le64_to_cpu(dir_disk->file_size), SFS_BLOCK_SIZE);
	__u64 b;

	for (b = 0; b < nblocks; b++) {
		__u64 phys; bool dirty;
		struct buffer_head *bh;
		struct sfs_dirent *ents;
		unsigned int s;

		if (sfs_resolve_block(sb, config, dir_disk, b, false, &phys, &dirty) || !phys)
			continue;
		bh = sb_bread(sb, phys);
		if (!bh)
			continue;
		ents = (struct sfs_dirent *)bh->b_data;
		for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++) {
			size_t enl;
			if (le64_to_cpu(ents[s].inode) == 0)
				continue;
			enl = strnlen(ents[s].name, sizeof(ents[s].name));
			if (enl == namelen && memcmp(ents[s].name, name, namelen) == 0) {
				memset(&ents[s], 0, sizeof(ents[s]));
				mark_buffer_dirty(bh);
				brelse(bh);
				return 0;
			}
		}
		brelse(bh);
	}
	return -ENOENT;
}

static bool sfs_dir_is_empty(struct super_block *sb, struct sfs_mount_config *config, struct sfs_inode *dir_disk)
{
	__u64 nblocks = DIV_ROUND_UP(le64_to_cpu(dir_disk->file_size), SFS_BLOCK_SIZE);
	__u64 b;

	for (b = 0; b < nblocks; b++) {
		__u64 phys; bool dirty;
		struct buffer_head *bh;
		struct sfs_dirent *ents;
		unsigned int s;

		if (sfs_resolve_block(sb, config, dir_disk, b, false, &phys, &dirty) || !phys)
			continue;
		bh = sb_bread(sb, phys);
		if (!bh)
			continue;
		ents = (struct sfs_dirent *)bh->b_data;
		for (s = 0; s < SFS_DIRENTS_PER_BLOCK; s++) {
			if (le64_to_cpu(ents[s].inode) != 0) {
				brelse(bh);
				return false;
			}
		}
		brelse(bh);
	}
	return true;
}

static int sfs_create(struct mnt_idmap *idmap, struct inode *dir,
                      struct dentry *d, umode_t mode, bool excl) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) dir->i_sb->s_fs_info;
	struct sfs_inode dir_disk, disk_ino;
	struct inode *new_node;
	long slot;
	unsigned int existing;
	struct timespec64 now = current_time(dir);

	if (config == NULL)
		return -EINVAL;
	if (d->d_name.len >= sizeof(((struct sfs_dirent *)0)->name))
		return -ENAMETOOLONG;

	down_write(&config->lock);

	if (sfs_read_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}
	if (sfs_dir_find(dir->i_sb, config, &dir_disk, d->d_name.name, d->d_name.len, &existing) == 0) {
		up_write(&config->lock);
		return -EEXIST;
	}

	slot = sfs_alloc_inode_slot(config);
	if (slot < 0) {
		up_write(&config->lock);
		return -ENOSPC;
	}

	memset(&disk_ino, 0, sizeof(disk_ino));
	disk_ino.mode = cpu_to_le32(S_IFREG | (mode & 07777));
	disk_ino.num_links = cpu_to_le32(1);
	disk_ino.access_time = disk_ino.modified_time =
		disk_ino.metadata_modified_time = disk_ino.birth_time = cpu_to_le64(now.tv_sec);

	if (sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino)) {
		config->inode_free_stack[config->inode_free_count++] = (unsigned int)slot;
		up_write(&config->lock);
		return -EIO;
	}

	if (sfs_dir_add(dir->i_sb, config, &dir_disk, d->d_name.name, d->d_name.len, (unsigned int)slot)) {
		memset(&disk_ino, 0, sizeof(disk_ino));
		sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino);
		config->inode_free_stack[config->inode_free_count++] = (unsigned int)slot;
		up_write(&config->lock);
		return -ENOSPC;
	}
	if (sfs_write_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}

	new_node = sfs_lookup_inode(dir->i_sb, (unsigned int)slot);
	if (IS_ERR(new_node)) {
		up_write(&config->lock);
		return PTR_ERR(new_node);
	}

	inode_init_owner(idmap, new_node, dir, mode);
	new_node->i_mode = S_IFREG | (new_node->i_mode & 07777);

	disk_ino.uid = cpu_to_le32(from_kuid(&init_user_ns, new_node->i_uid));
	disk_ino.gid = cpu_to_le32(from_kgid(&init_user_ns, new_node->i_gid));
	if (sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino)) {
		up_write(&config->lock);
		return -EIO;
	}

	d_instantiate(d, new_node);
	up_write(&config->lock);
	return 0;
}


/* symlink operation callbacks */
static int sfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *d, const char *target)
{
	struct sfs_mount_config *config = (struct sfs_mount_config*) dir->i_sb->s_fs_info;
	struct sfs_inode dir_disk, disk_ino;
	struct inode *new_node;
	size_t target_len = strlen(target);
	long slot;
	unsigned int existing;
	struct timespec64 now = current_time(dir);

	if (target_len == 0 || target_len >= PATH_MAX)
		return -ENAMETOOLONG;
	if (d->d_name.len >= SFS_MAX_FILE_NAME)
		return -ENAMETOOLONG;

	down_write(&config->lock);

	if (sfs_read_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}
	if (sfs_dir_find(dir->i_sb, config, &dir_disk, d->d_name.name, d->d_name.len, &existing) == 0) {
		up_write(&config->lock);
		return -EEXIST;
	}

	slot = sfs_alloc_inode_slot(config);
	if (slot < 0) {
		up_write(&config->lock);
		return -ENOSPC;
	}

	memset(&disk_ino, 0, sizeof(disk_ino));
	disk_ino.mode = cpu_to_le32(S_IFLNK | 0777);
	disk_ino.num_links = cpu_to_le32(1);
	disk_ino.access_time = disk_ino.modified_time =
		disk_ino.metadata_modified_time = disk_ino.birth_time = cpu_to_le64(now.tv_sec);

	/* Write the target string directly via buffer heads, matching
	 * sfs_get_link()'s read path exactly. This must NOT go through
	 * block_write_begin/the page cache - sb_bread() (used by
	 * sfs_get_link and everywhere else in this file) reads through the
	 * block device's own buffer cache, a *different* cache from the
	 * inode's page cache a folio-based write would dirty. Without this,
	 * a reader can observe stale/zeroed data until an unrelated
	 * writeback eventually flushes the folio. */
	{
		size_t written = 0;
		__u64 blk_idx = 0;

		while (written < target_len) {
			__u64 phys; bool dirty;
			struct buffer_head *bh;
			size_t chunk;
			int ret = sfs_resolve_block(dir->i_sb, config, &disk_ino, blk_idx, true, &phys, &dirty);
			if (ret) {
				config->inode_free_stack[config->inode_free_count++] = (unsigned int)slot;
				up_write(&config->lock);
				return ret;
			}
			bh = sb_bread(dir->i_sb, phys);
			if (!bh) {
				config->inode_free_stack[config->inode_free_count++] = (unsigned int)slot;
				up_write(&config->lock);
				return -EIO;
			}
			chunk = target_len - written;
			if (chunk > dir->i_sb->s_blocksize)
				chunk = dir->i_sb->s_blocksize;
			memcpy(bh->b_data, target + written, chunk);
			if (chunk < dir->i_sb->s_blocksize)
				memset(bh->b_data + chunk, 0, dir->i_sb->s_blocksize - chunk);
			mark_buffer_dirty(bh);
			brelse(bh);
			written += chunk;
			blk_idx++;
		}
	}
	disk_ino.file_size = cpu_to_le64(target_len);

	if (sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino)) {
		config->inode_free_stack[config->inode_free_count++] = (unsigned int)slot;
		up_write(&config->lock);
		return -EIO;
	}

	if (sfs_dir_add(dir->i_sb, config, &dir_disk, d->d_name.name, d->d_name.len, (unsigned int)slot)) {
		memset(&disk_ino, 0, sizeof(disk_ino));
		sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino);
		config->inode_free_stack[config->inode_free_count++] = (unsigned int)slot;
		up_write(&config->lock);
		return -ENOSPC;
	}
	if (sfs_write_disk_inode(dir->i_sb, dir->i_ino, &dir_disk)) {
		up_write(&config->lock);
		return -EIO;
	}

	new_node = sfs_lookup_inode(dir->i_sb, (unsigned int)slot);
	if (IS_ERR(new_node)) {
		up_write(&config->lock);
		return PTR_ERR(new_node);
	}

	inode_init_owner(idmap, new_node, dir, S_IFLNK | 0777);
	new_node->i_mode = S_IFLNK | 0777;

	disk_ino.uid = cpu_to_le32(from_kuid(&init_user_ns, new_node->i_uid));
	disk_ino.gid = cpu_to_le32(from_kgid(&init_user_ns, new_node->i_gid));
	if (sfs_write_disk_inode(dir->i_sb, (unsigned int)slot, &disk_ino)) {
		up_write(&config->lock);
		return -EIO;
	}

	up_write(&config->lock);

	d_instantiate(d, new_node);
	return 0;
}

static const char *sfs_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done)
{
	struct super_block *sb = inode->i_sb;
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	loff_t size = i_size_read(inode);
	struct sfs_inode disk_ino;
	char *buf;
	loff_t remaining, off = 0;
	__u64 blk_idx = 0;

	if (!dentry)
		return ERR_PTR(-ECHILD);
	if (size <= 0 || size >= PATH_MAX)
		return ERR_PTR(-EIO);

	buf = kmalloc(size + 1, GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	down_read(&config->lock);
	if (sfs_read_disk_inode(sb, inode->i_ino, &disk_ino)) {
		up_read(&config->lock);
		kfree(buf);
		return ERR_PTR(-EIO);
	}

	remaining = size;
	while (remaining > 0) {
		__u64 phys; bool dirty;
		struct buffer_head *bh;
		size_t chunk;

		if (sfs_resolve_block(sb, config, &disk_ino, blk_idx, false, &phys, &dirty) || !phys) {
			up_read(&config->lock);
			kfree(buf);
			return ERR_PTR(-EIO);
		}
		bh = sb_bread(sb, phys);
		if (!bh) {
			up_read(&config->lock);
			kfree(buf);
			return ERR_PTR(-EIO);
		}
		chunk = min_t(loff_t, remaining, sb->s_blocksize);
		memcpy(buf + off, bh->b_data, chunk);
		brelse(bh);
		off += chunk;
		remaining -= chunk;
		blk_idx++;
	}
	up_read(&config->lock);

	buf[size] = '\0';
	set_delayed_call(done, kfree_link, buf);
	return buf;
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
	struct super_block *sb = inode->i_sb;
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	struct sfs_inode disk_ino;
	__u64 phys = 0;
	bool dirty = false;
	int ret;

	down_write(&config->lock);

	if (sfs_read_disk_inode(sb, inode->i_ino, &disk_ino)) {
		up_write(&config->lock);
		return -EIO;
	}

	ret = sfs_resolve_block(sb, config, &disk_ino, iblock, create, &phys, &dirty);
	if (ret) {
		up_write(&config->lock);
		return ret;
	}

	if (dirty && sfs_write_disk_inode(sb, inode->i_ino, &disk_ino))
		ret = -EIO;

	up_write(&config->lock);

	if (ret)
		return ret;
	if (phys == 0)
		return 0; /* hole, create was false */

	if (create)
		set_buffer_new(bh_result);
	map_bh(bh_result, sb, phys);
	return 0;
}


/*** helper definitions ***/

static int sfs_resolve_block(struct super_block *sb, struct sfs_mount_config *config,
                              struct sfs_inode *disk_ino, __u64 blk_idx, bool create,
                              __u64 *out_phys, bool *out_dirty)
{
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	*out_dirty = false;
	*out_phys = 0;

	if (blk_idx < SFS_DIRECT_BLOCKS) {
		__u64 blk = le64_to_cpu(disk_ino->direct[blk_idx]);
		if (!blk) {
			long rel;
			if (!create)
				return 0;
			rel = sfs_alloc_data_block(config);
			if (rel < 0)
				return -ENOSPC;
			blk = data_start + rel;
			sfs_sync_bitmap_bit(sb, config, rel, true);
			disk_ino->direct[blk_idx] = cpu_to_le64(blk);
			*out_dirty = true;
		}
		*out_phys = blk;
		return 0;
	}

	__u64 idx = blk_idx - SFS_DIRECT_BLOCKS;
	if (idx >= SFS_PTRS_PER_INDIRECT)
		return -EFBIG;

	__u64 indirect = le64_to_cpu(disk_ino->indirect);
	if (!indirect) {
		struct buffer_head *ibh;
		long rel;
		if (!create)
			return 0;
		rel = sfs_alloc_data_block(config);
		if (rel < 0)
			return -ENOSPC;
		indirect = data_start + rel;
		sfs_sync_bitmap_bit(sb, config, rel, true);
		disk_ino->indirect = cpu_to_le64(indirect);
		*out_dirty = true;

		ibh = sb_bread(sb, indirect);
		if (!ibh)
			return -EIO;
		memset(ibh->b_data, 0, sb->s_blocksize);
		mark_buffer_dirty(ibh);
		brelse(ibh);
	}

	struct buffer_head *ibh = sb_bread(sb, indirect);
	if (!ibh)
		return -EIO;
	__le64 *ptrs = (__le64 *)ibh->b_data;
	__u64 blk = le64_to_cpu(ptrs[idx]);
	if (!blk) {
		long rel;
		if (!create) {
			brelse(ibh);
			return 0;
		}
		rel = sfs_alloc_data_block(config);
		if (rel < 0) {
			brelse(ibh);
			return -ENOSPC;
		}
		blk = data_start + rel;
		sfs_sync_bitmap_bit(sb, config, rel, true);
		ptrs[idx] = cpu_to_le64(blk);
		mark_buffer_dirty(ibh);
	}
	brelse(ibh);
	*out_phys = blk;
	return 0;
}

struct sfs_super* sfs_get_super(struct super_block *sb) {
	struct sfs_mount_config *config = (struct sfs_mount_config*) sb->s_fs_info;
	if (config == NULL)
		return NULL;
	return &config->super;
}

static void sfs_free_inode_blocks(struct super_block *sb, struct sfs_mount_config *config,
                                   struct sfs_inode *disk_ino)
{
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	int i;

	for (i = 0; i < SFS_DIRECT_BLOCKS; i++) {
		__u64 blk = le64_to_cpu(disk_ino->direct[i]);
		if (blk) {
			sfs_free_data_block(config, blk - data_start);
			sfs_sync_bitmap_bit(sb, config, blk - data_start, false);
		}
	}

	__u64 indirect = le64_to_cpu(disk_ino->indirect);
	if (indirect) {
		struct buffer_head *ibh = sb_bread(sb, indirect);
		if (ibh) {
			__le64 *ptrs = (__le64 *)ibh->b_data;
			int p;
			for (p = 0; p < SFS_PTRS_PER_INDIRECT; p++) {
				__u64 blk = le64_to_cpu(ptrs[p]);
				if (blk) {
					sfs_free_data_block(config, blk - data_start);
					sfs_sync_bitmap_bit(sb, config, blk - data_start, false);
				}
			}
			brelse(ibh);
		}
		sfs_free_data_block(config, indirect - data_start);
		sfs_sync_bitmap_bit(sb, config, indirect - data_start, false);
	}
}

static int sfs_build_free_lists(struct super_block *sb, struct sfs_mount_config *config)
{
	__u64 total_inodes = le64_to_cpu(config->super.total_inodes);
	__u64 i;

	config->inode_free_stack = kmalloc_array((size_t)total_inodes, sizeof(unsigned int), GFP_KERNEL);
	if (!config->inode_free_stack)
		return -ENOMEM;
	config->inode_free_count = 0;

	for (i = 1; i < total_inodes; i++) {
		struct sfs_inode disk_ino;
		if (sfs_read_disk_inode(sb, i, &disk_ino))
			continue;
		if (le32_to_cpu(disk_ino.num_links) == 0)
			config->inode_free_stack[config->inode_free_count++] = (unsigned int)i;
	}
	return 0;
}

static long sfs_alloc_inode_slot(struct sfs_mount_config *config)
{
	if (config->inode_free_count == 0)
		return -ENOSPC;
	return config->inode_free_stack[--config->inode_free_count];
}

static int sfs_sync_bitmap_bit(struct super_block *sb, struct sfs_mount_config *config,
                                __u64 rel_block, bool set)
{
	__u64 byte_off = rel_block / 8;
	unsigned int bit_off = rel_block % 8;
	__u64 abs_byte = le64_to_cpu(config->super.data_bitmap_block) * SFS_BLOCK_SIZE + byte_off;
	sector_t block = abs_byte / sb->s_blocksize;
	unsigned int block_off = abs_byte % sb->s_blocksize;
	struct buffer_head *bh = sb_bread(sb, block);

	if (!bh)
		return -EIO;
	if (set)
		bh->b_data[block_off] |= (1 << bit_off);
	else
		bh->b_data[block_off] &= ~(1 << bit_off);
	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

static int sfs_build_data_bitmap(struct super_block *sb, struct sfs_mount_config *config)
{
	__u64 total_blocks = le64_to_cpu(config->super.total_blocks);
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	__u64 bitmap_block = le64_to_cpu(config->super.data_bitmap_block);
	__u64 bitmap_blocks = le64_to_cpu(config->super.data_bitmap_blocks);
	__u64 i;

	config->data_block_count = total_blocks - data_start;
	config->data_bitmap = bitmap_zalloc(config->data_block_count, GFP_KERNEL);
	if (!config->data_bitmap)
		return -ENOMEM;

	for (i = 0; i < bitmap_blocks; i++) {
		struct buffer_head *bh = sb_bread(sb, bitmap_block + i);
		__u64 bit_base = i * SFS_BLOCK_SIZE * 8;
		unsigned int b;

		if (!bh) {
			bitmap_free(config->data_bitmap);
			return -EIO;
		}
		for (b = 0; b < SFS_BLOCK_SIZE * 8 && bit_base + b < config->data_block_count; b++) {
			if (bh->b_data[b / 8] & (1 << (b % 8)))
				set_bit(bit_base + b, config->data_bitmap);
		}
		brelse(bh);
	}
	return 0;
}

static long sfs_alloc_data_block(struct sfs_mount_config *config)
{
	unsigned long bit = find_first_zero_bit(config->data_bitmap, config->data_block_count);
	if (bit >= config->data_block_count)
		return -ENOSPC;
	set_bit(bit, config->data_bitmap);
	return (long)bit;
}

static void sfs_free_data_block(struct sfs_mount_config *config, __u64 rel_block)
{
	clear_bit(rel_block, config->data_bitmap);
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