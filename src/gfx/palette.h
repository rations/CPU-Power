// The palette, in one place.
//
// Carried over from the rations-amp plug-in editor so this window reads as part of the same
// family: the same near-black ground, the same green accent. geometry.h re-exports every name
// below into `geo`, so layout code says geo::kAccent and there is still exactly one definition.
//
// 0xRRGGBB throughout, matching Canvas::setColor.

#pragma once

#include <cstdint>

namespace cpupower
{
namespace pal
{

constexpr uint32_t kBgColor = 0x121011;   // the window ground
constexpr uint32_t kFaceColor = 0x1E1C1D; // panels and control troughs sitting on it
constexpr uint32_t kWellColor = 0x0C0B0B; // sunk areas: the slider track, the status strip
constexpr uint32_t kGold = 0xB88B4C;      // piping / hairlines, sampled from the amp art
constexpr uint32_t kTextColor = 0xFFFFFF; // labels
constexpr uint32_t kDimColor = 0x9A9490;  // secondary text

// The accent, and the whole reason a stop that is merely SELECTED looks different from one that
// is actually APPLIED: selection is drawn dim, the applied state is drawn in the accent. A window
// that shows a setting the machine is not in is a lie, so the two states
// cannot share a colour.
constexpr uint32_t kAccent = 0x3FD05A;
constexpr uint32_t kAccentBright = 0x7FE89A;

// A knob the running kernel does not offer is drawn in this and labelled, never silently hidden
//. It has to read as "not available here", not as "off".
constexpr uint32_t kDisabledColor = 0x4A4648;

// Reserved for the status strip when a write was rejected and for the Maximum stop's tick, which
// is the one setting that ignores the machine's own power management.
constexpr uint32_t kWarnColor = 0xE8A33F;
constexpr uint32_t kErrorColor = 0xFF3B30;

} // namespace pal
} // namespace cpupower
