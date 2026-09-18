// WritePlan — an ordered list of sysfs writes, and the reason for each.
//
// THE ORDER IS PART OF THE ARTEFACT. Three of the kernel's constraints are ordering constraints,
// not value constraints, so a plan that contains the right writes in the wrong order is a plan
// that silently half-applies. tools/coretest asserts the exact ordered dump of every plan against
// a golden file for exactly this reason: a changed order is a changed behaviour, and it must be
// explained rather than absorbed.
//
// Nothing here writes anything. A WritePlan is a value: it is built by the unprivileged model,
// displayed, diffed and tested offline, and only then handed to the privileged helper to execute.
// That separation is what lets the whole decision table be exercised on one machine against
// captured sysfs trees from others.

#pragma once

#include <string>
#include <vector>

namespace cpupower
{

// What KIND of write this is. The privileged helper never accepts a path -- it accepts a verb
// plus a policy index and builds the path itself from compiled-in literals. Recording the op here
// is what makes translating a plan into the helper's protocol exact and total, rather than a
// best-effort reverse-engineering of the path string.
enum class Op {
    Governor, // policyN/scaling_governor
    Epp,      // policyN/energy_performance_preference
    Turbo,    // whichever turbo knob this machine has; `value` is the RAW value for that knob
};

struct Write {
    Op op = Op::Governor;
    int policy = -1;   // policy index, or -1 for a machine-global knob
    std::string path;  // relative to the sysfs root, e.g. ".../policy0/scaling_governor"
    std::string value; // exactly the bytes to write, no trailing newline
    std::string prior; // what was there when the plan was built; empty if unreadable
    std::string why;   // short rationale, shown in the plan dump and the golden file

    // True when the machine is already in this state, so executing it is a no-op. Kept in the
    // plan rather than filtered out, because "already correct" and "not attempted" are different
    // answers and the display must be able to tell them apart.
    bool redundant() const
    {
        return !prior.empty() && prior == value;
    }
};

// Something the plan could NOT do, and why. These are surfaced in the window rather than swallowed:
// a control that appears to work and does nothing is worse than one that is greyed out
//.
struct Note {
    std::string text;
};

struct WritePlan {
    std::vector<Write> writes;
    std::vector<Note> notes;

    // What the requested stop actually resolved to on THIS machine. The window displays these, so
    // that a stop which collapses into its neighbour on some hardware is visible rather than a
    // silent no-op -- the GUI shows what the kernel reports, not what was requested
    //.
    std::string resolvedGovernor; // empty if no governor could be chosen
    std::string resolvedEpp;      // empty if this machine has no EPP, or the driver forces it
    std::string eppForcedReason;  // non-empty when EPP was deliberately not written

    // Intent for the turbo knob (true == turbo enabled), independent of whether the machine's
    // knob is inverted. The helper owns the inversion, so the protocol carries intent and the raw
    // value never crosses the boundary. Unset when the plan touches no turbo knob.
    bool turboIntent = false;
    bool touchesTurbo = false;

    void add(Op op, int policy, std::string path, std::string value, std::string prior,
             std::string why)
    {
        writes.push_back(
            {op, policy, std::move(path), std::move(value), std::move(prior), std::move(why)});
    }

    void note(std::string text)
    {
        notes.push_back({std::move(text)});
    }

    bool empty() const
    {
        return writes.empty();
    }

    // Number of writes that would actually change something.
    size_t effectiveCount() const;

    // Translate this plan into the privileged helper's protocol.
    //
    // THIS IS THE PRIVILEGE BOUNDARY, so it lives here and exactly here -- the GUI and
    // tools/cpu-power-cli both call it, and a second copy of it would be a second place for the
    // rules below to stop being true.
    //
    //   * NO PATH CROSSES. A Write carries an Op and a policy index; the helper rebuilds the path
    //     itself from compiled-in literals. What crosses is a verb, an integer, and a name the
    //     kernel has already vouched for.
    //
    //   * TURBO CROSSES AS INTENT, NOT AS A RAW VALUE. The plan may hold one turbo write per
    //     policy; they collapse into a single "SET turbo <0|1>", because which of four knobs this
    //     machine has and whether it is inverted is the helper's business. Re-encoding the
    //     inversion here would mean two places that have to agree about it.
    //
    // Returns an empty vector for a plan with no writes -- and therefore no COMMIT, because
    // committing nothing is not the same as having nothing to commit.
    std::vector<std::string> toProtocol() const;

    // Stable, human-readable, diffable rendering. This is what the golden files hold, so its
    // format is a tested interface: changing it changes every golden file, which is a deliberate
    // act rather than an accident.
    std::string dump() const;
};

} // namespace cpupower
