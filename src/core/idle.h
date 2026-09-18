// Idle — what this machine's CPUs do when they have NOTHING to do, and how long it takes them to
// start again.
//
// THIS IS A DIFFERENT KERNEL SUBSYSTEM FROM EVERYTHING ELSE IN core/. backend.h, plan.h and
// profile.h are cpufreq: P-states, governors, EPP, turbo -- how fast a core runs WHILE IT IS
// RUNNING. This file is cpuidle: C-states -- what an idle core does and what it costs to wake it.
// The slider cannot reach this, which is the whole reason DAW mode is a separate control: the
// `performance` governor pins the top P-state and does nothing whatsoever about idle.
//
// For a low-latency audio workload the idle side is usually the one that hurts, because an audio
// thread is exactly the workload that idles -- each block finishes early and the core sleeps
// until the next period interrupt. When that interrupt lands:
//
//   "the exit latency [...] is the maximum time it will take a CPU asking the processor hardware
//    to enter an idle state to start executing the first instruction after a wakeup from that
//    state."
//        -- Linux Documentation/admin-guide/pm/cpuidle.rst, lines 140-142
//
// Before the first instruction. The core's clock speed is not yet relevant, because it has not
// started. On the development machine C10 costs 680 us of a 1333 us period (64 samples at 48 kHz)
// and no cpufreq setting shortens it.
//
// ---------------------------------------------------------------------------------------------
// THE PROBE IS ARCHITECTURE-INDEPENDENT, AND THAT IS THE KERNEL'S DOING, NOT OURS:
//
//   "idle states that the hardware can be asked to enter by logical CPUs are represented in an
//    abstract way independent of the platform or the processor architecture and organized in a
//    one-dimensional (linear) array. [...] This allows ``CPUIdle`` governors to be independent of
//    the underlying hardware and to work with any platforms that the Linux kernel can run on."
//        -- cpuidle.rst:125-131
//
// So `cpuN/cpuidle/stateM/latency` means the same thing on x86, on AMD and on ARM, exactly as
// `scaling_available_governors` does for cpufreq. This file probes it and never names a state, a
// driver or an architecture -- there is no list of C-state names here on purpose.
//
// Note `cpuN/cpuidle` is a REAL DIRECTORY, unlike `cpuN/cpufreq`, which is a symlink into
// cpufreq/policyX. Nothing here has to work around that the way topology.h does.

#pragma once

#include "sysfs.h"

#include <optional>
#include <string>
#include <vector>

namespace cpupower
{

struct IdleStates {
    // One entry of the kernel's linear array, as one CPU reports it.
    struct State {
        int index = -1;             // the M in stateM; ascending index means increasing depth
        std::string name;           // "POLL", "C1E", "WFI" -- for display only, never matched on
        long long latencyUs = -1;   // stateM/latency, the exit latency
        long long residencyUs = -1; // stateM/residency, the target residency
    };

    // The states as CPU 0 reports them, sorted by index. Display only: `limitUs` is aggregated
    // across every CPU, because on a heterogeneous machine CPU 0 does not speak for all of them.
    std::vector<State> states;

    // True when the CPUs do NOT all publish the same shallowest halting state -- P-cores and
    // E-cores, or an ARM big.LITTLE. It matters for display: `states` is one CPU's table, while
    // `limitUs` is aggregated over all of them, so on such a machine the limit can be a number
    // that does not appear in the table shown. Saying so is better than looking like an error.
    bool heterogeneous = false;

    // True when this machine exposes cpuidle at all. A kernel built without CPUIdle, or a VM,
    // yields false -- which the GUI must report rather than silently treat as "no deep states".
    bool present = false;

    // The PM-QoS limit DAW mode should request, in microseconds, or nullopt when there is no
    // value that helps.
    //
    // WHY THIS IS NOT ZERO, WHICH IS WHAT THIS TOOL USED TO WRITE. The governors are bound by
    //
    //   "CPU idle time governors are expected to regard the minimum of the global (effective) CPU
    //    latency limit [...] as the upper limit for the exit latency of the idle states that they
    //    are allowed to select for that CPU. They should never select any idle states with exit
    //    latency beyond that limit."
    //        -- cpuidle.rst:577-581
    //
    // A limit of 0 therefore admits only states whose exit latency is 0, and the only such state
    // on any architecture is the POLL pseudo-state, which does not halt the core at all:
    //
    //   "there are strict latency constraints preventing any of the available idle states from
    //    being used, the CPU will simply execute more or less useless instructions in a loop
    //    until it is assigned a new task to run."
    //        -- cpuidle.rst:107-109
    //
    // Every core busy-spinning is a large, permanent power and thermal cost, and it buys nothing
    // over halting in the shallowest state -- which on the development machine is C1E at 2 us,
    // 0.15% of a 1333 us period against the 51% that C10 costs.
    //
    // So the limit is the SHALLOWEST STATE THAT ACTUALLY HALTS: the smallest non-zero exit
    // latency the machine reports. That is a machine-derived number with no constant behind it,
    // and it states DAW mode's contract exactly -- the lowest wake-up latency the hardware can
    // give without resorting to the spin loop.
    std::optional<long long> limitUs;

    // The state `limitUs` admits, for the readout, or nullptr. Named so a user can see WHAT the
    // machine resolved to rather than only a number -- a stop that collapses is visible, and so
    // is this.
    const State *admitted() const;

    // Why limitUs is absent, for display. Empty when it is present.
    std::string unavailableReason() const;

    // Probe. Never throws. Scans every cpuN the machine exposes, not just cpu0.
    static IdleStates probe(const Sysfs &fs);
};

} // namespace cpupower
