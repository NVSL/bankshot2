/*
 * Copied from pmfs/xip.c
 */

#include "bankshot2.h"

static int bankshot2_find_and_alloc_blocks(struct bankshot2_device *bs2_dev,
		struct bankshot2_inode *pi, sector_t iblock,
		sector_t *data_block, int create)
{
	int err = -EIO;
	u64 block;
//	bankshot2_transaction_t *trans;

	block = bankshot2_find_data_block(bs2_dev, pi, iblock);

	if (!block) {
		if (!create) {
			err = -ENODATA;
			goto err;
		}

		err = bankshot2_alloc_blocks(NULL, bs2_dev, pi, iblock,
						1, true);
		if (err) {
			bs2_dbg("[%s:%d] Alloc failed!\n", __func__, __LINE__);
			goto err;
		}
# if 0
		trans = bankshot2_current_transaction();
		if (trans) {
			err = bankshot2_alloc_blocks(trans, inode, iblock, 1, true);
			if (err) {
				bankshot2_dbg_verbose("[%s:%d] Alloc failed!\n",
					__func__, __LINE__);
				goto err;
			}
		} else {
			/* 1 lentry for inode, 1 lentry for inode's b-tree */
			trans = bankshot2_new_transaction(sb, MAX_INODE_LENTRIES);
			if (IS_ERR(trans)) {
				err = PTR_ERR(trans);
				goto err;
			}

			rcu_read_unlock();
			mutex_lock(&inode->i_mutex);

			bankshot2_add_logentry(sb, trans, pi, MAX_DATA_PER_LENTRY,
				LE_DATA);
			err = bankshot2_alloc_blocks(trans, inode, iblock, 1, true);

			bankshot2_commit_transaction(sb, trans);

			mutex_unlock(&inode->i_mutex);
			rcu_read_lock();
			if (err) {
				bankshot2_dbg_verbose("[%s:%d] Alloc failed!\n",
					__func__, __LINE__);
				goto err;
			}
		}
#endif
		block = bankshot2_find_data_block(bs2_dev, pi, iblock);
		if (!block) {
			bs2_dbg("[%s:%d] But alloc didn't fail!\n",
				  __func__, __LINE__);
			err = -ENODATA;
			goto err;
		}
	}
	bs2_dbg("iblock 0x%lx allocated_block 0x%llx\n", iblock, block);

	*data_block = block;
	err = 0;

err:
	return err;
}


static inline int __bankshot2_get_block(struct bankshot2_device *bs2_dev,
			struct bankshot2_inode *pi, pgoff_t pgoff, int create,
			sector_t *block)
{
	int ret = 0;

	ret = bankshot2_find_and_alloc_blocks(bs2_dev, pi, (sector_t)pgoff,
						block, create);

	return ret;
}

int bankshot2_get_xip_mem(struct bankshot2_device *bs2_dev,
			struct bankshot2_inode *pi, pgoff_t pgoff, int create,
			void **kmem, unsigned long *pfn)
{
	int ret;
	sector_t block = 0;

	ret = __bankshot2_get_block(bs2_dev, pi, pgoff, create, &block);
	if (ret)
		return ret;

	*kmem = bankshot2_get_block(bs2_dev, block);
	*pfn = bankshot2_get_pfn(bs2_dev, block);

	return 0;
}

static int bankshot2_xip_file_fault(struct vm_area_struct *vma,
					struct vm_fault *vmf)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	struct bankshot2_inode *pi;
	pgoff_t size;
	void *xip_mem;
	unsigned long xip_pfn;
	int ret = 0;

//	pi = bankshot2_get_inode(bs2_dev, inode->i_ino);
	pi = bankshot2_get_inode(bs2_dev, 1);

	rcu_read_lock();
	size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (vmf->pgoff >= size) {
		bs2_info("pgoff >= size(SIGBUS).\n");
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	ret = bankshot2_get_xip_mem(bs2_dev, pi, vmf->pgoff, 1,
				&xip_mem, &xip_pfn);
	if (unlikely(ret)) {
		bs2_info("get_xip_mem failed\n");
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	ret = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address,
				xip_pfn);
	if (ret == -ENOMEM) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	ret = VM_FAULT_NOPAGE;
out:
	rcu_read_unlock();
	return ret;
}

static const struct vm_operations_struct bankshot2_xip_vm_ops = {
	.fault	= bankshot2_xip_file_fault,
};

int bankshot2_xip_file_mmap(struct file *file, struct vm_area_struct *vma)
{
//	unsigned long block_sz;
	file_accessed(file);

	vma->vm_flags |= VM_MIXEDMAP;
	//FIXME: HUGE MMAP does not support yet
	vma->vm_ops = &bankshot2_xip_vm_ops;
	return 0;
}

void bankshot2_init_mmap(struct bankshot2_device *bs2_dev)
{
	bs2_dev->mmap = bankshot2_xip_file_mmap;
}
