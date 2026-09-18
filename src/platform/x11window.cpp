// See x11window.h.

#include "x11window.h"

#include "xerror.h"
#include "respath.h"
#include "appicon.h"

#include <cairo/cairo-xlib.h>

#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <sys/select.h>
#include <sys/time.h>

#include <cerrno>
#include <cstdio>
#include <vector>

namespace cpupower
{

namespace
{

//------------------------------------------------------------------------
// Publish the application icon on the window itself, as _NET_WM_ICON.
//
// THIS IS NOT THE SAME MECHANISM as the installed icon theme, and it is the one that matters for
// this program's audience. A desktop environment finds an icon through the desktop entry's Icon=
// key and the icon theme; a PLAIN WINDOW MANAGER never reads desktop entries at all, and takes a
// live window's icon from this property. CPU-Power is aimed at people running a window manager,
// so shipping only the theme copy would leave the titlebar and task list blank on exactly the
// systems it was written for.
//
// The payload is generated, so nothing here has to encode anything: appicon.h already holds the
// property's own layout, which EWMH 1.5 section 5.12 gives as an array of 32-bit packed ARGB
// cardinals -- high byte alpha, low byte blue -- with each image preceded by its width and
// height, rows left to right and top to bottom, and several images concatenated. The pixels are
// NOT premultiplied, which the specification does not say and which had to be measured; see the
// generated header for what was measured and how.
//
// The one conversion that is unavoidable is the width of the array. A property declared /32 goes
// over the wire as 32 bits per value, but Xlib takes it from the caller as an array of LONG,
// which is 64 bits here, and narrows it itself. Handing XChangeProperty the packed uint32_t
// array directly would read the right number of bytes and encode the wrong thing entirely --
// every second pixel becoming the top half of the pair before it -- so it is widened here.
static void publishIcon(Display *dpy, Window win)
{
    // One XChangeProperty is one X request, and a request that exceeds the server's limit is an
    // error rather than a silent truncation. XMaxRequestSize is in 4-byte units and this payload
    // is fixed at compile time, so the check is cheap and the failure says what happened instead
    // of surfacing later as a BadLength against an unrelated call.
    const long need = 6 + static_cast<long>(appicon::kWords); // request header + data, in words
    if (need > XMaxRequestSize(dpy)) {
        fprintf(stderr, "cpu-power: the window icon is too large for this X server, skipping it\n");
        return;
    }

    std::vector<unsigned long> prop(appicon::kData, appicon::kData + appicon::kWords);

    // Not error-checked here on purpose: XChangeProperty is asynchronous and its return value
    // carries no status. The round trip below -- sample errorCount(), XSync, sample again --
    // is what actually catches a rejected request, and it already covers this one.
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char *>(prop.data()),
                    static_cast<int>(prop.size()));
}

} // namespace

//------------------------------------------------------------------------
X11Window::~X11Window()
{
    close();
}

//------------------------------------------------------------------------
bool X11Window::open(const std::string &title, const std::string &wmClass, float logicalW,
                     float logicalH, float scale, int tickMs)
{
    mLogicalW = logicalW;
    mLogicalH = logicalH;
    mScale = scale > 0.0f ? scale : 1.0f;
    mTickMs = tickMs > 0 ? tickMs : 250;

    mDpy = XOpenDisplay(nullptr);
    if (!mDpy) {
        fprintf(stderr, "cpu-power: cannot open the X display ($DISPLAY)\n");
        return false;
    }

    // BEFORE any window exists: Xlib's default handler calls exit(), and the failure this
    // function goes on to detect would otherwise be detected by dying.
    registerDisplay(mDpy);

    const int screen = DefaultScreen(mDpy);
    const unsigned w = static_cast<unsigned>(mLogicalW * mScale + 0.5f);
    const unsigned h = static_cast<unsigned>(mLogicalH * mScale + 0.5f);

    const unsigned long before = errorCount();
    mWin = XCreateSimpleWindow(mDpy, RootWindow(mDpy, screen), 0, 0, w, h, 0,
                               BlackPixel(mDpy, screen), BlackPixel(mDpy, screen));

    XStoreName(mDpy, mWin, title.c_str());

    // So a desktop entry's StartupWMClass can match this window. That is the DESKTOP
    // ENVIRONMENT half of being identifiable -- it is what lets a dock or a taskbar tie a
    // running window back to the installed .desktop file and its themed icon. The window
    // manager half is _NET_WM_ICON, published below, which needs no desktop entry at all.
    // BOTH halves are the class token, not the human title. ICCCM gives res_name as the name the
    // program was invoked with and res_class as the general class of application, and a desktop
    // entry's StartupWMClass is matched against one or the other depending on whose
    // implementation is reading it -- so making them the same string removes the question. VLC
    // on this machine advertises ("vlc", "vlc") for the same reason. A res_class carrying the
    // display title, spaces and all, would match on some desktops and silently not on others.
    std::vector<char> resName(wmClass.begin(), wmClass.end());
    resName.push_back('\0');
    std::vector<char> resClass(wmClass.begin(), wmClass.end());
    resClass.push_back('\0');
    XClassHint classHint = {};
    classHint.res_name = resName.data();
    classHint.res_class = resClass.data();
    XSetClassHint(mDpy, mWin, &classHint);

    // Fixed size. The layout is a hand-composed panel, not a resizable document, and a window
    // manager that ignores this is handled anyway: ConfigureNotify is not acted on.
    if (XSizeHints *hints = XAllocSizeHints()) {
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = static_cast<int>(w);
        hints->min_height = hints->max_height = static_cast<int>(h);
        XSetWMNormalHints(mDpy, mWin, hints);
        XFree(hints);
    }

    XSelectInput(mDpy, mWin,
                 ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | LeaveWindowMask);

    // Before the map, because a window manager reads a new window's properties when it is mapped
    // and is not obliged to notice one that turns up afterwards.
    publishIcon(mDpy, mWin);

    mWmDelete = XInternAtom(mDpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(mDpy, mWin, &mWmDelete, 1);
    XMapWindow(mDpy, mWin);

    // THE ROUND TRIP. XCreateWindow is asynchronous: if it was rejected, nothing above has failed
    // yet and the first symptom would be an unrelated error against a window id that never
    // existed. Sample, sync, sample.
    XSync(mDpy, False);
    if (errorCount() != before) {
        fprintf(stderr, "cpu-power: the X server rejected the window (see the error above)\n");
        close();
        return false;
    }

    mTarget = cairo_xlib_surface_create(mDpy, mWin, DefaultVisual(mDpy, screen),
                                        static_cast<int>(w), static_cast<int>(h));
    if (cairo_surface_status(mTarget) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "cpu-power: could not create the drawing surface\n");
        close();
        return false;
    }

    mFontsLoaded = mFonts.load(resourceDir());
    return true;
}

