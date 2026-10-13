<#
.SYNOPSIS
  Builds a native Windows rsync.exe (no cygwin1.dll / msys-2.0.dll) with zig cc.

.DESCRIPTION
  1. Extracts rsync, zstd, lz4 and xxHash tarballs into build-win\ (skipped
     if already extracted; -Clean wipes build-win\ first).
  2. Applies the idempotent source patches in win32\patches.ps1.
  3. Generates the headers ./configure + make would (proto.h, help-*.h, ...)
     with awk.  There is no ./configure: win32\config.h replaces it.
  4. Compiles everything with "zig cc -target x86_64-windows-gnu" in
     parallel and links rsync.exe, with win32\rsync.rc embedded: the manifest
     (UTF-8 code page, long-path aware) and a VERSIONINFO resource.

  The only runtime dependencies are Windows system DLLs (KERNEL32, WS2_32,
  ADVAPI32, SHELL32 and the UCRT api-ms-win-crt-* set that ships with
  Windows 10/11).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File build-win.ps1
  powershell -ExecutionPolicy Bypass -File build-win.ps1 -Clean -Native
#>
[CmdletBinding()]
param(
  # Wipe build-win\ and re-extract everything.
  [switch]$Clean,
  # Tune for this machine's CPU (-march=native). Not portable to other PCs.
  [switch]$Native,
  # x64: leave out the hand-written asm (asm-roll, asm-MD5).
  [switch]$NoAsm,
  # Target CPU: x64 (default) or arm64 (Windows on ARM; cross-compiled).
  [ValidateSet('x64', 'arm64')]
  [string]$Arch = 'x64',
  # C library / ABI: gnu (zig's bundled mingw-w64, default) or msvc (the
  # installed Visual Studio C runtime + Windows SDK; still clang + lld).
  [ValidateSet('gnu', 'msvc')]
  [string]$Toolchain = 'gnu',
  # Where rsync.exe is copied to (default ..\bin, or ..\bin-arm64).
  [string]$OutDir = '',
  # Release revision: 4th field of the exe's file version (3.5.1-1 -> 3,5,1,1).
  [int]$Revision = 1,
  # Parallel compile jobs.
  [int]$Jobs = [Environment]::ProcessorCount
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

# ---- versions / tools ------------------------------------------------------
$Rsync  = 'rsync-3.5.1'
$Zstd   = 'zstd-1.5.7'
$Lz4    = 'lz4-1.10.0'
$XxHash = 'xxHash-0.8.4'

$Src      = $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $Src $(if ($Arch -eq 'arm64') { '..\bin-arm64' } else { '..\bin' }) }
$BuildDir = Join-Path $Src 'build-win'
$Win32Dir = Join-Path $Src 'win32'
# zig: the PortableApps copy next to this tree, else $env:ZIG, else PATH
# (e.g. GitHub Actions' setup-zig).
$Zig = @((Join-Path $Src '..\..\zig\zig.exe'), $env:ZIG) |
  Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if ($Zig) { $Zig = (Resolve-Path $Zig).Path }
else {
  $cmd = Get-Command zig -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($cmd) { $Zig = $cmd.Source } else { throw 'zig not found (put it in ..\..\zig, set ZIG=, or add it to PATH)' }
}

