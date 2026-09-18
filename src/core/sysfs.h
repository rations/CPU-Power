// Sysfs — every filesystem access the model makes, in one place.
//
// Two properties this file exists to guarantee:
//
//   1. EVERY path is built from a configurable ROOT plus fixed segments plus validated integers.
//      No caller hands in a path from outside, and nothing here concatenates untrusted input into
//      one. The privileged helper enforces the same rule with a held dirfd; this is the
//      unprivileged model's half of it.
//
//   2. The root is CONFIGURABLE, which is what makes "distro-agnostic" testable. The developer has
//      one machine and this tool claims to work on all of them; pointing the root at a captured
//      sysfs tree (tests/fixtures/) is how the other shapes get exercised.
//
// Note that an attribute may EXIST and still not hold a number: on intel_pstate,
// scaling_setspeed reads the literal string "<unsupported>". readInt therefore fails cleanly on
// anything that is not a complete integer, and callers must distinguish "absent" from
// "unparseable" rather than treating both as zero.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cpupower
{

// Largest attribute we will read. sysfs attributes are conventionally one page; the longest one
// this tool reads is scaling_available_frequencies, which is comfortably inside that.
inline constexpr size_t kMaxAttr = 4096;

class Sysfs
{
public:
    // root defaults to "/sys". A fixture directory stands in for it wholesale: the tree beneath
    // is identical, so nothing else in the model changes.
    explicit Sysfs(std::string root = "/sys");

    const std::string &root() const
    {
        return mRoot;
    }

    // All paths below are relative to the root, e.g.
    // "devices/system/cpu/cpufreq/policy0/scaling_governor".

    // Whole contents with the single trailing newline removed. nullopt if the file is absent,
    // unreadable, or larger than kMaxAttr.
    std::optional<std::string> read(const std::string &rel) const;

    // A complete integer, or nullopt. "<unsupported>", "" and "12x" all yield nullopt.
    std::optional<long long> readInt(const std::string &rel) const;

    // Whitespace-separated tokens. Empty vector if absent. This is how the kernel formats
    // scaling_available_governors and energy_performance_available_preferences, both of which are
    // space-separated and carry a trailing space.
    std::vector<std::string> readList(const std::string &rel) const;

    // True if the path exists and is a readable regular file. This is the ONLY capability probe
    // the model uses: a knob that is not here does not exist on this machine.
    bool exists(const std::string &rel) const;

    // Immediate subdirectory names of rel, sorted. Empty if rel is not a directory.
    std::vector<std::string> listDirs(const std::string &rel) const;

private:
    std::string mRoot;

    std::string absolute(const std::string &rel) const;
};

// Parse a kernel CPU list ("0-19", "0,2-4,7", "") into ascending indices. Returns an empty vector
// for an empty or malformed list rather than throwing: affected_cpus on an offline policy is
// legitimately empty, and a malformed one is a kernel we do not understand, which must degrade
// rather than abort.
std::vector<int> parseCpuList(const std::string &list);

} // namespace cpupower
