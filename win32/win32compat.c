/* Native Windows implementation of the POSIX bits rsync needs.
 * See win32compat.h for the overall design. */

#include "win32compat.h"
#include <winioctl.h>
#include <aclapi.h>
#include <conio.h>
#include <stdarg.h>

/* This file calls the real CRT / winsock functions. */
#undef stat
#undef lstat
#undef fstat
#undef open
#undef read
#undef write
#undef close
#undef fcntl
#undef dup
#undef dup2
#undef pipe
#undef unlink
#undef rmdir
#undef mkdir
#undef rename
#undef chdir
#undef getcwd
#undef chmod
#undef fchmod
#undef access
#undef utime
#undef utimes
#undef link
#undef symlink
#undef readlink
#undef mkstemp
#undef realpath
#undef fopen
#undef socket
#undef connect
#undef bind
#undef listen
#undef accept
#undef shutdown
#undef setsockopt
#undef getsockopt
#undef getsockname
#undef getpeername
#undef select
#undef poll
#undef pollfd
#undef socketpair
#undef signal
#undef strerror
#undef opendir
#undef readdir
#undef closedir
#undef sleep

#define W32_MAXFD 8192

/* ===================================================================== */
/* errno mapping                                                          */
/* ===================================================================== */

static int errno_from_win(DWORD e)
{
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_INVALID_DRIVE:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
	case ERROR_CANT_RESOLVE_FILENAME:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_PRIVILEGE_NOT_HELD:
	case ERROR_CURRENT_DIRECTORY:
	case ERROR_WRITE_PROTECT:
		return EACCES;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return EEXIST;
	case ERROR_DIR_NOT_EMPTY:
		return ENOTEMPTY;
	case ERROR_NOT_SAME_DEVICE:
		return EXDEV;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return ENOSPC;
	case ERROR_DIRECTORY:
		return ENOTDIR;
	case ERROR_NOT_SUPPORTED:
	case ERROR_INVALID_FUNCTION:
		return ENOSYS;
	case ERROR_INVALID_HANDLE:
		return EBADF;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return ENOMEM;
	case ERROR_FILENAME_EXCED_RANGE:
		return ENAMETOOLONG;
	case ERROR_BROKEN_PIPE:
	case ERROR_NO_DATA:
		return EPIPE;
	case ERROR_TOO_MANY_LINKS:
		return EMLINK;
	case ERROR_CANT_ACCESS_FILE:
		return EACCES;
	default:
		return EINVAL;
	}
}

static int set_errno_win(void)
{
	errno = errno_from_win(GetLastError());
	return -1;
}

static int errno_from_wsa(int e)
{
	switch (e) {
	case WSAEWOULDBLOCK: return EAGAIN;
	case WSAEINTR: return EINTR;
	case WSAEINPROGRESS: return EINPROGRESS;
	case WSAEALREADY: return EALREADY;
	case WSAECONNRESET: return ECONNRESET;
	case WSAECONNABORTED: return ECONNABORTED;
	case WSAESHUTDOWN: return EPIPE;
	case WSAECONNREFUSED: return ECONNREFUSED;
	case WSAENOTCONN: return ENOTCONN;
	case WSAETIMEDOUT: return ETIMEDOUT;
	case WSAEHOSTUNREACH: return EHOSTUNREACH;
	case WSAENETUNREACH: return ENETUNREACH;
	case WSAEADDRINUSE: return EADDRINUSE;
	case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
	case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
	case WSAEISCONN: return EISCONN;
	case WSAENOTSOCK: return ENOTSOCK;
	case WSAEMSGSIZE: return EMSGSIZE;
	case WSAENOBUFS: return ENOBUFS;
	case WSAEINVAL: return EINVAL;
	case WSAEACCES: return EACCES;
	case WSAEFAULT: return EFAULT;
	case WSAEMFILE: return EMFILE;
	default: return EIO;
	}
}

static int set_errno_wsa(void)
{
	errno = errno_from_wsa(WSAGetLastError());
	return -1;
}

char *w32_strerror(int err)
{
	switch (err) {
	case EAGAIN: return "Resource temporarily unavailable";
	case ECONNRESET: return "Connection reset by peer";
	case ECONNABORTED: return "Software caused connection abort";
	case ECONNREFUSED: return "Connection refused";
	case ENOTCONN: return "Transport endpoint is not connected";
	case ETIMEDOUT: return "Connection timed out";
	case EHOSTUNREACH: return "No route to host";
	case ENETUNREACH: return "Network is unreachable";
	case EADDRINUSE: return "Address already in use";
	case EADDRNOTAVAIL: return "Cannot assign requested address";
	case EINPROGRESS: return "Operation now in progress";
	case ENOTSOCK: return "Socket operation on non-socket";
	case ENOTSUP: return "Operation not supported";
	case ELOOP: return "Too many levels of symbolic links";
	case ENOSYS: return "Function not implemented";
	case EPIPE: return "Broken pipe";
	default: return strerror(err);
	}
}

/* ===================================================================== */
/* UTF-8 <-> UTF-16 and path translation                                  */
/* ===================================================================== */

static wchar_t *utf8_to_wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w;
	if (n <= 0)
		n = 1;
	w = malloc(n * sizeof (wchar_t));
	if (!w)
		return NULL;
	if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
		w[0] = 0;
	return w;
}

static char *wide_to_utf8(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s;
	if (n <= 0)
		n = 1;
	s = malloc(n);
	if (!s)
		return NULL;
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
		s[0] = 0;
	return s;
}

/* Characters NTFS won't take in a name are stored the way Cygwin (and MSYS2)
 * store them: as U+F000 + the ASCII code (e.g. ':' -> U+F03A).  That keeps
 * names like "a:b" round-tripping, and compatible with trees Cygwin wrote. */
static int cyg_special(unsigned c)
{
	return (c >= 1 && c <= 31) || c == '"' || c == '*' || c == ':' || c == '<'
	    || c == '>' || c == '?' || c == '|';
}

static void cyg_unmap(wchar_t *w)
{
	for (; *w; w++) {
		if (*w > 0xF000 && *w < 0xF080 && cyg_special(*w - 0xF000))
			*w -= 0xF000;
	}
}

static void cyg_map(wchar_t *w)
{
	for (; *w; w++) {
		if (*w < 0x80 && cyg_special(*w))
			*w += 0xF000;
	}
}

static int is_cygdrive(const char *p)
{
	return strncmp(p, "/cygdrive/", 10) == 0 && isalpha((unsigned char)p[10])
	    && (p[11] == '/' || p[11] == '\0');
}

/* Our own current directory (plain "C:\dir" form, no trailing backslash
 * except for a drive root), kept so it can be deeper than MAX_PATH. */
static wchar_t *w32_cwd;

static const wchar_t *cur_dir(void)
{
	if (!w32_cwd) {
		DWORD n = GetCurrentDirectoryW(0, NULL);
		w32_cwd = malloc((n + 1) * sizeof (wchar_t));
		if (!w32_cwd || !GetCurrentDirectoryW(n + 1, w32_cwd)) {
			free(w32_cwd);
			w32_cwd = _wcsdup(L"C:\\");
		}
	}
	return w32_cwd;
}

/* Store FULL (any of C:\x, \\?\C:\x, \\?\UNC\srv\x) as the current dir. */
static void set_cur_dir(const wchar_t *full)
{
	wchar_t *c;
	size_t n;
	if (wcsncmp(full, L"\\\\?\\UNC\\", 8) == 0) {
		c = malloc((wcslen(full) + 1) * sizeof (wchar_t));
		wcscpy(c, L"\\");
		wcscat(c, full + 7);
	} else
		c = _wcsdup(wcsncmp(full, L"\\\\?\\", 4) == 0 ? full + 4 : full);
	n = wcslen(c);
	while (n > 3 && c[n - 1] == L'\\')
		c[--n] = 0;
	free(w32_cwd);
	w32_cwd = c;
	/* Best effort: the real one only matters for child processes, and
	 * can't follow us past MAX_PATH. */
	SetCurrentDirectoryW(c);
}

/* "X:\..." or a bare "X:" -- but not a file name like "a:b" (which is
 * stored Cygwin-style as "a\xF03Ab"). */
static int is_wdrive(const wchar_t *w)
{
	return iswalpha(w[0]) && w[1] == L':' && (w[2] == L'\\' || w[2] == 0);
}

/* "C:\a\.\b\." -> "C:\a\b" (what Windows would do with it anyway), so that
 * the same directory always gets the same path (see the stat prefetch).
 * \\?\, UNC and device paths are left alone. */
static void drop_dot_segments(wchar_t *w)
{
	wchar_t *r = w, *o = w;
	if (w[0] == L'\\' && w[1] == L'\\')
		return;
	while (*r) {
		if (r[0] == L'\\' && r[1] == L'.' && (r[2] == L'\\' || !r[2]) && o > w) {
			r += 2;
			continue;
		}
		*o++ = *r++;
	}
	*o = 0;
	if (o - w == 2 && w[1] == L':')
		wcscpy(o, L"\\");  /* keep "C:\" a root, not "C:" */
}

/* rsync path (UTF-8, '/'-separated, /cygdrive/x/...) -> malloc'd wide
 * Windows path, \\?\-prefixed when it is long. */
static wchar_t *win_path(const char *path)
{
	char *tmp, *t;
	wchar_t *w;
	size_t len;

	if (!path) {
		errno = EFAULT;
		return NULL;
	}
	if (strcmp(path, "/dev/null") == 0)
		return utf8_to_wide("NUL");
	if (strcmp(path, "/dev/tty") == 0)
		return utf8_to_wide("CON");

	len = strlen(path);
	tmp = malloc(len + 4);
	if (!tmp) {
		errno = ENOMEM;
		return NULL;
	}
	if (is_cygdrive(path)) {
		tmp[0] = toupper((unsigned char)path[10]);
		tmp[1] = ':';
		tmp[2] = '/';
		strcpy(tmp + 3, path[11] ? path + 12 : "");
	} else
		strcpy(tmp, path);
	for (t = tmp; *t; t++) {
		if (*t == '/')
			*t = '\\';
	}

	w = utf8_to_wide(tmp);
	free(tmp);
	if (!w) {
		errno = ENOMEM;
		return NULL;
	}
	{
		wchar_t *start = w;
		if (wcsncmp(start, L"\\\\?\\", 4) == 0)
			start += 4;
		if (is_wdrive(start))
			start += 2;
		cyg_map(start);
	}

	/* Relative paths are resolved against our own record of the current
	 * directory (see w32_chdir()): Windows' real one can't be deeper than
	 * MAX_PATH, while rsync happily chdir()s into deep trees. */
	if (!(w[0] == L'\\' && w[1] == L'\\') && !is_wdrive(w)) {
		const wchar_t *cwd = cur_dir();
		size_t clen = wcslen(cwd), wlen = wcslen(w);
		wchar_t *j = malloc((clen + wlen + 2) * sizeof (wchar_t));
		if (!j) {
			free(w);
			errno = ENOMEM;
			return NULL;
		}
		if (w[0] == L'\\') {
			/* "\x": root of the current drive */
			wcsncpy(j, cwd, 2);
			wcscpy(j + 2, w);
		} else {
			wcscpy(j, cwd);
			if (clen && cwd[clen - 1] != L'\\')
				wcscat(j, L"\\");
			wcscat(j, w);
		}
		free(w);
		w = j;
	}
	drop_dot_segments(w);

	if (wcslen(w) >= 240 && wcsncmp(w, L"\\\\?\\", 4) != 0) {
		DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
		wchar_t *full = n ? malloc((n + 8) * sizeof (wchar_t)) : NULL;
		if (full) {
			wchar_t *body = full + 8;
			if (GetFullPathNameW(w, n, body, NULL)) {
				if (body[0] == '\\' && body[1] == '\\') {
					/* \\server\share -> \\?\UNC\server\share */
					wchar_t *res = malloc((wcslen(body) + 8) * sizeof (wchar_t));
					if (res) {
						wcscpy(res, L"\\\\?\\UNC");
						wcscat(res, body + 1);
						free(w);
						free(full);
						return res;
					}
				} else {
					body -= 4;
					memcpy(body, L"\\\\?\\", 4 * sizeof (wchar_t));
					memmove(full, body, (wcslen(body) + 1) * sizeof (wchar_t));
					free(w);
					return full;
				}
			}
			free(full);
		}
	}
	return w;
}

/* Wide Windows path -> malloc'd rsync path (UTF-8, /cygdrive/x/...). */
static char *posix_path(const wchar_t *w)
{
	char *s, *p, *out;

	wchar_t *copy;

	if (wcsncmp(w, L"\\\\?\\UNC\\", 8) == 0)
		w += 6;  /* keep the leading "\\" of \\server\share */
	else if (wcsncmp(w, L"\\\\?\\", 4) == 0 || wcsncmp(w, L"\\??\\", 4) == 0)
		w += 4;

	if (!(copy = _wcsdup(w)))
		return NULL;
	cyg_unmap(copy);
	s = wide_to_utf8(copy);
	free(copy);
	if (!s)
		return NULL;
	for (p = s; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}
	if (isalpha((unsigned char)s[0]) && s[1] == ':') {
		out = malloc(strlen(s) + 12);
		if (!out) {
			free(s);
			return NULL;
		}
		sprintf(out, "/cygdrive/%c%s%s", tolower((unsigned char)s[0]),
			s[2] == '/' || s[2] == '\0' ? "" : "/", s + 2);
		/* "/cygdrive/c/" for a bare root is fine, but drop a trailing
		 * slash on anything longer. */
		free(s);
		return out;
	}
	return s;
}

/* ===================================================================== */
/* stat                                                                   */
/* ===================================================================== */

#define FT_EPOCH_DIFF 116444736000000000ULL

static void ft_to_unix(const FILETIME *ft, time_t *sec, long *nsec)
{
	unsigned long long v = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
	if (v < FT_EPOCH_DIFF) {
		*sec = 0;
		*nsec = 0;
		return;
	}
	v -= FT_EPOCH_DIFF;
	*sec = (time_t)(v / 10000000ULL);
	*nsec = (long)(v % 10000000ULL) * 100;
}

