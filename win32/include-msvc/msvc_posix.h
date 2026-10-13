/* POSIX types and functions that mingw-w64's headers/CRT provide but the
 * MSVC C runtime does not (build-win.ps1 -Toolchain msvc).  Pulled in by the
 * stub <unistd.h>, <sys/time.h>, <strings.h> and <utime.h> in this directory. */
#ifndef RSYNC_MSVC_POSIX_H
#define RSYNC_MSVC_POSIX_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>   /* struct timeval; must precede windows.h */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif
#ifndef _PID_T_
#define _PID_T_
typedef int pid_t;
#endif
/* MSVC's off_t is a 32-bit long; rsync (config.h: SIZEOF_OFF_T 8) needs
 * 64-bit file offsets, as mingw gives with _FILE_OFFSET_BITS=64. */
#define off_t long long
#ifndef _USECONDS_T_DEFINED
#define _USECONDS_T_DEFINED
typedef unsigned int useconds_t;
#endif

#ifndef PATH_MAX
#define PATH_MAX 260   /* as mingw-w64's <limits.h> */
#endif

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};
int gettimeofday(struct timeval *tv, void *tz);
int usleep(useconds_t usec);
struct timespec;
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
