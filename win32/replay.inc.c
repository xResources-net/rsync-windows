/* ===================================================================== */
/* fork() for the receiver: "replay fork"                                 */
/* ===================================================================== */
/*
 * #included by win32compat.c (it needs that file's fd/socket tables).
 *
 * rsync's receiving side forks once, in do_recv(): the child becomes the
 * receiver, the parent the generator, and both start from identical memory
 * (option state, negotiated protocol, file list...).  Windows can't clone a
 * process, so the child is re-created deterministically instead:
 *
 *  - From startup, every byte this process reads from its protocol input
 *    (the remote shell's stdout, a daemon socket, or stdin when it is the
 *    server) is recorded.
 *  - At the fork point the parent spawns rsync.exe again with the same
 *    command line, environment and starting directory, plus
 *    RSYNC_W32_REPLAY=port,token,seed,mode,password.  The child replays the
 *    recorded input (then the live stream, pumped by the parent), throws
 *    its output away and doesn't spawn ssh / connect, until it reaches the
 *    same do_recv() call in the same state.  There it connects back for the
 *    error pipe, gets its stdout/stderr back, and carries on as the receiver.
 *  - The few non-deterministic inputs (the server's checksum seed, a typed
 *    daemon password) are handed over explicitly.
 */

extern int checksum_seed;
extern int am_server;

static int replay_active;                 /* child: still replaying */
static char replay_mode;                  /* 'c' client, 's' server */
static SOCKET replay_feed = INVALID_SOCKET;
static unsigned short replay_port;
static char replay_token[33];
static int replay_seed;
static char *replay_password;
static int saved_stdout = -1, saved_stderr = -1;
static wchar_t *startup_cwd, *startup_cmdline;
static char *remembered_password;
static HANDLE stdout_relay_thread;

#define REPLAY_ENV L"RSYNC_W32_REPLAY"
#define FAKE_CHILD_PID ((pid_t)0x3ffffff0)

int w32_replay_seed(void)
{
	return replay_seed;
}

static int send_all(SOCKET s, const char *buf, size_t len)
{
	while (len) {
		int n = send(s, buf, len > 1 << 20 ? 1 << 20 : (int)len, 0);
		if (n <= 0)
			return -1;
		buf += n;
		len -= n;
	}
	return 0;
}

static int recv_all(SOCKET s, char *buf, int len)
{
	while (len) {
		int n = recv(s, buf, len, 0);
		if (n <= 0)
			return -1;
		buf += n;
		len -= n;
	}
	return 0;
}

