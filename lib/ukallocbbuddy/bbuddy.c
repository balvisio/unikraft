/* SPDX-License-Identifier: MIT */
/*
 * MIT License
 ****************************************************************************
 * (C) 2003 - Rolf Neugebauer - Intel Research Cambridge
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 * (C) 2017 - Simon Kuenzer - NEC Europe Ltd.
 ****************************************************************************
 *
 *        File: mm.c
 *      Author: Rolf Neugebauer (neugebar@dcs.gla.ac.uk)
 *     Changes: Grzegorz Milos
 *     Changes: Simon Kuenzer <simon.kuenzer@neclab.eu>
 *
 *        Date: Aug 2003, changes Aug 2005, changes Oct 2017
 *
 * Environment: Unikraft
 * Description: buddy page allocator from Xen.
 *
 ****************************************************************************
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#include <uk/allocbbuddy.h>
#include <uk/arch/limits.h>
#include <uk/print.h>
#include <uk/assert.h>

typedef struct chunk_head_st chunk_head_t;
typedef struct chunk_tail_st chunk_tail_t;

struct chunk_head_st {
	chunk_head_t *next;
	chunk_head_t **pprev;
	unsigned int level;
};

struct chunk_tail_st {
	unsigned int level;
};

/* Linked lists of free chunks of different powers-of-two in size. */
#define FREELIST_SIZE ((sizeof(void *) << 3) - __PAGE_SHIFT)
#define FREELIST_EMPTY(_l) ((_l)->next == NULL)

/* keep a bitmap for each memory region separately */
struct uk_bbpalloc_memr {
	struct uk_bbpalloc_memr *next;
	unsigned long first_page;
	unsigned long nr_pages;
	unsigned long mm_alloc_bitmap_size;
	unsigned long mm_alloc_bitmap[];
};

struct uk_bbpalloc {
	unsigned long nr_free_pages;
	chunk_head_t *free_head[FREELIST_SIZE];
	chunk_head_t free_tail[FREELIST_SIZE];
	struct uk_bbpalloc_memr *memr_head;
};

#define round_pgup(a)   ALIGN_UP((a), __PAGE_SIZE)
#define round_pgdown(a) ALIGN_DOWN((a), __PAGE_SIZE)

/*********************
 * ALLOCATION BITMAP
 *  One bit per page of memory. Bit set => page is allocated.
 *
 * Hint regarding bitwise arithmetic in map_{alloc,free}:
 *  -(1<<n)  sets all bits >= n.
 *  (1<<n)-1 sets all bits <  n.
 * Variable names in map_{alloc,free}:
 *  *_idx == Index into `mm_alloc_bitmap' array.
 *  *_off == Bit offset within an element of the `mm_alloc_bitmap' array.
 */
#define PAGES_PER_MAPWORD (sizeof(unsigned long) * 8)

static inline struct uk_bbpalloc_memr *map_get_memr(struct uk_bbpalloc *b,
						    unsigned long page_num)
{
	struct uk_bbpalloc_memr *memr = NULL;

	/*
	 * Find bitmap of according memory region
	 * This is a linear search but it is expected that we have only a few
	 * of them. It should be just one region in most cases
	 */
	for (memr = b->memr_head; memr != NULL; memr = memr->next) {
		if ((page_num >= memr->first_page)
		    && (page_num < (memr->first_page + memr->nr_pages << __PAGE_SIZE)))
			return memr;
	}

	/*
	 * No region found
	 */
	return NULL;
}

static inline int allocated_in_map(struct uk_bbpalloc *b,
				   unsigned long page_num)
{
	struct uk_bbpalloc_memr *memr = map_get_memr(b, page_num);

	/* treat pages outside of region as allocated */
	if (!memr)
		return 1;

	page_num -= memr->first_page;
	return ((memr)->mm_alloc_bitmap[(page_num) / PAGES_PER_MAPWORD]
		& (1UL << ((page_num) & (PAGES_PER_MAPWORD - 1))));
}

