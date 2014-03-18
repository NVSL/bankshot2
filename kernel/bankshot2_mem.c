/*
 * Handle memory allocation.
 * Copied from PMFS balloc.c.
 */

#include "bankshot2.h"

static struct bankshot2_blocknode *
bankshot2_alloc_blocknode(struct bankshot2_device *bs2_dev)
{
	struct bankshot2_blocknode *p;

	p = (struct bankshot2_blocknode *)
		kmem_cache_alloc(bs2_dev->bs2_blocknode_cachep, GFP_NOFS);
	if (p) {
		bs2_dev->num_blocknode_allocated++;
	}
	return p;
}

static void __bankshot2_free_blocknode(struct bankshot2_device *bs2_dev,
					struct bankshot2_blocknode *bnode)
{
	kmem_cache_free(bs2_dev->bs2_blocknode_cachep, bnode);
}

int bankshot2_new_block(struct bankshot2_device *bs2_dev,
		unsigned long *blocknr, unsigned short btype, int zero)
{
	struct list_head *head = &(bs2_dev->block_inuse_head);
	struct bankshot2_blocknode *i, *next_i;
	struct bankshot2_blocknode *free_blocknode = NULL;
	void *bp;
	unsigned long num_blocks = 0;
	struct bankshot2_blocknode *curr_node;
	int errval = 0;
	bool found = 0;
	unsigned long next_block_low;
	unsigned long new_block_low;
	unsigned long new_block_high;

	num_blocks = bankshot2_get_numblocks(btype);

	mutex_lock(&bs2_dev->s_lock);

	list_for_each_entry(i, head, link) {
		if (i->link.next == head) {
			next_i = NULL;
			next_block_low = bs2_dev->block_end;
		} else {
			next_i = list_entry(i->link.next, typeof(*i), link);
			next_block_low = next_i->block_low;
		}

		new_block_low = (i->block_high + num_blocks) & ~(num_blocks - 1);
		new_block_high = new_block_low + num_blocks - 1;

		if (new_block_high >= next_block_low) {
			/* Does not fit - skip to next blocknode */
			continue;
		}

		if ((new_block_low == (i->block_high + 1)) &&
			(new_block_high == (next_block_low - 1)))
		{
			/* Fill the gap completely */
			if (next_i) {
				i->block_high = next_i->block_high;
				list_del(&next_i->link);
				free_blocknode = next_i;
				bs2_dev->num_blocknode_allocated--;
			} else {
				i->block_high = new_block_high;
			}
			found = 1;
			break;
		}

		if ((new_block_low == (i->block_high + 1)) &&
			(new_block_high < (next_block_low - 1))) {
			/* Aligns to left */
			i->block_high = new_block_high;
			found = 1;
			break;
		}

		if ((new_block_low > (i->block_high + 1)) &&
			(new_block_high == (next_block_low - 1))) {
			/* Aligns to right */
			if (next_i) {
				/* right node exist */
				next_i->block_low = new_block_low;
			} else {
				/* right node does NOT exist */
				curr_node = bankshot2_alloc_blocknode(bs2_dev);
				BUG_ON(!curr_node);
				if (curr_node == NULL) {
					errval = -ENOSPC;
					break;
				}
				curr_node->block_low = new_block_low;
				curr_node->block_high = new_block_high;
				list_add(&curr_node->link, &i->link);
			}
			found = 1;
			break;
		}

		if ((new_block_low > (i->block_high + 1)) &&
			(new_block_high < (next_block_low - 1))) {
			/* Aligns somewhere in the middle */
			curr_node = bankshot2_alloc_blocknode(bs2_dev);
			BUG_ON(!curr_node);
			if (curr_node == NULL) {
				errval = -ENOSPC;
				break;
			}
			curr_node->block_low = new_block_low;
			curr_node->block_high = new_block_high;
			list_add(&curr_node->link, &i->link);
			found = 1;
			break;
		}
	}
	
	if (found == 1) {
		bs2_dev->num_free_blocks -= num_blocks;
	}	

	mutex_unlock(&bs2_dev->s_lock);

	if (free_blocknode)
		__bankshot2_free_blocknode(bs2_dev, free_blocknode);

	if (found == 0) {
		return -ENOSPC;
	}

	if (zero) {
		size_t size;
		bp = bankshot2_get_block(bs2_dev,
			bankshot2_get_block_off(bs2_dev, new_block_low, btype));
//		bankshot2_memunlock_block(sb, bp); //TBDTBD: Need to fix this
		if (btype == BANKSHOT2_BLOCK_TYPE_4K)
			size = 0x1 << 12;
		else if (btype == BANKSHOT2_BLOCK_TYPE_2M)
			size = 0x1 << 21;
		else
			size = 0x1 << 30;
		memset_nt(bp, 0, size);
//		bankshot2_memlock_block(sb, bp);
	}
	*blocknr = new_block_low;

	return errval;
}


/*
 * allocate a data block for inode and return it's absolute blocknr.
 * Zeroes out the block if zero set. Increments inode->i_blocks.
 */
static int bankshot2_new_data_block(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, unsigned long *blocknr, int zero)
{
	unsigned int data_bits = PAGE_SHIFT;

	int errval = bankshot2_new_block(bs2_dev, blocknr, pi->i_blk_type, zero);

	if (!errval) {
//		bankshot2_memunlock_inode(sb, pi);
		le64_add_cpu(&pi->i_blocks,
			(1 << (data_bits - bs2_dev->s_blocksize_bits)));
//		bankshot2_memlock_inode(sb, pi);
	}

	return errval;
}

