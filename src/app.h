// App — the layer that joins the model, the privileged helper and the panel.
//
// Everything above it draws; everything below it decides or writes. Four rules live here, and
// they are the reason this is a layer rather than a handful of lambdas in main.cpp:
//
//   1. NOTHING IS APPLIED UNTIL THE MASTER TOGGLE IS ON. Moving the slider with Active off moves
//      the slider and stages nothing -- no helper is started, no password is asked for, and the
//      machine is untouched. That is what makes the tool safe to open and look at.
//
//   2. TURNING THE MASTER TOGGLE OFF PUTS THE MACHINE BACK. It sends RESTORE and closes the
//      session; closing the pipe would restore anyway, which is the point of the design, but
//      asking first means the restore has demonstrably happened before the status strip says so.
//
//   3. THE WINDOW SHOWS WHAT THE KERNEL REPORTS, NOT WHAT WAS REQUESTED. Every COMMIT is followed
//      by a SNAPSHOT and the readouts are filled from its answer. A slider that shows a setting
//      the machine is not in is a lie, and a rejected write is exactly when that lie would be
//      told.
//
//   4. THE STATE IS POLLED, NOT ASSUMED. The tick re-reads governor, EPP, clock and turbo, so a
//      change made by another tool -- or by the kernel's own thermal management -- shows up in
//      the window rather than being contradicted by a stale control.
//
// This file deliberately does not include X11. It exposes semantic actions (stepStop, toggleMaster)
// and the keyboard map lives in main.cpp, which is what lets the whole of the interaction logic be
// driven without a display.

#pragma once

#include "config.h"
#include "panel.h"
#include "core/backend.h"
#include "core/idle.h"
#include "core/profile.h"
#include "core/sysfs.h"
#include "platform/spawn.h"

#include <functional>
#include <string>

namespace cpupower
{

// What the kernel says right now. Filled from the helper's SNAPSHOT when a session is running and
// from unprivileged sysfs reads when one is not -- both of which are the same files; the
// difference is only which process is doing the reading.
struct LiveState {
    bool valid = false;
    std::string governor; // "mixed" when the policies disagree, which is a real state
    std::string epp;
    long long khz = 0; // the highest scaling_cur_freq across the policies
    int turboOn = -1;  // -1 when this machine has no turbo knob, or it could not be read
    bool latencyHeld = false;
};

class App
{
public:
    // sysroot is empty for the real machine. It exists so the interaction logic can be exercised
    // against a captured tree; it is unprivileged either way, and it never reaches the helper.
    explicit App(std::string sysroot = std::string());
    ~App();

    App(const App &) = delete;
    App &operator=(const App &) = delete;

    // Probe the machine and set up the controls, including disabling the ones whose knobs this
    // kernel does not offer. Returns false when there is nothing this tool can do here; fatal()
    // says why, in words that belong on screen.
    bool probe();

    const std::string &fatal() const
    {
        return mFatal;
    }

    Panel &panel()
    {
        return mPanel;
    }

    // Called before the first paint. Sets where the controls sit; never turns the master on.
    void applyPreferences(const Config &c);

    // Called by main.cpp before a handler blocks for the password prompt, so the frame the user
    // is looking at says what is happening rather than going stale for several seconds.
    std::function<void()> paintNow;

    // --- pointer, in logical units ---
    void press(float x, float y);
    void release();
    void motion(float x, float y);

    // --- semantic actions, so the key map stays in main.cpp and this layer stays X-free ---
    void stepStop(int delta);
    void selectStop(int index);
    void toggleMaster();
    void toggleTurbo();
    void toggleDaw();
    std::string dawLatencyCommand() const;
    std::string dawOnDescription() const;

    // The 250 ms tick: re-read the kernel.
    void tick();

    // Consume the repaint flag.
    bool takeDirty();

    // Called on the way out. The helper restores on EOF whatever happens; this makes the ordinary
    // exit wait for it, so the process is gone only once the machine is back.
    void shutdown();

private:
    Request currentRequest() const;

    bool startSession();
    void endSession();

    // Build the plan for the controls as they stand and send it. Assumes a session is running.
    void apply();

    void refresh();
    bool refreshFromHelper();
    void refreshFromSysfs();
    void showLive();

    void setStatus(std::string text, bool isError = false);
    void savePreferences();

    Sysfs mFs;
    Backend mBackend;
    // cpuidle, which is a different subsystem from mBackend's cpufreq and is reached by a
    // different control: the slider cannot touch C-states and DAW mode cannot touch P-states.
    IdleStates mIdle;
    Panel mPanel;
    HelperSession mSession;
    LiveState mLive;

    std::string mFatal;
    bool mDirty = true;
    bool mHaveLatencyDev = false;
    // Set while a control is being dragged, so the tick does not fight the pointer for the value.
    bool mDragging = false;
};

} // namespace cpupower