static void map_alloc(struct uk_bbpalloc *b, uintptr_t first_page,
		      unsigned long nr_pages)
{
	struct uk_bbpalloc_memr *memr;
	unsigned long start_off, end_off, curr_idx, end_idx;

	/*
	 * In case there was no memory region found, the allocator
	 * is in a really bad state. It means that the specified page
	 * region is not covered by our allocator.
	 */
	memr = map_get_memr(b, first_page);
	UK_ASSERT(memr != NULL);
	UK_ASSERT((first_page + nr_pages)
		  <= (memr->first_page + memr->nr_pages << __PAGE_SIZE));

	first_page -= memr->first_page;
	curr_idx = first_page / PAGES_PER_MAPWORD;
	start_off = first_page & (PAGES_PER_MAPWORD - 1);
	end_idx = (first_page + nr_pages) / PAGES_PER_MAPWORD;
	end_off = (first_page + nr_pages) & (PAGES_PER_MAPWORD - 1);

	if (curr_idx == end_idx) {
		memr->mm_alloc_bitmap[curr_idx] |=
		    ((1UL << end_off) - 1) & -(1UL << start_off);
	} else {
		memr->mm_alloc_bitmap[curr_idx] |= -(1UL << start_off);
		while (++curr_idx < end_idx)
			memr->mm_alloc_bitmap[curr_idx] = ~0UL;
		memr->mm_alloc_bitmap[curr_idx] |= (1UL << end_off) - 1;
	}

	b->nr_free_pages -= nr_pages;
}

static void map_free(struct uk_bbpalloc *b, uintptr_t first_page,
		     unsigned long nr_pages)
{
	struct uk_bbpalloc_memr *memr;
	unsigned long start_off, end_off, curr_idx, end_idx;

	/*
	 * In case there was no memory region found, the allocator
	 * is in a really bad state. It means that the specified page
	 * region is not covered by our allocator.
	 */
	memr = map_get_memr(b, first_page);
	UK_ASSERT(memr != NULL);
	UK_ASSERT((first_page + nr_pages)
		  <= (memr->first_page + memr->nr_pages));

	first_page -= memr->first_page;
	curr_idx = first_page / PAGES_PER_MAPWORD;
	start_off = first_page & (PAGES_PER_MAPWORD - 1);
	end_idx = (first_page + nr_pages) / PAGES_PER_MAPWORD;
	end_off = (first_page + nr_pages) & (PAGES_PER_MAPWORD - 1);

	if (curr_idx == end_idx) {
		memr->mm_alloc_bitmap[curr_idx] &=
		    -(1UL << end_off) | ((1UL << start_off) - 1);
	} else {
		memr->mm_alloc_bitmap[curr_idx] &= (1UL << start_off) - 1;
		while (++curr_idx != end_idx)
			memr->mm_alloc_bitmap[curr_idx] = 0;
		memr->mm_alloc_bitmap[curr_idx] &= -(1UL << end_off);
	}

	b->nr_free_pages += nr_pages;
}

#if LIBUKALLOC_IFSTATS
static ssize_t bbuddy_availmem(struct uk_alloc *a)
{
	struct uk_bbpalloc *b;

	UK_ASSERT(a != NULL);
	b = (struct uk_bbpalloc *)&a->private;
	return (ssize_t) b->nr_free_pages << __PAGE_SHIFT;
}
#endif

/*********************
 * BINARY BUDDY PAGE ALLOCATOR
 */
