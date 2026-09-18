#!/bin/sh
# gate.sh — the LIVE HARDWARE gate for CPU-Power.
#
# This must pass before any commit that touches src/core/ or helper/.
#
# A TOOL THAT CHANGES CPU POWER SETTINGS IS TESTED BY CHANGING CPU POWER SETTINGS. Reading the
# code is not the gate. tools/coretest proves the plan is the right plan against captured sysfs
# trees; scripts/sec-gate.sh proves the privilege boundary holds. Neither of them writes one byte
# to the running kernel, so neither can tell you the tool works. This does:
#
#   * it records every readable cpufreq attribute on this machine BEFORE anything runs,
#   * it applies all five stops with turbo both ways and reads every attribute back, asserting
#     the machine is in the state the plan said it would be -- the plan being the source, so this
#     cannot drift into asserting whatever the code happens to do,
#   * it asserts DAW mode is a HELD FILE DESCRIPTOR and not a write-and-forget,
#   * it SIGKILLs the driver mid-session, which is the case D5 exists for, and
#   * it asserts the machine ends value-for-value identical to where it started.
#
# WHAT THIS DOES TO YOUR MACHINE: it changes the CPU governor, the energy-performance preference
# and turbo, repeatedly, for a few seconds each, and puts them back. If it is interrupted, the
# helper restores on the way out -- that is what step 6 tests.
#
# IT ASKS FOR NO PASSWORD AT ALL from a normal desktop session. The helper is reached through
# polkit, and the installed action grants allow_active -- the person at this machine. If you are
# prompted, something is wrong and step 2b will say so: it means the action was not installed
# where polkitd looks, and pkexec fell back to org.freedesktop.policykit.exec (auth_admin).
#
#   * --fd-check IS THE ONE EXCEPTION, and it asks once. Step 5 can prove DAW mode is a held
#     descriptor two ways. By default it asks the helper, which reports whether its latency fd is
#     open -- good evidence, but the helper reporting on itself. With --fd-check it additionally
#     reads /proc/<helper>/fd and sees the descriptor directly, which is independent evidence but
#     is root-only, because the helper sets PR_SET_DUMPABLE(0) and its /proc entry is therefore
#     root-owned -- the very property sec-gate asserts. The output always says which of the two it
#     did. That prompt is an ordinary interactive `sudo` for a one-off read of /proc, run by you
#     rather than by the tool; the program itself still never invokes sudo at all.
#
# EVERYTHING STILL RUNS IN ONE HELPER SESSION, for a reason that outlived the original one
# ("otherwise sudo asks for a password per assertion", which stopped applying twice over):
# each new session RE-SNAPSHOTS THE MACHINE. The helper captures what it must put back at startup,
# so a gate that started a fresh helper per assertion would have each one capture the state the
# previous one left, and step 7's "identical to where it started" would compare against the wrong
# baseline. One session, one snapshot, one restore.
#
# It exercises the INSTALLED helper, through the real privilege path, because that is what a user
# runs. Build and install first:  cmake --build build && sudo cmake --install build
#
# Usage:  scripts/gate.sh [--fd-check] [build-dir]
#
# There is no --expect flag any more, because there is nothing left to choose between. The
# program has ONE privilege path -- polkit, or already being root -- since the `sudo -n` rung was
# deleted. Step 2b still asserts which route was taken, because a path that is used but never
# asserted is one that can change silently, but it now has only one right answer.

set -eu

build=build
fd_check=0
for arg in "$@"; do
    case $arg in
        --fd-check) fd_check=1 ;;
        -*) printf 'gate: unknown option %s\n' "$arg" >&2; exit 2 ;;
        *) build=$arg ;;
    esac
done
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cpupower-gate.XXXXXX")

sess_pid=""
step_result=""

