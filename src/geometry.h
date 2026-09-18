// CPU-Power window geometry.
//
// EVERYTHING HERE IS IN LOGICAL UNITS. The window applies one cairo_scale(s, s) at compose time
// and divides mouse coordinates by s before hit-testing, so a scale factor must never be baked
// into a constant here. Change the scale and every number below stays exactly as it is.
//
// Every clearance between two parts is a static_assert. A comment claiming two things do not
// overlap stops being true silently the moment somebody nudges a constant; a static_assert fails
// the build instead. Text is a different problem -- a string's width depends on the font, which
// is not a compile-time quantity -- so strings are measured against their real slots at run time
// by tools/uirender, which fails if any of them overflows.

#pragma once

#include "gfx/palette.h"

namespace cpupower
{
namespace geo
{

// Re-export the palette so layout code says geo::kAccent and there is still one definition.
using pal::kAccent;
using pal::kAccentBright;
using pal::kBgColor;
using pal::kDimColor;
using pal::kDisabledColor;
using pal::kErrorColor;
using pal::kFaceColor;
using pal::kGold;
using pal::kTextColor;
using pal::kWarnColor;
using pal::kWellColor;

// --- The window -------------------------------------------------------------
constexpr float kWinW = 420.0f;
// 285, not 300: the readout's last row ends at kReadoutY + 2*kReadoutRowH and the strip starts
// at kWinH - kStripH, so a taller window is dead space rather than breathing room. The
// static_assert below is what keeps the two from meeting if either block grows.
constexpr float kWinH = 285.0f;
constexpr float kMargin = 16.0f;

// The scales the layout is audited at. 0.75 is a 1024x768 laptop, 2.0 a HiDPI panel.
constexpr float kScaleMin = 0.75f;
constexpr float kScaleMax = 2.0f;

// --- Title row --------------------------------------------------------------
constexpr float kTitleBaseline = 30.0f;
constexpr float kTitleSize = 15.0f;
// The driver name sits right-aligned on the same baseline. It is read from the kernel, so its
// length is not ours to choose: uirender measures the longest name any fixture reports.
constexpr float kDriverSize = 10.0f;
constexpr float kDriverRightX = kWinW - kMargin;
// Where the driver text may start before it collides with the title. The title is drawn from
// kMargin, so this is the widest the title is allowed to render.
constexpr float kTitleMaxW = 150.0f;
constexpr float kDriverMaxW = kDriverRightX - (kMargin + kTitleMaxW) - 8.0f;
static_assert(kDriverMaxW > 60.0f, "no room left for the driver name beside the title");

// A hairline under the title row, the one piece of piping this window borrows from the amp.
constexpr float kRuleY = 42.0f;
static_assert(kRuleY > kTitleBaseline, "the title rule is drawn through the title");

// --- The slider -------------------------------------------------------------
// Five named stops -- the kernel's power knobs are enumerations, not a continuum, so the slider
// is detented rather than proportional. The track spans the full content width; the stops are the
// centres of the five equal segments' boundaries, so the first and last sit on the track's ends.
constexpr int kStopCount = 5;
constexpr float kStopLabelBaseline = 66.0f;
constexpr float kStopLabelSize = 9.0f;
// The clearance every stop label must keep from its neighbours' rendered edges. The labels are
// the profile names in full, so their widths are wildly uneven -- "Max" is 17 units and
// "Balanced-Performance" is 92 -- and a uniform per-stop budget of kStopSpacing is the wrong
// model for centre-aligned text: it charges the long label for space the short ones next to it
// are never going to use. What actually has to hold is that no two adjacent labels touch, so
// that is what tools/uirender measures, with the real fonts. This is the margin it demands on
// top of touching, so the audit fails on a machine whose font is wider than ours BEFORE anyone
// sees two legends run together.
constexpr float kStopLabelGap = 10.0f;
constexpr float kTrackY = 82.0f;
constexpr float kTrackH = 6.0f;
constexpr float kTrackX = kMargin + 10.0f;
constexpr float kTrackW = kWinW - 2.0f * kTrackX;
constexpr float kStopSpacing = kTrackW / (kStopCount - 1);
constexpr float kTickH = 10.0f;
constexpr float kThumbR = 9.0f;

static_assert(kTrackY > kRuleY + 8.0f, "the slider track has reached the title rule");
static_assert(kStopLabelBaseline < kTrackY - kThumbR,
              "a stop label is drawn across the slider thumb");
// The thumb must not leave the window when it is parked on the outermost stops.
static_assert(kTrackX - kThumbR > 0.0f, "the thumb hangs off the left edge at the first stop");
static_assert(kTrackX + kTrackW + kThumbR < kWinW,
              "the thumb hangs off the right edge at the last stop");
// Two adjacent thumbs' worth of width has to fit between stops, or the detents overlap and a
// click lands ambiguously between two of them.
static_assert(kStopSpacing > 2.0f * kThumbR, "the slider stops are closer together than the thumb");

// The generous vertical band a click anywhere in counts as hitting the slider.
constexpr float kSliderHitTop = kTrackY - kThumbR - 4.0f;
constexpr float kSliderHitBot = kTrackY + kTrackH + kThumbR + 4.0f;

// --- Master toggle ----------------------------------------------------------
// Off means the machine is exactly as the kernel left it, so this is the
// first control under the slider and the only one drawn at full size.
constexpr float kToggleY = 118.0f;
constexpr float kToggleW = 46.0f;
constexpr float kToggleH = 24.0f;
constexpr float kToggleX = kMargin;
constexpr float kToggleLabelX = kToggleX + kToggleW + 12.0f;
constexpr float kToggleLabelSize = 12.0f;
constexpr float kToggleLabelMaxW = kWinW - kMargin - kToggleLabelX;
static_assert(kToggleY > kSliderHitBot, "the master toggle overlaps the slider's hit band");
static_assert(kToggleLabelMaxW > 80.0f, "no room for the master toggle's label");

// --- The two checkboxes -----------------------------------------------------
// Turbo and DAW mode are independent of the slider and of each other: DAW mode is about C-state
// exit latency, not clock speed, so pinning one without the other is a real combination.
constexpr float kCheckY = 160.0f;
constexpr float kCheckBox = 15.0f;
constexpr float kCheckLabelDX = 9.0f;
constexpr float kCheckLabelSize = 11.0f;
constexpr float kCheckCol0X = kMargin;
constexpr float kCheckCol1X = kWinW * 0.5f + 6.0f;
constexpr float kCheckLabelMaxW = kWinW - kMargin - (kCheckCol1X + kCheckBox + kCheckLabelDX);
static_assert(kCheckY > kToggleY + kToggleH, "the checkbox row is drawn across the master toggle");
static_assert(kCheckCol1X > kCheckCol0X + kCheckBox + kCheckLabelDX + 90.0f,
              "the first checkbox's label runs into the second checkbox");
static_assert(kCheckLabelMaxW > 70.0f, "no room for the second checkbox's label");

// The hit rect for a checkbox covers its label too, which is what a user expects and what makes
// a 15-unit box usable at 0.75 scale.
constexpr float kCheckHitW = 120.0f;
constexpr float kCheckHitH = 22.0f;
static_assert(kCheckCol0X + kCheckHitW < kCheckCol1X,
              "the two checkboxes' hit rects overlap, so a click is ambiguous");

// --- The readout ------------------------------------------------------------
// What the KERNEL reports, re-read every tick -- never what was last requested. Two rows: the
// resolved governor and EPP, then the live numbers.
constexpr float kReadoutY = 196.0f;
constexpr float kReadoutRowH = 16.0f;
constexpr float kReadoutSize = 10.0f;
constexpr float kReadoutLabelW = 62.0f;
constexpr float kReadoutValueMaxW = kWinW * 0.5f - kMargin - kReadoutLabelW - 6.0f;
constexpr float kReadoutCol1X = kWinW * 0.5f + 6.0f;
static_assert(kReadoutY > kCheckY + kCheckHitH, "the readout is drawn across the checkbox row");
static_assert(kReadoutValueMaxW > 55.0f, "no room for a governor name in the readout");

// --- Status strip -----------------------------------------------------------
// The bottom of the window, above nothing. Errors from the kernel land here verbatim, so it is
// as wide as the window and clipped rather than wrapped.
constexpr float kStripH = 26.0f;
constexpr float kStripY = kWinH - kStripH;
constexpr float kStripTextSize = 10.0f;
constexpr float kStripBaseline = kStripY + 17.0f;
constexpr float kStripTextMaxW = kWinW - 2.0f * kMargin;
static_assert(kStripY > kReadoutY + 2.0f * kReadoutRowH,
              "the status strip has risen into the readout");
static_assert(kStripBaseline < kWinH, "the status strip's text is below the window");

} // namespace geo
} // namespace cpupower
