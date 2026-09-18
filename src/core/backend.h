// Backend — what power knobs THIS machine actually has.
//
// This is a CAPABILITY PROBE, not a driver whitelist. Nothing here is hardcoded per driver:
// governor names come from scaling_available_governors, energy-performance-preference names from
// energy_performance_available_preferences, frequency bounds from cpuinfo_{min,max}_freq, and the
// turbo knob from whichever of four possible attributes is present. A driver this code has never
// heard of still works, because probing is the whole of the detection.
//
// The driver name is recorded and displayed, but nothing BRANCHES on it except where the kernel
// documentation says the interface itself differs. That is the difference between a tool that
// works on the author's machine and one that works on the user's.
//
// Turbo is the messiest part of the interface, because four different attributes mean the same
// thing. The order below is not arbitrary; each step cites the documentation that fixes it:
//
//   1. intel_pstate/no_turbo  -- INVERTED (1 means no turbo).
//      "Note that ``intel_pstate`` does not support the general ``boost`` attribute (supported by
//       some other scaling drivers) which is replaced by this one."
//           -- intel_pstate.rst:501-503
//      So on intel_pstate this is the only knob, and looking for `boost` first would find nothing.
//
//   2. cpufreq/boost  -- the global knob, 0 or 1 (cpufreq.rst).
//      Preferred over `cpb` because the documentation says so outright: "it is always possible use
//      the ``boost`` knob instead of the ``cpb`` one which is highly recommended, as that is more
//      consistent with what all of the other systems do (and the ``cpb`` knob may not be supported
//      any more in the future)."  -- cpufreq.rst:713-716
//
//   3. policyX/boost  -- per-policy, used by amd_pstate.
//      "users can write a value of `0` to disable the boost or `1` to enable it, for the
//       respective CPU using the sysfs path `/sys/devices/system/cpu/cpuX/cpufreq/boost`"
//           -- amd-pstate.rst:349-351
//
//   4. policyX/cpb  -- legacy powernow-k8, last resort. "never present for any processors without
//      the underlying hardware feature (e.g. all Intel ones)"  -- cpufreq.rst:718-720
//
//   5. none -- the machine has no turbo control. The checkbox is DISABLED AND LABELLED, never
//      silently ignored.

#pragma once

#include "sysfs.h"
#include "topology.h"

#include <string>
#include <vector>

namespace cpupower
{

enum class TurboKnob {
    None,         // no turbo control on this machine
    IntelNoTurbo, // intel_pstate/no_turbo -- INVERTED
    GlobalBoost,  // cpufreq/boost
    PolicyBoost,  // policyX/boost
    PolicyCpb,    // policyX/cpb (legacy)
};

// intel_pstate/status and amd_pstate/status share a vocabulary but not a full one: amd_pstate
// additionally has "guided". Unknown is a mode string we do not recognise, which must not be
// treated as any of the others.
enum class PstateMode {
    NotApplicable, // driver is not intel_pstate or amd_pstate
    Active,
    Passive,
    Guided, // amd_pstate only (amd-pstate.rst:510)
    Disabled,
    Unknown,
};

struct Backend {
    std::string driver; // scaling_driver, e.g. "intel_pstate", "acpi-cpufreq"
    PstateMode mode = PstateMode::NotApplicable;
    std::string modeText; // the raw status string, for display

    std::vector<Policy> policies;

    std::vector<std::string> governors;  // scaling_available_governors
    std::vector<std::string> eppChoices; // energy_performance_available_preferences ({} if none)

    TurboKnob turbo = TurboKnob::None;

    // intel_pstate's global percentage limits. Absent when the driver is not intel_pstate, and
    // ALSO absent on intel_pstate when the kernel was booted with intel_pstate=per_cpu_perf_limits
    // -- "``max_perf_pct`` and ``min_perf_pct`` are not exposed at all" (intel_pstate.rst:657-659).
    // Which is exactly why this is probed rather than inferred from the driver name.
    bool hasPerfPct = false;

    long long cpuinfoMinKhz = 0; // from policy0; 0 if unreadable
    long long cpuinfoMaxKhz = 0;

    // Probe the machine. Never throws; a machine with no cpufreq at all yields a Backend with no
    // policies, which usable() reports as false.
    static Backend probe(const Sysfs &fs);

    // False when there is nothing this tool can do here: no policies, or no governors to pick
    // from. The GUI shows the reason rather than an empty window.
    bool usable() const;

    // Why this machine is not usable, for display. Empty when it is.
    std::string unusableReason() const;

    bool hasEpp() const
    {
        return !eppChoices.empty();
    }

    bool supports(const std::string &governor) const;
    bool supportsEpp(const std::string &preference) const;

    // Path of the turbo attribute for a given policy, relative to the sysfs root. Empty when
    // turbo == None. For the global knobs the policy argument is ignored.
    std::string turboPath(const Policy &p) const;

    // True when writing 1 to turboPath() DISABLES turbo (intel_pstate/no_turbo). The GUI shows a
    // "Turbo" checkbox, so the inversion has to be resolved in exactly one place -- here.
    bool turboInverted() const
    {
        return turbo == TurboKnob::IntelNoTurbo;
    }

    // True when the turbo knob is per-policy and must be written for every policy rather than once.
    bool turboPerPolicy() const
    {
        return turbo == TurboKnob::PolicyBoost || turbo == TurboKnob::PolicyCpb;
    }

    // Human-readable one-liner for the window's header, e.g.
    // "intel_pstate (active, HWP)  20 policies  0.80-4.80 GHz".
    std::string describe() const;
};

const char *turboKnobName(TurboKnob k);
const char *pstateModeName(PstateMode m);

} // namespace cpupower