cleanup() {
    # Killing the session closes the helper's pipe, which is what restores the machine. That is
    # the same path step 6 asserts, so an interrupted gate leaves the machine as it found it.
    if [ -n "${sess_pid:-}" ] && kill -0 "$sess_pid" 2>/dev/null; then
        kill "$sess_pid" 2>/dev/null || true
        wait "$sess_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

pass=0
fail=0
notrun=0

ok()     { pass=$((pass+1));     printf '  ok    %s\n' "$1"; }
bad()    { fail=$((fail+1));     printf '  FAIL  %s\n' "$1"; }
skip()   { notrun=$((notrun+1)); printf '  ....  NOT RUN  %s\n' "$1"; }
note()   { printf '        %s\n' "$1"; }

CLI="$build/cpu-power-cli"
CORETEST="$build/coretest"
UIRENDER="$build/uirender"

for f in "$CLI" "$CORETEST" "$UIRENDER"; do
    [ -x "$f" ] || { printf 'gate: %s not built. Run: cmake --build %s\n' "$f" "$build" >&2; exit 2; }
done

CPU=/sys/devices/system/cpu

# ---------------------------------------------------------------------------------------------
# Record every cpufreq attribute whose value the tool could plausibly disturb.
#
# A hand-picked list of attributes would only ever catch the damage somebody thought of. This
# takes everything readable under the cpufreq interface and the driver's own directory, which is
# what makes it able to catch COLLATERAL damage -- the case that matters here being
# intel_pstate.rst:577-585, where setting no_turbo pulls scaling_max_freq down with it and
# restoring no_turbo is supposed to put it back.
#
# Two kinds of file are excluded, and only two:
#   * anything matching *cur_freq* -- the current clock changes on its own, continuously, and is
#     not a setting,
#   * the stats/ subtree -- counters that only ever go up.
record_state() {
    _out=$1
    : > "$_out"
    for _dir in "$CPU/cpufreq" "$CPU/intel_pstate" "$CPU/amd_pstate" "$CPU/cpuidle"; do
        [ -d "$_dir" ] || continue
        find "$_dir" -maxdepth 2 -type f 2>/dev/null | sort | while read -r _f; do
            case $_f in
                *cur_freq*|*/stats/*) continue ;;
            esac
            _v=$(cat "$_f" 2>/dev/null) || continue
            printf '%s\t%s\n' "$_f" "$_v" >> "$_out"
        done
    done
}

# Every policy directory, canonical (policyN, never cpuN/cpufreq -- those are symlinks).
policies() { ls -d "$CPU"/cpufreq/policy* 2>/dev/null | sort -V; }

# The pid of the HELPER ITSELF. There is no wrapper process any more.
#
# pkexec does not fork: it execve()s and is replaced (polkit-126 pkexec.c:1052, and `nm -D` on the
# installed binary shows no fork/clone/wait symbols at all). So the process cpu-power-cli forked
# IS the helper, and the old sudo_wrapper_pid() had nothing left to find.
#
# Finding it needs care, twice over:
#
#   * `pgrep -f` CANNOT SEE IT. The helper calls prctl(PR_SET_DUMPABLE, 0), which makes
#     /proc/<pid>/cmdline root-owned, so an unprivileged pattern match against the command line
#     matches nothing. That is the hardening property sec-gate asserts, observed from outside.
#     /proc/<pid>/stat is world-readable even for a dumpable-0 process, which is why this walks
#     that instead.
#
#   * `pgrep -x` CANNOT SEE IT EITHER: comm is truncated to 15 characters, so the helper appears
#     as "cpu-power-helpe". Matching on an exact name would silently never match.
#
# So: walk /proc/[0-9]*/stat and return any process whose PARENT is $1. Field 2 is comm in
# parentheses and field 4 is ppid, but comm can itself contain parentheses, so this strips
# through the LAST ')' before splitting -- the standard way to parse that file, and the reason
# this is not a one-line awk.
child_pids_of() {
    _parent=$1
    for _st in /proc/[0-9]*/stat; do
        _rest=${_st#/proc/}
        _pid=${_rest%/stat}
        _line=$(cat "$_st" 2>/dev/null) || continue
        _after=${_line##*') '}
        # _after is now: state ppid pgrp ...
        set -- $_after
        [ "${2:-}" = "$_parent" ] && printf '%s\n' "$_pid"
    done
}

# Wait for the helper to be GONE, which is the only proof the restore has finished. Polling for
# its absence rather than sleeping a guessed interval is the difference between a test and a coin
# toss.
#
# It polls /proc/<hpid> for the pid CAPTURED WHILE THE SESSION WAS ALIVE, rather than re-deriving
# it. Re-deriving would break step 6, where the driver is SIGKILLed and the helper is reparented:
# after that it is nobody's child and a parent walk finds nothing, which would look like a pass.
wait_helper_gone() {
    _n=0
    while [ "$_n" -lt 150 ]; do
        [ -n "${hpid:-}" ] || return 0
        [ -d "/proc/$hpid" ] || return 0
        sleep 0.1
        _n=$((_n+1))
    done
    return 1
}

# ---------------------------------------------------------------------------------------------
# Precondition. Check this BEFORE anything else, because the failure it prevents is not a crash:
# if the action is not registered, pkexec falls back to org.freedesktop.policykit.exec, the
# desktop pops an administrator password dialog, and the gate would then measure the FALLBACK
# while reporting success. A gate that silently tests the wrong privilege path is worse than one
# that refuses to start.
# ---------------------------------------------------------------------------------------------
ACTION_ID=$(sed -n 's/^CPUPOWER_POLKIT_ACTION_ID:[^=]*=//p' "$build/CMakeCache.txt" 2>/dev/null || true)
[ -n "$ACTION_ID" ] || ACTION_ID=io.github.rations.cpu-power

# The helper path AS THE INSTALLED ACTION SPELLS IT. Taken from polkitd rather than from the
# build, because the whole point of step 2c is that the two must be the same string, and reading
# it from the build would make the check compare the build against itself.
HELPER=$(pkaction --action-id "$ACTION_ID" --verbose 2>/dev/null |
             sed -n 's/.*org\.freedesktop\.policykit\.exec\.path -> //p' | head -1)
[ -n "$HELPER" ] || HELPER=$(sed -n 's/^set(CPUPOWER_HELPER_PATH  *"\(.*\)").*$/\1/p' \
                                 "$(dirname "$0")/../CMakeLists.txt" 2>/dev/null |
                                 sed "s|\${CPUPOWER_LIBEXECDIR}|$(sed -n \
                                     's/^CMAKE_INSTALL_PREFIX:[^=]*=//p' \
                                     "$build/CMakeCache.txt")/libexec/cpu-power|")

if [ "$(id -u)" -eq 0 ]; then
    :   # running as root: the ladder short-circuits to Direct and step 2b says so.
else
    if command -v pkaction >/dev/null 2>&1 && ! pkaction --action-id "$ACTION_ID" >/dev/null 2>&1; then
        printf 'gate: NOT RUN -- the polkit action %s is not installed.\n' "$ACTION_ID" >&2
        printf '      polkitd reads only /usr/share/polkit-1/actions and /etc/polkit-1/actions.\n' >&2
        printf '      Install it, then re-run:  sudo cmake --install %s\n' "$build" >&2
        printf '\n      Refusing to continue: this is the only privilege path there is, so\n' >&2
        printf '      without the action there is nothing for the gate to measure. The tool\n' >&2
        printf '      itself fails the same way, by design, rather than asking for a password.\n' >&2
        exit 2
    fi
fi

printf '\n== 1. the offline gates, first ==\n'
if "$CORETEST" > "$tmp/coretest.out" 2>&1; then
    ok "$(tail -1 "$tmp/coretest.out")"
else
    bad "coretest failed"
    tail -20 "$tmp/coretest.out"
fi

if "$UIRENDER" > "$tmp/uirender.out" 2>&1; then
    ok "$(tail -1 "$tmp/uirender.out")"
else
    bad "uirender found a string that does not fit its slot"
    tail -20 "$tmp/uirender.out"
fi

printf '\n== 2. this machine, and the ONE session everything below runs in ==\n'

# ONE session for every hardware assertion in this script. See the header: it is forced by the
# restore contract, not chosen for tidiness -- each new helper re-snapshots the machine, so a
# second one would capture what the first left behind and step 7 would compare against the wrong
# baseline. Even reporting what this machine is goes through the same session.
mkfifo "$tmp/in"
"$CLI" --session < "$tmp/in" > "$tmp/session.out" 2>&1 &
sess_pid=$!
exec 3> "$tmp/in"

_n=0
while [ "$_n" -lt 600 ]; do
    grep -q '^READY' "$tmp/session.out" 2>/dev/null && break
    kill -0 "$sess_pid" 2>/dev/null || break
    sleep 0.1
    _n=$((_n+1))
done

if ! grep -q '^READY' "$tmp/session.out" 2>/dev/null; then
    printf '  FAIL  the privileged helper did not start\n'
    sed 's/^/        /' "$tmp/session.out"
    note "this gate drives the INSTALLED helper: sudo cmake --install $build"
    exit 1
fi

ready=$(grep '^READY' "$tmp/session.out" | head -1)
ok "$ready"

driver=$(printf '%s' "$ready" | sed -n 's/.*driver=\([^ ]*\).*/\1/p')
knob=$(printf '%s' "$ready" | sed -n 's/.*turbo=\([^ ]*\).*/\1/p')
npol=$(policies | wc -l)
note "$npol policy directories, turbo knob $knob"

# THE READY LINE ABOVE IS THE FD ASSERTION. It is worth naming, because a regression here would
# otherwise read as some vague startup failure rather than as what it is. That line travelled out
# of the helper on file descriptor 1 and arrived on our socketpair, through pkexec. pkexec leaves
# descriptors 0, 1 and 2 alone and sets FD_CLOEXEC only from 3 up (pkexec.c:967) -- if that ever
# changed, the protocol channel would not survive the exec and no READY could arrive at all.
ok "the protocol channel survived the exec (fds 0 and 1 passed through untouched)"

printf '\n== 2b. which rung of the privilege ladder carried us ==\n'
# Asserted, not assumed. A privilege path that is used but never checked is one that can change
# silently -- and the specific silent change to fear is falling back to a password prompt, which
# still WORKS and so looks like success.
via=$(sed -n 's/^via=//p' "$tmp/session.out" | head -1)
if [ -z "$via" ]; then
    bad "could not determine which privilege path was used"
elif [ "$via" = "direct" ] && [ "$(id -u)" -eq 0 ]; then
    skip "via=direct -- running as root, so no escalation path was exercised at all"
    note "run this gate as an ordinary user to test what a user actually runs"
elif [ "$via" = "polkit" ]; then
    ok "via=polkit -- the installed action authorised this session with no prompt"
else
    bad "via=$via, which is not a privilege path this program has"
    note "polkit and already-being-root are the only two; anything else means the ladder grew one"
fi

# ---------------------------------------------------------------------------------------------
# 2c. The action pins ONE SPELLING of the helper path, and the fallback really does want a
#     password.
#
# This is the failure mode the design fears most, because nothing about it looks like a failure.
# polkit compares the annotation against the program path with strcmp -- no realpath, no stat, no
# inode compare -- so a path naming the very same file in a different way stops matching, the
# action stops applying, and pkexec falls back to org.freedesktop.policykit.exec, whose default
# is auth_admin. The tool goes on working and merely starts asking for a password it was never
# meant to ask for.
#
# WHAT THIS DOES NOT DO IS EXEC THE DRIFTED PATH, and the reason is worth writing down because it
# corrects something that was believed here. --disable-internal-agent stops pkexec registering
# ITS OWN text agent; it does nothing about an agent the desktop session has already registered
# (lxpolkit, polkit-gnome, polkit-kde and the rest). With one of those running -- and on a
# desktop there usually is -- the auth_admin fallback is not a quiet failure on stderr at all: it
# puts a password dialog on the user's screen. A gate that did that would be a gate that
# interrupts whoever ran it, which is precisely the behaviour this project exists to remove.
#
# So the consequence is asserted with pkcheck instead, which answers the same question and, with
# no -u, is defined never to interact.
# ---------------------------------------------------------------------------------------------
if [ "$(id -u)" -ne 0 ] && command -v pkcheck >/dev/null 2>&1; then
    printf '\n== 2c. the action pins one spelling, and the fallback wants a password ==\n'
    drift="$(dirname "$HELPER")/./$(basename "$HELPER")"

    if [ "$drift" != "$HELPER" ] &&
       [ "$(readlink -f "$drift" 2>/dev/null)" = "$(readlink -f "$HELPER" 2>/dev/null)" ]; then
        ok "two different strings, one file: drift is about spelling, not about the binary"
    else
        bad "could not construct a second spelling of $HELPER -- this check proves nothing"
    fi

    # Our action, for this very process. Exit 0 means authorised with no interaction at all.
    if pkcheck --action-id "$ACTION_ID" --process $$ >/dev/null 2>&1; then
        ok "this session is authorised for $ACTION_ID with no interaction"
    else
        bad "this session is NOT authorised for $ACTION_ID"
    fi

    # The action pkexec falls back to when exec.path does not match. From the SAME session that
    # was just silently authorised above, this one must demand authentication -- which is both
    # the proof that allow_active=yes is doing the work, and the measure of what a drift costs.
    # `|| st=$?` rather than a bare call: this script runs under `set -e`, and a bare command
    # exiting non-zero would take the whole gate with it -- which is exactly what happened the
    # first time, since a non-zero exit is the RESULT this check is looking for.
    st=0
    pkcheck --action-id org.freedesktop.policykit.exec --process $$ >/dev/null 2>&1 || st=$?
    if [ "$st" -eq 0 ]; then
        bad "org.freedesktop.policykit.exec is already authorised here, so a drift would be INVISIBLE"
    elif [ "$st" -eq 2 ]; then
        ok "the fallback action requires authentication from this same session (exit 2)"
    else
        bad "the fallback action answered with exit $st, neither authorised nor challenged"
    fi

    # ---- and now the part that actually matters: IS the rung gated? ----
    #
    # Everything above describes polkit. This asserts our own code: that it asks pkcheck first and
    # DECLINES the pkexec rung when the answer is not an unqualified yes. Without that, a missing
    # or unparseable action does not make pkexec fail -- it makes it fall back to
    # org.freedesktop.policykit.exec (auth_admin) and the session's agent raises a root password
    # dialog. That happened on this machine, which is why this check exists.
    #
    # It cannot be a `strings` check. That was tried in sec-gate and was BLIND: deleting the gate
    # left the pkcheck path in the binary and the check still passed. So a second CLI is built
    # against an action id that is deliberately NOT registered, and the assertion is behavioural.
    #
    # THIS TEST CANNOT RAISE A DIALOG, EVEN WHEN IT FAILS, and that is by construction: only the
    # action ID is bogus, the helper path is the real one. An ungated build would therefore reach
    # pkexec, match the INSTALLED action by exec.path, and be authorised silently -- reporting
    # via=polkit, which is exactly what this looks for. A test for a no-prompt property must not
    # itself be able to prompt.
    if command -v cmake >/dev/null 2>&1; then
        ungated=$tmp/build-ungated
        if cmake -S "$(dirname "$0")/.." -B "$ungated" -DCMAKE_BUILD_TYPE=Release \
                 -DCPUPOWER_POLKIT_ACTION_ID=com.example.deliberately.unregistered \
                 >/dev/null 2>&1 &&
           cmake --build "$ungated" --target cpu-power-cli >/dev/null 2>&1; then
            uout=$("$ungated/cpu-power-cli" --show 2>&1 </dev/null || true)
            if printf '%s' "$uout" | grep -q '^via=polkit'; then
                bad "an UNREGISTERED action still took the pkexec rung -- the pre-flight is gone"
                note "a real user with a wrong install prefix would get a root password dialog"
            elif printf '%s' "$uout" | grep -q 'would not authorise'; then
                ok "an unregistered action makes the program decline pkexec instead of prompting"
            else
                bad "unregistered-action build neither used polkit nor explained why"
                note "it said: $(printf '%s' "$uout" | head -2 | tr '\n' ' ')"
            fi
        else
            skip "could not build the unregistered-action CLI; the pre-flight is unasserted"
        fi
    else
        skip "no cmake, so the pkcheck pre-flight could not be asserted behaviourally"
    fi
fi

# The helper's own pid, captured ONCE while the session is alive. Everything later that needs it
# uses this value rather than re-deriving it -- see wait_helper_gone() for why that matters.
hpid=$(child_pids_of "$sess_pid" | head -1)
if [ -n "$hpid" ]; then
    hcomm=$(sed -n 's/.*(\(.*\)).*/\1/p' "/proc/$hpid/stat" 2>/dev/null)
    note "helper pid $hpid (comm '$hcomm' -- truncated to 15 chars by the kernel)"
else
    note "warning: could not find the helper's pid under $sess_pid"
fi

seq_n=0

# Send a command and wait for the DONE line that echoes it back. Each command carries a unique
# sequence token, so waiting matches the answer to the command actually sent rather than to an
# identical earlier one -- `daw on` is issued more than once below.
step() {
    seq_n=$((seq_n+1))
    _cmd="$1 #$seq_n"
    printf '%s\n' "$_cmd" >&3
    _n=0
    while [ "$_n" -lt 600 ]; do
        if grep -q "^DONE $_cmd -- " "$tmp/session.out" 2>/dev/null; then
            step_result=$(sed -n "s/^DONE $_cmd -- //p" "$tmp/session.out" | tail -1)
            case $step_result in
                FAILED*) return 1 ;;
                *) return 0 ;;
            esac
        fi
        kill -0 "$sess_pid" 2>/dev/null || { step_result="the session died"; return 1; }
        sleep 0.1
        _n=$((_n+1))
    done
    step_result="timed out"
    return 1
}

end_session() {
    [ -n "${sess_pid:-}" ] || return 0
    printf 'quit\n' >&3 2>/dev/null || true
    exec 3>&-
    wait "$sess_pid" 2>/dev/null || true
    sess_pid=""
    wait_helper_gone || note "warning: a helper process is still running"
    return 0
}

printf '\n== 3. pre-state ==\n'
record_state "$tmp/pre"
ok "$(wc -l < "$tmp/pre") attributes recorded"

printf '\n== 4. five stops x turbo on/off, read back against the plan ==\n'

for stop in powersave balanced balanced-perf performance maximum; do
    for turbo in on off; do
        # The PLAN is the source of what to expect. Asserting against the plan rather than
        # against a table written into this script is what stops the gate agreeing with a
        # regression: change the plan and this expects the new thing, and coretest is what says
        # whether the new thing was meant.
        "$CLI" --plan --stop "$stop" --turbo "$turbo" > "$tmp/plan.out" 2>&1 || true
        want_gov=$(sed -n 's/^governor: *//p' "$tmp/plan.out" | head -1)
        want_epp=$(sed -n 's/^epp: *//p' "$tmp/plan.out" | head -1 | sed 's/ *\[.*//')

        if ! step "stop $stop $turbo"; then
            bad "$stop/turbo=$turbo: $step_result"
            continue
        fi

        bad_attrs=""

        # Governor and EPP, on EVERY policy. Not policy0 -- the kernel warns that mixed hints
        # across CPUs lead to undesirable outcomes (intel_pstate.rst:694-698), so "policy0 is
        # right" is not the claim this tool makes.
        for p in $(policies); do
            got=$(cat "$p/scaling_governor" 2>/dev/null || echo "?")
            [ "$got" = "$want_gov" ] || bad_attrs="$bad_attrs $(basename "$p")/gov=$got"

            if [ "$want_epp" != "(none)" ] && [ -r "$p/energy_performance_preference" ]; then
                got=$(cat "$p/energy_performance_preference" 2>/dev/null || echo "?")
                [ "$got" = "$want_epp" ] || bad_attrs="$bad_attrs $(basename "$p")/epp=$got"
            fi
        done

        # Turbo, read straight out of sysfs rather than asked of the helper. The helper reports
        # INTENT; this reads the raw knob and applies the inversion here, so the two have to
        # agree independently. intel_pstate/no_turbo is the inverted one
        # (intel_pstate.rst:501-503).
        #
        # want_turbo rather than the tempting bare `expect`: a name that generic invites exactly
        # the collision this loop used to have, where a per-iteration scratch value outlived the
        # loop and any later check reading it compared against a turbo bit.
        case $knob in
            intel_pstate/no_turbo)
                raw=$(cat "$CPU/intel_pstate/no_turbo" 2>/dev/null || echo "?")
                [ "$turbo" = on ] && want_turbo=0 || want_turbo=1 ;;
            cpufreq/boost)
                raw=$(cat "$CPU/cpufreq/boost" 2>/dev/null || echo "?")
                [ "$turbo" = on ] && want_turbo=1 || want_turbo=0 ;;
            policy/boost|policy/cpb)
                attr=$(basename "$knob")
                raw=$(cat "$(policies | head -1)/$attr" 2>/dev/null || echo "?")
                [ "$turbo" = on ] && want_turbo=1 || want_turbo=0 ;;
            *)
                raw=""; want_turbo="" ;;
        esac
        if [ -n "$want_turbo" ] && [ "$raw" != "$want_turbo" ]; then
            bad_attrs="$bad_attrs $knob=$raw(wanted $want_turbo)"
        fi

        if [ -z "$bad_attrs" ]; then
            ok "$stop turbo=$turbo -> governor=$want_gov epp=$want_epp, all $npol policies"
        else
            bad "$stop turbo=$turbo:$bad_attrs"
        fi
    done