static SOCKET replay_connect(char kind)
{
	struct sockaddr_in a;
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	char hello[34];
	int one = 1;
	if (s == INVALID_SOCKET)
		return s;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(replay_port);
	memcpy(hello, replay_token, 32);
	hello[32] = kind;
	hello[33] = '\n';
	if (connect(s, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR || send_all(s, hello, 34) < 0) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
	return s;
}

/* Parent: wait for the child to connect with the right token + KIND.
 * Gives up if the child process dies first. */
static SOCKET replay_accept(SOCKET lst, HANDLE child, char kind)
{
	for (;;) {
		fd_set r;
		struct timeval tv = { 0, 200000 };
		FD_ZERO(&r);
		FD_SET(lst, &r);
		if (select(0, &r, NULL, NULL, &tv) > 0) {
			char hello[34];
			int one = 1;
			SOCKET s = accept(lst, NULL, NULL);
			if (s == INVALID_SOCKET)
				continue;
			if (recv_all(s, hello, 34) == 0 && memcmp(hello, replay_token, 32) == 0 && hello[32] == kind) {
				setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
				SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
				return s;
			}
			closesocket(s);
		}
		if (WaitForSingleObject(child, 0) == WAIT_OBJECT_0)
			return INVALID_SOCKET;
	}
}

struct feeder {
	SOCKET out;    /* to the replaying child */
	SOCKET src;    /* our protocol input */
	int own_src;   /* close src when done (it was detached from our fd) */
	char *buf;
	size_t len;
};

/* Child -> parent bytes on the feed connection are leftovers from the
 * replayed run's writes (e.g. a daemon socket used both ways): discard. */
static DWORD WINAPI feed_drain(LPVOID arg)
{
	SOCKET s = (SOCKET)arg;
	char buf[16384];
	while (recv(s, buf, sizeof buf, 0) > 0) {}
	closesocket(s);
	return 0;
}

static DWORD WINAPI feeder_thread(LPVOID arg)
{
	struct feeder *f = arg;
	char *buf = malloc(65536);
	HANDLE th;

	if ((th = CreateThread(NULL, 0, feed_drain, (LPVOID)f->out, 0, NULL)) != NULL)
		CloseHandle(th);
	if (send_all(f->out, f->buf, f->len) == 0) {
		free(f->buf);
		f->buf = NULL;
		/* src may be non-blocking (rsync set it so); wait with select. */
		for (;;) {
			fd_set r;
			int n;
			FD_ZERO(&r);
			FD_SET(f->src, &r);
			if (select(0, &r, NULL, NULL, NULL) == SOCKET_ERROR)
				break;
			n = recv(f->src, buf, 65536, 0);
			if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
				continue;
			if (n <= 0 || send_all(f->out, buf, n) < 0)
				break;
		}
	}
	shutdown(f->out, SD_SEND);
	if (f->own_src)
		closesocket(f->src);
	free(f->buf);
	free(buf);
	free(f);
	return 0;
}

static HANDLE inheritable_dup(HANDLE h)
{
	HANDLE d = NULL;
	if (h && h != INVALID_HANDLE_VALUE)
		DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS);
	return d;
}

static void swap_socket(int fd, SOCKET s)
{
	SOCKET old = sock_tab[fd];
	u_long nb = sock_nonblock[fd];
	ioctlsocket(s, FIONBIO, &nb);
	sock_tab[fd] = s;
	if (old != INVALID_SOCKET)
		closesocket(old);
}

/* Child side, at the fork point. */
static pid_t replay_fork_point(int error_pipe[2])
{
	SOCKET s = replay_connect('E');
	if (s == INVALID_SOCKET) {
		fprintf(stderr, "rsync: replay child lost its parent\n");
		_exit(14);
	}
	swap_socket(error_pipe[1], s);
	fflush(stdout);
	fflush(stderr);
	if (saved_stdout >= 0) {
		_dup2(saved_stdout, 1);
		_close(saved_stdout);
		saved_stdout = -1;
	}
	if (saved_stderr >= 0) {
		_dup2(saved_stderr, 2);
		_close(saved_stderr);
		saved_stderr = -1;
	}
	replay_active = 0;
	return 0;
}

