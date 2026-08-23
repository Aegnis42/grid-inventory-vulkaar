#include "PCH.h"

#include "ui/Badges.h"

#include "api/HostApi.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"

#include <imgui.h>

#include <algorithm>
#include <array>

namespace FUI::Badges
{
    namespace
    {
        // ---- confirmed geometry (PLAN_INSTANCE 3-A) -----------------------
        constexpr float kDiameter = 0.86f;   // × cell
        constexpr float kPitch    = 0.96f;   // × cell, centre to centre

        // Rows per (tile, badge count). A row's value is how many badges sit in
        // it; each row is then centred on the tile's own centre line, which is
        // what puts a lone badge on a 2-wide tile BETWEEN the columns.
        //
        // ★The table is the truth. It has a rule hiding in it -- a 2-wide tile
        // keeps a single centre column while the badges still fit vertically,
        // then splits into two -- but 2×3 n=3 breaks that rule ([2,1]), so the
        // code only ever looks the answer up.
        struct Entry
        {
            std::uint8_t w, h, n;
            std::array<std::uint8_t, 4> rows;   // 0-terminated
        };

        constexpr Entry kTable[]{
            { 1, 1, 1, { 1 } },

            { 1, 2, 1, { 1 } },
            { 1, 2, 2, { 1, 1 } },

            { 1, 3, 1, { 1 } },
            { 1, 3, 2, { 1, 1 } },
            { 1, 3, 3, { 1, 1, 1 } },

            { 1, 4, 1, { 1 } },
            { 1, 4, 2, { 1, 1 } },
            { 1, 4, 3, { 1, 1, 1 } },
            { 1, 4, 4, { 1, 1, 1, 1 } },

            { 2, 1, 1, { 1 } },
            { 2, 1, 2, { 2 } },

            { 2, 2, 1, { 1 } },
            { 2, 2, 2, { 1, 1 } },
            { 2, 2, 3, { 2, 1 } },
            { 2, 2, 4, { 2, 2 } },

            { 2, 3, 1, { 1 } },
            { 2, 3, 2, { 1, 1 } },
            { 2, 3, 3, { 2, 1 } },
            { 2, 3, 4, { 2, 2 } },
            { 2, 3, 5, { 2, 1, 2 } },   // dice five: the middle one is half-cell
            { 2, 3, 6, { 2, 2, 2 } },

            { 2, 4, 1, { 1 } },
            { 2, 4, 2, { 1, 1 } },
            { 2, 4, 3, { 1, 1, 1 } },
            { 2, 4, 4, { 1, 1, 1, 1 } },
            { 2, 4, 5, { 2, 1, 2 } },
            { 2, 4, 6, { 2, 2, 2 } },
        };

        // Approximation for a (tile, n) pair the table does not carry. This is
        // reachable in normal play, not just from exotic tiles: the user can
        // retune an item's footprint in EDIT after its sockets were rolled, so
        // a 3-socket 2×2 helmet can become a 3-socket 1×2 one.
        void Approximate(int a_w, int a_n, std::array<std::uint8_t, 4>& a_out)
        {
            a_out = {};
            const int perRow = (a_w >= 2 && a_n >= 5) ? 2 : 1;
            int left = a_n, r = 0;
            while (left > 0 && r < 4) {
                const auto take = static_cast<std::uint8_t>((std::min)(left, perRow));
                a_out[r++] = take;
                left -= take;
            }
        }

        const std::array<std::uint8_t, 4>* Lookup(int a_w, int a_h, int a_n)
        {
            for (const auto& e : kTable) {
                if (e.w == a_w && e.h == a_h && e.n == a_n) return &e.rows;
            }
            return nullptr;
        }

        bool CellAt(const TileShape& a_s, int a_x, int a_y)
        {
            if (a_x < 0 || a_y < 0 || a_x >= a_s.w || a_y >= a_s.h) return false;
            if (a_x > 7 || a_y > 7) return true;   // outside the bitmask's reach
            return (a_s.cells >> (a_y * 8 + a_x)) & 1ull;
        }

