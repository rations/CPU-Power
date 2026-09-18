// WritePlan implementation. See plan.h.

#include "plan.h"

namespace cpupower
{

//------------------------------------------------------------------------
size_t WritePlan::effectiveCount() const
{
    size_t n = 0;
    for (const Write &w : writes) {
        if (!w.redundant())
            ++n;
    }
    return n;
}

//------------------------------------------------------------------------
std::vector<std::string> WritePlan::toProtocol() const
{
    std::vector<std::string> out;
    bool turboEmitted = false;

    for (const Write &w : writes) {
        switch (w.op) {
            case Op::Turbo:
                if (!turboEmitted) {
                    out.push_back(std::string("SET turbo ") + (turboIntent ? "1" : "0"));
                    turboEmitted = true;
                }
                break;
            case Op::Governor:
                out.push_back("SET gov " + std::to_string(w.policy) + " " + w.value);
                break;
            case Op::Epp:
                out.push_back("SET epp " + std::to_string(w.policy) + " " + w.value);
                break;
        }
    }
    if (!out.empty())
        out.push_back("COMMIT");
    return out;
}

//------------------------------------------------------------------------
std::string WritePlan::dump() const
{
    std::string out;

    out += "governor: " + (resolvedGovernor.empty() ? std::string("(none)") : resolvedGovernor);
    out += "\n";
    out += "epp:      " + (resolvedEpp.empty() ? std::string("(none)") : resolvedEpp);
    if (!eppForcedReason.empty())
        out += "  [" + eppForcedReason + "]";
    out += "\n";

    out += "writes:   " + std::to_string(writes.size()) + " (" + std::to_string(effectiveCount()) +
           " effective)\n";

    for (const Write &w : writes) {
        out += "  ";
        // A leading marker makes a golden-file diff readable at a glance: '=' is already-correct,
        // '>' actually changes the machine.
        out += w.redundant() ? "= " : "> ";
        out += w.path;
        out += " <- \"" + w.value + "\"";
        if (!w.prior.empty())
            out += " (was \"" + w.prior + "\")";
        out += "  # " + w.why;
        out += "\n";
    }

    for (const Note &n : notes)
        out += "  ! " + n.text + "\n";

    return out;
}

} // namespace cpupower
