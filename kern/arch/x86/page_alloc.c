/* Copyright (c) 2009 The Regents of the University  of California. 
 * See the COPYRIGHT files at the top of this source tree for full 
 * license information.
 * 
 * Barret Rhoden <brho@cs.berkeley.edu>
 * Kevin Klues <klueska@cs.berkeley.edu> */

#ifdef __SHARC__
#pragma nosharc
#define SINIT(x) x
#endif

#include <sys/queue.h>
#include <page_alloc.h>
#include <pmap.h>
#include <kmalloc.h>
#include <multiboot.h>

spinlock_t colored_page_free_list_lock = SPINLOCK_INITIALIZER_IRQSAVE;

page_list_t LCKD(&colored_page_free_list_lock) * CT(llc_cache->num_colors) RO
  colored_page_free_list = NULL;

static void page_alloc_bootstrap() {
	// Allocate space for the array required to manage the free lists
	size_t list_size = llc_cache->num_colors*sizeof(page_list_t);
	page_list_t LCKD(&colored_page_free_list_lock)*tmp =
	    (page_list_t*)boot_alloc(list_size,PGSIZE);
	colored_page_free_list = SINIT(tmp);
	for (int i = 0; i < llc_cache->num_colors; i++)
		LIST_INIT(&colored_page_free_list[i]);
}

/* Can do whatever here.  For now, our page allocator just works with colors,
 * not NUMA zones or anything. */
static void track_free_page(struct page *page)
{
	LIST_INSERT_HEAD(&colored_page_free_list[get_page_color(page2ppn(page),
	                                                        llc_cache)],
	                 page, pg_link);
	nr_free_pages++;
}

static struct page *pa64_to_page(uint64_t paddr)
{
	return &pages[paddr >> PGSHIFT];
}

static bool pa64_is_in_kernel(uint64_t paddr)
{
	extern char end[];
	/* kernel is linked and loaded here (in kernel{32,64}.ld */
	return (EXTPHYSMEM <= paddr) && (paddr < PADDR(end));
}

/* Helper.  For every page in the entry, this will determine whether or not the
 * page is free, and handle accordingly. */
static void parse_mboot_region(struct multiboot_mmap_entry *entry, void *data)
{
	physaddr_t boot_freemem_paddr = (physaddr_t)data;
	bool in_bootzone = (entry->addr <= boot_freemem_paddr) &&
	                   (boot_freemem_paddr < entry->addr + entry->len);

	if (entry->type != MULTIBOOT_MEMORY_AVAILABLE)
		return;
	/* TODO: we'll have some issues with jumbo allocation */
	/* Most entries are page aligned, though on some machines below EXTPHYSMEM
	 * we may have some that aren't.  If two regions collide on the same page
	 * (one of them starts unaligned), we need to only handle the page once, and
	 * err on the side of being busy.
	 *
	 * Since these regions happen below EXTPHYSMEM, they are all marked busy (or
	 * else we'll panic).  I'll probably rewrite this for jumbos before I find a
	 * machine with unaligned mboot entries in higher memory. */
	if (PGOFF(entry->addr))
		assert(entry->addr < EXTPHYSMEM);
	for (uint64_t i = ROUNDDOWN(entry->addr, PGSIZE);
	     i < entry->addr + entry->len;
	     i += PGSIZE) {
		/* Skip pages we'll never map (above KERNBASE).  Once we hit one of
		 * them, we know the rest are too (for this entry). */
		if (i >= max_paddr)
			return;
		/* Mark low mem as busy (multiboot stuff is there, usually, too).  Since
		 * that memory may be freed later (like the smp_boot page), we'll treat
		 * it like it is busy/allocated. */
		if (i < EXTPHYSMEM)
			goto page_busy;
		/* Mark as busy pages already allocated in boot_alloc() */
		if (in_bootzone && (i < boot_freemem_paddr))
			goto page_busy;
		/* Need to double check for the kernel, in case it wasn't in the
		 * bootzone.  If it was in the bootzone, we already skipped it. */
		if (pa64_is_in_kernel(i))
			goto page_busy;
		track_free_page(pa64_to_page(i));
		continue;
page_busy:
		page_setref(pa64_to_page(i), 1);
	}
}

