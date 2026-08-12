#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bitmap.h>
#include <linux/sched.h>
#include <linux/errno.h>

#include "sfs_extent.h"

#include "sfs_dir_index.h"

__u32 sfs_name_hash(const char *name, size_t len)
{
	__u32 h = 2166136261u;
	size_t i;
	for (i = 0; i < len; i++) {
		h ^= (unsigned char)name[i];
		h *= 16777619u;
	}
	return h % SFS_DIR_INDEX_BUCKETS;
}

int sfs_dir_ensure_index(struct super_block *sb, struct sfs_mount_config *config,
                                 struct sfs_inode *dir_disk)
{
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	long rel;
	__u64 blk;
	struct buffer_head *bh;

	if (dir_disk->index_block)
		return 0;

	rel = sfs_alloc_data_block(config);
	if (rel < 0)
		return -ENOSPC;
	blk = data_start + rel;
	sfs_sync_bitmap_bit(sb, config, rel, true);

	bh = sb_bread(sb, blk);
	if (!bh)
		return -EIO;
	memset(bh->b_data, 0, sb->s_blocksize);
	mark_buffer_dirty(bh);
	brelse(bh);

	dir_disk->index_block = cpu_to_le64(blk);
	return 0;
}

int sfs_dir_find(struct super_block *sb, struct sfs_mount_config *config,
                         struct sfs_inode *dir_disk, const char *name, size_t namelen,
                         unsigned int *out_ino)
{
	__u64 index_blk = le64_to_cpu(dir_disk->index_block);
	__u32 bucket;
	__u64 leaf;
	struct buffer_head *ibh;

	if (!index_blk)
		return -ENOENT;

	bucket = sfs_name_hash(name, namelen);
	ibh = sb_bread(sb, index_blk);
	if (!ibh)
		return -EIO;
	leaf = le64_to_cpu(((__le64 *)ibh->b_data)[bucket]);
	brelse(ibh);

	while (leaf) {
		struct buffer_head *lbh = sb_bread(sb, leaf);
		struct sfs_dirblock_header *hdr;
		struct sfs_dirent *ents;
		unsigned int s;
		__u64 next;

		if (!lbh)
			return -EIO;
		hdr = (struct sfs_dirblock_header *)lbh->b_data;
		ents = (struct sfs_dirent *)(lbh->b_data + sizeof(*hdr));

		for (s = 0; s < SFS_DIRENTS_PER_LEAF; s++) {
			size_t enl;
			if (le64_to_cpu(ents[s].inode) == 0)
				continue;
			enl = strnlen(ents[s].name, sizeof(ents[s].name));
			if (enl == namelen && memcmp(ents[s].name, name, namelen) == 0) {
				*out_ino = (unsigned int)le64_to_cpu(ents[s].inode);
				brelse(lbh);
				return 0;
			}
		}
		next = le64_to_cpu(hdr->next);
		brelse(lbh);
		leaf = next;
		cond_resched();
	}
	return -ENOENT;
}

int sfs_dir_add(struct super_block *sb, struct sfs_mount_config *config,
                        struct sfs_inode *dir_disk, const char *name, size_t namelen, unsigned int ino)
{
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	__u32 bucket;
	__u64 leaf, head;
	struct buffer_head *ibh;
	int ret;

	ret = sfs_dir_ensure_index(sb, config, dir_disk);
	if (ret)
		return ret;

	bucket = sfs_name_hash(name, namelen);
	ibh = sb_bread(sb, le64_to_cpu(dir_disk->index_block));
	if (!ibh)
		return -EIO;
	head = le64_to_cpu(((__le64 *)ibh->b_data)[bucket]);

	leaf = head;
	while (leaf) {
		struct buffer_head *lbh = sb_bread(sb, leaf);
		struct sfs_dirent *ents;
		unsigned int s;
		__u64 next;

		if (!lbh) {
			brelse(ibh);
			return -EIO;
		}
		ents = (struct sfs_dirent *)(lbh->b_data + sizeof(struct sfs_dirblock_header));
		for (s = 0; s < SFS_DIRENTS_PER_LEAF; s++) {
			if (le64_to_cpu(ents[s].inode) == 0) {
				memset(ents[s].name, 0, sizeof(ents[s].name));
				memcpy(ents[s].name, name, namelen);
				ents[s].inode = cpu_to_le64(ino);
				mark_buffer_dirty(lbh);
				brelse(lbh);
				brelse(ibh);
				return 0;
			}
		}
		next = le64_to_cpu(((struct sfs_dirblock_header *)lbh->b_data)->next);
		brelse(lbh);
		leaf = next;
		cond_resched();
	}

	/* no free slot anywhere in the chain - prepend a new leaf */
	{
		long rel = sfs_alloc_data_block_near(config, le64_to_cpu(dir_disk->index_block) - data_start);
		__u64 new_leaf;
		struct buffer_head *nbh;
		struct sfs_dirblock_header *hdr;
		struct sfs_dirent *ents;

		if (rel < 0) {
			brelse(ibh);
			return -ENOSPC;
		}
		new_leaf = data_start + rel;
		sfs_sync_bitmap_bit(sb, config, rel, true);

		nbh = sb_bread(sb, new_leaf);
		if (!nbh) {
			brelse(ibh);
			return -EIO;
		}
		memset(nbh->b_data, 0, sb->s_blocksize);
		hdr = (struct sfs_dirblock_header *)nbh->b_data;
		hdr->next = cpu_to_le64(head);
		ents = (struct sfs_dirent *)(nbh->b_data + sizeof(*hdr));
		memcpy(ents[0].name, name, namelen);
		ents[0].inode = cpu_to_le64(ino);
		mark_buffer_dirty(nbh);
		brelse(nbh);

		((__le64 *)ibh->b_data)[bucket] = cpu_to_le64(new_leaf);
		mark_buffer_dirty(ibh);
		brelse(ibh);
	}
	return 0;
}

