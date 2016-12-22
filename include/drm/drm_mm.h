/**************************************************************************
 *
 * Copyright 2006-2008 Tungsten Graphics, Inc., Cedar Park, TX. USA.
 * Copyright 2016 Intel Corporation
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 **************************************************************************/
/*
 * Authors:
 * Thomas Hellstrom <thomas-at-tungstengraphics-dot-com>
 */

#ifndef _DRM_MM_H_
#define _DRM_MM_H_

/*
 * Generic range manager structs
 */
#include <linux/bug.h>
#include <linux/rbtree.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#ifdef CONFIG_DEBUG_FS
#include <linux/seq_file.h>
#endif
#ifdef CONFIG_DRM_DEBUG_MM
#include <linux/stackdepot.h>
#endif

#ifdef CONFIG_DRM_DEBUG_MM
#define DRM_MM_BUG_ON(expr) BUG_ON(expr)
#else
#define DRM_MM_BUG_ON(expr) BUILD_BUG_ON_INVALID(expr)
#endif

enum drm_mm_search_flags {
	DRM_MM_SEARCH_DEFAULT =		0,
	DRM_MM_SEARCH_BEST =		1 << 0,
	DRM_MM_SEARCH_BELOW =		1 << 1,
};

enum drm_mm_allocator_flags {
	DRM_MM_CREATE_DEFAULT =		0,
	DRM_MM_CREATE_TOP =		1 << 0,
};

#define DRM_MM_BOTTOMUP DRM_MM_SEARCH_DEFAULT, DRM_MM_CREATE_DEFAULT
#define DRM_MM_TOPDOWN DRM_MM_SEARCH_BELOW, DRM_MM_CREATE_TOP

struct drm_mm_node {
	struct list_head node_list;
	struct list_head hole_stack;
	struct rb_node rb;
	unsigned hole_follows : 1;
	unsigned scanned_block : 1;
	unsigned scanned_prev_free : 1;
	unsigned scanned_next_free : 1;
	unsigned scanned_preceeds_hole : 1;
	unsigned allocated : 1;
	unsigned long color;
	u64 start;
	u64 size;
	u64 __subtree_last;
	struct drm_mm *mm;
#ifdef CONFIG_DRM_DEBUG_MM
	depot_stack_handle_t stack;
#endif
};

struct drm_mm {
	/* List of all memory nodes that immediately precede a free hole. */
	struct list_head hole_stack;
	/* head_node.node_list is the list of all memory nodes, ordered
	 * according to the (increasing) start address of the memory node. */
	struct drm_mm_node head_node;
	/* Keep an interval_tree for fast lookup of drm_mm_nodes by address. */
	struct rb_root interval_tree;

	void (*color_adjust)(const struct drm_mm_node *node,
			     unsigned long color,
			     u64 *start, u64 *end);

	unsigned long scan_active;
};

struct drm_mm_scan {
	struct drm_mm *mm;

	u64 size;
	u64 alignment;

	u64 range_start;
	u64 range_end;

	u64 hit_start;
	u64 hit_end;

	struct drm_mm_node *prev_scanned_node;

	unsigned long color;
	unsigned int flags;
};

/**
 * drm_mm_node_allocated - checks whether a node is allocated
 * @node: drm_mm_node to check
 *
 * Drivers are required to clear a node prior to using it with the
 * drm_mm range manager.
 *
 * Drivers should use this helper for proper encapsulation of drm_mm
 * internals.
 *
 * Returns:
 * True if the @node is allocated.
 */
static inline bool drm_mm_node_allocated(const struct drm_mm_node *node)
{
	return node->allocated;
}

/**
 * drm_mm_initialized - checks whether an allocator is initialized
 * @mm: drm_mm to check
 *
 * Drivers should clear the struct drm_mm prior to initialisation if they
 * want to use this function.
 *
 * Drivers should use this helper for proper encapsulation of drm_mm
 * internals.
 *
 * Returns:
 * True if the @mm is initialized.
 */
static inline bool drm_mm_initialized(const struct drm_mm *mm)
{
	return mm->hole_stack.next;
}

static inline u64 __drm_mm_hole_node_start(const struct drm_mm_node *hole_node)
{
	return hole_node->start + hole_node->size;
}

/**
 * drm_mm_hole_node_start - computes the start of the hole following @node
 * @hole_node: drm_mm_node which implicitly tracks the following hole
 *
 * This is useful for driver-specific debug dumpers. Otherwise drivers should
 * not inspect holes themselves. Drivers must check first whether a hole indeed
 * follows by looking at node->hole_follows.
 *
 * Returns:
 * Start of the subsequent hole.
 */
static inline u64 drm_mm_hole_node_start(const struct drm_mm_node *hole_node)
{
	DRM_MM_BUG_ON(!hole_node->hole_follows);
	return __drm_mm_hole_node_start(hole_node);
}

