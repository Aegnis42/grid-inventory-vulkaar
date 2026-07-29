#pragma once

#include <cstdint>
#include <vector>

namespace FUI::IconFilterB
{
    // "Style B" (user-approved 2026-07-29, spec: research/style_filter_B_
    // reference.py): flat-illustration stylization of a realistic capture.
    //   1) alpha-aware kuwahara, thickness-adaptive radius 3/4/6 -- the
    //      background never bleeds into thin parts (hilts, bottle necks)
    //   2) 14-colour median-cut palette drawn from the ITEM MASK only
    //   3) luminance-edge + silhouette outline, ink colour (20,18,16)
    // In-place on straight RGBA pixels; alpha is left untouched, and
    // transparent texels come out black (the premultiplied-safe property the
    // draw passes rely on).
    void Stylize(std::vector<std::uint8_t>& a_rgba, int a_w, int a_h);
}
