// Topology implementation. See topology.h.

#include "topology.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace cpupower
{

//------------------------------------------------------------------------
std::string Policy::dir() const
{
    return std::string(kCpufreqDir) + "/policy" + std::to_string(index);
}

//------------------------------------------------------------------------
std::string Policy::attr(const std::string &name) const
{
    return dir() + "/" + name;
}

//------------------------------------------------------------------------
std::vector<Policy> discoverPolicies(const Sysfs &fs)
{
    std::vector<Policy> out;

    for (const std::string &name : fs.listDirs(kCpufreqDir)) {
        // Only "policy" followed by digits. Anything else in this directory is not ours to
        // interpret: the cpufreq core also puts plain attribute files here (boost), and a future
        // kernel may add more.
        if (name.compare(0, 6, "policy") != 0)
            continue;

        const std::string digits = name.substr(6);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                           [](unsigned char c) { return c >= '0' && c <= '9'; }))
            continue;

        errno = 0;
        char *end = nullptr;
        const long index = std::strtol(digits.c_str(), &end, 10);
        if (errno != 0 || *end != '\0' || index < 0 || index > 65535)
            continue;

        Policy p;
        p.index = static_cast<int>(index);

        // A policy with no scaling_driver is not a policy we can act on. Checking here rather
        // than at use time means the rest of the model never has to hold a half-formed one.
        if (!fs.exists(p.attr("scaling_driver")))
            continue;

        p.affected = parseCpuList(fs.read(p.attr("affected_cpus")).value_or(""));
        p.related = parseCpuList(fs.read(p.attr("related_cpus")).value_or(""));

        out.push_back(std::move(p));
    }

    // Numerically, not lexicographically. listDirs sorts as strings, where policy10 precedes
    // policy9, which would make every listing this feeds look scrambled.
    std::sort(out.begin(), out.end(),
              [](const Policy &a, const Policy &b) { return a.index < b.index; });

    return out;
}

} // namespace cpupower
