# Limitations of the native Windows rsync.exe

What `build-win.ps1` produces is upstream rsync 3.5.1 plus a POSIX
compatibility layer (`win32\`).  Everything below is a known gap or a
behaviour that differs from rsync on Linux.  Items marked *untested* should
work by design but were not exercised by `win32\test\run-tests.sh` or the
manual tests.

## Not supported

| Feature | Why / what happens |
|---|---|
| `rsync --daemon` (running a daemon **on Windows**) | The daemon forks one process per connection; there is no fork(). Connecting **to** a daemon (`rsync://host/module`) works. |
| `--read-batch` | The receiving side's fork is a replay of the protocol stream (see below); a batch file is not a stream it can replay. Fails with "fork failed in do_recv". |
| `--write-batch` when receiving | *untested*; the replayed child would run the same batch-writing code again. Avoid. |
| ACLs (`-A`), xattrs (`-X`), `--fake-super` | Not compiled in. |
| `--iconv` | Not compiled in (file names are always UTF-8). |
| IDN host names | Not compiled in (use the ASCII/punycode host name). |
| OpenSSL | Not compiled in. Only affects MD4/MD5 speed; the default checksum is xxh128 anyway. |
| `--crtimes` (`-N`), symlink times | Not supported ("no crtimes", "no symtimes" in `--version`). |
| Devices, FIFOs, sockets (`-D`) | Windows has none; nothing is sent, nothing is created. |
| `RSYNC_CONNECT_PROG`, `RSYNC_SHELL`, daemon `pre-xfer exec` | All need fork(). |
| `--files-from=-` / `--exclude-from=-` (reading **stdin**) **when receiving** | The replayed receiver starts with stdin = NUL, so it can't re-read what the parent read. Use a real file instead of `-`. (Fine when sending.) |

## Differs from Linux rsync

### Owners, groups, permissions
- Windows has no numeric uid/gid. Every file reports uid = gid = 1000 and
  the user name of whoever runs rsync. `-o` / `-g` therefore carry no real
  information; when receiving, chown is a no-op.
- Permission bits are **derived from the NTFS ACL the way Cygwin does it**
  (owner ACE or the current user's group memberships → user bits; all other
  named ACEs → group/mask bits; Everyone → other bits; read-only attribute
  clears `w`). This keeps a server copy made by the old Cygwin build
  unchanged. It is an approximation: the owner's group membership is taken
  from the user *running* rsync, and exotic ACLs may map differently than
  Cygwin would.
- When receiving, the only permission that is actually applied is
  **write**: missing `w` sets the read-only attribute, present `w` clears it.
  No ACL is written, so `x` / group / other bits are not stored on disk.

### Symlinks and junctions
- Read as symlinks: Windows symlinks, directory junctions, Cygwin
  "system-file" symlinks (`!<symlink>`) and WSL/Cygwin `LX_SYMLINK` reparse
  points. Junctions are therefore **not followed** (they are sent as links).
- Creating symlinks (when receiving `-l`) makes **native Windows symlinks**,
  which needs Developer Mode or an elevated prompt; otherwise
  "Operation not permitted". Whether a new link is a file or a directory link
  is decided by what the target is at creation time.
- Hard links (`-H`) use the NTFS file index as the inode number. *untested*.

### Paths and command line
- `C:\dir` / `C:/dir` arguments are turned into `/cygdrive/c/dir`
  internally. Consequence: a one-letter remote host written as `c:/path` is
  taken as a local drive path.
- Backslashes in non-option arguments are turned into `/`. A filter given as
  a separate argument (`--exclude foo\bar`) is converted too; write
  `--exclude=foo/bar` to be explicit.
- UNC paths (`\\server\share\...`) as source/destination arguments are not
  supported (rsync collapses the leading `//`); map a drive letter instead.
- Names with characters NTFS forbids (`: * ? " < > |`, control chars) are
  stored as Unicode U+F0xx look-alikes, exactly like Cygwin/MSYS2. Other
  Windows tools see those look-alike characters.
- Paths longer than 260 characters work, but only through rsync itself;
  a child process (ssh) inherits the last working directory that fit in 260.

### Timestamps
- Modification times are set with microsecond precision (NTFS keeps 100 ns).
- As upstream, the quick check compares whole seconds
  (`--modify-window=0`): a same-size edit made within the same second as the
  previous sync is not noticed. Use `-c` or `--modify-window=-1` if that
  matters.

### Receiving side ("replay fork")
Receiving (pull, local copy, daemon pull, Windows as an ssh server) needs
rsync's generator/receiver fork. It is emulated by starting `rsync.exe` again
with the same command line; that child replays the recorded protocol input up
to the fork point with its output discarded. Consequences:
- The file list is built twice (CPU), and the protocol input received before
  the fork (mostly the file list) is kept in memory until the fork.
- Messages printed before the fork point appear once (from the parent); a
  `--log-file` may get its early lines written twice. *untested*.
- A daemon password typed at the prompt is handed to the child through an
  environment variable (hex-encoded) of that child process only.

### Interoperability
- Cygwin/MSYS2 rsync **cannot** start this rsync.exe as its remote side over
  their own `ssh`: they hand ssh a Cygwin socketpair that native programs
  can't read. (Windows → Linux, Linux → Windows via Windows OpenSSH server,
  and native ↔ native all use plain pipes and are fine.)