# awk is only needed at build time to generate headers from rsync's own
# templates; any gawk works (Git for Windows or MSYS2 ship one).
$Awk = @(
  (Join-Path $Src '..\..\Git\usr\bin\gawk.exe'),
  (Join-Path $Src '..\..\msys64\usr\bin\gawk.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Awk) {
  $cmd = Get-Command gawk, awk -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($cmd) { $Awk = $cmd.Source } else { throw 'awk not found (need Git for Windows or msys64)' }
}
$Tar = Join-Path $env:SystemRoot 'System32\tar.exe'

$IsX64 = $Arch -eq 'x64'
$Target = $(if ($IsX64) { 'x86_64' } else { 'aarch64' }) + "-windows-$Toolchain"
$IsMsvc = $Toolchain -eq 'msvc'
$RcTarget = if ($IsX64) { 'x86_64' } else { 'arm64' }
$OptFlags = @('-O3')
if ($Native) {
  if (-not $IsX64) { throw '-Native tunes for this (x64) PC; it cannot be combined with -Arch arm64' }
  $OptFlags += '-march=native'
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding $false

function Write-Step($msg) { Write-Host "[build] $msg" -ForegroundColor Cyan }

# Run a native command discarding its stderr. (Windows PowerShell 5.1 turns
# native stderr into terminating errors under ErrorActionPreference=Stop.)
function Invoke-Quiet([scriptblock]$Block) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try { & $Block 2>&1 | Out-Null } finally { $ErrorActionPreference = $old }
}

# ---- 1. extract ------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
  Write-Step "clean: removing $BuildDir"
  Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Force $BuildDir | Out-Null

function Expand-Pkg([string]$Dir, [string]$Tarball, [string]$Probe) {
  $dest = Join-Path $BuildDir $Dir
  if (Test-Path (Join-Path $dest $Probe)) { return }
  Write-Step "extract $Tarball"
  # bsdtar can't create the few symlinks some tarballs carry (unprivileged
  # Windows); those are test/doc helpers we don't need, so its exit code is
  # ignored and success is judged by the probe file instead.
  Invoke-Quiet { & $Tar -xzf (Join-Path $Src $Tarball) -C $BuildDir --exclude '._*' }
  if (-not (Test-Path (Join-Path $dest $Probe))) { throw "extracting $Tarball failed" }
}
Expand-Pkg $Rsync  "$Rsync.tar.gz"  'main.c'
Expand-Pkg $Zstd   "$Zstd.tar.gz"   'lib\zstd.h'
Expand-Pkg $Lz4    "$Lz4.tar.gz"    'lib\lz4.c'
Expand-Pkg $XxHash "xxHash-v$($XxHash -replace '^xxHash-','').tar.gz" 'xxhash.c'

$RsyncDir = Join-Path $BuildDir $Rsync

# ---- 2. patches ------------------------------------------------------------
Write-Step 'apply source patches'
. (Join-Path $Win32Dir 'patches.ps1')
Invoke-RsyncPatches -RsyncDir $RsyncDir

# ---- 3. generated headers --------------------------------------------------
# mkproto.awk shells out to `touch`; put awk's own bin dir (which ships
# touch/sh) on PATH so that call resolves.
function Invoke-Awk([string[]]$AwkArgs) {
  Push-Location $RsyncDir
  $oldPath = $env:PATH
  $env:PATH = "$(Split-Path $Awk);$oldPath"
  try {
    & $Awk @AwkArgs
    if ($LASTEXITCODE -ne 0) { throw "awk $($AwkArgs -join ' ') failed" }
  } finally { $env:PATH = $oldPath; Pop-Location }
}

Write-Step 'generate headers'
Copy-Item (Join-Path $Win32Dir 'config.h') (Join-Path $RsyncDir 'config.h') -Force
if (-not (Test-Path (Join-Path $RsyncDir 'git-version.h'))) {
  [IO.File]::WriteAllText((Join-Path $RsyncDir 'git-version.h'), '', $Utf8NoBom)
}
Invoke-Awk @('-f', 'daemon-parm.awk', 'daemon-parm.txt')
Invoke-Awk @('-f', 'help-from-md.awk', '-v', 'hfile=help-rsync.h', 'rsync.1.md')
Invoke-Awk @('-f', 'help-from-md.awk', '-v', 'hfile=help-rsyncd.h', 'rsync.1.md')
Invoke-Awk @('-f', 'define-from-md.awk', '-v', 'hfile=default-cvsignore.h', 'rsync.1.md')
Invoke-Awk @('-f', 'define-from-md.awk', '-v', 'hfile=default-dont-compress.h', 'rsync.1.md')
# proto.h: mkproto.awk over every .c file that the Makefile feeds it.
$protoSrc = @(Get-ChildItem -Path $RsyncDir -Filter '*.c' | Sort-Object Name | ForEach-Object Name) + 'lib/compat.c' + 'daemon-parm.h'
Invoke-Awk (@('-f', 'mkproto.awk') + $protoSrc)

# ---- 4. compile ------------------------------------------------------------
$ObjDir = Join-Path $BuildDir $(if ($IsMsvc) { "obj-$Arch-msvc" } else { "obj-$Arch" })
New-Item -ItemType Directory -Force $ObjDir | Out-Null

$CommonFlags = @('-target', $Target) + $OptFlags + @('-fno-strict-aliasing', '-w')

$ZstdLib = Join-Path $BuildDir "$Zstd\lib"
$Lz4Lib  = Join-Path $BuildDir "$Lz4\lib"
$XxDir   = Join-Path $BuildDir $XxHash

$RsyncFlags = $CommonFlags + @(
  "-DRSYNC_WIN32_VERSION=`"$($Rsync -replace '^rsync-','')`"",
  '-DHAVE_CONFIG_H', '-iquote', $RsyncDir, '-I', (Join-Path $RsyncDir 'popt'), '-I', (Join-Path $RsyncDir 'zlib'), '-I', $Win32Dir, '-I', (Join-Path $Win32Dir 'include'),
  '-I', $XxDir, '-I', $ZstdLib, '-I', $Lz4Lib
)
# The MSVC headers lack the POSIX bits mingw-w64 provides (unistd.h,
# sys/time.h, ssize_t, ...); win32\include-msvc fills them in.
if ($IsMsvc) { $RsyncFlags += @('-I', (Join-Path $Win32Dir 'include-msvc')) }

# rounding.h: the largest EXTRA_ROUNDING that still lets rounding.c compile.
$roundingH = Join-Path $RsyncDir 'rounding.h'
if (-not (Test-Path $roundingH)) {
  Write-Step 'probe rounding.h'
  $ok = $false
  foreach ($r in 0, 1, 3) {
    Invoke-Quiet { & $Zig cc @RsyncFlags "-DEXTRA_ROUNDING=$r" -c (Join-Path $RsyncDir 'rounding.c') -o (Join-Path $ObjDir 'rounding-probe.o') }
    if ($LASTEXITCODE -eq 0) {
      [IO.File]::WriteAllText($roundingH, "#define EXTRA_ROUNDING $r`n", $Utf8NoBom); $ok = $true; break
    }
  }
  if (-not $ok) { throw 'could not determine EXTRA_ROUNDING (rounding.c fails to compile)' }
}

