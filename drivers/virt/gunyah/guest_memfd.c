// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_guest_mem: " fmt

#include <linux/file.h>
#include <linux/guest_memfd.h>

#include <uapi/linux/gunyah.h>

#include "vm_mgr.h"

/**
 * struct gunyah_gmem_binding - Represents a binding of guestmem to a Gunyah VM
 * @gfn: Guest address to place acquired folios
 * @ghvm: Pointer to Gunyah VM in this binding
 * @i_off: offset into the guestmem to grab folios from
 * @file: Pointer to guest_memfd
 * @i_entry: list entry for inode->i_private_list
 * @flags: Access flags for the binding
 * @nr: Number of pages covered by this binding
 */
struct gunyah_gmem_binding {
	u64 gfn;
	struct gunyah_vm *ghvm;

	pgoff_t i_off;
	struct file *file;
	struct list_head i_entry;

	u32 flags;
	unsigned long nr;
};

static inline u64 gunyah_off_to_gfn(struct gunyah_gmem_binding *b, pgoff_t off)
{
	return off - b->i_off + b->gfn;
}

static inline bool gunyah_guest_mem_is_lend(struct gunyah_vm *ghvm, u32 flags)
{
	u8 access = flags & GUNYAH_MEM_ACCESS_MASK;

	if (access == GUNYAH_MEM_FORCE_LEND)
		return true;
	else if (access == GUNYAH_MEM_FORCE_SHARE)
		return false;

	/* RM requires all VMs to be protected (isolated) */
	return true;
}

static int gunyah_gmem_invalidate_begin(struct inode *inode, pgoff_t offset, unsigned long nr)
{
	return 0;
}

static int gunyah_gmem_accessible(struct inode *inode, struct folio *folio, pgoff_t offset, unsigned long nr)
{
	return 0;
}

static int gunyah_gmem_release(struct inode *inode)
{
	/**
	 * each binding increments refcount on file, so we shouldn't be here
	 * if i_private_list not empty.
	 */
	BUG_ON(!list_empty(&inode->i_mapping->i_private_list));

	return 0;
}

static const struct guest_memfd_operations gunyah_gmem_ops = {
	.invalidate_begin = gunyah_gmem_invalidate_begin,
	.accessible = gunyah_gmem_accessible,
	.release = gunyah_gmem_release,
};

int gunyah_guest_mem_create(struct gunyah_create_mem_args *args)
{
	const char *anon_name = "[gh-gmem]";
	unsigned long fd_flags = 0;
	struct file *file;
	int fd, err;

	if (args->flags & ~GHMF_CLOEXEC)
		return -EINVAL;

	if (args->flags & GHMF_CLOEXEC)
		fd_flags |= O_CLOEXEC;

	fd = get_unused_fd_flags(fd_flags);
	if (fd < 0)
		return fd;

	file = guest_memfd_alloc(anon_name, &gunyah_gmem_ops, args->size,
				 GUEST_MEMFD_FLAG_NO_DIRECT_MAP);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_fd;
	}

	fd_install(fd, file);
	return fd;
err_fd:
	put_unused_fd(fd);
	return err;
}

void gunyah_gmem_remove_binding(struct gunyah_gmem_binding *b)
{
	WARN_ON(gunyah_vm_reclaim_range(b->ghvm, b->gfn, b->nr));
	mtree_erase(&b->ghvm->bindings, b->gfn);
	list_del(&b->i_entry);
	fput(b->file);
	kfree(b);
}

static int gunyah_gmem_init_binding(struct gunyah_vm *ghvm, struct file *file,
				    struct gunyah_map_mem_args *args,
				    struct gunyah_gmem_binding *binding)
{
	if (args->flags & ~(GUNYAH_MEM_ALLOW_RWX | GUNYAH_MEM_ACCESS_MASK))
		return -EINVAL;

	if (args->guest_addr & ~PAGE_MASK)
		return -EINVAL;

	if (args->offset & ~PAGE_MASK)
		return -EINVAL;

	if (args->size & ~PAGE_MASK)
		return -EINVAL;

	binding->gfn = gunyah_gpa_to_gfn(args->guest_addr);
	binding->ghvm = ghvm;
	binding->i_off = args->offset >> PAGE_SHIFT;
	binding->file = file;
	binding->flags = args->flags;
	binding->nr = args->size >> PAGE_SHIFT;

	return 0;
}

static int gunyah_gmem_trim_binding(struct gunyah_gmem_binding *b,
				    unsigned long start_delta,
				    unsigned long end_delta)
{
	struct gunyah_vm *ghvm = b->ghvm;
	int ret;

	down_write(&ghvm->bindings_lock);
	if (!start_delta && !end_delta) {
		ret = gunyah_vm_reclaim_range(ghvm, b->gfn, b->nr);
		if (ret)
			goto unlock;
		gunyah_gmem_remove_binding(b);
	} else if (start_delta && !end_delta) {
		/* keep the start */
		ret = gunyah_vm_reclaim_range(ghvm, b->gfn + start_delta,
					      b->gfn + b->nr);
		if (ret)
			goto unlock;
		mtree_erase(&ghvm->bindings, b->gfn);
		b->nr = start_delta;
		ret = mtree_insert_range(&ghvm->bindings, b->gfn,
					 b->gfn + b->nr - 1, b, GFP_KERNEL);
	} else if (!start_delta && end_delta) {
		/* keep the end */
		ret = gunyah_vm_reclaim_range(ghvm, b->gfn,
					      b->gfn + b->nr - end_delta);
		if (ret)
			goto unlock;
		mtree_erase(&ghvm->bindings, b->gfn);
		b->gfn += b->nr - end_delta;
		b->i_off += b->nr - end_delta;
		b->nr = end_delta;
		ret = mtree_insert_range(&ghvm->bindings, b->gfn,
					 b->gfn + b->nr - 1, b, GFP_KERNEL);
	} else {
		/* TODO: split the mapping into 2 */
		ret = -EINVAL;
	}

unlock:
	up_write(&ghvm->bindings_lock);
	return ret;
}