done

printf '\n== 5. DAW mode is a held file descriptor ==\n'
# pm_qos_interface.rst: the kernel drops the PM-QoS constraint when the last fd closes, so
# holding the fd is the ONLY way to make it stick -- and it is why DAW mode cannot get stuck on.
# Asserting the fd is open is asserting the mechanism, not a side effect of it.
#
# The default assertion asks the helper, whose LATENCY line reports whether its fd is open. That
# is the helper reporting on itself. --fd-check adds the independent read of /proc/<helper>/fd,
# which costs a second password prompt (see the header), so it is opt-in.

# Read the LATENCY line the most recent `snapshot` produced.
latency_held() {
    step "snapshot" >/dev/null 2>&1 || true
    sed -n 's/^LATENCY held=\([01]\).*/\1/p' "$tmp/session.out" | tail -1
}

if [ ! -c /dev/cpu_dma_latency ]; then
    skip "this kernel has no /dev/cpu_dma_latency"
else
    if ! step "daw on"; then
        bad "DAW mode on: $step_result"
    elif [ "$(latency_held)" = "1" ]; then
        ok "DAW mode on: the helper reports its latency fd open (self-reported)"
    else
        bad "DAW mode on but the helper reports no latency fd held"
    fi

    # THE POINT OF DAW MODE, ASSERTED AGAINST THE HARDWARE RATHER THAN THE PROTOCOL.
    #
    # Holding the fd proves the mechanism; it does not prove the VALUE is useful. A limit of 0 --
    # which this tool used to write -- admits only the POLL pseudo-state, and POLL does not halt
    # the core: "there are strict latency constraints preventing any of the available idle states
    # from being used, the CPU will simply execute more or less useless instructions in a loop"
    # (cpuidle.rst:107-109). Every core then spins, which costs a great deal of power and heat and
    # buys nothing over halting in the shallowest real state.
    #
    # So the assertion is made of the kernel's own usage counters: with DAW mode on, some real
    # halting state must still be being ENTERED, and nothing deeper than the requested limit may
    # be. Both directions matter -- the first catches the spin loop, the second catches a limit so
    # loose it blocks nothing.
    idle_usage() {
        for _s in "$CPU/cpu0/cpuidle"/state*; do
            [ -d "$_s" ] || continue
            printf '%s\t%s\t%s\n' "$(cat "$_s/name")" "$(cat "$_s/latency")" \
                "$(cat "$_s/usage")"
        done
    }

    if [ ! -d "$CPU/cpu0/cpuidle" ]; then
        skip "no cpuidle tree: cannot check which states DAW mode leaves available"
    else
        limit=$("$CLI" --plan --daw on 2>/dev/null | sed -n 's/^ *limit \([0-9]*\)us .*/\1/p')
        if [ -z "$limit" ]; then
            bad "the model resolved no DAW mode latency limit on this machine"
        elif [ "$limit" -le 0 ]; then
            bad "DAW mode limit is ${limit}us, which admits only POLL and spins every core"
        else
            ok "DAW mode limit resolved from this machine's idle table: ${limit}us"

            idle_usage > "$tmp/idle.before"
            # Long enough that an idle machine takes many idle transitions, short enough not to
            # stretch the gate. Nothing else runs here, so the cores really are idle.
            sleep 3
            idle_usage > "$tmp/idle.after"

            entered_shallow=0
            entered_deep=""
            while IFS="$(printf '\t')" read -r nm lat before; do
                after=$(awk -F'\t' -v n="$nm" '$1==n {print $3}' "$tmp/idle.after")
                [ -n "$after" ] || continue
                [ "$after" -gt "$before" ] || continue
                if [ "$lat" -gt 0 ] && [ "$lat" -le "$limit" ]; then
                    entered_shallow=1
                elif [ "$lat" -gt "$limit" ]; then
                    entered_deep="$entered_deep $nm(${lat}us)"
                fi
            done < "$tmp/idle.before"

            if [ -n "$entered_deep" ]; then
                bad "DAW mode on, but the kernel still entered:$entered_deep"
            else
                ok "DAW mode on: no idle state deeper than ${limit}us was entered"
            fi

            if [ "$entered_shallow" -eq 1 ]; then
                ok "DAW mode on: the cores still HALT rather than spin in POLL"
            else
                # Not a failure by itself -- a busy machine takes no idle transitions at all, and
                # that is a statement about the machine, not about the tool.
                skip "no halting state was entered in 3s; cpu0 may simply not have been idle"
            fi
        fi
    fi

    if [ "$fd_check" -eq 1 ]; then
        # Independent evidence. The helper's /proc entry is root-owned because it sets
        # PR_SET_DUMPABLE(0), so READING it needs root -- but FINDING it does not, because
        # /proc/<pid>/stat stays world-readable. $hpid was captured at step 2 by walking parents.
        #
        # This is the one place the gate uses sudo, and an interactive prompt is fine here: `ls`
        # on a /proc entry is a one-off read, not the protocol channel, so the use_pty hazard
        # that shaped the rest of this design does not apply.
        have_sudo=0
        if sudo -n true 2>/dev/null; then
            have_sudo=1
        elif [ -t 0 ] && sudo -v 2>/dev/null; then
            have_sudo=1
        fi

        if [ -z "${hpid:-}" ]; then
            skip "the helper's pid was not captured, so its /proc entry could not be read"
        elif [ "$have_sudo" -eq 0 ]; then
            skip "--fd-check needs sudo to read the helper's /proc entry, and none was available"
            note "run from a terminal, or prime it first with: sudo -v"
        else
            # PROVE THE LOOK WORKED before drawing any conclusion from it. "I looked and the fd
            # was not there" and "I could not look" are different answers, and an earlier version
            # of this script reported the second as the first.
            fds=$(sudo -n ls "/proc/$hpid/fd" 2>/dev/null | wc -l)
            if [ "$fds" -eq 0 ]; then
                skip "could not read /proc/$hpid/fd, so nothing is asserted about it"
            else
                held=$(sudo -n ls -l "/proc/$hpid/fd" 2>/dev/null | grep -c 'cpu_dma_latency' || true)
                if [ "$held" -ge 1 ]; then
                    ok "DAW mode on: /dev/cpu_dma_latency held, seen directly in /proc/$hpid/fd ($fds fds)"
                else
                    bad "DAW mode on but /proc/$hpid/fd holds no cpu_dma_latency ($fds fds listed)"
                fi
            fi
        fi
    fi

    # Released while the SAME helper stays alive, so this is the LATENCY OFF path and not just
    # "the process exited".
    if ! step "daw off"; then
        bad "DAW mode off: $step_result"
    elif [ "$(latency_held)" = "0" ]; then
        ok "DAW mode off: the fd is released, same helper still running"
    else
        bad "DAW mode off but the helper still reports a latency fd held"
    fi