$units = New-Object System.Collections.Generic.List[object]
function Add-Unit([string]$Source, [string[]]$Flags, [string]$Compiler = 'cc') {
  $root = if ($Source.StartsWith($BuildDir + '\')) { $BuildDir } else { $Src }
  $rel = $Source.Substring($root.Length + 1) -replace '[\\/:]', '_'
  $units.Add([pscustomobject]@{ Src = $Source; Obj = (Join-Path $ObjDir ($rel -replace '\.(c|cpp|S)$', '.o')); Flags = $Flags; Cc = $Compiler })
}

# zstd (portable C only; its x86 asm decoder is ELF-only)
$zstdFlags = $CommonFlags + @('-DZSTD_DISABLE_ASM', '-DZSTD_MULTITHREAD=0', '-I', $ZstdLib, '-I', (Join-Path $ZstdLib 'common'))
foreach ($sub in 'common', 'compress', 'decompress') {
  Get-ChildItem (Join-Path $ZstdLib $sub) -Filter '*.c' | ForEach-Object { Add-Unit $_.FullName $zstdFlags }
}
# lz4: rsync only needs the block API
Add-Unit (Join-Path $Lz4Lib 'lz4.c') ($CommonFlags + @('-I', $Lz4Lib))
# xxHash
Add-Unit (Join-Path $XxDir 'xxhash.c') ($CommonFlags + @('-I', $XxDir))

# rsync + its bundled zlib and popt
$rsyncObjs = @(
  'flist','rsync','generator','receiver','cleanup','sender','exclude','util1','util2',
  'checksum','match','syscall','log','backup','delete','main',
  'options','io','compat','hlink','token','uidlist','socket','hashtable','usage',
  'fileio','batch','clientname','chmod','acls','xattrs',
  'progress','pipe',
  'params','loadparm','clientserver','access','connection','authenticate',
  'lib/wildmatch','lib/compat','lib/snprintf','lib/mdfour','lib/md5','lib/permstring',
  'lib/pool_alloc','lib/sysacls','lib/sysxattrs','lib/acl',
  'zlib/deflate','zlib/inffast','zlib/inflate','zlib/inftrees','zlib/trees','zlib/zutil',
  'zlib/adler32','zlib/compress','zlib/crc32',
  'popt/popt','popt/poptconfig','popt/popthelp','popt/poptparse','popt/poptint'
)
$UseAsm = $IsX64 -and -not $NoAsm
if (-not $UseAsm) { $RsyncFlags += '-DRSYNC_W32_NO_ASM' }
foreach ($o in $rsyncObjs) { Add-Unit (Join-Path $RsyncDir "$o.c") $RsyncFlags }
if ($IsX64) {
  # SSE2/SSSE3/AVX2 rolling checksum (config.h enables USE_ROLL_SIMD on x64
  # only), dispatched at run time via cpu_model.c.  ARM64 uses rsync's
  # portable C checksum.
  # (msvc: -fno-autolink drops the libc++.lib default-lib directive; nothing
  # from libc++ is used.)
  $cxxFlags = $RsyncFlags + @('-fno-exceptions', '-fno-rtti') + $(if ($IsMsvc) { @('-fno-autolink') } else { @() })
  Add-Unit (Join-Path $RsyncDir 'simd-checksum-x86_64.cpp') $cxxFlags 'c++'
}
if ($UseAsm) {
  # asm-roll + asm-MD5 (config.h: USE_ROLL_ASM / USE_MD5_ASM), with the Win64
  # ABI shims from win32\patches.ps1; verified by the ABI gate below.
  Add-Unit (Join-Path $RsyncDir 'simd-checksum-avx2.S') $RsyncFlags
  Add-Unit (Join-Path $RsyncDir 'lib\md5-asm-x86_64.S') $RsyncFlags
}
Add-Unit (Join-Path $Win32Dir 'win32compat.c') $RsyncFlags
if ($IsX64) { Add-Unit (Join-Path $Win32Dir 'cpu_model.c') $CommonFlags }

# Rebuild a unit when its source, or any header it could depend on, is newer.
$hdrStamp = @(Get-ChildItem $Win32Dir -Recurse -File) + @(Get-ChildItem $RsyncDir -Filter '*.h') |
  ForEach-Object { $_.LastWriteTimeUtc } | Sort-Object | Select-Object -Last 1

$todo = @($units | Where-Object {
  if (-not (Test-Path $_.Obj)) { return $true }
  $objTime = (Get-Item $_.Obj).LastWriteTimeUtc
  $objTime -lt (Get-Item $_.Src).LastWriteTimeUtc -or $objTime -lt $hdrStamp
})
Write-Step "compile $($todo.Count) of $($units.Count) units ($Jobs jobs)"

function Format-Arg([string]$a) { if ($a -match '[\s"]') { '"' + ($a -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"' } else { $a } }

$running = New-Object System.Collections.Generic.List[object]
$failed = New-Object System.Collections.Generic.List[object]
$queue = New-Object System.Collections.Generic.Queue[object]
$todo | ForEach-Object { $queue.Enqueue($_) }
while ($queue.Count -or $running.Count) {
  while ($queue.Count -and $running.Count -lt $Jobs) {
    $u = $queue.Dequeue()
    $log = "$($u.Obj).log"
    $argList = @($u.Cc) + $u.Flags + @('-c', $u.Src, '-o', $u.Obj)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Zig
    $psi.Arguments = ($argList | ForEach-Object { Format-Arg $_ }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $running.Add([pscustomobject]@{ P = $p; U = $u; Err = $errTask; Out = $outTask; Log = $log })
  }
  Start-Sleep -Milliseconds 50
  foreach ($r in $running.ToArray()) {
    if ($r.P.HasExited) {
      $r.P.WaitForExit()
      $text = $r.Out.Result + $r.Err.Result
      [IO.File]::WriteAllText($r.Log, $text, $Utf8NoBom)
      if ($r.P.ExitCode -ne 0) {
        $failed.Add($r)
        Write-Host "[fail] $($r.U.Src)" -ForegroundColor Red
        Write-Host $text
        if (Test-Path $r.U.Obj) { Remove-Item $r.U.Obj }
      }
      $running.Remove($r) | Out-Null
    }
  }
}
if ($failed.Count) { throw "$($failed.Count) unit(s) failed to compile" }

# ---- 4b. asm ABI gate ------------------------------------------------------
# The .S files are written for the Linux calling convention and only work on
# Windows through the shims in win32\patches.ps1.  Before linking them into
# rsync.exe, call them with a sentinel in every Win64 callee-saved register
# and compare their results with plain C (win32\test\abi_check.c).  A shim
# that misses a register (as the old Cygwin one did with xmm8/xmm9) stops the
# build here instead of silently corrupting checksums.
if ($UseAsm) {
  Write-Step 'asm ABI check'
  $testDir = Join-Path $Win32Dir 'test'
  $abiExe = Join-Path $ObjDir 'abi_check.exe'
  $asmObjs = $units | Where-Object { $_.Src -match '\.S$' -or $_.Src -match 'cpu_model\.c$' } | ForEach-Object { $_.Obj }
  & $Zig cc -target $Target @OptFlags -w -o $abiExe (Join-Path $testDir 'abi_check.c') (Join-Path $testDir 'abi_call.S') @asmObjs
  if ($LASTEXITCODE -ne 0) { throw 'building the asm ABI check failed' }
  & $abiExe
  if ($LASTEXITCODE -ne 0) { throw 'asm ABI check failed: fix the Win64 shims in win32\patches.ps1, or build with -NoAsm' }
}

# ---- 5. link ---------------------------------------------------------------
Write-Step 'link rsync.exe'
$exe = Join-Path $ObjDir 'rsync.exe'
$rc  = Join-Path $Win32Dir 'rsync.rc'
$res = Join-Path $ObjDir 'rsync.res'
# Version resource (Explorer "Details" tab): numbers come from the tarball name.
$verStr = $Rsync -replace '^rsync-', ''
$verNum = ((@($verStr -split '[^0-9]+' | Where-Object { $_ }) + @('0', '0', '0'))[0..2] + "$Revision") -join ','
$verHdr = @(
  "#define RSYNC_RC_VER_NUM $verNum",
  "#define RSYNC_RC_VER_STR `"$verStr`"",
  "#define RSYNC_RC_BUILD `"$Arch, $Toolchain$(if ($Native) { ', -march=native' })$(if (-not $UseAsm) { ', no asm' })`"",
  "#define RSYNC_RC_YEAR `"$((Get-Date).Year)`""
)
[IO.File]::WriteAllLines((Join-Path $ObjDir 'rsync-version.h'), $verHdr)
& $Zig rc /:target $RcTarget /i $ObjDir /fo $res $rc
if ($LASTEXITCODE -ne 0) { throw 'zig rc failed' }
$objs = $units | ForEach-Object { $_.Obj }
# The one C++ unit uses no exceptions/RTTI/STL.  With msvc, "zig c++" would
# try to build libc++ against the MSVC headers (which fails), so link as C.
# oldnames: the POSIX aliases (fdopen -> _fdopen, ...) MSVC links by default.
$Linker = if ($IsMsvc) { 'cc' } else { 'c++' }
$Libs = @('-lws2_32', '-ladvapi32', '-lshell32') + $(if ($IsMsvc) { @('-loldnames') } else { @() })
& $Zig $Linker -target $Target @OptFlags -s -static -o $exe @objs $res @Libs
if ($LASTEXITCODE -ne 0) { throw 'link failed' }

New-Item -ItemType Directory -Force $OutDir | Out-Null
Copy-Item $exe (Join-Path $OutDir 'rsync.exe') -Force
Write-Step "done: $((Resolve-Path (Join-Path $OutDir 'rsync.exe')).Path)"
