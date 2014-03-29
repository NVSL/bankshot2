/*
 * inode code.
 * Copied from pmfs inode code.
 */

#include "bankshot2.h"

unsigned int blk_type_to_shift[3] = {12, 21, 30};
uint32_t blk_type_to_size[3] = {0x1000, 0x200000, 0x40000000};

static inline unsigned int
bankshot2_inode_blk_shift (struct bankshot2_inode *pi)
{
	return blk_type_to_shift[pi->i_blk_type];
}

static inline uint32_t bankshot2_inode_blk_size (struct bankshot2_inode *pi)
{
	return blk_type_to_size[pi->i_blk_type];
}

static inline struct bankshot2_inode *
bankshot2_get_inode_table(struct bankshot2_device *bs2_dev)
{
	struct bankshot2_super_block *ps = bankshot2_get_super(bs2_dev);

	return (struct bankshot2_inode *)((char *)ps +
			le64_to_cpu(ps->s_inode_table_offset));
}

/*
 * find the offset to the block represented by the given inode's file
 * relative block number.
 */
u64 bankshot2_find_data_block(struct bankshot2_device *bs2_dev,
			struct bankshot2_inode *pi, unsigned long file_blocknr)
{
	u32 blk_shift;
	unsigned long blk_offset, blocknr = file_blocknr;
	unsigned int data_bits = blk_type_to_shift[pi->i_blk_type];
	unsigned int meta_bits = META_BLK_SHIFT;
	u64 bp;

	/* convert the 4K blocks into the actual blocks the inode is using */
	blk_shift = data_bits - bs2_dev->s_blocksize_bits;
	blk_offset = file_blocknr & ((1 << blk_shift) - 1);
	blocknr = file_blocknr >> blk_shift;

	if (blocknr >= (1UL << (pi->height * meta_bits)))
		return 0;

	bp = __bankshot2_find_data_block(bs2_dev, pi, blocknr);
	bs2_dbg("find_data_block %lx, %x %llx blk_p %p blk_shift %x"
		" blk_offset %lx\n", file_blocknr, pi->height, bp,
		bankshot2_get_block(bs2_dev, bp), blk_shift, blk_offset);

	if (bp == 0)
		return 0;
	return bp + (blk_offset << bs2_dev->s_blocksize_bits);
}

/* Initialize the inode table. The bankshot2_inode struct corresponding to the
 * inode table has already been zero'd out */
int bankshot2_init_inode_table(struct bankshot2_device *bs2_dev)
{
	struct bankshot2_inode *pi = bankshot2_get_inode_table(bs2_dev);
	unsigned long num_blocks = 0, init_inode_table_size;
	int errval;

	if (bs2_dev->num_inodes == 0) {
		/* initial inode table size was not specified. */
		init_inode_table_size = PAGE_SIZE;
	} else {
		init_inode_table_size =
			bs2_dev->num_inodes << BANKSHOT2_INODE_BITS;
	}

//	bankshot2_memunlock_inode(sb, pi);
	pi->i_mode = 0;
	pi->i_uid = 0;
	pi->i_gid = 0;
	pi->i_links_count = cpu_to_le16(1);
	pi->i_flags = 0;
	pi->height = 0;
	pi->i_dtime = 0;
	pi->i_blk_type = BANKSHOT2_BLOCK_TYPE_4K;

	// Allocate 1 block for now
	num_blocks = (init_inode_table_size + bankshot2_inode_blk_size(pi) - 1) 
			>> bankshot2_inode_blk_shift(pi);

	// PAGE_SIZE
	pi->i_size = cpu_to_le64(num_blocks << bankshot2_inode_blk_shift(pi));
	/* bankshot2_sync_inode(pi); */
//	bankshot2_memlock_inode(sb, pi);

	// 4096 / 128 = 32
	bs2_dev->s_inodes_count = num_blocks <<
			(bankshot2_inode_blk_shift(pi) - BANKSHOT2_INODE_BITS);
	/* calculate num_blocks in terms of 4k blocksize */
	num_blocks = num_blocks << (bankshot2_inode_blk_shift(pi) -
				bs2_dev->s_blocksize_bits);
	errval = __bankshot2_alloc_blocks(NULL, bs2_dev, pi, 0, num_blocks, true);

	if (errval != 0) {
		bs2_info("Err: initializing the Inode Table: %d\n", errval);
		return errval;
	}

	/* inode 0 is considered invalid and hence never used */
	bs2_dev->s_free_inodes_count =
		(bs2_dev->s_inodes_count - BANKSHOT2_FREE_INODE_HINT_START);
	bs2_dev->s_free_inode_hint = (BANKSHOT2_FREE_INODE_HINT_START);

	return 0;
}

