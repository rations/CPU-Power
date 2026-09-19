#!/bin/sh
# Build the release binary tarball.
#
# WHAT THIS SHIPS. Two binaries, the polkit action that connects them, the fonts the window is
# lettered in, the icon theme entries, a desktop entry and a manual page -- the installed surface
# and nothing else. No sources, no build system, no tests, no development tooling. The recipient
# unpacks it over / and runs the program.
#
# THE PREFIX IS PINNED TO /usr AND IS NOT AN OPTION. The helper's absolute path is a compile-time
# constant, and the same constant is written into the polkit action's exec.path annotation, which
# polkit compares against the exec'd path BYTE FOR BYTE with no canonicalisation. A relocatable
# binary tarball -- one the user unpacks wherever they like -- would break that match, and the
# failure is SILENT: pkexec falls back to org.freedesktop.policykit.exec and the tool starts
# asking for an administrator password it promises never to ask for. polkitd compounds this by
# reading actions from exactly two fixed directories, neither of which is prefix-relative. So the
# archive is laid out as an absolute tree rooted at usr/ and is unpacked at /, which is the only
# place the paths baked into it are true.
#
# HOW IT IS INSTALLED. Unpack it anywhere, cd in, and run the installer that ships at the top of
# the archive next to the usr/ tree:
#
#     tar -xf cpu-power-<version>-<arch>.tar.gz
#     cd cpu-power-<version>-<arch>
#     sudo ./install.sh
#
# packaging/install.sh is the source of that file and carries its own reasoning. It is not a
# convenience wrapper around tar: it refuses to install an archive whose polkit action does not
# name the exact path the helper is about to land on, which is a check only the recipient's
# machine is in a position to make, and it is the difference between a refusal and a silent
# password prompt later. It also uninstalls, which a tar command cannot.
#
# README.md, LICENSE and NOTICE ride at the top of the archive alongside it, for the person who
# unpacks it to read. They are not installed.
#
# WHAT A BINARY TARBALL COSTS, stated rather than discovered. The binaries are dynamically linked
# and carry the glibc symbol versions of the machine that built them, so a recipient on an older
# distribution gets a loader error rather than a program. The gates below MEASURE that -- the
# minimum glibc and the exact set of shared libraries -- and print it, because a number the
# publisher can put next to the download is the difference between an informed user and a bug
# report. The archive is also arch-specific and its name says so.
#
# ONE ARCHIVE: cpu-power-<version>-<arch>.tar.gz, plus its checksum. gzip because every machine
# can already open it.
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
# REPRODUCIBILITY is bounded here in a way it was not for a source tarball. Everything this
# script controls is normalised -- member order, owners, modes, mtimes, the gzip header -- so the
# archive is a function of the files that go into it. Whether the same tree yields the same
# BINARIES is a property of the toolchain, not of this script, and is not claimed.
#
# THE DEVELOPMENT GATES ARE NOT RUN HERE. coretest, scripts/sec-gate.sh and scripts/gate.sh are a
# condition on committing, not on packaging: they answer "is this code correct", they are run
# against the working tree by the person who changed it, and re-running them at release time
# would re-answer a settled question while giving the false impression that packaging is where
# the code gets checked. What this script asserts is narrower and is its own to assert --
# properties of the ARCHIVE. That the shipped action still matches the shipped helper IS checked
# below, because that pin is a property of the two files in the tarball and packaging alone can
# break it.
#
# This script is POSIX sh, but it is a MAINTAINER tool and it does use GNU tar, GNU findutils,
# coreutils and binutils -- --sort, --numeric-owner, find -printf, sha256sum, date -d, objdump.
# That asymmetry is deliberate and worth stating: portability is a property the ARCHIVE has to
# have, because it is unpacked on machines nobody here controls; the script only ever runs on the
# machine cutting the release. It checks for what it needs and says so rather than quietly
# writing a different format.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
outdir=$root/dist
verify=1

# Pinned, for the reason in the header. Not a flag: a release built for another prefix is a
# release whose polkit action does not match its helper.
prefix=/usr

