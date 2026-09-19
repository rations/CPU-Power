# CPU-Power

A small window for Linux CPU frequency scaling and power settings.
No GTK, no Qt.

![The CPU-Power window](docs/screenshot.png)

One top-level X11 window painted by hand with Cairo and FreeType: a five-stop slider, a
master toggle, and two checkboxes. It sets the scaling governor, the energy/performance
preference, the frequency limits, turbo, and the system's C-state exit latency limit, and
it puts all of them back when you turn it off.

## Why

A machine that boots with the `powersave` governor is doing the right thing most of the
time and the wrong thing for the hour you spend rendering, compiling or tracking audio.
Fixing that by hand means `su`, remembering which of five different attribute names your
running driver actually has, echoing into `sysfs`, and remembering to undo it afterwards.

This is that, in a window, correctly, and reversibly.

**Off means the machine is exactly as the kernel left it.** That is the whole safety
property, and it is what makes the tool safe to try. Turning the master toggle off
restores and so does the program being killed, because the privileged helper puts
everything back when its input closes unexpectedly, and closing that input is something
`SIGKILL` cannot prevent.

**Closing the window does not.** It is a window you open to change a setting and then
close; the setting is still there afterwards, which is the point of having it. You do not
leave it open while you work. The helper carries on without it and exits when you switch
off, so something runs as root exactly while your machine is overridden, and never
otherwise. `cpu-power --off` reaches it from a terminal if you would rather not open the
window again.

The difference between those two is deliberate and it is the safety property: persisting
has to be **asked for**. A window that crashes cannot ask, so a crash still puts the
machine back. Nothing is written to disk to make this work, what your machine looked like
beforehand is held by that process, which is why switching off still returns you there
rather than to whatever the last session happened to leave behind.

## What it sets

| Control | What it does |
|---|---|
| **The slider** | Five named stops Powersave, Balanced, Balanced-Performance, Performance, Maximum |
| **Active** | The master toggle. Off restores, nothing runs as root until it is on |
| **Turbo** | Whether the CPU may clock above its rated base frequency |
| **DAW mode** | Caps C-state exit latency, for audio work |

The stops are **named rather than numbered** because the kernel's power knobs are
enumerations. `ondemand` is not a number and there is no continuum between it and
`schedutil`, so a 0–100% slider would be a lie on most machines. Each stop is a
*preference-ordered wish*, resolved against what the running driver actually offers and
the window always shows the **resolved** outcome, so a stop that collapses into its
neighbour on your hardware is visible rather than a silent no-op.

**DAW mode is not a speed setting**, and no governor setting substitutes for it. A 64-sample
buffer at 48 kHz is a 1333 µs period. Waking a core from a deep C-state can cost 680 µs of
that and the slider makes it *worse*, because a faster core finishes its block earlier,
idles longer, and lets the kernel pick a deeper state. DAW mode holds `/dev/cpu_dma_latency`
open at **the smallest non-zero exit latency your machine reports** the shallowest state
that still actually halts the core. It does not request zero, which would admit only the
POLL pseudo-state and leave every core spinning for no benefit at all.

Because the kernel drops that constraint when the last descriptor closes, DAW mode is the
one setting that **cannot** outlive the process holding it, no tool can make it, on any
system. That is why the helper stays running after you close the window rather than exiting:
it is the descriptor. It still cannot get stuck on, because switching off closes it and so
does shutting down, and `cpu-power --off` closes it from a terminal.

## Installing

### From the release tarball

Download `cpu-power-<version>-<arch>.tar.gz`, check it against the `.sha256` beside it, unpack
it anywhere, and run the installer inside:

```sh
sha256sum -c cpu-power-0.1.0-x86_64.sha256
tar -xf cpu-power-0.1.0-x86_64.tar.gz
cd cpu-power-0.1.0-x86_64
sudo ./install.sh
```

That is the whole install, and there is nothing to run afterwards — polkitd picks the action up
on its own the next time it is asked. To see exactly what it would do first:

```sh
./install.sh --dry-run      # needs no root, changes nothing
```

And to take it back out again, from the same unpacked directory:

```sh
sudo ./install.sh --uninstall
```

It removes exactly the files it installed and its own two directories, and nothing else. Your
settings were never written to disk, so there is nothing else to clean up.

**There is no `--prefix`, and that is not an oversight.** The helper's absolute path is compiled
into the GUI and written into the polkit action, and polkit compares the two byte for byte; the
two directories polkitd reads actions from are fixed and belong to polkit rather than to this
project. Installing anywhere else gives you a tool that still works and **silently** starts
asking for an administrator password it was never meant to ask for. `install.sh` checks the
action against the payload before it copies anything and refuses rather than let that happen.
It is the same reason the source build below wants `-DCMAKE_INSTALL_PREFIX=/usr`.

Run time dependencies are `cairo`, `freetype`, `fontconfig` and `libX11` which a machine
running X11 already has, plus `libstdc++` and **glibc 2.34 or newer**. The archive is built for
one architecture and says which in its name. If your distribution is older than that glibc,
build from source instead, nothing about the tool needs a new one, the released binaries were
simply linked against it.

At run time you also want `polkit`. The tool works without it, but that is the path that needs
no password.

### From source

Build dependencies: a C and C++17 compiler, CMake ≥ 3.16, and the development packages for
`cairo`, `cairo-ft`, `cairo-xlib`, `freetype2`, `fontconfig` and `x11`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