        // A badge centre is given in CELL units. Half-cell coordinates are only
        // legal when both neighbouring cells exist; otherwise snap onto whichever
        // side is real, so a polyomino hole never gets a badge floating over it.
        bool Snap(const TileShape& a_s, float& a_cx, float& a_cy)
        {
            const auto axis = [](float v, int lo, int hi, bool loOk, bool hiOk) -> float {
                const bool half = (v - std::floor(v)) < 0.01f;   // integral = between cells
                if (!half) return v;
                if (loOk && hiOk) return v;
                if (loOk) return static_cast<float>(lo) + 0.5f;
                if (hiOk) return static_cast<float>(hi) + 0.5f;
                return -1.0f;
            };

            const int   y  = static_cast<int>(std::floor(a_cy));
            const bool  hx = (a_cx - std::floor(a_cx)) < 0.01f;
            if (hx) {
                const int l = static_cast<int>(a_cx) - 1, r = static_cast<int>(a_cx);
                a_cx = axis(a_cx, l, r, CellAt(a_s, l, y), CellAt(a_s, r, y));
                if (a_cx < 0.0f) return false;
            }
            const int  x  = static_cast<int>(std::floor(a_cx));
            const bool hy = (a_cy - std::floor(a_cy)) < 0.01f;
            if (hy) {
                const int t = static_cast<int>(a_cy) - 1, b = static_cast<int>(a_cy);
                a_cy = axis(a_cy, t, b, CellAt(a_s, x, t), CellAt(a_s, x, b));
                if (a_cy < 0.0f) return false;
            }
            return CellAt(a_s, static_cast<int>(a_cx), static_cast<int>(a_cy));
        }

        // ---- style B: engraved well ---------------------------------------
        //
        //  A recess, not a button: a dark bowl with a bright lower-right rim and
        //  a dark upper-left one, so it reads as cut INTO the item plate. Subtle
        //  by design -- badges are always visible, and anything glossier turns
        //  a full grid into confetti. Hovering lifts the alpha instead.
        void DrawWell(ImDrawList* a_dl, const ImVec2& a_c, float a_r, float a_alpha)
        {
            const auto A = [&](float f) { return static_cast<int>(255.0f * f * a_alpha); };
            a_dl->AddCircleFilled(a_c, a_r, IM_COL32(10, 11, 13, A(0.78f)), 0);
            // rim: two half-arcs, light from the upper left
            a_dl->PathArcTo(a_c, a_r - 0.5f, 2.36f, 5.50f, 0);   // lower-right = lit
            a_dl->PathStroke(IM_COL32(150, 156, 166, A(0.55f)), 0, 1.0f);
            a_dl->PathArcTo(a_c, a_r - 0.5f, 5.50f, 8.64f, 0);   // upper-left = shadow
            a_dl->PathStroke(IM_COL32(0, 0, 0, A(0.70f)), 0, 1.2f);
        }
    }