static void *bbuddy_palloc(struct uk_alloc *a, size_t order)
{
	struct uk_bbpalloc *b;
	size_t i;
	chunk_head_t *alloc_ch, *spare_ch;
	chunk_tail_t *spare_ct;

	UK_ASSERT(a != NULL);
	b = (struct uk_bbpalloc *)&a->private;

	/* Find smallest order which can satisfy the request. */
	for (i = order; i < FREELIST_SIZE; i++) {
		if (!FREELIST_EMPTY(b->free_head[i]))
			break;
	}
	if (i == FREELIST_SIZE)
		goto no_memory;

	/* Unlink a chunk. */
	alloc_ch = b->free_head[i];
	b->free_head[i] = alloc_ch->next;
	alloc_ch->next->pprev = alloc_ch->pprev;

	/* We may have to break the chunk a number of times. */
	while (i != order) {
		/* Split into two equal parts. */
		i--;
		spare_ch = (chunk_head_t *)((char *)alloc_ch
					    + (1UL << (i + __PAGE_SHIFT)));
		spare_ct = (chunk_tail_t *)((char *)spare_ch
					    + (1UL << (i + __PAGE_SHIFT))) - 1;

		/* Create new header for spare chunk. */
		spare_ch->level = i;
		spare_ch->next = b->free_head[i];
		spare_ch->pprev = &b->free_head[i];
		spare_ct->level = i;

		/* Link in the spare chunk. */
		spare_ch->next->pprev = &spare_ch->next;
		b->free_head[i] = spare_ch;
	}
	map_alloc(b, (uintptr_t)alloc_ch, 1UL << order);

	return ((void *)alloc_ch);

no_memory:
	uk_printd(DLVL_WARN, "%"__PRIuptr": Cannot handle palloc request of order %"__PRIsz": Out of memory\n",
		  (uintptr_t)a, order);
	errno = ENOMEM;
	return NULL;
}

static void bbuddy_pfree(struct uk_alloc *a, void *obj, size_t order)
{
	struct uk_bbpalloc *b;
	chunk_head_t *freed_ch, *to_merge_ch;
	chunk_tail_t *freed_ct;
	unsigned long mask;

	UK_ASSERT(a != NULL);
	b = (struct uk_bbpalloc *)&a->private;

	/* if the object is not page aligned it was clearly not from us */
	UK_ASSERT((((uintptr_t)obj) & (__PAGE_SIZE - 1)) == 0);

	/* First free the chunk */
	map_free(b, (uintptr_t)obj, 1UL << order);

	/* Create free chunk */
	freed_ch = (chunk_head_t *)obj;
	freed_ct = (chunk_tail_t *)((char *)obj
				    + (1UL << (order + __PAGE_SHIFT))) - 1;

	/* Now, possibly we can conseal chunks together */
	while (order < FREELIST_SIZE) {
		mask = 1UL << (order + __PAGE_SHIFT);
		if ((unsigned long)freed_ch & mask) {
			to_merge_ch = (chunk_head_t *)((char *)freed_ch - mask);
			if (allocated_in_map(b, (uintptr_t)to_merge_ch)
			    || to_merge_ch->level != order)
				break;

			/* Merge with predecessor */
			freed_ch = to_merge_ch;
		} else {
			to_merge_ch = (chunk_head_t *)((char *)freed_ch + mask);
			if (allocated_in_map(b, (uintptr_t)to_merge_ch)
			    || to_merge_ch->level != order)
				break;

			/* Merge with successor */
			freed_ct =
			    (chunk_tail_t *)((char *)to_merge_ch + mask) - 1;
		}

		/* We are commited to merging, unlink the chunk */
		*(to_merge_ch->pprev) = to_merge_ch->next;
		to_merge_ch->next->pprev = to_merge_ch->pprev;

		order++;
	}

	/* Link the new chunk */
	freed_ch->level = order;
	freed_ch->next = b->free_head[order];
	freed_ch->pprev = &b->free_head[order];
	freed_ct->level = order;

	freed_ch->next->pprev = &freed_ch->next;
	b->free_head[order] = freed_ch;
}