**Use `-DCMAKE_INSTALL_PREFIX=/usr`, or read the next paragraph.** polkit reads action files
from exactly two directories — `/usr/share/polkit-1/actions` and `/etc/polkit-1/actions` 
and nowhere else. A prefix such as `~/.local` puts the action somewhere polkitd will never
look, and **the failure is silent**: the tool still runs, and merely starts asking for an
administrator password it was never meant to ask for. The build warns at configure time when
the action directory is not one of those two, and you can override it on its own with
`-DCPUPOWER_POLKIT_ACTIONDIR=/usr/share/polkit-1/actions`.

### What lands either way

Two binaries, the GUI and the privileged helper, plus the polkit action that says who may run
the helper, the fonts the window is lettered in, the icon theme entries, a desktop entry and a
manual page:

```
/usr/bin/cpu-power
/usr/libexec/cpu-power/cpu-power-helper
/usr/share/polkit-1/actions/io.github.rations.cpu-power.policy
/usr/share/cpu-power/fonts/
/usr/share/icons/hicolor/*/apps/cpu-power.png
/usr/share/applications/cpu-power.desktop
/usr/share/man/man1/cpu-power.1
```

**It sets no setuid or setgid bit**, and a test asserts that — as does the release script,
against the bytes in the tarball.

## How it reaches root

The window is **never root and never setuid**, and this project **handles no password at
all**. Exactly one program runs as root `cpu-power-helper`, a few hundred lines of plain C
with no libraries and it is reached by the least intrusive route available:

| | Route | What it needs |
|---|---|---|
| 1 | already root | nothing |
| 2 | **`pkexec`** | polkit *already* saying yes to the installed action **no prompt at all** |
| 3 | — | fails, saying what to do about it |

**There is one way in and no fallback.** Not "pkexec is installed" *polkit already says yes*.
Before using `pkexec`, the program asks `pkcheck` whether the action will be authorised with no
interaction at all, and proceeds only on an unqualified yes. That check is what makes "no
password" true rather than merely intended: a missing or unparseable action does **not** make
`pkexec` fail. It makes it fall back to `org.freedesktop.policykit.exec`, whose defaults are
`auth_admin`, and your desktop's polkit agent turns that into a root password dialog. The
pre-flight question cannot itself prompt, because `--allow-user-interaction` is never passed.

So on a machine with no polkit, the tool says so and stops:

```
cannot reach the privileged helper, and this program will never run itself as root.
pkexec is installed, but polkit would not authorise io.github.rations.cpu-power without asking
for a password, so it was not used -- this program never asks for one.
```

### Who is allowed

The installed action's defaults are `allow_any=no`, `allow_inactive=no`, `allow_active=yes`.
That triple says the one thing `sudo` cannot **is this person physically at this machine
right now?** Adjusting your own CPU is a preference, not an administrative act, and every
desktop settings panel already treats it as one.

Measured on the development machine (Devuan 6, elogind, polkit 126) with
`pkcheck --action-id io.github.rations.cpu-power --process $$`

## Distro-agnostic, and how that is tested

Nothing is hardcoded per driver. Governor names come from `scaling_available_governors`, EPP
names from `energy_performance_available_preferences`, frequency bounds from
`cpuinfo_{min,max}_freq`, turbo from whichever of `intel_pstate/no_turbo`, `cpufreq/boost` or
`policyX/boost` exists. **A driver this code has never heard of still works**, and a knob that
does not exist is disabled and labelled rather than silently ignored.

## Security

The GUI is never privileged, the helper takes no path from anyone and never `exec`s anything,
`--sysroot` is not compiled into the privileged binary at all, and the install sets no mode
bits. The honest cost is that `pkexec` is setuid root and has CVE-2021-4034 in its history.
That is stated here rather than glossed, because it is the trade this design makes, what it
buys is that no part of this project ever handles your password.

## Building and testing

```sh
cmake --build build
build/coretest                 # the write plan, against every fixture
build/uirender --out /tmp/ui   # every string measured against its real slot
scripts/sec-gate.sh            # the privilege boundary. Needs no root
scripts/gate.sh                # the live hardware gate: apply, read back, restore
```

`scripts/gate.sh` changes this machine's CPU settings and asserts that every one of them is
value-for-value identical to where it started afterwards. A tool that changes CPU power
settings is tested by changing CPU power settings, reading the code is not the gate.

`scripts/sec-gate.sh` must pass before any change to the helper, the installed action, or the
code that spawns the helper. `scripts/gate.sh` must pass before any change to the model or the
helper.

The icon set and the icon data compiled into the binary are generated from `icon.png` by
`scripts/make-icons.sh`; both are committed, so building needs a compiler and not an image
toolchain.

`scripts/make-release.sh` cuts the release tarball: a clean `/usr` build, the installed surface
and nothing else, plus `packaging/install.sh` and the three documents at the top. It checks the
staged tree against an exact manifest, then unpacks what it wrote and runs the shipped
`install.sh` against a scratch root — installing, uninstalling, and confirming it refuses an
archive whose action does not match its helper. It does **not** run the gates above; those are a
condition on committing, not on packaging, so run them first.

## Licence

MIT — see [LICENSE](LICENSE). Third-party components and their licences are listed in
[NOTICE](NOTICE). polkit is **executed, never linked**: no polkit header enters the include graph
and no polkit library enters the link line, which is a licence requirement as much as a design one
polkit is LGPL and this project is MIT. `scripts/sec-gate.sh` asserts it rather than trusting
it.
