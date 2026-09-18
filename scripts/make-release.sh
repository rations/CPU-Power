#!/bin/sh
# Build the release source tarball.
#
# WHY A SOURCE TARBALL AND NOT A BINARY ONE. The helper's absolute path is a compile-time
# constant, and the same constant is written into the installed polkit action's exec.path
# annotation, which polkit compares against the exec'd path BYTE FOR BYTE with no
# canonicalisation. A relocatable binary tarball -- one the user unpacks wherever they like --
# would break that match for every prefix except the one it was built for, and the failure is
# SILENT: pkexec falls back to org.freedesktop.policykit.exec and the tool starts asking for an
# administrator password it promises never to ask for. So the archive ships sources, and the
# path is fixed when the recipient configures.
#
# WHY ustar. The archive is created with GNU tar but written in the POSIX.1-1988 ustar format,
# which every tar in existence reads -- bsdtar, busybox tar, toybox tar, Python tarfile, 7-Zip.
# pax (POSIX.1-2001) is the newer standard and is what GNU tar calls --format=posix, but it
# carries extended headers that older and smaller tars either ignore or spill into the tree as
# stray PaxHeaders files. ustar has real limits -- 255-byte paths, 8 GiB files, no sub-second
# mtimes -- and none of them are anywhere near binding here; the check below asserts that rather
# than assuming it.
#
# Nothing in the archive, and nothing in this script, is specific to an init system. The project
# ships no service unit and no init script: the GUI is started by the user and the helper is
# started by the GUI. What varies between a systemd machine and a sysvinit one is the session
# tracker polkit consults (logind, elogind or ConsoleKit2), which is resolved at run time by
# polkitd and not by anything packaged here.
#
# This script is POSIX sh, but it is a MAINTAINER tool and it does use GNU tar, GNU findutils and
# coreutils -- --sort, --numeric-owner, find -printf, sha256sum, date -d. That asymmetry is
# deliberate and worth stating: portability is a property the ARCHIVE has to have, because it is
# unpacked on machines nobody here controls; the script only ever runs on the machine cutting the
# release. It checks for GNU tar and says so rather than quietly writing a different format.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
outdir=$root/dist
verify=1

usage() {
    cat <<USAGE
usage: make-release.sh [--out DIR] [--no-verify]

  --out DIR     where to write the tarballs (default: dist/)
  --no-verify   skip the unpack-and-build check (not recommended)

Honours SOURCE_DATE_EPOCH. When unset, the newest mtime in the staged tree is
used, so the same tree yields a byte-identical archive.
USAGE
}

while [ $# -gt 0 ]; do
    case $1 in
        --out) outdir=$2; shift 2 ;;
        --out=*) outdir=${1#--out=}; shift ;;
        --no-verify) verify=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'make-release.sh: unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

die() { printf 'make-release.sh: %s\n' "$*" >&2; exit 1; }
say() { printf '  %s\n' "$*"; }

