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
 *
 * $FreeBSD$
 */
#ifndef	_LINUX_RADIX_TREE_H_
#define	_LINUX_RADIX_TREE_H_

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/rcupdate.h>

#define RADIX_TREE_MAX_TAGS 3

#define	RADIX_TREE_MAP_SHIFT	6
#define	RADIX_TREE_MAP_SIZE	(1 << RADIX_TREE_MAP_SHIFT)
#define	RADIX_TREE_MAP_MASK	(RADIX_TREE_MAP_SIZE - 1)
#define	RADIX_TREE_MAX_HEIGHT						\
	    DIV_ROUND_UP((sizeof(long) * NBBY), RADIX_TREE_MAP_SHIFT)

#define RADIX_TREE_TAG_LONGS	\
	((RADIX_TREE_MAP_SIZE + BITS_PER_LONG - 1) / BITS_PER_LONG)

struct radix_tree_node {
	unsigned char	shift;
	void		*slots[RADIX_TREE_MAP_SIZE];
	int		count;
	unsigned long	tags[RADIX_TREE_MAX_TAGS][RADIX_TREE_TAG_LONGS];
};

struct radix_tree_root {
	struct radix_tree_node	*rnode;
	gfp_t			gfp_mask;
	int			height;
};


#define	RADIX_TREE_INIT(mask)						\
	    { .rnode = NULL, .gfp_mask = mask, .height = 0 };
#define	INIT_RADIX_TREE(root, mask)					\
	    { (root)->rnode = NULL; (root)->gfp_mask = mask; (root)->height = 0; }
#define	RADIX_TREE(name, mask)						\
	    struct radix_tree_root name = RADIX_TREE_INIT(mask)

void	*radix_tree_lookup(struct radix_tree_root *, unsigned long);
void	*radix_tree_delete(struct radix_tree_root *, unsigned long);
int	radix_tree_insert(struct radix_tree_root *, unsigned long, void *);





// Radix Tree Iterator impl
// Review needed!

#define RADIX_TREE_ENTRY_MASK		3UL
#define RADIX_TREE_INTERNAL_NODE	1UL
#define RADIX_TREE_EXCEPTIONAL_ENTRY	2
#define RADIX_TREE_EXCEPTIONAL_SHIFT	2
#define ROOT_IS_IDR	((__force gfp_t)(1 << __GFP_BITS_SHIFT))
#define ROOT_TAG_SHIFT	(__GFP_BITS_SHIFT + 1)

struct radix_tree_iter {
	unsigned long	index;
	unsigned long	next_index;
	unsigned long	tags;
	struct radix_tree_node *node;
#ifdef CONFIG_RADIX_TREE_MULTIORDER  // Probably not using this? 
	unsigned int	shift;
#endif
};

enum {
	RADIX_TREE_ITER_TAG_MASK = 0x0f,	/* tag index in lower nybble */
	RADIX_TREE_ITER_TAGGED   = 0x10,	/* lookup tagged slots */
	RADIX_TREE_ITER_CONTIG   = 0x20,	/* stop at first hole */
};

static inline int radix_tree_exception(void *arg)
{
	return unlikely((unsigned long)arg & RADIX_TREE_ENTRY_MASK);
}

static inline bool radix_tree_is_internal_node(void *ptr)
{
	return ((unsigned long)ptr & RADIX_TREE_ENTRY_MASK) ==
				RADIX_TREE_INTERNAL_NODE;
}

static inline unsigned int iter_shift(const struct radix_tree_iter *iter)
{
#ifdef CONFIG_RADIX_TREE_MULTIORDER
	return iter->shift;
#else
	return 0;
#endif
}


void __rcu **radix_tree_next_chunk(const struct radix_tree_root *,
			     struct radix_tree_iter *iter, unsigned flags);



static __always_inline void __rcu **
radix_tree_iter_init(struct radix_tree_iter *iter, unsigned long start)
{
	/*
	 * Leave iter->tags uninitialized. radix_tree_next_chunk() will fill it
	 * in the case of a successful tagged chunk lookup.  If the lookup was
	 * unsuccessful or non-tagged then nobody cares about ->tags.
	 *
	 * Set index to zero to bypass next_index overflow protection.
	 * See the comment in radix_tree_next_chunk() for details.
	 */
	iter->index = 0;
	iter->next_index = start;
	return NULL;
}


static inline void __rcu **
radix_tree_iter_lookup(const struct radix_tree_root *root,
			struct radix_tree_iter *iter, unsigned long index)
{
	radix_tree_iter_init(iter, index);
	return radix_tree_next_chunk(root, iter, RADIX_TREE_ITER_CONTIG);
}

static inline void __rcu **
radix_tree_iter_find(const struct radix_tree_root *root,
			struct radix_tree_iter *iter, unsigned long index)
{
	radix_tree_iter_init(iter, index);
	return radix_tree_next_chunk(root, iter, 0);
}

static inline __must_check
void __rcu **radix_tree_iter_retry(struct radix_tree_iter *iter)
{
	iter->next_index = iter->index;
	iter->tags = 0;
	return NULL;
}

static inline unsigned long
__radix_tree_iter_add(struct radix_tree_iter *iter, unsigned long slots)
{
	return iter->index + (slots << iter_shift(iter));
}

void __rcu **__must_check radix_tree_iter_resume(void __rcu **slot,
					struct radix_tree_iter *iter);

static __always_inline long
radix_tree_chunk_size(struct radix_tree_iter *iter)
{
	return (iter->next_index - iter->index) >> iter_shift(iter);
}

#ifdef CONFIG_RADIX_TREE_MULTIORDER
void __rcu **__radix_tree_next_slot(void __rcu **slot,
				struct radix_tree_iter *iter, unsigned flags);
#else
/* Can't happen without sibling entries, but the compiler can't tell that */
static inline void __rcu **__radix_tree_next_slot(void __rcu **slot,
				struct radix_tree_iter *iter, unsigned flags)
{
	return slot;
}
#endif

static __always_inline void __rcu **radix_tree_next_slot(void __rcu **slot,
				struct radix_tree_iter *iter, unsigned flags)
{
	if (flags & RADIX_TREE_ITER_TAGGED) {
		iter->tags >>= 1;
		if (unlikely(!iter->tags))
			return NULL;
		if (likely(iter->tags & 1ul)) {
			iter->index = __radix_tree_iter_add(iter, 1);
			slot++;
			goto found;
		}
		if (!(flags & RADIX_TREE_ITER_CONTIG)) {
			unsigned offset = __ffs(iter->tags);

			iter->tags >>= offset++;
			iter->index = __radix_tree_iter_add(iter, offset);
			slot += offset;
			goto found;
		}
	} else {
		long count = radix_tree_chunk_size(iter);

		while (--count > 0) {
			slot++;
			iter->index = __radix_tree_iter_add(iter, 1);

			if (likely(*slot))
				goto found;
			if (flags & RADIX_TREE_ITER_CONTIG) {
				/* forbid switching to the next chunk */
				iter->next_index = 0;
				break;
			}
		}
	}
	return NULL;

 found:
	if (unlikely(radix_tree_is_internal_node(rcu_dereference_raw(*slot))))
		return __radix_tree_next_slot(slot, iter, flags);
	return slot;
}


#define radix_tree_for_each_slot(slot, root, iter, start)		\
	for (slot = radix_tree_iter_init(iter, start) ;			\
	     slot || (slot = radix_tree_next_chunk(root, iter, 0)) ;	\
	     slot = radix_tree_next_slot(slot, iter, 0))



#endif	/* _LINUX_RADIX_TREE_H_ */