fi

printf '\n== 6. SIGKILL mid-session restores (D5) ==\n'
# The one case that cleanup code cannot cover. The GUI being SIGKILLed still closes the pipe, the
# helper sees EOF, and the machine goes back. If this passes, reversibility is structural.
#
# AND EOF IS THE ONLY MECHANISM THAT COULD HAVE DONE IT, which is worth stating because it was
# nearly not true. pkexec sets PR_SET_PDEATHSIG to SIGTERM before it authorises anything
# (pkexec.c:737), and a second death trigger would mean this step could no longer tell which
# mechanism restored the machine. It turns out never to reach the helper: pkexec clears it again
# at its own setregid/setreuid, because the setting is cleared by CREDENTIAL CHANGES and not only
# by a setuid exec. Measured with tools/fdprobe, which reads PR_GET_PDEATHSIG back as 0.
#
# So this step still isolates the EOF path. If a future pkexec keeps the signal, this comment is
# the warning that it has stopped doing so -- and the pure-EOF claim would then have to rest on
# sec-gate.sh section 1b, which uses startUnprivileged() and involves no pkexec at all.
#
# THE REFERENCE IS THE PRE-STATE FROM STEP 3, not the state just before the kill. The helper
# restores what it captured when the SESSION started, which is the pristine machine -- so
# comparing against "whatever step 4 last applied" asserts the wrong thing entirely. An earlier
# version of this script did that and reported a correct restore as a failure.
if ! step "stop maximum on"; then
    bad "could not apply a setting to kill under: $step_result"
    end_session
