// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "gunyah_guest_mem: " fmt

#include <linux/file.h>
#include <linux/guest_memfd.h>

#include <uapi/linux/gunyah.h>

#include "vm_mgr.h"

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