static int gunyah_gmem_remove_mapping(struct gunyah_vm *ghvm, struct file *file,
				      struct gunyah_map_mem_args *args)
{
	struct inode *inode = file_inode(file);
	struct gunyah_gmem_binding *b = NULL;
	unsigned long start_delta, end_delta;
	struct gunyah_gmem_binding remove;
	int ret;

	ret = gunyah_gmem_init_binding(ghvm, file, args, &remove);
	if (ret)
		return ret;

	ret = -ENOENT;
	filemap_invalidate_lock(inode->i_mapping);
	list_for_each_entry(b, &inode->i_mapping->i_private_list, i_entry) {
		if (b->ghvm != remove.ghvm || b->flags != remove.flags ||
		    WARN_ON(b->file != remove.file))
			continue;
		/**
		 * Test if the binding to remove is within this binding
		 *  [gfn       b          nr]
		 *   [gfn   remove   nr]
		 */
		if (b->gfn > remove.gfn)
			continue;
		if (b->gfn + b->nr < remove.gfn + remove.nr)
			continue;

		/**
		 * We found the binding!
		 * Compute the delta in gfn start and make sure the offset
		 * into guest memfd matches.
		 */
		start_delta = remove.gfn - b->gfn;
		if (remove.i_off - b->i_off != start_delta)
			break;
		end_delta = b->gfn + b->nr - remove.gfn - remove.nr;

		ret = gunyah_gmem_trim_binding(b, start_delta, end_delta);
		break;
	}

	filemap_invalidate_unlock(inode->i_mapping);
	return ret;
}

static bool gunyah_gmem_binding_allowed_overlap(struct gunyah_gmem_binding *a,
						struct gunyah_gmem_binding *b)
{
	/* assumes we are operating on the same file, check to be sure */
	BUG_ON(a->file != b->file);

	/**
	 * Gunyah only guarantees we can share a page with one VM and
	 * doesn't (currently) allow us to share same page with multiple VMs,
	 * regardless whether host can also access.
	 * Gunyah supports, but Linux hasn't implemented mapping same page
	 * into 2 separate addresses in guest's address space. This doesn't
	 * seem reasonable today, but we could do it later.
	 * All this to justify: check that the `a` region doesn't overlap with
	 * `b` region w.r.t. file offsets.
	 */
	if (a->i_off + a->nr <= b->i_off)
		return true;
	if (a->i_off >= b->i_off + b->nr)
		return true;

	return false;
}

static int gunyah_gmem_add_mapping(struct gunyah_vm *ghvm, struct file *file,
				   struct gunyah_map_mem_args *args)
{
	struct gunyah_gmem_binding *b, *tmp = NULL;
	struct inode *inode = file_inode(file);
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	ret = gunyah_gmem_init_binding(ghvm, file, args, b);
	if (ret)
		return ret;

	/**
	 * When lending memory, we need to unmap single page from kernel's
	 * logical map. To do that, we need can_set_direct_map().
	 * arm64 doesn't map at page granularity without rodata=full.
	 */
	if (gunyah_guest_mem_is_lend(ghvm, b->flags) && !can_set_direct_map()) {
		kfree(b);
		pr_warn_once("Cannot lend memory without rodata=full");
		return -EINVAL;
	}

	filemap_invalidate_lock(inode->i_mapping);
	list_for_each_entry(tmp, &inode->i_mapping->i_private_list, i_entry) {
		if (!gunyah_gmem_binding_allowed_overlap(b, tmp)) {
			ret = -EEXIST;
			goto unlock;
		}
	}

	ret = mtree_insert_range(&ghvm->bindings, b->gfn, b->gfn + b->nr - 1, b,
				 GFP_KERNEL);
	if (ret)
		goto unlock;

	list_add(&b->i_entry, &inode->i_mapping->i_private_list);

unlock:
	filemap_invalidate_unlock(inode->i_mapping);
	return ret;
}

int gunyah_gmem_modify_mapping(struct gunyah_vm *ghvm,
			       struct gunyah_map_mem_args *args)
{
	u8 access = args->flags & GUNYAH_MEM_ACCESS_MASK;
	struct file *file;
	int ret = -EINVAL;

	file = fget(args->guest_mem_fd);
	if (!file)
		return -EINVAL;

	if (!is_guest_memfd(file, &gunyah_gmem_ops))
		goto err_file;

	if (args->flags & ~(GUNYAH_MEM_ALLOW_RWX | GUNYAH_MEM_UNMAP | GUNYAH_MEM_ACCESS_MASK))
		goto err_file;

	/* VM needs to have some permissions to the memory */
	if (!(args->flags & GUNYAH_MEM_ALLOW_RWX))
		goto err_file;

	if (access != GUNYAH_MEM_DEFAULT_ACCESS &&
	    access != GUNYAH_MEM_FORCE_LEND && access != GUNYAH_MEM_FORCE_SHARE)
		goto err_file;

	if (!PAGE_ALIGNED(args->guest_addr) || !PAGE_ALIGNED(args->offset) ||
	    !PAGE_ALIGNED(args->size))
		goto err_file;

	if (args->flags & GUNYAH_MEM_UNMAP) {
		args->flags &= ~GUNYAH_MEM_UNMAP;
		ret = gunyah_gmem_remove_mapping(ghvm, file, args);
	} else {
		ret = gunyah_gmem_add_mapping(ghvm, file, args);
	}

err_file:
	if (ret)
		fput(file);
	return ret;
}
