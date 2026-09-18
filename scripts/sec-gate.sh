#!/bin/sh
# sec-gate.sh — the security assertions for CPU-Power.
#
# This MUST pass before any commit that touches helper/, the installed .policy action, or the code
# that spawns the helper. It is not advisory.
#
# A security property that is not asserted by a test is a security property that will stop being
# true silently. Every claim this project makes about its privilege boundary is checked here
# against the BUILT BINARIES, not against the source, because a flag the toolchain quietly dropped
# is a flag that is not protecting anything.
#
# NOTHING HERE NEEDS ROOT, AND NOTHING HERE TOUCHES THE REAL /sys. The helper's test build drives
# the real protocol against a COPY of a captured fixture, and the polkit assertions read the
# generated .policy file and ask polkitd what it parsed -- they never invoke pkexec, so this gate
# never escalates and never prompts. Keep it that way: the live hardware gate is scripts/gate.sh.
#
# Section 1b is where the PURE-EOF restore claim lives, and it has to be here rather than in
# gate.sh: it uses startUnprivileged(), so there is no pkexec anywhere in the picture. That
# matters because pkexec sets PR_SET_PDEATHSIG, and although measurement shows it never reaches
# the helper, an assertion about EOF should not depend on that continuing to be true.
#
# Sections 7 through 8b are about the polkit action. They exist because THE .policy FILE FAILS
# SILENTLY: polkitd ignores a policy it cannot parse, pkexec then falls back to
# org.freedesktop.policykit.exec, and the tool keeps working while merely asking for a password it
# was never meant to ask for. Nothing errors. So validating the XML is not enough on its own --
# section 7b asks polkitd what it ACTUALLY PARSED and compares that against the build variables.
#
# This gate used to carry three assertions about a graphical password prompt, including one that
# needed a human to type into it. That binary no longer exists, so those assertions have been
# replaced rather than removed: the security boundary moved from "a process holding a secret" to
# "a file describing a grant", and this is where the new boundary is checked.
#
# Usage:  scripts/sec-gate.sh [--interactive] [build-dir]     (default: build, non-interactive)
#
# --interactive is accepted and currently changes nothing; it is kept so existing invocations and
# documentation do not break.

set -eu

build=build
interactive=0
for arg in "$@"; do
    case $arg in
        --interactive) interactive=1 ;;
        -*) printf 'sec-gate: unknown option %s\n' "$arg" >&2; exit 2 ;;
        *) build=$arg ;;
    esac
done
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cpupower-secgate.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

pass=0
fail=0
notrun=0

ok()   { pass=$((pass+1)); printf '  ok    %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL  %s\n' "$1"; }
note() { printf '        %s\n' "$1"; }

need() {
    [ -x "$1" ] || { printf 'sec-gate: %s not built. Run: cmake --build %s\n' "$1" "$build" >&2; exit 2; }
}

HELPER="$build/cpu-power-helper"
HELPER_TEST="$build/cpu-power-helper-test"
CLI="$build/cpu-power-cli"
GUI="$build/cpu-power"

need "$HELPER"; need "$HELPER_TEST"; need "$CLI"; need "$GUI"

# The build variables that generate BOTH the .policy file and the compiled-in constants. Read from
# CMakeCache.txt rather than re-derived, so this gate compares what was actually configured.
cache="$build/CMakeCache.txt"
ACTION_ID=$(sed -n 's/^CPUPOWER_POLKIT_ACTION_ID:[^=]*=//p' "$cache" 2>/dev/null || true)
ACTION_DIR=$(sed -n 's/^CPUPOWER_POLKIT_ACTIONDIR:[^=]*=//p' "$cache" 2>/dev/null || true)
[ -n "$ACTION_ID" ] || ACTION_ID=io.github.rations.cpu-power
POLICY="$build/$ACTION_ID.policy"

