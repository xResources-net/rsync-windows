#!/bin/bash
# Regression tests for the native Windows rsync.exe (run from Git Bash or
# MSYS2 bash).  Everything happens under %TEMP%\rsync-wtest; the "remote"
# side is reached through fakessh.exe (MSYS2 bash + MSYS2's own rsync, or
# this rsync.exe in --server mode), so no ssh server is needed.
#
#   win32/test/run-tests.sh [path\to\rsync.exe]
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

here="$(cd "$(dirname "$0")" && pwd -W)"
R="$(cygpath -m "${1:-$here/../../../bin/rsync.exe}")"
FAKESSH="$here/fakessh.exe"
MSYS_RSYNC=/c/PortableApps/msys64/usr/bin/rsync.exe
[ -x "$FAKESSH" ] || /c/PortableApps/zig/zig.exe cc -target x86_64-windows-gnu -O2 -municode \
	"$here/fakessh.c" -o "$FAKESSH" || exit 1

W="$(cygpath -m "$TEMP")/rsync-wtest"      # C:/Users/.../rsync-wtest (Windows form)
U="/$(echo "${W:0:1}" | tr A-Z a-z)${W:2}"     # /c/Users/... (understood by Git Bash and MSYS2)
C="/cygdrive/$(echo "${W:0:1}" | tr A-Z a-z)${W:2}"   # /cygdrive/c/... (rsync form)
pass=0 fail=0
ok()   { echo "  PASS $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL $1"; fail=$((fail+1)); }
run()  { "$@" || { bad "exit $? from: $*"; return 1; }; }
same() { diff -r "$U/$1" "$U/$2" >/dev/null && ok "$3" || bad "$3"; }

rm -rf "$U" && mkdir -p "$U/src/sub/deeper" "$U/src/空 dir"
cd "$U/src"
echo hello > a.txt; : > empty.txt
head -c 3000000 /dev/urandom > big.bin; head -c 70000 /dev/urandom > sub/mid.bin
echo deep > sub/deeper/d.txt; echo uni > "空 dir/ファイル.txt"
echo ro > ro.txt; attrib +R "$(cygpath -w "$U/src/ro.txt")" >/dev/null
cd "$U"

echo "== local copy"
run "$R" -a "$W/src/" "$W/local/" && same src local "local copy"
[ -z "$("$R" -ai "$W/src/" "$W/local/")" ] && ok "no-op rerun is silent" || bad "no-op rerun is silent"
attrib "$(cygpath -w "$U/local/ro.txt")" | grep -q "R " && ok "read-only attribute kept" || bad "read-only attribute kept"

echo "== local delta + delete"
sleep 1  # rsync compares whole seconds by default (--modify-window=0)
printf XYZ | dd of=src/big.bin bs=1 seek=1500000 conv=notrunc 2>/dev/null
touch local/extra.txt; mkdir -p local/extradir
lit=$("$R" -a --no-W --delete --stats "$W/src/" "$W/local/" | sed -n 's/^Literal data: \([0-9,]*\).*/\1/p' | tr -d ,)
same src local "delta result"
[ "${lit:-999999999}" -lt 100000 ] && ok "delta sent only ${lit} literal bytes" || bad "delta literal=$lit"
[ ! -e local/extra.txt ] && [ ! -e local/extradir ] && ok "--delete" || bad "--delete"

echo "== push to MSYS2 rsync over (fake) ssh"
run "$R" -az -e "$FAKESSH" "$W/src/" "localhost:$U/push/" && same src push "push"

echo "== checksums agree with MSYS2 rsync (asm-roll / asm-MD5 / SIMD vs. its C code)"
# Delta push: our rolling checksum + MD5 block sums must match what MSYS2's
# rsync computes, or nothing matches and the whole file goes as literal data.
sleep 1
printf ABCD | dd of=src/big.bin bs=1 seek=2000000 conv=notrunc 2>/dev/null
lit=$("$R" -a --no-W --checksum-choice=md5 --stats -e "$FAKESSH" "$W/src/" "localhost:$U/push/" |
	sed -n 's/^Literal data: \([0-9,]*\).*/\1/p' | tr -d ,)
same src push "md5 delta push result"
[ "${lit:-999999999}" -lt 100000 ] && ok "md5 delta push sent only ${lit} literal bytes" || bad "md5 delta push literal=$lit"
# Whole-file MD5 (-c): identical trees must give no transfer ('<'/'>') and no
# checksum difference (3rd itemize column 'c').  MSYS2 stores permissions its
# own way, so a 'p' column difference is expected and ignored.
[ -z "$("$R" -ai -c --checksum-choice=md5 -e "$FAKESSH" "$W/src/" "localhost:$U/push/" | grep -E '^[<>]|^..c')" ] \
	&& ok "-c md5 whole-file checksums agree" || bad "-c md5 whole-file checksums agree"

echo "== pull from MSYS2 rsync over (fake) ssh"
run "$R" -az -e "$FAKESSH" "localhost:$U/src/" "$W/pull/" && same src pull "pull"
attrib "$(cygpath -w "$U/pull/ro.txt")" | grep -q "R " && ok "pull keeps read-only" || bad "pull keeps read-only"

echo "== this rsync.exe as the remote server (receiving)"
run "$R" -a --delete --rsync-path="$R" -e "$FAKESSH" "$W/src/" "localhost:$C/srv/" && same src srv "native server receive"

echo "== this rsync.exe as the remote server (sending) to this rsync.exe"
# (MSYS2 rsync can't drive a native child: it hands ssh a Cygwin socketpair.)
run "$R" -a --rsync-path="$R" -e "$FAKESSH" "localhost:$C/src/" "$W/srvsend/" && same src srvsend "native server send"

echo "== long paths (> 260 chars)"
lp="$W/long"; for i in 1 2 3 4 5 6; do lp="$lp/dddddddddddddddddddddddddddddddddddddddddddddddd$i"; done
LP="$lp" powershell -NoProfile -Command '$p = "\\?\" + ($env:LP -replace "/","\"); New-Item -ItemType Directory -Force -Path $p | Out-Null; Set-Content -LiteralPath "$p\f.txt" -Value data'
run "$R" -a "$W/long/" "$W/long2/" && [ "$(find long2 -name f.txt | wc -l)" = 1 ] && ok "long path copy" || bad "long path copy"

echo "== names with characters Windows forbids (Cygwin-style U+F0xx mapping)"
mkdir -p "$U/msyssrc" && echo x > "$U/msyssrc/a:b?c.txt"
run "$R" -a -e "$FAKESSH" "localhost:$U/msyssrc/" "$W/special/" && \
	"$R" --list-only "$W/special/" | grep -q 'a:b?c.txt' && ok "special characters round-trip" || bad "special characters round-trip"

echo
echo "passed: $pass  failed: $fail"
[ "$fail" = 0 ]