pid_t w32_fork_receiver(int f_in, int f_out, int error_pipe[2])
{
	SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
	SOCKET src = fd_sock(f_in), lst = INVALID_SOCKET, feed, ep;
	struct sockaddr_in addr;
	int alen = sizeof addr, slot, i, ninh = 0;
	unsigned int r;
	wchar_t env[512], exe[MAX_PATH * 2], *pw = NULL;
	HANDLE nul = NULL, out = NULL, err = NULL, inh[3];
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T attrsz = 0;
	struct feeder *fd;
	HANDLE th;
	BOOL ok;

	if (replay_active)
		return replay_fork_point(error_pipe);

	if (src == INVALID_SOCKET) {
		/* e.g. --read-batch from a file: nothing we could replay */
		errno = ENOSYS;
		return -1;
	}
	for (slot = 0; slot < MAX_CHILDREN && children[slot].h; slot++) {}
	if (slot == MAX_CHILDREN) {
		errno = EAGAIN;
		return -1;
	}

	fflush(stdout);
	fflush(stderr);

	/* listener for the child's two connections */
	lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (lst == INVALID_SOCKET || bind(lst, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR
	 || getsockname(lst, (struct sockaddr *)&addr, &alen) == SOCKET_ERROR || listen(lst, 4) == SOCKET_ERROR) {
		set_errno_wsa();
		if (lst != INVALID_SOCKET)
			closesocket(lst);
		return -1;
	}
	SetHandleInformation((HANDLE)lst, HANDLE_FLAG_INHERIT, 0);
	for (i = 0; i < 32; i++) {
		rand_s(&r);
		replay_token[i] = "0123456789abcdef"[r & 15];
	}
	replay_token[32] = '\0';

	if (remembered_password) {
		size_t n = strlen(remembered_password);
		pw = malloc((n * 2 + 1) * sizeof (wchar_t));
		for (i = 0; i < (int)n; i++)
			swprintf(pw + i * 2, 3, L"%02x", (unsigned char)remembered_password[i]);
		pw[n * 2] = 0;
	}
	_snwprintf(env, 512, L"%u,%hs,%d,%c,%ls", ntohs(addr.sin_port), replay_token, checksum_seed,
		   am_server ? 's' : 'c', pw ? pw : L"-");
	free(pw);

	/* stdin: NUL.  stdout: the real one for a client (the receiver prints
	 * the file names), NUL for a server (its stdout is the protocol, which
	 * the child must never touch).  stderr: shared. */
	nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			  &sa, OPEN_EXISTING, 0, NULL);
	if (!am_server)
		out = inheritable_dup(GetStdHandle(STD_OUTPUT_HANDLE));
	err = inheritable_dup(GetStdHandle(STD_ERROR_HANDLE));
	inh[ninh++] = nul;
	if (out)
		inh[ninh++] = out;
	if (err)
		inh[ninh++] = err;

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = nul;
	si.StartupInfo.hStdOutput = out ? out : nul;
	si.StartupInfo.hStdError = err ? err : nul;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attrsz);
	si.lpAttributeList = malloc(attrsz);
	InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrsz);
	UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				  inh, ninh * sizeof (HANDLE), NULL, NULL);

	GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
	SetEnvironmentVariableW(REPLAY_ENV, env);
	ok = CreateProcessW(exe, startup_cmdline, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT,
			    NULL, startup_cwd, &si.StartupInfo, &pi);
	SetEnvironmentVariableW(REPLAY_ENV, NULL);
	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);
	CloseHandle(nul);
	if (out)
		CloseHandle(out);
	if (err)
		CloseHandle(err);
	if (!ok) {
		set_errno_win();
		closesocket(lst);
		return -1;
	}
	CloseHandle(pi.hThread);

	if ((feed = replay_accept(lst, pi.hProcess, 'F')) == INVALID_SOCKET)
		goto child_died;

	/* Hand our input stream to the feeder thread: everything recorded so
	 * far, then whatever arrives next. */
	fd = calloc(1, sizeof *fd);
	fd->out = feed;
	fd->src = src;
	fd->buf = rec_buf;
	fd->len = rec_len;
	rec_buf = NULL;
	rec_len = rec_cap = 0;
	rec_enabled = 0;
	memset(fd_record, 0, sizeof fd_record);
	if (f_in != f_out) {
		/* The generator closes f_in right after the fork; give it a dummy
		 * to close so the real socket stays with the feeder. */
		sock_tab[f_in] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		fd->own_src = 1;
	}
	if ((th = CreateThread(NULL, 0, feeder_thread, fd, 0, NULL)) != NULL)
		CloseHandle(th);

	/* The child connects again once it reaches this same fork point. */
	if ((ep = replay_accept(lst, pi.hProcess, 'E')) == INVALID_SOCKET)
		goto child_died;
	closesocket(lst);
	swap_socket(error_pipe[0], ep);

	children[slot].pid = (pid_t)pi.dwProcessId;
	children[slot].h = pi.hProcess;
	return (pid_t)pi.dwProcessId;

  child_died:
	closesocket(lst);
	children[slot].pid = (pid_t)pi.dwProcessId;
	children[slot].h = pi.hProcess;
	errno = ECHILD;
	return -1;
}

