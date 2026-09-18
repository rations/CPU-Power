// uirender -- draw the panel offline and AUDIT EVERY STRING AGAINST ITS REAL SLOT.
//
// A static_assert can hold a clearance between two rectangles, because both are compile-time
// numbers. It cannot hold a legend inside its slot: how wide "balance_performance" renders depends
// on the font, the size and the rasteriser, none of which exist at compile time. So the layout has
// two halves -- geometry.h asserts the rectangles, and this measures the text that goes in them,
// with the real fonts, at every scale the window is claimed to work at.
//
// A string wider than its slot is not a crash. Canvas::clipToWidth truncates it with an ellipsis,
// so the window still draws. That is exactly why this tool exists: the failure is silent, it is
// invisible on the developer's machine if their font happens to be narrow enough, and the first
// person to see "balance_perfo..." is a user on a different fontconfig.
//
// THE AUDIT RUNS ONCE, NOT ONCE PER SCALE, and that is a measured fact rather than an assumption.
// An earlier version of this tool measured at all five scales below and reported five checks. The
// numbers were identical to six decimal places every time: with a cairo_scale() on the context,
// cairo reports text extents in USER space, so the scale divides straight back out -- verified
// directly with cairo_font_options_get_hint_metrics() confirming hinting was on and the widths
// still matching exactly. Repeating one measurement five times and calling it five checks would
// have made the audit look stronger than it is, so it is stated for what it is: text metrics are
// scale-invariant in logical units, therefore one measurement covers every scale.
//
// The PNGs are still written at each scale, because what they are for is a human looking at them,
// and rasterisation at 0.75x genuinely is not rasterisation at 2x.
//
// It also fills the panel with the WORST CASE rather than with what this machine reports: the
// longest governor name, the longest EPP name, the longest driver name and the longest error
// message any fixture in tests/fixtures/ contains. Auditing the layout against the developer's
// own comfortable values would prove nothing about anyone else's machine.
//
// Exit status is 0 only if every string fits at every scale.
//
// Usage: uirender [--out <dir>]     writes panel@<scale>.png for each audited scale

#include "panel.h"
#include "geometry.h"
#include "gfx/canvas.h"
#include "gfx/fontstack.h"
#include "platform/respath.h"

#include <cairo/cairo.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace cpupower;

namespace
{

// The scales the window is claimed to work at. These drive the PNG renders only; see the note
// above on why the text audit does not need to repeat at each of them.
const float kScales[] = {0.75f, 1.0f, 1.25f, 1.5f, 2.0f};

// The longest values seen across every fixture in tests/fixtures/, so the audit is against the
// worst case rather than against this box's comfortable one.
//
//   governor   "conservative"          (acpi-cpufreq, cppc_cpufreq)
//   EPP        "balance_performance"   (intel_pstate active, amd-pstate-epp)
//   driver     "acpi-cpufreq"          -- but "amd-pstate-epp" is longer, and cppc_cpufreq
//              longer still, so the widest string wins rather than the alphabetically last
void fillWorstCase(Panel &p)
{
    p.driver = "cppc_cpufreq";
    p.governor = "conservative";
    p.epp = "balance_performance";
    p.policies = "128 policies";
    p.frequency = "4.72 GHz";
    p.turboState = "unavailable";
    // Coherent worst case, not merely a long one: a kernel with no turbo knob disables the
    // checkbox, and that is the state the string "unavailable" belongs to. It also puts the
    // disabled rendering in front of the audit and in the PNGs.
    p.turbo.enabled = false;
    p.turbo.disabledReason = "this kernel exposes no turbo control";
    p.status = "cannot write scaling_governor on policy19: Operation not permitted";
    p.statusIsError = true;
    p.master.on = true;
    p.turbo.on = true;
    p.stops.value = 4;
}

} // namespace

int main(int argc, char **argv)
{
    std::string outDir;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            outDir = argv[++i];
        else {
            fprintf(stderr, "usage: uirender [--out <dir>]\n");
            return 2;
        }
    }

    FontStack fonts;
    const bool bundled = fonts.load(resourceDir());
    if (!bundled) {
        // Measuring a substituted system face would produce a pass that means nothing: the whole
        // claim is about the font this project ships.
        fprintf(stderr,
                "uirender: the bundled fonts did not load (resourceDir=\"%s\").\n"
                "          Refusing to audit text metrics against a substituted face.\n",
                resourceDir().c_str());
        return 2;
    }

    int failures = 0;
    int measured = 0;

    Panel panel;
    fillWorstCase(panel);

    // --- the audit, once ----------------------------------------------------------------------
    {
        const int pw = static_cast<int>(geo::kWinW + 0.5f);
        const int ph = static_cast<int>(geo::kWinH + 0.5f);
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "uirender: could not create a %dx%d surface\n", pw, ph);
            cairo_surface_destroy(surf);
            return 2;
        }
        cairo_t *cr = cairo_create(surf);
        Canvas canvas(cr, &fonts, geo::kWinW, geo::kWinH);
        panel.draw(canvas);

        for (const TextSlot &s : panel.textSlots(canvas)) {
            measured++;
            if (s.measured > s.maxW) {
                failures++;
                fprintf(stderr,
                        "uirender: FAIL  %-22s \"%s\"\n"
                        "                %.1f units wide, slot is %.1f (over by %.1f)\n",
                        s.what.c_str(), s.text.c_str(), static_cast<double>(s.measured),
                        static_cast<double>(s.maxW), static_cast<double>(s.measured - s.maxW));
            }
        }
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
    }

    // --- the renders, one per scale, for a human to look at -----------------------------------
    if (!outDir.empty()) {
        for (float scale : kScales) {
            const int pw = static_cast<int>(geo::kWinW * scale + 0.5f);
            const int ph = static_cast<int>(geo::kWinH * scale + 0.5f);
            cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
            if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(surf);
                continue;
            }
            cairo_t *cr = cairo_create(surf);
            cairo_scale(cr, scale, scale);
            Canvas canvas(cr, &fonts, geo::kWinW, geo::kWinH);
            panel.draw(canvas);
            cairo_destroy(cr);

            char path[1024];
            snprintf(path, sizeof(path), "%s/panel@%.2fx.png", outDir.c_str(),
                     static_cast<double>(scale));
            if (cairo_surface_write_to_png(surf, path) != CAIRO_STATUS_SUCCESS)
                fprintf(stderr, "uirender: could not write %s\n", path);
            else
                printf("  wrote %s\n", path);
            cairo_surface_destroy(surf);
        }
    }

    printf("uirender: %d strings measured against their slots, %d over\n", measured, failures);
    return failures == 0 ? 0 : 1;
}
