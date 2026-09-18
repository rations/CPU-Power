// cpu-power-cli — headless driver for the privileged helper.
//
// It is the thing the gates drive, and it is also a perfectly good way to use this tool from a
// script or an init file. It does exactly what the GUI does, minus the window: probe the machine,
// build a WritePlan for a slider stop, translate that plan into the helper's protocol, and apply
// it.
//
// The translation from a WritePlan to the helper's protocol is WritePlan::toProtocol(), in
// src/core/plan.h -- shared with the GUI rather than written twice, because it is the privilege
// boundary and two copies of it would be two places for it to stop being true.

#include "../src/core/backend.h"
#include "../src/core/idle.h"
#include "../src/core/profile.h"
#include "../src/core/sysfs.h"
#include "../src/platform/spawn.h"

#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace cpupower;

namespace
{

void usage()
{
    std::cerr <<
        R"(usage: cpu-power-cli [options]

  --show                     report the machine's current state and exit
  --stop <key>               apply a stop: powersave | balanced | balanced-perf
                             | performance | maximum
  --turbo on|off             set turbo along with the stop
  --daw on|off               hold /dev/cpu_dma_latency at this machine's own
                             shallowest halting C-state latency while this runs,
                             so deep states are blocked but the cores still idle
  --hold                     stay running until interrupted (the settings are
                             reverted when this exits -- that is the point)
  --plan                     print the plan and the protocol, apply nothing
  --session                  read commands from stdin and run them all against
                             ONE helper session, which is also the only way to
                             keep ONE pre-state across several commands.
                             Commands, one per line:
                                 stop <key> <on|off>   apply a stop and turbo
                                 daw <on|off>          hold/release the latency fd
                                 snapshot              report the current state
                                 quit
                             Each is answered with a line beginning "DONE ",
                             echoing the command, so a script can synchronise.

  --fuzz                     drive hostile input at the helper and check it is refused

testing only (never used by the GUI):
  --helper <path>            run this helper binary directly, unprivileged
  --sysroot <dir>            point it at a captured sysfs tree
)";
}

// Apply one stop, in an already-running session. Shared by --stop and --session so the two
// cannot come to mean different things.
int applyStop(HelperSession &s, const Sysfs &fs, const Backend &backend, Stop stop, bool turbo,
              bool touchTurbo, std::string &detail)
{
    Request req;
    req.stop = stop;
    req.turbo = turbo;
    req.touchTurbo = touchTurbo;

    const WritePlan plan = buildPlan(fs, backend, req);
    for (const Note &n : plan.notes)
        std::cout << "note: " << n.text << "\n";

    for (const std::string &cmd : plan.toProtocol()) {
        std::vector<std::string> lines;
        if (!s.exchange(cmd, lines, 10000)) {
            detail = cmd + " -> " + (lines.empty() ? "no response" : lines.back());
            return 1;
        }
        if (cmd == "COMMIT" && !lines.empty())
            detail = lines.back();
    }
    return 0;
}