int sfs_dir_remove(struct super_block *sb, struct sfs_mount_config *config,
                           struct sfs_inode *dir_disk, const char *name, size_t namelen)
{
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	__u32 bucket;
	__u64 leaf, prev = 0;
	struct buffer_head *ibh;

	if (!dir_disk->index_block)
		return -ENOENT;

	bucket = sfs_name_hash(name, namelen);
	ibh = sb_bread(sb, le64_to_cpu(dir_disk->index_block));
	if (!ibh)
		return -EIO;
	leaf = le64_to_cpu(((__le64 *)ibh->b_data)[bucket]);

	while (leaf) {
		struct buffer_head *lbh = sb_bread(sb, leaf);
		struct sfs_dirblock_header *hdr;
		struct sfs_dirent *ents;
		unsigned int s;
		bool matched = false, any_left = false;
		__u64 next;

		if (!lbh) {
			brelse(ibh);
			return -EIO;
		}
		hdr = (struct sfs_dirblock_header *)lbh->b_data;
		ents = (struct sfs_dirent *)(lbh->b_data + sizeof(*hdr));

		for (s = 0; s < SFS_DIRENTS_PER_LEAF; s++) {
			if (le64_to_cpu(ents[s].inode) == 0)
				continue;
			if (!matched) {
				size_t enl = strnlen(ents[s].name, sizeof(ents[s].name));
				if (enl == namelen && memcmp(ents[s].name, name, namelen) == 0) {
					memset(&ents[s], 0, sizeof(ents[s]));
					matched = true;
					continue;
				}
			}
			any_left = true;
		}

		if (!matched) {
			next = le64_to_cpu(hdr->next);
			brelse(lbh);
			prev = leaf;
			leaf = next;
			cond_resched();
			continue;
		}

		mark_buffer_dirty(lbh);
		next = le64_to_cpu(hdr->next);

		if (!any_left) {
			/* leaf is now fully empty - unlink and free it */
			brelse(lbh);
			if (prev == 0) {
				((__le64 *)ibh->b_data)[bucket] = cpu_to_le64(next);
				mark_buffer_dirty(ibh);
			} else {
				struct buffer_head *pbh = sb_bread(sb, prev);
				if (pbh) {
					((struct sfs_dirblock_header *)pbh->b_data)->next = cpu_to_le64(next);
					mark_buffer_dirty(pbh);
					brelse(pbh);
				}
			}
			sfs_free_data_block(config, leaf - data_start);
			sfs_sync_bitmap_bit(sb, config, leaf - data_start, false);
		} else {
			brelse(lbh);
		}
		brelse(ibh);
		return 0;
	}

	brelse(ibh);
	return -ENOENT;
}

bool sfs_dir_is_empty(struct super_block *sb, struct sfs_mount_config *config, struct sfs_inode *dir_disk)
{
	struct buffer_head *ibh;
	__le64 *buckets;
	unsigned int b;

	if (!dir_disk->index_block)
		return true;

	ibh = sb_bread(sb, le64_to_cpu(dir_disk->index_block));
	if (!ibh)
		return true; /* fail safe-ish; caller already handles -EIO elsewhere */
	buckets = (__le64 *)ibh->b_data;
	for (b = 0; b < SFS_DIR_INDEX_BUCKETS; b++) {
		if (le64_to_cpu(buckets[b]) != 0) {
			brelse(ibh);
			return false;
		}
	}
	brelse(ibh);
	return true;
}

void sfs_free_dir_index(struct super_block *sb, struct sfs_mount_config *config, __u64 index_block)
{
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	struct buffer_head *ibh;
	__le64 *buckets;
	unsigned int b;

	if (!index_block)
		return;

	ibh = sb_bread(sb, index_block);
	if (!ibh)
		return;
	buckets = (__le64 *)ibh->b_data;

	for (b = 0; b < SFS_DIR_INDEX_BUCKETS; b++) {
		__u64 leaf = le64_to_cpu(buckets[b]);
		while (leaf) {
			struct buffer_head *lbh = sb_bread(sb, leaf);
			__u64 next, this_block = leaf;
			if (!lbh)
				break;
			next = le64_to_cpu(((struct sfs_dirblock_header *)lbh->b_data)->next);
			brelse(lbh);
			sfs_free_data_block(config, this_block - data_start);
			sfs_sync_bitmap_bit(sb, config, this_block - data_start, false);
			leaf = next;
		}
		if ((b & 0x3f) == 0)
			cond_resched();
	}
	brelse(ibh);

	sfs_free_data_block(config, index_block - data_start);
	sfs_sync_bitmap_bit(sb, config, index_block - data_start, false);
}