#include "ui/IconFilterB.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

// C++ port of the approved python prototype (research/style_filter_B_
// reference.py, improved(r_levels=(3,4,6), colors=14, edge=0.45)). Ported
// step-for-step so the sheet the user approved IS what ships; deviations are
// noted inline.

namespace FUI::IconFilterB
{
    namespace
    {
        constexpr int   kRadiusSmall = 3;
        constexpr int   kRadiusMid = 4;
        constexpr int   kRadiusBig = 6;
        constexpr int   kPaletteColors = 14;
        constexpr float kEdgeLum = 0.45f;
        constexpr float kEdgeSil = 0.35f;
        constexpr std::uint8_t kInkR = 20, kInkG = 18, kInkB = 16;

        // Chebyshev erosion (separable min filter, window 2r+1) on a 0/1 mask.
        void Erode(const std::vector<std::uint8_t>& a_in, std::vector<std::uint8_t>& a_out,
                   int a_w, int a_h, int a_r)
        {
            std::vector<std::uint8_t> tmp(a_in.size());
            for (int y = 0; y < a_h; ++y) {
                const std::uint8_t* row = a_in.data() + static_cast<size_t>(y) * a_w;
                for (int x = 0; x < a_w; ++x) {
                    std::uint8_t m = 1;
                    const int x0 = (std::max)(0, x - a_r), x1 = (std::min)(a_w - 1, x + a_r);
                    if (x - a_r < 0 || x + a_r >= a_w) m = 0;   // outside = background
                    for (int k = x0; m && k <= x1; ++k) m &= row[k];
                    tmp[static_cast<size_t>(y) * a_w + x] = m;
                }
            }
            for (int y = 0; y < a_h; ++y) {
                for (int x = 0; x < a_w; ++x) {
                    std::uint8_t m = 1;
                    const int y0 = (std::max)(0, y - a_r), y1 = (std::min)(a_h - 1, y + a_r);
                    if (y - a_r < 0 || y + a_r >= a_h) m = 0;
                    for (int k = y0; m && k <= y1; ++k) m &= tmp[static_cast<size_t>(k) * a_w + x];
                    a_out[static_cast<size_t>(y) * a_w + x] = m;
                }
            }
        }

        struct Sats
        {
            // (w+1)*(h+1) inclusive-prefix tables, weighted by the item mask
            std::vector<double> w, r, g, b, l, l2;
            int                 W = 0;

            double Sum(const std::vector<double>& a_t, int a_x0, int a_y0,
                       int a_x1, int a_y1) const   // inclusive box
            {
                return a_t[static_cast<size_t>(a_y1 + 1) * W + (a_x1 + 1)] -
                       a_t[static_cast<size_t>(a_y0) * W + (a_x1 + 1)] -
                       a_t[static_cast<size_t>(a_y1 + 1) * W + a_x0] +
                       a_t[static_cast<size_t>(a_y0) * W + a_x0];
            }
        };

        void BuildSats(const std::vector<std::uint8_t>& a_px,
                       const std::vector<std::uint8_t>& a_mask,
                       int a_w, int a_h, Sats& a_s)
        {
            a_s.W = a_w + 1;
            const size_t n = static_cast<size_t>(a_w + 1) * (a_h + 1);
            for (auto* t : { &a_s.w, &a_s.r, &a_s.g, &a_s.b, &a_s.l, &a_s.l2 }) {
                t->assign(n, 0.0);
            }
            for (int y = 0; y < a_h; ++y) {
                double rw = 0, rr = 0, rg = 0, rb = 0, rl = 0, rl2 = 0;
                for (int x = 0; x < a_w; ++x) {
                    const size_t i = (static_cast<size_t>(y) * a_w + x) * 4;
                    if (a_mask[static_cast<size_t>(y) * a_w + x]) {
                        const double cr = a_px[i], cg = a_px[i + 1], cb = a_px[i + 2];
                        const double lu = (cr + cg + cb) / 3.0;
                        rw += 1.0; rr += cr; rg += cg; rb += cb;
                        rl += lu; rl2 += lu * lu;
                    }
                    const size_t o = static_cast<size_t>(y + 1) * a_s.W + (x + 1);
                    const size_t p = static_cast<size_t>(y) * a_s.W + (x + 1);
                    a_s.w[o] = a_s.w[p] + rw;   a_s.r[o] = a_s.r[p] + rr;
                    a_s.g[o] = a_s.g[p] + rg;   a_s.b[o] = a_s.b[p] + rb;
                    a_s.l[o] = a_s.l[p] + rl;   a_s.l2[o] = a_s.l2[p] + rl2;
                }
            }
        }

