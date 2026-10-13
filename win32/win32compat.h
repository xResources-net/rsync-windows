/* POSIX compatibility layer for a native Windows build of rsync.
 *
 * Design notes (see win32compat.c for the implementation):
 *  - Paths: rsync keeps working with POSIX-style paths internally.  Windows
 *    drive paths are presented to it as /cygdrive/<x>/... (so "is absolute"
 *    == starts with '/', exactly as on Cygwin), and translated back to wide
 *    "X:\..." paths (with a \\?\ prefix when long) at the syscall boundary.
 *    All file APIs are the W variants; rsync sees UTF-8.
 *  - Sockets: winsock SOCKETs are given ordinary CRT fd numbers (a reserved
 *    NUL handle) and tracked in a side table, so read/write/close/poll/select
 *    keep working on plain ints.
 *  - Child processes (the remote shell, e.g. Windows' own OpenSSH ssh.exe):
 *    spawned with CreateProcessW on real anonymous pipes.  rsync's side of the
 *    connection is a loopback socket pair, pumped to/from the pipes by two
 *    relay threads, because rsync's I/O loop needs poll() and Windows can only
 *    poll sockets.
 *  - fork() does not exist; see win32compat.c for how the forking roles are
 *    handled.
 */

#ifndef RSYNC_WIN32COMPAT_H
#define RSYNC_WIN32COMPAT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif
#define _CRT_RAND_S 1

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <process.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utime.h>
#include <unistd.h>
#include <malloc.h>
#include <limits.h>

/* winsock/windows.h macro pollution that collides with rsync identifiers */
#undef ERROR
#undef DELETE
#undef IGNORE
#undef interface
#undef small
#undef min
#undef max

/* ---- types ---------------------------------------------------------- */
typedef unsigned int w32_uid_t;
#define uid_t w32_uid_t
#define gid_t w32_uid_t
typedef long long w32_ino_t;
#define ino_t w32_ino_t
typedef unsigned long long w32_dev_t;
#define dev_t w32_dev_t
typedef unsigned int w32_mode_t;
#define mode_t w32_mode_t
typedef unsigned int nlink_t;
typedef unsigned int id_t;
typedef long long blkcnt_t;
typedef long blksize_t;

/* ---- mode bits ------------------------------------------------------ */
#undef S_IFMT
#undef S_IFDIR
#undef S_IFCHR
#undef S_IFIFO
#undef S_IFREG
#undef S_IFBLK
#undef S_IFLNK
#undef S_IFSOCK
#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#undef _S_IFMT
#define _S_IFMT S_IFMT
#undef S_ISDIR
#undef S_ISREG
#undef S_ISCHR
#undef S_ISBLK
#undef S_ISFIFO
#undef S_ISLNK
#undef S_ISSOCK
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#ifndef S_ISUID
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#endif
#undef S_IRWXU
#undef S_IRUSR
#undef S_IWUSR
#undef S_IXUSR
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#ifndef S_IRWXG
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#endif
#ifndef S_IEXEC
#define S_IEXEC S_IXUSR
#endif

/* ---- stat ----------------------------------------------------------- */
struct w32_stat {
	dev_t st_dev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	dev_t st_rdev;
	long long st_size;
	time_t st_atime;
	time_t st_mtime;
	time_t st_ctime;
	long st_atimensec;
	long st_mtimensec;
	long st_ctimensec;
	time_t st_birthtime;   /* NTFS creation time */
	blksize_t st_blksize;
	blkcnt_t st_blocks;
};
#define stat w32_stat
#define lstat w32_lstat
#define fstat w32_fstat
int w32_stat(const char *path, struct w32_stat *st);
int w32_lstat(const char *path, struct w32_stat *st);
int w32_fstat(int fd, struct w32_stat *st);

/* ---- open flags ----------------------------------------------------- */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x40000000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
#define F_GETFL 3
#define F_SETFL 4
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1

/* ---- fd / file API redirection ------------------------------------- */
#define open w32_open
#define read(fd, b, n) w32_read(fd, b, n)
#define write(fd, b, n) w32_write(fd, b, n)
#define close(fd) w32_close(fd)
#define lseek(fd, o, w) _lseeki64(fd, o, w)
#define ftruncate(fd, sz) w32_ftruncate(fd, sz)
#define fsync(fd) _commit(fd)
#define fcntl w32_fcntl
#define dup(fd) w32_dup(fd)
#define dup2(a, b) w32_dup2(a, b)
#define pipe(fds) w32_pipe(fds)
#define isatty(fd) _isatty(fd)
int w32_open(const char *path, int flags, ...);
ssize_t w32_read(int fd, void *buf, size_t len);
ssize_t w32_write(int fd, const void *buf, size_t len);
int w32_close(int fd);
int w32_ftruncate(int fd, long long size);
int w32_fcntl(int fd, int cmd, ...);
int w32_dup(int fd);
int w32_dup2(int fd, int fd2);
int w32_pipe(int fds[2]);