/* Child: called from w32_init() when RSYNC_W32_REPLAY is set. */
static void replay_init(const wchar_t *spec)
{
	char buf[512], *p, *q;
	int nul;

	WideCharToMultiByte(CP_UTF8, 0, spec, -1, buf, sizeof buf, NULL, NULL);
	SetEnvironmentVariableW(REPLAY_ENV, NULL);
	_putenv("RSYNC_W32_REPLAY=");

	p = buf;
	replay_port = (unsigned short)strtoul(p, &p, 10);
	if (*p == ',')
		p++;
	memcpy(replay_token, p, 32);
	replay_token[32] = '\0';
	p += 32;
	if (*p == ',')
		p++;
	replay_seed = (int)strtol(p, &p, 10);
	if (*p == ',')
		p++;
	replay_mode = *p++;
	if (*p == ',')
		p++;
	if (*p && *p != '-') {
		size_t n = strlen(p) / 2, i;
		replay_password = malloc(n + 1);
		for (i = 0; i < n; i++) {
			char hx[3] = { p[i * 2], p[i * 2 + 1], 0 };
			replay_password[i] = (char)strtoul(hx, &q, 16);
		}
		replay_password[n] = '\0';
	}

	rec_enabled = 0;
	replay_active = 1;
	replay_feed = replay_connect('F');
	if (replay_feed == INVALID_SOCKET) {
		fprintf(stderr, "rsync: replay child could not reach its parent\n");
		_exit(14);
	}

	/* Silence everything up to the fork point: the parent already said it. */
	fflush(stdout);
	fflush(stderr);
	saved_stdout = _dup(1);
	saved_stderr = _dup(2);
	if ((nul = _open("NUL", _O_WRONLY | _O_BINARY)) >= 0) {
		_dup2(nul, 1);
		_dup2(nul, 2);
		_close(nul);
	}

	if (replay_mode == 's') {
		/* A server's protocol is its stdin/stdout. */
		sock_tab[0] = replay_feed;
		replay_feed = INVALID_SOCKET;
		fd_null[1] = 1;
	}
}

/* rsync is a server (--server): put its stdin/stdout behind pollable
 * sockets too.  Reads from stdin are recorded for a later replay fork. */
static void setup_server_stdio(void)
{
	SOCKET in_sv[2], out_sv[2];
	struct relay *ri, *ro;
	HANDLE hin = NULL, hout = NULL, th;

	DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_INPUT_HANDLE), GetCurrentProcess(),
			&hin, 0, FALSE, DUPLICATE_SAME_ACCESS);
	DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(),
			&hout, 0, FALSE, DUPLICATE_SAME_ACCESS);
	if (!hin || !hout || raw_socketpair(in_sv) < 0)
		return;
	if (raw_socketpair(out_sv) < 0) {
		closesocket(in_sv[0]);
		closesocket(in_sv[1]);
		return;
	}
	ri = malloc(sizeof *ri);
	ri->s = in_sv[1];
	ri->h = hin;
	ro = malloc(sizeof *ro);
	ro->s = out_sv[1];
	ro->h = hout;
	sock_tab[0] = in_sv[0];
	fd_record[0] = 1;
	sock_tab[1] = out_sv[0];
	if ((th = CreateThread(NULL, 0, relay_from_child, ri, 0, NULL)) != NULL)
		CloseHandle(th);
	stdout_relay_thread = CreateThread(NULL, 0, relay_to_child, ro, 0, NULL);
}

/* Make sure everything written to a relayed stdout reaches the real one
 * before the process goes away. */
static void flush_stdout_relay(void)
{
	if (!stdout_relay_thread)
		return;
	if (sock_tab[1] != INVALID_SOCKET)
		shutdown(sock_tab[1], SD_SEND);
	WaitForSingleObject(stdout_relay_thread, 30000);
	CloseHandle(stdout_relay_thread);
	stdout_relay_thread = NULL;
}

#undef _exit
void w32_exit(int code)
{
	fflush(stdout);
	fflush(stderr);
	flush_stdout_relay();
	_exit(code);
}