static int bbuddy_addmem(struct uk_alloc *a, void *base, size_t len)
{
	struct uk_bbpalloc *b;
	struct uk_bbpalloc_memr *memr;
	size_t memr_size;
	unsigned long count, i;
	chunk_head_t *ch;
	chunk_tail_t *ct;
	uintptr_t min, max, range;

	UK_ASSERT(a != NULL);
	UK_ASSERT(base != NULL);
	b = (struct uk_bbpalloc *)&a->private;

	min = round_pgup((uintptr_t)base);
	max = round_pgdown((uintptr_t)base + (uintptr_t)len);
	range = max - min;
	memr_size =
	    round_pgup(sizeof(*memr) + DIV_ROUND_UP(range >> __PAGE_SHIFT, 8));

	memr = (struct uk_bbpalloc_memr *)min;
	min += memr_size;
	range -= memr_size;
	if (max < min) {
		uk_printd(DLVL_ERR, "%"__PRIuptr": Failed to add memory region %"__PRIuptr"-%"__PRIuptr": Not enough space after applying page alignments\n",
			  (uintptr_t)a, (uintptr_t)base,
			  (uintptr_t)base + (uintptr_t)len);
		return -EINVAL;
	}

	/*
	 * Initialize region's bitmap
	 */
	memr->first_page = min;
	memr->nr_pages = (max - min) >> __PAGE_SIZE;

	/* add to list */
	memr->next = b->memr_head;
	b->memr_head = memr;
	/* free up the memory we've been given to play with */
	map_free(b, min, (unsigned long)(range >> __PAGE_SHIFT));

	count = 0;
	while (range != 0) {
		/*
		 * Next chunk is limited by alignment of min, but also
		 * must not be bigger than remaining range.
		 */
		for (i = __PAGE_SHIFT; (1UL << (i + 1)) <= range; i++)
			if (min & (1UL << i))
				break;

		uk_printd(DLVL_EXTRA, "%"__PRIuptr": Add allocate unit %"__PRIuptr" - %"__PRIuptr" (order %lu)\n",
			  (uintptr_t)a, min, (uintptr_t)(min + (1UL << i)),
			  (i - __PAGE_SHIFT));

		ch = (chunk_head_t *)min;
		min += 1UL << i;
		range -= 1UL << i;
		ct = (chunk_tail_t *)min - 1;
		i -= __PAGE_SHIFT;
		ch->level = i;
		ch->next = b->free_head[i];
		ch->pprev = &b->free_head[i];
		ch->next->pprev = &ch->next;
		b->free_head[i] = ch;
		ct->level = i;
		count++;
	}

	return 0;
}

struct uk_alloc *uk_allocbbuddy_init(void *base, size_t len)
{
	struct uk_alloc *a;
	struct uk_bbpalloc *b;
	size_t metalen;
	uintptr_t min, max;
	unsigned long i;

	min = round_pgup((uintptr_t)base);
	max = round_pgdown((uintptr_t)base + (uintptr_t)len);
	UK_ASSERT(max > min);

	/* Allocate space for allocator descriptor */
	metalen = round_pgup(sizeof(*a) + sizeof(*b));

	/* enough space for allocator available? */
	if (min + metalen > max) {
		uk_printd(DLVL_ERR, "Not enough space for allocator: %"__PRIsz" B required but only %"__PRIuptr" B usable\n",
			  metalen, (max - min));
		return NULL;
	}

	a = (struct uk_alloc *)min;
	uk_printd(DLVL_INFO, "Initialize binary buddy allocator %"__PRIuptr"\n",
		  (uintptr_t)a);
	min += metalen;
	memset(a, 0, metalen);
	b = (struct uk_bbpalloc *)&a->private;

	for (i = 0; i < FREELIST_SIZE; i++) {
		b->free_head[i] = &b->free_tail[i];
		b->free_tail[i].pprev = &b->free_head[i];
		b->free_tail[i].next = NULL;
	}
	b->memr_head = NULL;

	/* initialize and register allocator interface */
	uk_alloc_init_palloc(a, bbuddy_palloc, bbuddy_pfree,
			     bbuddy_addmem);
#if LIBUKALLOC_IFSTATS
	a->availmem = bbuddy_availmem;
#endif

	/* add left memory - ignore return value */
	bbuddy_addmem(a, (void *)(min + metalen),
		      (size_t)(max - min - metalen));
	return a;
}