#define unlink w32_unlink
#define rmdir w32_rmdir
#define mkdir(p, m) w32_mkdir(p, m)
#define rename w32_rename
#define chdir w32_chdir
#define getcwd w32_getcwd
#define chmod w32_chmod
#define fchmod w32_fchmod
#define access w32_access
#define utime w32_utime
#define utimes w32_utimes
#define link w32_link
#define symlink w32_symlink
#define readlink w32_readlink
#define mkstemp w32_mkstemp
#define chown(p, u, g) 0
#define lchown(p, u, g) 0
#define fchown(f, u, g) 0
#define realpath w32_realpath
int w32_unlink(const char *path);
int w32_rmdir(const char *path);
int w32_mkdir(const char *path, mode_t mode);
int w32_rename(const char *from, const char *to);
int w32_chdir(const char *path);
char *w32_getcwd(char *buf, size_t size);
int w32_chmod(const char *path, mode_t mode);
int w32_fchmod(int fd, mode_t mode);
int w32_access(const char *path, int mode);
struct utimbuf;
int w32_utime(const char *path, const struct utimbuf *ut);
int w32_utimes(const char *path, const struct timeval tv[2]);
int w32_link(const char *from, const char *to);
int w32_symlink(const char *target, const char *path);
ssize_t w32_readlink(const char *path, char *buf, size_t size);
int w32_mkstemp(char *tmpl);
char *w32_realpath(const char *path, char *resolved);
FILE *w32_fopen(const char *path, const char *mode);
#define fopen w32_fopen

/* ---- users / groups (Windows has no numeric POSIX ids) ------------- */
struct passwd {
	char *pw_name;
	char *pw_passwd;
	uid_t pw_uid;
	gid_t pw_gid;
	char *pw_gecos;
	char *pw_dir;
	char *pw_shell;
};
struct group {
	char *gr_name;
	char *gr_passwd;
	gid_t gr_gid;
	char **gr_mem;
};
#define W32_UID 1000
#define getuid() ((uid_t)W32_UID)
#define geteuid() ((uid_t)W32_UID)
#define getgid() ((gid_t)W32_UID)
#define getegid() ((gid_t)W32_UID)
#define setuid(x) 0
#define seteuid(x) 0
#define setgid(x) 0
#define setegid(x) 0
#define setgroups(n, l) 0
#define initgroups(n, g) 0
#define getgroups(n, l) 0
#define chroot(p) (errno = ENOSYS, -1)
#define getppid() 0
#define setsid() 0
#define getpgrp() 0
#define tcgetpgrp(fd) 0
struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);
struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
char *getpass(const char *prompt);

/* ---- signals / processes ------------------------------------------- */
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGUSR1
#define SIGUSR1 30
#endif
#ifndef SIGUSR2
#define SIGUSR2 31
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGXFSZ
#define SIGXFSZ 25
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#define signal(s, h) w32_signal(s, h)
typedef void (*w32_sighandler_t)(int);
w32_sighandler_t w32_signal(int sig, w32_sighandler_t h);

/* sigaction on top of signal(): only sa_handler is honoured. */
typedef unsigned long w32_sigset_t;
#define sigset_t w32_sigset_t
struct sigaction {
	w32_sighandler_t sa_handler;
	w32_sigset_t sa_mask;
	int sa_flags;
};
#define SA_NOCLDSTOP 1
#define SA_RESTART 2
#define sigemptyset(s) (*(s) = 0, 0)
#define sigaddset(s, n) (*(s) |= 1UL << (n), 0)
#define sigprocmask(h, s, o) 0
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int kill(pid_t pid, int sig);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t fork(void);
unsigned int alarm(unsigned int secs);
#define WNOHANG 1
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define WTERMSIG(s) ((s) & 0x7f)
#define WCOREDUMP(s) 0
#define WIFSTOPPED(s) 0

/* Spawn COMMAND (argv, NULL-terminated) with its stdin/stdout connected to
 * rsync through pollable fds.  Returns the child pid, or -1. */
pid_t w32_spawn_piped(char **command, int *f_in, int *f_out);

/* do_recv()'s fork(), done by deterministic replay (see replay.inc.c).
 * Returns the child pid in the parent, 0 in the child, -1 on error. */
