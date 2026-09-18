// The window's contents: the eight controls, the readout and the status strip.
//
// Panel is deliberately ignorant of X11, of the helper and of sysfs. It holds what to display and
// draws it, and Phase 4's app layer fills it in from the kernel. That split is what lets
// tools/uirender draw the real panel, with the real fonts, on a machine with no display and no
// privilege -- and therefore what makes the layout audit meaningful rather than decorative.

#pragma once

#include "gfx/widgets.h"

#include <string>
#include <vector>

namespace cpupower
{

// One string and the slot it has to fit in. Collected by textSlots() and checked by uirender:
// a string wider than its slot is clipped with an ellipsis at run time, which is survivable but
// means a legend nobody can read -- and it happens on someone else's fontconfig, not ours.
struct TextSlot {
    std::string what; // which piece of the panel, for the failure message
    std::string text; // the string as it would be drawn
    float maxW = 0;   // the slot it must fit in, in logical units
    float measured = 0;
};

class Panel
{
public:
    Panel();

    void draw(Canvas &c) const;

    // Every string this panel would draw, measured against its slot with the real font.
    std::vector<TextSlot> textSlots(Canvas &c) const;

    // --- what the panel shows (filled in by the app layer from the kernel) ---
    std::string driver = "unknown";
    std::string governor = "-";
    std::string epp = "-";
    std::string policies = "-";
    std::string frequency = "-";
    std::string turboState = "-";
    std::string status;
    bool statusIsError = false;

    // True when the machine is ACTUALLY in the state the selected stop asks for -- not when that
    // stop was merely requested. It is what puts the governor and EPP readouts in the accent
    // colour, so the colour means "this is what you asked for and it took" rather than "this is
    // what was sent". The app sets it from the kernel's own report after every re-read.
    bool stopIsLive = false;

    Slider stops;
    Toggle master;
    Checkbox turbo;
    Checkbox daw;

private:
    std::vector<Readout> readouts() const;
};

} // namespace cpupower
