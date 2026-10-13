# rsync for Windows

A plain Win32 `rsync.exe`, built with `zig cc` by `build-win.ps1`. No
`cygwin1.dll`, no `msys-2.0.dll`: it only needs Windows system DLLs (KERNEL32,
WS2_32, ADVAPI32, SHELL32 and the UCRT that ships with Windows 10/11).
Known gaps and differences from Linux rsync: see [LIMIT.md](LIMIT.md).

## Native build

```powershell
powershell -ExecutionPolicy Bypass -File build-win.ps1           # build
powershell -ExecutionPolicy Bypass -File build-win.ps1 -Clean    # re-extract everything first
powershell -ExecutionPolicy Bypass -File build-win.ps1 -Native   # -march=native (this PC only)
powershell -ExecutionPolicy Bypass -File build-win.ps1 -Arch arm64  # Windows on ARM -> ..\bin-arm64
powershell -ExecutionPolicy Bypass -File build-win.ps1 -NoAsm      # x64 without asm-roll / asm-MD5
powershell -ExecutionPolicy Bypass -File build-win.ps1 -Toolchain msvc  # MSVC CRT + Windows SDK
```

### Build on GitHub

`.github/workflows/build.yml` builds x64 and arm64 on every push (Actions →
artifacts `rsync-windows-x64` / `rsync-windows-arm64`). Pushing a `v*` tag
also attaches both zips to a GitHub Release. The CI build is generic (no
`-Native`).

`-Toolchain msvc` targets `x86_64-windows-msvc`: still zig's clang and lld,
but against the installed Visual Studio C runtime and Windows SDK instead of
zig's bundled mingw-w64. The CRT is linked statically, so the exe imports
only KERNEL32/WS2_32/ADVAPI32/SHELL32. The POSIX headers mingw has and MSVC
lacks (`unistd.h`, `sys/time.h`, ...) are stubbed in `win32\include-msvc`.
Objects go to `build-win\obj-x64-msvc`. The test suite passes the same as
the default gnu build.

Produces `..\bin\rsync.exe` (`-OutDir` to change). Needs zig
(`..\..\zig\zig.exe`, else `%ZIG%`, else `zig` on PATH) and, at build time
only, a `gawk` (taken from `..\..\Git\usr\bin`, `..\..\msys64\usr\bin` or
PATH) to generate rsync's headers. There is no `./configure`
step: `win32\config.h` replaces it. Bundled: zstd, lz4, xxHash (from the
tarballs here) plus rsync's own zlib and popt. Not included: OpenSSL (rsync's
built-in MD4/MD5 + xxHash are used), iconv, IDN, ACLs, xattrs.

What the build does: extract tarballs into `build-win\` → apply
`win32\patches.ps1` (idempotent, fails loudly on upstream drift) → generate
`proto.h`/`help-*.h`/... with awk → compile in parallel → on x64, run the asm
ABI check (below) → compile `win32\rsync.rc` with `zig rc` and link it in:
`win32\rsync.manifest` (UTF-8 code page, long-path aware) plus a version
resource (Explorer's Details tab). The version comes from the tarball name,
and the Comments field records the arch, toolchain and -Native/-NoAsm.

`rsync --version` on x64 reports `SIMD-roll, asm-roll, asm-MD5`. The two
asm files (`simd-checksum-avx2.S`, `lib/md5-asm-x86_64.S`) are written for
the Linux calling convention; `patches.ps1` adds Win64 shims that remap the
arguments and preserve the registers Windows requires (rdi, rsi,
xmm6-xmm15). Every build then runs `win32\test\abi_check.c`, which calls the
asm with a sentinel in every callee-saved register and compares results with
plain C. The build stops if anything is clobbered or wrong. (The old Cygwin
build's shim forgot xmm8/xmm9, which corrupted the caller's registers. That
was the real cause of the "wrong SSE2 checksums with asm-roll" it worked
around by disabling asm-roll.)

### How it behaves (win32\win32compat.c)

- **Paths.** `C:\x` on the command line becomes `/cygdrive/c/x` internally,
  so existing scripts using `/cygdrive/c/...` (e.g. `rsync-sync.cmd`) work
  unchanged. Unicode and paths longer than 260 characters work.
- **Same file metadata as the Cygwin build.** Permission bits come from the
  NTFS ACL using Cygwin's mapping (owner / group mask / Everyone), and the
  read-only attribute clears the `w` bits. Cygwin symlinks (`!<symlink>`
  system files and WSL-style reparse points), Windows symlinks and junctions
  all show up as symlinks. Characters NTFS forbids (`: * ? " < > |`) use
  Cygwin's U+F0xx mapping. A dry run of `rsync-sync.cmd` with this build
  lists exactly the same files as with the Cygwin build.
- **Remote shell.** `ssh` is started with `CreateProcess` on real pipes, so
  Windows' own `C:\Windows\System32\OpenSSH\ssh.exe` works directly. rsync's
  side of those pipes is a loopback socket pair pumped by two threads,
  because Windows can only `poll()` sockets.
- **No fork().** rsync forks in two places, and both are replaced:
  - A local copy (`rsync -a C:\a\ D:\b\`) starts `rsync.exe --server ...` as
    its child, exactly as a remote shell would.
  - The receiving side's generator/receiver split (pull, local copy, daemon
    pull, Windows as an ssh server) is a "replay fork" (`win32\replay.inc.c`).
    The parent starts `rsync.exe` again with the same command line. That
    child replays the protocol input the parent recorded, with its output
    discarded, until it reaches the same fork point in the same state. It
    then takes over as the receiver on the live stream.
- **Signals.** SIGUSR1/SIGUSR2 between rsync processes use named events.
- **Parallel stat.** A Windows `lstat` is an open + ACL read + close (~25 us
  per file even when cached), and rsync does one at a time. So `opendir()`
  hands every entry to a pool of worker threads, and `lstat()` picks up the
  finished result. The workers also read ahead: they enumerate and stat
  subdirectories before rsync gets there, in rsync's own depth-first, sorted
  order, up to 65536 entries ahead. A result is used at most once and only
  while its directory is open. Anything rsync itself changes on disk drops
  all of it, and read-ahead older than 10 s is ignored. Scanning
  C:\PortableApps (607k entries) takes 5.3 s instead of 23-27 s, and the
  `rsync-sync.cmd` dry run 10-12 s instead of 30-37 s. Tuning:
  `RSYNC_STAT_THREADS=n` (default: CPU count, at most 8; 0 turns all of this
  off) and `RSYNC_STAT_READAHEAD=n` (directory levels below the ones rsync
  has opened; 0 = no read-ahead).

### Tests

```sh
bash win32/test/run-tests.sh      # from Git Bash; uses %TEMP%\rsync-wtest
```

Local copy and delta, `--delete`, push and pull over a fake ssh (MSYS2's rsync
on the other end), this `rsync.exe` as the remote server in both
directions, read-only attributes, long paths, and Cygwin-style special
characters. Rolling and MD5 checksums are checked against MSYS2's rsync,
an independent C implementation: a delta push must match blocks, and `-c`
must find no difference. `win32\test\fakessh.c` stands in for
`ssh HOST CMD`.


## Layout

| File | Purpose |
|---|---|
| `build-win.ps1` | Build: extract, patch, generate headers, compile, link |
| `win32\` | `config.h`, compat layer, patches, manifest, tests |
| `build-win\` | Build tree (extracted sources + objects); safe to delete |
| `<pkg>-<version>.tar.gz` | Upstream source tarballs |
