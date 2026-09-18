// Profile implementation. See profile.h.

#include "profile.h"

#include <algorithm>

namespace cpupower
{

namespace
{

// The preference table. See the long comment in profile.h for where each ordering comes from and
// which part of it is a judgement rather than a documented fact.
struct StopSpec {
    const char *label;
    const char *key;
    std::vector<const char *> governors; // most preferred first
    const char *epp;                     // "" means "do not write EPP"
};

const StopSpec &spec(Stop s)
{
    static const StopSpec table[kStopCount] = {
        {"Powersave", "powersave", {"powersave"}, "power"},
        {"Balanced",
         "balanced",
         {"conservative", "ondemand", "schedutil", "powersave"},
         "balance_power"},
        {"Balanced-Performance",
         "balanced-perf",
         {"ondemand", "schedutil", "conservative", "powersave"},
         "balance_performance"},
        {"Performance", "performance", {"schedutil", "ondemand", "powersave"}, "performance"},
        // Maximum deliberately does NOT list a fallback. If `performance` does not exist there is
        // no honest way to express "maximum", and the plan says so rather than silently picking
        // the next best thing and calling it Maximum.
        {"Maximum", "maximum", {"performance"}, ""},
    };
    return table[static_cast<int>(s)];
}

} // namespace

//------------------------------------------------------------------------
const char *stopLabel(Stop s)
{
    return spec(s).label;
}

//------------------------------------------------------------------------
const char *stopKey(Stop s)
{
    return spec(s).key;
}

//------------------------------------------------------------------------
bool stopFromKey(const std::string &key, Stop &out)
{
    for (int i = 0; i < kStopCount; ++i) {
        const Stop s = static_cast<Stop>(i);
        if (key == spec(s).key) {
            out = s;
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------
std::vector<std::string> governorPreference(Stop s)
{
    std::vector<std::string> out;
    for (const char *g : spec(s).governors)
        out.emplace_back(g);
    return out;
}

//------------------------------------------------------------------------
const char *eppPreference(Stop s)
{
    return spec(s).epp;
}

//------------------------------------------------------------------------
Resolution resolve(const Backend &backend, Stop s)
{
    Resolution r;
    const StopSpec &want = spec(s);

    // ---- Resolve the governor -------------------------------------------------------------
    // First name in the preference list that this machine actually offers.
    for (const char *candidate : want.governors) {
        if (backend.supports(candidate)) {
            r.governor = candidate;
            break;
        }
    }

    if (r.governor.empty()) {
        // Only reachable for Maximum on a machine with no `performance` governor, because every
        // other stop ends its list with `powersave`, which every cpufreq machine has.
        std::string names;
        for (const char *g : want.governors) {
            names += names.empty() ? "" : ", ";
            names += g;
        }
        r.notes.push_back(std::string("this machine offers no governor for the \"") + want.label +
                          "\" setting (wanted one of: " + names + ")");
        return r;
    }

    // ---- Resolve the EPP ------------------------------------------------------------------
    //
    // ORDERING RULE 1, and the one that bites hardest. Under the `performance` governor the
    // driver forces EPP to 0 and rejects anything else:
    //
    //   "This will override the EPP/EPB setting coming from the ``sysfs`` interface [...]
    //    Moreover, any attempts to change the EPP/EPB to a value different from 0 ("performance")
    //    via ``sysfs`` in this configuration will be rejected."
    //        -- intel_pstate.rst:130-133
    //
    // Two consequences, both handled here rather than discovered at write time:
    //
    //   * The Maximum stop (governor=performance) must not attempt an EPP write at all. It would
    //     fail with EINVAL, and the tempting response -- ignoring the error -- would leave the
    //     machine on the PREVIOUS stop's EPP with a slider claiming otherwise.
    //
    //   * Every other stop writes its governor FIRST and its EPP SECOND. That ordering is
    //     enforced by buildPlan below, in the sequence in which the writes are appended, and it
    //     is what makes moving from Maximum down to any other stop work: the governor leaves
    //     `performance` before the EPP write is attempted.
    if (!backend.hasEpp()) {
        if (want.epp[0] != '\0')
            r.eppForcedReason = "this machine has no energy_performance_preference";
    } else if (want.epp[0] == '\0') {
        r.eppForcedReason = "forced to \"performance\" by the performance governor";
    } else if (!backend.supportsEpp(want.epp)) {
        r.notes.push_back(std::string("this machine does not offer the \"") + want.epp +
                          "\" energy-performance preference; leaving it unchanged");
    } else {
        r.epp = want.epp;
    }

    return r;
}

//------------------------------------------------------------------------
WritePlan buildPlan(const Sysfs &fs, const Backend &backend, const Request &request)
{
    WritePlan plan;

    if (!backend.usable()) {
        plan.note(backend.unusableReason());
        return plan;
    }

    const StopSpec &want = spec(request.stop);
    const Resolution res = resolve(backend, request.stop);

    for (const std::string &n : res.notes)
        plan.note(n);

    if (res.governor.empty())
        return plan;

    const std::string &governor = res.governor;
    const std::string &epp = res.epp;
    plan.resolvedGovernor = governor;
    plan.resolvedEpp = epp;
    plan.eppForcedReason = res.eppForcedReason;

    // ---- Emit the writes, in order --------------------------------------------------------
    //
    // Turbo first. It changes the frequency envelope the governor then operates within, and on
    // intel_pstate it also moves the per-policy frequency limits:
    //
    //   "setting ``no_turbo`` causes ``scaling_max_freq`` and ``scaling_min_freq`` to go down to
    //    that value if they were above it before. However, the old values [...] will be restored
    //    after unsetting ``no_turbo``, unless these attributes have been written to after
    //    ``no_turbo`` was set."   -- intel_pstate.rst:577-585
    //
    // This tool never writes scaling_min_freq or scaling_max_freq, precisely so that the kernel's
    // own restore behaviour above is left intact: a user who capped their frequency with another
    // tool gets that cap back when turbo is re-enabled, instead of having it silently erased by
    // us. That is also why ordering rule 2 (min/max must not cross) does not appear in this file
    // -- there is no write here for it to constrain. If a frequency cap is ever added, it goes
    // AFTER the turbo write, and rule 2 becomes live.
    if (request.touchTurbo) {
        if (backend.turbo == TurboKnob::None) {
            plan.note("no turbo/boost control on this machine");
        } else {
            // no_turbo is inverted: 1 means turbo OFF. Resolved in exactly one place.
            const std::string value = (request.turbo != backend.turboInverted()) ? "1" : "0";

            const std::string why = std::string(request.turbo ? "enable" : "disable") +
                                    " turbo via " + turboKnobName(backend.turbo) +
                                    (backend.turboInverted() ? " (inverted)" : "");

            if (backend.turboPerPolicy()) {
                for (const Policy &p : backend.policies) {
                    const std::string path = backend.turboPath(p);
                    plan.add(Op::Turbo, p.index, path, value, fs.read(path).value_or(""), why);
                }
            } else {
                const std::string path = backend.turboPath(backend.policies.front());
                plan.add(Op::Turbo, -1, path, value, fs.read(path).value_or(""), why);
            }
            plan.touchesTurbo = true;
            plan.turboIntent = request.turbo;
        }
    }

    // Governor, every policy. Policies are separate objects and setting one does not set the
    // others (cpufreq.rst).
    for (const Policy &p : backend.policies) {
        const std::string path = p.attr("scaling_governor");
        plan.add(Op::Governor, p.index, path, governor, fs.read(path).value_or(""),
                 std::string("governor for the \"") + want.label + "\" setting");
    }

    // EPP, every policy, AFTER the governor. Every policy rather than just the first, because the
    // kernel warns against mixing hints:
    //
    //   "tasks may by migrated from one CPU to another by the scheduler's load-balancing
    //    algorithm and if different energy vs performance hints are set for those CPUs, that may
    //    lead to undesirable outcomes. To avoid such issues it is better to set the same energy
    //    vs performance hint for all CPUs"   -- intel_pstate.rst:694-698
    if (!epp.empty()) {
        for (const Policy &p : backend.policies) {
            const std::string path = p.attr("energy_performance_preference");
            plan.add(Op::Epp, p.index, path, epp, fs.read(path).value_or(""),
                     "energy-performance hint (written after the governor: "
                     "the performance governor rejects EPP writes)");
        }
    }

    return plan;
}

} // namespace cpupower
