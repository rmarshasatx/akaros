/* See COPYRIGHT for copyright information. */
#ifdef __DEPUTY__
#pragma nodeputy
#endif

#include <arch/types.h>
#include <arch/arch.h>
#include <arch/mmu.h>
#include <arch/console.h>
#include <ros/timer.h>
#include <ros/error.h>

#include <string.h>
#include <assert.h>
#include <env.h>
#include <pmap.h>
#include <trap.h>
#include <syscall.h>

//Do absolutely nothing.  Used for profiling.
static void sys_null(void)
{
	return;
}

//Write a buffer over the serial port
static ssize_t sys_serial_write(env_t* e, const char *DANGEROUS buf, size_t len) 
{
	#ifdef SERIAL_IO
		char *COUNT(len) _buf = user_mem_assert(e, buf, len, PTE_USER_RO);
		for(int i =0; i<len; i++)
			serial_send_byte(buf[i]);	
		return (ssize_t)len;
	#else
		return -EINVAL;
	#endif
}

//Read a buffer over the serial port
static ssize_t sys_serial_read(env_t* e, char *DANGEROUS buf, size_t len) 
{
	#ifdef SERIAL_IO
	    char *COUNT(len) _buf = user_mem_assert(e, buf, len, PTE_USER_RO);
		size_t bytes_read = 0;
		int c;
		while((c = serial_read_byte()) != -1) {
			buf[bytes_read++] = (uint8_t)c;
			if(bytes_read == len) break;
		}
		return (ssize_t)bytes_read;
	#else
		return -EINVAL;
	#endif
}

// Invalidate the cache of this core
static void sys_cache_invalidate(void)
{
	// why is this necessary with cache coherence?
	// is it for coherence with respect to i/o?  --asw

	#ifdef __i386__
		wbinvd();
	#endif
	return;
}

// Writes 'val' to 'num_writes' entries of the well-known array in the kernel
// address space.  It's just #defined to be some random 4MB chunk (which ought
// to be boot_alloced or something).  Meant to grab exclusive access to cache
// lines, to simulate doing something useful.
static void sys_cache_buster(env_t* e, uint32_t num_writes, uint32_t num_pages,
                             uint32_t flags)
{
	#define BUSTER_ADDR		0xd0000000  // around 512 MB deep
	#define MAX_WRITES		1048576*8
	#define MAX_PAGES		32
	#define INSERT_ADDR 	(UINFO + 2*PGSIZE) // should be free for these tests
	uint32_t* buster = (uint32_t*)BUSTER_ADDR;
	static uint32_t buster_lock = 0;
	uint64_t ticks = -1;
	page_t* a_page[MAX_PAGES];

	/* Strided Accesses or Not (adjust to step by cachelines) */
	uint32_t stride = 1;
	if (flags & BUSTER_STRIDED) {
		stride = 16;
		num_writes *= 16;
	}
	
	/* Shared Accesses or Not (adjust to use per-core regions)
	 * Careful, since this gives 8MB to each core, starting around 512MB.
	 * Also, doesn't separate memory for core 0 if it's an async call.
	 */
	if (!(flags & BUSTER_SHARED))
		buster = (uint32_t*)(BUSTER_ADDR + core_id() * 0x00800000);

	/* Start the timer, if we're asked to print this info*/
	if (flags & BUSTER_PRINT_TICKS)
		ticks = start_timing();

	/* Allocate num_pages (up to MAX_PAGES), to simulate doing some more
	 * realistic work.  Note we don't write to these pages, even if we pick
	 * unshared.  Mostly due to the inconvenience of having to match up the
	 * number of pages with the number of writes.  And it's unnecessary.
	 */
	if (num_pages) {
		spin_lock(&buster_lock);
		for (int i = 0; i < MIN(num_pages, MAX_PAGES); i++) {
			page_alloc(&a_page[i]);
			page_insert(e->env_pgdir, a_page[i], (void*)INSERT_ADDR + PGSIZE*i,
			            PTE_USER_RW);
		}
		spin_unlock(&buster_lock);
	}

	if (flags & BUSTER_LOCKED)
		spin_lock(&buster_lock);
	for (int i = 0; i < MIN(num_writes, MAX_WRITES); i=i+stride)
		buster[i] = 0xdeadbeef;
	if (flags & BUSTER_LOCKED)
		spin_unlock(&buster_lock);

	if (num_pages) {
		spin_lock(&buster_lock);
		for (int i = 0; i < MIN(num_pages, MAX_PAGES); i++) {
			page_remove(e->env_pgdir, (void*)(INSERT_ADDR + PGSIZE * i));
			page_decref(a_page[i]);
		}
		spin_unlock(&buster_lock);
	}

	/* Print info */
	if (flags & BUSTER_PRINT_TICKS) {
		ticks = stop_timing(ticks);
		printk("%llu,", ticks);
	}
	return;
}

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static ssize_t sys_cputs(env_t* e, const char *DANGEROUS s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.
    char *COUNT(len) _s = user_mem_assert(e, s, len, PTE_USER_RO);

	// Print the string supplied by the user.
	printk("%.*s", len, _s);
	return (ssize_t)len;
}

