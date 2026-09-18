// See panel.h.

#include "panel.h"

#include "geometry.h"

namespace cpupower
{

//------------------------------------------------------------------------
Panel::Panel()
{
    // The five stops, in the order profile.cpp resolves them. The labels are
    // shortened from the profile names because five of them share one track: uirender is what
    // decides whether a shortening was enough, at every scale.
    stops.labels = {"Powersave", "Balanced", "Balanced-Performance", "Performance", "Max"};
    stops.value = 2;

    master.label = "Active";
    turbo.label = "Turbo";
    turbo.column = 0;
    daw.label = "DAW mode";
    daw.column = 1;
}

//------------------------------------------------------------------------
std::vector<Readout> Panel::readouts() const
{
    std::vector<Readout> r;
    // The accent colour is reserved for "this is what you asked for and it took", so it is keyed
    // on what the kernel reports back, never on what was requested.
    r.push_back({"governor", governor, 0, 0, stopIsLive});
    r.push_back({"EPP", epp, 0, 1, stopIsLive});
    r.push_back({"policies", policies, 1, 0, false});
    r.push_back({"clock", frequency, 1, 1, false});
    r.push_back({"turbo", turboState, 2, 0, turboState == "on"});
    return r;
}

//------------------------------------------------------------------------
void Panel::draw(Canvas &c) const
{
    // Ground.
    c.setColor(geo::kBgColor);
    c.fillRect(c.bounds());

    // Title row: the product on the left, the DRIVER THE KERNEL REPORTS on the right. The driver
    // is here rather than buried in the readout because it is what decides which of these
    // controls do anything at all: the model is a capability probe, not a driver whitelist,
    // so the driver name is the single most useful thing on screen.
    c.setFont(Font::Title);
    c.setFontSize(geo::kTitleSize);
    c.setColor(geo::kTextColor);
    c.drawString(c.clipToWidth("CPU POWER", geo::kTitleMaxW).c_str(), geo::kMargin,
                 geo::kTitleBaseline);

    c.setFont(Font::Body);
    c.setFontSize(geo::kDriverSize);
    c.setColor(geo::kDimColor);
    {
        const std::string text = c.clipToWidth(driver, geo::kDriverMaxW);
        c.drawString(text.c_str(), geo::kDriverRightX - c.stringWidth(text.c_str()),
                     geo::kTitleBaseline);
    }

    c.setColor(geo::kGold, 90);
    c.setPenSize(1.0f);
    c.strokeLine(geo::kMargin, geo::kRuleY, geo::kWinW - geo::kMargin, geo::kRuleY);

    stops.draw(c);
    master.draw(c);
    turbo.draw(c);
    daw.draw(c);

    for (const Readout &r : readouts())
        r.draw(c);

    // The status strip. A sunk well the full width of the window, because what lands in it is a
    // kernel error message and those are not ours to shorten.
    const Rect strip(0, geo::kStripY, geo::kWinW, geo::kStripH);
    c.setColor(geo::kWellColor);
    c.fillRect(strip);
    c.setColor(geo::kGold, 60);
    c.setPenSize(1.0f);
    c.strokeLine(0, geo::kStripY, geo::kWinW, geo::kStripY);

    if (!status.empty()) {
        c.setFont(Font::Body);
        c.setFontSize(geo::kStripTextSize);
        c.setColor(statusIsError ? geo::kErrorColor : geo::kDimColor);
        c.drawString(c.clipToWidth(status, geo::kStripTextMaxW).c_str(), geo::kMargin,
                     geo::kStripBaseline);
    }
}

//------------------------------------------------------------------------
std::vector<TextSlot> Panel::textSlots(Canvas &c) const
{
    std::vector<TextSlot> out;

    auto add = [&](const char *what, const std::string &text, float maxW, Font f, float size) {
        c.setFont(f);
        c.setFontSize(size);
        TextSlot s;
        s.what = what;
        s.text = text;
        s.maxW = maxW;
        s.measured = c.stringWidth(text.c_str());
        out.push_back(s);
    };

    add("title", "CPU POWER", geo::kTitleMaxW, Font::Title, geo::kTitleSize);
    add("driver name", driver, geo::kDriverMaxW, Font::Body, geo::kDriverSize);

    // The stop labels are audited against the room their NEIGHBOURS leave them, not against a
    // flat kStopSpacing: see layOutStopLabels(). A label that is over is one that has come within
    // geo::kStopLabelGap of the legend next to it.
    for (const StopLabel &s : layOutStopLabels(c, stops.labels)) {
        TextSlot t;
        t.what = "slider stop";
        t.text = s.text;
        t.maxW = s.budget;
        t.measured = s.natural;
        out.push_back(t);
    }

    add("master toggle label", master.label, geo::kToggleLabelMaxW, Font::Body,
        geo::kToggleLabelSize);
    add("turbo checkbox label", turbo.label, geo::kCheckLabelMaxW, Font::Body,
        geo::kCheckLabelSize);
    add("DAW checkbox label", daw.label, geo::kCheckLabelMaxW, Font::Body, geo::kCheckLabelSize);

    for (const Readout &r : readouts()) {
        add("readout label", r.label, geo::kReadoutLabelW, Font::Body, geo::kReadoutSize);
        add("readout value", r.value, geo::kReadoutValueMaxW, Font::Body, geo::kReadoutSize);
    }

    if (!status.empty())
        add("status strip", status, geo::kStripTextMaxW, Font::Body, geo::kStripTextSize);

    return out;
}

} // namespace cpupower
