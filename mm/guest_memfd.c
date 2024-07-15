// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/anon_inodes.h>
#include <linux/falloc.h>
#include <linux/guest_memfd.h>
#include <linux/pagemap.h>

struct folio *guest_memfd_grab_folio(struct inode *inode, pgoff_t index, u32 flags)
{
	struct guest_memfd *gmem = inode->i_private;
	struct folio *folio;

	/* TODO: Support huge pages. */
	folio = filemap_grab_folio(inode->i_mapping, index);
	if (IS_ERR(folio))
		return folio;

	/*
	 * Use the up-to-date flag to track whether or not the memory has been
	 * zeroed before being handed off to the guest.  There is no backing
	 * storage for the memory, so the folio will remain up-to-date until
	 * it's removed.
	 */
	if ((flags & GUEST_MEMFD_GRAB_UPTODATE) && !folio_test_uptodate(folio)) {
		unsigned long nr_pages = folio_nr_pages(folio);
		unsigned long i;

		for (i = 0; i < nr_pages; i++)
			clear_highpage(folio_page(folio, i));

		folio_mark_uptodate(folio);
	}

	if (flags & GUEST_MEMFD_PREPARE && gmem->ops->prepare) {
		int r = gmem->ops->prepare(inode, index, folio);
		if (r < 0) {
			folio_unlock(folio);
			folio_put(folio);
			return ERR_PTR(r);
		}
	}

	/*
	 * Ignore accessed, referenced, and dirty flags.  The memory is
	 * unevictable and there is no storage to write back to.
	 */
	return folio;
}
EXPORT_SYMBOL_GPL(guest_memfd_grab_folio);

static long gmem_punch_hole(struct inode *inode, loff_t offset, loff_t len)
{
	struct guest_memfd *gmem = inode->i_private;
	pgoff_t start = offset >> PAGE_SHIFT;
	unsigned long nr = len >> PAGE_SHIFT;
	long ret;

	/*
	 * Bindings must be stable across invalidation to ensure the start+end
	 * are balanced.
	 */
	filemap_invalidate_lock(inode->i_mapping);

	ret = gmem->ops->invalidate_begin(inode, start, nr);
	if (ret)
		goto out;

	truncate_inode_pages_range(inode->i_mapping, offset, offset + len - 1);

	if (gmem->ops->invalidate_end)
		gmem->ops->invalidate_end(inode, start, nr);

out:
	filemap_invalidate_unlock(inode->i_mapping);

	return 0;
}

static long gmem_allocate(struct inode *inode, loff_t offset, loff_t len)
{
	struct address_space *mapping = inode->i_mapping;
	pgoff_t start, index, end;
	int r;

	/* Dedicated guest is immutable by default. */
	if (offset + len > i_size_read(inode))
		return -EINVAL;

	filemap_invalidate_lock_shared(mapping);

	start = offset >> PAGE_SHIFT;
	end = (offset + len) >> PAGE_SHIFT;

	r = 0;
	for (index = start; index < end; ) {
		struct folio *folio;

		if (signal_pending(current)) {
			r = -EINTR;
			break;
		}

		folio = guest_memfd_grab_folio(inode, index,
					       GUEST_MEMFD_GRAB_UPTODATE | GUEST_MEMFD_PREPARE);
		if (!folio) {
			r = -ENOMEM;
			break;
		}

		index = folio_next_index(folio);

		folio_unlock(folio);
		folio_put(folio);

		/* 64-bit only, wrapping the index should be impossible. */
		if (WARN_ON_ONCE(!index))
			break;

		cond_resched();
	}

	filemap_invalidate_unlock_shared(mapping);

	return r;
}

