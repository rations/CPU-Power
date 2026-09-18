// Topology — which cpufreq policies exist on this machine, and which CPUs each one drives.
//
// THE MODEL WORKS IN POLICY SPACE, NOT CPU SPACE. /sys/devices/system/cpu/cpufreq/policyX is
// canonical; the cpufreq directories under cpuY/ are symbolic links into it:
//
//   "That directory contains a ``policyX`` subdirectory (where ``X`` represents an integer
//    number) for every policy object maintained by the ``CPUFreq`` core. Each ``policyX``
//    directory is pointed to by ``cpufreq`` symbolic links under
//    :file:`/sys/devices/system/cpu/cpuY/`"
//        -- Linux Documentation/admin-guide/pm/cpufreq.rst, lines 204-212
//
// Two consequences, and both matter:
//
//   * Correctness — a machine with 20 logical CPUs sharing 20 policies takes 20 writes, not 20
//     redundant ones through symlinks; and a machine where several CPUs share one policy takes
//     one write per policy rather than one per CPU.
//
//   * Security — the privileged helper opens everything with O_NOFOLLOW. That works on the
//     policyX paths because they are real directories, and would NOT work through cpuY/cpufreq,
//     which genuinely are symlinks. Working in policy space is what makes O_NOFOLLOW usable.
//
// Policy indices are NOT contiguous and NOT ordered lexicographically: policy10 sorts before
// policy9 as a string. They are sorted numerically here so that any output that lists them is
// stable and reads the way a person expects.

#pragma once

#include "sysfs.h"

#include <string>
#include <vector>

namespace cpupower
{

struct Policy {
    int index = -1;            // the N in policyN
    std::vector<int> affected; // online CPUs in this policy
    std::vector<int> related;  // online AND offline CPUs in this policy

    // Path of this policy's directory, relative to the sysfs root. Built from the integer index,
    // never from a string that came from outside.
    std::string dir() const;

    // Path of one attribute inside this policy, relative to the sysfs root.
    std::string attr(const std::string &name) const;
};

// Discover the policies. Returns them sorted by index ascending. An empty result means this
// machine has no cpufreq interface at all (a VM with no scaling driver, or a kernel built
// without CPUFreq), which is a state the GUI must report rather than crash on.
std::vector<Policy> discoverPolicies(const Sysfs &fs);

// Directory of the cpufreq core itself, relative to the root — where the generic `boost` knob
// lives, and where the policyN directories are.
inline constexpr const char *kCpuDir = "devices/system/cpu";
inline constexpr const char *kCpufreqDir = "devices/system/cpu/cpufreq";

} // namespace cpupower