// Read commands from stdin and run them against ONE helper session.
//
// THIS EXISTS BECAUSE ONE SESSION IS ONE PRE-STATE. The helper snapshots every attribute it is
// about to touch when it starts, and puts those values back when its input closes. Run three
// commands as three sessions and the second one snapshots what the first one left behind, so
// "restore" restores to the middle of the sequence rather than to where the machine began.
// Restoring correctly is the tool's whole safety property, so the batching is not a convenience.
//
// It used to be justified as one password instead of several. That argument has expired twice
// over: the polkit path asks for nothing, and the sudo fallback it referred to has been deleted.
// The pre-state argument is what carries the design now, and it is the stronger one anyway --
// it is about correctness rather than convenience.
int runScriptedSession(HelperSession &s, const Sysfs &fs, const Backend &backend,
                       const IdleStates &idle, const std::string &latencyOn)
{
    int rc = 0;
    std::string line;

    while (std::getline(std::cin, line)) {
        // Echoed back on the DONE line so a driving script can wait for the answer to the
        // command it actually sent, rather than for "some answer".
        const std::string echoed = line;

        std::vector<std::string> tok;
        {
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && line[i] == ' ')
                    ++i;
                const size_t a = i;
                while (i < line.size() && line[i] != ' ')
                    ++i;
                if (i > a)
                    tok.push_back(line.substr(a, i - a));
            }
        }
        if (tok.empty())
            continue;

        std::string status = "ok";

        if (tok[0] == "quit")
            break;

        else if (tok[0] == "stop" && tok.size() >= 2) {
            Stop stop;
            if (!stopFromKey(tok[1], stop)) {
                status = "unknown stop \"" + tok[1] + "\"";
                rc = 1;
            } else {
                const bool touchTurbo = tok.size() >= 3;
                const bool turbo = touchTurbo && tok[2] == "on";
                std::string detail;
                if (applyStop(s, fs, backend, stop, turbo, touchTurbo, detail) != 0) {
                    status = "FAILED " + detail;
                    rc = 1;
                } else {
                    status = detail;
                }
            }
        }

        else if (tok[0] == "daw" && tok.size() >= 2) {
            const bool on = (tok[1] == "on");
            if (on && latencyOn.empty()) {
                status = "FAILED " + idle.unavailableReason();
                rc = 1;
            } else {
                std::vector<std::string> lines;
                if (!s.exchange(on ? latencyOn : "LATENCY OFF", lines, 5000)) {
                    status =
                        "FAILED " + (lines.empty() ? std::string("no response") : lines.back());
                    rc = 1;
                }
            }
        }

        else if (tok[0] == "snapshot") {
            std::vector<std::string> lines;
            s.exchange("SNAPSHOT", lines, 5000);
            for (const std::string &l : lines) {
                if (l.compare(0, 2, "OK") != 0)
                    std::cout << l << "\n";
            }
        }

        else {
            status = "unknown command";
            rc = 1;
        }

        std::cout << "DONE " << echoed << " -- " << status << "\n";
    }

    return rc;
}

int runFuzz(HelperSession &s)
{
    // Every one of these must be refused. The interesting ones are not the malformed strings but
    // the plausible ones: a path that would escape the policy directory, an integer that
    // overflows, a governor that is real but not offered by this machine.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"SET gov ../../../etc/passwd performance", "path as a policy index"},
        {"SET gov 0 ../../../../etc/passwd", "path as a governor name"},
        {"SET gov 0 /etc/passwd", "absolute path as a governor name"},
        {"SET gov 0 perf/ormance", "slash in a name"},
        {"SET gov 0 ..", "dot-dot as a name"},
        {"SET gov -1 performance", "negative policy index"},
        {"SET gov 99999 performance", "out-of-range policy index"},
        {"SET gov 9223372036854775808 performance", "policy index overflows long"},
        {"SET gov 0x0 performance", "hex policy index"},
        {"SET gov 00 performance", "leading-zero policy index"},
        {"SET gov +0 performance", "leading-plus policy index"},
        {"SET gov 0 ondemand", "governor not offered by this machine"},
        {"SET gov 0 POWERSAVE", "wrong case"},
        {"SET gov 0 ", "empty name"},
        {"SET gov 0", "missing name"},
        {"SET gov", "missing everything"},
        {"SET epp 0 turbo_max", "EPP not offered by this machine"},
        {"SET epp 0 performance extra", "trailing junk"},
        {"SET turbo 2", "turbo out of range"},
        {"SET turbo -1", "negative turbo"},
        {"SET turbo yes", "non-numeric turbo"},
        {"SET nonsense 0 x", "unknown SET target"},
        {"NONSENSE", "unknown verb"},
        {"COMMIT extra", "COMMIT is fine with trailing args"},
        {"LATENCY -1", "negative latency"},
        {"LATENCY 99999999", "latency out of range"},
        {"LATENCY", "latency with no argument"},
    };

    int failures = 0;
    int refused = 0;

    for (const auto &[cmd, why] : cases) {
        std::vector<std::string> lines;
        const bool ok = s.exchange(cmd, lines, 5000);
        if (lines.empty()) {
            std::cout << "  FAIL  no response to: " << cmd << "  (" << why << ")\n";
            ++failures;
            continue;
        }

        // "COMMIT extra" is the one case that is legitimately accepted -- extra tokens after a
        // no-argument verb are ignored, and there is nothing hostile about that.
        const bool expectRefusal = (cmd != "COMMIT extra");

        if (expectRefusal && ok) {
            std::cout << "  FAIL  ACCEPTED hostile input: \"" << cmd << "\"  (" << why << ")\n";
            ++failures;
        } else if (expectRefusal) {
            ++refused;
        }
    }

    // An over-long line must end the session rather than be truncated and acted on.
    {
        std::string longLine = "SET gov 0 ";
        longLine.append(4096, 'a');
        std::vector<std::string> lines;
        s.exchange(longLine, lines, 5000);
        const bool refusedLong = !lines.empty() && lines.back().compare(0, 3, "ERR") == 0;
        if (refusedLong)
            ++refused;
        else {
            std::cout << "  FAIL  an over-long line was not refused\n";
            ++failures;
        }
    }

    std::cout << "  " << refused << " hostile inputs refused, " << failures << " failed\n";
    return failures;
}

} // namespace

