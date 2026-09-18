// The four controls this window is made of.
//
// Each is a plain struct with its state, a rect, and draw/hit-test methods. There is no widget
// tree, no parent pointers and no invalidation graph: there are eight controls on one panel, and
// a hierarchy would be more machinery than the thing it manages. This mirrors how the rations-amp
// editor draws its head page.
//
// THE STATE A CONTROL SHOWS IS NOT THE STATE IT REQUESTED. Every one of these takes what it
// displays from outside, and the app re-reads the kernel every tick to fill it in. A control
// that shows what it last asked for is a control that lies the moment a write is rejected.
//
// A control whose knob the running kernel does not offer is `enabled = false`: drawn dimmed, with
// its reason available for the status strip, and inert to clicks. Never hidden -- a knob that
// silently is not there is indistinguishable from one that does nothing.

#pragma once

#include "canvas.h"

#include <string>
#include <vector>

namespace cpupower
{

//------------------------------------------------------------------------
// Where one stop label lands, and the room it had to land in.
//
// The stop labels are the profile names in full, so their widths are very uneven and they are
// centre-aligned on stops that are evenly spaced. Charging every label a flat kStopSpacing --
// which is what this used to do -- makes the longest name pay for space its short neighbours
// were never going to use, and clips it for no reason. What has to hold is that no two adjacent
// labels touch, so the layout is resolved against the NEIGHBOURS' ACTUAL EXTENTS and that is the
// budget tools/uirender audits.
struct StopLabel {
    std::string text;  // clipped to `budget`, i.e. what is actually drawn
    float x = 0;       // left edge of the drawn text
    float natural = 0; // what the unclipped label measures
    float budget = 0;  // the room between the neighbours, less geo::kStopLabelGap each side
};

// Resolves all five at once -- a label's room depends on its neighbours', so this cannot be done
// one at a time. Draw and audit both go through here, so the audit measures the drawn layout
// rather than a second copy of the arithmetic that can drift away from it.
std::vector<StopLabel> layOutStopLabels(Canvas &c, const std::vector<std::string> &labels);

//------------------------------------------------------------------------
// A slider with named detents. It does not interpolate: there is no continuum between `ondemand`
// and `schedutil`, so a drag snaps to the nearest stop and the value is always an index.
struct Slider {
    std::vector<std::string> labels;
    int value = 0;
    bool enabled = true;
    bool dragging = false;
    // The stop the pointer is over, or -1. Drawn as a hint, never as the value.
    int hover = -1;

    void draw(Canvas &c) const;

    // Returns the stop nearest x, clamped. Only meaningful when hit() said yes.
    int stopAt(float x) const;
    bool hit(float x, float y) const;

    // Keyboard: Left/Right/Home/End move by a stop. Returns true if the value changed.
    bool step(int delta);
};

//------------------------------------------------------------------------
// The master toggle. Off means the machine is exactly as the kernel left it, so this is the
// control that makes the tool safe to try, and it is drawn like a switch rather than a checkbox
// to say so. Turning it off restores.
struct Toggle {
    std::string label;
    bool on = false;
    bool enabled = true;
    bool hover = false;

    void draw(Canvas &c) const;
    bool hit(float x, float y) const;
};

//------------------------------------------------------------------------
struct Checkbox {
    std::string label;
    bool on = false;
    bool enabled = true;
    bool hover = false;
    // Column 0 or 1 of the checkbox row.
    int column = 0;
    // Shown in the status strip when this is disabled: "this kernel exposes no turbo control".
    std::string disabledReason;

    void draw(Canvas &c) const;
    bool hit(float x, float y) const;
};

//------------------------------------------------------------------------
// A label/value pair in the readout block. The value is what the kernel just said.
struct Readout {
    std::string label;
    std::string value;
    int row = 0;
    int column = 0;
    // A value the kernel reports but that this tool did not ask for is still shown -- it is just
    // not shown in the accent colour, which is reserved for "this is what you asked for and it
    // took".
    bool applied = false;

    void draw(Canvas &c) const;
};

} // namespace cpupower
