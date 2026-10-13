/* <unistd.h> for the MSVC C runtime (build-win.ps1 -Toolchain msvc).
 * mingw-w64 ships this header; the MSVC/UCRT headers do not.  Only what the
 * mingw one provided and rsync relies on is here; everything POSIX-specific
 * beyond that lives in win32compat.h. */
#ifndef RSYNC_MSVC_UNISTD_H
#define RSYNC_MSVC_UNISTD_H

#include "msvc_posix.h"
#include <io.h>
#include <process.h>
#include <direct.h>

#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#endif