//------------------------------------------------------------------------
int main(int argc, char **argv)
{
    std::string stopKeyArg, helperOverride, sysroot;
    bool show = false, planOnly = false, fuzz = false, hold = false, scripted = false;
    bool turbo = true, touchTurbo = false;
    bool daw = false, touchDaw = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "cpu-power-cli: " << what << " needs a value\n";
                std::exit(64);
            }
            return argv[++i];
        };
        if (a == "--show")
            show = true;
        else if (a == "--plan")
            planOnly = true;
        else if (a == "--fuzz")
            fuzz = true;
        else if (a == "--hold")
            hold = true;
        else if (a == "--session")
            scripted = true;
        else if (a == "--stop")
            stopKeyArg = next("--stop");
        else if (a == "--turbo") {
            turbo = (next("--turbo") == "on");
            touchTurbo = true;
        } else if (a == "--daw") {
            daw = (next("--daw") == "on");
            touchDaw = true;
        } else if (a == "--helper")
            helperOverride = next("--helper");
        else if (a == "--sysroot")
            sysroot = next("--sysroot");
        else {
            usage();
            return 64;
        }
    }

    if (!show && !fuzz && !scripted && stopKeyArg.empty() && !touchDaw) {
        usage();
        return 64;
    }

    const Sysfs fs(sysroot.empty() ? "/sys" : sysroot);
    const Backend backend = Backend::probe(fs);

    // cpuidle, probed from the same root. The value DAW mode requests is this machine's
    // shallowest state that actually halts -- never a hardcoded 0, which admits only the POLL
    // pseudo-state and leaves every core spinning. See src/core/idle.h.
    const IdleStates idle = IdleStates::probe(fs);
    const std::string latencyOn =
        idle.limitUs ? "LATENCY " + std::to_string(*idle.limitUs) : std::string();

    if (!backend.usable() && !fuzz) {
        std::cerr << "cpu-power-cli: " << backend.unusableReason() << "\n";
        return 1;
    }

    // --plan needs no privilege at all: it is the model talking, and the model cannot write.
    if (planOnly) {
        Stop stop = Stop::Balanced;
        if (!stopKeyArg.empty() && !stopFromKey(stopKeyArg, stop)) {
            std::cerr << "cpu-power-cli: unknown stop \"" << stopKeyArg << "\"\n";
            return 64;
        }
        Request req;
        req.stop = stop;
        req.turbo = turbo;
        req.touchTurbo = touchTurbo;
        const WritePlan plan = buildPlan(fs, backend, req);
        std::cout << plan.dump() << "\nprotocol:\n";
        for (const std::string &c : plan.toProtocol())
            std::cout << "  " << c << "\n";

        // DAW mode appears here even though it contributes no sysfs write, because it is applied
        // over the same session and a plan that omitted it would be an incomplete account of
        // what the tool is about to do. What it resolved to is the interesting part.
        if (touchDaw) {
            std::cout << "  " << (daw && !latencyOn.empty() ? latencyOn : "LATENCY OFF") << "\n";
            if (daw) {
                if (latencyOn.empty()) {
                    std::cout << "\nidle: DAW mode unavailable: " << idle.unavailableReason()
                              << "\n";
                } else {
                    std::cout << "\nidle: ";
                    for (const IdleStates::State &st : idle.states)
                        std::cout << st.name << " " << st.latencyUs << "us  ";
                    const IdleStates::State *a = idle.admitted();
                    std::cout << "\n      limit " << *idle.limitUs << "us -> deepest state left "
                              << (a && !a->name.empty() ? a->name : "(none)") << "\n";
                    if (idle.heterogeneous)
                        std::cout << "      (the table above is cpu0's; the CPUs differ, and the "
                                     "limit is the\n       largest of their shallowest halting "
                                     "states so none is left spinning)\n";
                }
            }
        }
        return 0;
    }

    // UNBUFFERED STDOUT, and not as a nicety.
    //
    // With --hold this process applies a setting, prints that it is holding it, and then blocks
    // in pause() forever. std::cout to a pipe or a file is fully buffered, so that line -- and
    // the greeting, and every commit result before it -- would sit in the buffer until the
    // process exited, which with --hold is never. Anything driving this from a script (an init
    // file, scripts/gate.sh) would see an empty stream and conclude it had hung.
    //
    // Found by scripts/gate.sh, which is exactly the kind of thing that gate is for.
    std::cout << std::unitbuf;

    HelperSession session;
    bool started = false;
    if (!helperOverride.empty())
        started = session.startUnprivileged(helperOverride, sysroot);
    else
        started = session.start();

    if (!started) {
        std::cerr << "cpu-power-cli: " << session.error() << "\n";
        return 1;
    }
    // Which rung of the privilege ladder actually carried us. Printed on its own line, in a
    // fixed shape, because scripts/gate.sh asserts on it: a privilege path that is used but
    // never asserted is one that can change silently.
    std::cout << "via=" << HelperSession::methodName(session.method()) << "\n";
    std::cout << session.greeting() << "\n";

    int rc = 0;

    if (scripted) {
        rc = runScriptedSession(session, fs, backend, idle, latencyOn);
        session.stop();
        return rc;
    }

    if (fuzz)
        rc = runFuzz(session) == 0 ? 0 : 1;

    if (!stopKeyArg.empty()) {
        Stop stop;
        if (!stopFromKey(stopKeyArg, stop)) {
            std::cerr << "cpu-power-cli: unknown stop \"" << stopKeyArg << "\"\n";
            return 64;
        }
        std::string detail;
        if (applyStop(session, fs, backend, stop, turbo, touchTurbo, detail) != 0) {
            std::cerr << "cpu-power-cli: " << detail << "\n";
            rc = 1;
        } else {
            std::cout << "commit: " << detail << "\n";
        }
    }

    if (touchDaw) {
        if (daw && latencyOn.empty()) {
            std::cerr << "cpu-power-cli: --daw on: " << idle.unavailableReason() << "\n";
            rc = 1;
        } else {
            std::vector<std::string> lines;
            if (!session.exchange(daw ? latencyOn : "LATENCY OFF", lines, 5000)) {
                std::cerr << "cpu-power-cli: latency: "
                          << (lines.empty() ? "no response" : lines.back()) << "\n";
                rc = 1;
            }
        }
    }

    if (show || !stopKeyArg.empty()) {
        std::vector<std::string> lines;
        session.exchange("SNAPSHOT", lines, 5000);
        for (const std::string &l : lines) {
            if (l.compare(0, 2, "OK") != 0)
                std::cout << l << "\n";
        }
    }

    if (hold) {
        // The settings live exactly as long as this process does. That is the same contract the
        // GUI has, and it is what makes the tool safe to try: killing it puts the machine back.
        std::cout << "holding; ^C or kill to revert\n";
        for (;;)
            ::pause();
    }

    session.stop();
    return rc;
}
