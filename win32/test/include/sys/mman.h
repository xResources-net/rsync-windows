/* Test-only: just enough mmap()/mprotect() for simd-checksum-x86_64.cpp's
 * TEST_SIMD_CHECKSUM1 guard-page over-read check (build-win.ps1 -Test). */
#ifndef W32_TEST_SYS_MMAN_H
#define W32_TEST_SYS_MMAN_H

#include <windows.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)
#define _SC_PAGESIZE 30

static inline long sysconf(int name)
{
	SYSTEM_INFO si;
	(void)name;
	GetSystemInfo(&si);
	return (long)si.dwPageSize;
}

static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
	void *p;
	(void)addr; (void)prot; (void)flags; (void)fd; (void)off;
	p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	return p ? p : MAP_FAILED;
}

static inline int mprotect(void *addr, size_t len, int prot)
{
	DWORD old;
	return VirtualProtect(addr, len, prot == PROT_NONE ? PAGE_NOACCESS : PAGE_READWRITE, &old) ? 0 : -1;
}

static inline int munmap(void *addr, size_t len)
{
	(void)len;
	return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

#endif