# Compare two sysfs trees by VALUE rather than byte for byte.
#
# The fixtures were captured with the trailing newline sysfs presents; the helper writes without
# one, exactly as the kernel's own cpupower does (third_party/cpupower/lib/cpufreq.c:46), because
# sysfs neither needs nor keeps it. Both sides strip it on read, so the trailing newline is an
# artifact of how a fixture is STORED and not a property of the tool. Comparing raw bytes would
# therefore assert something that is not true and has no reason to be. What must be identical is
# the value the kernel would read back, and that is what this compares.
same_values() {
    _a=$1; _b=$2; _differ=""
    for _f in $(cd "$_a" && find . -type f ! -name README.md | sort); do
        _x=$(sed -e 's/[[:space:]]*$//' "$_a/$_f" 2>/dev/null || true)
        _y=$(sed -e 's/[[:space:]]*$//' "$_b/$_f" 2>/dev/null || true)
        [ "$_x" = "$_y" ] || _differ="$_differ $_f"
    done
    printf '%s' "$_differ"
}


printf '== 1. protocol refuses hostile input ==\n'
cp -r tests/fixtures/intel-pstate-active-hwp "$tmp/fx"
if out=$("$CLI" --fuzz --helper "$HELPER_TEST" --sysroot "$tmp/fx" 2>&1); then
    n=$(printf '%s' "$out" | sed -n 's/.*  \([0-9]*\) hostile inputs refused.*/\1/p')
    ok "$n hostile inputs refused (path injection, integer overflow, unknown verbs, over-long lines)"
else
    bad "the fuzz corpus was not fully refused"
    printf '%s\n' "$out" | sed 's/^/        /'
fi

# The fixture copy must be untouched: a refused command must write NOTHING.
if diff -r tests/fixtures/intel-pstate-active-hwp "$tmp/fx" >/dev/null 2>&1; then
    ok "refused commands wrote nothing (fixture byte-identical after the fuzz run)"
else
    bad "a refused command modified the machine"
    diff -r tests/fixtures/intel-pstate-active-hwp "$tmp/fx" | head -5 | sed 's/^/        /'
fi

printf '== 1b. apply then exit restores the machine exactly ==\n'
# The property that makes this tool safe to try: the helper restores on EOF, so
# there is no path -- including the caller being SIGKILLed -- that leaves the machine changed.
rm -rf "$tmp/rt"; cp -r tests/fixtures/intel-pstate-active-hwp "$tmp/rt"
"$CLI" --stop maximum --turbo off --helper "$HELPER_TEST" --sysroot "$tmp/rt" >/dev/null 2>&1 || true
d=$(same_values tests/fixtures/intel-pstate-active-hwp "$tmp/rt")
if [ -z "$d" ]; then
    ok "apply 'maximum' + turbo off, then exit -> every value back to where it started"
else
    bad "restore left these values changed:"
    printf '%s\n' "$d" | tr ' ' '\n' | grep -v '^$' | head -5 | sed 's/^/        /'
fi

# ... and the same when the caller is KILLED rather than exiting cleanly.
rm -rf "$tmp/rtk"; cp -r tests/fixtures/intel-pstate-active-hwp "$tmp/rtk"
"$CLI" --stop maximum --turbo off --hold --helper "$HELPER_TEST" --sysroot "$tmp/rtk" >/dev/null 2>&1 &
cli_pid=$!
sleep 1
kill -9 "$cli_pid" 2>/dev/null || true
wait "$cli_pid" 2>/dev/null || true
sleep 1
d=$(same_values tests/fixtures/intel-pstate-active-hwp "$tmp/rtk")
if [ -z "$d" ]; then
    ok "SIGKILL the caller mid-session -> the helper still restored everything (D5)"
else
    bad "SIGKILL left these values changed:"
    printf '%s\n' "$d" | tr ' ' '\n' | grep -v '^$' | head -5 | sed 's/^/        /'
fi

