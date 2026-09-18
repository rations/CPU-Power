// See app.h.

#include "app.h"

#include "geometry.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cpupower
{

namespace
{

// The PM-QoS character device DAW mode holds open. This is a fixed absolute path in every
// kernel that has cpuidle, it is not under /sys, and it is never opened here -- only stat'ed, so
// the checkbox can be disabled and labelled on a machine that does not have it. The helper is
// what opens it, as root, and the fd it holds is the constraint (pm_qos_interface.rst).
constexpr const char *kLatencyDev = "/dev/cpu_dma_latency";

// Split "gov=powersave" into "gov" and "powersave". Returns false for a token with no '='.
bool splitKV(const std::string &token, std::string &key, std::string &value)
{
    const size_t eq = token.find('=');
    if (eq == std::string::npos)
        return false;
    key = token.substr(0, eq);
    value = token.substr(eq + 1);
    return true;
}

std::vector<std::string> tokens(const std::string &line)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ')
            ++i;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ')
            ++i;
        if (i > start)
            out.push_back(line.substr(start, i - start));
    }
    return out;
}

std::string ghz(long long khz)
{
    if (khz <= 0)
        return "-";
    char buf[32];
    ::snprintf(buf, sizeof(buf), "%.2f GHz", static_cast<double>(khz) / 1000000.0);
    return buf;
}

} // namespace

//------------------------------------------------------------------------
App::App(std::string sysroot) : mFs(sysroot.empty() ? std::string("/sys") : std::move(sysroot))
{
}

//------------------------------------------------------------------------
App::~App()
{
    shutdown();
}

//------------------------------------------------------------------------
bool App::probe()
{
    mBackend = Backend::probe(mFs);
    mIdle = IdleStates::probe(mFs);

    if (!mBackend.usable()) {
        mFatal = mBackend.unusableReason();
        return false;
    }

    mPanel.driver = mBackend.driver;
    if (mBackend.mode != PstateMode::NotApplicable)
        mPanel.driver += " (" + std::string(pstateModeName(mBackend.mode)) + ")";

    mPanel.policies = std::to_string(mBackend.policies.size());

    // A KNOB THAT DOES NOT EXIST IS DISABLED AND LABELLED, NEVER SILENTLY IGNORED. Both reasons
    // go straight into the status strip at startup rather than waiting for the user to click a
    // control that will not respond -- a greyed control with no explanation is only half of the
    // rule.
    std::string missing;

    if (mBackend.turbo == TurboKnob::None) {
        mPanel.turbo.enabled = false;
        mPanel.turbo.disabledReason = "this kernel exposes no turbo or boost control";
        missing = mPanel.turbo.disabledReason;
    }

    // DAW mode needs two things, and they fail for different reasons: the PM-QoS device to hold
    // open, and an idle state worth blocking. Probing both is what stops the checkbox being a
    // control that writes a number and changes nothing.
    struct ::stat st;
    mHaveLatencyDev = (::stat(kLatencyDev, &st) == 0 && S_ISCHR(st.st_mode));
    if (!mHaveLatencyDev) {
        mPanel.daw.enabled = false;
        mPanel.daw.disabledReason = "no /dev/cpu_dma_latency: this kernel has no cpuidle PM-QoS";
    } else if (!mIdle.limitUs) {
        mPanel.daw.enabled = false;
        mPanel.daw.disabledReason = "DAW mode unavailable: " + mIdle.unavailableReason();
    }
    if (!mPanel.daw.enabled) {
        missing = missing.empty() ? mPanel.daw.disabledReason
                                  : "no turbo control, and " + mPanel.daw.disabledReason;
    }

    refreshFromSysfs();
    showLive();

    if (!missing.empty())
        setStatus(missing);
    else
        setStatus("ready; nothing is changed until Active is turned on");

    return true;
}

//------------------------------------------------------------------------
void App::applyPreferences(const Config &c)
{
    mPanel.stops.value = static_cast<int>(c.stop);
    if (mPanel.turbo.enabled)
        mPanel.turbo.on = c.turbo;
    if (mPanel.daw.enabled)
        mPanel.daw.on = c.daw;
    // The master toggle is deliberately NOT restored. See the header comment in config.h.
    showLive();
    mDirty = true;
}