static FILETIME unix_to_ft(time_t sec, long nsec)
{
	FILETIME ft;
	unsigned long long v = (unsigned long long)sec * 10000000ULL + nsec / 100 + FT_EPOCH_DIFF;
	ft.dwLowDateTime = (DWORD)v;
	ft.dwHighDateTime = (DWORD)(v >> 32);
	return ft;
}

static int has_exec_ext(const wchar_t *path)
{
	static const wchar_t *exts[] = { L".exe", L".com", L".bat", L".cmd", L".sh", NULL };
	const wchar_t *dot = wcsrchr(path, L'.');
	int i;
	if (!dot || wcschr(dot, L'\\'))
		return 0;
	for (i = 0; exts[i]; i++) {
		if (_wcsicmp(dot, exts[i]) == 0)
			return 1;
	}
	return 0;
}

/* Permission bits from the file's NTFS ACL, matching what Cygwin's stat()
 * reports (its POSIX-ACL view, as shown by getfacl):
 *   user  = the owner's own ACE; failing that, what the owner gets through
 *           the groups it belongs to (Cygwin's behaviour for the common
 *           "only inherited group ACEs" case)
 *   group = the ACL mask: owning group plus every other named user/group ACE
 *   other = the Everyone ACE
 * Deny ACEs remove bits from the class they apply to.  Returns (mode_t)-1 if
 * the security descriptor can't be read. */
static PSID world_sid(void)
{
	static BYTE buf[SECURITY_MAX_SID_SIZE];
	static PSID sid;
	if (!sid) {
		DWORD n = sizeof buf;
		if (CreateWellKnownSid(WinWorldSid, NULL, buf, &n))
			sid = buf;
	}
	return sid;
}

static PSID null_sid(void)
{
	static BYTE buf[SECURITY_MAX_SID_SIZE];
	static PSID sid;
	if (!sid) {
		DWORD n = sizeof buf;
		if (CreateWellKnownSid(WinNullSid, NULL, buf, &n))
			sid = buf;
	}
	return sid;
}

/* Is the current user a member of SID?  Cached: a tree has only a handful
 * of distinct SIDs in its ACLs. */
static int token_member(PSID sid)
{
	static struct {
		BYTE sid[SECURITY_MAX_SID_SIZE];
		int member;
	} cache[32];
	static int ncache;
	static SRWLOCK lock = SRWLOCK_INIT;  /* stat prefetch threads call this too */
	DWORD len = GetLengthSid(sid);
	BOOL member = FALSE;
	int i;

	AcquireSRWLockShared(&lock);
	for (i = 0; i < ncache; i++) {
		if (GetLengthSid((PSID)cache[i].sid) == len && memcmp(cache[i].sid, sid, len) == 0) {
			member = cache[i].member;
			ReleaseSRWLockShared(&lock);
			return member;
		}
	}
	ReleaseSRWLockShared(&lock);
	if (!CheckTokenMembership(NULL, sid, &member))
		member = FALSE;
	AcquireSRWLockExclusive(&lock);
	if (ncache < 32 && len <= SECURITY_MAX_SID_SIZE) {
		memcpy(cache[ncache].sid, sid, len);
		cache[ncache++].member = member ? 1 : 0;
	}
	ReleaseSRWLockExclusive(&lock);
	return member ? 1 : 0;
}

static mode_t mask_bits(ACCESS_MASK mask)
{
	mode_t bits = 0;
	if (mask & (GENERIC_ALL | GENERIC_READ | FILE_READ_DATA))
		bits |= 04;
	if (mask & (GENERIC_ALL | GENERIC_WRITE | FILE_WRITE_DATA))
		bits |= 02;
	if (mask & (GENERIC_ALL | GENERIC_EXECUTE | FILE_EXECUTE))
		bits |= 01;
	return bits;
}

static mode_t acl_mode(HANDLE h)
{
	PSID owner = NULL, group = NULL, world = world_sid();
	PACL dacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	mode_t user = 0, user_deny = 0, via_groups = 0, groups_deny = 0;
	mode_t grp = 0, grp_deny = 0, other = 0, other_deny = 0;
	int have_owner_ace = 0;
	WORD i;

	if (GetSecurityInfo(h, SE_FILE_OBJECT,
			    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
			    &owner, &group, &dacl, NULL, &sd) != ERROR_SUCCESS)
		return (mode_t)-1;
	if (!dacl) {
		LocalFree(sd);
		return 0777;
	}
	for (i = 0; i < dacl->AceCount; i++) {
		ACE_HEADER *ace;
		PSID sid;
		mode_t bits;
		int allowed;

		if (!GetAce(dacl, i, (LPVOID *)&ace) || (ace->AceFlags & INHERIT_ONLY_ACE))
			continue;
		if (ace->AceType == ACCESS_ALLOWED_ACE_TYPE) {
			allowed = 1;
			bits = mask_bits(((ACCESS_ALLOWED_ACE *)ace)->Mask);
			sid = &((ACCESS_ALLOWED_ACE *)ace)->SidStart;
		} else if (ace->AceType == ACCESS_DENIED_ACE_TYPE) {
			allowed = 0;
			bits = mask_bits(((ACCESS_DENIED_ACE *)ace)->Mask);
			sid = &((ACCESS_DENIED_ACE *)ace)->SidStart;
		} else
			continue;

		/* Cygwin keeps private metadata (setuid/setgid/sticky) in a NULL-SID
		 * deny ACE; it is not a permission. */
		if (EqualSid(sid, null_sid()))
			continue;
		if (world && EqualSid(sid, world)) {
			if (allowed) {
				other |= bits & ~other_deny;
				via_groups |= bits & ~groups_deny;
			} else {
				other_deny |= bits & ~other;
				groups_deny |= bits & ~via_groups;
			}
			continue;
		}
		if (owner && EqualSid(sid, owner)) {
			have_owner_ace = 1;
			if (allowed)
				user |= bits & ~user_deny;
			else
				user_deny |= bits & ~user;
			continue;
		}
		/* Owning group or any other named user/group: part of the mask. */
		if (allowed)
			grp |= bits & ~grp_deny;
		else
			grp_deny |= bits & ~grp;
		if (token_member(sid)) {
			if (allowed)
				via_groups |= bits & ~groups_deny;
			else
				groups_deny |= bits & ~via_groups;
		}
	}
	LocalFree(sd);
	if (!have_owner_ace)
		user = via_groups;
	return (user << 6) | (grp << 3) | other;
}

static void apply_acl_mode(HANDLE h, const BY_HANDLE_FILE_INFORMATION *fi, struct w32_stat *st);

static mode_t attr_to_mode(DWORD attr, const wchar_t *path)
{
	mode_t mode;
	if (attr & FILE_ATTRIBUTE_DIRECTORY)
		return S_IFDIR | 0755;
	mode = S_IFREG | 0644;
	if (attr & FILE_ATTRIBUTE_READONLY)
		mode &= ~0222;
	if (path && has_exec_ext(path))
		mode |= 0111;
	return mode;
}

#ifndef IO_REPARSE_TAG_LX_SYMLINK
#define IO_REPARSE_TAG_LX_SYMLINK 0xA000001DL
#endif

static int is_link_tag(DWORD tag)
{
	return tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT
	    || tag == IO_REPARSE_TAG_LX_SYMLINK;
}

/* Cygwin "system file" symlinks (winsymlinks:sys, and what Cygwin falls back
 * to on many setups): a small file with the SYSTEM attribute holding
 * "!<symlink>" followed by the target as UTF-16LE (with BOM) or UTF-8.
 * Returns the malloc'd UTF-8 target, or NULL if H is not one. */
#define CYG_SYMLINK_MAGIC "!<symlink>"
static char *cygwin_sys_symlink(HANDLE h, const BY_HANDLE_FILE_INFORMATION *fi)
{
	char buf[4096 + 2];
	DWORD got = 0, len = fi->nFileSizeLow;
	size_t mlen = sizeof CYG_SYMLINK_MAGIC - 1;
	LARGE_INTEGER zero;

	if (!(fi->dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
	 || (fi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	 || fi->nFileSizeHigh || len <= mlen || len > 4096)
		return NULL;
	zero.QuadPart = 0;
	SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
	if (!ReadFile(h, buf, len, &got, NULL) || got != len || memcmp(buf, CYG_SYMLINK_MAGIC, mlen) != 0)
		return NULL;
	buf[got] = buf[got + 1] = 0;
	if (got >= mlen + 2 && (unsigned char)buf[mlen] == 0xFF && (unsigned char)buf[mlen + 1] == 0xFE) {
		wchar_t w[2049];
		size_t n = (got - mlen - 2) / 2;
		memcpy(w, buf + mlen + 2, n * 2);
		w[n] = 0;
		while (n && w[n - 1] == 0)
			n--;
		return wide_to_utf8(w);
	}
	return strdup(buf + mlen);
}

/* Is the file at WP (opened for read) a Cygwin system-file symlink? */
static char *cygwin_sys_symlink_path(const wchar_t *wp)
{
	HANDLE h = CreateFileW(wp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			       NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	BY_HANDLE_FILE_INFORMATION fi;
	char *t = NULL;
	if (h == INVALID_HANDLE_VALUE)
		return NULL;
	if (GetFileInformationByHandle(h, &fi))
		t = cygwin_sys_symlink(h, &fi);
	CloseHandle(h);
	return t;
}

static ssize_t readlink_handle(HANDLE h, char *buf, size_t size);

static void fill_stat_from_info(struct w32_stat *st, const BY_HANDLE_FILE_INFORMATION *fi,
				const wchar_t *path)
{
	memset(st, 0, sizeof *st);
	st->st_dev = fi->dwVolumeSerialNumber;
	st->st_ino = (ino_t)((((unsigned long long)fi->nFileIndexHigh << 32) | fi->nFileIndexLow)
			     & 0x7fffffffffffffffULL);
	st->st_nlink = fi->nNumberOfLinks;
	st->st_mode = attr_to_mode(fi->dwFileAttributes, path);
	st->st_uid = W32_UID;
	st->st_gid = W32_UID;
	st->st_size = (fi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0
		    : (long long)(((unsigned long long)fi->nFileSizeHigh << 32) | fi->nFileSizeLow);
	ft_to_unix(&fi->ftLastAccessTime, &st->st_atime, &st->st_atimensec);
	ft_to_unix(&fi->ftLastWriteTime, &st->st_mtime, &st->st_mtimensec);
	ft_to_unix(&fi->ftLastWriteTime, &st->st_ctime, &st->st_ctimensec);
	{
		long ns;
		ft_to_unix(&fi->ftCreationTime, &st->st_birthtime, &ns);
	}
	st->st_blksize = 4096;
	st->st_blocks = (st->st_size + 511) / 512;
}

static void apply_acl_mode(HANDLE h, const BY_HANDLE_FILE_INFORMATION *fi, struct w32_stat *st)
{
	mode_t m = acl_mode(h);
	if (m == (mode_t)-1)
		return;
	if ((fi->dwFileAttributes & FILE_ATTRIBUTE_READONLY) && !(fi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		m &= ~0222;
	st->st_mode = (st->st_mode & S_IFMT) | m;
}

static int stat_by_find(const wchar_t *wp, struct w32_stat *st)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(wp, &fd);
	BY_HANDLE_FILE_INFORMATION fi;
	if (h == INVALID_HANDLE_VALUE)
		return set_errno_win();
	FindClose(h);
	memset(&fi, 0, sizeof fi);
	fi.dwFileAttributes = fd.dwFileAttributes;
	fi.ftCreationTime = fd.ftCreationTime;
	fi.ftLastAccessTime = fd.ftLastAccessTime;
	fi.ftLastWriteTime = fd.ftLastWriteTime;
	fi.nFileSizeHigh = fd.nFileSizeHigh;
	fi.nFileSizeLow = fd.nFileSizeLow;
	fi.nNumberOfLinks = 1;
	fill_stat_from_info(st, &fi, wp);
	return 0;
}

/* stat of the Windows path WP; thread-safe (the stat prefetch threads run
 * it too), so it must not touch the current directory or other state the
 * main thread changes. */
static int stat_wide(const wchar_t *wp, struct w32_stat *st, int follow)
{
	HANDLE h;
	BY_HANDLE_FILE_INFORMATION fi;
	int ret = 0, have_acl = 1;

	/* READ_CONTROL too, for the ACL-derived permission bits; retry without
	 * it for the odd file whose DACL we may not read. */
	h = CreateFileW(wp, FILE_READ_ATTRIBUTES | READ_CONTROL,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), NULL);
	if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
		have_acl = 0;
		h = CreateFileW(wp, FILE_READ_ATTRIBUTES,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), NULL);
	}
	if (h == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();
		if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED)
			return stat_by_find(wp, st);
		errno = errno_from_win(e);
		return -1;
	}

	if (!GetFileInformationByHandle(h, &fi)) {
		ret = set_errno_win();
		CloseHandle(h);
		return ret;
	}
	fill_stat_from_info(st, &fi, wp);
	if (have_acl)
		apply_acl_mode(h, &fi, st);

	if (!follow && (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
		FILE_ATTRIBUTE_TAG_INFO ti;
		if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti)
		 && is_link_tag(ti.ReparseTag)) {
			char tgt[4096];
			ssize_t n = readlink_handle(h, tgt, sizeof tgt);
			st->st_mode = S_IFLNK | 0777;
			st->st_size = n > 0 ? n : 0;
			st->st_blocks = 0;
		}
	} else if (!follow && (fi.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) && S_ISREG(st->st_mode)
		&& st->st_size <= 4096) {
		char *t = cygwin_sys_symlink_path(wp);
		if (t) {
			st->st_mode = S_IFLNK | 0777;
			st->st_size = strlen(t);
			st->st_blocks = 0;
			free(t);
		}
	}
	CloseHandle(h);
	return ret;
}

static int prefetch_take(const wchar_t *wp, int follow, struct w32_stat *st, int *ret);
static void prefetch_invalidate(void);

