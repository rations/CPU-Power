// Backend implementation. See backend.h.

#include "backend.h"

#include <algorithm>
#include <cstdio>

namespace cpupower
{

namespace
{

//------------------------------------------------------------------------
PstateMode parseMode(const std::string &text)
{
    if (text == "active")
        return PstateMode::Active;
    if (text == "passive")
        return PstateMode::Passive;
    if (text == "guided")
        return PstateMode::Guided;
    if (text == "off" || text == "disable")
        return PstateMode::Disabled;
    return PstateMode::Unknown;
}

//------------------------------------------------------------------------
// Format kHz as GHz with two decimals, for display only. Never used to build a value we write.
std::string ghz(long long khz)
{
    if (khz <= 0)
        return "?";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(khz) / 1000000.0);
    return buf;
}

} // namespace

//------------------------------------------------------------------------
const char *turboKnobName(TurboKnob k)
{
    switch (k) {
        case TurboKnob::None:
            return "none";
        case TurboKnob::IntelNoTurbo:
            return "intel_pstate/no_turbo";
        case TurboKnob::GlobalBoost:
            return "cpufreq/boost";
        case TurboKnob::PolicyBoost:
            return "policy/boost";
        case TurboKnob::PolicyCpb:
            return "policy/cpb";
    }
    return "none";
}

//------------------------------------------------------------------------
const char *pstateModeName(PstateMode m)
{
    switch (m) {
        case PstateMode::NotApplicable:
            return "";
        case PstateMode::Active:
            return "active";
        case PstateMode::Passive:
            return "passive";
        case PstateMode::Guided:
            return "guided";
        case PstateMode::Disabled:
            return "disabled";
        case PstateMode::Unknown:
            return "unknown";
    }
    return "";
}

//------------------------------------------------------------------------
Backend Backend::probe(const Sysfs &fs)
{
    Backend b;

    b.policies = discoverPolicies(fs);
    if (b.policies.empty())
        return b;

    const Policy &first = b.policies.front();

    b.driver = fs.read(first.attr("scaling_driver")).value_or("");

    // Governors and EPP choices are read from the FIRST policy and assumed uniform across the
    // machine. That assumption is checked below rather than trusted, because a heterogeneous
    // machine (big.LITTLE, or Intel P-cores and E-cores under some drivers) can genuinely differ
    // per policy, and silently applying one policy's vocabulary to another would produce writes
    // the kernel rejects.
    b.governors = fs.readList(first.attr("scaling_available_governors"));
    b.eppChoices = fs.readList(first.attr("energy_performance_available_preferences"));

    for (const Policy &p : b.policies) {
        const std::vector<std::string> govs = fs.readList(p.attr("scaling_available_governors"));
        if (!govs.empty() && govs != b.governors) {
            // Keep only what every policy offers. A governor we cannot set everywhere is not one
            // this tool will offer, because a stop that applies to half the CPUs is worse than a
            // stop that is not there.
            std::vector<std::string> common;
            for (const std::string &g : b.governors) {
                if (std::find(govs.begin(), govs.end(), g) != govs.end())
                    common.push_back(g);
            }
            b.governors = std::move(common);
        }

        const std::vector<std::string> epps =
            fs.readList(p.attr("energy_performance_available_preferences"));
        if (epps.empty()) {
            // One policy without EPP means EPP is not something we can set uniformly, and the
            // kernel warns that mixed hints across CPUs "may lead to undesirable outcomes"
            // (intel_pstate.rst:694-698). So: all policies, or none.
            b.eppChoices.clear();
        } else if (!b.eppChoices.empty() && epps != b.eppChoices) {
            std::vector<std::string> common;
            for (const std::string &e : b.eppChoices) {
                if (std::find(epps.begin(), epps.end(), e) != epps.end())
                    common.push_back(e);
            }
            b.eppChoices = std::move(common);
        }
    }

    // Driver operating mode. intel_pstate and amd_pstate expose this under different directory
    // names, and neither is named after the driver string, so both are probed.
    if (const std::optional<std::string> s = fs.read("devices/system/cpu/intel_pstate/status")) {
        b.modeText = *s;
        b.mode = parseMode(*s);
    } else if (const std::optional<std::string> s2 =
                   fs.read("devices/system/cpu/amd_pstate/status")) {
        b.modeText = *s2;
        b.mode = parseMode(*s2);
    }

    // Turbo, in the documented order of preference. See the comment block in backend.h.
    if (fs.exists("devices/system/cpu/intel_pstate/no_turbo"))
        b.turbo = TurboKnob::IntelNoTurbo;
    else if (fs.exists("devices/system/cpu/cpufreq/boost"))
        b.turbo = TurboKnob::GlobalBoost;
    else if (fs.exists(first.attr("boost")))
        b.turbo = TurboKnob::PolicyBoost;
    else if (fs.exists(first.attr("cpb")))
        b.turbo = TurboKnob::PolicyCpb;
    else
        b.turbo = TurboKnob::None;

    // Both must be present to be usable as a pair, and both are absent when the kernel was booted
    // with intel_pstate=per_cpu_perf_limits (intel_pstate.rst:657-659).
    b.hasPerfPct = fs.exists("devices/system/cpu/intel_pstate/min_perf_pct") &&
                   fs.exists("devices/system/cpu/intel_pstate/max_perf_pct");

    b.cpuinfoMinKhz = fs.readInt(first.attr("cpuinfo_min_freq")).value_or(0);
    b.cpuinfoMaxKhz = fs.readInt(first.attr("cpuinfo_max_freq")).value_or(0);

    return b;
}

//------------------------------------------------------------------------
bool Backend::usable() const
{
    return !policies.empty() && !governors.empty();
}

//------------------------------------------------------------------------
std::string Backend::unusableReason() const
{
    if (policies.empty())
        return "no CPU frequency scaling interface on this machine "
               "(/sys/devices/system/cpu/cpufreq is empty or absent)";
    if (governors.empty())
        return "the scaling driver \"" + driver + "\" offers no governors this tool can set";
    return "";
}

//------------------------------------------------------------------------
bool Backend::supports(const std::string &governor) const
{
    return std::find(governors.begin(), governors.end(), governor) != governors.end();
}

//------------------------------------------------------------------------
bool Backend::supportsEpp(const std::string &preference) const
{
    return std::find(eppChoices.begin(), eppChoices.end(), preference) != eppChoices.end();
}

//------------------------------------------------------------------------
std::string Backend::turboPath(const Policy &p) const
{
    switch (turbo) {
        case TurboKnob::None:
            return "";
        case TurboKnob::IntelNoTurbo:
            return "devices/system/cpu/intel_pstate/no_turbo";
        case TurboKnob::GlobalBoost:
            return "devices/system/cpu/cpufreq/boost";
        case TurboKnob::PolicyBoost:
            return p.attr("boost");
        case TurboKnob::PolicyCpb:
            return p.attr("cpb");
    }
    return "";
}

//------------------------------------------------------------------------
std::string Backend::describe() const
{
    std::string s = driver.empty() ? "unknown driver" : driver;

    if (mode != PstateMode::NotApplicable && mode != PstateMode::Unknown) {
        s += " (";
        s += pstateModeName(mode);
        if (hasEpp())
            s += ", HWP";
        s += ")";
    }

    s += "  " + std::to_string(policies.size());
    s += policies.size() == 1 ? " policy" : " policies";

    if (cpuinfoMinKhz > 0 && cpuinfoMaxKhz > 0)
        s += "  " + ghz(cpuinfoMinKhz) + "-" + ghz(cpuinfoMaxKhz) + " GHz";

    return s;
}

} // namespace cpupower
