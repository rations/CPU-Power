// See widgets.h.

#include "widgets.h"

#include "../geometry.h"

#include <algorithm>
#include <cmath>

namespace cpupower
{
namespace
{

float stopX(int i)
{
    return geo::kTrackX + static_cast<float>(i) * geo::kStopSpacing;
}

uint32_t inkFor(bool enabled, bool on)
{
    if (!enabled)
        return geo::kDisabledColor;
    return on ? geo::kAccent : geo::kDimColor;
}

} // namespace

//------------------------------------------------------------------------
std::vector<StopLabel> layOutStopLabels(Canvas &c, const std::vector<std::string> &labels)
{
    const int count = static_cast<int>(labels.size());
    std::vector<StopLabel> out(static_cast<size_t>(count < 0 ? 0 : count));
    if (count == 0)
        return out;

    c.setFont(Font::Body);
    c.setFontSize(geo::kStopLabelSize);

    // Pass one: where each label would sit if it had the window to itself. Centred on its stop,
    // except that the first and last stops sit ON THE ENDS OF THE TRACK, so a centred label
    // there would hang off the edge -- those two are pulled back inside the margin, which is
    // what makes their inner neighbour the tight pair rather than them.
    std::vector<float> lo(out.size()), hi(out.size());
    for (int i = 0; i < count; i++) {
        const size_t k = static_cast<size_t>(i);
        out[k].natural = c.stringWidth(labels[k].c_str());
        float x = stopX(i) - out[k].natural * 0.5f;
        x = std::max(x, geo::kMargin);
        x = std::min(x, geo::kWinW - geo::kMargin - out[k].natural);
        lo[k] = x;
        hi[k] = x + out[k].natural;
    }

    // Pass two: the room each label actually has is bounded by where its neighbours WANT to be,
    // less the clearance. Using the pass-one extents rather than the final ones keeps this
    // non-circular: a clipped label only ever gives space back, never takes it.
    for (int i = 0; i < count; i++) {
        const size_t k = static_cast<size_t>(i);
        const float left =
            (i == 0) ? geo::kMargin : std::max(geo::kMargin, hi[k - 1] + geo::kStopLabelGap);
        const float right =
            (i == count - 1) ? geo::kWinW - geo::kMargin
                             : std::min(geo::kWinW - geo::kMargin, lo[k + 1] - geo::kStopLabelGap);

        out[k].budget = std::max(0.0f, right - left);
        out[k].text = c.clipToWidth(labels[k], out[k].budget);

        const float w = c.stringWidth(out[k].text.c_str());
        float x = stopX(i) - w * 0.5f;
        x = std::max(x, left);
        x = std::min(x, right - w);
        out[k].x = x;
    }
    return out;
}

//------------------------------------------------------------------------
void Slider::draw(Canvas &c) const
{
    const int count = static_cast<int>(labels.size());
    if (count < 2)
        return;

    // The labels first, so the thumb is drawn over them rather than under.
    const std::vector<StopLabel> laid = layOutStopLabels(c, labels);
    c.setFont(Font::Body);
    c.setFontSize(geo::kStopLabelSize);
    for (int i = 0; i < count; i++) {
        const bool isValue = (i == value);
        if (!enabled)
            c.setColor(geo::kDisabledColor);
        else if (isValue)
            c.setColor(geo::kAccentBright);
        else if (i == hover)
            c.setColor(geo::kTextColor);
        else
            c.setColor(geo::kDimColor);

        const StopLabel &s = laid[static_cast<size_t>(i)];
        c.drawString(s.text.c_str(), s.x, geo::kStopLabelBaseline);
    }

    // The track: a sunk well, filled up to the current stop.
    const Rect track(geo::kTrackX, geo::kTrackY, geo::kTrackW, geo::kTrackH);
    c.setColor(geo::kWellColor);
    c.fillRoundRect(track, geo::kTrackH * 0.5f);

    if (enabled && value > 0) {
        const Rect filled(geo::kTrackX, geo::kTrackY, stopX(value) - geo::kTrackX, geo::kTrackH);
        c.setColor(geo::kAccent, 200);
        c.fillRoundRect(filled, geo::kTrackH * 0.5f);
    }

    // The detent ticks, so the stops are visible as positions and not only as words.
    c.setPenSize(1.0f);
    for (int i = 0; i < count; i++) {
        c.setColor(enabled ? geo::kGold : geo::kDisabledColor, 180);
        const float x = stopX(i);
        c.strokeLine(x, geo::kTrackY + geo::kTrackH + 2.0f, x,
                     geo::kTrackY + geo::kTrackH + geo::kTickH);
    }

    // The thumb.
    const float cx = stopX(value);
    const float cy = geo::kTrackY + geo::kTrackH * 0.5f;
    c.setColor(geo::kFaceColor);
    c.fillEllipse(cx, cy, geo::kThumbR, geo::kThumbR);
    c.setColor(enabled ? (dragging ? geo::kAccentBright : geo::kAccent) : geo::kDisabledColor);
    c.setPenSize(2.0f);
    c.strokeEllipse(cx, cy, geo::kThumbR - 1.0f, geo::kThumbR - 1.0f);
}

//------------------------------------------------------------------------
int Slider::stopAt(float x) const
{
    const int count = static_cast<int>(labels.size());
    if (count < 2)
        return 0;
    const float t = (x - geo::kTrackX) / geo::kStopSpacing;
    int i = static_cast<int>(std::lround(t));
    return std::max(0, std::min(count - 1, i));
}

bool Slider::hit(float x, float y) const
{
    if (!enabled)
        return false;
    return y >= geo::kSliderHitTop && y <= geo::kSliderHitBot && x >= geo::kTrackX - geo::kThumbR &&
           x <= geo::kTrackX + geo::kTrackW + geo::kThumbR;
}

bool Slider::step(int delta)
{
    if (!enabled)
        return false;
    const int count = static_cast<int>(labels.size());
    const int next = std::max(0, std::min(count - 1, value + delta));
    if (next == value)
        return false;
    value = next;
    return true;
}

//------------------------------------------------------------------------
void Toggle::draw(Canvas &c) const
{
    const Rect r(geo::kToggleX, geo::kToggleY, geo::kToggleW, geo::kToggleH);
    const float radius = geo::kToggleH * 0.5f;

    c.setColor(geo::kWellColor);
    c.fillRoundRect(r, radius);
    if (enabled && on) {
        c.setColor(geo::kAccent, 190);
        c.fillRoundRect(r, radius);
    }
    c.setColor(inkFor(enabled, on), hover && enabled ? 255 : 170);
    c.setPenSize(1.0f);
    c.strokeRoundRect(r, radius);

    // The knuckle, at whichever end the state is.
    const float knobR = radius - 3.0f;
    const float cx = on ? r.right() - radius : r.x + radius;
    c.setColor(enabled ? geo::kTextColor : geo::kDisabledColor);
    c.fillEllipse(cx, r.centerY(), knobR, knobR);

    c.setFont(Font::Body);
    c.setFontSize(geo::kToggleLabelSize);
    c.setColor(enabled ? geo::kTextColor : geo::kDisabledColor);
    const std::string text = c.clipToWidth(label, geo::kToggleLabelMaxW);
    c.drawString(text.c_str(), geo::kToggleLabelX, r.centerY() + geo::kToggleLabelSize * 0.36f);
}

bool Toggle::hit(float x, float y) const
{
    if (!enabled)
        return false;
    const Rect r(geo::kToggleX, geo::kToggleY, geo::kToggleW + 8.0f + geo::kToggleLabelMaxW,
                 geo::kToggleH);
    return r.contains(x, y);
}

//------------------------------------------------------------------------
namespace
{
float checkX(int column)
{
    return column == 0 ? geo::kCheckCol0X : geo::kCheckCol1X;
}
} // namespace

void Checkbox::draw(Canvas &c) const
{
    const float x = checkX(column);
    const Rect box(x, geo::kCheckY, geo::kCheckBox, geo::kCheckBox);

    c.setColor(geo::kWellColor);
    c.fillRoundRect(box, 3.0f);
    c.setColor(inkFor(enabled, on), hover && enabled ? 255 : 170);
    c.setPenSize(1.0f);
    c.strokeRoundRect(box, 3.0f);

    if (on && enabled) {
        // A tick, drawn as two strokes rather than a glyph: a checkmark from the body font would
        // depend on the font having one.
        c.setColor(geo::kAccentBright);
        c.setPenSize(2.0f);
        const float l = box.x + 3.0f, t = box.y + 3.0f, r = box.right() - 3.0f,
                    b = box.bottom() - 3.0f;
        c.strokeLine(l, (t + b) * 0.5f, (l + r) * 0.5f - 0.5f, b - 1.0f);
        c.strokeLine((l + r) * 0.5f - 0.5f, b - 1.0f, r, t);
    }

    c.setFont(Font::Body);
    c.setFontSize(geo::kCheckLabelSize);
    c.setColor(enabled ? geo::kTextColor : geo::kDisabledColor);
    const std::string text = c.clipToWidth(label, geo::kCheckLabelMaxW);
    c.drawString(text.c_str(), box.right() + geo::kCheckLabelDX,
                 box.centerY() + geo::kCheckLabelSize * 0.36f);
}

bool Checkbox::hit(float x, float y) const
{
    if (!enabled)
        return false;
    const Rect r(checkX(column), geo::kCheckY - 4.0f, geo::kCheckHitW, geo::kCheckHitH);
    return r.contains(x, y);
}

//------------------------------------------------------------------------
void Readout::draw(Canvas &c) const
{
    const float x = column == 0 ? geo::kMargin : geo::kReadoutCol1X;
    const float y = geo::kReadoutY + static_cast<float>(row) * geo::kReadoutRowH;

    c.setFont(Font::Body);
    c.setFontSize(geo::kReadoutSize);

    c.setColor(geo::kDimColor);
    const std::string lab = c.clipToWidth(label, geo::kReadoutLabelW);
    c.drawString(lab.c_str(), x, y);

    c.setColor(applied ? geo::kAccentBright : geo::kTextColor);
    const std::string val = c.clipToWidth(value, geo::kReadoutValueMaxW);
    c.drawString(val.c_str(), x + geo::kReadoutLabelW, y);
}

} // namespace cpupower