pid_t w32_fork_receiver(int f_in, int f_out, int error_pipe[2]);
/* Server checksum seed inherited by a replayed child (0 = none). */
int w32_replay_seed(void);
/* This process is the sender (never forks): stop recording its input. */
void w32_stop_recording(void);
/* _exit() that first drains a relayed stdout (server mode). */
void w32_exit(int code) __attribute__((noreturn));
#define _exit(c) w32_exit(c)

/* ---- sockets -------------------------------------------------------- */
#define socket w32_socket
#define connect w32_connect
#define bind w32_bind
#define listen w32_listen
#define accept w32_accept
#define shutdown w32_shutdown
#define setsockopt w32_setsockopt
#define getsockopt w32_getsockopt
#define getsockname w32_getsockname
#define getpeername w32_getpeername
#define select w32_select
#define poll w32_poll
#define socketpair w32_socketpair
#define gai_strerror gai_strerrorA
int w32_socket(int af, int type, int proto);
int w32_connect(int fd, const struct sockaddr *sa, socklen_t len);
int w32_bind(int fd, const struct sockaddr *sa, socklen_t len);
int w32_listen(int fd, int backlog);
int w32_accept(int fd, struct sockaddr *sa, socklen_t *len);
int w32_shutdown(int fd, int how);
int w32_setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int w32_getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
int w32_getsockname(int fd, struct sockaddr *sa, socklen_t *len);
int w32_getpeername(int fd, struct sockaddr *sa, socklen_t *len);
int w32_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv);
int w32_socketpair(int af, int type, int proto, int fds[2]);
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif

/* poll(): winsock's WSAPOLLFD has the same layout as struct pollfd */
#define pollfd w32_pollfd
struct w32_pollfd {
	int fd;
	short events;
	short revents;
};
typedef unsigned long nfds_t;
int w32_poll(struct w32_pollfd *fds, nfds_t n, int timeout);

/* ---- dirent --------------------------------------------------------- */
struct dirent {
	ino_t d_ino;
	char d_name[MAX_PATH * 4];
};
typedef struct w32_DIR DIR;
#define opendir w32_opendir
#define readdir w32_readdir
#define closedir w32_closedir
DIR *w32_opendir(const char *path);
struct dirent *w32_readdir(DIR *d);
int w32_closedir(DIR *d);

/* ---- misc ----------------------------------------------------------- */
#define makedev(ma, mi) ((dev_t)(((unsigned long long)(ma) << 32) | (unsigned)(mi)))
#define major(d) ((unsigned)((unsigned long long)(d) >> 32))
#define minor(d) ((unsigned)((d) & 0xffffffffU))
#define fchdir w32_fchdir
int w32_fchdir(int fd);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
#ifndef localtime_r
#define localtime_r(t, tm) (localtime_s(tm, t) ? NULL : (tm))
#define gmtime_r(t, tm) (gmtime_s(tm, t) ? NULL : (tm))
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
struct flock {
	short l_type;
	short l_whence;
	long long l_start;
	long long l_len;
	pid_t l_pid;
};
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define F_SETLK 6
#define F_SETLKW 7
#define sleep(s) (Sleep((s) * 1000), 0)
#define strerror w32_strerror
char *w32_strerror(int err);
#define umask(m) 022
#define openlog(a, b, c) ((void)0)
#define syslog(...) ((void)0)
#define closelog() ((void)0)
#define LOG_PID 0
#define LOG_NDELAY 0
#define LOG_DAEMON 0
#define LOG_INFO 6
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_DEBUG 7
#define LOG_USER 0
#define LOG_LOCAL0 0
#define LOG_LOCAL1 0
#define LOG_LOCAL2 0
#define LOG_LOCAL3 0
#define LOG_LOCAL4 0
#define LOG_LOCAL5 0
#define LOG_LOCAL6 0
#define LOG_LOCAL7 0
#define LOG_AUTH 0
#define LOG_AUTHPRIV 0
#define LOG_CRON 0
#define LOG_FTP 0
#define LOG_KERN 0
#define LOG_LPR 0
#define LOG_MAIL 0
#define LOG_NEWS 0
#define LOG_SYSLOG 0
#define LOG_UUCP 0
#define LOG_CONS 0
#define LOG_NOWAIT 0
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2

#ifndef EAI_SYSTEM
#define EAI_SYSTEM (-11)
#endif
#ifndef ELOOP
#define ELOOP 114
#endif
#ifndef ENOTSUP
#define ENOTSUP 129
#endif

/* Called first thing from main(): winsock, console code page, std fds in
 * binary mode, and argv path conversion (C:\x -> /cygdrive/c/x). */
void w32_init(int *argc, char ***argv);

#endif
