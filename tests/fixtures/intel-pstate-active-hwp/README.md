# Fixture: intel-pstate-active-hwp

The development machine. `intel_pstate` in **active** mode with **HWP**: only `performance` and `powersave` governors exist, so the five slider stops are separated by **EPP**, not by governor. Turbo is `intel_pstate/no_turbo` (inverted); the generic `cpufreq/boost` knob is absent.

**REAL** — captured from a running machine by `scripts/capture-sysfs.sh`.

## Machine

```
captured    2026-09-07 05:36:10Z
distro      Devuan GNU/Linux 6 (excalibur)
kernel      Linux 6.12.107+deb13-amd64 x86_64
cpu         12th Gen Intel(R) Core(TM) i7-12700F
policies    20
driver      intel_pstate
governors   performance powersave
governor    powersave
epp avail   default performance balance_performance balance_power power 
pstate mode active
boost knob  intel_pstate/no_turbo
```

## Files

```
devices/system/cpu/cpu0/cpuidle/state0/desc
devices/system/cpu/cpu0/cpuidle/state0/disable
devices/system/cpu/cpu0/cpuidle/state0/latency
devices/system/cpu/cpu0/cpuidle/state0/name
devices/system/cpu/cpu0/cpuidle/state0/residency
devices/system/cpu/cpu0/cpuidle/state1/desc
devices/system/cpu/cpu0/cpuidle/state1/disable
devices/system/cpu/cpu0/cpuidle/state1/latency
devices/system/cpu/cpu0/cpuidle/state1/name
devices/system/cpu/cpu0/cpuidle/state1/residency
devices/system/cpu/cpu0/cpuidle/state2/desc
devices/system/cpu/cpu0/cpuidle/state2/disable
devices/system/cpu/cpu0/cpuidle/state2/latency
devices/system/cpu/cpu0/cpuidle/state2/name
devices/system/cpu/cpu0/cpuidle/state2/residency
devices/system/cpu/cpu0/cpuidle/state3/desc
devices/system/cpu/cpu0/cpuidle/state3/disable
devices/system/cpu/cpu0/cpuidle/state3/latency
devices/system/cpu/cpu0/cpuidle/state3/name
devices/system/cpu/cpu0/cpuidle/state3/residency
devices/system/cpu/cpu0/cpuidle/state4/desc
devices/system/cpu/cpu0/cpuidle/state4/disable
devices/system/cpu/cpu0/cpuidle/state4/latency
devices/system/cpu/cpu0/cpuidle/state4/name
devices/system/cpu/cpu0/cpuidle/state4/residency
devices/system/cpu/cpufreq/policy0/affected_cpus
devices/system/cpu/cpufreq/policy0/base_frequency
devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq
devices/system/cpu/cpufreq/policy0/cpuinfo_min_freq
devices/system/cpu/cpufreq/policy0/cpuinfo_transition_latency
devices/system/cpu/cpufreq/policy0/energy_performance_available_preferences
devices/system/cpu/cpufreq/policy0/energy_performance_preference
devices/system/cpu/cpufreq/policy0/related_cpus
devices/system/cpu/cpufreq/policy0/scaling_available_governors
devices/system/cpu/cpufreq/policy0/scaling_cur_freq
devices/system/cpu/cpufreq/policy0/scaling_driver
devices/system/cpu/cpufreq/policy0/scaling_governor
devices/system/cpu/cpufreq/policy0/scaling_max_freq
devices/system/cpu/cpufreq/policy0/scaling_min_freq
devices/system/cpu/cpufreq/policy0/scaling_setspeed
... and 294 more (334 files total)
```
