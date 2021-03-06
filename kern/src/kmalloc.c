/* Copyright (c) 2009 The Regents of the University of California. 
 * See the COPYRIGHT files at the top of this source tree for full 
 * license information.
 * 
 * Barret Rhoden <brho@cs.berkeley.edu>
 * Kevin Klues <klueska@cs.berkeley.edu>    
 */

#ifdef __SHARC__
#pragma nosharc
#define SINIT(x) x
#endif

#ifdef __DEPUTY__
#pragma nodeputy
#endif

#include <ros/common.h>
#include <error.h>
#include <pmap.h>
#include <kmalloc.h>
#include <stdio.h>
#include <slab.h>
#include <assert.h>

#define kmallocdebug(args...)  //printk(args)

//List of physical pages used by kmalloc
static spinlock_t pages_list_lock = SPINLOCK_INITIALIZER;
static page_list_t LCKD(&pages_list_lock)pages_list;

struct kmem_cache *kmalloc_caches[NUM_KMALLOC_CACHES];
void kmalloc_init(void)
{
	// i want to know if we ever make the tag bigger (should be below 16 bytes)
	static_assert(sizeof(struct kmalloc_tag) <= KMALLOC_ALIGNMENT);
	// build caches of common sizes
	size_t ksize = KMALLOC_SMALLEST;
	for (int i = 0; i < NUM_KMALLOC_CACHES; i++) {
		kmalloc_caches[i] = kmem_cache_create("kmalloc_cache", ksize,
		                                      KMALLOC_ALIGNMENT, 0, 0, 0);
		ksize <<= 1;
	}
}

void *kmalloc(size_t size, int flags) 
{
	// reserve space for bookkeeping and preserve alignment
	size_t ksize = size + KMALLOC_OFFSET;
	void *buf;
	int cache_id;
	// determine cache to pull from
	if (ksize <= KMALLOC_SMALLEST)
		cache_id = 0;
	else
		cache_id = LOG2_UP(ksize) - LOG2_UP(KMALLOC_SMALLEST);
	// if we don't have a cache to handle it, alloc cont pages
	if (cache_id >= NUM_KMALLOC_CACHES) {
		size_t num_pgs = ROUNDUP(size + sizeof(struct kmalloc_tag), PGSIZE) /
		                           PGSIZE;
		buf = get_cont_pages(LOG2_UP(num_pgs), flags);
		if (!buf)
			panic("Kmalloc failed!  Handle me!");
		// fill in the kmalloc tag
		struct kmalloc_tag *tag = buf;
		tag->flags = KMALLOC_TAG_PAGES;
		tag->num_pages = num_pgs;
		tag->canary = KMALLOC_CANARY;
		return buf + KMALLOC_OFFSET;
	}
	// else, alloc from the appropriate cache
	buf = kmem_cache_alloc(kmalloc_caches[cache_id], flags);
	if (!buf)
		panic("Kmalloc failed!  Handle me!");
	// store a pointer to the buffers kmem_cache in it's bookkeeping space
	struct kmalloc_tag *tag = buf;
	tag->flags = KMALLOC_TAG_CACHE;
	tag->my_cache = kmalloc_caches[cache_id];
	tag->canary = KMALLOC_CANARY;
	return buf + KMALLOC_OFFSET;
}

void *kzmalloc(size_t size, int flags) 
{
	void *v = kmalloc(size, flags);
	if (! v)
		return v;
	memset(v, 0, size);
	return v;
}

void *krealloc(void* buf, size_t size, int flags) {
	struct kmalloc_tag *tag = (struct kmalloc_tag*)(buf - KMALLOC_OFFSET);
	if (tag->my_cache->obj_size >= size)
		return buf;
	kfree(buf);
	return kmalloc(size, flags);
}

void kfree(void *addr)
{
	if(addr == NULL)
		return;
	struct kmalloc_tag *tag = (struct kmalloc_tag*)(addr - KMALLOC_OFFSET);
	assert(tag->canary == KMALLOC_CANARY);
	if (tag->flags & KMALLOC_TAG_CACHE)
		kmem_cache_free(tag->my_cache, addr - KMALLOC_OFFSET);
	else if (tag->flags & KMALLOC_TAG_PAGES) {
		free_cont_pages(addr - KMALLOC_OFFSET, LOG2_UP(tag->num_pages));
	} else 
		panic("[Italian Accent]: Che Cazzo! BO! Flag in kmalloc!!!");
}