static int do_w32_stat(const char *path, struct w32_stat *st, int follow)
{
	wchar_t *wp = win_path(path);
	int ret;

	if (!wp)
		return -1;
	if (!prefetch_take(wp, follow, st, &ret))
		ret = stat_wide(wp, st, follow);
	free(wp);
	return ret;
}

int w32_stat(const char *path, struct w32_stat *st)
{
	return do_w32_stat(path, st, 1);
}

int w32_lstat(const char *path, struct w32_stat *st)
{
	return do_w32_stat(path, st, 0);
}

/* ===================================================================== */
/* socket <-> fd table                                                    */
/* ===================================================================== */

static SOCKET sock_tab[W32_MAXFD];
static unsigned char sock_nonblock[W32_MAXFD];
static int sock_tab_ready;

/* defined further down; declared here for the socket wrappers */
static unsigned char fd_record[W32_MAXFD];
static unsigned char fd_null[W32_MAXFD];
static SOCKET replay_feed;
static int replay_active;
static char *replay_password;
static char *remembered_password;

static void sock_tab_init(void)
{
	int i;
	if (sock_tab_ready)
		return;
	for (i = 0; i < W32_MAXFD; i++)
		sock_tab[i] = INVALID_SOCKET;
	sock_tab_ready = 1;
}

static SOCKET fd_sock(int fd)
{
	if (fd < 0 || fd >= W32_MAXFD || !sock_tab_ready)
		return INVALID_SOCKET;
	return sock_tab[fd];
}

/* Give SOCKET s an fd number (backed by a private NUL handle). */
static int sock_to_fd(SOCKET s)
{
	int fd;
	sock_tab_init();
	fd = _open("NUL", _O_RDWR | _O_BINARY | _O_NOINHERIT);
	if (fd < 0)
		return -1;
	if (fd >= W32_MAXFD) {
		_close(fd);
		errno = EMFILE;
		return -1;
	}
	sock_tab[fd] = s;
	sock_nonblock[fd] = 0;
	/* Never let a socket leak into a spawned child. */
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
	return fd;
}

/* Remove fd from the table, returning its SOCKET (left open). */
static SOCKET fd_detach(int fd)
{
	SOCKET s = fd_sock(fd);
	if (s != INVALID_SOCKET) {
		sock_tab[fd] = INVALID_SOCKET;
		_close(fd);
	}
	return s;
}

int w32_socket(int af, int type, int proto)
{
	SOCKET s = socket(af, type, proto);
	int fd;
	if (s == INVALID_SOCKET)
		return set_errno_wsa();
	if ((fd = sock_to_fd(s)) < 0)
		closesocket(s);
	return fd;
}

#define SOCK_OR_FAIL(fd, s) \
	SOCKET s = fd_sock(fd); \
	if (s == INVALID_SOCKET) { errno = ENOTSOCK; return -1; }

int w32_connect(int fd, const struct sockaddr *sa, socklen_t len)
{
	SOCK_OR_FAIL(fd, s);
	if (replay_active && replay_feed != INVALID_SOCKET) {
		/* replayed run: the "connection" is the parent's feed */
		sock_tab[fd] = replay_feed;
		replay_feed = INVALID_SOCKET;
		closesocket(s);
		return 0;
	}
	/* An outgoing connection (rsync://) is protocol input: record it. */
	fd_record[fd] = 1;
	if (connect(s, sa, len) == SOCKET_ERROR) {
		int e = WSAGetLastError();
		errno = e == WSAEWOULDBLOCK ? EINPROGRESS : errno_from_wsa(e);
		return -1;
	}
	return 0;
}

int w32_bind(int fd, const struct sockaddr *sa, socklen_t len)
{
	SOCK_OR_FAIL(fd, s);
	return bind(s, sa, len) == SOCKET_ERROR ? set_errno_wsa() : 0;
}

int w32_listen(int fd, int backlog)
{
	SOCK_OR_FAIL(fd, s);
	return listen(s, backlog) == SOCKET_ERROR ? set_errno_wsa() : 0;
}

int w32_accept(int fd, struct sockaddr *sa, socklen_t *len)
{
	SOCKET a;
	int nfd;
	SOCK_OR_FAIL(fd, s);
	a = accept(s, sa, len);
	if (a == INVALID_SOCKET)
		return set_errno_wsa();
	if ((nfd = sock_to_fd(a)) < 0)
		closesocket(a);
	return nfd;
}

int w32_shutdown(int fd, int how)
{
	SOCK_OR_FAIL(fd, s);
	return shutdown(s, how) == SOCKET_ERROR ? set_errno_wsa() : 0;
}

int w32_setsockopt(int fd, int level, int opt, const void *val, socklen_t len)
{
	SOCK_OR_FAIL(fd, s);
	return setsockopt(s, level, opt, val, len) == SOCKET_ERROR ? set_errno_wsa() : 0;
}

int w32_getsockopt(int fd, int level, int opt, void *val, socklen_t *len)
{
	SOCK_OR_FAIL(fd, s);
	if (getsockopt(s, level, opt, val, len) == SOCKET_ERROR)
		return set_errno_wsa();
	if (level == SOL_SOCKET && opt == SO_ERROR && *len >= (socklen_t)sizeof (int)) {
		int *e = val;
		if (*e)
			*e = errno_from_wsa(*e);
	}
	return 0;
}

int w32_getsockname(int fd, struct sockaddr *sa, socklen_t *len)
{
	SOCK_OR_FAIL(fd, s);
	return getsockname(s, sa, len) == SOCKET_ERROR ? set_errno_wsa() : 0;
}

int w32_getpeername(int fd, struct sockaddr *sa, socklen_t *len)
{
	SOCK_OR_FAIL(fd, s);
	return getpeername(s, sa, len) == SOCKET_ERROR ? set_errno_wsa() : 0;
}

/* A connected loopback TCP pair: the portable stand-in for an AF_UNIX
 * socketpair (and for pipe(), since only sockets can be polled). */
