#pragma once

#include <cstdint>

struct ImDrawList;
struct ImVec2;

// =============================================================================
//  GI8 -- extension badge overlay
// =============================================================================
//  A provider (see api/GridInventoryAPI.h) can put up to 8 "badges" on one item
//  instance -- the socket mod's wells, for example. The GEOMETRY is ours: the
//  provider returns a count and per-badge contents, and this module decides
//  where they sit inside the tile and what a well looks like.
//
//  ★Placement is a TABLE, not a formula (PLAN_INSTANCE 3-A). Three attempts at
//  deriving it from `cols = min(w, n)` disagreed with the original, because the
//  interesting positions are HALF-cell: five badges on a 2-wide tile is a dice
//  five, whose centre sits between the two columns. Cell-snapped maths cannot
//  express that, so the row shape per (tile, n) is written out and looked up.
// =============================================================================

namespace FUI::Badges
{
    // Tile geometry the overlay has to fit inside.
    //   w/h   = footprint in CELLS
    //   cells = bit (y * 8 + x) set means that cell is part of the item.
    //           Polyomino items (an L-shaped war axe) leave holes, and a badge
    //           must never be drawn over one.
    struct TileShape
    {
        int           w = 1;
        int           h = 1;
        std::uint64_t cells = ~0ull;   // default: full rectangle
    };

    // Draw the badges for one instance. `a_px` is the tile's top-left in screen
    // space and `a_pw/a_ph` its pixel size. Returns false when the provider has
    // nothing for this instance (the common case -- keep it cheap).
    //
    // Call it AFTER the icon and BEFORE the count badge / marker tray so the
    // corner chrome stays readable on top.
    bool Draw(ImDrawList* a_dl, const ImVec2& a_px, float a_pw, float a_ph,
              const TileShape& a_shape, std::uint32_t a_owner, std::uint32_t a_base,
              std::uint16_t a_uid, bool a_hovered);
}
