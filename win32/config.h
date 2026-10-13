/* Hand-written config.h for a native Windows (MinGW-w64 / UCRT, zig cc) build
 * of rsync.  There is no ./configure run: this replaces its output.  Anything
 * POSIX-only that rsync needs is supplied by win32compat.h / win32compat.c,
 * which this file pulls in at the end. */

#ifndef RSYNC_WIN32_CONFIG_H
#define RSYNC_WIN32_CONFIG_H

#define PACKAGE_NAME "rsync"
#define PACKAGE_TARNAME "rsync"
#define PACKAGE_VERSION RSYNC_WIN32_VERSION
#define PACKAGE_STRING "rsync " RSYNC_WIN32_VERSION
#define PACKAGE_BUGREPORT "https://rsync.samba.org/bug-tracking.html"
#define PACKAGE_URL ""
#define PACKAGE "rsync"

#define STDC_HEADERS 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_FCNTL_H 1
#define HAVE_CTYPE_H 1
#define HAVE_LIMITS_H 1
#define HAVE_FLOAT_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MALLOC_H 1
#define HAVE_UTIME_H 1
#define HAVE_DIRENT_H 1   /* our own, see win32/include/dirent.h */

#define HAVE_LONG_DOUBLE 1
#ifndef _MSC_VER   /* MSVC ABI: long double == double */
#define HAVE_LONG_DOUBLE_WIDER 1
#endif
#define HAVE_MODE_T 1
#define HAVE_OFF_T 1
#define HAVE_SIZE_T 1
#define HAVE_PID_T 1
#define HAVE_ID_T 1
#define HAVE_VA_COPY 1
#define HAVE_C99_VSNPRINTF 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1


#define HAVE_MEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRPBRK 1
#define HAVE_STRTOL 1
#define HAVE_STRERROR 1
#define HAVE_STRFTIME 1
#define HAVE_STRCASECMP 1
#define HAVE_MKTIME 1
#define HAVE_GETCWD 1
#define HAVE_FSTAT 1
#define HAVE_FTRUNCATE 1
#define HAVE_CHMOD 1
#define HAVE_PUTENV 1
#define HAVE_SETENV 1
#define HAVE_UNSETENV 1
#define HAVE_SETLOCALE 1
#define HAVE_SETVBUF 1
#define HAVE_SETMODE 1
#define HAVE_USLEEP 1
#define HAVE_NANOSLEEP 1
#define HAVE_GETTIMEOFDAY_TZ 1
#define HAVE_UTIME 1
#define HAVE_UTIMES 1
#define HAVE_STRUCT_UTIMBUF 1
#define HAVE_UTIME_NULL 1
#define HAVE_GETPASS 1
#define HAVE_GETEUID 1
#define HAVE_GETEGID 1
#define HAVE_WAITPID 1
#define HAVE_SIGACTION 1
#define HAVE_READLINK 1   /* symlinks: Windows reparse points, see win32compat.c */
#define HAVE_LINK 1       /* hard links: CreateHardLinkW */
#define HAVE_STRUCT_STAT_ST_MTIMENSEC 1

/* Networking (winsock2) */
#define HAVE_CONNECT 1
#define HAVE_GETADDRINFO 1
#define HAVE_STRUCT_ADDRINFO 1
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_SOCKADDR_IN6_SCOPE_ID 1
#define HAVE_INET_NTOP 1
#define HAVE_INET_PTON 1
#define HAVE_SOCKETPAIR 1  /* emulated with a loopback TCP pair */
#define INET6 1

#define SIZEOF_CHARP 8
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF_SHORT 2
#define SIZEOF_INT16_T 2
#define SIZEOF_UINT16_T 2
#define SIZEOF_INT32_T 4
#define SIZEOF_UINT32_T 4
#define SIZEOF_INT64_T 8
#define SIZEOF_OFF_T 8
#define SIZEOF_OFF64_T 8
#define SIZEOF_TIME_T 8
#define SIGNED_CHAR_OK 1
#define _FILE_OFFSET_BITS 64

/* Bundled/vendored compression + hashing libs */
#define SUPPORT_XXHASH 1
#define HAVE_XXHASH_H 1
#define SUPPORT_LZ4 1
#define HAVE_LZ4_H 1
#define SUPPORT_ZSTD 1
#define HAVE_ZSTD_H 1

#ifdef __x86_64__
#define USE_ROLL_SIMD 1  /* simd-checksum-x86_64.cpp; ARM64 uses plain C */
#ifndef RSYNC_W32_NO_ASM /* build-win.ps1 -NoAsm */
/* Hand-written asm, made Win64-ABI-safe by the shims in patches.ps1 and
 * checked by win32\test\abi_check.c during the build. */
#define USE_ROLL_ASM 1   /* simd-checksum-avx2.S (AVX2 CPUs only, runtime-checked) */
#define USE_MD5_ASM 1    /* lib/md5-asm-x86_64.S */
#endif
#endif

/* No: ACLs, xattrs, iconv, IDN, OpenSSL, daemon-side privilege handling. */

#define RSYNC_PATH "rsync"
#define RSYNC_RSH "ssh"
#define RSYNCD_SYSCONF "/etc/rsyncd.conf"
#define POPT_SYSCONFDIR "/etc"
#define NOBODY_USER "nobody"
#define NOBODY_GROUP "nogroup"
#define UTF8_CHARSET "UTF-8"
#define GETGROUPS_T int
#define ICONV_CONST

#ifndef __ASSEMBLER__   /* the .S files include config.h too */
#include "win32compat.h"
#endif

#endif
