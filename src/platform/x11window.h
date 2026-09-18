// X11Window -- one top-level window, painted by hand through Canvas.
//
// Simpler than the plug-in case this is ported from: a top-level window has no XEmbed, no host
// run loop to cooperate with, and no IRunLoop to register with. It owns its own select() loop.
//
// Four rules from the plug-in carry over unchanged, because they are the ones that bite:
//
//   * DOUBLE BUFFER, ALWAYS. Compose into an ARGB32 image surface, blit once with
//     CAIRO_OPERATOR_SOURCE. A partially drawn frame is never visible.
//   * NEVER PAINT SYNCHRONOUSLY FROM AN EVENT HANDLER. Handlers set a dirty flag; the paint
//     happens once per pass round the loop, so a burst of motion events costs one repaint.
//   * INSTALL THE NON-FATAL X ERROR HANDLER (xerror.h) before creating anything.
//   * DETECT ASYNCHRONOUS XCreateWindow FAILURE by counting X errors across an XSync. X requests
//     do not fail in place, so a window that was never created otherwise shows up much later as
//     an unrelated BadDrawable.
//
// LAYOUT IS IN LOGICAL UNITS. The window is created at logical size x scale, one cairo_scale is
// applied at compose time, and mouse coordinates are divided by the scale before they reach the
// callbacks. No geometry constant anywhere has a scale factor baked into it.

#pragma once

#include "gfx/canvas.h"
#include "gfx/fontstack.h"

#include <X11/Xlib.h>

#include <functional>
#include <string>

namespace cpupower
{

class X11Window
{
public:
    X11Window() = default;
    ~X11Window();

    X11Window(const X11Window &) = delete;
    X11Window &operator=(const X11Window &) = delete;

    // All coordinates handed to these are LOGICAL units, already divided by the scale.
    struct Callbacks {
        std::function<void(Canvas &)> draw;
        // pressed = true on ButtonPress, false on ButtonRelease. Button 1 only.
        std::function<void(float x, float y, bool pressed)> button;
        std::function<void(float x, float y)> motion;
        // Return true if the key was handled. Unhandled Escape closes the window.
        std::function<bool(KeySym sym)> key;
        // Called every tickMs, whether or not anything is dirty. This is where a live readout is
        // re-read, because the window shows what the kernel reports, not what was last requested.
        std::function<void()> tick;
    };

    // Returns false having already warned: no display, no window, or a font stack that fell back
    // to a system face when the caller said that mattered.
    bool open(const std::string &title, const std::string &wmClass, float logicalW, float logicalH,
              float scale, int tickMs);

    void run(const Callbacks &cb);
    void stop()
    {
        mRunning = false;
    }
    // Ask for a repaint on the next pass. Cheap and idempotent -- call it from any handler.
    void invalidate()
    {
        mDirty = true;
    }

    // Repaint NOW, from inside a handler, and only for the one case that needs it: a handler
    // that is about to block for a long time. Starting the privileged helper waits for the user
    // to type a password into another window, which is seconds, and leaving a stale frame up
    // -- with the status strip still saying whatever it said before the click -- would look like
    // the window had hung. Every other repaint goes through invalidate() and the loop's one
    // paint per pass, which is the rule this is the deliberate exception to.
    void paintNow()
    {
        if (mActive) {
            mDirty = false;
            paint(*mActive);
        }
    }

    bool fontsAreBundled() const
    {
        return mFontsLoaded;
    }

private:
    void paint(const Callbacks &cb);
    void close();

    ::Display *mDpy = nullptr;
    ::Window mWin = 0;
    Atom mWmDelete = 0;
    cairo_surface_t *mTarget = nullptr;

    FontStack mFonts;
    bool mFontsLoaded = false;

    float mLogicalW = 0, mLogicalH = 0, mScale = 1.0f;
    int mTickMs = 250;
    // The callbacks run() was given, so paintNow() can compose the same frame the loop
    // would. Null outside run().
    const Callbacks *mActive = nullptr;

    bool mRunning = false;
    bool mDirty = true;
};

} // namespace cpupower