usage() {
    cat <<USAGE
usage: make-release.sh [--out DIR] [--no-verify]

  --out DIR     where to write the tarball (default: dist/)
  --no-verify   skip the unpack-and-check stage (not recommended)

Builds a clean Release tree out of the current sources and packages the
installed surface as an absolute tree rooted at usr/, to be unpacked at /.

The development gates (coretest, sec-gate.sh, gate.sh) are NOT run here; they
belong to committing, not to packaging. Run them before you cut a release.

Honours SOURCE_DATE_EPOCH. When unset, the HEAD commit time is used, falling
back to the newest mtime in the staged tree.
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
# Version and architecture. The version's single source of truth is the project() line, the same
# place the binaries get it from. The architecture is in the name because a binary archive that
# does not say what it runs on is an archive somebody will download the wrong copy of.
# ---------------------------------------------------------------------------------------------
version=$(sed -n 's/^project(cpu-power VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || die "could not read the version out of CMakeLists.txt"
arch=$(uname -m)
name=cpu-power-$version-$arch

printf '\n%s %s\n\n' "Building release tarball for" "$name"

tar --version 2>/dev/null | head -1 | grep -q 'GNU tar' ||
    die "GNU tar is required to create the archive (the archive it writes is plain ustar and
reads anywhere; it is the --sort and --numeric-owner flags that are GNU)"
command -v objdump >/dev/null 2>&1 ||
    die "objdump (binutils) is required: the portability gates measure the shipped binaries"

stage=$(mktemp -d "${TMPDIR:-/tmp}/cpu-power-release.XXXXXX")
trap 'rm -rf "$stage"' EXIT INT TERM

# ---------------------------------------------------------------------------------------------
# Build. Out of tree and from scratch, never the working build/ directory: the release must not
# be able to inherit a stale object, a leftover cache variable or a debug flag from whatever was
# last configured by hand.
# ---------------------------------------------------------------------------------------------
printf 'Building\n'
build=$stage/build
if ! cmake -S "$root" -B "$build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" >"$stage/cmake.log" 2>&1 ||
   ! cmake --build "$build" -j"$(nproc 2>/dev/null || echo 2)" >>"$stage/cmake.log" 2>&1; then
    tail -40 "$stage/cmake.log" >&2
    die "the release build failed (full log: $stage/cmake.log)"
fi
say "clean Release build, prefix $prefix"

# ---------------------------------------------------------------------------------------------
# Stage. DESTDIR, not --prefix: the polkit actiondir is an absolute path that does NOT derive
# from CMAKE_INSTALL_PREFIX -- polkitd reads two fixed directories and that search path is not
# ours to relocate -- so --prefix alone would send the action to the real
# /usr/share/polkit-1/actions. DESTDIR prepends to the absolute paths too, which is what makes
# this runnable unprivileged and what makes the staged tree the exact tree the recipient gets.
# ---------------------------------------------------------------------------------------------
printf 'Staging\n'
mkdir "$stage/$name"
if ! DESTDIR="$stage/$name" cmake --install "$build" >"$stage/install.log" 2>&1; then
    tail -30 "$stage/install.log" >&2
    die "the install failed (full log: $stage/install.log)"
fi

# What rides at the TOP of the archive rather than being installed: the three documents, and the
# installer itself. install.sh sits next to the usr/ tree it copies and finds it relative to its
# own location, so the recipient unpacks the archive anywhere, cds in, and runs it.
for doc in README.md LICENSE NOTICE; do
    [ -e "$root/$doc" ] || die "missing from the tree: $doc"
    cp -p "$root/$doc" "$stage/$name/$doc"
done
[ -e "$root/packaging/install.sh" ] || die "missing from the tree: packaging/install.sh"
cp -p "$root/packaging/install.sh" "$stage/$name/install.sh"
say "$(find "$stage/$name" -type f | wc -l) files"

# ---------------------------------------------------------------------------------------------
# Gates. Each one is a thing that has to be true of the archive, asserted rather than assumed.
# ---------------------------------------------------------------------------------------------
printf 'Checking the staged tree\n'

# 1. The manifest, EXACTLY. This is the fail-closed half of the old allowlist, and it is stricter
#    than one: it fails on a file that appears as well as on a file that goes missing. An install
#    rule added to CMakeLists.txt does not reach a release until it is named here, and one
#    deleted by accident does not slip out unnoticed either. The paths are literal because the
#    prefix is pinned; that is the whole reason it is pinned.
manifest='
install.sh
README.md
LICENSE
NOTICE
usr/bin/cpu-power
usr/libexec/cpu-power/cpu-power-helper
usr/share/polkit-1/actions/io.github.rations.cpu-power.policy
usr/share/applications/cpu-power.desktop
usr/share/cpu-power/fonts/Michroma-OFL.txt
usr/share/cpu-power/fonts/Michroma-Regular.ttf
usr/share/cpu-power/fonts/Roboto-LICENSE.txt
usr/share/cpu-power/fonts/Roboto-Regular.ttf
usr/share/icons/hicolor/16x16/apps/cpu-power.png
usr/share/icons/hicolor/22x22/apps/cpu-power.png
usr/share/icons/hicolor/24x24/apps/cpu-power.png
usr/share/icons/hicolor/32x32/apps/cpu-power.png
usr/share/icons/hicolor/48x48/apps/cpu-power.png
usr/share/icons/hicolor/64x64/apps/cpu-power.png
usr/share/icons/hicolor/128x128/apps/cpu-power.png
usr/share/icons/hicolor/256x256/apps/cpu-power.png
usr/share/icons/hicolor/512x512/apps/cpu-power.png
usr/share/man/man1/cpu-power.1
'
printf '%s\n' "$manifest" | grep -v '^$' | sort > "$stage/expected"
( cd "$stage/$name" && find . -type f | sed 's|^\./||' | sort ) > "$stage/actual"
if ! diff -u "$stage/expected" "$stage/actual" > "$stage/manifest.diff"; then
    printf '%s\n' "the installed tree is not the manifest (- expected, + present):" >&2
    sed -n '3,$p' "$stage/manifest.diff" >&2
    die "update the manifest in this script, or the install rules in CMakeLists.txt"
fi
say "the installed tree is exactly the manifest ($(wc -l < "$stage/actual") files)"

# 2. Nothing local-only escaped -- in the TEXT files and in the BINARIES alike. Excluding a
#    development document is easy; shipping a binary with the build machine's home directory
#    baked into a path is the mistake that actually happens, and a text-only grep cannot see it.
#    An absolute /home path is a private detail of one machine and is never right on the
#    recipient's; a dangling citation of a document they were not given is worse than no citation.
leaks=$(grep -rl --binary-files=text \
             -e 'RULES\.md' -e 'CLAUDE\.md' -e 'PLAN\.md' -e 'task\.md' -e 'SECURITY\.md' \
             -e '/home/' -e '/root/' "$stage/$name" 2>/dev/null || true)
if [ -n "$leaks" ]; then
    die "shipped files name a local-only document or an absolute home path:
$(printf '%s\n' "$leaks" | sed "s|^$stage/$name/|  |")"
fi
say "no file names a local-only document or a development-machine path"

# 3. No mode bits that have no business here. The helper reaches root through polkit at run time
#    and never through a mode bit, and this is the last place that can still be asserted before
#    the bytes leave the building.
if find "$stage/$name" \( -perm -4000 -o -perm -2000 \) -print | grep -q .; then
    die "a setuid or setgid bit is set in the staged tree"
fi
say "no setuid or setgid bits"

# 4. No symlinks. A tarball with no symlinks cannot have a link-escape problem at unpack time,
#    and this one is unpacked at / by root, which is the worst place to find out otherwise.
if find "$stage/$name" -type l -print | grep -q .; then
    die "the staged tree contains a symlink; ustar would ship it and unpack is the recipient's"
fi
say "no symlinks"

# 5. Every path fits ustar. The format stores a 100-byte name plus a 155-byte prefix, split at a
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

# ---------------------------------------------------------------------------------------------
# 6. What the binaries need from the recipient's machine. This is the question a source tarball
#    never had to answer and a binary one cannot avoid: the loader resolves these on a machine
#    nobody here controls, and it fails with a message about a symbol version rather than one
#    about a distribution being too old. So measure it and print it, so the publisher can put the
#    number next to the download.
# ---------------------------------------------------------------------------------------------
printf 'Measuring what the binaries need\n'

gui=$stage/$name/usr/bin/cpu-power
helper=$stage/$name/usr/libexec/cpu-power/cpu-power-helper

needed() { objdump -p "$1" | awk '/NEEDED/ { print $2 }'; }
maxglibc() {
    objdump -p "$1" |
        sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' |
        sort -t. -k1,1n -k2,2n -k3,3n | tail -1
}

# An allowlist, for the same reason the manifest is one: a new NEEDED entry is a new thing the
# recipient has to already have, and it should have to be decided rather than discovered.
allowed='libcairo.so.2 libfreetype.so.6 libfontconfig.so.1 libX11.so.6
         libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6
         ld-linux-x86-64.so.2 ld-linux-aarch64.so.1 ld-linux-armhf.so.3'
for lib in $(needed "$gui"); do
    case " $(echo $allowed) " in
        *" $lib "*) ;;
        *) die "the GUI links a library that is not on the allowlist: $lib