static int raw_socketpair(SOCKET sv[2])
{
	SOCKET lst = INVALID_SOCKET, c = INVALID_SOCKET, a = INVALID_SOCKET;
	struct sockaddr_in addr, caddr, paddr;
	int alen = sizeof addr;
	int one = 1;

	lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lst == INVALID_SOCKET)
		goto fail;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	setsockopt(lst, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&one, sizeof one);
	if (bind(lst, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR
	 || getsockname(lst, (struct sockaddr *)&addr, &alen) == SOCKET_ERROR
	 || listen(lst, 1) == SOCKET_ERROR)
		goto fail;
	c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (c == INVALID_SOCKET || connect(c, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR)
		goto fail;
	alen = sizeof caddr;
	if (getsockname(c, (struct sockaddr *)&caddr, &alen) == SOCKET_ERROR)
		goto fail;
	for (;;) {
		alen = sizeof paddr;
		a = accept(lst, (struct sockaddr *)&paddr, &alen);
		if (a == INVALID_SOCKET)
			goto fail;
		/* Make sure it is our own connect, not some other local process. */
		if (paddr.sin_port == caddr.sin_port && paddr.sin_addr.s_addr == caddr.sin_addr.s_addr)
			break;
		closesocket(a);
	}
	closesocket(lst);
	setsockopt(a, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
	setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
	{
		int bufsz = 256 * 1024;
		setsockopt(a, SOL_SOCKET, SO_RCVBUF, (char *)&bufsz, sizeof bufsz);
		setsockopt(a, SOL_SOCKET, SO_SNDBUF, (char *)&bufsz, sizeof bufsz);
		setsockopt(c, SOL_SOCKET, SO_RCVBUF, (char *)&bufsz, sizeof bufsz);
		setsockopt(c, SOL_SOCKET, SO_SNDBUF, (char *)&bufsz, sizeof bufsz);
	}
	SetHandleInformation((HANDLE)a, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
	sv[0] = c;
	sv[1] = a;
	return 0;

  fail:
	set_errno_wsa();
	if (lst != INVALID_SOCKET)
		closesocket(lst);
	if (c != INVALID_SOCKET)
		closesocket(c);
	return -1;
}

int w32_socketpair(int af, int type, int proto, int fds[2])
{
	SOCKET sv[2];
	(void)af; (void)type; (void)proto;
	if (raw_socketpair(sv) < 0)
		return -1;
	if ((fds[0] = sock_to_fd(sv[0])) < 0) {
		closesocket(sv[0]);
		closesocket(sv[1]);
		return -1;
	}
	if ((fds[1] = sock_to_fd(sv[1])) < 0) {
		w32_close(fds[0]);
		closesocket(sv[1]);
		return -1;
	}
	return 0;
}

int w32_pipe(int fds[2])
{
	return w32_socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
}

/* ===================================================================== */
/* read / write / close / fcntl                                           */
/* ===================================================================== */

/* ---- protocol-input recording, for the replay "fork" (see below) ---- */
static unsigned char fd_record[W32_MAXFD];   /* reads on this fd are recorded */
static unsigned char fd_null[W32_MAXFD];     /* writes are discarded (replay) */
static char *rec_buf;
static size_t rec_len, rec_cap;
static int rec_enabled = 1;

static void record_bytes(const void *buf, size_t n)
{
	if (rec_len + n > rec_cap) {
		size_t cap = rec_cap ? rec_cap : 1 << 20;
		char *nb;
		while (cap < rec_len + n)
			cap *= 2;
		if (!(nb = realloc(rec_buf, cap))) {
			fprintf(stderr, "rsync: out of memory recording protocol input\n");
			exit(1);
		}
		rec_buf = nb;
		rec_cap = cap;
	}
	memcpy(rec_buf + rec_len, buf, n);
	rec_len += n;
}

ssize_t w32_read(int fd, void *buf, size_t len)
{
	SOCKET s = fd_sock(fd);
	ssize_t n;
	if (len > INT_MAX)
		len = INT_MAX;
	if (fd >= 0 && fd < W32_MAXFD && fd_null[fd])
		return 0;
	if (s != INVALID_SOCKET) {
		n = recv(s, buf, (int)len, 0);
		if (n == SOCKET_ERROR) {
			int e = WSAGetLastError();
			if (e == WSAESHUTDOWN || e == WSAECONNABORTED)
				return 0;
			errno = errno_from_wsa(e);
			return -1;
		}
	} else
		n = _read(fd, buf, (unsigned)len);
	if (n > 0 && rec_enabled && fd >= 0 && fd < W32_MAXFD && fd_record[fd])
		record_bytes(buf, n);
	return n;
}

ssize_t w32_write(int fd, const void *buf, size_t len)
{
	SOCKET s = fd_sock(fd);
	if (len > INT_MAX)
		len = INT_MAX;
	if (fd >= 0 && fd < W32_MAXFD && fd_null[fd])
		return (ssize_t)len;
	if (s != INVALID_SOCKET) {
		int n = send(s, buf, (int)len, 0);
		if (n == SOCKET_ERROR) {
			int e = WSAGetLastError();
			if (e == WSAECONNRESET || e == WSAECONNABORTED || e == WSAESHUTDOWN)
				errno = EPIPE;
			else
				errno = errno_from_wsa(e);
			return -1;
		}
		return n;
	}
	return _write(fd, buf, (unsigned)len);
}

int w32_close(int fd)
{
	SOCKET s = fd_sock(fd);
	if (fd >= 0 && fd < W32_MAXFD) {
		fd_record[fd] = 0;
		if (fd_null[fd]) {
			fd_null[fd] = 0;
			if (s == INVALID_SOCKET)
				return _close(fd);
		}
	}
	if (s != INVALID_SOCKET) {
		sock_tab[fd] = INVALID_SOCKET;
		closesocket(s);
		_close(fd);
		return 0;
	}
	return _close(fd);
}

int w32_ftruncate(int fd, long long size)
{
	errno_t e = _chsize_s(fd, size);
	prefetch_invalidate();
	if (e) {
		errno = e;
		return -1;
	}
	return 0;
}

int w32_fcntl(int fd, int cmd, ...)
{
	SOCKET s = fd_sock(fd);
	va_list ap;
	long arg = 0;

	va_start(ap, cmd);
	if (cmd == F_SETFL || cmd == F_SETFD)
		arg = va_arg(ap, long);
	va_end(ap);

	switch (cmd) {
	case F_GETFL:
		if (s != INVALID_SOCKET)
			return O_RDWR | (sock_nonblock[fd] ? O_NONBLOCK : 0);
		return O_RDWR;
	case F_SETFL:
		if (s != INVALID_SOCKET) {
			u_long nb = (arg & O_NONBLOCK) ? 1 : 0;
			if (ioctlsocket(s, FIONBIO, &nb) == SOCKET_ERROR)
				return set_errno_wsa();
			sock_nonblock[fd] = (unsigned char)nb;
		}
		return 0;
	case F_GETFD:
	case F_SETFD:
	case F_SETLK:   /* lock_range(): daemon "max connections" only */
	case F_SETLKW:
		return 0;
	default:
		errno = EINVAL;
		return -1;
	}
}

int w32_dup(int fd)
{
	if (fd_sock(fd) != INVALID_SOCKET) {
		errno = ENOTSUP;
		return -1;
	}
	return _dup(fd);
}

int w32_dup2(int fd, int fd2)
{
	if (fd_sock(fd) != INVALID_SOCKET || fd_sock(fd2) != INVALID_SOCKET) {
		errno = ENOTSUP;
		return -1;
	}
	return _dup2(fd, fd2) < 0 ? -1 : fd2;
}

int w32_fstat(int fd, struct w32_stat *st)
{
	HANDLE h;
	DWORD type;

	memset(st, 0, sizeof *st);
	st->st_uid = st->st_gid = W32_UID;
	st->st_nlink = 1;
	if (fd_sock(fd) != INVALID_SOCKET) {
		st->st_mode = S_IFSOCK | 0600;
		return 0;
	}
	h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	type = GetFileType(h) & ~FILE_TYPE_REMOTE;
	if (type == FILE_TYPE_DISK) {
		BY_HANDLE_FILE_INFORMATION fi;
		if (!GetFileInformationByHandle(h, &fi))
			return set_errno_win();
		fill_stat_from_info(st, &fi, NULL);
		apply_acl_mode(h, &fi, st);
		return 0;
	}
	st->st_mode = (type == FILE_TYPE_CHAR ? S_IFCHR : S_IFIFO) | 0600;
	return 0;
}

/* ===================================================================== */
/* select / poll (sockets only; anything else is reported ready)          */
/* ===================================================================== */

static int map_set(fd_set *in, fd_set *out, int *nonsock)
{
	u_int i;
	FD_ZERO(out);
	if (!in)
		return 0;
	for (i = 0; i < in->fd_count; i++) {
		int fd = (int)in->fd_array[i];
		SOCKET s = fd_sock(fd);
		if (s == INVALID_SOCKET)
			(*nonsock)++;
		else
			FD_SET(s, out);
	}
	return out->fd_count;
}

static void unmap_set(fd_set *in, fd_set *res, int keep_nonsock)
{
	u_int i, j = 0;
	if (!in)
		return;
	for (i = 0; i < in->fd_count; i++) {
		int fd = (int)in->fd_array[i];
		SOCKET s = fd_sock(fd);
		if (s == INVALID_SOCKET ? keep_nonsock : FD_ISSET(s, res))
			in->fd_array[j++] = fd;
	}
	in->fd_count = j;
}

int w32_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
	fd_set sr, sw, se;
	int nonsock_r = 0, nonsock_w = 0, nonsock_e = 0, cnt;
	struct timeval zero = { 0, 0 };
	(void)n;

	cnt = map_set(r, &sr, &nonsock_r) + map_set(w, &sw, &nonsock_w) + map_set(e, &se, &nonsock_e);
	if (cnt == 0) {
		if (nonsock_r + nonsock_w) {
			unmap_set(r, &sr, 1);
			unmap_set(w, &sw, 1);
			if (e)
				FD_ZERO(e);
			return nonsock_r + nonsock_w;
		}
		if (tv)
			Sleep((DWORD)(tv->tv_sec * 1000 + tv->tv_usec / 1000));
		else
			Sleep(INFINITE);
		return 0;
	}
	if (nonsock_r + nonsock_w)
		tv = &zero;
	cnt = select(0, r ? &sr : NULL, w ? &sw : NULL, e ? &se : NULL, tv);
	if (cnt == SOCKET_ERROR)
		return set_errno_wsa();
	unmap_set(r, &sr, 1);
	unmap_set(w, &sw, 1);
	unmap_set(e, &se, 0);
	return cnt + nonsock_r + nonsock_w;
}

/* Set by the cross-process signal watcher (see kill()); makes a blocked
 * poll() return EINTR the way a real signal would, so rsync looks at its
 * got_sigusr2 / got_kill_signal flags promptly. */
static volatile LONG w32_sig_pending;

int w32_poll(struct w32_pollfd *fds, nfds_t n, int timeout)
{
	fd_set r, w, e;
	struct timeval tv;
	nfds_t i;
	int nsock = 0, ready = 0, cnt;
	ULONGLONG deadline = timeout >= 0 ? GetTickCount64() + timeout : 0;

	FD_ZERO(&r);
	FD_ZERO(&w);
	FD_ZERO(&e);
	for (i = 0; i < n; i++) {
		SOCKET s;
		fds[i].revents = 0;
		if (fds[i].fd < 0)
			continue;
		if (fds[i].fd < W32_MAXFD && fd_null[fds[i].fd]) {
			/* discarded output: always writable, never readable */
			if ((fds[i].revents = fds[i].events & POLLOUT) != 0)
				ready++;
			continue;
		}
		s = fd_sock(fds[i].fd);
		if (s == INVALID_SOCKET) {
			HANDLE h = (HANDLE)_get_osfhandle(fds[i].fd);
			if (h == INVALID_HANDLE_VALUE) {
				fds[i].revents = POLLNVAL;
				ready++;
				continue;
			}
			if ((GetFileType(h) & ~FILE_TYPE_REMOTE) == FILE_TYPE_PIPE && (fds[i].events & POLLIN)) {
				DWORD avail = 0;
				if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
					fds[i].revents |= POLLHUP;
				else if (avail)
					fds[i].revents |= POLLIN;
				fds[i].revents |= fds[i].events & POLLOUT;
			} else
				fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
			if (fds[i].revents)
				ready++;
			continue;
		}
		if (nsock >= FD_SETSIZE)
			continue;
		if (fds[i].events & (POLLIN | POLLPRI))
			FD_SET(s, &r);
		if (fds[i].events & POLLOUT)
			FD_SET(s, &w);
		FD_SET(s, &e);
		nsock++;
	}
	if (ready)
		timeout = 0;

	for (;;) {
		DWORD slice = 100;
		if (InterlockedExchange(&w32_sig_pending, 0) && timeout != 0) {
			errno = EINTR;
			return -1;
		}
		if (timeout >= 0) {
			ULONGLONG now = GetTickCount64();
			DWORD left = now >= deadline ? 0 : (DWORD)(deadline - now);
			if (timeout == 0 || ready)
				left = 0;
			if (left < slice)
				slice = left;
		}
		if (!nsock) {
			if (ready || slice == 0)
				return ready;
			Sleep(slice);
			continue;
		}
		{
			fd_set rr = r, ww = w, ee = e;
			tv.tv_sec = 0;
			tv.tv_usec = slice * 1000;
			cnt = select(0, &rr, &ww, &ee, &tv);
			if (cnt == SOCKET_ERROR)
				return set_errno_wsa();
			if (cnt == 0 && !ready && slice != 0)
				continue;
			for (i = 0; i < n; i++) {
				SOCKET s;
				if (fds[i].fd < 0 || (fds[i].fd < W32_MAXFD && fd_null[fds[i].fd]))
					continue;
				s = fd_sock(fds[i].fd);
				if (s == INVALID_SOCKET)
					continue;
				if (FD_ISSET(s, &rr))
					fds[i].revents |= fds[i].events & (POLLIN | POLLPRI);
				if (FD_ISSET(s, &ww))
					fds[i].revents |= POLLOUT;
				if (FD_ISSET(s, &ee))
					fds[i].revents |= POLLERR;
				if (fds[i].revents)
					ready++;
			}
			return ready;
		}
	}
}

/* ===================================================================== */
/* files                                                                  */
/* ===================================================================== */

int w32_open(const char *path, int flags, ...)
{
	wchar_t *wp;
	DWORD access, disp, attrs = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS;
	HANDLE h;
	int fd, crtflags = _O_BINARY;

	if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND))
		prefetch_invalidate();
	if (!(wp = win_path(path)))
		return -1;

	switch (flags & (O_RDONLY | O_WRONLY | O_RDWR)) {
	case O_WRONLY:
		access = GENERIC_WRITE;
		crtflags |= _O_WRONLY;
		break;
	case O_RDWR:
		access = GENERIC_READ | GENERIC_WRITE;
		crtflags |= _O_RDWR;
		break;
	default:
		access = GENERIC_READ;
		crtflags |= _O_RDONLY;
		break;
	}
	if (flags & O_APPEND) {
		access |= FILE_APPEND_DATA;
		crtflags |= _O_APPEND;
	}
	if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
		disp = CREATE_NEW;
	else if ((flags & (O_CREAT | O_TRUNC)) == (O_CREAT | O_TRUNC))
		disp = CREATE_ALWAYS;
	else if (flags & O_CREAT)
		disp = OPEN_ALWAYS;
	else if (flags & O_TRUNC)
		disp = TRUNCATE_EXISTING;
	else
		disp = OPEN_EXISTING;

	h = CreateFileW(wp, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			NULL, disp, attrs, NULL);
	if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED
	 && (disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING)) {
		/* Hidden/system files refuse CREATE_ALWAYS with mismatched attrs. */
		DWORD a = GetFileAttributesW(wp);
		if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
			SetFileAttributesW(wp, FILE_ATTRIBUTE_NORMAL);
			h = CreateFileW(wp, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					NULL, disp, attrs, NULL);
		}
	}
	if (h == INVALID_HANDLE_VALUE) {
		DWORD e = GetLastError();
		free(wp);
		if (e == ERROR_FILE_EXISTS && !(flags & O_EXCL))
			e = ERROR_ACCESS_DENIED;
		errno = errno_from_win(e);
		return -1;
	}
	free(wp);

	if (access & GENERIC_WRITE) {
		BY_HANDLE_FILE_INFORMATION fi;
		if (GetFileInformationByHandle(h, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			CloseHandle(h);
			errno = EISDIR;
			return -1;
		}
	}

	fd = _open_osfhandle((intptr_t)h, crtflags);
	if (fd < 0)
		CloseHandle(h);
	return fd;
}

FILE *w32_fopen(const char *path, const char *mode)
{
	wchar_t *wp = win_path(path), *wm;
	FILE *f;
	if (!wp)
		return NULL;
	if (strpbrk(mode, "wa+"))
		prefetch_invalidate();
	wm = utf8_to_wide(mode);
	f = _wfopen(wp, wm);
	free(wp);
	free(wm);
	return f;
}

int w32_unlink(const char *path)
{
	wchar_t *wp = win_path(path);
	DWORD a;
	int ret = 0;
	prefetch_invalidate();
	if (!wp)
		return -1;
	a = GetFileAttributesW(wp);
	if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
		/* A directory symlink/junction is removed with RemoveDirectory;
		 * a real directory is EISDIR, like POSIX unlink(). */
		if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
			free(wp);
			errno = EISDIR;
			return -1;
		}
		if (!RemoveDirectoryW(wp))
			ret = set_errno_win();
		free(wp);
		return ret;
	}
	if (!DeleteFileW(wp)) {
		if (GetLastError() == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES
		 && (a & FILE_ATTRIBUTE_READONLY)) {
			SetFileAttributesW(wp, a & ~FILE_ATTRIBUTE_READONLY);
			if (DeleteFileW(wp)) {
				free(wp);
				return 0;
			}
			SetFileAttributesW(wp, a);
		}
		ret = set_errno_win();
	}
	free(wp);
	return ret;
}

int w32_rmdir(const char *path)
{
	wchar_t *wp = win_path(path);
	int ret = 0;
	prefetch_invalidate();
	if (!wp)
		return -1;
	if (!RemoveDirectoryW(wp)) {
		DWORD a = GetFileAttributesW(wp);
		if (GetLastError() == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES
		 && (a & FILE_ATTRIBUTE_READONLY)) {
			SetFileAttributesW(wp, a & ~FILE_ATTRIBUTE_READONLY);
			if (RemoveDirectoryW(wp)) {
				free(wp);
				return 0;
			}
		}
		ret = set_errno_win();
	}
	free(wp);
	return ret;
}

int w32_mkdir(const char *path, mode_t mode)
{
	wchar_t *wp = win_path(path);
	int ret = 0;
	(void)mode;
	prefetch_invalidate();
	if (!wp)
		return -1;
	if (!CreateDirectoryW(wp, NULL))
		ret = set_errno_win();
	free(wp);
	return ret;
}

int w32_rename(const char *from, const char *to)
{
	wchar_t *wf = win_path(from), *wt;
	int ret = 0;
	prefetch_invalidate();
	if (!wf)
		return -1;
	if (!(wt = win_path(to))) {
		free(wf);
		return -1;
	}
	if (!MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING)) {
		DWORD e = GetLastError(), a = GetFileAttributesW(wt);
		if (e == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES) {
			if (a & FILE_ATTRIBUTE_DIRECTORY) {
				/* POSIX lets a dir replace an empty dir. */
				if (RemoveDirectoryW(wt) && MoveFileExW(wf, wt, 0))
					goto done;
			} else if (a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
				SetFileAttributesW(wt, FILE_ATTRIBUTE_NORMAL);
				if (MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING))
					goto done;
				SetFileAttributesW(wt, a);
			}
		}
		errno = errno_from_win(e);
		ret = -1;
	}
  done:
	free(wf);
	free(wt);
	return ret;
}

int w32_chdir(const char *path)
{
	wchar_t *wp = win_path(path), *full;
	DWORD a, n;
	if (!wp)
		return -1;
	a = GetFileAttributesW(wp);
	if (a == INVALID_FILE_ATTRIBUTES) {
		set_errno_win();
		free(wp);
		return -1;
	}
	if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
		free(wp);
		errno = ENOTDIR;
		return -1;
	}
	/* Collapse "." / ".." (win_path() only does that for long paths). */
	n = GetFullPathNameW(wp, 0, NULL, NULL);
	full = n ? malloc((n + 1) * sizeof (wchar_t)) : NULL;
	if (full && GetFullPathNameW(wp, n + 1, full, NULL))
		set_cur_dir(full);
	else
		set_cur_dir(wp);
	free(full);
	free(wp);
	return 0;
}

int w32_fchdir(int fd)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	DWORD n;
	wchar_t *w;
	int ret = 0;
	if (h == INVALID_HANDLE_VALUE || fd_sock(fd) != INVALID_SOCKET) {
		errno = EBADF;
		return -1;
	}
	n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED);
	if (!n)
		return set_errno_win();
	w = malloc((n + 1) * sizeof (wchar_t));
	if (!w) {
		errno = ENOMEM;
		return -1;
	}
	if (!GetFinalPathNameByHandleW(h, w, n + 1, FILE_NAME_NORMALIZED))
		ret = set_errno_win();
	else
		set_cur_dir(w);
	free(w);
	return ret;
}

int setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && getenv(name))
		return 0;
	return _putenv_s(name, value) ? (errno = EINVAL, -1) : 0;
}

int unsetenv(const char *name)
{
	return _putenv_s(name, "") ? (errno = EINVAL, -1) : 0;
}

#ifdef _MSC_VER
/* mingw-w64's runtime provides these; the MSVC C runtime does not
 * (build-win.ps1 -Toolchain msvc, declared in win32\include-msvc). */
