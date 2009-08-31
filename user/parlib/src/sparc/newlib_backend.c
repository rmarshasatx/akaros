/* See COPYRIGHT for copyright information. */
/* Andrew Waterman <waterman@eecs.bekeley.edu> */

#include <arch/frontend.h>
#include <parlib.h>
#include <sys/unistd.h>

char *__env[1] = { 0 };
char **environ = __env;

#define IS_CONSOLE(fd) ((uint32_t)(fd) < 3)

int
getpid(void)
{
	static int pid = 0;
	if(pid == 0)
		return pid = sys_getpid();

	return pid;
}

void
_exit(int code)
{
	sys_proc_destroy(getpid());
	while(1);
}

int
isatty(int fd)
{
	return IS_CONSOLE(fd);
}

int
fork(void)
{
	return -1;
}

int
execve(char* name, char** argv, char** env)
{
	return -1;
}

int
kill(int pid, int sig)
{
	return -1;
}

int
wait(int* status)
{
	return -1;
}

int
link(char* old, char* new)
{
	return -1;
}

int
unlink(char* old)
{
	return syscall(SYS_frontend,RAMP_SYSCALL_unlink,(int)old,0,0,0);
}

int
fstat(int fd, struct stat* st)
{
	return syscall(SYS_frontend,RAMP_SYSCALL_fstat,fd,(int)st,0,0);
}

int
stat(char* name, struct stat* st)
{
	return syscall(SYS_frontend,RAMP_SYSCALL_stat,(int)name,(int)st,0,0);
}

off_t
lseek(int fd, off_t ptr, int dir)
{
	return syscall(SYS_frontend,RAMP_SYSCALL_lseek,fd,ptr,dir,0);
}

size_t
write(int fd, void* ptr, size_t len)
{
	return syscall(SYS_frontend,RAMP_SYSCALL_write,fd,(int)ptr,len,0);
}

size_t
read(int fd, void* ptr, size_t len)
{
	return syscall(SYS_frontend,RAMP_SYSCALL_read,fd,(int)ptr,len,0);
}

int
open(char* name, int flags, int mode)
{
	return syscall(SYS_frontend,RAMP_SYSCALL_open,(int)name,flags,mode,0);
}

int
close(int fd)
{
	if(IS_CONSOLE(fd))
		return 0;
	return syscall(SYS_frontend,RAMP_SYSCALL_close,fd,0,0,0);
}

int
times(struct tms* buf)
{
	return -1;
}

int
gettimeofday(struct timeval* tp, void* tzp)
{
	return -1;
}

void*SNT
sbrk(ptrdiff_t incr)
{
	#define HEAP_SIZE 8192
	static uint8_t array[HEAP_SIZE];
	static uint8_t*SNT heap_end = array;
	static uint8_t*SNT stack_ptr = &(array[HEAP_SIZE-1]);

	uint8_t*SNT prev_heap_end;

	prev_heap_end = heap_end;
	if (heap_end + incr > stack_ptr) {
		errno = ENOMEM;
		return (void*SNT)-1;
	}

	heap_end += incr;
	return (caddr_t SNT) prev_heap_end;
}