# Idempotent source patches for the native Windows rsync build.
#
# Each patch is a literal
# OLD -> NEW replacement that is skipped when NEW is already present, and fails
# loudly when neither is found (upstream drift after a version bump) instead of
# silently doing nothing.  Dot-source this file, then call Invoke-RsyncPatches.

$script:PatchUtf8 = New-Object System.Text.UTF8Encoding $false

function Set-SourcePatch {
  param(
    [Parameter(Mandatory)] [string]$File,
    [Parameter(Mandatory)] [string]$Old,
    [Parameter(Mandatory)] [string]$New,
    [Parameter(Mandatory)] [string]$Desc
  )
  if (-not (Test-Path $File)) { throw "[patch] $File not found ($Desc)" }
  $text = [IO.File]::ReadAllText($File)
  # Sources use LF; normalise in case this script was checked out with CRLF.
  $Old = $Old -replace "`r`n", "`n"
  $New = $New -replace "`r`n", "`n"
  if ($text.Contains($New)) { Write-Host "[skip] $Desc"; return }
  if (-not $text.Contains($Old)) { throw "[patch] anchor text not found for '$Desc' in $File (upstream drift?)" }
  [IO.File]::WriteAllText($File, $text.Replace($Old, $New), $script:PatchUtf8)
  Write-Host "[done] $Desc"
}