int gettimeofday(struct timeval *tv, void *tz)
{
	FILETIME ft;
	unsigned long long t;
	(void)tz;
	GetSystemTimePreciseAsFileTime(&ft);
	/* 100ns ticks since 1601 -> since 1970 */
	t = (((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL;
	tv->tv_sec = (long)(t / 10000000);
	tv->tv_usec = (long)(t % 10000000 / 10);
	return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) {
		errno = EINVAL;
		return -1;
	}
	Sleep((DWORD)(req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000));
	if (rem)
		rem->tv_sec = 0, rem->tv_nsec = 0;
	return 0;
}

int usleep(useconds_t usec)
{
	Sleep((usec + 999) / 1000);
	return 0;
}
#endif

char *w32_getcwd(char *buf, size_t size)
{
	size_t n;
	char *p = posix_path(cur_dir());
	if (!p) {
		errno = ENOMEM;
		return NULL;
	}
	/* "/cygdrive/c/" -> "/cygdrive/c" */
	n = (DWORD)strlen(p);
	if (n > 1 && p[n - 1] == '/')
		p[--n] = '\0';
	if (!buf)
		return p;
	if (strlen(p) + 1 > size) {
		free(p);
		errno = ERANGE;
		return NULL;
	}
	strcpy(buf, p);
	free(p);
	return buf;
}

char *w32_realpath(const char *path, char *resolved)
{
	wchar_t *wp = win_path(path), full[32768];
	char *p;
	if (!wp)
		return NULL;
	if (!GetFullPathNameW(wp, 32768, full, NULL)) {
		free(wp);
		set_errno_win();
		return NULL;
	}
	free(wp);
	if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
		set_errno_win();
		return NULL;
	}
	if (!(p = posix_path(full)))
		return NULL;
	if (!resolved)
		return p;
	strncpy(resolved, p, PATH_MAX - 1);
	resolved[PATH_MAX - 1] = '\0';
	free(p);
	return resolved;
}

int w32_chmod(const char *path, mode_t mode)
{
	wchar_t *wp = win_path(path);
	DWORD a, na;
	int ret = 0;
	prefetch_invalidate();
	if (!wp)
		return -1;
	a = GetFileAttributesW(wp);
	if (a == INVALID_FILE_ATTRIBUTES) {
		ret = set_errno_win();
	} else if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
		na = (mode & S_IWUSR) ? (a & ~FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY);
		if (na != a && !SetFileAttributesW(wp, na))
			ret = set_errno_win();
	}
	free(wp);
	return ret;
}

int w32_fchmod(int fd, mode_t mode)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	FILE_BASIC_INFO bi;
	prefetch_invalidate();
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	if (!GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof bi))
		return 0;  /* not fatal: rsync chmods by name afterwards anyway */
	if (bi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return 0;
	{
		DWORD want = (mode & S_IWUSR) ? (bi.FileAttributes & ~FILE_ATTRIBUTE_READONLY)
					       : (bi.FileAttributes | FILE_ATTRIBUTE_READONLY);
		FILE_BASIC_INFO nb;
		DWORD n;
		wchar_t *w;
		int ret = 0;

		if (want == bi.FileAttributes)
			return 0;
		/* Only the attributes: zero times mean "leave unchanged". */
		memset(&nb, 0, sizeof nb);
		nb.FileAttributes = want ? want : FILE_ATTRIBUTE_NORMAL;
		if (SetFileInformationByHandle(h, FileBasicInfo, &nb, sizeof nb))
			return 0;
		/* The fd may lack FILE_WRITE_ATTRIBUTES (e.g. opened O_RDONLY):
		 * go by the file's path instead. */
		n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED);
		if (!n || !(w = malloc((n + 1) * sizeof (wchar_t))))
			return set_errno_win();
		if (!GetFinalPathNameByHandleW(h, w, n + 1, FILE_NAME_NORMALIZED)
		 || !SetFileAttributesW(w, nb.FileAttributes))
			ret = set_errno_win();
		free(w);
		return ret;
	}
}

int w32_access(const char *path, int mode)
{
	wchar_t *wp = win_path(path);
	DWORD a;
	if (!wp)
		return -1;
	a = GetFileAttributesW(wp);
	free(wp);
	if (a == INVALID_FILE_ATTRIBUTES)
		return set_errno_win();
	if ((mode & 2) && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
		errno = EACCES;
		return -1;
	}
	return 0;
}

static int set_times(const char *path, time_t as, long ans, time_t ms, long mns)
{
	wchar_t *wp = win_path(path);
	HANDLE h;
	FILETIME at, mt;
	int ret = 0;
	prefetch_invalidate();
	if (!wp)
		return -1;
	h = CreateFileW(wp, FILE_WRITE_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	free(wp);
	if (h == INVALID_HANDLE_VALUE)
		return set_errno_win();
	at = unix_to_ft(as, ans);
	mt = unix_to_ft(ms, mns);
	if (!SetFileTime(h, NULL, &at, &mt))
		ret = set_errno_win();
	CloseHandle(h);
	return ret;
}

int w32_utime(const char *path, const struct utimbuf *ut)
{
	time_t now = time(NULL);
	if (!ut)
		return set_times(path, now, 0, now, 0);
	return set_times(path, ut->actime, 0, ut->modtime, 0);
}

int w32_utimes(const char *path, const struct timeval tv[2])
{
	time_t now = time(NULL);
	if (!tv)
		return set_times(path, now, 0, now, 0);
	return set_times(path, tv[0].tv_sec, tv[0].tv_usec * 1000, tv[1].tv_sec, tv[1].tv_usec * 1000);
}

int w32_link(const char *from, const char *to)
{
	wchar_t *wf = win_path(from), *wt;
	int ret = 0;
	prefetch_invalidate();
	if (!wf)
		return -1;
	if (!(wt = win_path(to))) {
		free(wf);
		return -1;
	}
	if (!CreateHardLinkW(wt, wf, NULL))
		ret = set_errno_win();
	free(wf);
	free(wt);
	return ret;
}

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

int w32_symlink(const char *target, const char *path)
{
	wchar_t *wl = win_path(path), *wtgt, *t;
	DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE, a;
	int ret = 0;
	prefetch_invalidate();
	if (!wl)
		return -1;
	/* Keep relative targets relative; only translate /cygdrive/x/. */
	if (target[0] == '/') {
		wtgt = win_path(target);
	} else {
		wtgt = utf8_to_wide(target);
		for (t = wtgt; t && *t; t++) {
			if (*t == L'/')
				*t = L'\\';
		}
	}
	if (!wtgt) {
		free(wl);
		return -1;
	}
	/* Directory-ness of the link is decided by what the target is now
	 * (resolved relative to the link's own directory). */
	if (target[0] == '/')
		a = GetFileAttributesW(wtgt);
	else {
		wchar_t *full = malloc((wcslen(wl) + wcslen(wtgt) + 2) * sizeof (wchar_t)), *slash;
		a = INVALID_FILE_ATTRIBUTES;
		if (full) {
			wcscpy(full, wl);
			slash = wcsrchr(full, L'\\');
			if (slash)
				slash[1] = 0;
			else
				full[0] = 0;
			wcscat(full, wtgt);
			a = GetFileAttributesW(full);
			free(full);
		}
	}
	if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
		flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
	if (!CreateSymbolicLinkW(wl, wtgt, flags)) {
		DWORD e = GetLastError();
		errno = e == ERROR_PRIVILEGE_NOT_HELD ? EPERM : errno_from_win(e);
		ret = -1;
	}
	free(wl);
	free(wtgt);
	return ret;
}

typedef struct {
	ULONG ReparseTag;
	USHORT ReparseDataLength;
	USHORT Reserved;
	union {
		struct {
			USHORT SubstituteNameOffset;
			USHORT SubstituteNameLength;
			USHORT PrintNameOffset;
			USHORT PrintNameLength;
			ULONG Flags;
			WCHAR PathBuffer[1];
		} SymbolicLinkReparseBuffer;
		struct {
			USHORT SubstituteNameOffset;
			USHORT SubstituteNameLength;
			USHORT PrintNameOffset;
			USHORT PrintNameLength;
			WCHAR PathBuffer[1];
		} MountPointReparseBuffer;
	};
} W32_REPARSE_DATA_BUFFER;

#define SYMLINK_FLAG_RELATIVE 1

static ssize_t readlink_handle(HANDLE h, char *buf, size_t size)
{
	union {
		W32_REPARSE_DATA_BUFFER rd;
		char raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
	} u;
	DWORD got;
	WCHAR *pb, *name;
	USHORT off, len;
	int relative = 0;
	char *s;
	size_t n;

	if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, &u, sizeof u, &got, NULL))
		return set_errno_win();
	if (u.rd.ReparseTag == IO_REPARSE_TAG_SYMLINK) {
		pb = u.rd.SymbolicLinkReparseBuffer.PathBuffer;
		relative = u.rd.SymbolicLinkReparseBuffer.Flags & SYMLINK_FLAG_RELATIVE;
		off = u.rd.SymbolicLinkReparseBuffer.SubstituteNameOffset;
		len = u.rd.SymbolicLinkReparseBuffer.SubstituteNameLength;
		if (!len) {
			off = u.rd.SymbolicLinkReparseBuffer.PrintNameOffset;
			len = u.rd.SymbolicLinkReparseBuffer.PrintNameLength;
		}
	} else if (u.rd.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
		pb = u.rd.MountPointReparseBuffer.PathBuffer;
		off = u.rd.MountPointReparseBuffer.SubstituteNameOffset;
		len = u.rd.MountPointReparseBuffer.SubstituteNameLength;
	} else if (u.rd.ReparseTag == IO_REPARSE_TAG_LX_SYMLINK) {
		/* WSL / Cygwin symlink: ULONG version, then the UTF-8 target,
		 * already in POSIX form. */
		const char *tgt = (const char *)&u.rd.Reserved + sizeof (USHORT) + sizeof (ULONG);
		n = u.rd.ReparseDataLength > sizeof (ULONG) ? u.rd.ReparseDataLength - sizeof (ULONG) : 0;
		if (n > size)
			n = size;
		memcpy(buf, tgt, n);
		return (ssize_t)n;
	} else {
		errno = EINVAL;
		return -1;
	}
	name = malloc(len + sizeof (WCHAR));
	if (!name) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(name, (char *)pb + off, len);
	name[len / sizeof (WCHAR)] = 0;
	if (relative) {
		WCHAR *t;
		for (t = name; *t; t++) {
			if (*t == L'\\')
				*t = L'/';
		}
		s = wide_to_utf8(name);
	} else
		s = posix_path(name);
	free(name);
	if (!s) {
		errno = ENOMEM;
		return -1;
	}
	n = strlen(s);
	if (n > size)
		n = size;
	memcpy(buf, s, n);
	free(s);
	return (ssize_t)n;
}

ssize_t w32_readlink(const char *path, char *buf, size_t size)
{
	wchar_t *wp = win_path(path);
	HANDLE h;
	ssize_t n;
	if (!wp)
		return -1;
	h = CreateFileW(wp, FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		free(wp);
		return set_errno_win();
	}
	{
		BY_HANDLE_FILE_INFORMATION fi;
		if (GetFileInformationByHandle(h, &fi) && !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
			char *t = cygwin_sys_symlink_path(wp);
			CloseHandle(h);
			free(wp);
			if (!t) {
				errno = EINVAL;
				return -1;
			}
			n = strlen(t);
			if ((size_t)n > size)
				n = size;
			memcpy(buf, t, n);
			free(t);
			return n;
		}
	}
	free(wp);
	n = readlink_handle(h, buf, size);
	CloseHandle(h);
	return n;
}

int w32_mkstemp(char *tmpl)
{
	static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t len = strlen(tmpl);
	char *x;
	int tries, i;
	prefetch_invalidate();
	if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
		errno = EINVAL;
		return -1;
	}
	x = tmpl + len - 6;
	for (tries = 0; tries < 1000; tries++) {
		int fd;
		for (i = 0; i < 6; i++) {
			unsigned int r;
			rand_s(&r);
			x[i] = chars[r % (sizeof chars - 1)];
		}
		fd = w32_open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0 || errno != EEXIST)
			return fd;
	}
	errno = EEXIST;
	return -1;
}

/* ===================================================================== */
/* directories                                                            */
/* ===================================================================== */

/* opendir() reads the whole directory up front.  Nearly every caller (the
 * sender's file-list scan above all) then lstat()s each entry in turn, and on
 * Windows each of those is an open + ACL read + close of the file: ~25 us
 * even when cached, much more when not, and rsync does them one at a time.
 * So the stats are done by a pool of worker threads instead:
 *
 *  - opendir() hands every entry to the workers, and w32_lstat() takes the
 *    finished result (or does the stat itself if no worker got to it yet).
 *  - Read-ahead: when a worker finds a subdirectory, it also enumerates it
 *    and stats its entries, up to RSYNC_STAT_READAHEAD levels below the
 *    directories rsync has opened and up to PF_BUDGET entries not yet
 *    claimed.  A later opendir() of such a directory takes that snapshot
 *    instead of enumerating again.  Most directories are small, so without
 *    this the workers mostly wait for the next opendir().  rsync walks the
 *    tree depth-first in sorted order, so the read-ahead does too
 *    (pf_keycmp), and drops what rsync has already gone past.
 *
 * Results are handed out at most once and only for a directory rsync has
 * open: a second lstat of the same name goes to the disk again.  Anything
 * this process changes on disk (open for writing, rename, unlink, chmod,
 * utime, ...) drops everything prefetched, and a read-ahead snapshot older
 * than PF_MAX_AGE_MS is not used, so results are never staler than a
 * racing outside writer could make them anyway.  stat() (following links)
 * is never served from here.
 *
 * RSYNC_STAT_THREADS=n: number of worker threads (0 = all of this off).
 * RSYNC_STAT_READAHEAD=n: read-ahead depth (0 = only opened directories). */

#define PF_BUDGET       65536   /* read-ahead entries not yet opened by rsync */
#define PF_MAX_AGE_MS   10000
#define PF_HASH         4096

enum { PF_PENDING, PF_RUNNING, PF_DONE, PF_TAKEN, PF_NONE };

struct pf_entry {
	int state;              /* PF_*, under pf_lock */
	int ret, err;
	int subdir;             /* a directory not yet queued for read-ahead */
	struct w32_stat st;
};

/* A directory snapshot: from opendir() or from read-ahead.  Everything but
 * the immutable path/names is under pf_lock. */
