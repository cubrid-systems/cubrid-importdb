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
#   --install-only                                skip the source tarball
#
# Leaves <dest>/src, <dest>/install, and <dest>/engine.env (VERSION, COMMIT,
# SOURCE_URL) behind. Skips the download when <dest>/dl already holds both
# tarballs, so a CI cache hit costs nothing.
#
# --install-only leaves no <dest>/src and halves the download. It is for an
# engine this repo does not COMPILE against but only RUNS: the crossversion test
# lane needs an older install to write a dump with (its createdb, csql -S and
# unloaddb -S), and never needs that engine's headers. Do not use it for the
# engine the utility is built against -- that one needs the source tree for its
# generated headers.
#
# Exit: 0 ok, 77 could not fetch (network, missing artifact) -- CI reads 77 as SKIP.
set -uo pipefail

FTP_BASE="${FTP_BASE:-https://ftp.cubrid.org/CUBRID_Engine}"
MODE=""; ARG=""; DEST=""; INSTALL_ONLY=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --nightly)         MODE=nightly; ARG="${2:-11.5}"; shift 2 ;;
    --nightly-version) MODE=nightly_version; ARG="${2:?}"; shift 2 ;;
    --release)         MODE=release; ARG="${2:?}"; shift 2 ;;
    --install-only)    INSTALL_ONLY=1; shift ;;
    -*) echo "fetch_engine: unknown option '$1'" >&2; exit 77 ;;
    *) DEST="$1"; shift ;;
  esac
done
[ -n "$MODE" ] && [ -n "$DEST" ] || {
  sed -n '3,29p' "$0" >&2
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
    # A `*_latest` area names its artifacts after the area: 11.4_latest holds
    # CUBRID-11.4-latest-Linux.x86_64.tar.gz.
    NAME=$(echo "$V" | tr '_' '-')
    SRC_TGZ="cubrid-$NAME.tar.gz"
    INS_TGZ="CUBRID-$NAME-Linux.x86_64.tar.gz"
    COMMIT=""
    # A patch-level area does not: 11.2.6 holds
    # CUBRID-11.2.6.0790-dda2520-Linux.x86_64.tar.gz, the build number and the
    # commit included. Pinning a patch level is the reason to reach for a release
    # area at all -- a compatibility lane wants the version the customer is
    # leaving, not whatever is newest on that line -- so resolve the real name
    # from the directory when the area-derived one is not there.
    if ! curl -sS --fail --head --max-time 60 "$DIR/$INS_TGZ" >/dev/null 2>&1; then
      say "  $INS_TGZ is not in that area -- resolving the published name"
      FOUND=$(curl -sS --fail --max-time 90 "$DIR/" 2>/dev/null \
              | grep -oE 'CUBRID-[0-9][0-9A-Za-z.-]*-Linux\.x86_64\.tar\.gz' | sort -u | head -1)
      if [ -n "$FOUND" ]; then
        INS_TGZ="$FOUND"
        # the source tarball of the same build differs only in case and prefix
        SRC_TGZ="cubrid-$(echo "$FOUND" | sed 's/^CUBRID-//; s/-Linux\.x86_64\.tar\.gz$//').tar.gz"
        COMMIT=$(echo "$FOUND" | sed -n 's/^CUBRID-[0-9][0-9.]*-\([0-9a-f]\{7,\}\)-Linux.*/\1/p')
        say "  resolved to $INS_TGZ"
      fi
    fi
    ;;
esac

# One list, so the download, the checksum pass and the extract cannot disagree
# about which halves of the pair are in play.
WANTED="$SRC_TGZ:src.tar.gz $INS_TGZ:install.tar.gz"
if [ "$INSTALL_ONLY" = "1" ]; then
  WANTED="$INS_TGZ:install.tar.gz"
fi

say "version $V"
say "  from   $DIR"
[ "$INSTALL_ONLY" = "1" ] && say "  install only -- no source tarball"

mkdir -p "$DEST/dl" || exit 77

# ---- download (skipped when both tarballs are already there, i.e. a cache hit)
have_all=1
for pair in $WANTED; do
  [ -s "$DEST/dl/${pair##*:}" ] || have_all=0
done
if [ "$have_all" = "1" ]; then
  say "every wanted tarball is already present -- skipping the download"
else
  for pair in $WANTED; do
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
  for pair in $WANTED; do
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
mkdir -p "$DEST/install" || exit 77
say "extracting"
if [ "$INSTALL_ONLY" != "1" ]; then
  mkdir -p "$DEST/src" || exit 77
  tar xzf "$DEST/dl/src.tar.gz" -C "$DEST/src" --strip-components=1 || exit 77
  [ -f "$DEST/src/src/executables/util_support.c" ] || { say "extracted source does not look like a CUBRID tree"; exit 77; }
fi
tar xzf "$DEST/dl/install.tar.gz" -C "$DEST/install" --strip-components=1 || exit 77
[ -f "$DEST/install/lib/libcubridcs.so" ] || { say "extracted install has no libcubridcs.so"; exit 77; }
# An install that cannot answer `cubrid --version` is not usable, whatever the
# tarball looked like. Reported here rather than at first use.
[ -x "$DEST/install/bin/cubrid" ] || { say "extracted install has no bin/cubrid"; exit 77; }

{
  echo "VERSION=$V"
  echo "COMMIT=$COMMIT"
  echo "SOURCE_URL=$DIR"
} > "$DEST/engine.env"

if [ "$INSTALL_ONLY" = "1" ]; then
  say "ready: install=$DEST/install (no source tarball was fetched)"
else
  say "ready: src=$DEST/src install=$DEST/install"
fi
[ -n "$COMMIT" ] && say "engine commit $COMMIT"
exit 0