printf '== 1c. DETACH persists on purpose, and ONLY on purpose ==\n'
# Settings outlive the window that set them, which means the helper now has an exit that does
# NOT restore. That is a real weakening of section 1b and the whole of its safety is the
# asymmetry: persisting must be ASKED FOR. A client that dies cannot ask, so a crash still puts
# the machine back. These assertions are what stop that asymmetry being quietly inverted.
#
# They also cover the property that makes "switch it off" still mean something: a detached helper
# keeps holding the state the machine had BEFORE any of this. If it did not, the next session
# would snapshot the already-modified machine and switching off would silently do nothing.
rm -rf "$tmp/dt"; cp -r tests/fixtures/intel-pstate-active-hwp "$tmp/dt"
detach_out=$(python3 - "$HELPER_TEST" "$tmp/dt" <<'PY'
import os, socket, subprocess, sys, time

helper, sysroot = sys.argv[1], sys.argv[2]
epp_path = f"{sysroot}/devices/system/cpu/cpufreq/policy0/energy_performance_preference"
def epp():
    with open(epp_path) as f: return f.read().strip()

def line(sock):
    sock.settimeout(10); buf = b""
    while not buf.endswith(b"\n"):
        c = sock.recv(1)
        if not c: return None
        buf += c
    return buf.decode().strip()

base = epp()
a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
proc = subprocess.Popen([helper, "--serve", "--sysroot", sysroot],
                        stdin=b.fileno(), stdout=b.fileno(), stderr=subprocess.DEVNULL)
b.close()
line(a)
a.sendall(b"SET epp 0 performance\n"); line(a)
a.sendall(b"COMMIT\n"); line(a)
a.sendall(b"DETACH\n")
resp = line(a) or ""
if not resp.startswith("OK "):
    print("FAIL detach-refused"); sys.exit(0)
sock = resp.split(" ", 1)[1]
a.close()
time.sleep(0.5)

print("ALIVE" if proc.poll() is None else "FAIL not-resident")
print("KEPT" if epp() == "performance" else "FAIL not-kept")
mode = oct(os.stat(sock).st_mode & 0o777)
print("MODE-OK" if mode == "0o600" else f"FAIL mode-{mode}")

# A second client while one is attached must be refused, not left hanging.
c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); c.connect(sock)
g = line(c)
print("REATTACH" if g and g.startswith("READY") else "FAIL no-reattach")
d = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    d.connect(sock)
    print("BUSY-REFUSED" if (line(d) or "").startswith("ERR busy") else "FAIL second-client")
except OSError:
    print("BUSY-REFUSED")
d.close()

