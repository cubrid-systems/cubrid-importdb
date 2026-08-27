#!/bin/bash
# Static half of the upstream contract: verify surface_manifest.txt without
# needing a server, and in source mode without needing a build.
#
#   --install <dir>   an installed CUBRID  ($CUBRID)     -- header/decl/export/grep-free
#   --source  <dir>   a CUBRID source tree               -- install/decl/const/grep
#
# Source mode is the cheap guard the engine repo can run on every PR: it reads
# the install rules and the headers straight out of the tree, in seconds.
set -uo pipefail
MODE=; ROOT=
while [ $# -gt 0 ]; do
  case "$1" in
    --install) MODE=install; ROOT="$2"; shift 2;;
    --source)  MODE=source;  ROOT="$2"; shift 2;;
    *) echo "usage: $0 --install <dir> | --source <dir>"; exit 2;;
  esac
done
[ -n "$MODE" ] && [ -d "$ROOT" ] || { echo "usage: $0 --install <dir> | --source <dir>"; exit 2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$HERE/surface_manifest.txt"
PASS=0; FAIL=0; SKIP=0

ok   () { PASS=$((PASS+1)); printf '  PASS  %s\n' "$1"; }
bad  () { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$1"; }
skip () { SKIP=$((SKIP+1)); }

# In source mode the installed name may differ from the file name (dbi.h is
# installed as a RENAME of src/compat/dbi_compat.h).
src_header () {
  case "$1" in
    dbi.h) echo "$ROOT/src/compat/dbi_compat.h";;
    error_code.h) echo "$ROOT/src/base/error_code.h";;
    *) echo "$ROOT/src/compat/$1";;
  esac
}

# Dump the dynamic symbol table once. Note: do NOT pipe nm into `grep -q` under
# `set -o pipefail` -- grep exits on the first match, nm dies of SIGPIPE, and
# pipefail then reports the whole pipeline as failed even though the symbol was
# found. That cost an hour once; hence the temp file.
SYMS=""
if [ "$MODE" = install ]; then
  SYMS="$(mktemp)"
  trap 'rm -f "$SYMS"' EXIT
  lib="$(ls "$ROOT"/lib/libcubridcs.so* 2>/dev/null | head -1)"
  if [ -n "$lib" ]; then
    nm -D --defined-only "$lib" 2>/dev/null | awk '{print $3}' > "$SYMS"
  fi
  echo "surface check: mode=$MODE root=$ROOT  ($(wc -l < "$SYMS") dynamic symbols)"
else
  echo "surface check: mode=$MODE root=$ROOT"
fi
while read -r kind a b rest; do
  case "${kind:-}" in
    ''|'#') continue;;
  esac
  case "$kind" in
    header)
      if [ "$MODE" = install ]; then
        [ -f "$ROOT/include/$a" ] && ok "header installed: $a" || bad "header MISSING from install: $a"
      else skip; fi
      ;;
    install)
      if [ "$MODE" = source ]; then
        if grep -qE "(^|/)$a\b" "$ROOT/cubrid/CMakeLists.txt" 2>/dev/null; then
          ok "install rule present: $a"
        else
          bad "install rule GONE (cubrid/CMakeLists.txt no longer installs $a)"
        fi
      else skip; fi
      ;;
    decl)
      if [ "$MODE" = install ]; then h="$ROOT/include/$a"; else h="$(src_header "$a")"; fi
      if [ ! -f "$h" ]; then
        bad "declaring header not found: $a (for $b)"
      elif grep -qE "\b$b[[:space:]]*\(" "$h"; then
        ok "declared: $b in $a"
      else
        bad "NOT declared: $b in $a"
      fi
      ;;
    export)
      if [ "$MODE" = install ]; then
        if [ ! -s "$SYMS" ]; then
          bad "no dynamic symbols read from libcubridcs (checked $a)"
        elif grep -qx "$a" "$SYMS"; then
          ok "exported unmangled: $a"
        else
          bad "NOT exported unmangled (mangled or hidden): $a"
        fi
      else skip; fi
      ;;
    const)
      if [ "$MODE" = source ]; then
        f="$ROOT/$a"
        if [ ! -f "$f" ]; then
          bad "constant file not found: $a"
        elif grep -qE "\b$b[[:space:]]*=[[:space:]]*$rest\b" "$f"; then
          ok "constant unchanged: $b = $rest"
        else
          got=$(grep -oE "\b$b[[:space:]]*=[[:space:]]*[0-9]+" "$f" | head -1)
          bad "constant CHANGED: expected $b = $rest, found '${got:-nothing}' (this repo replicates it)"
        fi
      else skip; fi
      ;;
    grep)
      if [ "$MODE" = source ]; then
        f="$ROOT/$a"
        if [ ! -f "$f" ]; then
          bad "file not found: $a"
        elif grep -qE "$b" "$f"; then
          ok "source contract holds: $a ~ $b"
        else
          bad "source contract BROKEN in $a (pattern '$b'): $rest"
        fi
      else skip; fi
      ;;
  esac
done < "$MANIFEST"

echo
echo "$PASS passed, $FAIL failed, $SKIP not applicable in $MODE mode"
[ "$FAIL" -eq 0 ] || exit 1