//------------------------------------------------------------------------
void App::savePreferences()
{
    Config c;
    c.stop = static_cast<Stop>(mPanel.stops.value);
    c.turbo = mPanel.turbo.on;
    c.daw = mPanel.daw.on;
    // The scale is set on the command line and is not ours to overwrite from here; loadConfig
    // supplies it and it is written back unchanged.
    c.scale = loadConfig().scale;
    (void)saveConfig(c);
}

//------------------------------------------------------------------------
Request App::currentRequest() const
{
    Request r;
    r.stop = static_cast<Stop>(mPanel.stops.value);
    r.turbo = mPanel.turbo.on;
    r.touchTurbo = mPanel.turbo.enabled;
    return r;
}

//------------------------------------------------------------------------
void App::setStatus(std::string text, bool isError)
{
    mPanel.status = std::move(text);
    mPanel.statusIsError = isError;
    mDirty = true;
}

//------------------------------------------------------------------------
bool App::takeDirty()
{
    const bool d = mDirty;
    mDirty = false;
    return d;
}

//------------------------------------------------------------------------
// Privilege.

bool App::startSession()
{
    if (mSession.running())
        return true;

    // start() can take a moment -- it stats the helper, probes the privilege ladder and waits for
    // the helper's greeting. Say so on screen first, or the window sits there looking hung with a
    // stale status line. It no longer says "asking for the administrator password", because on
    // the normal path nothing is asked: the polkit action authorises a locally seated user
    // outright. Promising a prompt that never comes is its own kind of wrong status line.
    setStatus("starting the privileged helper...");
    if (paintNow)
        paintNow();

    if (!mSession.start()) {
        setStatus(mSession.error(), true);
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
void App::endSession()
{
    if (!mSession.running())
        return;

    // Closing the pipe would restore on its own -- that is the whole design, and it is what
    // covers the GUI being killed. Asking explicitly first means the restore has demonstrably
    // finished, with a count of what could not be put back, before the strip claims it has.
    std::vector<std::string> lines;
    const bool ok = mSession.exchange("RESTORE", lines, 10000);
    mSession.stop();

    std::string detail;
    if (!lines.empty()) {
        // "OK failures=N"
        for (const std::string &t : tokens(lines.back())) {
            std::string k, v;
            if (splitKV(t, k, v) && k == "failures" && v != "0")
                detail = "; " + v + " attribute(s) could not be put back";
        }
    }

    setStatus(ok ? "off: the machine is back as the kernel left it" + detail
                 : "off: the helper was asked to restore and did not confirm it",
              !ok);
}

//------------------------------------------------------------------------
void App::apply()
{
    if (!mSession.running())
        return;

    const WritePlan plan = buildPlan(mFs, mBackend, currentRequest());

    std::string noteText;
    for (const Note &n : plan.notes)
        noteText += (noteText.empty() ? "" : "; ") + n.text;

    bool failed = false;
    std::string commitDetail;

    for (const std::string &cmd : plan.toProtocol()) {
        std::vector<std::string> lines;
        if (!mSession.exchange(cmd, lines, 10000)) {
            setStatus(lines.empty() ? cmd + ": the helper did not answer" : lines.back(), true);
            failed = true;
            break;
        }
        if (cmd == "COMMIT" && !lines.empty())
            commitDetail = lines.back(); // "OK applied=N failed=N skipped=N"
    }

    // DAW mode is independent of the slider: it is about C-state exit latency, not clock speed
    // (pm_qos_interface.rst). It is applied here rather than in its own path so that turning the
    // master toggle on brings the machine to the state the whole panel describes, in one go.
    if (!failed && mPanel.daw.enabled) {
        std::vector<std::string> lines;
        if (!mSession.exchange(mPanel.daw.on ? dawLatencyCommand() : "LATENCY OFF", lines, 5000)) {
            setStatus(lines.empty() ? "the helper did not answer the latency request"
                                    : lines.back(),
                      true);
            failed = true;
        }
    }

    // EVERY COMMIT IS FOLLOWED BY A RE-READ. What lands in the readouts is
    // what the kernel reports afterwards, not what was asked for -- which is exactly the case a
    // rejected write would otherwise hide.
    refresh();

    if (failed)
        return;

    // Report from the re-read, not from the request.
    std::string text = "governor " + (mLive.governor.empty() ? std::string("?") : mLive.governor);
    if (!mLive.epp.empty() && mLive.epp != "-")
        text += ", EPP " + mLive.epp;

    int applied = 0, wroteFailed = 0;
    for (const std::string &t : tokens(commitDetail)) {
        std::string k, v;
        if (!splitKV(t, k, v))
            continue;
        if (k == "applied")
            applied = ::atoi(v.c_str());
        else if (k == "failed")
            wroteFailed = ::atoi(v.c_str());
    }

    if (wroteFailed > 0) {
        setStatus(std::to_string(wroteFailed) + " write(s) rejected by the kernel; now on " + text,
                  true);
        return;
    }

    text += "  (" + std::to_string(applied) + " write" + (applied == 1 ? "" : "s") + ")";
    if (!noteText.empty())
        text = noteText + " -- " + text;
    setStatus(text, false);
}

//------------------------------------------------------------------------
// Actions.

void App::selectStop(int index)
{
    if (!mPanel.stops.enabled)
        return;
    if (index < 0)
        index = 0;
    if (index >= static_cast<int>(mPanel.stops.labels.size()))
        index = static_cast<int>(mPanel.stops.labels.size()) - 1;
    if (index == mPanel.stops.value)
        return;

    mPanel.stops.value = index;
    mDirty = true;
    savePreferences();

    // Rule 1: with Active off this moves the slider and stages nothing. No helper is started, no
    // password is asked for, and the machine is untouched.
    if (mPanel.master.on)
        apply();
    else
        setStatus(std::string("\"") + stopLabel(static_cast<Stop>(index)) +
                  "\" selected; turn Active on to apply it");
}

//------------------------------------------------------------------------
void App::stepStop(int delta)
{
    if (mPanel.stops.enabled)
        selectStop(mPanel.stops.value + delta);
}

//------------------------------------------------------------------------
void App::toggleMaster()
{
    if (!mPanel.master.enabled)
        return;

    if (!mPanel.master.on) {
        if (!startSession()) {
            // The toggle stays off, because the machine is unchanged. A control that shows `on`
            // over a machine nothing was written to is the lie this whole layer exists to avoid.
            mDirty = true;
            return;
        }
        mPanel.master.on = true;
        mDirty = true;
        apply();
    } else {
        mPanel.master.on = false;
        endSession();
        refresh();
    }
}

//------------------------------------------------------------------------
void App::toggleTurbo()
{
    if (!mPanel.turbo.enabled)
        return;
    mPanel.turbo.on = !mPanel.turbo.on;
    mDirty = true;
    savePreferences();

    if (mPanel.master.on)
        apply();
    else
        setStatus(std::string("turbo will be turned ") + (mPanel.turbo.on ? "on" : "off") +
                  " when Active is");
}

//------------------------------------------------------------------------
// The PM-QoS request DAW mode makes, resolved from THIS machine's idle table rather than from a
// constant. See the long note on IdleStates::limitUs for why the obvious value -- 0 -- is the
// wrong one: it admits only the POLL pseudo-state, which does not halt the core, so every CPU
// spins instead of idling and the machine runs hot for no latency benefit over the shallowest
// real halt state.
std::string App::dawLatencyCommand() const
{
    // Only ever called when mPanel.daw.enabled, which requires limitUs to have resolved.
    return "LATENCY " + std::to_string(mIdle.limitUs.value_or(0));
}

//------------------------------------------------------------------------
std::string App::dawOnDescription() const
{
    std::string text = "DAW mode on: wake-up latency capped at " +
                       std::to_string(mIdle.limitUs.value_or(0)) + " us";

    // Name the state, not just the number. "2 us" is a fact about the request; "C1E" is a fact
    // about what the machine will actually do with it, and it is what makes a collapsed or
    // surprising outcome visible rather than silent.
    if (const IdleStates::State *a = mIdle.admitted(); a && !a->name.empty())
        text += ", deepest idle state " + a->name;
    return text;
}

//------------------------------------------------------------------------
void App::toggleDaw()
{
    if (!mPanel.daw.enabled)
        return;
    mPanel.daw.on = !mPanel.daw.on;
    mDirty = true;
    savePreferences();

    if (!mPanel.master.on) {
        setStatus(std::string("DAW mode will be turned ") + (mPanel.daw.on ? "on" : "off") +
                  " when Active is");
        return;
    }

    std::vector<std::string> lines;
    if (!mSession.exchange(mPanel.daw.on ? dawLatencyCommand() : "LATENCY OFF", lines, 5000)) {
        setStatus(lines.empty() ? "the helper did not answer the latency request" : lines.back(),
                  true);
        mPanel.daw.on = !mPanel.daw.on;
    }
    refresh();

    if (!mPanel.statusIsError)
        setStatus(mLive.latencyHeld ? dawOnDescription()
                                    : "DAW mode off: the kernel may use every idle state again");
}

//------------------------------------------------------------------------
// Pointer.

void App::press(float x, float y)
{
    if (mPanel.stops.hit(x, y)) {
        mDragging = true;
        mPanel.stops.dragging = true;
        selectStop(mPanel.stops.stopAt(x));
    } else if (mPanel.master.hit(x, y)) {
        toggleMaster();
    } else if (mPanel.turbo.hit(x, y)) {
        toggleTurbo();
    } else if (mPanel.daw.hit(x, y)) {
        toggleDaw();
    }
    mDirty = true;
}

//------------------------------------------------------------------------
void App::release()
{
    if (mDragging) {
        mDragging = false;
        mPanel.stops.dragging = false;
        mDirty = true;
    }
}

//------------------------------------------------------------------------
void App::motion(float x, float y)
{
    if (mPanel.stops.dragging)
        selectStop(mPanel.stops.stopAt(x));

    const int hover = mPanel.stops.hit(x, y) ? mPanel.stops.stopAt(x) : -1;
    const bool m = mPanel.master.hit(x, y);
    const bool t = mPanel.turbo.hit(x, y);
    const bool d = mPanel.daw.hit(x, y);

    if (hover != mPanel.stops.hover || m != mPanel.master.hover || t != mPanel.turbo.hover ||
        d != mPanel.daw.hover) {
        mPanel.stops.hover = hover;
        mPanel.master.hover = m;
        mPanel.turbo.hover = t;
        mPanel.daw.hover = d;
        mDirty = true;
    }
}

//------------------------------------------------------------------------
// Polling.

void App::tick()
{
    // Not while a drag is in flight: the pointer owns the value until the button comes up, and a
    // re-read that fought it would make the slider stutter under the finger.
    if (mDragging)
        return;

    const LiveState before = mLive;
    refresh();

    if (mLive.governor != before.governor || mLive.epp != before.epp ||
        mLive.turboOn != before.turboOn || mLive.latencyHeld != before.latencyHeld ||
        mLive.khz != before.khz)
        mDirty = true;
}

//------------------------------------------------------------------------
void App::refresh()
{
    if (mSession.running()) {
        if (!refreshFromHelper())
            refreshFromSysfs();
    } else {
        refreshFromSysfs();
    }
    showLive();
}

//------------------------------------------------------------------------
bool App::refreshFromHelper()
{
    std::vector<std::string> lines;
    if (!mSession.exchange("SNAPSHOT", lines, 5000))
        return false;

    LiveState s;
    bool sawPolicy = false;

    for (const std::string &line : lines) {
        const std::vector<std::string> t = tokens(line);
        if (t.empty())
            continue;

        if (t[0] == "STATE") {
            std::string gov, epp;
            long long khz = 0;
            for (size_t i = 1; i < t.size(); ++i) {
                std::string k, v;
                if (!splitKV(t[i], k, v))
                    continue;
                if (k == "gov")
                    gov = v;
                else if (k == "epp")
                    epp = v;
                else if (k == "khz")
                    khz = ::atoll(v.c_str());
            }
            if (!sawPolicy) {
                s.governor = gov;
                s.epp = epp;
                sawPolicy = true;
            } else {
                // Policies CAN disagree -- another tool may have set one of them. Saying "mixed"
                // is the honest answer; picking policy0 and calling it the machine's state is not.
                if (s.governor != gov)
                    s.governor = "mixed";
                if (s.epp != epp)
                    s.epp = "mixed";
            }
            if (khz > s.khz)
                s.khz = khz;
        } else if (t[0] == "TURBO") {
            for (size_t i = 1; i < t.size(); ++i) {
                std::string k, v;
                if (splitKV(t[i], k, v) && k == "on")
                    s.turboOn = ::atoi(v.c_str());
            }
        } else if (t[0] == "LATENCY") {
            for (size_t i = 1; i < t.size(); ++i) {
                std::string k, v;
                if (splitKV(t[i], k, v) && k == "held")
                    s.latencyHeld = (v == "1");
            }
        }
    }

    s.valid = sawPolicy;
    if (!s.valid)
        return false;

    mLive = s;
    return true;
}

//------------------------------------------------------------------------
void App::refreshFromSysfs()
{
    // The same files the helper's SNAPSHOT reads -- governor, EPP and the current clock are all
    // world-readable. The privileged process is needed to WRITE them, not to look at them, so
    // with the master toggle off the window is still live and still honest.
    LiveState s;
    bool first = true;

    for (const Policy &p : mBackend.policies) {
        const std::string gov = mFs.read(p.attr("scaling_governor")).value_or("-");
        const std::string epp = mFs.read(p.attr("energy_performance_preference")).value_or("-");
        const long long khz = mFs.readInt(p.attr("scaling_cur_freq")).value_or(0);

        if (first) {
            s.governor = gov;
            s.epp = epp;
            first = false;
        } else {
            if (s.governor != gov)
                s.governor = "mixed";
            if (s.epp != epp)
                s.epp = "mixed";
        }
        if (khz > s.khz)
            s.khz = khz;
    }

    if (mBackend.turbo != TurboKnob::None && !mBackend.policies.empty()) {
        const std::string path = mBackend.turboPath(mBackend.policies.front());
        if (const auto raw = mFs.read(path)) {
            const bool bit = (*raw == "1");
            // The inversion is resolved in exactly one place, and this is a read of the same
            // knob the plan writes -- so it asks the Backend rather than re-deciding.
            s.turboOn = (bit != mBackend.turboInverted()) ? 1 : 0;
        }
    }

    // Whether the helper is holding the latency fd is not visible from here; only the helper
    // knows, and with no session there is nothing holding it.
    s.latencyHeld = false;
    s.valid = !mBackend.policies.empty();
    mLive = s;
}

//------------------------------------------------------------------------
void App::showLive()
{
    mPanel.governor = mLive.governor.empty() ? "-" : mLive.governor;
    mPanel.epp = mLive.epp.empty() ? "-" : mLive.epp;
    mPanel.frequency = ghz(mLive.khz);

    if (mBackend.turbo == TurboKnob::None)
        mPanel.turboState = "n/a";
    else if (mLive.turboOn < 0)
        mPanel.turboState = "?";
    else
        mPanel.turboState = mLive.turboOn ? "on" : "off";

    // THE CONTROLS FOLLOW THE KERNEL, NOT THE REQUEST. If a turbo write was rejected, or another
    // tool changed it, the checkbox moves to what the machine is actually doing -- which is the
    // whole contract in one line. The slider is the exception and stays where the
    // user put it: several stops can resolve to the same governor and EPP on a given machine, so
    // there is no honest reverse mapping from kernel state to a stop. The readouts carry the
    // truth instead, and the accent colour below says whether this stop is what the machine is in.
    if (mPanel.master.on && mPanel.turbo.enabled && mLive.turboOn >= 0)
        mPanel.turbo.on = (mLive.turboOn == 1);
    if (mPanel.master.on && mPanel.daw.enabled)
        mPanel.daw.on = mLive.latencyHeld;

    const Resolution r = resolve(mBackend, static_cast<Stop>(mPanel.stops.value));
    mPanel.stopIsLive = !r.governor.empty() && r.governor == mLive.governor &&
                        (r.epp.empty() || r.epp == mLive.epp);
}

//------------------------------------------------------------------------
void App::shutdown()
{
    if (mSession.running()) {
        // stop() closes the pipe, which is the restore trigger, and waits for the helper to go.
        mSession.stop();
    }
}

} // namespace cpupower
