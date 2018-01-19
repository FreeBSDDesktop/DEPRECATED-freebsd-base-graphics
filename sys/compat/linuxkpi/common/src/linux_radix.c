/*-
 * Copyright (c) 2010 Isilon Systems, Inc.
 * Copyright (c) 2010 iX Systems, Inc.
 * Copyright (c) 2010 Panasas, Inc.
 * Copyright (c) 2013, 2014 Mellanox Technologies, Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/radix-tree.h>
#include <linux/err.h>

static MALLOC_DEFINE(M_RADIX, "radix", "Linux radix compat");

static inline int
radix_max(struct radix_tree_root *root)
{
	return (1 << (root->height * RADIX_TREE_MAP_SHIFT)) - 1;
}

static inline int
radix_pos(long id, int height)
{
	return (id >> (RADIX_TREE_MAP_SHIFT * height)) & RADIX_TREE_MAP_MASK;
}

void *
radix_tree_lookup(struct radix_tree_root *root, unsigned long index)
{
	struct radix_tree_node *node;
	void *item;
	int height;

	item = NULL;
	node = root->rnode;
	height = root->height - 1;
	if (index > radix_max(root))
		goto out;
	while (height && node)
		node = node->slots[radix_pos(index, height--)];
	if (node)
		item = node->slots[radix_pos(index, 0)];

out:
	return (item);
}

void *
radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{
	struct radix_tree_node *stack[RADIX_TREE_MAX_HEIGHT];
	struct radix_tree_node *node;
	void *item;
	int height;
	int idx;

	item = NULL;
	node = root->rnode;
	height = root->height - 1;
	if (index > radix_max(root))
		goto out;
	/*
	 * Find the node and record the path in stack.
	 */
	while (height && node) {
		stack[height] = node;
		node = node->slots[radix_pos(index, height--)];
	}
	idx = radix_pos(index, 0);
	if (node)
		item = node->slots[idx];
	/*
	 * If we removed something reduce the height of the tree.
	 */
	if (item)
		for (;;) {
			node->slots[idx] = NULL;
			node->count--;
			if (node->count > 0)
				break;
			free(node, M_RADIX);
			if (node == root->rnode) {
				root->rnode = NULL;
				root->height = 0;
				break;
			}
			height++;
			node = stack[height];
			idx = radix_pos(index, height);
		}
out:
	return (item);
}

int
radix_tree_insert(struct radix_tree_root *root, unsigned long index, void *item)
{
	struct radix_tree_node *node;
	struct radix_tree_node *temp[RADIX_TREE_MAX_HEIGHT - 1];
	int height;
	int idx;

	/* bail out upon insertion of a NULL item */
	if (item == NULL)
		return (-EINVAL);

	/* get root node, if any */
	node = root->rnode;

	/* allocate root node, if any */
	if (node == NULL) {
		node = malloc(sizeof(*node), M_RADIX, root->gfp_mask | M_ZERO);
		if (node == NULL)
			return (-ENOMEM);
		root->rnode = node;
		root->height++;
	}

	/* expand radix tree as needed */
	while (radix_max(root) < index) {

		/* check if the radix tree is getting too big */
		if (root->height == RADIX_TREE_MAX_HEIGHT)
			return (-E2BIG);

		/*
		 * If the root radix level is not empty, we need to
		 * allocate a new radix level:
		 */
		if (node->count != 0) {
			node = malloc(sizeof(*node), M_RADIX, root->gfp_mask | M_ZERO);
			if (node == NULL)
				return (-ENOMEM);
			node->slots[0] = root->rnode;
			node->count++;
			root->rnode = node;
		}
		root->height++;
	}

	/* get radix tree height index */
	height = root->height - 1;

	/* walk down the tree until the first missing node, if any */
	for ( ; height != 0; height--) {
		idx = radix_pos(index, height);
		if (node->slots[idx] == NULL)
			break;
		node = node->slots[idx];
	}

	/* allocate the missing radix levels, if any */
	for (idx = 0; idx != height; idx++) {
		temp[idx] = malloc(sizeof(*node), M_RADIX,
		    root->gfp_mask | M_ZERO);
		if (temp[idx] == NULL) {
			while(idx--)
				free(temp[idx], M_RADIX);
			/* Check if we should free the root node as well. */
			if (root->rnode->count == 0) {
				free(root->rnode, M_RADIX);
				root->rnode = NULL;
				root->height = 0;
			}
			return (-ENOMEM);
		}
	}

	/* setup new radix levels, if any */
	for ( ; height != 0; height--) {
		idx = radix_pos(index, height);
		node->slots[idx] = temp[height - 1];
		node->count++;
		node = node->slots[idx];
	}

	/*
	 * Insert and adjust count if the item does not already exist.
	 */
	idx = radix_pos(index, 0);
	if (node->slots[idx])
		return (-EEXIST);
	node->slots[idx] = item;
	node->count++;
	
	return (0);
}




