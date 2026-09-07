#!/bin/bash
# Fail if the binary copy-relocates an ENGINE data symbol.
#
# A R_X86_64_COPY relocation means the binary reserves space in its own BSS and
# the loader copies the library's variable into it -- at the size the binary was
# linked with. If the engine's variable is a different size in the install the
# binary actually runs against, the engine's own code walks off the end of an
# array living in the utility. That is not theoretical: it pinned this utility to
# exactly one engine build until 2026-09-08. Built against 11.5.0.2513 and run
# against 11.5.0.2539 -- 26 builds later, same series -- the loader warned
#
#   Symbol `prm_Def' has different size in shared object, consider re-linking
#
# and then SIGSEGV'd inside the engine's prm_check_environment(), reached through
# db_restart(), the first engine call. prm_Def had grown by 120 bytes.
#
# The cause was one macro. system_parameter.h defines its accessors as
# STATIC_INLINE __attribute__((ALWAYS_INLINE)) over GET_PRM(id) -> &prm_Def[id],
# so any use of HA_DISABLED() / prm_get_*_value() inlines a direct reference to
# the engine's extern array into our object. The fix is to read parameters through
# the exported, non-inline db_get_system_parameters () -- which is what
# docs/out-of-tree.md said we did all along, and what contract/ already checks.
#
# libstdc++ and libc symbols are expected and not flagged: the libstdc++ vtables
# come from the engine statically linking libstdc++ and re-exporting it (see the
# ABI note in CMakeLists.txt), and optarg/optind/stderr/stdout are ordinary libc.
#
# Exit: 0 clean, 1 an engine data symbol is copy-relocated, 77 cannot check.
set -uo pipefail
BIN="${1:?usage: check_relocs.sh <path to cubrid-importdb>}"
[ -f "$BIN" ] || { echo "not a file: $BIN"; exit 77; }
command -v readelf >/dev/null 2>&1 || { echo "readelf not found"; exit 77; }

syms=$(readelf -rW "$BIN" 2>/dev/null | awk '/R_X86_64_COPY/ { print $5 }')
if [ -z "$syms" ]; then
  echo "check_relocs: no COPY relocations at all -- nothing to check"
  exit 0
fi

bad=0
while read -r s; do
  [ -n "$s" ] || continue
  case "$s" in
    _Z*|*@GLIBC*) ;;                          # libstdc++ / libc, expected
    *)
      echo "check_relocs: FAIL -- engine data symbol is copy-relocated: $s"
      bad=1
      ;;
  esac
done <<< "$syms"

if [ "$bad" -ne 0 ]; then
  cat <<'EOM'

  A copy relocation on an engine variable pins this binary to the engine build it
  was linked against: if that variable's size changes, the engine indexes past the
  end of an array that lives in this binary. Read the value through an exported
  function instead -- db_get_system_parameters () for system parameters -- rather
  than through a header macro that inlines an array subscript.
EOM
  exit 1
fi
echo "check_relocs: PASS -- $(echo "$syms" | grep -c .) COPY relocation(s), none of them engine data"
