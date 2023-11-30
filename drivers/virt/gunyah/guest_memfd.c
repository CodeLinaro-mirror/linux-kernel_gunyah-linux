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

static inline pgoff_t gunyah_gfn_to_off(struct gunyah_gmem_binding *b, u64 gfn)
{
	return gfn - b->gfn + b->i_off;
}

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
	struct gunyah_gmem_binding *b;
	u64 gfn, gnr;
	int r;

	list_for_each_entry(b, &inode->i_mapping->i_private_list, i_entry) {
		/* skip if no overlap */
		if (offset + nr < b->i_off)
			continue;
		if (offset > b->i_off + b->nr)
			continue;

		gfn = gunyah_off_to_gfn(b, offset);
		/* limit nr of pages to reclaim by end of binding */
		gnr = max(b->gfn + b->nr, gfn + nr) - gfn;
		r = gunyah_vm_reclaim_range(b->ghvm, gfn, gnr);
		if (r < 0)
			return r;
	}

	return 0;
}

static int gunyah_gmem_accessible(struct inode *inode, struct folio *folio, pgoff_t offset, unsigned long nr)
{
	struct address_space *const mapping = inode->i_mapping;
	struct gunyah_gmem_binding *b;
	int ret = 0;
	u64 gfn;

	/* TODO: Splitting large folios */
	if (offset || folio_nr_pages(folio) != nr)
		return -EPERM;

	list_for_each_entry(b, &mapping->i_private_list, i_entry) {
		if (!gunyah_guest_mem_is_lend(b->ghvm, b->flags))
			continue;

		/* if the binding doesn't cover the request range: skip*/
		if (offset + nr < b->i_off)
			continue;
		if (offset > b->i_off + b->nr)
			continue;

		gfn = gunyah_off_to_gfn(b, offset);
		ret = gunyah_vm_reclaim_folio(b->ghvm, gfn, folio);
		if (ret)
			break;
	}

	return ret;
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

int gunyah_gmem_share_parcel(struct gunyah_vm *ghvm, struct gunyah_rm_mem_parcel *parcel,
			     u64 *gfn, u64 *nr)
{
	struct folio *folio, *prev_folio;
	unsigned long nr_entries, i, j, start, end;
	struct gunyah_gmem_binding *b;
	bool lend;
	int ret;

	parcel->mem_handle = GUNYAH_MEM_HANDLE_INVAL;

	if (!*nr)
		return -EINVAL;

	down_read(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, *gfn);
	if (!b || *gfn > b->gfn + b->nr || *gfn < b->gfn) {
		ret = -ENOENT;
		goto unlock;
	}

	/**
	 * Generally, indices can be based on gfn, guest_memfd offset, or
	 * offset into binding. start and end are based on offset into binding.
	 */
	start = *gfn - b->gfn;

	if (start + *nr > b->nr) {
		ret = -ENOENT;
		goto unlock;
	}

	end = start + *nr;
	lend = parcel->n_acl_entries == 1 || gunyah_guest_mem_is_lend(ghvm, b->flags);

	/**
	 * First, calculate the number of physically discontiguous regions
	 * the parcel covers. Each memory entry corresponds to one folio.
	 * In future, each memory entry could correspond to contiguous
	 * folios that are also adjacent in guest_memfd, but parcels
	 * are only being used for small amounts of memory for now, so
	 * this optimization is premature.
	 */
	nr_entries = 0;
	prev_folio = NULL;
	for (i = start + b->i_off; i < end + b->i_off;) {
		folio = guest_memfd_grab_folio(b->file, i, GUEST_MEMFD_GRAB_UPTODATE); /* A */
		if (!folio) {
			ret = -ENOMEM;
			goto out;
		}

		if (lend) {
			/* don't lend a folio that is mapped by host */
			if (guest_memfd_make_inaccessible(file_inode(b->file), folio)) {
				folio_unlock(folio);
				folio_put(folio);
				ret = -EPERM;
				goto out;
			}
			folio_set_private(folio);
		}

		nr_entries++;
		i = folio_index(folio) + folio_nr_pages(folio);
	}
	end = i - b->i_off;

	parcel->mem_entries =
		kcalloc(nr_entries, sizeof(*parcel->mem_entries), GFP_KERNEL);
	if (!parcel->mem_entries) {
		ret = -ENOMEM;
		goto out;
	}

	/**
	 * Walk through all the folios again, now filling the mem_entries array.
	 */
	j = 0;
	prev_folio = NULL;
	for (i = start + b->i_off; i < end + b->i_off; j++) {
		folio = filemap_get_folio(file_inode(b->file)->i_mapping, i); /* B */
		if (WARN_ON(IS_ERR(folio))) {
			ret = PTR_ERR(folio);
			i = end + b->i_off;
			goto out;
		}

		parcel->mem_entries[j].size = cpu_to_le64(folio_size(folio));
		parcel->mem_entries[j].phys_addr = cpu_to_le64(PFN_PHYS(folio_pfn(folio)));
		i = folio_index(folio) + folio_nr_pages(folio);
		folio_put(folio); /* B */
	}
	BUG_ON(j != nr_entries);
	parcel->n_mem_entries = nr_entries;

	if (lend)
		parcel->n_acl_entries = 1;

	parcel->acl_entries = kcalloc(parcel->n_acl_entries,
				      sizeof(*parcel->acl_entries), GFP_KERNEL);
	if (!parcel->n_acl_entries) {
		ret = -ENOMEM;
		goto free_entries;
	}

	parcel->acl_entries[0].vmid = cpu_to_le16(ghvm->vmid);
	if (b->flags & GUNYAH_MEM_ALLOW_READ)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_R;
	if (b->flags & GUNYAH_MEM_ALLOW_WRITE)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_W;
	if (b->flags & GUNYAH_MEM_ALLOW_EXEC)
		parcel->acl_entries[0].perms |= GUNYAH_RM_ACL_X;

	if (!lend) {
		u16 host_vmid;

		ret = gunyah_rm_get_vmid(ghvm->rm, &host_vmid);
		if (ret)
			goto free_acl;

		parcel->acl_entries[1].vmid = cpu_to_le16(host_vmid);
		parcel->acl_entries[1].perms = GUNYAH_RM_ACL_R | GUNYAH_RM_ACL_W | GUNYAH_RM_ACL_X;
	}

	parcel->mem_handle = GUNYAH_MEM_HANDLE_INVAL;
	folio = filemap_get_folio(file_inode(b->file)->i_mapping, start); /* C */
	*gfn = folio_index(folio) - b->i_off + b->gfn;
	*nr = end - (folio_index(folio) - b->i_off);
	folio_put(folio); /* C */

	ret = gunyah_rm_mem_share(ghvm->rm, parcel);
	goto out;
free_acl:
	kfree(parcel->acl_entries);
	parcel->acl_entries = NULL;
free_entries:
	kfree(parcel->mem_entries);
	parcel->mem_entries = NULL;
	parcel->n_mem_entries = 0;
out:
	/* unlock the folios */
	for (j = start + b->i_off; j < i;) {
		folio = filemap_get_folio(file_inode(b->file)->i_mapping, j); /* D */
		if (WARN_ON(IS_ERR(folio)))
			continue;
		j = folio_index(folio) + folio_nr_pages(folio);
		folio_unlock(folio); /* A */
		if (ret) {
			folio_put(folio); /* A */
		}
		folio_put(folio); /* D */
		/* matching folio_put for A is done at
		 * (1) gunyah_gmem_reclaim_parcel or
		 * (2) after gunyah_gmem_parcel_to_paged, gunyah_vm_reclaim_folio
		 */
	}
unlock:
	up_read(&ghvm->bindings_lock);
	return ret;
}

int gunyah_gmem_reclaim_parcel(struct gunyah_vm *ghvm,
			       struct gunyah_rm_mem_parcel *parcel, u64 gfn,
			       u64 nr)
{
	struct gunyah_rm_mem_entry *entry;
	struct folio *folio;
	pgoff_t i;
	int ret;

	if (parcel->mem_handle != GUNYAH_MEM_HANDLE_INVAL) {
		ret = gunyah_rm_mem_reclaim(ghvm->rm, parcel);
		if (ret) {
			dev_err(ghvm->parent, "Failed to reclaim parcel: %d\n",
				ret);
			/* We can't reclaim the pages -- hold onto the pages
			 * forever because we don't know what state the memory
			 * is in
			 */
			return ret;
		}
		parcel->mem_handle = GUNYAH_MEM_HANDLE_INVAL;

		for (i = 0; i < parcel->n_mem_entries; i++) {
			entry = &parcel->mem_entries[i];
			folio = pfn_folio(PHYS_PFN(le64_to_cpu(entry->phys_addr)));
			folio_put(folio); /* A */
		}

		kfree(parcel->mem_entries);
		kfree(parcel->acl_entries);
	}

	return 0;
}

int gunyah_gmem_setup_demand_paging(struct gunyah_vm *ghvm)
{
	struct gunyah_rm_mem_entry *entries;
	struct gunyah_gmem_binding *b;
	unsigned long index = 0;
	u32 count = 0, i;
	int ret = 0;

	down_read(&ghvm->bindings_lock);
	mt_for_each(&ghvm->bindings, b, index, ULONG_MAX)
		if (gunyah_guest_mem_is_lend(ghvm, b->flags))
			count++;

	if (!count)
		goto out;

	entries = kcalloc(count, sizeof(*entries), GFP_KERNEL);
	if (!entries) {
		ret = -ENOMEM;
		goto out;
	}

	index = i = 0;
	mt_for_each(&ghvm->bindings, b, index, ULONG_MAX) {
		if (!gunyah_guest_mem_is_lend(ghvm, b->flags))
			continue;
		entries[i].phys_addr = cpu_to_le64(gunyah_gfn_to_gpa(b->gfn));
		entries[i].size = cpu_to_le64(b->nr << PAGE_SHIFT);
		if (++i == count)
			break;
	}

	ret = gunyah_rm_vm_set_demand_paging(ghvm->rm, ghvm->vmid, i, entries);
	kfree(entries);
out:
	up_read(&ghvm->bindings_lock);
	return ret;
}

int gunyah_gmem_demand_page(struct gunyah_vm *ghvm, u64 gpa, bool write)
{
	unsigned long gfn = gunyah_gpa_to_gfn(gpa);
	struct gunyah_gmem_binding *b;
	struct folio *folio;
	int ret;

	down_read(&ghvm->bindings_lock);
	b = mtree_load(&ghvm->bindings, gfn);
	if (!b) {
		ret = -ENOENT;
		goto unlock;
	}

	if (write && !(b->flags & GUNYAH_MEM_ALLOW_WRITE)) {
		ret = -EPERM;
		goto unlock;
	}

	filemap_invalidate_lock_shared(b->file->f_mapping);
	folio = guest_memfd_grab_folio(b->file, gunyah_gfn_to_off(b, gfn),
					GUEST_MEMFD_PREPARE);
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		pr_err_ratelimited(
			"Failed to obtain memory for guest addr %016llx: %d\n",
			gpa, ret);
		goto unlock;
	}

	if (gunyah_guest_mem_is_lend(ghvm, b->flags)) {
		ret = guest_memfd_make_inaccessible(file_inode(b->file), folio);
		if (ret) {
			pr_err_ratelimited(
				"Failed to make guest addr %016llx inaccessible: %d\n",
				gpa, ret);
			goto unlock;
		}
	}

	/**
	 * the folio covers the requested guest address, but the folio may not
	 * start at the requested guest address. recompute the gfn based on the
	 * folio itself.
	 */
	gfn = gunyah_off_to_gfn(b, folio_index(folio));

	ret = gunyah_vm_provide_folio(ghvm, folio, gfn,
				      !gunyah_guest_mem_is_lend(ghvm, b->flags),
				      !!(b->flags & GUNYAH_MEM_ALLOW_WRITE));
	filemap_invalidate_unlock_shared(b->file->f_mapping);
	if (ret) {
		if (ret != -EAGAIN)
			pr_err_ratelimited(
				"Failed to provide folio for guest addr: %016llx: %d\n",
				gpa, ret);
		goto out;
	}
out:
	folio_unlock(folio);
	folio_put(folio);
unlock:
	up_read(&ghvm->bindings_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_gmem_demand_page);
