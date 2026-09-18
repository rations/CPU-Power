// cpu-power -- the window.
//
// This file is deliberately thin. It parses two options, opens a window, maps X events onto the
// App's semantic actions, and gets out of the way. Everything that decides anything is in
// src/app.cpp, which does not include X11 -- so the whole of the interaction logic can be driven
// without a display, and this file has nothing in it worth testing.
//
// Nothing here starts the privileged helper. Turning the Active toggle on is what does that, and
// it is the only thing that does, because that toggle is the tool's whole safety property: OFF
// MEANS THE MACHINE IS EXACTLY AS THE KERNEL LEFT IT.

#include "app.h"
#include "config.h"
#include "geometry.h"
#include "panel.h"
#include "platform/x11window.h"

#include <X11/keysym.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace cpupower;

int main(int argc, char **argv)
{
    Config prefs = loadConfig();

    float scale = prefs.scale;
    bool scaleFromArgv = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = static_cast<float>(atof(argv[++i]));
            scaleFromArgv = true;
            if (scale < geo::kScaleMin || scale > geo::kScaleMax) {
                fprintf(stderr, "cpu-power: --scale must be between %.2f and %.2f\n",
                        static_cast<double>(geo::kScaleMin), static_cast<double>(geo::kScaleMax));
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: cpu-power [--scale <%.2f..%.2f>]\n\n"
                   "  Preferences are remembered in %s\n"
                   "  They set where the controls sit and are NOT applied at startup: nothing\n"
                   "  changes until the Active toggle is turned on.\n",
                   static_cast<double>(geo::kScaleMin), static_cast<double>(geo::kScaleMax),
                   configPath().empty() ? "(nowhere: neither $XDG_CONFIG_HOME nor $HOME is set)"
                                        : configPath().c_str());
            return 0;
        } else {
            fprintf(stderr, "cpu-power: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    // A remembered scale outside the audited range is clamped rather than refused: an option the
    // user typed is worth an error, a stale line in a config file is not worth a dead window.
    if (!scaleFromArgv) {
        if (scale < geo::kScaleMin)
            scale = geo::kScaleMin;
        if (scale > geo::kScaleMax)
            scale = geo::kScaleMax;
    }

    App app;
    if (!app.probe()) {
        fprintf(stderr, "cpu-power: %s\n", app.fatal().c_str());
        return 1;
    }
    app.applyPreferences(prefs);

    X11Window win;
    if (!win.open("CPU Power", "cpu-power", geo::kWinW, geo::kWinH, scale, 250))
        return 1;

    if (!win.fontsAreBundled())
        fprintf(stderr, "cpu-power: drawing with a system font; legends may not fit their slots\n");

    // The one handler that can block for seconds is the one that starts the helper: on the normal
    // path polkit answers immediately and nothing is asked, but an administrator may have
    // overridden the action with a rule that challenges. It repaints first so the strip says so.
    app.paintNow = [&win]() { win.paintNow(); };

    X11Window::Callbacks cb;

    cb.draw = [&](Canvas &c) { app.panel().draw(c); };

    cb.button = [&](float x, float y, bool pressed) {
        if (pressed)
            app.press(x, y);
        else
            app.release();
        if (app.takeDirty())
            win.invalidate();
    };

    cb.motion = [&](float x, float y) {
        app.motion(x, y);
        if (app.takeDirty())
            win.invalidate();
    };

    cb.key = [&](KeySym sym) {
        switch (sym) {
            case XK_Left:
                app.stepStop(-1);
                break;
            case XK_Right:
                app.stepStop(+1);
                break;
            case XK_Home:
                app.selectStop(0);
                break;
            case XK_End:
                app.selectStop(kStopCount - 1);
                break;
            case XK_space:
                app.toggleMaster();
                break;
            case XK_t:
                app.toggleTurbo();
                break;
            case XK_d:
                app.toggleDaw();
                break;
            case XK_q:
                win.stop();
                return true;
            default:
                return false;
        }
        if (app.takeDirty())
            win.invalidate();
        return true;
    };

    // The window shows what the KERNEL reports. This is where it asks.
    cb.tick = [&]() {
        app.tick();
        if (app.takeDirty())
            win.invalidate();
    };

    win.run(cb);

    // Explicit rather than left to the destructor, so the process is still here -- and can still
    // print -- while the helper puts the machine back.
    app.shutdown();
    return 0;
}