        // 14-colour median cut over the masked pixels of the smoothed image,
        // then nearest-palette mapping (mirrors PIL MEDIANCUT + argmin apply).
        void PaletteQuantize(std::vector<std::uint8_t>& a_px,
                             const std::vector<std::uint8_t>& a_mask, int a_w, int a_h)
        {
            std::vector<std::uint32_t> idx;
            idx.reserve(static_cast<size_t>(a_w) * a_h / 2);
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(a_w * a_h); ++i) {
                if (a_mask[i]) idx.push_back(i);
            }
            if (idx.size() < kPaletteColors) return;

            struct Box { size_t lo, hi; };
            std::vector<Box> boxes{ { 0, idx.size() } };
            auto chan = [&](std::uint32_t a_i, int a_c) {
                return a_px[static_cast<size_t>(a_i) * 4 + a_c];
            };
            while (boxes.size() < kPaletteColors) {
                int    best = -1, bestC = 0, bestR = -1;
                for (int bi = 0; bi < static_cast<int>(boxes.size()); ++bi) {
                    if (boxes[bi].hi - boxes[bi].lo < 2) continue;
                    int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
                    for (size_t k = boxes[bi].lo; k < boxes[bi].hi; ++k) {
                        for (int c = 0; c < 3; ++c) {
                            const int v = chan(idx[k], c);
                            mn[c] = (std::min)(mn[c], v); mx[c] = (std::max)(mx[c], v);
                        }
                    }
                    for (int c = 0; c < 3; ++c) {
                        if (mx[c] - mn[c] > bestR) { bestR = mx[c] - mn[c]; best = bi; bestC = c; }
                    }
                }
                if (best < 0 || bestR <= 0) break;
                Box bx = boxes[best];
                const size_t mid = (bx.lo + bx.hi) / 2;
                std::nth_element(idx.begin() + bx.lo, idx.begin() + mid, idx.begin() + bx.hi,
                    [&](std::uint32_t a, std::uint32_t b) { return chan(a, bestC) < chan(b, bestC); });
                boxes[best].hi = mid;
                boxes.push_back({ mid, bx.hi });
            }

            float pal[kPaletteColors][3] = {};
            const int np = static_cast<int>(boxes.size());
            for (int bi = 0; bi < np; ++bi) {
                double s[3] = {};
                const double n = static_cast<double>(boxes[bi].hi - boxes[bi].lo);
                for (size_t k = boxes[bi].lo; k < boxes[bi].hi; ++k) {
                    for (int c = 0; c < 3; ++c) s[c] += chan(idx[k], c);
                }
                for (int c = 0; c < 3; ++c) pal[bi][c] = static_cast<float>(s[c] / (std::max)(1.0, n));
            }
            for (const auto i : idx) {
                const size_t o = static_cast<size_t>(i) * 4;
                float bd = 1e12f; int bj = 0;
                for (int j = 0; j < np; ++j) {
                    const float dr = a_px[o] - pal[j][0], dg = a_px[o + 1] - pal[j][1],
                                db = a_px[o + 2] - pal[j][2];
                    const float d = dr * dr + dg * dg + db * db;
                    if (d < bd) { bd = d; bj = j; }
                }
                a_px[o]     = static_cast<std::uint8_t>(pal[bj][0] + 0.5f);
                a_px[o + 1] = static_cast<std::uint8_t>(pal[bj][1] + 0.5f);
                a_px[o + 2] = static_cast<std::uint8_t>(pal[bj][2] + 0.5f);
            }
        }