struct pf_dir {
	wchar_t *path;          /* Windows path with a trailing backslash */
	size_t dlen;
	wchar_t **names;        /* raw on-disk names (Cygwin-mapped) */
	size_t n;
	struct pf_entry *pf;    /* NULL: not prefetched */
	size_t next;            /* next entry for a worker to claim */
	size_t cursor;          /* where the main thread's next lookup likely is */
	int refs;               /* DIR, cache, pf_ahead_q, running entries */
	int level;              /* 0: opened by rsync; k: read-ahead k levels below */
	int live;               /* results may still be handed out */
	int cached;             /* read-ahead, waiting for an opendir() */
	ULONGLONG born;         /* GetTickCount64() at enumeration */
	unsigned hash;
	struct pf_dir *hnext;   /* cache hash chain */
	struct pf_dir *cprev, *cnext;  /* cache, oldest first */
	struct pf_dir *onext;   /* pf_open */
};

struct pf_enum_job {
	wchar_t *path;          /* with trailing backslash */
	int level;
};

/* Binary min-heap of pointers to structs whose first member is the path. */
struct pf_heap {
	void **v;
	size_t n, cap;
};

struct w32_DIR {
	struct pf_dir *pd;
	size_t pos;
	struct dirent de;
};

static SRWLOCK pf_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE pf_work = CONDITION_VARIABLE_INIT;  /* new work */
static CONDITION_VARIABLE pf_done = CONDITION_VARIABLE_INIT;  /* an entry finished */
static struct pf_dir *pf_open;            /* opened by rsync and live */
static struct pf_dir *pf_hash[PF_HASH];   /* read-ahead cache */
static struct pf_dir *pf_cache_head, *pf_cache_tail;
static struct pf_heap pf_ahead_q;         /* cached dirs with entries to stat */
static struct pf_heap pf_enum_q;          /* struct pf_enum_job: dirs to enumerate */
static wchar_t *pf_cur;                   /* the last directory rsync opened */
static size_t pf_ahead;                   /* entries in cached dirs */
static unsigned pf_gen;                   /* bumped by prefetch_invalidate() */
static int pf_nthreads = -1, pf_depth = 1000, pf_started;

/* rsync's order for directories (f_name_cmp): byte-wise, "/" a plain
 * character.  The paths here all end in a backslash. */
static int pf_keycmp(const wchar_t *a, const wchar_t *b)
{
	for (;; a++, b++) {
		unsigned ca = *a == L'\\' ? L'/' : *a, cb = *b == L'\\' ? L'/' : *b;
		if (ca != cb)
			return ca < cb ? -1 : 1;
		if (!ca)
			return 0;
	}
}

#define PF_KEY(p) (*(wchar_t **)(p))

static int pf_heap_push(struct pf_heap *h, void *item)
{
	size_t i;
	if (h->n == h->cap) {
		void **nv = realloc(h->v, (h->cap = h->cap ? h->cap * 2 : 256) * sizeof *nv);
		if (!nv)
			return 0;
		h->v = nv;
	}
	for (i = h->n++; i; ) {
		size_t parent = (i - 1) / 2;
		if (pf_keycmp(PF_KEY(h->v[parent]), PF_KEY(item)) <= 0)
			break;
		h->v[i] = h->v[parent];
		i = parent;
	}
	h->v[i] = item;
	return 1;
}

static void *pf_heap_pop(struct pf_heap *h)
{
	void *top, *last;
	size_t i = 0;
	if (!h->n)
		return NULL;
	top = h->v[0];
	last = h->v[--h->n];
	for (;;) {
		size_t c = 2 * i + 1;
		if (c >= h->n)
			break;
		if (c + 1 < h->n && pf_keycmp(PF_KEY(h->v[c + 1]), PF_KEY(h->v[c])) < 0)
			c++;
		if (pf_keycmp(PF_KEY(last), PF_KEY(h->v[c])) <= 0)
			break;
		h->v[i] = h->v[c];
		i = c;
	}
	if (h->n)
		h->v[i] = last;
	return top;
}

/* Has rsync already gone past directory KEY? */
static int pf_passed(const wchar_t *key)
{
	return pf_cur && pf_keycmp(key, pf_cur) < 0;
}

static unsigned pf_hash_of(const wchar_t *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned)*s) * 16777619u;
	return h;
}

static void pf_dir_free(struct pf_dir *pd)
{
	size_t i;
	for (i = 0; i < pd->n; i++)
		free(pd->names[i]);
	free(pd->names);
	free(pd->pf);
	free(pd->path);
	free(pd);
}

/* pf_lock held. */
static void pf_unref(struct pf_dir *pd)
{
	if (--pd->refs == 0)
		pf_dir_free(pd);
}

static void pf_job_free(struct pf_enum_job *j)
{
	free(j->path);
	free(j);
}

/* Enumerate DIR (trailing backslash) into a new pf_dir with refs = 0.
 * Returns NULL with the Win32 error in *ERR. */
static struct pf_dir *pf_enumerate(const wchar_t *dir, DWORD *err)
{
	WIN32_FIND_DATAW fd;
	struct pf_dir *pd;
	size_t dlen = wcslen(dir), cap = 0;
	wchar_t *pat;
	HANDLE h;

	if (!(pd = calloc(1, sizeof *pd)) || !(pd->path = malloc((dlen + 2) * sizeof (wchar_t)))
	 || !(pat = malloc((dlen + 3) * sizeof (wchar_t)))) {
		if (pd)
			free(pd->path);
		free(pd);
		*err = ERROR_NOT_ENOUGH_MEMORY;
		return NULL;
	}
	wcscpy(pd->path, dir);
	if (dlen && dir[dlen - 1] != L'\\')
		pd->path[dlen++] = L'\\', pd->path[dlen] = 0;
	pd->dlen = dlen;
	wcscpy(pat, pd->path);
	wcscat(pat, L"*");
	h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
			     FIND_FIRST_EX_LARGE_FETCH);
	free(pat);
	if (h == INVALID_HANDLE_VALUE) {
		*err = GetLastError();
		if (*err == ERROR_FILE_NOT_FOUND)
			return pd;  /* exists but is completely empty (a bare drive root) */
		free(pd->path);
		free(pd);
		return NULL;
	}
	do {
		wchar_t *nm;
		if (pd->n == cap) {
			wchar_t **nn = realloc(pd->names, (cap = cap ? cap * 2 : 64) * sizeof *nn);
			if (!nn)
				break;
			pd->names = nn;
		}
		if (!(nm = _wcsdup(fd.cFileName)))
			break;
		pd->names[pd->n++] = nm;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	pd->born = GetTickCount64();
	return pd;
}

/* Set up PD's entries for the workers.  Returns the number queued. */
static size_t pf_prepare(struct pf_dir *pd)
{
	int long_form = wcsncmp(pd->path, L"\\\\?\\", 4) == 0;
	size_t i, queued = 0;

	if (!pd->n || !(pd->pf = calloc(pd->n, sizeof *pd->pf)))
		return 0;
	for (i = 0; i < pd->n; i++) {
		const wchar_t *nm = pd->names[i];
		pd->pf[i].state = PF_NONE;
		if (nm[0] == L'.' && (!nm[1] || (nm[1] == L'.' && !nm[2])))
			continue;
		/* win_path() switches to the \\?\ form at 240 chars; don't try
		 * to second-guess that, just leave such names to the normal path. */
		if (!long_form && pd->dlen + wcslen(nm) >= 240)
			continue;
		pd->pf[i].state = PF_PENDING;
		queued++;
	}
	return queued;
}

/* pf_lock held: queue read-ahead of the subdirectory in PD's entry I. */
static void pf_queue_subdir(struct pf_dir *pd, size_t i, int level)
{
	const wchar_t *nm = pd->names[i];
	size_t nlen = wcslen(nm);
	struct pf_enum_job *j;

	pd->pf[i].subdir = 0;
	if (!(j = malloc(sizeof *j)) || !(j->path = malloc((pd->dlen + nlen + 2) * sizeof (wchar_t)))) {
		free(j);
		return;
	}
	memcpy(j->path, pd->path, pd->dlen * sizeof (wchar_t));
	memcpy(j->path + pd->dlen, nm, nlen * sizeof (wchar_t));
	j->path[pd->dlen + nlen] = L'\\';
	j->path[pd->dlen + nlen + 1] = 0;
	j->level = level;
	if (!pf_heap_push(&pf_enum_q, j)) {
		pf_job_free(j);
		return;
	}
	WakeConditionVariable(&pf_work);
}

/* pf_lock held: remove PD from the read-ahead cache. */
static void pf_uncache(struct pf_dir *pd)
{
	struct pf_dir **pp;
	for (pp = &pf_hash[pd->hash % PF_HASH]; *pp; pp = &(*pp)->hnext) {
		if (*pp == pd) {
			*pp = pd->hnext;
			break;
		}
	}
	if (pd->cprev)
		pd->cprev->cnext = pd->cnext;
	else
		pf_cache_head = pd->cnext;
	if (pd->cnext)
		pd->cnext->cprev = pd->cprev;
	else
		pf_cache_tail = pd->cprev;
	pd->cached = 0;
	pf_ahead -= pd->n;
}

/* pf_lock held: drop a cached directory nobody is going to open. */
static void pf_drop_cached(struct pf_dir *pd)
{
	pf_uncache(pd);
	pd->live = 0;
	pf_unref(pd);
}

/* pf_lock held: next pending entry of PD, or NULL. */
static struct pf_entry *pf_next_pending(struct pf_dir *pd)
{
	while (pd->pf && pd->next < pd->n) {
		struct pf_entry *e = &pd->pf[pd->next++];
		if (e->state == PF_PENDING)
			return e;
	}
	return NULL;
}

/* pf_lock held: a worker's next entry to stat (*PDP set), or an enumeration
 * job (*JP set), or NULL for both. */
static struct pf_entry *pf_claim(struct pf_dir **pdp, struct pf_enum_job **jp)
{
	struct pf_dir *pd;
	struct pf_enum_job *j;
	struct pf_entry *e;

	*jp = NULL;
	/* What rsync is reading right now comes first. */
	for (pd = pf_open; pd; pd = pd->onext) {
		if ((e = pf_next_pending(pd)) != NULL) {
			*pdp = pd;
			return e;
		}
	}
	for (;;) {
		/* Of the read-ahead work, whatever rsync will get to first. */
		pd = pf_ahead_q.n ? pf_ahead_q.v[0] : NULL;
		j = pf_enum_q.n ? pf_enum_q.v[0] : NULL;
		if (pd && (!pd->cached || pd->next >= pd->n)) {
			/* opened (then it's in pf_open), dropped, or all claimed */
			pf_heap_pop(&pf_ahead_q);
			pf_unref(pd);
			continue;
		}
		if (pd && pf_passed(pd->path)) {
			pf_heap_pop(&pf_ahead_q);
			pf_drop_cached(pd);
			pf_unref(pd);
			continue;
		}
		if (j && pf_passed(j->path)) {
			pf_heap_pop(&pf_enum_q);
			pf_job_free(j);
			continue;
		}
		if (j && pf_ahead >= PF_BUDGET)
			j = NULL;
		if (j && (!pd || pf_keycmp(j->path, pd->path) < 0)) {
			pf_heap_pop(&pf_enum_q);
			*jp = j;
			return NULL;
		}
		if (!pd)
			return NULL;
		if ((e = pf_next_pending(pd)) != NULL) {
			*pdp = pd;
			return e;
		}
	}
}

static DWORD WINAPI pf_worker(LPVOID arg)
{
	wchar_t *path = NULL;
	size_t cap = 0;

	(void)arg;
	AcquireSRWLockExclusive(&pf_lock);
	for (;;) {
		struct pf_dir *pd;
		struct pf_enum_job *j;
		struct pf_entry *e = pf_claim(&pd, &j);

		if (e) {
			size_t i = e - pd->pf, need = pd->dlen + wcslen(pd->names[i]) + 1;
			e->state = PF_RUNNING;
			pd->refs++;
			ReleaseSRWLockExclusive(&pf_lock);
			if (need > cap) {
				free(path);
				path = malloc((cap = need + 64) * sizeof (wchar_t));
			}
			if (path) {
				memcpy(path, pd->path, pd->dlen * sizeof (wchar_t));
				wcscpy(path + pd->dlen, pd->names[i]);
				errno = 0;
				e->ret = stat_wide(path, &e->st, 0);
				e->err = errno;
			} else {
				cap = 0;
				e->ret = -1;
				e->err = ENOMEM;
			}
			AcquireSRWLockExclusive(&pf_lock);
			e->state = PF_DONE;
			if (e->ret == 0 && S_ISDIR(e->st.st_mode) && pd->live) {
				if (pd->level < pf_depth)
					pf_queue_subdir(pd, i, pd->level + 1);
				else
					e->subdir = 1;  /* queued if rsync opens PD */
			}
			WakeAllConditionVariable(&pf_done);
			pf_unref(pd);
			continue;
		}

		if (j) {
			unsigned gen = pf_gen;
			DWORD err;
			ReleaseSRWLockExclusive(&pf_lock);
			if ((pd = pf_enumerate(j->path, &err)) != NULL)
				pf_prepare(pd);
			AcquireSRWLockExclusive(&pf_lock);
			if (pd && gen == pf_gen && !pf_passed(pd->path)) {
				pd->hash = pf_hash_of(pd->path);
				pd->level = j->level;
				pd->live = pd->cached = 1;
				pd->refs = 1;  /* the cache's */
				pd->hnext = pf_hash[pd->hash % PF_HASH];
				pf_hash[pd->hash % PF_HASH] = pd;
				pd->cprev = pf_cache_tail;
				if (pf_cache_tail)
					pf_cache_tail->cnext = pd;
				else
					pf_cache_head = pd;
				pf_cache_tail = pd;
				pf_ahead += pd->n;
				if (pd->pf && pf_heap_push(&pf_ahead_q, pd)) {
					pd->refs++;
					WakeAllConditionVariable(&pf_work);
				}
			} else if (pd)
				pf_dir_free(pd);
			pf_job_free(j);
			continue;
		}

		SleepConditionVariableSRW(&pf_work, &pf_lock, INFINITE, 0);
	}
	return 0;
}