static int bankshot2_increase_inode_table_size(struct bankshot2_device *bs2_dev)
{
	struct bankshot2_inode *pi = bankshot2_get_inode_table(bs2_dev);
	bankshot2_transaction_t *trans = NULL;
	int errval;

	/* 1 log entry for inode-table inode, 1 lentry for inode-table b-tree */
//	trans = bankshot2_new_transaction(sb, MAX_INODE_LENTRIES);
//	if (IS_ERR(trans))
//		return PTR_ERR(trans);

//	bankshot2_add_logentry(sb, trans, pi, MAX_DATA_PER_LENTRY, LE_DATA);

	errval = __bankshot2_alloc_blocks(trans, bs2_dev, pi,
			le64_to_cpup(&pi->i_size) >> bs2_dev->s_blocksize_bits,
			1, true);

	if (errval == 0) {
		u64 i_size = le64_to_cpu(pi->i_size);

		bs2_dev->s_free_inode_hint = i_size >> BANKSHOT2_INODE_BITS;
		i_size += bankshot2_inode_blk_size(pi);

//		bankshot2_memunlock_inode(sb, pi);
		pi->i_size = cpu_to_le64(i_size);
//		bankshot2_memlock_inode(sb, pi);

		bs2_dev->s_free_inodes_count +=
			INODES_PER_BLOCK(pi->i_blk_type);
		bs2_dev->s_inodes_count = i_size >> BANKSHOT2_INODE_BITS;
	} else
		bs2_dbg("no space left to inc inode table!\n");
	/* commit the transaction */
//	bankshot2_commit_transaction(sb, trans);
	return errval;
}

int bankshot2_new_inode(struct bankshot2_device *bs2_dev,
		bankshot2_transaction_t *trans,	umode_t mode, ino_t *new_ino)
{
	struct bankshot2_inode *pi = NULL, *inode_table;
	int i, errval;
	u32 num_inodes, inodes_per_block;
	ino_t ino = 0;

#if 0
	inode_init_owner(inode, dir, mode);
	inode->i_blocks = inode->i_size = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;

	inode->i_generation = atomic_add_return(1, &sbi->next_generation);
#endif

	inode_table = bankshot2_get_inode_table(bs2_dev);

	bs2_dbg("free_inodes %x total_inodes %x hint %x\n",
		bs2_dev->s_free_inodes_count, bs2_dev->s_inodes_count,
		bs2_dev->s_free_inode_hint);

	mutex_lock(&bs2_dev->inode_table_mutex);

	/* find the oldest unused bankshot2 inode */
	i = (bs2_dev->s_free_inode_hint);
	inodes_per_block = INODES_PER_BLOCK(inode_table->i_blk_type);
retry:
	num_inodes = (bs2_dev->s_inodes_count);
	while (i < num_inodes) {
		u32 end_ino;
		end_ino = i + (inodes_per_block - (i & (inodes_per_block - 1)));
//		ino = i << PMFS_INODE_BITS;
		pi = bankshot2_get_inode(bs2_dev, i);
		for (; i < end_ino; i++) {
			/* check if the inode is active. */
			if (le16_to_cpu(pi->i_links_count) == 0 &&
			(le16_to_cpu(pi->i_mode) == 0 ||
			 le32_to_cpu(pi->i_dtime)))
				/* this inode is free */
				break;
			pi = (struct bankshot2_inode *)((void *)pi +
							BANKSHOT2_INODE_SIZE);
		}
		/* found a free inode */
		if (i < end_ino)
			break;
	}
	if (unlikely(i >= num_inodes)) {
		errval = bankshot2_increase_inode_table_size(bs2_dev);
		if (errval == 0)
			goto retry;
		mutex_unlock(&bs2_dev->inode_table_mutex);
		bs2_dbg("Bankshot2: could not find a free inode\n");
		goto fail1;
	}

//	ino = i << PMFS_INODE_BITS;
	ino = i;
	bs2_dbg("allocating inode %lx\n", ino);

	/* chosen inode is in ino */
//	inode->i_ino = ino;
//	bankshot2_add_logentry(sb, trans, pi, sizeof(*pi), LE_DATA);

//	bankshot2_memunlock_inode(sb, pi);
	pi->i_blk_type = BANKSHOT2_DEFAULT_BLOCK_TYPE;
//	pi->i_flags = bankshot2_mask_flags(mode, diri->i_flags);
	pi->height = 0;
	pi->i_dtime = 0;
//	bankshot2_memlock_inode(sb, pi);

	bs2_dev->s_free_inodes_count -= 1;

	if (i < (bs2_dev->s_inodes_count) - 1)
		bs2_dev->s_free_inode_hint = (i + 1);
	else
		bs2_dev->s_free_inode_hint = (BANKSHOT2_FREE_INODE_HINT_START);

	mutex_unlock(&bs2_dev->inode_table_mutex);

//	bankshot2_update_inode(inode, pi);

//	bankshot2_set_inode_flags(inode, pi);

	*new_ino = ino;

	return 0;
fail1:
	return errval;
}

/* If this is part of a read-modify-write of the inode metadata,
 * bankshot2_memunlock_inode() before calling! */
struct bankshot2_inode *bankshot2_get_inode(struct bankshot2_device *bs2_dev,
						u64 ino)
{
	struct bankshot2_super_block *ps = bankshot2_get_super(bs2_dev);
	struct bankshot2_inode *inode_table = bankshot2_get_inode_table(bs2_dev);
	u64 bp, block, ino_offset;

	if (ino == 0)
		return NULL;

	block = ino >> (bankshot2_inode_blk_shift(inode_table)
			- BANKSHOT2_INODE_BITS);
	bp = __bankshot2_find_data_block(bs2_dev, inode_table, block);

	if (bp == 0)
		return NULL;
	ino_offset = ((ino << BANKSHOT2_INODE_BITS)
			& (bankshot2_inode_blk_size(inode_table) - 1));
	bs2_info("%s: internal block %llu, actual block %llu, ino_offset %llu\n",
			__func__, block, bp / PAGE_SIZE, ino_offset);
	return (struct bankshot2_inode *)((void *)ps + bp + ino_offset);
}