    bool Draw(ImDrawList* a_dl, const ImVec2& a_px, float a_pw, float a_ph,
              const TileShape& a_shape, std::uint32_t a_owner, std::uint32_t a_base,
              std::uint16_t a_uid, bool a_hovered)
    {
        if (!a_dl || a_shape.w <= 0 || a_shape.h <= 0) return false;
        // ★호버 중에만 표시 (원안 유지, 2026-07-30 사용자 재확인). 항상 표시로
        //  바꿔 봤으나 격자가 시끄러워진다 — 확인은 마우스를 올려서 한다.
        if (!a_hovered) return false;

        // HOT PATH: one hash lookup inside the provider, then nothing when it
        // has no opinion -- which is every tile for a player without the mod.
        const GridInvAPI::ItemKey key{ a_owner, a_base, a_uid, 0, 0 };
        const auto* ovl = HostApi::Overlay(key);
        if (!ovl || ovl->count == 0) return false;

        const int n = (std::min)(static_cast<int>(ovl->count),
                                static_cast<int>(GridInvAPI::kMaxBadges));

        std::array<std::uint8_t, 4> rowsBuf{};
        if (const auto* found = Lookup(a_shape.w, a_shape.h, n)) {
            rowsBuf = *found;
        } else {
            Approximate(a_shape.w, n, rowsBuf);
            logger::debug("[BADGE] no table entry for {}x{} n={} -- approximated",
                          a_shape.w, a_shape.h, n);
        }

        int nRows = 0;
        for (const auto r : rowsBuf) {
            if (r == 0) break;
            ++nRows;
        }
        if (nRows == 0) return false;

        // cell size in pixels (the tile box is w × h cells)
        const float cw = a_pw / static_cast<float>(a_shape.w);
        const float ch = a_ph / static_cast<float>(a_shape.h);
        const float cs = (std::min)(cw, ch);
        const float rad = cs * kDiameter * 0.5f;
        const float alpha = a_hovered ? 1.0f : 0.72f;

        // rows are centred on the tile's own centre lines
        const float cy0 = a_shape.h * 0.5f - (nRows - 1) * kPitch * 0.5f;

        int drawn = 0, idx = 0;
        for (int r = 0; r < nRows; ++r) {
            const int k = rowsBuf[r];
            const float cx0 = a_shape.w * 0.5f - (k - 1) * kPitch * 0.5f;
            for (int c = 0; c < k && idx < n; ++c, ++idx) {
                float bx = cx0 + c * kPitch;
                float by = cy0 + r * kPitch;
                if (!Snap(a_shape, bx, by)) continue;   // would land on a hole

                const ImVec2 ctr(a_px.x + bx * cw, a_px.y + by * ch);
                const auto&  b = ovl->badges[idx];

                DrawWell(a_dl, ctr, rad, alpha);

                if (b.state == GridInvAPI::kBadgeFilled && b.iconId != 0) {
                    // iconId is a TESBoundObject FormID, not an atlas index --
                    // so a provider gets our whole art pipeline (model capture,
                    // both styles) for free, with nothing to version.
                    if (auto* form = RE::TESForm::LookupByID(b.iconId)) {
                        if (auto* bound = form->As<RE::TESBoundObject>()) {
                            auto* cache = IconCache::GetSingleton();
                            if (const auto* ic = cache->Get(bound)) {
                                const float s = rad * 1.62f;   // inside the rim
                                const float m = static_cast<float>((std::max)(ic->w, ic->h));
                                const float dw = ic->w / m * s;
                                const float dh = ic->h / m * s;
                                UIRoot::DrawItemIcon(a_dl, ic->srv,
                                    ImVec2(ctr.x - dw * 0.5f, ctr.y - dh * 0.5f),
                                    ImVec2(ctr.x + dw * 0.5f, ctr.y + dh * 0.5f));
                            } else {
                                cache->QueueCapture(bound);
                            }
                        }
                    }
                }
                if (b.state == GridInvAPI::kBadgeLocked) {
                    // sealed (a completed rune word): a slash across the well
                    const float d = rad * 0.62f;
                    a_dl->AddLine(ImVec2(ctr.x - d, ctr.y + d), ImVec2(ctr.x + d, ctr.y - d),
                                  IM_COL32(190, 176, 120, static_cast<int>(220 * alpha)), 1.4f);
                }
                if (b.tintRGBA != 0) {
                    // ★THE ABI SAYS 0xRRGGBBAA; ImU32 IS 0xAABBGGRR here
                    // (IMGUI_USE_BGRA_PACKED_COLOR is off in this build).
                    // Handed over raw, an extension's opaque green arrived as
                    // alpha 0 and drew nothing -- and since the header is what
                    // consumers vendor, they had no way to see why.
                    const ImU32 tint = IM_COL32((b.tintRGBA >> 24) & 0xFF,
                                                (b.tintRGBA >> 16) & 0xFF,
                                                (b.tintRGBA >>  8) & 0xFF,
                                                 b.tintRGBA        & 0xFF);
                    a_dl->AddCircle(ctr, rad - 0.5f, tint, 0, 1.6f);
                }
                ++drawn;
            }
        }
        return drawn > 0;
    }
}