static int prefetch_threads(void)
{
	if (pf_nthreads < 0) {
		const char *s = getenv("RSYNC_STAT_THREADS");
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		if (s && *s)
			pf_nthreads = atoi(s);
		else
			pf_nthreads = si.dwNumberOfProcessors > 8 ? 8 : (int)si.dwNumberOfProcessors;
		if (pf_nthreads < 0)
			pf_nthreads = 0;
		if (pf_nthreads > 64)
			pf_nthreads = 64;
		if ((s = getenv("RSYNC_STAT_READAHEAD")) && *s)
			pf_depth = atoi(s);
	}
	if (pf_nthreads && !pf_started) {
		int t;
		/* Lazily-initialised statics the workers read. */
		world_sid();
		null_sid();
		for (t = 0; t < pf_nthreads; t++) {
			HANDLE th = CreateThread(NULL, 256 * 1024, pf_worker, NULL,
						 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
			if (!th)
				break;
			CloseHandle(th);
			pf_started++;
		}
		if (!pf_started)
			pf_nthreads = 0;
	}
	return pf_nthreads;
}

/* pf_lock held: rsync opened PD, so it is level 0 now; queue the read-ahead
 * of its subdirectories that were found beyond the depth limit. */
static void pf_open_link(struct pf_dir *pd)
{
	size_t i;
	pd->level = 0;
	pd->live = 1;
	pd->onext = pf_open;
	pf_open = pd;
	if (pf_depth > 0 && pd->pf) {
		for (i = 0; i < pd->n; i++) {
			if (pd->pf[i].subdir)
				pf_queue_subdir(pd, i, 1);
		}
	}
	WakeAllConditionVariable(&pf_work);
}

/* pf_lock held: stop handing out PD's results. */
static void pf_open_unlink(struct pf_dir *pd)
{
	struct pf_dir **pp;
	for (pp = &pf_open; *pp; pp = &(*pp)->onext) {
		if (*pp == pd) {
			*pp = pd->onext;
			break;
		}
	}
	pd->live = 0;
}

/* The file system is about to change: drop everything prefetched. */
static void prefetch_invalidate(void)
{
	if (!pf_started)
		return;
	AcquireSRWLockExclusive(&pf_lock);
	pf_gen++;
	while (pf_open)
		pf_open_unlink(pf_open);
	while (pf_cache_head)
		pf_drop_cached(pf_cache_head);
	while (pf_enum_q.n)
		pf_job_free(pf_heap_pop(&pf_enum_q));
	/* pf_ahead_q now holds only dropped dirs; pf_claim() releases them. */
	ReleaseSRWLockExclusive(&pf_lock);
}

/* lstat() from the prefetched results.  Returns 1 with *ST / *RET / errno
 * set if WP was served from there, 0 if the caller must stat it itself. */
static int prefetch_take(const wchar_t *wp, int follow, struct w32_stat *st, int *ret)
{
	struct pf_dir *pd;
	int got = 0;

	if (!pf_open || follow)
		return 0;
	AcquireSRWLockExclusive(&pf_lock);
	for (pd = pf_open; pd; pd = pd->onext) {
		const wchar_t *name;
		size_t k, i;
		struct pf_entry *e;

		if (!pd->pf || wcsncmp(wp, pd->path, pd->dlen) != 0)
			continue;
		name = wp + pd->dlen;
		if (!*name || wcschr(name, L'\\'))
			continue;
		/* Lookups come in readdir order, so this is usually k == 0. */
		for (k = 0; k < pd->n; k++) {
			i = pd->cursor + k;
			if (i >= pd->n)
				i -= pd->n;
			if (wcscmp(pd->names[i], name) == 0)
				break;
		}
		if (k == pd->n)
			break;
		pd->cursor = i + 1 < pd->n ? i + 1 : 0;
		e = &pd->pf[i];
		if (e->state == PF_PENDING) {
			/* No worker has it yet: cheaper to do it ourselves. */
			e->state = PF_TAKEN;
			break;
		}
		while (e->state == PF_RUNNING)
			SleepConditionVariableSRW(&pf_done, &pf_lock, INFINITE, 0);
		if (e->state == PF_DONE) {
			e->state = PF_TAKEN;
			*st = e->st;
			*ret = e->ret;
			if (e->ret < 0)
				errno = e->err;
			got = 1;
		}
		break;
	}
	ReleaseSRWLockExclusive(&pf_lock);
	return got;
}

/* pf_lock held: drop read-ahead that rsync went past without opening
 * (excluded directories, ...) or that got too old.  The cache is in
 * enumeration order, which is about rsync's order. */
static void pf_evict(void)
{
	ULONGLONG now = GetTickCount64();
	while (pf_cache_head && (pf_passed(pf_cache_head->path)
			      || now - pf_cache_head->born >= PF_MAX_AGE_MS))
		pf_drop_cached(pf_cache_head);
}

DIR *w32_opendir(const char *path)
{
	wchar_t *wp = win_path(path), *key;
	struct pf_dir *pd = NULL;
	size_t len;
	DIR *d;
	DWORD e;

	if (!wp)
		return NULL;
	len = wcslen(wp);
	if (!(d = calloc(1, sizeof *d)) || !(key = malloc((len + 2) * sizeof (wchar_t)))) {
		free(d);
		free(wp);
		errno = ENOMEM;
		return NULL;
	}
	wcscpy(key, wp);
	if (len && key[len - 1] != L'\\')
		wcscat(key, L"\\");

	if (prefetch_threads() > 0) {
		unsigned h = pf_hash_of(key);
		wchar_t *cur = _wcsdup(key);
		AcquireSRWLockExclusive(&pf_lock);
		if (cur) {
			free(pf_cur);
			pf_cur = cur;
		}
		for (pd = pf_hash[h % PF_HASH]; pd; pd = pd->hnext) {
			if (pd->hash == h && wcscmp(pd->path, key) == 0)
				break;
		}
		if (pd) {
			pf_uncache(pd);  /* its cache reference becomes the DIR's */
			if (GetTickCount64() - pd->born >= PF_MAX_AGE_MS) {
				pd->live = 0;
				pf_unref(pd);
				pd = NULL;
			} else
				pf_open_link(pd);
		}
		pf_evict();
		ReleaseSRWLockExclusive(&pf_lock);
	}

	if (!pd) {
		if (!(pd = pf_enumerate(key, &e))) {
			if (e == ERROR_PATH_NOT_FOUND || e == ERROR_DIRECTORY || e == ERROR_INVALID_NAME) {
				DWORD a = GetFileAttributesW(wp);
				if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
					e = ERROR_DIRECTORY;
			}
			free(key);
			free(wp);
			free(d);
			errno = errno_from_win(e);
			return NULL;
		}
		pd->refs = 1;
		if (pf_nthreads > 0 && pf_prepare(pd)) {
			AcquireSRWLockExclusive(&pf_lock);
			pf_open_link(pd);
			ReleaseSRWLockExclusive(&pf_lock);
		}
	}
	free(key);
	free(wp);
	d->pd = pd;
	return d;
}

struct dirent *w32_readdir(DIR *d)
{
	wchar_t *nm;
	if (d->pos >= d->pd->n) {
		errno = 0;
		return NULL;
	}
	nm = _wcsdup(d->pd->names[d->pos++]);
	if (!nm) {
		errno = ENOMEM;
		return NULL;
	}
	cyg_unmap(nm);
	if (!WideCharToMultiByte(CP_UTF8, 0, nm, -1, d->de.d_name,
				 sizeof d->de.d_name, NULL, NULL))
		d->de.d_name[0] = '\0';
	free(nm);
	d->de.d_ino = 0;
	return &d->de;
}

int w32_closedir(DIR *d)
{
	struct pf_dir *pd = d->pd;
	AcquireSRWLockExclusive(&pf_lock);
	if (pd->live)
		pf_open_unlink(pd);
	pf_unref(pd);  /* workers still busy with one of its entries hold refs */
	ReleaseSRWLockExclusive(&pf_lock);
	free(d);
	return 0;
}

/* ===================================================================== */
/* users / groups                                                         */
/* ===================================================================== */

static char *current_user(void)
{
	static char name[256];
	if (!name[0]) {
		wchar_t w[256];
		DWORD n = 256;
		if (GetUserNameW(w, &n))
			WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof name, NULL, NULL);
		else
			strcpy(name, "user");
	}
	return name;
}

struct passwd *getpwuid(uid_t uid)
{
	static struct passwd pw;
	if (uid != W32_UID)
		return NULL;
	pw.pw_name = current_user();
	pw.pw_passwd = "*";
	pw.pw_uid = W32_UID;
	pw.pw_gid = W32_UID;
	pw.pw_gecos = pw.pw_name;
	pw.pw_dir = getenv("USERPROFILE");
	pw.pw_shell = "";
	return &pw;
}

struct passwd *getpwnam(const char *name)
{
	if (name && _stricmp(name, current_user()) == 0)
		return getpwuid(W32_UID);
	return NULL;
}

struct group *getgrgid(gid_t gid)
{
	(void)gid;
	return NULL;
}

struct group *getgrnam(const char *name)
{
	(void)name;
	return NULL;
}

char *getpass(const char *prompt)
{
	static char buf[512];
	wchar_t w[256];
	int n = 0;
	wint_t c;

	if (replay_active && replay_password) {
		/* replayed run: the parent already asked (see replay.inc.c) */
		strncpy(buf, replay_password, sizeof buf - 1);
		return buf;
	}
	fputs(prompt, stderr);
	fflush(stderr);
	while ((c = _getwch()) != L'\r' && c != L'\n' && c != WEOF) {
		if (c == 3)  /* Ctrl-C */
			exit(1);
		if (c == 8) {
			if (n)
				n--;
			continue;
		}
		if (n < 255)
			w[n++] = (wchar_t)c;
	}
	w[n] = 0;
	fputs("\n", stderr);
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, NULL, NULL))
		buf[0] = '\0';
	SecureZeroMemory(w, sizeof w);
	free(remembered_password);
	remembered_password = strdup(buf);
	return buf;
}

/* ===================================================================== */
/* signals / processes                                                    */
/* ===================================================================== */

#define MAX_CHILDREN 64
static struct {
	pid_t pid;
	HANDLE h;
} children[MAX_CHILDREN];

static w32_sighandler_t fake_handlers[64];

w32_sighandler_t w32_signal(int sig, w32_sighandler_t h)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
	case SIGABRT:
	case SIGFPE:
	case SIGILL:
	case SIGSEGV:
	case SIGBREAK:
		return signal(sig, h);
	default:
		if (sig > 0 && sig < 64) {
			w32_sighandler_t old = fake_handlers[sig];
			fake_handlers[sig] = h;
			return old;
		}
		return SIG_ERR;
	}
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
	w32_sighandler_t prev = SIG_DFL;
	if (act) {
		prev = w32_signal(sig, act->sa_handler);
		if (prev == SIG_ERR) {
			errno = EINVAL;
			return -1;
		}
	}
	if (old) {
		memset(old, 0, sizeof *old);
		old->sa_handler = prev;
	}
	return 0;
}

static int child_slot(pid_t pid)
{
	int i;
	for (i = 0; i < MAX_CHILDREN; i++) {
		if (children[i].h && children[i].pid == pid)
			return i;
	}
	return -1;
}

/* SIGUSR1/SIGUSR2 between rsync processes: each process owns two named
 * auto-reset events, and a watcher thread runs the registered handler when
 * one is set.  rsync's handlers only set flags (or exit), and poll() is
 * woken with EINTR so the main thread acts on them promptly. */
static HANDLE sig_events[2];

static void sig_event_name(wchar_t *buf, size_t n, DWORD pid, int sig)
{
	_snwprintf(buf, n, L"Local\\rsync-w32-%lu-%s", (unsigned long)pid, sig == SIGUSR1 ? L"usr1" : L"usr2");
}

static DWORD WINAPI sig_watcher(LPVOID arg)
{
	(void)arg;
	for (;;) {
		DWORD r = WaitForMultipleObjects(2, sig_events, FALSE, INFINITE);
		int sig;
		if (r == WAIT_OBJECT_0)
			sig = SIGUSR1;
		else if (r == WAIT_OBJECT_0 + 1)
			sig = SIGUSR2;
		else
			return 0;
		if (fake_handlers[sig] && fake_handlers[sig] != SIG_IGN && fake_handlers[sig] != SIG_DFL)
			fake_handlers[sig](sig);
		else if (fake_handlers[sig] != SIG_IGN)
			_exit(128 + sig);
		InterlockedExchange(&w32_sig_pending, 1);
	}
}

static void sig_init(void)
{
	wchar_t name[64];
	DWORD pid = GetCurrentProcessId();
	HANDLE th;
	sig_event_name(name, 64, pid, SIGUSR1);
	sig_events[0] = CreateEventW(NULL, FALSE, FALSE, name);
	sig_event_name(name, 64, pid, SIGUSR2);
	sig_events[1] = CreateEventW(NULL, FALSE, FALSE, name);
	if (sig_events[0] && sig_events[1] && (th = CreateThread(NULL, 0, sig_watcher, NULL, 0, NULL)))
		CloseHandle(th);
}

int kill(pid_t pid, int sig)
{
	int i;
	if (pid == (pid_t)GetCurrentProcessId() || pid == 0) {
		if (sig == 0)
			return 0;
		if (sig > 0 && sig < 64 && fake_handlers[sig] && fake_handlers[sig] != SIG_IGN
		 && fake_handlers[sig] != SIG_DFL) {
			fake_handlers[sig](sig);
			return 0;
		}
		return raise(sig);
	}
	/* Only ever signal our own children: a pid we don't know about is
	 * likely stale (or a placeholder from a replayed run), and must not hit
	 * an unrelated process. */
	if (pid < 0 || (i = child_slot(pid)) < 0) {
		errno = ESRCH;
		return -1;
	}
	if (WaitForSingleObject(children[i].h, 0) != WAIT_TIMEOUT) {
		if (sig == 0)
			return 0;  /* exited but not yet reaped: still "exists" */
		return 0;
	}
	if (sig == 0)
		return 0;
	if (sig == SIGUSR1 || sig == SIGUSR2) {
		wchar_t name[64];
		HANDLE ev;
		sig_event_name(name, 64, (DWORD)pid, sig);
		if ((ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) != NULL) {
			SetEvent(ev);
			CloseHandle(ev);
			return 0;
		}
		/* Not an rsync (e.g. ssh): the default action is to terminate. */
	}
	TerminateProcess(children[i].h, 128 + sig);
	return 0;
}