// Radix Tree Iterator impl
// Review needed!

static inline void __set_iter_shift(struct radix_tree_iter *iter,
					unsigned int shift)
{
#ifdef CONFIG_RADIX_TREE_MULTIORDER
	iter->shift = shift;
#endif
}

static unsigned int iter_offset(const struct radix_tree_iter *iter)
{
	return (iter->index >> iter_shift(iter)) & RADIX_TREE_MAP_MASK;
}

static inline struct radix_tree_node *entry_to_node(void *ptr)
{
	return (void *)((unsigned long)ptr & ~RADIX_TREE_INTERNAL_NODE);
}

static inline unsigned long shift_maxindex(unsigned int shift)
{
	return (RADIX_TREE_MAP_SIZE << shift) - 1;
}

static inline unsigned long node_maxindex(const struct radix_tree_node *node)
{
	return shift_maxindex(node->shift);
}

static unsigned radix_tree_load_root(const struct radix_tree_root *root,
		struct radix_tree_node **nodep, unsigned long *maxindex)
{
	struct radix_tree_node *node = rcu_dereference_raw(root->rnode);

	*nodep = node;

	if (likely(radix_tree_is_internal_node(node))) {
		node = entry_to_node(node);
		*maxindex = node_maxindex(node);
		return node->shift + RADIX_TREE_MAP_SHIFT;
	}

	*maxindex = 0;
	return 0;
}

static __always_inline unsigned long
radix_tree_find_next_bit(struct radix_tree_node *node, unsigned int tag,
			 unsigned long offset)
{
	const unsigned long *addr = node->tags[tag];

	if (offset < RADIX_TREE_MAP_SIZE) {
		unsigned long tmp;

		addr += offset / BITS_PER_LONG;
		tmp = *addr >> (offset % BITS_PER_LONG);
		if (tmp)
			return __ffs(tmp) + offset;
		offset = (offset + BITS_PER_LONG) & ~(BITS_PER_LONG - 1);
		while (offset < RADIX_TREE_MAP_SIZE) {
			tmp = *++addr;
			if (tmp)
				return __ffs(tmp) + offset;
			offset += BITS_PER_LONG;
		}
	}
	return RADIX_TREE_MAP_SIZE;
}

static inline void *node_to_entry(void *ptr)
{
	return (void *)((unsigned long)ptr | RADIX_TREE_INTERNAL_NODE);
}

#define RADIX_TREE_RETRY	node_to_entry(NULL)

#ifdef CONFIG_RADIX_TREE_MULTIORDER
/* Sibling slots point directly to another slot in the same node */
static inline
bool is_sibling_entry(const struct radix_tree_node *parent, void *node)
{
	void __rcu **ptr = node;
	return (parent->slots <= ptr) &&
			(ptr < parent->slots + RADIX_TREE_MAP_SIZE);
}
#else
static inline
bool is_sibling_entry(const struct radix_tree_node *parent, void *node)
{
	return false;
}
#endif

/* Construct iter->tags bit-mask from node->tags[tag] array */
static void set_iter_tags(struct radix_tree_iter *iter,
				struct radix_tree_node *node, unsigned offset,
				unsigned tag)
{
	unsigned tag_long = offset / BITS_PER_LONG;
	unsigned tag_bit  = offset % BITS_PER_LONG;

	if (!node) {
		iter->tags = 1;
		return;
	}

	iter->tags = node->tags[tag][tag_long] >> tag_bit;

	/* This never happens if RADIX_TREE_TAG_LONGS == 1 */
	if (tag_long < RADIX_TREE_TAG_LONGS - 1) {
		/* Pick tags from next element */
		if (tag_bit)
			iter->tags |= node->tags[tag][tag_long + 1] <<
						(BITS_PER_LONG - tag_bit);
		/* Clip chunk size, here only BITS_PER_LONG tags */
		iter->next_index = __radix_tree_iter_add(iter, BITS_PER_LONG);
	}
}