else
    record_state "$tmp/during-kill"
    changed=$(diff "$tmp/pre" "$tmp/during-kill" | grep -c '^<' || true)
    if [ "${changed:-0}" -eq 0 ]; then
        skip "the machine was already in the Maximum state; the kill would prove nothing"
        end_session
    else
        note "$changed attributes differ from the pre-state while held"
        kill -9 "$sess_pid" 2>/dev/null || true
        wait "$sess_pid" 2>/dev/null || true
        exec 3>&-
        sess_pid=""
        if wait_helper_gone; then
            record_state "$tmp/after-kill"
            if diff -q "$tmp/pre" "$tmp/after-kill" >/dev/null; then
                ok "SIGKILL on the driver: the helper restored all $changed attributes and exited"
            else
                bad "SIGKILL on the driver left the machine changed:"
                diff "$tmp/pre" "$tmp/after-kill" | sed 's/^/        /' | head -20
            fi
        else
            bad "the helper was still running 15s after its driver was SIGKILLed"
        fi
    fi
fi

end_session

printf '\n== 7. the machine is where it started ==\n'
record_state "$tmp/post"
if diff -q "$tmp/pre" "$tmp/post" >/dev/null; then
    ok "all $(wc -l < "$tmp/pre") attributes are value-for-value identical to the pre-state"
else
    bad "the machine did not end where it started:"
    diff "$tmp/pre" "$tmp/post" | sed 's/^/        /' | head -40
fi

printf '\n%d passed, %d failed, %d not run\n' "$pass" "$fail" "$notrun"
if [ "$notrun" -gt 0 ]; then
    printf 'An assertion that did not run has established nothing.\n'
fi
[ "$fail" -eq 0 ] || exit 1
exit 0