int __bankshot2_alloc_blocks(bankshot2_transaction_t *trans,
	struct bankshot2_device *bs2_dev,
	struct bankshot2_inode *pi, unsigned long file_blocknr, unsigned int num,
	bool zero)
{
	int errval;
	unsigned long max_blocks;
	unsigned int height;
	unsigned int data_bits = PAGE_SHIFT;
	unsigned int blk_shift, meta_bits = META_BLK_SHIFT;
	unsigned long blocknr, first_blocknr, last_blocknr, total_blocks;
	/* convert the 4K blocks into the actual blocks the inode is using */
	blk_shift = data_bits - bs2_dev->s_blocksize_bits;

	first_blocknr = file_blocknr >> blk_shift;
	last_blocknr = (file_blocknr + num - 1) >> blk_shift;

	bs2_dbg("alloc_blocks height %d file_blocknr %lx num %x, "
		   "first blocknr 0x%lx, last_blocknr 0x%lx\n",
		   pi->height, file_blocknr, num, first_blocknr, last_blocknr);

	height = pi->height;

	blk_shift = height * meta_bits;

	max_blocks = 0x1UL << blk_shift;

	if (last_blocknr > max_blocks - 1) {
		/* B-tree height increases as a result of this allocation */
		total_blocks = last_blocknr >> blk_shift;
		while (total_blocks > 0) {
			total_blocks = total_blocks >> meta_bits;
			height++;
		}
		if (height > 3) {
			bs2_dbg("[%s:%d] Max file size. Cant grow the file\n",
				__func__, __LINE__);
			errval = -ENOSPC;
			goto fail;
		}
	}

	if (!pi->root) {
		if (height == 0) {
			__le64 root;
			errval = bankshot2_new_data_block(bs2_dev, pi, &blocknr, zero);
			if (errval) {
				bs2_dbg("[%s:%d] failed: alloc data"
					" block\n", __func__, __LINE__);
				goto fail;
			}
			root = cpu_to_le64(bankshot2_get_block_off(bs2_dev, blocknr,
					   pi->i_blk_type));
//			bankshot2_memunlock_inode(sb, pi);
			pi->root = root;
			pi->height = height;
//			bankshot2_memlock_inode(sb, pi);
		} else {
			errval = bankshot2_increase_btree_height(bs2_dev, pi, height);
			if (errval) {
				bs2_dbg("[%s:%d] failed: inc btree"
					" height\n", __func__, __LINE__);
				goto fail;
			}
			errval = recursive_alloc_blocks(trans, bs2_dev, pi, pi->root,
			pi->height, first_blocknr, last_blocknr, 1, zero);
			if (errval < 0)
				goto fail;
		}
	} else {
		/* Go forward only if the height of the tree is non-zero. */
		if (height == 0)
			return 0;

		if (height > pi->height) {
			errval = bankshot2_increase_btree_height(bs2_dev, pi, height);
			if (errval) {
				bs2_dbg("Err: inc height %x:%x tot %lx"
					"\n", pi->height, height, total_blocks);
				goto fail;
			}
		}
		errval = recursive_alloc_blocks(trans, bs2_dev, pi, pi->root, height,
				first_blocknr, last_blocknr, 0, zero);
		if (errval < 0)
			goto fail;
	}
	return 0;
fail:
	return errval;
}

/*
 * Allocate num data blocks for inode, starting at given file-relative
 * block number.
 */
/*
inline int bankshot2_alloc_blocks(bankshot2_transaction_t *trans, struct inode *inode,
		unsigned long file_blocknr, unsigned int num, bool zero)
{
	struct super_block *sb = inode->i_sb;
	struct bankshot2_inode *pi = bankshot2_get_inode(sb, inode->i_ino);
	int errval;

	errval = __bankshot2_alloc_blocks(trans, sb, pi, file_blocknr, num, zero);
	inode->i_blocks = le64_to_cpu(pi->i_blocks);

	return errval;
}
*/

void bankshot2_init_blockmap(struct bankshot2_device *bs2_dev,
				unsigned long init_used_size)
{
	unsigned long num_used_block;
	struct bankshot2_blocknode *blknode;

	num_used_block = (init_used_size + bs2_dev->blocksize - 1) >>
		bs2_dev->s_blocksize_bits;

	bs2_info("blockmap init: used %lu blocks\n", num_used_block);
	blknode = bankshot2_alloc_blocknode(bs2_dev);
	if (blknode == NULL)
		bs2_info("WARNING: blocknode allocation failed\n");

	blknode->block_low = bs2_dev->block_start;
	blknode->block_high = bs2_dev->block_start + num_used_block - 1;
	bs2_dev->num_free_blocks -= num_used_block;
	list_add(&blknode->link, &bs2_dev->block_inuse_head);
}

int bankshot2_init_kmem(struct bankshot2_device *bs2_dev)
{
	bs2_dev->bs2_blocknode_cachep = kmem_cache_create(
					"bankshot2_blocknode_cache",
					sizeof(struct bankshot2_blocknode),
					0, (SLAB_RECLAIM_ACCOUNT |
                                        SLAB_MEM_SPREAD), NULL);
	if (bs2_dev->bs2_blocknode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

void bankshot2_destroy_kmem(struct bankshot2_device *bs2_dev)
{
	kmem_cache_destroy(bs2_dev->bs2_blocknode_cachep);
}