static inline int tag_get(const struct radix_tree_node *node, unsigned int tag,
		int offset)
{
	return test_bit(offset, __DECONST(unsigned long *, node->tags[tag]));
}

static inline int root_tag_get(const struct radix_tree_root *root, unsigned tag)
{
	return (__force int)root->gfp_mask & (1 << (tag + ROOT_TAG_SHIFT));
}

static unsigned int radix_tree_descend(const struct radix_tree_node *parent,
			struct radix_tree_node **nodep, unsigned long index)
{
	unsigned int offset = (index >> parent->shift) & RADIX_TREE_MAP_MASK;
	void __rcu **entry = rcu_dereference_raw(parent->slots[offset]);

#ifdef CONFIG_RADIX_TREE_MULTIORDER
	if (radix_tree_is_internal_node(entry)) {
		if (is_sibling_entry(parent, entry)) {
			void __rcu **sibentry;
			sibentry = (void __rcu **) entry_to_node(entry);
			offset = get_slot_offset(parent, sibentry);
			entry = rcu_dereference_raw(*sibentry);
		}
	}
#endif

	*nodep = (void *)entry;
	return offset;
}

void __rcu **radix_tree_next_chunk(const struct radix_tree_root *root,
			     struct radix_tree_iter *iter, unsigned flags)
{
	unsigned tag = flags & RADIX_TREE_ITER_TAG_MASK;
	struct radix_tree_node *node, *child;
	unsigned long index, offset, maxindex;

	if ((flags & RADIX_TREE_ITER_TAGGED) && !root_tag_get(root, tag))
		return NULL;

	/*
	 * Catch next_index overflow after ~0UL. iter->index never overflows
	 * during iterating; it can be zero only at the beginning.
	 * And we cannot overflow iter->next_index in a single step,
	 * because RADIX_TREE_MAP_SHIFT < BITS_PER_LONG.
	 *
	 * This condition also used by radix_tree_next_slot() to stop
	 * contiguous iterating, and forbid switching to the next chunk.
	 */
	index = iter->next_index;
	if (!index && iter->index)
		return NULL;

 restart:
	radix_tree_load_root(root, &child, &maxindex);
	if (index > maxindex)
		return NULL;
	if (!child)
		return NULL;

	if (!radix_tree_is_internal_node(child)) {
		/* Single-slot tree */
		iter->index = index;
		iter->next_index = maxindex + 1;
		iter->tags = 1;
		iter->node = NULL;
		__set_iter_shift(iter, 0);
		return __DECONST(void __rcu **, &root->rnode);
	}

	do {
		node = entry_to_node(child);
		offset = radix_tree_descend(node, &child, index);

		if ((flags & RADIX_TREE_ITER_TAGGED) ?
				!tag_get(node, tag, offset) : !child) {
			/* Hole detected */
			if (flags & RADIX_TREE_ITER_CONTIG)
				return NULL;

			if (flags & RADIX_TREE_ITER_TAGGED)
				offset = radix_tree_find_next_bit(node, tag,
						offset + 1);
			else
				while (++offset	< RADIX_TREE_MAP_SIZE) {
					void *slot = rcu_dereference_raw(
							node->slots[offset]);
					if (is_sibling_entry(node, slot))
						continue;
					if (slot)
						break;
				}
			index &= ~node_maxindex(node);
			index += offset << node->shift;
			/* Overflow after ~0UL */
			if (!index)
				return NULL;
			if (offset == RADIX_TREE_MAP_SIZE)
				goto restart;
			child = rcu_dereference_raw(node->slots[offset]);
		}

		if (!child)
			goto restart;
		if (child == RADIX_TREE_RETRY)
			break;
	} while (radix_tree_is_internal_node(child));

	/* Update the iterator state */
	iter->index = (index &~ node_maxindex(node)) | (offset << node->shift);
	iter->next_index = (index | node_maxindex(node)) + 1;
	iter->node = node;
	__set_iter_shift(iter, node->shift);

	if (flags & RADIX_TREE_ITER_TAGGED)
		set_iter_tags(iter, node, offset, tag);

	return node->slots + offset;
}
