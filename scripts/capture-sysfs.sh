#!/bin/sh
# Capture this machine's CPU power sysfs tree into a fixture directory.
#
# The fixture mirrors the real tree exactly under a root, so that pointing the model at it with
# --sysroot <dir> is transparent: <dir> stands in for /sys and nothing else changes.
#
# Run this on ANY machine and commit the result. A fixture from a real machine is evidence; a
# hand-written one is only evidence that the documented attribute shape is handled, and must say
# so in its README.
#
# Usage:  scripts/capture-sysfs.sh <output-dir> [description...]
#
# Reads only. Needs no privilege: every attribute captured is world-readable.

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <output-dir> [description...]" >&2
    exit 64
fi

out=$1
shift
desc=${*:-}

src=/sys/devices/system/cpu
[ -d "$src" ] || { echo "$0: no $src on this machine" >&2; exit 1; }

dst="$out/devices/system/cpu"
mkdir -p "$dst"

# Copy one attribute, if it exists and is readable. A file that is write-only (some sysfs
# attributes are) or that errors on read is skipped rather than captured as empty, because an
# empty capture and an absent attribute must not look the same to the model.
copy_attr() {
    _rel=$1
    [ -f "$src/$_rel" ] || return 0
    [ -r "$src/$_rel" ] || return 0
    if _val=$(cat "$src/$_rel" 2>/dev/null); then
        mkdir -p "$(dirname "$dst/$_rel")"
        printf '%s\n' "$_val" > "$dst/$_rel"
    fi
}

# Top-level CPU enumeration.
for a in present online offline possible; do
    copy_attr "$a"
done

# Per-policy attributes. policyN is canonical; cpuN/cpufreq are symlinks into it
# (cpufreq.rst:204-212), so capturing policyN captures everything.
for p in "$src"/cpufreq/policy*; do
    [ -d "$p" ] || continue
    n=$(basename "$p")
    for a in \
        affected_cpus related_cpus \
        scaling_driver scaling_governor scaling_available_governors \
        scaling_cur_freq scaling_min_freq scaling_max_freq scaling_setspeed \
        cpuinfo_min_freq cpuinfo_max_freq cpuinfo_transition_latency \
        scaling_available_frequencies scaling_boost_frequencies \
        energy_performance_preference energy_performance_available_preferences \
        base_frequency bios_limit boost cpb
    do
        copy_attr "cpufreq/$n/$a"
    done
done

# Global cpufreq attributes (the generic boost knob lives here).
for a in boost; do
    copy_attr "cpufreq/$a"
done

# intel_pstate globals.
for a in status no_turbo min_perf_pct max_perf_pct num_pstates turbo_pct hwp_dynamic_boost; do
    copy_attr "intel_pstate/$a"
done

# amd_pstate globals (name differs from the driver name).
for a in status prefcore; do
    copy_attr "amd_pstate/$a"
done

# cpuidle: which C-states exist, and their exit latencies. Not written by this tool, but it is
# what DAW mode is about, so a fixture that records it can explain the machine.
for s in "$src"/cpu0/cpuidle/state*; do
    [ -d "$s" ] || continue
    n=$(basename "$s")
    for a in name desc latency residency disable; do
        copy_attr "cpu0/cpuidle/$n/$a"
    done
done

# Provenance. A fixture without this is not evidence about anything.
{
    echo "# Fixture: $(basename "$out")"
    echo
    if [ -n "$desc" ]; then
        echo "$desc"
        echo
    fi
    echo '**REAL** — captured from a running machine by `scripts/capture-sysfs.sh`.'
    echo
    echo '## Machine'
    echo
    echo '```'
    echo "captured    $(date -u '+%Y-%m-%d %H:%M:%SZ')"
    [ -r /etc/os-release ] && echo "distro      $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
    echo "kernel      $(uname -srm)"
    echo "cpu         $(grep -m1 '^model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
    echo "policies    $(find "$src/cpufreq" -maxdepth 1 -name 'policy*' 2>/dev/null | wc -l)"
    echo "driver      $(cat "$src/cpufreq/policy0/scaling_driver" 2>/dev/null || echo 'n/a')"
    echo "governors   $(cat "$src/cpufreq/policy0/scaling_available_governors" 2>/dev/null || echo 'n/a')"
    echo "governor    $(cat "$src/cpufreq/policy0/scaling_governor" 2>/dev/null || echo 'n/a')"
    echo "epp avail   $(cat "$src/cpufreq/policy0/energy_performance_available_preferences" 2>/dev/null || echo 'ABSENT')"
    echo "pstate mode $(cat "$src/intel_pstate/status" 2>/dev/null || echo 'n/a')"
    echo "boost knob  $([ -f "$src/cpufreq/boost" ] && echo 'cpufreq/boost' || { [ -f "$src/intel_pstate/no_turbo" ] && echo 'intel_pstate/no_turbo' || echo 'ABSENT'; })"
    echo '```'
    echo
    echo '## Files'
    echo
    echo '```'
    ( cd "$out" && find . -type f ! -name README.md | sort | sed 's|^\./||' | head -40 )
    _n=$( cd "$out" && find . -type f ! -name README.md | wc -l )
    [ "$_n" -gt 40 ] && echo "... and $((_n - 40)) more ($_n files total)"
    echo '```'
} > "$out/README.md"

echo "captured $(find "$out" -type f ! -name README.md | wc -l) attributes into $out"