//------------------------------------------------------------------------
void X11Window::close()
{
    if (mTarget) {
        cairo_surface_destroy(mTarget);
        mTarget = nullptr;
    }
    if (mDpy) {
        if (mWin) {
            XDestroyWindow(mDpy, mWin);
            mWin = 0;
        }
        unregisterDisplay(mDpy);
        XCloseDisplay(mDpy);
        mDpy = nullptr;
    }
}

//------------------------------------------------------------------------
void X11Window::paint(const Callbacks &cb)
{
    if (!cb.draw || !mTarget)
        return;

    const int pw = static_cast<int>(mLogicalW * mScale + 0.5f);
    const int ph = static_cast<int>(mLogicalH * mScale + 0.5f);

    // Compose offscreen. ARGB32 is premultiplied -- the one Cairo convention not pinned in Canvas,
    // because it belongs to whoever creates the surface, which is here.
    cairo_surface_t *buf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    if (cairo_surface_status(buf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(buf);
        return;
    }

    cairo_t *cr = cairo_create(buf);
    // ONE scale, here. Nothing downstream of this knows the scale exists.
    cairo_scale(cr, mScale, mScale);
    {
        Canvas canvas(cr, &mFonts, mLogicalW, mLogicalH);
        cb.draw(canvas);
    }
    cairo_destroy(cr);

    // Blit once, SOURCE not OVER: the buffer is the frame, not a layer on top of the last one.
    cairo_t *out = cairo_create(mTarget);
    cairo_set_source_surface(out, buf, 0, 0);
    cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
    cairo_paint(out);
    cairo_destroy(out);
    cairo_surface_destroy(buf);

    cairo_surface_flush(mTarget);
    XFlush(mDpy);
}

//------------------------------------------------------------------------
void X11Window::run(const Callbacks &cb)
{
    if (!mDpy || !mWin)
        return;

    mRunning = true;
    mDirty = true;
    mActive = &cb;

    const int xfd = ConnectionNumber(mDpy);

    while (mRunning) {
        // Drain everything the server has for us first, setting state but never painting: a drag
        // generates a MotionNotify per pixel and each one would otherwise be a full recompose.
        while (XPending(mDpy)) {
            XEvent ev;
            XNextEvent(mDpy, &ev);

            switch (ev.type) {
                case Expose:
                    mDirty = true;
                    break;

                case ClientMessage:
                    if (static_cast<Atom>(ev.xclient.data.l[0]) == mWmDelete)
                        mRunning = false;
                    break;

                case ButtonPress:
                case ButtonRelease:
                    if (ev.xbutton.button == Button1 && cb.button) {
                        cb.button(static_cast<float>(ev.xbutton.x) / mScale,
                                  static_cast<float>(ev.xbutton.y) / mScale,
                                  ev.type == ButtonPress);
                    }
                    break;

                case MotionNotify:
                    if (cb.motion) {
                        cb.motion(static_cast<float>(ev.xmotion.x) / mScale,
                                  static_cast<float>(ev.xmotion.y) / mScale);
                    }
                    break;

                case LeaveNotify:
                    // A pointer that left without a ButtonRelease would otherwise leave a control
                    // latched in its hover or dragging state.
                    if (cb.motion)
                        cb.motion(-1.0f, -1.0f);
                    break;

                case KeyPress: {
                    KeySym sym = NoSymbol;
                    char buf[16];
                    XLookupString(&ev.xkey, buf, sizeof(buf), &sym, nullptr);
                    const bool handled = cb.key ? cb.key(sym) : false;
                    if (!handled && sym == XK_Escape)
                        mRunning = false;
                    break;
                }

                default:
                    break;
            }
        }

        if (!mRunning)
            break;

        if (mDirty) {
            mDirty = false;
            paint(cb);
        }

        // Wait for the next event or the next tick, whichever comes first. XPending above may
        // have left events buffered inside Xlib that never reach the fd, so it is checked again
        // rather than slept through.
        if (XPending(mDpy))
            continue;

        fd_set r;
        FD_ZERO(&r);
        FD_SET(xfd, &r);
        struct timeval tv;
        tv.tv_sec = mTickMs / 1000;
        tv.tv_usec = (mTickMs % 1000) * 1000;

        const int n = select(xfd + 1, &r, nullptr, nullptr, &tv);
        if (n < 0 && errno != EINTR) {
            fprintf(stderr, "cpu-power: select on the X connection failed; closing\n");
            mRunning = false;
        } else if (n == 0 && cb.tick) {
            cb.tick();
        }
    }

    mActive = nullptr;
}

} // namespace cpupower
