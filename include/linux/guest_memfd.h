/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _LINUX_GUEST_MEMFD_H
#define _LINUX_GUEST_MEMFD_H

#include <linux/fs.h>

struct guest_memfd_operations {
	int (*invalidate_begin)(struct inode *inode, pgoff_t offset, unsigned long nr);
	void (*invalidate_end)(struct inode *inode, pgoff_t offset, unsigned long nr);
	int (*prepare)(struct inode *inode, pgoff_t offset, struct folio *folio);
	void (*free_folio)(struct inode *inode, struct folio *folio);
	int (*release)(struct inode *inode);
};

enum {
	GUEST_MEMFD_FLAG_ALLOW_HUGEPAGE = BIT(0),
};

struct guest_memfd {
	unsigned long flags;
	const struct guest_memfd_operations *ops;
};

enum {
	GUEST_MEMFD_GRAB_UPTODATE	= BIT(0),
	GUEST_MEMFD_PREPARE		= BIT(1),
};

struct folio *guest_memfd_grab_folio(struct inode *inode, pgoff_t index, u32 flags);
struct file *guest_memfd_alloc(const char *name, struct guest_memfd *gmem, loff_t size);
bool is_guest_memfd(struct file *file);

#endif