function Invoke-RsyncPatches {
  param([Parameter(Mandatory)] [string]$RsyncDir)
  $p = { param($f) Join-Path $RsyncDir $f }

  # Winsock start-up, UTF-8 console, binary std fds, and argv rebuilt from the
  # UTF-16 command line with C:\path args turned into /cygdrive/c/path.
  Set-SourcePatch -File (& $p 'main.c') -Desc 'main.c: call w32_init() first thing in main()' `
    -Old @'
	int ret;

	raw_argc = argc;
	raw_argv = argv;
'@ `
    -New @'
	int ret;

	w32_init(&argc, &argv);
	raw_argc = argc;
	raw_argv = argv;
'@

  # The remote shell (ssh) is started with CreateProcessW on real pipes, so
  # Windows' native OpenSSH ssh.exe works; rsync talks to it through pollable
  # loopback sockets pumped by relay threads (see w32_spawn_piped()).
  Set-SourcePatch -File (& $p 'pipe.c') -Desc 'pipe.c: piped_child() spawns via CreateProcess (w32_spawn_piped)' `
    -Old @'
	if (DEBUG_GTE(CMD, 1))
		print_child_argv("opening connection using:", command);

	if (fd_pair(to_child_pipe) < 0 || fd_pair(from_child_pipe) < 0) {
		rsyserr(FERROR, errno, "pipe");
		exit_cleanup(RERR_IPC);
	}

	pid = do_fork();
'@ `
    -New @'
	if (DEBUG_GTE(CMD, 1))
		print_child_argv("opening connection using:", command);

	pid = w32_spawn_piped(command, f_in, f_out);
	if (pid == -1) {
		rsyserr(FERROR, errno, "Failed to exec %s", command[0]);
		exit_cleanup(RERR_IPC);
	}
	if (blocking_io <= 0) {
		set_nonblocking(*f_in);
		set_nonblocking(*f_out);
	}
	return pid;

	if (fd_pair(to_child_pipe) < 0 || fd_pair(from_child_pipe) < 0) {
		rsyserr(FERROR, errno, "pipe");
		exit_cleanup(RERR_IPC);
	}

	pid = do_fork();
'@

  # Local copies: instead of forking a child that calls child_main(), start
  # "rsync.exe --server ..." with the same arguments a remote shell would
  # get (server_options()), on pipes -- exactly like a remote transfer.
  Set-SourcePatch -File (& $p 'pipe.c') -Desc 'pipe.c: local_child() spawns rsync.exe --server instead of forking' `
    -Old @'
	/* The parent process is always the sender for a local rsync. */
	assert(am_sender);

'@ `
    -New @'
	/* The parent process is always the sender for a local rsync. */
	assert(am_sender);

	{
		/* Windows: no fork().  Run ourselves as the server instead, with
		 * the arguments a remote shell would pass (args are not exposed to
		 * a shell here, so no need for --secluded-args). */
		extern int protect_args, local_server;
		char *sargs[MAX_ARGS + MAX_SERVER_ARGS];
		static char self[MAX_PATH * 4];
		wchar_t wself[MAX_PATH * 2];
		int sargc = 0, save_protect = protect_args, i;
		(void)child_main;

		GetModuleFileNameW(NULL, wself, MAX_PATH * 2);
		WideCharToMultiByte(CP_UTF8, 0, wself, -1, self, sizeof self, NULL, NULL);
		sargs[sargc++] = self;
		protect_args = 0;
		server_options(sargs, &sargc);
		protect_args = save_protect;
		for (i = 0; i < argc; i++)
			sargs[sargc++] = argv[i];
		sargs[sargc] = NULL;

		if (DEBUG_GTE(CMD, 1))
			print_child_argv("opening local server using:", sargs);
		pid = w32_spawn_piped(sargs, f_in, f_out);
		if (pid == -1) {
			rsyserr(FERROR, errno, "Failed to start local server %s", self);
			exit_cleanup(RERR_IPC);
		}
		set_nonblocking(*f_in);
		set_nonblocking(*f_out);
		/* From here on this is an ordinary client talking to a real
		 * --server process, so stop taking the shortcuts rsync makes for
		 * a forked local child that shares our memory (e.g. not sending
		 * the filter list). */
		local_server = 0;
		return pid;
	}

'@

  # The receiver's fork() in do_recv(): deterministic "replay fork".
  Set-SourcePatch -File (& $p 'main.c') -Desc 'main.c: do_recv() forks the receiver via w32_fork_receiver()' `
    -Old @'
	if ((pid = do_fork()) == -1) {
		rsyserr(FERROR, errno, "fork failed in do_recv");
'@ `
    -New @'
	if ((pid = w32_fork_receiver(f_in, f_out, error_pipe)) == -1) {
		rsyserr(FERROR, errno, "fork failed in do_recv");
'@

  # A replayed receiver must use the checksum seed its parent (the server)
  # already sent, not a fresh time()^pid one.
  Set-SourcePatch -File (& $p 'compat.c') -Desc 'compat.c: replayed server child inherits checksum_seed' `
    -Old @'
		if (!checksum_seed)
			checksum_seed = time(NULL) ^ (getpid() << 6);
'@ `
    -New @'
		if (!checksum_seed)
			checksum_seed = w32_replay_seed();
		if (!checksum_seed)
			checksum_seed = time(NULL) ^ (getpid() << 6);
'@

  # File names are UTF-8 (see win32compat.c) and the console is switched to
  # UTF-8, but the UCRT's isprint() calls every byte >= 0x80 unprintable, so
  # rsync would print "\#347\#251\#272" for a name like "空".  Treat those
  # bytes as printable, as Cygwin/MSYS2 rsync effectively do.
  Set-SourcePatch -File (& $p 'itypes.h') -Desc 'itypes.h: isPrint() passes UTF-8 bytes through' `
    -Old @'
isPrint(const char *ptr)
{
	return isprint(*(unsigned char *)ptr);
}
'@ `
    -New @'
isPrint(const char *ptr)
{
	return *(unsigned char *)ptr >= 0x80 || isprint(*(unsigned char *)ptr);
}
'@

  # clang's MSVC target (build-win.ps1 -Toolchain msvc) does not define
  # __GNUC__, so these headers would #define __attribute__ away, which drops
  # format/noreturn checks and breaks the vector types in <immintrin.h>.
  # clang always understands __attribute__.
  Set-SourcePatch -File (& $p 'rsync.h') -Desc 'rsync.h: keep __attribute__ under clang without __GNUC__' `
    -Old @'
#ifndef __GNUC__
#define __attribute__(x)
#else
# if __GNUC__ <= 2
# define NORETURN
# endif
#endif
'@ `
    -New @'
#if !defined __GNUC__ && !defined __clang__
#define __attribute__(x)
#elif defined __GNUC__ && __GNUC__ <= 2
# define NORETURN
#endif
'@
  Set-SourcePatch -File (& $p 'popt\system.h') -Desc 'popt/system.h: keep __attribute__ under clang without __GNUC__' `
    -Old '#if !defined(__GNUC__) && !defined(__attribute__)' `
    -New '#if !defined(__GNUC__) && !defined(__clang__) && !defined(__attribute__)'

  # --- asm-roll / asm-MD5 (USE_ROLL_ASM / USE_MD5_ASM in config.h) --------
  # Both .S files are written for the System V AMD64 ABI (Linux).  Windows
  # x64 passes args in rcx, rdx, r8, r9, [rsp+0x28] and treats rdi, rsi and
  # xmm6-xmm15 as callee-saved.  Called as-is they read garbage pointers and
  # can write through them (SIGSEGV / wrong checksums, as found in the old
  # Cygwin build).  Each shim remaps the Win64 args into the SysV registers
  # the untouched body expects and preserves what Win64 requires.

  # get_checksum1_avx2_asm(buf, len, i, ps1, ps2): SysV buf=rdi len=esi i=edx
  # ps1=rcx ps2=r8; Win64 buf=rcx len=edx i=r8d ps1=r9 ps2=[rsp+0x28].  The
  # body clobbers xmm6-xmm10, xmm12, xmm13, xmm15, whose low 128 bits are
  # callee-saved on Win64.  (The old Cygwin build's shim missed xmm8/xmm9:
  # the asm then silently corrupted its caller's registers, which surfaced
  # as wrong SSE2 checksums in simdtest at -O1 and up.)
  Set-SourcePatch -File (& $p 'simd-checksum-avx2.S') -Desc 'simd-checksum-avx2.S: Win64 ABI shim prologue' `
    -Old @'
get_checksum1_avx2_asm:
	vmovd	xmm6,[rcx] # load *ps1
'@ `
    -New @'
get_checksum1_avx2_asm:
	# Win64 ABI shim (see win32\patches.ps1)
	mov	rax, [rsp+0x28]    # ps2 (5th arg), read before rsp moves
	push	rdi
	push	rsi
	sub	rsp, 0x80
	movdqu	[rsp+0x00], xmm6
	movdqu	[rsp+0x10], xmm7
	movdqu	[rsp+0x20], xmm8
	movdqu	[rsp+0x30], xmm9
	movdqu	[rsp+0x40], xmm10
	movdqu	[rsp+0x50], xmm12
	movdqu	[rsp+0x60], xmm13
	movdqu	[rsp+0x70], xmm15
	mov	rdi, rcx           # buf
	mov	esi, edx           # len
	mov	edx, r8d           # i
	mov	rcx, r9            # ps1
	mov	r8, rax            # ps2
	vmovd	xmm6,[rcx] # load *ps1
'@
  Set-SourcePatch -File (& $p 'simd-checksum-avx2.S') -Desc 'simd-checksum-avx2.S: Win64 ABI shim epilogue' `
    -Old @'
.exit:
	vzeroupper
	mov	eax, edx
	ret
'@ `
    -New @'
.exit:
	vzeroupper
	mov	eax, edx
	movdqu	xmm6,  [rsp+0x00]
	movdqu	xmm7,  [rsp+0x10]
	movdqu	xmm8,  [rsp+0x20]
	movdqu	xmm9,  [rsp+0x30]
	movdqu	xmm10, [rsp+0x40]
	movdqu	xmm12, [rsp+0x50]
	movdqu	xmm13, [rsp+0x60]
	movdqu	xmm15, [rsp+0x70]
	add	rsp, 0x80
	pop	rsi
	pop	rdi
	ret
'@

  # md5_process_asm(ctx, data, nbr): SysV ctx=rdi data=rsi nbr=rdx; Win64
  # ctx=rcx data=rdx nbr=r8.  Integer-only; already saves rbp/rbx/r12-r15,
  # but uses rdi/rsi as scratch, which Win64 requires preserved.
  Set-SourcePatch -File (& $p 'lib\md5-asm-x86_64.S') -Desc 'md5-asm-x86_64.S: Win64 ABI shim prologue' `
    -Old @'
	push	%r14
	push	%r15

	# rdi = arg #1 (ctx, MD5_CTX pointer)
'@ `
    -New @'
	push	%r14
	push	%r15
	# Win64 ABI shim (see win32\patches.ps1)
	push	%rdi
	push	%rsi
	mov	%rcx,		%rdi
	mov	%rdx,		%rsi
	mov	%r8,		%rdx

	# rdi = arg #1 (ctx, MD5_CTX pointer)
'@
  Set-SourcePatch -File (& $p 'lib\md5-asm-x86_64.S') -Desc 'md5-asm-x86_64.S: Win64 ABI shim epilogue' `
    -Old @'
	pop	%r15
	pop	%r14
	pop	%r13				# not really useful (r13 is unused)
'@ `
    -New @'
	pop	%rsi
	pop	%rdi
	pop	%r15
	pop	%r14
	pop	%r13				# not really useful (r13 is unused)
'@

  # A sender never forks, so stop recording its input for a replay.
  Set-SourcePatch -File (& $p 'sender.c') -Desc 'sender.c: send_files() stops replay recording' `
    -Old @'
void send_files(int f_in, int f_out)
{
	int fd = -1;
	struct sum_struct *s;
'@ `
    -New @'
void send_files(int f_in, int f_out)
{
	int fd = -1;
	struct sum_struct *s;

	w32_stop_recording();
'@
}