static long gmem_fallocate(struct file *file, int mode, loff_t offset,
			       loff_t len)
{
	int ret;

	if (!(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	if (!PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
		return -EINVAL;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		ret = gmem_punch_hole(file_inode(file), offset, len);
	else
		ret = gmem_allocate(file_inode(file), offset, len);

	if (!ret)
		file_modified(file);
	return ret;
}

static int gmem_release(struct inode *inode, struct file *file)
{
	struct guest_memfd *gmem = file->private_data;

	return gmem->ops->release(inode);
}

static struct file_operations gmem_fops = {
	.open		= generic_file_open,
	.release	= gmem_release,
	.fallocate	= gmem_fallocate,
	.owner = THIS_MODULE,
};

static int gmem_migrate_folio(struct address_space *mapping,
				  struct folio *dst, struct folio *src,
				  enum migrate_mode mode)
{
	WARN_ON_ONCE(1);
	return -EINVAL;
}

static int gmem_error_folio(struct address_space *mapping, struct folio *folio)
{
	struct inode *inode = mapping->host;
	struct guest_memfd *gmem = inode->i_private;
	off_t offset = folio->index;
	size_t size = folio_nr_pages(folio);
	int ret;

	filemap_invalidate_lock_shared(mapping);

	ret = gmem->ops->invalidate_begin(inode, offset, size);
	if (!ret && gmem->ops->invalidate_end)
		gmem->ops->invalidate_end(inode, offset, size);

	filemap_invalidate_unlock_shared(mapping);

	return ret;
}

static void gmem_free_folio(struct folio *folio)
{
	/* TODO: this feels race-prone + wrong. Need to double check. */
	struct inode *inode = folio_inode(folio);
	struct guest_memfd *gmem = inode->i_private;

	if (gmem->ops->free_folio)
		gmem->ops->free_folio(inode, folio);
}

static const struct address_space_operations gmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.migrate_folio	= gmem_migrate_folio,
	.error_remove_folio = gmem_error_folio,
	.free_folio = gmem_free_folio,
};

static int gmem_getattr(struct mnt_idmap *idmap, const struct path *path,
			    struct kstat *stat, u32 request_mask,
			    unsigned int query_flags)
{
	struct inode *inode = path->dentry->d_inode;

	/* TODO */
	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

static int gmem_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			    struct iattr *attr)
{
	/* TODO */
	return -EINVAL;
}
static const struct inode_operations gmem_iops = {
	.getattr	= gmem_getattr,
	.setattr	= gmem_setattr,
};

static inline bool guest_memfd_check_ops(const struct guest_memfd_operations *ops)
{
	return ops->invalidate_begin && ops->release;
}

struct file *guest_memfd_alloc(const char *name, struct guest_memfd *gmem, loff_t size)
{
	struct inode *inode;
	struct file *file;

	if (size <= 0 || !PAGE_ALIGNED(size))
		return ERR_PTR(-EINVAL);

	if (!guest_memfd_check_ops(gmem->ops))
		return ERR_PTR(-EINVAL);

	/*
	 * Use the so called "secure" variant, which creates a unique inode
	 * instead of reusing a single inode.  Each guest_memfd instance needs
	 * its own inode to track the size, flags, etc.
	 */
	file = anon_inode_create_getfile(name, &gmem_fops, gmem, O_RDWR, NULL);
	if (IS_ERR(file))
		return file;

	file->f_flags |= O_LARGEFILE;

	inode = file_inode(file);
	WARN_ON(file->f_mapping != inode->i_mapping);

	inode->i_private = gmem;
	inode->i_op = &gmem_iops;
	inode->i_mapping->a_ops = &gmem_aops;
	inode->i_mode |= S_IFREG;
	inode->i_size = size;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_inaccessible(inode->i_mapping);
	/* Unmovable mappings are supposed to be marked unevictable as well. */
	WARN_ON_ONCE(!mapping_unevictable(inode->i_mapping));

	return file;
}
EXPORT_SYMBOL_GPL(guest_memfd_alloc);

bool is_guest_memfd(struct file *file)
{
	return file->f_op == &gmem_fops;
}
EXPORT_SYMBOL_GPL(is_guest_memfd);