// Read a character from the system console.
// Returns the character.
static uint16_t sys_cgetc(env_t* e)
{
	uint16_t c;

	// The cons_getc() primitive doesn't wait for a character,
	// but the sys_cgetc() system call does.
	while ((c = cons_getc()) == 0)
		cpu_relax();

	return c;
}

// Returns the current environment's envid.
static envid_t sys_getenvid(env_t* e)
{
	return e->env_id;
}

// Returns the id of the cpu this syscall is executed on.
static envid_t sys_getcpuid(void)
{
	return core_id();
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-EBADENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static error_t sys_env_destroy(env_t* e, envid_t envid)
{
	int r;
	env_t *env_to_die;

	if ((r = envid2env(envid, &env_to_die, 1)) < 0)
		return r;
	if (env_to_die == e)
		printk("[%08x] exiting gracefully\n", e->env_id);
	else
		printk("[%08x] destroying %08x\n", e->env_id, env_to_die->env_id);
	env_destroy(env_to_die);
	return 0;
}

/*
 * Current process yields its remaining "time slice".  Currently works for
 * single-core processes.
 */
static void sys_yield(env_t *e)
{
	// TODO: watch for races throughout anything related to process statuses
	// and schedule/yielding
	assert(e->env_status == ENV_RUNNING);
	e->env_status = ENV_RUNNABLE;
	schedule();
}

// TODO: Build a dispatch table instead of switching on the syscallno
// Dispatches to the correct kernel function, passing the arguments.
intreg_t syscall(env_t* e, uint32_t syscallno, uint32_t a1, uint32_t a2,
                uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.

	//cprintf("Incoming syscall number: %d\n    a1: %x\n   "
	//        " a2: %x\n    a3: %x\n    a4: %x\n    a5: %x\n", 
	//        syscallno, a1, a2, a3, a4, a5);

	assert(e); // should always have an env for every syscall
	//printk("Running syscall: %d\n", syscallno);
	if (INVALID_SYSCALL(syscallno))
		return -EINVAL;

	switch (syscallno) {
		case SYS_null:
			sys_null();
			return 0;
		case SYS_cache_buster:
			sys_cache_buster(e, a1, a2, a3);
			return 0;
		case SYS_cache_invalidate:
			sys_cache_invalidate();
			return 0;
		case SYS_cputs:
			return sys_cputs(e, (char *DANGEROUS)a1, (size_t)a2);
		case SYS_cgetc:
			return sys_cgetc(e);
		case SYS_getcpuid:
			return sys_getcpuid();
		case SYS_serial_write:
			return sys_serial_write(e, (char *DANGEROUS)a1, (size_t)a2);
		case SYS_serial_read:
			return sys_serial_read(e, (char *DANGEROUS)a1, (size_t)a2);
		case SYS_getenvid:
			return sys_getenvid(e);
		case SYS_env_destroy:
			return sys_env_destroy(e, (envid_t)a1);
		case SYS_yield:
			sys_yield(e);
			return 0;
		case SYS_proc_create:
		case SYS_proc_run:
			panic("Not implemented");
		default:
			// or just return -EINVAL
			panic("Invalid syscall number %d for env %x!", syscallno, *e);
	}
	return 0xdeadbeef;
}

intreg_t syscall_async(env_t* e, syscall_req_t *call)
{
	return syscall(e, call->num, call->args[0], call->args[1],
	               call->args[2], call->args[3], call->args[4]);
}

intreg_t process_generic_syscalls(env_t* e, size_t max)
{
	size_t count = 0;
	syscall_back_ring_t* sysbr = &e->env_sysbackring;

	// make sure the env is still alive.  incref will return 0 on success.
	if (env_incref(e))
		return -1;

	// max is the most we'll process.  max = 0 means do as many as possible
	while (RING_HAS_UNCONSUMED_REQUESTS(sysbr) && ((!max)||(count < max)) ) {
		if (!count) {
			// ASSUME: one queue per process
			// only switch cr3 for the very first request for this queue
			// need to switch to the right context, so we can handle the user pointer
			// that points to a data payload of the syscall
			lcr3(e->env_cr3);
		}
		count++;
		//printk("DEBUG PRE: sring->req_prod: %d, sring->rsp_prod: %d\n",\
			   sysbr->sring->req_prod, sysbr->sring->rsp_prod);
		// might want to think about 0-ing this out, if we aren't
		// going to explicitly fill in all fields
		syscall_rsp_t rsp;
		// this assumes we get our answer immediately for the syscall.
		syscall_req_t* req = RING_GET_REQUEST(sysbr, ++(sysbr->req_cons));
		rsp.retval = syscall_async(e, req);
		// write response into the slot it came from
		memcpy(req, &rsp, sizeof(syscall_rsp_t));
		// update our counter for what we've produced (assumes we went in order!)
		(sysbr->rsp_prod_pvt)++;
		RING_PUSH_RESPONSES(sysbr);
		//printk("DEBUG POST: sring->req_prod: %d, sring->rsp_prod: %d\n",\
			   sysbr->sring->req_prod, sysbr->sring->rsp_prod);
	}
	env_decref(e);
	return (intreg_t)count;
}
