#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/bitmap.h>
#include <linux/sched.h>
#include <linux/errno.h>

#include "sfs_extent.h"


int sfs_resolve_extent_block(struct super_block *sb, struct sfs_mount_config *config,
                                     struct sfs_inode *disk_ino, __u64 logical_block, bool create,
                                     __u64 *out_phys, bool *out_dirty)
{
	__u64 data_start = le64_to_cpu(config->super.data_start_block);
	bool have_grow = false, any_extent = false;
	int grow_is_inline = 0, grow_inline_idx = -1;
	__u64 grow_ovf_block = 0;
	unsigned int grow_ovf_slot = 0;
	__u64 hint = 0;
	__u64 cur_ovf;
	int i;

	*out_phys = 0;
	*out_dirty = false;

	/* 1) inline extents */
	for (i = 0; i < SFS_INLINE_EXTENTS; i++) {
		struct sfs_extent *e = &disk_ino->extents[i];
		__u64 len = le32_to_cpu(e->length);
		__u64 fb = le32_to_cpu(e->file_block);

		if (len == 0)
			continue;
		any_extent = true;

		if (logical_block >= fb && logical_block < fb + len) {
			*out_phys = le64_to_cpu(e->start_block) + (logical_block - fb);
			return 0;
		}
		if (create && logical_block == fb + len) {
			have_grow = true;
			grow_is_inline = 1;
			grow_inline_idx = i;
			hint = le64_to_cpu(e->start_block) + len;
		}
	}

	/* 2) overflow chain - must scan fully even after finding a grow
	 * candidate, since a covering match anywhere takes priority */
	cur_ovf = le64_to_cpu(disk_ino->extent_overflow);
	while (cur_ovf) {
		struct buffer_head *bh = sb_bread(sb, cur_ovf);
		struct sfs_extent *ents;
		unsigned int s;
		__u64 next;

		if (!bh)
			return -EIO;
		ents = (struct sfs_extent *)bh->b_data;

		for (s = 0; s < SFS_EXTENTS_PER_OVERFLOW_BLOCK - 1; s++) {
			__u64 len = le32_to_cpu(ents[s].length);
			__u64 fb = le32_to_cpu(ents[s].file_block);

			if (len == 0)
				continue;
			any_extent = true;

			if (logical_block >= fb && logical_block < fb + len) {
				*out_phys = le64_to_cpu(ents[s].start_block) + (logical_block - fb);
				brelse(bh);
				return 0;
			}
			if (create && logical_block == fb + len) {
				have_grow = true;
				grow_is_inline = 0;
				grow_ovf_block = cur_ovf;
				grow_ovf_slot = s;
				hint = le64_to_cpu(ents[s].start_block) + len;
			}
		}
		next = le64_to_cpu(ents[SFS_EXTENTS_PER_OVERFLOW_BLOCK - 1].start_block);
		brelse(bh);
		cur_ovf = next;
		cond_resched();
	}

	if (!create)
		return 0; /* hole */

	/* Grow an existing extent by one block if the immediately following
	 * physical block is actually free - keeps sequential writes as one
	 * contiguous extent instead of fragmenting into many tiny ones. */
	if (have_grow && sfs_block_free(config, hint - data_start)) {
		__u64 rel = hint - data_start;

		set_bit(rel, config->data_bitmap);
		sfs_sync_bitmap_bit(sb, config, rel, true);

		if (grow_is_inline) {
			struct sfs_extent *e = &disk_ino->extents[grow_inline_idx];
			e->length = cpu_to_le32(le32_to_cpu(e->length) + 1);
			*out_dirty = true;
		} else {
			struct buffer_head *bh = sb_bread(sb, grow_ovf_block);
			struct sfs_extent *ents;
			if (!bh)
				return -EIO;
			ents = (struct sfs_extent *)bh->b_data;
			ents[grow_ovf_slot].length = cpu_to_le32(le32_to_cpu(ents[grow_ovf_slot].length) + 1);
			mark_buffer_dirty(bh);
			brelse(bh);
		}
		*out_phys = hint;
		return 0;
	}

	/* Allocate a fresh block for a brand-new 1-block extent. Bias the
	 * search: near the growable candidate's tail if there was one
	 * (keeps new data close even when not literally contiguous);
	 * otherwise, if this file has no extents at all yet, near its
	 * creation-time locality hint (clusters with siblings in the same
	 * directory); otherwise unbiased. */
	{
		__u64 hint_rel;
		long rel;
		__u64 new_block;

		if (have_grow)
			hint_rel = hint - data_start;
		else if (!any_extent && le64_to_cpu(disk_ino->extent_overflow) == 0)
			hint_rel = le32_to_cpu(disk_ino->alloc_hint);
		else
			hint_rel = 0;

		rel = sfs_alloc_data_block_near(config, hint_rel);
		if (rel < 0)
			return -ENOSPC;
		new_block = data_start + rel;
		sfs_sync_bitmap_bit(sb, config, rel, true);

		for (i = 0; i < SFS_INLINE_EXTENTS; i++) {
			if (le32_to_cpu(disk_ino->extents[i].length) == 0) {
				disk_ino->extents[i].start_block = cpu_to_le64(new_block);
				disk_ino->extents[i].length = cpu_to_le32(1);
				disk_ino->extents[i].file_block = cpu_to_le32(logical_block);
				*out_dirty = true;
				*out_phys = new_block;
				return 0;
			}
		}

		/* no inline slot free - find room in the overflow chain */
		cur_ovf = le64_to_cpu(disk_ino->extent_overflow);
		while (cur_ovf) {
			struct buffer_head *bh = sb_bread(sb, cur_ovf);
			struct sfs_extent *ents;
			unsigned int s;
			__u64 next;

			if (!bh) {
				sfs_free_data_block(config, rel);
				sfs_sync_bitmap_bit(sb, config, rel, false);
				return -EIO;
			}
			ents = (struct sfs_extent *)bh->b_data;
			for (s = 0; s < SFS_EXTENTS_PER_OVERFLOW_BLOCK - 1; s++) {
				if (le32_to_cpu(ents[s].length) == 0) {
					ents[s].start_block = cpu_to_le64(new_block);
					ents[s].length = cpu_to_le32(1);
					ents[s].file_block = cpu_to_le32(logical_block);
					mark_buffer_dirty(bh);
					brelse(bh);
					*out_phys = new_block;
					return 0;
				}
			}
			next = le64_to_cpu(ents[SFS_EXTENTS_PER_OVERFLOW_BLOCK - 1].start_block);
			brelse(bh);
			if (next == 0)
				break; /* tail block, and it's full */
			cur_ovf = next;
		}

		/* every existing overflow block (if any) is full - allocate a
		 * new one and link it at the tail (or as the first) */
		{
			long ovf_rel = sfs_alloc_data_block(config);
			__u64 ovf_block;
			struct buffer_head *nbh;
			struct sfs_extent *nents;

			if (ovf_rel < 0) {
				sfs_free_data_block(config, rel);
				sfs_sync_bitmap_bit(sb, config, rel, false);
				return -ENOSPC;
			}
			ovf_block = data_start + ovf_rel;
			sfs_sync_bitmap_bit(sb, config, ovf_rel, true);

			nbh = sb_bread(sb, ovf_block);
			if (!nbh) {
				sfs_free_data_block(config, rel);
				sfs_sync_bitmap_bit(sb, config, rel, false);
				sfs_free_data_block(config, ovf_rel);
				sfs_sync_bitmap_bit(sb, config, ovf_rel, false);
				return -EIO;
			}
			memset(nbh->b_data, 0, sb->s_blocksize);
			nents = (struct sfs_extent *)nbh->b_data;
			nents[0].start_block = cpu_to_le64(new_block);
			nents[0].length = cpu_to_le32(1);
			nents[0].file_block = cpu_to_le32(logical_block);
			mark_buffer_dirty(nbh);
			brelse(nbh);

			if (disk_ino->extent_overflow == 0) {
				disk_ino->extent_overflow = cpu_to_le64(ovf_block);
				*out_dirty = true;
			} else {
				struct buffer_head *tbh = sb_bread(sb, cur_ovf);
				struct sfs_extent *tents;
				if (!tbh)
					return -EIO;
				tents = (struct sfs_extent *)tbh->b_data;
				tents[SFS_EXTENTS_PER_OVERFLOW_BLOCK - 1].start_block = cpu_to_le64(ovf_block);
				mark_buffer_dirty(tbh);
				brelse(tbh);
			}

			*out_phys = new_block;
			return 0;
		}
	}
}