c.sendall(b"QUIT\n"); line(c)
proc.wait(timeout=10)
print("PRISTINE" if epp() == base else f"FAIL restored-to-{epp()}")
print("CLEANED" if not os.path.exists(sock) else "FAIL socket-left")
PY
)
for claim in ALIVE:"the helper outlives a client that said DETACH" \
             KEPT:"the setting is still applied after the client is gone" \
             MODE-OK:"the rendezvous socket is mode 0600" \
             REATTACH:"a later client re-attaches and is greeted" \
             BUSY-REFUSED:"a SECOND client is refused rather than left hanging" \
             PRISTINE:"switching off after a detach restores the ORIGINAL state, not the detached one" \
             CLEANED:"the socket is removed when the helper exits"; do
    tag=${claim%%:*}; desc=${claim#*:}
    if printf '%s\n' "$detach_out" | grep -qx "$tag"; then
        ok "$desc"
    else
        bad "$desc"
        printf '%s\n' "$detach_out" | grep '^FAIL' | head -2 | sed 's/^/        /'
    fi
done

# The other half of the asymmetry, and the one that must never regress: an unannounced EOF is a
# client that died, and it still restores. 1b proves it for a helper that was never detached;
# this proves the DETACH verb did not quietly become the default.
rm -rf "$tmp/dte"; cp -r tests/fixtures/intel-pstate-active-hwp "$tmp/dte"
"$CLI" --stop maximum --turbo off --helper "$HELPER_TEST" --sysroot "$tmp/dte" >/dev/null 2>&1 || true
d=$(same_values tests/fixtures/intel-pstate-active-hwp "$tmp/dte")
if [ -z "$d" ]; then
    ok "a client that exits WITHOUT detaching still restores everything"
else
    bad "EOF stopped restoring -- persisting has become the default:"
    printf '%s\n' "$d" | tr ' ' '\n' | grep -v '^$' | head -5 | sed 's/^/        /'
fi

# The cross-uid half of "who may re-attach" needs a second uid, which this gate does not have.
# The filesystem half above (0600, owned by the session user) is asserted; the SO_PEERCRED check
# is not, and saying so is better than implying coverage that is not here.
notrun=$((notrun+1))
note "NOT RUN: re-attach from a DIFFERENT uid is refused (SO_PEERCRED) -- needs a second user"

printf '== 2. the pty case is loud, not silent ==\n'
# A terminal on the protocol channel means line discipline -- echo, CR/LF translation, ^D -- and
# that corrupts the protocol silently. The caller's setsid() is what prevents it; this asserts the
# backstop, that the helper REFUSES rather than letting it happen quietly.
if python3 - "$HELPER_TEST" "$tmp/fx" <<'PY'
import os, pty, sys, subprocess
helper, sysroot = sys.argv[1], sys.argv[2]
pid, fd = pty.fork()
if pid == 0:
    os.execv(helper, [helper, "--serve", "--sysroot", sysroot])
out = b""
try:
    while True:
        b = os.read(fd, 1024)
        if not b: break
        out += b
except OSError:
    pass
_, status = os.waitpid(pid, 0)
code = os.waitstatus_to_exitcode(status)
sys.exit(0 if (b"terminal" in out and code != 0) else 1)
PY
then
    ok "helper refuses to serve on a tty, with a message naming the cause"
else
    bad "helper did NOT refuse a pty -- the protocol would be silently corrupted from a shell"
fi

printf '== 3. --sysroot is absent from the privileged binary (D12) ==\n'
np=$(strings "$HELPER" | grep -c -- 'sysroot' || true)
nt=$(strings "$HELPER_TEST" | grep -c -- 'sysroot' || true)
if [ "$np" -eq 0 ] && [ "$nt" -gt 0 ]; then
    ok "privileged binary contains no 'sysroot' string; test build contains $nt"
else
    bad "privileged binary mentions sysroot $np time(s) (test build: $nt)"
fi

# ... and it must actually reject the option, not merely lack the string.
if "$HELPER" --sysroot /tmp --serve </dev/null >/dev/null 2>&1; then
    bad "the privileged helper ACCEPTED --sysroot"
else
    ok "the privileged helper rejects --sysroot as an unknown option"
fi

printf '== 4. the install sets no setuid/setgid bit (D3) ==\n'
rm -rf "$tmp/destdir"
if DESTDIR="$tmp/destdir" cmake --install "$build" >/dev/null 2>&1; then
    found=$(find "$tmp/destdir" -type f -perm /6000 2>/dev/null || true)
    if [ -z "$found" ]; then
        ok "no setuid or setgid bits anywhere in the installed tree"
    else
        bad "the install set a setuid/setgid bit:"
        printf '%s\n' "$found" | sed 's/^/        /'
    fi
    n=$(find "$tmp/destdir" -type f | wc -l)
    note "installed $n files"

    # Nothing installed may be writable by anyone but its owner. This matters more now than it
    # did: one of these files is the rule that says who may run as root, and a world-writable
    # .policy would let any local user rewrite that rule.
    loose=$(find "$tmp/destdir" -type f -perm /022 2>/dev/null || true)
    if [ -z "$loose" ]; then
        ok "no installed file is group- or world-writable"
    else
        bad "installed file(s) writable by group or other:"
        printf '%s\n' "$loose" | sed 's/^/        /'
    fi

    # The action file must be installed, and exactly one of them.
    staged=$(find "$tmp/destdir" -type f -name '*.policy' 2>/dev/null || true)
    nstaged=$(printf '%s\n' "$staged" | grep -c . || true)
    if [ "$nstaged" -eq 1 ]; then
        ok "the install stages exactly one polkit action: $(basename "$staged")"
    else
        bad "expected exactly one .policy in the installed tree, found $nstaged"
    fi

    # Non-binaries at 0644. A .policy at 0755 is not a hole, but it is a sign something is
    # installing it by a route that did not think about it.
    badmode=""
    for f in $staged; do
        m=$(stat -c '%a' "$f")
        [ "$m" = "644" ] || badmode="$badmode $f($m)"
    done
    if [ -z "$badmode" ]; then
        ok "the installed action is mode 0644"
    else
        bad "unexpected mode on installed action:$badmode"
    fi
else
    bad "cmake --install failed"
fi

printf '== 5. hardening is present in the built binaries ==\n'
# The GUI is in this list now, and it was not before. It is the process that builds an argv and
# hands it to a SETUID-ROOT binary, which makes its own memory safety part of the privilege
# boundary rather than merely a quality matter.
for bin in "$HELPER" "$GUI" "$CLI" "$build/coretest"; do
    name=$(basename "$bin")
    problems=""
    readelf -lW "$bin" 2>/dev/null | grep -q 'GNU_RELRO'                || problems="$problems no-relro"
    readelf -dW "$bin" 2>/dev/null | grep -q 'BIND_NOW\|FLAGS.*NOW'     || problems="$problems no-bindnow"
    readelf -hW "$bin" 2>/dev/null | grep -q 'Type:[[:space:]]*DYN'     || problems="$problems no-pie"
    readelf -lW "$bin" 2>/dev/null | grep 'GNU_STACK' | grep -q 'RWE'   && problems="$problems exec-stack"
    readelf -sW "$bin" 2>/dev/null | grep -q '__stack_chk'              || problems="$problems no-canary"
    if [ -z "$problems" ]; then
        ok "$name: full RELRO, BIND_NOW, PIE, NX stack, stack canary"
    else
        bad "$name:$problems"
    fi
done

printf '== 6. the helper path is compiled in, not taken from the environment (D13) ==\n'
# `expected` was computed here and then never used, which meant this check could pass against a
# path that had nothing to do with the configured one. Take it from CMakeCache.txt (what was
# actually configured) and compare against it.
expected=$(sed -n 's/^CPUPOWER_HELPER_PATH:[^=]*=//p' "$cache" 2>/dev/null || true)
if [ -z "$expected" ]; then
    expected=$(sed -n 's/^set(CPUPOWER_HELPER_PATH  *"\(.*\)").*$/\1/p' CMakeLists.txt 2>/dev/null || true)
    expected=$(printf '%s' "$expected" | sed "s|\${CPUPOWER_LIBEXECDIR}|$(sed -n 's/^CMAKE_INSTALL_PREFIX:[^=]*=//p' "$cache")/libexec/cpu-power|")
fi
if [ -n "$expected" ] && strings "$CLI" | grep -qxF "$expected"; then
    ok "the configured absolute helper path is baked into the binary ($expected)"
elif strings "$CLI" | grep -q 'libexec/cpu-power/cpu-power-helper'; then
    bad "a compiled-in helper path exists but does not match the configured one ($expected)"
else
    bad "no compiled-in helper path found in $CLI"
fi

# A decoy earlier in $PATH must never be executed. If the path were resolved at runtime, this
# would run the decoy; because it is a compile-time constant, the decoy is never touched.
mkdir -p "$tmp/decoy"
cat > "$tmp/decoy/cpu-power-helper" <<'DECOY'
#!/bin/sh
touch "$DECOY_MARKER"
echo "READY driver=decoy policies=1 turbo=none"
sleep 5
DECOY
chmod +x "$tmp/decoy/cpu-power-helper"
# A decoy pkexec and pkcheck as well as the helper. pkexec matters most: it is setuid root, so a
# $PATH-resolved pkexec would hand our argv to whatever an attacker put in front of it. pkcheck is
# here because the polkit rung is now gated on its answer -- a decoy that exits 0 would talk the
# program into using pkexec when polkit had not agreed to anything. Both are found by absolute
# path. A decoy 'sudo' is planted too, and it is a REGRESSION TEST NOW RATHER THAN A PATH TEST:
# the sudo rung was deleted, so this asserts that nothing quietly reintroduces it.
cp "$tmp/decoy/cpu-power-helper" "$tmp/decoy/sudo"
cp "$tmp/decoy/cpu-power-helper" "$tmp/decoy/pkexec"
cp "$tmp/decoy/cpu-power-helper" "$tmp/decoy/pkcheck"
rm -f "$tmp/decoy-ran"
DECOY_MARKER="$tmp/decoy-ran" PATH="$tmp/decoy:$PATH" \
    timeout 10 "$CLI" --show >/dev/null 2>&1 || true
if [ -f "$tmp/decoy-ran" ]; then
    bad "a decoy helper, sudo, pkexec or pkcheck earlier in \$PATH WAS EXECUTED"
else
    ok "decoy helper, sudo, pkexec and pkcheck in \$PATH were never executed"
fi

printf '== 7. the installed action pins the exact helper, and nothing wider ==\n'
# THE .policy FILE IS THE SECURITY BOUNDARY. Everything here is checked
# against the GENERATED file, not the template, because the template contains @VARIABLES@ and it
# is the substitution that can go wrong.
if [ ! -f "$POLICY" ]; then
    bad "no generated action at $POLICY -- run cmake to generate it"
else
    # It must parse AT ALL. polkitd does not report a malformed policy; it ignores the file, and
    # pkexec then falls back to org.freedesktop.policykit.exec (auth_admin). The tool keeps
    # working and merely starts asking for a password. This has already caught a real mistake
    # twice: a literal double hyphen inside an XML comment is illegal and silently fatal.
    dtd=/usr/share/polkit-1/policyconfig-1.dtd
    if ! command -v xmllint >/dev/null 2>&1; then
        notrun=$((notrun+1))
        printf '  NOT RUN  XML validation (xmllint not installed)\n'
    elif [ ! -f "$dtd" ]; then
        notrun=$((notrun+1))
        printf '  NOT RUN  XML validation (%s not present)\n' "$dtd"
    elif xmllint --noout --dtdvalid "$dtd" "$POLICY" 2>/dev/null; then
        ok "the generated action is well-formed and valid against policyconfig-1.dtd"
    else
        bad "the generated action does NOT validate -- polkitd would silently ignore it:"
        xmllint --noout --dtdvalid "$dtd" "$POLICY" 2>&1 | head -4 | sed 's/^/        /'
    fi

    nact=$(grep -c '<action ' "$POLICY" || true)
    if [ "$nact" -eq 1 ]; then
        ok "the file declares exactly one action"
    else
        bad "expected exactly one <action>, found $nact"
    fi

    got_id=$(sed -n 's/.*<action id="\([^"]*\)".*/\1/p' "$POLICY" | head -1)
    if [ "$got_id" = "$ACTION_ID" ]; then
        ok "action id is the configured literal ($ACTION_ID)"
    else
        bad "action id is '$got_id', configured is '$ACTION_ID'"
    fi

    # The id in the FILE and the id compiled into the BINARY must be the same string. If they
    # drift, pkexec matches no action and every user gets an administrator password prompt.
    if strings "$CLI" | grep -qxF "$ACTION_ID"; then
        ok "the same action id is compiled into the binary"
    else
        bad "the binary does not carry the action id '$ACTION_ID'"
    fi

    got_path=$(sed -n 's|.*exec\.path">\([^<]*\)<.*|\1|p' "$POLICY" | head -1)
    if [ -n "$expected" ] && [ "$got_path" = "$expected" ]; then
        ok "exec.path is the configured helper path, byte for byte"
    else
        bad "exec.path is '$got_path', configured helper path is '$expected'"
    fi

    # No wildcard, no shell metacharacter, no relative component. polkit does no canonicalisation,
    # so anything clever here is a way to make the pin mean less than it appears to.
    case $got_path in
        /*) case $got_path in
                *[\*\?\[\]\$\`\"\'\ ]*|*/../*|*/./*)
                    bad "exec.path contains a wildcard, metacharacter or relative component" ;;
                *) ok "exec.path is absolute, with no wildcard or metacharacter" ;;
            esac ;;
        *) bad "exec.path is not absolute: '$got_path'" ;;
    esac

    got_argv1=$(sed -n 's|.*exec\.argv1">\([^<]*\)<.*|\1|p' "$POLICY" | head -1)
    if [ "$got_argv1" = "--serve" ]; then
        ok "exec.argv1 pins argv[1] to --serve"
    else
        bad "exec.argv1 is '$got_argv1', expected --serve"
    fi

    # The three defaults ARE the security model. allow_active=yes is the grant; the other two
    # being 'no' is what makes it mean "the person at this machine" rather than "anyone".
    d_any=$(sed -n 's|.*<allow_any>\([^<]*\)<.*|\1|p' "$POLICY" | head -1)
    d_ina=$(sed -n 's|.*<allow_inactive>\([^<]*\)<.*|\1|p' "$POLICY" | head -1)
    d_act=$(sed -n 's|.*<allow_active>\([^<]*\)<.*|\1|p' "$POLICY" | head -1)
    if [ "$d_any" = "no" ] && [ "$d_ina" = "no" ] && [ "$d_act" = "yes" ]; then
        ok "defaults are strict: any=no inactive=no active=yes"
    else
        bad "defaults are any=$d_any inactive=$d_ina active=$d_act (want no/no/yes)"
    fi

    # The complete set of annotation KEYS must be exactly the two we pin. Matching on the whole
    # file is not good enough -- the first version of this check grepped for 'allow_gui' and
    # failed against the comment explaining that there deliberately is not one. Read the keys.
    #
    # Checking the whole set rather than just allow_gui also catches the other annotations polkit
    # understands: .imply would make this action authorise OTHER actions, and .owner would widen
    # who may query it. Neither belongs here, and neither would be noticed by a test that only
    # looked for the one name somebody happened to think of.
    keys=$(sed -n 's/.*<annotate key="\([^"]*\)".*/\1/p' "$POLICY" | sort | tr '\n' ' ')
    want="org.freedesktop.policykit.exec.argv1 org.freedesktop.policykit.exec.path "
    if [ "$keys" = "$want" ]; then
        ok "annotations are exactly exec.path and exec.argv1 -- no allow_gui, imply or owner"
    else
        bad "unexpected annotation set: [$keys]"
        note "expected exactly: [$want]"
    fi
fi

# An application ships a .policy and NEVER a .rules file: polkit(8) says applications "must never
# include any authorization rules", and a rules file is JavaScript evaluated inside a root daemon.
if find . -name '*.rules' -not -path './build/*' -not -path './third_party/*' 2>/dev/null | grep -q .; then
    bad "this project contains a .rules file; that namespace belongs to the administrator"
else
    ok "this project ships no polkit .rules file"
fi

printf '== 7b. polkitd agrees -- it parsed what we think we wrote ==\n'
# Section 7 reads OUR file. This asks the daemon what it actually ended up with, which is the only
# thing that matters at run time, and is the check that catches a policy installed somewhere
# polkitd does not look.
if ! command -v pkaction >/dev/null 2>&1; then
    notrun=$((notrun+1))
    printf '  NOT RUN  polkitd cross-check (pkaction not installed)\n'
elif ! pkaction --action-id "$ACTION_ID" >/dev/null 2>&1; then
    notrun=$((notrun+1))
    printf '  NOT RUN  polkitd cross-check (action not installed on this machine)\n'
    note "install it with: sudo cmake --install $build"
    note "expected location: ${ACTION_DIR:-/usr/share/polkit-1/actions}"
else
    v=$(pkaction --action-id "$ACTION_ID" --verbose 2>/dev/null)
    miss=""
    printf '%s' "$v" | grep -q "implicit any: *no"      || miss="$miss allow_any"
    printf '%s' "$v" | grep -q "implicit inactive: *no" || miss="$miss allow_inactive"
    printf '%s' "$v" | grep -q "implicit active: *yes"  || miss="$miss allow_active"
    printf '%s' "$v" | grep -q "exec.path -> $expected" || miss="$miss exec.path"
    printf '%s' "$v" | grep -q "exec.argv1 -> --serve"  || miss="$miss exec.argv1"
    if [ -z "$miss" ]; then
        ok "polkitd reports the same id, path, argv1 and three defaults"
    else
        bad "polkitd's parsed action differs from the file:$miss"
        printf '%s\n' "$v" | sed 's/^/        /'
    fi
fi

printf '== 8. the argv1 pin does something, and the helper covers what it cannot ==\n'
# pkexec pins argv[1] AND NOTHING AFTER IT -- it "does no validation of the ARGUMENTS". So
# `pkexec <helper> --serve <anything>` reaches the helper ALREADY AUTHORISED, for any locally
# seated user. The helper refusing unknown arguments is therefore not tidiness, it is the second
# half of the pin, and section 3 only ever asserted it for --sysroot.
for extra in "--extra" "-x" "/etc/shadow" "--sysroot /tmp"; do
    # shellcheck disable=SC2086
    if "$HELPER" --serve $extra </dev/null >/dev/null 2>&1; then
        bad "the privileged helper ACCEPTED '--serve $extra'"
    else
        ok "the privileged helper refuses '--serve $extra'"
    fi
done

# ... and it must refuse to serve at all with no --serve, rather than defaulting to it.
if "$HELPER" </dev/null >/dev/null 2>&1; then
    bad "the privileged helper served with no arguments at all"
else
    ok "the privileged helper refuses to serve without --serve"
fi

printf '== 8b. polkit is executed, never linked ==\n'
# pkexec is a separate process. No polkit header in the include
# graph, no polkit library on the link line. This is a licence property as much as a design one --
# polkit is LGPL and this project is MIT -- and asserting it is cheaper than remembering it.
linked=""
for bin in "$GUI" "$CLI" "$HELPER"; do
    l=$(ldd "$bin" 2>/dev/null | grep -i 'polkit' || true)
    [ -z "$l" ] || linked="$linked $(basename "$bin")"
done
if [ -z "$linked" ]; then
    ok "no binary links anything polkit"
else
    bad "polkit appears on the link line of:$linked"
fi

# The helper links libc and nothing else: less to audit, and nothing that
# could carry a surprise into the one process that runs as root.
nlib=$(ldd "$HELPER" 2>/dev/null | grep -c '=>' || true)
if [ "$nlib" -le 1 ]; then
    ok "the privileged helper links libc only ($nlib shared object)"
else
    bad "the privileged helper links $nlib shared objects; it should link libc only"
    ldd "$HELPER" | sed 's/^/        /'
fi

printf '== 8c. nothing here can ask polkit to interact ==\n'
# --allow-user-interaction would make the pre-flight pkcheck block on a dialog instead of
# answering; --enable-internal-agent would let pkcheck register an agent of its own. Neither may
# ever appear in anything this project ships.
#
# NOTE WHAT THIS DOES *NOT* CLAIM. It does not prove the polkit rung is gated on pkcheck at all.
# That was tried here as a `strings` check for the compiled-in pkcheck path, and it was BLIND:
# deleting the gate outright left the path string in the binary and the check still passed. A
# check that cannot fail is worse than no check, because it is mistaken for cover. The real
# assertion is behavioural and needs a live polkit session, so it lives in scripts/gate.sh step 2c.
prompting=""
for bin in "$GUI" "$CLI" "$HELPER"; do
    f=$(strings "$bin" | grep -x -e '--allow-user-interaction' -e '--enable-internal-agent' || true)
    [ -z "$f" ] || prompting="$prompting $(basename "$bin")($(echo $f | tr '\n' ' '))"
done
if [ -z "$prompting" ]; then
    ok "no binary contains a flag that would let polkit interact"
else
    bad "a flag that permits interaction appears in:$prompting"
fi

printf '== 9. the gate itself left no core file ==\n'
# Was "askpass leaves no core file". There is no askpass; the claim now covers every binary this
# gate ran, including the helper, which sets RLIMIT_CORE=0 for the same reason the askpass did:
# a core file is a copy of a privileged process's memory sitting on disk.
if [ -n "$(find . -maxdepth 1 -name 'core*' -newermt '-1 minute' 2>/dev/null)" ]; then
    bad "a core file was produced"
else
    ok "no core file produced by anything this gate ran"
fi

printf '\n%d passed, %d failed, %d not run\n' "$pass" "$fail" "$notrun"
# NOT RUN is reported separately and never counted as a pass: an assertion that did not run has
# not established anything, and rolling it into the pass count is how a gate quietly stops being
# one.
[ "$fail" -eq 0 ] || exit 1