        // PIL FIND_EDGES kernel (8*c - 8 neighbours), clamped at 0
        float EdgeAt(const std::vector<float>& a_lum, int a_w, int a_h, int a_x, int a_y)
        {
            auto at = [&](int x, int y) {
                x = (std::max)(0, (std::min)(a_w - 1, x));
                y = (std::max)(0, (std::min)(a_h - 1, y));
                return a_lum[static_cast<size_t>(y) * a_w + x];
            };
            const float v = 8.0f * at(a_x, a_y) -
                (at(a_x - 1, a_y - 1) + at(a_x, a_y - 1) + at(a_x + 1, a_y - 1) +
                 at(a_x - 1, a_y) + at(a_x + 1, a_y) +
                 at(a_x - 1, a_y + 1) + at(a_x, a_y + 1) + at(a_x + 1, a_y + 1));
            return (std::max)(0.0f, v);
        }
    }

    void Stylize(std::vector<std::uint8_t>& a_rgba, int a_w, int a_h)
    {
        if (a_w <= 0 || a_h <= 0 ||
            a_rgba.size() < static_cast<size_t>(a_w) * a_h * 4) {
            return;
        }
        const size_t n = static_cast<size_t>(a_w) * a_h;

        std::vector<std::uint8_t> mask(n);
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            mask[i] = a_rgba[i * 4 + 3] > 8 ? 1 : 0;
            any |= mask[i] != 0;
        }
        if (!any) return;

        // thickness-adaptive radius: pixels that do not survive a Chebyshev
        // erosion of r sit within r of the silhouette -> smaller quadrants
        std::vector<std::uint8_t> er3(n), er4(n);
        Erode(mask, er3, a_w, a_h, kRadiusSmall);
        Erode(mask, er4, a_w, a_h, kRadiusMid);

        Sats s;
        BuildSats(a_rgba, mask, a_w, a_h, s);

        // alpha-aware kuwahara: 4 corner quadrants of side r+1; the one with
        // the lowest luminance variance wins; empty quadrants never win.
        std::vector<std::uint8_t> sm(n * 4);
        for (int y = 0; y < a_h; ++y) {
            for (int x = 0; x < a_w; ++x) {
                const size_t i = static_cast<size_t>(y) * a_w + x;
                const size_t o = i * 4;
                sm[o + 3] = a_rgba[o + 3];
                if (!mask[i]) {   // background stays BLACK (premultiplied-safe)
                    sm[o] = sm[o + 1] = sm[o + 2] = 0;
                    continue;
                }
                const int r = er4[i] ? kRadiusBig : (er3[i] ? kRadiusMid : kRadiusSmall);
                const int qx[4][2] = { { x - r, x }, { x, x + r }, { x - r, x }, { x, x + r } };
                const int qy[4][2] = { { y - r, y }, { y - r, y }, { y, y + r }, { y, y + r } };
                double bestVar = 1e18, m[3] = { 0, 0, 0 };
                for (int q = 0; q < 4; ++q) {
                    const int x0 = (std::max)(0, qx[q][0]), x1 = (std::min)(a_w - 1, qx[q][1]);
                    const int y0 = (std::max)(0, qy[q][0]), y1 = (std::min)(a_h - 1, qy[q][1]);
                    const double cw = s.Sum(s.w, x0, y0, x1, y1);
                    if (cw < 1.0) continue;
                    const double ml = s.Sum(s.l, x0, y0, x1, y1) / cw;
                    const double var = s.Sum(s.l2, x0, y0, x1, y1) / cw - ml * ml;
                    if (var < bestVar) {
                        bestVar = var;
                        m[0] = s.Sum(s.r, x0, y0, x1, y1) / cw;
                        m[1] = s.Sum(s.g, x0, y0, x1, y1) / cw;
                        m[2] = s.Sum(s.b, x0, y0, x1, y1) / cw;
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    sm[o + c] = static_cast<std::uint8_t>(
                        (std::min)(255.0, (std::max)(0.0, m[c] + 0.5)));
                }
            }
        }

        PaletteQuantize(sm, mask, a_w, a_h);

        // outline: luminance edges of the palettized image + silhouette ring
        std::vector<float> lum(n), sil(n);
        for (size_t i = 0; i < n; ++i) {
            lum[i] = 0.299f * sm[i * 4] + 0.587f * sm[i * 4 + 1] + 0.114f * sm[i * 4 + 2];
            sil[i] = mask[i] ? 255.0f : 0.0f;
        }
        for (int y = 0; y < a_h; ++y) {
            for (int x = 0; x < a_w; ++x) {
                const size_t i = static_cast<size_t>(y) * a_w + x;
                const float e = (std::min)(255.0f, EdgeAt(lum, a_w, a_h, x, y)) / 255.0f;
                const float es = (std::min)(255.0f, EdgeAt(sil, a_w, a_h, x, y)) / 255.0f;
                float f = (std::min)(1.0f, e * 1.6f) * kEdgeLum +
                          (std::min)(1.0f, es) * kEdgeSil;
                f = (std::min)(1.0f, f);
                if (f <= 0.0f) continue;
                const size_t o = i * 4;
                sm[o]     = static_cast<std::uint8_t>(sm[o] * (1 - f) + kInkR * f + 0.5f);
                sm[o + 1] = static_cast<std::uint8_t>(sm[o + 1] * (1 - f) + kInkG * f + 0.5f);
                sm[o + 2] = static_cast<std::uint8_t>(sm[o + 2] * (1 - f) + kInkB * f + 0.5f);
            }
        }

        a_rgba.swap(sm);
    }
}
