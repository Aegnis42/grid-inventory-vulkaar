#pragma once

#include "ui/IconCache.h"

namespace FUI::Fallback
{
    // Hand-drawn flat icon set (flatA): loose .fic files in
    // Data/SKSE/Plugins/GridInventory_fallback/<key>[@variant].fic.
    // Used two ways:
    //   - icon theme "flat": every item draws its category icon
    //   - icon theme "real": only items whose 3D capture permanently failed
    // Returns null when no matching file exists (caller falls through).
    [[nodiscard]] const IconCache::Icon* Get(RE::TESBoundObject* a_obj);

    // True once ANY fallback file resolved this session — cheap gate so the
    // settings row can grey out when the asset folder is missing.
    [[nodiscard]] bool AssetsSeen();
}