static inline u64 __drm_mm_hole_node_end(const struct drm_mm_node *hole_node)
{
	return list_next_entry(hole_node, node_list)->start;
}

/**
 * drm_mm_hole_node_end - computes the end of the hole following @node
 * @hole_node: drm_mm_node which implicitly tracks the following hole
 *
 * This is useful for driver-specific debug dumpers. Otherwise drivers should
 * not inspect holes themselves. Drivers must check first whether a hole indeed
 * follows by looking at node->hole_follows.
 *
 * Returns:
 * End of the subsequent hole.
 */
static inline u64 drm_mm_hole_node_end(const struct drm_mm_node *hole_node)
{
	return __drm_mm_hole_node_end(hole_node);
}

/**
 * drm_mm_nodes - list of nodes under the drm_mm range manager
 * @mm: the struct drm_mm range manger
 *
 * As the drm_mm range manager hides its node_list deep with its
 * structure, extracting it looks painful and repetitive. This is
 * not expected to be used outside of the drm_mm_for_each_node()
 * macros and similar internal functions.
 *
 * Returns:
 * The node list, may be empty.
 */
#define drm_mm_nodes(mm) (&(mm)->head_node.node_list)

/**
 * drm_mm_for_each_node - iterator to walk over all allocated nodes
 * @entry: drm_mm_node structure to assign to in each iteration step
 * @mm: drm_mm allocator to walk
 *
 * This iterator walks over all nodes in the range allocator. It is implemented
 * with list_for_each, so not save against removal of elements.
 */
#define drm_mm_for_each_node(entry, mm) \
	list_for_each_entry(entry, drm_mm_nodes(mm), node_list)

/**
 * drm_mm_for_each_node_safe - iterator to walk over all allocated nodes
 * @entry: drm_mm_node structure to assign to in each iteration step
 * @next: drm_mm_node structure to store the next step
 * @mm: drm_mm allocator to walk
 *
 * This iterator walks over all nodes in the range allocator. It is implemented
 * with list_for_each_safe, so save against removal of elements.
 */
#define drm_mm_for_each_node_safe(entry, next, mm) \
	list_for_each_entry_safe(entry, next, drm_mm_nodes(mm), node_list)

#define __drm_mm_for_each_hole(entry, mm, hole_start, hole_end, backwards) \
	for (entry = list_entry((backwards) ? (mm)->hole_stack.prev : (mm)->hole_stack.next, struct drm_mm_node, hole_stack); \
	     &entry->hole_stack != &(mm)->hole_stack ? \
	     hole_start = drm_mm_hole_node_start(entry), \
	     hole_end = drm_mm_hole_node_end(entry), \
	     1 : 0; \
	     entry = list_entry((backwards) ? entry->hole_stack.prev : entry->hole_stack.next, struct drm_mm_node, hole_stack))

/**
 * drm_mm_for_each_hole - iterator to walk over all holes
 * @entry: drm_mm_node used internally to track progress
 * @mm: drm_mm allocator to walk
 * @hole_start: ulong variable to assign the hole start to on each iteration
 * @hole_end: ulong variable to assign the hole end to on each iteration
 *
 * This iterator walks over all holes in the range allocator. It is implemented
 * with list_for_each, so not save against removal of elements. @entry is used
 * internally and will not reflect a real drm_mm_node for the very first hole.
 * Hence users of this iterator may not access it.
 *
 * Implementation Note:
 * We need to inline list_for_each_entry in order to be able to set hole_start
 * and hole_end on each iteration while keeping the macro sane.
 *
 * The __drm_mm_for_each_hole version is similar, but with added support for
 * going backwards.
 */
#define drm_mm_for_each_hole(entry, mm, hole_start, hole_end) \
	__drm_mm_for_each_hole(entry, mm, hole_start, hole_end, 0)

/*
 * Basic range manager support (drm_mm.c)
 */
int drm_mm_reserve_node(struct drm_mm *mm, struct drm_mm_node *node);

int drm_mm_insert_node_generic(struct drm_mm *mm,
			       struct drm_mm_node *node,
			       u64 size,
			       u64 alignment,
			       unsigned long color,
			       enum drm_mm_search_flags sflags,
			       enum drm_mm_allocator_flags aflags);
/**
 * drm_mm_insert_node - search for space and insert @node
 * @mm: drm_mm to allocate from
 * @node: preallocate node to insert
 * @size: size of the allocation
 * @alignment: alignment of the allocation
 * @flags: flags to fine-tune the allocation
 *
 * This is a simplified version of drm_mm_insert_node_generic() with @color set
 * to 0.
 *
 * The preallocated node must be cleared to 0.
 *
 * Returns:
 * 0 on success, -ENOSPC if there's no suitable hole.
 */