- Windows as the **remote** side over Windows OpenSSH Server: works through
  pipes in the tests (`--rsync-path=...rsync.exe`), but was not tried with a
  real sshd. *untested*.

### Performance
- Copying many small files is slow: about 5 ms per new file (temp file,
  rename, timestamps, attributes, plus Defender scanning), the same as
  MSYS2's rsync. Large files and no-change runs are fast.
- Every `stat` opens the file to read its ACL and file index. Those stats run
  on up to 8 worker threads with read-ahead (README, "Parallel stat"), so
  scanning C:\PortableApps takes about 5 s (27 s single-threaded, 39 s with
  the Cygwin build). Beyond 8 threads it gets slower again: opening files
  doesn't scale further on NTFS + Defender. Read-ahead may enumerate up to
  65536 entries inside a directory that `--exclude` later skips; that work
  is wasted (no wrong results).
- File transfer itself is still single-threaded per process (as on Linux):
  checksumming, compression and file creation on the receiving side.
- x64 uses all of rsync's optimizations: `SIMD-roll` (SSE2/SSSE3/AVX2
  intrinsics, picked at run time), `asm-roll` (AVX2 CPUs only) and `asm-MD5`.
  The two asm files are written for the Linux calling convention; they run
  behind Win64 ABI shims (`win32\patches.ps1`) that every build checks with
  `win32\test\abi_check.c` before linking. `-NoAsm` builds without them.

### ARM64 build (`-Arch arm64`)
- Cross-compiled on x64 and **never run** (an x64 PC can't execute ARM64
  code; the reverse works through emulation). It builds, links and has the
  ARM64 PE machine type, but is *untested* on real hardware.
- No SIMD rolling checksum (the SIMD code is x86-only); rsync's portable C
  version is used. xxHash (the default xxh128 checksum) picks its own NEON
  code path automatically.

### Requirements
- Windows 10 or later (tested on Windows 11). The UCRT it links against is
  part of Windows 10+; Windows 7/8.1 would additionally need the UCRT update
  (KB2999226) and are *untested*.
- Windows 10 **1903+** only matters for non-ASCII values in environment
  variables (`RSYNC_PASSWORD`, `RSYNC_RSH`, `HOME`, ...): the manifest's
  UTF-8 code page makes `getenv()` return UTF-8 there. Older builds ignore
  that manifest setting and give such values in the ANSI code page. File
  names, the command line and console output don't depend on it; they use
  the Unicode (W) APIs and are converted by rsync itself.
- Creating symlinks without admin rights needs Windows 10 1703+ with
  Developer Mode enabled.
