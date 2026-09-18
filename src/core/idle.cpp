// Idle implementation. See idle.h.

#include "idle.h"

#include "topology.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace cpupower
{
namespace
{

// "cpu" followed by digits, and nothing else. /sys/devices/system/cpu also holds cpufreq,
// cpuidle, power, hotplug and more, none of which are a CPU.
bool parseIndexed(const std::string &name, const char *prefix, int &out)
{
    const size_t n = std::char_traits<char>::length(prefix);
    if (name.compare(0, n, prefix) != 0)
        return false;

    const std::string digits = name.substr(n);
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                       [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return false;

    errno = 0;
    char *end = nullptr;
    const long v = std::strtol(digits.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || v < 0 || v > 65535)
        return false;

    out = static_cast<int>(v);
    return true;
}

// The states one CPU reports, sorted by index.
std::vector<IdleStates::State> statesOf(const Sysfs &fs, int cpu)
{
    std::vector<IdleStates::State> out;
    const std::string dir = std::string(kCpuDir) + "/cpu" + std::to_string(cpu) + "/cpuidle";

    for (const std::string &name : fs.listDirs(dir)) {
        int index = -1;
        if (!parseIndexed(name, "state", index))
            continue;

        const std::string sdir = dir + "/" + name;

        // An exit latency we cannot read is not a state we can reason about. Skipping it is the
        // safe direction: it can only make the resolved limit larger, i.e. admit MORE states,
        // never fewer -- and the alternative, treating it as 0, would silently reintroduce the
        // spin loop this whole file exists to avoid.
        const std::optional<long long> latency = fs.readInt(sdir + "/latency");
        if (!latency || *latency < 0)
            continue;

        IdleStates::State s;
        s.index = index;
        s.name = fs.read(sdir + "/name").value_or("");
        s.latencyUs = *latency;
        s.residencyUs = fs.readInt(sdir + "/residency").value_or(-1);
        out.push_back(std::move(s));
    }

    std::sort(out.begin(), out.end(), [](const IdleStates::State &a, const IdleStates::State &b) {
        return a.index < b.index;
    });
    return out;
}

// The smallest non-zero exit latency in a list, i.e. the shallowest state that actually halts.
std::optional<long long> shallowestHalt(const std::vector<IdleStates::State> &states)
{
    std::optional<long long> best;
    for (const IdleStates::State &s : states) {
        if (s.latencyUs <= 0) // POLL, and anything else that does not halt
            continue;
        if (!best || s.latencyUs < *best)
            best = s.latencyUs;
    }
    return best;
}

} // namespace

//------------------------------------------------------------------------
const IdleStates::State *IdleStates::admitted() const
{
    if (!limitUs)
        return nullptr;

    // The deepest state at or under the limit -- which is what the governors are allowed to pick
    // (cpuidle.rst:577-581). Deepest, not shallowest, because that is the one whose cost the user
    // is being asked to accept.
    const State *best = nullptr;
    for (const State &s : states) {
        if (s.latencyUs > *limitUs)
            continue;
        if (!best || s.latencyUs > best->latencyUs)
            best = &s;
    }
    return best;
}

//------------------------------------------------------------------------
std::string IdleStates::unavailableReason() const
{
    if (limitUs)
        return "";
    if (!present)
        return "this kernel exposes no cpuidle states";
    // Every state reported an exit latency of 0, so there is nothing to block and nothing to
    // gain: the machine already wakes instantly. Saying so is better than offering a control
    // that would write a number and change nothing.
    return "every idle state on this machine already wakes in 0 us";
}

//------------------------------------------------------------------------
IdleStates IdleStates::probe(const Sysfs &fs)
{
    IdleStates out;

    std::vector<int> cpus;
    for (const std::string &name : fs.listDirs(kCpuDir)) {
        int index = -1;
        if (parseIndexed(name, "cpu", index))
            cpus.push_back(index);
    }
    std::sort(cpus.begin(), cpus.end());

    // EVERY CPU, NOT JUST CPU 0. The limit is a single global number, so it has to leave every
    // CPU a state it can halt in. On a heterogeneous machine -- P-cores and E-cores, or an ARM
    // big.LITTLE -- the clusters do not have to publish the same table, and a limit taken from
    // cpu0 alone could leave the other cluster spinning. Hence the MAXIMUM over each CPU's
    // shallowest halt state rather than the minimum: the smallest limit under which no CPU is
    // forced into the POLL loop.
    for (int cpu : cpus) {
        const std::vector<State> s = statesOf(fs, cpu);
        if (s.empty())
            continue;

        out.present = true;
        if (out.states.empty())
            out.states = s; // cpu0's table, or the first CPU that has one, for display

        const std::optional<long long> halt = shallowestHalt(s);
        if (!halt)
            continue;
        if (!out.limitUs)
            out.limitUs = halt;
        else if (*halt != *out.limitUs)
            out.heterogeneous = true;
        if (*halt > *out.limitUs)
            out.limitUs = halt;
    }

    return out;
}

} // namespace cpupower
