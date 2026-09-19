#!/bin/sh
# install.sh — install CPU-Power from the release tarball.
#
#     sudo ./install.sh              install
#     sudo ./install.sh --uninstall  remove exactly what was installed
#
# This script ships INSIDE the release tarball, at the top, next to the usr/ tree it installs.
# Unpack the archive anywhere, cd into it, and run it.
#
# WHY IT COPIES TO /usr AND OFFERS NO PREFIX. The helper's absolute path is a compile-time
# constant in the GUI, and the same string is written into the polkit action's exec.path
# annotation. polkit compares those BYTE FOR BYTE with no canonicalisation, and polkitd reads
# action files from exactly two fixed directories that are not relative to anything this project
# chose. So these binaries are true at one set of paths and nowhere else. A --prefix option here
# would not relocate the install, it would only break it, and break it SILENTLY: pkexec falls
# back to org.freedesktop.policykit.exec and the tool starts asking for an administrator password
# it promises never to ask for. Nothing crashes, so nobody finds out.
#
# That is also why this script CHECKS the action against the payload before copying anything,
# rather than trusting the archive it came in. It is the one machine-side opportunity to catch a
# mismatch while it is still a refusal instead of a password prompt.
#
# --root exists for packagers and for the release script's own test, and it is the DESTDIR idea:
# it stages the same files under a directory of your choosing. A staged tree is not a working
# install, for the reason above. It is not what you want if you are just installing the tool.
#
# PORTABILITY IS THE POINT HERE, unlike the maintainer scripts in this project. This runs on
# machines nobody here controls, so it is POSIX sh and POSIX utilities only: no bash, no GNU
# find, no install(1), no readlink -f. It assumes nothing about the init system, and it needs no
# network, no package manager and no build tools.

set -eu

# 022, set here rather than inherited: every directory this script creates gets its mode from the
# umask, and a caller with a restrictive one would otherwise produce an install the users of the
# machine cannot read. It also means the directories need no chmod afterwards, which keeps this
# script from touching the mode of shared directories like /usr/share that it merely passes
# through and does not own.
umask 022

self=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
payload=$self/usr

dest=
mode=install
dry=0

usage() {
    cat <<USAGE
usage: install.sh [--uninstall] [--dry-run] [--root DIR]

  (no options)   install into /. Needs root: run it with sudo.
  --uninstall    remove exactly the files this archive installs, and nothing else.
  --dry-run      print what would happen and change nothing. Needs no root.
  --root DIR     stage into DIR instead of / (the DESTDIR idea, for packagers).
                 A staged tree is NOT a working install: the paths in these
                 binaries are absolute and only true at /.

There is no --prefix. The helper's path is compiled into the GUI and written
into the polkit action, which polkit matches byte for byte, so this software
is only correct at the paths it was built for.
USAGE
}