static pid_t reap(int i, int *status)
{
	DWORD code = 0;
	pid_t pid = children[i].pid;
	GetExitCodeProcess(children[i].h, &code);
	CloseHandle(children[i].h);
	children[i].h = NULL;
	if (status)
		*status = (int)((code & 0xff) << 8);
	return pid;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
	DWORD timeout = (options & WNOHANG) ? 0 : INFINITE;
	int i;

	if (pid > 0) {
		if ((i = child_slot(pid)) < 0) {
			errno = ECHILD;
			return -1;
		}
		if (WaitForSingleObject(children[i].h, timeout) != WAIT_OBJECT_0)
			return 0;
		return reap(i, status);
	} else {
		HANDLE hs[MAX_CHILDREN];
		int idx[MAX_CHILDREN], n = 0;
		DWORD r;
		for (i = 0; i < MAX_CHILDREN; i++) {
			if (children[i].h) {
				hs[n] = children[i].h;
				idx[n++] = i;
			}
		}
		if (!n) {
			errno = ECHILD;
			return -1;
		}
		r = WaitForMultipleObjects(n, hs, FALSE, timeout);
		if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + (DWORD)n)
			return reap(idx[r - WAIT_OBJECT_0], status);
		return 0;
	}
}

pid_t fork(void)
{
	errno = ENOSYS;
	return -1;
}

unsigned int alarm(unsigned int secs)
{
	(void)secs;
	return 0;
}

/* Quote one argument per the MSVC CRT / CommandLineToArgvW rules. */
static void append_quoted(wchar_t **buf, size_t *len, size_t *cap, const wchar_t *arg)
{
	size_t need = wcslen(arg) * 2 + 4;
	const wchar_t *p;
	int quote = !*arg || wcspbrk(arg, L" \t\n\v\"") != NULL;
	wchar_t *o;

	if (*len + need + 1 > *cap) {
		*cap = (*len + need + 1) * 2;
		*buf = realloc(*buf, *cap * sizeof (wchar_t));
	}
	o = *buf + *len;
	if (*len)
		*o++ = L' ';
	if (!quote) {
		wcscpy(o, arg);
		*len = (o - *buf) + wcslen(arg);
		return;
	}
	*o++ = L'"';
	for (p = arg; ; p++) {
		size_t bs = 0;
		while (*p == L'\\') {
			p++;
			bs++;
		}
		if (!*p) {
			while (bs--) {
				*o++ = L'\\';
				*o++ = L'\\';
			}
			break;
		}
		if (*p == L'"') {
			bs = bs * 2 + 1;
			while (bs--)
				*o++ = L'\\';
			*o++ = L'"';
		} else {
			while (bs--)
				*o++ = L'\\';
			*o++ = *p;
		}
	}
	*o++ = L'"';
	*o = 0;
	*len = o - *buf;
}

struct relay {
	SOCKET s;
	HANDLE h;
};

/* rsync -> child's stdin */
static DWORD WINAPI relay_to_child(LPVOID arg)
{
	struct relay *r = arg;
	char *buf = malloc(65536);
	for (;;) {
		int n = recv(r->s, buf, 65536, 0);
		char *p = buf;
		if (n <= 0)
			break;
		while (n > 0) {
			DWORD wrote;
			if (!WriteFile(r->h, p, (DWORD)n, &wrote, NULL))
				goto done;
			p += wrote;
			n -= wrote;
		}
	}
  done:
	CloseHandle(r->h);
	closesocket(r->s);
	free(buf);
	free(r);
	return 0;
}

/* child's stdout -> rsync */
static DWORD WINAPI relay_from_child(LPVOID arg)
{
	struct relay *r = arg;
	char *buf = malloc(65536);
	for (;;) {
		DWORD got;
		char *p = buf;
		if (!ReadFile(r->h, buf, 65536, &got, NULL) || got == 0)
			break;
		while (got > 0) {
			int n = send(r->s, p, (int)got, 0);
			if (n <= 0)
				goto done;
			p += n;
			got -= n;
		}
	}
  done:
	shutdown(r->s, SD_SEND);
	closesocket(r->s);
	CloseHandle(r->h);
	free(buf);
	free(r);
	return 0;
}

pid_t w32_spawn_piped(char **command, int *f_in, int *f_out)
{
	SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
	HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, err = NULL;
	HANDLE inherit[3];
	int ninherit = 0, i, to_fds[2] = { -1, -1 }, from_fds[2] = { -1, -1 };
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T attrsz = 0;
	wchar_t *cmdline = NULL;
	size_t len = 0, cap = 0;
	struct relay *r1, *r2;
	HANDLE th;
	int slot;

	if (replay_active && replay_feed != INVALID_SOCKET) {
		/* Replayed run (see replay.inc.c): don't start a second ssh; read
		 * the parent's feed instead and discard what we'd send. */
		int nfd;
		if ((*f_in = sock_to_fd(replay_feed)) < 0)
			return -1;
		replay_feed = INVALID_SOCKET;
		if ((nfd = _open("NUL", _O_WRONLY | _O_BINARY | _O_NOINHERIT)) < 0)
			return -1;
		fd_null[nfd] = 1;
		*f_out = nfd;
		return (pid_t)0x3ffffff0;
	}

	for (slot = 0; slot < MAX_CHILDREN && children[slot].h; slot++) {}
	if (slot == MAX_CHILDREN) {
		errno = EAGAIN;
		return -1;
	}

	for (i = 0; command[i]; i++) {
		wchar_t *w;
		if (i == 0 && command[0][0] == '/') {
			w = win_path(command[0]);  /* e.g. /cygdrive/c/.../ssh.exe */
		} else
			w = utf8_to_wide(command[i]);
		if (!w) {
			free(cmdline);
			errno = ENOMEM;
			return -1;
		}
		append_quoted(&cmdline, &len, &cap, w);
		free(w);
	}

	if (!CreatePipe(&in_r, &in_w, &sa, 65536) || !CreatePipe(&out_r, &out_w, &sa, 65536)) {
		set_errno_win();
		goto fail;
	}
	SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
	inherit[ninherit++] = in_r;
	inherit[ninherit++] = out_w;
	{
		HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
		if (e && e != INVALID_HANDLE_VALUE
		 && DuplicateHandle(GetCurrentProcess(), e, GetCurrentProcess(), &err, 0, TRUE,
				    DUPLICATE_SAME_ACCESS))
			inherit[ninherit++] = err;
	}

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = in_r;
	si.StartupInfo.hStdOutput = out_w;
	si.StartupInfo.hStdError = err;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attrsz);
	si.lpAttributeList = malloc(attrsz);
	if (!si.lpAttributeList || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrsz)
	 || !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				       inherit, ninherit * sizeof (HANDLE), NULL, NULL)) {
		set_errno_win();
		goto fail;
	}

	if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT,
			    NULL, NULL, &si.StartupInfo, &pi)) {
		DWORD e = GetLastError();
		char *c8 = wide_to_utf8(cmdline);
		fprintf(stderr, "rsync: failed to run \"%s\": Windows error %lu\n", c8 ? c8 : "?", e);
		free(c8);
		errno = errno_from_win(e);
		DeleteProcThreadAttributeList(si.lpAttributeList);
		goto fail;
	}
	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);
	si.lpAttributeList = NULL;
	CloseHandle(pi.hThread);
	CloseHandle(in_r);
	CloseHandle(out_w);
	if (err)
		CloseHandle(err);
	in_r = out_w = err = NULL;
	free(cmdline);
	cmdline = NULL;

	if (w32_socketpair(AF_UNIX, SOCK_STREAM, 0, to_fds) < 0
	 || w32_socketpair(AF_UNIX, SOCK_STREAM, 0, from_fds) < 0) {
		TerminateProcess(pi.hProcess, 255);
		CloseHandle(pi.hProcess);
		goto fail;
	}

	r1 = malloc(sizeof *r1);
	r2 = malloc(sizeof *r2);
	r1->s = fd_detach(to_fds[1]);
	r1->h = in_w;
	r2->s = fd_detach(from_fds[1]);
	r2->h = out_r;
	in_w = out_r = NULL;
	if ((th = CreateThread(NULL, 0, relay_to_child, r1, 0, NULL)) != NULL)
		CloseHandle(th);
	if ((th = CreateThread(NULL, 0, relay_from_child, r2, 0, NULL)) != NULL)
		CloseHandle(th);

	children[slot].pid = (pid_t)pi.dwProcessId;
	children[slot].h = pi.hProcess;
	*f_out = to_fds[0];
	*f_in = from_fds[0];
	fd_record[from_fds[0]] = 1;
	return (pid_t)pi.dwProcessId;

  fail:
	if (si.lpAttributeList)
		free(si.lpAttributeList);
	free(cmdline);
	if (in_r) CloseHandle(in_r);
	if (in_w) CloseHandle(in_w);
	if (out_r) CloseHandle(out_r);
	if (out_w) CloseHandle(out_w);
	if (err) CloseHandle(err);
	if (to_fds[0] >= 0) w32_close(to_fds[0]);
	if (to_fds[1] >= 0) w32_close(to_fds[1]);
	return -1;
}

#include "replay.inc.c"

void w32_stop_recording(void)
{
	rec_enabled = 0;
	memset(fd_record, 0, sizeof fd_record);
	free(rec_buf);
	rec_buf = NULL;
	rec_len = rec_cap = 0;
}

/* ===================================================================== */
/* startup                                                                */
/* ===================================================================== */

static void invalid_param_handler(const wchar_t *e, const wchar_t *f, const wchar_t *file,
				  unsigned int line, uintptr_t r)
{
	(void)e; (void)f; (void)file; (void)line; (void)r;
}

/* Is this a local Windows path ("C:\x", "C:/x", "C:") that rsync must see in
 * its /cygdrive/c/x form? */
static int is_drive_path(const char *a)
{
	return isalpha((unsigned char)a[0]) && a[1] == ':'
	    && (a[2] == '\\' || a[2] == '/' || a[2] == '\0');
}

static char *convert_arg(const char *a)
{
	char *out, *p;
	if (is_drive_path(a)) {
		out = malloc(strlen(a) + 12);
		sprintf(out, "/cygdrive/%c%s%s", tolower((unsigned char)a[0]),
			a[2] ? "" : "/", a + 2);
	} else if (!strchr(a, ':'))
		out = strdup(a);
	else
		return strdup(a);
	for (p = out; *p; p++) {
		if (*p == '\\')
			*p = '/';
	}
	return out;
}

void w32_init(int *argc, char ***argv)
{
	WSADATA wsa;
	int wargc, i, keep_next = 0;
	wchar_t **wargv;
	char **nargv;

	_set_invalid_parameter_handler(invalid_param_handler);
	sock_tab_init();
	WSAStartup(MAKEWORD(2, 2), &wsa);
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	_setmode(0, _O_BINARY);
	_setmode(1, _O_BINARY);
	_setmode(2, _O_BINARY);

	/* What a replay "fork" needs to re-create this process later. */
	{
		DWORD n = GetCurrentDirectoryW(0, NULL);
		wchar_t spec[512];
		startup_cwd = malloc((n + 1) * sizeof (wchar_t));
		if (!startup_cwd || !GetCurrentDirectoryW(n + 1, startup_cwd)) {
			free(startup_cwd);
			startup_cwd = NULL;
		}
		startup_cmdline = _wcsdup(GetCommandLineW());
		sig_init();
		atexit(flush_stdout_relay);
		if (GetEnvironmentVariableW(REPLAY_ENV, spec, 512))
			replay_init(spec);
	}

	/* Rebuild argv from the real (UTF-16) command line, so non-ASCII file
	 * names survive whatever the ANSI code page is. */
	wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	if (!wargv)
		return;
	nargv = calloc(wargc + 1, sizeof (char *));
	for (i = 0; i < wargc; i++) {
		char *a = wide_to_utf8(wargv[i]);
		if (i == 0) {
			nargv[i] = a;
			continue;
		}
		if (keep_next) {
			/* value of -e / --rsync-path / -M: a command, not a local path */
			keep_next = 0;
			nargv[i] = a;
			continue;
		}
		if (a[0] == '-' && a[1] != '-' && a[1] && strchr("eM", a[strlen(a) - 1]))
			keep_next = 1;  /* "-e CMD", "-avze CMD", "-M OPT" */
		else if (strcmp(a, "--rsh") == 0 || strcmp(a, "--rsync-path") == 0
		      || strcmp(a, "--remote-option") == 0)
			keep_next = 1;
		if (a[0] == '-' && a[1] == '-' && strchr(a, '=')
		 && strncmp(a, "--rsh=", 6) != 0 && strncmp(a, "--rsync-path=", 13) != 0
		 && strncmp(a, "--remote-option=", 16) != 0) {
			/* --opt=C:\path  ->  --opt=/cygdrive/c/path */
			char *eq = strchr(a, '=');
			if (is_drive_path(eq + 1)) {
				char *v = convert_arg(eq + 1);
				char *n = malloc((eq - a) + strlen(v) + 2);
				memcpy(n, a, eq - a + 1);
				strcpy(n + (eq - a) + 1, v);
				free(v);
				free(a);
				a = n;
			}
			nargv[i] = a;
		} else if (a[0] == '-')
			nargv[i] = a;
		else {
			nargv[i] = convert_arg(a);
			free(a);
		}
	}
	LocalFree(wargv);
	*argc = wargc;
	*argv = nargv;

	if (!replay_active) {
		for (i = 1; i < wargc; i++) {
			if (strcmp(nargv[i], "--server") == 0) {
				setup_server_stdio();
				break;
			}
		}
	}
}
