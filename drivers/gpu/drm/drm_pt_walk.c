// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright © 2022 Intel Corporation
 */
#include <drm/drm_pt_walk.h>

/**
 * DOC: GPU page-table tree walking.
 * The utilities in this file are similar to the CPU page-table walk
 * utilities in mm/pagewalk.c. The main difference is that we distinguish
 * the various levels of a page-table tree with an unsigned integer rather
 * than by name. 0 is the lowest level, and page-tables with level 0 can
 * not be directories pointing to lower levels, whereas all other levels
 * can. The user of the utilities determines the highest level.
 *
 * Nomenclature:
 * Each struct drm_pt, regardless of level is referred to as a page table, and
 * multiple page tables typically form a page table tree with page tables at
 * intermediate levels being page directories pointing at page tables at lower
 * levels. A shared page table for a given address range is a page-table which
 * is neither fully within nor fully outside the address range and that can
 * thus be shared by two or more address ranges.
 */
static u64 drm_pt_addr_end(u64 addr, u64 end, unsigned int level,
			   const struct drm_pt_walk *walk)
{
	u64 size = 1ull << walk->shifts[level];
	u64 tmp = round_up(addr + 1, size);

	return min_t(u64, tmp, end);
}

static bool drm_pt_next(pgoff_t *offset, u64 *addr, u64 next, u64 end,
			unsigned int level, const struct drm_pt_walk *walk)
{
	pgoff_t step = 1;

	/* Shared pt walk skips to the last pagetable */
	if (unlikely(walk->shared_pt_mode)) {
		unsigned int shift = walk->shifts[level];
		u64 skip_to = round_down(end, 1ull << shift);

		if (skip_to > next) {
			step += (skip_to - next) >> shift;
			next = skip_to;
		}
	}

	*addr = next;
	*offset += step;

	return next != end;
}

/**
 * drm_pt_walk_range() - Walk a range of a gpu page table tree with callbacks
 * for each page-table entry in all levels.
 * @parent: The root page table for walk start.
 * @level: The root page table level.
 * @addr: Virtual address start.
 * @end: Virtual address end + 1.
 * @walk: Walk info.
 *
 * Similar to the CPU page-table walker, this is a helper to walk
 * a gpu page table and call a provided callback function for each entry.
 *
 * Return: 0 on success, negative error code on error. The error is
 * propagated from the callback and on error the walk is terminated.
 */
int drm_pt_walk_range(struct drm_pt *parent, unsigned int level,
		      u64 addr, u64 end, struct drm_pt_walk *walk)
{
	pgoff_t offset = drm_pt_offset(addr, level, walk);
	struct drm_pt **entries = parent->dir ? parent->dir->entries : NULL;
	const struct drm_pt_walk_ops *ops = walk->ops;
	enum page_walk_action action;
	struct drm_pt *child;
	int err = 0;
	u64 next;

	do {
		next = drm_pt_addr_end(addr, end, level, walk);
		if (walk->shared_pt_mode && drm_pt_covers(addr, next, level,
							  walk))
			continue;
again:
		action = ACTION_SUBTREE;
		child = entries ? entries[offset] : NULL;
		err = ops->pt_entry(parent, offset, level, addr, next,
				    &child, &action, walk);
		if (err)
			break;

		/* Probably not needed yet for gpu pagetable walk. */
		if (unlikely(action == ACTION_AGAIN))
			goto again;

		if (likely(!level || !child || action == ACTION_CONTINUE))
			continue;

		err = drm_pt_walk_range(child, level - 1, addr, next, walk);

		if (!err && ops->pt_post_descend)
			err = ops->pt_post_descend(parent, offset, level, addr,
						   next, &child, &action, walk);
		if (err)
			break;

	} while (drm_pt_next(&offset, &addr, next, end, level, walk));

	return err;
}
EXPORT_SYMBOL(drm_pt_walk_range);

/**
 * drm_pt_walk_shared() - Walk shared page tables of a page-table tree.
 * @parent: Root page table directory.
 * @level: Level of the root.
 * @addr: Start address.
 * @end: Last address + 1.
 * @walk: Walk info.
 *
 * This function is similar to drm_pt_walk_range() but it skips page tables
 * that are private to the range. Since the root (or @parent) page table is
 * typically also a shared page table this function is different in that it
 * calls the pt_entry callback and the post_descend callback also for the
 * root. The root can be detected in the callbacks by checking whether
 * parent == *child.
 * Walking only the shared page tables is common for unbind-type operations
 * where the page-table entries for an address range are cleared or detached
 * from the main page-table tree.
 *
 * Return: 0 on success, negative error code on error: If a callback
 * returns an error, the walk will be terminated and the error returned by
 * this function.
 */
int drm_pt_walk_shared(struct drm_pt *parent, unsigned int level,
		       u64 addr, u64 end, struct drm_pt_walk *walk)
{
	const struct drm_pt_walk_ops *ops = walk->ops;
	enum page_walk_action action = ACTION_SUBTREE;
	struct drm_pt *child = parent;
	int err;

	walk->shared_pt_mode = true;
	err = walk->ops->pt_entry(parent, 0, level + 1, addr, end,
				  &child, &action, walk);

	if (err || action != ACTION_SUBTREE)
		return err;

	err = drm_pt_walk_range(parent, level, addr, end, walk);
	if (!err && ops->pt_post_descend) {
		err = ops->pt_post_descend(parent, 0, level + 1, addr, end,
					   &child, &action, walk);
	}
	return err;
}
EXPORT_SYMBOL(drm_pt_walk_shared);
