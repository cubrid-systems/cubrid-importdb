#!/bin/bash
# Fetch a CUBRID engine from ftp.cubrid.org as a (source, install) PAIR.
#
# Both halves come from the same published directory, which is the point: the
# source tree provides the internal headers this utility compiles against, the
# install provides libcubridcs to link and a server to import into, and taking
# them from one place removes any question of the two disagreeing.
#
# The nightly and release areas have the same shape, so one mechanism covers both
# and there is no container registry in the loop.
#
#   fetch_engine.sh --nightly [SERIES] <dest>     newest nightly in SERIES (default 11.5)
#   fetch_engine.sh --nightly-version <V> <dest>  a specific nightly, e.g. 11.5.0.2494-4b6ae5c
#   fetch_engine.sh --release <NAME> <dest>       a release area, e.g. 11.4_latest
#
# Leaves <dest>/src, <dest>/install, and <dest>/engine.env (VERSION, COMMIT,
# SOURCE_URL) behind. Skips the download when <dest>/dl already holds both
# tarballs, so a CI cache hit costs nothing.
#
# Exit: 0 ok, 77 could not fetch (network, missing artifact) -- CI reads 77 as SKIP.
set -uo pipefail

FTP_BASE="${FTP_BASE:-https://ftp.cubrid.org/CUBRID_Engine}"
MODE=""; ARG=""; DEST=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --nightly)         MODE=nightly; ARG="${2:-11.5}"; shift 2 ;;
    --nightly-version) MODE=nightly_version; ARG="${2:?}"; shift 2 ;;
    --release)         MODE=release; ARG="${2:?}"; shift 2 ;;
    -*) echo "fetch_engine: unknown option '$1'" >&2; exit 77 ;;
    *) DEST="$1"; shift ;;
  esac
done
[ -n "$MODE" ] && [ -n "$DEST" ] || {
  sed -n '3,20p' "$0" >&2
  exit 77
}

say () { echo "fetch_engine: $*"; }

# ---- resolve the directory and the artifact names
case "$MODE" in
  nightly|nightly_version)
    if [ "$MODE" = nightly ]; then
      say "resolving the newest nightly in series $ARG"
      V=$(curl -sS --fail --max-time 90 "$FTP_BASE/nightly/daily_build/" 2>/dev/null \
          | grep -oE 'href="'"$ARG"'\.[0-9.]+-[0-9a-f]+/"' \
          | sed 's/href="//;s|/"$||' | sort -u | sort -V | tail -1)
      [ -n "$V" ] || { say "could not resolve a nightly in series $ARG"; exit 77; }
    else
      V="$ARG"
    fi
    DIR="$FTP_BASE/nightly/daily_build/$V/drop"
    SRC_TGZ="cubrid-$V.tar.gz"
    INS_TGZ="CUBRID-$V-Linux.x86_64.tar.gz"
    COMMIT="${V##*-}"
    ;;
  release)
    V="$ARG"
    DIR="$FTP_BASE/$V"
    # release areas name the artifacts after the area, e.g. 11.4_latest -> 11.4-latest
    NAME=$(echo "$V" | tr '_' '-')
    SRC_TGZ="cubrid-$NAME.tar.gz"
    INS_TGZ="CUBRID-$NAME-Linux.x86_64.tar.gz"
    COMMIT=""
    ;;
esac

say "version $V"
say "  from   $DIR"

mkdir -p "$DEST/dl" || exit 77

# ---- download (skipped when both tarballs are already there, i.e. a cache hit)
if [ -s "$DEST/dl/src.tar.gz" ] && [ -s "$DEST/dl/install.tar.gz" ]; then
  say "both tarballs already present -- skipping the download"
else
  for pair in "$SRC_TGZ:src.tar.gz" "$INS_TGZ:install.tar.gz"; do
    remote="${pair%%:*}"; local="${pair##*:}"
    say "downloading $remote"
    if ! curl -sS --fail --max-time 1800 -o "$DEST/dl/$local" "$DIR/$remote"; then
      say "could not download $DIR/$remote"
      exit 77
    fi
  done
  curl -sS --fail --max-time 120 -o "$DEST/dl/hash.md5" "$DIR/hash.md5" 2>/dev/null || true
fi

# ---- verify against the directory's own checksums when it publishes them
if [ -s "$DEST/dl/hash.md5" ]; then
  fail=0
  for pair in "$SRC_TGZ:src.tar.gz" "$INS_TGZ:install.tar.gz"; do
    remote="${pair%%:*}"; local="${pair##*:}"
    # the drop writes "<md5> *<filename>" (md5sum's binary marker)
    want=$(grep -E "[[:space:]][*]?${remote}$" "$DEST/dl/hash.md5" | awk '{print $1}' | head -1)
    if [ -z "$want" ]; then
      say "  $remote: not listed in hash.md5 -- not verified"
      continue
    fi
    got=$(md5sum "$DEST/dl/$local" | awk '{print $1}')
    if [ "$want" = "$got" ]; then
      say "  $remote: md5 ok"
    else
      say "  $remote: MD5 MISMATCH (want $want, got $got)"
      fail=1
    fi
  done
  [ "$fail" -eq 0 ] || exit 77
else
  say "no hash.md5 published here -- downloads not verified"
fi

# ---- extract
rm -rf "$DEST/src" "$DEST/install"
mkdir -p "$DEST/src" "$DEST/install" || exit 77
say "extracting"
tar xzf "$DEST/dl/src.tar.gz"     -C "$DEST/src"     --strip-components=1 || exit 77
tar xzf "$DEST/dl/install.tar.gz" -C "$DEST/install" --strip-components=1 || exit 77
[ -f "$DEST/src/src/executables/util_support.c" ] || { say "extracted source does not look like a CUBRID tree"; exit 77; }
[ -f "$DEST/install/lib/libcubridcs.so" ] || { say "extracted install has no libcubridcs.so"; exit 77; }

{
  echo "VERSION=$V"
  echo "COMMIT=$COMMIT"
  echo "SOURCE_URL=$DIR"
} > "$DEST/engine.env"

say "ready: src=$DEST/src install=$DEST/install"
[ -n "$COMMIT" ] && say "engine commit $COMMIT"
exit 0