while [ $# -gt 0 ]; do
    case $1 in
        --uninstall|-u) mode=uninstall; shift ;;
        --dry-run|-n) dry=1; shift ;;
        --root) dest=$2; shift 2 ;;
        --root=*) dest=${1#--root=}; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'install.sh: unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
say() { printf '  %s\n' "$*"; }
run() { if [ "$dry" -eq 1 ]; then printf '  would: %s\n' "$*"; else "$@"; fi; }

# The paths this archive is built for. Absolute, and used both to check the payload and to report
# what happened; $dest is prepended only when staging.
BIN=/usr/bin/cpu-power
HELPER=/usr/libexec/cpu-power/cpu-power-helper

# ---------------------------------------------------------------------------------------------
# Pre-flight. Everything that can be known before a single byte is copied is checked before a
# single byte is copied, because a half-done install of a privileged tool is the worst outcome
# available here.
# ---------------------------------------------------------------------------------------------
[ -d "$payload" ] ||
    die "no usr/ directory next to this script.
Run it from inside the unpacked release archive, not from a copy of the script on its own."

# $self$BIN, not $payload$BIN: BIN is the absolute destination path and already begins with
# /usr, which is what $payload is. Every path in this script is built the same way -- prepend a
# root ($self for the payload, $dest for the destination) to an absolute path that starts at
# /usr -- and that one rule is why the copy loops below need no path arithmetic of their own.
[ -s "$self$BIN" ] || die "the archive is incomplete: $BIN is missing or empty"
[ -s "$self$HELPER" ] || die "the archive is incomplete: $HELPER is missing or empty"

policy=
for p in "$payload"/share/polkit-1/actions/*.policy; do
    [ -f "$p" ] && policy=$p
done
[ -n "$policy" ] || die "the archive is incomplete: no polkit action was found"

# THE CHECK THIS SCRIPT EXISTS TO MAKE, and the reason it is worth having an installer at all
# rather than a tar command in a README. If the action does not name the exact path the helper is
# about to be copied to, installing is worse than not installing: the tool will work, and will
# quietly start asking for an administrator password. Refuse instead.
if [ "$mode" = install ]; then
    grep -q "<annotate key=\"org.freedesktop.policykit.exec.path\">$HELPER</annotate>" "$policy" ||
        die "this archive's polkit action does not name $HELPER.
polkit matches that path byte for byte, so installing this would silently cost you a password
prompt on every use. The archive is damaged or was built for different paths -- do not install
it; get a fresh one."
fi

# Root, unless we are only staging or only talking.
if [ "$dry" -eq 0 ] && [ -z "$dest" ]; then
    uid=$(id -u)
    [ "$uid" = 0 ] || die "this writes to /usr, so it needs root. Run: sudo ./install.sh"
fi

# A live session, warned about rather than broken. Settings outlive the window that set them, so
# the machine may be overridden right now by a helper that is still running. Replacing the files
# under it does not hurt the running process -- it holds open inodes -- but the user is left with
# an old helper in charge and new files on disk, and "switch it off first" is a better answer
# than explaining that.
if [ -d /run/cpu-power ]; then
    live=0
    for s in /run/cpu-power/*; do
        [ -e "$s" ] && live=1
    done
    if [ "$live" = 1 ]; then
        printf 'install.sh: cpu-power appears to be active right now.\n'
        printf '  Run "cpu-power --off" first, so the machine is put back by the version that\n'
        printf '  changed it. Continuing anyway is safe, but the running helper stays in charge\n'
        printf '  until it is switched off.\n\n'
    fi
fi

# ---------------------------------------------------------------------------------------------
# Uninstall. Removes exactly the paths this archive provides, derived from the payload rather
# than from a list written down somewhere that can drift out of date. Nothing else is touched:
# no shared directory is removed, and a path that is not there is not an error.
# ---------------------------------------------------------------------------------------------
if [ "$mode" = uninstall ]; then
    printf '\nRemoving CPU-Power from %s\n' "${dest:-/}"

    # No temp file. An earlier draft of this wrote the file list to a predictable name in /tmp,
    # which is a symlink target a local user can plant in advance -- and this script runs as
    # root. The pipeline needs no scratch space, so it gets none.
    find "$payload" -type f | sort | while read -r src; do
        target=$dest${src#"$self"}
        if [ -e "$target" ]; then
            run rm -f "$target"
        fi
    done

    # Only this project's OWN directories, and only if empty. /usr/bin and the hicolor icon tree
    # belong to the whole system; rmdir on them would be wrong even where it would succeed.
    # rmdir on a non-empty directory fails, which is exactly the guard wanted here.
    for d in /usr/share/cpu-power/fonts /usr/share/cpu-power /usr/libexec/cpu-power; do
        if [ -d "$dest$d" ]; then
            run rmdir "$dest$d" 2>/dev/null || true
        fi
    done

    # The runtime directory is the helper's, not the install's, and it does not survive a reboot.
    # Remove it only if nothing is left in it.
    if [ -z "$dest" ] && [ -d /run/cpu-power ]; then
        run rmdir /run/cpu-power 2>/dev/null || true
    fi

    printf '\nRemoved. Your settings were never written to disk, so there is nothing else to\n'
    printf 'clean up: the machine is as the kernel left it once the helper is off.\n\n'
    exit 0
fi

# ---------------------------------------------------------------------------------------------
# Install. Modes are set explicitly rather than preserved from the unpack: a tarball extracted by
# a file manager, or by a tar without -p, arrives with the umask applied, and an executable that
# is not executable is a confusing way to find that out.
# ---------------------------------------------------------------------------------------------
printf '\nInstalling CPU-Power into %s\n' "${dest:-/}"

find "$payload" -type d | sort | while read -r d; do
    run mkdir -p "$dest${d#"$self"}"
done

find "$payload" -type f | sort | while read -r src; do
    rel=${src#"$self"}
    run cp -f "$src" "$dest$rel"
    case $rel in
        "$BIN"|"$HELPER") run chmod 755 "$dest$rel" ;;
        *) run chmod 644 "$dest$rel" ;;
    esac
done

# root:root, and NO SETUID BIT ANYWHERE. The helper reaches root through polkit at run time and
# never through a mode bit; that is the difference between this tool and a privilege-escalation
# vector. chown is skipped when staging, because a packager's tree is not owned by root yet and
# does not need to be.
if [ -z "$dest" ] && [ "$dry" -eq 0 ]; then
    find "$payload" -type f | while read -r src; do
        chown 0:0 "${src#"$self"}" 2>/dev/null || true
    done
    find "$payload" -type d | while read -r d; do
        chown 0:0 "${d#"$self"}" 2>/dev/null || true
    done
fi

count=$(find "$payload" -type f | wc -l)
say "$(printf '%s' "$count" | tr -d ' ') files installed"

# ---------------------------------------------------------------------------------------------
# Caches. Best effort and never fatal: these make the icon and the menu entry show up now rather
# than at the next login, and a machine without the tools simply waits. polkitd is NOT in this
# list -- it watches its action directories and needs no prodding, and telling the user to
# restart it would be wrong as well as unnecessary.
# ---------------------------------------------------------------------------------------------
if [ -z "$dest" ] && [ "$dry" -eq 0 ]; then
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -qtf /usr/share/icons/hicolor >/dev/null 2>&1 || true
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
    fi
    say "icon and desktop caches refreshed where those tools exist"
fi

printf '\nInstalled.\n\n'
printf '  Run it from your menu, or with:  cpu-power\n'
printf '  Switch it off from a terminal:   cpu-power --off\n'
printf '  Read the manual:                 man cpu-power\n'
printf '  Remove it again:                 sudo ./install.sh --uninstall\n\n'
printf 'Nothing runs as root until you turn the Active toggle on, and turning it off -- or\n'
printf 'killing the program -- puts the machine back exactly as the kernel left it.\n\n'