static inline int drm_mm_insert_node(struct drm_mm *mm,
				     struct drm_mm_node *node,
				     u64 size,
				     u64 alignment,
				     enum drm_mm_search_flags flags)
{
	return drm_mm_insert_node_generic(mm, node, size, alignment, 0, flags,
					  DRM_MM_CREATE_DEFAULT);
}

int drm_mm_insert_node_in_range_generic(struct drm_mm *mm,
					struct drm_mm_node *node,
					u64 size,
					u64 alignment,
					unsigned long color,
					u64 start,
					u64 end,
					enum drm_mm_search_flags sflags,
					enum drm_mm_allocator_flags aflags);
/**
 * drm_mm_insert_node_in_range - ranged search for space and insert @node
 * @mm: drm_mm to allocate from
 * @node: preallocate node to insert
 * @size: size of the allocation
 * @alignment: alignment of the allocation
 * @start: start of the allowed range for this node
 * @end: end of the allowed range for this node
 * @flags: flags to fine-tune the allocation
 *
 * This is a simplified version of drm_mm_insert_node_in_range_generic() with
 * @color set to 0.
 *
 * The preallocated node must be cleared to 0.
 *
 * Returns:
 * 0 on success, -ENOSPC if there's no suitable hole.
 */
static inline int drm_mm_insert_node_in_range(struct drm_mm *mm,
					      struct drm_mm_node *node,
					      u64 size,
					      u64 alignment,
					      u64 start,
					      u64 end,
					      enum drm_mm_search_flags flags)
{
	return drm_mm_insert_node_in_range_generic(mm, node, size, alignment,
						   0, start, end, flags,
						   DRM_MM_CREATE_DEFAULT);
}

void drm_mm_remove_node(struct drm_mm_node *node);
void drm_mm_replace_node(struct drm_mm_node *old, struct drm_mm_node *new);
void drm_mm_init(struct drm_mm *mm, u64 start, u64 size);
void drm_mm_takedown(struct drm_mm *mm);

/**
 * drm_mm_clean - checks whether an allocator is clean
 * @mm: drm_mm allocator to check
 *
 * Returns:
 * True if the allocator is completely free, false if there's still a node
 * allocated in it.
 */
static inline bool drm_mm_clean(const struct drm_mm *mm)
{
	return list_empty(drm_mm_nodes(mm));
}

struct drm_mm_node *
__drm_mm_interval_first(const struct drm_mm *mm, u64 start, u64 last);

/**
 * drm_mm_for_each_node_in_range - iterator to walk over a range of
 * allocated nodes
 * @node__: drm_mm_node structure to assign to in each iteration step
 * @mm__: drm_mm allocator to walk
 * @start__: starting offset, the first node will overlap this
 * @end__: ending offset, the last node will start before this (but may overlap)
 *
 * This iterator walks over all nodes in the range allocator that lie
 * between @start and @end. It is implemented similarly to list_for_each(),
 * but using the internal interval tree to accelerate the search for the
 * starting node, and so not safe against removal of elements. It assumes
 * that @end is within (or is the upper limit of) the drm_mm allocator.
 */
#define drm_mm_for_each_node_in_range(node__, mm__, start__, end__)	\
	for (node__ = __drm_mm_interval_first((mm__), (start__), (end__)-1); \
	     node__ && node__->start < (end__);				\
	     node__ = list_next_entry(node__, node_list))

void drm_mm_scan_init_with_range(struct drm_mm_scan *scan,
				 struct drm_mm *mm,
				 u64 size, u64 alignment, unsigned long color,
				 u64 start, u64 end,
				 unsigned int flags);

/**
 * drm_mm_scan_init - initialize lru scanning
 * @scan: scan state
 * @mm: drm_mm to scan
 * @size: size of the allocation
 * @alignment: alignment of the allocation
 * @color: opaque tag value to use for the allocation
 * @flags: flags to specify how the allocation will be performed afterwards
 *
 * This simply sets up the scanning routines with the parameters for the desired
 * hole.
 *
 * Warning:
 * As long as the scan list is non-empty, no other operations than
 * adding/removing nodes to/from the scan list are allowed.
 */
static inline void drm_mm_scan_init(struct drm_mm_scan *scan,
				    struct drm_mm *mm,
				    u64 size,
				    u64 alignment,
				    unsigned long color,
				    unsigned int flags)
{
	drm_mm_scan_init_with_range(scan, mm,
				    size, alignment, color,
				    0, U64_MAX,
				    flags);
}

bool drm_mm_scan_add_block(struct drm_mm_scan *scan,
			   struct drm_mm_node *node);
bool drm_mm_scan_remove_block(struct drm_mm_scan *scan,
			      struct drm_mm_node *node);

void drm_mm_debug_table(const struct drm_mm *mm, const char *prefix);
#ifdef CONFIG_DEBUG_FS
int drm_mm_dump_table(struct seq_file *m, const struct drm_mm *mm);
#endif

#endif