# ---------------------------------------------------------------------------------------------
# Version. Single source of truth is the project() line, the same place the binaries get it from.
# ---------------------------------------------------------------------------------------------
version=$(sed -n 's/^project(cpu-power VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || die "could not read the version out of CMakeLists.txt"
name=cpu-power-$version

printf '\n%s %s\n\n' "Building release tarball for" "$name"

# ---------------------------------------------------------------------------------------------
# What ships. This is an ALLOWLIST, not an exclude list, and deliberately so: an exclude list
# fails open, so the day somebody drops a scratch file at the top of the tree it ships. This
# fails closed -- a new top-level entry is absent from the release until it is named here.
# ---------------------------------------------------------------------------------------------
members='
CMakeLists.txt
LICENSE
NOTICE
README.md
.clang-format
icon.png
docs
helper
packaging
resources
scripts
src
tests
tools
'

# RULES.md and SECURITY.md are deliberately ABSENT: both are development documents, both are
# git-ignored, and no shipped file cites either any more. The gate below fails the release if one
# starts to. .gitignore is absent for the same reason from the other direction -- the tarball is
# not a checkout, and naming the private documents is the one thing that file exists to do.

stage=$(mktemp -d "${TMPDIR:-/tmp}/cpu-power-release.XXXXXX")
trap 'rm -rf "$stage"' EXIT INT TERM

printf 'Staging\n'
mkdir "$stage/$name"
for m in $members; do
    [ -e "$root/$m" ] || die "listed member is missing from the tree: $m"
    # -p so mtimes survive the copy: without it every staged file is timestamped "now", and the
    # epoch derived below stops being a function of the content.
    cp -Rp "$root/$m" "$stage/$name/$m"
done
say "$(find "$stage/$name" -type f | wc -l) files"

# ---------------------------------------------------------------------------------------------
# Gates. Each one is a thing that has to be true of the archive, asserted rather than assumed.
# ---------------------------------------------------------------------------------------------
printf 'Checking the staged tree\n'

# 1. Nothing local-only escaped. These carry absolute paths from the development machine and
#    notes that are not part of the published project.
for forbidden in CLAUDE.md PLAN.md task.md .claude build .git; do
    if find "$stage/$name" -name "$forbidden" -print | grep -q .; then
        die "local-only path present in the release: $forbidden"
    fi
done
find "$stage/$name" \( -name 'build-*' -o -name '*.o' -o -name '*~' -o -name '.*.swp' \) \
     -exec rm -rf {} + 2>/dev/null || true
say "no local-only files"

# 1b. Nothing in the archive NAMES a local-only document or a path from the development machine.
#     This is the half of the rule that a file list cannot enforce: excluding RULES.md is easy,
#     and leaving eighty comments that cite it is the mistake that actually happens. A dangling
#     citation is worse than no citation -- it tells the reader a document exists and then does
#     not give it to them. Same for an absolute /home path: it is a private detail of one machine
#     and it is never right on the recipient's.
#     This script is exempt from its own scan, and only this script: a grep for a name has to
#     contain that name. Nothing else in the tree gets an exemption.
leaks=$(grep -rIln -e 'RULES\.md' -e 'CLAUDE\.md' -e 'PLAN\.md' -e 'task\.md' -e 'SECURITY\.md' \
                   -e '/home/' -e '/root/' "$stage/$name" 2>/dev/null |
        grep -v '/scripts/make-release\.sh$' || true)
if [ -n "$leaks" ]; then
    die "shipped files name a local-only document or an absolute home path:
$(printf '%s\n' "$leaks" | sed "s|^$stage/$name/|  |")"
fi
say "no file names a local-only document or a development-machine path"

# 2. No mode bits that have no business in a source archive.
if find "$stage/$name" \( -perm -4000 -o -perm -2000 \) -print | grep -q .; then
    die "a setuid or setgid bit is set in the staged tree"
fi
say "no setuid or setgid bits"

# 3. No symlinks. The fixtures are policy-space trees precisely so there are none, and a tarball
#    with no symlinks cannot have a link-escape problem at unpack time.
if find "$stage/$name" -type l -print | grep -q .; then
    die "the staged tree contains a symlink; ustar would ship it and unpack is the recipient's"
fi
say "no symlinks"

# 4. Every path fits ustar. The format stores a 100-byte name plus a 155-byte prefix, split at a
#    '/', so a path can reach 255 only if it splits. Checking the split is the actual constraint;
#    checking the total length is not.
too_long=$(cd "$stage" && find "$name" | awk '
    { p = $0; n = length(p)
      if (n <= 100) next
      ok = 0
      # A split at the "/" in position i gives prefix = 1..i-1 and name = i+1..n. Both halves
      # have to fit their fields, and the slash itself is not stored.
      for (i = 1; i < n; i++)
          if (substr(p, i, 1) == "/" && i - 1 <= 155 && n - i <= 100) { ok = 1; break }
      if (!ok) print p }')
[ -z "$too_long" ] || die "path does not fit the ustar name/prefix split:
$too_long"
say "every path fits ustar (longest: $(cd "$stage" && find "$name" | awk '{ if (length($0) > n) n = length($0) } END { print n }') bytes)"

# 5. Normalise modes and timestamps, so the archive is a function of the content.
find "$stage/$name" -type d -exec chmod 755 {} +
find "$stage/$name" -type f -exec chmod 644 {} +
find "$stage/$name" -name '*.sh' -exec chmod 755 {} +
chmod 755 "$stage/$name/scripts/emit-appicon.py"

if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    epoch=$SOURCE_DATE_EPOCH
else
    epoch=$(find "$stage/$name" -type f -printf '%T@\n' 2>/dev/null |
            awk -F. '{ if ($1 > n) n = $1 } END { print n+0 }')
    [ "${epoch:-0}" -gt 0 ] || epoch=$(date +%s)
fi
find "$stage/$name" -exec touch -d "@$epoch" {} +
say "mtime pinned to $(date -u -d "@$epoch" '+%Y-%m-%d %H:%M:%S UTC')"

# ---------------------------------------------------------------------------------------------
# Archive.
# ---------------------------------------------------------------------------------------------
printf 'Archiving\n'
tar --version 2>/dev/null | head -1 | grep -q 'GNU tar' ||
    die "GNU tar is required to create the archive (the archive it writes is plain ustar and
reads anywhere; it is the --sort and --numeric-owner flags that are GNU)"

mkdir -p "$outdir"
tar -C "$stage" \
    --format=ustar \
    --sort=name \
    --owner=0 --group=0 --numeric-owner \
    --mtime="@$epoch" \
    -cf "$stage/$name.tar" "$name"

# -n drops the filename and timestamp from the gzip header, which is what makes .gz reproducible.
gzip -9nc "$stage/$name.tar" > "$outdir/$name.tar.gz"
say "$outdir/$name.tar.gz ($(du -h "$outdir/$name.tar.gz" | cut -f1))"

if command -v xz >/dev/null 2>&1; then
    xz -9ec "$stage/$name.tar" > "$outdir/$name.tar.xz"
    say "$outdir/$name.tar.xz ($(du -h "$outdir/$name.tar.xz" | cut -f1))"
else
    say "xz not found -- gzip only. gzip is the one to publish if only one is published."
fi

( cd "$outdir" && sha256sum "$name".tar.* > "$name.sha256" )
say "$outdir/$name.sha256"

# ---------------------------------------------------------------------------------------------
# Verify. Unpack what was actually written, somewhere else, and build it. A tarball that has not
# been unpacked and built is a tarball nobody has tested.
# ---------------------------------------------------------------------------------------------
if [ "$verify" -eq 0 ]; then
    printf '\nSkipped verification (--no-verify).\n\n'
    exit 0
fi

printf 'Verifying the archive by unpacking and building it\n'
check=$stage/check
mkdir "$check"
tar -C "$check" -xf "$outdir/$name.tar.gz"
[ -d "$check/$name" ] || die "the archive does not unpack into a single $name/ directory"

for forbidden in CLAUDE.md PLAN.md task.md .claude; do
    if find "$check/$name" -name "$forbidden" -print | grep -q .; then
        die "local-only path survived into the written archive: $forbidden"
    fi
done
say "unpacks into $name/, nothing local-only inside"

if cmake -S "$check/$name" -B "$check/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$check/prefix" >"$stage/cmake.log" 2>&1 &&
   cmake --build "$check/build" -j"$(nproc 2>/dev/null || echo 2)" >>"$stage/cmake.log" 2>&1; then
    say "configures and builds from a clean unpack"
else
    tail -40 "$stage/cmake.log" >&2
    die "the unpacked archive does not build (full log: $stage/cmake.log)"
fi

if ( cd "$check/$name" && "$check/build/coretest" >"$stage/coretest.log" 2>&1 ); then
    say "coretest passes against every fixture"
else
    tail -40 "$stage/coretest.log" >&2
    die "coretest fails in the unpacked tree (full log: $stage/coretest.log)"
fi

# DESTDIR, not --prefix. The polkit actiondir is an absolute path that does NOT derive from
# CMAKE_INSTALL_PREFIX -- polkitd reads two fixed directories and that search path is not ours to
# relocate -- so --prefix alone would send the action to /usr/share/polkit-1/actions for real.
# DESTDIR prepends to the absolute paths too, which is what makes this runnable unprivileged and
# what makes it the same install a distro packager performs.
destdir=$check/destdir
if ! DESTDIR="$destdir" cmake --install "$check/build" >"$stage/install.log" 2>&1; then
    tail -30 "$stage/install.log" >&2
    die "the unpacked archive does not install (full log: $stage/install.log)"
fi

if find "$destdir" \( -perm -4000 -o -perm -2000 \) -print | grep -q .; then
    die "the install set a setuid or setgid bit"
fi
say "installs under DESTDIR with no setuid or setgid bit"

# The polkit action is the security boundary, and the way it fails is SILENT: if exec.path stops
# matching the path the GUI execs, byte for byte, pkexec falls back to
# org.freedesktop.policykit.exec and the desktop's agent asks for an administrator password.
# Releasing that is worse than releasing a build error, because nothing crashes. So the release
# asserts the shipped action against the shipped binary rather than trusting the build.
policy=$(find "$destdir" -name '*.policy' -print | head -1)
[ -n "$policy" ] || die "no .policy action was installed"

helper=$(find "$destdir" -name 'cpu-power-helper' -type f -print | head -1)
[ -n "$helper" ] || die "no helper binary was installed"
helper_abs=${helper#"$destdir"}

grep -q "<annotate key=\"org.freedesktop.policykit.exec.path\">$helper_abs</annotate>" "$policy" ||
    die "the action's exec.path does not match the installed helper path ($helper_abs).
polkit compares these byte for byte with no canonicalisation; a mismatch is silent and costs
the user a password prompt."
say "action exec.path matches the installed helper, byte for byte"

grep -q '<allow_any>no</allow_any>' "$policy" &&
grep -q '<allow_inactive>no</allow_inactive>' "$policy" &&
grep -q '<allow_active>yes</allow_active>' "$policy" ||
    die "the action's defaults are not no/no/yes -- that triple is the entire security model"
say "action defaults are allow_any=no, allow_inactive=no, allow_active=yes"

# Matched as an element, not as the bare word: the template carries a comment explaining why the
# annotation is absent, and a grep loose enough to hit that comment is a gate that cries wolf.
grep -q '<annotate[^>]*allow_gui' "$policy" &&
    die "the action sets allow_gui; it must never be set, in any form"
say "action does not set allow_gui"

printf '\n%s\n\n' "Release $version is built and verified in $outdir."
