// Preferences — the four things worth remembering between sessions.
//
// THIS FILE IS NEVER AUTO-APPLIED. Reading it sets where the slider and the checkboxes sit; it
// does NOT turn the master toggle on, and it does not write a single sysfs attribute. The whole
// safety property of the master toggle is that OFF MEANS THE MACHINE IS EXACTLY AS THE KERNEL
// LEFT IT, and a config file that quietly reapplied a setting at login would destroy it -- the
// tool would be changing the machine before the user had touched anything.
//
// Applying a stop at boot is a legitimate thing to want, and it has an answer that is honest
// about what it is: one `cpu-power-cli --stop <key> --hold` line in an init script. That is
// visible in the place system configuration belongs, rather than hidden in a per-user dotfile.
//
// THE FILE IS UNTRUSTED INPUT. It is unprivileged and user-writable, so it is parsed the way any
// other outside input is: bounded, key/value, unknown keys ignored, and every value validated
// before it is used. A stop name goes through stopFromKey(), which REJECTS anything it does not
// recognise rather than defaulting -- a config that has drifted must not silently select a
// different power level than the one it names. Nothing read here is ever handed to the helper:
// the app rebuilds the plan from live sysfs, so the file selects a stop, it does not describe
// one.

#pragma once

#include "core/profile.h"

#include <string>

namespace cpupower
{

struct Config {
    Stop stop = Stop::BalancedPerf;
    bool turbo = true;
    bool daw = false;
    float scale = 1.0f;
};

// $XDG_CONFIG_HOME/cpu-power/config, or $HOME/.config/cpu-power/config. Empty if neither
// variable is set, which is not an error -- it means preferences are not persisted this session.
std::string configPath();

// Read it. A missing file, an unreadable one and an empty one are all the same thing: defaults.
// A malformed line is skipped and the rest of the file is still read, because losing every
// preference over one bad line is a worse outcome than losing one.
Config loadConfig();

// Write it, creating the directory if needed. Returns false having warned on stderr; a
// preference that could not be saved is not worth interrupting the user for.
bool saveConfig(const Config &c);

} // namespace cpupower
