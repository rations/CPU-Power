// Profile — the five slider stops, and how each becomes an ordered WritePlan.
//
// WHY FIVE NAMED STOPS AND NOT A PERCENTAGE: the kernel's power knobs are
// enumerations. `ondemand` is not a number and there is no continuum between it and `schedutil`.
// A 0-100% slider maps onto intel_pstate's max_perf_pct and onto nothing meaningful anywhere
// else, so it would be a lie on most machines.
//
// Each stop is a PREFERENCE-ORDERED WISH, resolved against what the running kernel actually
// offers. The first governor in the list that appears in scaling_available_governors wins; the
// EPP name is used only if this machine has EPP at all. The same table therefore differentiates
// the stops by EPP on an intel_pstate machine (where only `performance` and `powersave` governors
// exist) and by GOVERNOR on an acpi-cpufreq machine (where EPP does not exist):
//
//                      intel_pstate active+HWP        acpi-cpufreq with the full governor set
//   Powersave          powersave + power              powersave
//   Balanced           powersave + balance_power      conservative
//   Balanced-Perf.     powersave + balance_perf.      ondemand
//   Performance        powersave + performance        schedutil
//   Maximum            performance (EPP forced)       performance
//
// On a machine offering fewer governors and no EPP, stops collapse into each other. That is
// correct and it is VISIBLE: WritePlan records what each stop resolved to and the window shows
// it, so a stop that does nothing new says so rather than pretending.
//
// ---------------------------------------------------------------------------------------------
// THE GOVERNOR RANKING IS A JUDGEMENT, and is marked as one.
//
// Two thirds of it is grounded in the kernel documentation:
//
//   * `conservative` below `ondemand`: conservative "avoids changing the frequency significantly
//     over short time intervals which may not be suitable for systems with limited power supply
//     capacity (e.g. battery-powered) [...] it changes the frequency in relatively small steps"
//     and its sampling_down_factor "causes the frequency to go down ``sampling_down_factor``
//     times slower than it ramps up."   -- cpufreq.rst:567-608
//
//   * `powersave` at the bottom and `performance` at the top: those are what they are.
//
// The remaining choice -- `schedutil` ranked above `ondemand` -- is NOT a documented ordering.
// It is this project's judgement, taken because ondemand's own documentation names the drawback
// schedutil exists to fix: ondemand's worker "is invoked asynchronously (via a workqueue) [...]
// it causes additional CPU context switches to happen relatively often and the CPU P-state
// updates triggered by it can be relatively irregular" (cpufreq.rst:468-473). Irregular P-state
// updates are precisely what hurts a low-latency audio workload, which is what this tool is for.
// If that judgement is wrong, it is wrong HERE, in one table, and nowhere else.

#pragma once

#include "backend.h"
#include "plan.h"
#include "sysfs.h"

#include <string>
#include <vector>

namespace cpupower
{

enum class Stop {
    Powersave = 0,
    Balanced = 1,
    BalancedPerf = 2,
    Performance = 3,
    Maximum = 4,
};

inline constexpr int kStopCount = 5;

// Short label for the slider tick, e.g. "Balanced".
const char *stopLabel(Stop s);

// Stable machine-readable name for golden files and the config file, e.g. "balanced-perf".
const char *stopKey(Stop s);

// Parse a stopKey back. Returns false for anything unrecognised rather than defaulting, because a
// config file that has drifted must not silently select a different power level than it names.
bool stopFromKey(const std::string &key, Stop &out);

struct Request {
    Stop stop = Stop::Balanced;

    // Desired turbo state. Only acted on when the machine has a turbo knob AND touchTurbo is set;
    // otherwise the plan records a note saying why not.
    bool turbo = true;
    bool touchTurbo = true;
};

// What a stop resolves to on a given machine, with no filesystem access at all: the governor and
// EPP names come out of the Backend's probe, which has already read the kernel's own lists.
//
// This is factored out of buildPlan because the window needs it four times a second -- to answer
// "is the machine in the state this stop asked for?" -- and buildPlan reads the prior value of
// every attribute it plans to write, which is around forty reads on a 20-policy machine. Doing
// that per frame to answer a question with no writes in it would be absurd. Having it in one
// function rather than two is what stops the window's idea of a stop drifting from the plan's.
struct Resolution {
    std::string governor;           // empty when this machine offers none of the stop's choices
    std::string epp;                // empty when no EPP write should be attempted
    std::string eppForcedReason;    // why, when epp is empty but the stop wanted one
    std::vector<std::string> notes; // things the user needs told; already user-facing prose
};

// Never fails. A machine offering no suitable governor yields an empty `governor` and a note.
Resolution resolve(const Backend &backend, Stop s);

// Build the ordered plan. Reads current values through `fs` so that every Write carries the prior
// value it replaces -- which is what makes the plan displayable, diffable, and restorable.
//
// Never throws. A machine the Backend reports as unusable yields a plan with no writes and a note
// explaining why.
WritePlan buildPlan(const Sysfs &fs, const Backend &backend, const Request &request);

// The governor preference list for a stop, most-preferred first. Exposed for tools/coretest so
// the table itself can be asserted, not just its outcome.
std::vector<std::string> governorPreference(Stop s);

// The EPP name a stop wants, or "" for Maximum (where the driver forces it).
const char *eppPreference(Stop s);

} // namespace cpupower