Either it belongs in the release's dependency list -- add it above and to the README -- or it
was linked by accident." ;;
    esac
done
say "GUI links: $(needed "$gui" | tr '\n' ' ')"

# The helper links libc AND NOTHING ELSE. That is a claim this project makes in its own header
# comments, it is a property of the one program that runs as root, and it is cheap to assert.
helper_needs=$(needed "$helper" | grep -v '^ld-linux' | tr '\n' ' ' | sed 's/ *$//')
[ "$helper_needs" = "libc.so.6" ] ||
    die "the root helper links more than libc: $helper_needs
It is plain C with no libraries by design; anything else is a new attack surface in the only
process here that runs as root."
say "helper links libc and nothing else"

glibc=$(printf '%s\n%s\n' "$(maxglibc "$gui")" "$(maxglibc "$helper")" |
        sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
[ -n "$glibc" ] || die "could not read the glibc symbol versions out of the binaries"
say "needs glibc >= $glibc on $arch (publish this next to the download)"

# ---------------------------------------------------------------------------------------------
# 7. Normalise modes and timestamps, so the archive is a function of the files in it.
# ---------------------------------------------------------------------------------------------
find "$stage/$name" -type d -exec chmod 755 {} +
find "$stage/$name" -type f -exec chmod 644 {} +
chmod 755 "$gui" "$helper" "$stage/$name/install.sh"

if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    epoch=$SOURCE_DATE_EPOCH
else
    # The commit, not the clock: the binaries have no meaningful mtime of their own -- they were
    # made a minute ago -- so the sources they were made from are the only honest timestamp.
    epoch=$(cd "$root" && git log -1 --format=%ct 2>/dev/null || true)
    if [ -z "$epoch" ]; then
        epoch=$(find "$stage/$name" -type f -printf '%T@\n' 2>/dev/null |
                awk -F. '{ if ($1 > n) n = $1 } END { print n+0 }')
    fi
    [ "${epoch:-0}" -gt 0 ] || epoch=$(date +%s)
fi
find "$stage/$name" -exec touch -d "@$epoch" {} +
say "mtime pinned to $(date -u -d "@$epoch" '+%Y-%m-%d %H:%M:%S UTC')"

# ---------------------------------------------------------------------------------------------
# Archive.
# ---------------------------------------------------------------------------------------------
printf 'Archiving\n'
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

# One archive, not two. gzip is the one every machine can already open without installing
# anything, which is the whole point of shipping a tarball rather than a package; a second
# compression of the same bytes is another file to publish, checksum and keep straight for a few
# hundred kilobytes.
( cd "$outdir" && sha256sum "$name.tar.gz" > "$name.sha256" )
say "$outdir/$name.sha256"

install_cmd="tar -xf $name.tar.gz && cd $name && sudo ./install.sh"

# ---------------------------------------------------------------------------------------------
# Verify. Unpack what was actually written, somewhere else, and check the bytes that will land on
# the recipient's machine. The security boundary is asserted here against the SHIPPED files and
# not against the build tree, because a staged tree and a written archive are two different
# things until one of them has been read back.
# ---------------------------------------------------------------------------------------------
if [ "$verify" -eq 0 ]; then
    printf '\nSkipped verification (--no-verify).\n\n'
    printf 'Install with:\n  %s\n\n' "$install_cmd"
    exit 0
fi

printf 'Verifying the written archive\n'
check=$stage/check
mkdir "$check"
tar -C "$check" -xf "$outdir/$name.tar.gz"
[ -d "$check/$name" ] || die "the archive does not unpack into a single $name/ directory"
[ -d "$check/$name/usr" ] || die "the archive has no usr/ tree to unpack at /"

if find "$check/$name" \( -perm -4000 -o -perm -2000 \) -print | grep -q .; then
    die "a setuid or setgid bit survived into the written archive"
fi
say "unpacks into $name/usr, no setuid or setgid bit"

# The polkit action is the security boundary, and the way it fails is SILENT: if exec.path stops
# matching the path the GUI execs, byte for byte, pkexec falls back to
# org.freedesktop.policykit.exec and the desktop's agent asks for an administrator password.
# Releasing that is worse than releasing a build error, because nothing crashes. So the release
# asserts the SHIPPED action against the SHIPPED binary rather than trusting the build.
policy=$(find "$check/$name" -name '*.policy' -print | head -1)
[ -n "$policy" ] || die "no .policy action is in the archive"

shipped_helper=$(find "$check/$name" -name 'cpu-power-helper' -type f -print | head -1)
[ -n "$shipped_helper" ] || die "no helper binary is in the archive"
# The path as it will exist once the archive is unpacked at /, which is the path polkit compares.
helper_abs=${shipped_helper#"$check/$name"}

grep -q "<annotate key=\"org.freedesktop.policykit.exec.path\">$helper_abs</annotate>" "$policy" ||
    die "the action's exec.path does not match where the archive puts the helper ($helper_abs).
polkit compares these byte for byte with no canonicalisation; a mismatch is silent and costs
the user a password prompt."
say "action exec.path matches where the archive puts the helper, byte for byte"

# And the third copy of that same path: the one compiled into the GUI. The action and the binary
# agreeing is only two thirds of the pin -- the program has to exec that path as well, and in a
# binary release the compiled-in constant is a shipped byte like any other, so read it back.
if ! grep -qa "$helper_abs" "$check/$name/usr/bin/cpu-power"; then
    die "the shipped GUI does not carry the helper path $helper_abs.
The path is a compile-time constant and is never derived at run time; if it is not in the
binary, this archive was built for a different prefix."
fi
say "the shipped GUI carries that same path as its compiled-in constant"

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

# The binaries run at all. A tarball whose binaries have not been executed is a tarball nobody
# has tested -- and --help is the one path through the GUI that needs neither a display nor a
# privileged helper, so it is the one the release machine can actually take.
if ! "$check/$name/usr/bin/cpu-power" --help >/dev/null 2>"$stage/help.log"; then
    cat "$stage/help.log" >&2
    die "the shipped GUI does not run"
fi
say "the shipped GUI runs (--help)"

# The helper rejects an argument it does not recognise, and rejects it BEFORE it gets as far as
# caring who is running it -- exit 64 rather than the 77 an unprivileged run would give. The
# action pins argv[1] and nothing after it, so this is the half of that pin the helper owns.
"$check/$name/usr/libexec/cpu-power/cpu-power-helper" --not-an-argument \
    >/dev/null 2>&1 && rc=0 || rc=$?
[ "$rc" -eq 64 ] ||
    die "the shipped helper did not reject an unrecognised argument (exit $rc, expected 64)"
say "the shipped helper rejects unrecognised arguments"

# ---------------------------------------------------------------------------------------------
# The installer, RUN rather than read. It is the first thing the recipient executes and the only
# shipped file whose bugs land on their filesystem, so shipping it untested is not an option --
# and --root makes testing it a matter of staging into a scratch directory as an ordinary user.
# ---------------------------------------------------------------------------------------------
fake=$stage/fakeroot
mkdir "$fake"
if ! ( cd "$check/$name" && sh ./install.sh --root "$fake" >"$stage/installsh.log" 2>&1 ); then
    tail -30 "$stage/installsh.log" >&2
    die "the shipped install.sh failed (full log: $stage/installsh.log)"
fi

# It put down the same usr/ tree the archive carries -- no more, no less.
( cd "$check/$name" && find usr -type f | sort ) > "$stage/want"
( cd "$fake" && find usr -type f | sort ) > "$stage/got"
if ! diff -u "$stage/want" "$stage/got" > "$stage/installsh.diff"; then
    sed -n '3,$p' "$stage/installsh.diff" >&2
    die "install.sh did not lay down the archive's usr/ tree (- archive, + installed)"
fi

# The two binaries are executable and everything else is not, whatever the umask of whoever
# unpacked the archive happened to be. install.sh sets modes rather than preserving them for
# exactly this reason, so assert that it actually does.
[ -x "$fake/usr/bin/cpu-power" ] ||
    die "install.sh left the GUI non-executable"
[ -x "$fake/usr/libexec/cpu-power/cpu-power-helper" ] ||
    die "install.sh left the helper non-executable"
if find "$fake" \( -perm -4000 -o -perm -2000 \) -print | grep -q .; then
    die "install.sh set a setuid or setgid bit"
fi
say "install.sh installs the archive's usr/ tree, executable where it should be, setuid nowhere"

# And it takes it all back. An installer whose uninstall leaves litter is one people do not trust
# enough to run in the first place.
if ! ( cd "$check/$name" && sh ./install.sh --uninstall --root "$fake" \
        >"$stage/uninstall.log" 2>&1 ); then
    tail -30 "$stage/uninstall.log" >&2
    die "the shipped install.sh --uninstall failed (full log: $stage/uninstall.log)"
fi
left=$(find "$fake" -type f -print)
[ -z "$left" ] || die "install.sh --uninstall left files behind:
$(printf '%s\n' "$left" | sed "s|^$fake|  |")"
for owned in usr/share/cpu-power usr/libexec/cpu-power; do
    [ -d "$fake/$owned" ] && die "install.sh --uninstall left its own directory behind: $owned"
done
say "install.sh --uninstall removes every file and its own directories"

# The installer refuses an archive whose action does not name the helper it is about to install.
# That refusal is the reason this project ships an installer rather than a tar command, so it is
# worth proving it fires rather than assuming it does.
tamper=$stage/tamper
cp -Rp "$check/$name" "$tamper"
tpolicy=$(find "$tamper/usr/share/polkit-1/actions" -name '*.policy' -print | head -1)
sed 's|/usr/libexec/cpu-power/cpu-power-helper|/usr/local/libexec/cpu-power/cpu-power-helper|' \
    "$tpolicy" > "$tpolicy.new" && mv "$tpolicy.new" "$tpolicy"
if ( cd "$tamper" && sh ./install.sh --root "$stage/nope" >/dev/null 2>&1 ); then
    die "install.sh installed an archive whose action names the WRONG helper path.
That check is the whole reason this ships an installer; without it the mismatch is silent and
costs the user a password prompt on every use."
fi
say "install.sh refuses an archive whose action does not match its helper"

printf '\n%s\n\n' "Release $version ($arch) is built and verified in $outdir."
printf 'Needs glibc >= %s and: %s\n\n' "$glibc" "$(needed "$gui" | grep -v '^ld-linux' | tr '\n' ' ')"
printf 'Install with:\n  %s\n\n' "$install_cmd"