static void check_range(uint64_t start, uint64_t end, int expect)
{
	int ref;
	if (PGOFF(start))
		printk("Warning: check_range given an unaligned addr %p\n", start);
	for (uint64_t i = start; i < end; i += PGSIZE)  {
		ref = kref_refcnt(&pa64_to_page(i)->pg_kref);
		if (ref != expect) {
			printk("Error: physaddr %p refcnt was %d, expected %d\n", i, ref,
			       expect);
			panic("");
		}
	}
}

static void check_mboot_region(struct multiboot_mmap_entry *entry, void *data)
{
	extern char end[];
	physaddr_t boot_freemem_paddr = (physaddr_t)data;
	bool in_bootzone = (entry->addr <= boot_freemem_paddr) &&
	                   (boot_freemem_paddr < entry->addr + entry->len);
	/* Need to deal with 32b wrap-around */
	uint64_t zone_end = MIN(entry->addr + entry->len, (uint64_t)max_paddr);

	if (entry->type != MULTIBOOT_MEMORY_AVAILABLE)
		return;
	if (zone_end <= EXTPHYSMEM) {
		check_range(entry->addr, zone_end, 1);
		return;
	}
	/* this may include the kernel */
	if (in_bootzone) {
		/* boot_freemem might not be page aligned.  If it's part-way through a
		 * page, that page should be busy */
		check_range(entry->addr, ROUNDUP(PADDR(boot_freemem), PGSIZE), 1);
		check_range(ROUNDUP(PADDR(boot_freemem), PGSIZE), zone_end, 0);
		assert(zone_end == PADDR(boot_freelimit));
		return;
	}
	/* kernel's range (hardcoded in the linker script).  If we're checking now,
	 * it means the kernel is not in the same entry as the bootzone. */
	if (entry->addr == EXTPHYSMEM) {
		check_range(EXTPHYSMEM, PADDR(end), 1);
		check_range(ROUNDUP(PADDR(end), PGSIZE), zone_end, 0);
		return;
	}
}

/* Initialize the memory free lists.  After this, do not use boot_alloc. */
void page_alloc_init(struct multiboot_info *mbi)
{
	page_alloc_bootstrap();
	/* To init the free list(s), each page that is already allocated/busy will
	 * get increfed.  All other pages that were reported as 'free' will be added
	 * to a free list.  Their refcnts are all 0 (when pages was memset).
	 *
	 * To avoid a variety of headaches, any memory below 1MB is considered busy.
	 * Likewise, everything in the kernel, up to _end is also busy.  And
	 * everything we've already boot_alloc'd is busy.
	 *
	 * We'll also abort the mapping for any addresses over max_paddr, since
	 * we'll never use them.  'pages' does not track them either.
	 *
	 * One special note: we actually use the memory at 0x1000 for smp_boot.
	 * It'll get set to 'used' like the others; just FYI.
	 *
	 * Finally, if we want to use actual jumbo page allocation (not just
	 * mapping), we need to round up _end, and make sure all of multiboot's
	 * sections are jumbo-aligned. */
	physaddr_t boot_freemem_paddr = PADDR(PTRROUNDUP(boot_freemem, PGSIZE));

	mboot_foreach_mmap(mbi, parse_mboot_region, (void*)boot_freemem_paddr);
	printk("Number of free pages: %lu\n", nr_free_pages);
	/* Test the page alloc - if this gets slow, we can CONFIG it */
	mboot_foreach_mmap(mbi, check_mboot_region, (void*)boot_freemem_paddr);
	printk("Page alloc init successful\n");
}