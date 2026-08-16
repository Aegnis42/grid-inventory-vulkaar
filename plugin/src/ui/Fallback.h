#pragma once

#include "ui/IconCache.h"

#include <functional>
#include <map>
#include <string>

namespace FUI::Fallback
{
    // Hand-drawn flat icon set (flatA): loose files in
    // Data/SKSE/Plugins/GridInventory_fallback/.
    // Used two ways:
    //   - icon theme "flat": every item draws its category icon
    //   - icon theme "real": only items whose 3D capture permanently failed
    //
    // ★The folder is the customisation surface — no tool, no manifest. Drop a
    // PNG named after the key and it wins; the shipped set is PNG for exactly
    // that reason, so "what do I name it" is answered by looking at what is
    // already there. Resolution order:
    //
    //   item\<Plugin.esp_0xLocalID>.png   one form only
    //   <key>@<variant>.png               e.g. wpn_sword@steel
    //   <key>.png                         e.g. wpn_sword
    //   <key>@<variant>.fic  /  <key>.fic  1.0.4 leftovers, still read
    //
    // ★The whole PNG layer outranks the whole .fic layer. Ranking them
    // key-by-key instead would mean a player who drops in wpn_sword.png to
    // change "all swords" finds steel swords unchanged, because our shipped
    // wpn_sword@steel.fic is the more specific match — a rule that is correct
    // and useless.
    //
    // Returns null when no matching file exists (caller falls through).
    [[nodiscard]] const IconCache::Icon* Get(RE::TESBoundObject* a_obj);

    // main.cpp installs its FormKey ("Plugin.esp|0xLocalID") here — the same
    // spelling the item ini uses. Kept as a hook so there is ONE implementation
    // of that string in the codebase.
    void SetFormKeyResolver(std::function<std::string(RE::TESForm*)> a_fn);

    // What to name a PNG for this item / for its category. The editor shows
    // both, which is what makes the folder usable without a tool.
    [[nodiscard]] std::string ItemFileName(RE::TESBoundObject* a_obj);
    [[nodiscard]] std::string KeyFileName(RE::TESBoundObject* a_obj);

    // Drop every loaded drawing so edited files are picked up without a
    // restart. Releases SRVs -> call from UIRoot::Tick (outside the ImGui
    // frame) only, never from a settings row mid-draw.
    void ReloadAssets();

    // True once ANY fallback file resolved this session — cheap gate so the
    // settings row can grey out when the asset folder is missing.
    [[nodiscard]] bool AssetsSeen();

    // GI53: which key WOULD this item draw, before any file check. Only the
    // export below uses it — the point is that the rules stay in ONE place.
    // A tool that re-implemented them in its own language would answer
    // confidently and wrongly the moment this file changes.
    struct Assignment
    {
        const char* base = nullptr;   // "msc_tool" (nullptr = no rule matched)
        std::string variant;          // "" or a material/tier name
    };
    [[nodiscard]] Assignment Classify(RE::TESBoundObject* a_obj);

    // ★★★ARROW OR BOLT, and the only place that answers it. TESAmmo::IsBolt()
    // is UNUSABLE: CommonLibSSE-NG reads the DATA member directly while its
    // real offset moves between SE (0x110) and AE (0x100), so on AE it looks 16
    // bytes past the record. Measured across a full load order: 40 of 41 ammo
    // forms reported flags = 0xCD (uninitialised fill), which makes every bolt
    // an arrow and leaves Bound Arrow -- whose garbage happened to read 0 --
    // as the one "bolt". The category assignment in main.cpp was calling it.
    // ★The MESH leads, and the record only breaks a tie: every bolt in the game
    // is Bolt.nif and every arrow Arrow.nif, and a path survives a mod setting
    // the flag wrong. GetRuntimeData() is what relocates correctly when the
    // path says nothing.
    [[nodiscard]] bool IsBoltAmmo(RE::TESAmmo* a_ammo);

    // GI60: a drawn transform that belongs to the ICON, not to the item.
    // One drawing is shared by everything that resolves to it — 472 swords all
    // draw wpn_sword — so "this sword sits too high" is a property of the
    // picture. Precedence when drawing is the same shape as category defaults:
    //   item def (if it names a value) > icon key default > untouched.
    // Variants (@steel, @ebony …) are the same artwork in another colour and
    // therefore share the base key's transform.
    struct KeyXform
    {
        float scale = 1.0f;   // 0.2 ~ 4.0
        float rot = 0.0f;     // -180 ~ 180 degrees
        float x = 0.0f;       // -1 ~ 1, in cells (horizontal nudge)

        [[nodiscard]] bool IsDefault() const
        {
            return scale == 1.0f && rot == 0.0f && x == 0.0f;
        }
    };
    [[nodiscard]] KeyXform XformOf(const std::string& a_key);
    void SetXform(const std::string& a_key, const KeyXform& a_x);   // clamps
    void ClearXforms();
    // sorted, and holds ONLY keys that carry a non-default value — that is
    // what gets written to disk
    [[nodiscard]] const std::map<std::string, KeyXform>& Xforms();

    // The icon AND the transform its key carries, resolved in one pass. Use
    // this when drawing: calling Get() then XformOf() would classify twice.
    struct Drawn
    {
        const IconCache::Icon* icon = nullptr;
        KeyXform x;
    };
    [[nodiscard]] Drawn GetDrawn(RE::TESBoundObject* a_obj);

    // Walk the whole load order and write every item's assignment to
    // Data/SKSE/Plugins/GridInventory_iconmap.json (IconStudio reads it).
    // Classification only — nothing is rendered, so this finishes at once.
    // Returns the number of rows written, or 0 on failure.
    //
    // NO LONGER WIRED TO ANY BUTTON. The settings row it lived on became the
    // icon RELOAD row: naming a PNG is answered in the editor now, so players
    // have no reason to export a map. Kept because IconStudio still reads that
    // file when the shipped set is re-authored, and the map only goes stale
    // when the load order changes.
    size_t ExportAssignments();
}
